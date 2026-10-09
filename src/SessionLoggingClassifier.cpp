#include "pch.h"
#include "SessionLoggingClassifier.h"
#include <cwctype>

namespace pc::sessionlog {
namespace {
std::wstring Names(const Object& message) {
    std::wstring names;
    for (const auto field : {L"type", L"action", L"event", L"method", L"operation", L"channel"}) names += L" " + json::GetString(message, field);
    for (const auto node : {json::GetObject(message, L"header"), json::GetObject(message, L"message"),
        json::GetObject(json::GetObject(message, L"body"), L"message")}) names += L" " + json::GetString(node, L"action");
    std::transform(names.begin(), names.end(), names.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return names;
}
Object Related(const Object& envelope, const Object& message, std::wstring_view event) {
    Object source;
    for (const auto& property : envelope) if (property.Key() != L"payload") source.Insert(property.Key(), property.Value());
    return Make({{L"event", Text(event)}, {L"source", source}, {L"message", message}});
}
}

std::vector<std::pair<std::wstring, Object>> EventClassifier::RelatedRecords(const Object& envelope) {
    std::vector<std::pair<std::wstring, Object>> records;
    Walk(envelope, Get(envelope, L"payload"), 0, records);
    return records;
}

void EventClassifier::Walk(const Object& envelope, const Value& value, int depth, std::vector<std::pair<std::wstring, Object>>& records) {
    if (!value || depth > 32) return;
    if (value.ValueType() == Type::String) {
        json::JsonValue embedded{nullptr};
        if (json::JsonValue::TryParse(value.GetString(), embedded)) Walk(envelope, embedded, depth + 1, records);
    } else if (value.ValueType() == Type::Array) {
        for (const auto& child : value.GetArray()) Walk(envelope, child, depth + 1, records);
    } else if (value.ValueType() == Type::Object) {
        const auto message = value.GetObject();
        const auto names = Names(message);
        const auto previousCount = records.size();
        if (names.find(L"provisioning_script_") != std::wstring::npos || names.find(L"powershell") != std::wstring::npos
            || names.find(L"cancelprovisioningscript") != std::wstring::npos || json::Has(message, L"completeScript") || json::Has(message, L"script"))
            records.emplace_back(L"scripts", Related(envelope, message, L"scriptCommunication"));
        if (names.find(L"enumapplications") != std::wstring::npos || names.find(L"enumrunning") != std::wstring::npos
            || names.find(L"listapplications") != std::wstring::npos || names.find(L"runningapplications") != std::wstring::npos
            || names.find(L"processcatalogstate") != std::wstring::npos || names.find(L"runningapplicationstate") != std::wstring::npos || names.find(L"playjectorhostmodestate") != std::wstring::npos)
            records.emplace_back(L"games", Related(envelope, message, L"nativeApplicationInventory"));
        if (names.find(L"modechanged") != std::wstring::npos || names.find(L"playjectorhostmodestate") != std::wstring::npos)
            records.emplace_back(L"session", Related(envelope, message, L"nativeModeChanged"));
        if (records.size() != previousCount || json::Has(message, L"header")) return;
        for (const auto& property : message) Walk(envelope, property.Value(), depth + 1, records);
    }
}
}
