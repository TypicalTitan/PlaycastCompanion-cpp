#include "pch.h"
#include "LogAnalysis.h"
#include "LogAnalysisSample.h"
#include "Json.h"
#include <fstream>
#include <iostream>

namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Fixture {
    std::filesystem::path Root;
    Fixture() {
        static std::atomic_uint sequence = 0;
        wchar_t buffer[MAX_PATH]{}; Require(GetTempPathW(MAX_PATH, buffer) != 0, "Temporary path unavailable");
        Root = std::filesystem::absolute(std::filesystem::path(buffer) / std::format(L"pc-analysis-contract-{}-{}-{}", GetCurrentProcessId(), GetTickCount64(), ++sequence));
        std::filesystem::create_directories(Root);
    }
    ~Fixture() {
        wchar_t buffer[MAX_PATH]{}; GetTempPathW(MAX_PATH, buffer);
        const auto temporary = std::filesystem::absolute(std::filesystem::path(buffer)).lexically_normal().parent_path();
        const auto resolved = Root.lexically_normal();
        if (resolved.parent_path() == temporary && resolved.filename().wstring().starts_with(L"pc-analysis-contract-")) {
            std::error_code error; std::filesystem::remove_all(resolved, error);
        }
    }
    void Put(const wchar_t* filename, std::string_view text) const {
        std::ofstream stream(Root / filename, std::ios::binary); stream << text;
    }
};
std::string Record(std::string_view payload, std::string_view timestamp = "2026-10-09T12:00:00.000Z") {
    return "{\"recordedAtUtc\":\"" + std::string(timestamp) + "\",\"includeSecrets\":false,\"payload\":" + std::string(payload) + "}\n";
}
void CorrelationAndProvenance() {
    Fixture fixture;
    const auto deploy = R"({"type":"PROVISIONING_SCRIPT_DEPLOY","deploymentId":"deploy","executionId":"run","stepIndex":0,"completeScript":"Write-Output 'display only'"})";
    const auto reply = R"({"type":"PROVISIONING_SCRIPT_RESULT","deploymentId":"deploy","executionId":"run","stepIndex":0,"status":"succeeded","exitCode":0,"stdout":"display only\n","stderr":""})";
    fixture.Put(L"realtime.001.jsonl", Record("{\"version\":1,\"category\":\"realtime\",\"direction\":\"received\",\"payload\":" + std::string(deploy) + "}"));
    fixture.Put(L"scripts.002.jsonl", Record("{\"event\":\"scriptCommunication\",\"message\":" + std::string(deploy) + "}")
        + Record("{\"event\":\"scriptCommunication\",\"message\":" + std::string(reply) + "}", "2026-10-09T12:00:01.000Z"));
    std::filesystem::create_directory(fixture.Root / L"child");
    const auto result = pc::LogAnalysisImporter{}.Import({fixture.Root, fixture.Root / L"REALTIME.001.JSONL",
        fixture.Root / L"child" / L".." / L"realtime.001.jsonl"});
    Require(result.FilesRead == 2 && result.Records.size() == 3, "Rotation/overlapping imports lost provenance or duplicated files");
    Require(result.Scripts.size() == 1 && result.Scripts[0].Records.size() == 3, "Realtime/script duplicate observations split a correlated script");
    Require(result.Scripts[0].ExitCode == 0 && result.Scripts[0].StdoutObserved && result.Scripts[0].StderrObserved, "Zero exit or observed empty output lost");
    const auto detail = pc::LogAnalysisProjection::ScriptDetail(result.Scripts[0], result);
    Require(detail.find(L"Write-Output") != std::wstring::npos && detail.find(L"scripts.002.jsonl:2") != std::wstring::npos, "Script/output/provenance inspector incomplete");
}
void CancellationDoesNotInventExecutions() {
    Fixture fixture;
    fixture.Put(L"scripts.log", Record(R"({"event":"scriptCommunication","message":{"type":"PROVISIONING_SCRIPT_DEPLOY","deploymentId":"deployment","executionId":"run","completeScript":"Get-Date"}})")
        + Record(R"({"event":"scriptCommunication","message":{"type":"PROVISIONING_SCRIPT_RESULT","deploymentId":"deployment","executionId":"run","status":"succeeded"}})", "2026-10-09T12:00:01.000Z")
        + Record(R"({"event":"scriptCommunication","message":{"type":"PROVISIONING_SCRIPT_REVOKE","deploymentId":"deployment"}})", "2026-10-09T12:00:02.000Z")
        + Record(R"({"event":"scriptCommunication","message":{"header":{"action":"cancelProvisioningScript","tag":"cancel-request"},"body":{"deploymentId":"deployment"}}})", "2026-10-09T12:00:02.000Z"));
    const auto result = pc::LogAnalysisImporter{}.Import({fixture.Root});
    Require(result.Scripts.size() == 1 && result.Scripts[0].Status == L"succeeded" && result.Scripts[0].Terminal,
        "Cancellation invented an execution or overwrote a terminal result with an absent exit code");
    Require(result.Records.size() == 4, "Cancellation source records must remain inspectable");
}
void NestedAndNativeScripts() {
    Fixture fixture;
    fixture.Put(L"realtime.jsonl", Record(R"({"version":1,"category":"realtime","payload":{"kind":"http_response","data":{"message":"{\"type\":\"PROVISIONING_SCRIPT_DEPLOY\",\"executionId\":\"nested-run\",\"deploymentId\":\"nested\",\"completeScript\":\"Get-Date\"}"}}})"));
    fixture.Put(L"ipc.jsonl", Record(R"({"version":1,"category":"ipc","payload":{"header":{"action":"launchElevatedPowerShellScript","tag":"native-run","isReply":false},"body":{"message":{"script":"Get-Process"}}}})")
        + Record(R"({"version":1,"category":"ipc","payload":{"header":{"action":"launchElevatedPowerShellScript","tag":"native-run","isReply":true},"body":{"message":{"success":true,"exitCode":0,"stdout":"native-output","stderr":""}}}})", "2026-10-09T12:00:01.000Z"));
    const auto result = pc::LogAnalysisImporter{}.Import({fixture.Root});
    Require(result.Scripts.size() == 2, "Nested HTTP or native request/reply was not correlated");
    bool nested = false, native = false;
    for (const auto& script : result.Scripts) {
        nested = nested || script.Script == L"Get-Date";
        native = native || (script.Script == L"Get-Process" && script.Status == L"Succeeded" && script.Stdout == L"native-output");
    }
    Require(nested && native, "Nested complete script or native completion/output projection incorrect");
}
void UnknownMetricsAndDates() {
    Fixture fixture;
    fixture.Put(L"diagnostics.jsonl", Record(R"({"event":"machineSnapshot","snapshot":{"machine":{"logicalProcessorCount":8},"companion":{"pid":1,"cpuSeconds":{"Status":"denied","Value":0},"workingSetBytes":{"Status":"available","Value":0}}}})")
        + Record(R"({"event":"machineSnapshot","snapshot":{"machine":{"logicalProcessorCount":8},"companion":{"pid":1,"cpuSeconds":{"Status":"available","Value":2},"workingSetBytes":{"Status":"unknown","Value":0}}}})", "2026-10-09T12:00:30.000Z")
        + Record(R"({"event":"machineSnapshot","snapshot":{"companion":{"pid":1}}})", "2026-02-30T12:00:00.000Z"));
    const auto result = pc::LogAnalysisImporter{}.Import({fixture.Root});
    Require(result.Records.size() == 3 && !result.Records.back().TimeMs && !result.Warnings.empty(), "Invalid calendar date became a valid timestamp");
    Require(result.Metrics.size() == 2 && result.Metrics[0].WorkingSetBytes == 0, "Known zero memory became unknown");
    Require(!result.Metrics[0].CpuSeconds && !result.Metrics[1].WorkingSetBytes && !result.Metrics[1].CpuPercent, "Denied/missing metrics were treated as known zero or comparable CPU");
    fixture.Put(L"games.jsonl", Record(R"({"event":"gameSnapshot","installedGames":[],"installedGameSources":[{"Status":"denied"}],"runningGames":[],"runningGameDetectionStatus":"unknown"})"));
    const auto games = pc::LogAnalysisImporter{}.Import({fixture.Root / L"games.jsonl"});
    Require(games.Games.Observed && !games.Games.InstalledCount && !games.Games.RunningCount, "Unavailable inventories were presented as zero games");
}
void BoundsAndMalformed() {
    Fixture fixture;
    fixture.Put(L"diagnostics.jsonl", std::string("\xEF\xBB\xBF") + Record(R"({"event":"feed_dropped","droppedEvents":2})") + "{broken\n" + std::string("\xFF") + "\n");
    const auto malformed = pc::LogAnalysisImporter{}.Import({fixture.Root});
    Require(malformed.Records.size() == 1 && malformed.InvalidLines == 2 && malformed.Gaps.size() == 1, "BOM, malformed UTF-8/JSON or gap projection contract failed");
    fixture.Put(L"oversized.jsonl", std::string(pc::LogAnalysisImporter::MaxLineBytes + 1, 'x') + "\n" + Record(R"({"event":"afterOversized"})"));
    const auto oversized = pc::LogAnalysisImporter{}.Import({fixture.Root / L"oversized.jsonl"});
    Require(oversized.InvalidLines == 1 && oversized.Records.size() == 1, "Oversized record was retained or blocked subsequent valid records");
    fixture.Put(L"many.jsonl", std::string(pc::LogAnalysisImporter::MaxScannedLines + 1, '\n'));
    const auto limited = pc::LogAnalysisImporter{}.Import({fixture.Root / L"many.jsonl"});
    Require(limited.LimitReached && limited.LinesRead == pc::LogAnalysisImporter::MaxScannedLines, "Blank lines bypassed the bounded scan budget");
    std::stop_source cancelled; cancelled.request_stop();
    Require(pc::LogAnalysisImporter{}.Import({fixture.Root}, cancelled.get_token()).Cancelled, "Import cancellation was ignored");
}
void DiscoveryDepthAndLatestInventory() {
    Fixture fixture; auto directory = fixture.Root;
    for (int level = 0; level < 35; ++level) directory /= L"d";
    std::filesystem::create_directories(directory);
    std::ofstream(directory / L"hidden.jsonl") << Record(R"({"event":"tooDeep"})");
    fixture.Put(L"games.jsonl", Record(R"({"event":"gameSnapshot","installedGames":[{"Name":"Newest known game"}],"runningGames":[],"runningGameDetectionStatus":"available"})")
        + Record(R"({"event":"gameSnapshot","installedGames":[],"runningGames":[],"runningGameDetectionStatus":"available"})", "unknown"));
    const auto inventory = pc::LogAnalysisImporter{}.Import({fixture.Root});
    Require(inventory.LimitReached && inventory.Records.size() == 2, "Directory depth limit did not bound discovery");
    Require(inventory.Games.InstalledCount == 1 && inventory.Games.TimeMs, "Unknown-time inventory replaced the latest known inventory");
    const auto snapshot = Record(R"({"event":"machineSnapshot","snapshot":{"companion":{"workingSetBytes":{"Value":0,"Status":"available"}}}})");
    std::filesystem::create_directory(fixture.Root / L"one"); std::filesystem::create_directory(fixture.Root / L"two");
    std::ofstream(fixture.Root / L"one" / L"diagnostics.jsonl") << snapshot;
    std::ofstream(fixture.Root / L"two" / L"diagnostics.jsonl") << snapshot;
    const auto metrics = pc::LogAnalysisImporter{}.Import({fixture.Root / L"one", fixture.Root / L"two"});
    Require(metrics.Metrics.size() == 2 && metrics.Metrics[0].Source != metrics.Metrics[1].Source
        && metrics.Metrics[0].Line == 1 && metrics.Metrics[1].Line == 1, "Unknown identities collapsed different scopes or lost metric provenance");
}
void ActualFixture(const std::filesystem::path& path, size_t expectedInvalid = 1) {
    const auto result = pc::LogAnalysisImporter{}.Import({path});
    Require(result.Records.size() == 14 && result.InvalidLines == expectedInvalid, "Expected 14 valid fixture records and the requested malformed-line count");
    Require(result.Scripts.size() == 1 && result.Games.InstalledCount == 2 && result.Games.RunningCount == 1 && result.Metrics.size() == 2,
        "Actual fixture should project one script, two installed/one running game, and two health samples");
    Require(!result.Metrics[0].CpuPercent && result.Metrics[1].CpuPercent && std::abs(*result.Metrics[1].CpuPercent - 0.625) < 0.0001,
        "Actual fixture CPU rate must preserve unknown first sample and machine-normalized 0.625 percent");
}
void EmbeddedSample() {
    std::filesystem::path path;
    { pc::TemporaryLogSample sample(GetModuleHandleW(nullptr)); path = sample.Path(); Require(std::filesystem::exists(path), "Sample was not written"); ActualFixture(path, 0); }
    Require(!std::filesystem::exists(path), "Synthetic sample temporary file was not cleaned up");
}
}
int wmain(int argc, wchar_t** argv) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    int status = 0;
    try {
        CorrelationAndProvenance(); CancellationDoesNotInventExecutions(); NestedAndNativeScripts(); UnknownMetricsAndDates(); BoundsAndMalformed();
        DiscoveryDepthAndLatestInventory(); EmbeddedSample();
        if (argc > 1) ActualFixture(argv[1]);
        std::cout << "Native log analysis contracts PASS\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; status = 1; }
    catch (...) { std::cerr << "Unexpected WinRT failure\n"; status = 1; }
    winrt::uninit_apartment(); return status;
}
