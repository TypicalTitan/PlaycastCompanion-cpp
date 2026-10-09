#pragma once
#include "Config.h"
#include <filesystem>
#include <memory>
#include <string>

namespace pc {
class SessionLogger {
public:
    SessionLogger(const AppConfig& config, bool headless, bool harness = false, std::filesystem::path logRoot = {});
    ~SessionLogger();
    SessionLogger(const SessionLogger&) = delete;
    SessionLogger& operator=(const SessionLogger&) = delete;
    void UpdateSession(bool active, std::wstring_view reason, std::wstring_view identity);
    void UpdateBackends(std::wstring_view statusJson);
    void SetIncludeSecrets(bool enabled);
    bool IncludeSecrets() const;
    std::wstring Status() const;
    std::wstring Directory() const;
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
