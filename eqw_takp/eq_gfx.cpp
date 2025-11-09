#include "eq_gfx.h"

#include <iostream>

#include "d3dx8/d3d8.h"
#include "iat_hook.h"
#include "vtable_hook.h"

// Notes:
// - the 3d device is created after the server is selected (post eqmain)
// - looks like DAT_10a4f764 == 1 is full screen and the style is set to
// 0x91000000 (WS_POPUP | WS_VISIBLE | WS_MAXIMIZE) versus
// 0x10ca0000 (WS_VISIBLE | WS_CAPTION | WS_SYSMENU | WS_GROUP).
// - Need to look into adding gamma support to match that eqgfix.exe
//
// TODO's:
// - Handle video resolution changes
// - Handle full screen mode
// - Handle reset / recovery as needed for device lost etc.

// Using an EqGfxInt namespace instead of a purely static class to reduce the qualifier clutter. The
// anonymous namespace forces it to private internal scope.
namespace EqGfxInt {
namespace {

HWND hwnd_;                                                                // Shared common window.
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
VTableHook hook_BeginScene_;

IDirect3DDevice8* device_;
// IDirect3DSurface8* surface;
D3DPRESENT_PARAMETERS present_;
// DWORD t3dChangeDeviceResolution;
// DWORD base;
bool reset_viewport_ = false;  // TODO: Obsolete?

// Internal methods

HRESULT WINAPI D3DDeviceResetHook(IDirect3DDevice8* Device, D3DPRESENT_PARAMETERS* Parameters) {
  HRESULT result = EqGfxInt::hook_Reset_.original(D3DDeviceResetHook)(Device, Parameters);
  if (SUCCEEDED(result)) {
    std::cout << "EqGFX: Reset: " << Parameters->BackBufferWidth << " x " << Parameters->BackBufferHeight << std::endl;
    set_client_size_cb_(Parameters->BackBufferWidth, Parameters->BackBufferHeight);
    // SetEqMainSurfaceResolution(Parameters->BackBufferWidth, Parameters->BackBufferHeight);
  }
  return result;
}

HRESULT WINAPI D3DDeviceBeginSceneHook(IDirect3DDevice8* Device) {
  // ResetViewport();
  HRESULT result = hook_BeginScene_.original(D3DDeviceBeginSceneHook)(Device);
  return result;
}

HRESULT WINAPI D3D8CreateDeviceHook(IDirect3D8* pD3D, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
                                    IDirect3DDevice8** ppReturnedDeviceInterface) {
  pPresentationParameters->Windowed = true;
  pPresentationParameters->hDeviceWindow = hwnd_;
  std::cout << "Create device hook " << present_.BackBufferFormat << std::endl;
  HRESULT result = hook_CreateDevice_.original(D3D8CreateDeviceHook)(
      pD3D, Adapter, DeviceType, hwnd_, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
  present_ = *pPresentationParameters;
  if (SUCCEEDED(result)) {
    std::cout << "EqGFX: D3D8CreateDeviceHook: " << present_.BackBufferWidth << " x " << present_.BackBufferHeight
              << std::endl;

    void** vtable = *(void***)*ppReturnedDeviceInterface;
    hook_Reset_ = VTableHook(vtable, 14, D3DDeviceResetHook, false);
    hook_BeginScene_ = VTableHook(vtable, 35, D3DDeviceBeginSceneHook, false);
    set_client_size_cb_(present_.BackBufferWidth, present_.BackBufferHeight);
    // SetEqMainSurfaceResolution(present_.BackBufferWidth, present_.BackBufferHeight);

    device_ = *ppReturnedDeviceInterface;
  }
  return result;
}

// Hook the device creation to install further hooks.
IDirect3D8* WINAPI D3D8Direct3DCreate8Hook(UINT SDK) {
  std::cout << "Direct3DCreate8 -- SDK Version: " << SDK << std::endl;
  IDirect3D8* rval = hook_Direct3DCreate8_.original(D3D8Direct3DCreate8Hook)(SDK);
  void** vtable = *(void***)rval;
  hook_CreateDevice_ = VTableHook(vtable, 15, D3D8CreateDeviceHook, false);

  return rval;
}

// Override the style by fetching the active window style so the SetWindowPos call is accurate.
BOOL WINAPI User32AdjustWindowRectHook(LPRECT lpRect, DWORD dwStyle, BOOL bMenu) {
  std::cout << "EqGfx: Overriding window style in AdjustWindowRect" << std::endl;
  dwStyle = ::GetWindowLongA(hwnd_, GWL_STYLE);
  return hook_AdjustWindowRect_.original(User32AdjustWindowRectHook)(lpRect, dwStyle, bMenu);
}

// Block the client from ever trying to capture the mouse.
HWND WINAPI User32SetCaptureHook(HWND hWnd) {
  std::cout << "EqGfx: Blocking SetCapture" << std::endl;
  return NULL;
}

// Block the disabling of the IDC_ARROW cursor by EqGfx.
HCURSOR WINAPI User32SetCursorHook(HCURSOR hcursor) {
  std::cout << "EqGfx: Blocking SetCursor " << hcursor << std::endl;
  return hcursor;
}

// Block setting different window styles.
LONG WINAPI User32SetWindowLongAHook(HWND wnd, int index, long dwNewLong) {
  std::cout << "EqGfx: Blocking SetWindowLong: " << index << ", " << dwNewLong << std::endl;
  return GetWindowLongA(wnd, index);
}

// TODO: If the adjustrect style is overridden, this may be okay to call for switching video resolutions.
HRESULT WINAPI User32SetWindowPosHook(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags) {
  std::cout << "EqGFX: Allowing SetWindowPos: " << X << " " << Y << " " << cx << " " << cy << std::endl;
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

  if (init_fn) init_fn();  // Execute registered callback if provided with one.
}

}  // namespace
}  // namespace EqGfxInt

void EqGfx::Initialize(HMODULE handle, void(__cdecl* init_fn)(),
                       std::function<void(int width, int height)> set_client_size_callback) {
  std::cout << "EqGfx::Initialize()" << std::endl;
  EqGfxInt::InitializeEqGfx(handle, init_fn, set_client_size_callback);
}

void EqGfx::SetWindow(HWND wnd) { EqGfxInt::hwnd_ = wnd; }

void EqGfx::ResetViewport() { EqGfxInt::reset_viewport_ = true; }

void EqGfx::ChangeResolution(UINT width, UINT height) {
  if (!EqGfxInt::device_) {
    return;  // Ensure the device exists
  }
  // TODO: Not implemented.
}
