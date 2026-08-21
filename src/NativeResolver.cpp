#include "pch.h"
#include "NativeResolver.h"

#include "Log.h"

namespace pc::native {
namespace {

/// Directory containing the running executable (the C# AppContext.BaseDirectory).
std::wstring ExeDirectory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return std::wstring();
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        if (buffer.size() >= 32768) return std::wstring();
        buffer.resize(buffer.size() * 2);
    }
    std::filesystem::path exe(buffer.begin(), buffer.end());
    return exe.parent_path().wstring();
}

std::wstring Join(const std::wstring& directory, const wchar_t* fileName) {
    return (std::filesystem::path(directory) / fileName).wstring();
}

/// Same candidate order as the C# NativeResolver.Resolve: a copy next to the
/// exe first, then the vendor software's own install. Full paths only.
std::vector<std::wstring> Candidates(std::wstring_view logicalName) {
    std::vector<std::wstring> candidates;
    if (logicalName == L"LOGI_LED") {
        const std::wstring exeDir = ExeDirectory();
        if (!exeDir.empty()) candidates.push_back(Join(exeDir, L"LogitechLedEnginesWrapper.dll"));
        candidates.emplace_back(L"C:\\Program Files\\LGHUB\\sdk_legacy_led_x64.dll");
        candidates.emplace_back(L"C:\\Program Files\\Logitech Gaming Software\\SDK\\LED\\x64\\LogitechLedEnginesWrapper.dll");
    } else if (logicalName == L"CUESDK") {
        const std::wstring exeDir = ExeDirectory();
        if (!exeDir.empty()) {
            candidates.push_back(Join(exeDir, L"CUESDK.x64_2017.dll"));
            candidates.push_back(Join(exeDir, L"CUESDK_2017.dll"));
        }
        candidates.emplace_back(L"C:\\Program Files\\Corsair\\CORSAIR iCUE 5 Software\\CUESDK.x64_2017.dll");
        candidates.emplace_back(L"C:\\Program Files\\Corsair\\CORSAIR iCUE 4 Software\\CUESDK.x64_2017.dll");
    }
    return candidates;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace

HMODULE LoadVendorLibrary(std::wstring_view logicalName) {
    static std::mutex cacheMutex;
    static std::map<std::wstring, HMODULE> cache;

    const std::wstring key(logicalName);
    std::lock_guard lock(cacheMutex);
    if (const auto it = cache.find(key); it != cache.end()) return it->second;

    for (const std::wstring& path : Candidates(logicalName)) {
        if (!FileExists(path)) continue;
        // Absolute path + restricted search flags: the DLL's own directory and
        // the system directories only, never the CWD or PATH.
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (module) {
            LogInfo(std::format(L"NativeResolver: {} loaded from {}", key, path));
            cache[key] = module;  // kept for the process lifetime, never freed
            return module;
        }
        LogInfo(std::format(L"NativeResolver: {} failed to load {} (error {})", key, path, GetLastError()));
    }
    // Like the C# resolver, a miss is not cached: a vendor install that appears
    // later is picked up on the next attempt.
    return nullptr;
}

}  // namespace pc::native
