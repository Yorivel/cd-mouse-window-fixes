# Borderless Lock (Crimson Desert)

A small ASI mod for Crimson Desert in borderless window mode. Private, in testing.

- **No accidental resize:** the game's borderless window reports an invisible 8 px resize border, and the cursor starts
  on its top-left corner, so a click and drag there shrank the game into windowed mode. The mod reports that border as
  game area while the window covers its monitor. Windowed mode is untouched.
- **No Windows arrow:** the game never answers Windows' cursor request, so clicks and the screen edges showed the plain
  Windows arrow. The mod answers with the game's own cursor.
- **Camera mode back at once:** after Alt+Tab or a load, the game left the mouse driving a cursor instead of the camera
  until Kliff moved. The mod puts it back the way the game does when Kliff moves, using the game's own mouse code
  (found by its bytes, so a patch that changes it switches only this part off). Menus keep their cursor.

Build: Visual Studio 2022, then `build.cmd` (CMake fetches MinHook 1.3.4). Output: `build\BorderlessLock.asi`, loaded
by an ASI loader or DMM. Log: `BorderlessLock.log` next to the ASI.
