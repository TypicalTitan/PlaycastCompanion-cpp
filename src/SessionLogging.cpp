#include "pch.h"
#include "SessionLogging.h"
#include "SessionLoggingClassifier.h"
#include "SessionLoggingPipe.h"
#include "SessionLoggingSnapshot.h"
#include "SessionLoggingWriter.h"

namespace pc {
using namespace sessionlog;
struct SessionLogger::Impl {
    AppConfig config;
    std::filesystem::path logRoot;
    bool headless, harness, active = false, stopped = false, includeSecrets = false;
    uint64_t generation = 0;
    std::wstring identity, directory, error, coverage = L"Waiting for Playcast diagnostic feed.", observer;
    std::wstring backendJson;
    mutable std::mutex mutex;
    std::condition_variable_any wake;
    bool snapshotRequested = false;
    std::unique_ptr<Writer> writer;
    std::unique_ptr<DiagnosticPipe> pipe;
    std::jthread snapshots;

    Impl(const AppConfig& value, bool guest, bool preview, std::filesystem::path root)
        : config(value), logRoot(std::move(root)), headless(guest), harness(preview) {
        try { directory = (logRoot.empty() ? Writer::DefaultRoot() : logRoot).wstring(); } catch (...) {}
        if (harness) return;
        if (headless) coverage = L"Passive guest capture; owner Companion is required for realtime and IPC feed capture.";
        else pipe = std::make_unique<DiagnosticPipe>(config.TargetUsername,
            [this](Object envelope) { Diagnostic(envelope); }, [this](std::wstring text) { Coverage(std::move(text)); });
        snapshots = std::jthread([this](std::stop_token stop) { SnapshotLoop(stop); });
    }
    void Write(std::wstring_view category, const Value& value) {
        if (active && writer) writer->Write(category, value, includeSecrets);
    }
    void Close(std::wstring_view reason) {
        if (writer) {
            Write(L"session", Make({{L"event", Text(L"sessionEnded")}, {L"reason", Text(reason)}, {L"bytesWritten", Number(static_cast<double>(writer->BytesWritten()))}}));
            error = writer->Error();
            writer.reset();
        }
    }
    void Coverage(std::wstring text) {
        std::lock_guard lock(mutex);
        if (stopped || coverage == text) return;
        coverage = std::move(text);
        if (coverage.find(L"disconnected") != std::wstring::npos || coverage.starts_with(L"Waiting")) observer.clear();
        Write(L"diagnostics", Make({{L"event", Text(L"diagnosticCoverageChanged")}, {L"coverage", Text(coverage)}}));
    }
    void Diagnostic(const Object& envelope) {
        std::lock_guard lock(mutex);
        if (stopped) return;
        const auto category = json::GetString(envelope, L"category");
        const auto payload = json::GetObject(envelope, L"payload");
        const auto event = json::GetString(payload, L"event");
        if (category == L"diagnostics" && event == L"observer_started") {
            std::wstring channels;
            for (const auto& [flag, label] : std::initializer_list<std::pair<const wchar_t*, const wchar_t*>>{
                {L"realtimeWebSocket", L"realtime WebSocket"}, {L"realtimeHttp", L"realtime fetch HTTP"},
                {L"realtimeAxios", L"realtime Axios HTTP"}, {L"nativeIpc", L"native IPC"}}) {
                if (!json::GetBool(payload, flag, false)) continue;
                if (!channels.empty()) channels += L", ";
                channels += label;
            }
            observer = channels.empty() ? L"Host observers started; capture channels unavailable."
                : L"Host observers active: " + channels + L".";
        }
        if (category == L"diagnostics" && event == L"observer_failed") observer = L"Host observer failed; realtime capture unavailable.";
        Write(category == L"state" ? L"diagnostics" : category, envelope);
        if (active) for (const auto& [relatedCategory, related] : EventClassifier::RelatedRecords(envelope)) Write(relatedCategory, related);
    }
    void SnapshotLoop(std::stop_token stop) {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            while (!stop.stop_requested()) {
                uint64_t capturedGeneration = 0;
                {
                    std::unique_lock lock(mutex);
                    wake.wait_for(lock, stop, std::chrono::seconds(config.SessionLogging.SnapshotIntervalSeconds), [this] { return snapshotRequested; });
                    snapshotRequested = false;
                    if (stop.stop_requested()) break;
                    if (!active || !writer) continue;
                    capturedGeneration = generation;
                }
                try {
                    auto snapshot = SnapshotSource().Capture(config.TargetUsername, config.IncludeDisconnectedSessions, stop);
                    std::lock_guard lock(mutex);
                    if (stopped || !active || capturedGeneration != generation) continue;
                    if (!backendJson.empty()) snapshot.Insert(L"companionBackends", json::Parse(json::WideToUtf8(backendJson)));
                    Write(L"diagnostics", Make({{L"event", Text(L"machineSnapshot")}, {L"snapshot", snapshot}}));
                    auto games = Make({{L"event", Text(L"gameSnapshot")}});
                    for (const auto name : {L"installedGames", L"runningGames", L"installedGameSources", L"inventoryLimitations", L"runningGameDetectionStatus", L"processCoverage", L"targetSessionIds"}) games.Insert(name, Get(snapshot, name));
                    Write(L"games", games);
                } catch (...) { std::lock_guard lock(mutex); Write(L"diagnostics", Make({{L"event", Text(L"snapshotFailed")}})); }
            }
            winrt::uninit_apartment();
        } catch (...) { std::lock_guard lock(mutex); error = L"Machine snapshot worker unavailable."; }
    }
};

SessionLogger::SessionLogger(const AppConfig& config, bool headless, bool harness, std::filesystem::path logRoot)
    : impl_(std::make_unique<Impl>(config, headless, harness, std::move(logRoot))) {}
SessionLogger::~SessionLogger() { Stop(); }
void SessionLogger::UpdateSession(bool active, std::wstring_view reason, std::wstring_view identity) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped || impl_->harness) return;
    active = active && impl_->config.SessionLogging.Enabled;
    if (active == impl_->active && (!active || identity == impl_->identity)) return;
    if (impl_->active) impl_->Close(active ? L"Guest session identity changed" : reason);
    impl_->active = active;
    impl_->identity = identity;
    ++impl_->generation;
    if (!active) return;
    impl_->writer = std::make_unique<Writer>(impl_->config.SessionLogging, impl_->logRoot);
    impl_->directory = impl_->writer->Directory().wstring();
    impl_->error.clear();
    impl_->Write(L"session", Make({{L"event", Text(L"sessionStarted")}, {L"reason", Text(reason)},
        {L"targetUsername", Text(impl_->config.TargetUsername)}, {L"sessionIdentity", Text(identity)}, {L"coverage", Text(impl_->coverage)}, {L"observerStatus", Text(impl_->observer)},
        {L"snapshotIntervalSeconds", Number(impl_->config.SessionLogging.SnapshotIntervalSeconds)}, {L"maximumSessionBytes", Number(impl_->config.SessionLogging.MaxSessionBytes)},
        {L"maximumFileBytes", Number(impl_->config.SessionLogging.MaxFileBytes)}, {L"retainedDays", Number(impl_->config.SessionLogging.RetentionDays)}}));
    impl_->snapshotRequested = true;
    impl_->wake.notify_one();
}
void SessionLogger::UpdateBackends(std::wstring_view statusJson) { std::lock_guard lock(impl_->mutex); impl_->backendJson = statusJson; }
void SessionLogger::SetIncludeSecrets(bool enabled) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->includeSecrets == enabled) return;
    impl_->includeSecrets = enabled;
    if (impl_->writer) impl_->writer->Write(L"session", Make({{L"event", Text(L"secretCaptureChanged")}, {L"includeSecrets", Boolean(enabled)}}), false);
}
bool SessionLogger::IncludeSecrets() const { std::lock_guard lock(impl_->mutex); return impl_->includeSecrets; }
std::wstring SessionLogger::Directory() const { std::lock_guard lock(impl_->mutex); return impl_->directory; }
std::wstring SessionLogger::Status() const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->harness) return L"Preview mode: session logger and diagnostic pipe are disabled.";

    if (!impl_->config.SessionLogging.Enabled) return L"Session logging disabled.";
    const auto error = impl_->writer ? impl_->writer->Error() : impl_->error;
    return std::wstring(impl_->active ? L"Session logging active. " : L"Waiting for an active Nonsole Mode session. ") + impl_->coverage + L" " + impl_->observer + (error.empty() ? L"" : L" " + error);
}
void SessionLogger::Stop() {
    { std::lock_guard lock(impl_->mutex); if (impl_->stopped) return; impl_->Close(L"Companion shutdown"); impl_->active = false; impl_->stopped = true; }
    if (impl_->pipe) impl_->pipe->Stop();
    impl_->snapshots.request_stop();
    impl_->wake.notify_all();
    if (impl_->snapshots.joinable()) impl_->snapshots.join();
}
}
