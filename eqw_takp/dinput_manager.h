#pragma once
#include <windows.h>

// This namespace aggregates the Direct Input related methods. The methods
// manipulate internal global state and hardware resources and are used
// by both the primary eqgame window and eqmain startup windows.
//
// The eqmain code requires operation in background nonexclusive mode since
// it will crash out if the keyboard acquire() fails while the primary
// game code runs in foreground nonexclusive which will automatically
// unacquire at loss of focus. The primary game code also internally
// tries to re-acquire each game loop so manual Acquire() calls are not
// necessary.
//
// This code performs a flush of the devices when they are acquired and
// patches a bug where the client assumes that a failed GetDeviceData()
// will set the number of read elements to zero.

namespace DInputManager {

void Initialize(HMODULE handle);
void SetBackgroundMode(bool background_mode);  // Set during eqmain.
void Acquire(bool keyboard_only = false);
void Unacquire();
};  // namespace DInputManager
