#include "pch.h"
#include "TrayAppInternal.h"

namespace pc {
void TrayApp::Impl::RequestEvaluate(std::wstring reason) {
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (exiting.load())
            return;
        queue.push_back(std::move(reason));
    }
    queueCv.notify_one();
}

void TrayApp::Impl::EvalLoop(std::stop_token stop) {
    MtaScope apartment;
    for (;;) {
        if (stop.stop_requested())
            break;
        std::wstring reason;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            if (!queueCv.wait(lock, stop, [this] { return !queue.empty(); }))
                break;
            reason = std::move(queue.front());
            queue.pop_front();
        }
        Evaluate(reason, stop);
    }
}

/// Quiesce every worker that reads `cfg` by reference, hand the swap to the UI
/// thread and wait for it. Returns false when shutdown interrupted the wait.
bool TrayApp::Impl::ApplySettings(std::stop_token stop) {
    std::vector<ILightingBackend*> all;
    all.reserve(lighting.size());
    for (const auto& backend : lighting)
        all.push_back(backend.get());
    StopBackendsParallel(all);  // each StopBlackout joins its worker
    if (discord) {
        try {
            discord->Clear();  // joins the presence worker
        } catch (const std::exception& ex) {
            LogInfo(L"discord clear failed: " + Describe(ex));
        } catch (...) {
            LogInfo(L"discord clear failed");
        }
    }
    PublishBackendStatus();
    if (sessionLogger) sessionLogger->Stop();
    if (channel) {
        try {
            channel->Stop();  // its listener thread reads TargetUsername/ShimPipeName
        } catch (...) {
            LogInfo(L"channel listener stop failed");
        }
        channel.reset();
    }

    {
        std::unique_lock<std::mutex> lock(settingsMutex);
        settingsApplied = false;
        if (!PostMessageW(Hwnd(), WM_PC_APPLY_SETTINGS, 0, 0)) {
            LogInfo(std::format(L"settings apply failed: PostMessage error {}", GetLastError()));
            return false;
        }
        // stop_token-aware so Shutdown()'s request_stop + join cannot deadlock.
        if (!settingsCv.wait(lock, stop, [this] { return settingsApplied; }))
            return false;
    }

    if (cfg.Discord.GamePassthroughEnabled && discord && cfg.Discord.Enabled && !g_harnessMode.load()) {
        channel = std::make_unique<DiscordChannelListener>(cfg, *discord);
        channel->Start();
    }
    return true;
}

/// TestGuestModeAsync body, run on the eval thread so the UI thread can never
/// start a worker between a settings quiesce and the config swap.
void TrayApp::Impl::RunPreview() {
    LogInfo(L"guest-mode preview started");
    previewArmed.store(true);
    if (!g_harnessMode.load()) {
        for (const auto& backend : lighting) {
            try {
                backend->StartBlackout();  // no-ops when the backend is disabled
            } catch (const std::exception& ex) {
                LogInfo(std::format(L"{} preview start failed: {}", backend->DisplayName(), Describe(ex)));
            }
        }
        if (discord)
            discord->SetActive();
    }
    // The 10 s wait lives on the UI thread's timer so repeated clicks just
    // extend it; it ends with a normal re-evaluation.
    PostMessageW(Hwnd(), WM_PC_PREVIEW, 0, 0);
}

void TrayApp::Impl::Evaluate(const std::wstring& reason, std::stop_token stop) {
    try {
        if (reason == L"preview") {
            RunPreview();
            return;
        }
        if (reason == L"settings" && !ApplySettings(stop))
            return;  // shutting down
        if (reason == L"preview end")
            previewArmed.store(false);
        const bool harness = g_harnessMode.load();
        const bool preview = previewArmed.load();
        const bool active = (sessions && sessions->IsTargetUserLoggedOn()) || (registry && registry->IsActive());
        if (harness) {
            // --snapshot: only the state the UI renders, never the side effects.
        } else if (preview && !active) {
            // Mid-preview poll/session event with no real guest: leave the
            // preview's blackout and presence alone until "preview end".
            PublishBackendStatus();
            if (sessionLogger) sessionLogger->UpdateSession(false, reason, L"");
            return;
        } else if (active) {
            if (!guestActive.load())
                LogInfo(std::format(L"guest session active ({}) -> stealth on", reason));
            for (const auto& backend : lighting) {
                const bool enabled = backend->Enabled();
                const bool holding = backend->IsHolding();
                if (enabled && !holding)
                    backend->StartBlackout();
                else if (!enabled && holding)
                    backend->StopBlackout();  // toggled off mid-hold
            }
        } else {
            std::vector<ILightingBackend*> holding;
            for (const auto& backend : lighting) {
                if (backend->IsHolding())
                    holding.push_back(backend.get());
            }
            if (!holding.empty()) {
                LogInfo(std::format(L"guest session gone ({}) -> restoring {} lighting system(s)", reason,
                                    holding.size()));
                StopBackendsParallel(holding);
            }
        }
        if (discord && !harness) {
            if (active)
                discord->SetActive();
            else
                discord->Clear();
        }
        PublishBackendStatus();
        if (sessionLogger)
            sessionLogger->UpdateSession(active, reason, sessions ? sessions->TargetSessionIdentity() : L"registry-only");
        guestActive.store(active);
        PostMessageW(Hwnd(), WM_PC_EVALUATED, active ? 1 : 0, 0);
    } catch (const std::exception& ex) {
        LogInfo(L"evaluate failed: " + Describe(ex));
    } catch (const winrt::hresult_error& e) {
        LogInfo(L"evaluate failed: " + std::wstring(e.message()));
    } catch (...) {
        LogInfo(L"evaluate failed: unknown error");
    }
}

void TrayApp::Impl::PublishBackendStatus() {
    if (!sessionLogger) return;
    using namespace sessionlog;
    Array backends;
    for (const auto& backend : lighting) backends.Append(Make({{L"name", Text(backend->DisplayName())},
        {L"enabled", Boolean(backend->Enabled())}, {L"holding", Boolean(backend->IsHolding())},
        {L"status", Text(backend->StatusText())}}));
    sessionLogger->UpdateBackends(json::Utf8ToWide(json::Stringify(Make({{L"lighting", backends},
        {L"discord", Text(discord ? discord->StatusText() : L"Unavailable in this Windows session")}}))));
}

void TrayApp::Impl::SignalLoop() {
    const HANDLE handles[2] = {showSettingsEvent, stopEvent.get()};
    for (;;) {
        const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (result != WAIT_OBJECT_0)
            break;  // stop event, abandoned handle or failure: shut the thread down
        if (!headless && !exiting.load())
            PostMessageW(Hwnd(), WM_PC_OPEN_SETTINGS, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------


}
