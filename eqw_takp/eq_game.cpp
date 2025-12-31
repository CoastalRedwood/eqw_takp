#include "eq_game.h"

#include <ddraw.h>
#include <shellapi.h>  // For ExtractIconExA
#include <windows.h>

#include <algorithm>
#include <filesystem>

#include "cpu_timestamp_fix.h"
#include "dinput_manager.h"
#include "eq_gfx.h"
#include "eq_main.h"
#include "game_input.h"
#include "iat_hook.h"
#include "ini.h"
#include "logger.h"
#include "vtable_hook.h"

// Using an EqGameInt namespace instead of a purely static class to reduce the qualifier clutter. The
// anonymous namespace forces it to private internal scope.
namespace EqGameInt {
namespace {
static constexpr int kStartupWidth = 640;  // Default eqmain size.
static constexpr int kStartupHeight = 480;
static constexpr char kIniLoginOffset[] = "Login";
static constexpr DWORD kWindowExStyle = 0;  // No flags.
static constexpr DWORD kWindowStyleNormal = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME;
static constexpr DWORD kWindowStyleFullScreen = WS_POPUP | WS_VISIBLE;
static constexpr int kDeviceForceReset = EqGfx::kDeviceLostMsgId + 1;
static constexpr int kIconId = 107;    // Resource ID in the exe.
static constexpr int kIconSize = 128;  // Largest icon in the exe.

// State and resources updated at initialization.
std::filesystem::path exe_path_;  // Full path filename for eqgame.exe file.
std::filesystem::path ini_path_;  // Full path filename for eqw.ini file.
HWND hwnd_ = nullptr;             // Primary shared visible window handle.
HICON hicon_large_ = nullptr;     // Icons retrieved from the executable.
HICON hicon_small_ = nullptr;     // Icons retrieved from the executable.
void(__cdecl* eqmain_init_fn_)() = nullptr;
void(__cdecl* eqgfx_init_fn_)() = nullptr;
void(__cdecl* eqcreatewin_init_fn_)() = nullptr;

// Hooks to enable hooking the other libraries and supporting windowed mode.
IATHook hook_LoadLibrary_;
IATHook hook_CreateWindow_;
IATHook hook_SetCapture_;
IATHook hook_SetCursor_;
IATHook hook_ShowCursor_;
IATHook hook_ShowWindow_;

// Internal state while eqgame is controlling the window (vs eqmain).
bool full_screen_mode_ = false;     // Enables full screen (non-windowed).
WNDPROC eqgame_wndproc_ = nullptr;  // Stores eqgame.exe's wndproc.
int win_width_ = kStartupWidth;     // Updated during window creation based on style.
int win_height_ = kStartupHeight;

// Internal methods.

// Returns true if the primary EQ game object is allocated.
bool IsGameInitialized() {
  return (*(void**)0x00809478 != nullptr);  // Created after login and destroyed before returning.
}

// Returns true if in the active in world game state.
bool IsGameInGameState() {
  const int* eq = *reinterpret_cast<int**>(0x00809478);
  return eq && eq[0x5AC / 4] == 5;  // Check the game state stored in the EQ object.
}

LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Creates the common, shared window used by eqgame and eqmain.
void CreateEqWindow() {
  Logger::Info("CreateEqWindow()");

  // Load the AK logo icons.
  hicon_small_ = ::LoadIconA(::GetModuleHandle(NULL), MAKEINTRESOURCEA(kIconId));
  hicon_large_ = (HICON)::LoadImageA(::GetModuleHandleA(NULL), MAKEINTRESOURCEA(kIconId), IMAGE_ICON, kIconSize,
                                     kIconSize, LR_DEFAULTCOLOR | LR_SHARED);

  WNDCLASSA wc = {};
  wc.lpfnWndProc = GameWndProc;  // Custom handler to intercept messages.
  wc.hIcon = hicon_small_;
  wc.hInstance = ::GetModuleHandleA(NULL);
  wc.hCursor = ::LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
  wc.lpszClassName = "_EqWwndclass";
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.hbrBackground = ::CreateSolidBrush(RGB(0, 0, 0));  // Black background.
  if (::RegisterClassA(&wc) == 0) {
    // Handle registration failure
    ::MessageBoxA(NULL, "Window class registration failed!", "Error", MB_OK | MB_ICONERROR);
    return;
  }

  // Adjust the rect for the specified window style
  RECT rect = {0, 0, kStartupWidth, kStartupHeight};  // Client area size
  DWORD dwexstyle = kWindowExStyle;                   // Fixed (no) flags.
  DWORD dwstyle = kWindowStyleNormal;                 // Used by eqmain.
  ::AdjustWindowRectEx(&rect, dwstyle, FALSE, 0);
  win_width_ = rect.right - rect.left;
  win_height_ = rect.bottom - rect.top;
  int x = (::GetSystemMetrics(SM_CXSCREEN) - win_width_) / 2;
  int y = (::GetSystemMetrics(SM_CYSCREEN) - win_height_) / 2;
  x = Ini::GetValue<int>("EqwOffsets", std::string(kIniLoginOffset) + "X", x, ini_path_.string().c_str());
  y = Ini::GetValue<int>("EqWOffsets", std::string(kIniLoginOffset) + "Y", y, ini_path_.string().c_str());

  hwnd_ = ::CreateWindowExA(dwexstyle, wc.lpszClassName, "EqW-TAKP", dwstyle, x, y, win_width_, win_height_, NULL, NULL,
                            NULL, NULL);
  EqGfx::SetWindow(hwnd_);

  full_screen_mode_ = Ini::GetValue<bool>("EqwGeneral", "FullScreenMode", false, ini_path_.string().c_str());

  // Install mouse and keyboard handling hooks.
  bool swap_mouse_buttons = Ini::GetValue<bool>("EqwGeneral", "SwapMouseButtons", false, ini_path_.string().c_str());
  bool disable_keydown_clear =
      Ini::GetValue<bool>("EqwGeneral", "DisableKeydownClear", false, ini_path_.string().c_str());
  GameInput::Initialize(hwnd_, swap_mouse_buttons, disable_keydown_clear);

  if (eqcreatewin_init_fn_) {
    Logger::Info("CreateEqWindow: Executing external init callback");
    eqcreatewin_init_fn_();  // Execute registered callback if provided with one.
  }
}

// The game client creates a window with the flags set to topmost maximize it, which we ignore.
HWND WINAPI User32CreateWindowExAHook(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X,
                                      int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance,
                                      LPVOID lpParam) {
  Logger::Info("EqGame: Create Window, ignoring size: x: %d, y: %d, W: %d, H: %d", X, Y, nWidth, nHeight);
  if (!hwnd_) CreateEqWindow();

  // It should now be safe to connect the client's wndproc handler. We could sniff this value with
  // a hooked RegisterClass that copies the wndproc from that, but this client has a hardcoded address.
  eqgame_wndproc_ = (WNDPROC)(0x0055a4f4);

  return hwnd_;
}

void SetClientSize(int client_width, int client_height, bool use_startup = false) {
  // The client generates 0x0 sizes while resetting D3D. We just ignore them and leave
  // our window size unchanged until a real (valid) size is commanded.
  if (client_width == 0 && client_height == 0) return;

  // Constrain the minimum sizes (shouldn't happen with the 0x0 check above but just in case).
  if (client_width < kStartupWidth || client_height < kStartupHeight) {
    Logger::Info("EqGame: Preventing small client size: %d x %d", client_width, client_height);
    client_width = max(client_width, kStartupWidth);
    client_height = max(client_height, kStartupHeight);
    use_startup = true;
  }

  // First try to retrieve the stored offset location for this resolution.
  bool center = false;
  std::string setting_stem = use_startup ? std::string(kIniLoginOffset)
                                         : (std::to_string(client_width) + "by" + std::to_string(client_height));
  int x = Ini::GetValue<int>("EqwOffsets", setting_stem + "X", -32768, ini_path_.string().c_str());
  int y = Ini::GetValue<int>("EqWOffsets", setting_stem + "Y", -32768, ini_path_.string().c_str());
  bool valid_offset = (x > -16384 && y > -16384);  // Very loose sanity check. Client game res < 16k max.

  // Then use that to retrieve the relevant monitor info.
  HMONITOR monitor =
      valid_offset ? ::MonitorFromPoint(POINT(x + client_width / 2, y + client_height / 2), MONITOR_DEFAULTTONEAREST)
                   : ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
  MONITORINFO monitor_info;
  monitor_info.cbSize = sizeof(monitor_info);
  RECT full_rect(0, 0, client_width + 100, client_height + 100);  // Fallback monitor size.
  if (::GetMonitorInfo(monitor, &monitor_info)) full_rect = monitor_info.rcMonitor;

  int full_width = full_rect.right - full_rect.left;
  int full_height = full_rect.bottom - full_rect.top;
  bool full_screen =
      IsGameInitialized() && (full_screen_mode_ || ((client_width == full_width) && (client_height == full_height)));

  Logger::Info("EqGame: Full %d W %d H %d CW %d CH %d", full_screen, full_width, full_height, client_width,
               client_height);

  // Adjust the rectangle to include non-client areas (title bar, borders, etc.)
  int width = full_screen ? full_width : client_width;
  int height = full_screen ? full_height : client_height;
  DWORD style = full_screen ? kWindowStyleFullScreen : kWindowStyleNormal;
  DWORD ex_style = kWindowExStyle;
  RECT adjusted_rect = full_screen ? full_rect : RECT(0, 0, width, height);
  ::AdjustWindowRectEx(&adjusted_rect, style, FALSE, ex_style);
  width = adjusted_rect.right - adjusted_rect.left;
  height = adjusted_rect.bottom - adjusted_rect.top;

  if (full_screen) {
    x = adjusted_rect.left;  // Should be same as full_rect.left when using WS_POPUP style.
    y = adjusted_rect.top;
  } else if (!valid_offset) {
    x = (full_width - width) / 2;  // Calculate centered defaults.
    y = (full_height - height) / 2;
  }

  // Clamp x and y so it always remains visible on the target monitor.
  int x_min = full_rect.left - width + 256;  // Show at least 256 pixels on right side.
  int x_max = full_rect.right - 256;         // Show at least 256 pixels on left side.
  x = std::clamp(x, x_min, x_max);
  int y_min = full_rect.top - 20;      // Keep title bar accessible.
  int y_max = full_rect.bottom - 256;  // Keep title bar accessible above taskbar etc.
  y = std::clamp(y, y_min, y_max);
  Logger::Info("EqGame: Set window position: %d %d %d x %d", x, y, width, height);
  win_width_ = width;  // Cache the final sizes for locking it down.
  win_height_ = height;

  // Update the window style, size, and position.
  ::SetWindowLongA(hwnd_, GWL_STYLE, style);
  ::SetWindowPos(hwnd_, HWND_TOP, x, y, width, height, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
}

HMODULE WINAPI Kernel32LoadLibraryAHook(LPCSTR lpLibFileName) {
  HMODULE hmod = hook_LoadLibrary_.original(Kernel32LoadLibraryAHook)(lpLibFileName);
  if (hmod) {
    if (!_stricmp(lpLibFileName, "eqmain.dll")) {
      EqMain::Initialize(hmod, hwnd_, ini_path_, eqmain_init_fn_);
    }
    if (!_stricmp(lpLibFileName, "eqgfx_dx8.dll")) {
      EqGfx::Initialize(hmod, eqgfx_init_fn_, [](int width, int height) { SetClientSize(width, height); });
      CpuTimestampFix::Initialize(ini_path_);
    }
  }
  return hmod;
}

// Block the client from ever trying to capture the mouse.
HWND WINAPI User32SetCaptureHook(HWND hWnd) {
  Logger::Info("EqGame: Blocking SetCapture");
  GameInput::HandleGainOfFocus();  // Likely unnecessary but a convenient callback to reset inputs.
  return NULL;
}

// Block the game's disabling of the IDC_ARROW cursor (controlled by eqw).
HCURSOR WINAPI User32SetCursorHook(HCURSOR hcursor) {
  Logger::Info("EqGame: Blocking SetCursor: %d", (DWORD)hcursor);
  return hcursor;
}

// Disable the show cursor call to make the win32 cursor always visible (including title bar).
int WINAPI User32ShowCursorHook(BOOL show) {
  Logger::Info("EqGame: Overriding ShowCursor to set it visible");
  int count = ::ShowCursor(true);
  int target = 0;                                      // Make it precisely visible.
  while (count < target) count = ::ShowCursor(true);   // Count up to target.
  while (count > target) count = ::ShowCursor(false);  // Count down to target.
  return show ? 0 : -1;                                // Effectively a no-op that leaves it visible and lies about it.
}

// Replace the client supplied flags that request the window to be maximized.
bool WINAPI User32ShowWindowHook(HWND wnd, DWORD flags) {
  Logger::Info("EqGame: ShowWindow with SW_SHOW");
  return hook_ShowWindow_.original(User32ShowWindowHook)(wnd, SW_SHOW);
}

void InstallHooks(HMODULE handle) {
  hook_LoadLibrary_ = IATHook(handle, "kernel32.dll", "LoadLibraryA", Kernel32LoadLibraryAHook);
  hook_CreateWindow_ = IATHook(handle, "user32.dll", "CreateWindowExA", User32CreateWindowExAHook);
  hook_SetCapture_ = IATHook(handle, "user32.dll", "SetCapture", User32SetCaptureHook);
  hook_SetCursor_ = IATHook(handle, "user32.dll", "SetCursor", User32SetCursorHook);
  hook_ShowCursor_ = IATHook(handle, "user32.dll", "ShowCursor", User32ShowCursorHook);
  hook_ShowWindow_ = IATHook(handle, "user32.dll", "ShowWindow", User32ShowWindowHook);

  DInputManager::Initialize(handle);
}

// Paints the executable icon to the screen.
void PaintIcon(HWND hwnd) {
  if (!hicon_large_) return;

  PAINTSTRUCT ps;
  HDC hdc = ::BeginPaint(hwnd, &ps);
  int size = kIconSize;
  int left = (kStartupWidth - size) / 2;
  int top = (kStartupHeight - size) / 2;
  ::DrawIconEx(hdc, left, top, hicon_large_, size, size, 0, 0, DI_NORMAL);
  ::EndPaint(hwnd, &ps);
}

// Updates the stored window offsets for the active window.
void StoreWindowOffsets(HWND hwnd) {
  if (hwnd != hwnd_ || full_screen_mode_ || !IsGameInGameState() || ::IsIconic(hwnd_)) return;

  RECT rect;
  if (!::GetWindowRect(hwnd_, &rect)) return;
  rect.top = (rect.top > -20) ? rect.top : -20;             // Clamp top so the titlebar is always accessible.
  int client_width = *reinterpret_cast<int*>(0x00798564);   // Game global screen x resolution.
  int client_height = *reinterpret_cast<int*>(0x00798568);  // Game global screen y resolution.

  // Update the ini if needed.
  std::string resolution = std::to_string(client_width) + "by" + std::to_string(client_height);
  int left = Ini::GetValue<int>("EqwOffsets", resolution + "X", 0, ini_path_.string().c_str());
  int top = Ini::GetValue<int>("EqwOffsets", resolution + "Y", 0, ini_path_.string().c_str());
  if (left != rect.left) Ini::SetValue<int>("EqwOffsets", resolution + "X", rect.left, ini_path_.string().c_str());
  if (top != rect.top) Ini::SetValue<int>("EqwOffsets", resolution + "Y", rect.top, ini_path_.string().c_str());
}

// Toggles full screen mode.
void SetFullScreenMode(bool enable) {
  bool change = (full_screen_mode_ != enable);
  full_screen_mode_ = enable;
  Logger::Info("EqGame: Full screen mode set to: %d", full_screen_mode_);
  if (!change) return;

  // This client size call will handle the full screen window style or maximizing flag as
  // needed based on the d3d resolution versus screen resolution.
  int width = IsGameInitialized() ? *reinterpret_cast<int*>(0x00798564) : kStartupWidth;
  int height = IsGameInitialized() ? *reinterpret_cast<int*>(0x00798568) : kStartupHeight;
  SetClientSize(width, height);
}

// Window processing loop for the primary window. Note that it is replaced by eqmain when
// that is active. Both wndprocs execute on the main (startup) thread. The eqgame spins off
// a separate thread for the primary game processing loop, so this WndProc must be sensitive
// to cross thread synchronization.
LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  bool execute_eqgame_wndproc = false;
  switch (msg) {
    // Implement a hard close to kill the client process immediately.
    case WM_DESTROY:
    case WM_CLOSE:
      Logger::Info("EqGame: Terminating process");
      ::TerminateProcess(::GetCurrentProcess(), 0);
      break;

    // Lock down the window size (primarily for dpi changes).
    case WM_GETMINMAXINFO: {
      MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
      info->ptMaxTrackSize = {win_width_, win_height_};
      info->ptMinTrackSize = {win_width_, win_height_};
      return 0;
    }

    case WM_WINDOWPOSCHANGED:
      StoreWindowOffsets(hwnd);
      return 0;

    case WM_DPICHANGED:
      Logger::Info("EqMain::DpiChanged to %d", LOWORD(wParam));
      return 0;  // Skip default processing that would try to rescale.

    case WM_SETCURSOR:
      // The client wndproc always snuffs the cursor. We want it visible when the game's
      // rendered cursor is not.
      if (LOWORD(lParam) == HTCLIENT && ::GetForegroundWindow() == hwnd_) {
        ::SetCursor(NULL);
        return TRUE;
      }
      break;

    case WM_SYSCOMMAND:
      if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // Suppress alt activated menu keys.
      if ((wParam & 0xfff0) == SC_MAXIMIZE) {         // Use as a signal for re-entering eqmain mode.
        if (!IsGameInitialized()) SetClientSize(kStartupWidth, kStartupHeight, true);
        return 0;
      }
      break;

    case WM_PAINT:
      if (IsGameInitialized()) {        // Check if the primary game object is allocated.
        ::ValidateRect(hwnd, nullptr);  // Removes entire client area from the update region.
      } else {
        PaintIcon(hwnd);
      }
      break;

    // These messages have eqgame wndproc handling (that we are ignoring).
    case WM_GETTEXT:
    case WM_NCCALCSIZE:
      // execute_eqgame_wndproc = true;
      break;

    case WM_USER:
      SetFullScreenMode(wParam != 0);
      break;

    // Custom message to handle d3d reset and recovery on the correct thread.
    case EqGfx::kDeviceLostMsgId:
      if (wParam == EqGfx::kDeviceLostMsgId || wParam == kDeviceForceReset)
        EqGfx::HandleDeviceLost(wParam == kDeviceForceReset);
      break;

    default:
      break;
  }

  // Note: This wndproc is currently filtering messages < WM_USER since we are handling
  // the windowing critical ones and direct input is handling the keyboard and mouse input.
  // Note this includes blocking messages needed for IME (Input Method Editor).
  if (msg >= WM_USER || msg == WM_POWERBROADCAST) execute_eqgame_wndproc = true;
  if (execute_eqgame_wndproc && eqgame_wndproc_) return ::CallWindowProcA(eqgame_wndproc_, hwnd, msg, wParam, lParam);

  return ::DefWindowProcA(hwnd, msg, wParam, lParam);
}

// Opens a debug log file with the log level from settings.
void InitializeDebugLog() {
  int log_level = Ini::GetValue<int>("EqwGeneral", "DebugLogLevel", static_cast<int>(Logger::Level::None),
                                     ini_path_.string().c_str());
  std::filesystem::path log_file = exe_path_.parent_path() / "eqw_debug.txt";
  Logger::Initialize(log_file.string().c_str(), static_cast<Logger::Level>(log_level));

  Logger::Info("EQW version: %s", EqGame::kVersionStr);
  Logger::Info("EQW debug log (level = %d)", log_level);
  Logger::Info("Exe path: %s", exe_path_.string().c_str());
  Logger::Info("Ini path: %s", ini_path_.string().c_str());
}

// Extracts and stores the full path eqw.ini filename.
void InitializeIniFilename() {
  char buffer[FILENAME_MAX + 1];
  ::GetModuleFileNameA(NULL, buffer, FILENAME_MAX + 1);
  exe_path_ = std::filesystem::path(buffer);
  ini_path_ = exe_path_.parent_path() / "eqclient.ini";
}

void InitializeDpiAware() {
  auto disable_dpi = Ini::GetValue<bool>("EqwGeneral", "DisableDpiAware", false, ini_path_.string().c_str());
  if (disable_dpi) {
    Logger::Info("EqGame: Dpi aware is disabled");
    return;
  }

  // We set MONITOR_AWARE and not MONITOR_AWARE_V2 so we don't have to worry about the window sizes
  // changing due to non-client elements. This avoids the need for some later OS dpi aware calls.
  if (!::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
    Logger::Error("EqGame: Failed to set dpi aware: 0x%08x", ::GetLastError());

  Logger::Info("EqGame: Active dpi_awareness: %d",
               ::GetAwarenessFromDpiAwarenessContext(::GetDpiAwarenessContextForProcess(NULL)));
}

}  // namespace
}  // namespace EqGameInt

void EqGame::Initialize() {
  EqGameInt::InitializeIniFilename();
  EqGameInt::InitializeDebugLog();
  EqGameInt::InstallHooks(GetModuleHandleA(NULL));
  EqGameInt::InitializeDpiAware();
}

HWND EqGame::GetGameWindow() {
  return EqGameInt::hwnd_;  // Handle to common shared game window.
}

int EqGame::GetEnableFullScreen() {
  return EqGameInt::full_screen_mode_;  // Note cross-threading is possible.
}

void EqGame::SetEnableFullScreen(int enable) {
  // Note this is a cross-threading call (eq game processing thread is spun off
  // from the main thread with the wndproc) so it will block until that thread
  // processes the message in the queue.
  ::SendMessageA(EqGameInt::hwnd_, WM_USER, enable, 0);
}

void EqGame::SetEqMainInitFn(void(__cdecl* init_fn)()) { EqGameInt::eqmain_init_fn_ = init_fn; }

void EqGame::SetEqGfxInitFn(void(__cdecl* init_fn)()) { EqGameInt::eqgfx_init_fn_ = init_fn; }

void EqGame::SetEqCreateWinInitFn(void(__cdecl* init_fn)()) { EqGameInt::eqcreatewin_init_fn_ = init_fn; }

void EqGame::ResetD3D8() {
  // Note this is also a potential blocking cross-threading call.
  Logger::Info("EqGame: Sending ResetD3D8 request");
  ::SendMessage(EqGameInt::hwnd_, EqGfx::kDeviceLostMsgId, EqGameInt::kDeviceForceReset, 0);
}
