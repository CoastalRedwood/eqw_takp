#pragma once
#include <windows.h>

// Handles low-level interactions with the TAKP client to implement reliable
// windowed keyboard and mouse functionality.

namespace GameInput {

void Initialize(HWND hwnd);  // Resets state and installs client hooks.
void HandleLossOfFocus();    // Resets state when the client loses focus.
void HandleGainOfFocus();    // Updates state when the client regains focus.

}  // namespace GameInput
