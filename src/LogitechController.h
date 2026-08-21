#pragma once
// Logitech blackout via the LED Illumination SDK DLL resolved at runtime by
// pc::native::LoadVendorLibrary(L"LOGI_LED") (never redistributed). Functions
// (cdecl, bool = 1 byte): LogiLedInit, LogiLedSaveCurrentLighting,
// LogiLedSetLighting(int r,int g,int b percent), LogiLedRestoreLighting,
// LogiLedShutdown. Init once, save once per hold, set 0,0,0 each tick; Release
// restores then shuts down.
#include "Config.h"
#include "LightingBackend.h"

namespace pc {
class LogitechController final : public LightingBackendBase {
public:
    explicit LogitechController(const LogitechConfig& cfg);
    std::wstring DisplayName() const override { return L"Logitech G"; }
    bool Enabled() const override;

protected:
    std::wstring UnavailableText(const std::exception&) const override;
    void ApplyTick(std::stop_token stop) override;
    void Release() override;
    void OnConnectionLost() override;

private:  // module owner may extend
    const LogitechConfig& cfg_;
    bool initialized_ = false;
    bool saved_ = false;
    bool dllMissing_ = false;

    // SDK exports (cdecl; bool is a 1-byte return, matching MSVC's bool).
    using BoolFn = bool(__cdecl*)();
    using SetLightingFn = bool(__cdecl*)(int, int, int);
    using VoidFn = void(__cdecl*)();

    /// Resolve the DLL and its exports on first use (mirrors the lazy
    /// DllImport binding of the C# app). Sets dllMissing_ and throws when the
    /// DLL or any export is absent; retried on the next tick.
    void EnsureSdk();

    bool sdkResolved_ = false;
    BoolFn logiLedInit_ = nullptr;
    BoolFn logiLedSaveCurrentLighting_ = nullptr;
    SetLightingFn logiLedSetLighting_ = nullptr;
    BoolFn logiLedRestoreLighting_ = nullptr;
    VoidFn logiLedShutdown_ = nullptr;
};
}  // namespace pc
