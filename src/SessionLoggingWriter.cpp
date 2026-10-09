#include "pch.h"
#include "SessionLoggingWriter.h"
#include <regex>

namespace pc::sessionlog {
namespace {
constexpr uint64_t Reserve = 2048;
std::string Encode(const Value& payload, bool includeSecrets) {
    return json::Stringify(Make({{L"recordedAtUtc", Text(UtcNow())}, {L"includeSecrets", Boolean(includeSecrets)}, {L"payload", payload}})) + "\n";
}
bool Generated(std::wstring name) {
    return std::regex_match(name, std::wregex(LR"(\d{8}T\d{6}\.\d{3}Z_[a-f0-9]{32})"));
}
bool SafeTree(const std::filesystem::path& directory) {
    size_t visited = 0;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(directory, error), end; it != end; it.increment(error)) {
        if (error || ++visited > 10000) return false;
        const auto attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return !error;
}
}

std::filesystem::path Writer::DefaultRoot() {
    PWSTR directory = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &directory)))
        throw std::runtime_error("Local app data folder unavailable");
    struct Memory { PWSTR value; ~Memory() { CoTaskMemFree(value); } } memory{ directory };
    return std::filesystem::path(directory) / L"PlaycastCompanion" / L"sessions";
}

Writer::Writer(SessionLoggingConfig config, std::filesystem::path baseDirectory) : config_(config) {
    try {
        const auto root = std::filesystem::absolute(baseDirectory.empty() ? DefaultRoot() : baseDirectory).lexically_normal();
        std::filesystem::create_directories(root);
        if (GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("Session root may not be a filesystem link");
        ProtectDirectory(root);
        auto stamp = UtcNow();
        stamp.erase(std::remove(stamp.begin(), stamp.end(), L'-'), stamp.end());
        stamp.erase(std::remove(stamp.begin(), stamp.end(), L':'), stamp.end());
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Session identifier unavailable");
        wchar_t guidText[40]{};
        StringFromGUID2(guid, guidText, static_cast<int>(std::size(guidText)));
        std::wstring id;
        for (wchar_t c : std::wstring_view(guidText))
            if (c != L'{' && c != L'}' && c != L'-') id += static_cast<wchar_t>(towlower(c));
        directory_ = root / (stamp + L"_" + id);
        if (!std::filesystem::create_directory(directory_)) throw std::runtime_error("Session directory already exists");
        ProtectDirectory(directory_);
        Retain(root);
    } catch (const std::exception& error) {
        error_ = L"Session storage unavailable: " + json::Utf8ToWide(error.what());
        stopped_ = true;
    }
}

void Writer::Retain(const std::filesystem::path& root) {
    std::error_code error;
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * config_.RetentionDays);
    for (std::filesystem::directory_iterator it(root, error), end; it != end; it.increment(error)) {
        if (error) return;
        const auto directory = it->path().lexically_normal();
        if (directory.parent_path() != root || directory == directory_ || !Generated(directory.filename().wstring())) continue;
        const auto attributes = GetFileAttributesW(directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        const auto written = std::filesystem::last_write_time(directory, error);
        if (!error && written < cutoff && SafeTree(directory)) std::filesystem::remove_all(directory, error);
        error.clear();
    }
}

bool Writer::Write(std::wstring_view category, const Value& payload, bool includeSecrets) {
    if (stopped_) return false;
    try {
        const auto bytes = Encode(redactor_.Redact(payload, includeSecrets), includeSecrets);
        if (bytes.size() > static_cast<size_t>(config_.MaxFileBytes)) {
            const auto marker = Encode(Make({{L"event", Text(L"recordRejected")}, {L"category", Text(category)},
                {L"bytes", Number(static_cast<double>(bytes.size()))}, {L"maximum", Number(config_.MaxFileBytes)}}), false);
            if (total_ + marker.size() > static_cast<uint64_t>(config_.MaxSessionBytes) - Reserve) return Limit();
            Append(L"diagnostics", marker);
            error_ = L"A diagnostic record exceeded the per-file limit and was rejected.";
            return false;
        }
        if (total_ + bytes.size() > static_cast<uint64_t>(config_.MaxSessionBytes) - Reserve) return Limit();
        Append(category, bytes);
        return true;
    } catch (const std::exception& error) {
        stopped_ = true;
        error_ = L"Session write failed: " + json::Utf8ToWide(error.what());
    } catch (...) {
        stopped_ = true;
        error_ = L"Session write failed; recording stopped.";
    }
    return false;
}

bool Writer::Limit() {
    stopped_ = true;
    error_ = L"Session storage limit reached; further records are dropped.";
    const auto marker = Encode(Make({{L"event", Text(L"sessionStorageLimitReached")}, {L"maximumBytes", Number(config_.MaxSessionBytes)}}), false);
    if (total_ + marker.size() <= static_cast<uint64_t>(config_.MaxSessionBytes)) Append(L"diagnostics", marker);
    return false;
}

void Writer::Append(std::wstring_view category, const std::string& bytes) {
    const std::wstring name = category == L"realtime" || category == L"ipc" || category == L"scripts"
        || category == L"games" || category == L"session" ? std::wstring(category) : L"diagnostics";
    auto& file = files_[name];
    if (!file.handle.valid() || file.bytes + bytes.size() > static_cast<uint64_t>(config_.MaxFileBytes)) {
        const auto suffix = file.index == 0 ? L"" : std::format(L".{:03}", file.index);
        ++file.index;
        file.handle.reset(CreateFileW((directory_ / (name + suffix + L".jsonl")).c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.handle.valid()) throw std::runtime_error(std::format("Cannot open category log (Win32 {})", GetLastError()));
        file.bytes = 0;
    }
    DWORD written = 0;
    if (!WriteFile(file.handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
        || written != bytes.size() || !FlushFileBuffers(file.handle.get()))
        throw std::runtime_error(std::format("Cannot write category log (Win32 {})", GetLastError()));
    file.bytes += written;
    total_ += written;
}
}
