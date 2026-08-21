#include "pch.h"
#include "OpenRgbController.h"

#include "Json.h"
#include "Log.h"

#pragma comment(lib, "ws2_32.lib")

namespace pc {
namespace {

// OpenRGB SDK protocol command ids.
constexpr uint32_t kRequestProtocolVersion = 40;
constexpr uint32_t kSetClientName = 50;
constexpr uint32_t kRequestSaveProfile = 151;
constexpr uint32_t kRequestLoadProfile = 152;
constexpr uint32_t kRequestDeleteProfile = 153;

constexpr uint32_t kClientProtocolVersion = 3;
constexpr uint32_t kMaxFrameLength = 1'000'000;  // never trust a peer's length field
constexpr size_t kHeaderLength = 16;
constexpr int kTimeoutMs = 3000;
constexpr long kSelectSliceUs = 250 * 1000;  // connect wait granularity (stop checks)

constexpr std::wstring_view kRestoreProfile = L"playcast-restore";
constexpr std::string_view kClientName = "Playcast Companion";

/// Thrown out of the connect wait when the hold loop is being stopped; caught
/// in ApplyTick so the base never logs it as an outage.
struct StopRequested final : std::exception {
    const char* what() const noexcept override { return "stop requested"; }
};

void EnsureWinsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0)
            throw std::runtime_error(std::format("WSAStartup failed ({})", rc));
    });
}

/// "<what> (<code>: <system text>)" as UTF-8, for exception messages.
std::string WsaError(std::string_view what, int code) {
    std::wstring text;
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (buffer != nullptr) {
        if (length > 0)
            text.assign(buffer, length);
        LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();
    if (text.empty())
        return std::format("{} ({})", what, code);
    return std::format("{} ({}: {})", what, code, json::WideToUtf8(text));
}

struct UniqueSocket {
    SOCKET s = INVALID_SOCKET;
    UniqueSocket() = default;
    explicit UniqueSocket(SOCKET sock) : s(sock) {}
    UniqueSocket(const UniqueSocket&) = delete;
    UniqueSocket& operator=(const UniqueSocket&) = delete;
    UniqueSocket(UniqueSocket&& o) noexcept : s(o.s) { o.s = INVALID_SOCKET; }
    UniqueSocket& operator=(UniqueSocket&& o) noexcept {
        if (this != &o) {
            reset();
            s = o.s;
            o.s = INVALID_SOCKET;
        }
        return *this;
    }
    ~UniqueSocket() { reset(); }
    void reset() {
        if (s != INVALID_SOCKET)
            closesocket(s);
        s = INVALID_SOCKET;
    }
    bool valid() const { return s != INVALID_SOCKET; }
    SOCKET get() const { return s; }
};

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/// Encoding.ASCII.GetBytes(name + "\0"): non-ASCII code points become '?'.
std::vector<uint8_t> NameBytes(std::wstring_view name) {
    std::vector<uint8_t> out;
    out.reserve(name.size() + 1);
    for (size_t i = 0; i < name.size(); ++i) {
        const wchar_t c = name[i];
        if (c < 0x80) {
            out.push_back(static_cast<uint8_t>(c));
        } else {
            out.push_back(static_cast<uint8_t>('?'));
            const bool highSurrogate = c >= 0xD800 && c <= 0xDBFF;
            if (highSurrogate && i + 1 < name.size() && name[i + 1] >= 0xDC00 && name[i + 1] <= 0xDFFF)
                ++i;  // one '?' per supplementary code point, like .NET
        }
    }
    out.push_back(0);
    return out;
}

std::vector<uint8_t> AsciiZ(std::string_view text) {
    std::vector<uint8_t> out(text.begin(), text.end());
    out.push_back(0);
    return out;
}

/// Non-blocking connect already issued; wait (in short slices so a stop request
/// is honoured promptly) until it completes, fails, or the 3 s budget is spent.
bool WaitConnected(SOCKET s, const std::stop_token& stop, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);
    for (;;) {
        if (stop.stop_requested())
            throw StopRequested{};

        fd_set writable{};
        writable.fd_count = 1;
        writable.fd_array[0] = s;
        fd_set failed = writable;
        timeval slice{};
        slice.tv_sec = 0;
        slice.tv_usec = kSelectSliceUs;

        const int ready = select(0, nullptr, &writable, &failed, &slice);
        if (ready == SOCKET_ERROR) {
            error = WsaError("select failed", WSAGetLastError());
            return false;
        }
        if (ready > 0) {
            int soError = 0;
            int optLen = sizeof soError;
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &optLen) == SOCKET_ERROR) {
                error = WsaError("getsockopt(SO_ERROR) failed", WSAGetLastError());
                return false;
            }
            if (soError != 0) {
                error = WsaError("connect failed", soError);
                return false;
            }
            if (writable.fd_count > 0 && writable.fd_array[0] == s)
                return true;
            error = "connect failed";
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "connect timed out after 3 s";
            return false;
        }
    }
}

bool TryConnect(const UniqueSocket& sock, const ADDRINFOW& address, const std::stop_token& stop, std::string& error) {
    u_long nonBlocking = 1;
    if (ioctlsocket(sock.get(), FIONBIO, &nonBlocking) == SOCKET_ERROR) {
        error = WsaError("ioctlsocket(FIONBIO) failed", WSAGetLastError());
        return false;
    }
    if (connect(sock.get(), address.ai_addr, static_cast<int>(address.ai_addrlen)) == SOCKET_ERROR) {
        const int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            error = WsaError("connect failed", err);
            return false;
        }
        if (!WaitConnected(sock.get(), stop, error))
            return false;
    }
    u_long blocking = 0;
    if (ioctlsocket(sock.get(), FIONBIO, &blocking) == SOCKET_ERROR) {
        error = WsaError("ioctlsocket(FIONBIO) failed", WSAGetLastError());
        return false;
    }
    const DWORD timeout = kTimeoutMs;
    if (setsockopt(sock.get(), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout) == SOCKET_ERROR ||
        setsockopt(sock.get(), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout) == SOCKET_ERROR) {
        error = WsaError("setsockopt(timeout) failed", WSAGetLastError());
        return false;
    }
    return true;
}

UniqueSocket ConnectSocket(const std::wstring& host, int port, const std::stop_token& stop) {
    EnsureWinsock();

    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    const std::wstring portText = std::to_wstring(port);
    PADDRINFOW raw = nullptr;
    const int rc = GetAddrInfoW(host.c_str(), portText.c_str(), &hints, &raw);
    if (rc != 0)
        throw std::runtime_error(WsaError("cannot resolve OpenRGB host", rc));
    const std::unique_ptr<ADDRINFOW, decltype(&FreeAddrInfoW)> addresses(raw, &FreeAddrInfoW);

    std::string lastError = "OpenRGB host resolved to no addresses";
    for (const ADDRINFOW* ai = addresses.get(); ai != nullptr; ai = ai->ai_next) {
        if (stop.stop_requested())
            throw StopRequested{};
        UniqueSocket sock(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
        if (!sock.valid()) {
            lastError = WsaError("socket() failed", WSAGetLastError());
            continue;
        }
        if (TryConnect(sock, *ai, stop, lastError))
            return sock;
    }
    throw std::runtime_error(lastError);
}

/// Minimal OpenRGB SDK protocol client ("ORGB"-magic framed TCP).
class OpenRgbClient {
public:
    OpenRgbClient(const std::wstring& host, int port, const std::stop_token& stop)
        : sock_(ConnectSocket(host, port, stop)) {
        std::vector<uint8_t> version;
        PutU32(version, kClientProtocolVersion);
        Send(kRequestProtocolVersion, version);
        const std::vector<uint8_t> reply = ReadPayload();
        const uint32_t serverVersion = reply.size() >= 4 ? GetU32(reply.data()) : 0u;
        if (serverVersion < 2)
            throw std::runtime_error(std::format("OpenRGB protocol {} has no profile support (need v2+)", serverVersion));
        Send(kSetClientName, AsciiZ(kClientName));
    }
    OpenRgbClient(const OpenRgbClient&) = delete;
    OpenRgbClient& operator=(const OpenRgbClient&) = delete;

    void SaveProfile(std::wstring_view name) { Send(kRequestSaveProfile, NameBytes(name)); }
    void LoadProfile(std::wstring_view name) { Send(kRequestLoadProfile, NameBytes(name)); }
    void DeleteProfile(std::wstring_view name) { Send(kRequestDeleteProfile, NameBytes(name)); }

private:
    void Send(uint32_t command, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> frame;
        frame.reserve(kHeaderLength + payload.size());
        frame.push_back(static_cast<uint8_t>('O'));
        frame.push_back(static_cast<uint8_t>('R'));
        frame.push_back(static_cast<uint8_t>('G'));
        frame.push_back(static_cast<uint8_t>('B'));
        PutU32(frame, 0);  // device id (unused here)
        PutU32(frame, command);
        PutU32(frame, static_cast<uint32_t>(payload.size()));
        frame.insert(frame.end(), payload.begin(), payload.end());

        const uint8_t* cursor = frame.data();
        size_t left = frame.size();
        while (left > 0) {
            const int chunk = static_cast<int>(std::min<size_t>(left, size_t{1} << 20));
            const int sent = send(sock_.get(), reinterpret_cast<const char*>(cursor), chunk, 0);
            if (sent == SOCKET_ERROR)
                throw std::runtime_error(WsaError("OpenRGB send failed", WSAGetLastError()));
            if (sent == 0)
                throw std::runtime_error("connection closed");
            cursor += sent;
            left -= static_cast<size_t>(sent);
        }
    }

    std::vector<uint8_t> ReadPayload() {
        std::array<uint8_t, kHeaderLength> header{};
        ReadExact(header.data(), header.size());
        if (header[0] != 'O' || header[1] != 'R' || header[2] != 'G' || header[3] != 'B')
            throw std::runtime_error("malformed OpenRGB frame");
        const uint32_t length = GetU32(header.data() + 12);
        if (length > kMaxFrameLength)
            throw std::runtime_error("malformed OpenRGB frame");
        std::vector<uint8_t> payload(length);
        if (length > 0)
            ReadExact(payload.data(), payload.size());
        return payload;
    }

    void ReadExact(uint8_t* buffer, size_t length) {
        size_t offset = 0;
        while (offset < length) {
            const int chunk = static_cast<int>(std::min<size_t>(length - offset, size_t{1} << 20));
            const int got = recv(sock_.get(), reinterpret_cast<char*>(buffer + offset), chunk, 0);
            if (got == 0)
                throw std::runtime_error("connection closed");
            if (got == SOCKET_ERROR)
                throw std::runtime_error(WsaError("OpenRGB read failed", WSAGetLastError()));
            offset += static_cast<size_t>(got);
        }
    }

    UniqueSocket sock_;
};

}  // namespace

OpenRgbController::OpenRgbController(const OpenRgbConfig& cfg) : cfg_(cfg) {}

bool OpenRgbController::Enabled() const { return cfg_.Enabled; }

std::wstring OpenRgbController::UnavailableText(const std::exception&) const {
    return L"OpenRGB server not reachable — retrying";
}

void OpenRgbController::ApplyTick(std::stop_token stop) {
    if (stop.stop_requested())
        return;
    try {
        OpenRgbClient client(cfg_.Host, cfg_.Port, stop);
        if (!saved_) {
            // capture the user's current lighting once per hold — never again,
            // or a failed tick could overwrite the restore point with black
            client.SaveProfile(kRestoreProfile);
            saved_ = true;
            LogInfo(std::format(L"OpenRGB: current lighting saved to profile '{}'", kRestoreProfile));
        }
        const std::wstring profile = AppConfig::Sanitize(cfg_.BlackoutProfile, 64, L"Blackout");
        client.LoadProfile(profile);
    } catch (const StopRequested&) {
        // the hold is ending; the base loop exits on its own
    }
}

void OpenRgbController::Release() {
    if (!saved_)
        return;
    try {
        OpenRgbClient client(cfg_.Host, cfg_.Port, std::stop_token{});
        client.LoadProfile(kRestoreProfile);
        client.DeleteProfile(kRestoreProfile);
        LogInfo(L"OpenRGB: restore profile loaded; lighting back to normal");
    } catch (...) {
        saved_ = false;
        throw;
    }
    saved_ = false;
}

}  // namespace pc
