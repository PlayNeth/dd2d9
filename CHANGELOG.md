# Changelog

Each entry says what changed and, for behaviour fixes, the game or test that showed it,
so a later change doesn't undo it without knowing why.

## 0.7.10
- **In exclusive mode the image fills the screen whatever size the window is.** JJ2+
  sizes its window past the screen while starting up (661x544 at 640x480); the image was
  centered in the whole window, which put a black bar at the top and cut off the bottom
  until the next mode change. As with `ddraw.dll`, the screen shows the game now.

## 0.7.9
- **A game changing its window's frame in exclusive mode** gets its window fitted to the
  screen again (the frame just off it).
- `log=2` also traces style changes, with the window and client rectangles.

## 0.7.8
- **Leaving exclusive mode loses the surfaces when the desktop's display mode comes
  back**, as in `ddraw.dll` (the game's mode is gone). The old fullscreen primary is then
  no longer shown in the window, which still covered the screen for a moment until the
  game resized it: a black flash after Alt+Enter. (JJ2+ switching to windowed.)

## 0.7.7
Matched to Windows XP SP3's `ddraw.dll` call for call (from its decompiled code; the
specifications are in `re/spec/`), method by method for the surfaces, palettes, clippers,
the exports and much of the DirectDraw object.
- **Blt and BltFast report lost surfaces** (`DDERR_SURFACELOST`), so a game that blits
  to the primary restores it after a mode change or Alt+Tab instead of drawing to a lost
  one. They check in `ddraw.dll`'s order: locked surfaces (or a DC out) are busy, a
  `DDBLTFX` needs its size, raster operations other than SRCCOPY, BLACKNESS and
  WHITENESS fail with `DDERR_NORASTEROPHW` (they used to succeed and draw nothing; GetCaps
  now lists those three), rotation and DDROPS are refused, fills check their surfaces.
- **Flip chains put together with AddAttachedSurface** work through IDirectDrawSurface
  1-4 for any two surfaces of a kind, as in `ddraw.dll`; a surface attached to the front
  becomes the back buffer. AddAttachedSurface follows its size, one-per-kind and memory
  rules (a Z buffer on any surface, not only primaries and 3D devices);
  DeleteAttachedSurface(NULL) detaches everything; attachments are listed newest first.
- **GetSurfaceDesc** builds its flags as `ddraw.dll` does; after a mode change the primary
  reports the display's pixel format. SetSurfaceDesc, GetPixelFormat, the uniqueness
  value, private data (`DDSPD_VOLATILE`), priority/LOD (managed textures only) and the
  overlay methods return what `ddraw.dll` returns.
- **Gamma ramps** belong to the primary and show only in exclusive mode;
  `SetGammaRamp(NULL)` resets.
- **Palettes and clippers**: CreatePalette's flag rules, entry ranges and flags,
  GetCaps; a window clipper's GetClipList is its visible region on the screen,
  IsClipListChanged says TRUE, SetClipList/SetHWnd validate, clippers made through
  DirectDraw 4/7 reference it.
- **DirectDraw object**: RestoreAllSurfaces returns the first failure;
  TestCooperativeLevel out of exclusive mode reports another object's exclusive mode or a
  changed display mode; Set/RestoreDisplayMode refuse while a video-memory surface is
  locked; a primary can't be made while another object is exclusive, and the exclusive
  object's new primary makes the others' lost; leaving exclusive mode flips back to the
  GDI surface; the mouse stays on the game's monitor in exclusive mode; checks for
  WaitForVerticalBlank, GetScanLine, GetAvailableVidMem, Compact, GetCaps,
  GetDeviceIdentifier, EnumDisplayModes, DuplicateSurface and GetSurfaceFromDC.
- Lock fills the DirectX 7 fields only for a whole DDSURFACEDESC2.
- Looking for display changes made by others costs less per call (once per call, timed
  with the processor's counter).

## 0.7.6
- **The window keeps its style in fullscreen.** Instead of removing the frame, dd2d9 grows
  the window until its client area covers the monitor, the frame just off the screen (a
  window region keeps it off neighbouring monitors; `WM_GETMINMAXINFO` lets the window be
  that large). Games reading their style see Windows' value, and D7VK, which always draws
  into the client area, still fills the screen. Leaving exclusive mode shrinks the window
  back to the screen. (Wine `test_window_style`.)
- **A hidden window stays hidden** when exclusive mode starts while another window is in
  front, and isn't minimized on focus loss, as with `ddraw.dll`.
- **DirectDraw 1 leaving exclusive mode drops the window's topmost state** (the display mode
  stays, as before).

## 0.7.5
- **Leaving exclusive mode after a focus loss loses the surfaces**, as in `ddraw.dll`
  (not with DirectDraw 1, or when the game never went to the background).
  (Wine `test_lost_device`.)
- **Interface tables (vtables) are writable**, as `ddraw.dll`'s: overlays and fan
  patches hook DirectDraw by rewriting them, which crashed on read-only memory.
  (Wine `test_vtbl_protection`.)
- **Attaching a surface again through another interface version** returns
  `DDERR_CANNOTATTACHSURFACE` (the same version still gives `DDERR_SURFACEALREADYATTACHED`).
- **GetDC**: none for 32-bit surfaces with red in the low byte (A8B8G8R8, X8B8G8R8), as
  on Vista and later; A2R10G10B10 gets a D3DKMT DC; 32-bit fallback DIBs are `BI_RGB`.
- Tests: `tests/wine/` builds and runs Wine's ddraw tests one sub-test per process and
  compares runs with a baseline (`tests/wine/README.md`).

## 0.7.4
- **`d3d9=system` (default): the 2D picture goes through Windows' own `d3d9.dll`**, even
  when the game folder holds DXVK's (which D7VK needs). Presenting through DXVK's Vulkan in
  a window covering the screen let the driver take the display over and later switch it
  back to the desktop resolution, which dropped Jazz Jackrabbit 2 (JJ2+) out of fullscreen.
  `d3d9=game` restores the old behaviour. The log names the `d3d9.dll` in use.

## 0.7.3
- **A display-change notice only loses the surfaces if the display really left the
  game's mode.** Windows sends `WM_DISPLAYCHANGE` twice, the second time late, and between
  two game modes one notice may name the desktop's; JJ2+ took the resulting lost surfaces
  as a reason to go windowed. (JJ2+ switching 640×480 → 800×600.)

## 0.7.2
- `log=2` also traces the game window's focus, activation, size and display-change
  messages, and every DirectDraw call that fails, with its error.

## 0.7.1
- **A new primary surface can replace one a display-mode change made lost.** (Jazz
  Jackrabbit 2 sets its mode again without releasing the old primary: crash.) `ddraw.dll`
  itself refuses a second primary; this is a dd2d9 workaround.
- **A second DirectDraw object takes exclusive mode over from the first** instead of
  getting `DDERR_EXCLUSIVEMODEALREADYSET`, as in Wine (`ddraw.dll` refuses it while the
  first is active); the old object drops to normal mode and hands over the saved window
  state. (Type Wild recreates its DirectDraw object without releasing the old one: hang.)
- **Only one DirectDraw object presents**: one whose primary is lost, or while another
  object is exclusive, stays off the window. (Hydro Thunder around its videos, Nerf Arena
  Blast after Alt+Tab: two presenters on one window flickered.)
- `ddtest` covers both game patterns.

## 0.7.0
- Surfaces in plain memory with `ddraw.dll`'s pitch; `GetDC` through
  `D3DKMTCreateDCFromMemory`, with a DIB copy as fallback.
- Per-interface reference counts (surfaces and DirectDraw), as `ddraw.dll` counts them.
- Lost surfaces: display-mode changes, entering exclusive mode and losing focus in it;
  `IsLost`, `Restore`, `RestoreAllSurfaces`, `TestCooperativeLevel`.
- Exclusive mode: the window covers the screen and is activated, topmost while in front;
  focus loss restores the desktop mode and minimizes.
- DirectDraw 1 flip chains put together with `AddAttachedSurface`.
- Overlays and colour-key ranges refused, as on hardware without them.
- Many return codes and validation orders matched to Windows using Wine's tests:
  1643 → 237 failed checks, 262 → 364 clean sub-tests.

Earlier versions aren't recorded here.
