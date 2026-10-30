# Changelog

Each entry says what changed and, for behaviour fixes, the game or test that showed it,
so a later change doesn't undo it without knowing why.

## 0.8.1
- **The taskbar no longer shows over Unreal engine games in fullscreen** (Nerf
  ArenaBlast). After restyling its window, the game sized it back to its own idea of
  fullscreen every time dd2d9 covered the screen again, until dd2d9 gave up with part of
  the screen uncovered. Those re-covers are now hidden from the game (its window procedure
  doesn't see the move). The game's own moves and sizes still stand, as in `ddraw.dll`
  (Wine's test_window_position).

## 0.8.0
**Direct3D 7 in dd2d9 itself**, no D7VK needed: `IDirect3D7`, `IDirect3DDevice7` and
`IDirect3DVertexBuffer7` (`src/d3d7.cpp`), matched to XP SP3's `d3dim700.dll`
(`re/spec/d3d7-*.md`), drawing through the Direct3D 9 device dd2d9 presents with
(`src/gpures.cpp`), so on Windows' own Direct3D 9 and on DXVK alike.
- EnumDevices lists RGB, HAL and T&L HAL with XP's names; the hardware devices' caps come
  from the Direct3D 9 adapter, RGB's are XP's fixed table. CreateDevice checks the render
  target and its attached Z buffer as XP does, and sets the x87 FPU to single precision
  unless DDSCL_FPUPRESERVE.
- Render states, texture stages (and the sampler states D3D9 moved them to, with
  MAGFILTER/MIPFILTER's changed values), transforms, lights, material, viewport, clip planes,
  clip status, state blocks (XP's own, handles from 1), Clear, scenes, every Draw*
  call, Load, ComputeSphereVisibility, and ProcessVertices on the CPU (transform, clip
  codes, lighting, vertex fog, vertex blending).
- Textures, render targets and Z buffers get a GPU copy (P8 and colour-keyed textures
  converted, the key becoming alpha while COLORKEYENABLE is on); what Direct3D drew is read
  back whenever DirectDraw touches the pixels, so 2D over a 3D frame works.
- Quirks for D3D9: D3DFVF_RESERVED1 (D3D9's PSIZE) is skipped; transformed vertices with
  ZENABLE off have their z clamped (D3D9 always clips it); rhw 0 on every vertex becomes 1
  (DXVK); lit vertices without a normal get a zero one.
- Deliberate differences: `ValidateDevice` reports one pass (XP returns E_NOTIMPL without
  a driver callback); render targets can't be palettized (D3D9 can't render to P8).
- DirectDraw objects made by DirectDrawCreateEx report 3D support (DDCAPS_3D and
  friends); Direct3D 1-6 aren't implemented yet, so DirectDrawCreate objects don't.
- DirectDraw side: Z buffers with an 8-bit stencil (24+8) can be created; DirectDraw 7
  managed textures take the system-memory textures' lock fast path; CreateSurface with an
  unwritable out pointer returns DDERR_INVALIDPARAMS (XP faults inside its handler).

## 0.7.19
- **In the background after Alt+Tab, the primary surface (and the other surfaces lost by
  the focus loss alone) can still be locked**, though it reports lost as in `ddraw.dll`
  (a deliberate difference: dd2d9 keeps the pixels). Captain Claw locks its primary while
  in the background (taking a picture of the screen for its pause menu), ignores the
  failure and writes through the null pointer: it crashed when Alt+Tabbed during play.
- Groundwork for Direct3D, not used yet: GPU copies of render targets, Z buffers and
  textures (`src/gpures.cpp`), read back before DirectDraw touches the pixels.

## 0.7.18
- **Back from Alt+Tab, the game's surfaces other than the primary and its back buffers
  can still be drawn with** (Blt, BltFast, Lock, GetDC), though they report lost as in
  `ddraw.dll` (IsLost, Restore): dd2d9 keeps every surface's pixels, where `ddraw.dll`
  frees their video memory. Captain Claw never restored some of its sprites after Alt+Tab:
  its text was gone, and the failing blits slowed it to 2 fps. After a display mode
  change nothing is drawable until restored, as before.
- **The Direct3D 9 device's hidden window has a thread of its own** that only handles its
  messages. 0.7.17 made it on the presenter thread, which can be busy or waiting on one
  of dd2d9's locks while Windows sends every window the news of a display mode change and
  waits for the answers: the likely cause of Nerf Arena Blast freezing behind D7VK with
  `display=switch` (fine in 0.7.16).
- **The device goes when a wrapper (D7VK) takes the presentation over**, as the
  presenter's own device did before 0.7.17, rather than stay beside the wrapper's.

## 0.7.17
Groundwork for Direct3D (dd2d9 implementing Direct3D 3-7 itself on Direct3D 9; design in
`re/d3d-design.md`):
- **One Direct3D 9 device per DirectDraw object** (`src/gpu.cpp`), created once on a hidden
  window of its own and presenting into the game's window through an additional swap
  chain. A window resize recreates only the swap chain, never resets the device, so the
  GPU resources Direct3D will keep there (render targets, textures) survive it. A lost
  device is reset once Windows gives it back.
- **`present=exclusive` is gone** (a Direct3D 9 exclusive-fullscreen device can't share
  the window with the swap chain). The key is still accepted, and ignored.

## 0.7.16
More of XP SP3's `ddraw.dll` (new specs: `re/spec/legacy-interfaces.md`,
`gamma-colorcontrol.md`, `window-procedure.md`):
- **The old interfaces:** IDirectDrawSurface2/3's GetAttachedSurface and
  EnumAttachedSurfaces hand out IDirectDrawSurface (v1) pointers; an attachment counts on
  the pointer the game passed, whatever interface it was called through (attach through
  one version, detach through another); EnumAttachedSurfaces (v1-3) and EnumSurfaces
  (IDirectDraw/2) describe a Z buffer by its pixel format, as `ddraw.dll`'s cut-down
  structure does; EnumSurfaces(CANBECREATED) through IDirectDraw2 tries IDirectDraw2's
  creation rules; GetAttachedSurface checks for a lost surface first and leaves the out
  pointer alone on failure; IDirectDraw's cooperative-level behaviour is kept even when
  the window can't be taken.
- **The window procedure:** the monitor may power down in exclusive mode (only the
  screensaver is blocked); a game that asked for DDSCL_NOWINDOWCHANGES isn't minimized or
  made non-topmost on focus loss; the cursor is kept on the screen after another
  program's mode change; no "Not Responding" ghost window replaces a busy game's; the hook
  goes with the window (WM_NCDESTROY).
- **The cursor imports are left alone with a real mode switch**: they're pointed at
  dd2d9's only while a mode is shown scaled or in a window.
- Gamma: no change needed (XP's DDERR_UNSUPPORTED when Windows has no ramp is replaced
  by dd2d9's own ramp, deliberately).

## 0.7.15
- `log=2` traces the reference counts of the primary surface's flip chain and of the
  DirectDraw objects (AddRef/Release), and each GetAttachedSurface, EnumAttachedSurfaces
  and surface QueryInterface call, to settle how Jazz Jackrabbit 2 and Type Wild get
  past `ddraw.dll`'s refusals (a second primary, a second exclusive object) on XP, and so
  replace those two workarounds with what XP really does.

## 0.7.14
- **3D games behind D7VK: use `display=switch`** to keep 4:3 modes 4:3. D7VK presents
  through its own swap chain, which fills the game's window, so dd2d9's `stretch` and
  `filter` don't apply to its frames (only to what dd2d9 presents: 2D games and screens).
  Fitting the window to the image behind the game's back worked, but needed black
  windows beside it, hiding the moves from the game (Unreal sizes its window back) and
  the taskbar: too much working around the wrapper, so it was left out.
- **A desktop display change no longer loses the surfaces of a game whose mode dd2d9
  emulates** (`display=borderless` or `window`, or a mode the monitor lacks). Nerf
  ArenaBlast quit with "Failed blt" while loading a level: a `WM_DISPLAYCHANGE` that left
  the desktop at the same resolution (a laptop changing its refresh rate on its own) lost
  every surface, and Unreal treats a failed texture upload as fatal. The game's mode
  isn't the desktop's there, and dd2d9's surfaces need no video memory freed for it. A
  real mode switch (`display=switch`) still loses them on a change, as `ddraw.dll` does.
- **The log names C++ exceptions too**, with the modules on the stack, as it does crashes.
  Unreal engine games (Nerf ArenaBlast) throw one for a "critical error" and then show
  their own error box, which left nothing in the log.
- A posted window check that finds the display busy (another thread setting a device up)
  is retried instead of dropped.

## 0.7.13
- **`filter=sharp`** (also `bilinear-sharp`): the image is scaled up by the largest whole
  multiple with sharp pixels, then smoothly the rest of the way. Pixels stay crisp and
  even, where `point` makes some rows and columns wider than others at non-integer
  scales, and `linear` blurs everything.
- **Other programs can't take the focus from an exclusive-mode game** (Captain Claw lost
  it a few seconds after starting, to a window of another program; the game then wrote
  through its failed Lock of the lost primary and crashed). While the game is in front,
  other programs can't bring their windows to the front on their own
  (`LockSetForegroundWindow`); Alt+Tab and clicks still can. When the game does lose the
  focus, the log names the program that took it.

## 0.7.12
- **The mouse in scaled and windowed display modes** (`display=borderless` or `window`, or
  a mode the monitor lacks): the game sees the cursor in its own mode's coordinates, its
  window as its screen at 0,0, as after a real mode switch. dd2d9 points the game's
  imports of the cursor functions at its own (`GetCursorPos`, `SetCursorPos`,
  `ClipCursor`, `GetClipCursor`, `GetMessagePos`, `GetSystemMetrics` for the screen size,
  `GetClientRect`/`GetWindowRect`/`ScreenToClient`/`ClientToScreen`/`MapWindowPoints`
  for its window) and translates its window's mouse messages. Clicks used to land where
  the unscaled coordinates pointed. `mouse=raw` turns it off.

## 0.7.11
More of XP SP3's `ddraw.dll`:
- **CreateSurface** checks descriptions as `ddraw.dll` does, in its order: its caps rules
  (e.g. PALETTE or VISIBLE asked for, invalid combinations), Z buffers (by depth through
  IDirectDraw/2, by pixel format through 4/7), no size or format for a primary, power-of-two
  mipmaps with no more levels than the size allows (they used to be cut down), color keys
  (no overlay keys or ranges), client memory (checked first; IDirectDraw4 takes an
  unaligned pointer), cube maps through IDirectDraw7 only; CLASS_E_NOAGGREGATION comes
  later through IDirectDraw/2.
- **EnumSurfaces** validates its flags and filter as `ddraw.dll` does and matches only the
  fields it compares (caps exactly, the size, keys, counts, and a pixel format only for
  surfaces created with one), newest surface first; CANBECREATED ignores the callback's answer.
- **SetCooperativeLevel**: exclusive mode stays on its window (another window gets
  `DDERR_HWNDALREADYSET` until the game goes back to normal); entering it loses every
  DirectDraw object's surfaces; `DDSCL_SETDEVICEWINDOW`'s checks; a window with a dialog
  frame but no border isn't changed.
- A DirectDraw object made by CoCreateInstance answers `DDERR_NOTINITIALIZED` until
  Initialize. A released palette's AddRef/Release return 0 and SetPalette refuses it.

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
