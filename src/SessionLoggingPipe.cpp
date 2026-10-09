#include "pch.h"
#include "SessionLoggingPipe.h"

namespace pc::sessionlog {
namespace {
constexpr DWORD FrameLimit = 1024 * 1024;
struct Descriptor {
    PSECURITY_DESCRIPTOR value = nullptr;
    explicit Descriptor(const std::wstring& sddl) {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &value, nullptr))
            throw std::runtime_error("Cannot create diagnostic pipe ACL");
    }
    ~Descriptor() { if (value) LocalFree(value); }
};
}

DiagnosticPipe::DiagnosticPipe(std::wstring target, std::function<void(Object)> onEvent, std::function<void(std::wstring)> onStatus, std::wstring pipeName)
    : target_(std::move(target)), pipeName_(std::move(pipeName)), onEvent_(std::move(onEvent)), onStatus_(std::move(onStatus)),
      stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!stop_.valid()) throw std::runtime_error("Cannot create diagnostic shutdown event");
    if (pipeName_.empty() || pipeName_.size() > 200 || pipeName_.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != std::wstring::npos)
        throw std::runtime_error("Invalid diagnostic pipe name");
    thread_ = std::jthread([this] { Run(); });
}
DiagnosticPipe::~DiagnosticPipe() { Stop(); }
void DiagnosticPipe::Stop() {
    if (stop_.valid()) SetEvent(stop_.get());
    if (thread_.joinable()) thread_.join();
}

bool DiagnosticPipe::Validate(const Object& envelope) {
    const auto category = json::GetString(envelope, L"category");
    const auto direction = json::GetString(envelope, L"direction");
    return json::GetNumber(envelope, L"version", 0) == 1 && json::Has(envelope, L"payload")
        && (category == L"realtime" || category == L"ipc" || category == L"state" || category == L"diagnostics")
        && (direction == L"sent" || direction == L"received" || direction == L"snapshot")
        && std::isfinite(json::GetNumber(envelope, L"timestamp", std::numeric_limits<double>::quiet_NaN()));
}

bool DiagnosticPipe::Await(HANDLE pipe, OVERLAPPED& operation, bool started, DWORD timeout, DWORD& transferred) {
    if (started) return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
    const auto error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;
    const HANDLE events[] = {stop_.get(), operation.hEvent};
    const auto wait = WaitForMultipleObjects(2, events, FALSE, timeout);
    if (wait == WAIT_OBJECT_0 + 1) return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
    CancelIoEx(pipe, &operation);
    GetOverlappedResult(pipe, &operation, &transferred, TRUE);
    return false;
}

void DiagnosticPipe::Run() {
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    catch (...) { onStatus_(L"Diagnostic pipe worker unavailable: cannot initialize Windows Runtime."); return; }
    while (WaitForSingleObject(stop_.get(), 0) == WAIT_TIMEOUT) {
        try {
            Descriptor descriptor(PipeSecurity(target_));
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor.value, FALSE};
            UniqueHandle pipe(CreateNamedPipeW((L"\\\\.\\pipe\\" + pipeName_).c_str(),
                PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                1, 0, 16384, 0, &security));
            if (!pipe.valid()) throw std::runtime_error(std::format("CreateNamedPipe failed (Win32 {})", GetLastError()));
            onStatus_(L"Waiting for Playcast diagnostic feed; realtime coverage unavailable until connected.");
            UniqueHandle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            OVERLAPPED operation{};
            operation.hEvent = ready.get();
            DWORD transferred = 0;
            const bool connected = ConnectNamedPipe(pipe.get(), &operation) != FALSE;
            if (!Await(pipe.get(), operation, connected, 30000, transferred)) continue;
            Consume(pipe.get());
            DisconnectNamedPipe(pipe.get());
            onStatus_(L"Playcast diagnostic feed disconnected; waiting for reconnect.");
        } catch (const std::exception& error) {
            onStatus_(L"Playcast diagnostic feed unavailable: " + json::Utf8ToWide(error.what()));
            WaitForSingleObject(stop_.get(), 3000);
        } catch (...) {
            onStatus_(L"Playcast diagnostic feed unavailable; invalid producer or transport.");
            WaitForSingleObject(stop_.get(), 3000);
        }
    }
    winrt::uninit_apartment();
}

void DiagnosticPipe::Consume(HANDLE pipe) {
    std::string frame;
    frame.reserve(16384);
    bool authenticated = false;
    UniqueHandle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    for (;;) {
        OVERLAPPED operation{};
        operation.hEvent = ready.get();
        ResetEvent(ready.get());
        char bytes[16384]{};
        DWORD received = 0;
        const bool read = ReadFile(pipe, bytes, sizeof(bytes), &received, &operation) != FALSE;
        if (!Await(pipe, operation, read, INFINITE, received) || !received) return;
        if (!authenticated) {
            std::wstring error;
            if (!VerifyPipeClient(pipe, target_, error)) { onStatus_(error); return; }
            authenticated = true;
            onStatus_(L"Playcast diagnostic feed connected; transport authenticated.");
        }
        for (DWORD index = 0; index < received; ++index) {
            if (bytes[index] != '\n') {
                if (frame.size() >= FrameLimit) { onStatus_(L"Diagnostic frame rejected: exceeds 1 MiB."); return; }
                frame += bytes[index];
                continue;
            }
            if (!frame.empty() && frame.back() == '\r') frame.pop_back();
            try {
                const auto envelope = json::Parse(frame);
                if (Validate(envelope)) onEvent_(envelope);
                else onStatus_(L"Diagnostic frame rejected: invalid envelope.");
            } catch (...) { onStatus_(L"Diagnostic frame rejected: invalid JSON."); }
            frame.clear();
        }
    }
}
}
