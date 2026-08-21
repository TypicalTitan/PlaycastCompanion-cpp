// Port of GameNameCache.cs: Discord Application ID -> display name, with a
// persistent cache under %LOCALAPPDATA% (never config.json).
#include "pch.h"
#include "GameNameCache.h"

#include "Http.h"
#include "Json.h"
#include "Log.h"

namespace pc {
namespace {

/// Version-info strings that are engines/launchers/OS, not the game itself.
constexpr std::wstring_view kGenericHints[] = {
    L"microsoft windows operating system", L"unreal engine", L"unity",
    L"steam", L"steamworks", L"epic games", L"launcher", L"godot",
};

constexpr std::size_t kMaxCacheFileBytes = std::size_t{4} << 20;  // a corrupt/hand-edited giant file is ignored
constexpr unsigned kLookupTimeoutMs = 3000;
constexpr std::wstring_view kCacheFileName = L"resolved-names.json";

/// FOLDERID_LocalAppData {F1B32785-6FBA-4FCF-9D55-7B8E7F157091} (local copy: no uuid.lib dependency).
constexpr GUID kLocalAppDataFolder = { 0xF1B32785, 0x6FBA, 0x4FCF, { 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91 } };

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

/// Decimal snowflake: digits only, fits in 64 bits.
bool IsSnowflake(std::wstring_view s) noexcept {
    if (s.empty() || s.size() > 20) return false;
    for (const wchar_t c : s) {
        if (c < L'0' || c > L'9') return false;
    }
    return s.size() < 20 || s <= std::wstring_view(L"18446744073709551615");
}

struct LocalFreeDeleter {
    void operator()(void* p) const noexcept { if (p) LocalFree(p); }
};

struct CoTaskMemDeleter {
    void operator()(void* p) const noexcept { if (p) CoTaskMemFree(p); }
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

/// %LOCALAPPDATA%\PlaycastCompanion\resolved-names.json; empty when no profile folder is available.
std::wstring CachePath() {
    std::wstring base;
    {
        PWSTR raw = nullptr;
        const HRESULT hr = SHGetKnownFolderPath(kLocalAppDataFolder, KF_FLAG_DEFAULT, nullptr, &raw);
        const std::unique_ptr<wchar_t, CoTaskMemDeleter> owner(raw);
        if (SUCCEEDED(hr) && raw) base = raw;
    }
    if (base.empty()) {
        wchar_t buffer[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
        if (n > 0 && n < MAX_PATH) base.assign(buffer, n);
    }
    if (base.empty()) return {};
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
    return base + L"\\PlaycastCompanion\\" + std::wstring(kCacheFileName);
}

/// Makes WinRT (pc::json) usable on the calling thread when it has no COM
/// apartment yet; a no-op when the caller already initialised one.
struct ApartmentScope {
    bool owned = false;
    ApartmentScope() noexcept {
        APTTYPE type{};
        APTTYPEQUALIFIER qualifier{};
        if (CoGetApartmentType(&type, &qualifier) == CO_E_NOTINITIALIZED) {
            owned = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        }
    }
    ~ApartmentScope() { if (owned) CoUninitialize(); }
    ApartmentScope(const ApartmentScope&) = delete;
    ApartmentScope& operator=(const ApartmentScope&) = delete;
};

/// string.ToLowerInvariant().
std::wstring ToLowerInvariant(std::wstring_view s) {
    std::wstring out(s);
    if (out.empty()) return out;
    const int len = static_cast<int>(std::min<std::size_t>(out.size(), 0x7FFFFFFF));
    std::wstring mapped(static_cast<std::size_t>(len), L'\0');
    const int written = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, out.data(), len,
                                      mapped.data(), len, nullptr, nullptr, 0);
    if (written > 0 && written <= len) {
        mapped.resize(static_cast<std::size_t>(written));
        return mapped;
    }
    for (wchar_t& c : out) {  // fallback: ASCII only
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return out;
}

/// Trimmed hint unless it is empty or names an engine/launcher/OS.
std::optional<std::wstring> CleanHint(std::wstring_view procHint) {
    const std::wstring_view hint = TrimView(procHint);
    if (hint.empty()) return std::nullopt;
    const std::wstring lower = ToLowerInvariant(hint);
    for (const std::wstring_view generic : kGenericHints) {
        if (lower.find(generic) != std::wstring::npos) return std::nullopt;
    }
    return std::wstring(hint);
}

/// Returns false when the file does not exist; throws on any other failure.
bool ReadWholeFile(const std::wstring& path, std::string& out) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
        FailWin32(L"open", error);
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) FailWin32(L"size", GetLastError());
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > kMaxCacheFileBytes) {
        Fail(L"cache file too large");
    }
    out.assign(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < out.size()) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - offset, std::size_t{1} << 20));
        if (!ReadFile(file.get(), out.data() + offset, want, &got, nullptr)) FailWin32(L"read", GetLastError());
        if (got == 0) {
            out.resize(offset);
            break;
        }
        offset += got;
    }
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);  // UTF-8 BOM (File.ReadAllText tolerates one)
    }
    return true;
}

void WriteWholeFile(const std::wstring& path, std::string_view bytes) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) FailWin32(L"create", GetLastError());
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD wrote = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, std::size_t{1} << 20));
        if (!WriteFile(file.get(), bytes.data() + offset, want, &wrote, nullptr)) FailWin32(L"write", GetLastError());
        if (wrote == 0) Fail(L"short write");
        offset += wrote;
    }
}

/// Unauthenticated GET https://discord.com/api/v9/applications/{id}/rpc -> "name".
/// Empty on any failure (non-2xx just falls through; transport/parse errors are logged).
std::wstring QueryDiscord(const std::wstring& clientId) {
    // Hardening: the id is peer-supplied (guest shim) text. Only a decimal
    // snowflake may shape the request path; anything else is a guaranteed 404.
    if (!IsSnowflake(clientId)) return {};
    const ApartmentScope apartment;
    try {
        const std::wstring url = L"https://discord.com/api/v9/applications/" + clientId + L"/rpc";
        const http::Response response =
            http::Request("GET", url, {}, L"application/json", kLookupTimeoutMs, L"PlaycastCompanion/1.0");
        if (!http::IsSuccess(response)) return {};  // 404 for non-app ids, 429 rate-limit: just fall through
        const json::JsonObject document = json::Parse(response.body);
        return std::wstring(TrimView(json::GetString(document, L"name")));
    } catch (const winrt::hresult_error& ex) {
        LogInfo(std::format(L"game name lookup failed for {}: {}", clientId, std::wstring(ex.message())));
    } catch (const std::exception& ex) {
        LogInfo(std::format(L"game name lookup failed for {}: {}", clientId, json::Utf8ToWide(ex.what())));
    }
    return {};
}

}  // namespace

GameNameCache::GameNameCache(const DiscordConfig& cfg) : cfg_(cfg) {
    // seed (admin-provisioned, read-only) sits UNDER the live cache
    for (const auto& [id, name] : cfg.ResolvedNames) cache_[id] = name;
    LoadDisk();
}

std::wstring GameNameCache::Resolve(std::wstring_view clientId, std::wstring_view procHint) {
    const std::wstring id(TrimView(clientId));
    if (!id.empty()) {
        {
            std::lock_guard lock(mutex_);
            if (const auto it = cache_.find(id); it != cache_.end()) return it->second;
        }
        std::wstring fromApi = QueryDiscord(id);
        if (!IsBlank(fromApi)) {
            fromApi = AppConfig::Sanitize(fromApi, 64, L"a game");
            {
                std::lock_guard lock(mutex_);
                cache_[id] = fromApi;
            }
            SaveDisk();
            return fromApi;
        }
    }

    if (const std::optional<std::wstring> hint = CleanHint(procHint)) {
        return AppConfig::Sanitize(*hint, 64, L"a game");
    }
    return AppConfig::Sanitize(cfg_.ShimFallbackGame, 64, L"a game");
}

void GameNameCache::LoadDisk() {
    const ApartmentScope apartment;
    try {
        const std::wstring path = CachePath();
        if (path.empty()) Fail(L"%LOCALAPPDATA% is not available");
        std::string text;
        if (!ReadWholeFile(path, text)) return;
        const json::JsonObject disk = json::Parse(text);
        std::map<std::wstring, std::wstring> entries;
        for (const auto& pair : disk) {
            const std::wstring key(pair.Key());
            const auto value = pair.Value();
            if (!value || value.ValueType() != winrt::Windows::Data::Json::JsonValueType::String) {
                Fail(std::format(L"value of '{}' is not a string", key));
            }
            entries[key] = std::wstring(value.GetString());
        }
        std::lock_guard lock(mutex_);
        for (const auto& [key, name] : entries) cache_[key] = name;  // disk cache wins over the seed
    } catch (const winrt::hresult_error& ex) {
        LogInfo(std::format(L"resolved-names cache load failed: {}", std::wstring(ex.message())));
    } catch (const std::exception& ex) {
        LogInfo(std::format(L"resolved-names cache load failed: {}", json::Utf8ToWide(ex.what())));
    }
}

void GameNameCache::SaveDisk() {
    const ApartmentScope apartment;
    try {
        const std::wstring path = CachePath();
        if (path.empty()) Fail(L"%LOCALAPPDATA% is not available");
        const std::wstring directory = path.substr(0, path.find_last_of(L'\\'));
        const int created = SHCreateDirectoryExW(nullptr, directory.c_str(), nullptr);
        if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS && created != ERROR_FILE_EXISTS) {
            FailWin32(L"create directory", static_cast<DWORD>(created));
        }
        std::map<std::wstring, std::wstring> snapshot;
        {
            std::lock_guard lock(mutex_);
            snapshot = cache_;
        }
        json::JsonObject object;
        for (const auto& [key, name] : snapshot) object.Insert(key, json::JsonValue::CreateStringValue(name));
        const std::string text = json::StringifyIndented(object);
        std::lock_guard diskLock(diskMutex_);
        WriteWholeFile(path, text);
    } catch (const winrt::hresult_error& ex) {
        LogInfo(std::format(L"resolved-names cache save failed: {}", std::wstring(ex.message())));
    } catch (const std::exception& ex) {
        LogInfo(std::format(L"resolved-names cache save failed: {}", json::Utf8ToWide(ex.what())));
    }
}

}  // namespace pc
