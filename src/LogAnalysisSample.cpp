#include "pch.h"
#include "LogAnalysisSample.h"

namespace pc {
TemporaryLogSample::TemporaryLogSample(HINSTANCE instance) {
    const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(204), RT_RCDATA);
    const auto data = resource ? LoadResource(instance, resource) : nullptr;
    const auto bytes = resource ? SizeofResource(instance, resource) : 0;
    const auto contents = data ? LockResource(data) : nullptr;
    if (!contents || !bytes) throw std::runtime_error("Embedded sample is unavailable");
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temporary)) throw std::runtime_error("Temporary directory is unavailable");
    static std::atomic_uint sequence = 0;
    path_ = std::filesystem::path(temporary) / std::format(L"playcast-companion-sample-{}-{}-{}.jsonl", GetCurrentProcessId(), GetTickCount64(), ++sequence);
    bool created = false;
    try {
        UniqueHandle file(CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr));
        if (!file.valid()) throw std::runtime_error("Temporary sample cannot be created");
        created = true; DWORD written = 0;
        if (!WriteFile(file.get(), contents, bytes, &written, nullptr) || written != bytes) throw std::runtime_error("Temporary sample cannot be written");
    } catch (...) { if (created) DeleteFileW(path_.c_str()); throw; }
}
TemporaryLogSample::~TemporaryLogSample() { if (!path_.empty()) DeleteFileW(path_.c_str()); }
}
