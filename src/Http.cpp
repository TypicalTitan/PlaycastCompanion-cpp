#include "pch.h"
#include "Http.h"

#include "Json.h"

// Synchronous WinHTTP wrapper. Loopback targets (Razer Chroma REST, SteelSeries
// GameSense) bypass any system proxy; everything else (discord.com) uses the
// WinHTTP default proxy configuration.

namespace pc::http {
namespace {

constexpr size_t kMaxBodyBytes = 4u * 1024 * 1024;

struct HInternet {
    HINTERNET h = nullptr;
    HInternet() = default;
    explicit HInternet(HINTERNET handle) : h(handle) {}
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
    HInternet(HInternet&& o) noexcept : h(o.h) { o.h = nullptr; }
    HInternet& operator=(HInternet&& o) noexcept {
        if (this != &o) {
            reset();
            h = o.h;
            o.h = nullptr;
        }
        return *this;
    }
    ~HInternet() { reset(); }
    void reset() {
        if (h) WinHttpCloseHandle(h);
        h = nullptr;
    }
    explicit operator bool() const { return h != nullptr; }
    HINTERNET get() const { return h; }
};

[[noreturn]] void Fail(const char* step) {
    const DWORD err = GetLastError();
    throw std::runtime_error(std::format("{} failed (error {})", step, err));
}

bool IsDottedDigits(std::wstring_view s) {
    if (s.empty()) return false;
    for (wchar_t c : s) {
        if (!((c >= L'0' && c <= L'9') || c == L'.')) return false;
    }
    return true;
}

bool IsLoopbackHost(std::wstring_view host) {
    if (host.size() >= 2 && host.front() == L'[' && host.back() == L']') {
        host = host.substr(1, host.size() - 2);
    }
    if (host == L"::1") return true;
    if (host.size() == 9 && CompareStringOrdinal(host.data(), 9, L"localhost", 9, TRUE) == CSTR_EQUAL) return true;
    return host.size() >= 4 && host.substr(0, 4) == L"127." && IsDottedDigits(host);
}

}  // namespace

Response Request(const std::string& method,
                 const std::wstring& url,
                 const std::string& body,
                 const std::wstring& contentType,
                 unsigned timeoutMs,
                 const std::wstring& userAgent) {
    if (url.empty() || url.size() > static_cast<size_t>(INT_MAX)) {
        throw std::runtime_error("WinHttpCrackUrl failed (error 87)");  // ERROR_INVALID_PARAMETER
    }
    if (body.size() > static_cast<size_t>(0xFFFFFFFFu)) {
        throw std::runtime_error("WinHttpSendRequest failed (error 87)");
    }

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) Fail("WinHttpCrackUrl");
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) {
        throw std::runtime_error("WinHttpCrackUrl failed (error 12006)");  // ERROR_WINHTTP_UNRECOGNIZED_SCHEME
    }
    if (!uc.lpszHostName || uc.dwHostNameLength == 0) {
        throw std::runtime_error("WinHttpCrackUrl failed (error 12005)");  // ERROR_WINHTTP_INVALID_URL
    }
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring objectName;
    if (uc.lpszUrlPath && uc.dwUrlPathLength > 0) objectName.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength > 0) objectName.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (objectName.empty()) objectName = L"/";

    const DWORD accessType = IsLoopbackHost(host) ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
    HInternet session(WinHttpOpen(userAgent.c_str(), accessType, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) Fail("WinHttpOpen");

    const int timeout = timeoutMs > static_cast<unsigned>(INT_MAX) ? INT_MAX : static_cast<int>(timeoutMs);
    if (!WinHttpSetTimeouts(session.get(), timeout, timeout, timeout, timeout)) Fail("WinHttpSetTimeouts");

    HInternet connection(WinHttpConnect(session.get(), host.c_str(), uc.nPort, 0));
    if (!connection) Fail("WinHttpConnect");

    const std::wstring wideMethod = json::Utf8ToWide(method);
    HInternet request(WinHttpOpenRequest(connection.get(), wideMethod.c_str(), objectName.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request) Fail("WinHttpOpenRequest");

    std::wstring headers;
    if (!body.empty() && !contentType.empty()) headers = L"Content-Type: " + contentType + L"\r\n";
    const wchar_t* headerText = headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str();
    const DWORD headerLength = static_cast<DWORD>(headers.size());
    void* optional = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    const DWORD bodyLength = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(request.get(), headerText, headerLength, optional, bodyLength, bodyLength, 0)) {
        Fail("WinHttpSendRequest");
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) Fail("WinHttpReceiveResponse");

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        Fail("WinHttpQueryHeaders");
    }

    Response response;
    response.status = static_cast<int>(status);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) Fail("WinHttpQueryDataAvailable");
        if (available == 0) break;
        if (response.body.size() + available > kMaxBodyBytes) {
            throw std::runtime_error("response body exceeds 4 MB limit");
        }
        const size_t offset = response.body.size();
        response.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), response.body.data() + offset, available, &read)) {
            Fail("WinHttpReadData");
        }
        response.body.resize(offset + read);
        if (read == 0) break;
    }
    return response;
}

}  // namespace pc::http
