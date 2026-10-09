#pragma once
#include "Json.h"
#include <initializer_list>

namespace pc::sessionlog {
using Value = winrt::Windows::Data::Json::IJsonValue;
using Type = winrt::Windows::Data::Json::JsonValueType;
using Object = json::JsonObject;
using Array = json::JsonArray;
inline Value Text(std::wstring_view text) { return json::JsonValue::CreateStringValue(winrt::hstring(text)); }
inline Value Number(double number) { return json::JsonValue::CreateNumberValue(number); }
inline Value Boolean(bool flag) { return json::JsonValue::CreateBooleanValue(flag); }
inline Value Null() { return json::JsonValue::CreateNullValue(); }
inline Object Make(std::initializer_list<std::pair<std::wstring_view, Value>> properties) {
    Object result;
    for (const auto& [name, value] : properties) result.Insert(winrt::hstring(name), value);
    return result;
}
inline Value Get(const Object& object, std::wstring_view name) {
    return object.HasKey(winrt::hstring(name)) ? object.Lookup(winrt::hstring(name)) : Null();
}
inline Value Metric(Value value, std::wstring_view status = L"available", std::wstring_view error = L"") {
    return Make({{L"Value", value}, {L"Status", Text(status)}, {L"Error", error.empty() ? Null() : Text(error)}});
}
std::wstring UtcNow();
std::wstring CurrentSid();
std::wstring AccountSid(std::wstring_view account);
void ProtectDirectory(const std::filesystem::path& directory);
bool VerifyPipeClient(HANDLE pipe, std::wstring_view target, std::wstring& error);
std::wstring PipeSecurity(std::wstring_view target);
}
