# Changelog

Each entry says what changed and, for behaviour fixes, the game or test that showed it,
so a later change doesn't undo it without knowing why.

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
- **A new primary surface can replace one a display-mode change made lost**, as in
  `ddraw.dll`. (Jazz Jackrabbit 2 sets its mode again without releasing the old primary:
  crash.)
- **A second DirectDraw object takes exclusive mode over from the first** instead of
  getting `DDERR_EXCLUSIVEMODEALREADYSET`, as in `ddraw.dll` and Wine; the old object drops
  to normal mode and hands over the saved window state. (Type Wild recreates its
  DirectDraw object without releasing the old one: hang.)
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
