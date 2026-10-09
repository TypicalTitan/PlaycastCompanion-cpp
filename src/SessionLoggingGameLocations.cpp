#include "pch.h"
#include "SessionLoggingGameLocations.h"
#include <sddl.h>
#include <cwctype>

namespace pc::sessionlog {
namespace {
void Unavailable(Array& sources, std::wstring_view source, std::wstring_view message, LONG status = ERROR_INVALID_DATA) {
    const auto state = status == ERROR_ACCESS_DENIED ? L"access_denied" : L"unavailable";
    Array errors;
    errors.Append(Make({{L"Source", Text(source)}, {L"Status", Text(state)}, {L"Message", Text(message)}}));
    sources.Append(Make({{L"Source", Text(source)}, {L"Status", Text(state)}, {L"EntriesRead", Number(0)}, {L"Errors", errors}}));
}
std::wstring ReadString(HKEY hive, const std::wstring& key, const wchar_t* name, Array& sources) {
    DWORD bytes = 0;
    // Raw target-account values must never expand against the Companion owner's environment.
    const auto flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_SUBKEY_WOW6464KEY | RRF_NOEXPAND;
    auto status = RegGetValueW(hive, key.c_str(), name, flags, nullptr, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return {};
    std::wstring value(bytes <= 65536 ? bytes / sizeof(wchar_t) : 0, L'\0');
    if (status == ERROR_SUCCESS && !value.empty())
        status = RegGetValueW(hive, key.c_str(), name, flags, nullptr, value.data(), &bytes);
    if (status != ERROR_SUCCESS || value.empty()) {
        Unavailable(sources, key + L"\\" + name, std::format(L"Registry query failed (Win32 {})", status), status);
        return {};
    }
    while (!value.empty() && !value.back()) value.pop_back();
    return value;
}
std::filesystem::path ProgramData(Array& sources) {
    PWSTR text = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &text))) {
        Unavailable(sources, L"ProgramData", L"System ProgramData location unavailable.");
        return {};
    }
    const std::filesystem::path result(text);
    CoTaskMemFree(text);
    return result;
}
std::filesystem::path SystemDrive() {
    wchar_t directory[MAX_PATH]{};
    const auto length = GetWindowsDirectoryW(directory, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::filesystem::path(directory).root_name() : std::filesystem::path{};
}
std::wstring Variable(std::wstring name, const std::filesystem::path& profile, const std::filesystem::path& systemDrive) {
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (name == L"systemdrive") return systemDrive.root_name().wstring();
    if (profile.empty() || !profile.is_absolute()) return {};
    if (name == L"userprofile") return profile.wstring();
    if (name == L"localappdata") return (profile / L"AppData" / L"Local").wstring();
    if (name == L"appdata") return (profile / L"AppData" / L"Roaming").wstring();
    return {};
}
std::filesystem::path ReadLocation(HKEY hive, const std::wstring& key, const wchar_t* name,
    const std::filesystem::path& profile, const std::filesystem::path& systemDrive, Array& sources) {
    const auto raw = ReadString(hive, key, name, sources);
    if (raw.empty()) return {};
    const auto location = ExpandGuestLocation(raw, profile, systemDrive);
    if (location) return *location;
    Unavailable(sources, key + L"\\" + name,
        L"Target location is not absolute or contains an unresolved target-account variable; owner environment is not substituted.");
    return {};
}
bool GuestHiveAvailable(const std::wstring& sid, Array& sources) {
    if (sid.empty()) return false;
    HKEY hive = nullptr;
    const auto status = RegOpenKeyExW(HKEY_USERS, sid.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &hive);
    if (status == ERROR_SUCCESS) { RegCloseKey(hive); return true; }
    Unavailable(sources, L"HKEY_USERS\\" + sid,
        std::format(L"Target account registry hive is unloaded or inaccessible (Win32 {}); launcher registration coverage is unavailable.", status), status);
    return false;
}
bool SameLocation(const std::filesystem::path& left, const std::filesystem::path& right) {
    if (left.empty() || right.empty()) return false;
    const auto normalize = [](const std::filesystem::path& path) {
        std::error_code error;
        const auto resolved = std::filesystem::weakly_canonical(path, error);
        auto text = (error ? path.lexically_normal() : resolved).wstring();
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        while (!text.empty() && text.back() == L'\\') text.pop_back();
        return text;
    };
    return normalize(left) == normalize(right);
}
}

std::optional<std::filesystem::path> ExpandGuestLocation(std::wstring_view value,
    const std::filesystem::path& profile, const std::filesystem::path& systemDrive) {
    std::wstring expanded;
    size_t cursor = 0;
    while (cursor < value.size()) {
        const auto opening = value.find(L'%', cursor);
        if (opening == std::wstring_view::npos) { expanded.append(value.substr(cursor)); break; }
        expanded.append(value.substr(cursor, opening - cursor));
        const auto closing = value.find(L'%', opening + 1);
        if (closing == std::wstring_view::npos) return std::nullopt;
        const auto replacement = Variable(std::wstring(value.substr(opening + 1, closing - opening - 1)), profile, systemDrive);
        if (replacement.empty()) return std::nullopt;
        expanded += replacement;
        cursor = closing + 1;
    }
    const std::filesystem::path result(expanded);
    return result.is_absolute() ? std::optional(result.lexically_normal()) : std::nullopt;
}

GameLocations GameLocationsForProfile(const std::filesystem::path& profile,
    const std::filesystem::path& registeredSteam, const std::filesystem::path& registeredEpic,
    const std::filesystem::path& sharedEpicManifests) {
    GameLocations result;
    if (!profile.empty() && profile.is_absolute()) {
        result.steamRoots.push_back(profile / L"AppData" / L"Local" / L"Playcast" / L"Steam");
        result.legendaryInstalled = profile / L".config" / L"legendary" / L"installed.json";
        result.epicInstallRoots.push_back(profile);
    }
    if (registeredSteam.is_absolute()) result.steamRoots.push_back(registeredSteam);
    if (registeredEpic.is_absolute()) {
        result.epicManifests = registeredEpic;
        result.filterSharedEpic = SameLocation(registeredEpic, sharedEpicManifests);
    }
    return result;
}

GameLocations DiscoverGameLocations(std::wstring_view targetUsername) {
    Array sources;
    const auto sid = AccountSid(targetUsername);
    const auto systemDrive = SystemDrive();
    const auto profile = sid.empty() ? std::filesystem::path{} : ReadLocation(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\" + sid, L"ProfileImagePath", {}, systemDrive, sources);
    const auto hiveAvailable = GuestHiveAvailable(sid, sources);
    const auto userKey = sid + L"\\Software\\";
    const auto programData = ProgramData(sources);
    const auto sharedEpic = programData.is_absolute()
        ? programData / L"Epic" / L"EpicGamesLauncher" / L"Data" / L"Manifests" : std::filesystem::path{};
    auto result = GameLocationsForProfile(profile,
        !hiveAvailable ? std::filesystem::path{} : ReadLocation(HKEY_USERS, userKey + L"Valve\\Steam", L"SteamPath", profile, systemDrive, sources),
        !hiveAvailable ? std::filesystem::path{} : ReadLocation(HKEY_USERS, userKey + L"Epic Games\\EOS", L"ModSdkMetadataDir", profile, systemDrive, sources), sharedEpic);
    if (programData.is_absolute()) {
        if (result.epicManifests.empty()) result.epicManifests = sharedEpic;
        // GuestModeManagerEnter configures this folder even before the target hive is loaded.
        result.legendaryAlternates.push_back(programData / L"Playcast" / L"LegendaryConfig" / L"installed.json");
    }
    const auto drive = ReadString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Playcast\\GameStorage", L"DriveLetter", sources);
    if (drive.size() >= 2 && iswalpha(drive[0]) && drive[1] == L':')
        result.epicInstallRoots.emplace_back(drive.substr(0, 2) + L"\\Playcast\\Epic");
    if (hiveAvailable) {
        auto config = ReadLocation(HKEY_USERS, sid + L"\\Environment", L"LEGENDARY_CONFIG_PATH", profile, systemDrive, sources);
        if (config.empty()) {
            const auto xdg = ReadLocation(HKEY_USERS, sid + L"\\Environment", L"XDG_CONFIG_HOME", profile, systemDrive, sources);
            if (!xdg.empty()) config = xdg / L"legendary";
        }
        if (config.is_absolute()) result.legendaryAlternates.push_back(config / L"installed.json");
    }
    Array errors;
    if (profile.empty()) errors.Append(Make({{L"Source", Text(L"target_profile")}, {L"Status", Text(L"unavailable")},
        {L"Message", Text(L"Could not resolve the target account's current SID/profile; owner launcher libraries are not substituted.")}}));
    sources.Append(Make({{L"Source", Text(profile.empty() ? L"target_profile" : profile.wstring())},
        {L"Status", Text(profile.empty() ? L"unavailable" : L"available")}, {L"EntriesRead", Number(profile.empty() ? 0 : 1)}, {L"Errors", errors}}));
    result.sources = sources;
    return result;
}
}
