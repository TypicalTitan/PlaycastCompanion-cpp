#include "pch.h"
#include "SessionLogging.h"
#include "SessionLoggingSnapshot.h"
#include "SessionLoggingLifecycleContracts.h"
#include <fstream>
#include <iostream>

using namespace pc;
using namespace pc::sessionlog;
namespace {
void Require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        wchar_t directory[MAX_PATH]{};
        Require(GetTempPathW(MAX_PATH, directory) != 0, "Temporary directory unavailable");
        static std::atomic_uint counter{0};
        root = std::filesystem::path(directory) / std::format(L"pc-lifecycle-contract-{}-{}-{}", GetCurrentProcessId(), GetTickCount64(), ++counter);
        std::filesystem::create_directories(root);
    }
    ~Fixture() {
        wchar_t directory[MAX_PATH]{};
        GetTempPathW(MAX_PATH, directory);
        auto parent = std::filesystem::absolute(std::filesystem::path(directory)).lexically_normal();
        if (parent.filename().empty()) parent = parent.parent_path();
        const auto resolved = std::filesystem::absolute(root).lexically_normal();
        if (resolved.parent_path() == parent && resolved.filename().wstring().starts_with(L"pc-lifecycle-contract-")) {
            std::error_code error;
            std::filesystem::remove_all(resolved, error);
        }
    }
};
std::vector<Object> Payloads(const std::filesystem::path& directory, std::wstring_view category) {
    std::ifstream file(directory / (std::wstring(category) + L".jsonl"));
    std::string line;
    std::vector<Object> result;
    while (std::getline(file, line)) result.push_back(json::GetObject(json::Parse(line), L"payload"));
    return result;
}
Object FinalMachine(const std::filesystem::path& directory) {
    for (const auto& payload : Payloads(directory, L"diagnostics"))
        if (json::GetString(payload, L"event") == L"machineSnapshot" && json::GetString(payload, L"phase") == L"sessionEnd") return json::GetObject(payload, L"snapshot");
    throw std::runtime_error("Terminal machine snapshot absent");
}
Object FinalGames(const std::filesystem::path& directory) {
    for (const auto& payload : Payloads(directory, L"games")) if (json::GetString(payload, L"phase") == L"sessionEnd") return payload;
    throw std::runtime_error("Terminal game snapshot absent");
}
class FakeSnapshots final : public SnapshotSource {
public:
    enum class Mode { Normal, BlockFirst, HoldTerminal, BlockAll, IgnoreCancellation, Fail };
    explicit FakeSnapshots(Mode mode = Mode::Normal) : mode_(mode) {}
    Object Capture(std::wstring_view, bool, std::stop_token stop) const override {
        unsigned call = 0;
        { std::lock_guard lock(mutex_); call = ++calls_; entered_.notify_all(); }
        if (mode_ == Mode::Fail) throw std::runtime_error("Synthetic snapshot source unavailable");
        if (mode_ == Mode::IgnoreCancellation) {
            std::unique_lock lock(mutex_);
            entered_.wait(lock, [&] { return released_; });
        }
        if (mode_ == Mode::HoldTerminal && call == 2) {
            std::unique_lock lock(mutex_);
            entered_.wait(lock, stop, [&] { return released_; });
        }
        if (mode_ == Mode::BlockAll || ((mode_ == Mode::BlockFirst || mode_ == Mode::HoldTerminal) && call == 1)) {
            std::unique_lock lock(mutex_);
            entered_.wait(lock, stop, [] { return false; });
        }
        Array games;
        games.Append(Make({{L"id", Number(call)}, {L"name", Text(L"Synthetic game")}}));
        return Make({{L"capturedAtUtc", Text(UtcNow())}, {L"installedGames", games}, {L"runningGames", games},
            {L"runningGameDetectionStatus", Text(L"available")}, {L"installedGameSources", Array()}, {L"inventoryLimitations", Array()},
            {L"processCoverage", Make({{L"Status", Text(L"available")}})}, {L"targetSessionIds", Array()},
            {L"password", Text(L"synthetic-closing-secret")}, {L"fixtureCapture", Number(call)}});
    }
    void WaitForFirst(unsigned call = 1) const {
        std::unique_lock lock(mutex_);
        Require(entered_.wait_for(lock, std::chrono::seconds(2), [&] { return calls_ >= call; }), "Requested snapshot did not start");
    }
    void Release() { std::lock_guard lock(mutex_); released_ = true; entered_.notify_all(); }
    unsigned Calls() const { std::lock_guard lock(mutex_); return calls_; }
private:
    Mode mode_;
    mutable std::mutex mutex_;
    mutable std::condition_variable_any entered_;
    mutable unsigned calls_ = 0;
    bool released_ = false;
};
void ShortSessionContract() {
    Fixture fixture;
    auto source = std::make_shared<FakeSnapshots>(FakeSnapshots::Mode::HoldTerminal);
    SessionLogger logger(AppConfig{}, true, false, fixture.root, source);
    logger.UpdateBackends(L"{\"lighting\":\"released\"}");
    logger.UpdateSession(true, L"active", L"guest-7");
    const auto directory = logger.Directory();
    source->WaitForFirst();
    const auto before = std::chrono::steady_clock::now();
    logger.UpdateSession(false, L"ended", L"guest-7");
    Require(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(100), "Session ending blocked on capture");
    source->WaitForFirst(2);
    Require(Payloads(directory, L"session").size() == 1, "Session ended before its terminal snapshot completed");
    source->Release();
    logger.Stop();
    const auto machine = FinalMachine(directory);
    Require(Get(machine, L"fixtureCapture").GetNumber() == 2, "Closing sample reused the initial capture");
    Require(json::GetString(json::GetObject(machine, L"companionBackends"), L"lighting") == L"released", "Closing backend status missing");
    Require(Get(FinalGames(directory), L"runningGames").GetArray().Size() == 1, "Short-session closing game state absent");
    const auto session = Payloads(directory, L"session");
    Require(session.size() == 2 && json::GetString(session.back(), L"event") == L"sessionEnded", "Short session did not finish");
    Require(json::GetString(session.back(), L"finalSnapshotStatus") == L"captured", "Successful final capture not reported");
    const auto observedEnd = json::GetString(session.back(), L"observedEndAtUtc");
    Require(!observedEnd.empty() && observedEnd <= json::GetString(machine, L"capturedAtUtc"), "Observed end time was replaced by final capture time");
    for (const auto& record : Payloads(directory, L"diagnostics"))
        Require(json::GetString(record, L"phase") == L"sessionEnd", "Cancelled initial capture entered the closed session");
}
void RotationContract() {
    Fixture fixture;
    auto source = std::make_shared<FakeSnapshots>(FakeSnapshots::Mode::BlockFirst);
    SessionLogger logger(AppConfig{}, true, false, fixture.root, source);
    logger.UpdateBackends(L"{\"session\":\"first\"}");
    logger.UpdateSession(true, L"active", L"guest-7");
    const auto first = logger.Directory();
    source->WaitForFirst();
    logger.UpdateSession(true, L"active", L"guest-8");
    const auto second = logger.Directory();
    logger.UpdateBackends(L"{\"session\":\"second\"}");
    logger.Stop();
    Require(first != second, "Session identity did not rotate writer");
    Require(json::GetString(json::GetObject(FinalMachine(first), L"companionBackends"), L"session") == L"first", "Old session received new backend metadata");
    Require(json::GetString(json::GetObject(FinalMachine(second), L"companionBackends"), L"session") == L"second", "Shutdown final sample lost its backend metadata");
    Require(json::GetString(Payloads(second, L"session").back(), L"reason") == L"Companion shutdown", "Shutdown did not close rotated session");
}
void TimeoutContract() {
    Fixture fixture;
    auto source = std::make_shared<FakeSnapshots>(FakeSnapshots::Mode::BlockAll);
    SessionLogger logger(AppConfig{}, true, false, fixture.root, source, std::chrono::milliseconds(40));
    logger.UpdateSession(true, L"active", L"guest-7");
    const auto directory = logger.Directory();
    source->WaitForFirst();
    const auto before = std::chrono::steady_clock::now();
    logger.Stop();
    Require(std::chrono::steady_clock::now() - before < std::chrono::seconds(1), "Terminal cancellation was not bounded");
    const auto machine = FinalMachine(directory);
    Require(json::GetString(machine, L"captureStatus") == L"partial" && Get(machine, L"companion").ValueType() == Type::Object, "Timeout omitted fresh Companion metrics");
    Require(Get(FinalGames(directory), L"runningGames").ValueType() == Type::Null, "Unavailable terminal games were reported as empty");
    Require(json::GetString(Payloads(directory, L"session").back(), L"finalSnapshotStatus") == L"partial", "Terminal timeout was hidden");
    const auto diagnostics = Payloads(directory, L"diagnostics");
    Require(diagnostics.size() == 2 && json::GetString(diagnostics[0], L"event") == L"snapshotFailed", "Timeout diagnostics missing");
}
void FailureContract() {
    Fixture fixture;
    auto source = std::make_shared<FakeSnapshots>(FakeSnapshots::Mode::Fail);
    SessionLogger logger(AppConfig{}, true, false, fixture.root, source);
    logger.UpdateSession(true, L"active", L"guest-7");
    const auto directory = logger.Directory();
    logger.Stop();
    Require(Get(FinalMachine(directory), L"companion").ValueType() == Type::Object, "Failed final sample omitted fresh resources");
    Require(Get(FinalGames(directory), L"installedGames").ValueType() == Type::Null, "Failed final inventory implied zero installs");
    Require(json::GetString(Payloads(directory, L"session").back(), L"event") == L"sessionEnded", "Source failure prevented session closure");
}
void UncooperativeSourceContract() {
    Fixture fixture;
    auto source = std::make_shared<FakeSnapshots>(FakeSnapshots::Mode::IgnoreCancellation);
    SessionLogger logger(AppConfig{}, true, false, fixture.root, source, std::chrono::milliseconds(40));
    logger.UpdateSession(true, L"active", L"guest-7");
    const auto directory = logger.Directory();
    source->WaitForFirst();
    std::jthread rescue([source](std::stop_token stop) {
        std::mutex mutex;
        std::condition_variable_any changed;
        std::unique_lock lock(mutex);
        changed.wait_for(lock, stop, std::chrono::seconds(2), [] { return false; });
        source->Release();
    });
    const auto before = std::chrono::steady_clock::now();
    logger.Stop();
    source->Release();
    rescue.request_stop();
    Require(std::chrono::steady_clock::now() - before < std::chrono::seconds(1), "Provider ignoring cancellation blocked shutdown");
    Require(source->Calls() == 1, "Unfinished provider overlapped with another snapshot");
    Require(Get(FinalGames(directory), L"runningGames").ValueType() == Type::Null, "Busy provider fabricated terminal game inventory");
    Require(json::GetString(Payloads(directory, L"session").back(), L"event") == L"sessionEnded", "Busy provider prevented terminal closure");
}
void ClosingPolicyContract() {
    for (const bool initialPolicy : {false, true}) {
        Fixture fixture;
        auto source = std::make_shared<FakeSnapshots>(FakeSnapshots::Mode::HoldTerminal);
        SessionLogger logger(AppConfig{}, true, false, fixture.root, source);
        logger.SetIncludeSecrets(initialPolicy);
        logger.UpdateSession(true, L"active", L"guest-7");
        const auto directory = logger.Directory();
        source->WaitForFirst();
        logger.UpdateSession(false, L"ended", L"guest-7");
        source->WaitForFirst(2);
        logger.SetIncludeSecrets(!initialPolicy);
        source->Release();
        logger.Stop();
        Require(json::GetString(FinalMachine(directory), L"password") == (initialPolicy ? L"synthetic-closing-secret" : L"[REDACTED]"),
            "Later secret toggle changed ending session policy");
    }
}
class FakeProcesses final : public ProcessSource {
public:
    bool failEnumeration = false;
    std::vector<ProcessIdentity> Entries(std::stop_token) const override {
        if (failEnumeration) throw std::runtime_error("Synthetic enumeration unavailable");
        return {{10, L"PlaycastService.exe"}, {20, L"PlaycastTrayApp.exe"}, {30, L"fixture-game.exe"}};
    }
    Value Session(const ProcessIdentity& process) const override {
        return process.pid == 10 ? Metric(Null(), L"access_denied", L"ProcessIdToSessionId failed (Win32 5)") : Metric(Number(7));
    }
    Object Details(const ProcessIdentity& process) const override {
        return Make({{L"ExecutablePath", Metric(Text(L"C:\\Fixture\\Installed\\game.exe"))},
            {L"CpuSeconds", Metric(Number(1))}, {L"WorkingSetBytes", process.pid == 20 ? Metric(Null(), L"unavailable", L"Synthetic memory unavailable") : Metric(Number(1000))},
            {L"StartedAtUtc", Metric(Text(L"2026-10-09T00:00:00Z"))}});
    }
};
void ProcessCoverageContract() {
    FakeProcesses source;
    Inventory inventory;
    inventory.games.push_back({L"fixture", L"game-1", L"Fixture game", L"C:\\Fixture\\Installed", L""});
    const auto captured = CaptureProcesses({7}, inventory, {}, &source);
    Require(json::GetString(captured, L"status") == L"partial", "Denied process session or memory was reported as complete");
    const auto playcast = Get(captured, L"playcastProcesses").GetArray();
    Require(playcast.Size() == 2, "Inaccessible named Playcast process disappeared");
    const auto inaccessible = playcast.GetAt(0).GetObject();
    Require(Get(inaccessible, L"Pid").GetNumber() == 10 && json::GetString(inaccessible, L"Name") == L"PlaycastService.exe", "Denied process identity missing");
    Require(Get(json::GetObject(inaccessible, L"SessionId"), L"Value").ValueType() == Type::Null, "Unavailable session was invented");
    Require(json::GetString(json::GetObject(inaccessible, L"SessionId"), L"Error").find(L"Win32 5") != std::wstring::npos, "Denied session error missing");
    Require(Get(captured, L"runningGames").GetArray().Size() == 2, "Known-session games not retained or unknown-session service counted");
    Require(Get(captured, L"errors").GetArray().Size() == 2, "Session and metric coverage errors missing");
    source.failEnumeration = true;
    const auto failed = CaptureProcesses({7}, inventory, {}, &source);
    Require(json::GetString(failed, L"status") == L"unavailable" && Get(failed, L"errors").GetArray().Size() != 0, "Enumeration failure was silent");
    source.failEnumeration = false;
    std::stop_source cancelled;
    cancelled.request_stop();
    const auto partial = CaptureProcesses({7}, inventory, cancelled.get_token(), &source);
    Require(json::GetString(partial, L"status") == L"partial" && Get(partial, L"errors").GetArray().Size() == 1, "Cancelled scan claimed complete coverage");
}
}

int RunSessionLoggingLifecycleContracts() {
    int count = 0;
    for (const auto& [name, test] : std::initializer_list<std::pair<const char*, void(*)()>>{
        {"short session final snapshot", ShortSessionContract}, {"rotated session final snapshots", RotationContract},
        {"terminal capture cancellation", TimeoutContract}, {"terminal capture source failure", FailureContract},
        {"uncooperative capture bounded", UncooperativeSourceContract},
        {"closing redaction policy frozen", ClosingPolicyContract},
        {"process coverage", ProcessCoverageContract}}) {
        test(); ++count; std::cout << "PASS " << name << '\n';
    }
    return count;
}
