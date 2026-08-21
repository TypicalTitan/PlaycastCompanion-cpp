#include "pch.h"
#include "RegistryWatcher.h"

#include "Log.h"

namespace pc {
namespace {

/// RAII for an HKEY opened with RegOpenKeyExW (RegCloseKey). Null-safe.
struct UniqueKey {
    HKEY key = nullptr;
    UniqueKey() = default;
    explicit UniqueKey(HKEY k) : key(k) {}
    UniqueKey(const UniqueKey&) = delete;
    UniqueKey& operator=(const UniqueKey&) = delete;
    ~UniqueKey() {
        if (key) RegCloseKey(key);
    }
};

/// "HKCU" / "HKEY_CURRENT_USER" (case-insensitive) => HKEY_CURRENT_USER, anything
/// else => HKEY_LOCAL_MACHINE. Mirrors the C# BaseKey()/HiveHandle().
HKEY HiveHandle(const std::wstring& hive) {
    if (_wcsicmp(hive.c_str(), L"HKCU") == 0 || _wcsicmp(hive.c_str(), L"HKEY_CURRENT_USER") == 0) return HKEY_CURRENT_USER;
    return HKEY_LOCAL_MACHINE;
}

/// Renders registry data the way .NET's RegistryKey.GetValue(...).ToString() does:
/// REG_DWORD -> Int32, REG_QWORD -> Int64, REG_SZ/REG_EXPAND_SZ -> the string,
/// REG_MULTI_SZ -> "System.String[]", everything else (incl. size-mismatched
/// numerics) -> "System.Byte[]".
std::wstring RenderValue(DWORD type, const std::vector<BYTE>& data, DWORD size) {
    switch (type) {
        case REG_DWORD: {
            if (size != sizeof(std::int32_t)) return L"System.Byte[]";
            std::int32_t v = 0;
            std::memcpy(&v, data.data(), sizeof(v));
            return std::to_wstring(v);
        }
        case REG_QWORD: {
            if (size != sizeof(std::int64_t)) return L"System.Byte[]";
            std::int64_t v = 0;
            std::memcpy(&v, data.data(), sizeof(v));
            return std::to_wstring(v);
        }
        case REG_SZ:
        case REG_EXPAND_SZ: {
            const size_t chars = size / sizeof(wchar_t);
            std::wstring text(chars, L'\0');
            if (chars) std::memcpy(text.data(), data.data(), chars * sizeof(wchar_t));
            const size_t nul = text.find(L'\0');
            if (nul != std::wstring::npos) text.resize(nul);
            return text;
        }
        case REG_MULTI_SZ:
            return L"System.String[]";
        default:
            return L"System.Byte[]";
    }
}

}  // namespace

RegistryWatcher::RegistryWatcher(const RegistryWatchConfig& cfg, std::function<void()> onChanged)
    : cfg_(cfg), onChanged_(std::move(onChanged)) {
    stopEvent_ = CreateEventW(nullptr, TRUE /*manual reset*/, FALSE, nullptr);
    if (!stopEvent_) {
        LogInfo(std::format(L"RegistryWatcher: CreateEvent failed (error {}); registry watch disabled", GetLastError()));
        return;
    }
    thread_ = std::jthread([this](std::stop_token st) { WatchLoop(st); });
}

RegistryWatcher::~RegistryWatcher() {
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    if (stopEvent_) {
        CloseHandle(stopEvent_);
        stopEvent_ = nullptr;
    }
}

bool RegistryWatcher::IsActive() const {
    try {
        HKEY raw = nullptr;
        if (RegOpenKeyExW(HiveHandle(cfg_.Hive), cfg_.SubKey.c_str(), 0, KEY_QUERY_VALUE, &raw) != ERROR_SUCCESS) return false;
        UniqueKey key(raw);
        if (cfg_.ValueName.empty()) return true;  // subkey existing at all counts as active

        DWORD type = REG_NONE;
        DWORD size = 0;
        LSTATUS status = RegGetValueW(key.key, nullptr, cfg_.ValueName.c_str(), RRF_RT_ANY, &type, nullptr, &size);
        if (status != ERROR_SUCCESS) return false;  // value missing or unreadable

        // RegGetValueW may under-report the size for REG_EXPAND_SZ (expansion
        // happens on the read), so grow and retry on ERROR_MORE_DATA.
        std::vector<BYTE> data;
        for (int attempt = 0; attempt < 8; ++attempt) {
            data.assign(static_cast<size_t>(size) + sizeof(wchar_t) * 2, 0);
            DWORD got = static_cast<DWORD>(data.size());
            status = RegGetValueW(key.key, nullptr, cfg_.ValueName.c_str(), RRF_RT_ANY, &type, data.data(), &got);
            if (status == ERROR_SUCCESS) {
                size = got;
                break;
            }
            if (status != ERROR_MORE_DATA) return false;
            size = (got > size) ? got : size * 2 + 64;
        }
        if (status != ERROR_SUCCESS) return false;
        if (size > data.size()) size = static_cast<DWORD>(data.size());

        const std::wstring text = RenderValue(type, data, size);
        if (cfg_.ActiveValue.empty()) return !(text.empty() || text == L"0");
        return _wcsicmp(text.c_str(), cfg_.ActiveValue.c_str()) == 0;
    } catch (...) {
        return false;
    }
}

void RegistryWatcher::WatchLoop(std::stop_token st) {
    UniqueHandle notify(CreateEventW(nullptr, FALSE /*auto reset*/, FALSE, nullptr));
    if (!notify.valid()) {
        LogInfo(std::format(L"RegistryWatcher: CreateEvent failed (error {}); registry watch disabled", GetLastError()));
        return;
    }

    const auto stopping = [&]() { return st.stop_requested() || WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0; };

    bool lastActive = IsActive();
    while (!stopping()) {
        HKEY raw = nullptr;
        if (RegOpenKeyExW(HiveHandle(cfg_.Hive), cfg_.SubKey.c_str(), 0, KEY_NOTIFY, &raw) != ERROR_SUCCESS) {
            // Key doesn't exist yet -- poll until it appears.
            if (WaitForSingleObject(stopEvent_, 5000) == WAIT_OBJECT_0) return;
            if (st.stop_requested()) return;
            RaiseIfChanged(lastActive);
            continue;
        }
        UniqueKey key(raw);
        while (!stopping()) {
            if (RegNotifyChangeKeyValue(key.key, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, notify.get(), TRUE) !=
                ERROR_SUCCESS) {
                break;  // key was likely deleted; reopen
            }
            const HANDLE handles[2] = {notify.get(), stopEvent_};
            const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (waited == WAIT_OBJECT_0 + 1) return;  // stop
            if (waited != WAIT_OBJECT_0) {
                LogInfo(std::format(L"RegistryWatcher: WaitForMultipleObjects failed (error {}); registry watch stopped", GetLastError()));
                return;
            }
            RaiseIfChanged(lastActive);
        }
    }
}

void RegistryWatcher::RaiseIfChanged(bool& lastActive) {
    const bool active = IsActive();
    if (active == lastActive) return;
    lastActive = active;
    // Never let an exception escape the watcher thread.
    try {
        if (onChanged_) onChanged_();
    } catch (const std::exception& ex) {
        LogInfo(std::string("RegistryWatcher: Changed handler threw: ") + ex.what());
    } catch (...) {
        LogInfo(L"RegistryWatcher: Changed handler threw an unknown exception");
    }
}

}  // namespace pc
