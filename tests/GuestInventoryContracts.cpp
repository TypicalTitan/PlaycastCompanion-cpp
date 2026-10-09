#include "pch.h"
#include "SessionLoggingInventory.h"
#include <fstream>
#include <iostream>

using namespace pc;
using namespace pc::sessionlog;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() /
        std::format(L"pc-guest-inventory-{}-{}", GetCurrentProcessId(), GetTickCount64());
    Fixture() { std::filesystem::create_directories(root); }
    ~Fixture() {
        std::error_code error;
        if (root.parent_path() == std::filesystem::temp_directory_path() && root.filename().wstring().starts_with(L"pc-guest-inventory-"))
            std::filesystem::remove_all(root, error);
    }
    void Write(const std::filesystem::path& path, std::string_view content) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << content;
    }
    void Steam(const std::filesystem::path& library, std::string_view id, std::string_view name) {
        Write(library / L"steamapps" / (L"appmanifest_" + json::Utf8ToWide(id) + L".acf"),
            "\"AppState\" { \"appid\" \"" + std::string(id) + "\" \"name\" \"" + std::string(name) +
            "\" \"installdir\" \"" + std::string(name) + "\" \"StateFlags\" \"4\" }");
        std::filesystem::create_directories(library / L"steamapps" / L"common" / json::Utf8ToWide(name));
    }
};
void GuestRootsContract() {
    Fixture fixture;
    const auto profile = fixture.root / L"NonsoleMode.000";
    auto locations = GameLocationsForProfile(profile);
    Require(locations.steamRoots.size() == 1 && locations.steamRoots[0] == profile / L"AppData" / L"Local" / L"Playcast" / L"Steam",
        "Guest launcher discovery must follow the resolved profile, including suffixes");
    fixture.Steam(locations.steamRoots[0], "10", "Guest");
    fixture.Steam(fixture.root / L"OwnerSteam", "20", "Owner");
    locations.epicManifests = fixture.root / L"SharedEpic";
    const auto guestEpic = profile / L"Games" / L"GuestEpic";
    const auto ownerEpic = fixture.root / L"OwnerEpic";
    for (const auto& path : {guestEpic, ownerEpic}) std::filesystem::create_directories(path);
    fixture.Write(locations.epicManifests / L"guest.item", json::Stringify(Make({{L"AppName", Text(L"guest")},
        {L"DisplayName", Text(L"Guest Epic")}, {L"InstallLocation", Text(guestEpic.wstring())}})));
    fixture.Write(locations.epicManifests / L"owner.item", json::Stringify(Make({{L"AppName", Text(L"owner")},
        {L"DisplayName", Text(L"Owner Epic")}, {L"InstallLocation", Text(ownerEpic.wstring())}})));
    const auto inventory = GameInventory(locations).Capture();
    Require(inventory.games.size() == 2 && std::none_of(inventory.games.begin(), inventory.games.end(),
        [](const auto& game) { return game.name == L"Owner" || game.id == L"owner"; }), "Owner installations must not contaminate guest inventory");
    const auto registered = GameLocationsForProfile(profile, fixture.root / L"RegisteredSteam", locations.epicManifests);
    Require(registered.steamRoots.size() == 2 && !registered.filterSharedEpic, "Explicit guest launcher registrations must remain usable");
    const auto registeredShared = GameLocationsForProfile(profile, {}, locations.epicManifests, locations.epicManifests);
    const auto sharedInventory = GameInventory(registeredShared).Capture();
    Require(registeredShared.filterSharedEpic && sharedInventory.games.size() == 2
        && std::none_of(sharedInventory.games.begin(), sharedInventory.games.end(), [](const auto& game) { return game.id == L"owner"; }),
        "A guest registration pointing at global Epic manifests must still exclude owner installation roots");
}
void LegendaryContract() {
    Fixture fixture;
    auto locations = GameLocationsForProfile(fixture.root / L"NonsoleMode.000");
    const auto install = fixture.root / L"PlaycastEpic" / L"Guest";
    std::filesystem::create_directories(install);
    fixture.Write(locations.legendaryInstalled, json::Stringify(Make({{L"game", Make({{L"app_name", Text(L"guest")},
        {L"title", Text(L"Guest Legendary")}, {L"install_path", Text(install.wstring())}})},
        {L"missing", Make({{L"app_name", Text(L"missing")}, {L"install_path", Text((fixture.root / L"deleted").wstring())}})}})));
    const auto inventory = GameInventory(locations).Capture();
    Require(inventory.games.size() == 1 && inventory.games[0].store == L"epic" && inventory.games[0].manifest == locations.legendaryInstalled,
        "Legendary inventory must read only guest metadata and retain installation/manifest provenance");
    Require(std::any_of(inventory.sources.begin(), inventory.sources.end(), [](const auto& value) {
        return json::GetString(value.GetObject(), L"Status") == L"partial"; }), "Missing Legendary directories must leave partial coverage");
}
void GuestVariableContract() {
    const std::filesystem::path profile = L"C:\\Users\\NonsoleMode.000";
    const auto local = ExpandGuestLocation(L"%LoCaLaPpDaTa%\\Playcast\\Steam", profile, L"C:");
    Require(local && *local == profile / L"AppData" / L"Local" / L"Playcast" / L"Steam",
        "Guest variables must expand with the resolved suffixed profile, never the owner environment");
    const auto roaming = ExpandGuestLocation(L"%APPDATA%\\legendary", profile, L"C:");
    Require(roaming && *roaming == profile / L"AppData" / L"Roaming" / L"legendary", "Guest APPDATA must resolve under the target profile");
    const auto target = ExpandGuestLocation(L"%USERPROFILE%\\.config\\legendary", profile, L"C:");
    Require(target && *target == profile / L".config" / L"legendary", "Guest USERPROFILE must resolve from ProfileList");
    const auto system = ExpandGuestLocation(L"%SystemDrive%\\Users\\NonsoleMode.000", {}, L"Z:");
    Require(system && *system == L"Z:\\Users\\NonsoleMode.000", "Profile discovery may expand only the actual Windows system drive");
    Require(!ExpandGuestLocation(L"%USERNAME%\\Steam", profile, L"C:")
        && !ExpandGuestLocation(L"%LOCALAPPDATA%\\Steam", {}, L"C:")
        && !ExpandGuestLocation(L"C:\\Games\\%UNKNOWN%", profile, L"C:")
        && !ExpandGuestLocation(L"C:\\Games\\%UNFINISHED", profile, L"C:"),
        "Unknown/unresolved guest variables must stay unavailable rather than using owner process values");
}
void LegendaryAlternatesContract() {
    Fixture fixture;
    auto locations = GameLocationsForProfile(fixture.root / L"NonsoleMode.000");
    const auto canonical = fixture.root / L"ProgramData" / L"Playcast" / L"LegendaryConfig" / L"installed.json";
    const auto registered = fixture.root / L"GuestRegistered" / L"installed.json";
    const auto excluded = fixture.root / L"OwnerConfig" / L"installed.json";
    const auto install = fixture.root / L"PlaycastEpic" / L"Guest";
    std::filesystem::create_directories(install);
    const auto metadata = json::Stringify(Make({{L"guest", Make({{L"app_name", Text(L"guest")},
        {L"title", Text(L"Canonical Guest")}, {L"install_path", Text(install.wstring())}})}}));
    fixture.Write(canonical, metadata);
    fixture.Write(registered, metadata);
    fixture.Write(excluded, metadata);
    auto alternateCase = canonical.wstring();
    std::transform(alternateCase.begin(), alternateCase.end(), alternateCase.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
    locations.legendaryAlternates = {canonical, alternateCase, registered, excluded};
    const auto inventory = GameInventory(locations).Capture();
    Require(inventory.games.size() == 1 && inventory.games[0].manifest == canonical,
        "Canonical guest Legendary metadata must work without a loaded hive and deduplicate repeated registered installations");
    int metadataSources = 0;
    for (const auto& source : inventory.sources) {
        const auto path = json::GetString(source.GetObject(), L"Source");
        if (std::filesystem::path(path).filename() == L"installed.json") ++metadataSources;
        Require(path != excluded.wstring(), "Legendary discovery must be bounded to three known target sources");
    }
    Require(metadataSources == 3, "Legendary metadata locations must be deduplicated case-insensitively and bounded");
}
}
int RunGuestInventoryContracts() {
    GuestRootsContract(); std::cout << "PASS guest launcher roots\n";
    LegendaryContract(); std::cout << "PASS guest Legendary inventory\n";
    GuestVariableContract(); std::cout << "PASS guest environment expansion\n";
    LegendaryAlternatesContract(); std::cout << "PASS canonical guest Legendary sources\n";
    return 4;
}
