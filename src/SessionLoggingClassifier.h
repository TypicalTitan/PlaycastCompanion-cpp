#pragma once
#include "SessionLoggingJson.h"

namespace pc::sessionlog {
class EventClassifier {
public:
    static std::vector<std::pair<std::wstring, Object>> RelatedRecords(const Object& envelope);
private:
    static void Walk(const Object& envelope, const Value& message, int depth, std::vector<std::pair<std::wstring, Object>>& records);
};
}
