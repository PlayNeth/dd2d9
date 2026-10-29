# dd2d9

DirectDraw (DirectX 1–7) for 2D games, implemented on Direct3D 9. Behind D7VK it covers
the 2D half (menus, movies, 2D games) that D7VK leaves to the system's DirectDraw. See
`CHANGELOG.md` for what changed and why.

## How it works
- **Surfaces live in system memory**, laid out as ddraw.dll lays them out (the same
  pitch). `Lock` returns a direct pointer, and `GetDC` puts a GDI device context
  right on that memory (`D3DKMTCreateDCFromMemory`), so games that draw text with
  GDI work, on the game's own memory too.
- **All blitting is done in software**: color fill, raster ops (SRCCOPY,
  BLACKNESS, WHITENESS), stretch, left-right and up-down mirroring, source and
  destination color keys, overlapping copies, and conversion
  between 8/16/24/32-bit formats. Color-keyed copies (sprites) use SSE2.
- **Flip chains** swap memory between surfaces, as the hardware does. Games can also
  put them together with `AddAttachedSurface` (IDirectDrawSurface 1-4), as `ddraw.dll`
  allows.
- **A presenter thread owns the D3D9 device.** It converts the primary surface to
  X8R8G8B8 (applying the palette and the gamma ramp), scales it into the game
  window with `StretchRect` and presents. Only the changed part of the image is
  converted, and the conversion runs without blocking the game: it holds the
  DirectDraw lock only long enough to see what changed.
- **The game decides vsync by default.** `Flip` waits until its frame is
  presented unless the game passes `DDFLIP_NOVSYNC`, and a limiter caps it at
  the refresh rate when the driver doesn't block on vsync. `vsync=on` and
  `vsync=off` force it. `WaitForVerticalBlank`, `GetScanLine` and
  `GetVerticalBlankStatus` run on a virtual vblank timer.
- **Exclusive-mode games get a real display-mode switch by default**, as with
  the original DirectDraw. The color depth stays virtual (an 8-bit game runs on
  a 32-bit desktop). If the monitor lacks the requested mode, dd2d9 falls back to
  a borderless window with the image scaled. `display=window` and
  `display=borderless` avoid mode switches altogether.
- **Focus handling as in ddraw.dll.** In exclusive mode the game's window is
  subclassed and covers the screen. Alt+Tab or another loss of focus restores the
  desktop resolution (or drops the borderless cover) and minimizes the game, and
  coming back sets it up again. The screensaver and monitor power-off are blocked.
- **Lost surfaces as in ddraw.dll.** Display-mode changes, entering exclusive mode,
  losing focus in it, and leaving it after a focus loss (except with DirectDraw 1) make
  video-memory surfaces (and the primary) lost until the game calls `Restore` or
  `RestoreAllSurfaces`; `Lock` and `Flip` say `DDERR_SURFACELOST` meanwhile, as games
  expect. Unlike on real hardware, the contents survive. A display-change notice only
  counts if the display really left the game's mode (Windows sends late and misleading
  ones), a lost primary isn't shown, and a new primary may replace a lost one.
- **Several DirectDraw objects**: a second one taking exclusive mode takes it over
  from the first (games that recreate their DirectDraw object without releasing the
  old one), and only the exclusive object's primary is presented. A primary made by the
  exclusive object makes the others' lost.
- **Calls behave as in XP SP3's ddraw.dll**: the checks, their order and the return
  codes follow its decompiled code (`re/spec/`), from `Blt`'s rectangle and flag rules to
  attachments, palettes and clippers. Without a clipper, `Blt` rejects destination
  rectangles that extend past the surface; with one, it clips to the clipper's
  window or clip list. `BltFast` never clips (with a clipper attached it fails
  with `DDERR_BLTFASTCANTCLIP`).
- **The mouse follows the game's display mode** when it is shown scaled or in a window
  (`display=borderless` or `window`, or a mode the monitor lacks): the game's imports of
  the cursor functions (`GetCursorPos`, `SetCursorPos`, `ClipCursor`, `GetMessagePos`,
  `GetSystemMetrics`' screen size, the window's rectangles and coordinate conversions) and
  its window's mouse messages are translated, so the game sees its mode's coordinates
  with its window as the screen, as after a real mode switch. Graphics and input DLLs in
  the game folder (D7VK, DXVK...) keep the real ones. `mouse=raw` turns it off.
- **Windowed games** (`DDSCL_NORMAL`) get a desktop-sized primary surface, and
  the part under their window is shown.
- **All interface versions are implemented**: IDirectDraw 1/2/4/7,
  IDirectDrawSurface 1/2/3/4/7, Palette, Clipper and GammaControl. Their method tables
  are writable, as `ddraw.dll`'s, for overlays and patches that hook them.
- **Presenting through Windows' own `d3d9.dll`** (`d3d9=system`), even when the game
  folder holds DXVK's for D7VK: Vulkan presentation in a window covering the screen can
  let the driver switch the display back to the desktop mode. `d3d9=game` uses the
  game's.

## Direct3D and D7VK
dd2d9 doesn't render 3D. For 3D games, install it behind D7VK (D7VK as `ddraw.dll`, dd2d9
as `ddraw_.dll`, which `install-dd2d9.sh --d7vk` does). D7VK builds its Direct3D 3–7 on
the DirectDraw it wraps, so in that setup dd2d9 provides the Direct3D objects D7VK needs
(devices, viewports, materials, textures, with `Clear` and `Load` applied to the
DirectDraw surfaces). D7VK renders and presents while a 3D device exists, and dd2d9
presents 2D the rest of the time (menus, movies, 2D-only games). Loaded on its own,
dd2d9 answers Direct3D queries with `E_NOINTERFACE`, so games fall back to their other
renderers. The `d3d=` ini key overrides this.

D7VK presents its 3D frames itself, through its own swap chain filling the game's
window, so `stretch` and `filter` don't apply to them: with `display=borderless`, a 4:3
mode is stretched over a wide screen. For 3D games keep `display=switch` (the default):
the display really changes mode, and the graphics driver's scaling setting (keep aspect
ratio, or centered) decides how it fills the screen.

## Differences from ddraw.dll on purpose
- **The window's frame sits just off the screen in fullscreen.** `ddraw.dll` leaves the
  window's style alone and draws over the whole screen, frame included. dd2d9 keeps the
  style too (games that read it see the same value), but presents into the client area, so
  it makes the window larger than the screen until the client area covers it exactly, the
  frame just outside (and clipped, so it can't show on a neighbouring monitor). Only the
  window's outer rectangle differs from Windows; leaving exclusive mode shrinks it back to
  the screen, as `ddraw.dll` leaves it.
- **8-bit to RGB blits use the palette** (`ddraw.dll` copies the index as grey).
- **`display=window` doesn't lose surfaces on focus loss**: the game keeps running.
- **Surfaces of a complex surface that outlive it stay usable** (Jazz Jackrabbit 2
  keeps drawing to its old back buffer).
- **No reference leak** when a DirectDraw 1 flip chain shrinks (`ddraw.dll` leaks one).
- **A second DirectDraw object takes exclusive mode over** from the first (as in Wine;
  `ddraw.dll` refuses it with `DDERR_EXCLUSIVEMODEALREADYSET` while the first is active).
- **A primary surface a mode change made lost can be replaced** by a new one
  (`ddraw.dll` refuses any second primary).
  These two only matter behind D7VK, which keeps references the games released (Type
  Wild's DirectDraw object, Jazz Jackrabbit 2's old front buffer), so on XP they're gone
  and behind D7VK they aren't. They go once dd2d9 does Direct3D itself.
- **`DDPCAPS_PRIMARYSURFACE` is accepted in CreatePalette** (`ddraw.dll` refuses it), for
  games made on Windows 9x.

## Not implemented (yet)
- **Overlays.** Overlay surfaces are refused with `DDERR_NOOVERLAYHW`
  (the caps say there are none), so games fall back to blitting. `BltBatch` checks its
  arguments and fails with `DDERR_UNSUPPORTED`, as in `ddraw.dll`.
- **Blits that convert RGB to an 8-bit surface** write index 0.
- **FourCC surfaces** are limited to DXT1–5, YUY2 and UYVY. They can be locked
  but not blitted. Other FourCC codes are refused.
- **`DDSCL_CREATEDEVICEWINDOW`**: the focus window serves as the device window.

## Configuration
See `dd2d9.ini.example`. dd2d9 reads these files in order, later ones winning:

1. `C:\windows\dd2d9.ini`. In a Wine prefix that's `drive_c/windows/dd2d9.ini`;
   `install-dd2d9.sh` creates it.
2. `dd2d9.ini` next to `ddraw.dll`.
3. `dd2d9.ini` next to the game's executable.
4. The file named by the `DD2D9_INI` environment variable.

Keys outside any section, in `[dd2d9]` or in `[default]` apply to every game. A
`[game.exe]` section (case-insensitive) applies to that executable only.

| Key | Values (default first) |
| --- | --- |
| `display` | `switch`, `window`, `borderless` |
| `present` | `window`, `exclusive` (D3D9 exclusive fullscreen after a real mode switch) |
| `stretch` | `aspect`, `fill`, `integer` |
| `filter` | `point`, `linear`, `sharp` (bilinear-sharp: whole-multiple point scaling, then linear) |
| `vsync` | `auto` (the game decides), `on`, `off` |
| `d3d` | `auto` (Direct3D objects for a wrapper when loaded as `ddraw_.dll`), `on`, `off` |
| `d3d9` | `system` (Windows' own `d3d9.dll` for presenting), `game` (the one the game loads) |
| `log` | `0`, `1` (info), `2` (trace every blit, lock, flip and palette change, the game window's messages and every failed call) |
| `logfile` | path; default `dd2d9.log` next to the game's executable |

`DD2D9_LOG=0|1|2` overrides `log`. Any other value of `DD2D9_LOG` is taken as a
log file path, for example `DD2D9_LOG='Z:\tmp\dd2d9.log'` under Wine. If the
log location can't be written to, dd2d9 tries the current directory and then
`%TEMP%`. The log header lists the config files that were read, every unknown
key, and the settings in effect.

On load, dd2d9 sends a debug message saying where it was loaded from and which
config file or log it's using. Under Wine 9, `WINEDEBUG=warn+seh` prints it;
on Windows, use DebugView.

## Building
Requires mingw-w64 (i686) and meson. On Arch: `pacman -S mingw-w64-gcc meson`.

    meson setup build --cross-file build-win32.txt
    ninja -C build

The output is `build/ddraw.dll`, which is statically linked with no MinGW runtime
dependencies.

## Using
Copy `ddraw.dll` next to the game's executable, and under Wine also set
`WINEDLLOVERRIDES="ddraw=n"`. Behind D7VK, D7VK is `ddraw.dll` and dd2d9 is
`ddraw_.dll` in the same folder. `install-dd2d9.sh` installs it into a Wine prefix,
either on its own or behind D7VK. Under Wine, `d3d9=system` presents through the prefix's
`d3d9.dll` (wined3d, or DXVK if it's installed into the prefix); `d3d9=game` also takes
one next to the game.

## Tests
`build/ddtest.exe` writes its results to `ddtest.txt`:
- `ddtest unit` runs pixel-exact checks of blits, keys, conversion, GDI, interface
  versions and palettes.
- `ddtest fs8` runs a 640×480×8 exclusive-mode flip-chain animation and checks
  the pacing.
- `ddtest win` runs a windowed primary with a clipper.
- `ddtest fuzz` runs thousands of random blits (8/16/32-bit, color keys and key
  ranges, mirroring, stretching, overlapping copies) against a reference implementation.

All checks pass under Wine 11 (wined3d, Xvfb).

`tests/wine/` runs Wine's own ddraw tests (expectations confirmed on Windows) against a
build and compares the result with a baseline; see `tests/wine/README.md`.

`build/ddbench.exe` measures performance and writes `ddbench.txt`: `ddbench blit` (blitter
throughput), `ddbench flip <w> <h> <bpp>` (a sprite game loop), `ddbench window` (small
updates to a windowed primary). With `log=1`, the presenter logs every 10 seconds how much of
the frame it converted and how long the game had to wait for it.

`build/reloadtest.exe [dll]` frees and reloads dd2d9 (default `ddraw_.dll`) while the game window
stays subclassed by a wrapper, as D7VK does when a game switches between Direct3D and software.

`build/d3d7test.exe [w h [bpp]]` renders a Direct3D 7 triangle with DirectDraw fills
and GDI text on top, then flips. It's meant to run behind D7VK and writes `d3d7test.txt`.

## Reverse-engineering notes
`re/` has the vtable map of the XP SP3 `ddraw.dll` (5.3.2600.5512), along with
naming scripts for Ghidra and IDA. `re/spec/` describes, from its decompiled code, what
each method checks and in which order, with the differences from dd2d9 that remain.
