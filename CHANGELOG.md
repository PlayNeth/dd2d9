# Changelog

Each entry says what changed and, for behaviour fixes, the game or test that showed it,
so a later change doesn't undo it without knowing why.

## 1.2.2
- **Movies in the video player's window control (MCIWnd) are scaled with the game too.**
  Rockman X4 plays its movies in an MCIWnd window (msvfw32's `MCIWndCreate`), a child of
  its own window that it moves and sizes (`MoveWindow`, `SetWindowPos`) and places the
  picture in (`MCIWNDM_PUT_DEST`) in its mode's coordinates: the movie played at its size
  in the corner of the screen, as Street Fighter Zero's did before 1.2.1. Those calls are
  now translated for an MCIWnd window in the window the game is shown in, scaled as the
  image is, and the game's unchanged frame isn't presented over one while it shows. New
  ddtest checks.
- **`display=borderless` covers the primary monitor**, as `display=switch` (and
  `ddraw.dll`'s fullscreen) does, instead of the one Windows had put the game's window on:
  Rockman X4 came up on either monitor from one start to the next. `display=window` stays
  where the window is.
- Tested: ddtest 321/321, d3d7test 40/40, Wine's ddraw tests with no regressions; Rockman
  X4's movies and monitor confirmed in the game by the user who reported them.

## 1.2.1
- **Videos that Windows' player draws into the game's window are scaled with the game.**
  Games that play their movies through MCI (`mciSendString`: "window ... handle", "put ...
  destination at x y w h") place the picture in their window's coordinates as they see
  them, their mode's; in a mode shown scaled (`display=borderless` or `window`,
  `mouse=scale`) the player drew it at those pixels of the real window: Street Fighter
  Zero's opening movie played at its size in the corner of the screen. The game's imports
  of `mciSendStringA/W` now put the destination where the image is, scaled and moved as
  the image is; a video shown in that window without a destination gets its natural size
  there, scaled too; and while one plays, an unchanged frame isn't presented again over
  it. (`mciSendCommand`'s `MCI_PUT` isn't translated.) New ddtest checks (a small AVI
  played into the window).
- The Direct3D design notes said cube maps were still to do; they have been in since 1.1.0.
- Tested: ddtest 319/319, d3d7test 40/40, Wine's ddraw tests with no regressions; Street
  Fighter Zero's movie confirmed in the game by the user who reported it.

## 1.2.0
The optional items left after 1.1.0:
- **Overlays in windowed programs.** 1.1.0 offered overlays to fullscreen programs only: a
  windowed one paints the destination colour key into its window with GDI, on the desktop
  dd2d9's primary doesn't hold, so its overlay stayed hidden (the DirectX 7 SDK's DDOverlay
  sample showed nothing). Now the presenter also looks for the key in what the window draws
  (`WM_PRINT`, ten times a second at most, waiting 50 ms at most for an answer), and a key
  that shows nowhere under the overlay doesn't hide it. DDOverlay shows its picture; Wine's
  `test_overlay_rect` (where its DWM check lets it run), `test_offscreen_overlay` and
  `test_yv12_overlay` run and pass. Only the part of an overlay over the program's own
  window can be seen (the SDK's Mosquito puts its overlay beside its window). New ddtest
  check: a window half key colour, half black.
- **YV12 surfaces** (and I420/IYUV): planar 4:2:0 video, offscreen or overlay, in
  `GetFourCCCodes`. Locked a luma byte per pixel (pitch 256-byte aligned, as overlay
  hardware had it), blitted to and from any surface through 32-bit colour, copied plane by
  plane between two of one format, laid over the screen as overlays. Not colour-filled, not
  textures. New ddtest checks.
- **Render-target textures copied on the GPU when mipmapped or colour-keyed** (until now
  read back through the CPU, which waits for the GPU): the levels Direct3D drew into copied
  there, the others uploaded from their CPU copies; a colour key made transparent black by a
  pixel shader, as the CPU path does it. Palettized ones still go through the CPU. New
  d3d7test checks.
- **The `DDSCL_CREATEDEVICEWINDOW` window** covers the screen in exclusive mode and shows the
  picture, as `ddraw.dll`'s did (it was kept hidden, the picture in the focus window, so a
  game that left that window small or hidden showed nothing). Its mouse messages go on to the
  focus window (double-clicks as plain clicks where that window takes no double-clicks). It
  hides while the game is in the background. New ddtest check (with `DDSCL_NOWINDOWCHANGES`).
- **Direct3D 7's `GetClipStatus`** takes vertex blending and the user clip planes into
  account (`D3DCLIP_GEN0`-`5`); **Direct3D 3–6's `Pick`** finds a triangle the clip volume
  cuts in its visible part. New d3d7test and ddtest checks.
- **Bump maps** (`DDPF_BUMPDUDV`: U8V8, U5V5L6, U8V8L8), found missing by the DirectX 7 SDK's
  BumpEarth: the caps said the GPU does environment-mapped bump mapping (D3D9's
  `D3DTEXOPCAPS_BUMPENVMAP`), but no bump format was listed or could be made, so the sample
  stopped with an error. Now `EnumTextureFormats` lists those the GPU has (Direct3D 6 and 7
  hardware devices), they are made and uploaded as D3D9's V8U8, L6V5U5 and X8L8V8U8 (U8V8L8
  widened), and BumpEarth draws as with Wine's own ddraw. Blits between a bump map and a
  colour surface are refused (they only copy as they are). New d3d7test check.
- **Overlays laid over the picture on the GPU** (a pixel shader with the destination and
  source colour keys) instead of on the CPU: until now a visible overlay made the presenter
  convert the whole primary every frame and read Direct3D's drawing back from the GPU to lay
  the overlay over it. Now only an overlay that changed is decoded and uploaded; the rest
  stays on the GPU. The CPU path stays for a GPU without pixel shaders.
- **Blits from a render target on the GPU**: a plain copy (`Blt` without effects, colour
  keys or a raster operation other than `SRCCOPY`) from a surface Direct3D drew into, to the
  primary, a back buffer or another surface on the GPU of one format, is made there with
  `StretchRect` (stretched too, when unclipped) instead of reading the drawing back to the
  CPU first. Windowed Direct3D games that blit their back buffer to the primary (the SDK's
  framework does) no longer wait for the GPU every frame. New d3d7test check.
- **SSE2** for YUY2/UYVY and YV12/I420 decoding (eight pixels at a time, the same results as
  before to the bit) and a faster U8V8L8 bump-map upload.
- **A review of the whole code** for speed and mistakes found:
  - *Faster software blits:* a blit from or to a DXT/YUV surface decodes only the rows it
    uses into memory kept from blit to blit (it decoded the whole surface into a newly
    allocated one, and for a source without alpha copied and compared all of it after: a
    32x32 sprite out of a 1024x1024 DXT atlas decoded a million pixels); blits between
    formats convert whole rows with SSE2 and pack through tables (they went through two
    out-of-line conversions per pixel); a stretched blit copies a repeated row instead of
    making it again; a plain copy of any depth, overlapping scrolls included, goes row by
    row with `memmove` (24-bit ones went pixel by pixel, overlapping ones through a copy of
    the whole source); 24-bit primaries and textures convert four pixels at a time.
    `ddbench blit` (new cases for these) against 1.1's blitter: a YUY2 frame stretched to
    640x480 2.8-7.5x faster, 16/32-bit conversion 3.4-3.7x, a scroll 2.3-4.5x, a 2x stretch
    1.2-1.4x, a sprite out of a DXT atlas about 40x; the rest as fast as before or faster.
  - *Fewer Direct3D 9 calls:* Direct3D 3-6 send stage 0 and 1's texture operations and
    the alpha test only when they change (up to 17 calls at every draw before), Direct3D 7
    the alpha test only when it changes (at every stage-0 texture change before); strided
    vertices are gathered into memory kept from draw to draw.
  - *Fixed:* a windowed overlay with a destination colour key didn't show under a gamma
    ramp (the key was compared with the frame after the ramp); the GPU overlay path could
    use a released texture when the device was reset while an overlay appeared; an 8-bit
    overlay's palette changes didn't reach the GPU path; a windowed render target blitted
    to the primary while the window was partly off the screen was read back every frame
    (it now goes through the CPU as before); no multisampled copy of the primary is made
    for it under `antialiasing`; a YUY2/YV12 blit between a surface and its
    `DuplicateSurface` copy that overlapped copied rows in the wrong order; a duplicate
    counted its memory twice; a palette released too often left a surface that outlived
    its flip chain pointing at it; Direct3D 7's `ProcessVertices` without clipping left
    the clip codes of an earlier clipping one in the destination (those vertices then
    drawn as clip-space ones); `SetViewport` with sizes that wrap around 4 GB was taken;
    indexed draws with indices past the vertices given read past them (now vertex 0, as
    the vertex buffer path did); a huge vertex count could end the game with an uncaught
    out-of-memory exception; Direct3D 3-6's textured viewport background turned texture
    wrapping off; `SetExecuteData` vertex counts and offsets near 4 GB weren't refused;
    matrix handles weren't freed with their device; a vertex buffer made with
    `D3DDP_DONOTCLIP` was still clipped when drawn; with `desktopbpp`, a game putting back
    the "current" mode it read switched the real desktop to that depth (or failed); an ini
    value that is only a comment (`logfile= ; ...`) was taken as the value.
  - *Startup:* the compatibility hooks walk each module's imports once (they were walked up
    to five times at every DirectDraw object a game created).
- **Overlay and capture tools** (RivaTuner Statistics Server, MSI Afterburner) show and
  record dd2d9's frames: they went out through an additional swap chain on the game's
  window, which those tools don't hook. Now the device's own implicit back buffer (as large
  as the largest monitor) holds the frame, `IDirect3DDevice9::Present` shows it in the
  game's window, the device is made on that window, and every frame has a draw call (one
  that draws nothing): RivaTuner draws only into frames that had one, and a 2D game's are
  copied into the back buffer. A window larger than every monitor still gets a swap chain.
- **Found in games after the review** (with users' logs):
  - *3DMark 2000:* the benchmark looked like 30 frames a second however fast it ran, and its
    logo's fade showed few steps: with `cpus=1` the presenter thread shared the one processor
    with a game drawing without waiting for the vertical blank, at equal priority, and only
    ran at the end of its time slices. It now runs above the game's threads. The splash
    showed nothing: it calls `LockWindowUpdate` on its full-screen window (which hid all of
    dd2d9's picture in it, where `ddraw.dll` drew on the screen itself); that lock is no
    longer done on the window dd2d9 shows the primary in.
  - *FlyingSB:* grey instead of its colours: it sets up its palette only when the screen is
    a palette device, so in an 8-bit mode of the game's own `GetDeviceCaps` now answers as XP
    did after the switch (8 bits, `RC_PALETTE`, 256 entries, 20 reserved), and in any mode of
    its own that mode's depth and size; and the display's palette is kept from one primary
    to the next (a primary made without a palette shows the colours the last one left, as
    the hardware palette did). Back from Alt+Tab nothing showed: it goes exclusive again
    while still minimized, and the window region was made from the icon's rectangle.
  - *Need for Speed: High Stakes* quit after Alt+Tab: it never restores its flip chain, and
    every Flip failed. With `display=borderless` or `window`, surfaces are no longer lost
    once the game is back in front (the display never left its mode).
  - *Two monitors with `display=switch`:* the game's window went to the monitor it was on
    and the first one changed mode; now both are the primary monitor's, as with
    `ddraw.dll`.
  - *Xuan-Yuan Sword 3 EX* crashed: it writes a few rows past the end of its back buffer
    (surfaces now have slack around their memory); back from Alt+Tab it was told its window
    was the monitor's size by `WM_SIZE` and made its buffers that size (its window's size and
    move messages now carry the mode's size, as `GetClientRect` already did); and it draws its
    cursor where `GetCursorPos` says, unclipped, which went past 640x480 while its window was
    exactly the mode's size (the cursor is now kept on the game's screen then too; with
    `mouse=raw` the game still crashes once the cursor leaves that area).
- Tested against the DirectX 7 SDK's samples (Direct3D and DirectDraw), each compared with
  the same sample on Wine's own ddraw: all draw what the reference draws (the fullscreen
  DirectDraw ones, whose mode switch Wine's ddraw can't make on the test display, draw with
  dd2d9 only).

## 1.1.0
The release after 1.0.1: 1.0.2, 1.0.3 and 1.0.4 (below), and the fixes that testing them in
games brought.

Since 1.0.1, in short:
- **Wine's ddraw test suite:** failed checks down from 463 to 162; 386 of its 423 sub-tests
  clean. The rest are deliberate differences or the test machine (tests/wine/README.md).
- **Direct3D 7 as Windows draws it:** ProcessVertices lighting (normals, vertex blending,
  specular power), fog with equal start and end and exponential vertex fog, `Load` of mip
  levels.
- **Z buffers DirectDraw can lock and blit**, where the GPU has the INTZ depth format and
  antialiasing is off (new `zaccess=off` turns it off); a small lock reads back only its
  rectangle.
- **FourCC surfaces:** DXT1–5 in the game's memory, and blits to and from DXT, YUY2 and
  UYVY surfaces. `GetFourCCCodes` lists them.
- **`DDSCL_CREATEDEVICEWINDOW`** makes `ddraw.dll`'s device window.
- **Direct3D 3–6** caches its list of active lights; `DDCREATE_EMULATIONONLY` objects make
  system-memory surfaces.

New in 1.1.0:
- **antialias2d=off: 2D draws that write depth are antialiased with the rest.** That path
  draws 2D without antialiasing into a surface with no Z buffer, so a draw there wrote no
  depth. NFS Porsche Unleashed draws its menu labels with the depth test passing always
  but writing depth, then translucent button panels behind them: the panels covered the
  labels, which came out dim. The published 1.0.1 already kept such draws out of that path
  (a fix made in review); the game showed why, and a draw with no Z buffer attached now
  takes the 2D path too. New d3d7test check.
- **In the background after Alt+Tab, back buffers can't be locked**, as with `ddraw.dll`
  (the primary and offscreen surfaces still can: Captain Claw, 0.7.19). NFS Porsche
  Unleashed stopped with "D3D pageflip called in while locked" when Alt+Tabbed or the
  Windows key was pressed. New ddtest check.
- **`zaccess=off`** (new option): Z buffers on the GPU only, as before 1.0.3, in case a
  driver misbehaves with the INTZ depth textures that let DirectDraw read and write them.
- `drawtrace=on` also shows the fog states, marks the draws taken as 2D without
  antialiasing (`FLAT2D`), and logs the copies between the antialiased and resolved images.
- New d3d7test check: a back buffer locked after every flip finds the frame drawn into it.

The last of what the README listed as not implemented:
- **Overlays in exclusive mode.** Overlay surfaces (video memory; RGB, YUY2 or UYVY) are
  made, and the presenter lays the visible ones over the primary: stretched or shrunk,
  mirrored, behind a destination colour key and with a source key, in their z-order.
  Windowed programs still get no overlay hardware: they paint the destination key into
  their window with GDI, which dd2d9's primary doesn't see (the DirectX 7 SDK's DDOverlay
  sample showed nothing), and overlays outside their window couldn't be shown. `UpdateOverlay`,
  `Get`/`SetOverlayPosition`, `UpdateOverlayZOrder`, `EnumOverlayZOrders`, overlay keys in
  `SetColorKey` and overlay flip chains (`DDFLIP_EVEN`/`ODD`) follow `ddraw.dll`'s checks;
  the caps report the overlay hardware. New `videooverlay=off` reports none, as before.
  Wine's overlay tests run windowed, so they still skip. New `ddtest overlay` (exclusive,
  read back from the screen).
- **Cube maps** in Direct3D 7: D3D9 cube textures, each face with its mip levels, faces
  Direct3D drew into read back first; `Load` copies the faces it is told to. Caps say
  `D3DPTEXTURECAPS_CUBEMAP` where the GPU has it. Wine's `test_device_load` passes all its
  checks, and the DirectX 7 SDK's EnvCube sample (a cube map drawn into every frame) looks
  as with Wine's own ddraw. New d3d7test check.
- **Blits that convert to an 8-bit surface** pick the nearest palette entry (they wrote
  index 0).
- **Colour keys on FourCC surfaces** (YUY2, UYVY, DXT) work in blits: compared as the
  colour they decode to.
- **Direct3D 3–6 `Pick`** finds the triangles under the point, with their offset and
  depth, as XP's driver path did (`GetPickRecords`); **`D3DOP_SPAN`** is drawn (as points)
  where XP drew it: unclipped execution, or after a `PROCESSVERTICES` copy.
- **Ramp devices light in the mono model**: the lights' brightness shades the material's
  colours, as XP's Ramp rasterizer did.
- **Direct3D 7 `GetClipStatus`** follows what is drawn: the clip codes of untransformed
  vertices, and their extents with `D3DRENDERSTATE_EXTENTS` (from the first call on, as it
  costs a transform of each vertex). New d3d7test check.
- New ddtest checks for the 8-bit conversion, FourCC keys, Pick and spans.

## 1.0.4
- **Small Z buffer locks read back only their rectangle.** 1.0.3 copied the whole Z buffer
  back for every lock: an 8x8 lock each frame (a lens flare's occlusion test) cost as
  much as a whole one. A lock of at most 64x64 pixels now copies just that part through a
  small target, the rest of the CPU copy staying behind until something reads it (what
  DirectDraw wrote into a part goes up to the GPU first). New `ddbench zlock`: under Wine
  (software rendering) an 8x8 lock went from about 1.7 to 0.6 ms a frame at 640x480 and
  from 4 to 0.6 ms at 1024x768; the rest is waiting for the GPU, which a read needs. New
  d3d7test check: Z read and written in small and whole locks, and drawn against.
- **YUY2 and UYVY surfaces blit** to and from any surface (a video player decoding into
  an offscreen surface and blitting it to the screen, stretched or not): converted
  through 32-bit colour with BT.601 video levels, the chroma of a written pixel pair
  averaged. A plain copy between two surfaces of one YUV format is copied as it is. A
  colour fill of a YUV surface writes the value as it is (Wine's `test_color_fill`).
  `GetFourCCCodes` now lists YUY2, UYVY and DXT1–5 and the caps say `DDCAPS_BLTFOURCC`, as
  a driver that blits them does (until now none were listed, so a game that checks first
  never used them). New ddtest checks. Wine's `test_rt_caps` now runs its YUY2 flip-chain
  case, which meets the known difference in SetRenderTarget's codes (XP's
  `DDERR_INVALIDPARAMS`, newer Windows' `DDERR_INVALIDCAPS`): 2 more expected failures,
  162 in all.

## 1.0.3
The three missing features behind Wine test failures (failed checks down from 367 to 160;
37 of 423 sub-tests left, each a difference on purpose, a feature still missing or the test
machine: tests/wine/README.md):
- **DXT1–5 surfaces in game memory** (`DDSD_LPSURFACE`): taken by their linear size, which
  must hold every block (a pitch is refused; IDirectDraw4 takes a pitch flag alone), and
  `GetSurfaceDesc` reports a linear size of ~0 for them, as `ddraw.dll`
  (`test_block_formats_creation`, ddraw4/7).
- **Blits from, to and between DXT surfaces:** each compressed side is decoded to A8R8G8B8
  for the blit (stretching, keys and mirroring as any blit), and the destination's blocks
  that the rectangle touches are encoded again (new `src/dxt.cpp`). As Windows' drivers
  encode: the two colours farthest apart are a block's endpoints (two colours come back
  exactly), and a source without alpha is opaque. Fills into them still fail with
  `DDERR_UNSUPPORTED` (`test_compressed_surface_stretch`,
  `test_surface_format_conversion_alpha`'s DXT cases, which used to be skipped).
- **Z buffers readable and writable by DirectDraw:** where the GPU samples `INTZ` depth
  textures (nearly all, DXVK too) and antialiasing is off, a Z buffer is made as one. Lock
  and Blt read it back by drawing it into an R32F target, and what DirectDraw writes goes
  up by drawing it with a shader writing depth, before the next draw or clear that needs
  it (a whole-target clear skips the upload). Depth fills stay on the GPU as before
  (`test_depth_blit`, `test_depth_readback`, ddraw2/4/7).
- **`DDSCL_CREATEDEVICEWINDOW`** makes `ddraw.dll`'s "DirectDrawDeviceWnd" window, owned
  by the focus window, handing it the keyboard focus. It stays hidden (the picture still
  goes to the focus window) and is destroyed by the next call with another window or the
  release. SetCooperativeLevel's rules about the focus window follow `ddraw.dll`'s
  window-and-focus bookkeeping: `SetCooperativeLevel(NULL, DDSCL_NORMAL)` forgets the
  focus window for `DDERR_NOFOCUSWINDOW`, and `DDSCL_SETFOCUSWINDOW` with
  `DDSCL_CREATEDEVICEWINDOW` and no window makes the device window, then fails with
  `DDERR_NOHWND` (`test_coop_level_create_device_window`, all four interfaces).

## 1.0.2
Wine's test suite down from 463 failed checks to 367 (50 of 423 sub-tests left, all either
differences on purpose, missing features or the test machine: tests/wine/README.md):
- **DirectDraw objects made with `DDCREATE_EMULATIONONLY`** put their surfaces in system
  memory (unless video memory is asked for) and refuse 24-bit Z buffers, as the emulation
  layer does (`test_caps`, all four interfaces).
- **Direct3D 7 ProcessVertices lighting, as XP's:**
  - normals go through the transpose of the inverse of the whole 4x4 world x view: a
    non-affine world turns them to zero, a singular one (no inverse) too (`test_lighting`);
  - normals are normalized only with `NORMALIZENORMALS`, so a world or view that shrinks
    lights stronger and saturates (`test_fog_process_vertices`: 57% green became full);
  - a material with specular power 0 keeps the last power above 0
    (`test_specular_lighting`, 47 checks);
  - vertex blending reads `VERTEXBLEND`'s count of weights after the position, whatever
    the FVF declares (this undoes 0.9.17's cap at the FVF's weights, which Wine's test on
    Windows contradicts), blends the matrices' whole 4-component results (a world with a w
    column), and blends each matrix's normal by the weights without normalizing the sum
    (`test_process_vertices`, `test_lighting`).
- **Direct3D 7 fog, as Windows draws it:** exponential vertex fog with lighting off on
  vertices that bring a specular colour takes the fog factor from its alpha
  (`test_fog_interpolation`); FOGSTART equal to FOGEND makes table fog a step at that depth
  and linear vertex fog fog everything (`test_fog_special`). Sent per draw (`SendFog`).
- **`IDirect3DDevice7::Load` of mip levels:** each smaller level's source rectangle rounds
  outward and the destination point halves, also over the source levels skipped to reach
  the destination's size (`test_device_load`: the levels were off by a pixel, and a load
  into a smaller texture failed).
- **Direct3D 3-6 keeps its list of active lights between draws** while no light, viewport
  light list, current viewport or view matrix changes (`g_lightGen`).
- The Wine test baseline is updated, and its README lists the newly triaged sub-tests.

## 1.0.1
- **Transformed vertices are drawn without Direct3D 9's clipping**, so depth past [0, 1]
  is clamped per pixel as the hardware of the era did, not cut off. NFS Porsche
  Unleashed's sky haze (depth up to 11, and the sky dome from -100 to 700) was cut in
  wedges on Intel's driver; Wine's own Direct3D clamps, so it never showed there.
  Untransformed vertices keep the game's CLIPPING state. d3d7test checks a triangle from
  depth 0.5 to 3 draws whole.
- **`antialias2d=` (off by default): with antialiasing, 2D drawn through Direct3D stays
  sharp.** A draw of transformed vertices at one depth and one rhw (screen-aligned: no
  perspective) with the depth test off, or ALWAYS without writing depth, is drawn into
  the resolved image after the 3D so far; the next 3D draw copies the part drawn into the
  multisampled image, as for the CPU's drawing. HUDs' edges (NFS Porsche Unleashed's dials) came out soft. `on`
  antialiases them as before. d3d7test's slanted-edge check is now depth tested (3D), and
  a 2D triangle is checked beside it.

## 1.0.0
First public release. Since 0.9.21:
- **`desktopbpp=` also answers GDI and EnumDisplaySettings** (`GetDeviceCaps` BITSPIXEL and
  NUMCOLORS on display and memory device contexts, the current and registry settings of
  the primary display), for games that check the desktop there rather than through
  DirectDraw. While the game has a display mode of its own, its depth is reported, as on
  XP; an 8-bit one is left real (GDI would have to act as a palette device). The imports
  are patched only when `desktopbpp` isn't 32.
- **`lockwhole=on`**: a Lock of a rectangle counts as a change to the whole surface (the
  GPU copy and the screen). For a game that writes outside the rectangle it locked, as
  XP's memory let it; off by default (since 0.9.18 only the rectangle reaches the GPU).
- **log=2 logs Direct3D 3 execute buffers** (one line per Execute: vertices and the
  instructions it holds) **and GetDisplayMode** (when its answer changes). Both of Moto
  Racer's problems were invisible at log=2 before.
- The Wine test baseline is brought up to date.

## 0.9.21
- Moto Racer's menu, continued: rhw that are tiny but not 0 (denormals, which the GPU takes
  as 0) count as bad too, and an untextured draw whose rhw differ over 10000 times gets
  one rhw (the smallest weigh nothing in Direct3D 9's interpolation). drawtrace prints rhw
  in full (%g), not to six decimals (tiny values read 0.000000).

## 0.9.20
- **Transformed vertices with rhw 0 beside valid ones in one draw** are drawn coloured.
  Hardware of the era interpolated colour across the screen without rhw; Direct3D 9
  divides every attribute by it, and a triangle whose rhw are all 0 came out black on
  Intel's driver (Moto Racer's menu letters flickered black: untextured, part of their
  vertices at rhw 0). Such a draw now gets rhw 1 on every vertex when untextured, else the
  draw's smallest valid rhw on the bad ones. (A draw with rhw 0 everywhere was already
  handled.) d3d7test checks it, though Wine's Direct3D draws it right either way.

## 0.9.19
- **`desktopbpp=` (16, 24 or 32; default 32): the desktop colour depth DirectDraw
  reports** before the game sets its own mode (GetDisplayMode, the windowed primary
  surface). Moto Racer calls GetDisplayMode at startup and stops with "16 bit screen mode
  required !" unless it says 16, even though it then switches to its own mode;
  `desktopbpp=16` in its section lets it start (as Windows' "Reduced color mode" did).

## 0.9.18 (optimizations)
No behaviour changes intended; each one checked with the full test set.
- **Presenter: the frame is converted before taking the GPU lock**, which now covers only
  copying the converted rows into the upload surface (the game's Direct3D draws on the
  same device no longer wait for the conversion).
- **Presenter's gamma and sharp-bilinear passes put back only the device state they
  change** (`SavedDeviceState`, src/devstate.h) instead of the game's Direct3D context
  sending all of its state again (some 450 calls) after every present.
- **antialiasing: CPU changes reach the GPU by dirty rectangle.** A lock or blit marks the
  rectangle it covers (`Buffer::gpuDirty`); only that part is uploaded and drawn into the
  multisampled surface, which also uses `SavedDeviceState` instead of a D3D9 state block.
  A HUD blit no longer copies the whole frame twice.
- **Direct3D 3-6 indexed draws light only the vertices the indices use.** The others get
  their position and clip code only (the clip status and extents stay as XP's transform
  of all of them made them), or are skipped where neither is kept.
- **Direct3D 3-6 pipeline:** clipping uses fixed arrays instead of vectors per triangle;
  the output vertex buffer grows geometrically instead of once per vertex; index and light
  lists reuse their storage; the eye vector is computed only for specular; the normal's
  length takes one square root instead of two; extents accumulate in locals.
- **Mouse: the game's image on the screen is a snapshot** (`GameImage`), so cursor
  functions and mouse messages neither wait for the DirectDraw lock nor ask Windows for
  the window's geometry each call. It's made again after a display-state change or a
  move of the game's window (and every 250 ms in case one went unseen); the window
  procedure never waits for it.

## 0.9.17 (review of 0.9.0-0.9.16)
Fixes from a line-by-line review (four reviewers, each finding checked against the code):
- **Direct3D 7: vertex buffers processed with `D3DVOP_CLIP` are drawn clipped.**
  ProcessVertices keeps the vertices outside the frustum in clip space, as XP does, and the
  draw sent them to the GPU as screen coordinates: triangles crossing a screen edge
  stretched to the corner. Now each primitive is clipped against the near plane in clip
  space and projected (`ExpandClipped`); d3d7test compares it with the direct draw.
- **Direct3D 3-6: a device outliving its DirectDraw object** no longer uses it after it is
  freed (the device keeps it, as XP's do through internal surfaces).
- **Textures given a new buffer by `SetSurfaceDesc`** (another size or format) are made
  again instead of being written past their end; a new buffer can't be taken for a freed
  one at the same address (its change count comes from one global counter).
- Direct3D 3-6: `CreateViewport`/`CreateMaterial` with a NULL pointer return
  `DDERR_INVALIDPARAMS` (one leaked a viewport, the other crashed); `SetRenderTarget` to a
  Z buffer doesn't leave the GPU context pointing at the old target; a surface outliving
  its DirectDraw object no longer makes a new Direct3D object on it; draws and execute
  buffers with absurd sizes fail instead of overflowing or throwing; an execute buffer's
  branch past its end stops instead of looping.
- Direct3D 7: `LightEnable` caps the index as `SetLight` does (a huge one threw);
  ProcessVertices blends min(the FVF's weights, VERTEXBLEND), draws send VERTEXBLEND no
  higher than the vertices' weights (D3D9 blended with weight 0), normals are lit through
  the inverse-transpose (non-uniform scale), and the destination format is checked as XP
  does (`D3DERR_INVALIDVERTEXFORMAT`).
- Z buffers: several depth fills before a draw all apply (a split screen's two halves);
  a new GPU depth buffer starts from the surface's own memory (zero-filled by DirectDraw, or
  the game's earlier fill), not as the driver leaves it; the
  emulated W buffer's scale starts again with each whole depth clear.
- GPU: before a Reset nothing of dd2d9's stays bound (native D3D9 refused the Reset, so
  every loss made a new device); a failed surface creation isn't retried (and logged) at
  every draw; CPU changes are marked sent only once they are; the slope-scaled overlay bias
  is sent again after a device change; palettized targets read back through their current
  palette.
- Windows: the topmost timer acts only while the game is in front; a deferred activation
  takes the real state when it runs (a stale "deactivated" could minimize a game in front)
  and is retried at most ~500 times; waiting for the display lock handles messages sent to
  the thread (a game with a render thread could deadlock); `WM_DISPLAYCHANGE` uses the
  DirectDraw object only under the lock; re-covering the screen gives up for real after 5
  tries in 2 s.
- `fpslimit`: windowed games are paced too (blits covering half the window), only after
  blits that succeeded, and never with the game's DirectDraw lock held.
- Compatibility: `cpus=N` takes one logical processor per physical core, core 0 last;
  `ram=`/`cpu=` also answer games that look the functions up with GetProcAddress, and keys
  opened with RegCreateKey(Ex) or through a copy of HKLM; a reused registry handle replaces
  the old entry. Import patching restores only slots still holding dd2d9's function, in
  loaded modules, writes them atomically, checks the import table lies in the image, and
  retries the module snapshot; `C:\WindowsGames\...` is no longer taken for Windows' folder.
- ini: a `[game.exe]` section overrides the general ones whichever file they are in (two
  passes); lines without `=` are logged; a comment may follow a tab.
- The log file is closed when the DLL is unloaded early (a reload truncated it under the
  open handle). MapWindowPoints' hook clears the last error when it returns 0.

Optimizations:
- Indexed draws send the GPU only the vertices their indices use (a few triangles from a
  big vertex buffer copied all of it, every draw).
- Waits (fpslimit, vblank pacing) use a high-resolution waitable timer, spinning only the
  last half millisecond instead of the last 2 ms on Sleep(0) (with `cpus=1` that took the
  game's own core).
- Direct3D 3-6: unchanged render states, stage states and textures return before the
  device check; Direct3D 7 re-sends the colour key's alpha test only when it changes; no
  heap allocation per bound texture per draw.

## 0.9.16 (0.9.15 had the timer on the window)
- **The exclusive window is made topmost again every 1.5 s** if something took that away,
  as `ddraw.dll`'s timer does (a hidden window stays hidden; not while the game is in the
  background, or with `DDSCL_NOWINDOWCHANGES`). A thread-pool timer, not one on the window:
  its ticks would go through the game's message loop. Wine's `test_window_style`
  "Expected WS_EX_TOPMOST" passes now (ddraw1/2/4/7).
- **A game resizing its own window in a scaled (borderless) mode, to a size that covers
  its screen, covers the real one again**: Jazz Jackrabbit 2 Plus sizes its window to
  661x544 now and then in its 640x480 mode (fullscreen on a real 640x480 display) and sat
  in a window until Alt+Enter or its next mode change. A smaller size stays a window.

## 0.9.14
- **`overlaybias` finds NFS III's light maps**: its trace showed every world draw alpha
  blended with depth writes on, so "blends" took in everything and the bias moved it all
  alike. An overlay is now a pass that doesn't write depth, or that adds or multiplies
  (DESTBLEND ONE or a colour factor, SRCBLEND ZERO or DESTCOLOR); ordinary alpha blending
  doesn't count. Overlays don't write depth while drawn: two lights laid on the same spot
  fought each other's depth as they fought the road's.
- `d3d7test`'s overlay check is NFS III's pattern: all blended, a road and two additive
  lights cut differently, all writing depth.

## 0.9.13
- `drawtrace=on` (diagnostic): every 600 scenes (about 10 s), one whole frame's draws are
  logged with their depth, blend and alpha-test states, texture, the depth bias dd2d9 put on
  them, and the range of z and rhw in their vertices.

## 0.9.12
- `overlaybias`: one 16-bit depth step whatever the game's Z buffer (NFS3+ asks for a 32-bit
  one, which got 16 steps of a 24-bit one: too little), plus a slope-scaled bias of one
  pixel's depth change (the mismatch grows with the depth gradient: roads at a grazing
  angle). `overlaybias=N` gives N steps, for games whose overlays need more.
- `filter=sharp`'s rewritten shader is taken by Windows' Direct3D 9 (Intel UHD 620).

## 0.9.11
- **No more hatched z-fighting of light maps and decals** (`overlaybias=on`, default): a
  pass laid on a surface already drawn (it doesn't write depth, or it blends) is pulled
  forward by one step of the depth precision the game asked for (a 16-bit step for a
  16-bit Z buffer, else 16 steps of a 24-bit one). The two differ only by rounding, which
  the 16-bit depth buffers of the era rounded the same way; 24-bit ones (`depth24`, most
  wrappers) show it as a fine hatch. NFS III's headlights and light maps on the road.
  (NFS III draws with Z buffering, not W: `wbuffer=force` hid the hatch but broke its depth
  order, the car drawn black.)
- `wbuffer=on` no longer makes 16-bit Z buffers 24-bit (only `force` does, and `depth24`).
- `filter=sharp`'s shader rewritten to stay clear of whatever Windows' ps_2_0 validator may
  refuse (Wine checks nothing): registers set whole before they are read, no negated
  constants, one constant per instruction. If it is refused still, the log says whether
  `frc` alone is.
- `d3d7test` checks an overlay on a floor cut differently, with Z buffering.

## 0.9.10
- The log names the Direct3D 9 adapter, its driver and pixel shader version when the
  device is made; when `filter=sharp`'s shader is refused, whether the device takes a
  pass-through shader at all. (On one Windows laptop the shader is refused with
  D3DERR_INVALIDCALL; the point-then-linear fallback is used there meanwhile.)
- The "game uses ZBIAS" line no longer fires for ZBIAS 0.

## 0.9.9
- **W buffering** (`wbuffer=on`, default): Direct3D 3-7 devices report
  `D3DPRASTERCAPS_WBUFFER` again, as most hardware of the era did, and `D3DZB_USEW` is
  emulated where the GPU lacks it (every current one): the depth of transformed vertices
  becomes 1 - k/w, linear across the screen over any flat surface, as a Z buffer
  interpolates it. Coplanar polygons cut differently then agree, where the game's own z
  (linear in w) fought: NFS III's headlights and light maps on the road, which every
  wrapper showed hatched. A pass that doesn't write depth (light maps, decals) gets a
  tolerance of 16 steps of a 24-bit depth buffer, which a 16-bit W buffer's coarser rounding
  gave for free. 16-bit Z buffers are made 24-bit with it. `wbuffer=force` treats Z buffering
  of transformed vertices the same way; `wbuffer=off` is the old behaviour. Untransformed
  vertices drawn by Direct3D 7 with W buffering still use the Z buffer.
- The log says when a game first draws with W buffering, and first uses ZBIAS.
- `d3d7test` checks a floor drawn twice, cut two ways (z-fighting or not).

## 0.9.8
- **`filter=sharp` on Windows**: Windows' Direct3D 9 refused its pixel shader (the texture
  read's coordinate register had components never written, which Wine doesn't check), and
  the frame was shown with plain linear filtering. Fixed; the log gives the error if a
  shader is refused again. Without pixel shaders, sharp now enlarges with point sampling by
  the whole factor at or above the scale, then linear down (no longer plain linear below 2x).
- **`antialiasing`: no lines at the edges of cut-out sprites** (NFS III's trees and corn):
  multisampling takes the texture coordinate at the pixel's centre, past the sprite's edge
  where it covers only part of the pixel, and wrapping read the other side of the texture
  there. A cut-out texture (colour-keyed or with alpha) drawn within its own coordinates
  (0 to 1, not tiled) is addressed clamped for that draw; tiled ones keep the game's.
- `d3d7test` checks a cut-out sprite's edge.

## 0.9.7
- **`fpslimit=N`**: frames paced to N per second at Flip and at blits covering half the
  primary or more; a frame later than a whole period starts the count again (no burst).
- **`render32=on`**: 16-bit render targets drawn in 32 bits on the GPU, converted when the
  CPU reads them. Read-back conversion to any RGB layout now goes through a table (the
  same rounding as before).
- **`depth24=on`**: 16-bit Z buffers made 24-bit on the GPU.
- **`antialiasing=2|4|8`**: multisampled render targets and Z buffers (32-bit and 24-bit,
  the formats every GPU multisamples; the largest sample count the GPU has, up to N).
  Direct3D draws into the multisampled surface, resolved before anything reads it (the
  CPU, the presenter, a texture copy). 2D the CPU writes between 3D draws is drawn back in
  as a quad, the game's device state kept around it in a state block; a full clear skips it.
- **`texfilter=linear`**: point-filtered textures drawn bilinear, cut-outs excepted.
- `d3d7test` checks faint additive passes (16- vs 32-bit precision), a slanted triangle
  edge (antialiased or not) and 2D written between 3D draws (kept).

## 0.9.6
- `mipmaps=on` leaves textures with an alpha channel alone too, as it did colour-keyed
  ones: their transparent texels blurred into the smaller levels and drew lines through
  NFS III's corn field in the distance.

## 0.9.5
- `anisotropy` leaves cut-out textures alone (colour-keyed, or with an alpha channel):
  its longer footprint reached past their edges and wrapped round, drawing thin vertical
  lines through NFS III's trees (as with other wrappers). Sprites are seen face-on, where
  anisotropic filtering changes nothing anyway.

## 0.9.4
- **`anisotropy=2|4|8|16`**: anisotropic filtering on every texture stage the game filters
  smoothly (linear or anisotropic minification), at most what the GPU can; point-filtered
  stages are left as they are. Off by default. The log says what the GPU allows.
- `d3d7test` checks one-pixel stripes squashed 8x vertically (blurred without anisotropic
  filtering, kept with it).

## 0.9.3
- **`filter=sharp` works at every scale**: it was plain bilinear below 2x (640x480 on a
  1366x768 or 1080p screen) and a two-pass approximation above. Now one pixel shader
  (sharp bilinear): each game pixel a flat block, blended with its neighbour only over the
  one screen pixel at its edge.
- **`mipmaps=on`**: textures the game gave no mipmaps get them from the GPU
  (`D3DUSAGE_AUTOGENMIPMAP`), and the mip filter is linear where the game asked for none:
  no shimmer in the distance. Colour-keyed, block-compressed and render-target textures are
  left alone. Off by default.
- **`alternatepixelcenter=on`**: Direct3D drawn half a pixel up and left, as old ATI and
  Nvidia drivers offered, for games with lines at the edges of textures. Transformed
  vertices are moved; the projection of untransformed ones is. Off by default.
- `d3d7test` checks a texture drawn 8x smaller (mipmapped or not) and 1:1 with bilinear
  filtering (where the pixel centres fall).
- After drawing through its shaders (gamma, sharp), the presenter sets the device's own
  back buffer as its render target again, so the device holds no surface of the presenter's.

## 0.9.2
- **Compatibility options** (`src/compat.cpp`), what the game is told about the machine:
  - `ram=1024` (default): `GlobalMemoryStatus(Ex)` reports 1 GB of memory (3/4 of it free
    at most) and a 1.5 GB commit limit. Windows tells programs that aren't
    large-address-aware 2 GB and a 4 GB page file, which old games overflow and then
    claim there isn't enough memory. `ram=0` gives the real values.
  - `vram=512` (default, as before): the video memory DirectDraw reports.
  - `cpu=2000` (default): the registry's `~MHz` for every processor reads 2 GHz
    (`RDTSC` can't be faked from a DLL; `cpu=0` gives the real clock).
  - `cpus=1` (default): the game runs on one processor (its affinity) and is told so
    (`GetSystemInfo`'s count and mask), as on the PCs of its era; `cpus=0` gives all.
  The imports are patched from `DllMain`, before the game's code runs, and again for each
  new DirectDraw object. The import patching is shared with the mouse translation
  (`src/hooks.cpp`).

## 0.9.1
- **The gamma ramp applies to Direct3D frames presented from the GPU** (0.8.3 showed them
  without it): the frame goes through a small pixel shader that looks each channel up in
  the ramp. Games with a brightness slider using `SetGammaRamp` look right again in 3D.
- **Render targets used as textures are copied on the GPU** while Direct3D holds their
  newest pixels, instead of being read back through system memory and uploaded again at
  every use (reflections, render-to-texture menus). Mipmapped or colour-keyed ones still go
  through the CPU copy.
- **D7VK support removed**: dd2d9's own Direct3D 3–7 replaces it. Gone: the Direct3D
  objects for a wrapper (`d3dproxy.cpp`), the presenter stepping aside for a wrapper's
  device, `install-dd2d9.sh --d7vk`. `d3d=on` now means the same as `auto`.
- With D7VK gone, two workarounds for references it kept go too, back to `ddraw.dll`'s
  rules (re/spec/ddraw-object.md): a second DirectDraw object gets
  `DDERR_EXCLUSIVEMODEALREADYSET` while the first holds exclusive mode in front (it takes
  over once the first lost activation, or its window is gone or hidden), and a new primary
  surface is refused with `DDERR_PRIMARYSURFACEALREADYEXISTS` while the old one exists, lost
  or not.
- **Faster texture and render-target uploads**: rows go to A8R8G8B8 through a converter
  built once per surface (`src/convert.cpp`): SSE2 for 565/555/1555/4444 and 32-bit RGB,
  colour key included, and a table for palettized ones. It is bit-exact with the old
  per-pixel path and about 7x faster (a 1024x768 565 texture: ~0.55 ms instead of ~4 ms).
- Built with `-msse2 -mfpmath=sse`: the Direct3D 3–6 transform, lighting and clipping
  code no longer runs on the x87 stack. (The presenter's 16-bit conversion stays a
  table: measured, SSE2 wasn't faster there.)
- `d3d7test` checks render to texture; `gamma` sets an inverting ramp.

## 0.9.0
**Direct3D 3–6 in dd2d9 itself** (`src/d3d6*.cpp`), matched to XP SP3's `d3dim.dll`
(`re/spec/d3d6-*.md`): DirectDraw objects made by `DirectDrawCreate` now offer
`IDirect3D`, `IDirect3D2` and `IDirect3D3` (and report 3D support), and their surfaces
`IDirect3DTexture`/`2` and Direct3D 1 devices.
- EnumDevices and FindDevice list Ramp, RGB and HAL (IDirect3D3: RGB and HAL) with XP's
  names and descriptors; the HAL's caps come from the Direct3D 9 adapter, the software
  devices' are XP's fixed tables (the DX3/5 RGB one without alpha blending).
- Devices v1 (made by a surface's QueryInterface, sharing its reference count), v2 and v3,
  with XP's creation checks, references, render-state defaults (version-dependent
  TEXTUREPERSPECTIVE, SPECULARENABLE, COLORKEYENABLE), light states with their override
  bits, texture stages, transforms, v1 matrix handles, viewports (XP's list order and
  reference rules), scenes (BeginScene restores lost surfaces for the game, as d3dim does),
  SetRenderTarget, GetCaps for all three descriptor sizes, EnumTextureFormats with XP's
  inclusion list, GetStats, clip status, Begin/Vertex/End and all the draws.
- The legacy render states reach Direct3D 9 the way Microsoft's own DX6 rasterizer took
  them: TEXTUREHANDLE (stage 0 only while set), TEXTUREMAPBLEND's table, TEXTUREMAG/MIN,
  TEXTUREADDRESS, WRAPU/V and the rest onto stage 0, last writer wins; Get still answers
  what the game set.
- Viewports with D3DVIEWPORT and D3DVIEWPORT2 semantics (the clip volume), TransformVertices,
  Clear and Clear2 (backgrounds from material handles, textured backgrounds), lights with
  both the D3DLIGHT (DX3/5) and D3DLIGHT2 (DX6) models, materials and their per-device
  handles, texture handles (one per surface, as XP's DP2 handles) with SwapTextureHandles
  and Load.
- Execute buffers and the whole instruction stream: PROCESSVERTICES (transform, light, copy),
  points, lines, triangles, state instructions with override bits, matrix load/multiply
  with live bindings, texture loads, branches, SETSTATUS, and the status written back.
- A CPU front end does every untransformed vertex, as XP's did (no hardware T&L for DX6):
  world, view, projection and the viewport's clip matrix, lighting, vertex fog, clip codes
  and status, clipping where primitives leave a guard band or cross the near or far plane.
  Direct3D 9 draws the result as transformed vertices.
- Deliberately safer than `d3dim.dll` where it dereferences blindly: unknown handles, no
  current viewport, bad vertex types, offsets past an execute buffer and NULL pointers give
  errors instead of crashes.
- The viewport's Clear is held to the viewport on version 1 devices too (Wine's test_clear:
  XP's drivers did it, AMD's didn't); `Clear(n, NULL)` clears nothing and succeeds (Wine's
  test_clear_rect_count). A hardware device draws untextured from a surface that isn't a
  texture (Wine's test_texture_wrong_caps). Matrix handles work on every device (Wine's
  test_multiple_devices).
- Surface QueryInterface: IDirectDrawColorControl is `E_NOINTERFACE`; other IIDs it doesn't
  know are `E_INVALIDARG` on DirectDraw 1-4 surfaces, as `d3dim.dll` answers them.
- Direct3D 9 is shared by several Direct3D devices: one taking it over sends all of its
  state again.
- **8-bit palettized render targets** for the software devices (Direct3D 3-6's RGB, Ramp; d3dim700's RGB never had them):
  drawn in true colour on the GPU, mapped back to palette indices when read (exact entry,
  else nearest). Direct3D 7's sysmem palettized target fails with E_FAIL as on XP.
  Palettized textures are listed by EnumTextureFormats on every device
  (converted on upload, as before), so they no longer land in video memory unlisted (Wine's
  test_unsupported_formats, test_rt_caps).
- Lit vertices get a specular alpha of 0, fog aside (Wine's test_lighting on Windows);
  colour keys act on the texel alone when the game's alpha isn't used (test_colorkey_precision).
- DX6 ComputeSphereVisibility answers in D3DVIS_* flags; legacy texture Load keeps a colour
  key only the destination has and refuses a palette into a palette-less destination.

## 0.8.3
- **Direct3D frames are presented straight from the GPU**: a flipped-in render target the
  GPU holds the newest copy of is stretched into the window as it is, instead of being
  read back into system memory at every Flip and converted again. Frames with DirectDraw
  or GDI drawing on top (the CPU copy is then the newest) still go through system memory.
  The gamma ramp isn't applied to frames presented this way yet.
- `d3d7test ... 3d` draws without the 2D on top (frames presented from the GPU).

## 0.8.2
- **Entering exclusive mode brings the game's window to the foreground** past Windows'
  foreground lock, as `ddraw.dll` does (FUN_7375d557: the lock timeout zeroed for the
  call, and a do-nothing mouse event so the process counts as the last to get input).
  Nerf ArenaBlast, started from Explorer, stayed behind it after its splash window
  closed: no keyboard or mouse input until Alt+Tab, and Windows' cursor showing in the
  middle of the screen.

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
