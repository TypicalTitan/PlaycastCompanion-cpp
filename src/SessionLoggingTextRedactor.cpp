#include "pch.h"
#include "SessionLoggingRedactor.h"
#include <cwctype>

namespace pc::sessionlog {
namespace {
bool NameCharacter(wchar_t character) { return iswalnum(character) || character == L'_' || character == L'$' || character == L'-' || character == L'.'; }
bool SensitiveWord(std::wstring_view name) {
    std::wstring normal;
    for (const auto character : name) if (iswalnum(character)) normal += static_cast<wchar_t>(towlower(character));
    for (const auto word : {L"password", L"passwd", L"pwd", L"secret", L"credential", L"token", L"authorization", L"cookie",
        L"apikey", L"accesskey", L"privatekey", L"clientkey", L"connectionstring", L"signingkey", L"encryptionkey", L"signature"})
        if (normal.find(word) != std::wstring::npos) return true;
    return normal == L"auth" || normal == L"key" || normal == L"sig";
}
size_t BlankEnd(std::wstring_view text, size_t index) {
    while (index < text.size() && iswspace(text[index])) ++index;
    return index;
}
size_t PlainEnd(std::wstring_view text, size_t index) {
    while (index < text.size() && !iswspace(text[index]) && text[index] != L';' && text[index] != L',' && text[index] != L'&' && text[index] != L'}') ++index;
    return index;
}
size_t SchemeEnd(std::wstring_view lower, size_t index) {
    for (const auto scheme : {std::wstring_view(L"bearer"), std::wstring_view(L"basic")}) {
        const auto end = index + scheme.size();
        if (end < lower.size() && lower.substr(index, scheme.size()) == scheme && iswspace(lower[end])) return end;
    }
    return index;
}
size_t ValueEnd(std::wstring_view text, std::wstring_view lower, size_t index) {
    if (index == text.size()) return index;
    if (text[index] == L'@' && index + 1 < text.size() && (text[index + 1] == L'\'' || text[index + 1] == L'"')) {
        const auto closing = std::wstring(L"\n") + text[index + 1] + L"@";
        const auto end = text.find(closing, index + 2);
        return end == std::wstring::npos ? text.size() : end + closing.size();
    }
    if (text[index] == L'"' || text[index] == L'\'') {
        const auto quote = text[index++];
        while (index < text.size()) {
            if ((text[index] == L'`' || text[index] == L'\\') && index + 1 < text.size()) { index += 2; continue; }
            if (text[index++] != quote) continue;
            if (quote == L'\'' && index < text.size() && text[index] == quote) { ++index; continue; }
            return index;
        }
        return text.size();
    }
    const auto scheme = SchemeEnd(lower, index);
    if (scheme != index) {
        const auto token = BlankEnd(text, scheme);
        return PlainEnd(text, token);
    }
    return PlainEnd(text, index);
}
size_t PrivateKeyEnd(std::wstring_view lower, size_t index) {
    if (lower.substr(index, 11) != L"-----begin ") return index;
    const auto headerOffset = lower.substr(index + 11, 100).find(L"-----");
    if (headerOffset == std::wstring::npos) return index;
    const auto header = index + 11 + headerOffset;
    if (lower.substr(index, header - index).find(L"private key") == std::wstring::npos) return index;
    const auto footer = lower.find(L"-----end ", header + 5);
    if (footer == std::wstring::npos) return lower.size();
    const auto end = lower.find(L"-----", footer + 9);
    return end == std::wstring::npos ? lower.size() : end + 5;
}
}

std::wstring Redactor::RedactText(std::wstring text) const {
    if (text.size() > 1024 * 1024) return L"[REDACTED: text exceeded diagnostic frame limit]";
    std::wstring lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    std::wstring output;
    output.reserve(text.size());
    for (size_t index = 0; index < text.size();) {
        const auto keyEnd = PrivateKeyEnd(lower, index);
        if (keyEnd != index) { output += L"[REDACTED]"; index = keyEnd; continue; }
        const auto scheme = SchemeEnd(lower, index);
        if ((index == 0 || !NameCharacter(text[index - 1])) && scheme != index) {
            const auto token = BlankEnd(text, scheme);
            output.append(text, index, token - index);
            output += L"[REDACTED]";
            index = PlainEnd(text, token);
            continue;
        }
        if (text.compare(index, 3, L"eyJ") == 0) {
            auto end = index;
            int dots = 0;
            while (end < text.size() && (iswalnum(text[end]) || text[end] == L'_' || text[end] == L'-' || text[end] == L'.')) { dots += text[end] == L'.'; ++end; }
            if (dots >= 2) { output += L"[REDACTED]"; index = end; continue; }
        }
        if (!NameCharacter(text[index])) { output += text[index++]; continue; }
        auto end = index;
        while (end < text.size() && NameCharacter(text[end])) ++end;
        if (end + 2 < text.size() && text.substr(end, 3) == L"://") {
            const auto begin = end + 3;
            const auto boundary = text.find_first_of(L"/?# \t\r\n", begin);
            const auto authorityEnd = boundary == std::wstring::npos ? text.size() : boundary;
            const auto authority = std::wstring_view(text).substr(begin, authorityEnd - begin);
            const auto at = authority.find(L'@');
            if (at != std::wstring::npos) {
                output.append(text, index, begin - index); output += L"[REDACTED]@"; index = begin + at + 1; continue;
            }
        }
        auto separator = end;
        if (index > 0 && (text[index - 1] == L'"' || text[index - 1] == L'\'') && separator < text.size() && text[separator] == text[index - 1]) ++separator;
        separator = BlankEnd(text, separator);
        const bool assignment = separator < text.size() && (text[separator] == L'=' || text[separator] == L':');
        const bool argument = end < separator && (text[index] == L'-' || (index > 0 && text[index - 1] == L'/'));
        if (SensitiveWord(std::wstring_view(text).substr(index, end - index)) && (assignment || argument)) {
            const auto value = BlankEnd(text, assignment ? separator + 1 : separator);
            const auto valueEnd = ValueEnd(text, lower, value);
            output.append(text, index, value - index); output += L"[REDACTED]"; index = valueEnd; continue;
        }
        output.append(text, index, end - index);
        index = end;
    }
    return output;
}
}
