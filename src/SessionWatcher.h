#pragma once
// Answers "is the guest account logged on?" and raises a callback on any WTS
// session change. The owner supplies a window: the watcher calls
// WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_ALL_SESSIONS) and the owner's
// window procedure must forward every message to HandleMessage(); the watcher
// fires onChanged for WM_WTSSESSION_CHANGE. Re-evaluating the full session list
// on every event keeps this stateless and immune to missed notifications.
#include "Config.h"
#include <functional>
#include <string_view>
#include <windows.h>

namespace pc {
class SessionWatcher {
public:
    SessionWatcher(const AppConfig& cfg, HWND notifyWindow, std::function<void()> onChanged);
    ~SessionWatcher();
    SessionWatcher(const SessionWatcher&) = delete;
    SessionWatcher& operator=(const SessionWatcher&) = delete;

    /// True if this process runs as the target account, or WTSEnumerateSessions
    /// + WTSQuerySessionInformation(WTSUserName) finds the target account in an
    /// Active/Connected (or, if configured, Disconnected) session. Case-insensitive.
    bool IsTargetUserLoggedOn() const;

    /// Forward from the window procedure. Returns true if the message was consumed.
    bool HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    /// Current process user name equals `name` (case-insensitive).
    static bool CurrentUserIs(std::wstring_view name);
    static std::wstring CurrentUserName();

private:  // module owner may extend
    // Snapshotted at construction (mirrors SessionWatcher.cs): the UI thread may
    // mutate AppConfig in place on Save while the eval thread polls us.
    std::wstring targetUser_;
    bool includeDisconnected_ = false;
    HWND hwnd_;
    std::function<void()> onChanged_;
    bool registered_ = false;
};
}  // namespace pc
