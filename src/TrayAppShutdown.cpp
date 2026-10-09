#include "pch.h"
#include "TrayAppInternal.h"

namespace pc {
void TrayApp::Impl::Shutdown() {
    if (shutDown)
        return;
    shutDown = true;
    exiting.store(true);

    if (Hwnd() != nullptr) {
        KillTimer(Hwnd(), kPollTimerId);
        KillTimer(Hwnd(), kPreviewTimerId);
    }
    if (window && window->IsAlive())
        ShowWindow(window->Handle(), SW_HIDE);
    RemoveTrayIcon();

    // Stop the evaluation worker so nothing races the teardown below.
    if (evalThread.joinable()) {
        evalThread.request_stop();
        queueCv.notify_all();
        if (evalThread.get_id() != std::this_thread::get_id())
            evalThread.join();
    }
    if (stopEvent.valid())
        SetEvent(stopEvent.get());
    if (signalThread.joinable() && signalThread.get_id() != std::this_thread::get_id())
        signalThread.join();

    // Hand every lighting system back (bounded: each StopBlackout returns
    // within ~5 s and they run in parallel), then clear the presence.
    std::vector<ILightingBackend*> all;
    all.reserve(lighting.size());
    for (const auto& backend : lighting)
        all.push_back(backend.get());
    StopBackendsParallel(all);

    if (discord) {
        try {
            discord->Clear();
        } catch (const std::exception& ex) {
            LogInfo(L"discord clear failed: " + Describe(ex));
        } catch (...) {
            LogInfo(L"discord clear failed");
        }
    }
    PublishBackendStatus();
    if (sessionLogger) sessionLogger->Stop();
    if (shim) {
        try {
            shim->Stop();
        } catch (...) {
            LogInfo(L"shim stop failed");
        }
    }
    if (channel) {
        try {
            channel->Stop();
        } catch (...) {
            LogInfo(L"channel listener stop failed");
        }
    }
    LogInfo(L"--- exited ---");
}

// ---------------------------------------------------------------------------
// TrayApp facade
// ---------------------------------------------------------------------------


}
