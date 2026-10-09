#pragma once
#include "SessionLoggingJson.h"

namespace pc::sessionlog {
class Redactor {
public:
    Value Redact(const Value& value, bool includeSecrets) const;
    std::wstring RedactText(std::wstring text) const;
private:
    Value Visit(const Value& value, int depth) const;
};
}
