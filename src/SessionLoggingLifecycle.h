#pragma once
#include "SessionLoggingSnapshot.h"
#include "SessionLoggingWriter.h"

namespace pc::sessionlog {
struct FinalSnapshot { Object snapshot{nullptr}; std::wstring error; };
class SnapshotCapture {
public:
    explicit SnapshotCapture(std::shared_ptr<SnapshotSource> source);
    FinalSnapshot Capture(const AppConfig& config, std::stop_token stop,
        std::optional<std::chrono::milliseconds> budget = {});
private:
    struct State;
    std::shared_ptr<State> state_;
};
FinalSnapshot CaptureFinal(SnapshotCapture& source, const AppConfig& config, std::chrono::milliseconds budget);
void WriteSnapshot(Writer& writer, Object snapshot, bool includeSecrets, std::wstring_view phase,
    std::wstring_view backendJson);
}
