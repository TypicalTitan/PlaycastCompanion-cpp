#pragma once
#include <filesystem>
#include <windows.h>

namespace pc {
/// Owns one synthetic sample file until the background importer finishes.
class TemporaryLogSample {
public:
    explicit TemporaryLogSample(HINSTANCE instance);
    ~TemporaryLogSample();
    TemporaryLogSample(const TemporaryLogSample&) = delete;
    TemporaryLogSample& operator=(const TemporaryLogSample&) = delete;
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};
}
