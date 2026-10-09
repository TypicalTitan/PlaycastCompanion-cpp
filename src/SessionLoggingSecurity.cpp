#include "pch.h"
#include "SessionLoggingJson.h"
#include "SessionWatcher.h"

namespace pc::sessionlog {
namespace {
struct LocalMemory {
    void* value = nullptr;
    ~LocalMemory() { if (value) LocalFree(value); }
};
std::wstring SidText(PSID sid) {
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) return {};
    LocalMemory memory{ text };
    return text;
}
std::wstring TokenSid(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return {};
    UniqueHandle tokenOwner(token);
    DWORD length = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &length);
    if (!length || length > 65536) return {};
    std::vector<unsigned char> bytes(length);
    if (!GetTokenInformation(token, TokenUser, bytes.data(), length, &length)) return {};
    return SidText(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid);
}
std::optional<std::wstring> SessionText(ULONG id, WTS_INFO_CLASS kind) {
    LPWSTR text = nullptr;
    DWORD size = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, id, kind, &text, &size)) return std::nullopt;
    struct Memory { LPWSTR text; ~Memory() { WTSFreeMemory(text); } } memory{ text };
    return text && size >= sizeof(wchar_t) ? std::wstring(text) : std::wstring();
}
}

std::wstring UtcNow() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    return std::format(L"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", time.wYear, time.wMonth,
        time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
}
std::wstring CurrentSid() { return TokenSid(GetCurrentProcess()); }

std::wstring AccountSid(std::wstring_view account) {
    DWORD sidSize = 0, domainSize = 0;
    SID_NAME_USE use{};
    std::wstring name(account);
    LookupAccountNameW(nullptr, name.c_str(), nullptr, &sidSize, nullptr, &domainSize, &use);
    if (!sidSize || sidSize > 65536 || domainSize > 32768) return {};
    std::vector<unsigned char> sid(sidSize);
    std::wstring domain(domainSize, L'\0');
    if (!LookupAccountNameW(nullptr, name.c_str(), sid.data(), &sidSize, domain.data(), &domainSize, &use)) return {};
    return SidText(sid.data());
}

void ProtectDirectory(const std::filesystem::path& directory) {
    const auto owner = CurrentSid();
    if (owner.empty()) throw std::runtime_error("Cannot identify the session log owner");
    const auto sddl = L"D:P(A;OICI;FA;;;" + owner + L")(A;OICI;FA;;;SY)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("Cannot create session directory ACL");
    LocalMemory memory{ descriptor };
    if (!SetFileSecurityW(directory.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor))
        throw std::runtime_error(std::format("Cannot restrict session directory (Win32 {})", GetLastError()));
}

std::wstring PipeSecurity(std::wstring_view target) {
    const auto owner = CurrentSid();
    if (owner.empty()) throw std::runtime_error("Cannot identify the diagnostic sink owner");
    auto sddl = L"D:P(D;;GRGW;;;NU)(A;;GA;;;" + owner + L")(A;;GRGW;;;SY)";
    const auto guest = AccountSid(target);
    if (!guest.empty() && guest != owner) sddl += L"(A;;GRGW;;;" + guest + L")";
    return sddl;
}

bool VerifyPipeClient(HANDLE pipe, std::wstring_view target, std::wstring& error) {
    ULONG pid = 0, session = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid) || !GetNamedPipeClientSessionId(pipe, &session) || !pid) {
        error = std::format(L"Cannot query diagnostic producer identity (Win32 {})", GetLastError());
        return false;
    }
    DWORD ownerSession = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &ownerSession) && session == ownerSession) return true;
    std::wstring user;
    const auto username = SessionText(session, WTSUserName);
    const auto domain = SessionText(session, WTSDomainName);
    if (username && domain && !username->empty())
        user = AccountSid(domain->empty() ? *username : *domain + L"\\" + *username);
    if (user.empty()) {
        UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (process.valid()) user = TokenSid(process.get());
    }
    const auto guest = AccountSid(target);
    if (!user.empty() && (user == CurrentSid() || user == L"S-1-5-18" || (!guest.empty() && user == guest))) return true;
    error = std::format(L"Cannot verify allowed diagnostic producer account in Windows session {} (Win32 {})", session, GetLastError());
    return false;
}
}
