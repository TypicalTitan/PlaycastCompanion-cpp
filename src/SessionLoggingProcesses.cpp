#include "pch.h"
#include "SessionLoggingSnapshot.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <cwctype>

namespace pc::sessionlog {
namespace {
double Seconds(FILETIME time) { ULARGE_INTEGER value{}; value.LowPart = time.dwLowDateTime; value.HighPart = time.dwHighDateTime; return static_cast<double>(value.QuadPart) / 10000000.; }
Value Started(FILETIME time) {
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&time, &utc)) return Metric(Null(), L"unavailable");
    return Metric(Text(std::format(L"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds)));
}
Object Metrics(HANDLE process) {
    FILETIME created{}, ended{}, kernel{}, user{};
    const bool times = GetProcessTimes(process, &created, &ended, &kernel, &user) != FALSE;
    const auto timeError = times ? L"" : std::format(L"GetProcessTimes failed (Win32 {})", GetLastError());
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    const bool memoryAvailable = K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) != FALSE;
    const auto memoryError = memoryAvailable ? L"" : std::format(L"GetProcessMemoryInfo failed (Win32 {})", GetLastError());
    return Make({{L"CpuSeconds", Metric(times ? Number(Seconds(kernel) + Seconds(user)) : Null(), times ? L"available" : L"unavailable", timeError)},
        {L"WorkingSetBytes", Metric(memoryAvailable ? Number(static_cast<double>(memory.WorkingSetSize)) : Null(), memoryAvailable ? L"available" : L"unavailable", memoryError)},
        {L"PrivateMemoryBytes", Metric(memoryAvailable ? Number(static_cast<double>(memory.PrivateUsage)) : Null(), memoryAvailable ? L"available" : L"unavailable", memoryError)},
        {L"StartedAtUtc", times ? Started(created) : Metric(Null(), L"unavailable", timeError)}});
}
bool Playcast(std::wstring name) {
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return name.find(L"playcast") != std::wstring::npos || name.find(L"playjector") != std::wstring::npos || name == L"nodeservice.exe";
}
Object Process(DWORD pid, const std::wstring& name, HANDLE handle, DWORD session, const std::wstring& path, const InstalledGame* game) {
    const auto metrics = handle ? Metrics(handle) : Make({{L"CpuSeconds", Metric(Null(), L"access_denied")},
        {L"WorkingSetBytes", Metric(Null(), L"access_denied")}, {L"StartedAtUtc", Metric(Null(), L"access_denied")}});
    auto result = Make({{L"Pid", Number(pid)}, {L"Name", Text(name)}, {L"SessionId", Metric(Number(session))},
        {L"ExecutablePath", Metric(path.empty() ? Null() : Text(path), path.empty() ? L"unavailable" : L"available")},
        {L"CpuSeconds", Get(metrics, L"CpuSeconds")}, {L"WorkingSetBytes", Get(metrics, L"WorkingSetBytes")},
        {L"StartedAtUtc", Get(metrics, L"StartedAtUtc")}, {L"MatchedGameId", game ? Text(game->id) : Null()},
        {L"MatchedGameName", game ? Text(game->name) : Null()}, {L"GameStore", game ? Text(game->store) : Null()}});
    return result;
}
}

Object CaptureCompanion() {
    const auto observed = Metrics(GetCurrentProcess());
    DWORD session = 0, handles = 0;
    const bool sessionAvailable = ProcessIdToSessionId(GetCurrentProcessId(), &session) != FALSE;
    const bool handlesAvailable = GetProcessHandleCount(GetCurrentProcess(), &handles) != FALSE;
    return Make({{L"pid", Number(GetCurrentProcessId())}, {L"sessionId", Metric(sessionAvailable ? Number(session) : Null(), sessionAvailable ? L"available" : L"unavailable")},
        {L"cpuSeconds", Get(observed, L"CpuSeconds")}, {L"workingSetBytes", Get(observed, L"WorkingSetBytes")},
        {L"privateMemoryBytes", Get(observed, L"PrivateMemoryBytes")}, {L"startedAtUtc", Get(observed, L"StartedAtUtc")},
        {L"handleCount", Metric(handlesAvailable ? Number(handles) : Null(), handlesAvailable ? L"available" : L"unavailable")},
        {L"managedHeapBytes", Metric(Null(), L"not_applicable_native")}});
}

Object CaptureProcesses(const std::vector<DWORD>& targetSessions, const Inventory& inventory, std::stop_token stop) {
    Array playcast, games, errors;
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid()) return Make({{L"status", Text(L"unavailable")}, {L"playcastProcesses", playcast}, {L"runningGames", games}, {L"errors", errors}});
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool partial = false;
    for (bool valid = Process32FirstW(snapshot.get(), &entry) != FALSE; valid && !stop.stop_requested(); valid = Process32NextW(snapshot.get(), &entry) != FALSE) {
        DWORD session = 0;
        if (!ProcessIdToSessionId(entry.th32ProcessID, &session)) continue;
        const bool target = std::find(targetSessions.begin(), targetSessions.end(), session) != targetSessions.end();
        const bool ours = Playcast(entry.szExeFile);
        if (!target && !ours) continue;
        UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID));
        if (!process.valid()) process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        std::wstring path(32768, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        if (!process.valid() || !QueryFullProcessImageNameW(process.get(), 0, path.data(), &length)) { path.clear(); partial = true; }
        else path.resize(length);
        const InstalledGame* match = nullptr;
        if (target && !path.empty()) for (const auto& game : inventory.games) if (GameInventory::ContainsExecutable(game.installRoot, path)) { match = &game; break; }
        if (ours || match) {
            const auto observed = Process(entry.th32ProcessID, entry.szExeFile, process.valid() ? process.get() : nullptr, session, path, match);
            if (ours) playcast.Append(observed);
            if (match) games.Append(observed);
        }
    }
    if (partial) errors.Append(Make({{L"Source", Text(L"processes")}, {L"Status", Text(L"partial")}, {L"Message", Text(L"Some target-session processes could not be inspected; running games may be unknown.")}}));
    return Make({{L"status", Text(partial ? L"partial" : L"available")}, {L"playcastProcesses", playcast}, {L"runningGames", games}, {L"errors", errors}});
}
}
