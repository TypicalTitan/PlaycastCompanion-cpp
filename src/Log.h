#pragma once
#include <string>
#include <string_view>

namespace pc {
/// Append a timestamped line to %LOCALAPPDATA%\PlaycastCompanion\playcast-companion.log
/// (rotates to playcast-companion.old.log above 1 MB; falls back to %TEMP% if the
/// primary path is unwritable). Thread-safe. Never throws, never shows UI.
void LogInfo(std::wstring_view message);
void LogInfo(std::string_view utf8Message);

/// Full path of the primary log file.
std::wstring LogFilePath();
}  // namespace pc
