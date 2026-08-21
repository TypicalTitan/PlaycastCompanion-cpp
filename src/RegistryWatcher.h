#pragma once
// Optional secondary guest-mode signal: watches a registry key via
// RegNotifyChangeKeyValue (REG_NOTIFY_CHANGE_NAME | LAST_SET, async with an
// event) on a background thread; polls every 5 s while the key doesn't exist.
// onChanged fires only when the computed IsActive() answer flips.
#include "Config.h"
#include <atomic>
#include <functional>
#include <stop_token>
#include <thread>
#include <windows.h>

namespace pc {
class RegistryWatcher {
public:
    RegistryWatcher(const RegistryWatchConfig& cfg, std::function<void()> onChanged);
    ~RegistryWatcher();  // stops the thread (returns within ~2 s)
    RegistryWatcher(const RegistryWatcher&) = delete;
    RegistryWatcher& operator=(const RegistryWatcher&) = delete;

    /// ValueName empty: key exists => active. Else value exists and
    /// (ActiveValue empty ? data not "" and not "0" : data == ActiveValue,
    /// case-insensitive string compare of the value rendered as text).
    bool IsActive() const;

private:  // module owner may extend
    const RegistryWatchConfig& cfg_;
    std::function<void()> onChanged_;
    std::jthread thread_;
    HANDLE stopEvent_ = nullptr;

    void WatchLoop(std::stop_token st);
    void RaiseIfChanged(bool& lastActive);
};
}  // namespace pc
