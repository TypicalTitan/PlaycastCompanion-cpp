#include "pch.h"
#include "SessionLoggingInventory.h"
#include "SessionLoggingVdf.h"
#include <cwctype>
#include <fstream>
#include <set>

namespace pc::sessionlog {
namespace {
void Coverage(Inventory& result, const std::filesystem::path& path, std::wstring_view status, int count, std::wstring_view error = L"") {
    Array errors;
    if (!error.empty()) errors.Append(Make({{L"Source", Text(path.wstring())}, {L"Status", Text(status)}, {L"Message", Text(error)}}));
    result.sources.Append(Make({{L"Source", Text(path.wstring())}, {L"Status", Text(status)}, {L"EntriesRead", Number(count)}, {L"Errors", errors}}));
}
std::wstring Normal(const std::filesystem::path& path) {
    std::error_code error;
    const auto resolved = std::filesystem::weakly_canonical(path, error);
    auto text = (error ? std::filesystem::absolute(path).lexically_normal() : resolved).wstring();
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

GameInventory::GameInventory(std::vector<std::filesystem::path> steamRoots, std::filesystem::path epicManifests,
    std::wstring targetUsername) {
    if (steamRoots.empty() && epicManifests.empty()) locations_ = DiscoverGameLocations(targetUsername);
    else {
        locations_.steamRoots = std::move(steamRoots);
        locations_.epicManifests = std::move(epicManifests);
        locations_.filterSharedEpic = false;
    }
    roots_ = locations_.steamRoots;
    epic_ = locations_.epicManifests;
}
GameInventory::GameInventory(GameLocations locations)
    : roots_(locations.steamRoots), epic_(locations.epicManifests), locations_(std::move(locations)) {}

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
    for (const auto& source : locations_.sources) result.sources.Append(source);
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
    std::set<std::wstring> metadataSeen;
    const auto readLegendary = [&](const std::filesystem::path& manifest) {
        if (manifest.empty() || stop.stop_requested()) return;
        if (!manifest.is_absolute()) {
            Coverage(result, manifest, L"unavailable", 0, L"Legendary location is not absolute; owner working directory is not substituted.");
            return;
        }
        if (metadataSeen.size() < 3 && metadataSeen.insert(Normal(manifest)).second)
            Legendary(manifest, result, stop);
    };
    readLegendary(locations_.legendaryInstalled);
    for (const auto& manifest : locations_.legendaryAlternates) readLegendary(manifest);
    std::set<std::wstring> seen;
    std::erase_if(result.games, [&](const InstalledGame& game) { return !seen.insert(game.store + L":" + game.id + L":" + Normal(game.installRoot)).second; });
    result.limitations.Append(Text(L"Steam and Epic manifests only; other launchers and standalone games are not inventoried."));
    result.limitations.Append(Text(L"Launcher discovery uses the target account's current SID/profile and registry, including Playcast Steam and Legendary metadata; protected guest files may be inaccessible from the owner session."));
    result.limitations.Append(Text(L"Shared Epic manifests without target-account registration are included only when their install location belongs to the target profile or Playcast game storage."));
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
    if (epic_.empty()) return;
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
                if (locations_.filterSharedEpic && std::none_of(locations_.epicInstallRoots.begin(), locations_.epicInstallRoots.end(),
                    [&](const auto& root) { return ContainsExecutable(root, install); })) continue;
                if (!install.is_absolute() || !std::filesystem::is_directory(install)) throw std::runtime_error("Epic installation directory unavailable");
                const auto id = json::GetString(item, L"AppName");
                if (id.empty()) throw std::runtime_error("Epic AppName missing");
                result.games.push_back({L"epic", id, json::GetString(item, L"DisplayName", id), install, entry.path()});
            } catch (const std::exception& error) { partial = true; Coverage(result, entry.path(), L"invalid_manifest", 0, json::Utf8ToWide(error.what())); }
        }
        Coverage(result, epic_, partial ? L"partial" : L"available", count);
    } catch (const std::exception& error) { Coverage(result, epic_, L"unavailable", count, json::Utf8ToWide(error.what())); }
}

void GameInventory::Legendary(const std::filesystem::path& manifest, Inventory& result, std::stop_token stop) const {
    if (manifest.empty()) return;
    int count = 0;
    bool partial = false;
    try {
        if (!std::filesystem::exists(manifest)) { Coverage(result, manifest, L"not_found", 0); return; }
        const auto entries = json::Parse(ReadManifest(manifest));
        for (const auto& entry : entries) {
            if (stop.stop_requested()) return;
            ++count;
            try {
                if (entry.Value().ValueType() != Type::Object) throw std::runtime_error("Legendary entry is not an object");
                const auto item = entry.Value().GetObject();
                const auto id = json::GetString(item, L"app_name", entry.Key().c_str());
                const std::filesystem::path install(json::GetString(item, L"install_path"));
                if (id.empty() || !install.is_absolute() || !std::filesystem::is_directory(install))
                    throw std::runtime_error("Legendary installation directory unavailable");
                result.games.push_back({L"epic", id, json::GetString(item, L"title", id), install, manifest});
            } catch (const std::exception& error) { partial = true; Coverage(result, manifest, L"invalid_manifest", 0, json::Utf8ToWide(error.what())); }
        }
        Coverage(result, manifest, partial ? L"partial" : L"available", count);
    } catch (const std::exception& error) { Coverage(result, manifest, L"unavailable", count, json::Utf8ToWide(error.what())); }
}
}
