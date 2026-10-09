#pragma once
#include "LogAnalysis.h"
#include "Json.h"

namespace pc::analysis {
using Value = winrt::Windows::Data::Json::IJsonValue;
Value Get(const Value& value, std::wstring_view name);
Value Field(const Value& value, std::wstring_view name);
Value Message(const Value& payload);
std::wstring Text(const Value& value);
std::optional<double> Number(const Value& value);
std::optional<double> Metric(const Value& value);
std::optional<int64_t> Timestamp(const Value& value);
std::wstring Type(const Value& message);
bool Contains(std::wstring_view text, std::wstring_view needle);
void Warn(LogAnalysisResult& result, std::wstring warning);
void Normalize(LogAnalysisResult& result, std::filesystem::path source, size_t line, std::string text);
} // namespace pc::analysis
