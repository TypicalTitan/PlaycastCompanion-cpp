#include "pch.h"
#include "SteelSeriesController.h"

#include <cctype>
#include <fstream>
#include <iterator>

#include "Http.h"
#include "Json.h"
#include "Log.h"

namespace pc {
namespace {
constexpr const char* kGame = "PLAYCAST_COMPANION";
constexpr std::array<const char*, 5> kDeviceTypes = {"keyboard", "mouse", "headset", "mousepad", "indicator"};

constexpr unsigned kTimeoutMs = 5000;         // registration (HttpClient.Timeout in the C# version)
constexpr unsigned kEventTimeoutMs = 3000;    // per-tick event/heartbeat posts: bounds a stop mid-request
constexpr unsigned kReleaseTimeoutMs = 3000;  // remove_game gets its own 3 s budget

// FOLDERID_ProgramData = {62AB5D82-FDC1-4DC3-A9DD-070D1D495D97}, spelled out so
// the translation unit does not depend on uuid.lib for the SDK's extern GUID.
constexpr GUID kProgramDataFolder = {
    0x62AB5D82, 0xFDC1, 0x4DC3, {0xA9, 0xDD, 0x07, 0x0D, 0x1D, 0x49, 0x5D, 0x97}};

std::wstring ProgramDataPath() {
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(kProgramDataFolder, KF_FLAG_DEFAULT, nullptr, &raw);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> guard(raw, &CoTaskMemFree);
    if (FAILED(hr) || raw == nullptr)
        throw std::runtime_error("ProgramData folder is unavailable");
    return std::wstring(raw);
}

std::string ReadFileUtf8(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("coreProps.json could not be read");
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF)
        data.erase(0, 3);
    return data;
}

// Cap an error body at `maxBytes` without splitting a UTF-8 sequence.
std::string TruncateUtf8(std::string body, size_t maxBytes) {
    if (body.size() <= maxBytes)
        return body;
    body.resize(maxBytes);
    while (!body.empty() && (static_cast<unsigned char>(body.back()) & 0xC0) == 0x80)
        body.pop_back();
    return body;
}

std::string ToUpperAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

void PostJson(const std::wstring& baseUrl, const wchar_t* path, const std::string& body, unsigned timeoutMs) {
    const http::Response resp = http::Request("POST", baseUrl + path, body, L"application/json", timeoutMs);
    if (http::IsSuccess(resp))
        return;
    throw std::runtime_error(std::format("{} -> HTTP {} {}", json::WideToUtf8(path), resp.status,
                                         TruncateUtf8(resp.body, 160)));
}
}  // namespace

SteelSeriesController::SteelSeriesController(const SteelSeriesConfig& cfg) : cfg_(cfg) {}

bool SteelSeriesController::Enabled() const { return cfg_.Enabled; }

std::wstring SteelSeriesController::UnavailableText(const std::exception&) const {
    return L"SteelSeries GG not reachable — retrying";
}

void SteelSeriesController::OnConnectionLost() { baseUrl_.clear(); }

void SteelSeriesController::ApplyTick(std::stop_token stop) {
    if (baseUrl_.empty()) {
        RegisterGame(stop);
        if (stop.stop_requested() || baseUrl_.empty())
            return;
    }

    for (const std::string& evt : events_) {
        PostJson(baseUrl_, L"/game_event",
                 std::format(R"({{"game":"{}","event":"{}","data":{{"value":1}}}})", kGame, evt), kEventTimeoutMs);
        if (stop.stop_requested())
            return;
    }
    PostJson(baseUrl_, L"/game_heartbeat", std::format(R"({{"game":"{}"}})", kGame), kEventTimeoutMs);
}

void SteelSeriesController::RegisterGame(std::stop_token stop) {
    const std::filesystem::path props =
        std::filesystem::path(ProgramDataPath()) / L"SteelSeries" / L"SteelSeries Engine 3" / L"coreProps.json";
    std::error_code ec;
    if (!std::filesystem::exists(props, ec))
        throw std::runtime_error("coreProps.json not found (is SteelSeries GG installed?)");
    const json::JsonObject doc = json::Parse(ReadFileUtf8(props));
    const std::wstring address = json::GetString(doc, L"address");
    if (address.empty())
        throw std::runtime_error("no address in coreProps.json");
    const std::wstring url = L"http://" + address;

    PostJson(url, L"/game_metadata",
             std::format(R"({{"game":"{}","game_display_name":"Playcast Companion","developer":"personal"}})", kGame),
             kTimeoutMs);

    std::vector<std::string> bound;
    for (const char* deviceType : kDeviceTypes) {
        const std::string evt = "BLACKOUT_" + ToUpperAscii(deviceType);
        try {
            PostJson(url, L"/bind_game_event",
                     std::format(R"({{"game":"{}","event":"{}","min_value":0,"max_value":1,)"
                                 R"("handlers":[{{"device-type":"{}","zone":"all","mode":"color",)"
                                 R"("color":{{"red":0,"green":0,"blue":0}}}}]}})",
                                 kGame, evt, deviceType),
                     kTimeoutMs);
            bound.push_back(evt);
        } catch (const std::exception& ex) {
            // a device type GG doesn't like shouldn't sink the rest
            LogInfo(std::format("GameSense: binding {} failed ({})", deviceType, ex.what()));
        }
        if (stop.stop_requested())
            return;
    }
    if (bound.empty())
        throw std::runtime_error("GameSense rejected every handler binding");
    events_ = std::move(bound);
    baseUrl_ = url;
    LogInfo(std::format(L"GameSense registered ({} device types) at {}", events_.size(), url));
}

void SteelSeriesController::Release() {
    std::wstring url = std::move(baseUrl_);
    baseUrl_.clear();
    if (url.empty())
        return;
    PostJson(url, L"/remove_game", std::format(R"({{"game":"{}"}})", kGame), kReleaseTimeoutMs);
    LogInfo(L"GameSense game removed; SteelSeries lighting restored");
}
}  // namespace pc
