#include "game_input.h"

#include <windows.h>

#include <iostream>

#include "function_hook.h"

namespace GameInputInt {
namespace {

HWND hwnd_ = nullptr;
bool full_screen_mode_ = false;

bool swap_mouse_buttons_ = false;

// Mouse state.
POINT saved_rmouse_pt_ = {0, 0};

typedef int(__cdecl* GetMouseDataRel_t)();
FunctionHook hook_get_mouse_data_rel_((GetMouseDataRel_t)(nullptr));
typedef void(__fastcall* RightMouseDown_t)(void* this_ptr, int unused_edx, short x, short y);
FunctionHook hook_right_mouse_down_((RightMouseDown_t)(nullptr));
typedef void(__fastcall* RightMouseUp_t)(void* this_ptr, int unused_edx, short x, short y);
FunctionHook hook_right_mouse_up_((RightMouseUp_t)(nullptr));

// Client mouse related globals.
short* const g_mouse_x_abs_used_in_proc = (short*)0x00798580;
short* const g_mouse_y_abs_used_in_proc = (short*)0x00798582;
BYTE* const g_mouse_rmb_down_mouse_look = (BYTE*)0x007985ea;  // Set between rmb down to up.
BYTE* const g_mouse_lmb_down_previous = (BYTE*)0x00798614;
BYTE* const g_mouse_rmb_down_previous = (BYTE*)0x00798615;
BYTE* const g_mouse_rmb_down_from_dinput = (BYTE*)0x00798616;
BYTE* const g_mouse_lmb_down_from_dinput = (BYTE*)0x00798617;
long* const g_mouse_scroll_delta_ticks = (long*)0x007b9640;
long* const g_mouse_x_abs_from_dinput_state = (long*)0x008090a8;  // DINPUT state fetch object field.
long* const g_mouse_y_abs_from_dinput_state = (long*)0x008090ac;
long* const g_mouse_x_abs_from_dinput = (long*)0x008092e8;  // Internal accumulators.
long* const g_mouse_y_abs_from_dinput = (long*)0x008092ec;
short* const g_mouse_x_delta_from_dinput = (short*)0x008092f0;
short* const g_mouse_y_delta_from_dinput = (short*)0x008092f4;

long* const g_mouse_screen_mode = (long*)0x0063b918;
short* const g_mouse_screen_rect_left = (short*)0x00798548;
short* const g_mouse_screen_rect_top = (short*)0x0079854a;
short* const g_mouse_screen_rect_right = (short*)0x0079854c;
short* const g_mouse_screen_rect_bottom = (short*)0x0079854e;
long* const g_mouse_screen_res_x = (long*)0x00798564;
long* const g_mouse_screen_res_y = (long*)0x00798568;
BYTE* const g_mouse_new_ui = (BYTE*)0x008092d8;

// Keyboard support functions.

bool IsCtrlPressed() { return *(DWORD*)0x00809320 > 0; }

void SetCtrlKeyState(bool down) {
  *(DWORD*)0x0079973C = down;
  *(DWORD*)0x00809320 = down;
  DWORD ptr = *(DWORD*)0x00809DB4;
  if (ptr) *(BYTE*)(ptr + 86) = down;
}

bool IsAltPressed() { return *(DWORD*)0x0080932C > 0; }

void SetAltKeyState(bool down) {
  *(DWORD*)0x00799740 = down;
  *(DWORD*)0x0080932C = down;
  DWORD ptr = *(DWORD*)0x00809DB4;
  if (ptr) {
    *(BYTE*)(ptr + 87) = down;  // Left Alt
    *(BYTE*)(ptr + 88) = 0;     // Right Alt
  }
}

bool IsShiftPressed() { return *(DWORD*)0x0080931C > 0; }

void SetShiftKeyState(bool down) {
  *(DWORD*)0x00799738 = down;
  *(DWORD*)0x0080931C = down;
  DWORD ptr = *(DWORD*)0x00809DB4;
  if (ptr) *(BYTE*)(ptr + 85) = down;
}

void SetCapsLockState(bool down) {
  *(DWORD*)0x00809324 = down;
  DWORD ptr = *(DWORD*)0x00809DB4;
  if (ptr) *(BYTE*)(ptr + 84) = down;
}

void UpdateModifierKeyStates() {
  bool in_foreground = (GetForegroundWindow() == hwnd_);
  SetAltKeyState(in_foreground && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
  SetCtrlKeyState(in_foreground && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
  SetShiftKeyState(in_foreground && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
  SetCapsLockState(in_foreground && (GetAsyncKeyState(VK_CAPITAL) & 0x8000) != 0);
}

void ResetKeyboardState(bool perform_update_query) {
  // The client reserves an array of mapped action/keydown states of at least 0xcd
  // and based on the initialize and enter zone it is a full 0x100 array although
  // the majority of those are not used. Just wipe them all clean like zoning.
  const int kNumKeyStates = 0x100;
  memset(reinterpret_cast<void*>(0x007ce04c), 0, kNumKeyStates * sizeof(int));

  SetAltKeyState(false);
  SetCtrlKeyState(false);
  SetShiftKeyState(false);
  SetCapsLockState(false);
  *(DWORD*)0x00809328 = 0;  // Num lock state

  if (perform_update_query) UpdateModifierKeyStates();
}

// Sets all internal absolute mouse position state to (x,y).
void SetGameMousePosition(int x, int y) {
  *g_mouse_x_abs_from_dinput = x;
  *g_mouse_y_abs_from_dinput = y;
  *g_mouse_x_abs_from_dinput_state = x;
  *g_mouse_y_abs_from_dinput_state = y;
  *g_mouse_x_abs_used_in_proc = x;
  *g_mouse_y_abs_used_in_proc = y;
}

// Resets the internal mouse state and makes the internal cursor invisible.
void ResetMouseUpdateValues() {
  SetGameMousePosition(32767, 32767);  // Off-screen so invisible and can't 'misclick' on something.
  *g_mouse_rmb_down_mouse_look = 0;    // Disable all other dinput related outputs and state.
  *g_mouse_lmb_down_previous = 0;
  *g_mouse_rmb_down_previous = 0;
  *g_mouse_rmb_down_from_dinput = 0;
  *g_mouse_lmb_down_from_dinput = 0;
  *g_mouse_x_delta_from_dinput = 0;
  *g_mouse_y_delta_from_dinput = 0;
  *g_mouse_scroll_delta_ticks = 0;
}

// Synchronizes the internal cursor position to the win32 cursor for smooth transitions.
void SyncToWin32Cursor() {
  POINT cursor;
  GetCursorPos(&cursor);  // This returns absolute screen x, y.
  POINT corner = {0};
  ClientToScreen(hwnd_, &corner);
  cursor.x -= corner.x;
  cursor.y -= corner.y;
  if (full_screen_mode_) {  // In full screen mode the image may be scaled.
    RECT rect;
    ::GetClientRect(hwnd_, &rect);
    bool mode = (*g_mouse_screen_mode == 1);  // Logic copied from SetMouseCenter.
    int width = mode ? *g_mouse_screen_res_x : (*g_mouse_screen_rect_left + *g_mouse_screen_rect_right);
    int height = mode ? *g_mouse_screen_res_y : (*g_mouse_screen_rect_top + *g_mouse_screen_rect_bottom);
    cursor.x = cursor.x * width / (rect.right - rect.left);
    cursor.y = cursor.y * height / (rect.bottom - rect.top);
  }
  SetGameMousePosition(cursor.x, cursor.y);
}

// Synchronizes the win32 cursor to the internal cursor position.
void SetWin32CursorToClientPosition(POINT pt) {
  if (!full_screen_mode_) {
    ClientToScreen(hwnd_, &pt);
  } else {
    RECT rect;
    ::GetClientRect(hwnd_, &rect);
    POINT corner = {0};
    ClientToScreen(hwnd_, &corner);
    bool mode = (*g_mouse_screen_mode == 1);  // Logic copied from SetMouseCenter.
    int width = mode ? *g_mouse_screen_res_x : (*g_mouse_screen_rect_left + *g_mouse_screen_rect_right);
    int height = mode ? *g_mouse_screen_res_y : (*g_mouse_screen_rect_top + *g_mouse_screen_rect_bottom);
    pt.x = pt.x * (rect.right - rect.left) / width + corner.x;
    pt.y = pt.y * (rect.bottom - rect.top) / height + corner.y;
  }

  SetCursorPos(pt.x, pt.y);
}

// Sets the internal cursor location and synchronizes the win32 cursor with it.
void SetBothCursorsToClientPosition(POINT pt) {
  SetGameMousePosition(pt.x, pt.y);
  SetWin32CursorToClientPosition(pt);
}

// Sets the internal cursor location to the middle of the screen.
void SetWin32CursorToCenter() {
  bool mode = (*g_mouse_screen_mode == 1);  // Logic copied from SetMouseCenter.
  int width = mode ? *g_mouse_screen_res_x : (*g_mouse_screen_rect_left + *g_mouse_screen_rect_right);
  int height = mode ? *g_mouse_screen_res_y : (*g_mouse_screen_rect_top + *g_mouse_screen_rect_bottom);
  POINT center = {width / 2, height / 2};
  SetWin32CursorToClientPosition(center);
}

// Returns true if the mouse is over a visible client window.
bool IsMouseOverClient() {
  if (!::IsWindowVisible(hwnd_)) return false;

  POINT cursor;
  ::GetCursorPos(&cursor);  // This returns absolute screen x, y.
  POINT offset = {0, 0};
  ::ClientToScreen(hwnd_, &offset);
  cursor.x -= offset.x;
  cursor.y -= offset.y;
  RECT rect;
  ::GetClientRect(hwnd_, &rect);
  return ::PtInRect(&rect, cursor);
}

// This hook is called near the beginning of input processing in each of the primary game loops.
// The windows message pump loop is running in a separate thread, so this code does brute force
// "synchronous" polling of the current window state and cursor position.
//
// It is only called when in the game or char select game states and it is skipped if the
// character is dead, stunned, or frozen and also skipped if the ui is safelocked (but the
// game ignores mouse input during that time anyways).
//
// It tracks two primary states:
// - The window has lost focus or the cursor is out of the client space
//  - Mouse inputs are ignored and win32 cursor is visible
// - The cursor is over the client space (or rmb is down and crossing the edge)
//  - Mouse inputs are active and the game cursor is enabled

int __cdecl GetMouseDataRelHook() {
  static bool mouse_disabled = true;
  static bool prev_has_focus = false;

  bool has_focus = (::GetForegroundWindow() == hwnd_ && !::IsIconic(hwnd_));
  bool over_client = IsMouseOverClient();
  bool internal_mode = has_focus && (over_client || *g_mouse_rmb_down_mouse_look);

  if (prev_has_focus != has_focus) {
    prev_has_focus = has_focus;
    ResetKeyboardState(has_focus);
  }

  if (!internal_mode) {
    mouse_disabled = true;
    ResetMouseUpdateValues();  // Drains the buffers and moves the cursor off screen.
    return 0;
  }

  if (mouse_disabled) {
    mouse_disabled = false;
    SyncToWin32Cursor();                              // Updates internal cursor position to match win32.
    ::SendMessage(hwnd_, WM_SETCURSOR, 0, HTCLIENT);  // Queue the WndProc to update the cursor visibility.
  }

  unsigned int result = hook_get_mouse_data_rel_.original(GetMouseDataRelHook)();

  // Handle button swap option.
  if (swap_mouse_buttons_) {
    BYTE temp = *g_mouse_rmb_down_from_dinput;
    *g_mouse_rmb_down_from_dinput = *g_mouse_lmb_down_from_dinput;
    *g_mouse_lmb_down_from_dinput = temp;
  }

  if (*g_mouse_rmb_down_mouse_look) {
    // Lock the game cursor to the initial position and the win32 cursor to the middle to avoid glitching
    // across the window edge. This is a brute force alternative versus using ClipCapture() with wndproc.
    SetGameMousePosition(saved_rmouse_pt_.x, saved_rmouse_pt_.y);
    SetWin32CursorToCenter();
  } else {
    SyncToWin32Cursor();  // Override the internal absolute cursor position with the win32 cursor value.
  }

  return result;
}

void __fastcall RightMouseUpHook(void* this_ptr, int unused_edx, short x, short y) {
  bool mouse_look_active = *g_mouse_rmb_down_mouse_look;
  hook_right_mouse_up_.original(RightMouseUpHook)(this_ptr, unused_edx, x, y);

  // The clamp during mouse_look keeps the win32 cursor in the enter to avoid boundary glitching,
  // and the call above puts the game cursor in the the middle at the end, so we restore both to
  // the starting state when it exits mouse_look.
  if (mouse_look_active && !*g_mouse_rmb_down_mouse_look) SetBothCursorsToClientPosition(saved_rmouse_pt_);
}

void __fastcall RightMouseDownHook(void* this_ptr, int unused_edx, short x, short y) {
  hook_right_mouse_down_.original(RightMouseDownHook)(this_ptr, unused_edx, x, y);

  // The call above updates the mouse_look state and this call only happens upon the RMB down event so
  // we capture the abs values here to know the starting location of mouse look.
  if (*g_mouse_rmb_down_mouse_look) {
    saved_rmouse_pt_.x = *g_mouse_x_abs_from_dinput;  // These values are clamped to within the screen res.
    saved_rmouse_pt_.y = *g_mouse_y_abs_from_dinput;
    SetWin32CursorToCenter();  // Centered so we don't glitch off the window.
  }
}

void Initialize(HWND hwnd) {
  hwnd_ = hwnd;
  full_screen_mode_ = false;
  swap_mouse_buttons_ = false;
  saved_rmouse_pt_ = {0, 0};

  hook_get_mouse_data_rel_.Initialize(0x0055B3B9, GetMouseDataRelHook, FunctionHook::HookType::Detour);
  hook_right_mouse_down_.Initialize(0x0054699d, RightMouseDownHook, FunctionHook::HookType::Detour);
  hook_right_mouse_up_.Initialize(0x00546b71, RightMouseUpHook, FunctionHook::HookType::Detour);
}

}  // namespace
}  // namespace GameInputInt

void GameInput::Initialize(HWND hwnd) {
  GameInputInt::Initialize(hwnd);  // Resets state and installs the hooks.
}

void GameInput::HandleGainOfFocus() {
  GameInputInt::ResetKeyboardState(true);  // Flushes and ensures our modifier keys are up to date.
}

void GameInput::HandleLossOfFocus() {
  // The mouse handling hooks will handle the loss of focus.
  GameInputInt::ResetKeyboardState(false);  // But we want to wipe internal keyboard state.
}
