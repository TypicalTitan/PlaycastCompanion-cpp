#include "pch.h"
#include "Log.h"

// Port of Log.cs: a single append-only text log under %LOCALAPPDATA%\PlaycastCompanion,
// rotated to playcast-companion.old.log once it passes 1,000,000 bytes, with a
// one-shot fallback to %TEMP%\playcast-companion.log if the primary path is
// unwritable. Every call is serialized by a mutex and nothing here may throw.

namespace pc {
namespace {

constexpr ULONGLONG kRotateAboveBytes = 1'000'000;  // FileInfo.Length > 1_000_000 in the C#
constexpr wchar_t kLogFileName[] = L"playcast-companion.log";
constexpr wchar_t kOldLogFileName[] = L"playcast-companion.old.log";

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
        result = raw;
    }
    if (raw) CoTaskMemFree(raw);
    return result;
}

std::wstring EnvironmentVariable(const wchar_t* name) {
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return {};
    std::wstring value(needed, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0 || written >= needed) return {};
    value.resize(written);
    return value;
}

std::wstring LocalAppDataDir() {
    std::wstring dir = KnownFolder(FOLDERID_LocalAppData);
    if (dir.empty()) dir = EnvironmentVariable(L"LOCALAPPDATA");
    return dir;
}

std::wstring TempDir() {
    std::wstring buf(MAX_PATH + 1, L'\0');
    DWORD n = GetTempPathW(static_cast<DWORD>(buf.size()), buf.data());
    if (n == 0 || n >= buf.size()) return EnvironmentVariable(L"TEMP");
    buf.resize(n);
    return buf;
}

std::wstring JoinPath(std::wstring dir, const wchar_t* leaf) {
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    dir += leaf;
    return dir;
}

struct LogState {
    std::mutex gate;
    std::wstring dir;
    std::wstring path;
    std::wstring oldPath;
    std::wstring fallbackPath;

    LogState() {
        dir = JoinPath(LocalAppDataDir(), L"PlaycastCompanion");
        path = JoinPath(dir, kLogFileName);
        oldPath = JoinPath(dir, kOldLogFileName);
        fallbackPath = JoinPath(TempDir(), kLogFileName);
    }
};

// Function-local static so logging works even from other static initializers.
LogState& State() {
    static LogState state;
    return state;
}

std::string ToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    if (wide.size() > static_cast<size_t>(INT_MAX)) wide = wide.substr(0, static_cast<size_t>(INT_MAX));
    const int len = static_cast<int>(wide.size());
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    int written = WideCharToMultiByte(CP_UTF8, 0, wide.data(), len, out.data(), needed, nullptr, nullptr);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
}

// "yyyy-MM-dd HH:mm:ss" in local time, as DateTime.Now formats it.
std::string Timestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

bool EnsureDirectory(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    const DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) return true;
    // LOCALAPPDATA itself may be missing (unusual); create intermediates too.
    const int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS;
}

// Mirrors `if (fi.Exists && fi.Length > 1_000_000) fi.MoveTo(old, overwrite: true)`.
// Returns false only when the move was needed and failed (the C# throws there).
bool RotateIfLarge(const std::wstring& path, const std::wstring& oldPath) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return true;  // not there yet
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return true;
    ULARGE_INTEGER size{};
    size.LowPart = fad.nFileSizeLow;
    size.HighPart = fad.nFileSizeHigh;
    if (size.QuadPart <= kRotateAboveBytes) return true;
    return MoveFileExW(path.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool AppendBytes(const std::wstring& path, const std::string& bytes) {
    if (path.empty()) return false;
    UniqueHandle file(CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) return false;
    const char* p = bytes.data();
    size_t remaining = bytes.size();
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file.get(), p, chunk, &written, nullptr) || written == 0) return false;
        p += written;
        remaining -= written;
    }
    return true;
}

void WriteLine(const std::string& utf8Message) noexcept {
    try {
        LogState& s = State();
        std::lock_guard<std::mutex> lock(s.gate);
        const std::string line = Timestamp() + " " + utf8Message + "\r\n";  // Environment.NewLine
        bool ok = false;
        try {
            ok = EnsureDirectory(s.dir) && RotateIfLarge(s.path, s.oldPath) && AppendBytes(s.path, line);
        } catch (...) {
            ok = false;
        }
        if (!ok) {
            // logging must never take the app down; try one fallback location
            try {
                AppendBytes(s.fallbackPath, line);
            } catch (...) {
            }
        }
    } catch (...) {
        // swallow everything: a failing logger is never an error
    }
}

}  // namespace

void LogInfo(std::wstring_view message) {
    try {
        WriteLine(ToUtf8(message));
    } catch (...) {
    }
}

void LogInfo(std::string_view utf8Message) {
    try {
        WriteLine(std::string(utf8Message));
    } catch (...) {
    }
}

std::wstring LogFilePath() {
    try {
        return State().path;
    } catch (...) {
        return {};
    }
}

}  // namespace pc
