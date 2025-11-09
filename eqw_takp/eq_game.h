#pragma once
#include <windows.h>

// Wrapper for the eqgame.exe executable. It installs the hooks and support functions to allow windowed mode
// and creates the shared window and dinput objects also used by the eqmain.dll code.

namespace EqGame {
void Initialize();
HWND GetGameWindow();

int GetEnableFullScreen();
void SetEnableFullScreen(int enable);
void SetEqMainInitFn(void(__cdecl* init_fn)());
void SetEqGfxInitFn(void(__cdecl* init_fn)());

}  // namespace EqGame
