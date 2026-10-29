# dd2d9

DirectDraw (DirectX 1–7) for 2D games, implemented on Direct3D 9. On Linux, DXVK
turns that into Vulkan, so dd2d9 covers the 2D half that D7VK leaves to wined3d.

## How it works
- **Surfaces live in system memory as GDI DIB sections.** `Lock` returns a direct
  pointer, and `GetDC` works natively, so games that draw text with GDI work.
- **All blitting is done in software**: color fill, raster ops (SRCCOPY,
  BLACKNESS, WHITENESS), stretch, left-right and up-down mirroring, source and
  destination color keys (including ranges), overlapping copies, and conversion
  between 8/16/24/32-bit formats. Color-keyed copies (sprites) use SSE2.
- **Flip chains** swap memory between surfaces, as the hardware does.
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
  subclassed. Alt+Tab or another loss of focus restores the desktop resolution
  (or drops the borderless cover) and minimizes the game, and coming back sets
  it up again. The screensaver and monitor power-off are blocked.
- **Blit rules match ddraw.dll.** Without a clipper, `Blt` rejects destination
  rectangles that extend past the surface; with one, it clips to the clipper's
  window or clip list. `BltFast` never clips, and it rejects a color key the
  surface doesn't have. These rules were checked against the decompiled XP SP3
  `ddraw.dll`.
- **Windowed games** (`DDSCL_NORMAL`) get a desktop-sized primary surface, and
  the part under their window is shown.
- **All interface versions are implemented**: IDirectDraw 1/2/4/7,
  IDirectDrawSurface 1/2/3/4/7, Palette, Clipper and GammaControl.

## Direct3D and D7VK
dd2d9 doesn't render 3D. For 3D games, install it behind D7VK (D7VK as `ddraw.dll`, dd2d9
as `ddraw_.dll`, which `install-dd2d9.sh --d7vk` does). D7VK builds its Direct3D 3–7 on
the DirectDraw it wraps, so in that setup dd2d9 provides the Direct3D objects D7VK needs
(devices, viewports, materials, textures, with `Clear` and `Load` applied to the
DirectDraw surfaces). D7VK renders and presents while a 3D device exists, and dd2d9
presents 2D the rest of the time (menus, movies, 2D-only games). Loaded on its own,
dd2d9 answers Direct3D queries with `E_NOINTERFACE`, so games fall back to their other
renderers. The `d3d=` ini key overrides this.

## Not implemented (yet)
- **Mouse coordinate scaling.** When the image is scaled (`display=borderless`,
  or the fallback when a mode switch fails), games that read the cursor with
  `GetCursorPos` or `WM_MOUSEMOVE` see unscaled coordinates.
- **Overlays, `BltBatch`, and `EnumSurfaces` with `CANBECREATED`.**
- **Blits that convert RGB to an 8-bit surface** write index 0.
- **FourCC surfaces** are limited to DXT1–5, YUY2 and UYVY. They can be locked
  but not blitted. Other FourCC codes are refused.
- **Surfaces are never lost**, so `IsLost` always succeeds. Games keep running
  while minimized.

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
| `filter` | `point`, `linear` |
| `vsync` | `auto` (the game decides), `on`, `off` |
| `log` | `0`, `1` (info), `2` (trace every blit, lock, flip and palette change) |
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
`WINEDLLOVERRIDES="ddraw=n"`. Alternatively, `install-dd2d9.sh` installs it
into a Wine prefix, either on its own or behind D7VK as `ddraw_.dll`. To get Vulkan on Linux,
install DXVK's `d3d9.dll` into the prefix, or put it next to the game.

## Tests
`build/ddtest.exe` writes its results to `ddtest.txt`:
- `ddtest unit` runs pixel-exact checks of blits, keys, conversion, GDI, interface
  versions and palettes.
- `ddtest fs8` runs a 640×480×8 exclusive-mode flip-chain animation and checks
  the pacing.
- `ddtest win` runs a windowed primary with a clipper.
- `ddtest fuzz` runs thousands of random blits (8/16/32-bit, color keys and key
  ranges, mirroring, stretching, overlapping copies) against a reference implementation.

All 76 checks pass under Wine 11 (wined3d, Xvfb).

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
naming scripts for Ghidra and IDA.
