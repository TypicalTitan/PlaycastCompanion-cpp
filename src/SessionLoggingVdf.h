#pragma once
#include <map>
#include <string>
#include <string_view>

namespace pc::sessionlog {
struct VdfNode {
    std::wstring value;
    std::map<std::wstring, VdfNode> children;
    const VdfNode& Child(std::wstring_view key) const;
    static VdfNode Parse(std::wstring_view text);
};
}
