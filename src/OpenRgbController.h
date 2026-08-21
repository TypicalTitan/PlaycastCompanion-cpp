#pragma once
// OpenRGB blackout via its SDK server TCP protocol ("ORGB" magic, 16-byte
// header: magic[4], device u32, command u32, length u32, little-endian):
// negotiate REQUEST_PROTOCOL_VERSION(40) >= 2, SET_CLIENT_NAME(50), then on the
// first tick of a hold REQUEST_SAVE_PROFILE(151) "playcast-restore" and
// REQUEST_LOAD_PROFILE(152) <BlackoutProfile>; Release loads the restore
// profile and REQUEST_DELETE_PROFILE(153)s it. Never re-save after the first
// successful save within one hold (a failed tick could overwrite the restore
// point with black). Profile name payloads are NUL-terminated ASCII.
#include "Config.h"
#include "LightingBackend.h"

namespace pc {
class OpenRgbController final : public LightingBackendBase {
public:
    explicit OpenRgbController(const OpenRgbConfig& cfg);
    std::wstring DisplayName() const override { return L"OpenRGB"; }
    bool Enabled() const override;

protected:
    std::chrono::seconds Tick() const override { return std::chrono::seconds(30); }
    std::wstring UnavailableText(const std::exception&) const override;
    void ApplyTick(std::stop_token stop) override;
    void Release() override;

private:  // module owner may extend
    const OpenRgbConfig& cfg_;
    bool saved_ = false;
};
}  // namespace pc
