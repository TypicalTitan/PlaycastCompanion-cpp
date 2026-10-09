#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace pc {
struct ImportedLogRecord {
    std::filesystem::path Source;
    size_t Line = 0;
    std::wstring Category, Type, Timestamp, Direction, Scope;
    std::optional<int64_t> TimeMs;
    std::string Json;
    bool IncludesSecrets = false;
};
struct ScriptLogGroup {
    std::wstring Key, Scope, Deployment, Execution, Name, Status = L"Observed";
    std::wstring Script, Stdout, Stderr, Error;
    bool ScriptObserved = false, StdoutObserved = false, StderrObserved = false, ErrorObserved = false;
    bool Terminal = false;
    std::optional<double> ExitCode;
    std::vector<size_t> Records;
};
struct CompanionMetricSample {
    std::filesystem::path Source;
    size_t Line = 0;
    int64_t TimeMs = 0;
    std::wstring Timestamp, Scope, ProcessIdentity;
    std::optional<double> WorkingSetBytes, PrivateBytes, CpuSeconds, CpuPercent, LogicalProcessors;
};
struct PassiveGameInventory {
    std::optional<int64_t> TimeMs;
    bool Observed = false;
    std::wstring Timestamp, Source, InstalledJson, RunningJson, SourcesJson, LimitationsJson;
    std::wstring RunningStatus = L"Unknown";
    std::optional<size_t> InstalledCount, RunningCount;
};
struct LogAnalysisResult {
    std::vector<ImportedLogRecord> Records;
    std::vector<ScriptLogGroup> Scripts;
    std::vector<CompanionMetricSample> Metrics;
    std::vector<size_t> Communications, Gaps;
    std::map<std::wstring, size_t> CategoryCounts;
    PassiveGameInventory Games;
    std::vector<std::wstring> Warnings;
    uint64_t BytesRead = 0;
    size_t FilesRead = 0, InvalidLines = 0, SecretRecords = 0, LinesRead = 0;
    bool LimitReached = false, Cancelled = false;
};

/// Passive local JSONL importer. Never executes, uploads, or evaluates imported content.
class LogAnalysisImporter {
public:
    static constexpr uint64_t MaxImportBytes = 128ull * 1024 * 1024;
    static constexpr size_t MaxRecords = 50000;
    static constexpr size_t MaxScannedLines = 100000;
    static constexpr size_t MaxLineBytes = 10 * 1024 * 1024;
    LogAnalysisResult Import(const std::vector<std::filesystem::path>& sources,
        std::stop_token stop = {}) const;
};

class LogAnalysisProjection {
public:
    static void Build(LogAnalysisResult& result, std::stop_token stop = {});
    static std::wstring RecordDetail(const ImportedLogRecord& record);
    static std::wstring ScriptDetail(const ScriptLogGroup& group, const LogAnalysisResult& result);
};
} // namespace pc
