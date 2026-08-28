#include "pch.h"
#include "ChromaController.h"

#include "Http.h"
#include "Json.h"
#include "Log.h"

namespace pc {
namespace {
// Device types the Chroma REST API accepts as PUT targets under a session URI.
constexpr std::array<const wchar_t*, 6> kDevices = {
    L"keyboard", L"mouse", L"headset", L"mousepad", L"keypad", L"chromalink"};

constexpr const char* kBlackEffectJson = R"({"effect":"CHROMA_STATIC","param":{"color":0}})";

// Byte-for-byte what the C# JsonSerializer emits for the anonymous init object
// (property order is the declaration order there, so keep it fixed here too).
constexpr const char* kInitJson =
    R"({"title":"Playcast Companion",)"
    R"("description":"Turns all Razer lighting off while a guest session is active",)"
    R"("author":{"name":"personal","contact":"local"},)"
    R"("device_supported":["keyboard","mouse","headset","mousepad","keypad","chromalink"],)"
    R"("category":"application"})";

// Init can legitimately take several seconds while the SDK enumerates devices.
// Everything after init is a millisecond-scale loopback call, so it gets a much
// tighter budget: a wedged Synapse then holds a StopBlackout join for ~3 s
// instead of ~10 s per HTTP phase.
constexpr unsigned kInitTimeoutMs = 10000;
constexpr unsigned kEffectTimeoutMs = 3000;

void EnsureSuccess(const http::Response& resp, const char* method, const std::wstring& url) {
    if (http::IsSuccess(resp))
        return;
    throw std::runtime_error(std::format("Response status code does not indicate success: {} ({} {})",
                                         resp.status, method, json::WideToUtf8(url)));
}

void Put(const std::wstring& url, const std::string& body) {
    EnsureSuccess(http::Request("PUT", url, body, L"application/json", kEffectTimeoutMs), "PUT", url);
}
}  // namespace

ChromaController::ChromaController(const AppConfig& cfg) : cfg_(cfg) {}

bool ChromaController::Enabled() const { return cfg_.RazerEnabled; }

std::chrono::seconds ChromaController::Tick() const {
    return std::chrono::seconds(std::clamp(cfg_.TickSeconds, 1, 10));
}

std::chrono::seconds ChromaController::RetryDelay() const {
    return std::chrono::seconds(std::max(5, cfg_.RetryInitSeconds));
}

std::wstring ChromaController::UnavailableText(const std::exception&) const {
    return L"Unreachable — retrying (is Razer Synapse running?)";
}

void ChromaController::OnConnectionLost() { sessionUri_.clear(); }

void ChromaController::ApplyTick(std::stop_token stop) {
    int applied = 0;
    if (sessionUri_.empty()) {
        InitSession();
        if (stop.stop_requested())
            return;
        // First black frame right away — a responsive SDK honours it at once,
        // so the room goes dark without waiting out the settle sleep below.
        applied += ApplyEffects(stop);
        if (stop.stop_requested())
            return;
        // Let the SDK finish registering the app, then re-assert: frames sent
        // before registration completes are sometimes silently dropped.
        if (!SleepFor(stop, std::chrono::milliseconds(750)))
            return;
    } else {
        Put(sessionUri_ + L"/heartbeat", "");
    }
    applied += ApplyEffects(stop);
    // A tick where every device PUT failed is not a hold — treat it as an
    // outage so the base loop's "engaged" log and retry backoff stay truthful.
    if (applied == 0 && !stop.stop_requested())
        throw std::runtime_error("Chroma accepted the session but no device took the effect");
}

int ChromaController::ApplyEffects(std::stop_token stop) {
    int applied = 0;
    for (const wchar_t* device : kDevices) {
        try {
            Put(sessionUri_ + L"/" + device, kBlackEffectJson);
            ++applied;
        } catch (const std::exception&) {
            // device types the user doesn't own can fail (and a per-device
            // timeout is retried next tick anyway); that's fine
        }
        if (stop.stop_requested())
            break;
    }
    return applied;
}

void ChromaController::InitSession() {
    const http::Response resp =
        http::Request("POST", cfg_.ChromaInitUrl, kInitJson, L"application/json", kInitTimeoutMs);
    EnsureSuccess(resp, "POST", cfg_.ChromaInitUrl);
    const json::JsonObject doc = json::Parse(resp.body);
    std::wstring uri = json::GetString(doc, L"uri");
    if (uri.empty())
        throw std::runtime_error("Chroma init response had no uri");
    sessionUri_ = std::move(uri);
    LogInfo(L"Chroma session opened: " + sessionUri_);
}

void ChromaController::Release() {
    std::wstring uri = std::move(sessionUri_);
    sessionUri_.clear();
    if (uri.empty())
        return;
    try {
        const http::Response resp = http::Request("DELETE", uri, {}, L"application/json", kEffectTimeoutMs);
        LogInfo(std::format(L"Chroma session released (HTTP {}); Synapse lighting restored", resp.status));
    } catch (const std::exception& ex) {
        LogInfo(L"Chroma release failed (" + json::Utf8ToWide(ex.what()) +
                L"); the session will time out on its own in ~15 s");
    }
}
}  // namespace pc
