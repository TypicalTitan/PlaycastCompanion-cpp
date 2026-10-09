#pragma once
#include "SessionLoggingJson.h"

namespace pc::sessionlog {
class DiagnosticPipe {
public:
    DiagnosticPipe(std::wstring target, std::function<void(Object)> onEvent, std::function<void(std::wstring)> onStatus,
        std::wstring pipeName = L"PlaycastCompanion.SessionDiagnostics");
    ~DiagnosticPipe();
    void Stop();
    static bool Validate(const Object& envelope);
private:
    void Run();
    void Consume(HANDLE pipe);
    bool Await(HANDLE pipe, OVERLAPPED& operation, bool started, DWORD timeout, DWORD& transferred);
    std::wstring target_;
    std::wstring pipeName_;
    std::function<void(Object)> onEvent_;
    std::function<void(std::wstring)> onStatus_;
    UniqueHandle stop_;
    std::jthread thread_;
};
}
