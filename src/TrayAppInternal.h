#pragma once
// TrayApp.cpp - orchestration: config, the six lighting backends, Discord
// presence (+ optional shim/relay), the session and registry watchers, the
// tray icon, a hidden message window and the evaluation worker.
// Port of TrayAppContext.cs (same log lines, same status strings).
#include "pch.h"
#include "TrayApp.h"
#include "SessionLogging.h"
#include "SessionLoggingJson.h"

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
namespace traydetail {
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
extern std::atomic<bool> g_harnessMode;

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

std::wstring Win32Message(DWORD error);
std::wstring Describe(const std::exception& error);
bool WriteTextFile(const std::wstring& path, const std::string& bytes, DWORD& error);
std::wstring TempDirectory();
std::wstring ModulePath();
std::wstring SystemCmdPath();
void StopBackendsParallel(const std::vector<ILightingBackend*>& backends);
}
using namespace traydetail;
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
    std::unique_ptr<SessionLogger> sessionLogger;

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


}
