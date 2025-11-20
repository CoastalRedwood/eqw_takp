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

// Direct3D hooks installed when the interface and devices are created.
VTableHook hook_CreateDevice_;  // Direct3D.
VTableHook hook_Release_;       // Direct3DDevice.
VTableHook hook_Reset_;         // Direct3DDevice.

IDirect3DDevice8* device_ = nullptr;  // Local pointer to the allocated d3d device.

// Internal methods

// Null our local pointer to the d3d device if the reference count drops to zero.
HRESULT WINAPI D3DDeviceReleaseHook(IDirect3DDevice8* Device) {
  int new_count = hook_Release_.original(D3DDeviceReleaseHook)(Device);
  if (new_count == 0) {
    Logger::Info("EqGfx: Releasing device: 0x%08x", (int)(Device));
    device_ = nullptr;
  }
  return new_count;
}

// The reset hook is used to override the game and remain in windowed mode as well as perform
// the callback to inform the eqw windowing system when the resolution is changing.
HRESULT WINAPI D3DDeviceResetHook(IDirect3DDevice8* Device, D3DPRESENT_PARAMETERS* Parameters) {
  // EqW overrides the presentation parameters to always remain in windowed mode. The outer code
  // logic in eqgame will toggle window styles as needed.

  Logger::Info("EqGFX: Reset: %d x %d", Parameters->BackBufferWidth, Parameters->BackBufferHeight);

  // Per the Nvidia "DX8_Overview.pdf", the fields below must be zero in windowed mode. It seems to work fine with
  // the BackBufferWidth and BackBufferHeight set to non-zero with the set_client_size_cb_() happening immediately
  // afterwards, and when the order was swapped with the client size callback before the original() call the
  // full screen mode was confused, so left it like this.

  if (!Parameters->Windowed) {
    Parameters->hDeviceWindow = hwnd_;  // Make this explicit.
    Parameters->Windowed = true;
    Parameters->FullScreen_PresentationInterval = 0;
    Parameters->FullScreen_RefreshRateInHz = 0;
    // Parameters->BackBufferWidth = 0;
    // Parameters->BackBufferHeight = 0;
  }
  HRESULT result = EqGfxInt::hook_Reset_.original(D3DDeviceResetHook)(Device, Parameters);
  if (SUCCEEDED(result))
    set_client_size_cb_(Parameters->BackBufferWidth, Parameters->BackBufferHeight);
  else
    Logger::Error("EqGFX: Reset failure: 0x%08x", result);

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

  Logger::Info("EqGFX: Create device: %d x %d", pPresentationParameters->BackBufferWidth,
               pPresentationParameters->BackBufferHeight);

  // See Reset for comments on these fields in windowed mode.
  if (!pPresentationParameters->Windowed) {
    pPresentationParameters->hDeviceWindow = hwnd_;  // Make this explicit.
    pPresentationParameters->Windowed = true;
    pPresentationParameters->FullScreen_PresentationInterval = 0;
    pPresentationParameters->FullScreen_RefreshRateInHz = 0;
    // pPresentationParameters->BackBufferWidth = 0;
    // pPresentationParameters->BackBufferHeight = 0;
  }

  HRESULT result = hook_CreateDevice_.original(D3D8CreateDeviceHook)(
      pD3D, Adapter, DeviceType, hwnd_, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
  if (SUCCEEDED(result)) {
    device_ = *ppReturnedDeviceInterface;
    Logger::Info("EqGFX: Installing D3D8CreateDeviceHook (0x%08x)", (int)(device_));
    void** vtable = *(void***)device_;
    hook_Release_ = VTableHook(vtable, 2, D3DDeviceReleaseHook, false);
    hook_Reset_ = VTableHook(vtable, 14, D3DDeviceResetHook, false);
    set_client_size_cb_(pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight);
  } else {
    Logger::Error("EqGFX: Create device failure: 0x%08x", result);
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

// Returns true if the primary EQ object is constructred and the flag used to control the pump loop is zero.
bool IsMessagePumpActive() {
  int* eq = *reinterpret_cast<int**>(0x00809478);
  int process_game_complete_flag = *reinterpret_cast<int*>(0x0080947c);
  return eq != nullptr && process_game_complete_flag == 0;
}

// The expected directx recovery procedure for a lost device is to recognize it happened in Present()
// and then poll TestCooperativeLevel for the device not reset result code. The directx api requires
// that the TestCooperativeLevel() and Reset() calls occur on the same thread that created the device
// and preferably that's the same thread as the wndproc (which is true for eqgame.exe).

// This should be called by a WndProc message.
void HandleDeviceLost(bool force_reset) {
  static int recovery_attempt_counter = 0;

  if (!IsMessagePumpActive() || !device_) return;  // Only attempt if we are actively in game.

  // Check if the device is ready (or even needs to be) Reset().
  auto result = device_->TestCooperativeLevel();
  if (result != D3DERR_DEVICENOTRESET && !force_reset)
    return;  // Either not necessary or device is lost, so return quietly.

  if (force_reset) {
    recovery_attempt_counter = 0;  // Expected to be rate limited by external caller.
    if (result == D3DERR_DEVICELOST) {
      Logger::Error("EqGfx: Ignoring forced reset due to lost device.");
      return;
    }
    Logger::Error("EqGfx: Forcing reset with status 0x%08x", result);
  }

  if (++recovery_attempt_counter <= 5) Logger::Error("EqGfx: Attempting d3d device recovery");

  auto handle = ::GetModuleHandleA("eqgfx_dx8.dll");
  FARPROC t3dSwitchD3DVideoMode = handle ? ::GetProcAddress(handle, "t3dSwitchD3DVideoMode") : nullptr;
  if (!t3dSwitchD3DVideoMode) return;

  t3dSwitchD3DVideoMode();  // Handles releasing resources, calling D3D Reset(), then restoring resources.

  if (recovery_attempt_counter > 5) return;

  result = device_->TestCooperativeLevel();
  if (result == D3D_OK)
    Logger::Info("EqGfx: Device is reporting okay");
  else
    Logger::Error("EqGfx: Device is not okay: 0x%08x", result);
}

// Sends a custom user message ID to notify the wndproc thread to attempt device recovery.
void SendDeviceLostMessage() {
  if (!IsMessagePumpActive() || !hwnd_)
    return;  // Only try to recover in-game when the message queue is actively listening.

  static int send_message_counter = 0;
  if (++send_message_counter <= 5) Logger::Error("EqGfx: Sending device lost");

  // Perform a blocking call that forces a check if the device is ready to recover.
  ::SendMessageA(hwnd_, EqGfx::kDeviceLostMsgId, EqGfx::kDeviceLostMsgId, 0);
}

template <typename T>
static void protected_mem_write(int target, const T& value) {
  DWORD oldprotect;
  size_t size = sizeof(value);
  ::VirtualProtect(reinterpret_cast<PVOID*>(target), size, PAGE_EXECUTE_READWRITE, &oldprotect);
  memcpy(reinterpret_cast<T*>(target), &value, size);
  ::VirtualProtect(reinterpret_cast<PVOID*>(target), size, oldprotect, &oldprotect);
  ::FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<PVOID*>(target), size);
}

// Patch a bug in t3dUpdateDisplay where it is checking for a D3DERR_DEVICENOTRESET result from present
// to trigger a d3d recovery attempt / Reset() by calling t3dSwitchD3DVideoMode. That has two bugs:
// (1) the Present() does not return DEVICENOTRESET and the code should be checking for DEVICELOST
// and (2) that the correct recovery procedure has to occur on the thread that created the device.
// This patch makes it look for the correct failure result code and then call a custom handler to trigger
// the recovery attempt on the proper thread. The eqmac.exe checks the expected DEVICELOST code.
void InstallDeviceLostRecoveryPatch(HMODULE handle) {
  Logger::Info("EqGfx: Installing device lost patch");
  const int base_addr = reinterpret_cast<int>(handle);
  const int result_code_patch_addr = base_addr + 0x0006bd07 + 1;  // Change  CMP EAX,0x88760869 to CMP EAX, 0x88760868.
  const int switch_mode_call_addr = base_addr + 0x0006bd0e + 1;   // Replace the direct reset call with our Send call.
  static const int switch_mode_call_addr_jump_value_unpatched = 0x0002ecd;
  FARPROC update_fn = ::GetProcAddress(handle, "t3dUpdateDisplay");
  // Quick sanity check that the eqfgx_dx8.dll is the one we expect.
  if ((reinterpret_cast<int>(update_fn) - base_addr != 0x6bca0) ||
      (*reinterpret_cast<BYTE*>(result_code_patch_addr) != 0x69) ||
      (*reinterpret_cast<int*>(switch_mode_call_addr) != switch_mode_call_addr_jump_value_unpatched)) {
    Logger::Error("EqGfx: Unrecognized eqgfx_dx8.dll, skipping recovery patch installation");
    return;
  }
  const BYTE result_code_patch = 0x68;
  protected_mem_write(result_code_patch_addr, result_code_patch);

  const int end_of_call_addr = switch_mode_call_addr + 4;  // Address at end of instruction.
  const int jump_value = reinterpret_cast<int>(&SendDeviceLostMessage) - end_of_call_addr;
  protected_mem_write(switch_mode_call_addr, jump_value);
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

  InstallDeviceLostRecoveryPatch(handle);  // Patch the recovery process in t3dUpdateDisplay.

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

void EqGfx::HandleDeviceLost(bool force_reset) { EqGfxInt::HandleDeviceLost(force_reset); }
