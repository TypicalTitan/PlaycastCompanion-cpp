#pragma once
// Corsair blackout via the CUE SDK v3 DLL resolved at runtime by
// pc::native::LoadVendorLibrary(L"CUESDK") (never redistributed). cdecl API:
// CorsairProtocolDetails CorsairPerformProtocolHandshake() (struct: const char*
// sdkVersion; const char* serverVersion; int sdkProtocolVersion; int
// serverProtocolVersion; bool breakingChanges) — serverProtocolVersion==0 means
// not connected; int CorsairGetLastError(); int CorsairGetDeviceCount();
// CorsairLedPositions* CorsairGetLedPositionsByDeviceIndex(int) (struct {int
// numberOfLed; CorsairLedPosition* pLedPosition} / {int ledId; double top,left,
// height,width}); bool CorsairSetLedsColorsBufferByDeviceIndex(int, int size,
// CorsairLedColor*) ({int ledId,r,g,b}); bool CorsairSetLedsColorsFlushBuffer();
// bool CorsairRequestControl(int accessMode=1 exclusive); bool
// CorsairReleaseControl(int). Handshake + RequestControl once per hold; paint
// every device black each tick; Release -> ReleaseControl.
#include "Config.h"
#include "LightingBackend.h"

namespace pc {
class CorsairController final : public LightingBackendBase {
public:
    explicit CorsairController(const CorsairConfig& cfg);
    std::wstring DisplayName() const override { return L"Corsair iCUE"; }
    bool Enabled() const override;

protected:
    std::wstring UnavailableText(const std::exception&) const override;
    void ApplyTick(std::stop_token stop) override;
    void Release() override;
    void OnConnectionLost() override;

private:  // module owner may extend
    const CorsairConfig& cfg_;
    bool connected_ = false;
    bool dllMissing_ = false;

    // SDK structs (defined in CorsairController.cpp with natural alignment).
    struct ProtocolDetails;
    struct LedPositions;
    struct LedColor;

    // SDK exports (cdecl; bool is a 1-byte return, matching MSVC's bool).
    using HandshakeFn = ProtocolDetails(__cdecl*)();
    using IntFn = int(__cdecl*)();
    using LedPositionsFn = LedPositions*(__cdecl*)(int);
    using SetColorsFn = bool(__cdecl*)(int, int, LedColor*);
    using BoolFn = bool(__cdecl*)();
    using ControlFn = bool(__cdecl*)(int);

    /// Resolve the DLL and its exports on first use (mirrors the lazy
    /// DllImport binding of the C# app). Sets dllMissing_ and throws when the
    /// DLL or any export is absent; retried on the next tick.
    void EnsureSdk();

    bool sdkResolved_ = false;
    HandshakeFn corsairPerformProtocolHandshake_ = nullptr;
    IntFn corsairGetLastError_ = nullptr;
    IntFn corsairGetDeviceCount_ = nullptr;
    LedPositionsFn corsairGetLedPositionsByDeviceIndex_ = nullptr;
    SetColorsFn corsairSetLedsColorsBufferByDeviceIndex_ = nullptr;
    BoolFn corsairSetLedsColorsFlushBuffer_ = nullptr;
    ControlFn corsairRequestControl_ = nullptr;
    ControlFn corsairReleaseControl_ = nullptr;
};
}  // namespace pc
