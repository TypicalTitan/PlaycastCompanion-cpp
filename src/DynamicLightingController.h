#pragma once
// Windows Dynamic Lighting blackout via WinRT LampArray
// (winrt::Windows::Devices::Lights). Each tick: DeviceInformation::FindAllAsync(
// LampArray::GetDeviceSelector()), LampArray::FromIdAsync(id), SetColor(black).
// Coexistence: when ExcludeVendorOwnedDevices, skip devices whose
// HardwareVendorId belongs to an ENABLED vendor engine (Razer 0x1532, Corsair
// 0x1B1C, Logitech 0x046D, SteelSeries 0x1038) so two engines never fight over
// the same hardware. "All skipped" is a calm idle status, not an error. No
// explicit restore exists in the API; Release just logs. Worker threads must
// winrt::init_apartment(multi_threaded) before any WinRT call.
#include "Config.h"
#include "LightingBackend.h"

namespace pc {
class DynamicLightingController final : public LightingBackendBase {
public:
    explicit DynamicLightingController(const AppConfig& cfg);
    std::wstring DisplayName() const override { return L"Windows Dynamic Lighting"; }
    bool Enabled() const override;

    /// Dev harness (--lamps): enumerate LampArray devices (name, kind, lamp
    /// count, VID/PID, connected, and whether SetColor works — repainting to
    /// white afterwards) into outPath and the log.
    static void RunLampDiagnostics(const std::wstring& outPath);

protected:
    std::chrono::seconds Tick() const override { return std::chrono::seconds(10); }
    std::wstring HoldingStatus() const override;
    std::wstring UnavailableText(const std::exception&) const override;
    void ApplyTick(std::stop_token stop) override;
    void Release() override;

private:  // module owner may extend
    const AppConfig& cfg_;
    mutable std::mutex statusMutex_;
    std::wstring holdingStatus_ = L"Holding blackout";
    std::wstring lastSummary_;

    void SetHoldingStatus(std::wstring status);
    /// Log "Dynamic Lighting: <summary>" once per change (dedupes repeats).
    void LogSummary(const std::wstring& summary);
};
}  // namespace pc
