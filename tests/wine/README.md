# Wine's ddraw tests against dd2d9

Wine's `dlls/ddraw/tests` check DirectDraw behaviour call by call (return codes,
reference counts, lost surfaces, window messages, pixel results), and their
expectations are confirmed on real Windows. Running them against dd2d9 finds
differences from Microsoft's `ddraw.dll` without needing a game that hits them.
These scripts build those tests, run each sub-test in its own process (so a crash
or hang costs one sub-test, not a whole file), and compare a run with a baseline.

## Requirements
Linux with Wine (11 or newer, with 32-bit support), mingw-w64 (i686), git, Python 3, and
TigerVNC's `Xtigervnc` with `xrandr` (Xvfb works too, but can't switch display modes,
so the mode-switch tests fail there).

## Use
    ./fetch.sh                  # Wine's tests at the pinned commit, prepared in work/
    ./build.sh                  # work/*.exe
    ./run.py ../../build/ddraw.dll mine      # results/mine.jsonl (several hours)
    ./compare.py baseline.json results/mine.jsonl

- `run.py` uses Windows' expectations by default (`WINETEST_PLATFORM=windows`):
  Wine's `todo_wine` marks count as failures, since the goal is to match Windows.
  `--lenient` uses Wine's. `--only ddraw7` runs one test file. `--display` and
  `--prefix` pick the X display and Wine prefix; `WINE=/path/to/wine` picks Wine.
- `compare.py` lists every sub-test that got worse (with its new failures) or
  better, and exits 1 if anything got worse.
- After an intended change, `compare.py baseline.json results/mine.jsonl
  --write-baseline baseline.json` records the new state.
- To look at one sub-test: `WT_ONLY=test_lost_device WINETEST_PLATFORM=windows wine work/ddraw7.exe`,
  with `log=2` in a `dd2d9.ini` next to it.

`baseline.json` was made from dd2d9 0.9.0 with Wine 11.0 (WoW64) on Xvnc. Another Wine
version or X server may move a few display-related results; compare runs from the
same machine when in doubt. A full run takes a few hours; the four `ddrawN` files can
run at once (`--only`, each with its own `--display` and `--prefix`), but then the
mode-switch tests miss a display-change message now and then (`test_coop_level_mode_set`).

## Expected failures
The differences dd2d9 makes on purpose (see the main README), and environment ones:

| Sub-tests | Why |
| --- | --- |
| `test_window_style`, `test_redundant_mode_set`, parts of `test_coop_level_mode_set` | in exclusive mode dd2d9 grows the window so that its frame lies off the screen (`ddraw.dll` puts the whole window on it) |
| `test_p8_blit` | 8-bit to RGB blits go through the palette |
| `test_rt_caps`, `test_set_render_state`, `d3d test_get_caps*` (SetRenderTarget) | XP's d3dim700 codes: SetRenderTarget's surface checks return DDERR_INVALIDPARAMS and retired render states are accepted; newer Windows (which the tests follow) returns INVALIDCAPS / INVALIDPARAMS |
| `test_depth_blit`, `test_depth_readback` | Z buffers live on the GPU: DirectDraw can't read or copy them |
| `test_block_formats_creation`, `test_compressed_surface_stretch` | DXT surfaces in game memory, and blits of DXT surfaces, aren't supported |
| `test_specular_lighting`, `test_fog_process_vertices`, `test_process_vertices`, `test_lighting`, `test_fog_*` | ProcessVertices quirks (specular with a local viewer, vertex blending beyond the FVF) and D3D9's lighting and fog at the edges |
| `test_device_load` | Load of mip levels the sizes don't line up for |
| `test_cross_device_blt` | dd2d9 converts between pixel formats in Blt; `ddraw.dll` refuses (`DDERR_UNSUPPORTED`) |
| `test_compute_sphere_visibility` (ddraw4, tests 2 and 5) | Windows' DX6 answers there aren't understood (Wine fails them too) |
| `test_coop_level_create_device_window` | `DDSCL_CREATEDEVICEWINDOW` isn't implemented |
| `test_surface_attachment`, `test_flip` (ddraw1) | a reference `ddraw.dll` leaks when a DirectDraw 1 flip chain shrinks |
| `test_getdc` (A2R10G10B10) | Wine's GDI has no DC for that format; Windows does |
| `test_initialize` | COM registration in the test prefix |

## Files
- `fetch.sh`, `patch_tests.py`: get Wine's tests and add the one-sub-test switch
  (`WT_ONLY`); `work/subtests.json` lists the sub-tests.
- `build.sh`, `shim.c` (Wine's debug helpers), `iid3.c` (a GUID mingw lacks).
- `display.sh`: the X server with the extra display modes the tests use.
- `run.py`, `compare.py`, `baseline.json`.

Wine's test files are LGPL-2.1 and stay in `work/`, fetched rather than copied here.
