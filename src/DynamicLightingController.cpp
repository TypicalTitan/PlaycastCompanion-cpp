#include "pch.h"
#include "DynamicLightingController.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Lights.h>
#include <winrt/Windows.UI.h>

#include "Json.h"
#include "Log.h"

namespace pc {
namespace {

using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::DeviceInformationCollection;
using winrt::Windows::Devices::Lights::LampArray;
using winrt::Windows::Devices::Lights::LampArrayKind;
using winrt::Windows::UI::Color;

// USB vendor IDs of the engines that own their own devices.
constexpr uint16_t kVidRazer = 0x1532;
constexpr uint16_t kVidCorsair = 0x1B1C;
constexpr uint16_t kVidLogitech = 0x046D;
constexpr uint16_t kVidSteelSeries = 0x1038;

constexpr Color kBlack{255, 0, 0, 0};
constexpr Color kWhite{255, 255, 255, 255};

/// Balances the apartment we opened on this thread when the thread ends.
struct ApartmentGuard {
    bool owned = false;
    ~ApartmentGuard() {
        if (owned)
            winrt::uninit_apartment();
    }
};

/// Join the MTA once per thread. A thread that already has an apartment of
/// another kind (RPC_E_CHANGED_MODE) is left as it is.
void EnsureApartment() {
    thread_local bool initialized = false;
    thread_local ApartmentGuard guard;
    if (initialized)
        return;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        guard.owned = true;
    } catch (const winrt::hresult_error& e) {
        if (e.code() != static_cast<winrt::hresult>(RPC_E_CHANGED_MODE))
            throw;
    }
    initialized = true;
}

using winrt::Windows::Foundation::AsyncStatus;

// Upper bounds on the WinRT waits. Both normally finish within milliseconds,
// but a LampArray open has been observed to never complete (vendor driver busy
// with the device); an unbounded .get() would then wedge the worker thread and
// every join on it (StopBlackout, shutdown).
constexpr std::chrono::seconds kEnumerateTimeout{10};
constexpr std::chrono::seconds kOpenTimeout{5};

std::wstring HresultText(const winrt::hresult_error& e) {
    const std::wstring_view message(e.message());
    return std::format(L"hresult 0x{:08X}: {}", static_cast<uint32_t>(e.code()), message);
}

std::wstring KindName(LampArrayKind kind) {
    switch (kind) {
        case LampArrayKind::Undefined: return L"Undefined";
        case LampArrayKind::Keyboard: return L"Keyboard";
        case LampArrayKind::Mouse: return L"Mouse";
        case LampArrayKind::GameController: return L"GameController";
        case LampArrayKind::Peripheral: return L"Peripheral";
        case LampArrayKind::Scene: return L"Scene";
        case LampArrayKind::Notification: return L"Notification";
        case LampArrayKind::Chassis: return L"Chassis";
        case LampArrayKind::Wearable: return L"Wearable";
        case LampArrayKind::Furniture: return L"Furniture";
        case LampArrayKind::Art: return L"Art";
        case LampArrayKind::Headset: return L"Headset";
        case LampArrayKind::Microphone: return L"Microphone";
        case LampArrayKind::Speaker: return L"Speaker";
    }
    return std::to_wstring(static_cast<int32_t>(kind));
}

std::wstring BoolText(bool value) { return value ? L"True" : L"False"; }

std::wstring Trunc(std::wstring_view id) {
    return id.size() > 60 ? std::wstring(id.substr(0, 57)) + L"..." : std::wstring(id);
}

std::wstring LocalClock() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return std::format(L"{:02}:{:02}:{:02}", st.wHour, st.wMinute, st.wSecond);
}

std::wstring CurrentUserName() {
    std::array<wchar_t, 257> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    if (GetUserNameW(buffer.data(), &size) && size > 0)
        return std::wstring(buffer.data(), size - 1);
    return L"?";
}

void WriteReport(const std::wstring& outPath, const std::wstring& text) {
    if (outPath.empty())
        return;
    const std::string utf8 = json::WideToUtf8(text);
    const UniqueHandle file(CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid())
        return;
    DWORD written = 0;
    WriteFile(file.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

/// Body of the --lamps harness; runs on a dedicated MTA thread. Appends lines
/// through `line`, never throws.
void EnumerateLamps(const std::function<void(const std::wstring&)>& line) {
    try {
        EnsureApartment();
        WinRtOperationGate operations;
        const DeviceInformationCollection devices = operations.Invoke(
            [] { return DeviceInformation::FindAllAsync(LampArray::GetDeviceSelector()); }, kEnumerateTimeout);
        line(std::format(L"selector matched {} device(s)", devices.Size()));

        int idx = 0;
        for (const DeviceInformation& info : devices) {
            ++idx;
            line(std::format(L"[{}] name='{}' enabled={} id={}", idx, std::wstring_view(info.Name()),
                             BoolText(info.IsEnabled()), Trunc(info.Id())));
            try {
                const LampArray lamp = operations.Invoke(
                    [&info] { return LampArray::FromIdAsync(info.Id()); }, kOpenTimeout);
                if (!lamp) {
                    line(L"     FromIdAsync returned null (device not openable by us)");
                    continue;
                }
                line(std::format(L"     kind={} lamps={} connected={} vid=0x{:04X} pid=0x{:04X}",
                                 KindName(lamp.LampArrayKind()), lamp.LampCount(), BoolText(lamp.IsConnected()),
                                 lamp.HardwareVendorId(), lamp.HardwareProductId()));

                // Can we actually paint it? Try black, then restore to white so
                // nothing is left dark by the probe.
                try {
                    lamp.SetColor(kBlack);
                    Sleep(400);
                    lamp.SetColor(kWhite);
                    line(L"     SetColor OK — we CAN drive this device");
                } catch (const winrt::hresult_error& e) {
                    line(std::format(L"     SetColor FAILED ({}) — likely owned by vendor app", HresultText(e)));
                } catch (const std::exception& e) {
                    line(std::format(L"     SetColor FAILED (exception: {}) — likely owned by vendor app",
                                     json::Utf8ToWide(e.what())));
                }
            } catch (const winrt::hresult_error& e) {
                line(std::format(L"     open failed ({})", HresultText(e)));
            } catch (const std::exception& e) {
                line(std::format(L"     open failed (exception: {})", json::Utf8ToWide(e.what())));
            }
        }
        if (devices.Size() == 0)
            line(L"No LampArray devices — either no LampArray-capable hardware, or Windows Dynamic Lighting is OFF / a vendor app owns every device.");
    } catch (const winrt::hresult_error& e) {
        line(std::format(L"enumeration error: {}", HresultText(e)));
    } catch (const std::exception& e) {
        line(std::format(L"enumeration error: exception: {}", json::Utf8ToWide(e.what())));
    } catch (...) {
        line(L"enumeration error: unknown error");
    }
}

}  // namespace

DynamicLightingController::DynamicLightingController(const AppConfig& cfg) : cfg_(cfg) {}

bool DynamicLightingController::Enabled() const { return cfg_.DynamicLighting.Enabled; }

std::wstring DynamicLightingController::HoldingStatus() const {
    const std::lock_guard<std::mutex> lock(statusMutex_);
    return holdingStatus_;
}

std::wstring DynamicLightingController::UnavailableText(const std::exception& ex) const {
    return std::string_view(ex.what()).find("no LampArray") != std::string_view::npos
               ? L"No LampArray devices found — retrying"
               : L"Dynamic Lighting error — retrying";
}

void DynamicLightingController::ApplyTick(std::stop_token stop) {
    // Vendor IDs whose devices an enabled dedicated engine already drives.
    std::vector<uint16_t> excluded;
    if (cfg_.DynamicLighting.ExcludeVendorOwnedDevices) {
        if (cfg_.RazerEnabled) excluded.push_back(kVidRazer);
        if (cfg_.Corsair.Enabled) excluded.push_back(kVidCorsair);
        if (cfg_.Logitech.Enabled) excluded.push_back(kVidLogitech);
        if (cfg_.SteelSeries.Enabled) excluded.push_back(kVidSteelSeries);
    }

    int painted = 0;
    int skipped = 0;
    try {
        EnsureApartment();
        if (stop.stop_requested())
            return;
        const DeviceInformationCollection devices = operations_.Invoke(
            [] { return DeviceInformation::FindAllAsync(LampArray::GetDeviceSelector()); }, kEnumerateTimeout, stop);
        if (stop.stop_requested())
            return;
        if (devices.Size() == 0)
            throw std::runtime_error("no LampArray devices connected");

        for (const DeviceInformation& info : devices) {
            if (stop.stop_requested())
                return;
            try {
                const LampArray lamp = operations_.Invoke(
                    [&info] { return LampArray::FromIdAsync(info.Id()); }, kOpenTimeout, stop);
                if (stop.stop_requested())
                    return;
                if (!lamp || !lamp.IsConnected())
                    continue;
                if (std::find(excluded.begin(), excluded.end(), lamp.HardwareVendorId()) != excluded.end()) {
                    ++skipped;
                    continue;  // its own engine handles this device
                }
                if (stop.stop_requested())
                    return;
                lamp.SetColor(kBlack);
                ++painted;
            } catch (...) {
                // a single flaky device shouldn't stop the rest
            }
        }
    } catch (const winrt::hresult_error& e) {
        // hresult_error is not a std::exception; the base only understands those.
        throw std::runtime_error(json::WideToUtf8(HresultText(e)));
    }

    if (painted > 0) {
        SetHoldingStatus(skipped > 0
                             ? std::format(L"Holding {} device(s); {} left to their vendor engine", painted, skipped)
                             : L"Holding blackout");
        LogSummary(std::format(L"painting {} device(s), yielding {} to their vendor engine", painted, skipped));
        return;
    }

    // Nothing painted. If we deliberately skipped everything, that's the
    // intended coexistence outcome, not an error — report it calmly and
    // keep holding (a no-op) so the toggle stays on without spamming retries.
    if (skipped > 0) {
        SetHoldingStatus(std::format(L"Idle — all {} device(s) handled by their vendor engine", skipped));
        LogSummary(std::format(L"yielding all {} device(s) to their vendor engine (nothing else to paint)", skipped));
        return;
    }

    // Devices exist but none were controllable (owned elsewhere / access denied).
    throw std::runtime_error("no controllable LampArray devices");
}

void DynamicLightingController::Release() {
    // No explicit restore in the LampArray API — Windows Dynamic Lighting
    // or the vendor software repaints on its next update.
    {
        const std::lock_guard<std::mutex> lock(statusMutex_);
        lastSummary_.clear();
    }
    LogInfo(L"Dynamic Lighting hold ended (devices repaint on the next OS/vendor update)");
}

void DynamicLightingController::SetHoldingStatus(std::wstring status) {
    const std::lock_guard<std::mutex> lock(statusMutex_);
    holdingStatus_ = std::move(status);
}

void DynamicLightingController::LogSummary(const std::wstring& summary) {
    {
        const std::lock_guard<std::mutex> lock(statusMutex_);
        if (summary == lastSummary_)
            return;
        lastSummary_ = summary;
    }
    LogInfo(L"Dynamic Lighting: " + summary);
}

void DynamicLightingController::RunLampDiagnostics(const std::wstring& outPath) {
    std::wstring report;
    const auto line = [&report](const std::wstring& s) {
        report += s;
        report += L"\r\n";
        LogInfo(L"lamps: " + s);
    };

    line(std::format(L"LampArray enumeration @ {} (user {})", LocalClock(), CurrentUserName()));
    // A dedicated thread keeps the blocking .get() waits off whatever apartment
    // the caller (usually the STA main thread) lives in.
    std::jthread worker([&line] { EnumerateLamps(line); });
    worker.join();

    WriteReport(outPath, report);
}

}  // namespace pc
