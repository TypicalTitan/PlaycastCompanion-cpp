#include "pch.h"
#include "Config.h"

#include "Json.h"
#include "Log.h"

// Port of AppConfig.cs. The JSON key names, defaults and Normalize() clamps are
// the contract: a config.json written by either app must load in the other.

namespace pc {
namespace {

using json::JsonObject;
using json::JsonValue;

constexpr wchar_t kDefaultChromaInitUrl[] = L"http://localhost:54235/razer/chromasdk";

// ---- file helpers ---------------------------------------------------------

std::wstring ExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        if (buf.size() >= 32768) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

bool FileExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ReadAllBytes(const std::wstring& path) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        const DWORD err = GetLastError();
        throw std::runtime_error(std::format("could not open config.json (error {})", err));
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) {
        const DWORD err = GetLastError();
        throw std::runtime_error(std::format("could not size config.json (error {})", err));
    }
    constexpr LONGLONG kMaxConfigBytes = 4LL * 1024 * 1024;
    if (size.QuadPart < 0 || size.QuadPart > kMaxConfigBytes) {
        throw std::runtime_error("config.json is unreasonably large");
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
        DWORD read = 0;
        if (!ReadFile(file.get(), bytes.data() + offset, chunk, &read, nullptr)) {
            const DWORD err = GetLastError();
            throw std::runtime_error(std::format("could not read config.json (error {})", err));
        }
        if (read == 0) break;
        offset += read;
    }
    bytes.resize(offset);
    return bytes;
}

// ---- character classes (char.IsControl / char.IsWhiteSpace equivalents) ----

bool IsControlChar(wchar_t c) {
    return c <= 0x1F || (c >= 0x7F && c <= 0x9F);
}

bool IsWhiteSpaceChar(wchar_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
           c == 0x205F || c == 0x3000;
}

std::wstring Trim(std::wstring_view s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && IsWhiteSpaceChar(s[b])) ++b;
    while (e > b && IsWhiteSpaceChar(s[e - 1])) --e;
    return std::wstring(s.substr(b, e - b));
}

bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size() || a.size() > static_cast<size_t>(INT_MAX)) return false;
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

// ---- tolerant, case-insensitive readers (PropertyNameCaseInsensitive = true) ----

// Resolve the key actually present in the object, preferring an exact match.
std::wstring KeyFor(const JsonObject& o, std::wstring_view key) {
    if (json::Has(o, key)) return std::wstring(key);
    try {
        for (const auto& kv : o) {
            const std::wstring_view actual(kv.Key());
            if (EqualsIgnoreCase(actual, key)) return std::wstring(actual);
        }
    } catch (const winrt::hresult_error&) {
    }
    return std::wstring(key);
}

std::wstring Str(const JsonObject& o, std::wstring_view key, std::wstring_view def) {
    return json::GetString(o, KeyFor(o, key), def);
}

// string? in the C#: an explicit JSON null deserialises to null there, which
// RegistryWatcher treats the same as empty. Absent key keeps the default.
std::wstring NullableStr(const JsonObject& o, std::wstring_view key, std::wstring_view def) {
    const std::wstring k = KeyFor(o, key);
    try {
        if (json::Has(o, k)) {
            const auto v = o.Lookup(winrt::hstring(k));
            if (v && v.ValueType() == winrt::Windows::Data::Json::JsonValueType::Null) return std::wstring();
        }
    } catch (const winrt::hresult_error&) {
    }
    return json::GetString(o, k, def);
}

bool Bool(const JsonObject& o, std::wstring_view key, bool def) {
    return json::GetBool(o, KeyFor(o, key), def);
}

int Int(const JsonObject& o, std::wstring_view key, int def) {
    const double d = json::GetNumber(o, KeyFor(o, key), static_cast<double>(def));
    if (!std::isfinite(d)) return def;
    if (d >= static_cast<double>(INT_MAX)) return INT_MAX;
    if (d <= static_cast<double>(INT_MIN)) return INT_MIN;
    return static_cast<int>(d);
}

JsonObject Obj(const JsonObject& o, std::wstring_view key) {
    return json::GetObject(o, KeyFor(o, key));
}

void Fill(AppConfig& c, const JsonObject& root) {
    c.TargetUsername = Str(root, L"TargetUsername", c.TargetUsername);
    c.TickSeconds = Int(root, L"TickSeconds", c.TickSeconds);
    c.RetryInitSeconds = Int(root, L"RetryInitSeconds", c.RetryInitSeconds);
    c.IncludeDisconnectedSessions = Bool(root, L"IncludeDisconnectedSessions", c.IncludeDisconnectedSessions);
    c.ChromaInitUrl = Str(root, L"ChromaInitUrl", c.ChromaInitUrl);
    c.RazerEnabled = Bool(root, L"RazerEnabled", c.RazerEnabled);

    {
        const JsonObject o = Obj(root, L"SessionLogging");
        c.SessionLogging.Enabled = Bool(o, L"Enabled", c.SessionLogging.Enabled);
        c.SessionLogging.SnapshotIntervalSeconds = Int(o, L"SnapshotIntervalSeconds", c.SessionLogging.SnapshotIntervalSeconds);
        c.SessionLogging.MaxFileBytes = Int(o, L"MaxFileBytes", c.SessionLogging.MaxFileBytes);
        c.SessionLogging.MaxSessionBytes = Int(o, L"MaxSessionBytes", c.SessionLogging.MaxSessionBytes);
        c.SessionLogging.RetentionDays = Int(o, L"RetentionDays", c.SessionLogging.RetentionDays);
    }
    {
        const JsonObject o = Obj(root, L"SteelSeries");
        c.SteelSeries.Enabled = Bool(o, L"Enabled", c.SteelSeries.Enabled);
    }
    {
        const JsonObject o = Obj(root, L"Logitech");
        c.Logitech.Enabled = Bool(o, L"Enabled", c.Logitech.Enabled);
    }
    {
        const JsonObject o = Obj(root, L"Corsair");
        c.Corsair.Enabled = Bool(o, L"Enabled", c.Corsair.Enabled);
    }
    {
        const JsonObject o = Obj(root, L"OpenRgb");
        c.OpenRgb.Enabled = Bool(o, L"Enabled", c.OpenRgb.Enabled);
        c.OpenRgb.Host = Str(o, L"Host", c.OpenRgb.Host);
        c.OpenRgb.Port = Int(o, L"Port", c.OpenRgb.Port);
        c.OpenRgb.BlackoutProfile = Str(o, L"BlackoutProfile", c.OpenRgb.BlackoutProfile);
    }
    {
        const JsonObject o = Obj(root, L"DynamicLighting");
        c.DynamicLighting.Enabled = Bool(o, L"Enabled", c.DynamicLighting.Enabled);
        c.DynamicLighting.ExcludeVendorOwnedDevices =
            Bool(o, L"ExcludeVendorOwnedDevices", c.DynamicLighting.ExcludeVendorOwnedDevices);
    }
    {
        const JsonObject o = Obj(root, L"RegistryWatch");
        c.RegistryWatch.Enabled = Bool(o, L"Enabled", c.RegistryWatch.Enabled);
        c.RegistryWatch.Hive = Str(o, L"Hive", c.RegistryWatch.Hive);
        c.RegistryWatch.SubKey = Str(o, L"SubKey", c.RegistryWatch.SubKey);
        // string? in the C#: explicit null means "no value name" / "any data".
        c.RegistryWatch.ValueName = NullableStr(o, L"ValueName", c.RegistryWatch.ValueName);
        c.RegistryWatch.ActiveValue = NullableStr(o, L"ActiveValue", c.RegistryWatch.ActiveValue);
    }
    {
        const JsonObject o = Obj(root, L"Discord");
        c.Discord.Enabled = Bool(o, L"Enabled", c.Discord.Enabled);
        c.Discord.ApplicationId = Str(o, L"ApplicationId", c.Discord.ApplicationId);
        c.Discord.Details = Str(o, L"Details", c.Discord.Details);
        c.Discord.State = Str(o, L"State", c.Discord.State);
        c.Discord.LargeImageKey = Str(o, L"LargeImageKey", c.Discord.LargeImageKey);
        c.Discord.LargeImageText = Str(o, L"LargeImageText", c.Discord.LargeImageText);
        c.Discord.GamePassthroughEnabled = Bool(o, L"GamePassthroughEnabled", c.Discord.GamePassthroughEnabled);
        c.Discord.HostingTemplate = Str(o, L"HostingTemplate", c.Discord.HostingTemplate);
        c.Discord.ShimFallbackGame = Str(o, L"ShimFallbackGame", c.Discord.ShimFallbackGame);
        c.Discord.ShimPipeName = Str(o, L"ShimPipeName", c.Discord.ShimPipeName);

        const JsonObject names = Obj(o, L"ResolvedNames");
        if (names && names.Size() > 0) {
            std::map<std::wstring, std::wstring> resolved;
            for (const auto& kv : names) {
                const auto v = kv.Value();
                if (v && v.ValueType() == winrt::Windows::Data::Json::JsonValueType::String) {
                    resolved[std::wstring(std::wstring_view(kv.Key()))] = std::wstring(std::wstring_view(v.GetString()));
                }
            }
            c.Discord.ResolvedNames = std::move(resolved);
        }
    }
}

// ---- Uri.IsLoopback equivalent over WinHttpCrackUrl --------------------------

bool IsDottedDigits(std::wstring_view s) {
    if (s.empty()) return false;
    for (wchar_t c : s) {
        if (!((c >= L'0' && c <= L'9') || c == L'.')) return false;
    }
    return true;
}

bool IsLoopbackHost(std::wstring_view host) {
    if (host.size() >= 2 && host.front() == L'[' && host.back() == L']') {
        host = host.substr(1, host.size() - 2);
    }
    if (EqualsIgnoreCase(host, L"localhost") || host == L"::1") return true;
    // 127.0.0.0/8 — what System.Uri.IsLoopback accepts for IPv4.
    return host.size() >= 4 && host.substr(0, 4) == L"127." && IsDottedDigits(host);
}

bool IsAbsoluteLoopbackUrl(const std::wstring& url) {
    if (url.empty() || url.size() > static_cast<size_t>(INT_MAX)) return false;
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    if (!uc.lpszHostName || uc.dwHostNameLength == 0) return false;
    return IsLoopbackHost(std::wstring_view(uc.lpszHostName, uc.dwHostNameLength));
}

// ---- ordered, indented writer (System.Text.Json WriteIndented layout) ---------


}  // namespace

// ---- AppConfig -----------------------------------------------------------------

std::wstring AppConfig::ConfigPath() {
    const std::wstring exe = ExePath();
    if (exe.empty()) return L"config.json";
    std::filesystem::path dir = std::filesystem::path(exe).parent_path();
    return (dir / L"config.json").wstring();
}

AppConfig AppConfig::Load() {
    try {
        const std::wstring path = ConfigPath();
        if (FileExists(path)) {
            const std::string bytes = ReadAllBytes(path);
            const JsonObject root = json::Parse(bytes);
            AppConfig cfg;
            Fill(cfg, root);
            cfg.Normalize();
            return cfg;
        }
    } catch (const winrt::hresult_error& ex) {
        LogInfo(L"config.json load failed, using defaults: " + std::wstring(std::wstring_view(ex.message())));
    } catch (const std::exception& ex) {
        LogInfo(std::string("config.json load failed, using defaults: ") + ex.what());
    } catch (...) {
        LogInfo(L"config.json load failed, using defaults: unknown error");
    }
    AppConfig defaults;
    defaults.Normalize();
    return defaults;
}

void AppConfig::Normalize() {
    SessionLogging.SnapshotIntervalSeconds = std::clamp(SessionLogging.SnapshotIntervalSeconds, 5, 600);
    SessionLogging.MaxFileBytes = std::clamp(SessionLogging.MaxFileBytes, 64 * 1024, 50 * 1024 * 1024);
    SessionLogging.MaxSessionBytes = std::clamp(SessionLogging.MaxSessionBytes, SessionLogging.MaxFileBytes + 4096, 1024 * 1024 * 1024);
    SessionLogging.RetentionDays = std::clamp(SessionLogging.RetentionDays, 1, 365);
    TargetUsername = Sanitize(TargetUsername, 104, L"NonsoleMode");
    TickSeconds = std::clamp(TickSeconds, 1, 10);
    RetryInitSeconds = std::clamp(RetryInitSeconds, 5, 600);
    if (!IsAbsoluteLoopbackUrl(ChromaInitUrl)) ChromaInitUrl = kDefaultChromaInitUrl;
    OpenRgb.Host = Sanitize(OpenRgb.Host, 253, L"127.0.0.1");
    OpenRgb.Port = std::clamp(OpenRgb.Port, 1, 65535);
    OpenRgb.BlackoutProfile = Sanitize(OpenRgb.BlackoutProfile, 64, L"Blackout");
    Discord.ApplicationId = Trim(Discord.ApplicationId);
    Discord.Details = Sanitize(Discord.Details, 128, L"");
    Discord.State = Sanitize(Discord.State, 128, L"");
    Discord.LargeImageKey = Sanitize(Discord.LargeImageKey, 64, L"");
    Discord.LargeImageText = Sanitize(Discord.LargeImageText, 128, L"");
    Discord.HostingTemplate = Sanitize(Discord.HostingTemplate, 128, L"Hosting {game} in Nonsole Mode");
    Discord.ShimFallbackGame = Sanitize(Discord.ShimFallbackGame, 64, L"a game");
    Discord.ShimPipeName = Sanitize(Discord.ShimPipeName, 64, L"playcast-companion-shim");
}


std::wstring AppConfig::Sanitize(std::wstring_view value, size_t maxLength, std::wstring_view fallback) {
    std::wstring cleaned;
    cleaned.reserve(value.size());
    for (wchar_t c : value) {
        if (!IsControlChar(c)) cleaned += c;
    }
    cleaned = Trim(cleaned);
    if (cleaned.empty()) return std::wstring(fallback);
    if (cleaned.size() > maxLength) cleaned.resize(maxLength);
    return cleaned;
}

}  // namespace pc
