// dllmain.cpp : Defines the entry point for the DLL application.
#include <windows.h>

#include "eq_game.h"

// The .def file aliases this call to ordinal 1.
extern "C" void __stdcall InitializeEqwDll() {
  EqGame::Initialize();  // Installs hooks that install the windowing wrappers.
}

static constexpr char kVersionStr[] = "0.0.4";

// Provide a method for users to check version compatibility.
extern "C" const char* __stdcall GetVersionStr() {
  return kVersionStr;  // Parseable static const string.
}

// Provide a clean method to acquire a window handle. Note that this also
// gets returned in the primary createwindow and stored in the game
// global variable as well.
extern "C" HWND __stdcall GetGameWindow() {
  return EqGame::GetGameWindow();  // Primary game window.
}

// Returns the current state of the internal flag.
extern "C" int __stdcall GetEnableFullScreen() {
  return EqGame::GetEnableFullScreen();  // 0 = Windowed mode, 1 = Full screen mode.
}

// Performs a blocking SendMessage() call that will block this thread until the
// WndProc method on the main thread processes this message.
extern "C" void __stdcall SetEnableFullScreen(int enable) {
  EqGame::SetEnableFullScreen(enable);  // 0 = Windowed mode, 1 = Full screen mode.
}

// Sets a callback to execute immediately after the eqmain.dll is loaded into memory.
extern "C" void __stdcall SetEqMainInitFn(void(__cdecl* init_fn)()) {
  EqGame::SetEqMainInitFn(init_fn);  // May get called repeatedly.
}

// Sets a callback to execute immediately after the eqgfx_dx8.dll is loaded into memory.
// This happens before the primary game window is created (SetEqCreateWinInitFn).
extern "C" void __stdcall SetEqGfxInitFn(void(__cdecl* init_fn)()) {
  EqGame::SetEqGfxInitFn(init_fn);  // Called after eqw does it's hooks.
}

// Sets a callback to execute immediately after eqw creates the primary game window but
// before the hook has returned to the game code and populated the global window handle.
extern "C" void __stdcall SetEqCreateWinInitFn(void(__cdecl* init_fn)()) {
  EqGame::SetEqCreateWinInitFn(init_fn);  // Use GetGameWindow() if a handle is needed.
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  return TRUE;  // Do nothing.  The ordinal 1 call above initializes and it is never unloaded.
}
