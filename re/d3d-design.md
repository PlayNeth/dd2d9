# dd2d9 Direct3D: design

Status (0.9.1): Direct3D 7 (`src/d3d7.cpp`) and 3-6 (`src/d3d6*.cpp`, a CPU front end as XP's)
done on `src/gpures.cpp`; frames presented from the GPU copy (gamma through a pixel shader);
render targets used as textures copied on the GPU; D7VK support removed. Cube maps since 1.1.0
(D3D9 cube textures, see the changelog).

Goal: Direct3D 3/5/6/7 (IDirect3D..IDirect3D7, devices, viewports, materials, lights, textures,
vertex buffers, execute buffers) implemented in dd2d9 on Direct3D 9, matched to XP SP3's
`d3dim700.dll` (DX7) and `d3dim.dll` (DX3-6) the way the 2D side is matched to `ddraw.dll`.
One path everywhere: Windows' own d3d9 on Windows, DXVK (or wined3d) on Linux. D7VK is no
longer needed; its support code (`d3dproxy.cpp`, the second-primary and exclusive-takeover
workarounds) goes once this works.

Reference binaries (XP SP3, saved in the scratchpad `xpd3d/`, decompiled to `d3d-decomp/`):
`d3dim700.dll` 5.3.2600.5512, `d3dim.dll` 5.1.2600.0, `d3dramp.dll` 5.1.2600.0.
How XP joins them to DirectDraw: `ddraw.dll` loads `d3dim700.dll` (IDirect3D7) or `d3dim.dll`
(IDirect3D..3) and calls `Direct3DCreate` when the DirectDraw object is asked for a Direct3D
interface (aggregated: the Direct3D object answers QueryInterface through DirectDraw);
textures are announced through `CreateTexture`/`D3DTextureUpdate`, flips through
`SurfaceFlipNotify`, palette changes through `PaletteUpdateNotify`. In dd2d9 these become
direct calls inside one DLL.

## 1. Surfaces get a GPU side

A surface's pixels live in its `Buffer` (Flip swaps buffers between the surfaces of a chain),
so the Direct3D 9 copy lives there too and travels with the pixels:

- `Buffer::gpu`: a D3D9 resource, created on first 3D use:
  - render target (back buffer, any surface created with DDSCAPS_3DDEVICE): a lockable-free
    `CreateRenderTarget` surface, or a texture with D3DUSAGE_RENDERTARGET when it is also a texture;
  - texture (DDSCAPS_TEXTURE): `D3DPOOL_DEFAULT` texture with its mip levels (members of the
    mipmap chain are the levels), uploaded through a `D3DPOOL_SYSTEMMEM` staging texture;
    cube maps as cube textures;
  - Z buffer (DDSCAPS_ZBUFFER): a depth-stencil surface in the closest D3D9 format
    (D16, D24X8, D24S8, D32 as available; D16_LOCKABLE / D32F_LOCKABLE when the game locks it).
- Two validity flags per buffer: `cpuValid`, `gpuValid`. A CPU write (Lock with write access,
  GDI ReleaseDC, a CPU Blt) clears `gpuValid`; 3D rendering or a GPU Blt clears `cpuValid`.
- Before the CPU reads or writes (Lock, GetDC, CPU Blt source/dest): if `!cpuValid`, read back
  (GetRenderTargetData for render targets; depth through a lockable format, see 6).
- Before the GPU uses it (draw with it bound as texture/target, GPU Blt, present): if
  `!gpuValid`, upload (dirty rectangle where known).
- Blt between two surfaces that both have a valid GPU side and supported formats/flags goes
  through `StretchRect`/a textured quad on the GPU; otherwise the existing CPU blitter, after
  syncing both to the CPU. So a 2D game never pays for any of this, and a 3D game that only
  renders and flips never reads back.

Palettized (P8) textures: D3D9 drivers and DXVK lack P8, so they are converted to A8R8G8B8 on
upload with the attached (or primary) palette; a palette change marks them for re-upload.
Colour-keyed textures (DDSD_CKSRCBLT, D3DRENDERSTATE_COLORKEYENABLE): the key becomes alpha 0
on upload (as DX7 HALs did) and alpha testing is enabled while COLORKEYENABLE is on.

## 2. One D3D9 device per DirectDraw object

Today the presenter thread creates its device lazily and Resets it whenever the window's
size changes; a Reset destroys every D3DPOOL_DEFAULT resource, which is fine for its one
staging surface but not for a game's textures and render targets. So:

- A `Gpu` object per DirectDraw object owns the IDirect3D9 and the IDirect3DDevice9, created
  when first needed (presenter or Direct3D), windowed, its focus window a hidden one of
  its own and its device window the game's, `D3DCREATE_MULTITHREADED`, and software vertex
  processing available (ProcessVertices; DXVK supports it). All device use goes through
  its lock.
- Presentation goes through the **device's implicit back buffer**, made as large as the
  largest monitor (current or registry mode) at creation, `D3DSWAPEFFECT_COPY`: the frame is
  drawn into its top left and shown with `IDirect3DDevice9::Present(source rectangle, NULL,
  game window, NULL)`. A window resize changes only the rectangle, never Resets the device,
  so the game's GPU resources survive; and overlay and capture tools that hook the device's
  `Present` (RivaTuner/Afterburner) see the frames (until 1.2.0 an **additional swap
  chain** on the game's window presented, which they didn't capture). Every frame also has
  a draw call (a triangle of no area, colour writes off): RivaTuner draws its overlay only
  into frames that had one, and a 2D game's frame is only copied in (`StretchRect`). A window
  larger than the back buffer still gets an additional swap chain. Windows' d3d9 and DXVK
  both take a destination window override and a source rectangle. (`present=exclusive`, a
  D3D9 exclusive-fullscreen device, can't work this way: it stays for 2D-only games, or
  goes; to decide.)
- 2D games: the presenter thread shows the primary as now, through that back buffer.
  3D games: the game's own Flip (or Blt to the primary) presents, on its thread, straight
  from the GPU copy of the back buffer (StretchRect to the back buffer, Present), with the
  presenter thread idle while a 3D device exists, as it is today behind D7VK. No readback,
  no cross-thread hand-off of render targets.
- Device loss (D3DERR_DEVICELOST: another program's mode change, the lock screen, UAC; and
  dd2d9's own mode switches): D3DPOOL_DEFAULT resources are gone. Surfaces whose only valid
  copy was on the GPU become lost (DDERR_SURFACELOST), as DirectDraw loses them at those
  moments anyway; the game restores and reloads them. The device is Reset once the loss
  ends (TestCooperativeLevel), and surfaces get new GPU resources on next use.

## 3. The interfaces

One implementation per object, interface versions as thin wrappers (as the 2D side does):

| Object | Interfaces | Notes |
| --- | --- | --- |
| Direct3D | IDirect3D, 2, 3, 7 | from DirectDraw's QueryInterface; EnumDevices (RGB, HAL, T&L HAL; Ramp for v1/2), FindDevice, CreateDevice, EnumZBufferFormats, CreateVertexBuffer, CreateLight/Material/Viewport (v1-3) |
| Device | IDirect3DDevice, 2, 3, 7 | states, transforms, lights, materials, clip planes, viewport, texture stages, DrawPrimitive families, BeginScene/EndScene, state blocks, Load, ValidateDevice, ComputeSphereVisibility, execute buffers (v1) |
| Texture | IDirect3DTexture, 2 | QueryInterface on a surface; GetHandle, Load, PaletteChanged |
| Viewport | IDirect3DViewport, 2, 3 | clear, transform (TransformVertices), lights, background material |
| Material, Light | IDirect3DMaterial 1-3, IDirect3DLight | handles for v1-3 |
| Vertex buffer | IDirect3DVertexBuffer, 7 | D3D9 vertex buffer; Lock/Unlock, ProcessVertices(Strided), Optimize |
| Execute buffer | IDirect3DExecuteBuffer | DX3/5: command stream parsed into draw calls |

State mapping is mostly one-to-one: DX7 render states → D3D9 render states (a few removed
in D3D9 — TEXTUREPERSPECTIVE, ANTIALIAS, edge AA, stipple, W-buffer... — handled or ignored
as XP's HAL did); DX7 texture stage states → D3D9 texture stage + sampler states (ADDRESS,
MAG/MIN/MIPFILTER, MAXMIPLEVEL, MIPMAPLODBIAS moved to samplers); legacy DX5 render states
(TEXTUREHANDLE, TEXTUREMAPBLEND, TEXTUREMAG/MIN) translated into stage 0 states as d3dim
does; lights/materials/transforms copy straight across.

## 4. Drawing

- DrawPrimitive / DrawIndexedPrimitive (user pointer) → D3D9 `*UP` calls with the FVF;
  Strided variants gathered into a temporary interleaved buffer; VB variants on D3D9 VBs.
- Pre-transformed vertices (D3DFVF_XYZRHW / D3DTLVERTEX) go through as-is; the viewport and
  guard band follow XP's clipping rules (spec to be written from d3dim700).
- DX5/6 `D3DVERTEX`/`D3DLVERTEX`/`D3DTLVERTEX` vertex types map to the matching FVFs.
- Execute buffers (DX3/5): their instruction stream (D3DOP_*) is decoded into the same calls;
  D3DOP_PROCESSVERTICES uses D3D9 ProcessVertices or a CPU transform.

## 5. Order of work

1. Specs from the decompile (`re/spec/d3d-*.md`), DX7 first: Direct3D7, Device7, VertexBuffer7.
2. Surfaces' GPU side, the shared device, readback/upload, GPU blits, presentation from GPU.
3. Device7: states, transforms, lights, DrawPrimitive family, textures, Clear, BeginScene/EndScene.
4. Test: Wine's ddraw7/d3d/visual tests (today ~240 Direct3D sub-tests skip), d3d7test, Nerf
   (Unreal), then the user's other 3D games.
5. Device3/2/1, viewports, materials, lights, execute buffers, ramp; Wine's ddraw1/2/4 tests.
6. Remove D7VK support (`d3dproxy.cpp`, the two workarounds in README "Differences").

## 6. Open questions (to settle from the decompile or by test)

- Depth buffer locking (games that read or write Z): D3D9 depth formats aren't lockable on
  most drivers; D16_LOCKABLE (and DXVK's support for it) decides how close this can get.
- Exact EnumDevices output (device GUIDs, caps) on XP with an emulated HAL: what games see.
- Whether to report a T&L HAL device (IID_IDirect3DTnLHalDevice): DX7 games pick it if offered.
- Software rasterizer devices (RGB, MMX, Ramp, Ref): XP offers them; here they would all be the
  same D3D9 device, reported with the software caps.
