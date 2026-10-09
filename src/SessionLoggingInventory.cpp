#include "pch.h"
#include "SessionLoggingInventory.h"
#include "SessionLoggingVdf.h"
#include <cwctype>
#include <fstream>
#include <set>

namespace pc::sessionlog {
namespace {
std::wstring RegistryString(HKEY root, const wchar_t* key, const wchar_t* name, DWORD flags = RRF_RT_REG_SZ) {
    DWORD bytes = 0;
    if (RegGetValueW(root, key, name, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes > 65536) return {};
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, name, flags, nullptr, value.data(), &bytes) != ERROR_SUCCESS) return {};
    while (!value.empty() && !value.back()) value.pop_back();
    return value;
}
std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &value))) return {};
    struct Memory { PWSTR value; ~Memory() { CoTaskMemFree(value); } } memory{value};
    return value;
}
void Coverage(Inventory& result, const std::filesystem::path& path, std::wstring_view status, int count, std::wstring_view error = L"") {
    Array errors;
    if (!error.empty()) errors.Append(Make({{L"Source", Text(path.wstring())}, {L"Status", Text(status)}, {L"Message", Text(error)}}));
    result.sources.Append(Make({{L"Source", Text(path.wstring())}, {L"Status", Text(status)}, {L"EntriesRead", Number(count)}, {L"Errors", errors}}));
}
std::wstring Normal(const std::filesystem::path& path) {
    auto text = std::filesystem::absolute(path).lexically_normal().wstring();
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    std::replace(text.begin(), text.end(), L'/', L'\\');
    while (!text.empty() && text.back() == L'\\') text.pop_back();
    return text;
}
}

Object InstalledGame::ToJson() const {
    return Make({{L"Store", Text(store)}, {L"Id", Text(id)}, {L"Name", Text(name)},
        {L"InstallRoot", Text(installRoot.wstring())}, {L"ManifestPath", Text(manifest.wstring())}});
}
Array Inventory::GamesJson() const { Array result; for (const auto& game : games) result.Append(game.ToJson()); return result; }

GameInventory::GameInventory(std::vector<std::filesystem::path> steamRoots, std::filesystem::path epicManifests)
    : roots_(std::move(steamRoots)), epic_(std::move(epicManifests)) {
    if (roots_.empty()) {
        roots_.push_back(KnownFolder(FOLDERID_ProgramFilesX86) / L"Steam");
        for (const auto& path : {RegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
            RegistryString(HKEY_LOCAL_MACHINE, L"Software\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY)})
            if (!path.empty() && std::filesystem::path(path).is_absolute()) roots_.emplace_back(path);
    }
    if (epic_.empty()) epic_ = KnownFolder(FOLDERID_ProgramData) / L"Epic" / L"EpicGamesLauncher" / L"Data" / L"Manifests";
}

bool GameInventory::ContainsExecutable(const std::filesystem::path& root, const std::filesystem::path& executable) {
    try { return Normal(executable).starts_with(Normal(root) + L"\\"); }
    catch (...) { return false; }
}

std::string GameInventory::ReadManifest(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Manifest inaccessible");
    const auto length = file.tellg();
    if (length < 0 || length > 4 * 1024 * 1024) throw std::runtime_error("Manifest exceeds 4 MiB");
    std::string bytes(static_cast<size_t>(length), '\0');
    file.seekg(0);
    if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("Manifest read failed");
    return json::DecodeTextFile(bytes);
}

Inventory GameInventory::Capture(std::stop_token stop) const {
    Inventory result;
    std::set<std::filesystem::path> libraries;
    for (const auto& root : roots_) {
        if (stop.stop_requested()) return result;
        libraries.insert(root);
        const auto manifest = root / L"steamapps" / L"libraryfolders.vdf";
        try {
            if (!std::filesystem::exists(manifest)) { Coverage(result, manifest, L"not_found", 0); continue; }
            const auto folders = VdfNode::Parse(json::Utf8ToWide(ReadManifest(manifest))).Child(L"libraryfolders");
            for (const auto& [key, node] : folders.children) {
                if (key.empty() || !std::all_of(key.begin(), key.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) continue;
                const auto value = node.value.empty() ? node.Child(L"path").value : node.value;
                if (std::filesystem::path(value).is_absolute()) libraries.emplace(value);
            }
            Coverage(result, manifest, L"available", static_cast<int>(folders.children.size()));
        } catch (const std::exception& error) { Coverage(result, manifest, L"partial", 0, json::Utf8ToWide(error.what())); }
    }
    for (const auto& library : libraries) { if (stop.stop_requested()) return result; SteamLibrary(library, result, stop); }
    Epic(result, stop);
    std::set<std::wstring> seen;
    std::erase_if(result.games, [&](const InstalledGame& game) { return !seen.insert(game.store + L":" + game.id + L":" + Normal(game.installRoot)).second; });
    result.limitations.Append(Text(L"Steam and Epic manifests only; other launchers and standalone games are not inventoried."));
    result.limitations.Append(Text(L"Steam discovery uses the Companion account registry and common paths; another account's custom libraries may be unavailable."));
    result.limitations.Append(Text(L"Running-game matches require an accessible executable under a known installation in the target Windows session."));
    return result;
}

void GameInventory::SteamLibrary(const std::filesystem::path& root, Inventory& result, std::stop_token stop) const {
    const auto apps = root / L"steamapps";
    int count = 0;
    bool partial = false;
    try {
        if (!std::filesystem::is_directory(apps)) { Coverage(result, apps, L"not_found", 0); return; }
        for (const auto& entry : std::filesystem::directory_iterator(apps)) {
            if (stop.stop_requested()) return;
            const auto name = entry.path().filename().wstring();
            if (!name.starts_with(L"appmanifest_") || entry.path().extension() != L".acf") continue;
            ++count;
            try {
                const auto state = VdfNode::Parse(json::Utf8ToWide(ReadManifest(entry.path()))).Child(L"AppState");
                const auto directory = std::filesystem::path(state.Child(L"installdir").value);
                const auto common = apps / L"common";
                const auto install = (common / directory).lexically_normal();
                if (directory.is_absolute() || !ContainsExecutable(common, install)) throw std::runtime_error("Install directory escapes Steam library");
                if ((std::stoul(state.Child(L"StateFlags").value) & 4) == 0) continue;
                if (!std::filesystem::is_directory(install)) throw std::runtime_error("Installation directory unavailable");
                result.games.push_back({L"steam", state.Child(L"appid").value, state.Child(L"name").value, install, entry.path()});
            } catch (const std::exception& error) { partial = true; Coverage(result, entry.path(), L"invalid_manifest", 0, json::Utf8ToWide(error.what())); }
        }
        Coverage(result, apps, partial ? L"partial" : L"available", count);
    } catch (const std::exception& error) { Coverage(result, apps, L"unavailable", count, json::Utf8ToWide(error.what())); }
}

void GameInventory::Epic(Inventory& result, std::stop_token stop) const {
    int count = 0;
    bool partial = false;
    try {
        if (!std::filesystem::is_directory(epic_)) { Coverage(result, epic_, L"not_found", 0); return; }
        for (const auto& entry : std::filesystem::directory_iterator(epic_)) {
            if (stop.stop_requested()) return;
            if (entry.path().extension() != L".item") continue;
            ++count;
            try {
                const auto item = json::Parse(ReadManifest(entry.path()));
                if (json::GetBool(item, L"bIsIncompleteInstall", false)) continue;
                const std::filesystem::path install(json::GetString(item, L"InstallLocation"));
                if (!install.is_absolute() || !std::filesystem::is_directory(install)) throw std::runtime_error("Epic installation directory unavailable");
                const auto id = json::GetString(item, L"AppName");
                if (id.empty()) throw std::runtime_error("Epic AppName missing");
                result.games.push_back({L"epic", id, json::GetString(item, L"DisplayName", id), install, entry.path()});
            } catch (const std::exception& error) { partial = true; Coverage(result, entry.path(), L"invalid_manifest", 0, json::Utf8ToWide(error.what())); }
        }
        Coverage(result, epic_, partial ? L"partial" : L"available", count);
    } catch (const std::exception& error) { Coverage(result, epic_, L"unavailable", count, json::Utf8ToWide(error.what())); }
}
}
