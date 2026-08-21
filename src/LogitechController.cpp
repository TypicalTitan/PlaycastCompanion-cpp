#include "pch.h"
#include "LogitechController.h"

#include "Log.h"
#include "NativeResolver.h"

namespace pc {

namespace {
template <typename Fn>
void ResolveExport(HMODULE module, const char* name, Fn& slot, bool& dllMissing) {
    const FARPROC proc = GetProcAddress(module, name);
    if (!proc) {
        dllMissing = true;
        throw std::runtime_error(std::string("LED SDK DLL export missing: ") + name);
    }
    slot = reinterpret_cast<Fn>(proc);
}
}  // namespace

LogitechController::LogitechController(const LogitechConfig& cfg) : cfg_(cfg) {}

bool LogitechController::Enabled() const { return cfg_.Enabled; }

std::wstring LogitechController::UnavailableText(const std::exception&) const {
    return dllMissing_ ? L"LED SDK DLL not found — see the note on this card"
                       : L"G HUB not reachable — retrying";
}

void LogitechController::EnsureSdk() {
    if (sdkResolved_)
        return;
    const HMODULE module = native::LoadVendorLibrary(L"LOGI_LED");
    if (!module) {
        dllMissing_ = true;
        throw std::runtime_error("LED SDK DLL not found");
    }
    ResolveExport(module, "LogiLedInit", logiLedInit_, dllMissing_);
    ResolveExport(module, "LogiLedSaveCurrentLighting", logiLedSaveCurrentLighting_, dllMissing_);
    ResolveExport(module, "LogiLedSetLighting", logiLedSetLighting_, dllMissing_);
    ResolveExport(module, "LogiLedRestoreLighting", logiLedRestoreLighting_, dllMissing_);
    ResolveExport(module, "LogiLedShutdown", logiLedShutdown_, dllMissing_);
    sdkResolved_ = true;
    dllMissing_ = false;
}

void LogitechController::ApplyTick(std::stop_token stop) {
    EnsureSdk();
    if (!initialized_) {
        if (!logiLedInit_())
            throw std::runtime_error("LogiLedInit failed (is G HUB running?)");
        // The SDK wants a beat after init. Interruptible so Stop() stays snappy;
        // the init already happened, so record it either way for Release().
        const bool completed = SleepFor(stop, std::chrono::milliseconds(150));
        initialized_ = true;
        if (!completed)
            return;
    }
    if (!saved_) {
        logiLedSaveCurrentLighting_();
        saved_ = true;
    }
    if (!logiLedSetLighting_(0, 0, 0))
        throw std::runtime_error("LogiLedSetLighting failed");
}

void LogitechController::Release() {
    try {
        if (initialized_) {
            if (saved_)
                logiLedRestoreLighting_();
            logiLedShutdown_();
            LogInfo(L"Logitech lighting restored");
        }
    } catch (...) {
    }
    initialized_ = false;
    saved_ = false;
}

void LogitechController::OnConnectionLost() {
    try {
        if (initialized_)
            logiLedShutdown_();
    } catch (...) {
    }
    initialized_ = false;
    saved_ = false;
}

}  // namespace pc
