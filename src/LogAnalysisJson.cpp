#include "pch.h"
#include "LogAnalysisInternal.h"
#include <cmath>
#include <deque>
#include <cwctype>

namespace pc::analysis {
namespace {
bool Equal(std::wstring_view left, std::wstring_view right) {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
        static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
Value Embedded(Value value) {
    if (value && value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String) {
        auto text = Text(value);
        const auto first = text.find_first_not_of(L" \r\n\t");
        if (first != std::wstring::npos && (text[first] == L'{' || text[first] == L'[')) {
            try { return json::JsonValue::Parse(text); } catch (...) { }
        }
    }
    return value;
}
std::optional<int64_t> IsoTime(std::wstring_view text) {
    if (text.size() < 20) return {};
    SYSTEMTIME time{};
    auto digits = [&text](size_t offset, size_t count) -> WORD {
        WORD number = 0;
        for (size_t index = offset; index < offset + count; ++index) {
            if (index >= text.size() || text[index] < L'0' || text[index] > L'9') throw std::runtime_error("Invalid timestamp");
            number = static_cast<WORD>(number * 10 + text[index] - L'0');
        }
        return number;
    };
    try {
        if (text[4] != L'-' || text[7] != L'-' || text[10] != L'T' || text[13] != L':' || text[16] != L':') return {};
        time.wYear = digits(0, 4); time.wMonth = digits(5, 2); time.wDay = digits(8, 2);
        time.wHour = digits(11, 2); time.wMinute = digits(14, 2); time.wSecond = digits(17, 2);
        size_t zone = 19;
        if (text[zone] == L'.') {
            ++zone;
            const size_t start = zone;
            while (zone < text.size() && text[zone] >= L'0' && text[zone] <= L'9') ++zone;
            if (zone == start) return {};
            for (size_t index = 0; index < 3; ++index)
                time.wMilliseconds = static_cast<WORD>(time.wMilliseconds * 10 + (start + index < zone ? text[start + index] - L'0' : 0));
        }
        int offsetMinutes = 0;
        if (zone >= text.size()) return {};
        if (text[zone] == L'Z') { if (zone + 1 != text.size()) return {}; }
        else if ((text[zone] == L'+' || text[zone] == L'-') && zone + 6 == text.size() && text[zone + 3] == L':') {
            const auto hour = digits(zone + 1, 2), minute = digits(zone + 4, 2);
            if (hour > 23 || minute > 59) return {};
            offsetMinutes = (hour * 60 + minute) * (text[zone] == L'+' ? 1 : -1);
        } else return {};
        FILETIME file{};
        if (!SystemTimeToFileTime(&time, &file)) return {};
        SYSTEMTIME roundTrip{};
        if (!FileTimeToSystemTime(&file, &roundTrip) || roundTrip.wYear != time.wYear || roundTrip.wMonth != time.wMonth
            || roundTrip.wDay != time.wDay || roundTrip.wHour != time.wHour || roundTrip.wMinute != time.wMinute || roundTrip.wSecond != time.wSecond) return {};
        ULARGE_INTEGER ticks{}; ticks.LowPart = file.dwLowDateTime; ticks.HighPart = file.dwHighDateTime;
        return static_cast<int64_t>(ticks.QuadPart / 10000) - 11644473600000ll - offsetMinutes * 60000ll;
    } catch (...) { return {}; }
}
} // namespace

Value Get(const Value& value, std::wstring_view name) {
    if (!value || value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Object) return nullptr;
    for (const auto& item : value.GetObjectW()) if (Equal(item.Key(), name)) return item.Value();
    return nullptr;
}
Value Field(const Value& value, std::wstring_view name) {
    std::deque<Value> queue{Embedded(value)};
    for (size_t count = 0; !queue.empty() && count < 150; ++count) {
        auto item = queue.front(); queue.pop_front();
        if (auto found = Get(item, name); found && found.ValueType() != winrt::Windows::Data::Json::JsonValueType::Null) return found;
        if (item && item.ValueType() == winrt::Windows::Data::Json::JsonValueType::Object)
            for (const auto& child : item.GetObjectW()) {
                auto nested = Embedded(child.Value());
                if (nested && queue.size() < 150 && (nested.ValueType() == winrt::Windows::Data::Json::JsonValueType::Object
                    || nested.ValueType() == winrt::Windows::Data::Json::JsonValueType::Array)) queue.push_back(nested);
            }
        else if (item && item.ValueType() == winrt::Windows::Data::Json::JsonValueType::Array)
            for (const auto& child : item.GetArray()) if (queue.size() < 150) queue.push_back(Embedded(child));
    }
    return nullptr;
}
Value Message(const Value& payload) {
    if (auto message = Get(payload, L"message")) return Embedded(message);
    if (auto category = Get(payload, L"category"); category && Get(payload, L"payload")) return Embedded(Get(payload, L"payload"));
    return Embedded(payload);
}
std::wstring Text(const Value& value) {
    if (!value) return {};
    if (value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String) return std::wstring(value.GetString());
    if (value.ValueType() == winrt::Windows::Data::Json::JsonValueType::Number) return std::wstring(value.Stringify());
    return {};
}
std::optional<double> Number(const Value& value) {
    if (!value || value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number) return {};
    const auto number = value.GetNumber();
    return std::isfinite(number) ? std::optional<double>(number) : std::nullopt;
}
std::optional<double> Metric(const Value& value) {
    if (auto status = Get(value, L"status"); status && !Equal(Text(status), L"available")) return {};
    auto observed = Get(value, L"value");
    return Number(observed ? observed : value);
}
std::optional<int64_t> Timestamp(const Value& value) {
    if (auto number = Number(value); number && *number >= -8640000000000000.0 && *number <= 8640000000000000.0)
        return static_cast<int64_t>(*number);
    return IsoTime(Text(value));
}
std::wstring Type(const Value& message) {
    for (const auto& key : {L"type", L"kind", L"channel", L"event", L"action"})
        if (auto text = Text(Get(message, key)); !text.empty()) return text;
    return Text(Get(Get(message, L"header"), L"action"));
}
bool Contains(std::wstring_view text, std::wstring_view needle) {
    std::wstring left(text), right(needle);
    std::transform(left.begin(), left.end(), left.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    std::transform(right.begin(), right.end(), right.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return left.find(right) != std::wstring::npos;
}
void Warn(LogAnalysisResult& result, std::wstring warning) {
    if (result.Warnings.size() < 200) result.Warnings.push_back(std::move(warning));
}
void Normalize(LogAnalysisResult& result, std::filesystem::path source, size_t line, std::string text) {
    const int length = static_cast<int>(text.size());
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length, nullptr, 0) == 0)
        throw std::runtime_error("Invalid UTF-8");
    if (line == 1 && text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    const auto raw = json::JsonObject::Parse(json::Utf8ToWide(text));
    Value payload = Get(raw, L"payload"); if (!payload) payload = raw;
    auto message = Message(payload);
    auto envelope = Get(payload, L"version") ? payload : Get(payload, L"source");
    auto category = source.filename().wstring();
    category = category.substr(0, category.find(L'.'));
    const auto known = std::array{L"realtime", L"ipc", L"scripts", L"games", L"diagnostics", L"session"};
    if (std::none_of(known.begin(), known.end(), [&](const wchar_t* name) { return Equal(category, name); })) {
        category = Text(Get(payload, L"category"));
        const auto event = Text(Get(payload, L"event"));
        if (event == L"scriptCommunication") category = L"scripts";
        else if (event == L"gameSnapshot" || event == L"nativeApplicationInventory") category = L"games";
        else if (event == L"sessionStarted" || event == L"sessionEnded") category = L"session";
        if (category.empty() || category == L"state") category = L"diagnostics";
    }
    std::transform(category.begin(), category.end(), category.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    auto timestamp = Get(raw, L"recordedAtUtc");
    if (!timestamp) timestamp = Get(envelope, L"timestamp");
    if (!timestamp) timestamp = Get(Get(payload, L"snapshot"), L"capturedAtUtc");
    auto time = Timestamp(timestamp);
    if (!time) Warn(result, source.filename().wstring() + L":" + std::to_wstring(line) + L" has an unknown timestamp.");
    const auto secret = Get(raw, L"includeSecrets");
    const bool includesSecrets = secret && secret.ValueType() == winrt::Windows::Data::Json::JsonValueType::Boolean && secret.GetBoolean();
    result.Records.push_back({source, line, category, Type(message), Text(timestamp), Text(Get(envelope, L"direction")),
        source.parent_path().wstring(), time, std::move(text), includesSecrets});
    if (result.Records.back().Type.empty()) result.Records.back().Type = L"Record";
    if (includesSecrets) ++result.SecretRecords;
}
} // namespace pc::analysis
