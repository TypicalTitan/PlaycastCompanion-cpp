#pragma once
#include "SessionLoggingInventory.h"

namespace pc::sessionlog {
class SnapshotSource {
public:
    virtual ~SnapshotSource() = default;
    virtual Object Capture(std::wstring_view target, bool includeDisconnected, std::stop_token stop) const;
};
struct ProcessIdentity { DWORD pid; std::wstring name; };
class ProcessSource {
public:
    virtual ~ProcessSource() = default;
    virtual std::vector<ProcessIdentity> Entries(std::stop_token stop) const = 0;
    virtual Value Session(const ProcessIdentity& process) const = 0;
    virtual Object Details(const ProcessIdentity& process) const = 0;
};
Object CaptureProcesses(const std::vector<DWORD>& targetSessions, const Inventory& inventory,
    std::stop_token stop, const ProcessSource* source = nullptr);
Object CaptureCompanion();
}
