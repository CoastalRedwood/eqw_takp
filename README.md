# Windowed-mode support for the TAKP client.

Eqw TAKP custom code is entirely open source. The releases are built by github servers
directly from the repo source, providing full transparency on the release contents.

## Features
  - Significantly improved mouse and keyboard input behavior
    - Eliminates phantom mouse and keyboard input glitches when switching focus back to game
    - Wipes keydown states so game input goes to neutral (except autorun) when losing focus
    - Synchronizes the game mouse position and win 32 cursor improvement to eliminate surprise
      off screen clicks and loss of focus (also matches sensitivities across windows)
    - Better locking of the mouse during RMB mouse look
    - Reliable window clicking when clicking back into the game
    - Cleaner cursor transitions across game window boundaries and as indicator of focus
    - Should eliminate the common dinput mutex crash in current eqw
  - Windowing mode improvements and features:
    - Supports in-game setting of video modes (32-bit resolutions) without crashing
    - Supports in-game toggling of stretched full screen mode (requires eqgame.dll support to activate)
    - Automatically goes borderless if the ini resolution is set to match the monitor
    - Generally cleaner window transitions and some polish like icons
  - All ini settings were moved to eqclient.ini and adds ini options settings for:
    - High frequency cpu timebase fix
    - Swapping left / mouse buttons
    - Stretched full screen mode
    - Login window and per resolution game window positions
  - Supports lighter weight dgvoodoo alternative d3d8to9 (below)

## Installation
  - Download the [latest release](https://github.com/CoastalRedwood/eqw_takp/releases/latest) eqw.dll
    and copy it to your client game directory
    - Note: Browsers may complain about downloading a dll from a website. See comment at top that
      the dll content is entirely transparent and compiled directly on github.
  - Install (copy over) a compatible (updated) eqgame.dll file
  - If using Zeal a version newer than 1.3.0-beta2 is required for lmb panning to function
  - Review the Settings section below if any desire to modify defaults
    - First run the game once to populate defaults and then modify
  - See compatibility notes below for dgvoodoo or d3d8tod9

## Settings
The eqw settings are stored in the `eqclient.ini` file under the following sections. The
general ones are read at boot time while the offsets are written and read while in the game.

### `[EqwGeneral]`
- `FullScreenMode`
  - **Values:** `FALSE` (default) or `TRUE`
  - **Description:** Setting `TRUE` will enable stretching to full screen when the
                     specified resolution is less than the full screen size. Note that
                     a better alternative is to set the `[VideoMode]` `Width` and
                     `Height` to the monitor resolution which avoids stretching and
                     automatically removes the titlebar and border.
  
- `SwapMouseButtons`
  - **Values:** `FALSE` (default) or `TRUE`
  - **Description:** Setting `TRUE` will swap the left and right mouse buttons.
  
- `DisableCpuTimebaseFix`
  - **Values:** `FALSE` (default) or `TRUE`
  - **Description:** Setting `TRUE` will disable the patch that implements a more
                     accurate timebase counter (high frequency cpu fix).

- `DebugLogLevel`
  - **Values:** `0` (default=None), `1` (Error) or `2` (Info)
  - **Description:** Setting non-zero will enable the output of an `eqw_debug.txt` with
                     logging messages from the code.

### `[EqwOffsets]`
- `<width>by<height>X`
- `<width>by<height>Y`
  - **Values:** Integer values in screen pixels.
  - **Description:** The X and Y offsets of the windows at resolution `<width>` by `<height>`
  - **Note:** When changing video modes in game, both the original and new are reset to zero
              x and y offsets for the current monitor for safe starting position and recovery.

- `LoginX`
- `LoginY`
  - **Values:** Integer values in screen pixels.
  - **Description:** The X and Y offsets of the startup and login windows.

 ### Existing ini fields
 - The `[VideoMode]: RefreshRate` value is ignored
 - The `[VideoMode]: BitsPerPixel` value should be set to `32` (others are untested)

## Unsupported legacy EqW 2.32 features
- Since this always runs in windowed mode, the gamma fix feature of legacy eqw 2.32 was dropped
  - Check your updated eqgame.dll for gamma customization support
- No hotkeys are supported (ReleaseMouse, ReloadIni, EQWSwitch)

## Direct3D8 support

Depending on your system, GPU, drivers, etc. you will most likely need a Direct3D 8 wrapper.
These are mods that work by placing a d3d8.dll file into the game directory. Don't install
these in the system directories. Try it without any d3d8.dll file first to see if that works
for you. You may get drastically better or worse performance with or without a D3D8 mod
and it's worth playing around with these even if seems to be working already.

## Testing

Initial testing was performed using either dgvoodoo or [d38to9](https://github.com/crosire/d3d8to9).

Only 32-bit video modes were tested.

## Known issues

### Features / polishing
- Transition glitches
   - Some dirty screens are briefly flashed to/from char select

### HW compatibility (comments will be system dependent)
 - d3d8to9 d3d8.dll:
   - NVidia fps limiter is not functional (Zeal version works fine)
   - Loading screen progress bar and text are not updating
   - The presence of the dgvoodoo ddraw.dll results in a black game screen
 - intel gpu w/out d3d8.dll:
   - black screen (but game is running in background with sounds and ui)
 - dgvoodoo:
   - Crashes char select -> login w/out ever entering world (dx 6.0 error dialog, same as old eqw)
   - It is failing a DirectDrawCreate() inside an early eqmain quality check (dx 6.0 error)
   - Without dgvoodoo ddraw.dll it hangs with a black screen trying to go back into eqmain

