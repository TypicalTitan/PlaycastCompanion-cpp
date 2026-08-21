#pragma once
// Thin, dependency-free JSON helpers over the Windows Runtime JSON API
// (winrt::Windows::Data::Json). All text crossing this boundary is UTF-8
// std::string on the wire side and UTF-16 std::wstring on the app side.
#include <string>
#include <string_view>
#include <winrt/Windows.Data.Json.h>

namespace pc::json {
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

std::wstring Utf8ToWide(std::string_view utf8);
std::string WideToUtf8(std::wstring_view wide);

/// Decode the raw bytes of a text file the way .NET's File.ReadAllText does:
/// detect a UTF-8 / UTF-16 LE / UTF-16 BE / UTF-32 LE / UTF-32 BE byte-order
/// mark, strip it and transcode the payload to UTF-8. Bytes without a BOM are
/// returned unchanged (no BOM-less sniffing, matching StreamReader).
std::string DecodeTextFile(std::string_view bytes);

/// Parse UTF-8 JSON text into an object. Tolerates `//` line comments and
/// trailing commas (the shipped config.json uses both). Throws
/// std::runtime_error with a readable message on failure.
JsonObject Parse(std::string_view utf8);

/// Parse UTF-8 JSON text into an array; same tolerance/throw rules.
JsonArray ParseArray(std::string_view utf8);

std::string Stringify(const JsonObject& obj);          // compact UTF-8
std::string StringifyIndented(const JsonObject& obj);  // 2-space indented UTF-8

// Tolerant getters: return the default when the key is missing or has the wrong type.
std::wstring GetString(const JsonObject& o, std::wstring_view key, std::wstring_view def = L"");
bool GetBool(const JsonObject& o, std::wstring_view key, bool def);
double GetNumber(const JsonObject& o, std::wstring_view key, double def);
JsonObject GetObject(const JsonObject& o, std::wstring_view key);  // empty object when missing
bool Has(const JsonObject& o, std::wstring_view key);
}  // namespace pc::json
