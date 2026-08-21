#pragma once
// A lighting system that can be blacked out while a guest is hosting, plus the
// shared hold/release loop every backend reuses. Mirrors the C# design:
// StartBlackout spins a background loop that re-asserts the blackout every
// Tick() and retries after RetryDelay() on failure; StopBlackout stops the
// loop and runs Release(). HTTP timeouts are ordinary failures (retry), NEVER a
// reason to exit the loop — only an explicit stop request ends it.
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>

namespace pc {
class ILightingBackend {
public:
    virtual ~ILightingBackend() = default;
    virtual std::wstring DisplayName() const = 0;
    virtual bool Enabled() const = 0;          // reads live config
    virtual bool IsHolding() const = 0;
    virtual std::wstring StatusText() const = 0;
    virtual void StartBlackout() = 0;          // no-op if already running or !Enabled()
    virtual void StopBlackout() = 0;           // stops, joins, releases; returns within ~5 s
};

class LightingBackendBase : public ILightingBackend {
public:
    ~LightingBackendBase() override;
    bool IsHolding() const override;
    std::wstring StatusText() const override;
    void StartBlackout() override;
    void StopBlackout() override;

protected:
    virtual std::chrono::seconds Tick() const { return std::chrono::seconds(5); }
    virtual std::chrono::seconds RetryDelay() const { return std::chrono::seconds(30); }
    /// Status shown after a successful tick (override to report nuance).
    virtual std::wstring HoldingStatus() const { return L"Holding blackout"; }

    /// Connect if needed and (re-)assert the blackout once. Throw
    /// std::runtime_error (or any std::exception) to signal failure: the base
    /// logs it once, calls OnConnectionLost(), waits RetryDelay(), and retries.
    /// Check `stop` between slow steps and return early when requested.
    virtual void ApplyTick(std::stop_token stop) = 0;
    /// Hand control back to the vendor software. May throw (logged).
    virtual void Release() = 0;
    /// Called after a failed tick, before the retry delay.
    virtual void OnConnectionLost() {}
    virtual std::wstring UnavailableText(const std::exception&) const { return L"Unreachable — retrying"; }

    void SetStatus(std::wstring status);
    /// Interruptible sleep; returns false if stop was requested meanwhile.
    bool SleepFor(std::stop_token stop, std::chrono::milliseconds duration);

private:
    void Run(std::stop_token stop);

    std::mutex lifecycle_;  // serialises StartBlackout/StopBlackout/dtor (never taken by the worker)
    mutable std::mutex mutex_;
    std::wstring status_ = L"Ready";
    std::jthread thread_;
    bool running_ = false;
    bool loggedUnavailable_ = false;
    std::mutex sleepMutex_;
    std::condition_variable_any sleepCv_;
};
}  // namespace pc
