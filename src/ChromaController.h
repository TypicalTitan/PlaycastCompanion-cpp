#pragma once
// Razer Chroma blackout via the local REST API (POST ChromaInitUrl to open a
// session, PUT {uri}/{device} CHROMA_STATIC black to keyboard/mouse/headset/
// mousepad/keypad/chromalink, PUT {uri}/heartbeat each tick, DELETE {uri} to
// release so Synapse's profile returns).
#include "Config.h"
#include "LightingBackend.h"
#include "Http.h"

namespace pc {
class ChromaController final : public LightingBackendBase {
public:
    using Request = std::function<http::Response(const std::string&, const std::wstring&,
                                                const std::string&, unsigned)>;
    explicit ChromaController(const AppConfig& cfg, Request request = {});
    std::wstring DisplayName() const override { return L"Razer Chroma"; }
    bool Enabled() const override;

protected:
    std::chrono::seconds Tick() const override;
    std::chrono::seconds RetryDelay() const override;
    std::wstring UnavailableText(const std::exception&) const override;
    void ApplyTick(std::stop_token stop) override;
    void Release() override;
    std::wstring ReleasedStatus() const override { return releaseStatus_; }
    void OnConnectionLost() override;

private:  // module owner may extend
    const AppConfig& cfg_;
    std::wstring sessionUri_;
    std::wstring releaseStatus_ = L"Ready";
    Request request_;
    void Put(const std::wstring& url, const std::string& body);
    void InitSession();
    int ApplyEffects(std::stop_token stop);  // returns how many device PUTs succeeded
};
}  // namespace pc
