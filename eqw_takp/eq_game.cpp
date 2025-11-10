#include "eq_game.h"

#include <ddraw.h>
#include <shellapi.h>  // For ExtractIconExA
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>

#include "cpu_timestamp_fix.h"
#include "dinput_manager.h"
#include "eq_gfx.h"
#include "eq_main.h"
#include "game_input.h"
#include "iat_hook.h"
#include "ini.h"
#include "vtable_hook.h"

// Using an EqGameInt namespace instead of a purely static class to reduce the qualifier clutter. The
// anonymous namespace forces it to private internal scope.
namespace EqGameInt {
namespace {
static constexpr int kStartupWidth = 640;  // Default eqmain size.
static constexpr int kStartupHeight = 480;
static constexpr DWORD kWindowExStyle = 0;  // No flags.
static constexpr DWORD kWindowStyleNormal = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME;
static constexpr DWORD kWindowStyleFullScreen = WS_POPUPWINDOW | WS_VISIBLE;

// State and resources updated at initialization.
std::ofstream debug_log_file_;    // Status updates are streamed here.
std::filesystem::path exe_path_;  // Full path filename for eqgame.exe file.
std::filesystem::path ini_path_;  // Full path filename for eqw.ini file.
HWND hwnd_ = nullptr;             // Primary shared visible window handle.
HICON hicon_large_ = nullptr;     // Icons retrieved from the executable.
HICON hicon_small_ = nullptr;     // Icons retrieved from the executable.
void(__cdecl* eqmain_init_fn_)() = nullptr;
void(__cdecl* eqgfx_init_fn_)() = nullptr;

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
bool isFocused_ = false;

// Internal methods.

LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Returns true if the primary EQ game object is allocated.
bool IsGameInitialized() {
  return (*(void**)0x00809478 != nullptr);  // Created after login and destroyed before returning.
}

// Returns true if in the active in world game state.
bool IsGameInGameState() {
  const int* eq = *reinterpret_cast<int**>(0x00809478);
  return eq && eq[0x5AC / 4] == 5;  // Check the game state stored in the EQ object.
}

// Creates the common, shared window used by eqgame and eqmain.
void CreateEqWindow() {
  std::cout << "CreateEqWindow()" << std::endl;

  // Extract the first large and small icon
  HICON large_icons[1];
  HICON small_icons[1];
  UINT extracted_count = ::ExtractIconExA(exe_path_.string().c_str(), 0, large_icons, small_icons, 1);
  if (extracted_count) {
    hicon_large_ = large_icons[0];
    hicon_small_ = small_icons[0];
  }

  WNDCLASSA wc = {};
  wc.lpfnWndProc = GameWndProc;  // Custom handler to intercept messages.
  wc.hIcon = hicon_small_;
  wc.hInstance = GetModuleHandleA(NULL);
  wc.hCursor = ::LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
  wc.lpszClassName = "_EqWwndclass";
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));  // Black background.
  if (RegisterClassA(&wc) == 0) {
    // Handle registration failure
    MessageBoxA(NULL, "Window class registration failed!", "Error", MB_OK | MB_ICONERROR);
    return;
  }

  // Adjust the rect for the specified window style
  RECT rect = {0, 0, 640, 480};        // Client area size
  DWORD dwexstyle = kWindowExStyle;    // Fixed (no) flags.
  DWORD dwstyle = kWindowStyleNormal;  // TODO: Read ini file?
  AdjustWindowRectEx(&rect, dwstyle, FALSE, 0);
  int win_width = rect.right - rect.left;
  int win_height = rect.bottom - rect.top;
  int x = (GetSystemMetrics(SM_CXSCREEN) - win_width) / 2;
  int y = (GetSystemMetrics(SM_CYSCREEN) - win_height) / 2;

  hwnd_ = CreateWindowExA(dwexstyle, wc.lpszClassName, "EqW-TAKP", dwstyle, x, y, win_width, win_height, NULL, NULL,
                          NULL, NULL);
  EqGfx::SetWindow(hwnd_);

  full_screen_mode_ = Ini::GetValue<bool>("EqwGeneral", "FullScreenMode", ini_path_.string().c_str());
  bool swap_mouse_buttons = Ini::GetValue<bool>("EqwGeneral", "SwapMouseButtons", ini_path_.string().c_str());
  Ini::SetValue("EqwGeneral", "FullScreenMode", full_screen_mode_, ini_path_.string().c_str());
  Ini::SetValue("EqwGeneral", "SwapMouseButtons", swap_mouse_buttons, ini_path_.string().c_str());
  GameInput::Initialize(hwnd_, swap_mouse_buttons);  // Install mouse and keyboard handling hooks.
}

// The game client creates a window with the flags set to topmost maximize it, which we ignore.
HWND WINAPI User32CreateWindowExAHook(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X,
                                      int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance,
                                      LPVOID lpParam) {
  std::cout << "EqGame: Create Window, ignoring size: " << X << " " << Y << " " << nWidth << " " << nHeight
            << std::endl;
  if (!hwnd_) CreateEqWindow();

  // It should now be safe to connect the client's wndproc handler. We could sniff this value with
  // a hooked RegisterClass that copies the wndproc from that, but this client has a hardcoded address.
  eqgame_wndproc_ = (WNDPROC)(0x0055a4f4);

  return hwnd_;
}

void SetClientSize(int client_width, int client_height, bool center = false) {
  // Constrain the minimum sizes.
  if (client_width < kStartupHeight || client_height < kStartupWidth) {
    std::cout << "Preventing small client size: " << client_width << " x " << client_height << std::endl;
    client_width = max(client_width, kStartupWidth);
    client_height = max(client_height, kStartupHeight);
    center = true;
  }

  // Retrieve the screen coordinates of the monitor with the most current window overlap.
  MONITORINFO monitor_info;
  monitor_info.cbSize = sizeof(monitor_info);
  GetMonitorInfo(::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor_info);
  RECT rect(monitor_info.rcMonitor);
  int full_width = rect.right - rect.left;
  int full_height = rect.bottom - rect.top;
  bool full_screen =
      IsGameInitialized() && (full_screen_mode_ || ((client_width == full_width) && (client_height == full_height)));

  std::cout << "EqGame: Full " << full_screen << " W " << full_width << " H " << full_height;
  std::cout << " CW " << client_width << " CH " << client_height << std::endl;

  // Adjust the rectangle to include non-client areas (title bar, borders, etc.)
  int width = full_screen ? full_width : client_width;
  int height = full_screen ? full_height : client_height;
  DWORD style = full_screen ? kWindowStyleFullScreen : kWindowStyleNormal;
  DWORD ex_style = kWindowExStyle;
  RECT desiredRect = {0, 0, width, height};
  AdjustWindowRectEx(&desiredRect, style, FALSE, ex_style);
  width = desiredRect.right - desiredRect.left;
  height = desiredRect.bottom - desiredRect.top;

  int x = 0;
  int y = 0;
  if (full_screen || center) {
    x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
  } else {
    std::stringstream ss;
    ss << client_width << "by" << client_height;
    x = Ini::GetValue<int>("EqwOffsets", ss.str() + "X", ini_path_.string().c_str());
    y = Ini::GetValue<int>("EqWOffsets", ss.str() + "Y", ini_path_.string().c_str());
  }
  std::cout << "EqGame: Set window position: " << x << " " << y << " " << width << " x " << height << std::endl;

  // Update the window style, size, and position.
  ::SetWindowLongA(hwnd_, GWL_STYLE, style);
  ::SetWindowPos(hwnd_, HWND_TOP, x, y, width, height, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_SHOWWINDOW);
}

HMODULE WINAPI Kernel32LoadLibraryAHook(LPCSTR lpLibFileName) {
  HMODULE hmod = hook_LoadLibrary_.original(Kernel32LoadLibraryAHook)(lpLibFileName);
  if (hmod) {
    if (!_stricmp(lpLibFileName, "eqmain.dll")) {
      EqMain::Initialize(hmod, hwnd_, eqmain_init_fn_);
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
  std::cout << "EqGame: Blocking SetCapture" << std::endl;
  GameInput::HandleGainOfFocus();  // Likely unnecessary but a convenient callback to reset inputs.
  return NULL;
}

// Block the game's disabling of the IDC_ARROW cursor (controlled by eqw).
HCURSOR WINAPI User32SetCursorHook(HCURSOR hcursor) {
  std::cout << "EqGame: Blocking SetCursor: " << (DWORD)hcursor << std::endl;
  return hcursor;
}

// Disable the show cursor call to make the win32 cursor always visible (including title bar).
int WINAPI User32ShowCursorHook(BOOL show) {
  std::cout << "EqGame: Overriding ShowCursor to set it visible" << std::endl;
  int count = ::ShowCursor(true);
  int target = 0;                                      // Make it precisely visible.
  while (count < target) count = ::ShowCursor(true);   // Count up to target.
  while (count > target) count = ::ShowCursor(false);  // Count down to target.
  return show ? 0 : -1;                                // Effectively a no-op that leaves it visible and lies about it.
}

// Replace the client supplied flags that request the window to be maximized.
bool WINAPI User32ShowWindowHook(HWND wnd, DWORD flags) {
  std::cout << "EqGame: ShowWindow with SW_SHOW." << std::endl;
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
  HDC hdc = BeginPaint(hwnd, &ps);
  int size = 128;
  int left = (kStartupWidth - size) / 2;
  int top = (kStartupHeight - size) / 2;
  DrawIconEx(hdc, left, top, hicon_large_, size, size, 0, 0, DI_NORMAL);
  EndPaint(hwnd, &ps);
}

// Updates the stored window offsets for the active window.
void StoreWindowOffsets() {
  if (full_screen_mode_ || !IsGameInGameState()) return;

  RECT rect;
  if (!::GetWindowRect(hwnd_, &rect)) return;
  int client_width = *reinterpret_cast<int*>(0x00798564);   // Game global screen x resolution.
  int client_height = *reinterpret_cast<int*>(0x00798568);  // Game global screen y resolution.

  std::stringstream ss;
  ss << client_width << "by" << client_height;
  Ini::SetValue("EqwOffsets", ss.str() + "X", rect.left, ini_path_.string().c_str());
  Ini::SetValue("EqwOffsets", ss.str() + "Y", rect.top, ini_path_.string().c_str());
}

// Toggles full screen mode.
void SetFullScreenMode(bool enable) {
  bool change = (full_screen_mode_ != enable);
  full_screen_mode_ = enable;
  std::cout << "Full screen mode set to: " << full_screen_mode_ << std::endl;
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
      std::cout << "EqGame: Terminating process" << std::endl;
      ::TerminateProcess(::GetCurrentProcess(), 0);
      break;

    case WM_WINDOWPOSCHANGED:
      StoreWindowOffsets();
      return 0;

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
      if ((wParam & 0xfff0) == SC_MAXIMIZE) {
        if (!IsGameInitialized()) SetClientSize(kStartupWidth, kStartupHeight, true);
        return 0;
      }
      break;

    case WM_PAINT:
      if (IsGameInitialized()) {      // Check if the primary game object is allocated.
        ValidateRect(hwnd, nullptr);  // Removes entire client area from the update region.
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

// Opens a debug log file and re-routes std::cout to it.
void InitializeDebugLog() {
  debug_log_file_.open("eqw_debug.txt");
  if (!debug_log_file_.is_open()) return;

  std::cout.rdbuf(debug_log_file_.rdbuf());
  std::cout << std::unitbuf;  // Ensure immediate flushing.

  std::cout << "EQW debug log" << std::endl;
}

// Extracts and stores the full path eqw.ini filename.
void InitializeIniFilename() {
  char buffer[FILENAME_MAX + 1];
  ::GetModuleFileNameA(NULL, buffer, FILENAME_MAX + 1);
  exe_path_ = std::filesystem::path(buffer);
  std::cout << "Exe path: " << exe_path_.string() << std::endl;
  ini_path_ = exe_path_.parent_path() / "eqclient.ini";
  std::cout << "ini path: " << ini_path_.string() << std::endl;
}
}  // namespace
}  // namespace EqGameInt

void EqGame::Initialize() {
  EqGameInt::InitializeDebugLog();
  EqGameInt::InitializeIniFilename();
  EqGameInt::InstallHooks(GetModuleHandleA(NULL));
}

HWND EqGame::GetGameWindow() {
  return EqGameInt::hwnd_;  // Handle to common shared game window.
}

int EqGame::GetEnableFullScreen() {
  return EqGameInt::full_screen_mode_;  // Note cross-threading is possible.
}

void EqGame::SetEnableFullScreen(int enable) {
  // Note this is a cross-threading call (eq game processingi thread is spun off
  // from the main thread with the wndproc) so it will block until that thread
  // processes the message in the queue.
  SendMessageA(EqGameInt::hwnd_, WM_USER, enable, 0);
}

void EqGame::SetEqMainInitFn(void(__cdecl* init_fn)()) { EqGameInt::eqmain_init_fn_ = init_fn; }

void EqGame::SetEqGfxInitFn(void(__cdecl* init_fn)()) { EqGameInt::eqgfx_init_fn_ = init_fn; }
