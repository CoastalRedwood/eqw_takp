// eqmain.dll creates the user interface for the login, options, and server select screens.
//
// Lifecycle:
//  - The eqmain.dll is loaded at the start of the WinMain infinite loop to present the login
//    screen. It then blocks until it completes. FreeLibrary is called on the way out, so
//    this dll and resources can be cycled in and out of memory.
//  - With dgvoodoo's d3d8.dll active, the memory location of the ddraw.dll module was changing
//    between login character select and back, so that library also may be getting reloaded.

#include "eq_main.h"

#include <ddraw.h>

#include "dinput_manager.h"
#include "iat_hook.h"
#include "ini.h"
#include "logger.h"
#include "vtable_hook.h"

// Using an EqMainInt namespace instead of a purely static class to reduce the qualifier clutter. The
// anonymous namespace forces it to private internal scope.
namespace EqMainInt {
namespace {
// Shared window state.
HWND hwnd_ = nullptr;                 // Shared game window.
WNDPROC original_wndproc_ = nullptr;  // Stores the previous wndproc.
WNDPROC eqmain_wndproc_ = nullptr;    // Stores eqmain.dll's wndproc.

// DirectDraw related state.
IDirectDraw* dd_ = nullptr;
IDirectDrawSurface* primary_surface_ = nullptr;
IDirectDrawSurface* secondary_surface_ = nullptr;

// The eqmain.dll graphics are designed for 640x480, so just hard-code it.
static constexpr int kClientWidth = 640;
static constexpr int kClientHeight = 480;
RECT client_rect_ = {0, 0, kClientWidth, kClientHeight};  // Updated to screen coords.
int win_width_ = kClientWidth;                            // Increases later due to border.
int win_height_ = kClientHeight;                          // Increases later due to border and title bar.
std::filesystem::path ini_path_;
static constexpr char kIniLoginOffsetX[] = "LoginX";
static constexpr char kIniLoginOffsetY[] = "LoginY";

// EqMain.dll hooks installed when the library is loaded.
IATHook hook_DirectDrawCreate_;
IATHook hook_CreateWindow_;
IATHook hook_DestroyWindow_;
IATHook hook_SetForegroundWindow_;
IATHook hook_SetWindowPos_;
IATHook hook_SetCapture_;
IATHook hook_SetFocus_;
IATHook hook_ShowCursor_;
IATHook hook_GetCursorPos_;
IATHook hook_ClientToScreen_;
IATHook hook_SetWindowLongA_;

// DirectDraw hook set in DirectDrawCreate hook.
VTableHook hook_DDrawRelease_;
VTableHook hook_SetCooperativeLevel_;
VTableHook hook_SetDisplayMode_;
VTableHook hook_CreateSurface_;
VTableHook hook_GetDisplayMode_;

// DirectDrawSurface hooks set in DirectDraw CreateSurface hook.
VTableHook hook_Flip_;
VTableHook hook_GetAttachedSurface_;

// Internal methods.

// Perform an explict bitblit copy from the secondary to the primary (instead of a buffer toggle) for windowed mode.
HRESULT WINAPI DDrawSurfaceFlipHook(IDirectDrawSurface* surface, IDirectDrawSurface* surface2, DWORD flags) {
  static bool success_report = false;
  static bool error_logged = false;
  HRESULT result = DD_OK;
  if (secondary_surface_->IsLost() == DDERR_SURFACELOST || primary_surface_->IsLost() == DDERR_SURFACELOST) {
    if (!error_logged) {
      error_logged = true;  // Just report errors once until success.
      Logger::Error("EqMain: Lost a surface. Blt was skipped.");
    }
    return DDERR_SURFACELOST;
  }

  RECT srcRect = {0, 0, kClientWidth, kClientHeight};
  RECT destRect = client_rect_;
  result = surface->Blt(&destRect, secondary_surface_, &srcRect, DDBLT_WAIT, nullptr);
  if (FAILED(result)) {
    if (!error_logged) {
      error_logged = true;
      Logger::Error("EqMain: Blt DDraw failed: 0x%x", result);
    }
    return result;
  }

  error_logged = false;
  if (!success_report) {
    success_report = true;  // Just report once.
    Logger::Info("EqMain: DDraw blt succeeded");
  }
  return result;
}

// Override to provide the pre-allocated secondary surface as the backbuffer.
HRESULT WINAPI DDrawSurfaceGetAttachedSurfaceHook(IDirectDrawSurface* surface, DDSCAPS* caps,
                                                  LPDIRECTDRAWSURFACE* backbuffer) {
  if (surface == primary_surface_) {
    Logger::Info("EqMain: GetAttachedSurface Primary");
    *backbuffer = secondary_surface_;
    return DD_OK;
  }
  HRESULT result = hook_GetAttachedSurface_.original(DDrawSurfaceGetAttachedSurfaceHook)(surface, caps, backbuffer);
  if (!SUCCEEDED(result)) Logger::Error("EqMain: GetAttachedSurface Failed HRESULT: 0x%x", (DWORD)result);
  return result;
}

// Helper function to install the required surface hooks.
void InstallDirectDrawSurfaceHooks(IDirectDrawSurface* surface) {
  void** vtable = *(void***)(surface);
  hook_Flip_ = VTableHook(vtable, 11, DDrawSurfaceFlipHook);
  hook_GetAttachedSurface_ = VTableHook(vtable, 12, DDrawSurfaceGetAttachedSurfaceHook);
}

// Fix the pixel format required by eqmain.dll.
DDPIXELFORMAT GetPixelFormat(void) {
  DDPIXELFORMAT format;
  ZeroMemory(&format, sizeof(DDPIXELFORMAT));
  format.dwSize = sizeof(DDPIXELFORMAT);
  format.dwFlags = DDPF_RGB;
  format.dwRGBBitCount = 16;   // Match primary surface bit depth
  format.dwRBitMask = 0xF800;  // 5 bits for Red (RGB 5-6-5)
  format.dwGBitMask = 0x07E0;  // 6 bits for Green (RGB 5-6-5)
  format.dwBBitMask = 0x001F;  // 5 bits for Blue (RGB 5-6-5)
  return format;
}

HRESULT WINAPI DDrawCreateSurfaceHook(IDirectDraw* lpDD, LPDDSURFACEDESC lpDDSurfaceDesc,
                                      LPDIRECTDRAWSURFACE* lplpDDSurface, IUnknown* pUnkOuter);

// Fix up the surface descriptors to play nice with windowed mode.
HRESULT CreatePrimarySurface(IDirectDraw* lpDD) {
  DDSURFACEDESC surface_desc;
  ZeroMemory(&surface_desc, sizeof(DDSURFACEDESC));
  surface_desc.dwSize = sizeof(DDSURFACEDESC);
  surface_desc.dwFlags = DDSD_CAPS;
  surface_desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
  surface_desc.ddpfPixelFormat = GetPixelFormat();
  HRESULT result = hook_CreateSurface_.original(DDrawCreateSurfaceHook)(lpDD, &surface_desc, &primary_surface_,
                                                                        NULL);  // create the primary surface

  if (!SUCCEEDED(result)) {
    Logger::Error("EqMain: Primary Surface Creation Failed with HRESULT: 0x%x", (DWORD)result);
    return result;
  }

  return result;
}

// Create a backbuffer to ensure GetAttachedSurface() doesn't fail.
HRESULT CreateSecondarySurface(IDirectDraw* lpDD) {
  DDSURFACEDESC surface_desc;
  ZeroMemory(&surface_desc, sizeof(DDSURFACEDESC));
  surface_desc.dwSize = sizeof(DDSURFACEDESC);
  surface_desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
  surface_desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
  surface_desc.dwWidth = kClientWidth;
  surface_desc.dwHeight = kClientHeight;
  surface_desc.ddpfPixelFormat = GetPixelFormat();
  HRESULT result = hook_CreateSurface_.original(DDrawCreateSurfaceHook)(lpDD, &surface_desc, &secondary_surface_, NULL);
  if (!SUCCEEDED(result)) Logger::Error("EqMain: Secondary Surface Creation Failed with HRESULT: 0x%x", (DWORD)result);
  return result;
}

// Need to add a clipper to the primary surface to play nice with other windows.
HRESULT AddClipper(IDirectDraw* lpDD) {
  LPDIRECTDRAWCLIPPER lpClipper;
  HRESULT result = lpDD->CreateClipper(0, &lpClipper, NULL);
  if (FAILED(result)) {
    Logger::Error("EqMain:: DDraw createclipper failed: %d", result);
    return result;
  }

  result = DD_OK;
  if (FAILED(lpClipper->SetHWnd(0, hwnd_)) || FAILED(primary_surface_->SetClipper(lpClipper))) {
    Logger::Error("EqMain: Error configuring DDraw clipper");
    result = E_FAIL;
  }
  lpClipper->Release();

  return result;
}

// The eqmain code expects the ddraw surface to be operating in full screen mode with
// a backbuffer and expects to use the flip() to display updates. That is not supported
// by ddraw in windowed mode, so we manually emulate it by creating our own backbuffer
// and our own flip routine.
HRESULT CreatePrimarySurfaces(IDirectDraw* lpDD, LPDDSURFACEDESC lpDDSurfaceDesc) {
  // First sanity check that eqmain is trying to create what we expect (no hacked mods).
  static const DWORD dwFlags = DDSD_BACKBUFFERCOUNT | DDSD_CAPS;
  static const DWORD dwCaps = DDSCAPS_SYSTEMMEMORY | DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
  if (lpDDSurfaceDesc->dwFlags != dwFlags || lpDDSurfaceDesc->ddsCaps.dwCaps != dwCaps ||
      lpDDSurfaceDesc->dwBackBufferCount != 1) {
    Logger::Error("EqMain: DDraw primary surface has unexpected parameters");
    return E_FAIL;
  }

  HRESULT result = CreatePrimarySurface(lpDD);
  if (FAILED(result)) return result;

  result = CreateSecondarySurface(lpDD);  // Extra surface to support a backbuffer.
  if (FAILED(result)) return result;      // Note: Not bothering to release primary if failed.

  InstallDirectDrawSurfaceHooks(primary_surface_);
  InstallDirectDrawSurfaceHooks(secondary_surface_);

  result = AddClipper(lpDD);
  if (FAILED(result)) return result;  // Note: Not bothering to release surfaces if failed.

  Logger::Info("EqMain: Surfaces created!");
  return DD_OK;
}

// Hook the create surface to support windowed mode.
HRESULT WINAPI DDrawCreateSurfaceHook(IDirectDraw* lpDD, LPDDSURFACEDESC lpDDSurfaceDesc,
                                      LPDIRECTDRAWSURFACE* lplpDDSurface, IUnknown* pUnkOuter) {
  if (lpDDSurfaceDesc == nullptr || lplpDDSurface == nullptr) return E_POINTER;

  HRESULT result = E_FAIL;

  // The primary surface requires significant modifications to work in windowed mode properly.
  if (lpDDSurfaceDesc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) {
    result = CreatePrimarySurfaces(lpDD, lpDDSurfaceDesc);
    *lplpDDSurface = (SUCCEEDED(result)) ? primary_surface_ : nullptr;
    return result;
  }

  // Other surfaces just get some light description patching for windowed mode.
  DDSURFACEDESC surface_desc;
  memcpy(&surface_desc, lpDDSurfaceDesc, sizeof(DDSURFACEDESC));

  if (!(surface_desc.dwFlags & DDSD_PIXELFORMAT)) {
    surface_desc.dwFlags |= DDSD_PIXELFORMAT;
    surface_desc.ddpfPixelFormat = GetPixelFormat();
  }
  surface_desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;

  result = hook_CreateSurface_.original(DDrawCreateSurfaceHook)(lpDD, &surface_desc, lplpDDSurface, pUnkOuter);
  if (!SUCCEEDED(result)) Logger::Error("EqMain: Surface Creation Failed with HRESULT: 0x%x", (DWORD)result);

  return result;
}

// Override the results with the pixel format of our re-used eqw window instead of the full screen.
HRESULT WINAPI DDrawGetDisplayModeHook(IDirectDraw* lpDD, LPDDSURFACEDESC lpDDSurfaceDesc) {
  Logger::Info("EqMain: Overriding DDraw GetDisplayMode");
  HRESULT result = hook_GetDisplayMode_.original(DDrawGetDisplayModeHook)(lpDD, lpDDSurfaceDesc);
  lpDDSurfaceDesc->ddpfPixelFormat = GetPixelFormat();
  return result;
}

// Clean up any extra custom resources and null our references.
ULONG WINAPI DDrawReleaseHook(IDirectDraw* lpDD) {
  if (dd_ != lpDD)
    Logger::Error("EqMain: Unexpected DDrawRelease: 0x%08x vs 0x%08x", (DWORD)dd_, (DWORD)lpDD);
  else
    Logger::Info("EqMain: DDrawRelease: 0x%08x", (DWORD)lpDD);

  dd_ = nullptr;               // Released below.
  primary_surface_ = nullptr;  // Released by eqmain/ddraw.

  // Clean up the extra secondary surface we manually created. As an experiment,
  // tried releasing the primary surface and that crashed.
  if (secondary_surface_) secondary_surface_->Release();
  secondary_surface_ = nullptr;

  int ref_count = hook_DDrawRelease_.original(DDrawReleaseHook)(lpDD);
  if (ref_count != 0) Logger::Error("EqMain: DDraw is leaking with ref count: %d", ref_count);
  return ref_count;
}

// Block the attempt to set the monitor to a full screen display mode (we use windows).
HRESULT WINAPI DDrawSetDisplayModeHook(IDirectDraw* lpDD, DWORD dwWidth, DWORD dwHeight, DWORD dwBpp) {
  Logger::Info("EqMain: Blocked DDraw SetDisplayMode %d x %d %d", dwWidth, dwHeight, dwBpp);
  return DD_OK;
}

// Hook the cooperative level to always be in non-exclusive normal windowed mode.
HRESULT WINAPI DDrawSetCooperativeLevelHook(IDirectDraw* lpDD, HWND hWnd, DWORD dwFlags) {
  Logger::Info("EqMain: Overriding DDraw SetCooperativeLevel (%d to %d)", dwFlags, DDSCL_NORMAL);
  HRESULT res = hook_SetCooperativeLevel_.original(DDrawSetCooperativeLevelHook)(lpDD, hWnd, DDSCL_NORMAL);
  if (res != DD_OK) Logger::Error("EqMain: Error setting cooperative level: %d", res);
  return res;
}

// Hooks the create so it can install the required vtable hooks.
HRESULT WINAPI DDrawDirectDrawCreateHook(GUID FAR* lpGUID, LPDIRECTDRAW* lplpDD, IUnknown FAR* pUnkOuter) {
  HRESULT rval = hook_DirectDrawCreate_.original(DDrawDirectDrawCreateHook)(lpGUID, lplpDD, pUnkOuter);
  dd_ = *lplpDD;

  if (SUCCEEDED(rval) && dd_) {
    Logger::Info("EqMain: DDrawCreated: 0x%08x", (DWORD)dd_);
    void** vtable = *(void***)dd_;
    hook_DDrawRelease_ = VTableHook(vtable, 2, DDrawReleaseHook);
    hook_CreateSurface_ = VTableHook(vtable, 6, DDrawCreateSurfaceHook);
    hook_GetDisplayMode_ = VTableHook(vtable, 12, DDrawGetDisplayModeHook);
    hook_SetDisplayMode_ = VTableHook(vtable, 21, DDrawSetDisplayModeHook);
    hook_SetCooperativeLevel_ = VTableHook(vtable, 20, DDrawSetCooperativeLevelHook);
  } else {
    Logger::Error("EqMain: DDrawCreate failed: %d", rval);
  }
  return rval;
}

// Helper function to synchronize the cached client_rect with the actual window client rect.
void UpdateClientRegion(HWND hwnd) {
  POINT offset = {0, 0};
  ::ClientToScreen(hwnd, &offset);
  ::GetClientRect(hwnd, &client_rect_);
  client_rect_ = {offset.x + client_rect_.left, offset.y + client_rect_.top, offset.x + client_rect_.right,
                  offset.y + client_rect_.bottom};

  // This is just temporary sanity checking.
  int width = client_rect_.right - client_rect_.left;
  int height = client_rect_.bottom - client_rect_.top;
  if (width != kClientWidth || height != kClientHeight) {
    RECT win_rect;
    ::GetWindowRect(hwnd, &win_rect);
    Logger::Error("EqMain: Incorrect client width %d x %d from %d x %d", width, height, win_rect.right - win_rect.left,
                  win_rect.bottom - win_rect.top);
  }
}

// Updates the win_width_ and win_height_ parameters based on the fixed client size.
void UpdateWinSizeFromFixedClientSize(HWND hwnd) {
  // Get the current window styles.
  DWORD dwStyle = (DWORD)::GetWindowLong(hwnd, GWL_STYLE);
  DWORD dwExStyle = (DWORD)::GetWindowLong(hwnd, GWL_EXSTYLE);

  // Define a RECT with the desired client size.
  // Adjust the rectangle to include non-client areas (title bar, borders, etc.)
  // Not using the DPI aware version for greater OS compatibility (assuming not v2 awareness).
  RECT win_rect = {0, 0, kClientWidth, kClientHeight};
  ::AdjustWindowRectEx(&win_rect, dwStyle, FALSE, dwExStyle);

  // Calculate the width and height including non-client areas
  win_width_ = win_rect.right - win_rect.left;
  win_height_ = win_rect.bottom - win_rect.top;

  Logger::Info("EqMain: Wnd: 0x%08x, (%d x %d)", (DWORD)hwnd, win_width_, win_height_);
}

// This specialized WndProc is installed to intercept messages to our shared window while EqMain's
// window is active to handle login. It handles the normal window methods and only uses the default
// EqMain's wndproc for mouse interaction with the UI buttons.
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  // if (msg != WM_MOUSEMOVE && msg != 0x404 && msg != WM_TIMER)
  //  Logger::info("EqMain: WndProc: 0x%x", msg);

  switch (msg) {
    case WM_QUIT:
      return 0;  // Ignore for this shared window.

    case WM_DESTROY:
    case WM_CLOSE:
      Logger::Info("EqMain: Terminating process");
      ::TerminateProcess(::GetCurrentProcess(), 0);
      break;

    case WM_ACTIVATEAPP:
      Logger::Info("WM_ACTIVATE: %d", LOWORD(wParam));
      if (LOWORD(wParam) == WA_INACTIVE)
        DInputManager::Unacquire();
      else
        DInputManager::Acquire(true);  // Keyboard only.
      break;

    case WM_GETMINMAXINFO: {
      MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
      info->ptMaxTrackSize = {win_width_, win_height_};  // Restrict min and max during dpi changes (no scaling).
      info->ptMinTrackSize = {win_width_, win_height_};  // Prevent it collapsing when moved/minimized in relogin.
      return 0;
    }

    case WM_WINDOWPOSCHANGED:
      UpdateClientRegion(hwnd);
      return 0;

    case WM_DPICHANGED:
      // We skip calling ::SetWindowPos() here and handle it later in WM_WINDOWPOSCHANGING.
      Logger::Info("EqMain::DpiChanged to %d", LOWORD(wParam));
      return 0;  // Skip default processing that would try to rescale.

    case WM_SETCURSOR:
      SetCursor(::LoadCursor(NULL, IDC_ARROW));
      return TRUE;

    case WM_SYSCOMMAND:
      if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // Suppress alt activated menu keys.
      break;

    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
      if (eqmain_wndproc_) return ::CallWindowProcA(eqmain_wndproc_, hwnd, msg, wParam, lParam);
      break;
    default:
      break;
  }

  return ::DefWindowProcA(hwnd, msg, wParam, lParam);
}

// Reconfigures the shared window to fit the expected eqmain's window size and plugs in the customized
// WndProc handler for eqmain.
HWND WINAPI User32CreateWindowExAHook(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X,
                                      int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance,
                                      LPVOID lpParam) {
  Logger::Info("EqMain: Create Window %d %d %d %d", X, Y, nWidth, nHeight);
  // The dll sets dwExStyle to 0x40008 (WS_EX_APPWINDOW | WS_EX_TOPMOST) and
  // dwStyle to 0x80000000 (WS_POPUP).

  original_wndproc_ = reinterpret_cast<WNDPROC>(::GetWindowLongA(hwnd_, GWL_WNDPROC));
  ::SetWindowLongA(hwnd_, GWL_WNDPROC, reinterpret_cast<LONG>(WndProc));

  if (nWidth != kClientWidth || nHeight != kClientHeight) Logger::Error("EqMain: Ignoring unexpected size");
  UpdateWinSizeFromFixedClientSize(hwnd_);

  // Calculate centered defaults and then try to retrieve ini settings.
  int center_X = (GetSystemMetrics(SM_CXSCREEN) - win_width_) / 2;
  int center_Y = (GetSystemMetrics(SM_CYSCREEN) - win_height_) / 2;
  X = Ini::GetValue<int>("EqwOffsets", kIniLoginOffsetX, center_X, ini_path_.string().c_str());
  Y = Ini::GetValue<int>("EqWOffsets", kIniLoginOffsetY, center_Y, ini_path_.string().c_str());

  // Go back to center if fails a sanity check (the ini values can get set to -32000 if minimized).
  if (X < -kClientWidth / 2 || Y < -20) {
    X = center_X;
    Y = center_Y;
  }

  SetWindowPos(hwnd_, 0, X, Y, win_width_, win_height_, 0);
  UpdateClientRegion(hwnd_);

  DInputManager::SetBackgroundMode(true);  // eqmain.dll will crash if keyboard acquire fails.

  return hwnd_;
}

// Updates the stored window offsets for the active window.
void StoreWindowOffsets() {
  RECT rect;
  if (!::GetWindowRect(hwnd_, &rect) || ::IsIconic(hwnd_)) return;

  // There was a bug reported (before the IsIconic() check above) where it stored -32000.
  if (rect.left < -4000 || rect.top < -20) return;  // Sanity check to prevent storing bad values.

  // Update the ini if needed.
  int left = Ini::GetValue<int>("EqwOffsets", kIniLoginOffsetX, 0, ini_path_.string().c_str());
  int top = Ini::GetValue<int>("EqwOffsets", kIniLoginOffsetY, 0, ini_path_.string().c_str());
  if (left != rect.left) Ini::SetValue<int>("EqwOffsets", kIniLoginOffsetX, rect.left, ini_path_.string().c_str());
  if (top != rect.top) Ini::SetValue<int>("EqwOffsets", kIniLoginOffsetY, rect.top, ini_path_.string().c_str());
}

// EqMain is re-using our Eqw window, so don't let it be destroyed but reset the wndproc handlers.
BOOL WINAPI User32DestroyWindowHook(HWND hwnd) {
  Logger::Info("EqMain: Destroy - Disconnecting eqmain wndproc");

  StoreWindowOffsets();  // Save the location if updated.

  // Disable special window handling.
  eqmain_wndproc_ = nullptr;
  ::SetWindowLongA(hwnd_, GWL_WNDPROC, (LONG)original_wndproc_);

  DInputManager::SetBackgroundMode(false);  // Let eqgame.exe run in foreground only mode.

  return true;
}

// The dll is aggressively setting foreground window which is annoying when windowed, so disable that.
BOOL WINAPI User32SetForegroundWindowHook(HWND hWnd) {
  ::ShowWindow(hWnd, SW_SHOW);
  return true;
}

// Note: This is primarily for logging and could be pruned out in the future.
HRESULT WINAPI User32SetWindowPosHook(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags) {
  Logger::Info("EqMain: SetWindowPos %d %d %d %d 0x%x 0x%x", X, Y, cx, cy, (DWORD)hWndInsertAfter, uFlags);
  return hook_SetWindowPos_.original(User32SetWindowPosHook)(hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags);
}

// Block EqMain from capturing the mouse.
HWND WINAPI User32SetCaptureHook(HWND hWnd) {
  Logger::Info("EqMain: Blocking SetCapture");
  return 0;
}

// Disable the set focus call. Not clear if this is necessary.
HWND WINAPI User32SetFocusHook(HWND hWnd) {
  return hWnd;  // No-op that lies that it did something.
}

// Disable the show cursor call to make the win32 cursor always visible (including title bar).
int WINAPI User32ShowCursorHook(BOOL show) {
  int count = ::ShowCursor(true);
  int target = 0;                                      // Make it precisely visible.
  while (count < target) count = ::ShowCursor(true);   // Count up to target.
  while (count > target) count = ::ShowCursor(false);  // Count down to target.
  return show ? 0 : -1;                                // Effectively a no-op that leaves it visible and lies about it.
}

// Return a fixed off screen cursor position so the self-drawn cursor is invisible (use win32).
BOOL WINAPI User32GetCursorPosHook(LPPOINT pt) {
  if (!pt) return false;
  pt->x = kClientWidth;
  pt->y = kClientHeight;

  // Alternatively, to make the rendered game cursor visible (but will need to hide the win32 cursor):
  // hook_GetCursorPos_.original(User32GetCursorPosHook)(pt);
  //::ScreenToClient(hwnd_, pt);  // Remove window offset.
  return true;
}

// Override so EqMain thinks it is running at (0,0) offset.
BOOL WINAPI User32ClientToScreenHook(HWND wnd, LPPOINT pt) {
  return true;  // Return unmodified coordinates.
}

// Ignore calls to set style but register the active WndProc with eqw's WndProc.
LONG WINAPI User32SetWindowLongAHook(HWND wnd, int index, long dwNewLong) {
  Logger::Info("EqMain: SetWindowLong: %d 0x%x", index, dwNewLong);
  if (index == GWL_WNDPROC) {
    eqmain_wndproc_ = reinterpret_cast<WNDPROC>(dwNewLong);
    ::SetWindowLongA(hwnd_, GWL_WNDPROC, reinterpret_cast<LONG>(WndProc));
  }

  return ::GetWindowLongA(wnd, index);
}

// Initializes state and installs the hooks to bootstrap the rest.
// Note that unlike eqgame, this will get called multiple times if dropping back to login screen.
void InitializeEqMain(HMODULE handle, HWND hwnd, const std::filesystem::path& ini_path, void(__cdecl* init_fn)()) {
  hwnd_ = hwnd;
  eqmain_wndproc_ = nullptr;
  ini_path_ = ini_path;

  DInputManager::Initialize(handle);

  // This state should have been cleaned up by the previous release but just in case clean them.
  dd_ = nullptr;
  primary_surface_ = nullptr;
  secondary_surface_ = nullptr;

  hook_DirectDrawCreate_ = IATHook(handle, "ddraw.dll", "DirectDrawCreate", DDrawDirectDrawCreateHook);
  hook_CreateWindow_ = IATHook(handle, "user32.dll", "CreateWindowExA", User32CreateWindowExAHook);
  hook_DestroyWindow_ = IATHook(handle, "user32.dll", "DestroyWindow", User32DestroyWindowHook);
  hook_SetForegroundWindow_ = IATHook(handle, "user32.dll", "SetForegroundWindow", User32SetForegroundWindowHook);
  hook_SetWindowPos_ = IATHook(handle, "user32.dll", "SetWindowPos", User32SetWindowPosHook);
  hook_SetCapture_ = IATHook(handle, "user32.dll", "SetCapture", User32SetCaptureHook);
  hook_SetWindowLongA_ = IATHook(handle, "user32.dll", "SetWindowLongA", User32SetWindowLongAHook);
  hook_SetFocus_ = IATHook(handle, "user32.dll", "SetFocus", User32SetFocusHook);
  hook_ShowCursor_ = IATHook(handle, "user32.dll", "ShowCursor", User32ShowCursorHook);
  hook_GetCursorPos_ = IATHook(handle, "user32.dll", "GetCursorPos", User32GetCursorPosHook);
  hook_ClientToScreen_ = IATHook(handle, "user32.dll", "ClientToScreen", User32ClientToScreenHook);

  if (init_fn) {
    Logger::Info("EqMain: Executing external init callback");
    init_fn();  // Execute registered callback if provided with one.
  }
}

}  // namespace
}  // namespace EqMainInt

void EqMain::Initialize(HMODULE handle, HWND hwnd, const std::filesystem::path& ini_path, void(__cdecl* init_fn)()) {
  Logger::Info("EqMain: Initialize");
  EqMainInt::InitializeEqMain(handle, hwnd, ini_path, init_fn);
}
