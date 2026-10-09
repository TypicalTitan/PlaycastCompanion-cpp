#pragma once
#include "SessionLoggingJson.h"
#include "SessionLoggingGameLocations.h"

namespace pc::sessionlog {
struct InstalledGame {
    std::wstring store, id, name;
    std::filesystem::path installRoot, manifest;
    Object ToJson() const;
};
struct Inventory {
    std::vector<InstalledGame> games;
    Array sources;
    Array limitations;
    Array GamesJson() const;
};
class GameInventory {
public:
    explicit GameInventory(std::vector<std::filesystem::path> steamRoots = {}, std::filesystem::path epicManifests = {},
        std::wstring targetUsername = L"NonsoleMode");
    explicit GameInventory(GameLocations locations);
    Inventory Capture(std::stop_token stop = {}) const;
    static bool ContainsExecutable(const std::filesystem::path& root, const std::filesystem::path& executable);
    static std::string ReadManifest(const std::filesystem::path& path);
private:
    void SteamLibrary(const std::filesystem::path& root, Inventory& result, std::stop_token stop) const;
    void Epic(Inventory& result, std::stop_token stop) const;
    void Legendary(const std::filesystem::path& manifest, Inventory& result, std::stop_token stop) const;
    std::vector<std::filesystem::path> roots_;
    std::filesystem::path epic_;
    GameLocations locations_;
};
}
