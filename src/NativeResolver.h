#pragma once
// Locates vendor SDK DLLs that are NOT redistributed with the app: a copy
// dropped next to the exe first, then the vendor software's own install.
#include <string_view>
#include <windows.h>

namespace pc::native {
/// logicalName is "LOGI_LED" (Logitech LED Illumination SDK) or "CUESDK"
/// (Corsair CUE SDK v3). Returns a loaded module or nullptr if no candidate
/// exists. Modules are cached for the process lifetime; never FreeLibrary them.
HMODULE LoadVendorLibrary(std::wstring_view logicalName);
}  // namespace pc::native
