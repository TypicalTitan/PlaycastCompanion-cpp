// main.cpp - process entry. Port of Program.cs: dev flags (--lamps,
// --snapshot), single instance per session via a Local\ mutex, a named event
// that asks the running instance to show its window, and "log, never crash
// dialog" handling of anything unhandled.
#include "pch.h"
#include "TrayApp.h"

#include "DynamicLightingController.h"
#include "Log.h"
#include "MainWindow.h"

#include <cstdint>
#include <cstdlib>
#include <exception>

namespace {

constexpr wchar_t kMutexName[] = L"Local\\PlaycastCompanion";
constexpr wchar_t kShowSettingsEventName[] = L"Local\\PlaycastCompanion.ShowSettings";

struct Options {
    bool autostart = false;
    bool lamps = false;
    std::optional<std::wstring> lampsPath;
    bool snapshot = false;
    std::optional<std::wstring> snapshotDir;
    std::optional<std::wstring> snapshotLog;
};

bool EqualsIgnoreCase(const wchar_t* a, const wchar_t* b) {
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

/// Same rules as Program.cs: first occurrence of a flag wins; "--lamps" and
/// "--snapshot" take the following argument verbatim when there is one.
Options ParseArgs() {
    Options options;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr)
        return options;
    struct ArgvFree {
        LPWSTR* p;
        ~ArgvFree() { LocalFree(p); }
    } argvFree{argv};

    int lampsIdx = -1;
    int snapshotIdx = -1;
    for (int i = 1; i < argc; ++i) {
        if (EqualsIgnoreCase(argv[i], L"--autostart"))
            options.autostart = true;
        else if (lampsIdx < 0 && EqualsIgnoreCase(argv[i], L"--lamps"))
            lampsIdx = i;
        else if (snapshotIdx < 0 && EqualsIgnoreCase(argv[i], L"--snapshot"))
            snapshotIdx = i;
        else if (!options.snapshotLog && EqualsIgnoreCase(argv[i], L"--snapshot-log") && i + 1 < argc)
            options.snapshotLog = argv[i + 1];
    }
    if (lampsIdx >= 0) {
        options.lamps = true;
        if (lampsIdx + 1 < argc)
            options.lampsPath = argv[lampsIdx + 1];
    }
    if (snapshotIdx >= 0) {
        options.snapshot = true;
        if (snapshotIdx + 1 < argc)
            options.snapshotDir = argv[snapshotIdx + 1];
    }
    return options;
}

/// %TEMP% with a trailing backslash; empty on failure.
std::wstring TempDirectory() {
    wchar_t buffer[MAX_PATH + 2]{};
    const DWORD n = GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
    if (n == 0 || n >= std::size(buffer))
        return {};
    return std::wstring(buffer, n);
}

// --- "log instead of crashing with a dialog": a stray error dialog inside
// the guest session would be visible to guests. ------------------------------

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
    DWORD code = 0;
    std::uintptr_t address = 0;
    if (info != nullptr && info->ExceptionRecord != nullptr) {
        code = info->ExceptionRecord->ExceptionCode;
        address = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    }
    pc::LogInfo(std::format(L"fatal: unhandled exception 0x{:08X} at 0x{:X}", code, address));
    return EXCEPTION_EXECUTE_HANDLER;  // terminate quietly, no WER dialog
}

[[noreturn]] void OnTerminate() noexcept {
    try {
        if (const std::exception_ptr current = std::current_exception())
            std::rethrow_exception(current);
        pc::LogInfo(L"fatal: std::terminate called");
    } catch (const std::exception& ex) {
        pc::LogInfo(std::string("fatal: unhandled C++ exception: ") + ex.what());
    } catch (const winrt::hresult_error& e) {
        pc::LogInfo(L"fatal: unhandled WinRT exception: " + std::wstring(e.message()));
    } catch (...) {
        pc::LogInfo(L"fatal: unhandled exception of unknown type");
    }
    TerminateProcess(GetCurrentProcess(), 3);
    ExitProcess(3);
}

void InstallCrashHandling() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(&OnUnhandledException);
    std::set_terminate(&OnTerminate);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

// --- RAII scopes ---------------------------------------------------------------

/// Single-threaded apartment for the UI thread.
struct StaScope {
    bool initialized = false;
    StaScope() {
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            initialized = true;
        } catch (const winrt::hresult_error& e) {
            pc::LogInfo(L"init_apartment failed: " + std::wstring(e.message()));
        }
    }
    ~StaScope() {
        if (initialized)
            winrt::uninit_apartment();
    }
    StaScope(const StaScope&) = delete;
    StaScope& operator=(const StaScope&) = delete;
};

/// Multi-threaded apartment for a worker thread (rule 8 of PORTING.md).
struct MtaScope {
    bool initialized = false;
    MtaScope() {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialized = true;
        } catch (...) {
        }
    }
    ~MtaScope() {
        if (initialized)
            winrt::uninit_apartment();
    }
    MtaScope(const MtaScope&) = delete;
    MtaScope& operator=(const MtaScope&) = delete;
};

struct GdiplusScope {
    ULONG_PTR token = 0;
    bool started = false;
    GdiplusScope() {
        Gdiplus::GdiplusStartupInput input;
        started = Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
        if (!started)
            pc::LogInfo(L"GdiplusStartup failed");
    }
    ~GdiplusScope() {
        if (started)
            Gdiplus::GdiplusShutdown(token);
    }
    GdiplusScope(const GdiplusScope&) = delete;
    GdiplusScope& operator=(const GdiplusScope&) = delete;
};

/// Dispatch pending messages for roughly `durationMs` (snapshot harness).
void PumpMessages(DWORD durationMs) {
    const ULONGLONG deadline = GetTickCount64() + durationMs;
    for (;;) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(deadline - now), QS_ALLINPUT);
    }
}

// --- the three entry modes ---------------------------------------------------

/// Hidden dev flag: enumerate LampArray (Dynamic Lighting) devices and exit.
int RunLamps(const Options& options) {
    const std::wstring outPath = options.lampsPath.value_or(TempDirectory() + L"playcast-lamps.txt");
    // Blocking WinRT waits belong on an MTA thread, not the STA UI thread.
    std::jthread worker([&outPath] {
        MtaScope apartment;
        try {
            pc::DynamicLightingController::RunLampDiagnostics(outPath);
        } catch (const std::exception& ex) {
            pc::LogInfo(std::string("lamps: failed: ") + ex.what());
        } catch (const winrt::hresult_error& e) {
            pc::LogInfo(L"lamps: failed: " + std::wstring(e.message()));
        }
    });
    worker.join();
    return 0;
}

/// Hidden dev flag: render each settings tab to PNGs and exit.
int RunSnapshot(HINSTANCE instance, const Options& options) {
    const std::wstring dir = options.snapshotDir.value_or(TempDirectory());
    pc::UniqueHandle snapshotEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    pc::TrayApp::SetHarnessMode(true);  // never touch the installed instance's lights/Discord
    pc::TrayApp app(instance, false, snapshotEvent.get());
    pc::MainWindow window(app);
    window.Show();
    if (HWND h = window.Handle(); h != nullptr)
        SetWindowPos(h, HWND_BOTTOM, -9000, -9000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    PumpMessages(500);
    if (options.snapshotLog) window.ImportLogs({std::filesystem::path(*options.snapshotLog)});
    window.SaveSnapshots(dir);
    PumpMessages(100);
    return 0;  // TrayApp's destructor hides the tray icon and releases everything
}

int RunApp(HINSTANCE instance, bool autostart) {
    // Local\ namespace => one instance per Windows session; instances in the
    // owner session and the guest session may coexist (that is intentional).
    HANDLE rawMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    const DWORD mutexError = GetLastError();
    pc::UniqueHandle mutex(rawMutex);
    if (!mutex.valid()) {
        pc::LogInfo(std::format(L"single-instance mutex unavailable (error {}); continuing", mutexError));
    } else if (mutexError == ERROR_ALREADY_EXISTS) {
        // Another instance already runs in this session: ask it to show its
        // settings window instead of starting a duplicate...
        pc::UniqueHandle existing(OpenEventW(EVENT_MODIFY_STATE, FALSE, kShowSettingsEventName));
        if (existing.valid())
            SetEvent(existing.get());
        // ...but also wait briefly in case it is actually shutting down (e.g.
        // a settings restart) and we should take over.
        const DWORD wait = WaitForSingleObject(mutex.get(), 3000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
            return 0;
    }

    pc::UniqueHandle showSettings(CreateEventW(nullptr, FALSE, FALSE, kShowSettingsEventName));
    if (!showSettings.valid())
        pc::LogInfo(std::format(L"show-settings event unavailable (error {})", GetLastError()));
    else
        ResetEvent(showSettings.get());  // drop a signal we sent to the exiting predecessor (restart handoff)

    int exitCode = 0;
    {
        // The HKLM Run entry starts us with --autostart (quiet). Any manual
        // launch without it opens the settings window (never in the guest
        // session - the app runs headless there).
        pc::TrayApp app(instance, !autostart, showSettings.get());
        exitCode = app.Run();
    }
    if (mutex.valid())
        ReleaseMutex(mutex.get());
    return exitCode;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    InstallCrashHandling();
    const Options options = ParseArgs();

    if (options.lamps)
        return RunLamps(options);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = static_cast<DWORD>(sizeof(icc));
    icc.dwICC = ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_UPDOWN_CLASS | ICC_LINK_CLASS;
    InitCommonControlsEx(&icc);

    StaScope apartment;
    GdiplusScope gdiplus;

    try {
        if (options.snapshot)
            return RunSnapshot(instance, options);
        return RunApp(instance, options.autostart);
    } catch (const std::exception& ex) {
        pc::LogInfo(std::string("fatal: ") + ex.what());
    } catch (const winrt::hresult_error& e) {
        pc::LogInfo(L"fatal: " + std::wstring(e.message()));
    } catch (...) {
        pc::LogInfo(L"fatal: unknown exception");
    }
    return 1;
}
