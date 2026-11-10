# IDirect3D, IDirect3D2, IDirect3D3 and the surface hand-off (d3dim.dll)

Reference: `d3dim.dll` 5.1.2600.0 (`xpclient.010817-1148`, the version XP SP3 ships), Ghidra decompile with PDB
public names. `$D` = `$S/d3d-decomp/d3dim-named/` (`$S` = the session scratchpad), one `.c` per function, line
numbers are those of the file. Return codes the decompiler lost were read from the disassembly (`llvm-objdump` on
`$S/xpd3d/d3dim.dll`, saved as `$S/d3dim.dis`); those cite an address. ddraw.dll references are to
`$S/decomp-annotated/` (prefix DD:). Ghidra shifted the parameters of every `__stdcall` COM method by one (its
`param_1` is `this`); the text below uses the real parameter names. Static tables were dumped from the binary with
`$S/d3d6spec/tbl.py` (device table) and `$S/d3d6spec/desc.py` (D3DDEVICEDESC dumps).

The model and companion is `d3d7-direct3d.md` (d3dim700.dll, the DirectDraw 7 path); this file says where d3dim.dll
differs and does not repeat what is the same.

Out of scope: device state/drawing methods (IDirect3DDevice/2/3), execute buffers, viewports, materials, lights and
texture methods beyond their creation, the rasterizers.

## Objects

- **DIRECT3DI** (0x8C bytes, `Direct3DCreate_6dea7f3a.c:16`). One object carries all three interfaces
  (`DIRECT3DI_6dea7dd2.c:17-23`, offsets checked in the disassembly at 6dea7dda-6dea7e05):

  | Offset | Field |
  | --- | --- |
  | +0x00 / +0x04 / +0x08 | IDirect3D / IDirect3D2 / IDirect3D3 vtables (three distinct interface pointers) |
  | +0x0c / +0x10 | device count / device list head |
  | +0x14 / +0x18 | viewport count / list (`hookViewportToD3D_6deab997.c:10-19`) |
  | +0x1c / +0x20 | light count / list (`hookLightToD3D_6deb51e0.c:10-19`) |
  | +0x24 / +0x28 | material count / list (`hookMaterialToD3D_6deafc53.c:10-19`) |
  | +0x2c / +0x30 | vertex-buffer count / list (`hookVertexBufferToD3D_6dea9860.c:10-19`) |
  | +0x34 | **inner (non-delegating) IUnknown** `CDirect3DUnk`; +0x38 its refcount (1), +0x3c back pointer (`DIRECT3DI_6dea7dd2.c:23,60-61`) |
  | +0x40 | next viewport handle, starts at 1 (`DIRECT3DI_6dea7dd2.c:66`; `DIRECT3DVIEWPORTI_6deaca30.c:25-26`) |
  | +0x4c | TextureCacheManager (allocation failure leaves NULL silently: `DIRECT3DI_6dea7dd2.c:69-76`) |
  | +0x50 | pointer to a private copy (+0x58, 6 dwords) of the **IDirectDraw (v1) INT** |
  | +0x54 | pointer to a private copy (+0x70) of the **IDirectDraw4 INT** (NULL if that QI fails) |
  | +0x88 | outer IUnknown (the DirectDraw INT passed in, or +0x34 if none) |

- Construction (`DIRECT3DI_6dea7dd2.c:24-55`, disassembly 6dea7dd2-6dea7ec7): `dd->QueryInterface(IID_IDirectDraw)`,
  copy the INT, **release it**; then `dd->QueryInterface(IID_IDirectDraw4)`, copy, **release**. The Direct3D object
  holds **no reference** on any DirectDraw interface the game sees. (The copies are full INT structures; calls
  through them, e.g. CreateSurface for internal buffers, AddRef the *copy*, see "Reference counting".)
  Difference from d3dim700: two private INTs (v1 and v4) instead of one v1 copy plus the original INT.

## How ddraw reaches d3dim.dll

### Loading — DD:`FUN_7373d0b5_7373d0b5.c`, DD:`FUN_7373d0ea_7373d0ea.c`

1. Version: LCL `dwLocalFlags & DDRAWILCL_DIRECTDRAW7` (0x20000) → 0x700, else 0x600
   (DD:`FUN_7373d0b5_7373d0b5.c:9-15`). **0x600 loads `D3DIM.DLL`** (DD:`FUN_7373d0ea_7373d0ea.c:13-16`), once per
   DirectDraw object: `hD3DInstance` = LCL+0x38, `pD3DIUnknown` = LCL+0x3c. LoadLibrary failure → E_NOINTERFACE (19-21).
2. Exports fetched for version < 0x700 (23-45): Direct3DCreate, PaletteUpdateNotify, PaletteAssociateNotify,
   SurfaceFlipNotify, FlushD3DDevices, D3DTextureUpdate, **FlushD3DDevices2** (LCL+0x8c); CreateTexture, DestroyTexture,
   Set/GetPriority, Set/GetLOD, D3DBreakVBLock are set to NULL. d3dim.dll **does not export D3DTextureUpdate** (export
   table: D3DFree 1, D3DMalloc 2, D3DRealloc 3, Direct3DCreateDevice 4, Direct3DCreateTexture 5,
   Direct3DGetSWRastZPixFmts 6, Direct3DCreate 7, Direct3D_HALCleanUp 8, FlushD3DDevices2 9, FlushD3DDevices 10,
   PaletteAssociateNotify 11, PaletteUpdateNotify 12, SurfaceFlipNotify 13); ddraw calls it only for DirectDraw 7
   objects anyway (DD:`FUN_7375a555_7375a555.c:10-11`).
3. `Direct3DCreate(NULL, &pD3DIUnknown, ddInt)` (68). No Direct3DCreate export → E_NOINTERFACE (64-65); failure →
   `*out = NULL`, FreeLibrary, error returned (69-76).
4. Trigger points:
   - **IDirectDraw/2/4::QueryInterface** of an IID ddraw doesn't own (anything but IUnknown, IDirectDraw/2/4/7, the
     video/kernel containers): DD:`IDirectDraw7_QueryInterface.c:204-219`. `ddInt` = the `This` of that call.
     (Before it, ddraw calls `IsWow64Process` and returns E_NOINTERFACE under WOW64, 205-213: on 32-bit XP never.)
   - **IDirectDrawSurface/2/3/4::QueryInterface** (below), with `ddInt` = the surface's creating DirectDraw interface
     `lpSurfMore->lpDD_int` (DD:`IDirectDrawSurface7_QueryInterface.c:234-235`).
5. ddraw releases `pD3DIUnknown` (the inner IUnknown) when the DirectDraw LCL count reaches 0, **before** it destroys
   the object's surfaces, and clears `hD3DInstance` without FreeLibrary (DD:`IDirectDraw7_Release.c:94-99`).

### Direct3DCreate (export 7) — `Direct3DCreate_6dea7f3a.c`

`HRESULT Direct3DCreate(CRITICAL_SECTION *cs, IUnknown **out, IUnknown *dd)`:
1. `lpD3DCSect = cs` (9), DD thread lock (10), `*out = NULL` (11).
2. `dd == NULL` → DDERR_INVALIDPARAMS (12-13).
3. Allocation (0x8C) failure → E_OUTOFMEMORY (16-24).
4. Constructor with outer = dd (21). **The IDirectDraw QI failed (+0x50 == 0) → E_NOINTERFACE**, object deleted
   (26-28).
5. `*out` = the inner IUnknown (`this+0x34`, refcount 1), D3D_OK (31-32). There is no separate Initialize step
   (unlike d3dim700).

### Aggregation: QueryInterface / AddRef / Release

- **IDirect3D/2/3::QueryInterface** (`QueryInterface_6deb50f7.c`; the v2/v3 vtable slots are adjustor thunks to it,
  e.g. `QueryInterface\`adjustor{4}'_6dea7eca.c:10`): `ppv == NULL` → DDERR_INVALIDPARAMS (13-15); `*ppv = NULL`;
  forwarded to the outer, i.e. **DirectDraw's QueryInterface** (18-20). So IDirect3D2 → IDirectDraw2 works, and
  IDirect3D → IDirect3D3 goes DirectDraw → inner.
- **IDirect3D/2/3::AddRef / Release** (`AddRef_6deb5167.c:11-12`, `Release_6deb518a.c:11-12`): forwarded to the outer —
  **the DirectDraw INT that first triggered the load** (not necessarily the one the game QI'd) — and return its count
  (`IDirectDraw7_AddRef.c:27-34`: the INT's `dwIntRefCnt`).
- **Inner IUnknown** (`CDirect3DUnk`):
  - QueryInterface (`QueryInterface_6deb4f79.c`): NULL `ppv` → DDERR_INVALIDPARAMS (17-20); `*ppv = NULL` (21);
    | IID | Result |
    | --- | --- |
    | IID_IUnknown | the inner IUnknown itself (33-38) |
    | IID_IDirect3D | `DIRECT3DI+0` (44-52 → 35-38) |
    | IID_IDirect3D2 | `DIRECT3DI+4` (88-95) |
    | IID_IDirect3D3 | `DIRECT3DI+8` (64-86) |
    | anything else (IDirect3D7 included) | E_NOINTERFACE (75-77) |

    Every success calls `IDirect3D::AddRef`, i.e. **DirectDraw's AddRef on the outer INT** (35, 79, 88). The DirectDraw
    QI that forwarded here returns E_NOINTERFACE whenever the inner fails (DD:`IDirectDraw7_QueryInterface.c:219,79-84`:
    `*ppv` is NULL, so the inner error is not passed through).
  - AddRef: the vtable slot is folded with `CDirect3DVertexBuffer::AddRef` (+4 count, +1) (`AddRef_6dea9cb7.c:20`;
    vtable dump of 0x6dea179c). Release (`Release_6deb51ad.c:12-20`): 0 → `~DIRECT3DI` and delete.
- **~DIRECT3DI** (`~DIRECT3DI_6deb5070.c:13-24`) calls `Release` (vtable +8) on the head of the **viewport, light,
  device and material** lists, in that order, until each list is empty — it forcibly destroys everything still alive.
  Vertex buffers are not touched (their back pointer dangles). For a device made through a surface QI, its Release slot
  forwards to the surface (see below), so this loop releases the surface until the surface's destruction frees the
  device (reading; consequences not tested). Then the texture manager and bucket free-list are freed (25-34).
- **Consequence for games**: on a DirectDraw 1/2/4 object, IID_IDirect3D, IID_IDirect3D2 and IID_IDirect3D3 all work
  and return three different pointers to one object; IID_IDirect3D7 → E_NOINTERFACE. On a DirectDraw 7 object the
  opposite (d3dim700, see d3d7-direct3d.md).

## Surface hand-off: IDirectDrawSurface QueryInterface — DD:`IDirectDrawSurface7_QueryInterface.c`

After the IIDs ddraw owns (IUnknown, IDirectDrawSurface/2/3/4/7, gamma and colour control, kernel), for any other IID
(214-264):
1. **Per-surface cache**: `lpSurfMore->lpIUnknowns` is a list of {IID, IUnknown}; a matching entry → step 6 with that
   IUnknown (214-232).
2. **DirectDraw 7 object** (LCL DIRECTDRAW7) → E_NOINTERFACE (233).
3. d3dim.dll loaded if needed, outer = the surface's `lpDD_int`; a loader error is returned (234-235).
4. **`Direct3DCreateTexture(riid, This, &unk, This)`** (236-238): success → cached under `riid` (243-244, the helper
   `FUN_737646e1_737646e1.c:8-20` copies the IID); cache allocation failure → `unk->Release()`, E_OUTOFMEMORY
   (245-248). **E_NOINTERFACE → go on to 5; any other error is returned** (239-241).
5. **`Direct3DCreateDevice(riid, pD3DIUnknown, This, &unk, This, 1)`** (250-258): success → cached (256-257);
   E_NOINTERFACE → step 7; **any other error is returned** (259).
6. `unk->QueryInterface(riid, ppv)`: success → S_OK; failure → E_NOINTERFACE (261-264).
7. E_NOINTERFACE (289-290).

Because `Direct3DCreateDevice` rejects an unknown GUID with DDERR_INVALIDPARAMS (below), **any IID that is neither
ddraw's, a texture IID nor a device GUID returns E_INVALIDARG (0x80070057) from a DirectDraw 1-4 surface**, e.g.
IID_IDirect3D, IID_IDirect3DDevice2, IID_IDirectDrawPalette. Wine's `test_surface_qi` (ddraw2.c:2481-2507) expects
exactly this. A device GUID whose device can't be made returns that creation error from QueryInterface (e.g.
DDERR_INVALIDCAPS for a surface without DDSCAPS_3DDEVICE).

The cache holds the one reference on each inner IUnknown; it is released when the surface LCL is destroyed
(DD:`FUN_73733099_73733099.c:60,132` → DD:`FUN_73734033_73734033.c:9-19`). **One object per IID**: IDirect3DTexture
and IDirect3DTexture2 on the same surface are two different texture objects; each device GUID gives its own device.

### Direct3DCreateTexture (export 5) — `Direct3DCreateTexture_6dea7092.c`

`HRESULT Direct3DCreateTexture(REFIID riid, IDirectDrawSurface *surf, IUnknown **out, IUnknown *outer)`:
1. `*out = NULL` (24), DD lock (25).
2. `riid` not IID_IDirect3DTexture / IID_IDirect3DTexture2 → E_NOINTERFACE (26-52).
3. Allocation (0xD0) → E_OUTOFMEMORY (54-61). The constructor stores the outer (`outer`, or its own inner IUnknown
   when NULL) at +0xc4 (`DIRECT3DTEXTUREI_6dea7024.c:13-17`); inner refcount 1.
4. `surf->QueryInterface(IID_IDirectDrawSurface4)` → its error, object deleted (62-66); the v4 INT is copied into the
   object (+0x18) and **released**: the texture holds no surface reference (67-76). `surf` (the game's interface) is
   stored raw (82 or 88).
5. **No DDSCAPS_TEXTURE check**: any surface can be QI'd for a texture.
6. DDSCAPS2_TEXTUREMANAGE (`lpSurfMore->ddsCapsEx.dwCaps2 & 0x10`, 78): every level of the mipmap chain (walked with
   GetAttachedSurface(DDSCAPS_TEXTURE|DDSCAPS_MIPMAP) on the private v4 copy) gets `lpSurfMore+0x54` = the texture's
   "dirty" flag (+0xcc) (84-101); an error other than DDERR_NOTFOUND → object deleted, error returned (102). The dirty
   flag starts at 1 (106) and ddraw sets it to 1 whenever the surface's contents change
   (DD:`FUN_7375a555_7375a555.c:7-8`) — managed DirectX 6 textures are re-uploaded after Lock/Blt.
7. Palettized pixel format (DDPF_PALETTEINDEXED1/2/4/8, the surface's own format if DDRAWISURF_HASPIXELFORMAT, else the
   primary's) (107-113): `surf->GetPalette` → success sets a "has palette" byte and releases the palette; error other
   than DDERR_NOPALETTEATTACHED → object deleted, **that error is returned from the surface QI** (114-125).
8. `*out` = inner IUnknown (`+0xc`), D3D_OK (127-128).

Texture interfaces (`DIRECT3DTEXTUREI` +0 IDirect3DTexture, +4 IDirect3DTexture2):
- inner QI (`QueryInterface_6deb4cb5.c`): NULL → DDERR_INVALIDPARAMS (17-20); IUnknown → inner (34-37); IDirect3DTexture →
  +0, IDirect3DTexture2 → +4, each with an AddRef of the outer (40-77); else E_NOINTERFACE (65-67).
- public QI (`QueryInterface_6deb4dd5.c`): `This == NULL` → DDERR_INVALIDOBJECT (13-14); `ppv == NULL` →
  DDERR_INVALIDPARAMS (23); else **forwarded to the surface's QueryInterface** (17-21).
- AddRef/Release (`AddRef_6deb4e53.c`, `Release_6deb4ea9.c`): NULL `This` → 0; else **the surface's AddRef/Release** on
  the interface the texture was first queried through. A texture's count is the surface interface's count.

### Direct3DCreateDevice (export 4) — `Direct3DCreateDevice_6dea6324.c`

`HRESULT Direct3DCreateDevice(REFCLSID guid, IUnknown *d3dInner, IDirectDrawSurface *rt, IUnknown **out, IUnknown
*outer, DWORD version)`; version 1 from the surface QI (outer = the surface), 2 from IDirect3D2::CreateDevice, 3 from
IDirect3D3::CreateDevice (outer NULL in both):
1. DD lock. `IsValidD3DDeviceGuid` (`IsValidD3DDeviceGuid_6dea763e.c`: IsBadReadPtr(16) then one of Ramp, RGB, MMX,
   HAL, Ref, Null, **NewRGB** `15b29400-2abf-11d1-89aa-00a0c9054129` (internal); **T&L HAL is not valid here**) fails
   → **DDERR_INVALIDPARAMS** (34-39). Note `*out` is not cleared on this path.
2. `*out = NULL`; the DIRECT3DI = `d3dInner->backpointer` (41-42).
3. **`version >= 3` and the Ramp GUID → DDERR_INVALIDPARAMS** (44, 126-140).
4. MMX GUID on a CPU without MMX (`isMMXprocessor`: registry `HKLM\Software\Microsoft\Direct3D\DisableMMX` forces "no",
   `isMMXprocessor_6deb0471.c:21-30`) → DDERR_INVALIDPARAMS (49-57, 140).
5. Registry `HKLM\Software\Microsoft\Direct3D\DisableST` (58-67).
6. **One device class for everything**: `CDirect3DDeviceIDP2` (0x1E38 bytes) (71-77, 110-116); a HAL device on a driver
   whose hwCaps.dwDevCaps has D3DDEVCAPS_DRAWPRIMITIVES2EX (byte +0x11 bit 0x80 of `lpD3DGlobalDriverData`) gets
   +0xf58 = 3 instead of 2 (99-119). No class selection fails (unlike d3dim700's E_OUTOFMEMORY for missing DDIs).
   Allocation failure → E_OUTOFMEMORY (122).
7. Lost managed textures are evicted (81-84). `device->Init(guid, d3d, rt, outer, out, version)` (85); failure →
   device deleted, error returned (92-93).
8. Success: DisableST → device flag (87-89). **If the DirectDraw LCL has DDRAWILCL_FPUSETUP (dwLocalFlags 0x8000,
   i.e. the game passed DDSCL_FPUSETUP)** the x87 control word is set to 24-bit precision, round to nearest, all
   exceptions masked (@6dea64d4-6dea6501; the decompiler dropped this block). **Without DDSCL_FPUSETUP the FPU is left
   alone** — the reverse of d3dim700, which sets it unless DDSCL_FPUPRESERVE.
9. An exception → DDERR_INVALIDPARAMS (@6dea6513-6dea6520).

### Device Init — `Init_6deab45e.c` then `Init_6dea5b06.c`

1. HAL GUID → hardware flag (`Init_6deab45e.c:32-34`). A 16 KB command buffer is created as an execute-buffer surface
   through the **private IDirectDraw4 copy** (+0x54): DDSCAPS_EXECUTEBUFFER plus VIDEOMEMORY for hardware devices,
   SYSTEMMEMORY otherwise or on failure, then locked (`Init_6deab45e.c:35`, `GrowCommandBuffer_6deaa5f4.c:29-47`).
2. `DIRECT3DDEVICEI::Init` (`Init_6dea5b06.c`):
   1. `+4` = version (36); inner refcount = 1 (40). DDRAWILCL_MULTITHREADED → multithreaded flag (45-47).
   2. **Aggregated** (`outer != NULL`, i.e. the surface-QI device) → the outer is the surface; else the outer is the
      device's own inner IUnknown (48-54).
   3. RGB on an MMX CPU uses the MMX rasterizer (MMXAsRGB) unless registry `Drivers\UseMMXForRGB` = 0 (72-98); with the
      app-compat flag `dwAppHackFlags & 4` (DD LCL+0x48) the stored GUID becomes IID_IDirect3DMMXDevice (122-125).
   4. `rt->GetSurfaceDesc` (DDSURFACEDESC 0x6C) → its error (127-136).
   5. **Not DDSCAPS_3DDEVICE, or DDSCAPS_ZBUFFER → DDERR_INVALIDCAPS** (137-139).
   6. **Width or height > 2048 → DDERR_INVALIDOBJECT** (140-142).
   7. `rt->GetPalette`: error other than DDERR_NOPALETTEATTACHED → DDERR_INVALIDPARAMS; none and dwRGBBitCount < 16 →
      **DDERR_NOPALETTEATTACHED** (143-151). An aggregated device releases the palette at once (152-154).
   8. `rt->GetAttachedSurface(DDSCAPS_ZBUFFER)` (156-158): an error other than DDERR_NOTFOUND is returned (via the
      cleanup at 265-277); an aggregated device releases the Z surface at once (159-161). **Only a Z buffer attached to
      the render target counts**; none is fine.
   9. Device GUID stored at +0xfa4 (163-166). Provider: `GetSwHalProvider`, then (on E_NOINTERFACE) `GetHwHalProvider`
      (167-177); a failure is returned — **E_NOINTERFACE for Ref/Null without d3dref.dll and for HAL when the driver's
      `lpD3DHALCallbacks` fail `D3DI_isHALValid`** (`_GetSwHalProvider@12_6deb6cdd.c:155-178`,
      `_GetHwHalProvider@16_6deb13eb.c:20-25`). Ramp has a working provider in this DLL (RampRastHalProvider).
      Registry `DisableRendering` swaps in a test provider (178-184).
   10. `InitDeviceI` (`InitDeviceI_6dea56b9.c`): the device's HAL/HEL descs = `provider->GetCaps(gbl, hal, hel,
       version)` (21-23), **the same call EnumDevices makes, without fix-ups**, so IDirect3DDevice*::GetCaps matches
       EnumDevices of the same interface version (except dwSize). A hardware device (HAL desc dwFlags ≠ 0) clears
       DDBD_8 from the driver's `dwDeviceRenderBitDepth` **in place** (43-45) — later EnumDevices calls no longer report
       DDBD_8 for HAL. Driver callbacks failing `D3DI_isHALValid` → D3DERR_INITFAILED (46-48).
   11. `checkDeviceSurface(rt, z, guid)` (`checkDeviceSurface_6dea3ce5.c`), hardware = "HAL desc dwFlags ≠ 0" from the
       device's own GetCaps (49-54):
       - rt GetCaps / GetPixelFormat errors returned (25-38);
       - hardware and rt not DDSCAPS_VIDEOMEMORY → **D3DERR_SURFACENOTINVIDMEM** (55-57);
       - rt DDPF_RGB: depth not 8/16/24/32, or < 16 on Ref/Null, or in neither the HAL nor the HEL
         dwDeviceRenderBitDepth → **DDERR_INVALIDPIXELFORMAT** (58-96);
       - z (if any): GetPixelFormat/GetCaps errors returned (107-114); software device and z not SYSTEMMEMORY →
         **D3DERR_ZBUFF_NEEDS_SYSTEMMEMORY** (115-119); Ramp with a stencil Z (DDPF_STENCILBUFFER) →
         DDERR_INVALIDPARAMS (120-133); hardware and z not VIDEOMEMORY → **D3DERR_ZBUFF_NEEDS_VIDEOMEMORY** (136-138);
         DDPF_ZBUFFER with a depth not 8/16/24/32 → DDERR_INVALIDPIXELFORMAT (139-141).
       - A software device accepts a video-memory render target.
   12. `D3DFE_Create` (`D3DFE_Create_6dea834c.c`):
       - +0xfcc = `rt` as passed; **version 2: `rt->AddRef()`** (27-32);
       - `rt->QueryInterface(IID_IDirectDrawSurface4)` → its error; **kept for version 3, released at once for versions
         1 and 2** (33-39);
       - Z QI'd for IDirectDrawSurface4 and released at once (40-47);
       - HAL: a DirectDraw clipper is created through the private v1 INT (`CreateClipper`, vtable +0x10); its error is
         returned (84-90);
       - **render-target depth not in the provider's driver-data dwDeviceRenderBitDepth → DDERR_INVALIDOBJECT**
         (118-119, 160); driver context creation / transform / lighting init errors returned (129-151).
   13. Internal buffers: a 1 KB state array, a 64 KB TL vertex buffer (`CBufferVB::Grow`), aligned/DDS scratch buffers;
       any failure → E_OUTOFMEMORY (189-261).
   14. `hookDeviceToD3D` (218; `hookDeviceToD3D_6dea3f33.c:10-20`): linked at the head of the device list, back pointer
       +0xf74, device count +1. **No AddRef of the Direct3D object** (d3dim700 AddRefs it).
   15. `HookD3DDeviceToSurface` for the render target and the Z buffer (239-244; a device list in
       `lpSurfMore+0x44`, `HookD3DDeviceToSurface_6dea3fb9.c`). Only E_OUTOFMEMORY aborts; "already hooked"
       (DDERR_ALREADYINITIALIZED) is ignored.
   16. `stateInitialize` (245); `*out` = the inner IUnknown (+0xf98) (247).

### Device IUnknown — `QueryInterface_6deb021b.c`, `QueryInterface_6deb0396.c`

Interfaces: +0 IDirect3DDevice, +8 IDirect3DDevice2, +0xc IDirect3DDevice3 (`CDirect3DDeviceIDP2_6dea62d7.c:8-10`).
- Public QueryInterface/AddRef/Release on all three forward to the outer at +0x1504 (`QueryInterface_6deb0396.c:13-20`,
  `AddRef_6deb0406.c:11-12`, `Release_6deb0429.c`). NULL `ppv` → DDERR_INVALIDPARAMS.
- Inner QueryInterface (`QueryInterface_6deb021b.c`): NULL → DDERR_INVALIDPARAMS (20-23); IUnknown → inner (36-38);
  - **version 1 (surface-QI device)**: only the device's own GUID (+0xfa4) → `+0` (IDirect3DDevice) (43-60); nothing else.
  - **version ≥ 2**: IID_IDirect3DDevice → +0, IID_IDirect3DDevice2 → +8, IID_IDirect3DDevice3 → +0xc **only if
    version ≥ 3** (61-121); E_NOINTERFACE otherwise (110). Each success **overwrites the stored device GUID at +0xfa4
    with the IID just asked for** (122-126) — CreateDevice's own QI does this immediately, so on a v2/v3 device that
    field never holds the device GUID afterwards (consumers of +0xfa4 are `D3DFE_Create` and `stateInitialize`, both
    run before; whether anything later depends on it is not determined).
- Inner Release (`Release_6deb0363.c:12-20`): 0 → scalar deleting destructor → `DestroyDevice`.

Game-visible consequences of the aggregation:
- **IDirect3DDevice from a surface QI shares the surface's refcount** (its AddRef/Release are the surface interface's)
  and its QueryInterface is the surface's. So `device->QueryInterface(IID_IDirect3DTexture)` returns a texture,
  `device->QueryInterface(IID_IDirectDrawSurface)` the surface, and `device->QueryInterface(IID_IDirect3DDevice)`
  returns **E_INVALIDARG** (surface QI → `Direct3DCreateDevice` rejects that IID as a device GUID).
- The surface-QI device lives until the surface is destroyed (the cache's reference).

### DestroyDevice — `DestroyDevice_6dea5889.c`

Deletes the viewports still attached (IDirect3DDevice3::DeleteViewport each, 24-27), releases the 8 bound stage
textures (28-39), frees texture and material handles (40-69), releases the device's execute buffers (63-66), unhooks
from the surfaces (83-88) and from the Direct3D object (94-96), releases the provider and frees its module (119-125).
**Only for a non-aggregated device** (render target ≠ outer, 73-81, 126-139): releases the render target (the passed
interface for version 2, the IDirectDrawSurface4 for version 3), the Z surface and the palette.

## IDirect3D2::CreateDevice — `CreateDevice_6deb14a7.c` (and @6deb14a7-6deb1536)

`CreateDevice(REFCLSID rclsid, IDirectDrawSurface *lpDDS, IDirect3DDevice2 **lplpD3DDevice2)`; no DD lock of its own.
1. Object pointer (`This - 4`) NULL → DDERR_INVALIDOBJECT (17; @6deb14bb-6deb14ce).
2. `lplpD3DDevice2 == NULL` → DDERR_INVALIDPARAMS (18-19).
3. `lpDDS == NULL` → **DDERR_INVALIDOBJECT** (21, 38).
4. `*lplpD3DDevice2 = NULL` (23); `Direct3DCreateDevice(rclsid, inner, lpDDS, &unk, NULL, 2)` (24-26); its error
   returned (27-32).
5. `unk->QueryInterface(IID_IDirect3DDevice2, lplpD3DDevice2)`, `unk->Release()` (33-35; out pointer from @6deb1502):
   the game gets refcount 1. An exception → DDERR_INVALIDPARAMS (@6deb152c).

The surface is used as given (no QI): IDirect3D2 is meant to receive an IDirectDrawSurface (v1) pointer, but any
version's INT works since the methods used (GetSurfaceDesc, GetPalette, GetAttachedSurface, GetCaps, GetPixelFormat)
sit at the same vtable slots in v1-v3. GetAttachedSurface returns the Z buffer's INT of that same version.

## IDirect3D3::CreateDevice — `CreateDevice_6deb1b9a.c`

`CreateDevice(REFCLSID rclsid, IDirectDrawSurface4 *lpDDS, IDirect3DDevice3 **lplpD3DDevice3, IUnknown *pUnkOuter)`:
1. DD lock. Object pointer NULL (`This - 8`) → DDERR_INVALIDOBJECT (26-28).
2. `lplpD3DDevice3 == NULL` → DDERR_INVALIDPARAMS (30-31).
3. `lpDDS == NULL` → DDERR_INVALIDOBJECT (34, 57-59).
4. `pUnkOuter != NULL` → **CLASS_E_NOAGGREGATION** (0x80040110) (36, 53).
5. `*lplpD3DDevice3 = NULL` (37); `lpDDS->QueryInterface(IID_IDirectDrawSurface)` → its error; the v1 pointer is
   released at once (38-40).
6. `Direct3DCreateDevice(rclsid, inner, surfV1, &unk, NULL, 3)` → its error (41-46).
7. `unk->QueryInterface(IID_IDirect3DDevice3, lplpD3DDevice3)`, `unk->Release()` (48-49).

### CreateDevice: full check order (both versions)

1. NULL out → INVALIDPARAMS; NULL surface → INVALIDOBJECT; (v3) outer → CLASS_E_NOAGGREGATION; (v3) surface QI error.
2. GUID not Ramp/RGB/MMX/HAL/Ref/Null/NewRGB → INVALIDPARAMS (**T&L HAL included**); (v3) Ramp → INVALIDPARAMS; MMX on
   a non-MMX CPU → INVALIDPARAMS.
3. GetSurfaceDesc error; not 3DDEVICE or is ZBUFFER → INVALIDCAPS; > 2048 → INVALIDOBJECT; palette error →
   INVALIDPARAMS; < 16 bpp without palette → NOPALETTEATTACHED; GetAttachedSurface error (≠ NOTFOUND).
4. Provider: Ref/Null without d3dref.dll, HAL without a valid driver → E_NOINTERFACE.
5. InitDeviceI → INITFAILED; checkDeviceSurface → SURFACENOTINVIDMEM, INVALIDPIXELFORMAT, ZBUFF_NEEDS_SYSTEMMEMORY,
   INVALIDPARAMS (Ramp + stencil), ZBUFF_NEEDS_VIDEOMEMORY, INVALIDPIXELFORMAT (Z).
6. D3DFE_Create: surface QI errors, clipper error, depth not in driver render depths → INVALIDOBJECT, context errors.
7. Allocations → E_OUTOFMEMORY.

### What a device holds (references)

| Device made by | Render target | Z buffer | Palette | Direct3D / DirectDraw |
| --- | --- | --- | --- | --- |
| surface QI (v1, aggregated) | nothing (raw pointer) | nothing | nothing | nothing visible |
| IDirect3D2::CreateDevice | +1 on the passed interface (`D3DFE_Create:30-31`) | +1 on the Z INT of the same version (`Init:156-162`) | +1 (`Init:143,155`) | nothing visible |
| IDirect3D3::CreateDevice | +1 on the render target's IDirectDrawSurface4 INT (`D3DFE_Create:33-39`) | +1 on the Z's v1 INT | +1 | nothing visible |

"Nothing visible": the device does not AddRef the Direct3D object (`hookDeviceToD3D_6dea3f33.c:10-20`), but its internal
execute-buffer surfaces are created through the private IDirectDraw4 copy, and DirectDraw 4/7 CreateSurface AddRefs
the interface it was called on (DD:`FUN_73736f21_73736f21.c:60-64`). AddRef on the copy raises the copy's own
`dwIntRefCnt` and the DirectDraw LCL/GBL counts (DD:`IDirectDraw7_AddRef.c:27-31`): **the DirectDraw object is kept
alive, but no count a game can read changes** (IDirect3D2/IDirectDraw AddRef/Release return the game INT's count).
d3dim700 differs (its internal surfaces AddRef the game's INT).

## IDirect3D::Initialize

Vtable slot 3 of IDirect3D is a folded function that **always returns DDERR_ALREADYINITIALIZED** (0x88760005)
(`Initialize_6deb520d.c:7`; @6deb520d; vtable dump at 0x6dea18dc). No parameter checks.

## EnumDevices (all versions) — `EnumDevices_6dea724d.c`, `_6dea7268.c`, `_6dea7283.c`, worker `EnumDevices_6dea76e5.c`

The three interface methods call the worker with (descriptor size, version) (@6dea724d-6dea729b):

| Interface | Size passed (= dwSize of both descs) | Version |
| --- | --- | --- |
| IDirect3D | 0xAC (D3DDEVICEDESC_V1) | 1 |
| IDirect3D2 | 0xCC (D3DDEVICEDESC_V2) | 2 |
| IDirect3D3 | 0xFC (D3DDEVICEDESC_V3 = D3DDEVICEDESC) | 3 |

Worker:
1. DD lock. `IsBadCodePtr(callback)` (NULL included) → DDERR_INVALIDPARAMS (51-52, 198-201). No check of `This`.
2. Registry `HKLM\Software\Microsoft\Direct3D\Drivers` (58-91), REG_DWORD values `SoftwareOnly`, `EnumReference`,
   `EnumSeparateMMX` (read for version ≥ 3 only), `EnumNullDevice`. **If the key opens and version < 3, "list MMX" is
   set to 1** (73-74); if the key does not exist, it stays 0 (see Not determined).
3. Walks the static table at 0x6deff364 (92-94), stopping at its end or **as soon as the callback returns anything
   other than 1** (97, 184). Table (dumped; entry stride 0x308, name at +4, description at +0x104):

   | # | GUID | lpDeviceName | lpDeviceDescription |
   | --- | --- | --- | --- |
   | 1 | IID_IDirect3DRampDevice `f2086b20-259f-11cf-a31a-00aa00b93356` | `Ramp Emulation` | `Microsoft Direct3D Mono(Ramp) Software Emulation` |
   | 2 | IID_IDirect3DRGBDevice `a4665c60-2673-11cf-a31a-00aa00b93356` | `RGB Emulation` | `Microsoft Direct3D RGB Software Emulation` |
   | 3 | IID_IDirect3DHALDevice `84e63de0-46aa-11cf-816f-0000c020156e` | `Direct3D HAL` | `Microsoft Direct3D Hardware acceleration through Direct3D HAL` |
   | 4 | IID_IDirect3DMMXDevice `881949a1-d6f3-11d0-89ab-00a0c9054129` | `MMX Emulation` | `Microsoft Direct3D MMX Software Emulation` |
   | 5 | IID_IDirect3DRefDevice `50936643-13e9-11d1-89aa-00a0c9054129` | `Reference Rasterizer` | `Microsoft Reference Rasterizer` |
   | 6 | IID_IDirect3DNullDevice `8767df22-bacc-11d1-8969-00a0c90629a8` | `Null device` | `Microsoft Null Device` |

   Callback: `cb(lpGUID, lpDeviceDescription, lpDeviceName, &halDesc, &helDesc, ctx)` (184-186); the GUID pointer
   points into the DLL's GUID constants, the strings into the (writable) table, the descs to stack buffers.
4. Per entry, skipped when:
   - MMX: version < 2 or no MMX CPU (102-114); or "list MMX" = 0 (117-130).
   - Ref: unless EnumReference = 1, or = 2 with version ≥ 3 (131-142).
   - Null: unless EnumNullDevice ≠ 0 (143-156).
   - **Ramp: version ≥ 3** (157-170).
   - Provider: `GetSwHalProvider`, then, if it returned E_NOINTERFACE and SoftwareOnly is off, `GetHwHalProvider`
     (171-175); any failure → silently skipped. So HAL is listed only when the driver's `lpD3DHALCallbacks` pass
     `D3DI_isHALValid`; Ref/Null only with d3dref.dll (`LoadReferenceDeviceProc_6deb6973.c:10-14`).
5. `provider->GetCaps(gbl, &hal, &hel, version)` (176), provider released and module freed (177-180); success → both
   descs' dwSize = the version's size (182-183), callback. A GetCaps failure skips the entry. **No fix-ups** (d3dim700
   patches lights/clip planes/blend matrices; d3dim.dll reports the provider values as they are).
6. D3D_OK (195-196).

**Resulting list with the default registry and an MMX CPU:**

| Interface | Devices, in order |
| --- | --- |
| IDirect3D | Ramp, RGB, HAL |
| IDirect3D2 | Ramp, RGB, HAL, MMX (only if the `Drivers` key exists; see Not determined) |
| IDirect3D3 | RGB, HAL |

(HAL only with a Direct3D-capable driver.) No T&L HAL in any version; T&L HAL is IDirect3D7-only.

### The descriptors

- **HAL** (`HwHalProvider::GetCaps`, `GetCaps_6deb1282.c`):
  - HAL desc = `D3DDeviceDescConvert(hal, lpD3DGlobalDriverData->hwCaps, lpD3DExtendedCaps)` (17-19). Convert
    (`D3DDeviceDescConvert_6dea5560.c`): copies the driver's 0xAC-byte D3DDEVICEDESC_V1 verbatim (13-20), then, with
    extended caps, dwSize = 0xFC and min/max texture size, stipple sizes, repeat, aspect, anisotropy, guard band,
    extents adjust, stencil, FVF, texture-op caps, blend stages, simultaneous textures from them (21-44). **Without
    extended caps the v2/v3 fields are whatever the caller's buffer held** (uninitialised stack in EnumDevices).
  - dwMaxVertexCount 0 → **0x400** (20-23). **D3DDEVCAPS_HWRASTERIZATION is not added** (d3dim700 adds it).
  - **Version ≥ 3 only**, pre-DX6 driver (no `lpD3DHALCallbacks3` or no DrawPrimitives2 in it: GBL+0x638, +0x14):
    synthesized multitexture caps in the HAL desc, as in d3dim700 (33-65): tri dwRasterCaps |= WFOG; dwMaxAnisotropy 1;
    1 blend stage, 1 texture; dwTextureOpCaps DISABLE, + SELECTARG1 (DECAL/COPY), MODULATE (MODULATE/MODULATEALPHA), ADD,
    BLENDTEXTUREALPHA (DECALALPHA); tri MIN/MAG/MIP filter bits from the legacy ones.
  - HEL desc = static desc at 0x6deff398 (24-30) with the HAL's dwMaxBufferSize and dwMaxVertexCount (31-32):
    dwFlags 0x1F, dcmColorModel 0, dwDevCaps D3DDEVCAPS_FLOATTLVERTEX, dtcTransformCaps {8, D3DTRANSFORMCAPS_CLIP},
    bClipping TRUE, dlcLightingCaps {0x10, 0xF, D3DLIGHTINGMODEL_RGB, 0}, primitive caps empty (dwSize 0x38), rest 0.
  - Hardware device creation clears DDBD_8 from the driver's render depths (see Init 10).
- **RGB and MMX** (`OptRastHalProvider::GetCaps`, `GetCaps_6deb6bb2.c`; the RGB, MMX, NewRGB and MMXAsRGB provider
  objects share this vtable): HAL desc = `g_nullDevDesc` (0x6df02ee0: all zero except the dwSize fields 0xFC / 8 /
  0x10 / 0x38 / 0x38) (14-19). HEL desc: **version ≥ 3** → a desc built once (`FUN_6deb69bb_6deb69bb.c:11-76` then
  Convert with the extended caps it wrote, 20-25); **version 1/2** → the static desc at 0x6df02fe0 (28-30).
- **Ramp** (`RampRastHalProvider::GetCaps`, `GetCaps_6deb9d07.c`): HAL desc = g_nullDevDesc; HEL desc = static desc at
  0x6df03438 for every version (the device is only listed for versions 1-2).

Software HEL descs (as the game sees them; dwSize replaced by the version's size; the v1/v2 callbacks only "own"
the first 0xAC / 0xCC bytes):

| Field | RGB/MMX, IDirect3D3 | RGB/MMX, IDirect3D/2 | Ramp |
| --- | --- | --- | --- |
| dwFlags | 0x7FF | 0x7DF (no D3DDD_LINECAPS) | 0x7DF |
| dcmColorModel | D3DCOLOR_RGB (2) | 2 | D3DCOLOR_MONO (1) |
| dwDevCaps | 0x551 (FLOATTLVERTEX, EXECUTESYSTEMMEMORY, TLVERTEXSYSTEMMEMORY, TEXTURESYSTEMMEMORY, DRAWPRIMTLVERTEX) | 0x55B (+ SORTINCREASINGZ, SORTEXACT) | 0x55B |
| dtcTransformCaps / bClipping | {8, CLIP} / TRUE | same | same |
| dlcLightingCaps | {0x10, 0xF, RGB, 0} | {0x10, 0xF, RGB, 0} | {0x10, 0xF, MONO (2), 0} |
| line = tri caps: Misc | 0x70 | 0x70 | 0x70 |
| Raster | 0x121F7 | 0x1D1 | 0x51 |
| ZCmp | 0xFF | 0xFF | 0xFF |
| SrcBlend / DestBlend / AlphaCmp | 0x1FFF / 0x7FF / 0xFF | 0 / 0 / 0 | 0 / 0 / 0 |
| Shade | 0xC528A | 0xCA28A | 0xA145 |
| Texture | 0xDF | 0xB | 0xB |
| TextureFilter | 0x0303033F | 0x1F | 0x5 |
| TextureBlend | 0xCF | 0x42 | 0x42 |
| TextureAddress | 0x1F | 0x1 | 0x1 |
| Stipple w/h | 4 / 4 | 4 / 4 | 4 / 4 |
| dwDeviceRenderBitDepth / ZBufferBitDepth | 0xF00 / 0x400 | 0xF00 / 0x400 | 0xF00 / 0x400 |
| dwMaxBufferSize / dwMaxVertexCount | 0 / 2021 | 0 / 2021 | 0 / 2021 |
| min / max texture (v2+ fields) | 1×1 / 1024×1024 | 0 | 0 |
| stipple min/max | 0 | 0 | 0 |
| dwMaxTextureRepeat, AspectRatio, Anisotropy | 256, 0, 1 | 0 | 0 |
| guard band L/T/R/B, dvExtentsAdjust | −32768, −32768, 32767, 32767, 0 | 0 | 0 |
| dwStencilCaps, dwFVFCaps, dwTextureOpCaps | 0xFF, 2, 0x78FF | 0 | 0 |
| wMaxTextureBlendStages / wMaxSimultaneousTextures | 2 / 2 | 0 | 0 |

(RGB v3: `FUN_6deb69bb_6deb69bb.c` writes + `D3DDeviceDescConvert_6dea5560.c:21-44`; others: static .data dumps with
`desc.py`; nothing in the decompile writes those statics.) Note the v1/v2 RGB desc reports **no alpha-blend caps**.

## FindDevice (all versions) — `FindDevice_6dea7478.c`/`_748e`/`_74a4` (version 1/2/3, @6dea7478-6dea74b7), `FindDevice_6dea79f0.c`, `enumFunc_6dea736d.c`

1. DD lock. `lpD3DFDS == NULL`, its dwSize ≠ 0x5C, `lpD3DFDR == NULL`, or `lpD3DFDR->dwSize` not one of **0x16C, 0x1AC,
   0x20C** → DDERR_INVALIDPARAMS (37-42). Any of the three result sizes is accepted on any interface version.
2. The search is copied into a context; that interface version's EnumDevices runs with `enumFunc` (50-71), i.e. with
   that version's device list and desc size.
3. `enumFunc` (always returns 1, so every device is seen): hardware = HAL desc dcmColorModel ≠ 0 (16). Rejects on
   D3DFDS_GUID mismatch (18-33); D3DFDS_HARDWARE with bHardware ≠ hardware (34-36); D3DFDS_COLORMODEL when neither
   desc's dcmColorModel shares a bit with the search's (37-40); D3DFDS_TRIANGLES / D3DFDS_LINES when neither the HAL
   nor the HEL tri (line) caps satisfy every requested D3DFDS_*CAPS field, each tested as `(dev & search) == search`
   (41-52; field map in `FUN_6dea729e`, @6dea729e-6dea735b: MISC 0x20, RASTER 0x40, ZCMP 0x80, ALPHACMP 0x100,
   SRCBLEND 0x200, DSTBLEND 0x400, SHADE 0x800, TEXTURE 0x1000, TEXTUREFILTER 0x2000, TEXTUREBLEND 0x4000,
   TEXTUREADDRESS 0x8000).
   Selection (53-79): **a hardware match always replaces the result; a software match is taken only while no
   hardware device has matched**. Result: the **last** matching hardware device, else the **last** matching software
   device (with the default list: HAL, else MMX/RGB).
4. Nothing matched → **DDERR_NOTFOUND** (72-74).
5. Copy-out (76-120): 0x20C → the whole result; 0x16C/0x1AC → dwSize, GUID, then each desc truncated to
   `(dwSize − 0x14) / 2` bytes, packed back to back. The descs' own dwSize stays the enumerating version's.
6. D3D_OK.

## CreateLight / CreateMaterial / CreateViewport

| Method | Code | Behaviour |
| --- | --- | --- |
| IDirect3D/2/3::CreateLight(lplpLight, pUnkOuter) | `CreateLight_6deb5691.c` (v2/v3 via adjustors) | pUnkOuter ≠ NULL → CLASS_E_NOAGGREGATION (12, 46, before the lock); object NULL → INVALIDOBJECT (14-15); `lplpLight == NULL` → INVALIDPARAMS (17-18); `*lplpLight = NULL`; 0x128-byte light, refcount 1 (`DIRECT3DLIGHTI_6deb5667.c:22`); allocation → E_OUTOFMEMORY (29-30); linked into the Direct3D object's light list (+0x20, count +0x1c), back pointer, **no AddRef of Direct3D** (33; `hookLightToD3D_6deb51e0.c`). One interface (IDirect3DLight) for all versions. |
| IDirect3D3::CreateMaterial(lplpMat3, pUnkOuter) | `CreateMaterial_6deb018e.c` | pUnkOuter → CLASS_E_NOAGGREGATION (14, 45); object NULL → INVALIDOBJECT (16-17); NULL out → INVALIDPARAMS (19-20); `*out = NULL`; 0x74 bytes, refcount 1, interfaces +0 IDirect3DMaterial, +4 IDirect3DMaterial2, +8 IDirect3DMaterial3 (`DIRECT3DMATERIALI_6deb00ee.c`); allocation → E_OUTOFMEMORY (29); list +0x28 / count +0x24, no AddRef of Direct3D; `*out` = +8. |
| IDirect3D/2::CreateMaterial(lplpMat, pUnkOuter) | `CreateMaterial_6deafd88.c`, `_6deafdb9.c` (@6deafd88-6deafdb6) | Call IDirect3D3::CreateMaterial(&tmp, pUnkOuter) on this object; **on success only**, `*lplpMat` = material+0 (v1) / +4 (v2). Errors as above, except that **a NULL `lplpMat` is not checked** (written on success: access violation). |
| IDirect3D3::CreateViewport(lplpVP3, pUnkOuter) | `CreateViewport_6deaca83.c` | pUnkOuter → CLASS_E_NOAGGREGATION (14, 46); object NULL → INVALIDOBJECT (16-17); NULL out → INVALIDPARAMS (19-20); `*out = NULL`; 0x7C bytes, refcount 1, **handle = the Direct3D object's counter (from 1, +1 per viewport)** (`DIRECT3DVIEWPORTI_6deaca30.c:17,25-26`); allocation → E_OUTOFMEMORY (30); list +0x18 / count +0x14, no AddRef. One object pointer answers IDirect3DViewport/2/3. |
| IDirect3D/2::CreateViewport(lplpVP, pUnkOuter) | `CreateViewport_6deaba65.c`, `_6deaba7b.c` | Forward to IDirect3D3::CreateViewport(lplpVP, **NULL**): **pUnkOuter is ignored** (@6deaba69, @6deaba7f); same pointer returned. |

"Object NULL" is the `DIRECT3DI*` derived from `This` (only reachable with a NULL IDirect3D pointer, or This = 4 / 8 on
IDirect3D2 / 3); a NULL IDirect3D2/3 pointer faults in the thunk instead.

## IDirect3D3::EnumZBufferFormats — `EnumZBufferFormats_6dea7b81.c`

1. DD lock. Object NULL → DDERR_INVALIDOBJECT (36-41); `IsBadCodePtr(callback)` → DDERR_INVALIDPARAMS (43-48);
   invalid device GUID (`IsValidD3DDeviceGuid`, so **T&L HAL → INVALIDPARAMS**) → DDERR_INVALIDPARAMS (49-54).
2. HAL (56-118): no `lpD3DGlobalDriverData` → **DDERR_NOZBUFFERHW** (69-72); the driver's own list (GBL+0x63c count,
   +0x640 formats) as is (110-116); else one format per bit of hwCaps.dwDeviceZBufferBitDepth **in the order 8, 16,
   24, 32**: dwSize 32, DDPF_ZBUFFER, dwZBufferBitDepth = bits, dwZBitMask 0xFF/0xFFFF/0xFFFFFF/0xFFFFFFFF (76-107); no
   bits → DDERR_NOZBUFFERHW (78). (Other fields rely on the allocator zeroing, as in d3dim700's LMEM_ZEROINIT
   `_MemAlloc` — not re-checked here.)
3. Every other GUID (`_GetSwZBufferFormats@8_6deb6e9a.c`):
   - RGB, MMX, NewRGB → two formats (`ZBufferFormats_6deb6893.c:86-102`): DDPF_ZBUFFER 16-bit, Z mask 0xFFFF; and
     DDPF_ZBUFFER|DDPF_STENCILBUFFER, dwZBufferBitDepth 32, dwStencilBitDepth 8, Z mask 0xFFFFFF00, stencil mask 0xFF;
   - **Ramp → one format: DDPF_ZBUFFER 16-bit, mask 0xFFFF** (`RampZBufferFormats_6deb9d76.c:8-15`) — reachable only through IDirect3D3, which doesn't enumerate Ramp;
   - Ref/Null → `GetRefZBufferFormats` from d3dref.dll; without it none (D3D_OK, no callbacks) (66-73).
4. Callbacks while they return exactly 1 (137-145); D3D_OK. Allocation failure → E_OUTOFMEMORY (151).

## IDirect3D3::EvictManagedTextures — `EvictManagedTextures_6dea7da5.c`

DD lock; object NULL → DDERR_INVALIDOBJECT (11-12); else the texture manager evicts everything (15) and D3D_OK. Unlike
d3dim700 there is **no hardware-device lookup and no D3DRENDERSTATE_EVICTMANAGEDTEXTURES** to the driver.

## IDirect3D3::CreateVertexBuffer — `CreateVertexBuffer_6deaa511.c`, `CreateVertexBufferI_6deaa4b6.c`, `Init_6dea9a03.c`

`CreateVertexBuffer(D3DVERTEXBUFFERDESC *desc, IDirect3DVertexBuffer **lplpVB, DWORD dwFlags, IUnknown *pUnkOuter)`:
1. pUnkOuter ≠ NULL → CLASS_E_NOAGGREGATION, before the lock (`CreateVertexBuffer_6deaa511.c:13, 19-20`).
2. DD lock; `*lplpVB = NULL` (`CreateVertexBufferI_6deaa4b6.c:16`) — **no NULL checks** on `desc` or `lplpVB`, no SEH
   frame (a NULL faults).
3. Allocation (0x58) → E_OUTOFMEMORY (16-22). The constructor links the VB into the Direct3D object's list (+0x30,
   count +0x2c) without AddRef; refcount 1 (`CDirect3DVertexBuffer_6deaa474.c:9-10`).
4. `Init` (`Init_6dea9a03.c`), reading dwCaps (+4), dwFVF (+8), dwNumVertices (+0xC); **dwSize not checked**:
   1. dwNumVertices ≥ 0x10000 → **D3DERR_TOOMANYVERTICES** (21, 92).
   2. D3DVBCAPS_OPTIMIZED in dwCaps → DDERR_INVALIDPARAMS (22, 88).
   3. **FVF with any bit in 0xFFFFF000 → D3DERR_INVALIDVERTEXFORMAT** (24, 85); vertex size 0 → the same (26-28, 85).
      (d3dim700 has no high-bits check.)
   4. Memory: an execute-buffer surface created through the **private IDirectDraw4 copy**
      (`CreateMemoryBuffer_6dea9906.c`): video memory (EXECUTEBUFFER|VIDEOMEMORY, + WRITEONLY when dwCaps has
      D3DVBCAPS_WRITEONLY and dwFlags has bit 2, D3DDP_DONOTCLIP) **only for XYZRHW vertices without
      D3DVBCAPS_SYSTEMMEMORY** (29-40); else / on failure system memory (41-50); CreateSurface errors returned (49-51).
      System-memory buffers are locked once and stay locked (53-60). The VB keeps an IDirectDrawSurface v1 reference on
      its buffer (61-63). dwFlags bit 31 suppresses DDSCAPS2 hint 0x20 (`Init_6dea9a03.c:29`).
   5. XYZRHW without D3DDP_DONOTCLIP in dwFlags: a clip-code array (2 bytes per vertex); allocation → E_OUTOFMEMORY
      (39-58).
   6. Failure → VB deleted, error returned (`CreateVertexBufferI_6deaa4b6.c:25-31`).
5. The returned pointer is the IDirect3DVertexBuffer (v1) interface.
6. Refcounts: **no game-visible count changes** (the buffer surface AddRefs the private INT copy, see "What a device
   holds"). d3dim700's CreateVertexBuffer raises the IDirect3D7 count; d3dim.dll's does not.

---

## For dd2d9: what to implement

1. **Hand-off**: for DirectDraw objects not made by DirectDrawCreateEx, the first QueryInterface of a non-DirectDraw
   IID (on the DirectDraw object or on one of its surfaces) creates one Direct3D object per DirectDraw object. Its
   outer is the DirectDraw interface that triggered it (for a surface QI: the interface the surface was created
   through). IDirect3D, IDirect3D2, IDirect3D3 → three pointers to that object; IDirect3D7 → E_NOINTERFACE. DirectDraw
   holds the inner reference and drops it when its last reference goes, before its surfaces; that drop force-releases
   remaining viewports, lights, devices and materials.
2. **IDirect3D/2/3 IUnknown**: QueryInterface → DirectDraw's (NULL out → INVALIDPARAMS); AddRef/Release → DirectDraw's
   on the creating interface, returning its count. DirectDraw's QI returns E_NOINTERFACE whenever the Direct3D side
   fails.
3. **IDirect3D::Initialize** → DDERR_ALREADYINITIALIZED.
4. **EnumDevices**: descs sized 0xAC / 0xCC / 0xFC by interface; lists IDirect3D: Ramp, RGB, HAL; IDirect3D2: Ramp,
   RGB, HAL (+ MMX: decide, see Not determined); IDirect3D3: RGB, HAL. Exact names/descriptions above; callback
   argument order (GUID, description, name, HAL, HEL, ctx); stop on any return ≠ 1; bad callback → INVALIDPARAMS;
   D3D_OK. Software devices: HAL desc = the null desc, HEL = the tables above (version-dependent RGB). HAL: HAL desc
   from D3D9, HEL = the fixed HAL-HEL desc. No DX7 fix-ups.
5. **FindDevice**: size checks (0x5C; 0x16C/0x1AC/0x20C), matching rules, "last hardware, else last software",
   DDERR_NOTFOUND, truncating copy-out.
6. **CreateDevice (v2/v3)** in the check order above. T&L HAL GUID → INVALIDPARAMS; Ramp on IDirect3D3 →
   INVALIDPARAMS; v3 pUnkOuter → CLASS_E_NOAGGREGATION; NULL surface → INVALIDOBJECT; > 2048 → INVALIDOBJECT;
   memory-placement errors as in checkDeviceSurface. Device refcount 1. References per the table (v2: the passed
   surface interface; v3: the surface's v4 interface; both: Z and palette); none on Direct3D/DirectDraw that a game
   can read.
7. **Surface QueryInterface** (DirectDraw 1-4 objects only): IID_IDirect3DTexture/2 → a texture object, IID per IID,
   cached on the surface until it dies, AddRef/Release/QI = the surface's (interface first queried through); any
   surface qualifies; palettized surface with a broken palette → that error. A device GUID → an aggregated v1 device
   (same checks as CreateDevice, holding no references, QI answering only its GUID; FPU only with DDSCL_FPUSETUP);
   device creation errors returned from the QI. **Any other IID → E_INVALIDARG** (E_NOINTERFACE only when Direct3D is
   unavailable).
8. **FPU**: set single precision / round-nearest / exceptions masked at device creation only when DDSCL_FPUSETUP was
   given (DDSCL_FPUPRESERVE irrelevant here).
9. **CreateLight / CreateMaterial / CreateViewport**: outer → CLASS_E_NOAGGREGATION (except v1/v2 CreateViewport,
   which ignore it), NULL out → INVALIDPARAMS, refcount 1, no Direct3D reference; viewport handles from a per-Direct3D
   counter starting at 1; v1/v2 CreateMaterial don't check a NULL out.
10. **EnumZBufferFormats**: RGB/MMX the two software formats; Ramp one 16-bit; Ref/Null none; HAL from D3D9 (as for
    DX7); T&L HAL GUID → INVALIDPARAMS.
11. **EvictManagedTextures**: evict managed copies, D3D_OK, no device needed.
12. **CreateVertexBuffer**: outer → CLASS_E_NOAGGREGATION; > 65535 → TOOMANYVERTICES; OPTIMIZED → INVALIDPARAMS; FVF
    high bits or zero size → INVALIDVERTEXFORMAT; no game-visible refcount change.
13. **Managed DX6 textures**: re-upload after any content change (ddraw's dirty flag in `lpSurfMore+0x54`).

## Implementation notes for dd2d9 (on D3D9)

**Where it hooks in.** `ddraw.cpp:81` sends unknown IIDs to `D3D7Query`, which today returns E_NOINTERFACE for
IDirect3D/2/3 and for non-DD7 objects (`D3D7Query` in `d3d7obj.cpp`); `surface.cpp:321` sends unknown IIDs to the D7VK proxy
only. Add a `D3DLegacyQuery(DDraw*, riid, out, ddVersion)` for `!dd->dd7` objects and a `D3DLegacyQuerySurface(Surface*,
riid, out, version)` that implements the surface protocol (texture cache list on `Surface`, released in the Surface
destructor where `D3DProxySurfaceDestroyed` already sits), returning E_INVALIDARG for other IIDs when Direct3D is
enabled (`g_config.d3d`), E_NOINTERFACE when disabled (= "no d3dim.dll").

**One Direct3D object, three vtables.** A `Direct3D3Obj` deriving from IDirect3D, IDirect3D2 and IDirect3D3 (three
sub-objects, as d3dim does), with `outer_` = the DirectDraw interface version like `Direct3D7::outer_`
(`d3d7int.h`); keep it in a new `DDraw::d3dLegacy` next to `d3d7`, destroyed in the same place as
`D3D7Destroy`, force-destroying its devices/lights/materials/viewports.

**Devices.** The drawing machinery of `Device7` (the `GpuContext` from `gpures.h`: SetTargets, states, Draw) is what a
v1-3 device needs. Factor Device7's core (render target/Z binding, state cache, flush-before-CPU-access, Flip
re-binding — see d3d7-direct3d.md "Notifications") into a shared class and put an `IDirect3DDevice/2/3` front on it:
DrawPrimitive with D3DVERTEXTYPE (v2) maps to FVF 0x112/0x1C4/0x1E2 (D3DVT_VERTEX/TLVERTEX/LVERTEX), v3 is already FVF,
v1 is execute buffers (separate spec). The `GpuContext` stays one per DirectDraw object's D3D9 device; the v1-3 GUID
only decides caps, the memory-placement checks and the aggregation/reference rules. Ramp: render as RGB (D3D9 has no
mono lighting); accept its creation and report its desc so that games which pick it work. MMX = RGB.

**Caps.** RGB/MMX/Ramp descs are fixed tables (above): emit them verbatim per interface version (keep the v1/v2 RGB
desc with no alpha blending — games pick paths from it). HAL desc from `GpuGetCaps()` as `HalDesc()` does for DX7
(`d3d7obj.cpp`'s HAL table in d3d7-direct3d.md), converted to D3DDEVICEDESC: dwFlags 0x7FF, dcmColorModel D3DCOLOR_RGB,
dtcTransformCaps {8, CLIP}, bClipping TRUE, dlcLightingCaps {0x10, 0xF, RGB, 8 or 0}, render depths DDBD_16|DDBD_32 (no
DDBD_8), dwMaxBufferSize 0, dwMaxVertexCount 0x400 (as XP substitutes for 0), v3 fields from D3D9 (blend stages /
textures clamped to 8); no HWTRANSFORMANDLIGHT-style fix-ups, no added HWRASTERIZATION (a real DX7 driver sets it
itself, so setting it is harmless). HEL for HAL: the fixed HAL-HEL desc. IDirect3D(1)/2 callers get the same structure
with dwSize 0xAC/0xCC.

**Texture objects.** A `Texture3Obj` (IDirect3DTexture + IDirect3DTexture2 vtables) per {surface, IID}, holding the
`Surface*` and the interface slot; AddRef/Release → `Surface::AddRef/Release(slot)`; QI → `Surface::QueryInterface`.
GetHandle/Load are the texture spec's; the D3D9 texture is the surface's existing `gpuTex`. For TEXTUREMANAGE textures,
`GpuCpuAccess(write)` already marks the GPU copy stale — that is the dirty flag.

**Refcount model.** dd2d9's `DDraw` keeps a total and per-interface counts (`ddraw.cpp` AddRef). XP's internal holds
(command buffer and VB surfaces through the private DirectDraw 4 INT) raise only the object's lifetime count: model
them as an internal hold on `DDraw` that bumps `ref_` but no `ifaceRef_` slot — a device or VB keeps the DirectDraw
object alive without changing what IDirectDraw::Release returns. Do **not** reuse `VertexBuffer7`'s `d3d_->AddRef()`
for the DX6 vertex buffer.

**Pitfalls**
- FPU: per thread, only with DDSCL_FPUSETUP; still create the D3D9 device with D3DCREATE_FPU_PRESERVE.
- The surface-QI device and texture share the surface's refcount; Wine's d3d1 tests check this, and games that
  Release a device then the surface rely on it. Its own QI for IID_IDirect3DDevice → E_INVALIDARG.
- Memory rules as in DX7: HAL needs video-memory render target and Z; RGB/MMX/Ramp need a system-memory Z. RGB/Ramp
  accept 8-bit palettized targets (need a palette): D3D9 can't render to P8 (render to X8R8G8B8 and convert on read-back,
  or refuse with INVALIDPIXELFORMAT deliberately).
- IDirect3D3 never lists Ramp and rejects it in CreateDevice; IDirect3D(1) never lists MMX.
- Viewport handles are per Direct3D object, starting at 1.

## Not determined

- Whether `HKLM\Software\Microsoft\Direct3D\Drivers` exists on a stock XP SP3 install. If it does, IDirect3D2 lists MMX
  on every MMX CPU; if not, MMX is never listed by default (`EnumDevices_6dea76e5.c:58-91`). Needs a check on a real
  XP registry.
- Whether XP SP3 ships d3dref.dll (if so, the registry could expose Ref/Null).
- The effect of `~DIRECT3DI` releasing a surface-QI device through its outer (the surface) in a loop when the
  DirectDraw object dies while such a device lives (reading of `~DIRECT3DI_6deb5070.c:19-21` plus the device's
  forwarding Release; not traced at run time).
- Whether anything after creation reads the device GUID field (+0xfa4) that the device QI overwrites with interface
  IIDs (only `D3DFE_Create` and `stateInitialize` read it, both before).
- What IDirectDraw::Release/AddRef return values look like when the last game reference goes while internal holds
  (private-INT surfaces) remain — the object survives; the exact INT/LCL interplay at teardown was not traced.
- HAL device creation on a pre-DX6 (no DrawPrimitives2) driver: every device is `CDirect3DDeviceIDP2`; whether context
  creation then fails and with which code.
- Return values after exceptions in EnumDevices/FindDevice/EnumZBufferFormats (SEH frames whose catch values weren't
  traced); CreateVertexBuffer and v1/v2 CreateMaterial have none and fault on NULL pointers.
- d3dim's other FPU save/restore sites (`fldcw` at 6deabb95, 6deabbc5, 6dec67a6, 6dec693f, 6dec6b63, 6ded7b0d, 6ded7b5a)
  belong to device/rasterizer code and were not examined.
- Allocator zeroing for the HAL Z-format list (`D3DMalloc` → `_MemAlloc`, assumed LMEM_ZEROINIT as in d3dim700).
