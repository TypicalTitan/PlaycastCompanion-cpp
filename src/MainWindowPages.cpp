#include "pch.h"
#include "MainWindowInternal.h"
#include "TrayApp.h"
#include "DiscordPresence.h"
#include "LightingBackend.h"
#include "Branding.h"

namespace pc {
void MainWindow::Impl::BuildPages() {
    for (int index = 0; index < ui::PageCount; ++index) {
        contexts[index] = {this, index};
        pages[index] = CreateWindowExW(WS_EX_CONTROLPARENT, L"PlaycastCompanion.SidebarPage", L"", WS_CHILD | WS_CLIPCHILDREN | WS_VSCROLL,
            0, 0, 0, 0, hwnd, nullptr, app.Instance(), &contexts[index]);
        Button(ui::NavBase + index, -2, ui::PageNames[index], 0, 0, 0);
    }
    BuildOverview(); BuildLighting(); BuildDiscord(); BuildSessionLogs(); BuildAnalysis(); BuildSettings();
    Button(ui::Save, -1, L"Save changes", 0, 0, 136, true); Button(ui::Discard, -1, L"Discard", 0, 0, 98);
    feedback = Label(-1, L"Settings saved", 0, 0, 0, 30);
    SelectPage(ui::Overview);
}
void MainWindow::Impl::BuildOverview() {
    overviewStatus = Label(ui::Overview, L"Checking guest session...", 0, 96, -1, 32);
    Label(ui::Overview, L"GUEST SESSION", 0, 166, 180);
    Label(ui::Overview, L"Detected from the configured Windows account and optional registry signal.", 0, 192, -1, 44);
    Label(ui::Overview, L"Lighting", 0, 264, 160); overviewLighting = Label(ui::Overview, L"Checking...", 166, 264, -1, 44);
    Label(ui::Overview, L"Discord presence", 0, 326, 160); overviewDiscord = Label(ui::Overview, L"Checking...", 166, 326, -1, 44);
    Label(ui::Overview, L"Starts with Windows", 0, 388, 160); overviewStartup = Label(ui::Overview, L"Unknown", 166, 388, -1, 44);
    Button(ui::Preview, ui::Overview, L"Preview guest mode (10 s)", 0, 460, 245);
    Label(ui::Overview, L"The preview tests lighting and presence. It never creates a guest session or enables session capture.", 0, 510, -1, 52);
    contentHeights[ui::Overview] = 575;
}
void MainWindow::Impl::BuildLighting() {
    constexpr const wchar_t* names[] = {L"Razer Chroma", L"SteelSeries", L"Logitech G", L"Corsair", L"OpenRGB", L"Windows Dynamic Lighting"};
    for (int index = 0; index < 6; ++index) {
        Check(ui::Razer + index, ui::Lighting, names[index], 0, 94 + index * 58, 248);
        lightingStatus[index] = Label(ui::Lighting, L"Not observed", 264, 96 + index * 58, -1, 40);
    }
    Button(ui::AdvancedLighting, ui::Lighting, L"Advanced options", 0, 458, 180);
    Label(ui::Lighting, L"Razer re-assert interval (seconds, 1–10)", 0, 510, 360); Edit(ui::Tick, ui::Lighting, 380, 506, 70, true);
    Label(ui::Lighting, L"OpenRGB blackout profile", 0, 562, 235); Edit(ui::Profile, ui::Lighting, 250, 556, 280);
    Label(ui::Lighting, L"OpenRGB host / port", 0, 614, 235); Edit(ui::OpenRgbHost, ui::Lighting, 250, 608, 190); Edit(ui::OpenRgbPort, ui::Lighting, 454, 608, 76, true);
    Check(ui::ExcludeVendor, ui::Lighting, L"Dynamic Lighting: skip devices handled by enabled vendor engines", 0, 660, 570);
    contentHeights[ui::Lighting] = 710;
}
void MainWindow::Impl::BuildDiscord() {
    Check(ui::DiscordEnabled, ui::Discord, L"Show Discord presence during guest hosting", 0, 92, 560);
    discordStatus = Label(ui::Discord, L"Checking Discord configuration...", 0, 138, -1, 42);
    discordPreview = Label(ui::Discord, L"Playing Playcast", 20, 202, -1, 82);
    Label(ui::Discord, L"Status line", 0, 312, 180); Edit(ui::LineOne, ui::Discord, 0, 340, -1);
    Label(ui::Discord, L"Second line (optional)", 0, 392, 240); Edit(ui::LineTwo, ui::Discord, 0, 420, -1);
    Check(ui::Passthrough, ui::Discord, L"Pass the guest's current game through when observed", 0, 466, 570);
    Label(ui::Discord, L"Hosting template — use {game} for the captured game name", 0, 516, -1, 32); Edit(ui::HostTemplate, ui::Discord, 0, 552, -1);
    Label(ui::Discord, L"Game capture is partial. Discord Application ID remains a config.json setting.", 0, 608, -1, 50);
    contentHeights[ui::Discord] = 675;
}
void MainWindow::Impl::BuildSessionLogs() {
    Check(ui::LogEnabled, ui::SessionLogs, L"Automatically log active guest sessions", 0, 92, 540);
    loggingStatus = Label(ui::SessionLogs, L"Checking capture status...", 0, 144, -1, 82);
    Label(ui::SessionLogs, L"LOCAL CAPTURE FOLDER", 0, 248, -1);
    loggingPath = Label(ui::SessionLogs, L"", 0, 280, -1, 58);
    Button(ui::OpenSessions, ui::SessionLogs, L"Open capture folder", 0, 354, 190);
    Label(ui::SessionLogs, L"Realtime messages and complete PowerShell content require a Playcast host with the Companion diagnostic feed. Imported and captured scripts are never run by this app.", 0, 412, -1, 78);
    Check(ui::RawSecrets, ui::SessionLogs, L"Include raw secrets for this app run", 0, 512, 540);
    Label(ui::SessionLogs, L"Off by default; it resets when the app exits. Common credentials are redacted in normal capture. Arbitrary script output may still contain sensitive text.", 0, 558, -1, 76);
    contentHeights[ui::SessionLogs] = 650;
}
void MainWindow::Impl::BuildSettings() {
    Label(ui::Settings, L"Guest Windows account", 0, 98, 270); Edit(ui::Username, ui::Settings, 0, 132, 340);
    Check(ui::Disconnected, ui::Settings, L"Keep guest mode active while the guest is disconnected but signed in", 0, 186, 580);
    Check(ui::Registry, ui::Settings, L"Also use the configured Playcast registry signal", 0, 234, 580);
    Label(ui::Settings, L"SESSION CAPTURE LIMITS", 0, 306, 400);
    Label(ui::Settings, L"Snapshot interval (seconds, 5–600)", 0, 352, 370); Edit(ui::SnapshotInterval, ui::Settings, 390, 348, 100, true);
    Label(ui::Settings, L"Keep completed sessions (days, 1–365)", 0, 408, 370); Edit(ui::Retention, ui::Settings, 390, 404, 100, true);
    Label(ui::Settings, L"Maximum category file (MiB, 1–50)", 0, 464, 370); Edit(ui::FileBudget, ui::Settings, 390, 460, 100, true);
    Label(ui::Settings, L"Maximum session capture (MiB, up to 1024)", 0, 520, 385); Edit(ui::SessionBudget, ui::Settings, 390, 516, 100, true);
    Button(ui::OpenLog, ui::Settings, L"Open app log", 0, 586, 158); Button(ui::OpenInstall, ui::Settings, L"Open install folder", 174, 586, 182);
    Label(ui::Settings, L"Playcast Companion Native · Win32 + GDI+ · local logs only", 0, 650, -1, 42);
    contentHeights[ui::Settings] = 710;
}
void MainWindow::Impl::RefreshStatus() {
    if (!overviewStatus) return;
    const bool guest = app.GuestActive();
    SetWindowTextW(overviewStatus, guest ? L"Guest session detected" : L"All quiet — no guest session observed");
    const auto& lighting = app.Lighting(); size_t enabled = 0, holding = 0;
    for (size_t index = 0; index < lighting.size(); ++index) {
        const auto& backend = lighting[index]; if (!backend) continue;
        if (backend->Enabled()) ++enabled; if (backend->IsHolding()) ++holding;
        const auto state = !backend->Enabled() ? L"Disabled" : backend->IsHolding() ? backend->StatusText()
            : guest ? L"Enabled; waiting for blackout" : L"Enabled; idle (not tested)";
        if (index < lightingStatus.size()) SetWindowTextW(lightingStatus[index], state.c_str());
    }
    const auto lightState = enabled == 0 ? L"No lighting systems enabled" : guest
        ? std::format(L"{} enabled · {} blackout workers active", enabled, holding)
        : std::format(L"{} enabled · vendor software in control", enabled);
    SetWindowTextW(overviewLighting, lightState.c_str());
    const auto discord = app.Discord(); const auto presence = discord ? discord->StatusText() : L"Unavailable in this Windows session";
    SetWindowTextW(overviewDiscord, presence.c_str()); SetWindowTextW(discordStatus, presence.c_str());
    constexpr const wchar_t* run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    const auto machine = RegGetValueW(HKEY_LOCAL_MACHINE, run, L"PlaycastCompanion", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, nullptr);
    const auto user = RegGetValueW(HKEY_CURRENT_USER, run, L"PlaycastCompanion", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, nullptr, nullptr);
    const auto startup = machine == ERROR_SUCCESS ? L"On for every account" : user == ERROR_SUCCESS ? L"On for this account"
        : machine == ERROR_FILE_NOT_FOUND && user == ERROR_FILE_NOT_FOUND ? L"Off — use the installer to enable" : L"Unknown (registry unavailable)";
    SetWindowTextW(overviewStartup, startup);
    SetWindowTextW(loggingStatus, app.SessionLogStatus().c_str()); SetWindowTextW(loggingPath, app.SessionLogDirectory().c_str());
}
void MainWindow::Impl::PaintMain(HDC dc, RECT client) {
    ui::Fill(dc, client, ui::Back);
    const int width = MulDiv(client.right, 96, static_cast<int>(dpi)); const int sidebar = width < 820 ? 150 : 174;
    ui::Fill(dc, {0, 0, Scale(sidebar), client.bottom - Scale(58)}, ui::Nav);
    ui::Fill(dc, {Scale(sidebar), 0, Scale(sidebar + 1), client.bottom - Scale(58)}, ui::Border);
    ui::Fill(dc, {0, client.bottom - Scale(59), client.right, client.bottom - Scale(58)}, ui::Border);
    Gdiplus::Graphics graphics(dc); std::unique_ptr<Gdiplus::Bitmap> mark(brand::MakeAppMark(Scale(32)));
    if (mark) graphics.DrawImage(mark.get(), Scale(18), Scale(22), Scale(32), Scale(32));
    ui::DrawText(dc, L"Companion", {Scale(18), Scale(58), Scale(sidebar - 8), Scale(83)}, baseFont.Get(), ui::Text);
    ui::DrawText(dc, L"Native", {Scale(18), Scale(81), Scale(sidebar - 8), Scale(102)}, smallFont.Get(), ui::Dim);
}
void MainWindow::Impl::PaintPage(int page, HDC dc, RECT client) {
    ui::Fill(dc, client, ui::Back); const int offset = Scale(offsets[page]);
    ui::DrawText(dc, ui::PageNames[page], {0, Scale(0) - offset, client.right - Scale(10), Scale(34) - offset}, titleFont.Get(), ui::Text);
    constexpr const wchar_t* subtitles[] = {L"A clear view of the current guest session.", L"Choose which systems join guest blackout.",
        L"Presence while guests play on your PC.", L"Automatic local records of an actual guest session.",
        L"Import captures to explore events, scripts and performance.", L"Guest detection and capture limits."};
    ui::DrawText(dc, subtitles[page], {0, Scale(40) - offset, client.right - Scale(10), Scale(77) - offset}, smallFont.Get(), ui::Dim, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    if (page == ui::Lighting) {
        for (int index = 0; index < 6; ++index) ui::Fill(dc, {0, Scale(140 + index * 58) - offset, client.right - Scale(10), Scale(141 + index * 58) - offset}, ui::Border);
    }
    if (page == ui::Discord) ui::Fill(dc, {0, Scale(188) - offset, client.right - Scale(10), Scale(298) - offset}, ui::Surface);
    if (page == ui::AnalyzeLogs && (analysisTab == ui::Timeline || analysisTab == ui::Performance))
        PaintAnalysisGraph(dc, {0, Scale(204) - offset, client.right - Scale(10), Scale(374) - offset});
}
void MainWindow::Impl::DrawButton(const DRAWITEMSTRUCT& item) {
    if (item.CtlType == ODT_COMBOBOX) {
        ui::Fill(item.hDC, item.rcItem, item.itemState & ODS_SELECTED ? RGB(42, 53, 69) : ui::Surface);
        const auto index = item.itemID == static_cast<UINT>(-1) ? SendMessageW(item.hwndItem, CB_GETCURSEL, 0, 0) : item.itemID;
        if (index >= 0) {
            wchar_t text[128]{}; SendMessageW(item.hwndItem, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text));
            auto rectangle = item.rcItem; rectangle.left += Scale(6);
            ui::DrawText(item.hDC, text, rectangle, baseFont.Get(), ui::Text);
        }
        return;
    }
    auto found = controls.find(static_cast<int>(item.CtlID)); if (found == controls.end()) return;
    const auto& control = found->second; auto rectangle = item.rcItem;
    const bool selected = item.CtlID >= ui::NavBase && item.CtlID < ui::NavBase + static_cast<int>(ui::PageCount)
        && static_cast<int>(item.CtlID) - ui::NavBase == selectedPage;
    const bool navigation = item.CtlID >= ui::NavBase && item.CtlID < ui::NavBase + static_cast<int>(ui::PageCount);
    const bool enabled = (item.itemState & ODS_DISABLED) == 0;
    const auto background = control.Check ? ui::Back : control.Gold && enabled ? ui::Gold : selected ? RGB(42, 53, 69) : navigation ? ui::Nav : ui::Surface;
    ui::Fill(item.hDC, rectangle, background);
    if (selected) ui::Fill(item.hDC, {rectangle.left, rectangle.top, rectangle.left + Scale(3), rectangle.bottom}, ui::Gold);
    if (!navigation && !control.Check && !control.Gold) { ui::GdiOwner<HBRUSH> border(CreateSolidBrush(ui::Border)); FrameRect(item.hDC, &rectangle, border.Get()); }
    if (control.Check) {
        const RECT box{Scale(2), Scale(9), Scale(19), Scale(26)};
        ui::Fill(item.hDC, box, control.Checked ? ui::Gold : ui::Surface);
        ui::GdiOwner<HBRUSH> border(CreateSolidBrush(ui::Border)); FrameRect(item.hDC, &box, border.Get());
        if (control.Checked) ui::DrawText(item.hDC, L"✓", box, baseFont.Get(), ui::Back, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        rectangle.left += Scale(30);
    } else rectangle.left += Scale(navigation ? 14 : 4);
    auto foreground = !enabled ? ui::Dim : control.Gold ? ui::Back : ui::Text;
    ui::DrawText(item.hDC, ui::ReadText(item.hwndItem), rectangle, baseFont.Get(), foreground,
        (control.Check || navigation ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (item.itemState & ODS_FOCUS) { InflateRect(&rectangle, -2, -2); DrawFocusRect(item.hDC, &rectangle); }
}
} // namespace pc
