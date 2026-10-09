#include "pch.h"
#include "ChromaController.h"
#include "WinRtOperationGate.h"
#include "Log.h"
#include <iostream>

namespace pc {
void LogInfo(std::wstring_view) {}
void LogInfo(std::string_view) {}
}

namespace {
using namespace winrt::Windows::Foundation;
void Check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template <typename Call>
void ExpectFailure(Call&& call) {
    bool failed = false;
    try { call(); } catch (const std::runtime_error&) { failed = true; }
    Check(failed, "Expected bounded operation failure");
}

struct ControlledOperation : winrt::implements<ControlledOperation, IAsyncOperation<int32_t>, IAsyncInfo> {
    std::atomic<AsyncStatus> state{AsyncStatus::Started};
    std::atomic<int> cancels{0}, closes{0}, results{0};
    AsyncOperationCompletedHandler<int32_t> completion{nullptr};
    uint32_t Id() const { return 1; }
    AsyncStatus Status() const { return state.load(); }
    winrt::hresult ErrorCode() const { return S_OK; }
    void Cancel() { ++cancels; } // Simulates a provider that delays cancellation acknowledgement.
    void Close() { Check(Status() != AsyncStatus::Started, "A pending operation was closed"); ++closes; }
    int32_t GetResults() { Check(Status() == AsyncStatus::Completed, "Results read before completion"); ++results; return 42; }
    void Completed(const AsyncOperationCompletedHandler<int32_t>& handler) { completion = handler; }
    AsyncOperationCompletedHandler<int32_t> Completed() const { return completion; }
};

void PendingRetriesAreBoundedAndRecover() {
    pc::WinRtOperationGate gate;
    auto stuck = winrt::make_self<ControlledOperation>();
    int starts = 0;
    const auto create = [&] { ++starts; return stuck.as<IAsyncOperation<int32_t>>(); };
    ExpectFailure([&] { gate.Invoke(create, std::chrono::milliseconds(5)); });
    for (int attempt = 0; attempt < 200; ++attempt) {
        ExpectFailure([&] { gate.Invoke(create, std::chrono::milliseconds(5)); });
    }
    Check(starts == 1 && stuck->cancels == 1 && stuck->closes == 0,
          "Retries accumulated provider operations or repeatedly cancelled the same operation");
    stuck->state = AsyncStatus::Canceled;
    auto complete = winrt::make_self<ControlledOperation>();
    complete->state = AsyncStatus::Completed;
    Check(gate.Invoke([&] { return complete.as<IAsyncOperation<int32_t>>(); }, std::chrono::milliseconds(5)) == 42,
          "Completed cancellation prevented recovery");
    Check(stuck->closes == 1 && complete->closes == 1, "Terminal operations were not closed");
}

void CancellationStopsWaitingAndPreventsNewWork() {
    pc::WinRtOperationGate gate;
    auto operation = winrt::make_self<ControlledOperation>();
    std::stop_source stop;
    std::jthread cancel([&] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); stop.request_stop(); });
    const auto start = std::chrono::steady_clock::now();
    ExpectFailure([&] { gate.Invoke([&] { return operation.as<IAsyncOperation<int32_t>>(); }, std::chrono::seconds(10), stop.get_token()); });
    Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1) && operation->cancels == 1,
          "Cancellation did not bound shutdown");
    int started = 0;
    ExpectFailure([&] { gate.Invoke([&] { ++started; return operation.as<IAsyncOperation<int32_t>>(); },
                      std::chrono::milliseconds(5), stop.get_token()); });
    Check(started == 0, "Cancelled caller started or consumed new work");

    pc::WinRtOperationGate completedGate;
    auto complete = winrt::make_self<ControlledOperation>();
    complete->state = AsyncStatus::Completed;
    std::stop_source race;
    ExpectFailure([&] { completedGate.Invoke([&] { race.request_stop(); return complete.as<IAsyncOperation<int32_t>>(); },
                                           std::chrono::seconds(1), race.get_token()); });
    Check(complete->results == 0 && complete->closes == 1,
          "Cancellation arriving at completion still released a result for painting");
}

void ChromaCleanupReportsActualOutcome(int status, bool transportFailure) {
    pc::AppConfig config;
    std::atomic<int> deletes{0};
    pc::ChromaController controller(config, [&](const std::string& method, const std::wstring&,
                                                const std::string&, unsigned) -> pc::http::Response {
        if (method == "POST") return {200, R"({"uri":"http://fixture.invalid/session"})"};
        if (method == "DELETE") {
            ++deletes;
            if (transportFailure) throw std::runtime_error("WinHttpSendRequest failed (error 12029)");
            return {status, {}};
        }
        return {200, {}};
    });
    controller.StartBlackout();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (controller.StatusText() != L"Holding blackout" && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Check(controller.StatusText() == L"Holding blackout", "Synthetic Chroma did not engage");
    controller.StopBlackout();
    const bool success = !transportFailure && status >= 200 && status < 300;
    Check(deletes == 1 && !controller.IsHolding(), "Stop must release once and stop reassertion");
    Check(controller.StatusText() == (success ? L"Hold ended; Chroma session released"
                                              : L"Hold ended; Chroma release unverified"),
          "Cleanup claimed a result that was not observed");
}
}

int main() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        PendingRetriesAreBoundedAndRecover();
        CancellationStopsWaitingAndPreventsNewWork();
        ChromaCleanupReportsActualOutcome(200, false);
        ChromaCleanupReportsActualOutcome(500, false);
        ChromaCleanupReportsActualOutcome(0, true);
        winrt::uninit_apartment();
        std::cout << "PASS: bounded WinRT retry/recovery, shutdown cancellation, and truthful Chroma release contracts\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    catch (const winrt::hresult_error& error) { std::wcerr << error.message().c_str() << '\n'; return 1; }
}
