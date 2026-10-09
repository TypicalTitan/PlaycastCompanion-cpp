#include "pch.h"
#include "TrayAppInternal.h"

namespace pc {
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


}
