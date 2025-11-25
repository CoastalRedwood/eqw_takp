#include "dinput_manager.h"

#define DIRECTINPUT_VERSION 0x0800
#define INITGUID
#include <dinput.h>

#include "iat_hook.h"
#include "logger.h"
#include "vtable_hook.h"

// Using a DInput namespace instead of a purely static class to reduce the qualifier clutter. The
// anonymous namespace forces it to private internal scope.
namespace DInput {
namespace {  // anonymous

bool enable_background_mode_ = false;

// Allocated DirectInput hardware resources.
LPDIRECTINPUT8 dinput_ = nullptr;
LPDIRECTINPUTDEVICE8W keyboard_ = nullptr;
LPDIRECTINPUTDEVICE8W mouse_ = nullptr;

// Primary hook used to install lower level hooks.
IATHook hook_DirectInput_;

// Direct input vtable hooks.
VTableHook hook_DInputCreateDevice_;
VTableHook hook_DInputRelease_;

// Device level functional hooks.
VTableHook hook_key_SetCooperativeLevel_;
VTableHook hook_key_GetDeviceData_;
VTableHook hook_key_GetDeviceState_;
VTableHook hook_key_Release_;
VTableHook hook_key_Acquire_;
VTableHook hook_key_Unacquire_;

VTableHook hook_mouse_SetCooperativeLevel_;
VTableHook hook_mouse_GetDeviceData_;
VTableHook hook_mouse_GetDeviceState_;
VTableHook hook_mouse_Release_;
VTableHook hook_mouse_Acquire_;
VTableHook hook_mouse_Unacquire_;

// Internal methods.

const char* GetDeviceStr(LPDIRECTINPUTDEVICE8W device) {
  return (device == keyboard_) ? "Keyboard" : (device == mouse_) ? "Mouse" : "Unknown";
}

// Block any client attempts to release the dinput device resources.
HRESULT WINAPI DeviceReleaseHook(LPDIRECTINPUTDEVICE8W device) {
  Logger::Info("Blocking DInput %s release request (0x%08x) on thread %d", GetDeviceStr(device), (int)device,
               ::GetCurrentThreadId());
  return DI_OK;
}

// Wrapper layer to redirect the device and correct an eqgame bug.
HRESULT WINAPI DeviceGetDeviceDataHook(LPDIRECTINPUTDEVICE8W device, size_t buffer_size, LPDIDEVICEOBJECTDATA data,
                                       DWORD* event_count_max, LPUNKNOWN unk) {
  HRESULT result = DIERR_NOTINITIALIZED;
  if (device == keyboard_)
    result = hook_key_GetDeviceData_.original(DeviceGetDeviceDataHook)(device, buffer_size, data, event_count_max, unk);
  else if (device == mouse_)
    result =
        hook_mouse_GetDeviceData_.original(DeviceGetDeviceDataHook)(device, buffer_size, data, event_count_max, unk);

  // The game client has a bug where it assumes that the event_count_max parameter is always set, even on failure.
  // Ensure that it is set to zero on failure in this layer.
  if (!SUCCEEDED(result) && event_count_max) *event_count_max = 0;
  return result;
}

// Wrapper layer to allow for flushing of device data upon acquisition.
HRESULT WINAPI DeviceAcquireHook(LPDIRECTINPUTDEVICE8W device) {
  auto result = DIERR_NOTINITIALIZED;

  if (device == keyboard_)
    result = hook_key_Acquire_.original(DeviceAcquireHook)(device);
  else if (device == mouse_)
    result = hook_mouse_Acquire_.original(DeviceAcquireHook)(device);

  if (result == DI_OK) {  // Returns DI_OK if new acquire else S_FALSE if already acquired.
    Logger::Info("Acquired and flushing %s", GetDeviceStr(device));
    DWORD items = INFINITE;  // Perform a flush of any stale data.
    DeviceGetDeviceDataHook(device, sizeof(DIDEVICEOBJECTDATA), NULL, &items, 0);
  } else if (result != S_FALSE && result != DIERR_OTHERAPPHASPRIO) {
    Logger::Error("Error acquiring %s: %d", GetDeviceStr(device), result);
  }

  return result;
}

// Wrappers for device unacquire to allow for resetting buffers, etc.
HRESULT WINAPI DeviceUnacquireHook(LPDIRECTINPUTDEVICE8W device) {
  HRESULT result = DI_OK;
  if (device == keyboard_)
    result = hook_key_Unacquire_.original(DeviceUnacquireHook)(device);
  else if (device == mouse_)
    result = hook_mouse_Unacquire_.original(DeviceUnacquireHook)(device);

  const char* effect = (result == DI_OK) ? "" : " (no effect)";
  Logger::Info("Unacquire %s %s", GetDeviceStr(device), effect);
  return result;
}

// Wrappers for modifying device state fetches.
HRESULT WINAPI DeviceGetDeviceStateHook(LPDIRECTINPUTDEVICE8W device, size_t buffer_size, LPDIDEVICEOBJECTDATA data) {
  if (device == keyboard_)
    return hook_key_GetDeviceState_.original(DeviceGetDeviceStateHook)(device, buffer_size, data);
  else if (device == mouse_)
    return hook_mouse_GetDeviceState_.original(DeviceGetDeviceStateHook)(device, buffer_size, data);
  else
    return DIERR_NOTINITIALIZED;
}

// Override the cooperative level to make the DInput play nice in windowed mode.
HRESULT WINAPI DeviceSetCooperativeLevelHook(LPDIRECTINPUTDEVICE8W device, HWND wnd, DWORD flags) {
  // The client varies in how it handles failures to acquire the devices. The eqmain.dll only acquires
  // the keyboard but it throws fatal errors if that fails, so we use the BACKGROUND mode to always
  // enable the keyboard to be acquired. However eqclient includes logic to try and reacquire the
  // device whenenever it initially fails to retrieve data, so it can support FOREGROUND mode.
  flags = enable_background_mode_ ? DISCL_BACKGROUND : DISCL_FOREGROUND;
  flags |= DISCL_NONEXCLUSIVE;

  Logger::Info("DInput %s coop level set to 0x%x", GetDeviceStr(device), flags);
  if (device == keyboard_)
    return hook_key_SetCooperativeLevel_.original(DeviceSetCooperativeLevelHook)(device, wnd, flags);
  else if (device == mouse_)
    return hook_mouse_SetCooperativeLevel_.original(DeviceSetCooperativeLevelHook)(device, wnd, flags);

  Logger::Error("Bad device in set cooperative levels");
  return DIERR_NOTINITIALIZED;
}

// Handles reuse of existing created devices and installs hooks in any newly created devices.
HRESULT WINAPI DirectInputCreateDeviceHook(LPDIRECTINPUT8* ppvOut, GUID& guid, LPDIRECTINPUTDEVICE8W* device,
                                           LPUNKNOWN unk) {
  bool is_keyboard = IsEqualGUID(guid, GUID_SysKeyboard);
  bool is_mouse = !is_keyboard && (IsEqualGUID(guid, GUID_SysMouse) || IsEqualGUID(guid, GUID_SysMouseEm) ||
                                   IsEqualGUID(guid, GUID_SysMouseEm2));

  if (!is_keyboard && !is_mouse) {
    Logger::Error("Unknown create device GUID");
    return DIERR_NOINTERFACE;
  }

  auto target_device = is_keyboard ? &keyboard_ : &mouse_;
  auto target_string = is_keyboard ? "Keyboard" : "Mouse";

  if (*target_device) {
    Logger::Info("DInput: Reusing %s on thread %d", target_string, ::GetCurrentThreadId());
    *device = *target_device;
    return DI_OK;
  }

  // We actually need to create a new device. Do that anad then hook it up.
  HRESULT result = hook_DInputCreateDevice_.original(DirectInputCreateDeviceHook)(ppvOut, guid, device, unk);
  if (result != DI_OK) {
    Logger::Error("Error: Failed to create %s device: %d", target_string, result);
    return result;
  }

  // The create device was returning the same vtable for both the keyboard and mouse in a test system,
  // but support separate vtable's with shared common handlers that route to possibly different
  // original table functions.
  *target_device = *device;
  void** vtable = *(void***)(*target_device);
  Logger::Info("%s (0x%08x) VTABLE 0x%x on thread %d", target_string, (DWORD)*target_device, (DWORD)vtable,
               ::GetCurrentThreadId());
  if (is_keyboard) {
    hook_key_Release_ = VTableHook(vtable, 2, DeviceReleaseHook);
    hook_key_Acquire_ = VTableHook(vtable, 7, DeviceAcquireHook);
    hook_key_Unacquire_ = VTableHook(vtable, 8, DeviceUnacquireHook);
    hook_key_GetDeviceState_ = VTableHook(vtable, 9, DeviceGetDeviceStateHook);
    hook_key_GetDeviceData_ = VTableHook(vtable, 10, DeviceGetDeviceDataHook);
    hook_key_SetCooperativeLevel_ = VTableHook(vtable, 13, DeviceSetCooperativeLevelHook);
  } else {
    hook_mouse_Release_ = VTableHook(vtable, 2, DeviceReleaseHook);
    hook_mouse_Acquire_ = VTableHook(vtable, 7, DeviceAcquireHook);
    hook_mouse_Unacquire_ = VTableHook(vtable, 8, DeviceUnacquireHook);
    hook_mouse_GetDeviceState_ = VTableHook(vtable, 9, DeviceGetDeviceStateHook);
    hook_mouse_GetDeviceData_ = VTableHook(vtable, 10, DeviceGetDeviceDataHook);
    hook_mouse_SetCooperativeLevel_ = VTableHook(vtable, 13, DeviceSetCooperativeLevelHook);
  }

  return DI_OK;
}

// Blocks any attempt by the client to release the DirectInput object.
HRESULT WINAPI DirectInputReleaseHook(LPDIRECTINPUT8* ppvOut) {
  Logger::Info("Blocking DirectInput8 Release call: 0x%08x on thread %d", reinterpret_cast<int>(ppvOut),
               ::GetCurrentThreadId());
  return DI_OK;
}

// Maintains the singleton dinput object and installs the hooks for managing devices and its own lifecycle.
HRESULT WINAPI DirectInput8CreateHook(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPDIRECTINPUT8* ppvOut,
                                      LPDIRECTINPUT8 punkOuter) {
  Logger::Info("DirectInput8CreateHook -- Version: 0x%x on thread %d", dwVersion, ::GetCurrentThreadId());
  if (dinput_) {
    Logger::Info("Reusing dinput object");
    *ppvOut = dinput_;
    return DI_OK;
  }

  auto result = hook_DirectInput_.original(DirectInput8CreateHook)(hinst, dwVersion, riidltf, ppvOut, punkOuter);
  if (result == DI_OK) {
    dinput_ = *ppvOut;
    void** vtable = *(void***)(*ppvOut);
    hook_DInputRelease_ = VTableHook(vtable, 2, DirectInputReleaseHook);
    hook_DInputCreateDevice_ = VTableHook(vtable, 3, DirectInputCreateDeviceHook);
    Logger::Debug("DInput create success: 0x%08x on thread %d", reinterpret_cast<int>(dinput_), ::GetCurrentThreadId());
  } else {
    Logger::Error("DInput create error: %d", result);
  }
  return result;
}

}  // namespace
}  // namespace DInput

// Installs the primary hook used to modify the vtables of all created dinput devices.
void DInputManager::Initialize(HMODULE handle) {
  // Note that multiple hmodules will get hooked and the original of this common global hook will point
  // to the final one, but they should all be equivalent / identical.
  DInput::hook_DirectInput_ = IATHook(handle, "dinput8.dll", "DirectInput8Create", DInput::DirectInput8CreateHook);
}

// Support enabling background mode during eqmain which will crash out if the acquire fails.
void DInputManager::SetBackgroundMode(bool background_mode) {
  Logger::Info("DInput background mode: %d", background_mode);
  DInput::enable_background_mode_ = background_mode;
}

// Manual methods to acquire and unacquire the devices.  Note that eqmain only creates the keyboard.
void DInputManager::Acquire(bool keyboard_only) {
  if (!keyboard_only && DInput::mouse_) DInput::mouse_->Acquire();
  if (DInput::keyboard_) DInput::keyboard_->Acquire();
}

void DInputManager::Unacquire() {
  if (DInput::mouse_) DInput::mouse_->Unacquire();
  if (DInput::keyboard_) DInput::keyboard_->Unacquire();
}
