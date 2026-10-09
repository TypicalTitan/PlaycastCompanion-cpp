#pragma once
#include "Config.h"
#include "SessionLoggingRedactor.h"

namespace pc::sessionlog {
class Writer {
public:
    explicit Writer(SessionLoggingConfig config, std::filesystem::path baseDirectory = {});
    bool Write(std::wstring_view category, const Value& payload, bool includeSecrets);
    const std::filesystem::path& Directory() const { return directory_; }
    const std::wstring& Error() const { return error_; }
    uint64_t BytesWritten() const { return total_; }
    static std::filesystem::path DefaultRoot();
private:
    struct File { UniqueHandle handle; uint64_t bytes = 0; unsigned index = 0; };
    void Append(std::wstring_view category, const std::string& bytes);
    bool Limit();
    void Retain(const std::filesystem::path& root);
    SessionLoggingConfig config_;
    Redactor redactor_;
    std::filesystem::path directory_;
    std::map<std::wstring, File> files_;
    uint64_t total_ = 0;
    bool stopped_ = false;
    std::wstring error_;
};
}
