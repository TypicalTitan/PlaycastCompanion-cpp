#include "pch.h"
#include "TrayAppInternal.h"

namespace pc {
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

std::wstring TrayApp::SessionLogStatus() const { return impl_->sessionLogger ? impl_->sessionLogger->Status() : L"Session logger unavailable."; }
std::wstring TrayApp::SessionLogDirectory() const { return impl_->sessionLogger ? impl_->sessionLogger->Directory() : L""; }
bool TrayApp::SessionLogIncludeSecrets() const { return impl_->sessionLogger && impl_->sessionLogger->IncludeSecrets(); }
void TrayApp::SetSessionLogIncludeSecrets(bool enabled) { if (impl_->sessionLogger) impl_->sessionLogger->SetIncludeSecrets(enabled); }
void TrayApp::OpenSessionLogFolder() {
    const auto folder = SessionLogDirectory();
    if (!folder.empty() && std::filesystem::is_directory(folder))
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}
