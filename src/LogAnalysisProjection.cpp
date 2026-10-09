#include "pch.h"
#include "LogAnalysisInternal.h"
#include <deque>
#include <set>

namespace pc {
namespace {
using namespace analysis;
using ValueType = winrt::Windows::Data::Json::JsonValueType;
Value ScriptMessage(Value message) {
    std::deque<Value> queue{message};
    for (size_t count = 0; !queue.empty() && count < 150; ++count) {
        auto item = queue.front(); queue.pop_front();
        const auto type = Type(item);
        if (Contains(type, L"PROVISIONING_SCRIPT_") || Contains(type, L"PowerShell")) return item;
        if (item && item.ValueType() == ValueType::Object)
            for (const auto& child : item.GetObjectW()) {
                auto value = child.Value();
                if (value.ValueType() == ValueType::String) {
                    try { value = json::JsonValue::Parse(value.GetString()); } catch (...) { }
                }
                if (queue.size() < 150 && (value.ValueType() == ValueType::Object || value.ValueType() == ValueType::Array)) queue.push_back(value);
            }
        else if (item && item.ValueType() == ValueType::Array)
            for (const auto& child : item.GetArray()) if (queue.size() < 150) queue.push_back(child);
    }
    return nullptr;
}
std::wstring Stringified(const Value& value) {
    if (!value) return L"Unknown — no observation captured";
    if (value.ValueType() == ValueType::Object) return json::Utf8ToWide(json::StringifyIndented(value.GetObjectW()));
    if (value.ValueType() == ValueType::Array) {
        json::JsonObject wrapper; wrapper.Insert(L"observed", value);
        return json::Utf8ToWide(json::StringifyIndented(wrapper));
    }
    return std::wstring(value.Stringify());
}
std::optional<size_t> ArrayCount(const Value& value) {
    return value && value.ValueType() == ValueType::Array ? std::optional<size_t>(value.GetArray().Size()) : std::nullopt;
}
void ReadGames(LogAnalysisResult& result, const ImportedLogRecord& record, const Value& inventory) {
    auto& games = result.Games;
    if (games.Observed && games.TimeMs && (!record.TimeMs || *record.TimeMs < *games.TimeMs)) return;
    games.Observed = true;
    games.TimeMs = record.TimeMs;
    games.Timestamp = record.Timestamp;
    games.Source = record.Source.wstring() + L":" + std::to_wstring(record.Line);
    games.InstalledJson = Stringified(Get(inventory, L"installedGames"));
    games.RunningJson = Stringified(Get(inventory, L"runningGames"));
    games.SourcesJson = Stringified(Get(inventory, L"installedGameSources"));
    games.LimitationsJson = Stringified(Get(inventory, L"inventoryLimitations"));
    games.RunningStatus = Text(Get(inventory, L"runningGameDetectionStatus"));
    if (games.RunningStatus.empty()) games.RunningStatus = L"Unknown";
    games.InstalledCount = ArrayCount(Get(inventory, L"installedGames"));
    games.RunningCount = ArrayCount(Get(inventory, L"runningGames"));
    const auto sources = Get(inventory, L"installedGameSources");
    if (games.InstalledCount == 0 && sources && sources.ValueType() == ValueType::Array) {
        bool available = false;
        for (const auto& source : sources.GetArray()) if (Text(Get(source, L"status")) == L"available") available = true;
        if (!available) games.InstalledCount.reset();
    }
    if (Contains(games.RunningStatus, L"unknown") || Contains(games.RunningStatus, L"denied") || games.RunningStatus == L"error") games.RunningCount.reset();
}
void ReadMetrics(LogAnalysisResult& result, const ImportedLogRecord& record, const Value& snapshot) {
    if (!record.TimeMs) return;
    const auto companion = Get(snapshot, L"companion");
    if (!companion || companion.ValueType() != ValueType::Object) return;
    CompanionMetricSample sample;
    sample.Source = record.Source; sample.Line = record.Line;
    const auto captured = Get(snapshot, L"capturedAtUtc");
    sample.TimeMs = Timestamp(captured).value_or(*record.TimeMs); sample.Timestamp = Timestamp(captured) ? Text(captured) : record.Timestamp; sample.Scope = record.Scope;
    const auto pid = Number(Get(companion, L"pid"));
    const auto start = Get(companion, L"startedAtUtc");
    const auto started = Get(start, L"value") ? Get(start, L"value") : start;
    const auto startStatus = Text(Get(start, L"status"));
    const bool knownStart = startStatus.empty() || CompareStringOrdinal(startStatus.c_str(), -1, L"available", -1, TRUE) == CSTR_EQUAL;
    if (pid && knownStart && Timestamp(started)) sample.ProcessIdentity = record.Scope + L"/" + Text(Get(companion, L"pid")) + L"/" + Text(started);
    sample.WorkingSetBytes = Metric(Get(companion, L"workingSetBytes"));
    sample.PrivateBytes = Metric(Get(companion, L"privateMemoryBytes"));
    sample.CpuSeconds = Metric(Get(companion, L"cpuSeconds"));
    sample.LogicalProcessors = Number(Get(Get(snapshot, L"machine"), L"logicalProcessorCount"));
    if (!result.Metrics.empty() && result.Metrics.back().TimeMs == sample.TimeMs
        && result.Metrics.back().Scope == sample.Scope && result.Metrics.back().ProcessIdentity == sample.ProcessIdentity) return;
    result.Metrics.push_back(std::move(sample));
}
void UpdateScript(ScriptLogGroup& group, const Value& message, std::wstring_view type) {
    auto copy = [&](std::wstring_view field, std::wstring& destination) {
        if (auto value = Field(message, field); value && value.ValueType() != ValueType::Null) destination = Text(value);
    };
    copy(L"scriptName", group.Name); if (group.Name.empty()) copy(L"scriptId", group.Name);
    copy(L"completeScript", group.Script); if (group.Script.empty()) copy(L"script", group.Script);
    copy(L"stdout", group.Stdout); copy(L"stderr", group.Stderr); copy(L"error", group.Error);
    group.ScriptObserved = group.ScriptObserved || Field(message, L"completeScript") || Field(message, L"script");
    group.StdoutObserved = group.StdoutObserved || Field(message, L"stdout");
    group.StderrObserved = group.StderrObserved || Field(message, L"stderr");
    group.ErrorObserved = group.ErrorObserved || Field(message, L"error");
    if (auto code = Number(Field(message, L"exitCode"))) group.ExitCode = code;
    if (Contains(type, L"DEPLOY")) group.Status = L"Received";
    if (Contains(type, L"STARTED")) group.Status = L"Started";
    if (Contains(type, L"REVOKE")) group.Status = L"Cancellation requested";
    if (Contains(type, L"PowerShellScriptReceived")) group.Status = L"Acknowledged";
    if (Contains(type, L"RESULT") || type == L"launchElevatedPowerShellScript") {
        if (Contains(type, L"RESULT")) group.Terminal = true;
        const auto reply = Get(Get(message, L"header"), L"isReply");
        const bool isReply = reply && reply.ValueType() == ValueType::Boolean && reply.GetBoolean();
        const auto status = Text(Field(message, L"status"));
        if (!status.empty()) group.Status = status;
        else if (Contains(type, L"RESULT") || isReply) {
            const auto success = Field(message, L"success");
            const bool failed = success && success.ValueType() == ValueType::Boolean && !success.GetBoolean();
            group.Status = Contains(group.Error, L"cancel") ? L"Cancelled" : Contains(group.Error, L"timeout") ? L"Timed out"
                : group.ExitCode ? (*group.ExitCode == 0 && !failed ? L"Succeeded" : L"Failed")
                : failed ? L"Launch failed" : L"Reply observed (no exit code)";
        } else group.Status = L"Received";
    }
}
void ReadScript(LogAnalysisResult& result, size_t index, Value message, std::map<std::wstring, size_t>& groups) {
    const auto& record = result.Records[index];
    if (auto script = ScriptMessage(message)) message = script;
    else if (record.Category != L"scripts") return;
    const auto type = Type(message);
    if (Contains(type, L"cancelProvisioningScript")) return;
    const auto deployment = Text(Field(message, L"deploymentId"));
    const auto execution = Text(Field(message, L"executionId"));
    if (execution.empty() && !deployment.empty() && Contains(type, L"REVOKE")) {
        for (auto& group : result.Scripts) if (group.Deployment == deployment && group.Scope == record.Scope) {
            group.Records.push_back(index);
            if (!group.Terminal && !group.ExitCode) group.Status = L"Cancellation requested";
        }
        return;
    }
    const auto tag = Text(Get(Get(message, L"header"), L"tag"));
    const auto key = record.Scope + L"|" + (!execution.empty() ? deployment + L"/" + execution + L"/" + Text(Field(message, L"stepIndex"))
        : !tag.empty() ? L"native/" + tag : L"unmatched/" + std::to_wstring(index));
    if (!groups.contains(key)) {
        groups[key] = result.Scripts.size();
        ScriptLogGroup group; group.Key = key; group.Scope = record.Scope; group.Deployment = deployment;
        group.Execution = execution.empty() ? (tag.empty() ? L"Unmatched event" : tag) : execution;
        result.Scripts.push_back(std::move(group));
    }
    auto& group = result.Scripts[groups[key]];
    group.Records.push_back(index);
    UpdateScript(group, message, type);
    if (group.Name.empty()) group.Name = L"PowerShell script";
}
void CalculateCpu(LogAnalysisResult& result) {
    std::map<std::wstring, size_t> previous;
    for (size_t index = 0; index < result.Metrics.size(); ++index) {
        auto& sample = result.Metrics[index];
        if (sample.ProcessIdentity.empty()) continue;
        if (auto found = previous.find(sample.ProcessIdentity); found != previous.end()) {
            const auto& prior = result.Metrics[found->second];
            const auto elapsed = (sample.TimeMs - prior.TimeMs) / 1000.0;
            if (elapsed > 0 && sample.CpuSeconds && prior.CpuSeconds && *sample.CpuSeconds >= *prior.CpuSeconds
                && sample.LogicalProcessors && *sample.LogicalProcessors > 0)
                sample.CpuPercent = (*sample.CpuSeconds - *prior.CpuSeconds) / elapsed / *sample.LogicalProcessors * 100;
        }
        previous[sample.ProcessIdentity] = index;
    }
}
} // namespace

void LogAnalysisProjection::Build(LogAnalysisResult& result, std::stop_token stop) {
    std::map<std::wstring, size_t> groups;
    for (size_t index = 0; index < result.Records.size() && !stop.stop_requested(); ++index) {
        const auto& record = result.Records[index];
        ++result.CategoryCounts[record.Category];
        if (record.Category == L"realtime" || record.Category == L"ipc") result.Communications.push_back(index);
        try {
            const auto raw = json::JsonObject::Parse(json::Utf8ToWide(record.Json));
            Value payload = Get(raw, L"payload"); if (!payload) payload = raw;
            const auto message = Message(payload);
            if (record.Type == L"gameSnapshot") ReadGames(result, record, payload);
            else if (record.Type == L"machineSnapshot") {
                const auto snapshot = Get(payload, L"snapshot"); ReadMetrics(result, record, snapshot);
                if (Get(snapshot, L"installedGames") || Get(snapshot, L"runningGames")) ReadGames(result, record, snapshot);
            }
            if (Contains(record.Type, L"dropped") || Contains(record.Type, L"disconnected") || Contains(record.Type, L"LimitReached")
                || record.Type == L"recordRejected" || record.Type == L"snapshotFailed") result.Gaps.push_back(index);
            else if (record.Type == L"diagnosticCoverageChanged" && (Contains(Text(Get(payload, L"coverage")), L"unavailable")
                || Contains(Text(Get(payload, L"coverage")), L"disconnected"))) result.Gaps.push_back(index);
            ReadScript(result, index, message, groups);
        } catch (...) { Warn(result, L"Projection unavailable for " + record.Source.filename().wstring() + L":" + std::to_wstring(record.Line)); }
    }
    CalculateCpu(result);
}

std::wstring LogAnalysisProjection::RecordDetail(const ImportedLogRecord& record) {
    std::wstring formatted;
    try { formatted = json::Utf8ToWide(json::StringifyIndented(json::JsonObject::Parse(json::Utf8ToWide(record.Json)))); }
    catch (...) { formatted = json::Utf8ToWide(record.Json); }
    return L"Source: " + record.Source.wstring() + L":" + std::to_wstring(record.Line) + L"\r\nTimestamp: "
        + (record.Timestamp.empty() ? L"Unknown" : record.Timestamp) + L"\r\nCategory: " + record.Category
        + L"\r\nSecrets flag: " + (record.IncludesSecrets ? L"Raw secrets included" : L"Redacted / unspecified") + L"\r\n\r\n" + formatted;
}
std::wstring LogAnalysisProjection::ScriptDetail(const ScriptLogGroup& group, const LogAnalysisResult& result) {
    auto observed = [](const std::wstring& value) { return value.empty() ? L"Not observed" : value; };
    auto output = [](const std::wstring& value, bool present) { return !present ? L"Not observed" : value.empty() ? L"(empty)" : value; };
    std::wstring detail = L"Script: " + group.Name + L"\r\nExecution: " + group.Execution + L"\r\nDeployment: "
        + observed(group.Deployment) + L"\r\nStatus: " + group.Status + L"\r\nScope: " + group.Scope
        + L"\r\nExit code: " + (group.ExitCode ? std::format(L"{:g}", *group.ExitCode) : L"Unknown")
        + L"\r\n\r\nSCRIPT (display only)\r\n" + output(group.Script, group.ScriptObserved) + L"\r\n\r\nSTDOUT\r\n"
        + output(group.Stdout, group.StdoutObserved) + L"\r\n\r\nSTDERR\r\n" + output(group.Stderr, group.StderrObserved)
        + L"\r\n\r\nERROR\r\n" + output(group.Error, group.ErrorObserved) + L"\r\n\r\nOBSERVATIONS\r\n";
    for (const auto index : group.Records) {
        const auto& record = result.Records[index];
        detail += record.Timestamp + L"  " + record.Type + L"  " + record.Source.filename().wstring() + L":" + std::to_wstring(record.Line) + L"\r\n";
    }
    return detail;
}
} // namespace pc
