#include "pch.h"
#include "DiscordShim.h"

#include "GameNameCache.h"
#include "Json.h"
#include "Log.h"

// Port of DiscordShimServer.cs + DiscordChannelListener.cs. Both classes run a
// single std::jthread whose every blocking wait includes a manual-reset stop
// event, so Stop() returns within a few seconds. All pipe I/O is overlapped and
// is always cancelled + drained before its OVERLAPPED goes out of scope.

namespace pc {
namespace {

constexpr DWORD kMaxFrameBytes = 1'000'000;  // C#: length > 1_000_000 => malformed
constexpr DWORD kIdleSliceMs = 30'000;       // WaitForConnection idle slice
constexpr DWORD kHandshakeTimeoutMs = 3'000; // first-frame read budget
constexpr DWORD kRelayConnectMs = 2'000;     // NamedPipeClientStream.ConnectAsync(2000)
constexpr DWORD kRelayIoMs = 3'000;          // linked CancelAfter(3000)
constexpr DWORD kLineTimeoutMs = 3'000;      // ReadLineAsync CancelAfter(3000)
constexpr size_t kMaxLineChars = 8192;       // runaway-sender guard

/// Thrown to unwind a worker when Stop() was requested (OperationCanceledException).
struct StopRequested {};

std::wstring Win32Message(DWORD err) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring text;
    if (n && buf) text.assign(buf, n);
    if (buf) LocalFree(buf);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();
    if (text.empty()) return std::format(L"Win32 error {}", err);
    return text;
}

[[noreturn]] void ThrowWin32(std::wstring_view what, DWORD err) {
    throw std::runtime_error(pc::json::WideToUtf8(std::format(L"{}: {}", what, Win32Message(err))));
}

std::wstring Describe(const std::exception& e) { return pc::json::Utf8ToWide(e.what()); }
std::wstring Describe(const winrt::hresult_error& e) { return std::wstring(e.message()); }

/// Manual-reset event; Set() by Stop(), waited on alongside every overlapped op.
struct StopEvent {
    UniqueHandle ev;
    StopEvent() : ev(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    HANDLE get() const { return ev.get(); }
    void Set() const { SetEvent(ev.get()); }
    void Reset() const { ResetEvent(ev.get()); }
    bool IsSet() const { return WaitForSingleObject(ev.get(), 0) == WAIT_OBJECT_0; }
    /// Stop-aware sleep: true when the stop event fired before the delay elapsed.
    bool Wait(DWORD ms) const { return WaitForSingleObject(ev.get(), ms) == WAIT_OBJECT_0; }
};

/// One OVERLAPPED + its event, reusable across operations on the same handle.
class Overlapped {
public:
    Overlapped() : ev_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (!ev_.valid()) ThrowWin32(L"CreateEvent", GetLastError());
    }
    OVERLAPPED* Begin() {
        ov_ = OVERLAPPED{};
        ov_.hEvent = ev_.get();
        ResetEvent(ev_.get());
        return &ov_;
    }
    OVERLAPPED* Get() { return &ov_; }
    HANDLE Event() const { return ev_.get(); }

private:
    UniqueHandle ev_;
    OVERLAPPED ov_{};
};

enum class IoKind { Ok, Timeout, Stopped, Failed };
struct IoResult {
    IoKind kind = IoKind::Failed;
    DWORD bytes = 0;
    DWORD error = 0;
};

/// Complete an overlapped op that was just issued (`issued` = the API's return
/// value). Waits for {io event, stop event} up to timeoutMs; on timeout/stop the
/// op is cancelled and drained so the OVERLAPPED may be released safely.
IoResult FinishOverlapped(HANDLE pipe, Overlapped& ov, BOOL issued, const StopEvent& stop, DWORD timeoutMs) {
    IoResult r;
    if (!issued) {
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) { r.kind = IoKind::Ok; return r; }  // ConnectNamedPipe raced a client
        if (err != ERROR_IO_PENDING) { r.kind = IoKind::Failed; r.error = err; return r; }
    }
    const HANDLE handles[2] = { ov.Event(), stop.get() };
    const DWORD w = WaitForMultipleObjects(2, handles, FALSE, timeoutMs);
    if (w == WAIT_OBJECT_0) {
        if (GetOverlappedResult(pipe, ov.Get(), &r.bytes, FALSE)) { r.kind = IoKind::Ok; return r; }
        r.kind = IoKind::Failed;
        r.error = GetLastError();
        return r;
    }
    CancelIoEx(pipe, ov.Get());
    DWORD dummy = 0;
    GetOverlappedResult(pipe, ov.Get(), &dummy, TRUE);  // drain; result irrelevant
    r.kind = (w == WAIT_OBJECT_0 + 1) ? IoKind::Stopped : IoKind::Timeout;
    return r;
}

/// Deadline helper: milliseconds left, or INFINITE when no deadline.
struct Deadline {
    bool infinite = true;
    std::chrono::steady_clock::time_point at{};
    explicit Deadline(DWORD totalMs) {
        if (totalMs != INFINITE) { infinite = false; at = std::chrono::steady_clock::now() + std::chrono::milliseconds(totalMs); }
    }
    DWORD Remaining() const {
        if (infinite) return INFINITE;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(at - std::chrono::steady_clock::now()).count();
        return left <= 0 ? 0 : static_cast<DWORD>(left);
    }
};

/// Read exactly `len` bytes (Stream.ReadAsync loop). Throws "pipe closed" on
/// EOF/broken pipe, "read timed out" past the deadline, StopRequested on stop.
void ReadExact(HANDLE pipe, Overlapped& ov, const StopEvent& stop, std::byte* buf, DWORD len, const Deadline& deadline) {
    DWORD offset = 0;
    while (offset < len) {
        const BOOL issued = ReadFile(pipe, buf + offset, len - offset, nullptr, ov.Begin());
        const IoResult r = FinishOverlapped(pipe, ov, issued, stop, deadline.Remaining());
        switch (r.kind) {
        case IoKind::Ok:
            if (r.bytes == 0) throw std::runtime_error("pipe closed");
            offset += r.bytes;
            break;
        case IoKind::Stopped: throw StopRequested{};
        case IoKind::Timeout: throw std::runtime_error("read timed out");
        case IoKind::Failed:
            if (r.error == ERROR_BROKEN_PIPE || r.error == ERROR_NO_DATA || r.error == ERROR_PIPE_NOT_CONNECTED)
                throw std::runtime_error("pipe closed");
            ThrowWin32(L"read", r.error);
        }
    }
}

/// Write the whole buffer (WriteAsync + FlushAsync). Same throw rules as ReadExact.
void WriteAll(HANDLE pipe, Overlapped& ov, const StopEvent& stop, const std::byte* data, DWORD len, DWORD timeoutMs) {
    const Deadline deadline(timeoutMs);
    DWORD offset = 0;
    while (offset < len) {
        const BOOL issued = WriteFile(pipe, data + offset, len - offset, nullptr, ov.Begin());
        const IoResult r = FinishOverlapped(pipe, ov, issued, stop, deadline.Remaining());
        switch (r.kind) {
        case IoKind::Ok:
            if (r.bytes == 0) throw std::runtime_error("pipe closed");
            offset += r.bytes;
            break;
        case IoKind::Stopped: throw StopRequested{};
        case IoKind::Timeout: throw std::runtime_error("write timed out");
        case IoKind::Failed: ThrowWin32(L"write", r.error);
        }
    }
}

/// Tracks the pipe handle a worker is currently blocked on so Stop() can
/// CancelIoEx it from another thread (belt and braces next to the stop event).
struct CurrentPipe {
    std::mutex mx;
    HANDLE h = nullptr;
    void Set(HANDLE pipe) { std::scoped_lock lock(mx); h = pipe; }
    void Cancel() { std::scoped_lock lock(mx); if (h) CancelIoEx(h, nullptr); }
};

std::wstring PipePath(std::wstring_view name) { return std::format(L"\\\\.\\pipe\\{}", name); }

}  // namespace

// ===========================================================================
// DiscordShimServer (guest session)
// ===========================================================================
namespace {

/// Exe hint for a pid: ProductName, else FileDescription, else file stem; ""
/// when the process can't be opened (mirrors ProductNameOf).
std::wstring ProductNameOf(DWORD pid) {
    if (pid == 0) return L"";
    UniqueHandle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!proc.valid()) return L"";
    std::wstring path(MAX_PATH * 2, L'\0');
    DWORD len = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(proc.get(), 0, path.data(), &len) || len == 0) return L"";
    path.resize(len);

    const std::wstring stem = std::filesystem::path(path).stem().wstring();
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return stem;
    std::vector<std::byte> block(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, block.data())) return stem;

    // Candidate translations: the file's own table first, then the defaults
    // FileVersionInfo falls back to.
    std::vector<std::wstring> langs;
    struct LangCp { WORD lang; WORD cp; };
    LangCp* table = nullptr;
    UINT tableBytes = 0;
    if (VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&table), &tableBytes) && table) {
        for (UINT i = 0; i < tableBytes / sizeof(LangCp); ++i)
            langs.push_back(std::format(L"{:04X}{:04X}", table[i].lang, table[i].cp));
    }
    for (const wchar_t* def : { L"040904B0", L"040904E4", L"04090000" }) langs.emplace_back(def);

    auto query = [&](const wchar_t* key) -> std::wstring {
        for (const auto& lang : langs) {
            const std::wstring sub = std::format(L"\\StringFileInfo\\{}\\{}", lang, key);
            wchar_t* value = nullptr;
            UINT chars = 0;
            if (VerQueryValueW(block.data(), sub.c_str(), reinterpret_cast<LPVOID*>(&value), &chars) && value && chars) {
                std::wstring s(value, wcsnlen_s(value, chars));
                const auto first = s.find_first_not_of(L" \t\r\n");
                if (first == std::wstring::npos) continue;
                const auto last = s.find_last_not_of(L" \t\r\n");
                return s.substr(first, last - first + 1);
            }
        }
        return L"";
    };
    std::wstring name = query(L"ProductName");
    if (!name.empty()) return name;
    name = query(L"FileDescription");
    if (!name.empty()) return name;
    return stem;
}

/// Session of a pid, or -1 when unknown (Process.SessionId throwing).
int SessionOf(DWORD pid) {
    if (pid == 0) return -1;
    DWORD session = 0;
    return ProcessIdToSessionId(pid, &session) ? static_cast<int>(session) : -1;
}

/// {"client_id": "..."} (string) or a bare number; nullopt when absent/unparseable.
std::optional<std::wstring> TryExtractClientId(const std::string& body) {
    try {
        const auto doc = pc::json::Parse(body);
        if (!doc.HasKey(L"client_id")) return std::nullopt;
        const auto id = doc.GetNamedValue(L"client_id");
        std::wstring text;
        if (id.ValueType() == winrt::Windows::Data::Json::JsonValueType::String) text = id.GetString();
        else text = id.Stringify();
        while (!text.empty() && text.front() == L'"') text.erase(0, 1);
        while (!text.empty() && text.back() == L'"') text.pop_back();
        // Peer bytes: strip control characters, cap length (hardening).
        return AppConfig::Sanitize(text, 64, L"");
    } catch (...) {
        return std::nullopt;
    }
}

// Exact READY payload from DiscordShimServer.SendReadyAsync.
constexpr std::string_view kReadyJson =
    R"({"cmd":"DISPATCH","evt":"READY","data":{"v":1,"config":{"cdn_host":"cdn.discordapp.com",)"
    R"("api_endpoint":"//discord.com/api","environment":"production"},"user":{"id":"0","username":"playcast",)"
    R"("discriminator":"0000","global_name":"playcast"}}})";

}  // namespace

struct DiscordShimServer::Impl {
    const AppConfig& cfg;
    const DWORD ownSessionId;
    StopEvent stop;
    CurrentPipe current;
    std::jthread worker;
    int boundSlot = -1;
    std::optional<std::wstring> currentClientId;
    long long heartbeat = 0;

    explicit Impl(const AppConfig& c) : cfg(c), ownSessionId(OwnSession()) {}

    static DWORD OwnSession() {
        DWORD s = 0;
        return ProcessIdToSessionId(GetCurrentProcessId(), &s) ? s : 0;
    }

    void Run() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            LogOccupancy();
            Loop();
        } catch (const StopRequested&) {
        } catch (const winrt::hresult_error& e) {
            LogInfo(std::format(L"Shim: worker terminated ({})", Describe(e)));
        } catch (const std::exception& e) {
            LogInfo(std::format(L"Shim: worker terminated ({})", Describe(e)));
        } catch (...) {
            LogInfo(L"Shim: worker terminated (unknown error)");
        }
        current.Set(nullptr);
        winrt::uninit_apartment();
    }

    void Loop() {
        while (!stop.IsSet()) {
            UniqueHandle server;
            try {
                int slot = -1;
                server = CreateServer(slot);
                if (!server.valid()) {
                    // every slot 1..9 taken — unusual; wait and retry
                    LogInfo(L"Shim: no free discord-ipc slot (1..9) to bind; retrying in 30s");
                    if (stop.Wait(kIdleSliceMs)) return;
                    continue;
                }
                if (slot != boundSlot) {
                    boundSlot = slot;
                    LogInfo(std::format(L"Shim: listening on discord-ipc-{} (guest session {})", slot, ownSessionId));
                }
                current.Set(server.get());
                Overlapped ov;
                if (!WaitForConnection(server.get(), ov)) { current.Set(nullptr); return; }
                HandleClient(server.get(), ov);
                DisconnectNamedPipe(server.get());
                current.Set(nullptr);
            } catch (const StopRequested&) {
                current.Set(nullptr);
                return;
            } catch (const winrt::hresult_error& e) {
                current.Set(nullptr);
                LogInfo(std::format(L"Shim: accept loop error ({}); retrying", Describe(e)));
                if (stop.Wait(2'000)) return;
            } catch (const std::exception& e) {
                current.Set(nullptr);
                LogInfo(std::format(L"Shim: accept loop error ({}); retrying", Describe(e)));
                if (stop.Wait(2'000)) return;
            }
        }
    }

    void LogOccupancy() const {
        std::wstring occupied;
        for (int i = 0; i <= 9; ++i) {
            const std::wstring name = PipePath(std::format(L"discord-ipc-{}", i));
            bool exists = WaitNamedPipeW(name.c_str(), 0) != FALSE;
            if (!exists) {
                const DWORD err = GetLastError();
                exists = err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND && err != ERROR_BAD_PATHNAME;
            }
            if (exists) {
                if (!occupied.empty()) occupied += L",";
                occupied += std::to_wstring(i);
            }
        }
        LogInfo(std::format(L"Shim: startup — discord-ipc slots occupied: [{}]", occupied));
    }

    /// Bind the lowest free discord-ipc slot in 1..9 (never 0).
    static UniqueHandle CreateServer(int& slot) {
        for (int i = 1; i <= 9; ++i) {
            const std::wstring name = PipePath(std::format(L"discord-ipc-{}", i));
            HANDLE h = CreateNamedPipeW(name.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1, 64 * 1024, 64 * 1024, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) { slot = i; return UniqueHandle(h); }
            // ERROR_PIPE_BUSY / ERROR_ACCESS_DENIED / ERROR_ALREADY_EXISTS (and any
            // other failure, as the C# IOException catch does): try the next slot.
        }
        slot = -1;
        return {};
    }

    /// true = a client connected; false = stop requested.
    bool WaitForConnection(HANDLE server, Overlapped& ov) {
        const BOOL issued = ConnectNamedPipe(server, ov.Begin());
        if (issued) return true;
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) return true;
        if (err != ERROR_IO_PENDING) ThrowWin32(L"ConnectNamedPipe", err);

        const HANDLE handles[2] = { ov.Event(), stop.get() };
        for (;;) {
            const DWORD w = WaitForMultipleObjects(2, handles, FALSE, kIdleSliceMs);
            if (w == WAIT_OBJECT_0) {
                DWORD bytes = 0;
                if (!GetOverlappedResult(server, ov.Get(), &bytes, FALSE)) {
                    const DWORD e = GetLastError();
                    if (e != ERROR_PIPE_CONNECTED) ThrowWin32(L"ConnectNamedPipe", e);
                }
                return true;
            }
            if (w == WAIT_TIMEOUT) {
                if (++heartbeat % 6 == 0)  // ~ every 3 min of idle
                    LogInfo(std::format(L"Shim: no game has connected yet (discord-ipc-{})", boundSlot));
                continue;
            }
            // stop event (or wait failure): cancel and drain the pending connect
            CancelIoEx(server, ov.Get());
            DWORD dummy = 0;
            GetOverlappedResult(server, ov.Get(), &dummy, TRUE);
            return false;
        }
    }

    struct Frame { int opcode = 0; std::string body; };

    Frame ReadFrame(HANDLE pipe, Overlapped& ov, DWORD timeoutMs) const {
        const Deadline deadline(timeoutMs);
        std::array<std::byte, 8> header{};
        ReadExact(pipe, ov, stop, header.data(), 8, deadline);
        int32_t opcode = 0, length = 0;
        std::memcpy(&opcode, header.data(), 4);
        std::memcpy(&length, header.data() + 4, 4);
        if (length < 0 || static_cast<DWORD>(length) > kMaxFrameBytes) throw std::runtime_error("malformed shim frame");
        Frame f;
        f.opcode = opcode;
        f.body.resize(static_cast<size_t>(length));
        if (length > 0) ReadExact(pipe, ov, stop, reinterpret_cast<std::byte*>(f.body.data()), static_cast<DWORD>(length), deadline);
        return f;
    }

    void SendReady(HANDLE pipe, Overlapped& ov) const {
        std::vector<std::byte> frame(8 + kReadyJson.size());
        const int32_t opcode = 1;  // FRAME
        const int32_t length = static_cast<int32_t>(kReadyJson.size());
        std::memcpy(frame.data(), &opcode, 4);
        std::memcpy(frame.data() + 4, &length, 4);
        std::memcpy(frame.data() + 8, kReadyJson.data(), kReadyJson.size());
        WriteAll(pipe, ov, stop, frame.data(), static_cast<DWORD>(frame.size()), kRelayIoMs);
    }

    void HandleClient(HANDLE server, Overlapped& ov) {
        // Session-gate: only accept a game running in THIS guest session.
        ULONG pidRaw = 0;
        const DWORD clientPid = GetNamedPipeClientProcessId(server, &pidRaw) ? pidRaw : 0;
        const int clientSession = SessionOf(clientPid);
        const std::wstring exeName = AppConfig::Sanitize(ProductNameOf(clientPid), 128, L"");
        if (clientPid > 0 && clientSession >= 0 && clientSession != static_cast<int>(ownSessionId)) {
            LogInfo(std::format(L"Shim: dropping foreign-session client pid {} (session {} != {})", clientPid, clientSession, ownSessionId));
            return;
        }

        // First inbound frame is the handshake (opcode 0, {v, client_id}).
        const Frame hello = ReadFrame(server, ov, kHandshakeTimeoutMs);
        std::optional<std::wstring> clientId;
        if (hello.opcode == 0) clientId = TryExtractClientId(hello.body);
        LogInfo(std::format(L"Shim: game connected pid {} exe '{}' client_id '{}'", clientPid, exeName, clientId ? *clientId : L"?"));

        // Reply with a minimal READY so the game stays quiet; capture already done.
        SendReady(server, ov);

        const bool hasId = clientId && !clientId->empty();
        if (hasId || !exeName.empty()) {
            currentClientId = clientId;
            pc::json::JsonObject msg;
            msg.SetNamedValue(L"event", pc::json::JsonValue::CreateStringValue(L"game"));
            msg.SetNamedValue(L"clientId", clientId ? pc::json::JsonValue::CreateStringValue(*clientId)
                                                    : pc::json::JsonValue::CreateNullValue());
            msg.SetNamedValue(L"procHint", pc::json::JsonValue::CreateStringValue(exeName));
            Relay(pc::json::Stringify(msg));
        }

        // Drain and DISCARD subsequent frames (SET_ACTIVITY, etc.) until close.
        auto finish = [&] {
            if (currentClientId || !exeName.empty()) {
                currentClientId.reset();
                LogInfo(L"Shim: game disconnected -> clear");
                Relay(R"({"event":"clear"})");
            }
        };
        try {
            while (!stop.IsSet()) ReadFrame(server, ov, INFINITE);
        } catch (const StopRequested&) {
            finish();
            throw;
        } catch (...) {
            // game closed the pipe
        }
        finish();
    }

    /// Connect to the owner's relay pipe and write one JSON line.
    void Relay(std::string line) const {
        line += '\n';
        try {
            const std::wstring path = PipePath(cfg.Discord.ShimPipeName);
            UniqueHandle client;
            const ULONGLONG start = GetTickCount64();
            for (;;) {
                client.reset(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                         SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
                if (client.valid()) break;
                const DWORD err = GetLastError();
                client.reset();
                const ULONGLONG elapsed = GetTickCount64() - start;
                if (elapsed >= kRelayConnectMs) throw std::runtime_error("connect timed out");
                const DWORD remaining = static_cast<DWORD>(kRelayConnectMs - elapsed);
                if (err == ERROR_PIPE_BUSY) {
                    // Owner instance is serving another message; wait for it to listen again.
                    // WaitNamedPipe fails with ERROR_FILE_NOT_FOUND the moment the owner closes
                    // the instance (it re-creates it after every message) -> keep polling, as
                    // .NET's ConnectAsync does; ERROR_SEM_TIMEOUT is handled by the deadline.
                    if (!WaitNamedPipeW(path.c_str(), remaining)) {
                        const DWORD e2 = GetLastError();
                        if (e2 != ERROR_FILE_NOT_FOUND && e2 != ERROR_SEM_TIMEOUT) ThrowWin32(L"connect", e2);
                    }
                    continue;
                }
                if (err == ERROR_FILE_NOT_FOUND) { Sleep(10); continue; }  // close/re-create gap or owner not up yet
                ThrowWin32(L"connect", err);
            }
            DWORD written = 0;
            if (!WriteFile(client.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr) || written != line.size())
                ThrowWin32(L"write", GetLastError());
        } catch (const std::exception& e) {
            // owner instance may not be up yet; harmless, next event retries
            LogInfo(std::format(L"Shim: relay to owner failed ({})", Describe(e)));
        }
    }
};

DiscordShimServer::DiscordShimServer(const AppConfig& cfg) : impl_(std::make_unique<Impl>(cfg)) {}
DiscordShimServer::~DiscordShimServer() { Stop(); }

void DiscordShimServer::Start() {
    if (impl_->worker.joinable()) return;
    impl_->stop.Reset();
    impl_->worker = std::jthread([this](std::stop_token) { impl_->Run(); });
}

void DiscordShimServer::Stop() {
    if (!impl_->worker.joinable()) return;
    impl_->stop.Set();
    impl_->worker.request_stop();
    impl_->current.Cancel();
    impl_->worker.join();
}

// ===========================================================================
// DiscordChannelListener (owner session)
// ===========================================================================
namespace {

struct LocalFreeDeleter {
    void operator()(void* p) const { if (p) LocalFree(p); }
};
using LocalPtr = std::unique_ptr<void, LocalFreeDeleter>;

/// SID of the current process token's user (WindowsIdentity.GetCurrent().User).
std::vector<std::byte> CurrentUserSid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) ThrowWin32(L"OpenProcessToken", GetLastError());
    UniqueHandle token(raw);
    DWORD need = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &need);
    std::vector<std::byte> buf(need ? need : sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE);
    if (!GetTokenInformation(token.get(), TokenUser, buf.data(), static_cast<DWORD>(buf.size()), &need))
        ThrowWin32(L"GetTokenInformation", GetLastError());
    const PSID sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
    std::vector<std::byte> out(GetLengthSid(sid));
    if (!CopySid(static_cast<DWORD>(out.size()), out.data(), sid)) ThrowWin32(L"CopySid", GetLastError());
    return out;
}

/// SID for an account name (NTAccount.Translate); empty when not resolvable.
std::vector<std::byte> LookupSid(const std::wstring& account) {
    DWORD sidBytes = 0, domainChars = 0;
    SID_NAME_USE use{};
    LookupAccountNameW(nullptr, account.c_str(), nullptr, &sidBytes, nullptr, &domainChars, &use);
    if (sidBytes == 0) return {};
    std::vector<std::byte> sid(sidBytes);
    std::wstring domain(domainChars ? domainChars : 1, L'\0');
    if (!LookupAccountNameW(nullptr, account.c_str(), sid.data(), &sidBytes, domain.data(), &domainChars, &use)) return {};
    return sid;
}

std::vector<std::byte> AuthenticatedUsersSid() {
    std::vector<std::byte> sid(SECURITY_MAX_SID_SIZE);
    DWORD len = static_cast<DWORD>(sid.size());
    if (!CreateWellKnownSid(WinAuthenticatedUserSid, nullptr, sid.data(), &len)) ThrowWin32(L"CreateWellKnownSid", GetLastError());
    sid.resize(len);
    return sid;
}

/// Holds the DACL + SD for the relay pipe (SIDs must outlive the ACL build).
struct PipeSecurity {
    std::vector<std::byte> selfSid;
    std::vector<std::byte> guestSid;
    LocalPtr acl;
    SECURITY_DESCRIPTOR sd{};
    SECURITY_ATTRIBUTES sa{};

    explicit PipeSecurity(const std::wstring& targetUser) {
        selfSid = CurrentUserSid();
        guestSid = LookupSid(targetUser);
        if (guestSid.empty()) {
            // guest account not resolvable (e.g. never created yet) — admit any
            // authenticated local user; the per-message CallerIsGuest check is
            // still the real trust gate.
            guestSid = AuthenticatedUsersSid();
        }
        EXPLICIT_ACCESS_W ea[2]{};
        ea[0].grfAccessPermissions = GENERIC_ALL;  // PipeAccessRights.FullControl
        ea[0].grfAccessMode = SET_ACCESS;
        ea[0].grfInheritance = NO_INHERITANCE;
        ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
        ea[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(selfSid.data());
        ea[1].grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;  // PipeAccessRights.ReadWrite
        ea[1].grfAccessMode = SET_ACCESS;
        ea[1].grfInheritance = NO_INHERITANCE;
        ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[1].Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
        ea[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(guestSid.data());
        PACL rawAcl = nullptr;
        const DWORD rc = SetEntriesInAclW(2, ea, nullptr, &rawAcl);
        if (rc != ERROR_SUCCESS) ThrowWin32(L"SetEntriesInAcl", rc);
        acl.reset(rawAcl);
        if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) ThrowWin32(L"InitializeSecurityDescriptor", GetLastError());
        if (!SetSecurityDescriptorDacl(&sd, TRUE, rawAcl, FALSE)) ThrowWin32(L"SetSecurityDescriptorDacl", GetLastError());
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;
    }
    PipeSecurity(const PipeSecurity&) = delete;
    PipeSecurity& operator=(const PipeSecurity&) = delete;
};

}  // namespace

struct DiscordChannelListener::Impl {
    const AppConfig& cfg;
    DiscordPresence& discord;
    GameNameCache names;
    StopEvent stop;
    CurrentPipe current;
    std::jthread worker;

    Impl(const AppConfig& c, DiscordPresence& d) : cfg(c), discord(d), names(c.Discord) {}

    void Run() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            LogInfo(std::format(L"Relay: listening for the guest shim on pipe '{}'", cfg.Discord.ShimPipeName));
            Loop();
        } catch (const StopRequested&) {
        } catch (const winrt::hresult_error& e) {
            LogInfo(std::format(L"Relay: worker terminated ({})", Describe(e)));
        } catch (const std::exception& e) {
            LogInfo(std::format(L"Relay: worker terminated ({})", Describe(e)));
        } catch (...) {
            LogInfo(L"Relay: worker terminated (unknown error)");
        }
        current.Set(nullptr);
        winrt::uninit_apartment();
    }

    void Loop() {
        while (!stop.IsSet()) {
            UniqueHandle server;
            try {
                server = CreateServer();
                current.Set(server.get());
                Overlapped ov;
                if (!WaitForConnection(server.get(), ov)) { current.Set(nullptr); return; }

                if (!CallerIsGuest(server.get())) {
                    LogInfo(L"Relay: rejected a connection from a non-guest caller");
                } else {
                    const std::string line = ReadLine(server.get(), ov);
                    if (line.find_first_not_of(" \t\r\n") != std::string::npos) HandleMessage(line);
                }
                DisconnectNamedPipe(server.get());
                current.Set(nullptr);
            } catch (const StopRequested&) {
                current.Set(nullptr);
                return;
            } catch (const winrt::hresult_error& e) {
                current.Set(nullptr);
                LogInfo(std::format(L"Relay: listener error ({}); retrying", Describe(e)));
                if (stop.Wait(2'000)) return;
            } catch (const std::exception& e) {
                current.Set(nullptr);
                LogInfo(std::format(L"Relay: listener error ({}); retrying", Describe(e)));
                if (stop.Wait(2'000)) return;
            }
        }
    }

    UniqueHandle CreateServer() const {
        PipeSecurity security(cfg.TargetUsername);
        const std::wstring path = PipePath(cfg.Discord.ShimPipeName);
        HANDLE h = CreateNamedPipeW(path.c_str(),
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 0, 0, 0, &security.sa);
        if (h == INVALID_HANDLE_VALUE) ThrowWin32(L"CreateNamedPipe", GetLastError());
        return UniqueHandle(h);
    }

    /// true = client connected; false = stop requested.
    bool WaitForConnection(HANDLE server, Overlapped& ov) const {
        const BOOL issued = ConnectNamedPipe(server, ov.Begin());
        const IoResult r = FinishOverlapped(server, ov, issued, stop, INFINITE);
        if (r.kind == IoKind::Failed) ThrowWin32(L"ConnectNamedPipe", r.error);
        return r.kind == IoKind::Ok;
    }

    /// Caller account name (domain stripped) equals TargetUsername, case-insensitive.
    bool CallerIsGuest(HANDLE server) const {
        try {
            const std::wstring caller = CallerName(server);  // e.g. MACHINE\NonsoleMode
            const auto slash = caller.find(L'\\');
            const std::wstring shortName = slash == std::wstring::npos ? caller : caller.substr(slash + 1);
            return CompareStringOrdinal(shortName.c_str(), static_cast<int>(shortName.size()),
                                        cfg.TargetUsername.c_str(), static_cast<int>(cfg.TargetUsername.size()), TRUE) == CSTR_EQUAL;
        } catch (const std::exception& e) {
            LogInfo(std::format(L"Relay: could not read caller identity ({})", Describe(e)));
            return false;
        }
    }

    static std::wstring CallerName(HANDLE server) {
        if (ImpersonateNamedPipeClient(server)) {
            std::wstring name(256 + 1, L'\0');
            DWORD len = static_cast<DWORD>(name.size());
            const BOOL ok = GetUserNameW(name.data(), &len);
            const DWORD err = ok ? ERROR_SUCCESS : GetLastError();
            RevertToSelf();  // always
            if (!ok) ThrowWin32(L"GetUserName", err);
            name.resize(len ? len - 1 : 0);
            return name;
        }
        const DWORD impErr = GetLastError();
        // ERROR_CANNOT_IMPERSONATE before any data was read: fall back to the
        // pipe's own record of the client user (what GetImpersonationUserName reads).
        std::wstring name(256 + 1, L'\0');
        if (!GetNamedPipeHandleStateW(server, nullptr, nullptr, nullptr, nullptr, name.data(), static_cast<DWORD>(name.size())))
            ThrowWin32(L"ImpersonateNamedPipeClient", impErr);
        name.resize(wcsnlen_s(name.c_str(), name.size()));
        return name;
    }

    /// One newline-terminated line (<= 8 KB) within 3 s; newline excluded.
    std::string ReadLine(HANDLE pipe, Overlapped& ov) const {
        const Deadline deadline(kLineTimeoutMs);
        std::array<std::byte, 4096> buffer{};
        std::string line;
        for (;;) {
            const BOOL issued = ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, ov.Begin());
            const IoResult r = FinishOverlapped(pipe, ov, issued, stop, deadline.Remaining());
            if (r.kind == IoKind::Stopped) throw StopRequested{};
            if (r.kind == IoKind::Timeout) throw std::runtime_error("read timed out");
            if (r.kind == IoKind::Failed) {
                if (r.error == ERROR_BROKEN_PIPE || r.error == ERROR_NO_DATA || r.error == ERROR_PIPE_NOT_CONNECTED) break;
                ThrowWin32(L"read", r.error);
            }
            if (r.bytes == 0) break;
            const std::string_view chunk(reinterpret_cast<const char*>(buffer.data()), r.bytes);
            const auto nl = chunk.find('\n');
            if (nl != std::string_view::npos) { line.append(chunk.substr(0, nl)); break; }
            line.append(chunk);
            if (line.size() > kMaxLineChars) break;  // guard against a runaway sender
        }
        return line;
    }

    void HandleMessage(const std::string& line) {
        try {
            const auto root = pc::json::Parse(line);
            const std::wstring evt = pc::json::GetString(root, L"event");
            if (evt == L"clear") {
                discord.SetHostedGame(std::nullopt);
                LogInfo(L"Relay: guest game cleared");
                return;
            }
            if (evt == L"game") {
                // Peer bytes: strip control characters, cap length (hardening).
                const std::wstring clientId = AppConfig::Sanitize(pc::json::GetString(root, L"clientId"), 64, L"");
                const std::wstring procHint = AppConfig::Sanitize(pc::json::GetString(root, L"procHint"), 128, L"");
                const std::wstring name = names.Resolve(clientId, procHint);
                discord.SetHostedGame(name);
                LogInfo(std::format(L"Relay: guest game '{}' (client_id {})", name, clientId.empty() ? std::wstring(L"?") : clientId));
            }
        } catch (const winrt::hresult_error& e) {
            LogInfo(std::format(L"Relay: bad message ({})", Describe(e)));
        } catch (const std::exception& e) {
            LogInfo(std::format(L"Relay: bad message ({})", Describe(e)));
        }
    }
};

DiscordChannelListener::DiscordChannelListener(const AppConfig& cfg, DiscordPresence& presence)
    : impl_(std::make_unique<Impl>(cfg, presence)) {}
DiscordChannelListener::~DiscordChannelListener() { Stop(); }

void DiscordChannelListener::Start() {
    if (impl_->worker.joinable()) return;
    impl_->stop.Reset();
    impl_->worker = std::jthread([this](std::stop_token) { impl_->Run(); });
}

void DiscordChannelListener::Stop() {
    if (!impl_->worker.joinable()) return;
    impl_->stop.Set();
    impl_->worker.request_stop();
    impl_->current.Cancel();
    impl_->worker.join();
}
}  // namespace pc
