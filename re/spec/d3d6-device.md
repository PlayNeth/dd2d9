# IDirect3DDevice, IDirect3DDevice2, IDirect3DDevice3 (d3dim.dll, DirectX 3-6)

Reference: `d3dim.dll` 5.1.2600.0 (XP SP3), Ghidra decompile with PDB public names in
`$D` = `$S/d3d-decomp/d3dim-named/` (`$S` = the session scratchpad). File names are
`<ShortMethod>_<address>.c`; classes share method names, so the address picks the class
(`$S/d3d-decomp/d3dim-functions.txt`). Line numbers are those of the `.c` files. Values Ghidra lost
(mangled calls in `stateInitialize`, table contents) were read from the disassembly of
`$S/xpd3d/d3dim.dll` and are cited as `@6dexxxxx`. Vtables were dumped from the binary (see "Object
layout"). IDirect3D*/CreateDevice, and the viewport, material, light, texture, execute-buffer and
vertex-buffer objects' own methods are other specs; this one covers every method of the three device
interfaces except creation. Models: `d3d7-device-state.md` (S) and `d3d7-draw-vb.md` (V), for
d3dim700; this spec says "as S §x" where d3dim.dll does the same and spells out the differences.

## Conventions

- **Ghidra shifts stdcall parameters by one** (`param_1` = This, `param_2` = first argument, the last
  argument appears as `in_stack_000000NN`). Below, arguments are named by the real API.
- **This is an interface pointer, not the object.** One C++ object (`CDirect3DDeviceIDP2`, 0x1e38
  bytes) carries the three interfaces: IDirect3DDevice (v1) at `dev+0`, IDirect3DDevice2 at `dev+8`,
  IDirect3DDevice3 at `dev+0xc` (`DIRECT3DDEVICEI_6dea627f.c:8-10`, `CDirect3DDeviceIDP2_6dea62d7.c:30-33`).
  A method implemented for Device2 sees `This = dev+8`, so the decompile's offsets there are 8 less than
  the object offsets used in this spec; Device3 methods are 0xc less. "`This == NULL`" checks therefore
  compare `This` with 8 or 0xc (e.g. `GetRenderState_6deb18e8.c:23`, `GetTexture_6deb1992.c:24`).
- **Locking.** Methods that use `CLockD3DMT` (`CLockD3DMT_6dea9c44.c:8-14`) take the DirectDraw lock only
  when `dev+0xf04 & 0x10` (multithreaded), but read `dev+0xf04` first, so their `This == NULL` checks are
  unreachable (they fault first), as S §Conventions. Methods that call `_AcquireDDThreadLock` directly
  (BeginScene, EndScene, viewports, matrices, GetCaps, SetRenderTarget, ...) can return
  DDERR_INVALIDOBJECT for a NULL This.
- **No pointer validation** beyond explicit NULL checks (listed per method); no range validation of stage
  numbers, texture-stage-state types, matrix/material handles. A bad pointer faults.
- **One device class.** `Direct3DCreateDevice` only ever makes `CDirect3DDeviceIDP2`
  (`Direct3DCreateDevice_6dea6324.c:71-76, 110-116`). Its "DDI level" `dev+0xf58` is 2 (constructor,
  `CDirect3DDeviceIDP2_6dea62d7.c:34`) or 3 for a HAL device whose driver sets a DX6 capability bit
  (`Direct3DCreateDevice_6dea6324.c:109-118`). Both levels use the DrawPrimitives2 command buffer; level 3
  additionally sends SCENECAPTURE as render state 62, takes SetRenderTarget as a DP2 op and swaps texture
  handles itself (see those methods). Software devices (RGB, MMX, Ref) are level 2; their "driver" is the
  rasterizer built into d3dim.dll (`D3DContext::*`, see §2.3).
- **"Passed to the driver"** = appended to the DP2 command buffer: RENDERSTATE op 8
  (`SetRenderStateI_6deaa6d8.c:17-49`), TEXTURESTAGESTATE op 0x19 (`SetTSSI_6deaac84.c:17-48`), VIEWPORTINFO
  op 0x1c (`UpdateDrvViewInfo_6deaae9a.c:20-29`), WINFO op 0x1d (`UpdateDrvWInfo_6deaaf2f.c:22-41`).
- **Interface version** `dev+4` = the version the device was created for: 1 (IDirectDrawSurface QI,
  aggregated), 2 (IDirect3D2::CreateDevice), 3 (IDirect3D3::CreateDevice) (`Init_6dea5b06.c:36`; the
  value is Direct3DCreateDevice's last argument, `Direct3DCreateDevice_6dea6324.c:44,85`). Several defaults
  and behaviours depend on it, **whichever interface the call comes through** (§10).
- "HAL device" = `dev+0xf1c & 8`, set for IID_IDirect3DHALDevice (`Init_6deab45e.c:24-34`).

### Object layout and vtables

Vtables (dumped by scanning `.text` for runs of function addresses; the `??_7` symbols are not in this
old-format PDB):

| Address | Interface | Notes |
| --- | --- | --- |
| 0x6dea15e8 | IDirect3DDevice (v1), CDirect3DDeviceIDP2 | QI, AddRef, Release, Initialize, GetCaps, SwapTextureHandles, CreateExecuteBuffer, GetStats, Execute, AddViewport, DeleteViewport, NextViewport, Pick, GetPickRecords, EnumTextureFormats, CreateMatrix, SetMatrix, GetMatrix, DeleteMatrix, BeginScene, EndScene, GetDirect3D, then internal virtuals (GetCapsI +0x58, destructor +0x5c, Init +0x60, FlushStates +0x64, ExecuteI +0x68, ... ProcessPrimitive +0x70, CheckSurfaces +0x74, UpdateDrvViewInfo +0x78, UpdateDrvWInfo +0x7c, SetRenderStateI +0x80) |
| 0x6dea14b8 | IDirect3DDevice3 (42 slots) | then IDirect3DDevice2 at 0x6dea1560 (adjacent) |
| 0x6dea1308 | CDirect3DDeviceUnk (IUnknown, at `dev+0xf98`) | non-delegating QI/AddRef/Release |

Which code each slot runs ("thunk" = an `adjustor{n}` that subtracts n from This and jumps):

| Method | v1 | v2 | v3 |
| --- | --- | --- | --- |
| QI, AddRef, Release, GetCaps, GetStats, BeginScene, EndScene | own | thunk → v1 | thunk → v1 |
| EnumTextureFormats | own (DDSURFACEDESC cb) | thunk → v1 | own (DDPIXELFORMAT cb) |
| SwapTextureHandles | wrapper → v2 | own | — |
| AddViewport, DeleteViewport, NextViewport | wrapper → v3 | wrapper → v3 | own |
| GetDirect3D | wrapper → v3 | wrapper → v3 | own |
| SetCurrentViewport, GetCurrentViewport, SetRenderTarget, GetRenderTarget, Begin, BeginIndexed | — | own/wrapper (see methods) | own |
| Vertex, Index, End, Get/SetRenderState, Get/SetLightState, Set/Get/MultiplyTransform, Set/GetClipStatus | — | own | thunk → v2 |
| DrawPrimitive, DrawIndexedPrimitive | — | own (D3DVERTEXTYPE) | own (FVF) |
| Strided, VB draws, ComputeSphereVisibility, Get/SetTexture, Get/SetTextureStageState, ValidateDevice | — | — | own |
| Initialize, CreateExecuteBuffer, Execute, Pick, GetPickRecords, matrices | own | — | — |

(Slots from the vtable dump; thunks e.g. `SetRenderState\`adjustor{4}'_6dea61cd.c:10`,
`GetCaps\`adjustor{12}'_6dea6245.c:10`; wrappers e.g. `AddViewport_6dea4187.c:9`, `Begin_6deb413b.c:10`.)

### Device fields used in this spec (object offsets; the PDB has no types)

| Offset | Meaning (inferred) |
| --- | --- |
| `0x4` | interface version 1/2/3 (`Init_6dea5b06.c:36`) |
| `0x114` | LIGHTSTATE_MATERIAL handle; `0xc4` the material copied from it (`SetMaterial_6decd97a.c:18-25`) |
| `0x124`, `0x128`, `0x12c`, `0x130`, `0x140`, `0x144` | light states FOGMODE, FOGDENSITY, FOGSTART, FOGEND, COLORMODEL, AMBIENT (`D3DHELInst_D3DOP_STATELIGHT_6decdfa9.c:49-70`) |
| `0xf04` | 1 = in scene, 2 = between Begin and End, 0x10 = multithreaded |
| `0xf1c` | flags: 8 = HAL device, 0x80000 = COLORVERTEX, 0x8000000 = "surface lost during the scene" |
| `0xf58` | DDI level 2/3 |
| `0xf54` / `0xf50` | current viewport / serial number of the viewport data last sent to the driver |
| `0xf8c`, `0xf90`, `0xf88` | viewport list head (first, last) and count |
| `0xfec`, `0x1038`, `0x1084` | PROJECTION, VIEW, WORLD matrices (`GetTransform_6deb1e33.c:19-30`) |
| `0xe40` | draw flags (game's D3DDP_* ORed with internal bits: 0x400 = vertices have normals, 0x2000 strided) |
| `0xea4`, `0xea8`, `0xeac`, `0xeb0`, `0x12e0` | clip-status extents minx, miny, maxx, maxy; dwStatus (`SetClipStatus_6deb265b.c:23-30`) |
| `0xf5c..0xf70` | D3DSTATS (dwSize, triangles, lines, points, spans, vertices) |
| `0xfcc` / `0xfd8` / `0xfd0` / `0xfd4` | render target as IDirectDrawSurface / as IDirectDrawSurface4 / Z buffer (v1) / palette |
| `0x1504` | the controlling IUnknown (the creating surface when aggregated, else `dev+0xf98`) (`Init_6dea5b06.c:48-54`) |
| `0x1508 + s*4` | texture object bound to stage s by Device3::SetTexture |
| `0x1528 + s*4` | stage s driver texture block (1 = "send a NULL texture") |
| `0x15d0` | pointer to the render-state array, 256 DWORDs |
| `0x15d4 + (s*64 + t)*4` | texture-stage-state array, 8 × 64 |
| `0x1dd4` | number of stages the driver takes (`wMaxTextureBlendStages`, 1 for a driver without extended caps) (`InitDeviceI_6dea56b9.c:51-60`) |
| `0x130c`, `0x1408` | stored HAL and HEL D3DDEVICEDESC (`GetCapsI_6dea45b3.c:28,40`) |

HRESULTs: DDERR_INVALIDOBJECT 0x88760082, DDERR_INVALIDPARAMS 0x80070057, E_OUTOFMEMORY 0x8007000E,
E_NOTIMPL 0x80004001, E_NOINTERFACE 0x80004002, CLASS_E_NOAGGREGATION 0x80040110, DDERR_ALREADYINITIALIZED
0x88760005, DDERR_INVALIDCAPS 0x88760064, DDERR_SURFACEBUSY 0x887601AE, DDERR_SURFACELOST 0x887601C2,
DDERR_NOPALETTEATTACHED 0x8876023C, DDERR_NOTFOUND 0x887600FF, D3DERR_DEVICEAGGREGATED 0x887602C3,
D3DERR_EXECUTE_LOCKED 0x887602CA, D3DERR_TEXTURE_NO_SUPPORT 0x887602D0, D3DERR_TEXTURE_SWAP_FAILED
0x887602D6, D3DERR_INVALIDVERTEXTYPE 0x887602E1, D3DERR_SCENE_IN_SCENE / NOT_IN_SCENE / BEGIN_FAILED /
END_FAILED 0x887602F8-FB, D3DERR_INBEGIN 0x88760302, D3DERR_NOVIEWPORTS 0x88760304,
D3DERR_VIEWPORTDATANOTSET 0x88760305, D3DERR_NOCURRENTVIEWPORT 0x88760307.

---

## 1. Render states

### 1.1 SetRenderState (v2, v3) — `SetRenderState_6deb1737.c`

1. `state == 0` or `state > 255` → **DDERR_INVALIDPARAMS** (lines 23-28). This is the only check; no value
   check, **no retired-state check for any interface version** (d3dim.dll has no retired list: every
   DX3-DX6 state is legal on Device2 and Device3).
2. **FLUSHBATCH (50)**: the command buffer is flushed (vtable +0x64), the flush result is returned; the
   value is **not stored** (lines 29-47), so GetRenderState(50) keeps reading the initial value.
3. **WRAPU (5) / WRAPV (6)**: WRAP0 (128) gets bit D3DWRAP_U (1) / D3DWRAP_V (2) set (value ≠ 0) or cleared
   (value 0); if WRAP0 changed it is stored and passed to the driver (lines 49-62). Then WRAPU/WRAPV itself
   continues to step 5. (The reverse, WRAP0 → WRAPU/WRAPV, exists at lines 63-73 but only runs when the DDI
   level is < 2, which never happens: dead code.)
4. (no special case for any other state in the front end)
5. If the value equals the stored one → **D3D_OK**, nothing sent (lines 74-75; the "`1 < DDI`" condition is
   always true).
6. Otherwise `SetDeviceRenderState` stores it (`SetDeviceRenderState_6deb1556.c:31`), and the state is
   passed to the driver; the result of appending (a flush error at worst) is returned (lines 76-82).
   Side effects in `SetDeviceRenderState`: TEXTUREHANDLE (1) switches the front end's "legacy texture"
   flag (`dev+0xe26 & 1`, i.e. one texture coordinate set is produced) and invalidates the vertex-format
   cache when texturing turns on/off (lines 12-22); FOGENABLE (28) re-evaluates vertex fog (`SetFogFlags`,
   lines 23-26); SPECULARENABLE (29) marks lighting dirty (lines 27-30).

So, unlike d3dim700 (S §SetRenderState), **d3dim.dll does not translate or emulate any render state**:
TEXTUREHANDLE, TEXTUREMAPBLEND, TEXTUREMAG/MIN, TEXTUREADDRESS/U/V, BORDERCOLOR, MIPMAPLODBIAS, ANISOTROPY,
COLORKEYENABLE, WRAPU/V, STIPPLE*, ROP2, ... are stored and handed to the driver as render states. The
**driver** maps the legacy texture states onto stage 0; §1.4 gives Microsoft's own mapping (the software
rasterizer in d3dim.dll), which is what dd2d9 must implement in its place.

### 1.2 GetRenderState (v2, v3) — `GetRenderState_6deb18e8.c`

`state == 0` or `> 255` → DDERR_INVALIDPARAMS (lines 21, 38-43); `This == NULL` → DDERR_INVALIDOBJECT
(lines 23-28, unreachable); otherwise `*pValue = array[state]`, D3D_OK (lines 31-35). No NULL check on
pValue. Every state, legacy ones included, reads back what was last set through SetRenderState (or the
default); a state never set reads **0xFFFFFFFF** (array initialisation, §1.3). The value the driver derived
(e.g. the stage-0 states a TEXTUREMAPBLEND produced) is never visible through GetRenderState or
GetTextureStageState.

(Not determined: for HAL devices at DDI ≥ 2 the array is the buffer set up at context creation
(`Init_6dea5b06.c:189-196`, `halCreateContext_6dea816a.c:52-53,82`) and might be written by the driver.)

### 1.3 Defaults at creation — `DIRECT3DDEVICEI::stateInitialize` (`stateInitialize_6dea3654.c`, disassembly `@6dea3654-6dea3c95`)

Called at the end of device Init with `zenable = (the render target has an attached Z buffer)`
(`Init_6dea5b06.c:245`). The render-state array is filled with 0xFFFFFFFF (`@6dea36d8-6dea36e6`); entries
13, 58, 59 are pre-set to 0 (`@6dea36e8-6dea3715`) so that their ~0 defaults count as changes. Every
default then goes through the **public Device2 SetRenderState** (vtable +0x5c of `dev+8`), so the driver
receives all of them and step 3 of §1.1 applies to 5/6.

| State | Default | Where |
| --- | --- | --- |
| TEXTUREHANDLE 1 | 0 | `@6dea370f-371b` |
| ANTIALIAS 2 | 0 | `@6dea3720` |
| TEXTUREADDRESS 3 | WRAP (1) | `@6dea372c` |
| **TEXTUREPERSPECTIVE 4** | **TRUE for version 3, FALSE for versions 1 and 2** | `@6dea3733-6dea374c` |
| **SPECULARENABLE 29** | **TRUE for versions 1 and 2, FALSE for version 3** | `@6dea3742-6dea3752` |
| WRAPU 5 / WRAPV 6 | 0 / 0 (also WRAP0 bits) | `@6dea3757-3764` |
| ZENABLE 7 | Z buffer attached ? TRUE : FALSE | `@6dea3767` |
| FILLMODE 8 / SHADEMODE 9 | SOLID / GOURAUD | `@6dea3778-3787` |
| LINEPATTERN 10 | 0 | `@6dea378c-379d` |
| ROP2 12 / PLANEMASK 13 | 13 (R2_COPYPEN) / 0xFFFFFFFF | `@6dea37a2-37b1` |
| ZWRITEENABLE 14 / ALPHATESTENABLE 15 / LASTPIXEL 16 | TRUE / FALSE / TRUE | `@6dea37b6-37cc` |
| SRCBLEND 19 / DESTBLEND 20 | ONE / ZERO | `@6dea37d1-37df` |
| CULLMODE 22 / ZFUNC 23 | CCW / LESSEQUAL | `@6dea37e4-37f3` |
| ALPHAREF 24 / ALPHAFUNC 25 | 0 / ALWAYS | `@6dea37f8-3806` |
| DITHERENABLE 26 / ALPHABLENDENABLE 27 / FOGENABLE 28 | FALSE | `@6dea380b-3821`, again `@6dea38b0` |
| ZVISIBLE 30, SUBPIXEL 31, SUBPIXELX 32, STIPPLEDALPHA 33, FOGCOLOR 34, FOGTABLEMODE 35 | 0 | `@6dea3826-3857` |
| FOGTABLESTART 36 / END 37 / DENSITY 38 | 0.0f / 1.0f / 1.0f | `@6dea385a-388d` |
| STIPPLEENABLE 39 / EDGEANTIALIAS 40 | 0 / 0 | `@6dea3892-389f` |
| **COLORKEYENABLE 41** | **TRUE when version < 2 and (HAL device or IID_IDirect3DRefDevice); else FALSE** | `@6dea36ac-6dea36d0`, `@6dea38a2-38ab` |
| BORDERCOLOR 43 | 0 | `@6dea38b9` |
| TEXTUREADDRESSU 44 / V 45 | WRAP / WRAP | `@6dea38c2-38cf` |
| MIPMAPLODBIAS 46 / ZBIAS 47 / RANGEFOGENABLE 48 | 0 | `@6dea38d4-38ea` |
| ANISOTROPY 49 | 1 | `@6dea38ef` |
| STIPPLEPATTERN00-31 (64-95) | 0 | `@6dea38f8-6dea3a13` |
| STENCILENABLE 52 / FAIL 53 / ZFAIL 54 / PASS 55 / FUNC 56 / REF 57 | FALSE / KEEP / KEEP / KEEP / ALWAYS / 0 | `@6dea3a18-3a4a` |
| STENCILMASK 58 / WRITEMASK 59 / TEXTUREFACTOR 60 | 0xFFFFFFFF | `@6dea3a4f-3a68` |
| MONOENABLE 11 | FALSE if the colour model is RGB, TRUE otherwise (HAL desc if it has D3DDD_COLORMODEL, else the HEL desc; **neither → stateInitialize returns E_FAIL and creation fails**) | `@6dea3a6b-6dea3a9a`, `@6dea3c84` |
| WRAP0-7 (128-135) | 0 | `@6dea3a9d-6dea3abc` |
| TEXTUREMAG 17 / TEXTUREMIN 18 | NEAREST (1) / NEAREST (1) | `@6dea3c59-3c68` |
| TEXTUREMAPBLEND 21 | MODULATE (2) — sent **last**, after the stage states | `@6dea3c6d-3c72` |

Never set, so 0xFFFFFFFF: 0, 42, 50 (FLUSHBATCH), 51, 61-63, 96-127, 136-255.

Other initial state:
- **Light states**: SetLightState(COLORVERTEX, TRUE) (`@6dea3ad4-3ad8`); the others are never set and
  read 0 (the object comes zero-filled from `LocalAlloc(LMEM_ZEROINIT)`: `CD3DAlloc::operator_new` →
  `MallocAligned` → `D3DMalloc` → `_MemAlloc`, disassembly `@6decaba8-6decabb4`): MATERIAL 0, AMBIENT 0,
  COLORMODEL 0, FOGMODE NONE, FOGSTART/END/DENSITY 0.0.
- **Textures**: both texture arrays zeroed (`@6dea3abe-3ad0`), then for each of the 8 stages the public
  Device3 `SetTexture(s, NULL)` (`@6dea3af4-3af9`).
- **Texture stages** (all 8 stages, through the public Device3 SetTextureStageState, after filling the TSS
  array with 0xFFFFFFFF, `@6dea3adb-3ae9`; values `@6dea3b01-6dea3c47`): COLOROP MODULATE on stage 0,
  DISABLE on 1-7; COLORARG1 TEXTURE; COLORARG2 CURRENT; ALPHAOP SELECTARG1 on stage 0, DISABLE on 1-7;
  ALPHAARG1 TEXTURE; ALPHAARG2 CURRENT; BUMPENVMAT00-11 0; **TEXCOORDINDEX 0 on every stage**; ADDRESS,
  ADDRESSU, ADDRESSV WRAP; BORDERCOLOR 0; MAGFILTER POINT; MINFILTER POINT; MIPFILTER NONE (1);
  MIPMAPLODBIAS 0; MAXMIPLEVEL 0; MAXANISOTROPY 1; BUMPENVLSCALE 0; BUMPENVLOFFSET 0. Type 0 and 24-63 stay
  0xFFFFFFFF (24 = TEXTURETRANSFORMFLAGS does not exist in DX6). Stages ≥ the driver's stage count are
  stored but not sent (§3.1).
- **Transforms**: WORLD, VIEW, PROJECTION identity (`D3DFE_InitTransform_6decba0f.c:19-22`).
- **Clip status**: dwStatus 0x01FFF000, minx = miny = 2048.0, maxx = maxy = 0 (`D3DFE_Create_6dea834c.c:147-156`).
- No current viewport, no viewports, no matrices (`Init_6dea5b06.c:56-63`).

Because TEXTUREMAPBLEND = MODULATE is the last state sent, a driver that maps legacy states (§1.4) ends
creation with stage 0 = MODULATE(TEXTURE, DIFFUSE) / SELECTARG1(TEXTURE), while GetTextureStageState(0,
COLORARG2) reads CURRENT (1). On stage 0 CURRENT and DIFFUSE are the same input, so this is invisible.

### 1.4 What the driver does with the legacy states: Microsoft's mapping (`D3DContext`, d3dim's software rasterizer)

d3dim.dll's RGB/MMX rasterizer receives the same DP2 stream as a HAL. Its handling
(`D3DContext::SetRenderState`, `SetRenderState_6deb80bd.c`, called per DP2 render state by
`Dp2SetRenderStates_6deb840c.c:22-33` and per stage state by `Dp2TextureStageState_6deb9619.c:20-21` with
stage states numbered `(stage + 4) * 64 + type`, i.e. stage 0 type t = 256 + t) is the reference for what a
DX6 driver had to do. It keeps **one** set of stage-0 slots that both the legacy states and
SetTextureStageState write; the last writer wins.

| Legacy state | Effect on the driver's stage states | Lines |
| --- | --- | --- |
| TEXTUREHANDLE 1 | stage-0 texture = the handle's texture (0 = none); **while non-zero, only stage 0 is active** (stages ≥ 1 ignored whatever their COLOROP) | `SetRenderState_6deb80bd.c:90-107`, `UpdateActiveTexStageCount_6deb7d56.c:16-29` |
| TEXTUREADDRESS 3 | stage-0 ADDRESS, ADDRESSU **and** ADDRESSV | 85-88 |
| WRAPU 5 / WRAPV 6 | WRAP0 bit 0 / bit 1 | 62-83 |
| TEXTUREMAG 17 | MAGFILTER = LINEAR if D3DFILTER_LINEAR (2), else POINT | `MapLegacyTextureFilter_6deb77e1.c:9-14` |
| TEXTUREMIN 18 | NEAREST 1 → MIN POINT, MIP NONE; LINEAR 2 → LINEAR, NONE; MIPNEAREST 3 → POINT, POINT; MIPLINEAR 4 → LINEAR, POINT; LINEARMIPNEAREST 5 → POINT, LINEAR; LINEARMIPLINEAR 6 → LINEAR, LINEAR; other values: no change | `MapLegacyTextureFilter_6deb77e1.c:15-46` |
| TEXTUREMAPBLEND 21 | see next table; also stage 1 COLOROP = DISABLE | `MapLegacyTextureBlend_6deb7f66.c` |
| BORDERCOLOR 43 | stage-0 BORDERCOLOR | 10-12 |
| TEXTUREADDRESSU 44 / V 45 | stage-0 ADDRESSU / ADDRESSV | 154-156, 158, 183 |
| MIPMAPLODBIAS 46 | stage-0 MIPMAPLODBIAS | 159-161 |
| ANISOTROPY 49 | **ignored** (stored only; no MAXANISOTROPY mapping) | 163-166 |
| COLORKEYENABLE 41 | read directly by the span code (keyed texels dropped) | — |
| stage state ADDRESS (256+12, and 320+12 for stage 1) | also sets ADDRESSU and ADDRESSV of that stage | 150-152, 183; 111-113 |
| SetTexture on stage 0 / 1 (types 256/320) | **clears TEXTUREHANDLE** when it binds a texture; a NULL stage-0 texture while TEXTUREHANDLE ≠ 0 keeps the legacy texture | 172-180, 134-136 |

TEXTUREMAPBLEND (D3DTA_DIFFUSE 0, CURRENT 1, TEXTURE 2):

| Value | COLOROP (ARG1, ARG2) | ALPHAOP (ARG1, ARG2) | Lines |
| --- | --- | --- | --- |
| MODULATE 2, MODULATEMASK 6 | MODULATE (TEXTURE, DIFFUSE) | SELECTARG1 (TEXTURE, DIFFUSE) | 11-20 |
| DECALALPHA 3 | BLENDTEXTUREALPHA (TEXTURE, DIFFUSE) | SELECTARG2 (TEXTURE, DIFFUSE) | 22-28, 45-46 |
| MODULATEALPHA 4 | MODULATE (TEXTURE, DIFFUSE) | MODULATE (TEXTURE, DIFFUSE) | 30-33, 45-46 |
| ADD 8 | ADD (TEXTURE, DIFFUSE) | SELECTARG2 (TEXTURE, DIFFUSE) | 36-46 |
| DECAL 1, DECALMASK 5, COPY 7, anything else | SELECTARG1 (TEXTURE, ARG2 unchanged) | SELECTARG1 (TEXTURE, ARG2 unchanged) | 36-41, 18-20 |

MODULATE's alpha: the span setup recognises "single stage, COLOROP MODULATE (TEXTURE, DIFFUSE), ALPHAOP
SELECTARG1" and picks the *modulate-alpha* span routine when the texture has an alpha channel and the plain
*modulate* routine otherwise (`_SpanInit_6ded65e2.c:301-311`), i.e. the DX5 rule "alpha from the texture
if it has alpha, else from the vertex" (the meaning of `local_84` = "texture has alpha" is inferred;
*uncertain*).

## 2. Light states

### 2.1 SetLightState (v2, v3) — `SetLightState_6deb1c91.c`, `D3DHELInst_D3DOP_STATELIGHT_6decdfa9.c`

1. Between Begin and End (`dev+0xf04 & 2`) → **D3DERR_INBEGIN** (lines 22, 28-33).
2. Otherwise the execute-buffer handler STATELIGHT is run on `{state, value}` and its result returned
   (line 23). The handler:
   - `state > 256`: an **override** (D3DSTATE_OVERRIDE_BIAS): sets/clears the "overridden" bit of
     `state - 256` (value ≠ 0 / 0), D3D_OK (lines 25-35). Accepted through the API too.
   - a state whose override bit is set → silently ignored, D3D_OK (lines 36-37).
   - MATERIAL 1: equal to the current handle → nothing; else the internal `SetMaterial(handle)`
     (lines 39-47): handle 0 = no material; otherwise the handle is used as a pointer to the material
     block and its D3DMATERIAL (at handle+8) is copied, **no validation** (`SetMaterial_6decd97a.c:14-25`);
     power ≤ 0.001 → no specular, else a specular table (allocation failure → DDERR_INVALIDPARAMS,
     lines 27-55).
   - AMBIENT 2: stored raw; r, g, b also stored as floats 0..255; lighting dirty (lines 48-55).
   - COLORMODEL 3: stored only (lines 56-58).
   - FOGMODE 4: stored, `SetFogFlags` (lines 59-62): vertex fog is on when FOGMODE ≠ NONE **and**
     render state FOGENABLE (`SetFogFlags_6deb1438.c:7-12`).
   - FOGSTART 5 / FOGEND 6 / FOGDENSITY 7: stored; fog factors dirty when vertex fog is on (lines 63-76).
   - COLORVERTEX 8: flag `dev+0xf1c & 0x80000` (lines 77-84). **Accepted on Device2 too** (no version test).
   - 0 or 9-256 → **DDERR_INVALIDPARAMS** (lines 85-86). (State 0 first indexes the override mask with
     `0 - 1`, reading outside it; the result depends on memory: *uncertain*.)
   - then the ramp service (software Ramp devices only) is told; it returns 0 for every other device
     (`CallRampService_6dea6a4f.c:12-14`).
   No dedupe except for MATERIAL; no value validation.

### 2.2 GetLightState (v2, v3) — `GetLightState_6deb1cfc.c`

Returns MATERIAL (handle), AMBIENT (the raw D3DCOLOR), COLORMODEL, FOGMODE, FOGSTART, FOGEND, FOGDENSITY
(lines 16-39). **COLORVERTEX (8) only when the device was created as version 3**; on a version 1/2 device
→ DDERR_INVALIDPARAMS (lines 40-42). Other states → DDERR_INVALIDPARAMS (lines 44-49). No NULL check.
D3D_OK.

### 2.3 How the light states are used when drawing — `DoUpdateState_6dea9770.c`

For untransformed vertices: **lighting runs only if the vertex format has a normal, D3DDP_DONOTLIGHT is
not set and a material handle is set** (lines 12-29); otherwise the "light" bit is cleared for the draw.
When lighting, the current viewport's light list is (re)loaded if it changed (lines 14-21, §5.6). With
COLORVERTEX on, a vertex diffuse colour replaces the material diffuse and a vertex specular colour the
material specular (lines 33-40). There is no LIGHTING render state in DX6 (states 136-152 are DX7's; if a
game sets them they are only stored and passed on).

## 3. Texture stage states and textures (Device3)

### 3.1 SetTextureStageState — `SetTextureStageState_6deab516.c`

1. Value equal to the stored one → D3D_OK (lines 23-25, 42-45).
2. Store (line 26). **No check of stage, type or value**: a stage ≥ 8 writes past the 8×64 array (stage 8
   type 0 is `dev+0x1dd4`, the driver's stage count).
3. TEXCOORDINDEX on a stage that has a texture and COLOROP ≠ DISABLE, or COLOROP switching to/from DISABLE,
   invalidates the vertex-format cache (lines 27-32; `TextureStageEnabled_6dea942d.c:7-10`).
4. Passed to the driver only when `stage < dev+0x1dd4` (line 33); the append result is returned (34-39).
   Every type (0-63) is passed; ADDRESS is not split (the driver splits it, §1.4).

### 3.2 GetTextureStageState — `GetTextureStageState_6deb1b05.c`

`This == NULL` → DDERR_INVALIDOBJECT (23-28, unreachable); `*pValue = array[stage][type]` (31), D3D_OK.
No range or NULL check. Type 0 reads the driver texture handle d3dim last sent (`UpdateTextures_6deb2746.c:65-66`).

### 3.3 SetTexture — `SetTexture_6deb1a36.c`

`SetTexture(stage, IDirect3DTexture2*)`; the texture object is the interface pointer − 4.
1. Same texture as bound → D3D_OK (line 24).
2. Old texture: one reference on **its surface** is dropped (through the texture's Release when the surface
   is down to its last reference, else by decrementing the surface counts directly) (lines 25-38).
3. New texture: stored; a non-NULL texture **AddRefs its surface** (INT, LCL, GBL counts) (lines 39-54).
4. If the stage's COLOROP ≠ DISABLE, the vertex-format cache is invalidated (lines 55-58).
5. **D3D_OK always** (line 63). No stage check, no NULL-This check, nothing validated about the interface.
Nothing is sent now: at the next draw (or ValidateDevice) `UpdateTextures` uploads dirty D3D-managed
textures, obtains driver handles and sends stage-state type 0 for stages `< dev+0x1dd4` whose handle changed
(`UpdateTextures_6deb2746.c:17-73`).

### 3.4 GetTexture — `GetTexture_6deb1992.c`

`This == NULL` → DDERR_INVALIDOBJECT (24-29, unreachable); `*pp` = the stage's IDirect3DTexture2 (NULL if
none), AddRef'd through that interface (32-42); D3D_OK. No stage or NULL check.

### 3.5 Texture handles (v1, v2) vs SetTexture (v3)

v1/v2 games texture through **TEXTUREHANDLE** with handles from IDirect3DTexture(2)::GetHandle (texture
spec): the device only stores the render state and passes it on (§1.1); the handle is the driver's. Device3
games use SetTexture. Both reach the driver: the handle as render state 1, SetTexture as stage-state type 0;
the driver resolves the conflict (§1.4: last writer wins, TEXTUREHANDLE ≠ 0 restricts drawing to stage 0).
A Device3 game that also sets TEXTUREHANDLE is legal.

Texture handles are **invalidated by SetRenderTarget** (§7.1: `InvalidateHandles` destroys every driver
handle of the device and resets material texture handles that used them, `InvalidateHandles_6deb1539.c:9-12`,
`D3DI_RemoveTextureHandle_6dea65ab.c:15-31`); a game must call GetHandle again. What a stale TEXTUREHANDLE then
does is up to the driver (*not determined*).

### 3.6 SwapTextureHandles (v1, v2) — `SwapTextureHandles_6dea4e0b.c`, `SwapTextureHandles_6dea4e9f.c`

v1: NULL This or either texture NULL → DDERR_INVALIDOBJECT; otherwise the v2 method with the textures'
IDirect3DTexture2 interfaces (v1 lines 14-23). v2:
1. `This == NULL`, texture 1 NULL, texture 2 NULL → **DDERR_INVALIDOBJECT** each (lines 37-50).
2. Either texture D3D-managed (texture object +0x3c, the same test that makes GetHandle fail) or either
   without a handle on this device (GetHandle never called) → **DDERR_INVALIDPARAMS** (lines 55-60, 103-104).
3. Flush (error returned, line 61-63).
4. DDI 2: the driver's TextureSwap callback; DDI 3: d3dim swaps the two surfaces' driver handles in
   DirectDraw's tables and notifies the driver itself (lines 64-99). Failure → **D3DERR_TEXTURE_SWAP_FAILED**.
5. The two handle values are exchanged in the texture blocks (lines 68-73), D3D_OK. Game-visible result: the
   handle the game holds for texture A now draws B and vice versa; GetHandle returns the swapped values.

## 4. Transforms and matrices

### 4.1 SetTransform / GetTransform / MultiplyTransform (v2, v3) — `SetTransform_6deb1dc1.c`, `GetTransform_6deb1e33.c`, `MultiplyTransform_6deb1e99.c`

Only WORLD 1, VIEW 2, PROJECTION 3 exist.
- **SetTransform**: any other type → **DDERR_INVALIDPARAMS** (line 28) — unlike DX7, which ignores bad
  types with D3D_OK. Copies the matrix, marks it dirty (`D3DFE_SetMatrixWorld_6decbf91.c`, `D3DFE_SetMatrixView_6decbf70.c`, `D3DFE_SetMatrixProj_6decbf4f.c`). PROJECTION also sends
  the driver's W range (WINFO: `wnear = m44 − m43/m33·m34`, `wfar = (m44 − m43)/(m33 − m34)·m34 + m44`, or
  0/1 when m33 = m34 or m33 = 0) and returns that append's result (lines 23-25;
  `UpdateDrvWInfo_6deaaf2f.c:29-41`). No NULL check.
- **GetTransform**: bad type → DDERR_INVALIDPARAMS; else 64 bytes, D3D_OK (lines 19-36).
- **MultiplyTransform**: bad type → DDERR_INVALIDPARAMS; else `new = pMatrix × current`
  (`MatrixProduct(dst, a, b)` = a·b, `MatrixProduct_6decbfb2.c:8-12`; `D3DFE_MultMatrixWorld_6deccae7.c:7-9`),
  D3D_OK. PROJECTION through MultiplyTransform does **not** resend the W range (lines 28 vs SetTransform 25).

### 4.2 v1 matrices: CreateMatrix / SetMatrix / GetMatrix / DeleteMatrix — `CreateMatrix_6dea5078.c`, `SetMatrix_6dea5105.c`, `GetMatrix_6dea518d.c`, `DeleteMatrix_6dea521f.c`

A matrix handle is a pointer to a heap block (D3DMATRIX + list links) linked into the device
(`D3DHAL_MatrixCreate_6decb561.c:11-23`). **Handles are not validated**: any non-zero value is
dereferenced.

| Method | Checks, in order | Effect |
| --- | --- | --- |
| CreateMatrix(lpHandle) | This NULL → DDERR_INVALIDOBJECT; lpHandle NULL → DDERR_INVALIDPARAMS; allocation failure → **E_OUTOFMEMORY** (`*lpHandle = 0`) | new identity matrix, `*lpHandle` = its address (lines 17-42) |
| SetMatrix(h, m) | This NULL → INVALIDOBJECT; m NULL → INVALIDPARAMS; h 0 → INVALIDPARAMS | copy; if h is the matrix currently bound as WORLD/VIEW/PROJECTION by an execute buffer (D3DOP_STATETRANSFORM), that device transform is updated at once (`D3DHAL_MatrixSetData_6decb5df.c:11-28`) |
| GetMatrix(h, m) | This NULL → INVALIDOBJECT; m NULL → INVALIDPARAMS; then **m is zeroed**; h 0 → INVALIDPARAMS | copy (lines 15-39) |
| DeleteMatrix(h) | This NULL → INVALIDOBJECT; h 0 → INVALIDPARAMS | unlink, free (`D3DHAL_MatrixDestroy_6decb5b1.c:7-16`); **no check that it is still bound** by STATETRANSFORM (dangling) |

All four take the DirectDraw lock themselves, so the NULL-This check works. The device's WORLD/VIEW/
PROJECTION (§4.1) and v1 matrices meet only through execute buffers (D3DOP_STATETRANSFORM, MATRIXLOAD,
MATRIXMULTIPLY: execute-buffer spec).

## 5. Viewports on the device

The viewport object (viewport spec) holds: owning device `vp+0x14`, list links `vp+0x18/0x1c`, light list
(`vp+0x20` count, `vp+0x24` head), a data serial number `vp+0x2c`, a D3DVIEWPORT2 copy at `vp+0x30`
(D3DVIEWPORT data set through SetViewport is kept in that form; how is the viewport spec's business),
"data set" `vp+0x5c`, background material `vp+0x64`, "lights changed" `vp+0x70`.

### 5.1 AddViewport — `AddViewport_6dea4187.c` (v1), `_6dea419b.c` (v2), `_6dea41af.c` (v3)

v1 and v2 call the v3 method with the same pointer (the viewport interfaces share one pointer).
1. **No NULL check** (NULL faults).
2. Viewport already attached to a device — **any** device, this one included → **DDERR_INVALIDPARAMS**
   (lines 11, 15-16).
3. Inserted at the head of the device's circular list, `vp->device = dev`, count + 1
   (`hookViewportToDevice_6dea4150.c:10-21`), **AddRef** on the viewport (line 13). D3D_OK.

### 5.2 DeleteViewport — `DeleteViewport_6dea4210.c` (v3; v1/v2 forward)

1. NULL → DDERR_INVALIDPARAMS (15-18). 2. Between Begin and End → D3DERR_INBEGIN (20, 54-56).
3. Not attached to this device → DDERR_INVALIDPARAMS (21, 50-52).
4. Unlinked, count − 1, `vp->device = NULL` (22-38).
5. If it is the current viewport: the current viewport becomes NULL (and the "downloaded" serial 0); the
   **current-viewport reference is released only on a version 3 device** (`dev+0xe24 & 0x20` is set for
   versions < 3, `D3DFE_Create_6dea834c.c:70-72`) (39-46).
6. The AddViewport reference is released (47). D3D_OK.

### 5.3 NextViewport — `NextViewport_6dea4332.c` (v3; v1/v2 forward)

`NextViewport(lpRef, lplpNext, dwFlags)`:
1. lplpNext NULL → DDERR_INVALIDPARAMS (18-19); `*lplpNext = NULL` (22).
2. This NULL → DDERR_INVALIDOBJECT (23, 70).
3. D3DNEXT_NEXT (1): lpRef NULL or not attached to this device → DDERR_INVALIDPARAMS (24-33).
4. No viewports → **D3DERR_NOVIEWPORTS** (35-36) (before the flag check: a bad flag on an empty list gives
   this code).
5. Between Begin and End → D3DERR_INBEGIN (38, 63-64).
6. NEXT → the viewport added after lpRef; HEAD (2) → the first one added; TAIL (4) → the last one added
   (the list is head-inserted and walked backwards, lines 39-51); any other flag → DDERR_INVALIDPARAMS.
   Past the end → `*lplpNext = NULL` and **D3D_OK** (52-56). A viewport returned is AddRef'd (57-60).

### 5.4 SetCurrentViewport (v2, v3) — `SetCurrentViewport_6deb15ce.c` (v3), `SetCurrentViewport_6deb1468.c` (v2 → v3)

1. This NULL → DDERR_INVALIDOBJECT (15-16). 2. Viewport NULL → DDERR_INVALIDPARAMS (19, 47).
3. Between Begin and End → D3DERR_INBEGIN (20, 41-42). 4. Not attached to this device → DDERR_INVALIDPARAMS (21, 37-38).
5. It becomes current. If its data were never set, or are already the ones the driver has, nothing more;
   otherwise they are sent now (`downloadView`); a failure restores the previous current viewport and is
   returned (22-35).
6. The previous current viewport is Released and the new one **AddRef'd — on every device version**
   (27-30). Together with §5.2 step 5 this means a version 1/2 device leaks one reference on a current
   viewport that is deleted (observed; whether DX5 games depend on it is *not determined*).
Setting a viewport whose data were never set succeeds; drawing then fails (§5.6).

### 5.5 GetCurrentViewport (v2, v3) — `GetCurrentViewport_6deb16a6.c` (v3), `_6deb147c.c` (v2 → v3)

This NULL → DDERR_INVALIDOBJECT; between Begin and End → D3DERR_INBEGIN; none → **D3DERR_NOCURRENTVIEWPORT**;
else AddRef and store, D3D_OK (lines 15-34). The out pointer is not checked (the AddRef happens, then the
store faults).

### 5.6 How the current viewport reaches the device — `downloadView_6deab934.c`, `UpdateViewportCache_6decb659.c`, `CheckDeviceSettings_6dea3597.c`, `ProcessPrimitive_6deb38c0.c`, `setLights_6deab9f3.c`

- Every draw (`ProcessPrimitive_6deb38c0.c:32-35`), Begin/BeginIndexed (`CheckBegin_6deb406a.c:14`) and
  Execute/Pick (`Execute_6dea444e.c:50`) compare the current viewport's serial number with the one last sent
  and call `downloadView` when they differ. **None of them checks that there is a current viewport: drawing
  with none dereferences NULL** (faults). `downloadView`: data never set → **D3DERR_VIEWPORTDATANOTSET**
  (returned by the draw); else the viewport transform is recomputed, VIEWPORTINFO (dwX, dwY, dwWidth,
  dwHeight) goes to the driver, the viewport's lights are marked for reloading and the serial recorded
  (`downloadView_6deab934.c:10-21`).
- **The viewport transform** (`UpdateViewportCache_6decb659.c:19-108`) from the D3DVIEWPORT2 (dwX, dwY, dwWidth,
  dwHeight, dvClipX, dvClipY, dvClipWidth, dvClipHeight, dvMinZ, dvMaxZ), skipped entirely (previous
  transform kept) when dwWidth, dwHeight, dvClipWidth or dvClipHeight is 0 or dvMaxZ = dvMinZ (19-22).
  For a projected point (x, y, z, w):
  - normalised: `xn = (x − dvClipX·w)/dvClipWidth`, `yn = (y − dvClipY·w)/dvClipHeight + w`,
    `zn = (z − dvMinZ·w)/(dvMaxZ − dvMinZ)` (lines 43-54), so the clip volume is
    `dvClipX ≤ x/w ≤ dvClipX + dvClipWidth`, `dvClipY − dvClipHeight ≤ y/w ≤ dvClipY`,
    `dvMinZ ≤ z/w ≤ dvMaxZ`;
  - screen: `sx = dwX + dwWidth·xn/w`, `sy = dwY + dwHeight − dwHeight·yn/w`, `sz = zn/w` ∈ [0, 1]
    (scale/offset at lines 52-57); HAL devices add 1e-6 to the x and y offsets (lines 58-62; constant at
    0x6dea3154 = 9.99999997e-7).
  **dvMinZ/dvMaxZ select the projected z range that maps to depth 0..1; they are not a depth range** (DX7's
  D3DVIEWPORT7 MinZ/MaxZ are).
- **Lights**: before a lit draw, if the viewport's lights changed, the device's active light list is rebuilt
  from the viewport's lights that have data and D3DLIGHT_ACTIVE set, in list order, with no count limit
  (`DoUpdateState_6dea9770.c:14-21`, `setLights_6deab9f3.c:12-29`).
- **Background**: only used by the viewport's own Clear (viewport spec); the device draws nothing with it.

## 6. Scenes

### 6.1 BeginScene (all versions) — `BeginScene_6dea5292.c`

1. This NULL → DDERR_INVALIDOBJECT (16-19). 2. In a scene → **D3DERR_SCENE_IN_SCENE** (20-22).
3. `CheckSurfaces` (`CheckSurfaces_6deaa549.c:9-24`): a lost D3D-managed texture, a lost render target or Z
   buffer, or a lost internal command/vertex buffer → DDERR_SURFACELOST; the render target or Z buffer
   **locked** (usage count ≠ 0) → **DDERR_SURFACEBUSY**.
4. Any error other than DDERR_SURFACELOST is returned as is. On DDERR_SURFACELOST:
   `IDirectDraw4::TestCooperativeLevel` (the DIRECT3DI's IDirectDraw4 copy, `DIRECT3DI_6dea7dd2.c:38-54`);
   failure → **DDERR_SURFACELOST**; success → managed textures evicted and
   **`IDirectDraw4::RestoreAllSurfaces`** called (lines 39-46): **d3dim restores every surface of the
   DirectDraw object on the game's behalf** (contents lost, the game is not told). RestoreAllSurfaces failure
   → **D3DERR_SCENE_BEGIN_FAILED** (47-49).
5. SCENECAPTURE(TRUE): DDI 2 through the driver's SceneCapture callback, DDI 3 as render state 62
   (`D3DHAL_SceneCapture_6deafbd4.c:12-25`); a result other than 0/DDERR_NOTFOUND → D3DERR_SCENE_BEGIN_FAILED
   (27-28, 49).
6. In-scene flag set; texture-manager scene counter + 1 on drivers with a caps bit (29-34). D3D_OK.
Not marked dirty: textures (unlike DX7's BeginScene). No state-block test (no state blocks in DX6).

### 6.2 EndScene (all versions) — `EndScene_6dea538c.c`

1. This NULL → DDERR_INVALIDOBJECT (15, 46-47). 2. Not in a scene → **D3DERR_SCENE_NOT_IN_SCENE** (17-18).
3. The in-scene flag is cleared first (21).
4. DDI 3: SCENECAPTURE(FALSE) appended (22-24); the command buffer is flushed (25); DDI 2: SceneCapture
   callback after the flush. Flush failure or a capture result other than 0/DDERR_NOTFOUND →
   **D3DERR_SCENE_END_FAILED** (26-29, 39-41).
5. A surface lost during the scene (flag set by Execute, `ExecuteI_6deadc06.c:27-32`, and the flush) →
   flag cleared, **DDERR_SURFACELOST** (30-37). Otherwise D3D_OK. No flip/present.

## 7. Render target, caps, formats, Direct3D, statistics

### 7.1 SetRenderTarget — `SetRenderTarget_6deb1f07.c` (v2, IDirectDrawSurface), `SetRenderTarget_6deb1fb5.c` (v3, IDirectDrawSurface4)

v2: This NULL or surface NULL → DDERR_INVALIDOBJECT (22-24, 42-45); device aggregated into its current render
target (created by surface QI and still on that surface) → **D3DERR_DEVICEAGGREGATED** (28-29); the surface is
QI'd for IDirectDrawSurface4 (failure returned), that reference dropped, and the v3 method called with it
(32-36). The flags argument is passed on and ignored.

v3, in order:
1. This NULL → DDERR_INVALIDOBJECT; surface NULL → **DDERR_INVALIDOBJECT** (52-61).
2. The command buffer is flushed (error returned) (63-64).
3. Aggregated into the current target → D3DERR_DEVICEAGGREGATED (65-67).
4. GetSurfaceDesc (error returned) (69-76); no DDSCAPS_3DDEVICE or a bit depth not in the device's
   dwDeviceRenderBitDepth → **DDERR_INVALIDCAPS** (77-82); width or height > 2048 → DDERR_INVALIDPARAMS (83-86).
5. Attached Z buffer: `GetAttachedSurface(DDSCAPS_ZBUFFER)`; an error other than DDERR_NOTFOUND →
   DDERR_INVALIDPARAMS (87-89, 281-283).
6. QI for IDirectDrawSurface (error returned) and GetPalette: an error other than DDERR_NOPALETTEATTACHED →
   DDERR_INVALIDPARAMS; no palette on a target below 16 bpp → **DDERR_NOPALETTEATTACHED** (93-101).
7. **New target without a Z buffer while the device has one → DDERR_INVALIDPARAMS** (102-104). (Gaining a Z
   buffer is allowed.)
8. The driver is switched: DDI 3 through a DP2 SETRENDERTARGET (225-226); DDI 2 through its SetRenderTarget
   callback, or by destroying and recreating the driver context and resending render states 1-135 and every
   stage state (118-189).
9. References (240-268): new Z buffer (v1 interface) held, the old released; palette likewise; render target:
   **a version 2 device holds the new target's IDirectDrawSurface interface** (releasing the old), a version 3
   device holds the **IDirectDrawSurface4** (`dev+0xfd8`) and keeps the v1 pointer unreferenced; a version 1
   device (aggregated) holds neither (`D3DFE_Create_6dea834c.c:29-38` sets the same rule at creation).
10. **All texture handles of the device are invalidated** (`InvalidateHandles`, 269; §3.5).
11. The surface-info recalculation result is returned (D3D_OK normally) (270-271).
The current viewport is **not** changed or revalidated; no in-scene check.

### 7.2 GetRenderTarget — `GetRenderTarget_6deb2557.c` (v2), `GetRenderTarget_6deb25d9.c` (v3)

This NULL → DDERR_INVALIDOBJECT; out NULL → DDERR_INVALIDPARAMS; v2 returns `dev+0xfcc` (IDirectDrawSurface),
v3 `dev+0xfd8` (IDirectDrawSurface4), AddRef'd, D3D_OK (lines 14-27).

### 7.3 GetCaps (all versions) — `GetCaps_6dea459f.c`, `GetCapsI_6dea45b3.c`

`GetCaps(lpHalDesc, lpHelDesc)`; the v2/v3 slots are thunks to the same code.
1. This NULL → DDERR_INVALIDOBJECT (unreachable: GetCaps calls through This's vtable first, line 10).
2. lpHalDesc NULL, or its dwSize not 0xAC (DX3 D3DDEVICEDESC), 0xCC (DX5) or 0xFC (DX6) →
   DDERR_INVALIDPARAMS (`GetCapsI_6dea45b3.c:21-23`).
3. lpHelDesc NULL or a bad dwSize → DDERR_INVALIDPARAMS (26-27, 54).
4. Each structure gets **its own dwSize bytes** of the stored HAL / HEL descriptor (29-50), D3D_OK. **Any
   interface accepts any of the three sizes.** The copy includes offset 0, so the caller's dwSize is
   overwritten with the stored descriptor's dwSize (0xFC when the driver supplied extended caps,
   `D3DDeviceDescConvert_6dea5560.c:21-22`; *the stored value otherwise is not determined*).
The descriptors themselves come from the device's HAL provider at creation (`InitDeviceI_6dea56b9.c:21-23`;
CreateDevice/caps spec).

### 7.4 EnumTextureFormats — `EnumTextureFormats_6dea4cfb.c` (v1, v2: DDSURFACEDESC callback), `EnumTextureFormats_6dea4d80.c` (v3: DDPIXELFORMAT callback), `DoEnumTextureFormats_6dea4a6b.c`

1. This NULL → DDERR_INVALIDOBJECT; `IsBadCodePtr(callback)` → DDERR_INVALIDPARAMS (lines 16-29).
2. The driver lists no formats → **D3DERR_TEXTURE_NO_SUPPORT**; copy allocation fails → E_OUTOFMEMORY
   (`DoEnumTextureFormats_6dea4a6b.c:35-41, 114-116`).
3. Filtering, with `mode = (DirectDraw dwAppHackFlags >> 6) & 3` (`lcl+0x48`, line 54-55; 0 unless an
   app-compat entry sets it):
   - a non-FOURCC format is enumerated only if it **exactly matches** (`MatchDDPIXELFORMAT_6dea48ab.c:7-14`: flags,
     FOURCC, bit count, four masks) an entry of the built-in DX5 inclusion list or of the registry lists
     `HKLM\Software\Microsoft\Direct3D\DX5TextureEnumInclusionList` (always) and `...\DX6TextureEnumInclusionList`
     (for version 3 with mode ≠ 2, or mode 3) (lines 60-67, 77-134). Mode 1 skips this filter. **FOURCC
     formats skip it.** The built-in list (9 entries at 0x6defe010, count at 0x6defe130):
     X1R5G5B5, A1R5G5B5, R5G6B5, A4R4G4B4, A8R8G8B8, X8R8G8B8 (32-bit RGB), R3G3B2 (8-bit RGB),
     4-bit palettized (DDPF_RGB|PALETTEINDEXED4), 8-bit palettized (DDPF_RGB|PALETTEINDEXED8).
     So by default **every interface version sees only these 9 formats plus FOURCC formats** (luminance,
     bump, 24-bit RGB and alpha-only formats are hidden unless listed in the registry).
   - **version 1/2 devices**: DXT1-DXT5 are never enumerated, and on a DDI 3 driver no FOURCC format at all;
     and at most **10** formats are enumerated unless dwAppHackFlags & 0x100 (lines 138-144).
4. Callback per format in the driver's order, v1/v2 with the 0x6C-byte DDSURFACEDESC, v3 with its
   DDPIXELFORMAT; enumeration stops at the first return value ≠ D3DENUMRET_OK (lines 73, 145-150). D3D_OK.
The rules depend on the **creation version** `dev+4`, not on the interface called.

### 7.5 GetDirect3D — `GetDirect3D_6dea5464.c` (v1), `_6dea5492.c` (v2), `_6dea54d6.c` (v3)

v3: This NULL → DDERR_INVALIDOBJECT; out NULL → DDERR_INVALIDPARAMS; the owning DIRECT3DI's IDirect3D3,
AddRef'd, D3D_OK (lines 14-30). v1/v2 call the v3 method with a local and convert the result to IDirect3D /
IDirect3D2 of the same object (same reference) — **without a NULL check on their out pointer** (faults).

### 7.6 GetStats (all versions) — `GetStats_6dea40cf.c`

**Device created as version 3 → E_NOTIMPL**, whatever interface is called, without taking the lock (15,
32-34). Otherwise: NULL or dwSize ≠ 0x18 → DDERR_INVALIDPARAMS; else the six DWORDs, dwSize set, D3D_OK
(16-30). Counters: triangles, lines, points accumulate the primitive counts of every draw (`GetNumPrim_6dea96e4.c:11-39`),
vertices the vertex counts (`ProcessPrimitive_6deb38c0.c:29`); spans are never updated (*as far as found*).
No NULL-This check.

## 8. Drawing (Device2, Device3)

General: DX6 shares the DX7 structure (V §1-§7) — store the call, compute the output vertex format, count
primitives, `ProcessPrimitive`. Differences that a game can see are listed here; the transform/clip/light
details match V §5, §7, §9 except where noted.

- **No validation**: no primitive-type, count, flag, pointer, FVF or in-scene check on any draw except the
  Strided XYZRHW test. Invalid primitive type → 0 primitives (`GetNumPrim_6dea96e4.c:25`), drawn as nothing
  (*as far as read*). Draws work outside BeginScene/EndScene.
- **A current viewport is required** (§5.6): none → NULL dereference; data never set →
  D3DERR_VIEWPORTDATANOTSET.
- **D3DDP_DONOTCLIP is honoured for transformed vertices** (no clip codes, no clipping:
  `ProcessPrimitive_6deb38c0.c:39, 62-73`), and with it **D3DDP_DONOTUPDATEEXTENTS** (lines 64-66). (DX7
  ignores both, V §1.) For untransformed vertices the flags go to the transform code (*not traced*).
- **D3DDP_DONOTLIGHT** and the material handle decide lighting (§2.3).
- Primitive counts (`GetNumPrim_6dea96e4.c`): POINTLIST n, LINELIST n/2, LINESTRIP n−1 (0 if n < 2),
  TRIANGLELIST n/3, STRIP/FAN n−2 (0 if n < 3).
- The output texture-coordinate count is the largest TEXCOORDINDEX + 1 over the stages that have a texture,
  up to the first stage with COLOROP = DISABLE, capped by the input count; 1 when TEXTUREHANDLE ≠ 0
  (`ComputeOutputFVF_6dea94c8.c:23-58`).

### 8.1 Device2 DrawPrimitive / DrawIndexedPrimitive (D3DVERTEXTYPE) — `DrawPrimitive_6deb2ed5.c`, `DrawIndexedPrimitive_6deb3139.c`

- The vertex type is turned into an FVF by a table **with no range check** (`DrawPrimitive_6deb2ed5.c:36`;
  table at 0x6deff498): D3DVT_VERTEX 1 → 0x112 (XYZ|NORMAL|TEX1), D3DVT_LVERTEX 2 → 0x1E2
  (XYZ|RESERVED1|DIFFUSE|SPECULAR|TEX1), D3DVT_TLVERTEX 3 → 0x1C4 (XYZRHW|DIFFUSE|SPECULAR|TEX1); 0 → FVF 0;
  ≥ 4 reads the following data (4 → 4, 5 → 8: undefined). Stride is always 32 (line 34).
- **More than 65535 vertices** (DrawPrimitive) are split into batches of 0xFFFC (lists), 0xFFFB (line strip)
  or 0xFFFA (triangle strip/fan) vertices; for a fan the centre vertex is copied into the app's array before
  each batch and restored afterwards (lines 41-160). The remainder count is computed as if every batch had
  0xFFFC new vertices, so strips/fans of ≥ 65536 vertices lose or repeat vertices at the end (lines 72-74,
  117-118; a bug). DrawIndexedPrimitive splits on ≥ 65536 **primitives** similarly, writing the app's index
  array temporarily for fans (lines 40-120).
- DrawIndexedPrimitive with fewer indices than half the vertex count (and untransformed vertices) transforms
  only the referenced vertices (`DereferenceIndexedPrim`, lines 41-44): invisible except for speed.

### 8.2 Device3 DrawPrimitive / DrawIndexedPrimitive (FVF) — `DrawPrimitive_6deb2d65.c`, `DrawIndexedPrimitive_6deb2e1b.c`

Store type, FVF, pointer, count, flags; stride from the FVF (`GetVertexSizeFVF`); normals set the internal
"may light" bit (lines 20-32); ComputeOutputFVF, GetNumPrim, ProcessPrimitive, result returned. **No split
and no limit** on the vertex count; no FVF validation (an invalid FVF gives a wrong stride, *undefined*).

### 8.3 DrawPrimitiveStrided / DrawIndexedPrimitiveStrided (v3) — `DrawPrimitiveStrided_6deb3b22.c`, `DrawIndexedPrimitiveStrided_6deb3c5a.c`

`(fvf & 0xE) == D3DFVF_XYZRHW` → **D3DERR_INVALIDVERTEXTYPE** (lines 26-31), as DX7. Else the position,
normal, diffuse, specular pointer/stride pairs and as many texture-coordinate pairs as the FVF's count are
copied (33-55), and the draw proceeds as §8.2 with the strided bit (56-65).

### 8.4 DrawPrimitiveVB / DrawIndexedPrimitiveVB (v3, IDirect3DVertexBuffer) — `DrawPrimitiveVB_6deaa0ad.c`, `DrawIndexedPrimitiveVB_6dea9edb.c`

Signatures: `DrawPrimitiveVB(type, lpVB, dwStartVertex, dwNumVertices, dwFlags)`;
`DrawIndexedPrimitiveVB(type, lpVB, lpwIndices, dwIndexCount, dwFlags)` — **no start vertex: indices address
the whole buffer** (`DrawIndexedPrimitiveVB_6dea9edb.c:28-31`; mingw `d3d.h:1194`).
- No check of lpVB (NULL faults), of its lock state or of start + count against its size.
- The VB's own draw-flag bits are ORed in (line 30 / 28).
- Untransformed VB: as §8.2 from the VB memory (`DrawPrimitiveVB_6deaa0ad.c:35-47`).
- Transformed VB (processed/XYZRHW): the driver draws straight from the VB (StartPrimVB, the VB remembers the
  device); **without D3DDP_DONOTCLIP the vertices are clipped** using the VB's clip codes (the VB is locked
  for the call if its memory is not mapped, DDLOCK 0x811) (62-96); with DONOTCLIP they are drawn unclipped
  (97-99). (DX7 clips only VBs that have clip codes from ProcessVertices, V §4.)

### 8.5 Begin / BeginIndexed / Vertex / Index / End (v2, v3) — `Begin_6deb44e3.c`, `BeginIndexed_6deb4574.c`, `CheckBegin_6deb406a.c`, `Vertex_6deb4610.c`, `Index_6deb46a6.c`, `End_6deb4725.c`

- v2 Begin/BeginIndexed convert the D3DVERTEXTYPE with the same unchecked table and call v3
  (`Begin_6deb413b.c:10-11`, `BeginIndexed_6deb415e.c:12-13`).
- Begin(type, fvf, flags) / BeginIndexed(type, fvf, lpvVertices, dwNumVertices, flags): the current viewport
  is brought up to date (errors returned: D3DERR_VIEWPORTDATANOTSET; no viewport → fault), the batch buffer
  set up (E_OUTOFMEMORY), **the "in Begin" flag set — with no check that one is already open**
  (`CheckBegin_6deb406a.c:12-34`). Multithreaded devices enter the device critical section until End
  (`Begin_6deb44e3.c:24-26`).
- Vertex(lpVertex) / Index(wIndex): copy into a 32 KB batch (vertex size from the FVF); full → the batch is
  drawn (keeping the strip/fan continuity), an error ends the Begin/End (**no "in Begin" check**: called
  outside a Begin they write through the stale batch pointer) (`Vertex_6deb4610.c:16-47`, `Index_6deb46a6.c:16-38`).
- End(flags): the rest is drawn; **0 primitives (too few vertices, or an invalid type) → DDERR_INVALIDPARAMS**
  (`FlushBeginEndBatch_6deb3ef5.c:23-25`); the Begin state is cleared in every case (`End_6deb4725.c:16-27`,
  `CleanupBeginEnd_6deb3dbf.c:7-11`). **D3DERR_NOTINBEGIN is never returned by d3dim.dll** (grep).
- Between Begin and End: SetLightState, DeleteViewport, NextViewport, Set/GetCurrentViewport return
  D3DERR_INBEGIN; everything else (SetRenderState, transforms, textures, other draws) is allowed and not
  checked.

### 8.6 ComputeSphereVisibility (v3) — `ComputeSphereVisibility_6deb34a4.c`

Dirty transforms updated (19-26), then the geometry pipeline's routine with the same arguments (27-29): the
algorithm of S §ComputeSphereVisibility (axis-aligned box per sphere, union | intersection << 12; DX6 has no
user clip planes). Flags ignored, no checks.

### 8.7 SetClipStatus / GetClipStatus (v2, v3) — `SetClipStatus_6deb265b.c`, `GetClipStatus_6deb26e1.c`

SetClipStatus: **on a version 3 device** `dwFlags & D3DCLIPSTATUS_EXTENTS3` → DDERR_INVALIDPARAMS (16-21); on
a version 1/2 device EXTENTS3 is accepted and acts like EXTENTS2. STATUS (1) → dwStatus; EXTENTS2 or
EXTENTS3 → minx, maxx, miny, maxy (23-31). D3D_OK. GetClipStatus: dwFlags = EXTENTS2, dwStatus and the 2D
extents written, minz/maxz untouched, D3D_OK (15-24). No NULL checks. Execute buffers' D3DOP_SETSTATUS write
the same fields (`ExecuteI_6deadc06.c:132-140`).

### 8.8 ValidateDevice (v3) — `ValidateDevice_6deab5cc.c`

As S §ValidateDevice: This NULL → DDERR_INVALIDOBJECT (25-31, unreachable); textures updated, command buffer
flushed (errors returned); `*pdwPasses = 0`; the driver's ValidateTextureStageState callback: missing or not
handled → **E_NOTIMPL**; handled → its pass count and ddrval (34-57).

## 9. Execute buffers and picking (v1)

### 9.1 Execute(lpBuffer, lpViewport, dwFlags) — `Execute_6dea444e.c`, `ExecuteI_6deadc06.c`

1. This NULL → DDERR_INVALIDOBJECT; buffer NULL → **DDERR_INVALIDOBJECT** (32-41).
2. **Viewport NULL → the current viewport** (43-46) (a pure v1 device has none: fault).
3. Viewport not attached to this device → DDERR_INVALIDPARAMS (48, 97).
4. The viewport becomes current for the call and its data are sent if needed; an error (e.g.
   D3DERR_VIEWPORTDATANOTSET) restores the previous one and is returned (49-54).
5. Buffer created by another device → DDERR_INVALIDPARAMS; buffer locked → **D3DERR_EXECUTE_LOCKED** (55-56,
   91-93). In both cases **the viewport stays current** (no restore on this path: a bug).
6. Flush; the D3DEXECUTEDATA of the buffer is executed (`ExecuteI`, vtable +0x68) (57-73):
   render target or Z buffer lost → DDERR_SURFACELOST and the "lost during scene" flag (`ExecuteI_6deadc06.c:27-32`);
   textures brought up to date; `dwFlags` must be D3DEXECUTE_CLIPPED (1) or D3DEXECUTE_UNCLIPPED (2, = no
   clipping) else DDERR_INVALIDPARAMS (33-39).
7. Flush; the resulting D3DSTATUS copied back into the buffer's execute data (74-84); the previous current
   viewport restored; D3D_OK.
Device state used by the D3DOP stream (opcode details: execute-buffer spec): the render-state array and
driver through STATERENDER (respecting override bits, `trackState_6dead325.c:11-29`; the WRAPU/WRAPV → WRAP0 coupling
of §1.1 is not applied there), the light states through the same handler as SetLightState (§2.1), the
WORLD/VIEW/PROJECTION matrices bound by matrix handle (STATETRANSFORM; §4.2), the clip status (SETSTATUS,
BRANCHFORWARD tests it), the given viewport's transform and lights. No in-scene requirement.

### 9.2 Pick(lpBuffer, lpViewport, dwFlags, lpRect) / GetPickRecords(lpCount, lpRecords) — `Pick_6dea4695.c`, `GetPickRecords_6dea47d9.c`

Pick: This, buffer, viewport (**NULL is an error here**) or rect NULL → DDERR_INVALIDOBJECT (35-45, 91-94);
buffer or viewport of another device → DDERR_INVALIDPARAMS; buffer locked → D3DERR_EXECUTE_LOCKED; the
viewport made current for the call and restored (also on failure) (47-83); `D3DHAL_ExecutePick` records the
hits; **its result is ignored: D3D_OK** (75-77). dwFlags unused.
GetPickRecords: This NULL → DDERR_INVALIDOBJECT (27-31); the record count is computed; if lpRecords and lpCount
are non-NULL and `*lpCount ≥ count`, the records are copied (allocation failure → E_OUTOFMEMORY); then
**`*lpCount = count` (lpCount is dereferenced even when NULL)**, D3D_OK (33-59).

### 9.3 CreateExecuteBuffer(lpDesc, lplpBuffer, pUnkOuter) — `CreateExecuteBuffer_6deb4b39.c`

This NULL → DDERR_INVALIDOBJECT; lpDesc NULL, dwSize ≠ 0x14 or lplpBuffer NULL → DDERR_INVALIDPARAMS;
pUnkOuter ≠ NULL → **CLASS_E_NOAGGREGATION**; `*lplpBuffer = NULL`; dwFlags 0, dwBufferSize 0 or larger than
the HAL's dwMaxBufferSize (when non-zero) → DDERR_INVALIDPARAMS; allocation/creation errors returned; D3D_OK
(lines 19-70). The buffer object itself: execute-buffer spec.

## 10. QueryInterface, AddRef, Release, Initialize; versions sharing one object

- **QueryInterface / AddRef / Release** (all three interfaces, `QueryInterface_6deb0396.c`, `AddRef_6deb0406.c`,
  `Release_6deb0429.c`) forward to the controlling unknown `dev+0x1504` — **the surface** for a version 1
  (aggregated) device, else the device's own `CDirect3DDeviceUnk` (`Init_6dea5b06.c:48-54`). QI first checks
  the out pointer (NULL → DDERR_INVALIDPARAMS) and clears it (12-18).
- `CDirect3DDeviceUnk::QueryInterface` (`QueryInterface_6deb021b.c`): IID_IUnknown → the Unk; **version 1:
  only the device's own GUID** (e.g. IID_IDirect3DHALDevice) → IDirect3DDevice, anything else E_NOINTERFACE
  (43-60); **version ≥ 2**: IID_IDirect3DDevice → v1, IID_IDirect3DDevice2 → v2, IID_IDirect3DDevice3 → v3
  **only on a version 3 device** (110), else E_NOINTERFACE. Each successful device-IID QI **overwrites the
  stored device GUID `dev+0xfa4` with the IID** (121-126; only read during creation, so harmless). Release at 0
  destroys the device (`Release_6deb0363.c:11-19`): all viewports deleted (through DeleteViewport), stage
  textures released, texture handles and material blocks removed, execute buffers released, then the render
  target / Z / palette references of §7.1 (`DestroyDevice_6dea5889.c:24-138`).
- **Initialize** (v1): **DDERR_ALREADYINITIALIZED** always (`Initialize_6dea3fb1.c:8`).

Version-dependent behaviour (creation version `dev+4`, whatever interface is called):

| Behaviour | v1 (surface QI) | v2 | v3 | Where |
| --- | --- | --- | --- | --- |
| TEXTUREPERSPECTIVE default | FALSE | FALSE | TRUE | §1.3 |
| SPECULARENABLE default | TRUE | TRUE | FALSE | §1.3 |
| COLORKEYENABLE default | TRUE on HAL/Ref | FALSE | FALSE | §1.3 |
| QI for IDirect3DDevice2 / 3 | no / no | yes / no | yes / yes | §10 |
| GetLightState(COLORVERTEX) | INVALIDPARAMS | INVALIDPARAMS | OK | §2.2 |
| SetClipStatus(EXTENTS3) | accepted | accepted | INVALIDPARAMS | §8.7 |
| GetStats | OK | OK | E_NOTIMPL | §7.6 |
| EnumTextureFormats: DXTn / FOURCC on DDI 3 / max 10 / DX6 registry list | hidden / hidden / yes / no | same as v1 | shown / shown / no / yes | §7.4 |
| current viewport reference released by DeleteViewport | no | no | yes | §5.2 |
| render-target reference held | none | IDirectDrawSurface | IDirectDrawSurface4 | §7.1 |
| Z buffer / palette references held | no | yes | yes | `Init_6dea5b06.c:152-162` |

---

## For dd2d9: what to implement

1. **One device object, three interfaces** (v1 IDirect3DDevice, v2, v3) over one state store, created with a
   version 1/2/3; QI rules of §10 (v1: own GUID only; v3 only on version 3 devices). AddRef/Release to the
   surface for version 1 (aggregated) devices.
2. **Front-end state store** exactly as d3dim keeps it: 256 render states initialised to 0xFFFFFFFF then the
   §1.3 defaults (version-dependent TEXTUREPERSPECTIVE, SPECULARENABLE, COLORKEYENABLE; MONOENABLE FALSE for
   RGB devices), 8×64 stage states (§1.3 values, TEXCOORDINDEX 0 everywhere), light states (COLORVERTEX TRUE,
   others 0), WORLD/VIEW/PROJECTION identity, clip status 0x01FFF000/2048/2048/0/0. All Get* answer from it.
3. **SetRenderState**: 1..255 accepted on Device2 and Device3 (no retired states); 0 or > 255 →
   DDERR_INVALIDPARAMS; FLUSHBATCH not stored; WRAPU/WRAPV update WRAP0's bits; **skip when equal to the stored
   value** (this dedupe matters for §1.4 emulation); store; forward to the emulated driver (item 4).
4. **Emulate the driver's legacy-state mapping (§1.4)** in dd2d9's D3D9 layer, not in the front end: a separate
   "driver" copy of stage 0/1 states that both legacy render states and forwarded stage states write, last
   writer wins; TEXTUREMAPBLEND table, TEXTUREMAG/MIN table, TEXTUREADDRESS → ADDRESS/U/V, U/V/BORDERCOLOR/
   LODBIAS → stage 0, ANISOTROPY ignored, WRAPU/V → WRAP0, stage ADDRESS → U and V, TEXTUREHANDLE ≠ 0 → stage-0
   texture and only stage 0 active, SetTexture on stage 0/1 clears TEXTUREHANDLE. Never let this change what
   GetRenderState/GetTextureStageState return.
5. **SetLightState/GetLightState** per §2 (override states > 256, INBEGIN between Begin/End, COLORVERTEX Get
   only on version 3, MATERIAL handle dedupe); lighting only with a normal, a material and no D3DDP_DONOTLIGHT.
6. **Stage states / SetTexture (v3)** per §3: no validation in XP (dd2d9: ignore stage ≥ 8 with D3D_OK),
   dedupe, SetTexture AddRefs the texture's surface and always returns D3D_OK; GetTexture AddRefs.
7. **SwapTextureHandles** per §3.6 (INVALIDOBJECT for NULLs, INVALIDPARAMS without handles or for managed
   textures, then swap the handle values).
8. **Transforms**: types 1-3 only, INVALIDPARAMS otherwise for Set/Get/Multiply; MultiplyTransform = M × current.
   **v1 matrices**: handles as opaque non-zero values (dd2d9 should validate them against its own table and
   return DDERR_INVALIDPARAMS for unknown ones instead of faulting); GetMatrix zeroes the output first;
   SetMatrix updates a transform bound by STATETRANSFORM.
9. **Viewports**: list with XP's order (HEAD = first added), AddViewport refuses a viewport attached anywhere,
   AddRef/Release as §5.1-5.4 (including the version-dependent current-viewport reference), NextViewport
   returning D3D_OK + NULL at the end, NOVIEWPORTS / NOCURRENTVIEWPORT / INBEGIN codes.
10. **Viewport-driven transforms**: on each draw, if the current viewport's data changed since last sent, rebuild
    the D3D9 viewport and projection (implementation notes) and reload the active lights from the viewport's
    lights (D3DLIGHT_ACTIVE, list order). Draw with no current viewport: return D3DERR_NOCURRENTVIEWPORT
    instead of crashing (deliberate); data never set → D3DERR_VIEWPORTDATANOTSET.
11. **BeginScene/EndScene** per §6, including SURFACEBUSY for locked targets, **RestoreAllSurfaces on
    SURFACELOST when the cooperative level is fine** (the game's surfaces come back empty, BeginScene succeeds),
    SCENE_BEGIN_FAILED when restoring fails, the in-scene flag cleared before EndScene's later errors, and
    EndScene's DDERR_SURFACELOST after a loss during the scene.
12. **SetRenderTarget** v2/v3 per §7.1 (INVALIDOBJECT for NULL, DEVICEAGGREGATED, INVALIDCAPS, 2048 limit,
    palette rule, **no Z → Z-less switch**, version-dependent references, texture handles invalidated,
    viewport kept). GetRenderTarget per version.
13. **GetCaps**: any of 0xAC/0xCC/0xFC on any interface for both structures, each filled to its own size, with
    the stored dwSize copied over (0xFC).
14. **EnumTextureFormats**: dd2d9's format list filtered as §7.4 (9-format inclusion list + FOURCC; DXTn hidden
    and 10-format cap for version 1/2), DDSURFACEDESC callback for v1/v2, DDPIXELFORMAT for v3; stop on non-OK.
    (Registry inclusion lists and dwAppHackFlags: read them only if dd2d9 already reads app-compat flags; else
    mode 0.)
15. **GetStats**: E_NOTIMPL on version 3 devices; else triangles/lines/points/vertices counters.
16. **Draws**: Device2 D3DVERTEXTYPE → FVF table (types outside 1-3: return DDERR_INVALIDPARAMS rather than
    reading past the table — deliberate); no other validation; D3DDP_DONOTCLIP/DONOTUPDATEEXTENTS honoured
    for transformed vertices; Strided XYZRHW → D3DERR_INVALIDVERTEXTYPE; DrawIndexedPrimitiveVB without a
    start vertex; transformed VBs clipped unless DONOTCLIP. Device2's ≥ 65536-vertex split need not be copied
    (D3D9 takes the batch whole), except that XP's strip/fan remainder bug is not worth reproducing.
17. **Begin/Vertex/Index/End**: batch to D3D9 DrawPrimitiveUP; End with 0 primitives → DDERR_INVALIDPARAMS;
    no NOTINBEGIN; Vertex/Index outside Begin: ignore with D3D_OK (XP writes through a stale pointer).
18. **Clip status**: EXTENTS3 refused only on version 3 devices; GetClipStatus as DX7.
19. **ValidateDevice, ComputeSphereVisibility**: share dd2d9's Device7 code (S items 17, 19).
20. **Execute/Pick/GetPickRecords/CreateExecuteBuffer** per §9 (NULL viewport = current; the viewport
    swapped for the call and restored; CLIPPED/UNCLIPPED only; CLASS_E_NOAGGREGATION). Decoding the D3DOP stream
    is the execute-buffer spec's.
21. **Initialize** → DDERR_ALREADYINITIALIZED.

---

## Implementation notes for dd2d9 (on D3D9)

dd2d9's `Device7` (`src/d3d7.cpp`) already owns the D3D9 path: DX7-numbered render/stage state arrays,
`PushRS`/`PushTSS` translating them to D3D9 (`d3d7.cpp:711-799`), `GpuContext` in `src/gpures.h` for the D3D9
calls, `DrawCommon` for draws. The DX6 device is best built as **a front end that owns the DX6 state and
semantics and drives a Device7-like back end**, rather than a Device7 wrapper, because Get/dedupe semantics,
viewports and the legacy-state "driver" layer differ.

### Structure

- `Device3Impl` (one object, three COM interface sub-objects for v1/v2/v3, plus the version) holds §1.3's
  front-end arrays and implements every method of this spec with XP's checks.
- A "driver" layer between it and `GpuContext` holds what a DX6 HAL would hold: the driver-side stage states
  after legacy mapping (§1.4), the stage textures (from SetTexture or TEXTUREHANDLE), the viewport-derived
  projection. It receives exactly what d3dim would put in the DP2 stream (render states after the front-end
  dedupe, stage states for stages < 8, texture changes at draw time) and pushes D3D9 state. Reusing Device7's
  `PushRS`/`PushTSS` mapping tables (DX7 numbers) for the non-legacy states keeps one translation:
  - render states 7-10, 14-16, 19-20, 22-29, 34-38, 48, 52-60, 128-135 → as `Device7::PushRS`;
  - legacy/removed states (1-6, 11-13, 17, 18, 21, 30-33, 39-46, 49-51, 64-95) → the §1.4 mapping or nothing;
    never forward them as D3D9 states (D3D9 numbers ≥ 154 mean other things; 1-6 etc. are unused but keep the
    rule uniform);
  - COLORKEYENABLE 41 → `GpuContext::SetColorKeyEnable` + the alpha test (as `Device7::PushAlphaTest`); note
    its version-1 HAL default of TRUE;
  - stage states → `Device7::PushTSS` (MAGFILTER/MIPFILTER value translation, ADDRESS → U and V).
- States 136-152 do not exist in DX6. D3D9's LIGHTING, CLIPPING, COLORVERTEX, AMBIENT, FOGVERTEXMODE and the
  vertex-fog parameters must be **derived** per draw (below), never taken from the DX6 render-state array.

### Legacy texture states on D3D9 (the emulated driver)

| DX6 input | D3D9 |
| --- | --- |
| TEXTUREMAPBLEND | stage 0 COLOROP/ARG1/ARG2, ALPHAOP/ARG1/ARG2 per the §1.4 table; stage 1 COLOROP DISABLE. MODULATE: ALPHAOP SELECTARG1(TEXTURE) when the bound texture's format has alpha, SELECTARG2(DIFFUSE) otherwise (re-evaluate when the stage-0 texture changes). DECAL/COPY on a texture with alpha: SELECTARG1 as XP |
| TEXTUREMAG / TEXTUREMIN | D3DSAMP_MAGFILTER / MINFILTER / MIPFILTER (D3D9 values: POINT 1, LINEAR 2; MIP NONE 0, POINT 1, LINEAR 2) per §1.4 |
| TEXTUREADDRESS / U / V | D3DSAMP_ADDRESSU/V of sampler 0 (same values) |
| BORDERCOLOR, MIPMAPLODBIAS | D3DSAMP_BORDERCOLOR / MIPMAPLODBIAS of sampler 0 |
| WRAPU / WRAPV | D3DRS_WRAP0 bits (the front end already keeps WRAP0 in step) |
| TEXTUREHANDLE ≠ 0 | SetTexture(0, the handle's texture), stages 1-7 disabled while it is set (send COLOROP DISABLE to D3D9 stage 1 without touching the front-end array); 0 → stage 0 untextured until a SetTexture(0) reaches the driver |
| TEXTUREPERSPECTIVE, ANTIALIAS, SUBPIXEL*, STIPPLE*, ROP2, PLANEMASK, MONOENABLE, ZVISIBLE, ANISOTROPY | nothing (stored for Get) |

Because the front end dedupes against *its* arrays, a sequence "SetTSS(0, COLOROP, MODULATE); SetRenderState
(TEXTUREMAPBLEND, DECAL); SetTSS(0, COLOROP, MODULATE)" leaves the driver at SELECTARG1 on XP (the last call is
deduped). Keeping the two layers separate reproduces this for free.

### D3DVERTEXTYPE and FVF

D3DVT_VERTEX → 0x112, LVERTEX → 0x1E2, TLVERTEX → 0x1C4, stride 32. LVERTEX's D3DFVF_RESERVED1 (0x20) is
D3DFVF_PSIZE in D3D9: `GpuContext::Draw` already skips it (gpures.h comment), so pass 0x1E2 through it.
Device3 FVFs as V §Implementation notes (strip reserved bits, clamp texcoord count).

### Viewport-driven transforms

D3D9 has one viewport (pixels + depth range) and the projection matrix; DX5/6 add a clip volume. With the
D3DVIEWPORT2 of the current viewport (§5.6):

- D3D9 viewport = `(dwX, dwY, dwWidth, dwHeight, MinZ 0, MaxZ 1)`.
- D3D9 projection = DX6 PROJECTION × M (row vectors, `v' = v·P·M`), with
  `cw = dvClipWidth`, `ch = dvClipHeight`, `dz = dvMaxZ − dvMinZ`:

  ```
  M = | 2/cw                0                 0           0 |
      | 0                   2/ch              0           0 |
      | 0                   0                 1/dz        0 |
      | -(1 + 2*dvClipX/cw) 1 - 2*dvClipY/ch  -dvMinZ/dz  1 |
  ```

  (identity for the default clip volume −1, 1, 2, 2, 0, 1). This reproduces §5.6 exactly: XP's
  `xn, yn, zn` are D3D9's NDC mapped to [0, 1]. Recompute when PROJECTION or the viewport data change; keep the
  previous M when XP would skip (zero sizes or dvMaxZ = dvMinZ). The 1e-6 HAL offset is not worth emulating.
- GetTransform(PROJECTION) must return the game's matrix, not P·M.
- Transformed vertices (XYZRHW): the viewport only clips (D3D9 viewport rectangle); as V §Implementation notes
  (z clamp when ZENABLE is off, rhw = 0).
- D3DVIEWPORT (v1) data: whatever form the viewport object stores (viewport spec), the device consumes only its
  D3DVIEWPORT2 equivalent.

### Lighting, material, fog, specular

- D3D9 LIGHTING = (FVF has D3DFVF_NORMAL) && !(dwFlags & D3DDP_DONOTLIGHT) && material handle ≠ 0, per draw.
- Material: the handle's D3DMATERIAL → D3DMATERIAL9 (diffuse, ambient, specular, emissive, power; drop dwSize,
  hTexture, dwRampSize). Re-send when the handle changes or the material object changes (material spec).
- AMBIENT light state → D3DRS_AMBIENT. COLORVERTEX light state → D3DRS_COLORVERTEX with DIFFUSEMATERIALSOURCE
  COLOR1, SPECULARMATERIALSOURCE COLOR2, AMBIENT/EMISSIVE MATERIAL (D3D9's defaults; §2.3).
- Lights: the current viewport's active lights converted to D3DLIGHT9 (light spec) and enabled in list order up
  to D3DCAPS9.MaxActiveLights (DXVK 8); XP has no limit — log the excess.
- SPECULARENABLE → D3DRS_SPECULARENABLE (note the TRUE default on v1/v2 devices).
- Fog: vertex fog = light state FOGMODE ≠ NONE && FOGENABLE: D3DRS_FOGVERTEXMODE = FOGMODE, and D3D9's
  FOGSTART/FOGEND/FOGDENSITY (shared by vertex and table fog in D3D9) loaded from the **light states**; table
  fog (FOGTABLEMODE ≠ NONE) uses render states 36-38. If both are on, D3D9 uses table fog (as DX6 HALs did):
  load 36-38 from the render states in that case.

### Scenes, targets, references

- BeginScene's RestoreAllSurfaces: call dd2d9's own DirectDraw RestoreAllSurfaces path when the render target, Z
  or a managed texture is lost and the cooperative level is OK; then proceed as for DX7 (dd2d9 keeps D3D9 in its
  own scene, S/V notes).
- SetRenderTarget: D3D9 resets its viewport on SetRenderTarget; re-apply the DX6 viewport/projection after every
  D3D9 target change (DX6 keeps the viewport, §7.1). Texture handles are invalidated (dd2d9's handle table must
  forget the device's handles: texture spec).
- References exactly as §7.1/§10 (version-dependent RT interface; Z/palette held except for version 1).

### Pitfalls

- Do not apply d3dim700's retired-state rules to Device2/3: every DX5 state must be accepted and readable.
- Get must never reflect the driver layer (legacy mapping, derived LIGHTING/fog, P·M).
- D3D9 has no D3DDP_DONOTCLIP; CLIPPING FALSE for that draw is the closest (harmless: the game promised the
  vertices are inside).
- EnumTextureFormats' 9-format inclusion list hides formats DX6 games may expect from Windows 9x; it is what XP
  shows, so match it (a game that needs more had an app-compat entry).
- Device2 vertex types and Begin's vertex type index a table without a check on XP; dd2d9 must check.

---

## Not determined

- Whether a NULL/bad pointer inside the SEH-protected methods (GetRenderState, GetTexture, CreateMatrix, ...) is
  turned into an error or crashes; only explicit NULL checks are listed.
- Whether the render-state array of HAL devices (buffer from context creation) is written by drivers, which
  would make GetRenderState return driver-changed values.
- The meaning of `local_84` in `_SpanInit_6ded65e2.c:307` ("texture has alpha", inferred) and so the exact alpha
  rule of legacy MODULATE in the reference rasterizer; HAL drivers each implemented their own mapping.
- What a stale TEXTUREHANDLE does after SetRenderTarget invalidated the handles (driver-specific).
- The stored descriptors' dwSize when the driver gives no extended caps (GetCaps overwrites the caller's dwSize).
- Handling of D3DDP_DONOTCLIP / D3DDP_DONOTUPDATEEXTENTS for untransformed vertices (inside the PSGP/FE
  transform functions, not traced), and the vertex colour used for unlit vertices without a diffuse colour.
- SetLightState with state 0 (reads the override mask at index −1).
- Which app-compat entries set dwAppHackFlags bits 6-8 (texture-enumeration mode, 10-format cap).
- The DDI 2 SetRenderTarget fallback (context destroy/recreate) in detail; XP-era HAL drivers are DDI 3.
- The D3D_OK-but-viewport-left-current path of Execute (step 5) was read from the decompile only.
- The D3DSTATS dwSpansDrawn counter (never seen updated).
