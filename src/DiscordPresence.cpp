// Port of DiscordPresence.cs: a minimal Discord Rich Presence client over the
// local IPC named pipe. No third-party code; overlapped pipe I/O so every wait
// is bounded (3 s reads) and unblockable from Clear() via the stop event.
#include "pch.h"
#include "DiscordPresence.h"

#include "Json.h"
#include "Log.h"

#include <cstdint>
#include <cstring>

namespace pc {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxFrameBytes = 1000000;   // C#: length < 0 or > 1_000_000 => malformed
constexpr DWORD kReadTimeoutMs = 3000;            // whole reply frame (header + body)
constexpr DWORD kWriteTimeoutMs = 3000;
constexpr DWORD kClearTimeoutMs = 2000;           // activity:null on Clear()
constexpr DWORD kHeartbeatMs = 30000;             // re-send cadence while live
constexpr DWORD kRetryMs = 30000;                 // steady-state back-off after repeated failures
constexpr DWORD kPipeBusyWaitMs = 250;
constexpr const char* kErrorMarker = "\"evt\":\"ERROR\"";

/// char.IsWhiteSpace(): what string.Trim()/IsNullOrWhiteSpace consider blank.
bool IsWhite(wchar_t c) noexcept {
    switch (c) {
    case L'\t': case L'\n': case L'\v': case L'\f': case L'\r': case L' ':
    case 0x0085: case 0x00A0: case 0x1680:
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
        return true;
    default:
        return c >= 0x2000 && c <= 0x200A;
    }
}

std::wstring_view TrimView(std::wstring_view s) noexcept {
    while (!s.empty() && IsWhite(s.front())) s.remove_prefix(1);
    while (!s.empty() && IsWhite(s.back())) s.remove_suffix(1);
    return s;
}

bool IsBlank(std::wstring_view s) noexcept { return TrimView(s).empty(); }

/// ulong.TryParse on the trimmed id: decimal digits only, fits in 64 bits.
bool IsSnowflake(std::wstring_view s) noexcept {
    if (s.empty() || s.size() > 20) return false;
    for (const wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
    }
    return s.size() < 20 || s <= std::wstring_view(L"18446744073709551615");
}

std::wstring ReplaceAll(std::wstring text, std::wstring_view from, std::wstring_view to) {
    if (from.empty()) return text;
    std::size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::wstring::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

struct LocalFreeDeleter {
    void operator()(void* p) const noexcept { if (p) LocalFree(p); }
};

std::wstring Win32Message(DWORD error) {
    wchar_t* raw = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    const std::unique_ptr<wchar_t, LocalFreeDeleter> owner(raw);
    std::wstring message = (len && raw) ? std::wstring(raw, len) : std::format(L"error {}", error);
    while (!message.empty() && (IsWhite(message.back()) || message.back() == L'.')) message.pop_back();
    return message;
}

[[noreturn]] void Fail(std::wstring_view message) {
    throw std::runtime_error(json::WideToUtf8(message));
}

[[noreturn]] void FailWin32(std::wstring_view what, DWORD error) {
    Fail(std::format(L"{}: {}", what, Win32Message(error)));
}

bool IsDisconnect(DWORD error) noexcept {
    return error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED || error == ERROR_NO_DATA;
}

DWORD RemainingMs(Clock::time_point deadline) noexcept {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (left <= 0) return 0;
    return static_cast<DWORD>(std::min<long long>(left, 0x7FFFFFFF));
}

Clock::time_point DeadlineIn(DWORD ms) noexcept {
    return Clock::now() + std::chrono::milliseconds(ms);
}

/// Guid.NewGuid().ToString() ("D" format, lowercase).
std::wstring NewNonce() {
    GUID g{};
    if (FAILED(CoCreateGuid(&g))) Fail(L"CoCreateGuid failed");
    return std::format(L"{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                       g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                       g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

/// One overlapped pipe operation: OVERLAPPED + its manual-reset completion event.
struct OverlappedOp {
    OVERLAPPED ov{};
    UniqueHandle event;
    OverlappedOp() : event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!event.valid()) FailWin32(L"CreateEvent", GetLastError());
        ov.hEvent = event.get();
    }
    OverlappedOp(const OverlappedOp&) = delete;
    OverlappedOp& operator=(const OverlappedOp&) = delete;
};

/// Finishes the ReadFile/WriteFile just issued on `op` (`started` is its return
/// value, `error` the GetLastError() captured immediately after). Blocks until
/// the operation completes, the deadline passes, or `stopEvent` (optional) is
/// signalled; the latter two cancel the I/O and throw. Returns bytes transferred.
DWORD CompleteIo(HANDLE pipeHandle, OverlappedOp& op, BOOL started, DWORD error,
                 Clock::time_point deadline, HANDLE stopEvent, const wchar_t* what) {
    if (!started && error != ERROR_MORE_DATA) {
        if (error != ERROR_IO_PENDING) {
            if (IsDisconnect(error)) Fail(L"pipe closed");
            FailWin32(what, error);
        }
        HANDLE handles[2] = { op.event.get(), stopEvent };
        const DWORD count = stopEvent ? 2u : 1u;
        const DWORD wait = WaitForMultipleObjects(count, handles, FALSE, RemainingMs(deadline));
        if (wait != WAIT_OBJECT_0) {
            const DWORD waitError = GetLastError();
            CancelIoEx(pipeHandle, &op.ov);
            DWORD ignored = 0;
            GetOverlappedResult(pipeHandle, &op.ov, &ignored, TRUE);  // the OVERLAPPED must outlive the I/O
            if (wait == WAIT_OBJECT_0 + 1) Fail(L"cancelled");
            if (wait == WAIT_TIMEOUT) Fail(std::format(L"{} timed out", what));
            FailWin32(L"WaitForMultipleObjects", waitError);
        }
    }
    DWORD transferred = 0;
    if (!GetOverlappedResult(pipeHandle, &op.ov, &transferred, FALSE)) {
        const DWORD resultError = GetLastError();
        if (resultError != ERROR_MORE_DATA) {
            if (IsDisconnect(resultError)) Fail(L"pipe closed");
            FailWin32(what, resultError);
        }
    }
    return transferred;
}

void WriteAll(HANDLE pipeHandle, const std::vector<unsigned char>& data, Clock::time_point deadline, HANDLE stopEvent) {
    OverlappedOp op;
    const BOOL ok = WriteFile(pipeHandle, data.data(), static_cast<DWORD>(data.size()), nullptr, &op.ov);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    const DWORD written = CompleteIo(pipeHandle, op, ok, error, deadline, stopEvent, L"pipe write");
    if (written != data.size()) Fail(L"short pipe write");
}

void ReadExact(HANDLE pipeHandle, unsigned char* buffer, std::size_t length, Clock::time_point deadline, HANDLE stopEvent) {
    std::size_t offset = 0;
    while (offset < length) {
        OverlappedOp op;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(length - offset, 0x7FFFFFFF));
        const BOOL ok = ReadFile(pipeHandle, buffer + offset, chunk, nullptr, &op.ov);
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        const DWORD read = CompleteIo(pipeHandle, op, ok, error, deadline, stopEvent, L"pipe read");
        if (read == 0) Fail(L"pipe closed");
        offset += read;
    }
}

/// int32 opcode LE + int32 length LE + UTF-8 JSON.
std::vector<unsigned char> BuildFrame(std::int32_t opcode, const std::string& body) {
    if (body.size() > kMaxFrameBytes) Fail(L"IPC frame too large");
    std::vector<unsigned char> frame(8 + body.size());
    const std::int32_t length = static_cast<std::int32_t>(body.size());
    std::memcpy(frame.data(), &opcode, sizeof opcode);
    std::memcpy(frame.data() + 4, &length, sizeof length);
    if (!body.empty()) std::memcpy(frame.data() + 8, body.data(), body.size());
    return frame;
}

/// Reads one frame (header + body) within `deadline`; returns the UTF-8 body.
std::string ReadFrame(HANDLE pipeHandle, Clock::time_point deadline, HANDLE stopEvent) {
    unsigned char header[8]{};
    ReadExact(pipeHandle, header, sizeof header, deadline, stopEvent);
    std::int32_t length = 0;
    std::memcpy(&length, header + 4, sizeof length);
    if (length < 0 || static_cast<std::size_t>(length) > kMaxFrameBytes) Fail(L"malformed IPC frame");
    std::string body(static_cast<std::size_t>(length), '\0');
    if (length > 0) ReadExact(pipeHandle, reinterpret_cast<unsigned char*>(body.data()), body.size(), deadline, stopEvent);
    return body;
}

/// MTA for the worker thread (it builds JSON through WinRT), torn down on exit.
struct ApartmentGuard {
    bool initialized = false;
    ApartmentGuard() noexcept {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialized = true;
        } catch (...) {
        }
    }
    ~ApartmentGuard() { if (initialized) winrt::uninit_apartment(); }
    ApartmentGuard(const ApartmentGuard&) = delete;
    ApartmentGuard& operator=(const ApartmentGuard&) = delete;
};

}  // namespace

struct DiscordPresence::Impl {
    const DiscordConfig& cfg;
    std::mutex lifecycle;          // serialises SetActive() / Clear()
    mutable std::mutex state;      // status + hostedGame (read from UI/listener threads)
    std::wstring status;
    std::optional<std::wstring> hostedGame;
    std::jthread worker;           // joinable <=> loop running (C# _cts != null)
    UniqueHandle stopEvent;        // manual-reset; set by Clear(), part of every worker wait
    UniqueHandle nudge;            // auto-reset; set by SetHostedGame() (C# SemaphoreSlim(0,1))
    UniqueHandle pipe;             // worker-owned while running; Clear() touches it only after join
    long long activeSinceMs = 0;
    bool loggedUnavailable = false;

    explicit Impl(const DiscordConfig& c)
        : cfg(c),
          stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          nudge(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        if (!stopEvent.valid() || !nudge.valid()) FailWin32(L"CreateEvent", GetLastError());
        status = InitialStatus();
    }

    bool IsConfigured() const { return cfg.Enabled && IsSnowflake(TrimView(cfg.ApplicationId)); }

    std::wstring InitialStatus() const {
        if (!cfg.Enabled) return L"Off";
        if (IsConfigured()) return L"Ready — shows while a guest is hosting";
        return L"Not linked — set Discord.ApplicationId in config.json";
    }

    void SetStatus(std::wstring text) {
        std::lock_guard lock(state);
        status = std::move(text);
    }

    std::optional<std::wstring> HostedGame() const {
        std::lock_guard lock(state);
        return hostedGame;
    }

    void ClosePipe() noexcept { pipe.reset(); }

    void Run(const std::stop_token& st);
    void ConnectAndHandshake(const std::stop_token& st);
    void SendActivity(HANDLE stop);
    void SendCommand(const json::JsonObject* activity, Clock::time_point deadline, HANDLE stop);
    void SendFrame(std::int32_t opcode, const std::string& body, Clock::time_point deadline, HANDLE stop);
    std::string ReadReply(HANDLE stop);
};

void DiscordPresence::Impl::Run(const std::stop_token& st) {
    const HANDLE stop = stopEvent.get();
    int failureStreak = 0;  // worker-local: first retries come fast, then settle at kRetryMs
    while (!st.stop_requested()) {
        std::string failure;
        try {
            if (!pipe.valid()) ConnectAndHandshake(st);
            SendActivity(stop);
            failureStreak = 0;
            if (loggedUnavailable) LogInfo(L"Discord presence restored");
            loggedUnavailable = false;
            SetStatus(L"Live on your profile");
            // Wait for a nudge (a captured game changed) or the 30 s heartbeat,
            // whichever comes first; the stop event ends the loop at once.
            HANDLE handles[2] = { stop, nudge.get() };
            if (WaitForMultipleObjects(2, handles, FALSE, kHeartbeatMs) == WAIT_OBJECT_0) return;
            continue;
        } catch (const winrt::hresult_error& ex) {
            failure = json::WideToUtf8(std::wstring(ex.message()));
        } catch (const std::exception& ex) {
            failure = ex.what();
        } catch (...) {
            failure = "unknown error";
        }
        if (st.stop_requested()) return;
        SetStatus(L"Discord not reachable — retrying");
        // 2 s, 4 s, 8 s, 16 s, then kRetryMs: a Discord client that was still
        // starting up when the guest arrived gets the presence within seconds.
        if (failureStreak < 5) ++failureStreak;
        const DWORD delay = failureStreak >= 5 ? kRetryMs : std::min<DWORD>(kRetryMs, 1000u << failureStreak);
        if (!loggedUnavailable) {
            loggedUnavailable = true;
            LogInfo(std::format("Discord presence unavailable ({}); retrying in {}s (backing off to {}s)",
                                failure, delay / 1000, kRetryMs / 1000));
        }
        ClosePipe();
        if (WaitForSingleObject(stop, delay) == WAIT_OBJECT_0) return;
    }
}

void DiscordPresence::Impl::ConnectAndHandshake(const std::stop_token& st) {
    const std::wstring clientId(TrimView(cfg.ApplicationId));
    const HANDLE stop = stopEvent.get();
    for (int i = 0; i < 10; ++i) {
        if (st.stop_requested()) Fail(L"cancelled");
        const std::wstring path = std::format(L"\\\\.\\pipe\\discord-ipc-{}", i);
        HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED, nullptr);
        if (raw == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(path.c_str(), kPipeBusyWaitMs)) {
            raw = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED, nullptr);
        }
        if (raw == INVALID_HANDLE_VALUE) continue;
        pipe.reset(raw);
        try {
            json::JsonObject handshake;
            handshake.Insert(L"v", json::JsonValue::CreateNumberValue(1));
            handshake.Insert(L"client_id", json::JsonValue::CreateStringValue(clientId));
            SendFrame(0, json::Stringify(handshake), DeadlineIn(kWriteTimeoutMs), stop);
            const std::string ready = ReadReply(stop);
            if (ready.find(kErrorMarker) != std::string::npos) {
                Fail(L"Discord rejected the handshake (check the Application ID)");
            }
            LogInfo(std::format(L"Discord presence connected (discord-ipc-{})", i));
            return;
        } catch (...) {
            ClosePipe();
            if (st.stop_requested()) throw;
        }
    }
    Fail(L"no Discord IPC pipe answered (is Discord running?)");
}

void DiscordPresence::Impl::SendActivity(HANDLE stop) {
    json::JsonObject assets;
    if (!IsBlank(cfg.LargeImageKey)) {
        assets.Insert(L"large_image", json::JsonValue::CreateStringValue(std::wstring(TrimView(cfg.LargeImageKey))));
    }
    if (!IsBlank(cfg.LargeImageText)) {
        assets.Insert(L"large_text", json::JsonValue::CreateStringValue(std::wstring(TrimView(cfg.LargeImageText))));
    }

    json::JsonObject timestamps;
    timestamps.Insert(L"start", json::JsonValue::CreateNumberValue(static_cast<double>(activeSinceMs)));
    json::JsonObject activity;
    activity.Insert(L"timestamps", timestamps);

    const std::optional<std::wstring> hosted = HostedGame();
    if (cfg.GamePassthroughEnabled && hosted && !IsBlank(*hosted)) {
        activity.Insert(L"details", json::JsonValue::CreateStringValue(
            AppConfig::Sanitize(ReplaceAll(cfg.HostingTemplate, L"{game}", *hosted), 128, L"Hosting a game")));
    } else if (!IsBlank(cfg.Details)) {
        activity.Insert(L"details", json::JsonValue::CreateStringValue(AppConfig::Sanitize(cfg.Details, 128, L"Hosting")));
    }
    if (!IsBlank(cfg.State)) {
        activity.Insert(L"state", json::JsonValue::CreateStringValue(AppConfig::Sanitize(cfg.State, 128, L"")));
    }
    if (assets.Size() > 0) activity.Insert(L"assets", assets);

    SendCommand(&activity, DeadlineIn(kWriteTimeoutMs), stop);
    const std::string reply = ReadReply(stop);
    if (reply.find(kErrorMarker) != std::string::npos) {
        Fail(L"Discord rejected the activity (check the Application ID and asset key)");
    }
}

void DiscordPresence::Impl::SendCommand(const json::JsonObject* activity, Clock::time_point deadline, HANDLE stop) {
    json::JsonObject args;
    args.Insert(L"pid", json::JsonValue::CreateNumberValue(static_cast<double>(GetCurrentProcessId())));
    if (activity) {
        args.Insert(L"activity", *activity);
    } else {
        args.Insert(L"activity", json::JsonValue::CreateNullValue());
    }
    json::JsonObject payload;
    payload.Insert(L"cmd", json::JsonValue::CreateStringValue(L"SET_ACTIVITY"));
    payload.Insert(L"nonce", json::JsonValue::CreateStringValue(NewNonce()));
    payload.Insert(L"args", args);
    SendFrame(1, json::Stringify(payload), deadline, stop);
}

void DiscordPresence::Impl::SendFrame(std::int32_t opcode, const std::string& body, Clock::time_point deadline, HANDLE stop) {
    if (!pipe.valid()) Fail(L"not connected");
    WriteAll(pipe.get(), BuildFrame(opcode, body), deadline, stop);
}

std::string DiscordPresence::Impl::ReadReply(HANDLE stop) {
    if (!pipe.valid()) Fail(L"not connected");
    return ReadFrame(pipe.get(), DeadlineIn(kReadTimeoutMs), stop);
}

// ---------------------------------------------------------------------------

DiscordPresence::DiscordPresence(const DiscordConfig& cfg) : impl_(std::make_unique<Impl>(cfg)) {}

DiscordPresence::~DiscordPresence() {
    try {
        Clear();
    } catch (...) {
    }
}

bool DiscordPresence::IsConfigured() const { return impl_->IsConfigured(); }

std::wstring DiscordPresence::StatusText() const {
    std::lock_guard lock(impl_->state);
    return impl_->status;
}

void DiscordPresence::SetActive() {
    Impl& im = *impl_;
    std::lock_guard lock(im.lifecycle);
    if (im.worker.joinable()) return;
    if (!im.IsConfigured()) {
        im.SetStatus(im.InitialStatus());
        return;
    }
    im.activeSinceMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count();
    im.loggedUnavailable = false;
    ResetEvent(im.stopEvent.get());
    im.worker = std::jthread([&im](std::stop_token st) {
        const ApartmentGuard apartment;
        try {
            im.Run(st);
        } catch (...) {
            LogInfo(L"Discord presence worker stopped unexpectedly");
        }
    });
}

void DiscordPresence::Clear() {
    Impl& im = *impl_;
    std::lock_guard lock(im.lifecycle);
    if (!im.worker.joinable()) return;
    im.worker.request_stop();
    SetEvent(im.stopEvent.get());
    CancelSynchronousIo(im.worker.native_handle());
    im.worker.join();
    if (im.pipe.valid()) {
        // Belt-and-braces: Discord also drops the activity as soon as the pipe closes.
        try {
            im.SendCommand(nullptr, DeadlineIn(kClearTimeoutMs), nullptr);
        } catch (...) {
        }
    }
    im.ClosePipe();
    {
        std::lock_guard stateLock(im.state);
        im.hostedGame.reset();  // never let a stale game survive a guest-session-end
    }
    im.SetStatus(im.InitialStatus());
}

void DiscordPresence::SetHostedGame(std::optional<std::wstring> name) {
    std::optional<std::wstring> normalized;
    if (name && !IsBlank(*name)) normalized = std::wstring(TrimView(*name));
    {
        std::lock_guard lock(impl_->state);
        if (impl_->hostedGame == normalized) return;
        impl_->hostedGame = std::move(normalized);
    }
    SetEvent(impl_->nudge.get());
}

}  // namespace pc
