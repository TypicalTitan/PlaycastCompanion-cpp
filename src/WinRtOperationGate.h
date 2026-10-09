#pragma once

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <winrt/Windows.Foundation.h>

namespace pc {
// Cancellation is asynchronous. Keep one unfinished operation across retries
// and session restarts instead of starting more work behind a stuck provider.
class WinRtOperationGate {
public:
    template <typename Factory>
    auto Invoke(Factory&& create, std::chrono::milliseconds timeout, std::stop_token stop = {}) {
        std::lock_guard lock(mutex_);
        if (stop.stop_requested()) throw std::runtime_error("LampArray operation cancelled");
        ReapPending();
        const auto operation = create();
        const auto info = operation.template as<winrt::Windows::Foundation::IAsyncInfo>();
        try {
            Wait(info, timeout, stop);
            if (stop.stop_requested()) throw std::runtime_error("LampArray operation cancelled");
            auto result = operation.GetResults();
            Close(info);
            return result;
        } catch (...) {
            if (Started(info)) {
                try { info.Cancel(); } catch (...) {}
                if (Started(info)) pending_ = info;
                else Close(info);
            } else Close(info);
            throw;
        }
    }

private:
    std::mutex mutex_;
    winrt::Windows::Foundation::IAsyncInfo pending_{nullptr};

    static bool Started(const winrt::Windows::Foundation::IAsyncInfo& info) noexcept {
        try { return info.Status() == winrt::Windows::Foundation::AsyncStatus::Started; }
        catch (...) { return true; } // Unqueryable completion must not permit overlapping work.
    }
    static void Close(const winrt::Windows::Foundation::IAsyncInfo& info) noexcept {
        try { info.Close(); } catch (...) {}
    }
    void ReapPending() {
        if (!pending_) return;
        if (Started(pending_))
            throw std::runtime_error("Previous LampArray operation is still cancelling; retry deferred");
        Close(pending_);
        pending_ = nullptr;
    }
    static void Wait(const winrt::Windows::Foundation::IAsyncInfo& info,
                     std::chrono::milliseconds timeout, std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (info.Status() == winrt::Windows::Foundation::AsyncStatus::Started) {
            if (stop.stop_requested()) throw std::runtime_error("LampArray operation cancelled");
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) throw std::runtime_error("LampArray operation timed out");
            std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(50)));
        }
    }
};
} // namespace pc
