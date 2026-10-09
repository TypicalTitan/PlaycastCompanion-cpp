#pragma once
#include "SessionLoggingJson.h"
#include <optional>

namespace pc::sessionlog {
struct GameLocations {
    std::vector<std::filesystem::path> steamRoots;
    std::filesystem::path epicManifests, legendaryInstalled;
    std::vector<std::filesystem::path> legendaryAlternates;
    std::vector<std::filesystem::path> epicInstallRoots;
    bool filterSharedEpic = true;
    Array sources;
};
GameLocations DiscoverGameLocations(std::wstring_view targetUsername);
std::optional<std::filesystem::path> ExpandGuestLocation(std::wstring_view value,
    const std::filesystem::path& profile, const std::filesystem::path& systemDrive);
GameLocations GameLocationsForProfile(const std::filesystem::path& profile,
    const std::filesystem::path& registeredSteam = {}, const std::filesystem::path& registeredEpic = {},
    const std::filesystem::path& sharedEpicManifests = {});
}
