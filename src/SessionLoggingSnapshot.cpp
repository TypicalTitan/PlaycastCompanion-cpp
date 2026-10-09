#include "pch.h"
#include "SessionLoggingSnapshot.h"
#include "SessionWatcher.h"

namespace pc::sessionlog {
namespace {
struct WtsMemory { void* value = nullptr; ~WtsMemory() { if (value) WTSFreeMemory(value); } };
Object Sessions(std::wstring_view target, bool includeDisconnected, std::vector<DWORD>& targetIds) {
    Array items, errors;
    PWTS_SESSION_INFOW entries = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &entries, &count)) {
        errors.Append(Make({{L"Source", Text(L"WTS")}, {L"Status", Text(L"unavailable")}, {L"Message", Text(std::format(L"Enumeration failed (Win32 {})", GetLastError()))}}));
        return Make({{L"Status", Text(L"unavailable")}, {L"Sessions", items}, {L"Errors", errors}});
    }
    WtsMemory memory{ entries };
    bool partial = false;
    for (DWORD index = 0; index < count; ++index) {
        LPWSTR username = nullptr;
        DWORD bytes = 0;
        const bool available = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, entries[index].SessionId, WTSUserName, &username, &bytes) != FALSE;
        WtsMemory userMemory{ username };
        const auto user = available && username ? std::wstring(username) : L"";
        const bool relevant = entries[index].State == WTSActive || entries[index].State == WTSConnected || (includeDisconnected && entries[index].State == WTSDisconnected);
        const bool match = relevant && CompareStringOrdinal(user.c_str(), static_cast<int>(user.size()), target.data(), static_cast<int>(target.size()), TRUE) == CSTR_EQUAL;
        if (match) targetIds.push_back(entries[index].SessionId);
        partial |= !available;
        items.Append(Make({{L"SessionId", Number(entries[index].SessionId)}, {L"State", Text(entries[index].State == WTSActive ? L"Active" : entries[index].State == WTSConnected ? L"Connected" : entries[index].State == WTSDisconnected ? L"Disconnected" : L"Other")},
            {L"Username", available ? Text(user) : Null()}, {L"IsTarget", Boolean(match)}, {L"UsernameStatus", Text(available ? L"available" : L"unavailable")}}));
    }
    if (partial) errors.Append(Make({{L"Source", Text(L"WTS")}, {L"Status", Text(L"partial")}, {L"Message", Text(L"Some Windows session usernames could not be inspected; the target session may be unknown.")}}));
    return Make({{L"Status", Text(partial ? L"partial" : L"available")}, {L"Sessions", items}, {L"Errors", errors}});
}
Value RegistryMetric(const wchar_t* key, const wchar_t* value, bool number = false) {
    DWORD type = 0, bytes = 0;
    const auto flags = RRF_RT_ANY | RRF_SUBKEY_WOW6464KEY;
    auto status = RegGetValueW(HKEY_LOCAL_MACHINE, key, value, flags, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || bytes > 65536) return Metric(Null(), status == ERROR_ACCESS_DENIED ? L"access_denied" : L"not_found");
    std::vector<unsigned char> data(bytes + sizeof(wchar_t), 0);
    status = RegGetValueW(HKEY_LOCAL_MACHINE, key, value, flags, &type, data.data(), &bytes);
    if (status != ERROR_SUCCESS) return Metric(Null(), L"unavailable");
    if (number && type == REG_DWORD && bytes == sizeof(DWORD)) { DWORD integer = 0; memcpy(&integer, data.data(), sizeof(integer)); return Metric(Number(integer)); }
    if (type == REG_QWORD && bytes == sizeof(ULONGLONG)) {
        FILETIME time{};
        memcpy(&time, data.data(), sizeof(time));
        SYSTEMTIME utc{};
        if (!FileTimeToSystemTime(&time, &utc) || (time.dwLowDateTime == 0 && time.dwHighDateTime == 0)) return Metric(Null(), L"invalid_value");
        return Metric(Text(std::format(L"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds)));
    }
    if (type == REG_SZ || type == REG_EXPAND_SZ) return Metric(Text(reinterpret_cast<const wchar_t*>(data.data())));
    return Metric(Null(), L"unsupported_type");
}
Object Registry() {
    return Make({{L"Status", Text(L"observed")},
        {L"SessionLoggedInUtc", RegistryMetric(L"SOFTWARE\\Playcast\\Nonsole", L"SessionLoggedInUtc")},
        {L"LastHeartbeatUtc", RegistryMetric(L"SOFTWARE\\Playcast\\Nonsole", L"LastHeartbeatUtc")},
        {L"HeartbeatIntervalSeconds", RegistryMetric(L"SOFTWARE\\Playcast\\Nonsole", L"HeartbeatIntervalSeconds", true)},
        {L"VirtualDisplayDeviceName", RegistryMetric(L"SOFTWARE\\Playcast\\GuestMode", L"VirtualDisplayDeviceName")}});
}
Value HeartbeatAge() {
    ULONGLONG heartbeat = 0;
    DWORD bytes = sizeof(heartbeat);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Playcast\\Nonsole", L"LastHeartbeatUtc",
        RRF_RT_REG_QWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &heartbeat, &bytes) != ERROR_SUCCESS || !heartbeat) return Null();
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER current{};
    current.LowPart = now.dwLowDateTime;
    current.HighPart = now.dwHighDateTime;
    return Number((static_cast<double>(current.QuadPart) - static_cast<double>(heartbeat)) / 10000000.);
}
Object Disks() {
    Array volumes;
    const auto drives = GetLogicalDrives();
    for (unsigned index = 0; index < 26; ++index) {
        if (!(drives & (1u << index))) continue;
        std::wstring drive = L"A:\\";
        drive[0] = static_cast<wchar_t>(L'A' + index);
        if (GetDriveTypeW(drive.c_str()) != DRIVE_FIXED) continue;
        ULARGE_INTEGER free{}, total{}, unused{};
        const bool available = GetDiskFreeSpaceExW(drive.c_str(), &free, &total, &unused) != FALSE;
        volumes.Append(Make({{L"drive", Text(drive)}, {L"freeBytes", Metric(available ? Number(static_cast<double>(free.QuadPart)) : Null(), available ? L"available" : L"unavailable")},
            {L"totalBytes", Metric(available ? Number(static_cast<double>(total.QuadPart)) : Null(), available ? L"available" : L"unavailable")}}));
    }
    return Make({{L"status", Text(drives ? L"available" : L"unavailable")}, {L"volumes", volumes}});
}
}

Object SnapshotSource::Capture(std::wstring_view target, bool includeDisconnected, std::stop_token stop) const {
    const auto inventory = GameInventory().Capture(stop);
    std::vector<DWORD> targetIds;
    const auto sessions = Sessions(target, includeDisconnected, targetIds);
    const auto processes = CaptureProcesses(targetIds, inventory, stop);
    const auto sessionStatus = json::GetString(sessions, L"Status");
    const auto detectionStatus = sessionStatus == L"available" ? json::GetString(processes, L"status")
        : targetIds.empty() ? L"unknown_session_inventory" : L"partial";
    Array coverageErrors;
    for (const auto& error : Get(processes, L"errors").GetArray()) coverageErrors.Append(error);
    for (const auto& error : Get(sessions, L"Errors").GetArray()) coverageErrors.Append(error);
    Array ids;
    for (const auto id : targetIds) ids.Append(Number(id));
    SYSTEM_INFO system{};
    GetNativeSystemInfo(&system);
    const auto machine = Make({{L"operatingSystem", Text(L"Windows")}, {L"osArchitecture", Text(system.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? L"ARM64" : L"X64")},
        {L"logicalProcessorCount", Number(system.dwNumberOfProcessors)}, {L"uptimeSeconds", Number(static_cast<double>(GetTickCount64()) / 1000)}, {L"managedRuntime", Text(L"Native C++20")}});
    return Make({{L"capturedAtUtc", Text(UtcNow())}, {L"targetUsername", Text(target)}, {L"targetSessionIds", ids}, {L"sessions", sessions},
        {L"installedGames", inventory.GamesJson()}, {L"installedGameSources", inventory.sources}, {L"inventoryLimitations", inventory.limitations},
        {L"runningGames", Get(processes, L"runningGames")}, {L"runningGameDetectionStatus", Text(detectionStatus)},
        {L"playcastProcesses", Get(processes, L"playcastProcesses")}, {L"processCoverage", Make({{L"Status", Text(detectionStatus)}, {L"Errors", coverageErrors}})},
        {L"nonsoleRegistry", Registry()}, {L"heartbeatAgeSeconds", HeartbeatAge()}, {L"machine", machine}, {L"companion", CaptureCompanion()}, {L"disks", Disks()}});
}
}
