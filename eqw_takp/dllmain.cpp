// dllmain.cpp : Defines the entry point for the DLL application.
#include <windows.h>

#include "eq_game.h"

// The .def file aliases this call to ordinal 1.
extern "C" __declspec(dllexport) void __stdcall InitializeEqwDll() {
  EqGame::Initialize();  // Installs hooks that install the windowing wrappers.
}

// Provide a clean method to acquire a window handle.
extern "C" __declspec(dllexport) HWND __stdcall GetGameWindow() {
  return EqGame::GetGameWindow();  // Primary game window.
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  return TRUE;  // Do nothing.  The ordinal 1 call above initializes and it is never unloaded.
}
