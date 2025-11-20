#pragma once
#include <windows.h>

#include <functional>

// Installs the hooks to allow windowed mode while using eqgfx_dx8.dll.

namespace EqGfx {
static constexpr int kDeviceLostMsgId = 0x4645;  // Custom WM_USER message ID.

void Initialize(HMODULE handle, void(__cdecl* init_fn)(),
                std::function<void(int width, int height)> set_client_size_callback);

void SetWindow(HWND hwnd);

void HandleDeviceLost(bool force_reset);  // Attempts to recover the device. Must call from wndproc.
}  // namespace EqGfx
