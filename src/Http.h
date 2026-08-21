#pragma once
// Small synchronous WinHTTP wrapper. Everything the app talks to over HTTP is
// either loopback (Razer Chroma, SteelSeries GameSense) or discord.com.
#include <string>

namespace pc::http {
struct Response {
    int status = 0;     // HTTP status code (0 never happens on success)
    std::string body;   // raw UTF-8 body
};

/// Perform a request. `method` is "GET"/"POST"/"PUT"/"DELETE"; `url` is an
/// absolute http(s) URL. Throws std::runtime_error on transport failure or
/// timeout (connection refused, DNS, TLS, timeout). Non-2xx responses are
/// RETURNED, not thrown — callers decide. `timeoutMs` bounds resolve/connect/
/// send/receive each.
Response Request(const std::string& method,
                 const std::wstring& url,
                 const std::string& body = {},
                 const std::wstring& contentType = L"application/json",
                 unsigned timeoutMs = 10000,
                 const std::wstring& userAgent = L"PlaycastCompanion/3.0");

inline bool IsSuccess(const Response& r) { return r.status >= 200 && r.status < 300; }
}  // namespace pc::http
