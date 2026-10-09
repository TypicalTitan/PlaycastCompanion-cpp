#include "pch.h"
#include "TrayAppInternal.h"

namespace pc::traydetail {
std::atomic<bool> g_harnessMode{false};
std::wstring Win32Message(DWORD error) {
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text;
    if (length != 0 && buffer != nullptr)
        text.assign(buffer, length);
    if (buffer != nullptr)
        LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();
    if (text.empty())
        text = std::format(L"error {}", error);
    return text;
}

std::wstring Describe(const std::exception& ex) {
    return json::Utf8ToWide(ex.what());
}

/// CREATE_ALWAYS + write all bytes. On failure `error` holds the Win32 code.
bool WriteTextFile(const std::wstring& path, const std::string& bytes, DWORD& error) {
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD createError = GetLastError();
    UniqueHandle file(raw);
    if (!file.valid()) {
        error = createError;
        return false;
    }
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file.get(), bytes.data() + offset, chunk, &written, nullptr)) {
            error = GetLastError();
            return false;
        }
        if (written == 0) {
            error = ERROR_WRITE_FAULT;
            return false;
        }
        offset += written;
    }
    error = ERROR_SUCCESS;
    return true;
}

/// %TEMP% with a trailing backslash (GetTempPathW); empty on failure.
std::wstring TempDirectory() {
    wchar_t buffer[MAX_PATH + 2]{};
    const DWORD n = GetTempPathW(static_cast<DWORD>(std::size(buffer)), buffer);
    if (n == 0 || n >= std::size(buffer))
        return {};
    return std::wstring(buffer, n);
}

std::wstring ModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0)
            return {};
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring SystemCmdPath() {
    wchar_t buffer[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(buffer, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return L"cmd.exe";
    return std::wstring(buffer, n) + L"\\cmd.exe";
}

/// Stop several backends at once (mirrors Task.WhenAll in C#); each
/// StopBlackout is bounded (~5 s) so the whole call is too.
void StopBackendsParallel(const std::vector<ILightingBackend*>& backends) {
    std::vector<std::jthread> workers;
    workers.reserve(backends.size());
    for (ILightingBackend* backend : backends) {
        workers.emplace_back([backend] {
            MtaScope apartment;
            try {
                backend->StopBlackout();
            } catch (const std::exception& ex) {
                LogInfo(std::format(L"{} stop failed: {}", backend->DisplayName(), Describe(ex)));
            } catch (...) {
                LogInfo(std::format(L"{} stop failed", backend->DisplayName()));
            }
        });
    }
    // jthread destructors join every worker before returning
}


}
