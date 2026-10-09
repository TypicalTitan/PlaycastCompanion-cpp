#pragma once
#include "SessionLoggingInventory.h"

namespace pc::sessionlog {
class SnapshotSource {
public:
    Object Capture(std::wstring_view target, bool includeDisconnected, std::stop_token stop) const;
};
Object CaptureProcesses(const std::vector<DWORD>& targetSessions, const Inventory& inventory, std::stop_token stop);
Object CaptureCompanion();
}
