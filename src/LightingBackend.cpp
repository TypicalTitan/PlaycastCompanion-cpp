#include "pch.h"
#include "LightingBackend.h"

#include "Json.h"
#include "Log.h"

namespace pc {
namespace {

std::wstring WhatToWide(const std::exception& ex) {
    const char* what = ex.what();
    return json::Utf8ToWide(what ? std::string_view(what) : std::string_view());
}

/// Initialises the MTA for the worker thread (derived ApplyTick/Release may use
/// WinRT) and tears it down on exit. Failure to initialise is not fatal: the
/// thread may simply already belong to an apartment.
struct ApartmentScope {
    bool initialised = false;
    ApartmentScope() {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialised = true;
        } catch (...) {
        }
    }
    ~ApartmentScope() {
        if (initialised) winrt::uninit_apartment();
    }
    ApartmentScope(const ApartmentScope&) = delete;
    ApartmentScope& operator=(const ApartmentScope&) = delete;
};

}  // namespace

LightingBackendBase::~LightingBackendBase() {
    // The derived part is already gone here, so no virtual (Release/DisplayName)
    // may be called: only make sure the worker is stopped and joined. Derived
    // classes that want a proper release call StopBlackout() in their own
    // destructor.
    std::lock_guard life(lifecycle_);
    std::jthread worker;
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        worker = std::move(thread_);
    }
    if (worker.joinable()) {
        worker.request_stop();
        worker.join();
    }
}

bool LightingBackendBase::IsHolding() const {
    std::lock_guard lock(mutex_);
    return running_;
}

std::wstring LightingBackendBase::StatusText() const {
    std::lock_guard lock(mutex_);
    return status_;
}

void LightingBackendBase::SetStatus(std::wstring status) {
    std::lock_guard lock(mutex_);
    status_ = std::move(status);
}

void LightingBackendBase::StartBlackout() {
    std::lock_guard life(lifecycle_);
    {
        std::lock_guard lock(mutex_);
        if (running_) return;
    }
    // Enabled() reads live config; evaluate it outside our own lock so a
    // backend is free to consult its status from there.
    if (!Enabled()) return;

    std::lock_guard lock(mutex_);
    if (running_) return;
    loggedUnavailable_ = false;
    failureStreak_ = 0;
    running_ = true;
    thread_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}

void LightingBackendBase::StopBlackout() {
    std::lock_guard life(lifecycle_);
    std::jthread worker;
    {
        std::lock_guard lock(mutex_);
        if (!running_ && !thread_.joinable()) return;
        running_ = false;
        worker = std::move(thread_);
    }
    if (worker.joinable()) {
        worker.request_stop();
        worker.join();
    }
    try {
        Release();
    } catch (const std::exception& ex) {
        LogInfo(std::format(L"{}: release failed ({})", DisplayName(), WhatToWide(ex)));
    } catch (const winrt::hresult_error& ex) {
        LogInfo(std::format(L"{}: release failed ({})", DisplayName(), std::wstring(ex.message())));
    } catch (...) {
        LogInfo(std::format(L"{}: release failed (unknown error)", DisplayName()));
    }
    SetStatus(ReleasedStatus());
}

bool LightingBackendBase::SleepFor(std::stop_token stop, std::chrono::milliseconds duration) {
    std::unique_lock lock(sleepMutex_);
    const bool stopped = sleepCv_.wait_for(lock, stop, duration, [&stop] { return stop.stop_requested(); });
    return !stopped;
}

void LightingBackendBase::Run(std::stop_token stop) {
    ApartmentScope apartment;
    const auto started = std::chrono::steady_clock::now();
    bool engaged = false;

    // Mirrors the C# RunAsync: a failed tick is logged once, the backend is told
    // the connection is gone, and we retry — quickly at first (2/4/8/16 s, for
    // an engine that just wasn't up yet when the guest session appeared), then
    // settling at RetryDelay. Only a stop request ends the loop.
    auto onFailure = [this, &stop](const std::exception& ex) -> bool {
        // An exception raised while a stop is pending is the cancellation
        // unwinding (the OperationCanceledException filter in C#): exit quietly.
        if (stop.stop_requested()) return false;
        SetStatus(UnavailableText(ex));
        // 2/4/8/16 s for the first failures, then exactly RetryDelay() — never
        // min()'d against the ramp, or a configured delay above 32 s would be
        // silently ignored forever.
        if (failureStreak_ < 5) ++failureStreak_;
        const auto delay = failureStreak_ >= 5
                               ? RetryDelay()
                               : std::min(std::chrono::seconds(1LL << failureStreak_), RetryDelay());
        if (!loggedUnavailable_) {
            loggedUnavailable_ = true;
            LogInfo(std::format(L"{} unavailable ({}); retrying in {}s (backing off to {}s)", DisplayName(),
                                WhatToWide(ex), static_cast<long long>(delay.count()),
                                static_cast<long long>(RetryDelay().count())));
        }
        OnConnectionLost();
        return SleepFor(stop, delay);
    };

    while (!stop.stop_requested()) {
        try {
            ApplyTick(stop);
            if (stop.stop_requested()) return;
            if (!engaged) {
                engaged = true;
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started).count();
                LogInfo(std::format(L"{}: blackout engaged in {} ms", DisplayName(), ms));
            }
            failureStreak_ = 0;
            if (loggedUnavailable_) {
                loggedUnavailable_ = false;
                LogInfo(std::format(L"{}: blackout applied (recovered)", DisplayName()));
            }
            SetStatus(HoldingStatus());
            if (!SleepFor(stop, Tick())) return;
        } catch (const std::exception& ex) {
            if (!onFailure(ex)) return;
        } catch (const winrt::hresult_error& ex) {
            const std::runtime_error wrapped(json::WideToUtf8(std::wstring(ex.message())));
            if (!onFailure(wrapped)) return;
        } catch (...) {
            const std::runtime_error wrapped("unknown error");
            if (!onFailure(wrapped)) return;
        }
    }
}

}  // namespace pc
