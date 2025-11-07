#pragma once
#include <windows.h>

// Wrapper for the eqgame.exe executable. It installs the hooks and support functions to allow windowed mode
// and creates the shared window and dinput objects also used by the eqmain.dll code.

namespace EqGame {
void Initialize();
HWND GetGameWindow();
}  // namespace EqGame
