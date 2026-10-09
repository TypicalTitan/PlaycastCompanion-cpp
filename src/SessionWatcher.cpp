#include "pch.h"
#include "SessionWatcher.h"

#include "Log.h"

#include <lmcons.h>

namespace pc {
namespace {

/// RAII for buffers returned by WTSEnumerateSessionsW / WTSQuerySessionInformationW.
struct WtsMemory {
    void* p = nullptr;
    WtsMemory() = default;
    WtsMemory(const WtsMemory&) = delete;
    WtsMemory& operator=(const WtsMemory&) = delete;
    ~WtsMemory() {
        if (p) WTSFreeMemory(p);
    }
};

/// Mirrors the C# CountsAsLoggedOn(): Active/Connected always, Disconnected only
/// when IncludeDisconnectedSessions is set.
bool CountsAsLoggedOn(WTS_CONNECTSTATE_CLASS state, bool includeDisconnected) {
    return state == WTSActive || state == WTSConnected || (includeDisconnected && state == WTSDisconnected);
}

/// WTSQuerySessionInformationW(WTSUserName); nullopt when the query fails.
std::optional<std::wstring> GetSessionUser(DWORD sessionId) {
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSUserName, &buffer, &bytes)) return std::nullopt;
    WtsMemory guard;
    guard.p = buffer;
    if (!buffer) return std::wstring();
    return std::wstring(buffer);  // NUL-terminated per the WTS API contract
}

/// StringComparison.OrdinalIgnoreCase equivalent. _wcsicmp under the static CRT
/// (no setlocale) folds only A-Z, so non-ASCII account names would not match.
bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size() || a.size() > static_cast<size_t>(INT_MAX)) return false;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

}  // namespace

SessionWatcher::SessionWatcher(const AppConfig& cfg, HWND notifyWindow, std::function<void()> onChanged)
    : targetUser_(cfg.TargetUsername),
      includeDisconnected_(cfg.IncludeDisconnectedSessions),
      hwnd_(notifyWindow),
      onChanged_(std::move(onChanged)) {
    registered_ = WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_ALL_SESSIONS) != FALSE;
    if (!registered_) {
        const DWORD error = GetLastError();
        LogInfo(std::format(L"WTSRegisterSessionNotification failed (error {}); relying on the periodic poll only", error));
    }
}

SessionWatcher::~SessionWatcher() {
    if (registered_) {
        WTSUnRegisterSessionNotification(hwnd_);
        registered_ = false;
    }
}

bool SessionWatcher::HandleMessage(UINT msg, WPARAM /*wParam*/, LPARAM /*lParam*/) {
    if (msg != WM_WTSSESSION_CHANGE) return false;
    // Never let an exception escape a window procedure.
    try {
        if (onChanged_) onChanged_();
    } catch (const std::exception& ex) {
        LogInfo(std::string("SessionWatcher: SessionsChanged handler threw: ") + ex.what());
    } catch (...) {
        LogInfo(L"SessionWatcher: SessionsChanged handler threw an unknown exception");
    }
    return true;
}

bool SessionWatcher::IsTargetUserLoggedOn() const {
    // If this instance itself runs under the target account, blackout applies
    // for the whole lifetime of the process.
    if (CurrentUserIs(targetUser_)) return true;

    PWTS_SESSION_INFOW sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return false;
    WtsMemory guard;
    guard.p = sessions;
    if (!sessions) return false;

    for (DWORD i = 0; i < count; ++i) {
        const WTS_SESSION_INFOW& info = sessions[i];
        if (!CountsAsLoggedOn(info.State, includeDisconnected_)) continue;
        const auto user = GetSessionUser(info.SessionId);
        if (!user) continue;
        if (EqualsIgnoreCase(*user, targetUser_)) return true;
    }
    return false;
}

bool SessionWatcher::CurrentUserIs(std::wstring_view name) {
    return EqualsIgnoreCase(CurrentUserName(), name);
}

std::wstring SessionWatcher::TargetSessionIdentity() const {
    PWTS_SESSION_INFOW sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return L"WTS-unavailable";
    WtsMemory memory;
    memory.p = sessions;
    std::vector<DWORD> ids;
    for (DWORD index = 0; index < count; ++index) {
        const auto user = GetSessionUser(sessions[index].SessionId);
        if (user && CountsAsLoggedOn(sessions[index].State, includeDisconnected_) && EqualsIgnoreCase(*user, targetUser_)) ids.push_back(sessions[index].SessionId);
    }
    std::sort(ids.begin(), ids.end());
    std::wstring identity = targetUser_;
    for (const auto id : ids) identity += L":" + std::to_wstring(id);
    return identity;
}

std::wstring SessionWatcher::CurrentUserName() {
    std::wstring buffer(UNLEN + 1, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!GetUserNameW(buffer.data(), &size)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return std::wstring();
        buffer.assign(size, L'\0');
        if (!GetUserNameW(buffer.data(), &size)) return std::wstring();
    }
    // `size` includes the terminating NUL on success.
    buffer.resize(size ? size - 1 : 0);
    return buffer;
}

}  // namespace pc
