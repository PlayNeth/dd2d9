# IDirect3D7 and device creation (d3dim700.dll)

Reference: `d3dim700.dll` 5.3.2600.5512 (XP SP3), Ghidra decompile with PDB public names. The PDB is stripped
(no types), so structure offsets were read against the DDK layouts in `$S/types/layouts.txt` and the DX7 headers.
`$D` = `$S/d3d-decomp/d3dim700-named/` (`$S` = the session scratchpad), one `.c` per function, line numbers are
those of the file. Return codes the decompiler lost were read from the disassembly (`llvm-objdump` on
`$S/xpd3d/d3dim700.dll`); those cite an address. ddraw.dll references are to `$S/decomp-annotated/` (prefix DD:).
Ghidra shifted the parameters of every `__stdcall` COM method by one (its `param_1` is `this`); the text below
uses the real parameter names.

Out of scope: the device's state/drawing methods, the DDI command stream, the software rasterizers, IDirect3D1-3
(d3dim.dll).

## Objects and how ddraw reaches them

- **DIRECT3DI** (0x58 bytes, `Direct3DCreate_7391e612.c:23`). Layout used below:
  `+0` IDirect3D7 vtable, `+4` number of devices, `+8` device list head, `+0xc` number of vertex buffers,
  `+0x10` vertex-buffer list head, `+0x18` the **inner (non-delegating) IUnknown** (`CDirect3DUnk`: `+0x1c`
  refcount = 1, `+0x20` back pointer) (`DIRECT3DI_7391e5d6.c:7-19`), `+0x2c` TextureCacheManager,
  `+0x30` pointer to a private copy of the IDirectDraw (v1) INT, `+0x34` the DirectDraw INT passed in,
  `+0x50` the outer IUnknown, `+0x54` "no video-memory vertex buffers".
- ddraw loads the DLL lazily, once per DirectDraw object (DD:`FUN_7373d0b5_7373d0b5.c:8-15`): **`D3DIM700.DLL` if the
  object's LCL has DDRAWILCL_DIRECTDRAW7 (0x20000), otherwise `D3DIM.DLL`**. It stores `hD3DInstance` (LCL+0x38)
  and the returned IUnknown in `pD3DIUnknown` (LCL+0x3c), and fetches the exports PaletteUpdateNotify,
  PaletteAssociateNotify, SurfaceFlipNotify, FlushD3DDevices, D3DTextureUpdate, CreateTexture, DestroyTexture,
  Set/GetPriority, Set/GetLOD, D3DBreakVBLock into the LCL (DD:`FUN_7373d0ea_7373d0ea.c:23-63`). Then it calls
  `Direct3DCreate(NULL, &pD3DIUnknown, ddInt)` (line 68); on failure the library is freed (74-75).
- Loading happens on IDirectDraw::QueryInterface of a non-DirectDraw IID (DD:`IDirectDraw7_QueryInterface.c:216-218`)
  and, for DirectDraw7 objects, when a texture surface is created (DD:`FUN_73736f21_73736f21.c:51-56`, see
  CreateTexture). The QI is forwarded to `pD3DIUnknown->QueryInterface` (line 219).
- **IDirect3DDevice7 and textures are not reachable through a surface's QueryInterface on a DirectDraw7 object**:
  that path goes to E_NOINTERFACE when the LCL has DIRECTDRAW7 (DD:`IDirectDrawSurface7_QueryInterface.c:233`).
  The `Direct3DCreateDevice`/`Direct3DCreateTexture` exports it would call belong to d3dim.dll's protocol.

### Direct3DCreate (export 9) — `Direct3DCreate_7391e612.c`

`HRESULT Direct3DCreate(CRITICAL_SECTION *cs, IUnknown **out, IDirectDraw *dd)`:
1. Stores `cs` (ddraw passes NULL) as `lpD3DCSect` (17), takes the DD thread lock (19), `*out = NULL` (21).
2. `dd == NULL` → DDERR_INVALIDPARAMS (7391e645).
3. Allocates and constructs DIRECT3DI; allocation failure → E_OUTOFMEMORY (7391e670).
4. `Initialize(dd, dd)` (31); failure → object deleted, its error returned (36; 7391e68a-7391e695).
5. Success: `*out` = the inner IUnknown (`this+0x18`) (33), S_OK.

### DIRECT3DI::Initialize — `Initialize_7391da8b.c`

1. `dd->QueryInterface(IID_IDirectDraw, &tmp)` (24); failure → that error.
2. Copies the 6-dword v1 INT into `this+0x38`, **releases `tmp`**, and from then on uses the copy (`+0x30`) for
   calls into DirectDraw: **the Direct3D object holds no reference on DirectDraw** (26-35). `+0x34` = `dd` (36).
3. Creates the TextureCacheManager; failure → E_OUTOFMEMORY / its Initialize error (37-52).
4. `+0x54` (no vidmem VBs) = 1 if registry `DisableVidMemVBs` is set (54-59) **or the driver's
   `lpD3DGlobalDriverData->hwCaps.dwDevCaps` lacks D3DDEVCAPS_HWVERTEXBUFFER (0x02000000)** (60-63; GBL+0x478 is
   lpD3DGlobalDriverData, its byte 0x13 is the top byte of hwCaps.dwDevCaps).
5. Outer IUnknown `+0x50` = `dd` (64-67).

### Aggregation: QueryInterface / AddRef / Release

- **IDirect3D7::QueryInterface** (`QueryInterface_7392b461.c`): `ppv == NULL` → DDERR_INVALIDPARAMS (20-21);
  `*ppv = NULL`; then forwarded to the outer (DirectDraw) QueryInterface (24-26). So IDirect3D7 → IDirectDraw7 works,
  and IDirect3D7 → IDirect3D7 goes DD → inner.
- **IDirect3D7::AddRef / Release** (`AddRef_7392b4b3.c:20-21`, `Release_7392b4f1.c:20-21`): forwarded to the outer,
  i.e. **to the DirectDraw INT that first triggered the load** (`+0x50` is the `This` of that first
  IDirectDraw::QueryInterface), and they return that INT's count.
- **Inner IUnknown** (`CDirect3DUnk`):
  - QueryInterface (`QueryInterface_7392b37d.c`): NULL out → INVALIDPARAMS (24-25); IID_IUnknown or
    **IID_IDirect3D7 only** (29-50) → `IDirect3D7::AddRef` (= DirectDraw AddRef) and the IDirect3D7 pointer
    (56-58); anything else → E_NOINTERFACE (52). So on a DirectDraw7 object, IID_IDirect3D/2/3 → E_NOINTERFACE.
  - AddRef/Release act on the inner count (`AddRef_7392b3fe.c:11`, `Release_7392b52f.c:21`); 0 → the object is
    deleted (24-25). Only ddraw holds the inner reference, and releases it when the DirectDraw LCL count reaches 0
    (DD:`IDirectDraw7_Release.c:94-99`), **before** that release destroys the object's surfaces (102-107).
- ~DIRECT3DI (`~DIRECT3DI_7392b423.c`) frees the texture manager and a free-list; it does not look at devices or
  vertex buffers.

### Other exports called by ddraw

| Export | Called by ddraw | What d3dim700 does |
| --- | --- | --- |
| CreateTexture(surfInt) | DD7 surface creation with DDSCAPS_TEXTURE (DD:`FUN_73736f21_73736f21.c:51-56`) | `CreateTexture_7391d9a4.c`: picks the texture class from `lpSurfMore->ddsCapsEx.dwCaps2`: TEXTUREMANAGE without driver management (GBL dwFlags bit 0x00800000), or D3DTEXTUREMANAGE → runtime-managed `DIRECT3DTEXTURED3DM`; TEXTUREMANAGE with driver management → `DIRECT3DTEXTUREM`; else `DIRECT3DTEXTUREI` (16-45). Initializes it with the DIRECT3DI (52-53) and stores it in `lpSurfMore+0x78` (55); allocation → E_OUTOFMEMORY (49), and an Initialize error is returned, which ddraw returns from CreateSurface. |
| DestroyTexture(tex) | surface destruction | `tex->Destroy()`, free (`DestroyTexture_7391d983.c:6-7`). |
| Set/GetPriority, Set/GetLOD(tex, v) | IDirectDrawSurface7 methods | Virtual call (`SetPriority_7391d41d.c` etc.). **A non-managed texture returns DDERR_INVALIDPARAMS from all four** (all four vtable slots are `DIRECT3DTEXTUREI::GetPriority`, which returns 0x80070057: `GetPriority_7391d3da.c`). Managed: Get returns the stored value (`GetPriority_7391d3e7.c`, `GetLOD_7391d402.c`); SetLOD with `lod >= dwMipMapCount` → INVALIDPARAMS (`SetLOD_7391d811.c`, `SetLOD_7391d745.c`); SetPriority always D3D_OK for runtime-managed (`SetPriority_7391d720.c`). This settles the open point in surface-other.md (§SetPriority). |
| FlushD3DDevices(surfLcl) | before Lock, Blt, BltFast, SetSurfaceDesc, Add/DeleteAttachedSurface (DD:`FUN_73732f07`, called from those) | `FlushD3DDevices_7391dbab.c`: flushes the pending command batch of every device whose batch stamp is ≤ the surface's stamp (`lpSurfMore+0x70/+0x74`, 64-bit; cube faces use the parent's) (21-41). Render target and Z get stamp ‑1 at device creation (`Init_7391b66b.c:205-212`), so **any CPU access to them flushes**. |
| D3DTextureUpdate(pD3DIUnknown) | after a texture's contents change (DD:`FUN_7375a555_7375a555.c:9-11`, from Lock/Blt paths) | Marks every device "textures dirty" (`D3DTextureUpdate_7391dd63.c:8-10`). |
| SurfaceFlipNotify(pD3DIUnknown) | after Flip | For DX7-DDI devices (class ≥ 4) whose render target's driver handle changed, re-sends the render target to the driver (`SurfaceFlipNotify_7391dcd3.c:15-33`). Game-visible meaning: **the device keeps rendering to the same surface object (the back buffer interface) after a flip**, not to the memory it had. |
| PaletteUpdateNotify / PaletteAssociateNotify | SetEntries / SetPalette | Forwarded to the first hardware DX7-DDI device (`PaletteUpdateNotify_7391dc47.c:18-37`, `PaletteAssociateNotify_7391e540.c:18-37`); driver-side. |
| D3DBreakVBLock(vb) | surface loss of a VB's buffer | `CDirect3DVertexBuffer::BreakLock` (`D3DBreakVBLock_7391dd8d.c:6`). |
| Direct3D_HALCleanUp(cb, pid) | not fetched by XP ddraw's loader | Calls the driver's ContextDestroyAll (`Direct3D_HALCleanUp_7391e8b2.c:9-12`). |

---

## IDirect3D7::EnumDevices — `EnumDevices_7391e26f.c`, `EnumDevices_7391de5e.c`

The method calls the worker with descriptor size 0xec and version 4 (`EnumDevices_7391e26f.c:12`).
1. DD lock; `IsBadCodePtr(callback)` → DDERR_INVALIDPARAMS (`EnumDevices_7391de5e.c:61`; 7391dead).
2. Registry `HKLM\Software\Microsoft\Direct3D\Drivers` (66-88): `SoftwareOnly`, `EnumReference`, `EnumNullDevice`
   (REG_DWORD). All absent on a normal XP install.
3. Walks the static table at 0x739cef3c (91-92), stopping at its end or **as soon as the callback returns anything
   other than 1 (D3DENUMRET_OK)** (92, 228). Table (dumped from the binary; GUID, name, description):

   | # | GUID | lpDeviceName | lpDeviceDescription |
   | --- | --- | --- | --- |
   | 1 | IID_IDirect3DRGBDevice `a4665c60-2673-11cf-a31a-00aa00b93356` | `RGB Emulation` | `Microsoft Direct3D RGB Software Emulation` |
   | 2 | IID_IDirect3DHALDevice `84e63de0-46aa-11cf-816f-0000c020156e` | `Direct3D HAL` | `Microsoft Direct3D Hardware acceleration through Direct3D HAL` |
   | 3 | IID_IDirect3DRefDevice `50936643-13e9-11d1-89aa-00a0c9054129` | `Reference Rasterizer` | `Microsoft Reference Rasterizer` |
   | 4 | IID_IDirect3DNullDevice `8767df22-bacc-11d1-8969-00a0c90629a8` | `Null device` | `Microsoft Null Device` |
   | 5 | IID_IDirect3DTnLHalDevice `f5049e78-4861-11d2-a407-00a0c90629a8` | `Direct3D T&L HAL` | `Microsoft Direct3D Hardware Transform and Lighting acceleration capable device` |

   The callback gets `(description, name, desc, ctx)` in that order: description = entry+0x104, name = entry+4
   (228). Ramp and MMX are not in the DX7 table.
4. Per entry:
   - Ref skipped unless EnumReference ≠ 0 (95-107); Null skipped unless EnumNullDevice ≠ 0 (111-123).
   - T&L HAL skipped when the driver has no `lpD3DGlobalDriverData` or its hwCaps.dwDevCaps lacks
     D3DDEVCAPS_HWTRANSFORMANDLIGHT (0x10000) (125-138).
   - Provider: `GetSwHalProvider` first; if it returns E_NOINTERFACE and SoftwareOnly is off, `GetHwHalProvider`
     (139-141). Any other failure → the entry is skipped silently.
     - Sw (`_GetSwHalProvider@12_7392b8cf.c`): RGB, MMX, MMXAsRGB → built-in rasterizer providers (23-95, with a
       `ForceRgbRasterizer` registry override 35-63); Ref and Null → `GetRefHalProvider` from **d3dref.dll**
       (`LoadReferenceDeviceProc_7392b57f.c:10-14`); without that DLL → E_NOINTERFACE, so Ref/Null are not listed
       even when the registry asks for them (109-132).
     - Hw (`_GetHwHalProvider@16_73924592.c:2-43`): HAL and T&L HAL only, and only if the driver's
       `lpD3DHALCallbacks` (GBL+0x47c) passes `D3DI_isHALValid` (dwSize 0x8c, reserved fields zero:
       `D3DI_isHALValid_7391a273.c:7-23`), else E_NOINTERFACE (skipped).
   - `provider->GetCaps(gbl, &halDesc, &helDesc, 4)` (143), then the provider is released and its module freed
     (144-147). A GetCaps failure skips the entry (148).
   - Fix-ups (149-190): a `wMaxVertexBlendMatrices` of 1 becomes 0 in both descs. **HAL**: dwMaxActiveLights =
     0xFFFFFFFF, D3DDEVCAPS_HWTRANSFORMANDLIGHT cleared from dwDevCaps, wMaxUserClipPlanes = 6,
     wMaxVertexBlendMatrices = 4, dwVertexProcessingCaps = 0x3F (166-173). **RGB**: the same four values in the HEL
     desc (185-190). T&L HAL: nothing (driver values as converted).
   - HAL and T&L HAL report the **HAL desc**, every other device the **HEL desc**; the entry's GUID is written into
     `deviceGUID` (194-227).
5. Returns D3D_OK (7391e248). An exception returns an uninitialised local (7391e240; see Not determined).

### Where the descriptors come from

- **HAL / T&L HAL** — `HwHalProvider::GetCaps` (`GetCaps_73924442.c`):
  - HAL desc = `D3DDeviceDescConvert(lpD3DGlobalDriverData->hwCaps, lpD3DExtendedCaps)` (15-17). Convert
    (`D3DDeviceDescConvert_7391b035.c`): dwDevCaps, both D3DPRIMCAPS, dwDeviceRenderBitDepth and
    dwDeviceZBufferBitDepth from the V1 desc (14-31); min/max texture size, repeat, aspect, anisotropy, guard band,
    extents adjust, stencil, FVF, texture-op caps, blend stages, simultaneous textures, active lights, max W, user
    clip planes, blend matrices, vertex processing caps and the reserved dwords from the extended caps (32-59).
    With no extended caps those fields keep whatever the caller's buffer held (uninitialised stack in EnumDevices).
  - **D3DDEVCAPS_HWRASTERIZATION is always added** to the HAL desc (24).
  - Pre-DX6 drivers (no `lpD3DHALCallbacks3` or no DrawPrimitives2 in it: GBL+0x638, +0x14) get synthesized
    multitexture caps (25-57): dpcTriCaps.dwRasterCaps |= WFOG; dwMaxAnisotropy 1; 1 blend stage, 1 texture;
    dwTextureOpCaps = DISABLE, plus SELECTARG1 (from DECAL/COPY blend), MODULATE (MODULATE/MODULATEALPHA), ADD (ADD),
    BLENDTEXTUREALPHA (DECALALPHA); the new-style MIN/MAG/MIP filter bits derived from the legacy ones (tri caps
    only).
  - HEL desc = a static desc with dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX and empty primitive caps
    (0x739cf230; 18-22).
  - Additionally, **creating any hardware device clears DDBD_8 from the driver's dwDeviceRenderBitDepth**
    (`InitDeviceI_7391ac0a.c:93-95`; it edits the driver data in place, so later EnumDevices calls no longer
    report DDBD_8 for HAL).
- **RGB (and MMX)** — `OptRastHalProvider::GetCaps` (`GetCaps_7392b767.c`): HAL desc = `g_nullDevDesc` (all zero
  except the two dwSize = 0x38) (15-21); HEL desc = a static desc built once (`FUN_7392b5a6_7392b5a6.c:11-70`,
  converted at `GetCaps_7392b767.c:22-27`). Final values as the game sees them (after EnumDevices' RGB fix-ups):

  | Field | Value |
  | --- | --- |
  | dwDevCaps | 0x551 (FLOATTLVERTEX, EXECUTESYSTEMMEMORY, TLVERTEXSYSTEMMEMORY, TEXTURESYSTEMMEMORY, DRAWPRIMTLVERTEX) |
  | dpcLineCaps = dpcTriCaps | dwSize 0x38, Misc 0x70, Raster 0x2121F7, ZCmp 0xFF, SrcBlend 0x1FFF, DestBlend 0x7FF, AlphaCmp 0xFF, Shade 0xC528A, Texture 0xDF, TextureFilter 0x0303033F, TextureBlend 0xCF, TextureAddress 0x1F, Stipple 4×4 |
  | dwDeviceRenderBitDepth / ZBufferBitDepth | 0xF00 (DDBD_8/16/24/32) / 0x400 (DDBD_16) |
  | min / max texture | 1×1 / 1024×1024 |
  | dwMaxTextureRepeat, AspectRatio, Anisotropy | 256, 0, 1 |
  | guard band L/T/R/B, dvExtentsAdjust | −4096, −4096, 4095, 4095, 0 |
  | dwStencilCaps, dwFVFCaps, dwTextureOpCaps | 0xFF, 8, 0x78FF |
  | wMaxTextureBlendStages, wMaxSimultaneousTextures | 8, 8 |
  | dwMaxActiveLights, dvMaxVertexW | 0xFFFFFFFF (EnumDevices fix-up; 0 in the static desc), 10000.0 |
  | wMaxUserClipPlanes, wMaxVertexBlendMatrices, dwVertexProcessingCaps | 6, 4, 0x3F (fix-ups) |
  | deviceGUID | IID_IDirect3DRGBDevice; reserved dwords 0 |

  (Computed from the writes in `FUN_7392b5a6` plus Convert; the descs live in BSS, so unwritten fields are 0.
  Script: `$S/d3d7spec/rgbcaps.py`.)

---

## IDirect3D7::CreateDevice — `CreateDevice_7391fcfb.c`, `Direct3DCreateDevice_7391cf86.c`

Order of checks and effects (`CreateDevice(rclsid, lpDDS7, lplpD3DDevice)`):
1. DD lock. `lplpD3DDevice == NULL` → DDERR_INVALIDPARAMS (`CreateDevice_7391fcfb.c:26-28`).
   `lpDDS7 == NULL` → **DDERR_INVALIDOBJECT** (30, 47). `*lplpD3DDevice = NULL` (31).
2. `lpDDS7->QueryInterface(IID_IDirectDrawSurface)` → its error; the v1 pointer is released at once (32-34).
   An exception anywhere → DDERR_INVALIDPARAMS (catch at 7391fd9b).
3. `Direct3DCreateDevice(rclsid, innerUnknown, surfV1, &unk, NULL)` (35-37):
   1. `IsValidD3DDeviceGuid`: IsBadReadPtr(16) and one of Ramp, RGB, MMX, HAL, Ref, Null, T&L HAL
      (`IsValidD3DDeviceGuid_7391dda3.c:14-107`), else DDERR_INVALIDPARAMS (`Direct3DCreateDevice_7391cf86.c:42-46`).
   2. MMX GUID on a CPU without MMX → DDERR_INVALIDPARAMS (53-64).
   3. Registry `HKLM\Software\Microsoft\Direct3D`: `DriverStyle` (default 5, capped at 5) and `DisableST` (65-80).
   4. **Device class** ("style"): 3 = `CDirect3DDeviceIDP2` (DX6 DrawPrimitives2 DDI), 4 = `CDirect3DDevice7`
      (DX7 DDI), 5 = `CDirect3DDeviceTL` (DX7 DDI with hardware T&L) (constructors set `+0x2284`:
      `CDirect3DDeviceIDP2_7391ce98.c`, `CDirect3DDevice7_7391cee8.c`, `CDirect3DDeviceTL_73927143.c`).
      - T&L HAL → 5 if the driver's hwCaps.dwDevCaps has HWTRANSFORMANDLIGHT, else none (81-101).
      - HAL → 4 if the driver has the DX7 DDI (a non-NULL entry at DDHAL_CALLBACKS+0x37c of `lpDDCBtmp`), else 3
        if it has DrawPrimitives2 (`lpD3DHALCallbacks3->DrawPrimitives2`); DriverStyle 3/4 picks that one if
        available (103-137). Never 5.
      - Ref → DriverStyle if 2..5, else 5 (138-150). Everything else (RGB, MMX, Null, Ramp) → 3 (153).
      - **No class (T&L HAL without hardware T&L, a HAL driver without DrawPrimitives2, DriverStyle 2 for Ref) →
        E_OUTOFMEMORY** (158-210: none of the three branches, falls to 210).
   5. Allocation (0x33C8 bytes) failure → E_OUTOFMEMORY (210). If managed textures were lost, they are evicted
      (172-175). DisableST → device flag "multithreaded" (176-178).
   6. `device->Init(rclsid, d3d, surfV1, NULL, &unk)` (179) — below. Failure → the device is deleted (189) and the
      error returned.
   7. Success, unless the DirectDraw LCL has **DDSCL_FPUPRESERVE** (dwLocalFlags 0x80000): the x87 control word is
      set to **24-bit precision, round to nearest, all exceptions masked** (181-186; `fnstcw/and 0xFCFF/or 0x3F/and
      0xF3FF/fldcw` at 7391d248-7391d260). This is done on the calling thread at CreateDevice time only.
4. `unk->QueryInterface(IID_IDirect3DDevice7, lplpD3DDevice)` and `unk->Release()` (`CreateDevice_7391fcfb.c:38-41`):
   the game receives a device with refcount 1.

### Device Init — `Init_73925ffa.c` (class 3; classes 4 and 5 call it: `Init_73926573.c:13`, `Init_73926d08.c:13-14`) then `Init_7391b66b.c`

1. HAL and T&L HAL set the "hardware device" flag (`+0x1a6c` bit 0x40) (`Init_73925ffa.c:31-56`); class 5 also sets
   hardware T&L (`Init_73926d08.c:13`).
2. A 16 KB command buffer is created as a DirectDraw execute-buffer surface through the DirectDraw INT
   (`GrowCommandBuffer_739246d2.c:31-40`) — video memory for hardware devices, falling back to system memory.
3. `DIRECT3DDEVICEI::Init` (`Init_7391b66b.c`):
   1. Refcount = 1, batch stamp = 1 (48-49). DDSCL_MULTITHREADED (LCL dwLocalFlags 0x4000) → multithreaded flag
      (55-57).
   2. RGB on an MMX CPU uses the MMX rasterizer under the RGB name unless registry `UseMMXForRGB` = 0 (61-87); with
      the app-compat flag `dwAppHackFlags & 4` the device's GUID becomes IID_IDirect3DMMXDevice (88-102, reported
      back in GetCaps' deviceGUID).
   3. `surf->GetSurfaceDesc` (DDSURFACEDESC, 0x6C) (104-110) → its error.
   4. **Not DDSCAPS_3DDEVICE, or DDSCAPS_ZBUFFER → DDERR_INVALIDCAPS** (112-113).
   5. **Width or height > 2048 → DDERR_INVALIDOBJECT** (115, 293-294).
   6. `surf->GetPalette`: an error other than DDERR_NOPALETTEATTACHED → DDERR_INVALIDPARAMS; no palette and
      dwRGBBitCount < 16 → **DDERR_NOPALETTEATTACHED** (116-126). The palette reference is kept (127).
   7. `surf->GetAttachedSurface(DDSCAPS_ZBUFFER)` (128-130): an error other than DDERR_NOTFOUND is returned
      (after releasing the palette, 284-291); the Z surface (v1 interface, AddRef'd) is kept (131). **Only a Z
      buffer attached to the render target is used**; none is fine.
   8. Provider: `GetSwHalProvider` (MMXAsRGB when 2. chose it), then `GetHwHalProvider` (140-148). Ramp, and
      Ref/Null without d3dref.dll, fail here with **E_NOINTERFACE**. (Registry `DisableRendering` swaps in a test
      provider, 149-157.)
   9. `InitDeviceI` (`InitDeviceI_7391ac0a.c`): the device's D3DDEVICEDESC7 (`+0x284c`) = the provider's HAL desc
      for hardware devices, HEL desc otherwise (26-35), with the **same fix-ups as EnumDevices** (deviceGUID,
      blend matrices 1→0, HAL/RGB: lights −1, 4 matrices, 6 clip planes, VP caps 0x3F) (38-63), so
      IDirect3DDevice7::GetCaps matches what EnumDevices reported. Driver interface data, FVF caps, DDBD_8
      removal for hardware (83-124).
   10. `checkDeviceSurface(rt, z, guid)` (`checkDeviceSurface_7391aa9f.c`, codes from the disassembly):
       - rt GetCaps / GetPixelFormat errors returned (29-37);
       - hardware device and rt not DDSCAPS_VIDEOMEMORY → **D3DERR_SURFACENOTINVIDMEM** (7391aafe);
       - rt DDPF_RGB: bit count not 8/16/24/32, or < 16 on Ref/Null, or not in the device's
         dwDeviceRenderBitDepth → **DDERR_INVALIDPIXELFORMAT** (39-68; 7391abed);
       - z: GetPixelFormat/GetCaps errors returned (77-79); hardware device and z not VIDEOMEMORY →
         **D3DERR_ZBUFF_NEEDS_VIDEOMEMORY** (7391aba6); software device and z not SYSTEMMEMORY →
         **D3DERR_ZBUFF_NEEDS_SYSTEMMEMORY** (7391abb3); Ramp with a stencil Z → DDERR_INVALIDPARAMS (7391abd1);
         DDPF_ZBUFFER with an unsupported depth → DDERR_INVALIDPIXELFORMAT (7391abd8-7391abed).
       - A software device accepts a video-memory render target here (no check).
   11. `D3DFE_Create` (`D3DFE_Create_7391eaa5.c`): **QI the render target for IDirectDrawSurface7 and keep it**
       (25-29) — the reference the device holds on its render target; QI the Z buffer for IDirectDrawSurface7 and
       release it at once (30-37); a guard band in the extended caps enables guard-band clipping (47-57); hardware
       devices create a DirectDraw clipper through the DD INT (59-67); **render target bit depth not in the driver's
       dwDeviceRenderBitDepth → DDERR_INVALIDOBJECT** (89-90, 130); driver context creation
       (`halCreateContext_7391e8e1.c:61`) failure → E_FAIL (84-85); all transforms (WORLD, VIEW, PROJECTION,
       WORLD1-3, TEXTURE0-7) set to identity (102-116).
   12. `HookToD3D` (`HookToD3D_7391af00.c`): device linked at the head of the Direct3D object's device list,
       `+0x2288` = the DIRECT3DI, **`IDirect3D7::AddRef` (= DirectDraw AddRef)**, device count +1 (7-20).
   13. Stamps of the render target and Z set to −1 (205-212); `stateInitialize` (213); **default viewport
       (0, 0, surface width, height, 0.0, 1.0) set through SetViewport** (215-223); 8 light slots, state-block
       store (225-256).
4. Back in the class-3 Init: a 64 KB internal TL vertex buffer and a 1-vertex internal vertex buffer
   (FVF 0x1C4, D3DVBCAPS_SYSTEMMEMORY) are created through CreateVertexBufferI (`Init_73925ffa.c:67-75`,
   `TLVbuf_Grow_73925849.c:70`); failure → E_OUTOFMEMORY or their error.

### Reference counting and destruction

- Device: AddRef/Release on `+0x22b8` (`AddRef_7392b2e1.c:12`, `Release_7392b309.c:20`), 0 → virtual delete (25).
  QueryInterface: NULL out → INVALIDPARAMS; IID_IUnknown or IID_IDirect3DDevice7 → the same pointer, AddRef'd;
  else E_NOINTERFACE (`QueryInterface_7392b266.c:24-57`). GetDirect3D: NULL out → INVALIDPARAMS, else the
  IDirect3D7 pointer AddRef'd (= DirectDraw AddRef) (`GetDirect3D_7391c103.c`).
- What a live device holds:
  - the **render target's IDirectDrawSurface7 interface** (+1 on it; it is the game's own INT when the game
    holds a v7 pointer, since ddraw reuses one INT per version) (`D3DFE_Create_7391eaa5.c:26`);
  - the attached **Z buffer's IDirectDrawSurface (v1) interface** (+1 on the v1 INT, not on the game's v7 one)
    (`Init_7391b66b.c:129-131`);
  - the render target's **palette** (`Init_7391b66b.c:116,127`);
  - **one reference on the DirectDraw INT** through IDirect3D7::AddRef (`HookToD3D_7391af00.c:19`), and, since
    DirectDraw4/7 surfaces AddRef the DirectDraw interface they were created through (DD:`FUN_73736f21_73736f21.c:59-63`),
    one more per internal execute-buffer surface (command buffer, TL vertex buffer, 1-vertex vertex buffer). So the
    IDirect3D7/IDirectDraw7 count rises by several on CreateDevice (exact number uncertain: it depends on how many
    internal buffers exist at that moment).
- Consequences:
  - Releasing the render target or the DirectDraw object before the device does not destroy them; the device keeps
    working.
  - ~DIRECT3DDEVICEI (`~DIRECT3DDEVICEI_7391c9a8.c`): render target / Z stamps cleared (28-37), front end
    destroyed (42), **UnhookFromD3D** (44: unlink, device count −1, IDirect3D7::Release = DirectDraw Release:
    `UnhookFromD3D_7391af47.c:20-21`), provider released and its module freed (49-55), then **Z, palette and
    render target released** (63-69). If that last render-target release drops the final reference on a game
    surface, that surface's own DirectDraw reference goes with it.
  - The device is not tied to the render target in the other direction: there is no notification when the game
    detaches or loses the surface; ddraw only calls FlushD3DDevices/SurfaceFlipNotify.

---

## IDirect3D7::CreateVertexBuffer — `CreateVertexBuffer_7392b223.c`, `CreateVertexBufferI_7392b1bd.c`, `Init_73929b01.c`

1. DD lock; `*lplpVB = NULL` (`CreateVertexBufferI_7392b1bd.c:16`). No explicit pointer checks.
2. Allocation → E_OUTOFMEMORY (22). The constructor links the VB into the Direct3D object's list and increments
   its VB count; **it does not AddRef the Direct3D object** (`hookVertexBufferToD3D_73929713.c:7-19`).
3. `Init` (`Init_73929b01.c`), reading desc+4 dwCaps, +8 dwFVF, +0xC dwNumVertices; **`dwSize` is not checked**:
   1. dwNumVertices > 0xFFFF → **D3DERR_TOOMANYVERTICES** (33, 94).
   2. dwCaps has D3DVBCAPS_OPTIMIZED → DDERR_INVALIDPARAMS (34, 90).
   3. Vertex size (position + normal/size/colours + texture coordinates, `GetVertexSizeFVF_7391f626.c`,
      `ComputeTextureCoordSize_7391f6e1.c`) of 0 → **D3DERR_INVALIDVERTEXFORMAT** (36-42). No other FVF validation
      here (e.g. FVF 0 with a texture count would pass).
   4. `dwFlags` is ignored except bit 31 (internal: suppresses DDSCAPS2 hint 0x20 on the buffer surface) (45).
   5. The memory is a DirectDraw execute-buffer surface created through the DD INT (`CreateMemoryBuffer_739297ca.c`):
      video memory (+ DDSCAPS_WRITEONLY if D3DVBCAPS_WRITEONLY) only when not D3DVBCAPS_SYSTEMMEMORY, vidmem VBs
      are allowed (`+0x54`), and the driver has hardware T&L or the FVF is XYZRHW (40-52); otherwise / on failure,
      system memory (52-55). CreateSurface errors are returned (58-61). System-memory buffers are locked once and
      stay locked (65-66). Because the surface AddRefs the DirectDraw interface, **CreateVertexBuffer raises the
      IDirect3D7 refcount** (matches Wine's test_vb_refcount on Windows).
   6. Init failure → VB deleted, error returned (`CreateVertexBufferI_7392b1bd.c:25-31`).
4. The VB's refcount starts at 1 (`CDirect3DVertexBuffer_7392b16b.c:9`). Its release frees the buffer surface
   (`~CDirect3DVertexBuffer_73929753.c`).

## IDirect3D7::EnumZBufferFormats — `EnumZBufferFormats_7391e292.c`

1. `this == NULL` → DDERR_INVALIDOBJECT (7391e2b9); bad callback (`IsBadCodePtr`) or invalid device GUID →
   DDERR_INVALIDPARAMS (36-38; 7391e2e5).
2. HAL / T&L HAL (49-89):
   - no `lpD3DGlobalDriverData` → **DDERR_NOZBUFFERHW** (52; 7391e371);
   - the driver's own list (GBL `dwNumZPixelFormats`/`lpZPixelFormats`, +0x63c/+0x640, from GetDriverInfo) is
     enumerated as is, in driver order (53-61, 109-115);
   - otherwise one format per bit set in hwCaps.dwDeviceZBufferBitDepth, **in the order 8, 16, 24, 32 bits**:
     dwSize 32, dwFlags DDPF_ZBUFFER, dwZBufferBitDepth = bits, dwZBitMask = 0xFF/0xFFFF/0xFFFFFF/0xFFFFFFFF, other
     fields 0 (memory from LocalAlloc(LMEM_ZEROINIT): `_MemAlloc_73990514.c:5`) (62-88); no bits →
     DDERR_NOZBUFFERHW (63; 7391e371).
3. Software GUIDs (`_GetSwZBufferFormats@8_7392ba35.c`): RGB/MMX → two formats (`ZBufferFormats_7392e8d1.c:18-29`):
   1. DDPF_ZBUFFER, 16 bits, Z mask 0xFFFF;
   2. DDPF_ZBUFFER|DDPF_STENCILBUFFER, dwZBufferBitDepth 32, dwStencilBitDepth 8, Z mask 0xFFFFFF00,
      stencil mask 0xFF.

   Ref/Null → `GetRefZBufferFormats` from d3dref.dll; Ramp → none (D3D_OK, no callbacks) (60-85).
4. Callbacks while they return exactly 1 (124-127); the buffer is freed; D3D_OK (7391e47c). Allocation failure →
   E_OUTOFMEMORY (7391e31d).

## IDirect3D7::EvictManagedTextures — `EvictManagedTextures_7391e4a2.c`

`this == NULL` → DDERR_INVALIDOBJECT (21-22). Otherwise it looks for the first **hardware** device (25-27); with
driver texture management (GBL flag 0x00800000) it sends render state 61 (D3DRENDERSTATE_EVICTMANAGEDTEXTURES) and
flushes (28-31); then the runtime texture manager evicts everything (33). **With no hardware device nothing is
evicted.** Always D3D_OK (37).

---

## For dd2d9: what to implement

1. **Hand-off**: the Direct3D object belongs to the DirectDraw object, created on the first QueryInterface for an
   IID ddraw doesn't know, only for objects from DirectDrawCreateEx (DIRECTDRAW7); IDirect3D/2/3 on such an object
   → E_NOINTERFACE; IDirect3D7 → the single Direct3D object. Created once, released when the DirectDraw object
   dies (before its surfaces).
2. **IDirect3D7 IUnknown**: QueryInterface forwards to DirectDraw (NULL out → INVALIDPARAMS); AddRef/Release are
   DirectDraw's AddRef/Release **on the interface that first created Direct3D**, returning its count.
3. **EnumDevices**: entries in the order RGB, HAL, (Ref, Null: not by default), T&L HAL; exact names and
   descriptions from the table above; argument order (description, name); stop on any return ≠ 1; bad callback →
   INVALIDPARAMS; D3D_OK. Ramp and MMX never listed.
4. **CreateDevice**, in this order: NULL out → INVALIDPARAMS; NULL surface → INVALIDOBJECT; unknown GUID →
   INVALIDPARAMS; Ramp (and Ref/Null) → E_NOINTERFACE; T&L HAL when not enumerated → E_OUTOFMEMORY; then the surface
   checks of Init 3.3-3.7 and checkDeviceSurface (INVALIDCAPS, size > 2048 → INVALIDOBJECT, palette for < 16 bpp,
   SURFACENOTINVIDMEM, INVALIDPIXELFORMAT, ZBUFF_NEEDS_VIDEOMEMORY / SYSTEMMEMORY). MMX GUID: accepted on any modern
   CPU (behaves as RGB).
5. **Device state at creation**: identity transforms, viewport = whole render target with Z 0..1, GetCaps = the
   EnumDevices desc of that GUID; Z buffer = the one attached to the render target (DDSCAPS_ZBUFFER), none allowed.
6. **References**: device +1 on the render target's v7 interface, +1 on the Z surface (v1 count), +1 on the palette,
   and ≥ 1 on the DirectDraw interface; all released when the device dies (DirectDraw first, surfaces last).
   CreateVertexBuffer must also raise the IDirect3D7 count (Wine checks "greater than before").
7. **FPU**: at CreateDevice, unless DDSCL_FPUPRESERVE, set the calling thread's x87 control word to single
   precision / round-nearest / exceptions masked.
8. **CreateVertexBuffer** validation: > 65535 vertices → TOOMANYVERTICES; OPTIMIZED cap → INVALIDPARAMS; zero-size
   FVF → INVALIDVERTEXFORMAT; dwSize and dwFlags not checked.
9. **EnumZBufferFormats**: software GUIDs → the two RGB formats; HAL/T&L HAL → the list derived from D3D9 (below);
   errors as above.
10. **EvictManagedTextures**: D3D_OK; evict the managed textures' D3D9 copies (only meaningful once a device exists).
11. **Notifications become direct calls**: before CPU access (Lock, Blt, BltFast, GetDC, SetSurfaceDesc,
    Add/DeleteAttachedSurface) to any surface the device may have used, flush the device and make the CPU copy
    valid (the design's cpuValid model); after Flip, the device's render target stays the back-buffer surface object,
    so rebind the D3D9 render target to whatever buffer that surface now owns; texture Lock/Blt marks textures for
    re-upload; palette changes mark P8 textures.
12. **Texture management exports**: Set/GetPriority, Set/GetLOD on a non-managed texture → INVALIDPARAMS; SetLOD
    ≥ mip count → INVALIDPARAMS.

## Implementation notes for dd2d9 (on D3D9)

**Which devices to expose.** RGB, HAL and T&L HAL, in that order. T&L HAL when the D3D9 adapter reports
D3DDEVCAPS_HWTRANSFORMANDLIGHT (every D3D9 GPU and DXVK do). Ref and Null: no (XP doesn't list them without the SDK's
d3dref.dll). All three map to the one D3D9 device of the DirectDraw object; the GUID only decides the caps reported,
the memory-placement rules of checkDeviceSurface, and (for T&L HAL) whether vertex processing may run on the GPU.

**RGB**: report the fixed table above verbatim (a game that picks RGB expects exactly those limits, e.g. 1024² max
texture, 16-bit Z only, system-memory Z buffer).

**HAL desc from D3D9 caps** (`IDirect3D9::GetDeviceCaps(D3DDEVTYPE_HAL)`):

| DX7 field | Source |
| --- | --- |
| dwDevCaps | D3D9 DevCaps & 0x000BFFF0 (same bit values for the DX7 bits), plus FLOATTLVERTEX (1), DRAWPRIMITIVES2/2EX, HWRASTERIZATION; HWTRANSFORMANDLIGHT **cleared** for HAL, set for T&L HAL |
| Misc caps | D3D9 PrimitiveMiscCaps & (MASKZ\|CULLNONE\|CULLCW\|CULLCCW) |
| Raster caps | D3D9 RasterCaps & (DITHER\|ZTEST\|FOGVERTEX\|FOGTABLE\|MIPMAPLODBIAS\|ZBUFFERLESSHSR\|FOGRANGE\|ANISOTROPY\|WBUFFER\|WFOG\|ZFOG), plus SUBPIXEL, plus ZBIAS (0x4000) when D3D9 has DEPTHBIAS |
| ZCmp, AlphaCmp, Src/DestBlend, TextureAddress (& 0x1F), Stencil (& 0xFF) | D3D9 values (same bits) |
| Shade caps | D3D9 ShadeCaps plus the FLAT/GOURAUD MONO/RGB colour, specular and alpha-flat bits DX7 drivers set |
| Texture caps | D3D9 TextureCaps & (PERSPECTIVE\|POW2\|ALPHA\|SQUAREONLY\|TEXREPEATNOTSCALEDBYSIZE\|ALPHAPALETTE\|NONPOW2CONDITIONAL\|PROJECTED\|CUBEMAP), plus TRANSPARENCY (colour key emulated) |
| TextureFilter | D3D9 MIN/MAG/MIP bits, plus the legacy NEAREST..LINEARMIPLINEAR bits derived from them (inverse of HwHalProvider's legacy mapping) |
| TextureBlend (legacy) | synthesize DECAL\|MODULATE\|DECALALPHA\|MODULATEALPHA\|COPY\|ADD (0xCF, as RGB) |
| Line caps | = Tri caps |
| Render / Z bit depth | DDBD_16\|DDBD_32 (never DDBD_8: XP strips it); Z from CheckDepthStencilMatch (DDBD_16/24/32) |
| min/max texture, repeat, aspect, anisotropy, guard band, extents adjust, max W | D3D9 fields (consider clamping max texture to 2048-4096 for old games that overflow) |
| FVF caps | min(D3D9 texcoord count, 8) \| DONOTSTRIPELEMENTS if set |
| TextureOpCaps | D3D9 TextureOpCaps & 0x00FFFFFF |
| blend stages / simultaneous textures | min(D3D9, 8) |
| HAL fixed values | dwMaxActiveLights 0xFFFFFFFF, wMaxUserClipPlanes 6, wMaxVertexBlendMatrices 4, dwVertexProcessingCaps 0x3F |
| T&L HAL | D3D9 MaxActiveLights, MaxUserClipPlanes, MaxVertexBlendMatrices (1 → 0), VertexProcessingCaps & 0x3F |
| deviceGUID, reserved | the device GUID; 0 |

**HAL Z formats**: D3D9 depth formats that pass CheckDeviceFormat + CheckDepthStencilMatch for the display format,
listed like a DX7 driver's GetDriverInfo list (16-bit first, then 24/32-bit, then 24+8 stencil), each as a
DDPF_ZBUFFER(|STENCILBUFFER) pixel format with dwZBufferBitDepth = total bits.

**Pitfalls**
- The FPU control word is per thread: set it on the game's thread in CreateDevice (step 7 above), and create the D3D9
  device with D3DCREATE_FPU_PRESERVE so that D3D9 doesn't change the presenter thread's FPU on its own.
- HAL reports unlimited lights (−1) and 4 blend matrices with software T&L; D3D9 hardware vertex processing caps
  lights at MaxActiveLights (usually 8). A HAL/RGB device needs software vertex processing (or CPU lighting) when
  more lights are enabled.
- Memory rules a game relies on: HAL/T&L HAL need render target and Z in video memory (else SURFACENOTINVIDMEM /
  ZBUFF_NEEDS_VIDEOMEMORY); RGB needs a system-memory Z (ZBUFF_NEEDS_SYSTEMMEMORY) but accepts any render target.
  dd2d9's GetSurfaceDesc caps (VIDEOMEMORY/SYSTEMMEMORY) must be the ones these checks see; RGB rendering then
  means rendering into system-memory surfaces through the GPU with read-back.
- RGB accepts an 8-bit palettized render target (needs a palette, else NOPALETTEATTACHED). D3D9 can't render to P8:
  render to X8R8G8B8 and quantize on read-back, or refuse with INVALIDPIXELFORMAT (a deliberate difference).
- XP rejects render targets wider or taller than 2048 (INVALIDOBJECT). dd2d9 may expose larger modes; decide
  whether to keep the limit (faithful) or use D3D9's MaxTextureWidth (works at modern resolutions).
- T&L HAL is what most DX7 games pick; with HWTRANSFORMANDLIGHT cleared in HAL's dwDevCaps, games that test
  that bit on HAL fall back to T&L HAL, so both must be offered.
- Refcounts: Wine's tests check that the render target's count rises by one on CreateDevice, that GetDirect3D and
  CreateVertexBuffer raise the Direct3D/DirectDraw count, and that VB QueryInterface does not.
- After Flip, the device's render target is the surface object, not its old memory; with dd2d9's buffer swapping,
  the D3D9 render target must be re-resolved from the surface after every Flip (SurfaceFlipNotify's job on XP).

## Not determined

- EnumDevices' return value after an exception: the catch path returns an uninitialised local (`ebp-0x22c`,
  7391e240).
- The DDHAL_CALLBACKS member at +0x37c that marks a DX7-DDI driver (probably a DX7 Miscellaneous2 callback such as
  CreateSurfaceEx); and the GBL dwFlags bit 0x00800000 (driver texture management; presumably from
  DDCAPS2_CANMANAGETEXTURE).
- How many internal execute-buffer surfaces exist right after CreateDevice (so the exact rise of the DirectDraw
  refcount), and whether a VB created through a v1/v2 DirectDraw INT (`+0x34`) holds a DirectDraw reference at all.
- Whether XP SP3 ships d3dref.dll (if it did, the registry keys would expose Ref/Null).
- CreateVertexBuffer with a NULL desc or out pointer: no explicit check; the fault happens inside the method's EH
  frame, whose catch value wasn't traced.
- What the extended-caps fields of the HAL desc contain when a driver supplies no `lpD3DExtendedCaps` (left
  uninitialised by Convert).
- Exact meaning of `lpSurfMore+0x70/+0x74` beyond its use as a batch stamp, and of DDSCAPS2 0x20/0x40 on execute
  buffers (probably VERTEXBUFFER/COMMANDBUFFER hints).
