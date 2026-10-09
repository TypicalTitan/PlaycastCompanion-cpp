#include "pch.h"
#include "MainWindowInternal.h"
#include "TrayApp.h"
#include "Log.h"

namespace pc {
namespace {
void PaintCombo(HWND window, HDC dc) {
    RECT client{}; GetClientRect(window, &client); ui::Fill(dc, client, ui::Surface);
    ui::GdiOwner<HBRUSH> border(CreateSolidBrush(GetFocus() == window ? ui::Gold : ui::Border)); FrameRect(dc, &client, border.Get());
    const auto selected = SendMessageW(window, CB_GETCURSEL, 0, 0);
    const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    if (selected >= 0) {
        const auto length = SendMessageW(window, CB_GETLBTEXTLEN, selected, 0);
        if (length >= 0 && length < 512) {
            std::wstring text(static_cast<size_t>(length) + 1, L'\0'); SendMessageW(window, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(text.data()));
            text.resize(static_cast<size_t>(length)); auto rectangle = client; rectangle.left += 8; rectangle.right -= 24;
            ui::DrawText(dc, text, rectangle, font, IsWindowEnabled(window) ? ui::Text : ui::Dim);
        }
    }
    ui::DrawText(dc, L"⌄", {client.right - 22, 0, client.right - 3, client.bottom}, font, ui::Dim, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
LRESULT CALLBACK ComboProc(HWND window, UINT message, WPARAM word, LPARAM parameter, UINT_PTR id, DWORD_PTR) {
    if (message == WM_PAINT) { PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint); PaintCombo(window, dc); EndPaint(window, &paint); return 0; }
    if (message == WM_PRINT || message == WM_PRINTCLIENT) { PaintCombo(window, reinterpret_cast<HDC>(word)); return 0; }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ComboProc, id);
    const auto result = DefSubclassProc(window, message, word, parameter);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == CB_SETCURSEL || message == WM_ENABLE) InvalidateRect(window, nullptr, FALSE);
    return result;
}
}
HWND MainWindow::Impl::Add(int id, int page, const wchar_t* kind, const std::wstring& text,
    int x, int y, int width, int height, DWORD style) {
    const auto parent = page >= 0 ? pages[page] : hwnd;
    auto window = CreateWindowExW(0, kind, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app.Instance(), nullptr);
    if (!window) throw std::runtime_error("Cannot create settings control");
    controls.emplace(id, Control{window, page, x, y, width, height});
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(baseFont.Get()), FALSE);
    SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
    if (wcscmp(kind, L"COMBOBOX") == 0) SetWindowSubclass(window, ComboProc, 1, 0);
    SetWindowSubclass(window, ScrollControlProc, 2, reinterpret_cast<DWORD_PTR>(this));
    return window;
}
HWND MainWindow::Impl::Label(int page, const std::wstring& text, int x, int y, int width, int height) {
    return Add(1000 + static_cast<int>(controls.size()), page, L"STATIC", text, x, y, width, height, SS_LEFT | SS_NOPREFIX);
}
HWND MainWindow::Impl::Button(int id, int page, const std::wstring& text, int x, int y, int width, bool gold) {
    const auto window = Add(id, page, L"BUTTON", text, x, y, width, 34, BS_OWNERDRAW | WS_TABSTOP);
    controls.at(id).Gold = gold; return window;
}
HWND MainWindow::Impl::Check(int id, int page, const std::wstring& text, int x, int y, int width) {
    const auto window = Button(id, page, text, x, y, width);
    controls.at(id).Check = true; return window;
}
HWND MainWindow::Impl::Edit(int id, int page, int x, int y, int width, bool numeric) {
    auto window = Add(id, page, L"EDIT", L"", x, y, width, 32, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | (numeric ? ES_NUMBER : 0));
    SendMessageW(window, EM_SETLIMITTEXT, 512, 0); return window;
}
void MainWindow::Impl::SetDirty(bool value) {
    dirty = value; EnableWindow(controls.at(ui::Save).Window, value); EnableWindow(controls.at(ui::Discard).Window, value);
    SetWindowTextW(feedback, value ? L"Unsaved changes" : L"Settings saved");
    InvalidateRect(controls.at(ui::Save).Window, nullptr, TRUE); InvalidateRect(controls.at(ui::Discard).Window, nullptr, TRUE);
}
void MainWindow::Impl::LoadSettings(bool fromRuntime) {
    loading = true; if (fromRuntime) draft = app.Config(); savedDraft = draft; savedJson = draft.ToJson();
    auto check = [&](int id, bool checked) { controls.at(id).Checked = checked; InvalidateRect(controls.at(id).Window, nullptr, TRUE); };
    auto text = [&](int id, const std::wstring& value) { SetWindowTextW(controls.at(id).Window, value.c_str()); };
    check(ui::Razer, draft.RazerEnabled); check(ui::Steel, draft.SteelSeries.Enabled); check(ui::Logitech, draft.Logitech.Enabled);
    check(ui::Corsair, draft.Corsair.Enabled); check(ui::OpenRgb, draft.OpenRgb.Enabled); check(ui::Dynamic, draft.DynamicLighting.Enabled);
    check(ui::ExcludeVendor, draft.DynamicLighting.ExcludeVendorOwnedDevices);
    check(ui::DiscordEnabled, draft.Discord.Enabled); check(ui::Passthrough, draft.Discord.GamePassthroughEnabled);
    check(ui::Disconnected, draft.IncludeDisconnectedSessions); check(ui::Registry, draft.RegistryWatch.Enabled);
    check(ui::LogEnabled, draft.SessionLogging.Enabled); check(ui::RawSecrets, app.SessionLogIncludeSecrets());
    text(ui::Tick, std::to_wstring(draft.TickSeconds)); text(ui::Profile, draft.OpenRgb.BlackoutProfile);
    text(ui::OpenRgbHost, draft.OpenRgb.Host); text(ui::OpenRgbPort, std::to_wstring(draft.OpenRgb.Port));
    text(ui::LineOne, draft.Discord.Details); text(ui::LineTwo, draft.Discord.State); text(ui::HostTemplate, draft.Discord.HostingTemplate);
    text(ui::Username, draft.TargetUsername); text(ui::SnapshotInterval, std::to_wstring(draft.SessionLogging.SnapshotIntervalSeconds));
    text(ui::Retention, std::to_wstring(draft.SessionLogging.RetentionDays));
    text(ui::FileBudget, std::to_wstring(draft.SessionLogging.MaxFileBytes / (1024 * 1024)));
    text(ui::SessionBudget, std::to_wstring(draft.SessionLogging.MaxSessionBytes / (1024 * 1024)));
    SetWindowTextW(discordPreview, (L"Playing Playcast\r\n" + draft.Discord.Details + L"\r\n" + draft.Discord.State).c_str());
    loading = false; SetDirty(false);
}
void MainWindow::Impl::ReadSettings() {
    if (loading) return;
    auto checked = [&](int id) { return controls.at(id).Checked; };
    auto text = [&](int id) { return ui::Trim(ui::ReadText(controls.at(id).Window)); };
    auto number = [&](int id, int fallback) { try { return std::stoi(text(id)); } catch (...) { return fallback; } };
    draft.RazerEnabled = checked(ui::Razer); draft.SteelSeries.Enabled = checked(ui::Steel); draft.Logitech.Enabled = checked(ui::Logitech);
    draft.Corsair.Enabled = checked(ui::Corsair); draft.OpenRgb.Enabled = checked(ui::OpenRgb); draft.DynamicLighting.Enabled = checked(ui::Dynamic);
    draft.DynamicLighting.ExcludeVendorOwnedDevices = checked(ui::ExcludeVendor);
    draft.TickSeconds = number(ui::Tick, 5); draft.OpenRgb.BlackoutProfile = text(ui::Profile);
    draft.OpenRgb.Host = text(ui::OpenRgbHost); draft.OpenRgb.Port = number(ui::OpenRgbPort, 6742);
    draft.Discord.Enabled = checked(ui::DiscordEnabled); draft.Discord.GamePassthroughEnabled = checked(ui::Passthrough);
    draft.Discord.Details = text(ui::LineOne); draft.Discord.State = text(ui::LineTwo); draft.Discord.HostingTemplate = text(ui::HostTemplate);
    draft.TargetUsername = text(ui::Username); draft.IncludeDisconnectedSessions = checked(ui::Disconnected);
    draft.RegistryWatch.Enabled = checked(ui::Registry); draft.SessionLogging.Enabled = checked(ui::LogEnabled);
    draft.SessionLogging.SnapshotIntervalSeconds = number(ui::SnapshotInterval, 30);
    draft.SessionLogging.RetentionDays = number(ui::Retention, 14);
    draft.SessionLogging.MaxFileBytes = static_cast<int64_t>(std::clamp(number(ui::FileBudget, 10), 1, 50)) * 1024 * 1024;
    draft.SessionLogging.MaxSessionBytes = static_cast<int64_t>(std::clamp(number(ui::SessionBudget, 100), 1, 1024)) * 1024 * 1024;
    SetDirty(draft.ToJson() != savedJson);
    SetWindowTextW(discordPreview, (L"Playing Playcast\r\n" + (draft.Discord.Details.empty() ? L"Status line is empty" : draft.Discord.Details)
        + L"\r\n" + draft.Discord.State).c_str());
}
void MainWindow::Impl::SaveSettings() {
    ReadSettings();
    if (draft.TargetUsername.empty()) { SetWindowTextW(feedback, L"Enter the guest account name before saving."); return; }
    wchar_t current[257]{}; DWORD size = static_cast<DWORD>(std::size(current));
    if (GetUserNameW(current, &size) && CompareStringOrdinal(current, -1, draft.TargetUsername.c_str(), -1, TRUE) == CSTR_EQUAL
        && MessageBoxW(hwnd, L"This is your current Windows account. Lighting would stay blacked out while you are signed in. Save anyway?",
            L"Guest account", MB_YESNO | MB_ICONWARNING) != IDYES) return;
    draft.Normalize(); app.MutableConfig() = draft;
    std::wstring error;
    if (!app.SaveConfig(error)) { SetWindowTextW(feedback, (L"Could not save: " + error).c_str()); return; }
    LoadSettings(false); SetWindowTextW(feedback, L"Saved. Changes are applying to the app.");
    LogInfo(L"settings saved from sidebar UI");
}
void MainWindow::Impl::Command(int id, int notification) {
    if (loading) return;
    if (id >= ui::NavBase && id < ui::NavBase + static_cast<int>(ui::PageCount)) { SelectPage(id - ui::NavBase); return; }
    if (id == ui::AnalysisList && notification == LBN_SELCHANGE) { SelectAnalysisRow(); return; }
    if ((id == ui::AnalysisView || id == ui::AnalysisFilter) && notification == CBN_SELCHANGE) {
        if (id == ui::AnalysisView) analysisTab = static_cast<int>(SendMessageW(controls.at(id).Window, CB_GETCURSEL, 0, 0));
        analysisOffset = 0; RefreshAnalysis(); LayoutAnalysis(); InvalidateRect(pages[ui::AnalyzeLogs], nullptr, TRUE); return;
    }
    if (id == ui::NextPage || id == ui::PreviousPage) { analysisOffset = id == ui::NextPage ? analysisOffset + 500 : analysisOffset >= 500 ? analysisOffset - 500 : 0; RefreshAnalysis(); return; }
    if (id == ui::ImportFiles || id == ui::ImportFolder) { Import(id == ui::ImportFolder); return; }
    if (id == ui::LoadSample) {
        if (importing) return;
        auto sample = std::make_unique<TemporaryLogSample>(app.Instance()); const auto path = sample->Path();
        BeginImport({path}, std::move(sample)); return;
    }
    if (id == ui::Save) { SaveSettings(); return; }
    if (id == ui::Discard) { draft = savedDraft; LoadSettings(false); return; }
    if (id == ui::OpenLog) { app.OpenLog(); return; }
    if (id == ui::OpenSessions) { app.OpenSessionLogFolder(); return; }
    if (id == ui::OpenInstall) { const auto directory = std::filesystem::path(AppConfig::ConfigPath()).parent_path().wstring(); ShellExecuteW(hwnd, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return; }
    if (id == ui::Preview) {
        previewing = true; app.TestGuestMode(); EnableWindow(controls.at(id).Window, FALSE);
        SetWindowTextW(controls.at(id).Window, L"Previewing... (not a session)"); SetTimer(hwnd, 2, 10000, nullptr); return;
    }
    if (id == ui::AdvancedLighting) { advanced = !advanced; Layout(); InvalidateRect(pages[ui::Lighting], nullptr, TRUE); return; }
    auto found = controls.find(id);
    if (found != controls.end() && found->second.Check && notification == BN_CLICKED) {
        if (id == ui::RawSecrets) {
            const bool enable = !found->second.Checked;
            if (enable && MessageBoxW(hwnd, L"Raw capture may contain account credentials, script secrets and tokens. It is local and lasts only until this app exits. Enable it?",
                L"Raw session capture", MB_YESNO | MB_ICONWARNING) != IDYES) return;
            found->second.Checked = enable; app.SetSessionLogIncludeSecrets(enable);
            InvalidateRect(found->second.Window, nullptr, TRUE); return;
        }
        found->second.Checked = !found->second.Checked; InvalidateRect(found->second.Window, nullptr, TRUE); ReadSettings(); return;
    }
    if (notification == EN_CHANGE && found != controls.end() && id >= ui::Tick && id <= ui::OpenRgbPort) ReadSettings();
}
} // namespace pc
