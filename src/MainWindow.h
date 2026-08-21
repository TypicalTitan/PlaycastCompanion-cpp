#pragma once
// The settings window: raw Win32 + GDI+, dark themed, fixed 584x568 client,
// mirroring the C# MainForm exactly:
//  - Navy header (84 px): generated app mark at left, "Playcast Companion"
//    title (Segoe UI Semibold 15pt, white), subtitle "Lights out and status up
//    while guests play on your PC." (light grey).
//  - Owner-drawn dark tab strip: Status | Lighting | Discord | Advanced; active
//    tab = UiSurface fill + 2 px Gold underline + white text; inactive = dim.
//  - Status tab: summary line (green "All quiet — your PC is yours." / amber
//    "A guest is hosting — stealth mode is on."), four dot rows (Guest mode,
//    Lighting, Discord, Starts with Windows — HKLM/HKCU Run value
//    "PlaycastCompanion"), "Preview guest mode (10 s)" button. Refresh 1 s.
//  - Lighting tab: vertically scrollable stack of six brand cards (506x130):
//    Razer (black, green "Razer", re-assert seconds spinner 1..10),
//    SteelSeries (orange), Logitech G (blue), Corsair (yellow), OpenRGB
//    (with a "blackout profile" edit), Windows Dynamic Lighting (WHITE card,
//    Windows blue). Each: drawn mark, name, tagline, status dot + text ("Off" /
//    "Ready" / backend StatusText; amber dot if it contains retry/Unreachable/
//    not found/error), "Include in guest blackout" checkbox.
//  - Discord tab: enable checkbox, a dark preview panel ("Playing Playcast",
//    live status line, "12:34 elapsed"), "Status line", "Second line
//    (optional)", "Hosting template" edits, "Pass the guest's current game
//    through (...)" checkbox, and a read-only "Linked to your Discord app ✓
//    (the ID lives in config.json)" / "Not linked yet — add your Application ID
//    to config.json (see README)" line. ApplicationId is NEVER editable here.
//  - Advanced tab: "Guest account name" edit + blurb, "Stay stealthy while the
//    guest is disconnected but still signed in" and "Also watch the Playcast
//    GuestMode registry key (extra signal)" checkboxes, "Open log" / "Open
//    install folder" buttons, "Playcast Companion v3.x.y" label.
//  - Bottom bar: gold "Save changes" button at right, feedback label at left.
//    Save validates (non-empty account; warn if it equals the current user),
//    writes all UI-editable fields into MutableConfig(), calls
//    TrayApp::SaveConfig, then offers a restart (MessageBox Yes/No).
//  - DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE) for the title bar;
//    controls themed dark (WM_CTLCOLOR*, owner-draw buttons, custom-drawn
//    checkboxes); Per-Monitor-V2 DPI aware (scale all metrics by DPI).
//  - SaveSnapshots(dir): for each tab (and the Lighting tab scrolled to the
//    bottom as "-scrolled"), render the window to tab-<i>-<name>.png via
//    PrintWindow + GDI+ PNG encoder. Used by the --snapshot dev flag.
#include <string>
#include <windows.h>

namespace pc {
class TrayApp;

class MainWindow {
public:
    explicit MainWindow(TrayApp& app);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    void Show();       // create if needed, show, activate
    void Activate();   // restore from minimized + bring to front
    bool IsAlive() const;
    HWND Handle() const;
    void SaveSnapshots(const std::wstring& dir);

private:  // module owner may extend
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace pc
