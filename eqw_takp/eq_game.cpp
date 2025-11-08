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

// TODO: Stability and compatibility
// - d3d8to9: No issues.
// - intel gpu w/out d3d8.dll: black screen (but game is running in background with sounds and ui)
// - dgvoodoo:
//   - Crashes char select -> login w/out ever entering world (dx 6.0 error dialog, same as old eqw)
//   - It is failing a DirectDrawCreate() inside an early eqmain quality check (dx 6.0 error)
//   - Without dgvoodoo ddraw.dll it hangs with a black screen trying to go back into eqmain
//
// - Review threading: EqGame is spinning off a separate processing thread for the primary
//   game loops so think through threading and synchronization of video res changes
//
// TODO: Features / polishing
//// - Add additional dll exports along the lines of GetGameWindow()
//   - maybe swapmouse, fullscreen, resolution?
// - Transition glitches:
//   - Some title bars are squared vs rounded temporarily
//   - Some dirty screens are briefly flashed to/from char select
// - Improve INI settings (location, storing, sanity check values, other options)
// - Video modes: changing in ini, changing in-game, full screen mode
// - Support mouse button swap
// - Clean up debug logging
//
// OTHER:
// - Zeal breakage: eqw get_game_window() needs update, external map window is broken

// Using an EqGameInt namespace instead of a purely static class to reduce the qualifier clutter. The
// anonymous namespace forces it to private internal scope.
namespace EqGameInt {
namespace {
static constexpr int kStartupWidth = 640;  // Default eqmain size.
static constexpr int kStartupHeight = 480;
static constexpr DWORD kWindowStyleNormal = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME;
static constexpr DWORD kWindowStyleFullScreen = WS_POPUPWINDOW | WS_VISIBLE;  // TODO: | WS_MAXIMIZE?

// State and resources updated at initialization.
std::ofstream debug_log_file_;    // Status updates are streamed here.
std::filesystem::path exe_path_;  // Full path filename for eqgame.exe file.
std::filesystem::path ini_path_;  // Full path filename for eqw.ini file.
HWND hwnd_ = nullptr;             // Primary shared visible window handle.
HICON hicon_large_ = nullptr;     // Icons retrieved from the executable.
HICON hicon_small_ = nullptr;     // Icons retrieved from the executable.

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
RECT client_rect_ = {0, 0, 640, 480};    // Screen coordinates of client rect.
RECT stored_dimensions_ = client_rect_;  // Stored when maximized.

// Internal methods.

LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Updates the client_rect_ variable to the current screen coordinates.
void UpdateClientRegion() {
  // Convert the client area point (0, 0) to screen coordinates and add it to the current client rect size.
  POINT offset = {0, 0};
  ::ClientToScreen(hwnd_, &offset);
  ::GetClientRect(hwnd_, &client_rect_);
  client_rect_ = {offset.x + client_rect_.left, offset.y + client_rect_.top, offset.x + client_rect_.right,
                  offset.y + client_rect_.bottom};
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
  DWORD dwstyle = kWindowStyleNormal;  // TODO: Read ini file?
  AdjustWindowRectEx(&rect, dwstyle, FALSE, 0);
  int win_width = rect.right - rect.left;
  int win_height = rect.bottom - rect.top;
  int x = (GetSystemMetrics(SM_CXSCREEN) - win_width) / 2;
  int y = (GetSystemMetrics(SM_CYSCREEN) - win_height) / 2;

  hwnd_ =
      CreateWindowExA(0, wc.lpszClassName, "EqW-TAKP", dwstyle, x, y, win_width, win_height, NULL, NULL, NULL, NULL);
  EqGfx::SetWindow(hwnd_);
  UpdateClientRegion();

  GameInput::Initialize(hwnd_);  // Install mouse and keyboard handling hooks.

  // ShowWindow(hwnd_, SW_SHOW);
  // UpdateWindow(hwnd_);  // This forces the window to be updated and painted
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
  // Adjust the rectangle to include non-client areas (title bar, borders, etc.)
  DWORD dwStyle = (DWORD)GetWindowLong(hwnd_, GWL_STYLE);
  DWORD dwExStyle = (DWORD)GetWindowLong(hwnd_, GWL_EXSTYLE);
  RECT desiredRect = {0, 0, client_width, client_height};
  AdjustWindowRectEx(&desiredRect, dwStyle, FALSE, dwExStyle);

  // Calculate the width and height including non-client areas
  int width = desiredRect.right - desiredRect.left;
  int height = desiredRect.bottom - desiredRect.top;

  int x = 0;
  int y = 0;
  if (center) {
    x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
  } else {
    std::stringstream ss;
    ss << client_width << "by" << client_height;
    x = Ini::GetValue<int>("Positions", ss.str() + "X", ini_path_.string().c_str());
    y = Ini::GetValue<int>("Positions", ss.str() + "Y", ini_path_.string().c_str());
  }
  std::cout << "EqGame: Set window position: " << x << " " << y << " " << width << " x " << height << std::endl;

  // Set the window size and position
  ::SetWindowPos(hwnd_, NULL, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
  UpdateClientRegion();
}

HMODULE WINAPI Kernel32LoadLibraryAHook(LPCSTR lpLibFileName) {
  HMODULE hmod = hook_LoadLibrary_.original(Kernel32LoadLibraryAHook)(lpLibFileName);
  if (hmod) {
    if (!_stricmp(lpLibFileName, "eqmain.dll")) {
      EqMain::Initialize(hmod, hwnd_);
    }
    if (!_stricmp(lpLibFileName, "eqgfx_dx8.dll")) {
      EqGfx::Initialize(hmod, [](int width, int height) { SetClientSize(width, height); });
      CpuTimestampFix::Initialize(ini_path_);
    }
  }
  return hmod;
}

// Block the client from ever trying to capture the mouse.
HWND WINAPI User32SetCaptureHook(HWND hWnd) {
  // ::ShowWindow(hWnd, SW_HIDE); // TODO TODO: Try to force an activate
  GameInput::HandleGainOfFocus();  // TODO TODO Testing
  std::cout << "EqGame: Blocking SetCapture" << std::endl;
  return NULL;
}

// Block the disabling of the IDC_ARROW cursor.
HCURSOR WINAPI User32SetCursorHook(HCURSOR hcursor) {
  std::cout << "EqGame: Blocking SetCursor: " << (DWORD)hcursor << std::endl;
  return hcursor;
}

// Disable the show cursor call to make the win32 cursor always visible (including title bar).
int WINAPI User32ShowCursorHook(BOOL show) {
  std::cout << "EqGame: Ignoring ShowCursor and setting it visible"
            << std::endl;  // TODO Blocking ShowCursor: " << show << std::endl;
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
  hook_SetCursor_ = IATHook(handle, "user32.dll", "SetCursor", User32SetCursorHook);     // TODO: TBD if needed.
  hook_ShowCursor_ = IATHook(handle, "user32.dll", "ShowCursor", User32ShowCursorHook);  // TODO: TBD if needed.
  hook_ShowWindow_ = IATHook(handle, "user32.dll", "ShowWindow", User32ShowWindowHook);

  DInputManager::Initialize(handle);
}

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
      UpdateClientRegion();  // TODO: Force the client rect to be the surface size (Threading?)
      // TODO: Save the ini location.
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
        if (*(void **)0x00809478 == nullptr) SetClientSize(kStartupWidth, kStartupHeight, true);
        return 0;
      }
      break;

    case WM_PAINT:
      if (*(void **)0x00809478 != nullptr) {  // Check if the primary game object is allocated.
        ValidateRect(hwnd, nullptr);          // Removes entire client area from the update region.
      } else {
        PaintIcon(hwnd);
      }
      break;

    // These messages have eqgame wndproc handling (that we are ignoring).
    case WM_GETTEXT:
    case WM_NCCALCSIZE:
      // execute_eqgame_wndproc = true;
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
  ini_path_ = exe_path_.parent_path() / "eqw.ini";
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
