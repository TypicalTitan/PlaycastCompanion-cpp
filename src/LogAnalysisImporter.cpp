#include "pch.h"
#include "LogAnalysisInternal.h"
#include <fstream>
#include <set>

namespace pc {
namespace {
struct WindowsPathLess {
    bool operator()(const std::filesystem::path& left, const std::filesystem::path& right) const {
        const auto& a = left.native(); const auto& b = right.native();
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};
bool IsLogFile(const std::filesystem::path& path) {
    const auto extension = path.extension().wstring();
    return CompareStringOrdinal(extension.c_str(), -1, L".jsonl", -1, TRUE) == CSTR_EQUAL
        || CompareStringOrdinal(extension.c_str(), -1, L".ndjson", -1, TRUE) == CSTR_EQUAL
        || CompareStringOrdinal(extension.c_str(), -1, L".log", -1, TRUE) == CSTR_EQUAL;
}
std::vector<std::filesystem::path> Discover(const std::vector<std::filesystem::path>& sources,
    LogAnalysisResult& result, std::stop_token stop) {
    std::set<std::filesystem::path, WindowsPathLess> unique;
    size_t visited = 0;
    for (const auto& selected : sources) {
        if (stop.stop_requested()) break;
        std::error_code error;
        const auto source = std::filesystem::absolute(selected, error).lexically_normal();
        if (error) { analysis::Warn(result, selected.wstring() + L": invalid source path."); continue; }
        if (std::filesystem::is_directory(source, error)) {
            std::filesystem::recursive_directory_iterator iterator(source, error), end;
            while (!error && iterator != end && unique.size() < 5000 && !stop.stop_requested()) {
                if (++visited > 100000) { result.LimitReached = true; analysis::Warn(result, L"Source discovery stopped at 100,000 directory entries."); break; }
                const auto status = iterator->symlink_status(error);
                if (std::filesystem::is_symlink(status)) iterator.disable_recursion_pending();
                else if (std::filesystem::is_regular_file(status) && IsLogFile(iterator->path()))
                    unique.insert(std::filesystem::absolute(iterator->path()).lexically_normal());
                if (iterator.depth() >= 32 && std::filesystem::is_directory(status)) {
                    iterator.disable_recursion_pending(); result.LimitReached = true;
                    analysis::Warn(result, L"Source discovery skipped folders deeper than 32 levels.");
                }
                iterator.increment(error);
            }
            if (unique.size() >= 5000) { result.LimitReached = true; analysis::Warn(result, L"Source discovery stopped at 5,000 files."); }
        } else if (!error && IsLogFile(source)) unique.insert(std::filesystem::absolute(source));
        else if (!error) analysis::Warn(result, source.wstring() + L": select a .jsonl/.ndjson/.log file or a session folder.");
        if (error) analysis::Warn(result, source.wstring() + L": " + json::Utf8ToWide(error.message()));
        if (unique.size() >= 5000) { result.LimitReached = true; analysis::Warn(result, L"Source discovery stopped at 5,000 files."); break; }
        if (visited > 100000) break;
    }
    return {unique.begin(), unique.end()};
}
void ConsumeLine(LogAnalysisResult& result, const std::filesystem::path& source, size_t lineNumber,
    std::string& line, bool oversized) {
    ++result.LinesRead;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (oversized) {
        ++result.InvalidLines;
        analysis::Warn(result, source.filename().wstring() + L":" + std::to_wstring(lineNumber) + L" skipped: record exceeds 10 MiB.");
    } else if (!line.empty()) {
        try { analysis::Normalize(result, source, lineNumber, std::move(line)); }
        catch (...) {
            ++result.InvalidLines;
            analysis::Warn(result, source.filename().wstring() + L":" + std::to_wstring(lineNumber) + L" skipped: invalid JSON record or UTF-8.");
        }
    }
    line.clear();
}
void ReadFile(LogAnalysisResult& result, const std::filesystem::path& source, std::stop_token stop) {
    std::ifstream stream(source, std::ios::binary);
    if (!stream) { analysis::Warn(result, source.wstring() + L": unable to open file (missing or access denied)."); return; }
    ++result.FilesRead;
    std::array<char, 64 * 1024> chunk{};
    std::string line;
    bool oversized = false;
    size_t lineNumber = 1;
    while (stream && !stop.stop_requested() && !result.LimitReached) {
        if (result.Records.size() >= LogAnalysisImporter::MaxRecords || result.LinesRead >= LogAnalysisImporter::MaxScannedLines) { result.LimitReached = true; break; }
        const auto remaining = LogAnalysisImporter::MaxImportBytes - result.BytesRead;
        if (remaining == 0) { result.LimitReached = true; break; }
        stream.read(chunk.data(), static_cast<std::streamsize>(std::min<uint64_t>(remaining, chunk.size())));
        const auto count = static_cast<size_t>(stream.gcount());
        result.BytesRead += count;
        for (size_t position = 0; position < count; ++position) {
            if (chunk[position] == '\n') {
                ConsumeLine(result, source, lineNumber++, line, oversized); oversized = false;
                if (result.Records.size() >= LogAnalysisImporter::MaxRecords || result.LinesRead >= LogAnalysisImporter::MaxScannedLines) { result.LimitReached = true; break; }
            } else if (!oversized) {
                line += chunk[position];
                if (line.size() > LogAnalysisImporter::MaxLineBytes) { line.clear(); oversized = true; }
            }
        }
    }
    if (!stop.stop_requested() && !result.LimitReached && (!line.empty() || oversized))
        ConsumeLine(result, source, lineNumber, line, oversized);
    if (result.Records.size() >= LogAnalysisImporter::MaxRecords || result.LinesRead >= LogAnalysisImporter::MaxScannedLines) result.LimitReached = true;
    if (stream.bad()) analysis::Warn(result, source.wstring() + L": file read failed; imported records may be incomplete.");
}
} // namespace

LogAnalysisResult LogAnalysisImporter::Import(const std::vector<std::filesystem::path>& sources, std::stop_token stop) const {
    LogAnalysisResult result;
    auto files = Discover(sources, result, stop);
    const bool discoveryLimited = result.LimitReached;
    result.LimitReached = false;
    for (const auto& file : files) {
        if (stop.stop_requested() || result.LimitReached) break;
        ReadFile(result, file, stop);
    }
    if (result.LimitReached) analysis::Warn(result, L"Import stopped at the 128 MiB / 50,000-record / 100,000-line limit. Partial results are shown.");
    result.LimitReached = result.LimitReached || discoveryLimited;
    result.Cancelled = stop.stop_requested();
    if (files.empty()) analysis::Warn(result, L"No JSONL files found. Import a session folder containing rotated category logs.");
    std::stable_sort(result.Records.begin(), result.Records.end(), [](const auto& left, const auto& right) {
        if (!left.TimeMs) return false;
        return !right.TimeMs || *left.TimeMs < *right.TimeMs;
    });
    LogAnalysisProjection::Build(result, stop);
    return result;
}
} // namespace pc
