#include "pch.h"
#include "SessionLoggingLifecycle.h"

namespace pc::sessionlog {
namespace {
Object UnavailableFinal(std::wstring_view reason) {
    Array errors;
    errors.Append(Make({{L"Source", Text(L"terminalSnapshot")}, {L"Status", Text(L"unavailable")}, {L"Message", Text(reason)}}));
    return Make({{L"capturedAtUtc", Text(UtcNow())}, {L"captureStatus", Text(L"partial")},
        {L"installedGames", Null()}, {L"runningGames", Null()}, {L"installedGameSources", Null()},
        {L"inventoryLimitations", errors}, {L"targetSessionIds", Null()}, {L"runningGameDetectionStatus", Text(L"unknown")},
        {L"processCoverage", Make({{L"Status", Text(L"unavailable")}, {L"Errors", errors}})}, {L"companion", CaptureCompanion()}});
}
}

struct SnapshotCapture::State {
    explicit State(std::shared_ptr<SnapshotSource> value) : source(std::move(value)) {}
    std::shared_ptr<SnapshotSource> source;
    std::mutex mutex;
    std::condition_variable_any finished;
    bool busy = false;
};
namespace {
struct CaptureAttempt {
    std::mutex mutex;
    std::condition_variable_any finished;
    std::stop_source cancellation;
    bool complete = false;
    FinalSnapshot result;
};
void RunCapture(const std::shared_ptr<SnapshotSource>& source, const AppConfig& config, CaptureAttempt& attempt) {
    bool apartment = false;
    FinalSnapshot result;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment = true;
        result.snapshot = source->Capture(config.TargetUsername, config.IncludeDisconnectedSessions, attempt.cancellation.get_token());
    } catch (const winrt::hresult_error& exception) { result.error = exception.message().c_str(); }
    catch (const std::exception& exception) { result.error = json::Utf8ToWide(exception.what()); }
    catch (...) { result.error = L"Machine snapshot failed."; }
    { std::lock_guard lock(attempt.mutex); attempt.result = std::move(result); attempt.complete = true; }
    attempt.finished.notify_all();
    if (apartment) winrt::uninit_apartment();
}
}

SnapshotCapture::SnapshotCapture(std::shared_ptr<SnapshotSource> source) : state_(std::make_shared<State>(std::move(source))) {}
FinalSnapshot SnapshotCapture::Capture(const AppConfig& config, std::stop_token stop,
    std::optional<std::chrono::milliseconds> budget) {
    const auto deadline = budget ? std::chrono::steady_clock::now() + *budget : std::chrono::steady_clock::time_point::max();
    const auto unavailable = [&] { return FinalSnapshot{Object(), stop.stop_requested() ? L"Machine snapshot cancelled."
        : L"Terminal machine snapshot exceeded its cancellation budget; inventory is unknown."}; };
    {
        std::unique_lock lock(state_->mutex);
        const bool available = budget ? state_->finished.wait_until(lock, stop, deadline, [&] { return !state_->busy; })
            : state_->finished.wait(lock, stop, [&] { return !state_->busy; });
        if (!available || stop.stop_requested()) return unavailable();
        state_->busy = true;
    }
    auto attempt = std::make_shared<CaptureAttempt>();
    try {
        std::thread([state = state_, attempt, config] {
            RunCapture(state->source, config, *attempt);
            { std::lock_guard lock(state->mutex); state->busy = false; }
            state->finished.notify_all();
        }).detach();
    } catch (...) {
        { std::lock_guard lock(state_->mutex); state_->busy = false; }
        state_->finished.notify_all();
        return {Object(), L"Machine snapshot worker could not be started."};
    }
    std::unique_lock lock(attempt->mutex);
    const bool available = budget ? attempt->finished.wait_until(lock, stop, deadline, [&] { return attempt->complete; })
        : attempt->finished.wait(lock, stop, [&] { return attempt->complete; });
    if (available && !stop.stop_requested()) return attempt->result;
    lock.unlock();
    attempt->cancellation.request_stop();
    return unavailable();
}

FinalSnapshot CaptureFinal(SnapshotCapture& source, const AppConfig& config, std::chrono::milliseconds budget) {
    auto result = source.Capture(config, {}, budget);
    if (!result.error.empty()) result.snapshot = UnavailableFinal(result.error);
    return result;
}

void WriteSnapshot(Writer& writer, Object snapshot, bool includeSecrets, std::wstring_view phase,
    std::wstring_view backendJson) {
    if (!backendJson.empty()) {
        try { snapshot.Insert(L"companionBackends", json::Parse(json::WideToUtf8(backendJson))); }
        catch (...) { snapshot.Insert(L"companionBackends", Metric(Null(), L"unavailable", L"Backend status JSON could not be parsed.")); }
    }
    writer.Write(L"diagnostics", Make({{L"event", Text(L"machineSnapshot")}, {L"phase", Text(phase)}, {L"snapshot", snapshot}}), includeSecrets);
    auto games = Make({{L"event", Text(L"gameSnapshot")}, {L"phase", Text(phase)}, {L"capturedAtUtc", Get(snapshot, L"capturedAtUtc")}});
    for (const auto name : {L"installedGames", L"runningGames", L"installedGameSources", L"inventoryLimitations", L"runningGameDetectionStatus", L"processCoverage", L"targetSessionIds"}) games.Insert(name, Get(snapshot, name));
    writer.Write(L"games", games, includeSecrets);
}
}
