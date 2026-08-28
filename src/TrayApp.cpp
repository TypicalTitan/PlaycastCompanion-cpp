// TrayApp.cpp - orchestration: config, the six lighting backends, Discord
// presence (+ optional shim/relay), the session and registry watchers, the
// tray icon, a hidden message window and the evaluation worker.
// Port of TrayAppContext.cs (same log lines, same status strings).
#include "pch.h"
#include "TrayApp.h"

#include "Branding.h"
#include "ChromaController.h"
#include "CorsairController.h"
#include "DiscordShim.h"
#include "DynamicLightingController.h"
#include "Json.h"
#include "Log.h"
#include "LogitechController.h"
#include "MainWindow.h"
#include "OpenRgbController.h"
#include "RegistryWatcher.h"
#include "SessionWatcher.h"
#include "SteelSeriesController.h"

#include <deque>

namespace pc {
namespace {

constexpr wchar_t kWindowClass[] = L"PlaycastCompanionMsg";
constexpr wchar_t kAppTitle[] = L"Playcast Companion";
constexpr wchar_t kTipStealth[] = L"Playcast Companion — guest hosting, stealth on";
constexpr wchar_t kTipQuiet[] = L"Playcast Companion — all quiet";

// Private window messages (hidden window, UI thread).
constexpr UINT WM_PC_TRAY = WM_APP + 1;           // Shell_NotifyIcon callback
constexpr UINT WM_PC_EVALUATED = WM_APP + 2;      // wParam != 0 => guest active; swap icon/tip
constexpr UINT WM_PC_OPEN_SETTINGS = WM_APP + 3;  // posted by the show-settings signal thread
constexpr UINT WM_PC_PREVIEW = WM_APP + 4;        // arm the 10 s guest-mode preview timer
constexpr UINT WM_PC_APPLY_SETTINGS = WM_APP + 5; // posted by the eval worker: copy pending -> cfg

// --snapshot harness: never touch lights, Discord or the pipes (a second Chroma
// session with the same app title would invalidate the installed instance's).
std::atomic<bool> g_harnessMode{false};

constexpr UINT kTrayIconId = 1;
constexpr UINT_PTR kPollTimerId = 1;
constexpr UINT_PTR kPreviewTimerId = 2;
// Safety net only (WTS + registry notifications carry the real events), but it
// is also the worst-case entry/exit latency when a notification is missed —
// keep it tight; the check is a microsecond-scale WTS enumeration.
constexpr UINT kPollIntervalMs = 10'000;
constexpr UINT kPreviewMs = 10'000;

constexpr UINT kMenuOpen = 1001;
constexpr UINT kMenuPreview = 1002;
constexpr UINT kMenuCheck = 1003;
constexpr UINT kMenuExit = 1004;

std::wstring Win32Message(DWORD error) {
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text;
    if (length != 0 && buffer != nullptr)
        text.assign(buffer, length);
    if (buffer != nullptr)
        LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();
    if (text.empty())
        text = std::format(L"error {}", error);
    return text;
}

std::wstring Describe(const std::exception& ex) {
    return json::Utf8ToWide(ex.what());
}

/// Multi-threaded apartment for a worker thread (rule 8 of PORTING.md).
struct MtaScope {
    bool initialized = false;
    MtaScope() {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialized = true;
        } catch (...) {
            // apartment already set up differently on this thread; carry on
        }
    }
    ~MtaScope() {
        if (initialized)
            winrt::uninit_apartment();
    }
    MtaScope(const MtaScope&) = delete;
    MtaScope& operator=(const MtaScope&) = delete;
};

struct IconHandle {
    HICON h = nullptr;
    IconHandle() = default;
    ~IconHandle() { reset(); }
    IconHandle(const IconHandle&) = delete;
    IconHandle& operator=(const IconHandle&) = delete;
    void reset(HICON icon = nullptr) {
        if (h)
            DestroyIcon(h);
        h = icon;
    }
};

struct MenuHandle {
    HMENU h = nullptr;
    explicit MenuHandle(HMENU menu) : h(menu) {}
    ~MenuHandle() {
        if (h)
            DestroyMenu(h);
    }
    MenuHandle(const MenuHandle&) = delete;
    MenuHandle& operator=(const MenuHandle&) = delete;
};

struct ClassRegistration {
    HINSTANCE instance = nullptr;
    bool registered = false;
    ClassRegistration() = default;
    ~ClassRegistration() {
        if (registered)
            UnregisterClassW(kWindowClass, instance);
    }
    ClassRegistration(const ClassRegistration&) = delete;
    ClassRegistration& operator=(const ClassRegistration&) = delete;
};

struct WindowHandle {
    HWND h = nullptr;
    WindowHandle() = default;
    ~WindowHandle() {
        if (h)
            DestroyWindow(h);
    }
    WindowHandle(const WindowHandle&) = delete;
    WindowHandle& operator=(const WindowHandle&) = delete;
};

/// CREATE_ALWAYS + write all bytes. On failure `error` holds the Win32 code.
bool WriteTextFile(const std::wstring& path, const std::string& bytes, DWORD& error) {
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD createError = GetLastError();
    UniqueHandle file(raw);
    if (!file.valid()) {
        error = createError;
        return false;
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file.get(), bytes.data() + offset, chunk, &written, nullptr)) {
            error = GetLastError();
            return false;
        }
        if (written == 0) {
            error = ERROR_WRITE_FAULT;
            return false;
        }
        offset += written;
    }
    error = ERROR_SUCCESS;
    return true;
}

/// %TEMP% with a trailing backslash (GetTempPathW); empty on failure.
std::wstring TempDirectory() {
    wchar_t buffer[MAX_PATH + 2]{};
    const DWORD n = GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
    if (n == 0 || n >= std::size(buffer))
        return {};
    return std::wstring(buffer, n);
}

std::wstring ModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0)
            return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring SystemCmdPath() {
    wchar_t buffer[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(buffer, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return L"cmd.exe";
    return std::wstring(buffer, n) + L"\\cmd.exe";
}

/// Stop several backends at once (mirrors Task.WhenAll in C#); each
/// StopBlackout is bounded (~5 s) so the whole call is too.
void StopBackendsParallel(const std::vector<ILightingBackend*>& backends) {
    std::vector<std::jthread> workers;
    workers.reserve(backends.size());
    for (ILightingBackend* backend : backends) {
        workers.emplace_back([backend] {
            MtaScope apartment;
            try {
                backend->StopBlackout();
            } catch (const std::exception& ex) {
                LogInfo(std::format(L"{} stop failed: {}", backend->DisplayName(), Describe(ex)));
            } catch (...) {
                LogInfo(std::format(L"{} stop failed", backend->DisplayName()));
            }
        });
    }
    // jthread destructors join every worker before returning
}

}  // namespace

struct TrayApp::Impl {
    TrayApp& owner;
    HINSTANCE instance;
    HANDLE showSettingsEvent;  // borrowed from wWinMain; may be null
    DWORD uiThreadId = GetCurrentThreadId();
    AppConfig cfg;
    bool headless = false;

    // Settings edits (UI thread only). Workers read `cfg` by reference without
    // locks, so the UI edits a copy; SaveConfig writes the copy to disk and
    // queues a "settings" evaluation, which quiesces every worker and then has
    // the UI thread swap the copy in (WM_PC_APPLY_SETTINGS).
    AppConfig pending;
    bool pendingValid = false;
    std::mutex settingsMutex;
    std::condition_variable_any settingsCv;
    bool settingsApplied = false;  // guarded by settingsMutex

    IconHandle iconIdle;
    IconHandle iconStealth;

    std::vector<std::unique_ptr<ILightingBackend>> lighting;
    std::unique_ptr<DiscordPresence> discord;
    std::unique_ptr<DiscordShimServer> shim;
    std::unique_ptr<DiscordChannelListener> channel;

    // Shared between the UI thread and the evaluation worker.
    UniqueHandle stopEvent;  // manual-reset; set once at shutdown
    std::mutex queueMutex;
    std::condition_variable_any queueCv;
    std::deque<std::wstring> queue;
    std::atomic<bool> guestActive{false};
    // While the 10 s tray-menu preview runs, periodic polls and session events
    // must not tear the preview's blackout down — only "preview end" may.
    std::atomic<bool> previewArmed{false};
    std::atomic<bool> exiting{false};
    bool shutDown = false;  // UI thread only

    // Tray state (UI thread only).
    bool trayAdded = false;
    bool trayV4 = false;
    std::wstring trayTip = kAppTitle;
    UINT taskbarCreatedMsg = 0;

    // Hidden window, watchers and the settings window (UI thread only).
    // Declared after the shared state so watcher callbacks can still reach
    // it while these are torn down.
    ClassRegistration windowClass;
    WindowHandle msgWindow;
    std::unique_ptr<SessionWatcher> sessions;
    std::unique_ptr<RegistryWatcher> registry;
    std::unique_ptr<MainWindow> window;

    // Threads last: joined before anything they touch is destroyed.
    std::jthread evalThread;
    std::jthread signalThread;

    Impl(TrayApp& o, HINSTANCE inst, HANDLE evt);
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    HWND Hwnd() const { return msgWindow.h; }

    // setup
    void CreateMessageWindow();
    void AddTrayIcon();
    void RemoveTrayIcon();
    void UpdateTray(bool active);
    void ShowTrayMenu(POINT anchor);
    void OnTrayMessage(WPARAM wParam, LPARAM lParam);

    // window procedure
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND h, UINT msg, WPARAM wParam, LPARAM lParam);

    // workers
    void EvalLoop(std::stop_token stop);
    void Evaluate(const std::wstring& reason, std::stop_token stop);
    bool ApplySettings(std::stop_token stop);  // eval thread; false when stopping
    void RunPreview();                         // eval thread
    void SignalLoop();

    // actions
    void RequestEvaluate(std::wstring reason);
    void TestGuestMode();
    void OpenSettings();
    void OpenLog();
    AppConfig& PendingConfig();  // UI thread: the editable copy, seeded from cfg
    bool SaveConfig(std::wstring& error);
    bool SaveElevated(const std::wstring& path, const std::string& json, std::wstring& error);
    void RestartApp();
    void Exit();
    void Shutdown();
    void PostQuit();
    void LaunchSelf();
};

// ---------------------------------------------------------------------------
// construction / teardown
// ---------------------------------------------------------------------------

TrayApp::Impl::Impl(TrayApp& o, HINSTANCE inst, HANDLE evt)
    : owner(o), instance(inst), showSettingsEvent(evt), cfg(AppConfig::Load()) {
    headless = SessionWatcher::CurrentUserIs(cfg.TargetUsername);
    LogInfo(std::format(L"--- started (pid {}, user {}, target {}{}) ---", GetCurrentProcessId(),
                        SessionWatcher::CurrentUserName(), cfg.TargetUsername,
                        headless ? L", headless" : L""));

    iconIdle.reset(brand::MakeAppIcon(32, 1.0f));
    iconStealth.reset(brand::MakeAppIcon(32, 0.35f));

    lighting.reserve(6);
    lighting.emplace_back(std::make_unique<ChromaController>(cfg));
    lighting.emplace_back(std::make_unique<SteelSeriesController>(cfg.SteelSeries));
    lighting.emplace_back(std::make_unique<LogitechController>(cfg.Logitech));
    lighting.emplace_back(std::make_unique<CorsairController>(cfg.Corsair));
    lighting.emplace_back(std::make_unique<OpenRgbController>(cfg.OpenRgb));
    lighting.emplace_back(std::make_unique<DynamicLightingController>(cfg));

    // Discord runs in the owner's session only; the guest session has no
    // Discord client (and its pipe wouldn't be ours to talk to anyway).
    if (!headless)
        discord = std::make_unique<DiscordPresence>(cfg.Discord);

    // Game pass-through (default off): the shim runs ONLY in the guest
    // session; the relay listener runs ONLY in the owner session.
    if (cfg.Discord.GamePassthroughEnabled && !g_harnessMode.load()) {
        if (headless) {
            shim = std::make_unique<DiscordShimServer>(cfg);
            shim->Start();
        } else if (discord && cfg.Discord.Enabled) {
            channel = std::make_unique<DiscordChannelListener>(cfg, *discord);
            channel->Start();
        }
    }

    stopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    CreateMessageWindow();

    sessions = std::make_unique<SessionWatcher>(cfg, Hwnd(), [this] { RequestEvaluate(L"session change"); });
    if (cfg.RegistryWatch.Enabled)
        registry = std::make_unique<RegistryWatcher>(cfg.RegistryWatch,
                                                     [this] { RequestEvaluate(L"registry change"); });

    if (!headless)
        AddTrayIcon();

    // Safety net: periodic re-check in case a WTS notification was missed.
    SetTimer(Hwnd(), kPollTimerId, kPollIntervalMs, nullptr);

    evalThread = std::jthread([this](std::stop_token stop) { EvalLoop(stop); });

    // A second launch of the exe signals this event instead of starting a
    // duplicate; respond by bringing up the settings window.
    if (showSettingsEvent != nullptr && stopEvent.valid())
        signalThread = std::jthread([this] { SignalLoop(); });
}

TrayApp::Impl::~Impl() {
    Shutdown();
    // Tear down everything the hidden window's procedure can reach before the
    // window itself: DestroyWindow (in ~WindowHandle below) delivers
    // WM_DESTROY/WM_NCDESTROY synchronously to HandleMessage, which tests
    // `sessions`. unique_ptr::reset() nulls the pointer, so that test is safe.
    window.reset();
    registry.reset();
    sessions.reset();  // WTSUnRegisterSessionNotification while Hwnd() is still valid
    // Remaining members are destroyed in reverse order: hidden window, class,
    // ..., backends.
}

void TrayApp::Impl::CreateMessageWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = static_cast<UINT>(sizeof(wc));
    wc.lpfnWndProc = &Impl::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    if (RegisterClassExW(&wc) != 0) {
        windowClass.instance = instance;
        windowClass.registered = true;
    } else {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error(std::format("RegisterClassExW failed (error {})", error));
    }
    // A normal (never shown) top-level window: WTS session notifications and
    // tray callbacks need a real window, not an HWND_MESSAGE one.
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, kAppTitle, WS_OVERLAPPED, 0, 0, 0, 0,
                             nullptr, nullptr, instance, this);
    if (h == nullptr) {
        const DWORD error = GetLastError();
        throw std::runtime_error(std::format("CreateWindowExW failed (error {})", error));
    }
    msgWindow.h = h;
    taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
}

// ---------------------------------------------------------------------------
// tray icon + menu
// ---------------------------------------------------------------------------

void TrayApp::Impl::AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = static_cast<DWORD>(sizeof(nid));
    nid.hWnd = Hwnd();
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_PC_TRAY;
    nid.hIcon = guestActive.load() ? iconStealth.h : iconIdle.h;
    wcsncpy_s(nid.szTip, ARRAYSIZE(nid.szTip), trayTip.c_str(), _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        trayAdded = false;
        LogInfo(std::format(L"tray icon add failed (error {})", GetLastError()));
        return;
    }
    trayAdded = true;
    nid.uVersion = NOTIFYICON_VERSION_4;
    trayV4 = Shell_NotifyIconW(NIM_SETVERSION, &nid) != FALSE;
}

void TrayApp::Impl::RemoveTrayIcon() {
    if (!trayAdded)
        return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = static_cast<DWORD>(sizeof(nid));
    nid.hWnd = Hwnd();
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayAdded = false;
}

void TrayApp::Impl::UpdateTray(bool active) {
    trayTip = active ? kTipStealth : kTipQuiet;
    if (!trayAdded)
        return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = static_cast<DWORD>(sizeof(nid));
    nid.hWnd = Hwnd();
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = active ? iconStealth.h : iconIdle.h;
    wcsncpy_s(nid.szTip, ARRAYSIZE(nid.szTip), trayTip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayApp::Impl::ShowTrayMenu(POINT anchor) {
    MenuHandle menu(CreatePopupMenu());
    if (menu.h == nullptr)
        return;
    AppendMenuW(menu.h, MF_STRING, kMenuOpen, L"Open Playcast Companion");
    AppendMenuW(menu.h, MF_STRING, kMenuPreview, L"Preview guest mode (10 s)");
    AppendMenuW(menu.h, MF_STRING, kMenuCheck, L"Check again now");
    AppendMenuW(menu.h, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.h, MF_STRING, kMenuExit, L"Exit");
    SetMenuDefaultItem(menu.h, kMenuOpen, FALSE);  // bold, also the double-click action

    // Standard tray-menu dance: the menu closes when the window loses
    // foreground status; the WM_NULL nudge afterwards avoids a stuck menu.
    SetForegroundWindow(Hwnd());
    UINT flags = TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN;
    flags |= (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const int command = TrackPopupMenuEx(menu.h, flags, anchor.x, anchor.y, Hwnd(), nullptr);
    PostMessageW(Hwnd(), WM_NULL, 0, 0);

    switch (static_cast<UINT>(command)) {
    case kMenuOpen:
        OpenSettings();
        break;
    case kMenuPreview:
        TestGuestMode();
        break;
    case kMenuCheck:
        RequestEvaluate(L"manual");
        break;
    case kMenuExit:
        Exit();
        break;
    default:
        break;
    }
}

void TrayApp::Impl::OnTrayMessage(WPARAM wParam, LPARAM lParam) {
    // NOTIFYICON_VERSION_4: LOWORD(lParam) = event, wParam = anchor point.
    // Legacy (SETVERSION failed): lParam = event, no coordinates.
    const UINT event = trayV4 ? LOWORD(lParam) : static_cast<UINT>(lParam);
    switch (event) {
    case WM_LBUTTONDBLCLK:
    case NIN_KEYSELECT:
        OpenSettings();
        break;
    case WM_CONTEXTMENU:
        if (trayV4)
            ShowTrayMenu(POINT{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)});
        break;
    case WM_RBUTTONUP:
        if (!trayV4) {
            POINT pt{};
            GetCursorPos(&pt);
            ShowTrayMenu(pt);
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// hidden window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK TrayApp::Impl::WndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    Impl* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<Impl*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self == nullptr)
        return DefWindowProcW(h, msg, wParam, lParam);

    // Never let an exception escape a window procedure (rule 10).
    try {
        return self->HandleMessage(h, msg, wParam, lParam);
    } catch (const std::exception& ex) {
        LogInfo(L"UI exception: " + Describe(ex));
    } catch (const winrt::hresult_error& e) {
        LogInfo(L"UI exception: " + std::wstring(e.message()));
    } catch (...) {
        LogInfo(L"UI exception: unknown error");
    }
    return 0;
}

LRESULT TrayApp::Impl::HandleMessage(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCDESTROY) {
        // Last message: detach before touching any member (the watchers may
        // already be gone when the window is destroyed during teardown).
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return DefWindowProcW(h, msg, wParam, lParam);
    }
    if (sessions && sessions->HandleMessage(msg, wParam, lParam))
        return 0;

    if (taskbarCreatedMsg != 0 && msg == taskbarCreatedMsg) {
        // Explorer restarted: re-add the icon (NotifyIcon did this for us in C#).
        if (!headless && !exiting.load()) {
            trayAdded = false;
            AddTrayIcon();
        }
        return 0;
    }

    switch (msg) {
    case WM_PC_TRAY:
        OnTrayMessage(wParam, lParam);
        return 0;
    case WM_PC_EVALUATED:
        UpdateTray(wParam != 0);
        return 0;
    case WM_PC_OPEN_SETTINGS:
        OpenSettings();
        return 0;
    case WM_PC_PREVIEW:
        SetTimer(h, kPreviewTimerId, kPreviewMs, nullptr);
        return 0;
    case WM_PC_APPLY_SETTINGS:
        // The eval worker has quiesced every config reader and is waiting on
        // settingsCv; nothing on this thread is mid-read between messages.
        {
            std::lock_guard<std::mutex> lock(settingsMutex);
            cfg = pending;
            pendingValid = false;
            settingsApplied = true;
        }
        settingsCv.notify_all();
        return 0;
    case WM_TIMER:
        if (wParam == kPollTimerId) {
            RequestEvaluate(L"poll");
        } else if (wParam == kPreviewTimerId) {
            KillTimer(h, kPreviewTimerId);
            RequestEvaluate(L"preview end");
        }
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;  // never block logoff/shutdown; the work happens on WM_ENDSESSION
    case WM_ENDSESSION:
        // Release promptly when this session logs off or the machine shuts
        // down (SystemEvents.SessionEnded in C#).
        if (wParam != 0)
            Exit();
        return 0;
    case WM_CLOSE:
        Exit();  // e.g. a polite taskkill
        return 0;
    default:
        return DefWindowProcW(h, msg, wParam, lParam);
    }
}

// ---------------------------------------------------------------------------
// evaluation worker
// ---------------------------------------------------------------------------

void TrayApp::Impl::RequestEvaluate(std::wstring reason) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (exiting.load())
            return;
        queue.push_back(std::move(reason));
    }
    queueCv.notify_one();
}

void TrayApp::Impl::EvalLoop(std::stop_token stop) {
    MtaScope apartment;
    for (;;) {
        if (stop.stop_requested())
            break;
        std::wstring reason;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            if (!queueCv.wait(lock, stop, [this] { return !queue.empty(); }))
                break;
            reason = std::move(queue.front());
            queue.pop_front();
        }
        Evaluate(reason, stop);
    }
}

/// Quiesce every worker that reads `cfg` by reference, hand the swap to the UI
/// thread and wait for it. Returns false when shutdown interrupted the wait.
bool TrayApp::Impl::ApplySettings(std::stop_token stop) {
    std::vector<ILightingBackend*> all;
    all.reserve(lighting.size());
    for (const auto& backend : lighting)
        all.push_back(backend.get());
    StopBackendsParallel(all);  // each StopBlackout joins its worker
    if (discord) {
        try {
            discord->Clear();  // joins the presence worker
        } catch (const std::exception& ex) {
            LogInfo(L"discord clear failed: " + Describe(ex));
        } catch (...) {
            LogInfo(L"discord clear failed");
        }
    }
    if (channel) {
        try {
            channel->Stop();  // its listener thread reads TargetUsername/ShimPipeName
        } catch (...) {
            LogInfo(L"channel listener stop failed");
        }
        channel.reset();
    }

    {
        std::unique_lock<std::mutex> lock(settingsMutex);
        settingsApplied = false;
        if (!PostMessageW(Hwnd(), WM_PC_APPLY_SETTINGS, 0, 0)) {
            LogInfo(std::format(L"settings apply failed: PostMessage error {}", GetLastError()));
            return false;
        }
        // stop_token-aware so Shutdown()'s request_stop + join cannot deadlock.
        if (!settingsCv.wait(lock, stop, [this] { return settingsApplied; }))
            return false;
    }

    if (cfg.Discord.GamePassthroughEnabled && discord && cfg.Discord.Enabled && !g_harnessMode.load()) {
        channel = std::make_unique<DiscordChannelListener>(cfg, *discord);
        channel->Start();
    }
    return true;
}

/// TestGuestModeAsync body, run on the eval thread so the UI thread can never
/// start a worker between a settings quiesce and the config swap.
void TrayApp::Impl::RunPreview() {
    LogInfo(L"guest-mode preview started");
    previewArmed.store(true);
    if (!g_harnessMode.load()) {
        for (const auto& backend : lighting) {
            try {
                backend->StartBlackout();  // no-ops when the backend is disabled
            } catch (const std::exception& ex) {
                LogInfo(std::format(L"{} preview start failed: {}", backend->DisplayName(), Describe(ex)));
            }
        }
        if (discord)
            discord->SetActive();
    }
    // The 10 s wait lives on the UI thread's timer so repeated clicks just
    // extend it; it ends with a normal re-evaluation.
    PostMessageW(Hwnd(), WM_PC_PREVIEW, 0, 0);
}

void TrayApp::Impl::Evaluate(const std::wstring& reason, std::stop_token stop) {
    try {
        if (reason == L"preview") {
            RunPreview();
            return;
        }
        if (reason == L"settings" && !ApplySettings(stop))
            return;  // shutting down
        if (reason == L"preview end")
            previewArmed.store(false);
        const bool harness = g_harnessMode.load();
        const bool preview = previewArmed.load();
        const bool active = (sessions && sessions->IsTargetUserLoggedOn()) || (registry && registry->IsActive());
        if (harness) {
            // --snapshot: only the state the UI renders, never the side effects.
        } else if (preview && !active) {
            // Mid-preview poll/session event with no real guest: leave the
            // preview's blackout and presence alone until "preview end".
            return;
        } else if (active) {
            if (!guestActive.load())
                LogInfo(std::format(L"guest session active ({}) -> stealth on", reason));
            for (const auto& backend : lighting) {
                const bool enabled = backend->Enabled();
                const bool holding = backend->IsHolding();
                if (enabled && !holding)
                    backend->StartBlackout();
                else if (!enabled && holding)
                    backend->StopBlackout();  // toggled off mid-hold
            }
        } else {
            std::vector<ILightingBackend*> holding;
            for (const auto& backend : lighting) {
                if (backend->IsHolding())
                    holding.push_back(backend.get());
            }
            if (!holding.empty()) {
                LogInfo(std::format(L"guest session gone ({}) -> restoring {} lighting system(s)", reason,
                                    holding.size()));
                StopBackendsParallel(holding);
            }
        }
        if (discord && !harness) {
            if (active)
                discord->SetActive();
            else
                discord->Clear();
        }
        guestActive.store(active);
        PostMessageW(Hwnd(), WM_PC_EVALUATED, active ? 1 : 0, 0);
    } catch (const std::exception& ex) {
        LogInfo(L"evaluate failed: " + Describe(ex));
    } catch (const winrt::hresult_error& e) {
        LogInfo(L"evaluate failed: " + std::wstring(e.message()));
    } catch (...) {
        LogInfo(L"evaluate failed: unknown error");
    }
}

void TrayApp::Impl::SignalLoop() {
    const HANDLE handles[2] = {showSettingsEvent, stopEvent.get()};
    for (;;) {
        const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (result != WAIT_OBJECT_0)
            break;  // stop event, abandoned handle or failure: shut the thread down
        if (!headless && !exiting.load())
            PostMessageW(Hwnd(), WM_PC_OPEN_SETTINGS, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------

void TrayApp::Impl::TestGuestMode() {
    RequestEvaluate(L"preview");  // serialized with evaluations and settings swaps
}

void TrayApp::Impl::OpenSettings() {
    if (headless || exiting.load())
        return;
    pendingValid = false;  // a (re)opened window edits a fresh copy of the live config
    if (!window || !window->IsAlive()) {
        window.reset();
        window = std::make_unique<MainWindow>(owner);
    }
    window->Show();
    window->Activate();
}

void TrayApp::Impl::OpenLog() {
    LogInfo(L"log opened from UI");
    const std::wstring path = LogFilePath();
    const auto rc = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (rc <= 32) {
        const DWORD error = GetLastError();
        LogInfo(std::format(L"open log failed: {} (ShellExecute {})", Win32Message(error), rc));
    }
}

AppConfig& TrayApp::Impl::PendingConfig() {
    if (!pendingValid) {
        pending = cfg;  // cfg is only ever written on this (UI) thread
        pendingValid = true;
    }
    return pending;
}

bool TrayApp::Impl::SaveConfig(std::wstring& error) {
    error.clear();
    std::string json;
    try {
        json = PendingConfig().ToJson();
    } catch (const std::exception& ex) {
        error = Describe(ex);
    } catch (const winrt::hresult_error& e) {
        error = std::wstring(e.message());
    }
    if (!error.empty()) {
        LogInfo(L"settings save failed: " + error);
        return false;
    }

    const std::wstring path = AppConfig::ConfigPath();
    DWORD writeError = ERROR_SUCCESS;
    if (!WriteTextFile(path, json, writeError)) {
        if (writeError == ERROR_ACCESS_DENIED) {
            // Installed under Program Files: the config is deliberately not
            // writable by standard users, so saving takes one UAC prompt.
            if (!SaveElevated(path, json, error)) {
                LogInfo(L"settings save failed: " + error);
                return false;
            }
        } else {
            error = Win32Message(writeError);
            LogInfo(L"settings save failed: " + error);
            return false;
        }
    }
    LogInfo(L"settings saved from UI");
    // Live-apply (C#: "toggles apply right away"): the eval worker quiesces
    // every reader, then the UI thread swaps `pending` into `cfg`.
    RequestEvaluate(L"settings");
    return true;
}

bool TrayApp::Impl::SaveElevated(const std::wstring& path, const std::string& json, std::wstring& error) {
    const std::wstring tempDir = TempDirectory();
    if (tempDir.empty()) {
        error = L"could not locate the temp folder";
        return false;
    }
    const std::wstring tmp = tempDir + L"playcast-companion-config.json";
    DWORD writeError = ERROR_SUCCESS;
    if (!WriteTextFile(tmp, json, writeError)) {
        error = L"could not write temporary config: " + Win32Message(writeError);
        return false;
    }

    const std::wstring cmd = SystemCmdPath();
    const std::wstring parameters = std::format(L"/c copy /y \"{}\" \"{}\"", tmp, path);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = static_cast<DWORD>(sizeof(sei));
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = cmd.c_str();
    sei.lpParameters = parameters.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        const DWORD launchError = GetLastError();
        DeleteFileW(tmp.c_str());
        error = (launchError == ERROR_CANCELLED) ? L"The operation was cancelled"
                                                 : L"could not start elevated copy: " + Win32Message(launchError);
        return false;
    }
    UniqueHandle process(sei.hProcess);
    if (!process.valid()) {
        DeleteFileW(tmp.c_str());
        error = L"could not start elevated copy";
        return false;
    }
    const DWORD wait = WaitForSingleObject(process.get(), 60'000);
    DeleteFileW(tmp.c_str());
    if (wait != WAIT_OBJECT_0) {
        error = L"elevated copy did not finish";
        return false;
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process.get(), &exitCode)) {
        error = L"elevated copy: exit code unavailable";
        return false;
    }
    if (exitCode != 0) {
        error = std::format(L"elevated copy exited with code {}", exitCode);
        return false;
    }
    return true;
}

void TrayApp::Impl::LaunchSelf() {
    const std::wstring exe = ModulePath();
    if (exe.empty()) {
        LogInfo(L"restart failed: module path unavailable");
        return;
    }
    const std::wstring dir = std::filesystem::path(exe).parent_path().wstring();
    // Application.Restart re-launches with the original arguments, so an
    // --autostart instance comes back quiet (no settings window).
    std::wstring params;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc); argv != nullptr) {
        for (int i = 1; i < argc; ++i) {
            if (!params.empty())
                params += L' ';
            params += L'"';
            params += argv[i];
            params += L'"';
        }
        LocalFree(argv);
    }
    const auto rc = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exe.c_str(),
                                                            params.empty() ? nullptr : params.c_str(),
                                                            dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL));
    if (rc <= 32) {
        const DWORD error = GetLastError();
        LogInfo(std::format(L"restart failed: {} (ShellExecute {})", Win32Message(error), rc));
    }
}

void TrayApp::Impl::RestartApp() {
    LogInfo(L"restart requested (settings changed)");
    // Shut down first so the new instance's 3 s mutex wait (Program.cs
    // handoff) starts only once our lights/presence are already released.
    Shutdown();
    LaunchSelf();
    PostQuit();
}

void TrayApp::Impl::PostQuit() {
    if (GetCurrentThreadId() == uiThreadId)
        PostQuitMessage(0);
    else
        PostThreadMessageW(uiThreadId, WM_QUIT, 0, 0);
}

void TrayApp::Impl::Exit() {
    Shutdown();
    PostQuit();
}

void TrayApp::Impl::Shutdown() {
    if (shutDown)
        return;
    shutDown = true;
    exiting.store(true);

    if (Hwnd() != nullptr) {
        KillTimer(Hwnd(), kPollTimerId);
        KillTimer(Hwnd(), kPreviewTimerId);
    }
    if (window && window->IsAlive())
        ShowWindow(window->Handle(), SW_HIDE);
    RemoveTrayIcon();

    // Stop the evaluation worker so nothing races the teardown below.
    if (evalThread.joinable()) {
        evalThread.request_stop();
        queueCv.notify_all();
        if (evalThread.get_id() != std::this_thread::get_id())
            evalThread.join();
    }
    if (stopEvent.valid())
        SetEvent(stopEvent.get());
    if (signalThread.joinable() && signalThread.get_id() != std::this_thread::get_id())
        signalThread.join();

    // Hand every lighting system back (bounded: each StopBlackout returns
    // within ~5 s and they run in parallel), then clear the presence.
    std::vector<ILightingBackend*> all;
    all.reserve(lighting.size());
    for (const auto& backend : lighting)
        all.push_back(backend.get());
    StopBackendsParallel(all);

    if (discord) {
        try {
            discord->Clear();
        } catch (const std::exception& ex) {
            LogInfo(L"discord clear failed: " + Describe(ex));
        } catch (...) {
            LogInfo(L"discord clear failed");
        }
    }
    if (shim) {
        try {
            shim->Stop();
        } catch (...) {
            LogInfo(L"shim stop failed");
        }
    }
    if (channel) {
        try {
            channel->Stop();
        } catch (...) {
            LogInfo(L"channel listener stop failed");
        }
    }
    LogInfo(L"--- exited ---");
}

// ---------------------------------------------------------------------------
// TrayApp facade
// ---------------------------------------------------------------------------

TrayApp::TrayApp(HINSTANCE instance, bool openSettings, HANDLE showSettingsEvent)
    : impl_(std::make_unique<Impl>(*this, instance, showSettingsEvent)) {
    impl_->RequestEvaluate(L"startup");
    if (openSettings && !impl_->headless) {
        try {
            impl_->OpenSettings();
        } catch (const std::exception& ex) {
            LogInfo(L"UI exception: " + Describe(ex));
        } catch (const winrt::hresult_error& e) {
            LogInfo(L"UI exception: " + std::wstring(e.message()));
        }
    }
}

TrayApp::~TrayApp() = default;

int TrayApp::Run() {
    MSG msg{};
    for (;;) {
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result == 0)
            break;  // WM_QUIT
        if (result == -1) {
            LogInfo(std::format(L"GetMessage failed (error {})", GetLastError()));
            return 1;
        }
        // Keyboard navigation (Tab/arrows/Enter) inside the settings window.
        if (impl_->window && impl_->window->IsAlive() && IsDialogMessageW(impl_->window->Handle(), &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

HINSTANCE TrayApp::Instance() const { return impl_->instance; }
const AppConfig& TrayApp::Config() const { return impl_->cfg; }
AppConfig& TrayApp::MutableConfig() { return impl_->PendingConfig(); }
void TrayApp::SetHarnessMode(bool enabled) { g_harnessMode.store(enabled); }
const std::vector<std::unique_ptr<ILightingBackend>>& TrayApp::Lighting() const { return impl_->lighting; }
DiscordPresence* TrayApp::Discord() const { return impl_->discord.get(); }
bool TrayApp::GuestActive() const { return impl_->guestActive.load(); }
bool TrayApp::IsHeadless() const { return impl_->headless; }
HICON TrayApp::AppIcon() const { return impl_->iconIdle.h; }

void TrayApp::RequestEvaluate(std::wstring reason) { impl_->RequestEvaluate(std::move(reason)); }
void TrayApp::TestGuestMode() { impl_->TestGuestMode(); }
void TrayApp::OpenSettings() { impl_->OpenSettings(); }
void TrayApp::OpenLog() { impl_->OpenLog(); }
bool TrayApp::SaveConfig(std::wstring& error) { return impl_->SaveConfig(error); }
void TrayApp::RestartApp() { impl_->RestartApp(); }
void TrayApp::Exit() { impl_->Exit(); }

}  // namespace pc
