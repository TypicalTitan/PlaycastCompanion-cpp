#include "pch.h"
#include "CorsairController.h"

#include "Log.h"
#include "NativeResolver.h"

namespace pc {

// CUE SDK v3 structs, natural (MSVC x64) alignment — the DLL is an MSVC build.
struct CorsairController::ProtocolDetails {
    const char* sdkVersion;
    const char* serverVersion;
    int sdkProtocolVersion;
    int serverProtocolVersion;
    bool breakingChanges;
};

namespace {
struct LedPosition {
    int ledId;
    double top;
    double left;
    double height;
    double width;
};
static_assert(sizeof(LedPosition) == 40, "CorsairLedPosition layout");

constexpr int kExclusiveLightingControl = 1;  // CAM_ExclusiveLightingControl

template <typename Fn>
void ResolveExport(HMODULE module, const char* name, Fn& slot, bool& dllMissing) {
    const FARPROC proc = GetProcAddress(module, name);
    if (!proc) {
        dllMissing = true;
        throw std::runtime_error(std::string("CUE SDK DLL export missing: ") + name);
    }
    slot = reinterpret_cast<Fn>(proc);
}
}  // namespace

struct CorsairController::LedPositions {
    int numberOfLed;
    LedPosition* pLedPosition;
};

struct CorsairController::LedColor {
    int ledId;
    int r;
    int g;
    int b;
};

CorsairController::CorsairController(const CorsairConfig& cfg) : cfg_(cfg) {}

bool CorsairController::Enabled() const { return cfg_.Enabled; }

std::wstring CorsairController::UnavailableText(const std::exception&) const {
    return dllMissing_ ? L"CUE SDK DLL not found — see the note on this card"
                       : L"iCUE not reachable — retrying";
}

void CorsairController::EnsureSdk() {
    // Layout checks live here because the structs are private nested types.
    static_assert(sizeof(ProtocolDetails) == 32, "CorsairProtocolDetails layout");
    static_assert(sizeof(LedPositions) == 16, "CorsairLedPositions layout");
    static_assert(sizeof(LedColor) == 16, "CorsairLedColor layout");
    if (sdkResolved_)
        return;
    const HMODULE module = native::LoadVendorLibrary(L"CUESDK");
    if (!module) {
        dllMissing_ = true;
        throw std::runtime_error("CUE SDK DLL not found");
    }
    ResolveExport(module, "CorsairPerformProtocolHandshake", corsairPerformProtocolHandshake_, dllMissing_);
    ResolveExport(module, "CorsairGetLastError", corsairGetLastError_, dllMissing_);
    ResolveExport(module, "CorsairGetDeviceCount", corsairGetDeviceCount_, dllMissing_);
    ResolveExport(module, "CorsairGetLedPositionsByDeviceIndex", corsairGetLedPositionsByDeviceIndex_, dllMissing_);
    ResolveExport(module, "CorsairSetLedsColorsBufferByDeviceIndex", corsairSetLedsColorsBufferByDeviceIndex_, dllMissing_);
    ResolveExport(module, "CorsairSetLedsColorsFlushBuffer", corsairSetLedsColorsFlushBuffer_, dllMissing_);
    ResolveExport(module, "CorsairRequestControl", corsairRequestControl_, dllMissing_);
    ResolveExport(module, "CorsairReleaseControl", corsairReleaseControl_, dllMissing_);
    sdkResolved_ = true;
    dllMissing_ = false;
}

void CorsairController::ApplyTick(std::stop_token stop) {
    EnsureSdk();
    if (!connected_) {
        const ProtocolDetails details = corsairPerformProtocolHandshake_();
        if (details.serverProtocolVersion == 0) {
            throw std::runtime_error(
                std::format("handshake failed (is iCUE running? error {})", corsairGetLastError_()));
        }
        if (!corsairRequestControl_(kExclusiveLightingControl))
            throw std::runtime_error("iCUE refused exclusive lighting control");
        connected_ = true;
    }

    const int devices = corsairGetDeviceCount_();
    std::vector<LedColor> colors;
    for (int i = 0; i < devices; ++i) {
        if (stop.stop_requested())
            return;
        const LedPositions* positions = corsairGetLedPositionsByDeviceIndex_(i);
        if (!positions)
            continue;
        if (positions->numberOfLed <= 0 || !positions->pLedPosition)
            continue;
        const size_t count = static_cast<size_t>(positions->numberOfLed);
        colors.assign(count, LedColor{});  // r,g,b = 0 -> black
        for (size_t led = 0; led < count; ++led)
            colors[led].ledId = positions->pLedPosition[led].ledId;
        corsairSetLedsColorsBufferByDeviceIndex_(i, positions->numberOfLed, colors.data());
    }
    corsairSetLedsColorsFlushBuffer_();
}

void CorsairController::Release() {
    if (connected_) {
        try {
            corsairReleaseControl_(kExclusiveLightingControl);
            LogInfo(L"Corsair lighting control released; iCUE profile restored");
        } catch (...) {
        }
        connected_ = false;
    }
}

void CorsairController::OnConnectionLost() { connected_ = false; }

}  // namespace pc
