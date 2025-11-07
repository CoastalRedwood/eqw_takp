#pragma once
#include <windows.h>

// Installs the hooks to allow windowed mode during eqmain.dll execution.
// The HWND window is shared shared with the primary eqgame.exe and both
// eqmain.dll and eqmain.exe share the use of dinput_manager's resources.
// A custom wndproc handler is installed between the eqmain's CreateWindow
// and DestroyWindow calls.

namespace EqMain {
void Initialize(HMODULE handle, HWND hWnd);
}  // namespace EqMain
