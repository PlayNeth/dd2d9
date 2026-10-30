# IDirect3DDevice7: the non-drawing methods (states, stages, textures, transforms, viewport, material, lights, clip planes/status, scenes, Clear, state blocks, Load/PreLoad, ValidateDevice, GetInfo, render target, caps)

Reference: d3dim700.dll 5.3.2600.5512 (XP SP3), Ghidra decompile with PDB public names in
`$D` = `$S/d3d-decomp/d3dim700-named/` (`$S` = the session scratchpad). File names are
`<ShortMethod>_<address>.c`; the class is picked by address from `$S/d3d-decomp/d3dim700-functions.txt`.
Line numbers are those of the `.c` files. Where the decompile hides a return value (see below) the
disassembly of `$S/xpd3d/d3dim700.dll` was read and the instruction address is cited as `@7391xxxx`.
DrawPrimitive*, vertex buffers and IDirect3D7/CreateDevice are out of scope (other specs).

## Conventions (they apply to every method below)

- **Ghidra shifts stdcall parameters by one.** In every `DIRECT3DDEVICEI::Xxx` API method the decompile's
  `param_1` is `This`, `param_2` is the first real argument, and the last argument appears as
  `in_stack_000000NN`. Read e.g. `SetRenderState(param_1=This, param_2=state, in_stack_0000000c=value)`.
- **SEH/C++-EH hides return values.** Methods with `__EH_prolog` return through a stub (`FUN_xxxx`) that
  only restores `fs:0`. The real value is `eax` at the jump; it was taken from the disassembly. The
  common shape is `try { ... return D3D_OK; } catch (HRESULT hr) { return hr; }`: internal helpers
  report errors by `__CxxThrowException_8(&hr)` and the API method returns that `hr`.
- **Locking.** `CLockD3DMT` (`CLockD3DMT_7391f55e.c:8-14`) takes the DirectDraw lock only when the device
  was created multithreaded (`dev+0x2270 & 2`). It reads `This->0x2270` first, so the explicit
  `This == NULL → DDERR_INVALIDOBJECT` checks in methods that use it are unreachable (they fault first).
  Methods using `_AcquireDDThreadLock` directly (BeginScene, Clear, SetViewport, ...) do not have that problem.
- **No pointer validation and almost no range validation** in this retail build: out-pointers, stage
  numbers, state numbers, light/plane indices are used as given. A bad pointer faults (inside the try:
  the outcome then depends on the EH setup, not determined). This is called out per method.
- **Device classes.** One vtable per class, all sharing the DIRECT3DDEVICEI API methods (vtable dump of
  `??_7DIRECT3DDEVICEI`, `CDirect3DDeviceIDP2`, `CDirect3DDevice7`, `CDirect3DDeviceTL` at 0x73918448,
  0x739185a0, 0x739186f8, 0x739194f0). The class sets the "DDI level" `dev+0x2284`:
  CDirect3DDeviceIDP2 = 3 (DX6 DrawPrimitives2 driver), CDirect3DDevice7 = 4 (DX7 driver, no T&L),
  CDirect3DDeviceTL = 5 (T&L HAL) (`CDirect3DDeviceIDP2_7391ce98.c:27`, `CDirect3DDevice7_7391cee8.c:40`,
  `CDirect3DDeviceTL_73927143.c:54`). Which class a GUID gets is decided in `Direct3DCreateDevice` (out of
  scope). The overrides that change behaviour visible to a game are: `CDirect3DDevice7::GetInfo`,
  `::ClearI`, `::SetRenderTargetI`, `::UpdateTextures`, `::CanDoTexBlt` (classes 4 and 5) and the TL
  overrides that additionally send transforms/lights/material/clip planes to the driver (class 5).
- **"Passed to the driver"** below means the value is appended to the DrawPrimitives2 command buffer
  (D3DDP2OP_RENDERSTATE 8, TEXTURESTAGESTATE 0x19, SETTRANSFORM 0x24, SETLIGHT 0x22, CREATELIGHT 0x23,
  SETMATERIAL 0x21, SETCLIPPLANE 0x2c, CLEAR 0x2a, SETRENDERTARGET 0x29), flushed later
  (`SetRenderStateI_7392486e.c:207-239`, `SetTSSI_73924eae.c:235-266`).
- "Hardware device" = `dev+0x1a6c & 0x40`, set for IID_IDirect3DHALDevice / IID_IDirect3DTnLHalDevice
  (`Init_73925ffa.c:43-56`).

### Device fields used in this spec (offsets into DIRECT3DDEVICEI; the PDB has no type records)

| Offset | Meaning (inferred) |
| --- | --- |
| `0x2998` | pointer to the render-state array, 256 DWORDs (`Init_7391b66b.c:164-169`) |
| `0x299c + (stage*64 + type)*4` | texture-stage-state array, 8 stages × 64 DWORDs (`SetTextureStageStateFast_73921367.c:107`) |
| `0x2938 + stage*4` | bound texture object per stage (`SetTextureInternal_7391fb68.c:113`) |
| `0x2978` | number of stages the driver has (`wMaxTextureBlendStages`, 1 without extended caps) (`InitDeviceI_7391ac0a.c:100-107`) |
| `0x297c` | mask of stages whose texture must be re-sent (`SetTextureInternal_7391fb68.c:123`) |
| `0x31ac` | first render state the driver does **not** get from the front-end path: 0x88 (base, IDP2, Device7), 0x100 (TL) (`DIRECT3DDEVICEI_7391ce28.c:13`, `CDirect3DDeviceTL_73927143.c:55`) |
| `0x31a8` | first TSS type the driver does not get: 0x18 (base/IDP2), 0x1d (Device7/TL) (`DIRECT3DDEVICEI_7391ce28.c:14`, `CDirect3DDevice7_7391cee8.c:41`) |
| `0x31b0` | 256-bit mask "render state needs front-end processing" (table at 0x73918160) |
| `0x31d0` | 256-bit mask "retired render state" (table at 0x739182a8) |
| `0x1a6c` bit 0x1000 (= byte `0x1a6d` & 0x10) | recording a state block |
| `0x2270` bit 1 / bit 2 | in scene / multithreaded |
| `0x2298` | D3DVIEWPORT7 |
| `0x2378`,`0x2338`,`0x22f8`,`0x23b8`,`0x23f8`,`0x2438`, `0x8 + i*0x40` | WORLD, VIEW, PROJECTION, WORLD1, WORLD2, WORLD3, TEXTUREi matrices |
| `0x578` | D3DMATERIAL7 |
| `0x282c` / `0x2830` | light array (0x134 bytes each; D3DLIGHT7 at +8, flags at +0x90: 0x40 = set, 0x80 = enabled) / its length |
| `0x25d8 + i*16` | user clip plane i |
| `0x27e0`, `0x1b20..0x1b2c` | clip status dwStatus, extents (minx, miny, maxx, maxy) |
| `0x319c` | CStateSets |
| `0x22e4` / `0x22e8` / `0x22f0` | render target (IDirectDrawSurface v1) / Z buffer (v1) / render target (v7, the one GetRenderTarget returns) |
| `0x22d0` | driver's D3DHAL_GLOBALDRIVERDATA (caps); `0x2994` extended caps; `0x284c` the D3DDEVICEDESC7 GetCaps returns |

---

## SetRenderState — `SetRenderState_7391f8c9.c`

1. **Recording a state block** (`0x1a6d & 0x10`, line 28):
   - retired state (see table) → **DDERR_INVALIDPARAMS** (lines 32-38, `@7391f939`);
   - otherwise appended to the block (`CStateSets::InsertRenderState`, lines 40-42), **D3D_OK**. Nothing is
     applied to the device.
2. Otherwise `SetRenderStateFast` (vtable +0x10c, line 29) and **D3D_OK regardless of its result**
   (`@7391f959: xor eax,eax`).
3. **No check of the state number** (0, ≥ 0x99, ≥ 0x100) and **no check of the value**, retail or recording.
   A state ≥ 256 indexes past the 256-entry array and the 256-bit masks (`SetRenderStateFast_73921237.c:20-21`).

### SetRenderStateFast — `SetRenderStateFast_73921237.c`

- State **not** in the front-end mask (`0x31b0`) (line 20): if the value differs from the stored one, store it
  and pass it to the driver (lines 21-28). There is no `0x31ac` limit on this path.
- State **in** the front-end mask:
  1. if texture-coordinate remapping is active (`0x1a66 & 1`, see "Texture-stage remapping" below),
     `RestoreTextureStages` first (lines 35-40);
  2. if the value equals the stored one → done (line 41);
  3. `UpdateInternalState` (line 42) — updates the front end and **stores the value**
     (`UpdateInternalState_739204e2.c:187`, reached by every case);
  4. if `state < 0x31ac`: a retired state returns DDERR_INVALIDPARAMS (ignored by SetRenderState), others
     go to the driver (lines 43-49).
- So **retired states are stored, never reach the driver, and are not emulated** on the IDirect3DDevice7
  path (UpdateInternalState has no case for them: `UpdateInternalState_739204e2.c:151-158`). DX5-style
  TEXTUREHANDLE/TEXTUREMAPBLEND/TEXTUREMAG/... translation happens in d3dim.dll (Device2/3), not here.

### UpdateInternalState — what the front end does itself (`UpdateInternalState_739204e2.c`)

| State | Front-end effect |
| --- | --- |
| SHADEMODE (9) | stored, interpolation flags recomputed (lines 78-82) |
| FOGENABLE (28) | `SetFogFlags` (lines 106-109) |
| SPECULARENABLE (29) | lighting/vertex-format dirty (lines 84-94) |
| FOGSTART/FOGEND (36/37) | `UpdateFogFactor` (lines 96-104) |
| FOGDENSITY (38) | stored at `0x5cc` (lines 74-76) |
| RANGEFOGENABLE (48) | flag `0x1a64 & 4` (lines 36-43) |
| CLIPPING (136) | `0x1a66 & 8` when off; clip union/intersection reset (lines 45-54) |
| LIGHTING (137) | `0x1a67 & 0x80` (lines 55-61) |
| EXTENTS (138) | `0x1a66 & 0x10` when **off** (lines 63-71) |
| AMBIENT (139) | converted to floats 0..255 and 0..1 (`* 1/255`), lighting dirty (lines 21-33) |
| FOGVERTEXMODE (140) | stored at `0x5c8`, `SetFogFlags` (lines 112-114) |
| COLORVERTEX (141), LOCALVIEWER (142), NORMALIZENORMALS (143) | flags + dirty (lines 115-150) |
| DIFFUSE/SPECULAR/AMBIENT/EMISSIVE MATERIALSOURCE (145-148) | mapped COLOR1→0, COLOR2→1, anything else (incl. MATERIAL) → 2 (lines 159-178) |
| VERTEXBLEND (151) | dirty (line 179-181) |
| CLIPPLANEENABLE (152) | dirty, transformed planes invalidated (lines 182-185) |
| WRAP0..7 (128-135) | `RestoreTextureStages` if remapping active (lines 153-157) |

These states (plus the retired ones) are the front-end mask (data at 0x73918160, set in
`stateInitialize_7391a31b.c:15-21`): 28, 29, 48, 38, 36, 37, 128-148, 151, 152, 9, and all retired states.
On a non-T&L device (`0x31ac = 0x88`) states ≥ 136 in this mask never reach the driver; on a TL device they do.

### Retired states (data at 0x739182a8, `stateInitialize_7391a31b.c:22-28`; tested by `CheckForRetiredRenderState_7391f442.c:8`)

1 TEXTUREHANDLE, 3 TEXTUREADDRESS, 5 WRAPU, 6 WRAPV, 11 MONOENABLE, 12 ROP2, 13 PLANEMASK, 17 TEXTUREMAG,
18 TEXTUREMIN, 21 TEXTUREMAPBLEND, 31 SUBPIXEL, 32 SUBPIXELX, 39 STIPPLEENABLE, 43 BORDERCOLOR,
44 TEXTUREADDRESSU, 45 TEXTUREADDRESSV, 46 MIPMAPLODBIAS, 49 ANISOTROPY, 51 TRANSLUCENTSORTINDEPENDENT,
64-95 STIPPLEPATTERN00-31.

**Not** retired (so accepted, stored, returned by Get, and passed to the driver): 2 ANTIALIAS,
4 TEXTUREPERSPECTIVE, 10 LINEPATTERN, 30 ZVISIBLE, 33 STIPPLEDALPHA, 40 EDGEANTIALIAS, 41 COLORKEYENABLE,
42 (OLDALPHABLENDENABLE), 47 ZBIAS, 50 FLUSHBATCH, 144 COLORKEYBLENDENABLE, and every undefined number.

## GetRenderState — `GetRenderState_7391f987.c`

1. `This == NULL` → DDERR_INVALIDOBJECT (lines 22-26) (unreachable in practice, see Conventions).
2. Retired state → **DDERR_INVALIDPARAMS** (lines 29-35), recording or not.
3. WRAP0-7 while texture-coordinate remapping is active: returns the value the application set, from
   the remap table (lines 37-50).
4. Otherwise `*pValue = array[state]` (line 51-53), **D3D_OK**. No range check; NULL pValue faults.
- Values never set read back as their initial value. The array starts as **0xFFFFFFFF** everywhere
  (`stateInitialize_7391a31b.c:29-33`), so on a DDI ≥ 4 device (every DX7 HAL/TL device) ANTIALIAS (2),
  EDGEANTIALIAS (40), 42, 50, 61-63, 96-127, 149, 150 and 153-255 read **0xFFFFFFFF** (see defaults).

## Render-state defaults — `DIRECT3DDEVICEI::stateInitialize` (`stateInitialize_7391a31b.c`)

Called from `Init_7391b66b.c:213` with `zenable = (a Z buffer is attached to the render target)`
(`Init_7391b66b.c:128-131`: `GetAttachedSurface(DDSCAPS_ZBUFFER)`). Each value is applied through
`SetRenderStateInternal` (`SetRenderStateInternal_73921300.c`: skip if equal, else UpdateInternalState and
send if `< 0x31ac`). Entries 13, 58, 59, 60 are pre-set to 0 (lines 34-37) so that their ~0 defaults are
actually sent.

| State | Default | Line |
| --- | --- | --- |
| TEXTUREPERSPECTIVE (4) | TRUE | 38 |
| SPECULARENABLE (29) | FALSE | 39 |
| ZENABLE (7) | TRUE if a Z buffer is attached, else FALSE | 40 |
| FILLMODE (8) | SOLID (3) | 41 |
| SHADEMODE (9) | GOURAUD (2) | 42 |
| LINEPATTERN (10) | 0 | 43 |
| ZWRITEENABLE (14) | TRUE | 44 |
| ALPHATESTENABLE (15) | FALSE | 45 |
| LASTPIXEL (16) | TRUE | 46 |
| SRCBLEND (19) / DESTBLEND (20) | ONE (2) / ZERO (1) | 47-48 |
| CULLMODE (22) | CCW (3) | 49 |
| ZFUNC (23) | LESSEQUAL (4) | 50 |
| ALPHAREF (24) / ALPHAFUNC (25) | 0 / ALWAYS (8) | 51-52 |
| DITHERENABLE (26) | FALSE | 53 |
| FOGENABLE (28) | FALSE | 54 |
| ZVISIBLE (30) | FALSE | 55 |
| STIPPLEDALPHA (33) | FALSE | 56 |
| FOGCOLOR (34) / FOGTABLEMODE (35) | 0 / NONE | 57-58 |
| FOGSTART (36) / FOGEND (37) / FOGDENSITY (38) | 0.0f / 1.0f / 1.0f | 59-61 |
| COLORKEYENABLE (41) | FALSE | 62 |
| ALPHABLENDENABLE (27) | FALSE | 63 |
| ZBIAS (47) | 0 | 64 |
| RANGEFOGENABLE (48) | FALSE | 65 |
| only if DDI < 4: STIPPLEENABLE 0, MONOENABLE 0, ROP2 13 (R2_COPYPEN), PLANEMASK ~0, WRAPU 0, WRAPV 0, ANTIALIAS 0, SUBPIXEL 0, SUBPIXELX 0, EDGEANTIALIAS 0, STIPPLEPATTERN00-31 0 | | 66-109 |
| STENCILENABLE (52) | FALSE | 110 |
| STENCILFAIL/ZFAIL/PASS (53-55) | KEEP (1) | 111-113 |
| STENCILFUNC (56) | ALWAYS (8) | 114 |
| STENCILREF (57) / STENCILMASK (58) / STENCILWRITEMASK (59) | 0 / ~0 / ~0 | 115-117 |
| TEXTUREFACTOR (60) | 0xFFFFFFFF | 118 |
| WRAP0-7 (128-135) | 0 | 119-123 |
| AMBIENT (139) | 0 | 183 |
| COLORVERTEX (141) | TRUE | 184 |
| FOGVERTEXMODE (140) | NONE | 185 |
| CLIPPING (136) / LIGHTING (137) / EXTENTS (138) | TRUE / TRUE / FALSE | 186-188 |
| NORMALIZENORMALS (143) / LOCALVIEWER (142) | FALSE / TRUE | 189-190 |
| COLORKEYBLENDENABLE (144) | FALSE | 191 |
| EMISSIVE/AMBIENT/DIFFUSE/SPECULAR MATERIALSOURCE (148/147/145/146) | MATERIAL / MATERIAL / COLOR1 / COLOR2 | 192-195 |
| VERTEXBLEND (151) / CLIPPLANEENABLE (152) | DISABLE / 0 | 196-197 |

Everything not listed stays 0xFFFFFFFF.

Other initial state (all through the normal setters, so the driver sees it):

- **Texture stages** (`stateInitialize_7391a31b.c:138-182`), for each of the 8 stages, via the public
  `SetTexture(stage, NULL)` and `SetTextureStageState`: COLOROP = MODULATE on stage 0, DISABLE on 1-7;
  COLORARG1 = TEXTURE; COLORARG2 = CURRENT; ALPHAOP = SELECTARG1 on stage 0, DISABLE on 1-7; ALPHAARG1 = TEXTURE;
  ALPHAARG2 = CURRENT; BUMPENVMAT00-11 = 0; **TEXCOORDINDEX = 0 on every stage**; ADDRESS, ADDRESSU, ADDRESSV = WRAP;
  BORDERCOLOR = 0; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE (D3DTFP_NONE = 1); MIPMAPLODBIAS = 0;
  MAXMIPLEVEL = 0; MAXANISOTROPY = 1; BUMPENVLSCALE = BUMPENVLOFFSET = 0; TEXTURETRANSFORMFLAGS = DISABLE.
  The other 39 entries of each stage (0 and 25-63) start as 0xFFFFFFFF (lines 133-137); entry 0 later holds
  the driver texture handle (`UpdateTextures_739274f4.c:80-82`).
- **Transforms**: all of VIEW, PROJECTION, WORLD, WORLD1-3, TEXTURE0-7 = identity (`D3DFE_Create_7391eaa5.c:102-116`).
- **Clip status**: dwStatus = 0x01FFF000 (all CLIPINTERSECTION bits, no union), extents minx = miny = 2048.0,
  maxx = maxy = 0 (`D3DFE_Create_7391eaa5.c:117-126`).
- **Viewport**: (0, 0, render-target width, height, 0.0, 1.0) through the public SetViewport (`Init_7391b66b.c:215-223`).
- **Material**: all zero (`Init_7391b66b.c:237-241`). **Lights**: 8 empty slots (`Init_7391b66b.c:225-236`).
- Lines 198-212: on some drivers (caps bit not identified) the priority/LOD of every managed texture is re-sent
  (vtable +8/+0x10 of each texture object). Not game-visible as far as determined.

## SetTextureStageState — `SetTextureStageState_7391fbcf.c`

1. Recording → appended (`CStateSets::InsertTextureStageState`, lines 27-34), **D3D_OK**.
2. Otherwise `SetTextureStageStateFast` (vtable +0x108, line 36); its result is returned (`@7391fc2e-7391fc39`).
3. **No check of stage, type or value.** A stage ≥ 8 writes outside the 8×64 array.

### SetTextureStageStateFast — `SetTextureStageStateFast_73921367.c`

- Types other than COLOROP (1), TEXCOORDINDEX (11) and ≥ TEXTURETRANSFORMFLAGS (24)
  (`NeedInternalTSSUpdate_7391f41e.c:280`): if equal → D3D_OK; else store (lines 105-112).
- Those three: remapping restore if active (lines 114-118); equal → D3D_OK (119-121); else
  `UpdateInternalTextureStageState` (122), which stores the value (`UpdateInternalTextureStageState_73921081.c:215`)
  and returns "do not send" for: TEXCOORDINDEX with generation flags (value > 7) on DDI < 5 (the front end
  generates the coordinates) (169-171); TEXTURETRANSFORMFLAGS and types > 24 on DDI 3 (206-213).
- Sent to the driver only if `stage < wMaxTextureBlendStages` (line 128) and `type < 0x31a8` (`SetTSSI_73924eae.c:235`).
- **D3DTSS_ADDRESS (12) is not split into ADDRESSU/ADDRESSV** by d3dim700: it is stored and passed on; the
  driver applies it to U and V. GetTextureStageState(ADDRESSU) does **not** change after SetTSS(ADDRESS).

## GetTextureStageState — `GetTextureStageState_7391fc63.c`

`This == NULL` → DDERR_INVALIDOBJECT (70-75, unreachable); TEXCOORDINDEX while remapping is active → restore
first so the app's value is returned (77-81); `*pValue = array[stage][type]` (82), **D3D_OK**. No range check.
Type 0 returns the driver texture handle (internal), types 25-63 0xFFFFFFFF.

### Texture-stage remapping (internal, explains the special cases above)

On non-T&L devices, when a texture transform changes the number of coordinates, the front end rewrites
TEXCOORDINDEX and WRAPn for the driver (`EvalTextureTransforms_73925a44.c:104-146`) and remembers the app's
values in the table at `0x1b30` (`0x1c30` entries); `RestoreTextureStages_7391f7ed.c` puts the app's values
back. Get methods always return the app's values. On D3D9 none of this is needed.

## SetTexture / GetTexture — `SetTexture_7391fad7.c`, `SetTextureInternal_7391fb68.c`, `GetTexture_7391fa4e.c`

SetTexture:
1. Recording → `CStateSets::InsertTexture` records `(stage, surface pointer)` **without AddRef** (`InsertTexture_73922479.c:12-14`), D3D_OK.
2. Otherwise `SetTextureInternal` (vtable +0x148), result returned (always 0):
   - texture object = `lpSurfMore->(+0x78)` of the surface, NULL for a NULL surface (lines 107-112). That field
     is set by d3dim's `CreateTexture` callback (`CreateTexture_7391d9a4.c:55`), i.e. for surfaces DirectDraw
     announced as textures. **No validation** that the surface is a texture: a surface without a texture object
     binds NULL (probably; it depends on which surfaces DirectDraw announces — see the ddraw specs).
   - **no stage range check** (line 113);
   - if different from the bound one: Release the old, AddRef the new (`DIRECT3DTEXTUREI::AddRef`
     `AddRef_7391f4bb.c:10-18` increments the **surface's** INT, LCL and GBL counts, i.e. a real surface AddRef),
     mark the stage dirty (lines 115-125). The texture is sent to the driver at the next draw/ValidateDevice for
     stages `< wMaxTextureBlendStages` only (`UpdateTextures_739274f4.c:30-88`); D3D-managed textures are
     uploaded there (lines 44-69).
GetTexture: `This == NULL` → DDERR_INVALIDOBJECT (27-32, unreachable); no stage check; `*pp = NULL` if none,
else the surface interface stored in the texture object (`+0x30` for managed, else `+0x2c`), **AddRef'd**
(34-45); D3D_OK.

## SetTransform / GetTransform / MultiplyTransform — `SetTransform_7391fdbd.c`, `GetTransform_7391fe41.c`, `MultiplyTransform_7392086b.c`, `SetTransformI_7391f15a.c`

Types: WORLD 1, VIEW 2, PROJECTION 3, WORLD1 4, WORLD2 5, WORLD3 6, TEXTURE0-7 16-23.

- SetTransform: recording → recorded (any type, unvalidated) (lines 25-31); otherwise `SetTransformI`
  (vtable +0x100). **An invalid type is silently ignored** (`SetTransformI_7391f15a.c:181-249` has no else),
  and SetTransform returns **D3D_OK always** (`@7391fe1e`). No NULL check (NULL faults).
  Side effects: dirty flags; PROJECTION also recomputes the driver's W-range info (UpdateDrvWInfo, line 216-219);
  the TL class also sends D3DDP2OP_SETTRANSFORM (`SetTransformI_73926de4.c:265-273`).
- GetTransform: invalid type → **DDERR_INVALIDPARAMS** (lines 79-82); otherwise copies 64 bytes, D3D_OK.
- MultiplyTransform: invalid type → **DDERR_INVALIDPARAMS** (`@739208fc`); else
  `new = pMatrix × current` (`MatrixProduct(dst, pMatrix, current)`, line 160; `MatrixProduct_73991428.c`
  computes dst[i][j] = Σ a[i][k]·b[k][j]) applied through `SetTransformI` (161), D3D_OK.
  **MultiplyTransform is not recorded while recording a state block: it changes the device immediately**
  (no recording test in the function).

## SetViewport / GetViewport — `SetViewport_73927be6.c`, `SetViewportI_73927b55.c`, `UpdateViewportCache_73927838.c`, `GetViewport_73927c5f.c`

SetViewport:
1. NULL → **DDERR_INVALIDPARAMS** (`@73927bff`).
2. Recording → recorded **without validation** (line 30; validated when applied).
3. `SetViewportI` (vtable +0x104), against the current render target's width W and height H
   (`SetViewportI_73927b55.c:77-82`), all unsigned: `dwX ≤ W`, `dwY ≤ H`, `dwX + dwWidth ≤ W`,
   `dwY + dwHeight ≤ H`, else **DDERR_INVALIDPARAMS** (thrown, line 97-99). Width/height 0 and wrap-around
   sums are not rejected. **dvMinZ/dvMaxZ are not validated.**
4. Stored, then `UpdateViewportCache` **modifies the stored copy**: if `MaxZ − MinZ < 0.001`: if `MaxZ < 0.5`
   then `MaxZ = MinZ + 0.001` else `MinZ = MaxZ − 0.001` (`UpdateViewportCache_73927838.c:18-25`).
   GetViewport returns the adjusted values. Then the driver gets the viewport (vtable +0xe0) unless replaying a
   state block into the driver's own copy.
5. D3D_OK (`@73927c3c`).
GetViewport: NULL → DDERR_INVALIDPARAMS (49-51), else the stored 24 bytes, D3D_OK.
SetRenderTarget does **not** reset or revalidate the viewport (see SetRenderTarget).

## SetMaterial / GetMaterial — `SetMaterial_73928197.c`, `SetMaterialI_7392812d.c`, `GetMaterial_7392820d.c`

SetMaterial: NULL → **DDERR_INVALIDPARAMS** (`@739281b0`); recording → recorded (line 30); else copy 68 bytes
and mark lighting dirty (TL: also D3DDP2OP_SETMATERIAL, `MaterialChanged_73926d1f.c:206-216`); D3D_OK.
**No value validation** (negative power etc. accepted). GetMaterial: NULL → DDERR_INVALIDPARAMS, else copy, D3D_OK.

## SetLight / GetLight / LightEnable / GetLightEnable — `SetLight_73928388.c`, `SetLightI_73928249.c`, `CheckLightParams_73928028.c`, `LightEnable_73928542.c`, `LightEnableI_73928456.c`, `GetLight_739283fd.c`, `GetLightEnable_739285b1.c`

SetLight:
1. `CheckLightParams` runs first, **also while recording** (line 20). DDERR_INVALIDPARAMS (thrown) unless
   (`CheckLightParams_73928028.c:95-111`):
   - `pLight != NULL`;
   - `dltType` is POINT (1), SPOT (2) or DIRECTIONAL (3) (PARALLELPOINT 4 and GLSPOT 5 are rejected);
   - `0 ≤ dvRange ≤ sqrt(FLT_MAX)` (for every type, directional included);
   - SPOT and DIRECTIONAL: `|dvDirection|² ≥ 1e-5`;
   - SPOT only: `0 ≤ dvPhi ≤ π`, `0 ≤ dvTheta ≤ dvPhi`, `dvAttenuation0/1/2 ≥ 0`.
     **Attenuation is not checked for point lights**; colours, falloff and position are never checked.
2. Recording → recorded (line 25), D3D_OK.
3. `SetLightI`: index ≥ array length → the array grows to **index + 10** slots, copying existing set lights and
   rebuilding the enabled list (`SetLightI_73928249.c:90-141`); allocation failure → E_OUTOFMEMORY. No upper
   bound on the index (a huge index makes a huge or overflowing allocation). Copies the 104-byte D3DLIGHT7
   (143-148), marks it set (`LightChanged_73928166.c:9-11`: flags |= 0x50). The TL class also sends
   CREATELIGHT the first time and SETLIGHT (`LightChanged_739271e1.c:30-46`). D3D_OK.
   Setting a light does not change whether it is enabled.

GetLight: NULL out → DDERR_INVALIDPARAMS; index ≥ length or light never set → **DDERR_INVALIDPARAMS**
(`GetLight_739283fd.c:45-59`); else the stored D3DLIGHT7, D3D_OK.

LightEnable: recording → recorded (line 74). Else `LightEnableI` (vtable +0x110): **an index never set (or beyond
the array) first gets a default light** (`LightEnableI_73928456.c:120-135`): DIRECTIONAL, diffuse (1,1,1,0),
specular/ambient 0, position 0, direction (0,0,1), range 0, attenuation 0 — then is linked into/out of the
active list (136-164). The TL class sends D3DDP2OP_SETLIGHT enable/disable (`LightEnableI_73926da3.c:177-182`).
No limit on the number of enabled lights in d3dim700 (the HAL's dwMaxActiveLights is not checked here). D3D_OK.

GetLightEnable: index ≥ length or never set → DDERR_INVALIDPARAMS; else **`*pbEnable = 0x80` when enabled**, 0 when
not (`GetLightEnable_739285b1.c:92-96`) — the TRUE value is 128, not 1. NULL out faults.

## SetClipPlane / GetClipPlane — `SetClipPlane_7391f65d.c`, `SetClipPlaneI_7391f5df.c`, `GetClipPlane_7391f73f.c`

SetClipPlane: recording → recorded; else store 4 floats at `0x25d8 + i*16` and invalidate the transformed
planes (`SetClipPlaneI_7391f5df.c:70-76`); TL also sends D3DDP2OP_SETCLIPPLANE (`SetClipPlaneI_73926d53.c:89-98`).
**No index check, no NULL check**; D3D_OK (`@7391f6be`). GetClipPlane: same, no checks, D3D_OK.
The number of planes used at draw time is the driver's wMaxUserClipPlanes (`InitDeviceI_7391ac0a.c:123`);
state blocks of type ALL capture that many (`CreatePredefined_73922553.c:78-86`).

## SetClipStatus / GetClipStatus — `SetClipStatus_73920258.c`, `GetClipStatus_739202de.c`

SetClipStatus (no NULL check):
1. `dwFlags & D3DCLIPSTATUS_EXTENTS3 (4)` → **DDERR_INVALIDPARAMS** (lines 116, 131-135).
2. `& STATUS (1)` → dwStatus stored (117-119).
3. `& EXTENTS2 (2)` → minx, maxx, miny, maxy stored (120-125). Z extents ignored.
4. D3D_OK.
GetClipStatus (no NULL check): `dwFlags = D3DCLIPSTATUS_EXTENTS2`, dwStatus, minx/maxx/miny/maxy; **minz/maxz
are left untouched** (lines 153-158); D3D_OK.
The status is accumulated by drawing (`D3DFE_UpdateClipStatus_7392464f.c:171-173`: union bits ORed, intersection
bits ANDed) and the extents only while EXTENTS is TRUE (DrawPrimitive spec).

## BeginScene — `BeginScene_7391bd87.c` (codes from `@7391bd87-7391bff2`)

1. `This == NULL` → DDERR_INVALIDOBJECT.
2. Already in a scene → **D3DERR_SCENE_IN_SCENE** (0x887602F8).
3. `CheckSurfaces` (`CheckSurfaces_739251bd.c`):
   - a lost managed texture → DDERR_SURFACELOST;
   - render target or Z buffer **locked** (GBL usage count ≠ 0) → **DDERR_SURFACEBUSY** (returned as is);
   - render target or Z buffer (or internal vertex buffers) lost → DDERR_SURFACELOST.
   On DDERR_SURFACELOST: `IDirectDraw7::TestCooperativeLevel`; if it fails → DDERR_SURFACELOST; else managed
   textures are evicted, internal vertex buffers restored (failure → D3DERR_SCENE_BEGIN_FAILED), and the check is
   repeated; any error then (e.g. the game has not restored its render target) is returned (lines 39-80).
   Internal buffer growth failure → E_OUTOFMEMORY.
4. DDI ≥ 4 and recording a state block → **D3DERR_SCENE_END_FAILED** (0x887602FB, `@7391bf27`) — note: END, not
   INBEGINSTATEBLOCK. This test comes after the surface checks.
5. SCENECAPTURE(TRUE) to the driver (DDI ≥ 4: render state 62 in the command buffer; DDI 3: SceneCapture callback).
   A failure other than DDERR_NOTFOUND → D3DERR_SCENE_BEGIN_FAILED.
6. In-scene flag set, every stage's texture marked for re-send (lines 88-93). D3D_OK.

## EndScene — `EndScene_7391bffa.c` (codes from `@7391bffa-7391c0fb`)

1. `This == NULL` → DDERR_INVALIDOBJECT.
2. Not in a scene → **D3DERR_SCENE_NOT_IN_SCENE** (0x887602F9).
3. The in-scene flag is cleared **before** anything can fail (line 125).
4. DDI ≥ 4: recording → D3DERR_SCENE_END_FAILED; SCENECAPTURE(FALSE) failure → D3DERR_SCENE_END_FAILED; then
   **the command buffer is flushed to the driver** (FlushStates, vtable +0xcc) (lines 132-135). DDI 3: flush, then
   SceneCapture. A result other than 0/DDERR_NOTFOUND → D3DERR_SCENE_END_FAILED (`@7391c09b`).
5. If a surface was lost during the scene (flag set by FlushStates, `FlushStates_7392526b.c:58-63`, and by Load,
   see below) → clear it and return **DDERR_SURFACELOST** (`@7391c0b8`).
6. Texture-manager scene counter incremented; D3D_OK.
Nothing else is flushed (no implicit present/flip).

## Clear — `Clear_73927c9b.c` (codes `@73927c9b-73927d6b`), `ProcessRects_*.c`, `ClearI_73926c36.c`

Arguments: `(count, pRects, flags, color, z, stencil)`; flags TARGET 1, ZBUFFER 2, STENCIL 4.
1. `count > 4096` → **DDERR_INVALIDPARAMS**.
2. Unless the driver reports D3DPRASTERCAPS_ZBUFFERLESSHSR, if `flags & (ZBUFFER|STENCIL)`:
   - no Z buffer attached → **D3DERR_ZBUFFER_NOTPRESENT** (0x88760816);
   - STENCIL requested and the Z format lacks DDPF_STENCILBUFFER → **D3DERR_STENCILBUFFER_NOTPRESENT** (0x88760817).
3. `(flags & 7) == 0` → **DDERR_INVALIDPARAMS**. Other flag bits are ignored, not rejected.
4. Rectangles (`ProcessRects`): `pRects == NULL` (whatever `count`) → one rectangle = the whole viewport;
   otherwise each rectangle is intersected with the viewport and empty results dropped, so **`count = 0` with a
   non-NULL `pRects` clears nothing** (still D3D_OK). Growth failure → E_OUTOFMEMORY.
5. DDI ≥ 4: one D3DDP2OP_CLEAR with flags/color/z/stencil and the rectangles is queued (`ClearI_73926c36.c`),
   no flush. Not in-scene-only (Clear outside BeginScene/EndScene works). Not recorded in state blocks.
   Z is not clamped; colour is ARGB. D3D_OK.

## State blocks — `BeginStateBlock_7392095d.c`, `EndStateBlock_73920a32.c`, `ApplyStateBlock_73920bc5.c`, `CaptureStateBlock_73920c4b.c`, `CreateStateBlock_73920cd2.c`, `DeleteStateBlock_73920b3e.c`, `CStateSets*`, `CStateSet*`

Handles: `CHandleFactory`, handle 0 is reserved at construction (`CStateSets_73921f4b.c:30-33`), so the first block
is **1**; a deleted handle is the next one reused (free list, LIFO: `ReleaseHandle_7391ef0f.c:31-32`,
`CreateNewHandle_7391f0d1.c:10-22`). Storage grows by 10 sets (`StartNewSet_73922041.c:28-61`).

- **BeginStateBlock**: recording → **D3DERR_INBEGINSTATEBLOCK** (0x88760835, `@739209a5`); StartNewSet failure →
  E_OUTOFMEMORY; else recording on, D3D_OK.
- While recording, SetRenderState (non-retired), SetTextureStageState, SetTexture, SetTransform, SetViewport,
  SetMaterial, SetLight, LightEnable, SetClipPlane are **appended in call order** (consecutive commands of the same
  kind share a header: `InsertCommand_7392148a.c:18-62`) and **not applied**. Nothing is deduplicated; replay order
  = call order, so the last value wins. MultiplyTransform, Clear, draws, Get* are not recorded (they act normally;
  BeginScene/EndScene fail as above).
- **EndStateBlock**: not recording → **D3DERR_NOTINBEGINSTATEBLOCK** (0x88760836, `@73920a79`); else recording off,
  the set is finalized (`EndSet_73921716.c`), the driver gets its part (DDI 4/5: `WriteStateSetToDevice`),
  `*pHandle = handle`, D3D_OK. On an exception: the set is freed and **`*pHandle = 0xFFFFFFFF`** (`@73920b14-73920b1c`).
- **CreateStateBlock(type)**: recording → D3DERR_INBEGINSTATEBLOCK (`@73920d17`); StartNewSet failure →
  E_OUTOFMEMORY; then `CreatePredefined` (`CreatePredefined_73922553.c`):
  - type 0 → an **empty** block, success;
  - D3DSBT_ALL (1): render states 4, 2, 29, 7, 8, 9, 10, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 28, 33, 34, 35, 36,
    37, 38, 40, 41, 27, 47, 48, 52-60, 128-135, 139, 141, 140, 136, 137, 138, 143, 142, 148, 147, 145, 146, 151, 152,
    144 (table 0x739cefb8); TSS 1-24 for each stage `< wMaxTextureBlendStages`; textures of those stages; viewport;
    WORLD, VIEW, PROJECTION, WORLD1-3; TEXTUREi for those stages; wMaxUserClipPlanes clip planes; material; every
    light that has been set (SetLight data + its enable state) (lines 19-101);
  - D3DSBT_PIXELSTATE (2): render states 4, 2, 7, 8, 9, 10, 14, 15, 16, 19, 20, 23, 24, 25, 26, 33, 36, 37, 38, 40,
    41, 27, 47, 52-60, 128-135, 144 (table 0x739cf110) and **all TSS 1-24** (including TEXCOORDINDEX and
    TEXTURETRANSFORMFLAGS) (lines 103-126);
  - D3DSBT_VERTEXSTATE (3): render states 9, 29, 22, 28, 34, 35, 36, 37, 38, 48, 139, 141, 140, 136, 137, 138, 143,
    142, 148, 147, 145, 146, 151, 152 (table 0x739cf1c0), TSS TEXCOORDINDEX and TEXTURETRANSFORMFLAGS per stage,
    and the **enable state** of every set light (lines 134-169);
  - any other type → **DDERR_INVALIDPARAMS** (thrown; the set is freed and `*pHandle = 0xFFFFFFFF`, `@73920d84-73920d98`).
  Captured values are the current ones; `*pHandle` set; D3D_OK.
- **ApplyStateBlock(h)**: recording → D3DERR_INBEGINSTATEBLOCK; else `CStateSets::Execute` **with no validation of
  the handle** (`Execute_739224a7.c:9-10`): an unused or deleted handle inside the storage applies nothing
  (D3D_OK); a handle beyond the storage reads out of bounds. Replay (`Execute_739217b6.c`): render states as
  SetRenderStateFast minus the retired test (skip if equal; lines 134-163); TSS as SetTextureStageStateFast
  (185-213); textures through the public **SetTexture** (120-131, so AddRef/Release happen then); transforms,
  clip planes, material, lights (enable/disable/set) through the internal setters; viewport through `SetViewportI`
  (**validated now**: an invalid recorded viewport makes Apply return DDERR_INVALIDPARAMS with the earlier
  commands already applied). Driver errors are returned. Note: a recorded texture pointer was not AddRef'd, so
  applying a block after the game released that texture uses a dangling pointer.
- **CaptureStateBlock(h)**: recording → D3DERR_INBEGINSTATEBLOCK; handle ≥ storage or not a finished block →
  **D3DERR_INVALIDSTATEBLOCK** (0x88760834, `Capture_73922173.c:9-22`). Each recorded item takes the current
  value (`Capture_73921be8.c`); light enable entries are flipped to the current enable state; a light index now
  beyond the light array → **D3DERR_LIGHT_SET_FAILED** (0x887602EE, line 118-122). D3D_OK.
- **DeleteStateBlock(h)**: recording → D3DERR_INBEGINSTATEBLOCK (`@73920b82`); invalid handle →
  D3DERR_INVALIDSTATEBLOCK (`DeleteStateSet_739224f3.c:11-21`); else freed, D3D_OK.

## Load — `Load_7391cc61.c` (codes `@7391cc61-7391ce20`), `CopySurface_7391c15e.c`

`Load(dest, pDestPoint, src, pSrcRect, flags)`. No NULL checks on the surfaces (they and their texture objects are
dereferenced at lines 33-39; a non-texture surface faults).
1. `pDestPoint == NULL`:
   - `pSrcRect == NULL` → whole surface;
   - else: empty rect → DDERR_INVALIDPARAMS; the rect must satisfy `0 ≤ left < W`, `0 ≤ top < H`,
     `1 ≤ right ≤ W`, `1 ≤ bottom ≤ H` where **W, H are the source's** size, else DDERR_INVALIDPARAMS; the
     destination rect is the same rect (lines 40-60).
2. `pDestPoint != NULL`: `pSrcRect` must be non-empty (**NULL counts as empty → DDERR_INVALIDPARAMS**), valid as
   above, and the destination rect (point + rect size) must satisfy the same bounds — **checked against the source
   surface's size, not the destination's** (lines 62-85).
3. `CopySurface(dest, destRect, src, srcRect, flags)`, its result returned (`@7391ce00`):
   - cube-map face bits (`dwCaps2 & 0xFC00`) must be equal on both, else DDERR_INVALIDPARAMS; with a cube map,
     `flags` selects the faces to copy (DDSCAPS2_CUBEMAP_* bits; 0 copies none) (lines 79-87, 488-502);
   - if both have a palette: palette types must match (else DDERR_INVALIDPARAMS) and the source palette's entries
     are copied into the destination palette (2/4/16/256 entries; other formats → DDERR_GENERIC) (lines 90-121);
   - colour keys: the source's DESTBLT and SRCBLT keys are copied to the destination with SetColorKey, or removed
     from the destination if the source has none (lines 122-143);
   - mipmaps: the source level whose size equals the destination's top level is found by walking the source chain,
     halving both rects per level; none → DDERR_INVALIDPARAMS; the destination may not have more levels than the
     source has left, else DDERR_INVALIDPARAMS (lines 171-223);
   - copy, level by level: hardware TexBlt when the driver can (`CanDoTexBlt_73926eff.c`; locked source or
     destination → DDERR_SURFACEBUSY), else `IDirectDrawSurface7::Blt(DDBLT_WAIT)` per level; if Blt returns
     E_NOTIMPL and the format is 1/2/4-bit palettized, a whole-surface row copy through Lock (lines 367-442);
   - **DDERR_SURFACELOST from the copy inside a scene returns D3D_OK** and makes the next EndScene return
     DDERR_SURFACELOST; outside a scene it is returned (lines 386-406, 444-452).
   - a D3D-managed destination is marked dirty (lines 302-330).

## PreLoad — `PreLoad_7391b3ca.c`, `PreLoadFast_7391cb47.c`

Software devices: does nothing, **D3D_OK** (lines 19-23). Hardware devices: `PreLoadFast`; only managed textures do
anything (D3D-managed: upload if dirty; driver-managed: get the driver handle; others: driver TexBlt preload after
flushing other devices). No NULL/texture validation (faults). Result of that work returned.

## ValidateDevice — `ValidateDevice_73925546.c` (codes `@73925546-73925631`)

`This == NULL` → DDERR_INVALIDOBJECT (unreachable); textures brought up to date (UpdateTextures; errors returned),
command buffer flushed (errors returned); `*pdwPasses = 0`; the driver's ValidateTextureStageState callback:
missing or not handled → **E_NOTIMPL (DDERR_UNSUPPORTED)**; handled → `*pdwPasses` = the driver's count and its
ddrval is returned (e.g. D3DERR_UNSUPPORTEDCOLOROPERATION...). NULL pdwPasses faults.

## GetInfo — `GetInfo_7392700e.c` (CDirect3DDevice7, used by DX7 drivers), `GetInfo_7391b4cc.c` (base)

1. `This == NULL`, `pData == NULL` or `size == 0` → **DDERR_INVALIDOBJECT** (lines 209-211).
2. The buffer is zeroed (213-221).
3. D3DDEVINFOID_TEXTUREMANAGER (1) → **S_FALSE** (223-225). D3DD3DTEXTUREMANAGER (2) and TEXTURING (3) →
   GetInfoInternal throws 1, caught and returned → **S_FALSE** (data zero) (`GetInfoInternal_7391b485.c:285-297`,
   catch at `@739270fb-7392711a`; the catch-object slot is inferred).
4. Any other ID: flush (error returned), then the driver's GetDriverState callback: handled with ddrval 0 →
   D3D_OK; handled with an error → data zeroed, **E_FAIL**; not handled → data zeroed, **S_FALSE** (231-262).
   No size check against the ID.

## SetRenderTarget / GetRenderTarget — `SetRenderTarget_7391feea.c` (codes `@7391feea-739201f2`), `SetRenderTargetI_73926b99.c`, `GetRenderTarget_739201fa.c`

SetRenderTarget(pNew, flags) — `flags` ignored:
1. `This == NULL` or **`pNew == NULL` → DDERR_INVALIDOBJECT** (`@7391ff2f`, `@7391ff40`).
2. `GetSurfaceDesc` must succeed, the surface must have **DDSCAPS_3DDEVICE**, its bit depth must be in the device's
   dwDeviceRenderBitDepth, and width/height ≤ 2048; else **DDERR_INVALIDPARAMS** (lines 51-62, `@73920198`).
3. Attached Z buffer looked up (`GetAttachedSurface(DDSCAPS_ZBUFFER)`; none is fine). Any other error →
   DDERR_INVALIDPARAMS (lines 63-65, 147-149).
4. Palette: `GetPalette` error other than DDERR_NOPALETTEATTACHED → DDERR_INVALIDPARAMS; no palette on a target
   below 16 bpp → **DDERR_NOPALETTEATTACHED** (lines 71-83).
5. The driver gets SETRENDERTARGET (RT + Z handles) and the command buffer is flushed; a flush error is returned
   (`SetRenderTargetI_73926b99.c:211-224`).
6. References: the old render target (v7) is released and the new one **AddRef'd**; the old Z buffer released and the
   new one held (lines 123-140). The viewport is **not** changed or revalidated, so a viewport larger than a smaller
   new target stays in effect. No in-scene check. Result of `CalcDDSurfInfo` returned (D3D_OK normally,
   `@73920191`).
GetRenderTarget: `This == NULL` → DDERR_INVALIDOBJECT; NULL out → DDERR_INVALIDPARAMS; else the v7 render target,
AddRef'd, D3D_OK (lines 184-194).

## EnumTextureFormats — `EnumTextureFormats_7391bc47.c` (codes `@7391bc47-7391bd7f`)

`This == NULL` → DDERR_INVALIDOBJECT; `IsBadCodePtr(callback)` → DDERR_INVALIDPARAMS; the driver lists no formats →
**D3DERR_TEXTURE_NO_SUPPORT** (0x887602D0); allocation failure → E_OUTOFMEMORY. Otherwise the callback gets each
format's DDPIXELFORMAT (from a copy of the driver's list) in the driver's order, skipping formats with flag
0x00200000 and, when a DirectDraw-level flag (bit 10 of the dword at +0x48 of the object at `dev+0x22dc`, not
identified) is set, FOURCC formats other than DXT1-DXT5 (lines 191-198). Enumeration stops when the callback
returns anything but D3DENUMRET_OK (1). D3D_OK.

## GetCaps / GetDirect3D — `GetCaps_7391bc00.c`, `GetDirect3D_7391c103.c`

GetCaps: `This == NULL` → DDERR_INVALIDOBJECT; NULL → DDERR_INVALIDPARAMS; copies the 236-byte D3DDEVICEDESC7
(no dwSize in that struct), D3D_OK. GetDirect3D: same checks; returns the IDirect3D7 AddRef'd, D3D_OK.

## ComputeSphereVisibility — `ComputeSphereVisibility_73929522.c`, `ComputeSphereVisibility_739295b9.c`, `CheckClipStatus_7399283f.c`

`(centers, radii, count, flags, pResults)`; flags ignored, no NULL checks, no recording. Dirty transforms are
updated first (line 28-30). The implementation is that of the active geometry pipeline: the SSE/SSE2 pipelines
forward to the generic one (`ComputeSphereVisibility_73953b20.c:13-14`); the AMD 3DNow! one (`x3d_*`) was not read.
Generic algorithm, per sphere: the **axis-aligned box** `center ± radius` (8 corners, in model space) is transformed by
the combined world·view·projection used for clipping, and each corner gets clip codes: LEFT 1 (x<0), RIGHT 2 (x>w),
TOP 4 (y>w), BOTTOM 8 (y<0), FRONT 0x10 (z<0), BACK 0x20 (z>w) in the front end's [0,w] clip space, plus
GEN0..5 (0x40<<i) for each enabled user clip plane with a negative distance (`CheckClipStatus_7399283f.c:33-61`).
`result = union | (intersection << 12)` (D3DSTATUS_CLIPUNION*/CLIPINTERSECTION* bits)
(`ComputeSphereVisibility_739295b9.c:94-97`). Returns D3D_OK. A box test is conservative: a sphere near a frustum
corner can report "partly visible" where an exact sphere test would say "outside".

---

## For dd2d9

dd2d9 has only stubs for Direct3D today (`src/d3dstubs.h`), so this is the list of behaviours to implement.

1. **State storage on the dd2d9 side.** Keep DX7's own arrays (256 render states, 8×64 stage states, 8 textures,
   matrices, viewport, material, growable light array, clip planes, clip status) and answer every Get* from them,
   never from D3D9. Initialise them exactly as `stateInitialize` does for a DDI ≥ 4 device (table above, untouched
   entries 0xFFFFFFFF; TEXCOORDINDEX 0 on all stages; identity matrices; clip status 0x01FFF000 / 2048.0 / 0).
2. **SetRenderState**: no validation; recording rejects retired states with DDERR_INVALIDPARAMS; otherwise store and
   return D3D_OK even for retired states. Ignore (do not forward) states ≥ 256 rather than reproduce XP's memory
   corruption; still return D3D_OK.
3. **GetRenderState**: DDERR_INVALIDPARAMS for retired states, stored value otherwise (0xFFFFFFFF for never-set).
4. **SetTextureStageState/GetTextureStageState**: no validation in XP; dd2d9 should store stages 0-7 and silently
   ignore stage ≥ 8 (D3D_OK) instead of corrupting memory. Keep ADDRESS separate from ADDRESSU/ADDRESSV in storage.
5. **SetTexture**: AddRef new/Release old (real surface references); stage ≥ 8 ignored; a surface that is not a texture
   binds nothing. GetTexture AddRefs. Record (stage, pointer) in state blocks without AddRef, but guard the pointer
   (e.g. validate against live surfaces on Apply) to avoid XP's dangling-pointer crash.
6. **Transforms**: SetTransform ignores bad types with D3D_OK; Get/Multiply return DDERR_INVALIDPARAMS;
   MultiplyTransform = pMatrix × current, computed by dd2d9 and **never recorded** into a state block.
7. **Viewport**: validate only X/Y/X+W/Y+H against the current target; apply the 0.001 MinZ/MaxZ nudge to the stored
   copy; recording stores unvalidated values.
8. **Lights**: CheckLightParams exactly (including the spot-only attenuation test and range check for directional
   lights); grow to index+10; LightEnable creates the default directional light; GetLightEnable returns 0x80.
9. **Clip planes**: no index check in XP; store up to some generous bound (e.g. 32) and ignore beyond.
10. **Clip status**: emulate fully on the dd2d9 side (see implementation notes); EXTENTS3 → DDERR_INVALIDPARAMS;
    GetClipStatus writes dwFlags = EXTENTS2 and leaves minz/maxz alone.
11. **BeginScene/EndScene**: error order as above, including SCENE_END_FAILED for BeginScene while recording,
    SURFACEBUSY when the render target/Z is locked, the in-scene flag cleared before EndScene's later failures, and
    the "lost during scene" SURFACELOST reported by EndScene.
12. **Clear**: count > 4096, Z/stencil presence, `(flags & 7) == 0`; NULL rects = viewport; rects clipped to the
    viewport; count 0 with non-NULL rects = nothing; allowed outside a scene.
13. **State blocks**: handles from 1, LIFO reuse; recording appends in order; the three predefined lists exactly as
    tabled above (including PIXELSTATE capturing TEXCOORDINDEX/TEXTURETRANSFORMFLAGS and VERTEXSTATE capturing only
    light enables); Create/End write 0xFFFFFFFF on failure; Capture/Delete return D3DERR_INVALIDSTATEBLOCK for bad
    handles; Apply of an unknown handle: return D3D_OK doing nothing (XP reads out of bounds; never crash).
14. **Load**: the rect rules above (including the source-size bound on the destination rect and NULL rect with a dest
    point → DDERR_INVALIDPARAMS), palette and colour-key copying, mip-level matching, cube faces selected by flags.
15. **SetRenderTarget**: NULL → DDERR_INVALIDOBJECT; 3DDEVICE/bit depth/2048 checks; palette rule; keep the viewport.
16. **GetInfo**: zero the buffer; S_FALSE for IDs 1-3 and for anything else (no driver).
17. **ValidateDevice**: dd2d9 has no driver callback; XP returns E_NOTIMPL in that case, but games treat that as
    failure. Return D3D_OK with 1 pass unless the stage setup is something D3D9 cannot do (then the matching
    D3DERR_UNSUPPORTED* code). This is a deliberate difference.
18. **EnumTextureFormats**: callback check, stop on non-OK; the list itself belongs to the caps spec.
19. **ComputeSphereVisibility**: the box algorithm above with dd2d9's own matrices and clip planes.

---

## Implementation notes for dd2d9 (on D3D9)

### Render states

| DX7 state | D3D9 | Notes |
| --- | --- | --- |
| ZENABLE 7 | D3DRS_ZENABLE | D3DZB_USEW (2) only if the D3D9 device reports D3DPRASTERCAPS_WBUFFER; DXVK does not, so send D3DZB_TRUE |
| FILLMODE 8, SHADEMODE 9, ZWRITEENABLE 14, ALPHATESTENABLE 15, LASTPIXEL 16, SRCBLEND 19, DESTBLEND 20, CULLMODE 22, ZFUNC 23, ALPHAREF 24, ALPHAFUNC 25, DITHERENABLE 26, ALPHABLENDENABLE 27, FOGENABLE 28, SPECULARENABLE 29, FOGCOLOR 34, FOGTABLEMODE 35, FOGSTART 36, FOGEND 37, FOGDENSITY 38, RANGEFOGENABLE 48, STENCIL* 52-59, TEXTUREFACTOR 60, WRAP0-7 128-135, CLIPPING 136, LIGHTING 137, AMBIENT 139, FOGVERTEXMODE 140, COLORVERTEX 141, LOCALVIEWER 142, NORMALIZENORMALS 143, *MATERIALSOURCE 145-148, VERTEXBLEND 151, CLIPPLANEENABLE 152 | same number | values identical (blend, cmp, stencil op, fill, shade, cull, fog mode, material source, vertex blend 0-3, wrap bits) |
| ALPHAREF 24 | same | DX7 HALs used 0-255; D3D9 the same |
| ZBIAS 47 | D3DRS_DEPTHBIAS (195, float) + maybe SLOPESCALEDEPTHBIAS | needs D3DPRASTERCAPS_DEPTHBIAS; the scale per ZBIAS unit (depends on depth format) is uncertain — Wine converts with a small negative factor; tune by test |
| COLORKEYENABLE 41 | emulated | design §1: key → alpha 0 at upload, ALPHATEST(GREATER, 0) while on, combined with the game's own alpha test |
| COLORKEYBLENDENABLE 144 | emulated | same keyed-alpha texture; with blending on the keyed texels are transparent |
| EDGEANTIALIAS 40 | optionally D3DRS_ANTIALIASEDLINEENABLE (176) | lines only; ignoring is acceptable |
| ANTIALIAS 2, TEXTUREPERSPECTIVE 4, LINEPATTERN 10, ZVISIBLE 30, STIPPLEDALPHA 33, 42, FLUSHBATCH 50 | none | store for Get, do not forward (D3D9 has no such states; perspective is always on) |
| EXTENTS 138 | none | drives dd2d9's clip-status extents emulation |
| retired states (list above) | none | stored, never forwarded, Get fails |
| 0, 61-63, 96-127, 149, 150, 153-255 | **never forward** | D3D9 assigns meanings to 154+ (POINTSIZE, MULTISAMPLEANTIALIAS, ...) |

### Texture stage states

| DX7 TSS | D3D9 | Notes |
| --- | --- | --- |
| COLOROP..ALPHAARG2 1-6, BUMPENVMAT00-11 7-10, TEXCOORDINDEX 11, BUMPENVLSCALE 22, BUMPENVLOFFSET 23, TEXTURETRANSFORMFLAGS 24 | SetTextureStageState, same numbers | D3DTOP/D3DTA/TCI/TTFF values identical |
| ADDRESS 12 | sampler ADDRESSU **and** ADDRESSV | stored DX7 ADDRESSU/V are not changed by it |
| ADDRESSU 13, ADDRESSV 14 | D3DSAMP_ADDRESSU/V | same values (WRAP 1, MIRROR 2, CLAMP 3, BORDER 4) |
| BORDERCOLOR 15 | D3DSAMP_BORDERCOLOR | |
| MAGFILTER 16 | D3DSAMP_MAGFILTER | **value change**: DX7 POINT 1, LINEAR 2, FLATCUBIC 3, GAUSSIANCUBIC 4, ANISOTROPIC 5 → D3D9 POINT 1, LINEAR 2, LINEAR (cubics; D3D9 3 means ANISOTROPIC), ANISOTROPIC 3 |
| MINFILTER 17 | D3DSAMP_MINFILTER | POINT 1, LINEAR 2, ANISOTROPIC 3: same numbers |
| MIPFILTER 18 | D3DSAMP_MIPFILTER | **value change**: DX7 NONE 1, POINT 2, LINEAR 3 → D3D9 NONE 0, POINT 1, LINEAR 2 (0 is invalid in DX7; treat as NONE) |
| MIPMAPLODBIAS 19, MAXMIPLEVEL 20, MAXANISOTROPY 21 | D3DSAMP_MIPMAPLODBIAS / MAXMIPLEVEL / MAXANISOTROPY | LOD bias is a float in both |
| 0, 25-63 | none | type 0 is d3dim's internal texture handle |

D3D9 defaults that differ and must be set explicitly at device creation: **TEXCOORDINDEX = 0 on every stage**
(D3D9 uses the stage number), ZENABLE (Z attached), and stage 1-7 COLOROP/ALPHAOP DISABLE (already D3D9's default).
Because dd2d9 shares one D3D9 device with its 2D presenter (design §2), the presenter must save and restore every
render/sampler/stage state, texture, viewport, transform and shader it touches (a D3D9 state block captured around
presentation does this), or the game's DX7 state is silently changed.

### Transforms, lights, material, viewport, clip planes

- Transform types: WORLD 1 → D3DTS_WORLD (256), WORLD1-3 4-6 → D3DTS_WORLDMATRIX(1..3) (257-259); VIEW 2,
  PROJECTION 3, TEXTURE0-7 16-23 are the same numbers.
- D3DLIGHT7/D3DLIGHT9, D3DMATERIAL7/D3DMATERIAL9 and D3DVIEWPORT7/D3DVIEWPORT9 have identical layouts. Validate
  lights with XP's rules before calling D3D9 (the D3D9 runtime's own checks differ; DXVK checks little).
- D3D9 drivers (DXVK: 8) limit simultaneously enabled lights (D3DCAPS9.MaxActiveLights); d3dim700 imposes no limit
  for software-T&L devices. If a game enables more, pick the first N in DX7's active-list order (newest enabled
  first, `LightEnableI_73928456.c:155-163`) and log it.
- Viewport: apply the MinZ/MaxZ nudge before passing it on. **D3D9 SetRenderTarget resets the viewport to the full
  target**; re-apply dd2d9's stored DX7 viewport after every D3D9 render-target change (DX7 keeps it).
- Clip planes are world-space in both with the fixed-function pipeline; D3D9 accepts only `< MaxUserClipPlanes`
  (DXVK 6), so forward only those; CLIPPLANEENABLE bits are the same.

### Things D3D9 does not do that dd2d9 must emulate

- **Clip status** (Set/GetClipStatus, D3DRENDERSTATE_EXTENTS): D3D9's D3DCLIPSTATUS9 is a different structure and is
  only computed with software vertex processing; DXVK does not compute it. To be exact, dd2d9 must transform each
  untransformed vertex batch on the CPU when a game reads clip status (cheap to detect: compute only after the game
  has called GetClipStatus once). Until then: return the stored values.
- **ComputeSphereVisibility**: entirely on the CPU (box test above).
- **Legacy/removed states** listed above: stored, not forwarded.
- **State blocks**: implement DX7 blocks in dd2d9 (recorded DX7 calls replayed through dd2d9's own setters), not as
  D3D9 state blocks: the predefined DX7 lists, handle numbering, in-order replay, viewport validation on apply,
  colour-key and legacy-state emulation and non-recorded MultiplyTransform all differ from D3D9's.
- **GetLightEnable** must return 0x80 from dd2d9's own storage.
- **Clear** argument rules: D3D9 fails `count > 0 && pRects == NULL` (DX7 clears the viewport) — pass `count = 0`;
  handle `count = 0 && pRects != NULL` as a no-op before calling D3D9; check Z/stencil presence before D3D9 does.
- **Load** palette/colour-key copying and mip matching (D3D9 UpdateTexture has different rules); P8 textures are
  converted at upload anyway (design §1), so a palette copy must mark the destination for re-upload.
- **BeginScene/EndScene**: D3D9 needs Begin/End pairs around draws on the shared device; dd2d9's 2D presenter must not
  present inside a game's DX7 scene, and the DX7 scene flag and error codes are dd2d9's own.
- **ValidateDevice**: D3D9's ValidateDevice is meaningful for fixed-function stages on native drivers but DXVK returns
  D3D_OK for nearly everything; prefer dd2d9's own checks (see item 17).

### Pitfalls

- DXVK: no W-buffer, no cubic magnification filters, no P8 formats, clip status not computed, ValidateDevice lenient.
- D3D9's defaults differ from DX7's (TEXCOORDINDEX, ZENABLE); always push the full DX7 default set once the D3D9
  device exists and after every D3D9 Reset (Reset restores D3D9 defaults).
- Do not forward unknown DX7 render-state numbers: numbers ≥ 154 mean something in D3D9.
- Keep MAGFILTER/MIPFILTER value translation in one place: both are easy to pass through unchanged by mistake, and
  MIPFILTER = NONE (1) unchanged would turn on point mipmapping.
- A recorded texture in a DX7 state block is not referenced; a game may release it and later apply the block.

---

## Not determined

- Whether a NULL/bad out-pointer inside the try blocks is turned into an error code (would need /EHa catch(...)
  handlers) or crashes. Only the explicit NULL checks are listed above.
- Which device class (and so DDI level, `0x31ac`) each device GUID gets on XP — decided in `Direct3DCreateDevice`
  (CreateDevice spec). It decides whether ANTIALIAS/EDGEANTIALIAS etc. read 0 or 0xFFFFFFFF.
- The surfaces DirectDraw announces to `CreateTexture` (so what SetTexture/Load do with non-texture surfaces).
- The DirectDraw flag hiding non-DXT FOURCC formats in EnumTextureFormats, and the meaning of pixel-format flag 0x00200000.
- The ZBIAS → DEPTHBIAS scale to use on D3D9.
- The AMD 3DNow! (`x3d_ComputeSphereVisibility_73977180.c`) variant of ComputeSphereVisibility.
- The priority/LOD re-send at the end of `stateInitialize` (lines 198-212): which caps bit enables it.
- The exact catch-object slot in `CDirect3DDevice7::GetInfo` (S_FALSE for IDs 2/3 is inferred from the thrown value 1).
