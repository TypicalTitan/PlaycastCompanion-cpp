// Common includes for every translation unit. Keep this header-only and stable:
// every module includes it first.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <wtsapi32.h>
#include <winhttp.h>
#include <sddl.h>
#include <aclapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

namespace pc {
/// RAII wrapper for kernel HANDLEs (CloseHandle). Null-safe.
struct UniqueHandle {
    HANDLE h = nullptr;
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : h(handle) {}
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& o) noexcept : h(o.h) { o.h = nullptr; }
    UniqueHandle& operator=(UniqueHandle&& o) noexcept { reset(o.h); o.h = nullptr; return *this; }
    ~UniqueHandle() { reset(); }
    void reset(HANDLE handle = nullptr) {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = handle;
    }
    bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return h; }
    HANDLE release() { HANDLE t = h; h = nullptr; return t; }
};
}  // namespace pc
