#pragma once
// Schema-compatible with the C# app's config.json — an installed config written
// by either version must load in the other unchanged.
#include <map>
#include <string>
#include <string_view>

namespace pc {
struct SteelSeriesConfig { bool Enabled = false; };
struct LogitechConfig { bool Enabled = false; };
struct CorsairConfig { bool Enabled = false; };

struct OpenRgbConfig {
    bool Enabled = false;
    std::wstring Host = L"127.0.0.1";
    int Port = 6742;
    std::wstring BlackoutProfile = L"Blackout";
};

struct DynamicLightingConfig {
    bool Enabled = false;
    /// Skip LampArray devices whose vendor already has an enabled engine, so two
    /// engines never fight over the same hardware.
    bool ExcludeVendorOwnedDevices = true;
};

struct DiscordConfig {
    bool Enabled = true;
    /// Config-file only by design; never exposed in the UI.
    std::wstring ApplicationId;
    std::wstring Details = L"In Nonsole Mode: Accruing Rewards by Hosting";
    std::wstring State;
    std::wstring LargeImageKey = L"playcast_squid";
    std::wstring LargeImageText = L"Playcast";
    bool GamePassthroughEnabled = false;
    std::wstring HostingTemplate = L"Hosting {game} in Nonsole Mode";
    std::wstring ShimFallbackGame = L"a game";
    std::wstring ShimPipeName = L"playcast-companion-shim";
    std::map<std::wstring, std::wstring> ResolvedNames;
};

struct RegistryWatchConfig {
    bool Enabled = false;
    std::wstring Hive = L"HKLM";
    std::wstring SubKey = L"SOFTWARE\\Playcast\\GuestMode";
    std::wstring ValueName = L"VirtualDisplayDeviceName";
    std::wstring ActiveValue;  // empty = any non-empty, non-"0" data counts
};

struct SessionLoggingConfig {
    bool Enabled = true;
    int SnapshotIntervalSeconds = 30;
    int MaxFileBytes = 10 * 1024 * 1024;
    int MaxSessionBytes = 100 * 1024 * 1024;
    int RetentionDays = 14;
};

struct AppConfig {
    std::wstring TargetUsername = L"NonsoleMode";
    int TickSeconds = 5;
    int RetryInitSeconds = 30;
    bool IncludeDisconnectedSessions = true;
    std::wstring ChromaInitUrl = L"http://localhost:54235/razer/chromasdk";
    bool RazerEnabled = true;
    SteelSeriesConfig SteelSeries;
    LogitechConfig Logitech;
    CorsairConfig Corsair;
    OpenRgbConfig OpenRgb;
    DynamicLightingConfig DynamicLighting;
    RegistryWatchConfig RegistryWatch;
    DiscordConfig Discord;
    SessionLoggingConfig SessionLogging;

    /// <exe directory>\config.json
    static std::wstring ConfigPath();
    /// Load ConfigPath(); any failure (missing file, bad JSON) yields defaults.
    /// Always applies Normalize() to the result. Never throws.
    static AppConfig Load();
    /// Clamp every user-editable value into a safe range (ticks 1..10, retry
    /// 5..600, port 1..65535, Chroma URL must be loopback, strings sanitized).
    void Normalize();
    /// Indented UTF-8 JSON using the C# app's key names exactly.
    std::string ToJson() const;
    /// Strip control chars, trim, cap length; empty -> fallback.
    static std::wstring Sanitize(std::wstring_view value, size_t maxLength, std::wstring_view fallback);
};
}  // namespace pc
