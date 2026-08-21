#pragma once
// Minimal Discord Rich Presence client over the local IPC named pipe
// \\.\pipe\discord-ipc-0..9. Frame = int32 opcode LE + int32 length LE + UTF-8
// JSON. Handshake: opcode 0 {"v":1,"client_id":"<ApplicationId>"}; then
// opcode 1 {"cmd":"SET_ACTIVITY","nonce":<guid>,"args":{"pid":<pid>,
// "activity":{...}}}. Activity: timestamps.start (unix ms of activation),
// details (HostingTemplate with {game} when GamePassthroughEnabled and a hosted
// game is set, else Details), optional state, assets{large_image,large_text}.
// Re-send every 30 s or immediately when SetHostedGame changes. A response
// containing "evt":"ERROR" is a failure. Clear sends activity null then closes.
// Discord absent / no Application ID => quiet retry / no-op, never an error.
#include "Config.h"
#include <optional>
#include <string>

namespace pc {
class DiscordPresence {
public:
    explicit DiscordPresence(const DiscordConfig& cfg);
    ~DiscordPresence();  // Clear()s
    DiscordPresence(const DiscordPresence&) = delete;
    DiscordPresence& operator=(const DiscordPresence&) = delete;

    /// Enabled && ApplicationId is a decimal snowflake.
    bool IsConfigured() const;
    std::wstring StatusText() const;  // "Off" / "Not linked — set Discord.ApplicationId in config.json" / "Ready — shows while a guest is hosting" / "Live on your profile" / "Discord not reachable — retrying"
    void SetActive();   // idempotent; starts the loop if configured
    void Clear();       // stops loop, clears activity, closes pipe, forgets hosted game; returns within ~3 s
    void SetHostedGame(std::optional<std::wstring> name);  // nudges an immediate re-send

private:  // module owner may extend
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace pc
