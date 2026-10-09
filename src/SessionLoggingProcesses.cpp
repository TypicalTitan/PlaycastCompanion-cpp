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
Value Failed(DWORD error, std::wstring_view operation) {
    return Metric(Null(), error == ERROR_ACCESS_DENIED ? L"access_denied" : L"unavailable",
        std::format(L"{} failed (Win32 {})", operation, error));
}
class WindowsProcessSource final : public ProcessSource {
public:
    std::vector<ProcessIdentity> Entries(std::stop_token stop) const override {
        UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot.valid()) throw std::runtime_error(std::format("CreateToolhelp32Snapshot failed (Win32 {})", GetLastError()));
        std::vector<ProcessIdentity> entries;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        bool valid = Process32FirstW(snapshot.get(), &entry) != FALSE;
        while (valid && !stop.stop_requested()) {
            if (entry.th32ProcessID != 0) entries.push_back({entry.th32ProcessID, entry.szExeFile});
            valid = Process32NextW(snapshot.get(), &entry) != FALSE;
        }
        if (!valid && GetLastError() != ERROR_NO_MORE_FILES)
            throw std::runtime_error(std::format("Process enumeration failed (Win32 {})", GetLastError()));
        return entries;
    }
    Value Session(const ProcessIdentity& process) const override {
        DWORD session = 0;
        if (!ProcessIdToSessionId(process.pid, &session)) return Failed(GetLastError(), L"ProcessIdToSessionId");
        return Metric(Number(session));
    }
    Object Details(const ProcessIdentity& entry) const override {
        UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, entry.pid));
        if (!process.valid()) process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.pid));
        if (!process.valid()) {
            const auto unavailable = Failed(GetLastError(), L"OpenProcess");
            return Make({{L"ExecutablePath", unavailable}, {L"CpuSeconds", unavailable},
                {L"WorkingSetBytes", unavailable}, {L"StartedAtUtc", unavailable}});
        }
        auto details = Metrics(process.get());
        std::wstring path(32768, L'\0');
        DWORD length = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &length))
            details.Insert(L"ExecutablePath", Failed(GetLastError(), L"QueryFullProcessImageName"));
        else { path.resize(length); details.Insert(L"ExecutablePath", Metric(Text(path))); }
        return details;
    }
};
void CoverageError(Array& errors, DWORD pid, std::wstring_view field, const Value& metric) {
    const auto object = metric.GetObject();
    errors.Append(Make({{L"Source", Text(L"processes")}, {L"Pid", Number(pid)}, {L"Field", Text(field)},
        {L"Status", Get(object, L"Status")}, {L"Message", Get(object, L"Error")}}));
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

Object CaptureProcesses(const std::vector<DWORD>& targetSessions, const Inventory& inventory,
    std::stop_token stop, const ProcessSource* source) {
    Array playcast, games, errors;
    WindowsProcessSource windows;
    if (!source) source = &windows;
    bool partial = false;
    try {
        for (const auto& entry : source->Entries(stop)) {
            if (stop.stop_requested()) break;
            const bool ours = Playcast(entry.name);
            const auto session = source->Session(entry);
            const auto sessionValue = Get(session.GetObject(), L"Value");
            if (sessionValue.ValueType() != Type::Number) { CoverageError(errors, entry.pid, L"SessionId", session); partial = true; }
            const bool target = sessionValue.ValueType() == Type::Number && std::find(targetSessions.begin(), targetSessions.end(),
                static_cast<DWORD>(sessionValue.GetNumber())) != targetSessions.end();
            if (!target && !ours) continue;
            const auto details = source->Details(entry);
            for (const auto name : {L"ExecutablePath", L"CpuSeconds", L"WorkingSetBytes", L"StartedAtUtc"}) {
                const auto metric = Get(details, name);
                if (Get(metric.GetObject(), L"Value").ValueType() == Type::Null) { CoverageError(errors, entry.pid, name, metric); partial = true; }
            }
            const auto path = Get(json::GetObject(details, L"ExecutablePath"), L"Value");
            const InstalledGame* match = nullptr;
            if (target && path.ValueType() == Type::String) for (const auto& game : inventory.games)
                if (GameInventory::ContainsExecutable(game.installRoot, std::wstring(path.GetString()))) { match = &game; break; }
            if (!ours && !match) continue;
            auto observed = Make({{L"Pid", Number(entry.pid)}, {L"Name", Text(entry.name)}, {L"SessionId", session},
                {L"MatchedGameId", match ? Text(match->id) : Null()}, {L"MatchedGameName", match ? Text(match->name) : Null()},
                {L"GameStore", match ? Text(match->store) : Null()}});
            for (const auto name : {L"ExecutablePath", L"CpuSeconds", L"WorkingSetBytes", L"StartedAtUtc"}) observed.Insert(name, Get(details, name));
            if (ours) playcast.Append(observed);
            if (match) games.Append(observed);
        }
    } catch (const std::exception& exception) {
        errors.Append(Make({{L"Source", Text(L"processes")}, {L"Status", Text(L"unavailable")}, {L"Message", Text(json::Utf8ToWide(exception.what()))}}));
        return Make({{L"status", Text(L"unavailable")}, {L"playcastProcesses", playcast}, {L"runningGames", games}, {L"errors", errors}});
    }
    if (stop.stop_requested()) { partial = true; errors.Append(Make({{L"Source", Text(L"processes")}, {L"Status", Text(L"cancelled")}, {L"Message", Text(L"Process capture cancelled; running games may be unknown.")}})); }
    return Make({{L"status", Text(partial ? L"partial" : L"available")}, {L"playcastProcesses", playcast}, {L"runningGames", games}, {L"errors", errors}});
}
}
