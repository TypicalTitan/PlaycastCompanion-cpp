#pragma once
// SteelSeries GameSense blackout: address from %ProgramData%\SteelSeries\
// SteelSeries Engine 3\coreProps.json; POST /game_metadata, then one
// /bind_game_event per device type (keyboard, mouse, headset, mousepad,
// indicator) with a color handler {zone:"all", mode:"color", color:black} —
// bound independently so an unsupported zone can't sink the rest; each tick
// POSTs /game_event value 1 per bound event + /game_heartbeat; Release POSTs
// /remove_game.
#include "Config.h"
#include "LightingBackend.h"
#include <vector>

namespace pc {
class SteelSeriesController final : public LightingBackendBase {
public:
    explicit SteelSeriesController(const SteelSeriesConfig& cfg);
    std::wstring DisplayName() const override { return L"SteelSeries GameSense"; }
    bool Enabled() const override;

protected:
    std::wstring UnavailableText(const std::exception&) const override;
    void ApplyTick(std::stop_token stop) override;
    void Release() override;
    void OnConnectionLost() override;

private:  // module owner may extend
    const SteelSeriesConfig& cfg_;
    std::wstring baseUrl_;
    std::vector<std::string> events_;
    /// Returns early (without setting baseUrl_) if `stop` is requested mid-binding.
    void RegisterGame(std::stop_token stop);
};
}  // namespace pc
