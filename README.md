# dd2d9

A drop-in `ddraw.dll` that runs **DirectDraw and Direct3D 3–7 games** on Direct3D 9, on modern Windows and on Linux.

dd2d9 is a reimplementation of Windows XP SP3's `ddraw.dll`: DirectDraw 1–7 and Direct3D 3–7, drawn through Direct3D 9. It aims to behave call for call like the original (its checks, return codes and their order follow XP's own code), with XP's bugs fixed and modern options added: scaling filters, antialiasing, anisotropic filtering, a frame limiter, and the answers old games expect about memory, the processor and the desktop's colour depth. It's a single file, `ddraw.dll`, with no runtime to install.

---

## Contents

- [Requirements](#requirements)
- [Installing](#installing)
- [Uninstalling](#uninstalling)
- [What it does](#what-it-does)
- [Settings](#settings)
- [Files dd2d9 uses](#files-dd2d9-uses)
- [Troubleshooting](#troubleshooting)
- [Building from source](#building-from-source)
- [For developers](#for-developers)
- [License](#license)

---

## Requirements

- A **32-bit game** that uses DirectDraw (DirectX 1–7), with or without Direct3D 3–7.
- **Windows Vista or later** with Direct3D 9, or **Linux** with Wine or Proton. (Not Windows XP: dd2d9 uses Vista's locks and timers.) Under Wine, dd2d9 presents through the prefix's Direct3D 9: Wine's own (wined3d), or DXVK if it's installed in the prefix.
- **For games that also use 3dfx Glide** (Need for Speed III, High Stakes, NFS4 and others of the Voodoo era): [gl2d9](https://github.com/PlayNeth/gl2d9) for the Glide part. Its README's [Using gl2d9 with dd2d9](https://github.com/PlayNeth/gl2d9#using-gl2d9-with-dd2d9) has the recommended settings for running both.

## Installing

### Windows

Copy `ddraw.dll` next to the game's exe. Don't replace the one in `System32` or `SysWOW64`.

### Linux (Wine / Proton)

Either install it into the prefix the game uses:

```sh
WINEPREFIX=~/Games/foo ./install-dd2d9.sh install ddraw.dll
```

That copies it into the prefix's 32-bit system folder (`syswow64` in a 64-bit prefix, `system32` in a 32-bit one), keeping Wine's own copy, sets the `ddraw=native,builtin` override, and creates `C:\windows\dd2d9.ini` from `dd2d9.ini.example` if there's none. After a Wine or Proton update, run it again: updating the prefix puts Wine's `ddraw.dll` back.

Or, for one game: put `ddraw.dll` next to the game's exe and run the game with `WINEDLLOVERRIDES="ddraw=n"`.

### Checking it works

Set `log=1` in `dd2d9.ini` and start the game. `dd2d9.log` appears next to the game's exe, with the version, the config files read and the settings in effect.

## Uninstalling

- **Windows:** delete `ddraw.dll` from the game's folder.
- **Wine / Proton:** `WINEPREFIX=~/Games/foo ./install-dd2d9.sh uninstall` puts Wine's `ddraw.dll` back and removes the override. It keeps `dd2d9.ini`.
- You can also delete `dd2d9.ini` and `dd2d9.log`.

---

## What it does

### DirectDraw

- **Every interface version:** IDirectDraw 1, 2, 4 and 7, IDirectDrawSurface 1, 2, 3, 4 and 7, palettes, clippers and gamma control. Their method tables are writable, as `ddraw.dll`'s, for overlays and patches that hook them.
- **Calls behave as in XP's `ddraw.dll`:** the checks, their order and the return codes follow its decompiled code, from `Blt`'s rectangle and flag rules to attachments, palettes and clippers.
- **Surfaces in system memory,** laid out as `ddraw.dll` lays them out. `Lock` gives a direct pointer, and `GetDC` puts a GDI device context right on that memory, so games that draw text with GDI work.
- **A software blitter for all 2D:** colour fills, raster ops, stretching, mirroring, source and destination colour keys, overlapping copies, and conversion between 8, 16, 24 and 32-bit formats (to 8 bits, the nearest palette entry). Colour-keyed copies (sprites) use SSE2.
- **Flip chains** swap memory between surfaces, as the hardware did, including chains games put together themselves.
- **Presenting:** a thread of its own owns the Direct3D 9 device. It converts only the changed part of the image (applying the palette and the gamma ramp), scales it into the game's window and presents it, without holding up the game. Frames go out through the device's own `Present`, so overlay and capture tools (RivaTuner, Afterburner) show and record them, 2D games included.
- **V-sync as the game asks:** `Flip` waits for its frame unless the game passes `DDFLIP_NOVSYNC`. `WaitForVerticalBlank`, `GetScanLine` and `GetVerticalBlankStatus` run on a virtual vblank timer.

### Display modes and windows

- **Real display mode switches** for exclusive-mode games, as the original did. The colour depth stays virtual: an 8-bit game runs on a 32-bit desktop. If the monitor lacks the mode, dd2d9 falls back to a borderless window with the image scaled. `display=window` and `display=borderless` avoid mode switches altogether.
- **Alt+Tab as in `ddraw.dll`:** losing focus restores the desktop and minimizes the game, and coming back sets it up again. A window that would be minimized with no taskbar button (owned, a tool window, or one the taskbar missed) stays reachable: its owner is minimized instead, or it gets a button while in the background. The screensaver and monitor power-off are blocked while the game is in front.
- **Lost surfaces as in `ddraw.dll`:** mode changes and focus loss make surfaces lost until the game restores them, as games expect. Unlike on real hardware, their contents survive.
- **Several DirectDraw objects:** a second one taking exclusive mode takes it over from the first, for games that recreate their DirectDraw object without releasing the old one. In normal mode, a window shows the object that last drew into it (a video renderer's movie over the game, as with DirectShow's Video Mixing Renderer 7).
- **The game sees its own display mode as the screen** when it's shown scaled or in a window, as after a real mode switch. Its cursor functions, every way of asking for the screen's size (`GetSystemMetrics`, the desktop window's rectangle, `GetMonitorInfo`, `EnumDisplaySettings`' current mode), its window's rectangles and coordinate conversions, and its window's mouse messages are translated, so it sees its mode's coordinates with its window as the screen, and the cursor never leaves that screen. A `ChangeDisplaySettings` to that mode does nothing. The window's size messages and GDI's screen size are the mode's too. Graphics and input DLLs in the game folder (DXVK...) keep the real values.
- **Movies played by Windows' video player (MCI) into the game's window** are scaled and placed with the game's image: the position the game gives in its mode's coordinates is translated, as the mouse's is, and so are the player's window control (MCIWnd), the picture placed in it and the command interface (`mciSendCommand`, the player's own child window included, also opened by command string and moved by the game). The area a game is asked to repaint (`WM_PAINT`) is in its mode's coordinates too.
- **Fullscreen goes to the primary monitor,** as with `ddraw.dll`: `display=switch` and `display=borderless` alike, wherever Windows put the game's window. `display=window` stays where the window is.
- **Windowed games** get a desktop-sized primary surface, and the part under their window is shown.
- **Overlays:** overlay surfaces (RGB, YUY2, UYVY or YV12) are laid over the primary as it's presented, as the hardware showed them without touching its pixels: stretched or shrunk, mirrored, behind a destination colour key, with a source key, in their z-order. `UpdateOverlay`, the position and z-order methods and overlay flip chains follow `ddraw.dll`'s checks. A windowed program paints the destination key into its window with GDI, so dd2d9 also looks for the key in what that window draws; in a window, only the part of an overlay over the program's own window shows. `videooverlay=off` reports no overlay hardware.

### Direct3D

- **Direct3D 7** (`IDirect3D7`, `IDirect3DDevice7`, vertex buffers), matched to XP's `d3dim700.dll`, drawn through the same Direct3D 9 device dd2d9 presents with. It works on Windows' Direct3D 9 and on DXVK alike.
- **Direct3D 3–6** (devices, viewports, lights, materials, textures, execute buffers and vertex buffers), matched to XP's `d3dim.dll`. As on XP, every vertex goes through a CPU front end (transformed, lit with the DirectX 5 or 6 light model, fogged and clipped), then Direct3D 9 draws it. The old render states are mapped onto texture stages as Microsoft's own rasterizer mapped them.
- **2D over 3D works:** textures, render targets and Z buffers get a GPU copy, and what Direct3D drew is read back before DirectDraw touches the pixels, so HUDs and menus drawn over a 3D frame show.
- **Z buffers can be locked and blitted** by DirectDraw like any surface, where the GPU samples `INTZ` depth textures (nearly all do, DXVK too), antialiasing is off and `zaccess` isn't `off`. Each lock waits for the GPU and copies the buffer back; a lock of at most 64×64 pixels (a lens flare's occlusion test) copies only its rectangle.
- **Cube maps** become Direct3D 9 cube textures, their faces drawn into read back as other render targets are.
- **Bump maps** (`DDPF_BUMPDUDV`: U8V8, U5V5L6, U8V8L8) are listed and drawn where the GPU has environment-mapped bump mapping.
- **Textures Direct3D drew into** are copied on the GPU when used, mipmapped or colour-keyed ones too (the key made transparent by a pixel shader); palettized ones go through the CPU.
- **Clip status and picking:** Direct3D 7's `GetClipStatus` follows what's drawn (the clip codes of untransformed vertices, with vertex blending and the user clip planes, and their extents with `D3DRENDERSTATE_EXTENTS`). Direct3D 3–6's `Pick` finds the triangles under the point with their depth (the visible part of one the clip volume cuts), `D3DOP_SPAN` is drawn (as points, as XP's driver path did), and Ramp devices light in shades of the material's colour, as the monochromatic Ramp model did.
- **What Direct3D 9 lacks is emulated:** 8-bit palettized textures, and the software devices rendering to 8-bit surfaces as XP's RGB and Ramp rasterizers did.
- **The gamma ramp** applies to 3D frames too.

### Options beyond the original

- **Scaling:** keep the aspect ratio, fill the screen or scale by whole numbers, with point, linear or sharp-bilinear filtering.
- **3D quality:** antialiasing, anisotropic filtering, mipmaps for textures that have none, linear filtering of point-filtered textures, 32-bit rendering of 16-bit targets (no banding) and 24-bit Z buffers.
- **Old-hardware fixes:** emulated W buffering (no z-fighting of light maps on roads in NFS III), and light maps and decals pulled forward as 16-bit depth buffers rounded them.
- **A frame limiter** for games that run too fast or use a whole processor core.
- **What the game is told about the machine,** as a PC of its era would answer:
  - 1 GB of memory by default (`ram`): Windows reports 2 GB and a 4 GB page file, and old games overflow adding them up and claim there isn't enough;
  - one processor (`cpus`), and a 2 GHz clock in the registry (`cpu`);
  - a 16 or 24-bit desktop (`desktopbpp`) for games that refuse a 32-bit one, through DirectDraw, GDI and the display settings alike.
- **Windows 9x behaviour games relied on,** always on: memory from the process heap comes zeroed (games that read it before writing, such as Daytona USA Deluxe).

### Differences from `ddraw.dll` on purpose

- **Fullscreen windows:** dd2d9 keeps the window's style, as `ddraw.dll` does, but presents into the client area, so it makes the window slightly larger than the screen with its frame just off it.
- **8-bit to RGB blits use the palette** (`ddraw.dll` copies the index as grey).
- **`display=window` doesn't lose surfaces on focus loss:** the game keeps running.
- **`display=borderless` (and `window`) don't lose surfaces across Alt+Tab:** they report lost in the background, but back in front nothing is lost, as the display never left the game's mode (`display=switch` still loses them, as `ddraw.dll` did). Need for Speed: High Stakes never restores its flip chain and quit at the first failed Flip.
- **Surface memory has slack around it:** games that clip sprites loosely write a few rows past the bottom or above the top of their buffers, which XP's video memory and heap took without a crash.
- **Back from Alt+Tab, surfaces outside the primary's flip chain can still be drawn with.** They report lost as in `ddraw.dll`, but keep their pixels, so games that don't restore all of theirs keep working (Captain Claw). In the background, lost surfaces can still be locked, for the same game.
- **Surfaces of a complex surface that outlive it stay usable** (Jazz Jackrabbit 2 keeps drawing to its old back buffer).
- **No reference leak** when a DirectDraw 1 flip chain shrinks (`ddraw.dll` leaks one).
- **A second DirectDraw object takes exclusive mode over** from a first one that has lost activation, or whose window is gone or hidden.
- **Direct3D 3–6 checks what `d3dim.dll` used blindly:** unknown handles, a draw with no viewport, bad vertex types, execute-buffer offsets past the buffer and NULL pointers give errors instead of crashes.
- **`DDPCAPS_PRIMARYSURFACE` is accepted in CreatePalette**, for games made on Windows 9x.

### Not implemented (yet)

- **`BltBatch`** checks its arguments and fails with `DDERR_UNSUPPORTED`, as in `ddraw.dll`.
- **FourCC surfaces** are limited to DXT1–5, YUY2, UYVY and YV12 (what `GetFourCCCodes` lists), plus I420/IYUV. They blit to and from any surface through a decoding to 32-bit colour, encoded again when written (YUV as BT.601 video levels); their colour keys are compared as the colour they decode to. YV12 and I420 can't be colour-filled or used as textures.
- **Overlays** have no colour-key ranges, alpha, bob or interleaving, and no video ports.
- **`DDSCL_CREATEDEVICEWINDOW`**'s `DirectDrawDeviceWnd` window covers the screen in exclusive mode and shows the picture, as `ddraw.dll`'s; mouse messages over it go on to the focus window (`ddraw.dll` left them with the device window).
- **Direct3D:**
  - palettized textures Direct3D drew into still go through the CPU;
  - without `INTZ` support, with antialiasing on or with `zaccess=off`, Z buffers can only be cleared by DirectDraw (`DDBLT_DEPTHFILL`), not read or copied.

---

## Settings

dd2d9 reads these files in order, later ones winning:

1. `C:\windows\dd2d9.ini` (in a Wine prefix, `drive_c/windows/dd2d9.ini`; `install-dd2d9.sh` creates it);
2. `dd2d9.ini` next to `ddraw.dll`;
3. `dd2d9.ini` next to the game's exe;
4. the file named by the `DD2D9_INI` environment variable.

Keys outside any section, in `[dd2d9]` or in `[default]` apply to every game. A `[game.exe]` section applies to that exe only. [`dd2d9.ini.example`](dd2d9.ini.example) explains every key.

| Setting | Values (default first) |
| --- | --- |
| `display` | `switch` (real mode switches), `window`, `borderless` |
| `stretch` | `aspect`, `fill`, `integer` |
| `filter` | `point`, `linear`, `sharp` (sharp bilinear: flat pixels, blended only over one screen pixel at their edges) |
| `vsync` | `auto` (the game decides), `on`, `off` |
| `d3d` | `auto`: dd2d9's own Direct3D 3–7; `off`: no Direct3D (2D only) |
| `d3d9` | `system` (Windows' or the prefix's `d3d9.dll`, even when the game folder has DXVK's), `game` (the one the game loads) |
| `fpslimit` | `0` (off); frames per second at most |
| `antialiasing` | `0` (off); `2`, `4` or `8` samples |
| `antialias2d` | `off`: with `antialiasing`, 2D drawn through Direct3D (HUDs, menus: screen-aligned, the depth test off or always passing, no depth written) stays pixel-sharp; `on`: antialiased like the 3D |
| `anisotropy` | `0` (off); `2`, `4`, `8` or `16`, on textures the game filters smoothly |
| `mipmaps` | `off`; `on`: the GPU makes smaller levels for textures the game gave none |
| `texfilter` | `game`; `linear`: point-filtered textures drawn bilinear (cut-outs keep the game's filtering) |
| `render32` | `off`; `on`: 16-bit render targets drawn in 32 bits (no banding in blending and fog) |
| `depth24` | `off`; `on`: 16-bit Z buffers made 24-bit (less z-fighting in the distance) |
| `wbuffer` | `on`: W buffering offered and emulated; `force`: also for transformed vertices; `off` |
| `overlaybias` | `on`: light maps and decals pulled forward one 16-bit depth step; `off`; a number for more steps |
| `alternatepixelcenter` | `off`; `on`: Direct3D drawn half a pixel up and left, as some old drivers offered |
| `ram` | `1024`: MB of memory games are told about, at most `2047`; `0`: the real values |
| `vram` | `512`: MB of video memory DirectDraw reports, `1`–`2047` |
| `cpu` | `2000`: MHz in the registry; `0`: the real clock |
| `cpus` | `1`: processors the game runs on and is told about; `0`: all |
| `desktopbpp` | `32`; `16` or `24`: the desktop's colour depth the game is told about; while the game has a mode of its own, that mode's depth |
| `lockwhole` | `off`; `on`: a `Lock` of part of a surface counts as a change to all of it, for a game that writes outside what it locked |
| `zaccess` | `on`: Z buffers can be locked and blitted by DirectDraw where the GPU allows it; `off`: on the GPU only, for a driver that misbehaves with it |
| `videooverlay` | `on`: overlay surfaces offered and laid over the primary when shown; `off`: no overlay hardware, so games blit instead |
| `log` | `0`; `1` (info); `2` (every blit, lock, flip, palette change, Direct3D 3 execute buffer, window message and failed call) |
| `drawtrace` | `off`; `on`: one frame's Direct3D draws logged every 600 scenes (about 10 s) |
| `logfile` | a path; by default `dd2d9.log` next to the game's exe |

**Environment variables:** `DD2D9_INI` names an extra config file. `DD2D9_LOG=0`, `1` or `2` overrides `log`; any other value is taken as a log file path (for example `DD2D9_LOG='Z:\tmp\dd2d9.log'` under Wine).

## Files dd2d9 uses

| File | |
| --- | --- |
| `ddraw.dll` | dd2d9 itself. |
| `dd2d9.ini` | Settings (see [Settings](#settings)). |
| `dd2d9.log` | Next to the game's exe, with `log=1` or more. If that folder can't be written to, the current folder or `%TEMP%`. Send it with any bug report. |

## Troubleshooting

- **Nothing changes, and `log=1` writes no `dd2d9.log`:** dd2d9 isn't being loaded. Under Wine, check the `ddraw=n` override, or run `install-dd2d9.sh install` again after a Wine update. On load, dd2d9 also sends a debug message saying where it was loaded from: under Wine 9, `WINEDEBUG=warn+seh` prints it; on Windows, use DebugView.
- **A game refuses to start on a 32-bit desktop** ("16 bit screen mode required"): set `desktopbpp=16` in a `[game.exe]` section.
- **A game says there isn't enough memory:** keep `ram` at its default, or lower it.
- **A game runs too fast:** set `fpslimit`. If it times itself from the registry's processor clock, try `cpu=0`.
- **The mouse is off in a scaled or windowed mode:** please report it with `dd2d9.log` (`log=2`); the translation can't be turned off since 1.3.0.
- **Z-fighting (hatched light maps or decals):** try `wbuffer=force` or a higher `overlaybias`, and `depth24=on`.
- **Display mode switches are a problem:** `display=borderless` or `display=window` avoid them.
- **Under Wine with DXVK in the game's folder:** `d3d9=system`, the default, presents through the prefix's own Direct3D 9; `d3d9=game` uses the game folder's.
- **Something else:** set `log=2` and look for failed calls in `dd2d9.log`. Please report problems with the log.

## Building from source

Requires mingw-w64 (i686) and Meson. On Arch: `pacman -S mingw-w64-gcc meson`.

```sh
meson setup build --cross-file build-win32.txt
ninja -C build
```

The output is `build/ddraw.dll`, statically linked, with no MinGW runtime to install.

---

## For developers

### Repo layout

| Path | What |
| --- | --- |
| `src/` | The DLL. `ddraw.cpp` (the DirectDraw object, display modes, focus, the window), `surface.cpp` and `blit.cpp` (surfaces, the software blitter), `presenter.cpp` (presenting through Direct3D 9), `gpu.cpp` and `gpures.cpp` (the shared Direct3D 9 device, GPU copies of surfaces and textures, drawing), `d3d7.cpp` (Direct3D 7), `d3d6*.cpp` (Direct3D 3–6: objects, devices, viewports, the CPU vertex pipeline and execute buffers), `compat.cpp` and `hooks.cpp` (import patching for the compatibility options), `input.cpp` (mouse translation), `convert.cpp` (SSE2 pixel conversion), `util.cpp` (config, logging). |
| `tests/` | The test programs below. |
| `tests/wine/` | Runs Wine's own ddraw test suite against a build and compares it with a baseline (see its README). |
| `re/` | Reverse-engineering notes (below). |
| `tools/` | The generator for the Direct3D interface stubs, and `ddtrace/`: a separate `ddraw.dll` that logs a game's DirectDraw and Direct3D calls in front of any `ddraw.dll` (Windows' own, dd2d9 or another wrapper), for comparing them call for call (see its README). |

### Tests

`build/ddtest.exe` writes its results to `ddtest.txt`:
- with no argument, pixel-exact checks of blits, keys, conversion, GDI, interface versions, palettes, object lifetimes and cooperative levels;
- `ddtest fs8` runs a 640×480×8 exclusive-mode flip-chain animation and checks the pacing;
- `ddtest win` runs a windowed primary with a clipper;
- `ddtest mouse` checks the mouse translation (run it with `display=borderless`);
- `ddtest audit borderless` and `ddtest audit window` (run with that `display` in `dd2d9.ini`) take fullscreen the ways games do and check where the picture lands on the real screen, and that every way of asking for the screen's size gives the game's mode;
- `ddtest fuzz` runs thousands of random blits (8, 16 and 32-bit, colour keys (key ranges where `SetColorKey` takes them: dd2d9, as `ddraw.dll`'s software blitter, takes single colours), mirroring, stretching, overlapping copies) against a reference implementation.

`build/focustest.exe [paintfg]` runs a 640×480×16 exclusive-mode flip chain for a minute, to be switched away from and back to (Alt+Tab); each second it writes the frames flipped and whether the window is minimized to `focustest.txt`. `paintfg` takes the foreground back in the window's `WM_PAINT`, as Need for Speed III does: switched away, it must stay away.

`build/d3d7test.exe [w h [bpp]] [3d] [gamma]` renders a Direct3D 7 triangle with DirectDraw fills and GDI text on top (`3d`: without), then flips; `gamma` sets an inverting gamma ramp. It writes `d3d7test.txt`.

All checks pass under Wine 11 (wined3d, Xvfb).

`tests/wine/` runs Wine's own ddraw tests (expectations confirmed on Windows) against a build and compares the result with a baseline; see `tests/wine/README.md`.

`tests/sdk/` runs the DirectX 7 SDK's samples with dd2d9 and with Wine's own ddraw and compares screenshots of the two (`run.sh`, then `compare.py`; the samples' locations are set in `run.sh`).

`build/ddbench.exe` measures performance and writes `ddbench.txt`: `ddbench blit` (blitter throughput), `ddbench flip <w> <h> <bpp>` (a sprite game loop), `ddbench window` (small updates to a windowed primary), `ddbench zlock [w h]` (Direct3D frames with small and whole locks of the Z buffer). With `log=1`, the presenter logs every 10 seconds how much of the frame it converted and how long the game waited for it.

`build/d3dimbench.exe [seconds]` checks and times Direct3D 3-6's CPU vertex pipeline: ProcessVertices with lights, fog and clipping (and odd vertices: NaN, infinities, zero normals), a lit sphere drawn with DrawIndexedPrimitive, 2048 small DrawPrimitive calls of transformed vertices a frame (the cost of a draw), and execute buffers (a TRIANGLE instruction per row, per triangle, and cut by the near plane), and checks that consecutive TRIANGLE instructions draw the same pixels as drawn one by one. It writes `d3dimbench.txt` with a hash of each part's output, the time spent in the drawing calls, the transformed vertices (`d3dimbench-tl-*.bin`) and images (`d3dimbench-*.raw`, 256x256 32-bit): two builds meant to give the same output are compared by those.

`build/reloadtest.exe [dll]` frees and reloads dd2d9 (by default `ddraw_.dll`) while the game window stays subclassed, as a wrapper loading dd2d9 under another name may do.

### Reverse-engineering notes

`re/` has the vtable map of XP SP3's `ddraw.dll` (5.3.2600.5512), with naming scripts for Ghidra and IDA. `re/spec/` describes, from its decompiled code, what each method checks and in which order, with the differences from dd2d9 that remain. [`CHANGELOG.md`](CHANGELOG.md) explains every change and which game or test showed the need for it.

---

## License

MIT, see [LICENSE](LICENSE). dd2d9 contains no Microsoft code or binaries.
