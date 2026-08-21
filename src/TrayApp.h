#pragma once
// Orchestration: owns config, the six lighting backends, Discord presence (and
// the optional shim/relay), the session + registry watchers, the tray icon and
// a hidden message window, and an evaluation worker. Behaviour mirrors the C#
// TrayAppContext exactly (same log lines where practical):
//  - Headless when the process user == TargetUsername: no tray icon, no window,
//    no Discord; lighting still runs.
//  - Evaluate(reason): guestActive = session watcher || registry watcher. When
//    active: StartBlackout on every Enabled backend (StopBlackout any holding
//    but now-disabled one); Discord SetActive. When inactive: StopBlackout all
//    holding backends; Discord Clear. Update tray icon (bright/dimmed) + tooltip
//    "Playcast Companion — guest hosting, stealth on" / "— all quiet".
//    Evaluations are serialized on one worker thread.
//  - 30 s safety-net poll; re-evaluate on WM_WTSSESSION_CHANGE and registry
//    change; on WM_ENDSESSION / exit, stop every backend and clear presence.
//  - A thread waits on showSettingsEvent and calls OpenSettings() (a second
//    launch of the exe sets it instead of starting a duplicate).
//  - Tray menu: "Open Playcast Companion" (bold, default; also double-click),
//    "Preview guest mode (10 s)", "Check again now", separator, "Exit".
#include "Config.h"
#include "DiscordPresence.h"
#include "LightingBackend.h"
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace pc {
class MainWindow;

class TrayApp {
public:
    TrayApp(HINSTANCE instance, bool openSettings, HANDLE showSettingsEvent);
    ~TrayApp();
    TrayApp(const TrayApp&) = delete;
    TrayApp& operator=(const TrayApp&) = delete;

    /// Runs the message loop until Exit(); returns the process exit code.
    int Run();

    // --- state for the UI ---
    HINSTANCE Instance() const;
    const AppConfig& Config() const;
    AppConfig& MutableConfig();      // UI edits then calls SaveConfig()
    const std::vector<std::unique_ptr<ILightingBackend>>& Lighting() const;  // order: Chroma, SteelSeries, Logitech, Corsair, OpenRGB, DynamicLighting
    DiscordPresence* Discord() const;  // nullptr when headless
    bool GuestActive() const;
    bool IsHeadless() const;
    HICON AppIcon() const;           // bright icon for window use (owned by TrayApp)

    // --- actions ---
    void RequestEvaluate(std::wstring reason);
    void TestGuestMode();            // 10 s preview of lights-off + presence, then re-evaluate
    void OpenSettings();
    void OpenLog();
    /// Write Config() to ConfigPath(); if access is denied (Program Files) write a
    /// temp file and elevate a `cmd /c copy` via ShellExecute "runas". Returns
    /// true on success; false (with a user-readable reason) on failure/cancel.
    bool SaveConfig(std::wstring& error);
    void RestartApp();               // relaunch self, then Exit()
    void Exit();

    /// Dev harness (--snapshot): set before constructing a TrayApp. Evaluations
    /// still compute GuestActive() for the UI but never start lighting
    /// blackouts, Discord presence, the shim or the relay listener.
    static void SetHarnessMode(bool enabled);

private:  // module owner may extend
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace pc
