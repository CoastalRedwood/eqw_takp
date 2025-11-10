# Windowed-mode support for the TAKP client.
  - Improves mouse behavior when switching windows focus
  - Eliminates phantom input glitches when switching focus
  - Automatically goes borderless if the ini resolution is set
    to match the monitor
  - Supports in-game toggling of stretched full screen mode
  - Adds ini options settings for:
    - High frequency cpu timebase fix
    - Swapping left / mouse buttons
    - Stretched full screen mode
  - Supports in-game setting of video modes (32-bit resolutions)
    and stores the window position offsets

---
The eqgame.dll is a dummy placeholder for testing.

Initial testing performed using either dgvoodoo or [d38to9](https://github.com/crosire/d3d8to9).
See issues list below.

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
              x and y offsets for safe starting position and recovery.

 ### Existing ini fields
 - The `[VideoMode]: RefreshRate` value is ignored
 - The `[VideoMode]: BitsPerPixel` value should be set to `32` (others are untested)


## Known issues
 
### Stability and compatibility
 - d3d8to9 d3d8.dll:
   - NVidia fps limiter is not functional (Zeal version works fine)
   - Loading screen progress bar and text are not updating
   - The presence of the dgvoodoo ddraw.dll results in a black game screen
 - intel gpu w/out d3d8.dll: black screen (but game is running in background with sounds and ui)
 - dgvoodoo:
   - Crashes char select -> login w/out ever entering world (dx 6.0 error dialog, same as old eqw)
   - It is failing a DirectDrawCreate() inside an early eqmain quality check (dx 6.0 error)
   - Without dgvoodoo ddraw.dll it hangs with a black screen trying to go back into eqmain

### Features / polishing
- Since this always runs in windowed mode, the gamma fix feature of legacy eqw 2.32 was dropped
- Transition glitches
   - Some dirty screens are briefly flashed to/from char select
 - Clean up debug logging

### OTHER:
 - Zeal breakage: eqw get_game_window() needs update, external map window is broken
