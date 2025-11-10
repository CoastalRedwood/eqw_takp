#include "eq_gfx.h"

#include "d3dx8/d3d8.h"
#include "iat_hook.h"
#include "logger.h"
#include "vtable_hook.h"

// Notes:
// - This always runs in windowed mode so a custom gamma mode is not supported.

// Using an EqGfxInt namespace instead of a purely static class to reduce the qualifier clutter. The
// anonymous namespace forces it to private internal scope.
namespace EqGfxInt {
namespace {

HWND hwnd_ = nullptr;                                                      // Shared common window.
std::function<void(int width, int height)> set_client_size_cb_ = nullptr;  // Updates the game when res changes.

// EqGfx_dx.dll hooks installed when the library is loaded.
IATHook hook_AdjustWindowRect_;
IATHook hook_SetCapture_;
IATHook hook_SetCursor_;
IATHook hook_SetWindowLongA_;
IATHook hook_SetWindowPos_;
IATHook hook_Direct3DCreate8_;

// Direct3D hooks installed when the device is created.
VTableHook hook_CreateDevice_;
VTableHook hook_Reset_;

IDirect3DDevice8* device_;
D3DPRESENT_PARAMETERS present_;

// Internal methods

// The reset hook is used to override the game and remain in windowed mode as well as perform
// the callback to inform the eqw windowing system when the resolution is changing.
HRESULT WINAPI D3DDeviceResetHook(IDirect3DDevice8* Device, D3DPRESENT_PARAMETERS* Parameters) {
  // EqW overrides the presentation parameters to alway remain in windowed mode. The outer code
  // logic in eqgame will toggle window styles as needed.
  if (!Parameters->Windowed) {
    Parameters->Windowed = true;
    Parameters->FullScreen_PresentationInterval = 0;
    Parameters->FullScreen_RefreshRateInHz = 0;
  }
  HRESULT result = EqGfxInt::hook_Reset_.original(D3DDeviceResetHook)(Device, Parameters);
  if (SUCCEEDED(result)) {
    Logger::Info("EqGFX: Reset: %d x %d", Parameters->BackBufferWidth, Parameters->BackBufferHeight);
    set_client_size_cb_(Parameters->BackBufferWidth, Parameters->BackBufferHeight);
    // SetEqMainSurfaceResolution(Parameters->BackBufferWidth, Parameters->BackBufferHeight);
  }
  return result;
}

HRESULT WINAPI D3D8CreateDeviceHook(IDirect3D8* pD3D, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
                                    IDirect3DDevice8** ppReturnedDeviceInterface) {
  // Don't interfere with any devices hooked to the non-game window (from custom add-ons).
  if (hFocusWindow != hwnd_) {
    Logger::Info("EqGfx: D3D device created for non-primary window 0x%x", (DWORD)hFocusWindow);
    return hook_CreateDevice_.original(D3D8CreateDeviceHook)(pD3D, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                                             pPresentationParameters, ppReturnedDeviceInterface);
  }

  Logger::Info("EqGfx: Create device with format %d", present_.BackBufferFormat);
  pPresentationParameters->Windowed = true;
  pPresentationParameters->hDeviceWindow = hwnd_;
  Logger::Info("Create device hook %d", present_.BackBufferFormat);
  HRESULT result = hook_CreateDevice_.original(D3D8CreateDeviceHook)(
      pD3D, Adapter, DeviceType, hwnd_, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
  present_ = *pPresentationParameters;
  if (SUCCEEDED(result)) {
    Logger::Info("EqGFX: D3D8CreateDeviceHook: %d x %d", present_.BackBufferWidth, present_.BackBufferHeight);
    void** vtable = *(void***)*ppReturnedDeviceInterface;
    hook_Reset_ = VTableHook(vtable, 14, D3DDeviceResetHook, false);
    set_client_size_cb_(present_.BackBufferWidth, present_.BackBufferHeight);

    device_ = *ppReturnedDeviceInterface;
  }
  return result;
}

// Hook the device creation to install further hooks.
IDirect3D8* WINAPI D3D8Direct3DCreate8Hook(UINT SDK) {
  Logger::Info("EqGFX: Direct3DCreate8 -- SDK Version: %d", SDK);
  IDirect3D8* rval = hook_Direct3DCreate8_.original(D3D8Direct3DCreate8Hook)(SDK);
  void** vtable = *(void***)rval;
  hook_CreateDevice_ = VTableHook(vtable, 15, D3D8CreateDeviceHook, false);

  return rval;
}

// Override the style by fetching the active window style so the SetWindowPos call is accurate.
BOOL WINAPI User32AdjustWindowRectHook(LPRECT lpRect, DWORD dwStyle, BOOL bMenu) {
  Logger::Info("EqGfx: Overriding window style in AdjustWindowRect");
  dwStyle = ::GetWindowLongA(hwnd_, GWL_STYLE);
  return hook_AdjustWindowRect_.original(User32AdjustWindowRectHook)(lpRect, dwStyle, bMenu);
}

// Block the client from ever trying to capture the mouse.
HWND WINAPI User32SetCaptureHook(HWND hWnd) {
  Logger::Info("EqGfx: Blocking SetCapture");
  return NULL;
}

// Block the disabling of the IDC_ARROW cursor by EqGfx.
HCURSOR WINAPI User32SetCursorHook(HCURSOR hcursor) {
  Logger::Info("EqGfx: Blocking SetCursor %d", (DWORD)hcursor);
  return hcursor;
}

// Block setting different window styles.
LONG WINAPI User32SetWindowLongAHook(HWND wnd, int index, long dwNewLong) {
  Logger::Info("EqGfx: Blocking SetWindowLong: %d, 0x%x", index, dwNewLong);
  return GetWindowLongA(wnd, index);
}

// This hook is just for debug / logging. Could possibly just block this and adjustrect completely.
HRESULT WINAPI User32SetWindowPosHook(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags) {
  Logger::Info("EqGfx: SetWindowPos: %d %d %d %d", X, Y, cx, cy);
  return hook_SetWindowPos_.original(User32SetWindowPosHook)(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
  return S_FALSE;
}

// Initializes state and installs the initial hooks into the dll.
void InitializeEqGfx(HMODULE handle, void(__cdecl* init_fn)(),
                     std::function<void(int width, int height)> set_client_size_callback) {
  // base = DWORD(handle);
  hwnd_ = nullptr;  // This must be set later with SetWindow() before more active use.
  set_client_size_cb_ = set_client_size_callback;

  hook_Direct3DCreate8_ = IATHook(handle, "d3d8.dll", "Direct3DCreate8", D3D8Direct3DCreate8Hook);

  hook_AdjustWindowRect_ = IATHook(handle, "user32.dll", "AdjustWindowRect", User32AdjustWindowRectHook);
  hook_SetCapture_ = IATHook(handle, "user32.dll", "SetCapture", User32SetCaptureHook);
  hook_SetCursor_ = IATHook(handle, "user32.dll", "SetCursor", User32SetCursorHook);
  hook_SetWindowLongA_ = IATHook(handle, "user32.dll", "SetWindowLongA", User32SetWindowLongAHook);
  hook_SetWindowPos_ = IATHook(handle, "user32.dll", "SetWindowPos", User32SetWindowPosHook);
  // t3dChangeDeviceResolution = (DWORD)GetProcAddress(handle, "t3dChangeDeviceResolution");

  if (init_fn) {
    Logger::Info("EqGfx: Executing external init callback");
    init_fn();  // Execute registered callback if provided with one.
  }
}

}  // namespace
}  // namespace EqGfxInt

void EqGfx::Initialize(HMODULE handle, void(__cdecl* init_fn)(),
                       std::function<void(int width, int height)> set_client_size_callback) {
  Logger::Info("EqGfx::Initialize()");
  EqGfxInt::InitializeEqGfx(handle, init_fn, set_client_size_callback);
}

void EqGfx::SetWindow(HWND wnd) { EqGfxInt::hwnd_ = wnd; }
