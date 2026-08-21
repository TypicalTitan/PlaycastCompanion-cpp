#include "pch.h"
#include "Json.h"

#include <cstdint>
#include <cstring>

// Thin wrapper over winrt::Windows::Data::Json. The only additions are the
// lenient pre-pass (comments + trailing commas, which System.Text.Json was
// configured to skip in the C# app), std::runtime_error translation, UTF-8
// conversion, and a 2-space pretty printer (WinRT has none).

namespace pc::json {
namespace {

using winrt::Windows::Data::Json::IJsonValue;
using winrt::Windows::Data::Json::JsonValueType;

std::string HresultMessage(const winrt::hresult_error& e) {
    std::string msg = WideToUtf8(std::wstring_view(e.message()));
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' ' || msg.back() == '\t')) {
        msg.pop_back();
    }
    const auto code = static_cast<uint32_t>(e.code().value);
    if (msg.empty()) return std::format("HRESULT 0x{:08X}", code);
    return std::format("{} (0x{:08X})", msg, code);
}

bool IsJsonWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Pass 1: drop `//` line comments and `/* */` block comments outside string literals.
std::string StripComments(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    bool inString = false;
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (inString) {
            out += c;
            if (c == '\\' && i + 1 < in.size()) {
                out += in[++i];
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
            out += c;
            continue;
        }
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            i += 2;
            while (i < in.size() && in[i] != '\n') ++i;
            if (i < in.size()) out += '\n';  // keep the line break as whitespace
            continue;
        }
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            const size_t end = in.find("*/", i + 2);
            i = (end == std::string_view::npos) ? in.size() : end + 1;
            continue;
        }
        out += c;
    }
    return out;
}

// Pass 2: drop a comma whose next significant character closes an array/object.
std::string StripTrailingCommas(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    bool inString = false;
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (inString) {
            out += c;
            if (c == '\\' && i + 1 < in.size()) {
                out += in[++i];
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
            out += c;
            continue;
        }
        if (c == ',') {
            size_t j = i + 1;
            while (j < in.size() && IsJsonWhitespace(in[j])) ++j;
            if (j < in.size() && (in[j] == ']' || in[j] == '}')) continue;
        }
        out += c;
    }
    return out;
}

unsigned char ByteAt(std::string_view bytes, size_t i) {
    return static_cast<unsigned char>(bytes[i]);
}

// UTF-16 payload (BOM already removed) -> UTF-8. Copies into a wstring rather
// than reinterpreting the char buffer (alignment); an odd trailing byte is dropped.
std::string Utf16BytesToUtf8(std::string_view payload, bool bigEndian) {
    const size_t units = payload.size() / 2;
    std::wstring wide(units, L'\0');
    if (units != 0) std::memcpy(wide.data(), payload.data(), units * 2);
    if (bigEndian) {
        for (wchar_t& u : wide) {
            const uint16_t v = static_cast<uint16_t>(u);
            u = static_cast<wchar_t>(static_cast<uint16_t>((v >> 8) | (v << 8)));
        }
    }
    return WideToUtf8(wide);
}

// UTF-32 payload (BOM already removed) -> UTF-8. Invalid scalars (surrogates,
// > U+10FFFF) become U+FFFD, as .NET's decoder would; a trailing partial unit is dropped.
std::string Utf32BytesToUtf8(std::string_view payload, bool bigEndian) {
    const size_t units = payload.size() / 4;
    std::wstring wide;
    wide.reserve(units);
    for (size_t i = 0; i < units; ++i) {
        const size_t p = i * 4;
        uint32_t cp = 0;
        if (bigEndian) {
            cp = (static_cast<uint32_t>(ByteAt(payload, p)) << 24) | (static_cast<uint32_t>(ByteAt(payload, p + 1)) << 16) |
                 (static_cast<uint32_t>(ByteAt(payload, p + 2)) << 8) | static_cast<uint32_t>(ByteAt(payload, p + 3));
        } else {
            cp = static_cast<uint32_t>(ByteAt(payload, p)) | (static_cast<uint32_t>(ByteAt(payload, p + 1)) << 8) |
                 (static_cast<uint32_t>(ByteAt(payload, p + 2)) << 16) | (static_cast<uint32_t>(ByteAt(payload, p + 3)) << 24);
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            wide.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            wide.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            wide.push_back(static_cast<wchar_t>(cp));
        }
    }
    return WideToUtf8(wide);
}

std::wstring Preprocess(std::string_view utf8) {
    // A BOM (as File.ReadAllText would silently strip) is not valid JSON; the
    // config/cache files may also be UTF-16/32 (PowerShell 5.1 Out-File, Notepad).
    const std::string decoded = DecodeTextFile(utf8);
    return Utf8ToWide(StripTrailingCommas(StripComments(decoded)));
}

void WriteIndent(std::wstring& out, int depth) {
    for (int i = 0; i < depth; ++i) out += L"  ";
}

void WriteValue(const IJsonValue& v, int depth, std::wstring& out);

void WriteObject(const JsonObject& o, int depth, std::wstring& out) {
    if (!o || o.Size() == 0) {
        out += L"{}";
        return;
    }
    out += L"{\n";
    bool first = true;
    for (const auto& kv : o) {
        if (!first) out += L",\n";
        first = false;
        WriteIndent(out, depth + 1);
        out += JsonValue::CreateStringValue(kv.Key()).Stringify();
        out += L": ";
        WriteValue(kv.Value(), depth + 1, out);
    }
    out += L'\n';
    WriteIndent(out, depth);
    out += L'}';
}

void WriteArray(const JsonArray& a, int depth, std::wstring& out) {
    if (!a || a.Size() == 0) {
        out += L"[]";
        return;
    }
    out += L"[\n";
    bool first = true;
    for (const auto& item : a) {
        if (!first) out += L",\n";
        first = false;
        WriteIndent(out, depth + 1);
        WriteValue(item, depth + 1, out);
    }
    out += L'\n';
    WriteIndent(out, depth);
    out += L']';
}

void WriteValue(const IJsonValue& v, int depth, std::wstring& out) {
    if (!v) {
        out += L"null";
        return;
    }
    switch (v.ValueType()) {
        case JsonValueType::Object:
            WriteObject(v.GetObjectW(), depth, out);
            break;
        case JsonValueType::Array:
            WriteArray(v.GetArray(), depth, out);
            break;
        default:
            // null / boolean / number / string: WinRT already emits valid JSON text.
            out += v.Stringify();
            break;
    }
}

// Returns the value for `key` only if present and of the requested type.
IJsonValue LookupTyped(const JsonObject& o, std::wstring_view key, JsonValueType type) {
    if (!o) return nullptr;
    const winrt::hstring k(key);
    if (!o.HasKey(k)) return nullptr;
    IJsonValue v = o.Lookup(k);
    if (!v || v.ValueType() != type) return nullptr;
    return v;
}

}  // namespace

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    if (utf8.size() > static_cast<size_t>(INT_MAX)) throw std::runtime_error("UTF-8 text too long to convert");
    const int len = static_cast<int>(utf8.size());
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), len, nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), len, out.data(), needed);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    if (wide.size() > static_cast<size_t>(INT_MAX)) throw std::runtime_error("UTF-16 text too long to convert");
    const int len = static_cast<int>(wide.size());
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, wide.data(), len, out.data(), needed, nullptr, nullptr);
    if (written <= 0) return {};
    out.resize(static_cast<size_t>(written));
    return out;
}

std::string DecodeTextFile(std::string_view bytes) {
    const size_t n = bytes.size();
    if (n >= 3 && ByteAt(bytes, 0) == 0xEF && ByteAt(bytes, 1) == 0xBB && ByteAt(bytes, 2) == 0xBF) {
        return std::string(bytes.substr(3));
    }
    // UTF-32 LE must be tested before UTF-16 LE (shared FF FE prefix), as StreamReader does.
    if (n >= 4 && ByteAt(bytes, 0) == 0xFF && ByteAt(bytes, 1) == 0xFE && ByteAt(bytes, 2) == 0x00 &&
        ByteAt(bytes, 3) == 0x00) {
        return Utf32BytesToUtf8(bytes.substr(4), false);
    }
    if (n >= 4 && ByteAt(bytes, 0) == 0x00 && ByteAt(bytes, 1) == 0x00 && ByteAt(bytes, 2) == 0xFE &&
        ByteAt(bytes, 3) == 0xFF) {
        return Utf32BytesToUtf8(bytes.substr(4), true);
    }
    if (n >= 2 && ByteAt(bytes, 0) == 0xFF && ByteAt(bytes, 1) == 0xFE) {
        return Utf16BytesToUtf8(bytes.substr(2), false);
    }
    if (n >= 2 && ByteAt(bytes, 0) == 0xFE && ByteAt(bytes, 1) == 0xFF) {
        return Utf16BytesToUtf8(bytes.substr(2), true);
    }
    return std::string(bytes);
}

JsonObject Parse(std::string_view utf8) {
    const std::wstring text = Preprocess(utf8);
    try {
        return JsonObject::Parse(winrt::hstring(text));
    } catch (const winrt::hresult_error& e) {
        throw std::runtime_error("JSON object parse failed: " + HresultMessage(e));
    }
}

JsonArray ParseArray(std::string_view utf8) {
    const std::wstring text = Preprocess(utf8);
    try {
        return JsonArray::Parse(winrt::hstring(text));
    } catch (const winrt::hresult_error& e) {
        throw std::runtime_error("JSON array parse failed: " + HresultMessage(e));
    }
}

std::string Stringify(const JsonObject& obj) {
    if (!obj) return "{}";
    try {
        return WideToUtf8(std::wstring_view(obj.Stringify()));
    } catch (const winrt::hresult_error& e) {
        throw std::runtime_error("JSON stringify failed: " + HresultMessage(e));
    }
}

std::string StringifyIndented(const JsonObject& obj) {
    try {
        std::wstring out;
        WriteObject(obj, 0, out);
        return WideToUtf8(out);
    } catch (const winrt::hresult_error& e) {
        throw std::runtime_error("JSON stringify failed: " + HresultMessage(e));
    }
}

std::wstring GetString(const JsonObject& o, std::wstring_view key, std::wstring_view def) {
    try {
        if (IJsonValue v = LookupTyped(o, key, JsonValueType::String)) {
            return std::wstring(std::wstring_view(v.GetString()));
        }
    } catch (const winrt::hresult_error&) {
    }
    return std::wstring(def);
}

bool GetBool(const JsonObject& o, std::wstring_view key, bool def) {
    try {
        if (IJsonValue v = LookupTyped(o, key, JsonValueType::Boolean)) {
            return v.GetBoolean();
        }
    } catch (const winrt::hresult_error&) {
    }
    return def;
}

double GetNumber(const JsonObject& o, std::wstring_view key, double def) {
    try {
        if (IJsonValue v = LookupTyped(o, key, JsonValueType::Number)) {
            return v.GetNumber();
        }
    } catch (const winrt::hresult_error&) {
    }
    return def;
}

JsonObject GetObject(const JsonObject& o, std::wstring_view key) {
    try {
        if (IJsonValue v = LookupTyped(o, key, JsonValueType::Object)) {
            return v.GetObjectW();
        }
    } catch (const winrt::hresult_error&) {
    }
    return JsonObject();
}

bool Has(const JsonObject& o, std::wstring_view key) {
    try {
        return o && o.HasKey(winrt::hstring(key));
    } catch (const winrt::hresult_error&) {
        return false;
    }
}

}  // namespace pc::json
