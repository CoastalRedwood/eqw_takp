#pragma once
#include <windows.h>

#include <functional>

// Installs the hooks to allow windowed mode while using eqgfx_dx8.dll.

namespace EqGfx {
void Initialize(HMODULE handle, void(__cdecl* init_fn)(),
                std::function<void(int width, int height)> set_client_size_callback);
void SetWindow(HWND hwnd);
}  // namespace EqGfx
