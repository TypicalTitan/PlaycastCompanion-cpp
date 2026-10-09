#pragma once
#include <windows.h>

namespace pc {
/// Real-control diagnostic contracts; no installed instance or device side effects.
int RunUiScrollContracts(HINSTANCE instance);
}
