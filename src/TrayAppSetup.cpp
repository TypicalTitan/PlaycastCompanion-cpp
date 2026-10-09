#include "pch.h"
#include "TrayAppInternal.h"

namespace pc {
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

    try { sessionLogger = std::make_unique<SessionLogger>(cfg, headless, g_harnessMode.load()); }
    catch (...) { LogInfo(L"Session logger unavailable; application continues."); }

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


}
