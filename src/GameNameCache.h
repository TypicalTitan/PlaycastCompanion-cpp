#pragma once
// Owner-side resolver: Discord Application ID -> display name. Ladder: cache
// (config seed ResolvedNames under a persistent %LOCALAPPDATA%\PlaycastCompanion\
// resolved-names.json), then unauthenticated GET
// https://discord.com/api/v9/applications/{id}/rpc (3 s timeout, take "name"),
// then a non-generic exe hint (reject engine/launcher/OS strings), then
// ShimFallbackGame. Results sanitized to 64 chars. Never writes config.json.
#include "Config.h"
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace pc {
class GameNameCache {
public:
    explicit GameNameCache(const DiscordConfig& cfg);
    std::wstring Resolve(std::wstring_view clientId, std::wstring_view procHint);

private:  // module owner may extend
    const DiscordConfig& cfg_;
    std::mutex mutex_;
    std::map<std::wstring, std::wstring> cache_;
    void LoadDisk();
    void SaveDisk();
    std::mutex diskMutex_;  // serialises writers of resolved-names.json
};
}  // namespace pc
