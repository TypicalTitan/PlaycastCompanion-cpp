#include "pch.h"
#include "Config.h"
#include "Json.h"

namespace pc {
namespace {
using json::JsonValue;
class JsonWriter {
public:
    void Begin() {
        out_ += L'{';
        depth_ = 1;
        first_ = true;
    }
    void End() {
        depth_ = 0;
        out_ += L"\r\n}";
    }
    void BeginObject(std::wstring_view key) {
        Key(key);
        out_ += L'{';
        ++depth_;
        first_ = true;
    }
    void EndObject() {
        --depth_;
        if (first_) {
            out_ += L'}';  // empty object: "{}"
        } else {
            NewLine();
            out_ += L'}';
        }
        first_ = false;
    }
    void String(std::wstring_view key, std::wstring_view value) {
        Key(key);
        out_ += Quote(value);
    }
    void NullableString(std::wstring_view key, std::wstring_view value) {
        Key(key);
        if (value.empty()) {
            out_ += L"null";
        } else {
            out_ += Quote(value);
        }
    }
    void Bool(std::wstring_view key, bool value) {
        Key(key);
        out_ += value ? L"true" : L"false";
    }
    void Int(std::wstring_view key, int value) {
        Key(key);
        out_ += std::to_wstring(value);
    }
    const std::wstring& Text() const { return out_; }

private:
    static std::wstring Quote(std::wstring_view s) {
        return std::wstring(std::wstring_view(JsonValue::CreateStringValue(winrt::hstring(s)).Stringify()));
    }
    void NewLine() {
        out_ += L"\r\n";
        for (int i = 0; i < depth_; ++i) out_ += L"  ";
    }
    void Key(std::wstring_view key) {
        if (!first_) out_ += L',';
        first_ = false;
        NewLine();
        out_ += Quote(key);
        out_ += L": ";
    }

    std::wstring out_;
    int depth_ = 0;
    bool first_ = true;
};

}

std::string AppConfig::ToJson() const {
    // Property order follows the C# class declarations (System.Text.Json emits
    // properties in declaration order).
    JsonWriter w;
    w.Begin();
    w.String(L"TargetUsername", TargetUsername);
    w.Int(L"TickSeconds", TickSeconds);
    w.Int(L"RetryInitSeconds", RetryInitSeconds);
    w.Bool(L"IncludeDisconnectedSessions", IncludeDisconnectedSessions);
    w.String(L"ChromaInitUrl", ChromaInitUrl);
    w.Bool(L"RazerEnabled", RazerEnabled);

    w.BeginObject(L"SessionLogging");
    w.Bool(L"Enabled", SessionLogging.Enabled);
    w.Bool(L"IncludeSecrets", false); // opt-in is intentionally process-lifetime only
    w.Int(L"SnapshotIntervalSeconds", SessionLogging.SnapshotIntervalSeconds);
    w.Int(L"MaxFileBytes", SessionLogging.MaxFileBytes);
    w.Int(L"MaxSessionBytes", SessionLogging.MaxSessionBytes);
    w.Int(L"RetentionDays", SessionLogging.RetentionDays);
    w.EndObject();

    w.BeginObject(L"SteelSeries");
    w.Bool(L"Enabled", SteelSeries.Enabled);
    w.EndObject();

    w.BeginObject(L"Logitech");
    w.Bool(L"Enabled", Logitech.Enabled);
    w.EndObject();

    w.BeginObject(L"Corsair");
    w.Bool(L"Enabled", Corsair.Enabled);
    w.EndObject();

    w.BeginObject(L"OpenRgb");
    w.Bool(L"Enabled", OpenRgb.Enabled);
    w.String(L"Host", OpenRgb.Host);
    w.Int(L"Port", OpenRgb.Port);
    w.String(L"BlackoutProfile", OpenRgb.BlackoutProfile);
    w.EndObject();

    w.BeginObject(L"DynamicLighting");
    w.Bool(L"Enabled", DynamicLighting.Enabled);
    w.Bool(L"ExcludeVendorOwnedDevices", DynamicLighting.ExcludeVendorOwnedDevices);
    w.EndObject();

    w.BeginObject(L"RegistryWatch");
    w.Bool(L"Enabled", RegistryWatch.Enabled);
    w.String(L"Hive", RegistryWatch.Hive);
    w.String(L"SubKey", RegistryWatch.SubKey);
    w.NullableString(L"ValueName", RegistryWatch.ValueName);
    w.NullableString(L"ActiveValue", RegistryWatch.ActiveValue);
    w.EndObject();

    w.BeginObject(L"Discord");
    w.Bool(L"Enabled", Discord.Enabled);
    w.String(L"ApplicationId", Discord.ApplicationId);
    w.String(L"Details", Discord.Details);
    w.String(L"State", Discord.State);
    w.String(L"LargeImageKey", Discord.LargeImageKey);
    w.String(L"LargeImageText", Discord.LargeImageText);
    w.Bool(L"GamePassthroughEnabled", Discord.GamePassthroughEnabled);
    w.String(L"HostingTemplate", Discord.HostingTemplate);
    w.String(L"ShimFallbackGame", Discord.ShimFallbackGame);
    w.String(L"ShimPipeName", Discord.ShimPipeName);
    w.BeginObject(L"ResolvedNames");
    for (const auto& [id, name] : Discord.ResolvedNames) {
        w.String(id, name);
    }
    w.EndObject();
    w.EndObject();

    w.End();
    return json::WideToUtf8(w.Text());
}

}
