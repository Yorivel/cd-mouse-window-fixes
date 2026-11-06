# Mouse & Window Fixes (Crimson Desert)

An ASI mod that fixes how Crimson Desert handles the mouse in borderless window mode.

- **No accidental resize.** The borderless game window has an invisible resize border, and the cursor starts right on
  its corner, so a click and drag there shrank the game into windowed mode. The mod turns that border off while the
  game covers the screen. Windowed mode is untouched.
- **No Windows arrow.** Clicks and the screen edges no longer flash the plain Windows arrow over the game.
- **The camera works straight away.** The game sometimes leaves the mouse in menu mode after you are back in the
  world: moving the mouse steers an invisible menu cursor instead of turning the camera, until Kliff takes a step.
  It happens when you load in (as soon as the HUD appears) and when you come back to the game from another window
  (Alt+Tab or a click). The mod hands the mouse back to the camera right away. Menus keep their cursor.
- **The title cursor.** No cursor over the intro; when the title prompts appear it starts on the logo's ring.
- **One notch, one zoom.** A single mouse-wheel notch zooms the camera (the game needed two in quick succession).

Every game address is found by searching the game's code when it starts, so game updates rarely break it. If one of
the searches fails after an update, only that fix switches off; the log says which.

## Install

Needs an ASI loader (DMM installs one). Put `MouseWindowFixes.asi` in the game's `bin64` folder, or add it to DMM.
The log is `MouseWindowFixes.log` next to it.

## Build

Visual Studio 2022, then `build.cmd` (CMake fetches MinHook 1.3.4). Output: `build\MouseWindowFixes.asi`.

`tools/bl_backtest.py <exe>...` runs the mod's code searches on game executables, to check a new game build before
playing it (each search must find exactly one place).

MIT licence (`LICENSE.txt`); MinHook keeps its own BSD 2-Clause licence (`THIRD-PARTY-MinHook-LICENSE.txt`).
