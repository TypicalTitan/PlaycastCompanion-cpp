#include "pch.h"
#include "SessionLoggingVdf.h"
#include <cwctype>

namespace pc::sessionlog {
namespace {
class Parser {
public:
    explicit Parser(std::wstring_view text) : text_(text) {}
    VdfNode Read(int depth = 0, bool nested = false) {
        if (depth > 64) throw std::runtime_error("VDF nesting limit exceeded");
        VdfNode object;
        for (;;) {
            const auto key = Token();
            if (!key) { if (nested) throw std::runtime_error("Unclosed VDF object"); return object; }
            if (*key == L"}") { if (!nested) throw std::runtime_error("Unexpected VDF close"); return object; }
            if (*key == L"{") throw std::runtime_error("Missing VDF property name");
            const auto value = Token();
            if (!value || *value == L"}") throw std::runtime_error("Missing VDF property value");
            if (*value == L"{") object.children[*key] = Read(depth + 1, true);
            else object.children[*key].value = *value;
        }
    }
private:
    std::optional<std::wstring> Token() {
        Skip();
        if (index_ == text_.size()) return std::nullopt;
        const auto first = text_[index_++];
        if (first == L'{' || first == L'}') return std::wstring(1, first);
        std::wstring token;
        if (first != L'"') {
            token += first;
            while (index_ < text_.size() && !iswspace(text_[index_]) && text_[index_] != L'{' && text_[index_] != L'}') token += text_[index_++];
            return token;
        }
        while (index_ < text_.size()) {
            auto character = text_[index_++];
            if (character == L'"') return token;
            if (character == L'\\' && index_ < text_.size()) {
                const auto escaped = text_[index_];
                if (escaped == L'\\' || escaped == L'"') { character = escaped; ++index_; }
            }
            token += character;
        }
        throw std::runtime_error("Unclosed VDF string");
    }
    void Skip() {
        while (index_ < text_.size()) {
            if (iswspace(text_[index_])) { ++index_; continue; }
            if (text_[index_] == L'/' && index_ + 1 < text_.size() && text_[index_ + 1] == L'/') {
                while (index_ < text_.size() && text_[index_] != L'\n') ++index_;
                continue;
            }
            break;
        }
    }
    std::wstring_view text_;
    size_t index_ = 0;
};
}

const VdfNode& VdfNode::Child(std::wstring_view key) const {
    const auto found = children.find(std::wstring(key));
    if (found == children.end()) throw std::runtime_error("Required VDF property missing");
    return found->second;
}
VdfNode VdfNode::Parse(std::wstring_view text) { return Parser(text).Read(); }
}
