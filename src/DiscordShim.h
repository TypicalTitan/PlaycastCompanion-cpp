#pragma once
// Optional game pass-through (default OFF).
//
// DiscordShimServer — GUEST (headless) session only. Binds the lowest free
// \\.\pipe\discord-ipc-N for N in 1..9 (NEVER 0: the owner's real Discord holds
// it and we must not contend), single instance, byte mode. Per connection:
// GetNamedPipeClientProcessId -> session id via ProcessIdToSessionId; drop and
// log clients from another session. Read the opcode-0 handshake frame, extract
// client_id (the game's Application ID), read the exe ProductName as a hint,
// reply with a minimal READY frame, relay {"event":"game","clientId":..,
// "procHint":..} to the owner over the ShimPipeName pipe, then read and
// DISCARD frames until the game closes, then relay {"event":"clear"}. Logs
// startup slot occupancy, every connection, and a periodic no-traffic heartbeat.
//
// DiscordChannelListener — OWNER session only. Hosts \\.\pipe\<ShimPipeName>
// with a DACL: owner SID full control + the target account's SID (resolved via
// LookupAccountName) read/write (fallback: Authenticated Users). For each
// connection: ImpersonateNamedPipeClient -> verify the caller's account name
// equals TargetUsername (case-insensitive, domain stripped) -> RevertToSelf;
// reject anything else. Read one newline-terminated JSON message (<= 8 KB),
// resolve via GameNameCache, call DiscordPresence::SetHostedGame.
#include "Config.h"
#include "DiscordPresence.h"
#include <memory>

namespace pc {
class DiscordShimServer {
public:
    explicit DiscordShimServer(const AppConfig& cfg);
    ~DiscordShimServer();  // Stop()s
    void Start();
    void Stop();  // returns within ~3 s

private:  // module owner may extend
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class DiscordChannelListener {
public:
    DiscordChannelListener(const AppConfig& cfg, DiscordPresence& presence);
    ~DiscordChannelListener();  // Stop()s
    void Start();
    void Stop();  // returns within ~3 s

private:  // module owner may extend
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace pc
