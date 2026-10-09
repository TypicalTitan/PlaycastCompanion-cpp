#include "pch.h"
#include "SessionLoggingRedactor.h"
#include <cwctype>

namespace pc::sessionlog {
namespace {
bool Sensitive(std::wstring name) {
    std::wstring normal;
    for (const auto character : name) if (iswalnum(character)) normal += static_cast<wchar_t>(towlower(character));
    for (const auto word : {L"password", L"passwd", L"pwd", L"secret", L"credential", L"token", L"authorization",
        L"authentication", L"cookie", L"privatekey", L"apikey", L"accesskey", L"connectionstring", L"clientkey", L"signingkey", L"encryptionkey"})
        if (normal.find(word) != std::wstring::npos) return true;
    return normal == L"auth" || normal == L"key";
}
}

Value Redactor::Redact(const Value& value, bool includeSecrets) const {
    if (includeSecrets) return json::JsonValue::Parse(value.Stringify());
    try { return Visit(value, 0); }
    catch (...) { return Text(L"[REDACTED: payload could not be safely processed]"); }
}

Value Redactor::Visit(const Value& value, int depth) const {
    if (!value || depth > 64) return Text(L"[REDACTED: nested payload depth limit]");
    if (value.ValueType() == Type::Object) {
        const auto input = value.GetObject();
        Object output;
        const auto header = json::GetObject(input, L"header");
        auto action = json::GetString(header, L"action", json::GetString(input, L"action"));
        std::transform(action.begin(), action.end(), action.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        const bool secure = action.find(L"securestorage") != std::wstring::npos;
        for (const auto& property : input) {
            const std::wstring name(property.Key());
            const bool policy = name == L"includeSecrets" && property.Value().ValueType() == Type::Boolean;
            const bool secureValue = secure && (name == L"value" || name == L"body" || name == L"message");
            output.Insert(property.Key(), ((Sensitive(name) && !policy) || secureValue)
                ? Text(L"[REDACTED]") : Visit(property.Value(), depth + 1));
        }
        return output;
    }
    if (value.ValueType() == Type::Array) {
        Array output;
        for (const auto& item : value.GetArray()) output.Append(Visit(item, depth + 1));
        return output;
    }
    if (value.ValueType() != Type::String) return value;
    std::wstring text(value.GetString());
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first != std::wstring::npos && text.size() <= 1024 * 1024 && (text[first] == L'{' || text[first] == L'[' || text[first] == L'"')) {
        json::JsonValue embedded{ nullptr };
        if (json::JsonValue::TryParse(winrt::hstring(text), embedded))
            return Text(std::wstring(Visit(embedded, depth + 1).Stringify()));
    }
    return Text(RedactText(std::move(text)));
}

}
