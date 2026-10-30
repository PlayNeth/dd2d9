# Direct3D 3–6 (d3dim.dll): viewports, materials, lights, textures, execute buffers, DX6 vertex buffers

Reference: Windows XP `d3dim.dll` 5.1.2600.0 (the Direct3D runtime for the DX3–DX6 interfaces;
retail build, PDB with names but no types). File references `File.c:line` are to the Ghidra
output in `$S/d3d-decomp/d3dim-named/` (S = the session scratchpad; one file per function, named
`<method>_<address>.c`). Several classes share method names; the address picks the class (see
`$S/d3d-decomp/d3dim-functions.txt`). `@address` marks facts read from the disassembly of
`$S/xpd3d/d3dim.dll` (llvm-objdump). Vtables were located through the PDB's public symbols
(`??_7…`) and dumped from the binary:

| Vtable | Address | Slots |
|---|---|---|
| DIRECT3DVIEWPORTI (one vtable for IDirect3DViewport, 2 and 3) | 0x6dea1ff8 | QI, AddRef, Release, Initialize, GetViewport, SetViewport, TransformVertices, LightElements, SetBackground, GetBackground, SetBackgroundDepth, GetBackgroundDepth, Clear, AddLight, DeleteLight, NextLight, GetViewport2, SetViewport2, SetBackgroundDepth2, GetBackgroundDepth2, Clear2 |
| DIRECT3DMATERIALI for IDirect3DMaterial / 2 / 3 | 0x6dea2214 / 0x6dea21fc / 0x6dea21e4 | v1: QI, AddRef, Release, Initialize (= viewport's), SetMaterial, GetMaterial, GetHandle(IDirect3DDevice*), Reserve, Unreserve (Reserve and Unreserve are one function, 0x6deb0091). v2/v3: QI, AddRef, Release, SetMaterial, GetMaterial, GetHandle |
| DIRECT3DLIGHTI | 0x6dea2ea4 | QI, AddRef, Release, Initialize (= viewport's), SetLight, GetLight |
| DIRECT3DTEXTUREI for IDirect3DTexture / 2 | 0x6dea177c / 0x6dea1764 | v1: QI, AddRef, Release, Initialize (= the execute buffer's), GetHandle, PaletteChanged, Load, Unload. v2: QI, AddRef, Release, GetHandle, PaletteChanged, Load |
| DIRECT3DEXECUTEBUFFERI | 0x6dea2dbc | QI, AddRef, Release, Initialize, Lock, Unlock, SetExecuteData, GetExecuteData, Validate, Optimize |
| CDirect3DVertexBuffer (IDirect3DVertexBuffer, DX6) | 0x6dea1b1c | QI, AddRef, Release, Lock, Unlock, ProcessVertices, GetVertexBufferDesc, Optimize |

Ghidra mislabels COM parameters: `param_1` is `This` and every later parameter is shifted one
place (the decompile's `param_2` is the first real argument, the last argument shows up as
`in_stack_000000xx`). Below, arguments are named by the real API. Where a decompile compares a
pointer with 4, 8 or 0xc instead of 0, it is testing a NULL interface pointer after an interface
adjustment (e.g. `param_1 == 0x8` = "This of IDirect3DMaterial3 is NULL").

**Big picture, read first.**
* This is the DX6 runtime, so every IDirect3DDevice/2/3 on XP is a `CDirect3DDeviceIDP2`
  (DrawPrimitives2 driver interface): the only Execute implementation is
  `CDirect3DDeviceIDP2::ExecuteI` (the index has no other class with device methods). The old
  "DDI < 3" branches below (device+0xf58 < 3) exist in the code but don't run with XP's drivers;
  they are described only where they explain an odd result.
* Almost every method takes DirectDraw's global lock (`_AcquireDDThreadLock`) and is wrapped in
  SEH. **Any access fault inside such a method returns DDERR_INVALIDPARAMS** (handler code
  `mov eax, 0x80070057` found in every one, e.g. Clear `@6deac8ba–6deac8d1`, Clear2 `@6deac6ae`,
  SetViewport `@6deabddd`, TransformVertices `@6deac211`, Execute `@6dea4586`, Lock
  `@6deb48a4`; AddRef/Release return 0 on a fault). So a garbage pointer gives
  DDERR_INVALIDPARAMS, not a crash, and some "missing checks" below (a viewport without device,
  a NULL rect array) also end as DDERR_INVALIDPARAMS. The IDirect3DVertexBuffer methods and the
  execute-buffer QI have no SEH and no NULL checks.
* NULL `This` is tested explicitly and gives DDERR_INVALIDOBJECT; a NULL or wrongly sized
  structure gives DDERR_INVALIDPARAMS. Beyond that validation is thin, in the same spirit as
  DX7 (`d3d7-draw-vb.md`).
* Handles (material, texture, matrix) are **raw pointers or driver handles, not validated**.
  A bad handle reads arbitrary memory (caught by SEH where present).

## 0. Common names

HRESULTs (decompile shows them as negative ints):

| Value | Name |
|---|---|
| 0x88760082 (−0x7789ff7e) | DDERR_INVALIDOBJECT |
| 0x80070057 (−0x7ff8ffa9) | DDERR_INVALIDPARAMS |
| 0x8007000E (−0x7ff8fff2) | E_OUTOFMEMORY / DDERR_OUTOFMEMORY |
| 0x80004001 (−0x7fffbfff) | E_NOTIMPL / DDERR_UNSUPPORTED |
| 0x80004002 | E_NOINTERFACE |
| 0x80004005 (−0x7fffbffb) | E_FAIL / DDERR_GENERIC |
| 0x80040110 (−0x7ffbfef0) | CLASS_E_NOAGGREGATION |
| 0x88760005 (−0x7789fffb) | DDERR_ALREADYINITIALIZED |
| 0x887601C2 | DDERR_SURFACELOST |
| 0x887602C6 / C8 / CA / CB | D3DERR_EXECUTE_CREATE_FAILED (710) / EXECUTE_LOCK_FAILED (712) / EXECUTE_LOCKED (714) / EXECUTE_NOT_LOCKED (715) |
| 0x887602D0 / D1 / D2 / D5 / D6 | D3DERR_TEXTURE_NO_SUPPORT (720) / TEXTURE_CREATE_FAILED (721) / TEXTURE_DESTROY_FAILED (722) / TEXTURE_LOAD_FAILED (725) / TEXTURE_SWAP_FAILED (726) |
| 0x887602E2 | D3DERR_TEXTURE_BADSIZE (738) |
| 0x887602E4 … E7 | D3DERR_MATERIAL_CREATE_FAILED (740), _DESTROY_ (741), _SETDATA_ (742), _GETDATA_FAILED (743) |
| 0x887602EF / F0 | D3DERR_LIGHTHASVIEWPORT (751) / LIGHTNOTINTHISVIEWPORT (752) |
| 0x88760302 / 304 / 305 / 306 / 307 | D3DERR_INBEGIN (770) / NOVIEWPORTS (772) / VIEWPORTDATANOTSET (773) / VIEWPORTHASNODEVICE (774) / NOCURRENTVIEWPORT (775) |
| 0x88760800 / 80C / 80E / 816 / 817 / 825 | D3DERR_INVALIDVERTEXFORMAT / VERTEXBUFFEROPTIMIZED / VERTEXBUFFERLOCKED / ZBUFFER_NOTPRESENT / STENCILBUFFER_NOTPRESENT / TOOMANYVERTICES |
| 0x88760BB8 | D3DERR_COMMAND_UNPARSED (3000, driver → runtime only) |

Device fields used here (DIRECT3DDEVICEI; the front-end block D3DFE_PROCESSVERTICES the PV
functions receive starts at **device+0x10**, so PV offsets are 0x10 less):

| Offset | Meaning (inferred from use) |
|---|---|
| +0x04 | interface version the device was created for: 1 = IDirect3DDevice (execute buffers), 2 = IDirect3DDevice2, 3 = IDirect3DDevice3 (D3DFE_Create_6dea834c.c:30–37, 66–68) |
| +0x20 | combined world·view·projection·Mclip matrix used by the transforms (updateTransform_6decc77e.c:77) |
| +0x114 | current material handle (D3DLIGHTSTATE_MATERIAL) (SetMaterial_6decd97a.c:15, 25) |
| +0x14c | extents adjust (driver's dvExtentsAdjust, else 1.0) (D3DFE_Create_6dea834c.c:48–54) |
| +0x150 … +0x15c | screen scale/offset: W, −H, X, Y+H (UpdateViewportCache_6decb659.c:52–57) |
| +0x170 … +0x17c | viewport X, Y, X+W, Y+H as floats; +0x1b8 … +0x1c4 the same rounded to ints |
| +0x1c8 … +0x1dc | Mclip coefficients (§1.6); +0x1e0 … +0x1f4 its inverse (updateInverseMclip.c) |
| +0xc0 | head of the device's active-light list (setLights_6deab9f3.c:14–21) |
| +0xe24 | bit 2: guard band in use; bit 4 (0x4): device version 1 (D3DFE_Create:56–68) |
| +0xe40 | per-operation front-end flags: 0x4 don't clip, 0x8 don't update extents, 0x400 light, 0x800000 "execute-buffer D3DTRIANGLE stream" |
| +0xea4 … +0xeb0 | extents minX, minY, maxX, maxY (float) (D3DFE_updateExtents_6deb28de.c:12–23) |
| +0xf1c | dirty/state flags (bit 0x4: "TL vertices copied this Execute") |
| +0xf50 | id of the viewport whose data was last sent ("downloaded") to the device |
| +0xf54 | current viewport (DIRECT3DVIEWPORTI*) |
| +0xf58 | DDI level (≥ 3 on XP) |
| +0xf80 / +0xf84 / +0xf8c / +0xf94 | lists: texture blocks / execute buffers / viewports / material blocks |
| +0xfcc / +0xfd0 | render target / Z buffer surfaces |
| +0x12e0 | D3DSTATUS dwStatus shared by Execute and Get/SetClipStatus; initial 0x01FFF000 (D3DFE_Create_6dea834c.c:153) |
| +0x15d0 | render-state array (`[30]` = D3DRENDERSTATE_ZVISIBLE) |

---------------------------------------------------------------------------------------------

## 1. IDirect3DViewport, IDirect3DViewport2, IDirect3DViewport3 (DIRECT3DVIEWPORTI)

One object, one vtable for all three interfaces (the v1/v2/v3 pointers are the same pointer).
Object (0x7c bytes, CreateViewport_6deaca83.c:24):

| Offset | Meaning |
|---|---|
| +0x04 | reference count, starts at 1 (DIRECT3DVIEWPORTI_6deaca30.c:17) |
| +0x08 / +0x0c / +0x10 | owning DIRECT3DI; link in its viewport list (hookViewportToD3D_6deab997.c:10–19) |
| +0x14 | device it was added to (NULL until AddViewport) |
| +0x18 / +0x1c | link in the device's viewport list |
| +0x20 | light count; +0x24 / +0x28 light list (first = newest, last = oldest) |
| +0x2c | id, unique per Direct3D object (counter at D3D+0x40) (DIRECT3DVIEWPORTI:25–26) |
| +0x30 … +0x58 | D3DVIEWPORT2 as last set (11 DWORDs) |
| +0x5c | "viewport data set" flag |
| +0x60 / +0x64 | background-material valid flag / handle |
| +0x68 | background depth surface (IDirectDrawSurface*) |
| +0x70 | "lights changed" flag (starts 1) |
| +0x74 / +0x78 | clear-rect scratch array count / pointer |

### 1.1 Creation, QueryInterface, AddRef, Release, Initialize

* IDirect3D::CreateViewport / IDirect3D2::CreateViewport are thunks to IDirect3D3's with
  pUnkOuter = NULL (CreateViewport_6deaba65.c:10, CreateViewport_6deaba7b.c:10).
  IDirect3D3::CreateViewport (CreateViewport_6deaca83.c): pUnkOuter ≠ NULL →
  CLASS_E_NOAGGREGATION (14, 46); This NULL → DDERR_INVALIDOBJECT (16–17); lplpViewport NULL →
  DDERR_INVALIDPARAMS (19–20); `*lplp = NULL` (23); allocation failure → E_OUTOFMEMORY (29–30).
  The viewport is linked into the Direct3D object's list and a counter is incremented there; **no
  AddRef of the Direct3D object** (hookViewportToD3D:10–19).
* QueryInterface (QueryInterface_6deb59a7.c): This NULL → DDERR_INVALIDOBJECT (18–19); ppv NULL
  → DDERR_INVALIDPARAMS (24–25); `*ppv = NULL` (28); IUnknown, IDirect3DViewport,
  IDirect3DViewport2, IDirect3DViewport3 → AddRef, same pointer, S_OK (29–84); else
  E_NOINTERFACE (76).
* AddRef / Release: one count; This NULL → 0; Release to 0 destroys (AddRef_6deb5a6e.c:13–20,
  Release_6deb5abc.c:13–23).
* Destructor (~DIRECT3DVIEWPORTI_6deb593e.c): deletes every light (each DeleteLight releases
  that light) (10–13); frees the rect scratch (14–16); **if still attached to a device, calls the
  device's DeleteViewport** (17–20); unlinks from the Direct3D object (21–29). **The background
  depth surface (+0x68) is not released** (leak; see §1.10).
* Initialize (Initialize_6deb520d.c:7): always DDERR_ALREADYINITIALIZED (also used as
  IDirect3DMaterial::Initialize and IDirect3DLight::Initialize, vtable dumps above).

### 1.2 Lifetime with devices (device methods, for reference)

* `AddViewport(vp)` (AddViewport_6dea41af.c; v1/v2 thunk to it, AddViewport_6dea4187.c:9):
  viewport already attached to a device (any device) → DDERR_INVALIDPARAMS (11, 16); else link
  into the device, set vp+0x14, **AddRef the viewport** (12–13, hookViewportToDevice_6dea4150.c).
  No NULL check (fault → DDERR_INVALIDPARAMS).
* `DeleteViewport(vp)` (DeleteViewport_6dea4210.c): NULL → DDERR_INVALIDPARAMS (15–16); inside
  Begin/End (device+0xf04 bit 1) → D3DERR_INBEGIN (20, 55); not attached to this device →
  DDERR_INVALIDPARAMS (21, 51); unlink, vp+0x14 = NULL (22–38); if it is the current viewport:
  Release it (the current-viewport reference) and clear current (39–46); Release (the
  AddViewport reference) (47).
* `SetCurrentViewport(vp)` (SetCurrentViewport_6deb15ce.c): This NULL → DDERR_INVALIDOBJECT;
  vp NULL → DDERR_INVALIDPARAMS; INBEGIN; vp not attached to this device → DDERR_INVALIDPARAMS;
  if its data is set and it is not the downloaded one, download it (§1.6) — failure is returned
  and the old current viewport kept; else AddRef new, Release old (14–48).
* `GetCurrentViewport`: none → D3DERR_NOCURRENTVIEWPORT; else AddRef and return
  (GetCurrentViewport_6deb16a6.c:20–29). `NextViewport`: no viewports → D3DERR_NOVIEWPORTS
  (NextViewport_6dea4332.c:35–36), flags 1/2/4 as for NextLight (§1.14).
* Device destruction calls DeleteViewport for every attached viewport
  (DestroyDevice_6dea5889.c:24–27).
* Execute temporarily makes its viewport current **without AddRef** and does not restore the
  previous one on most error paths (§6.1).

### 1.3 SetViewport2(lpData) — SetViewport2_6deabdf7.c

| # | Check / action | Line |
|---|---|---|
| 1 | This NULL → DDERR_INVALIDOBJECT. | 19–21 |
| 2 | lpData NULL or dwSize ≠ 0x2c → DDERR_INVALIDPARAMS. | 25–27 |
| 3 | Not attached to a device → **D3DERR_VIEWPORTHASNODEVICE**. | 29–32 |
| 4 | Device version ≥ 2 only (IDirect3DDevice2/3): dvClipWidth ≠ 0 and dvClipHeight ≠ 0 and dvMinZ ≠ dvMaxZ, and dwX ≤ W_rt, dwY ≤ H_rt, dwX+dwWidth ≤ W_rt, dwY+dwHeight ≤ H_rt (unsigned, render-target width/height from its surface; wrap-around not caught); otherwise DDERR_INVALIDPARAMS. Negative clip sizes, dwWidth/dwHeight 0, dvMinZ > dvMaxZ are accepted. | 34, 48–59 |
| 4' | **Version-1 devices (IDirect3DDevice): no validation at all.** | 34 |
| 5 | Copy the 11 DWORDs to +0x30, set "data set". | 35–42 |
| 6 | If this viewport is the one downloaded to the device (id = device+0xf50), download again (§1.6); its error is returned. Otherwise nothing reaches the device now: the data is used when the viewport is next made current, used by Execute/Clear/TransformVertices, or (DX6 ProcessVertices) is current. | 43–46 |

### 1.4 SetViewport(lpData) (D3DVIEWPORT) — SetViewport_6deabcd7.c

| # | Check / action | Line |
|---|---|---|
| 1 | This NULL → DDERR_INVALIDOBJECT. | 31–33 |
| 2 | lpData NULL or dwSize ≠ 0x2c → DDERR_INVALIDPARAMS. | 36, 68 |
| 3 | Convert to a D3DVIEWPORT2 (dwX, dwY, dwWidth, dwHeight copied): if dvScaleX == 0 or dvScaleY == 0 then clipW = clipH = 0, else **clipW = dwWidth/dvScaleX, clipH = dwHeight/dvScaleY** (unsigned → float); **dvClipX = −clipW/2, dvClipY = +clipH/2**; dvMinZ = 0, dvMaxZ = 1. **dvMaxX, dvMaxY, dvMinZ, dvMaxZ of the D3DVIEWPORT are ignored.** | 38–63 |
| 4 | Call SetViewport2 through the vtable (+0x44) and return its result (so §1.3 validation applies: a zero scale on a Device2/3 gives DDERR_INVALIDPARAMS). | 64 |

Resulting mapping (§1.6): sx = X + W/2 + dvScaleX·(x/w), sy = Y + H/2 − dvScaleY·(y/w),
sz = z/w — the classic D3DVIEWPORT semantics (scale in pixels per unit, origin at the centre).

### 1.5 GetViewport2 / GetViewport

* GetViewport2 (GetViewport2_6deac015.c): This NULL → DDERR_INVALIDOBJECT (15–16); lpData NULL or
  dwSize ≠ 0x2c → DDERR_INVALIDPARAMS (19, 35); data never set → **D3DERR_VIEWPORTDATANOTSET**
  (20–21); else copy the 11 stored DWORDs (24–30). No device needed.
* GetViewport (GetViewport_6deabf27.c): same first two checks (30–33, 35, 61); calls
  GetViewport2 on a local copy (37–38) and then **writes the caller's structure even when that
  failed** (stack garbage on VIEWPORTDATANOTSET) and returns GetViewport2's result (39–59). Fields:
  dwX…dwHeight as stored, **dvScaleX = dwWidth/dvClipWidth, dvScaleY = dwHeight/dvClipHeight,
  dvMaxX = dvClipX + dvClipWidth, dvMaxY = dvClipY, dvMinZ = 0, dvMaxZ = 1** (float stores
  `@6deabf91–6deabfd3`; the decompile's `(int)` casts are wrong).

### 1.6 The transform a viewport implies (downloadView, UpdateViewportCache, updateTransform)

`downloadView` (downloadView_6deab934.c): data not set → D3DERR_VIEWPORTDATANOTSET (10–11);
UpdateViewportCache (14); the device's viewport method (vtable +0x78, sends the viewport to the
driver) (15); on success mark the viewport's lights dirty and record its id as downloaded
(16–19). `ValidateViewport` / `CheckDeviceSettings` download when the viewport isn't the
downloaded one (ValidateViewport_6deab979.c:9–13, CheckDeviceSettings_6dea3597.c:9–12).

UpdateViewportCache (UpdateViewportCache_6decb659.c, lines below are of that file) does nothing
**at all** if dwWidth, dwHeight, dvClipWidth or dvClipHeight is 0 or dvMaxZ − dvMinZ == 0 (19–22;
reachable only on version-1 devices, which skip validation; the previous mapping stays). Else,
with X, Y, W, H the viewport rectangle and cx, cy, cw, ch, zmin, zmax the clip fields:

* **Mclip**, post-multiplied onto the projection (updateTransform_6decc77e.c:10–76; the combined
  matrix is world·view·projection·Mclip, 74–78):
  `x' = (x − cx·w)/cw`, `y' = y/ch + w·(1 − cy/ch)`, `z' = (z − zmin·w)/(zmax − zmin)`, `w' = w`
  (coefficients at device+0x1c8…0x1dc: 1/cw, −cx/cw, 1/ch, 1 − cy/ch, 1/(zmax−zmin),
  −zmin/(zmax−zmin); lines 44–55).
  So the visible volume is **0 ≤ x' ≤ w, 0 ≤ y' ≤ w, 0 ≤ z' ≤ w**, i.e. the projected point
  x/w ∈ [cx, cx+cw], **y/w ∈ [cy−ch, cy]** (dvClipY is the *top* edge), z/w ∈ [zmin, zmax].
* **Screen mapping**: `sx = X + W·x'/w`, `sy = Y + H − H·y'/w`, `sz = z'/w`, `rhw = 1/w`
  (scale/offset W, −H, X, Y+H: lines 52–57). **Screen z is always 0…1**: in DX5/6, D3DVIEWPORT2
  dvMinZ/dvMaxZ select the clip-space z range that maps onto the full depth range (unlike DX7's
  D3DVIEWPORT7, where they are the output range).
* If device flag 0xf1c bit 3 is set, 1e-6 (`_DAT_6dea3154`) is added to the x and y offsets
  (58–62).
* Guard band (device+0xe24 bit 2): clip limits are derived from the driver's guard band
  (78–103); else from the viewport rectangle (105–108). Extents are clamped to the viewport
  rectangle (+0x170…+0x17c) or its rounded copy (+0x1b8…).

### 1.7 TransformVertices(dwVertexCount, lpData, dwFlags, lpOffscreen) — TransformVertices_6deac09d.c

| # | Check / action | Line |
|---|---|---|
| 1 | This NULL → DDERR_INVALIDOBJECT. | 38–40 |
| 2 | lpData NULL or dwSize ≠ 0x34 → DDERR_INVALIDPARAMS. | 43, 108 |
| 3 | (dwFlags & 3) == 0 → DDERR_INVALIDPARAMS. Only the low two bits matter: **== 1 is D3DTRANSFORM_CLIPPED; 2 or 3 are UNCLIPPED**; other bits ignored. | 45–46, 70 |
| 4 | Viewport not downloaded → download (§1.6); D3DERR_VIEWPORTDATANOTSET etc. returned. **No device → access fault → DDERR_INVALIDPARAMS** (SEH). | 49–51 |
| 5 | Make this viewport current for the call (restored afterwards), copy D3DTRANSFORMDATA, convert drExtent to floats, rebuild the matrix if dirty. | 52–68, 100 |
| 6 | CLIPPED: recompute the inverse Mclip if needed, clear "don't clip"; UNCLIPPED: set "don't clip". | 70–81 |
| 7 | Front end TransformVertices; its return value → `*lpOffscreen` (**lpOffscreen not checked**: NULL → fault → DDERR_INVALIDPARAMS, after the work is done). | 89–90 |
| 8 | Copy the structure back; extent → integers (§ below). Return D3D_OK. | 91–101 |

Front end (D3DFE_PVFUNCS::TransformVertices, TransformVertices_6deb3880.c:13–18):

* **Input layout**: lpIn with stride dwInSize; position = DWORDs 0–2; **DWORDs 4–7 are copied
  verbatim into the output's color, specular, tu, tv** (whatever they are: for a D3DVERTEX that
  is ny, nz, tu, tv). No lighting, ever. dwOutSize is the output stride; output is D3DTLVERTEX.
* **CLIPPED** (D3DFE_TransformClippedVp_6decc1e2.c): per vertex, clip-space x', y', z', w' (§1.6);
  clip code = D3DCLIP_LEFT x'<0, RIGHT x'>w', TOP y'>w', BOTTOM y'<0, FRONT z'<0, BACK z'>w'
  (bits 1…0x20, from sign bits, 72–74). Inside (code 0): sx, sy, sz = z'/w', rhw = 1/w' written
  and the extent grown (75–97). Outside: **sx, sy, sz untouched**, only rhw = 1/w'
  (**2097152.0 when w' = 0**) (98–108). Every vertex: `lpHOut[i].dwFlags = code` and
  hx, hy, hz = the vertex back in the viewport's own clip coordinates (inverse Mclip applied to
  x', y', z') (63–68, 109; **lpHOut must be valid even for inside vertices**).
  dwClipIntersection = AND of all codes, dwClipUnion = OR (105–106, 142–143). Return value
  (→ *lpOffscreen) = the intersection: non-zero iff every vertex is outside the same plane;
  **with dwVertexCount = 0 it is 0xFFFFFFFF** (38, 121, 145). dwClip is never read.
* **UNCLIPPED** (D3DFE_TransformUnclippedVp_6decc4ce.c, lines of that file): every vertex
  projected (no clip codes, lpHOut, dwClipIntersection/Union untouched) (43–90). On a version-1
  device, if the extent left the viewport, **every output x, y is clamped to the viewport
  rectangle** (109–129). Returns 0.
* **Extent**: drExtent is an *in/out* union: it starts from the caller's drExtent (converted to
  float), grows by the vertices, then is widened by the extents-adjust value (default 1.0) on
  each side and clamped to the viewport rectangle (Clipped 122–141, Unclipped 91–108). Then
  D3DFE_ConvertExtent: round each edge, **x1−1, y1−1, x2+1, y2+1**, clamp to the integer viewport
  rectangle (D3DFE_ConvertExtent_6decbd35.c:9–28). A caller that does not
  initialise drExtent gets garbage mixed in.

### 1.8 LightElements — LightElements_6deaba91.c:8

Always E_NOTIMPL (no checks).

### 1.9 SetBackground(hMat) / GetBackground(lphMat, lpValid)

* SetBackground (SetBackground_6deac233.c): This NULL → DDERR_INVALIDOBJECT (14–16); **hMat == 0
  → DDERR_INVALIDPARAMS** (19–20); else store, valid = 1 (23–24). The handle is not checked.
  There is no way to clear the background back to "none".
* GetBackground (GetBackground_6deac2a5.c): This NULL → DDERR_INVALIDOBJECT; lphMat NULL →
  DDERR_INVALIDPARAMS (17, 29); not set: `*lphMat = 0, *lpValid = FALSE`; else
  `*lpValid = TRUE, *lphMat = handle` (18–25). lpValid unchecked.

### 1.10 SetBackgroundDepth(lpDDS) / SetBackgroundDepth2 / GetBackgroundDepth(2)

* SetBackgroundDepth (SetBackgroundDepth_6deac330.c): This NULL → DDERR_INVALIDOBJECT; **Release
  the previous surface, store the new one without AddRef** (20–24); D3D_OK. NULL clears.
  (Refcount bug: the viewport consumes the game's reference.)
* SetBackgroundDepth2 (IDirectDrawSurface4) (SetBackgroundDepth2_6deac39e.c): This NULL →
  DDERR_INVALIDOBJECT; non-NULL: QueryInterface(IID_IDirectDrawSurface) (this AddRefs), its
  failure is returned (30–32); Release previous, store the IDirectDrawSurface (33–38).
* GetBackgroundDepth (GetBackgroundDepth_6deac4c0.c): lplpDDS NULL → DDERR_INVALIDPARAMS;
  `*lplpDDS = surface` **without AddRef**, `*lpValid = (surface != NULL)` (18–22).
* GetBackgroundDepth2 (GetBackgroundDepth2_6deac42d.c): `*lplpDDS4 = NULL`; if set,
  QI(IID_IDirectDrawSurface4) (AddRefs) and its result is returned; `*lpValid = (*lplpDDS4 != 0)`
  (20–28).
* Use: only the old-DDI Clear blits it (§1.11 note). On XP's DP2 path **Clear ignores it**;
  Clear2 refuses D3DCLEAR_ZBUFFER while one is set (§1.12). Never released by the destructor.

### 1.11 Clear(dwCount, lpRects, dwFlags) — Clear_6deac6c8.c

| # | Check / action | Line |
|---|---|---|
| 1 | This NULL → DDERR_INVALIDOBJECT. | 36–38 |
| 2 | No device → **D3DERR_VIEWPORTHASNODEVICE**. | 41–42, 111 |
| 3 | D3DCLEAR_ZBUFFER requested and the device has no Z buffer, unless the driver is D3DPRASTERCAPS_ZBUFFERLESSHSR (with a clear callback or DP2): **the ZBUFFER bit is dropped silently; if nothing is left, return D3D_OK** (no other check runs). | 44–48, 104–105 |
| 4 | (dwFlags & 3) == 0 → DDERR_INVALIDPARAMS. Bits other than TARGET/ZBUFFER are not rejected; **D3DCLEAR_STENCIL (4) is passed on** to the DP2 clear (with stencil value 0) when TARGET or ZBUFFER is also set. | 49–50, 94–97 |
| 5 | Download the viewport if needed (VIEWPORTDATANOTSET etc. returned). | 53 |
| 6 | ProcessRects (§1.13): E_OUTOFMEMORY possible. | 54–56 |
| 7 | Background colour: no background → colour (0,0,0,0), no texture; else read the material from its handle (fails only for handle 0, impossible here) (57–68). | 57–69 |
| 8 | Make the viewport current for the call (restored after). | 70–73, 100 |
| 9 | DP2 path: if the background material has a **texture handle** and TARGET is requested: flush, then **fill the rects with that texture** (TriFillRectsTex, below) and drop TARGET (80–86). If TARGET/ZBUFFER remain: DP2 clear of the rects with **colour = ARGB(a·255, r·255, g·255, b·255) from the material's *diffuse*, each truncated (`__ftol`) and not clamped** (a value > 1.0 spills into the next byte), **Z = 1.0, stencil = 0** (87–98; multiplier 255.0 at 0x6dea1928, order a,r,g,b `@6deac82a–6deac872`). | 74–99 |
| 10 | Return D3D_OK (the DP2 clear's errors surface only as a flush exception → DDERR_INVALIDPARAMS via SEH). | 101–109 |

TriFillRectsTex (TriFillRectsTex_6dea878e.c): inside a BeginScene/EndScene of its own if the
game is not in a scene (95–99, 228–231); saves, sets and afterwards restores ZENABLE 0,
ZWRITEENABLE 0, ZFUNC ALWAYS, WRAPU/WRAPV 0, FILLMODE SOLID, FOGTABLEMODE 0, FOGENABLE 0,
ALPHABLENDENABLE 0, COLORKEYENABLE 0, STENCILENABLE 0, TEXTUREPERSPECTIVE 0, DITHERENABLE 0,
WRAP0–7 0 and **TEXTUREHANDLE = the background's hTexture** (100–141, 205–224); TEXTUREMAPBLEND
and filtering are left as the game set them. Per rect, one DrawPrimitive(TRIANGLEFAN,
D3DTLVERTEX, 4 vertices, D3DDP_WAIT|DONOTCLIP) with the rect's corners, z = 0, rhw = 1, colour
0xFFFFFFFF, specular 0 and **tu = (x − (vpX − 0.41))/vpW, tv = (y − (vpY − 0.41))/vpH** (the
texture is stretched over the whole viewport; 0.41 = 0x3ed1eb85) (73–93, 158–202). Ramp devices
use a ramp service instead (142–156).

Old-DDI path (D3DFE_Clear_6dea8df3.c, not used on XP): background texture → TriFillRectsTex,
else a colour blit fill; ZBUFFER: background depth set → the whole background-depth surface is
stretch-blitted onto the viewport rectangle of the Z buffer (rects ignored), else Z filled with
0xFFFFFFFF (121–150).

### 1.12 Clear2(dwCount, lpRects, dwFlags, dwColor, dvZ, dwStencil) — Clear2_6deac540.c

| # | Check / action | Line |
|---|---|---|
| 1 | This NULL → DDERR_INVALIDOBJECT. | 23–27 |
| 2 | No device → D3DERR_VIEWPORTHASNODEVICE. | 29–32 |
| 3 | Not ZBUFFERLESSHSR: (ZBUFFER or STENCIL) and no Z buffer → **D3DERR_ZBUFFER_NOTPRESENT**; STENCIL and the Z buffer's pixel format lacks DDPF_STENCILBUFFER → **D3DERR_STENCILBUFFER_NOTPRESENT**. | 36–46 |
| 3' | ZBUFFERLESSHSR without DP2/clear callback (not XP): STENCIL → ZBUFFER_NOTPRESENT; ZBUFFER with dvZ ≠ 1.0 or no clear callback → ZBUFFER dropped. | 48–56 |
| 4 | (dwFlags & 7) == 0 → DDERR_INVALIDPARAMS; **ZBUFFER while a background depth surface is set → DDERR_INVALIDPARAMS**. Other bits not rejected. | 57–58 |
| 5 | Download the viewport; ProcessRects. | 61–64 |
| 6 | Make the viewport current for the call; DP2 clear with the given flags, colour, Z, stencil (no range checks on dvZ). Return D3D_OK. | 65–80 |

### 1.13 ProcessRects (shared by Clear and Clear2) — ProcessRects_6deaba99.c

1. Grow the viewport's scratch array to dwCount; failure → E_OUTOFMEMORY and the array is
   dropped (16–23).
2. Clip rectangle: **version-1 device (IDirect3DDevice): the whole render target**; otherwise the
   viewport rectangle (X, Y, X+W, Y+H) (24–36).
3. Each rect is intersected with it (IntersectRect); **empty results are dropped** (37–49).
4. **dwCount = 0 → zero rects → nothing is cleared** (the DP2 clear is still sent with 0 rects,
   ClearI_6deab1ea.c:20–48). lpRects NULL with dwCount > 0 → fault → DDERR_INVALIDPARAMS.
   D3DRECT is not normalised (x2 < x1 yields an empty intersection → dropped).

### 1.14 AddLight / DeleteLight / NextLight; how lights reach the device

* AddLight(lpLight) (AddLight_6deac8d4.c): This NULL → DDERR_INVALIDOBJECT (13–14); lpLight NULL →
  DDERR_INVALIDPARAMS (17, 32); **light already in a viewport (this or another) →
  D3DERR_LIGHTHASVIEWPORT** (18, 26–27); link at the head (hookLightToViewport_6deab9c4.c),
  lights dirty, **AddRef the light** (19–23).
* DeleteLight(lpLight) (DeleteLight_6deac970.c): This NULL → DDERR_INVALIDOBJECT; lpLight NULL →
  DDERR_INVALIDPARAMS; not in this viewport → **D3DERR_LIGHTNOTINTHISVIEWPORT** (19, 41–42); unlink,
  lights dirty, **Release the light** (20–39).
* NextLight(lpLight, lplpLight, dwFlags) (NextLight_6deabbca.c), **in this order**: lplpLight NULL
  → DDERR_INVALIDPARAMS (17–21) (before the This check); `*lplpLight = NULL` (23); This NULL →
  DDERR_INVALIDOBJECT (24–26); if dwFlags & 1: lpLight NULL → DDERR_INVALIDPARAMS, lpLight not in
  this viewport → D3DERR_LIGHTNOTINTHISVIEWPORT (28–37). Then dwFlags == D3DNEXT_NEXT (1): the
  light added after lpLight; == D3DNEXT_HEAD (2): the **first-added** light; == D3DNEXT_TAIL (4):
  the **last-added** light; any other value (0, 3, 5, …) → DDERR_INVALIDPARAMS (38–53). End of
  list / empty → `*lplpLight = NULL` and **D3D_OK** (57–60). A returned light is AddRef'd (61–64).
* Reaching the device: when vertices are lit and the device has a material, and the current
  viewport's lights are dirty, `setLights` rebuilds the device's light list from that viewport
  (DoUpdateState_6dea9770.c:12–20): it walks from the first-added light and **includes a light
  only if SetLight was ever called on it and it is active** (D3DLIGHT: always; D3DLIGHT2:
  dwFlags & D3DLIGHT_ACTIVE) (setLights_6deab9f3.c:17–26), then marks lighting dirty (28).
  Lights are therefore a property of the viewport in use: Execute's viewport, or the current one
  for DrawPrimitive and ProcessVertices. A light's SetLight, AddLight, DeleteLight and download
  set the viewport's dirty flag (D3DI_UpdateLightInternal_6deb5215.c:85–87, downloadView:17).

---------------------------------------------------------------------------------------------

## 2. IDirect3DLight (DIRECT3DLIGHTI)

Object 0x128 bytes: +0x04 refcount (1), +0x08…+0x10 Direct3D link, +0x14 viewport, +0x18/+0x1c
viewport links, +0x20 the D3DLIGHT/D3DLIGHT2 as given (0x50 bytes), +0x70… internal light
(D3DI_LIGHT: +0x8c version 1/2, +0x9c flags, +0xbc "set", position, direction, …).

* IDirect3D*::CreateLight (CreateLight_6deb5691.c): pUnkOuter → CLASS_E_NOAGGREGATION (12, 46);
  This NULL → DDERR_INVALIDOBJECT; lplpLight NULL → DDERR_INVALIDPARAMS; `*lplp = NULL`;
  E_OUTOFMEMORY (14–30). Linked into the Direct3D object, no AddRef of it (hookLightToD3D_6deb51e0.c).
  All fields zero (DIRECT3DLIGHTI_6deb5667.c:10–22): GetLight before SetLight returns dwSize 0.
* QueryInterface: IUnknown, IDirect3DLight (QueryInterface_6deb604a.c:17–38). AddRef/Release as for
  viewports; the destructor unlinks from its viewport (marking it dirty) and from Direct3D
  (~DIRECT3DLIGHTI_6deb5fc9.c:10–37). While in a viewport the viewport holds a reference.

### 2.1 SetLight(lpLight) — SetLight_6deb5417.c

| # | Check | Line |
|---|---|---|
| 1 | This NULL → DDERR_INVALIDOBJECT. | 17–18 |
| 2 | lpLight NULL or dwSize ∉ {0x4c (D3DLIGHT), 0x50 (D3DLIGHT2)} → DDERR_INVALIDPARAMS. | 20–23 |
| 3 | **D3DLIGHT (0x4c): no validation at all**, copied (26, 35–49). | |
| 4 | D3DLIGHT2: dltType ∉ {POINT 1, SPOT 2, DIRECTIONAL 3, PARALLELPOINT 4} → DDERR_INVALIDPARAMS (**D3DLIGHT_GLSPOT 5 rejected**). | 27–28, 80–81 |
| 5 | dvRange < 0 or > √FLT_MAX → DDERR_INVALIDPARAMS. | 29–32 |
| 6 | SPOT or DIRECTIONAL: \|dvDirection\|² < 1e-5 → DDERR_INVALIDPARAMS. | 51–54, 75–76 |
| 7 | SPOT: dvPhi ∈ [0, π] and dvTheta ∈ [0, dvPhi], else DDERR_INVALIDPARAMS. | 55–72 |
| 8 | Copy dwSize bytes; D3DI_UpdateLightInternal; D3D_OK. | 35–49 |

GetLight (GetLight_6deb55d8.c): dwSize of the caller's structure must be 0x4c or 0x50
(DDERR_INVALIDPARAMS otherwise) but **the stored structure is copied with the stored dwSize**
(23–34): a D3DLIGHT2 set and read with a D3DLIGHT-sized buffer overruns it by 4 bytes; before any
SetLight nothing is copied.

### 2.2 Internal light and lighting model (D3DI_UpdateLightInternal, D3DFE_UpdateLights, _PointSpot*)

D3DI_UpdateLightInternal (D3DI_UpdateLightInternal_6deb5215.c): D3DLIGHT → version 1, flags =
D3DLIGHT_ACTIVE always (15–18); D3DLIGHT2 → version 2, flags = dwFlags (19–22). Direction is
normalised (38); range and range² kept (39–40); a ramp brightness 0.3r + 0.59g + 0.11b (41–44);
spot: cos(θ/2), cos(φ/2) (46–50); version 1 spot falloff factor 1/(cos(θ/2) − cos(φ/2)) or 0
(51–58); version 2 keeps dvFalloff and 1/(cos(θ/2) − cos(φ/2)) (or 1) (59–69); version 2 notes
which attenuation terms are non-zero and whether falloff == 1 (71–84); "set" = 1 (36).

The two versions use **different lighting functions** (table at device+0x12fc chosen per light,
D3DFE_UpdateLights_6decda45.c:237–269: `_Directional1C`, `_PointSpot1C` for D3DLIGHT,
`_Directional2`, `_PointSpot2` for D3DLIGHT2):

| | D3DLIGHT (DX3/5 semantics) — `_PointSpot1C_6decd5f2.c` | D3DLIGHT2 (DX6) — `_PointSpot2_6decd286.c` |
|---|---|---|
| Range | **ignored** | d² ≥ dvRange² → no contribution (36) |
| Attenuation | att = a0 + a1·d + a2·d²; **att > 1000 → no contribution; att ≤ 1 (incl. negative) → treated as 1**; intensity ∝ N·L / (d·att) (22–29) | **intensity factor = a0 + a1·(R−d)/R + a2·((R−d)/R)²** (R = dvRange), each term only if non-zero (39–51) — a *multiplier*, not 1/att |
| Spot | cosα ≤ cos(φ/2) → none; between: linear `1 − (cos(θ/2) − cosα)·k` (30–40); **dvFalloff ignored** | cosα ≤ cos(φ/2) → none; between: `((cosα − cos(φ/2))·k)^dvFalloff` (pow skipped when falloff = 1) (54–69) |
| Specular | if the material has power > 0.001 and the light has specular (53–58) | as left, unless D3DLIGHT_NO_SPECULAR (D3DFE_UpdateLights:228–233) |

Both require N·L > 0 (\_PointSpot1C:19, \_PointSpot2:36–38). D3DLIGHT_PARALLELPOINT (4) is lit as
a directional light whose direction is the (model-space) position vector itself
(D3DFE_UpdateLights:143–148), i.e. light arriving from the position toward the origin; it uses
the directional function (253–254, 266–267). A D3DLIGHT with dltType 5 or garbage gets **no lighting
function** (D3DFE_UpdateLights:253, 266 leave it NULL; likely a fault when lit: *uncertain*).
Lighting needs a material: with material handle 0 the light flag is cleared and vertices are not
lit (DoUpdateState_6dea9770.c:12–30; §6.4).

---------------------------------------------------------------------------------------------

## 3. IDirect3DMaterial, IDirect3DMaterial2, IDirect3DMaterial3 (DIRECT3DMATERIALI)

Object 0x74 bytes: vtables at +0 (v1), +4 (v2), +8 (v3); +0x0c refcount (1); +0x10…+0x18
Direct3D link; +0x1c list of per-device handle blocks; +0x20 D3DMATERIAL (dwSize preset 0x50, the
rest 0) (DIRECT3DMATERIALI_6deb00ee.c:10–21).

* Creation: IDirect3D3::CreateMaterial (CreateMaterial_6deb018e.c): pUnkOuter →
  CLASS_E_NOAGGREGATION; This NULL → DDERR_INVALIDOBJECT; lplp NULL → DDERR_INVALIDPARAMS;
  `*lplp = NULL`; E_OUTOFMEMORY (14–30); returns the v3 pointer (34). IDirect3D/IDirect3D2
  versions call it and convert the pointer to the v1/v2 interface (CreateMaterial_6deafd88.c:10–13,
  CreateMaterial_6deafdb9.c:10–15). No AddRef of Direct3D.
* QueryInterface (QueryInterface_6deb5cae.c): IUnknown and IDirect3DMaterial3 → v3 pointer;
  IDirect3DMaterial2 → v2; IDirect3DMaterial → v1 (29–96). One shared count.
* Destructor (~DIRECT3DMATERIALI_6deb5c5d.c): destroys every per-device handle (10–12), unlinks.
* Initialize → DDERR_ALREADYINITIALIZED (vtable shares the viewport's).
* **Reserve / Unreserve** (Reserve_6deb0091.c, one function): This NULL → DDERR_INVALIDOBJECT,
  else **E_NOTIMPL** (12–20). (Ramp colour reservation is not supported by this runtime.)

### 3.1 SetMaterial / GetMaterial

* SetMaterial (SetMaterial_6deafe7e.c): This NULL → DDERR_INVALIDOBJECT (19–20); lpMat NULL or
  dwSize ≠ 0x50 → DDERR_INVALIDPARAMS (23, 55). **No other validation** (any power, any hTexture,
  dwRampSize). If identical to the stored one (memcmp of 0x50 bytes) → D3D_OK, nothing else
  (24–36). Else store it and **update every device handle's copy** (D3DHAL_MaterialSetData:
  copies; if that handle is the device's current material, re-applies it immediately)
  (37–48; D3DHAL_MaterialSetData_6deb5c09.c:15–23). An error from a handle stops the loop and is
  returned (47).
* GetMaterial (GetMaterial_6deaff31.c): This NULL → DDERR_INVALIDOBJECT; NULL or dwSize ≠ 0x50 →
  DDERR_INVALIDPARAMS; copy 0x50 bytes (19–27). Before any SetMaterial: dwSize 0x50, all zero.

### 3.2 GetHandle(lpDevice, lphMat)

v1/v2 convert the device pointer and call the v3 method (GetHandle_6deafe00.c:10–11,
GetHandle_6deafe26.c:10–13). v3 (GetHandle_6deaffac.c):

| # | Check / action | Line |
|---|---|---|
| 1 | This NULL → DDERR_INVALIDOBJECT. | 24–29 |
| 2 | lpDevice NULL → **DDERR_INVALIDOBJECT**. | 31–34 |
| 3 | lphMat NULL → DDERR_INVALIDPARAMS. | 36–39 |
| 4 | Look for this material's block for that device; found → same handle again (**one handle per material per device, stable**). | 42, 52 |
| 5 | Else create: a 0x5c-byte block holding a copy of the D3DMATERIAL, linked into the device; **the handle value is the block's address** (D3DHAL_MaterialCreate_6deb5b41.c:14–31); allocation failure → D3DERR_MATERIAL_CREATE_FAILED (15–16); then link the block to the material and device (hookMaterialToDevice; E_OUTOFMEMORY, handle destroyed) (44–50). | 43–51 |

Handle lifetime: until the material is destroyed (destructor) or the device is destroyed
(DestroyDevice_6dea5889.c:67–69). Destroying a handle that is the device's current material sets
the current material to 0 (D3DHAL_MaterialDestroy_6deb5bae.c:13–15) — lighting then stops.
**Viewport backgrounds keep the stale handle** (Clear would then read freed memory).

Uses of a material handle: D3DLIGHTSTATE_MATERIAL (device SetLightState / execute buffer
STATELIGHT, §6.5) — `SetMaterial(device, h)` copies the D3DMATERIAL from the handle
(SetMaterial_6decd97a.c:14–25), power ≤ 0.001 disables specular (27–30), h = 0 means "no
material"; the viewport background (§1.11: diffuse colour, hTexture for a textured fill); ramp
devices. The material's hTexture is otherwise unused by RGB lighting.

---------------------------------------------------------------------------------------------

## 4. IDirect3DTexture, IDirect3DTexture2 (DIRECT3DTEXTUREI)

The texture object is **aggregated by the DirectDraw surface**: QueryInterface(IID_IDirect3DTexture
or IID_IDirect3DTexture2) on a texture surface calls `Direct3DCreateTexture(riid, surface, ppUnk,
pUnkOuter = surface)` (Direct3DCreateTexture_6dea7092.c). Object 0xd0 bytes: +0 v1 vtable, +4 v2
vtable, +8 list of per-device handle blocks, +0xc inner IUnknown (CDirect3DTextureUnk) with its
own count at +0x10, +0x30/+0x34 the surface (IDirectDrawSurface / IDirectDrawSurface4) or
+0x38/+0x3c for a DDSCAPS2_TEXTUREMANAGE surface, +0xc4 outer unknown (DIRECT3DTEXTUREI_6dea7024.c).

* Direct3DCreateTexture: riid other than IDirect3DTexture/2 → E_NOINTERFACE (26–52);
  E_OUTOFMEMORY (54–60); the surface is QI'd for IDirectDrawSurface4 and released again (no
  reference kept on its own surface) (62–76); managed surfaces walk the mip chain (84–103);
  paletted formats record whether a palette is attached (107–126).
* IDirect3DTexture(2)::QueryInterface/AddRef/Release **delegate to the outer unknown, i.e. the
  surface** (QueryInterface_6deb4dd5.c:18–19, AddRef_6deb4e53.c:18–19, Release_6deb4ea9.c:18–19):
  the texture lives as long as the surface; QI for IDirectDrawSurface* from a texture works.
* Destructor (~DIRECT3DTEXTUREI_6deb4c2c.c): destroys every per-device handle (13–34).

### 4.1 GetHandle(lpDevice, lpHandle)

v1 (GetHandle_6dea6aa8.c) and v2 (GetHandle_6dea6b7d.c):

| # | Check / action | v1 line | v2 line |
|---|---|---|---|
| 1 | This, lpDevice or lpHandle NULL → **DDERR_INVALIDOBJECT** (all three). | 15–18 | 17–20 |
| 2 | Managed texture (DDSCAPS2_TEXTUREMANAGE) → DDERR_INVALIDOBJECT. | 21, 40–41 | 22–24 |
| 3 | Find this texture's block for the device. | 22 | 27 |
| 4 | New block: **v2 only**: VerifyTextureCaps → D3DERR_TEXTURE_BADSIZE when the device requires power-of-two sizes (D3DPTEXTURECAPS_POW2 without NONPOW2CONDITIONAL) and a side isn't, or requires square (SQUAREONLY) and it isn't (VerifyTextureCaps_6dea69bf.c:12–28). Allocation failure → E_OUTOFMEMORY. | 24–27 | 29–36 |
| 5 | `*lpHandle = 0` is written before step 4 (v1 line 23, v2 line 28) — a failure leaves 0. | | |
| 6 | No handle yet in the block → D3DHAL_TextureCreate; on the DP2 path **the handle is the DirectDraw surface's kernel handle** (local surface +0x6c) (D3DHAL_TextureCreate_6deaf5ea.c:36–38); old DDI: driver TextureCreate (TEXTURE_NO_SUPPORT / TEXTURE_CREATE_FAILED) (16–34). | 30–37 | 38–44 |

One handle per texture per device, stable; the same surface gives the same value on every device
(DP2). Mapping back to a surface for rendering: D3DRENDERSTATE_TEXTUREHANDLE is passed straight to
the driver in the DP2 command stream (SetRenderStateI_6deaa6d8.c:17–49); the runtime's own lookup
(TEXTURELOAD, D3DHAL_TextureGetSurf_6deaf809.c:37–42) walks the device's texture blocks for a
matching handle → that texture's surface. Handle 0 = no texture (SetDeviceRenderState_6deb1556.c:12–21).

Handle destruction (texture released with its surface, or device destroyed:
DestroyDevice:41–62) (D3DI_RemoveTextureHandle_6dea65ab.c, D3DHAL_TextureDestroy_6deaf66a.c):
* every material handle on that device whose hTexture equals it is re-set with **hTexture = 0**
  (RemoveTextureHandle:16–30);
* texture stages bound to it are unbound (TextureDestroy:29–39); if
  D3DRENDERSTATE_TEXTUREHANDLE equals it, it is set to 0 (40–45).

### 4.2 Load, PaletteChanged, Unload, Initialize

* Load(lpSrc) (v1 Load_6dea6e17.c, v2 Load_6dea6ea3.c): This or lpSrc NULL →
  DDERR_INVALIDOBJECT (v1 15–18, v2 16–19); then `CopySurface(dest, src)` — the same helper as
  IDirect3DDevice7::Load without rectangles (see `d3d7-device-state.md`, Load) — on the managed
  copy if there is one; its result is returned (v1 20–28, v2 22–30). Neither handles nor devices
  are involved.
* PaletteChanged(dwStart, dwCount) (PaletteChanged_6dea6f98.c): This NULL → DDERR_INVALIDOBJECT;
  arguments ignored; only ramp devices are told (19–25); D3D_OK.
* Unload (Unload_6dea6f3e.c): This NULL → DDERR_INVALIDOBJECT, else D3D_OK, no effect (13–20).
* Initialize (v1 only) → DDERR_ALREADYINITIALIZED (shared with the execute buffer, Initialize_6deb47d7.c:8).
* IDirect3DDevice2/3::SwapTextureHandles exists (SwapTextureHandles_6dea4e9f.c) — device scope,
  not covered.

---------------------------------------------------------------------------------------------

## 5. IDirect3DExecuteBuffer (DIRECT3DEXECUTEBUFFERI)

Object 100 bytes: +0x04 refcount (1), +0x08 device, +0x0c/+0x10 device-list link, +0x14 locking
process id, +0x2c D3DEXECUTEDATA (0x30 bytes, zero until set), +0x5c locked flag, +0x60 buffer
block (D3DHAL_AllocateBuffer: +0x14 size, +0x1c the DirectDraw surface)
(DIRECT3DEXECUTEBUFFERI_6deb4b06.c).

### 5.1 IDirect3DDevice::CreateExecuteBuffer(lpDesc, lplpEB, pUnkOuter) — CreateExecuteBuffer_6deb4b39.c

| # | Check / action | Line |
|---|---|---|
| 1 | This NULL → DDERR_INVALIDOBJECT. | 20–22 |
| 2 | lpDesc NULL, dwSize ≠ 0x14, or lplpEB NULL → DDERR_INVALIDPARAMS. | 24–25, 67 |
| 3 | pUnkOuter → CLASS_E_NOAGGREGATION. | 26–28 |
| 4 | `*lplpEB = NULL`. dwFlags == 0, dwBufferSize == 0, or dwBufferSize > the device's dwMaxBufferSize (if non-zero) → DDERR_INVALIDPARAMS. D3DDEB_BUFSIZE need not be among the flags; dwCaps and lpData are ignored. | 30, 38–39, 67 |
| 5 | E_OUTOFMEMORY; the buffer is **a DirectDraw surface DDSCAPS_EXECUTEBUFFER\|DDSCAPS_SYSTEMMEMORY, width = dwBufferSize — always system memory, D3DDEBCAPS_VIDEOMEMORY ignored** (D3DHAL_AllocateBuffer_6deb571a.c:49–55); failure → D3DERR_EXECUTE_CREATE_FAILED (83). | 40–53 |
| 6 | Linked into the device (no AddRef either way) (hookBufferToDevice_6deb47aa.c). The caller's descriptor is **not** written back. | 55–58 |

Device destruction releases each of its execute buffers **until it is destroyed** (loop until the
list is empty, DestroyDevice_6dea5889.c:63–66): game references to them dangle afterwards.

### 5.2 Methods

| Method | Behaviour (check order) | Ref |
|---|---|---|
| QueryInterface | **No lock, no SEH.** This NULL → DDERR_INVALIDOBJECT; ppv NULL → DDERR_INVALIDPARAMS; `*ppv = NULL`; IUnknown / IDirect3DExecuteBuffer → AddRef, S_OK; else E_NOINTERFACE. | QueryInterface_6deb5e96.c:17–53 |
| AddRef / Release | one count; Release to 0: unlock if locked, unlink, free the buffer surface. | Release_6deb5f6b.c:18–23; ~DIRECT3DEXECUTEBUFFERI_6deb5e4c.c:8–21 |
| Initialize | DDERR_ALREADYINITIALIZED. | Initialize_6deb47d7.c:8 |
| Lock(lpDesc) | This NULL → DDERR_INVALIDOBJECT; lpDesc NULL or dwSize ≠ 0x14 → DDERR_INVALIDPARAMS; already locked → **D3DERR_EXECUTE_LOCKED**; mark locked, remember the process id; lock the surface with DDLOCK_WAIT, **retrying while DDERR_WASSTILLDRAWING**; failure → D3DERR_EXECUTE_LOCK_FAILED and unlocked again. On success the descriptor is overwritten: **dwSize 0x14, dwFlags = D3DDEB_LPDATA (4), dwCaps 0, dwBufferSize, lpData**. The caller's flags are ignored. Being a system-memory surface, lpData is the same on every Lock. | Lock_6deb47fb.c:25–64; D3DHAL_LockBuffer_6deb58b7.c:22–40 (file lines) |
| Unlock | This NULL → DDERR_INVALIDOBJECT; not locked → **D3DERR_EXECUTE_NOT_LOCKED**; called from another process → DDERR_INVALIDPARAMS; unlock the surface (its result returned) and clear the flag. | Unlock_6deb48bd.c:14–35 |
| SetExecuteData(lpData) | This NULL → DDERR_INVALIDOBJECT; NULL or dwSize ≠ 0x30 → DDERR_INVALIDPARAMS; copy. **Offsets and counts are not validated** (not against the buffer size either); allowed while locked. | SetExecuteData_6deb4951.c:16–30 |
| GetExecuteData(lpData) | This NULL → DDERR_INVALIDOBJECT; NULL → DDERR_INVALIDPARAMS; **dwSize not checked**; copy 0x30 bytes (dsStatus as left by the last Execute). | GetExecuteData_6deb49cc.c:16–30 |
| Validate | This NULL → DDERR_INVALIDOBJECT; else **E_NOTIMPL**. | Validate_6deb4a42.c:14–20 |
| Optimize(dwFlags) | This NULL → DDERR_INVALIDOBJECT; dwFlags ≠ 0 → DDERR_INVALIDPARAMS; else **E_NOTIMPL**. | Optimize_6deb4a9f.c:13–21 |

---------------------------------------------------------------------------------------------

## 6. IDirect3DDevice::Execute and the instruction stream

### 6.1 Execute(lpExecBuf, lpViewport, dwFlags) — Execute_6dea444e.c

| # | Check / action | Line |
|---|---|---|
| 1 | Device NULL or lpExecBuf NULL → DDERR_INVALIDOBJECT. | 32–41 |
| 2 | lpViewport NULL → the current viewport (none → fault → DDERR_INVALIDPARAMS). | 43–46 |
| 3 | Viewport not attached to this device → DDERR_INVALIDPARAMS. | 48, 97 |
| 4 | Make it the current viewport (**no AddRef**); download it if needed — failure (e.g. D3DERR_VIEWPORTDATANOTSET) restores the old current viewport and is returned. | 49–54 |
| 5 | Execute buffer not created on this device → DDERR_INVALIDPARAMS; **locked → D3DERR_EXECUTE_LOCKED**. (From here on the previous current viewport is **not** restored on failure.) | 55–56, 91–92 |
| 6 | Flush; build the internal execute data from the buffer's D3DEXECUTEDATA (dwVertexOffset, dwVertexCount, dwInstructionOffset, dwInstructionLength, dwHVertexOffset); **the D3DSTATUS part starts zeroed** (SetExecuteData's dsStatus is not an input). Output vertex format TLVERTEX, front-end flags reset to 0x800000. | 57–72 |
| 7 | ExecuteI(data, dwFlags) (§6.2); its error is returned. | 73 |
| 8 | Success: clear "TL vertices copied", flush, **copy the status out into the buffer's dsStatus** (dwFlags = 0, dwStatus, drExtent), restore the previous current viewport, D3D_OK. | 74–86 |

### 6.2 ExecuteI — CDirect3DDeviceIDP2::ExecuteI (ExecuteI_6deadc06.c)

Set-up, in order:

1. Render target or Z buffer lost → **DDERR_SURFACELOST** (28–34).
2. Update managed textures (35).
3. dwFlags: **2 = D3DEXECUTE_UNCLIPPED** (sets "don't clip"); **1 = D3DEXECUTE_CLIPPED**; anything
   else (0, 3, other bits) → DDERR_INVALIDPARAMS (36–41).
4. Unless the driver has D3DDEVCAPS_CANRENDERAFTERFLIP, the first drawing instruction waits for
   a pending flip (42–44, 154–157).
5. Instructions start at **buffer + dwInstructionOffset** (45–51). **dwVertexOffset and
   dwHVertexOffset are never read** (only `param_1+0xc/0x10/0x14` are used: lines 46, 52, 162–163):
   PROCESSVERTICES reads source vertices at **buffer + wStart·32** (argument verified
   `@6deadda3–6deaddae`: the 4th argument's +0x10 is the buffer base stored at ebp−0x18).
   **dwInstructionLength is used only to tell the driver how much follows** (161–165); the runtime
   itself runs until D3DOP_EXIT (or an unknown opcode). Nothing is checked against the buffer
   size.
6. The device's TL vertex buffer and clip-code buffer are grown for dwVertexCount: size = 4096 if
   dwVertexCount > 4096, else max(dwVertexCount, the driver's dwMaxVertexCount)
   (GrowTLandHbuffers_6deadbb9.c:10–18; E_OUTOFMEMORY possible). Vertex indices are indices into
   this TL buffer (32-byte D3DTLVERTEX). (*Uncertain*: whether > 4096 vertices then overflow; the
   clamp reads as written.)

Instruction loop (D3DINSTRUCTION = bOpcode, bSize, wCount, then data). **The runtime advances by
fixed structure sizes, not by bSize** (only D3DOP_SPAN uses bSize):

| Opcode | Data (size) | Action | Line |
|---|---|---|---|
| D3DOP_POINT 1 | D3DPOINT {wCount, wFirst} (4) ×wCount | Draw each run of points from the TL buffer, clipped (D3DFEClipPointsHW). wCount = 0 → nothing. | 191–216 |
| D3DOP_LINE 2 | D3DLINE {v1, v2} (4) | Indexed line list, clipped (D3DFEClipLinesHW). | 218–231 |
| D3DOP_TRIANGLE 3 | D3DTRIANGLE {v1, v2, v3, wFlags} (8) | Indexed triangle list, clipped (D3DFEClipTrianglesHW → ProcessClippedIndexedTriangleList_6decef41.c): triangles fully outside one plane dropped, fully inside drawn as is (the D3DTRIANGLE records incl. wFlags go to the driver: index stride 4 WORDs, :38), others clipped in software with wFlags giving the edge flags of the new edges (:85–91). **Edge flags (D3DTRIFLAG_EDGEENABLE1–3) only matter for wireframe fill; STARTFLAT/ODD/EVEN are not used by the runtime.** | 234–249 |
| D3DOP_MATRIXLOAD 4 | D3DMATRIXLOAD {hDest, hSrc} (8) | §6.3 | 78–89 |
| D3DOP_MATRIXMULTIPLY 5 | D3DMATRIXMULTIPLY {hDest, hSrc1, hSrc2} (12) | §6.3 | 104–110 |
| D3DOP_STATETRANSFORM 6 | D3DSTATE (8) | §6.3 | 91–95 |
| D3DOP_STATELIGHT 7 | D3DSTATE (8) | §6.5 | 97–100 |
| D3DOP_STATERENDER 8 | D3DSTATE (8) | For each: state < 0x101: skipped if its override bit is set, else stored and sent to the driver; **state ≥ 0x101 (D3DSTATE_OVERRIDE(n) = n + 256): value ≠ 0 sets, 0 clears the override bit of state n** (and is also forwarded to the driver). No state-number validation. | 257–268; trackState_6dead325.c:11–29 |
| D3DOP_PROCESSVERTICES 9 | D3DPROCESSVERTICES (16) | §6.4 | 70–76 |
| D3DOP_TEXTURELOAD 10 | D3DTEXTURELOAD {hDest, hSrc} (8) | §6.6 | 112–116 |
| D3DOP_EXIT 11 | — | Stop: extent → dsStatus.drExtent (D3DFE_ConvertExtent, §1.7), dwStatus → dsStatus.dwStatus, return D3D_OK. | 67, 349–355 |
| D3DOP_BRANCHFORWARD 12 | D3DBRANCH {dwMask, dwValue, bNegate, dwOffset} (16) | Only the **first** D3DBRANCH is evaluated (wCount only sets the skip size). Condition c = ((dwStatus & dwMask) == dwValue); branch if c ≠ bNegate (i.e. bNegate = FALSE: branch when equal). Branch: **dwOffset = 0 → behave as EXIT**; else continue at **the BRANCHFORWARD instruction header + dwOffset**. No branch: next instruction. | 118–131 |
| D3DOP_SPAN 13 | D3DSPAN, wCount × bSize bytes | **Skipped (not drawn) in clipped execution**; drawn only on the driver path below. | 251–255 |
| D3DOP_SETSTATUS 14 | D3DSTATUS {dwFlags, dwStatus, drExtent} (24) | First record only: D3DSETSTATUS_STATUS (1) → dwStatus = value; D3DSETSTATUS_EXTENTS (2) → extents = drExtent (as floats). | 132–145 |
| 0, ≥ 15 | — | **Stop silently with D3D_OK**; the status/extent are *not* written (Execute then copies a zero dsStatus). | 147–152, 252 |

After every POINT/LINE/TRIANGLE/STATERENDER/SPAN instruction the extents are clamped to the
viewport when the guard band is in use (ClampExtents_6dead2a0.c:7–19).

**D3DRENDERSTATE_ZVISIBLE ≠ 0**: points/lines/triangles are not drawn and D3DSTATUS_ZNOTVISIBLE
is **cleared** (= "visible") (205–215, 225–231, 241–243). There is no real visibility test.

**Driver path** (LAB_6deaddec, 147–190): when the execute is UNCLIPPED, or a
D3DPROCESSVERTICES_COPY ran earlier in this Execute (device 0xf1c bit 2, set by the COPY op), the
first POINT/LINE/TRIANGLE/STATERENDER/SPAN instruction and *everything after it* are handed to the
driver's DrawPrimitives2 as one command stream; the driver draws without runtime clipping (no
ZVISIBLE handling, no software clip) and either consumes everything (the runtime then only walks
to EXIT recording render-state values and evaluating BRANCHFORWARD, 288–348) or returns
D3DERR_COMMAND_UNPARSED with an offset where the runtime resumes (matrix ops, PROCESSVERTICES …)
(180–188). The net effect is in-order execution, unclipped. D3DOP_SPAN is drawn only this way.

Status: dwStatus (device+0x12e0) and the extents **persist across Executes** and are shared with
IDirect3DDevice2/3 Get/SetClipStatus (SetClipStatus_6deb265b.c:28–29); Execute does not reset
them — games use D3DOP_SETSTATUS. Initial dwStatus 0x01FFF000 (ZNOTVISIBLE + all
CLIPINTERSECTION bits). Clipped PROCESSVERTICES: `dwStatus = (dwStatus | union) & (intersection << 12 | 0xFF000FFF)`
(D3DFE_UpdateClipStatus_6dea9835.c); COPY and unclipped processing clear the intersection bits
(`& 0xFF000FFF`) (D3DHELInst_D3DOP_TRANSFORM_6deada50.c:53, 70).

### 6.3 Matrix handles: MATRIXLOAD, MATRIXMULTIPLY, STATETRANSFORM

A D3DMATRIXHANDLE (IDirect3DDevice::CreateMatrix, device scope) is used as a pointer to the
matrix data (STATETRANSFORM passes the handle as the matrix to D3DFE_SetMatrix*).

* MATRIXLOAD (D3DHELInst_D3DOP_MATRIXLOAD_6dead3c1.c): per record: either handle 0 →
  **DDERR_INVALIDPARAMS** (stops the Execute) (23–27); copy 17 DWORDs src → dest (28–34); **if dest
  is the handle currently bound as world/view/projection, that transform is updated** (35–43);
  failure → E_FAIL (44–46).
* MATRIXMULTIPLY (D3DHELInst_D3DOP_MATRIXMULTIPLY_6dead452.c): **dest = src1 · src2** (row-vector
  convention, MatrixProduct(dst, a, b)); a source handle 0 → **E_FAIL** (dest 0 not checked)
  (18–23); re-apply if dest is bound (24–32).
* STATETRANSFORM (D3DHELInst_D3DOP_STATETRANSFORM_6dead4d5.c): state ≥ 0x101 sets/clears the
  override bit (45–54); overridden states are skipped (22); handle 0 → **DDERR_INVALIDPARAMS on
  Device2/3, but on a version-1 device the rest of this instruction is skipped and D3D_OK
  continues** (25–27); WORLD 1 / VIEW 2 / PROJECTION 3 → set that transform from the handle and
  **remember the handle** (28–42); any other state → E_FAIL (37–39).

The binding is live: a later MATRIXLOAD/MATRIXMULTIPLY into a bound handle changes the device
transform without another STATETRANSFORM.

### 6.4 PROCESSVERTICES — D3DHELInst_D3DOP_TRANSFORM_6deada50.c

Per D3DPROCESSVERTICES {dwFlags, wStart, wDest, dwCount, dwReserved} (stride 16; all records of
the instruction are processed):

* source = buffer + wStart·32; destination = TL buffer + wDest·32; clip codes at H-buffer + wDest·2
  (27–31, 63). **No range checks** (wStart/wDest/dwCount vs dwVertexCount or the buffer size).
* `dwFlags & 7` (D3DPROCESSVERTICES_OPMASK):
  * **2 = COPY**: the vertices are D3DTLVERTEX, copied verbatim (dwCount·32 bytes); extents grown
    from them; "TL copied" flag set → every later drawing instruction of this Execute takes the
    unclipped driver path (§6.2); clip-intersection bits cleared (33–54).
  * **0 = TRANSFORMLIGHT**: input FVF 0x112 (D3DVERTEX), and the "light" flag is set (57–59);
  * **1 = TRANSFORM** and **3–7**: input FVF 0x1E2 (D3DLVERTEX) (56–61). (Values 3–7 are not
    rejected; they act as TRANSFORM.)
  * Then transform (and light) with the current transforms and the viewport's Mclip; clipped
    execute: clip codes into the H buffer and clip status merged; unclipped: no codes (62–71).
* **D3DPROCESSVERTICES_UPDATEEXTENTS (8) and NOCOLOR (0x10) are ignored**: extents are always
  updated (the front-end "don't update extents" bit is never set by Execute: Execute_6dea444e.c:71).
* Lighting (DoUpdateState_6dea9770.c:12–30): only if a material is set (D3DLIGHTSTATE_MATERIAL ≠ 0);
  otherwise the light flag is cleared and a D3DVERTEX gets **diffuse 0xFFFFFFFF, specular 0**
  (ProcessVerticesLegacy_6ded2c0f.c:108–113). Lit vertices use the viewport's lights (§1.14)
  (127). Untransformed-but-unlit D3DLVERTEX keep their colour and specular (114–117). Vertex fog
  per D3DLIGHTSTATE_FOGMODE etc. (132–140).
* *Uncertain quirk*: the light flag set by a TRANSFORMLIGHT record is not cleared by a later
  TRANSFORM record in the same Execute (only Execute's reset clears it, Execute_6dea444e.c:71); a
  following D3DLVERTEX batch would then be "lit" using its reserved/colour DWORDs as the normal.
  Not verified by test.
* Output of an outside vertex (clipped execute) is **clip-space x', y', z', w'** in the sx, sy,
  sz, rhw slots, an inside vertex screen coordinates and rhw = 1/w (ProcessVerticesLegacy:80–101,
  155–168); the clipper consumes these. The TL buffer is internal (games never see it).
* Statistics counter only otherwise (73).

### 6.5 STATELIGHT — D3DHELInst_D3DOP_STATELIGHT_6decdfa9.c

Override bits for states ≥ 0x101 (25–34); overridden states skipped (36–37). D3DLIGHTSTATE_:

| State | Action | Line |
|---|---|---|
| MATERIAL 1 | same handle as current → nothing; else SetMaterial(handle) (unvalidated; 0 = none) | 39–47 |
| AMBIENT 2 | RGBA DWORD; r, g, b kept as 0…255 floats | 48–55 |
| COLORMODEL 3 | stored only | 56–58 |
| FOGMODE 4 | stored, fog flags recomputed | 59–62 |
| FOGSTART 5 / FOGEND 6 / FOGDENSITY 7 | stored (floats as DWORDs) | 63–76 |
| COLORVERTEX 8 | on/off (DX6 addition; accepted in execute buffers too) | 77–84 |
| other | **DDERR_INVALIDPARAMS** (stops the Execute) | 85–86 |

### 6.6 TEXTURELOAD — D3DHELInst_D3DOP_TEXTURELOAD_6deaf89b.c

For each D3DTEXTURELOAD: look both handles up among the device's texture blocks (unknown handle
→ D3DERR_TEXTURE_CREATE_FAILED, sic: D3DHAL_TextureGetSurf_6deaf809.c:44) (44–48); destination
GetSurfaceDesc failure → D3DERR_TEXTURE_LOAD_FAILED (54–57); 8-bit or 4-bit palettised destination: copy the source palette's 256/16
entries into the destination's palette; **Blt the whole source onto the destination level by
level along the mipmap chain** (DDBLT_WAIT), with a locked row copy fallback when Blt is
unsupported for 4/2/1-bit formats (palette 69–78, Blt 95, fallback 96–150); failures →
D3DERR_TEXTURE_LOAD_FAILED (173–187).

### 6.7 Vertex types

All execute-buffer vertices are 32 bytes: D3DVERTEX (x, y, z, nx, ny, nz, tu, tv — FVF 0x112),
D3DLVERTEX (x, y, z, dwReserved, color, specular, tu, tv — 0x1E2), D3DTLVERTEX (sx, sy, sz, rhw,
color, specular, tu, tv — 0x1C4). Which one a batch is depends only on the PROCESSVERTICES op.
D3DLVERTEX's dwReserved is ignored.

---------------------------------------------------------------------------------------------

## 7. IDirect3DVertexBuffer (DX6, CDirect3DVertexBuffer in d3dim.dll)

Object 0x58 bytes: +0x04 refcount (1), +0x08… Direct3D link (no AddRef), +0x14 dwCaps, +0x18
dwNumVertices, +0x1c lock count, +0x20 0x4000 = video memory, +0x2c legacy type (1 = 0x112,
2 = 0x1E2, 3 = 0x1C4, 0 = other), +0x34 texcoord count, +0x38 memory pointer, +0x3c stride, +0x40
dwFVF, +0x44 clip codes, +0x48/+0x4c surfaces, +0x50 1 = optimized into the PSGP layout, +0x54
device that last used it (CDirect3DVertexBuffer_6deaa474.c, Init_6dea9a03.c).

### 7.1 IDirect3D3::CreateVertexBuffer(lpDesc, lplpVB, dwFlags, pUnkOuter)

CreateVertexBuffer_6deaa511.c → CreateVertexBufferI_6deaa4b6.c → Init_6dea9a03.c:

| # | Check / action | Line |
|---|---|---|
| 1 | pUnkOuter → CLASS_E_NOAGGREGATION. **No This, lpDesc, lplpVB or dwSize check** (faults are not caught: no SEH). | CreateVertexBuffer:13–20 |
| 2 | `*lplpVB = NULL`; E_OUTOFMEMORY. | CreateVertexBufferI:16–23 |
| 3 | dwNumVertices ≥ 0x10000 → D3DERR_TOOMANYVERTICES. | Init:22, 92–93 |
| 4 | dwCaps & D3DVBCAPS_OPTIMIZED → DDERR_INVALIDPARAMS. | Init:23, 88–89 |
| 5 | **FVF & 0xFFFFF000 ≠ 0 or vertex size 0 → D3DERR_INVALIDVERTEXFORMAT** (stricter than DX7, which ignores high bits). | Init:24–29, 86 |
| 6 | Memory (CreateMemoryBuffer_6dea9906.c): XYZRHW and no D3DVBCAPS_SYSTEMMEMORY → try DDSCAPS_EXECUTEBUFFER\|VIDEOMEMORY (+WRITEONLY if asked and dwFlags has D3DDP_DONOTCLIP), on failure system memory (29–43); **every untransformed FVF: system memory** (44–47). A system-memory buffer is locked once, the pointer kept for the VB's life (53–60). | Init:30–36 |
| 7 | XYZRHW without D3DDP_DONOTCLIP in **dwFlags** (DX6 uses the CreateVertexBuffer flag, not D3DVBCAPS_DONOTCLIP): a zeroed WORD clip-code array. | Init:40–60 |

### 7.2 Methods

| Method | Behaviour | Ref |
|---|---|---|
| QueryInterface | Lock; IUnknown or **IID_IDirect3DVertexBuffer** → AddRef, S_OK; else E_NOINTERFACE **without writing *ppv**. No NULL checks. | QueryInterface_6dea9c67.c:15–48 |
| AddRef / Release | one count, no NULL check; Release to 0 destroys: unlink, free clip codes, release surfaces. | AddRef_6dea9cb7.c, Release_6deaa448.c, ~CDirect3DVertexBuffer_6dea988b.c |
| Lock(dwFlags, lplpData, lpdwSize) | Lock; optimized → **D3DERR_VERTEXBUFFEROPTIMIZED**; `*lpdwSize = stride·count` if given; pointer NULL (video memory, unmapped) → lock the surface with `dwFlags \| DDLOCK_NOSYSLOCK` (its error returned); count++; if a device uses the VB and DDLOCK_READONLY (0x10) is absent → that device flushes (**DDLOCK_NOOVERWRITE / DISCARDCONTENTS not looked at**, unlike DX7). No flag validation, no lplpData check. | Lock_6dea9cee.c:10–18; LockI_6dea9b6b.c:13–42 |
| Unlock | **No lock, no checks**: if count > 0, count−−; a video-memory VB reaching 0 is unmapped *immediately* (surface Unlock, its result returned). Never fails otherwise. | Unlock_6dea9bf4.c:11–21 |
| GetVertexBufferDesc | writes dwCaps, dwFVF, dwNumVertices; **no NULL or dwSize check**, dwSize not written. | GetVertexBufferDesc_6dea9c25.c:10–13 |
| ProcessVertices | §7.3 | |
| Optimize(lpDevice, dwFlags) | This NULL → DDERR_INVALIDPARAMS; lpDevice NULL → DDERR_INVALIDOBJECT; dwFlags ≠ 0 → DDERR_INVALIDPARAMS; already optimized → D3DERR_VERTEXBUFFEROPTIMIZED; locked → **D3DERR_VERTEXBUFFERLOCKED**; XYZRHW → just mark OPTIMIZED; else the CPU front end may convert to its own layout (new buffer; E_NOTIMPL means "keep as is"); mark D3DVBCAPS_OPTIMIZED; D3D_OK. **No ValidateFVF** (DX7 has one). A fault inside returns D3D_OK (`@6deaa437–6deaa445`). | Optimize_6deaa2a4.c:22–94 |

### 7.3 ProcessVertices(dwVertexOp, dwDestIndex, dwCount, lpSrcBuffer, dwSrcIndex, lpD3DDevice, dwFlags) — ProcessVertices_6dea9d24.c

`this` is the destination.

| # | Step | Line |
|---|---|---|
| 1 | Device = lpD3DDevice (IDirect3DDevice3) — **not checked** (NULL faults); MT lock. | 18–21 |
| 2 | Front-end flags = (source's "optimized SOA" bit) \| (dwVertexOp XOR 0xC) \| 1: **D3DVOP_CLIP (4) and D3DVOP_EXTENTS (8) select clipping/extents, D3DVOP_LIGHT (0x400) lighting; D3DVOP_TRANSFORM is implied** (always transformed). Independent of the render states. | 23 |
| 3 | The device's current viewport is downloaded if needed; error returned (no current viewport → fault). | 24–31 |
| 4 | Lock destination (DDLOCK_WAIT) and source (WAIT\|READONLY) through LockI: **no OPTIMIZED check on either** (LockI is the unchecked inner function, unlike DX7 where an optimized destination fails); a source failure leaves the destination locked. | 33–40 |
| 5 | Output at dest + stride·dwDestIndex in the **destination's FVF**; clip codes at the destination's clip array + dwDestIndex (no array → bogus pointer if D3DVOP_CLIP: *likely fault*); input from source + stride·dwSrcIndex, or the optimized SOA layout. Texcoord count = min(src, dest) unless both are legacy types. **No check that the destination is XYZRHW, no output-format check, no range check, dwFlags (D3DPV_DONOTCOPYDATA) ignored.** | 41–68 |
| 6 | Process; merge clip status if clipping; unlock both; D3D_OK (the front end's result is ignored). | 70–80 |

Lighting uses the device's current viewport's lights and current material (§1.14). Outside
vertices (with D3DVOP_CLIP) are written in clip space, inside ones in screen space (as in DX7).

### 7.4 Differences from IDirect3DVertexBuffer7 (`d3d7-draw-vb.md` §8)

| | DX6 (this file) | DX7 |
|---|---|---|
| Interfaces | IDirect3DVertexBuffer only; QI for IDirect3DVertexBuffer7 fails | IDirect3DVertexBuffer7 only |
| ProcessVerticesStrided | absent | present |
| Creation | FVF high bits → INVALIDVERTEXFORMAT; clip codes controlled by CreateVertexBuffer's dwFlags (D3DDP_DONOTCLIP); untransformed VBs always in system memory | high bits ignored; D3DVBCAPS_DONOTCLIP; driver may host untransformed VBs |
| Lock | flush unless READONLY; NOOVERWRITE/DISCARD ignored | NOOVERWRITE / DISCARDCONTENTS honoured |
| Unlock | video-memory VB unmapped at count 0 immediately | unmapped lazily at the next flush |
| ProcessVertices | no OPTIMIZED check on the destination, no output-format check, DONOTCOPYDATA ignored, no NULL-source mode, device's current viewport/lights used | dest OPTIMIZED → error; INVALIDVERTEXFORMAT rules; DONOTCOPYDATA; NULL source = clip-only |
| Optimize | no ValidateFVF; XYZRHW only marked | ValidateFVF; T&L HAL copies |
| Lighting model | D3DLIGHT/D3DLIGHT2 (§2.2), viewport lights | D3DLIGHT7 |

---------------------------------------------------------------------------------------------

## For dd2d9: what to implement

1. **Viewport object** with the layout of §1: one COM object answering IDirect3DViewport/2/3,
   refcount, D3DVIEWPORT2 storage + "set" flag, background handle/flag, background depth pointer,
   light list (insertion order), id. Initialize → DDERR_ALREADYINITIALIZED; LightElements →
   E_NOTIMPL.
2. SetViewport2/SetViewport/GetViewport2/GetViewport with exactly the checks and conversions of
   §1.3–1.5, including: no validation on version-1 devices; render-target bound check on 2/3;
   SetViewport ignoring dvMaxX/dvMaxY/dvMinZ/dvMaxZ and routing through SetViewport2; GetViewport
   writing the structure even on VIEWPORTDATANOTSET.
3. Download semantics (§1.6): the device records which viewport id was last applied; Execute,
   Clear/Clear2, TransformVertices, SetCurrentViewport and DX6 ProcessVertices/DrawPrimitive
   (with the current viewport) apply it when needed; SetViewport2 on the applied viewport
   re-applies at once. Degenerate data on a version-1 device leaves the previous mapping.
4. AddViewport/DeleteViewport/SetCurrentViewport/GetCurrentViewport/NextViewport reference rules
   (§1.2); the viewport destructor detaches from its device and releases its lights; the device
   destructor deletes its viewports.
5. TransformVertices (§1.7) on the CPU: Mclip, clip codes in the [0,w] volume, lpHOut always
   written, inside/outside output rules, return = clip intersection (0xFFFFFFFF for 0
   vertices), drExtent as an in/out union widened by extents-adjust (1.0) and ±1 after rounding,
   version-1 clamp in UNCLIPPED mode.
6. SetBackground (0 rejected), GetBackground, SetBackgroundDepth (no AddRef, releases the old
   one), SetBackgroundDepth2 (QI = AddRef), GetBackgroundDepth (no AddRef), GetBackgroundDepth2
   (QI) — reproduce the reference counts; never release the depth surface in the destructor (or
   do: a leak is invisible, a double release is not — releasing it once on destruction only when
   set via SetBackgroundDepth2 is the safe choice).
7. Clear (§1.11) and Clear2 (§1.12) with the check order given, ProcessRects (§1.13: clip to the
   viewport, to the whole render target on version-1 devices, drop empty rects, **0 rects = clear
   nothing**), background colour = truncated diffuse·255 (no clamp, byte spill), Z = 1.0,
   stencil 0; textured background fill when the background material has hTexture; Clear2's
   ZBUFFER_NOTPRESENT / STENCILBUFFER_NOTPRESENT and "ZBUFFER with a background depth surface →
   INVALIDPARAMS".
8. AddLight/DeleteLight/NextLight with their error codes (§1.14, NextLight's odd order and S_OK
   with NULL at the end).
9. **Light object** (§2): SetLight validation only for D3DLIGHT2, GetLight copying the stored
   size, version tag; lights active = D3DLIGHT always / D3DLIGHT2 by D3DLIGHT_ACTIVE; a light
   never set is ignored; NO_SPECULAR.
10. **Lighting** for DX5/6 vertices with both models of §2.2 (D3DLIGHT: range ignored,
    1/max(1, a0+a1d+a2d²) with the >1000 cutoff, linear spot; D3DLIGHT2: range cutoff, multiplier
    a0 + a1(R−d)/R + a2((R−d)/R)², spot power), parallel point, no lighting without a material.
11. **Material object** (§3): three interface pointers, SetMaterial (dwSize 0x50, no-op when
    unchanged, propagates to all device copies and re-applies the current one), GetMaterial,
    GetHandle (one stable non-zero handle per device; DDERR_INVALIDOBJECT for a NULL device),
    Reserve/Unreserve → E_NOTIMPL. Handle destruction clears the device's current material.
12. **Texture handles** (§4): IDirect3DTexture/2 as an aggregate of the surface (QI/AddRef/Release
    forwarded to the surface); GetHandle errors (all-NULL → INVALIDOBJECT, managed →
    INVALIDOBJECT, v2 size check → TEXTURE_BADSIZE); one handle per surface (same on all devices
    is fine); a per-device handle → Surface map used by D3DRENDERSTATE_TEXTUREHANDLE and
    TEXTURELOAD; on texture destruction: materials' hTexture → 0, TEXTUREHANDLE → 0 if it was
    current. Load = the DX7 CopySurface; PaletteChanged/Unload → D3D_OK.
13. **Execute buffers** (§5): system-memory storage of dwBufferSize, Lock/Unlock (EXECUTE_LOCKED /
    NOT_LOCKED, descriptor rewritten with D3DDEB_LPDATA), Set/GetExecuteData (no validation),
    Validate/Optimize → E_NOTIMPL, CreateExecuteBuffer checks, device destruction force-releases.
14. **Execute** (§6.1–6.7): flag check (1/2 only), viewport selection (temporary current viewport,
    not restored on late errors), the interpreter with fixed record sizes, source vertices at
    `base + wStart·32` (dwVertexOffset ignored), a TL array of dwVertexCount (at least)
    entries, clip codes, all opcodes incl. override bits, live matrix-handle bindings,
    BRANCHFORWARD relative to the instruction header with dwOffset 0 = exit, SETSTATUS, unknown
    opcode = silent stop with zero status, ZVISIBLE behaviour, persistent dwStatus/extents shared
    with Get/SetClipStatus (initial 0x01FFF000), the dsStatus write-back, "COPY ⇒ rest unclipped".
15. **DX6 vertex buffer** (§7): either a separate class or a mode of VertexBuffer7 with the DX6
    differences of §7.4 (FVF high-bit check, D3DDP_DONOTCLIP flag, QI IID, Lock flush rule,
    Unlock, ProcessVertices without dest/format checks and with the DX5/6 lighting/viewport,
    Optimize without ValidateFVF).

## Implementation notes for dd2d9 (on D3D9)

dd2d9 implements Direct3D 7 in `src/d3d7.cpp` over `GpuContext` (`src/gpures.h`: SetTransform,
SetViewport, SetMaterial/SetLight/LightEnable, Clear, Draw with an FVF and optional 16-bit
indices) and has a CPU ProcessVertices (d3d7.cpp "ProcessVertices (V §8.5)": Layout, transform,
light, clip). The DX3–6 objects map onto that as follows.

**Viewport → D3D9 viewport + projection.** Set the D3D9 viewport to (X, Y, W, H) with MinZ 0,
MaxZ 1, and give D3D9 the projection `P · M` where M maps the D3DVIEWPORT2 clip window to D3D9's
NDC (row vectors, D3D convention):

```
      | 2/cw            0              0                 0 |
M  =  | 0               2/ch           0                 0 |
      | 0               0              1/(zmax-zmin)     0 |
      | -1 - 2*cx/cw    1 - 2*cy/ch    -zmin/(zmax-zmin) 1 |
```

(x_ndc = 2(x/w − cx)/cw − 1, y_ndc = 2(y/w − cy)/ch + 1, so y/w = cy lands on the top edge;
z_ndc = (z/w − zmin)/(zmax − zmin).) This reproduces §1.6 exactly, including D3DVIEWPORT's
scale semantics after the §1.4 conversion; D3D9's clip volume then equals XP's [0,w] volume. Keep
the game's projection separately (GetTransform-style queries and matrix handles see the
unmodified matrix). Recompute M whenever the applied viewport changes. The 1e-6 screen offset is
not worth emulating.

**Execute buffers → CPU front end + Draw.** Recommended structure:
* Keep the execute buffer as a plain heap block (the "surface" is never visible to the game
  except through lpData). Lock returns the same pointer every time.
* Interpret the stream on the CPU (§6.2 table). PROCESSVERTICES runs the existing CPU
  ProcessVertices code (FVF 0x112 lit / 0x1E2 / COPY 0x1C4) into a per-device TL array sized
  max(dwVertexCount, 4096) of D3DTLVERTEX plus a WORD clip-code array, using XP's legacy rules:
  inside vertices in screen space, outside in clip space, lighting per §2.2 (the D3D7 code's light
  model must gain the two DX5/6 attenuation/spot variants: D3D9's FFP cannot express either, so
  **the lighting has to be done on the CPU**), fog per the light states.
* POINT/LINE/TRIANGLE: build a 16-bit index list from the records (dropping D3DTRIANGLE.wFlags),
  and for primitives whose vertices all have clip code 0 call `GpuContext::Draw(D3DPT_*, 0x1C4,
  tl, 32, n, indices, …)` (pre-transformed). Primitives with non-zero codes need clipping in the
  [0,w] space before projection (XP does it in software: ProcessClippedIndexedTriangleList); either
  port a Sutherland–Hodgman clipper over the six planes (and interpolate colour/specular/uv), or
  avoid it by drawing such batches *untransformed*: emit D3DFVF_XYZ|DIFFUSE|SPECULAR|TEX1 vertices
  (CPU-lit colours baked in), set D3D9 LIGHTING = FALSE and the transforms world/view/(proj·M),
  and let D3D9 transform and clip (same result to rasterisation precision; vertex fog then needs
  care: with FOGVERTEXMODE = NONE D3D9 takes the fog factor from specular alpha — verify under
  DXVK). Trivially rejected triangles (all codes share a bit) can be dropped.
* COPY batches and D3DEXECUTE_UNCLIPPED: draw the TL vertices as they are (XP gives them to the
  driver unclipped; D3D9's viewport scissor gives the same picture). Note the d3d7-draw-vb.md
  pitfalls for XYZRHW (z outside [0,1] with ZENABLE off, rhw = 0).
* STATERENDER → the D3D7 SetRenderState path (legacy states such as TEXTUREHANDLE,
  TEXTUREMAPBLEND, TEXTUREMAG/MIN need the legacy mapping onto texture stages that the DX7 code
  already does or must add; device scope), honouring the override bits (a per-device 256-bit set
  each for render, transform and light states).
* STATETRANSFORM/MATRIXLOAD/MATRIXMULTIPLY: matrix handles as indices into a per-device table of
  D3DMATRIX; the device keeps the bound handle per WORLD/VIEW/PROJECTION and re-sends the
  transform when a bound handle is overwritten. Feed the projection through ·M.
* SPAN: draw only in the driver-path cases (UNCLIPPED or after COPY); otherwise skip. D3DSPAN
  draws horizontal runs of TL vertices (*format: not determined here*; rare).
* ZVISIBLE: skip drawing and clear ZNOTVISIBLE.
* Write dsStatus back only on EXIT/BRANCH-exit; on an unknown opcode write zeros.

**Material handles.** Use a per-device block holding a D3DMATERIAL copy; the handle value can be
the block address as on XP (non-zero, unique, stable) or an index ≥ 1. D3DLIGHTSTATE_MATERIAL →
copy into the device's current material (used by the CPU lighting; with the "untransformed draw"
option also into GpuContext::SetMaterial with LIGHTING off it is not needed). Background: Clear
reads diffuse from the handle's block.

**Lights.** CPU lighting reads the active lights of the viewport in use (Execute's viewport, or the
current one); rebuild the list when the viewport's dirty flag is set. Do not try to express
D3DLIGHT/D3DLIGHT2 as D3D9 lights for execute buffers. (IDirect3DDevice2/3 DrawPrimitive with
D3DVERTEX would need the same CPU path for exact results; with D3DLIGHT2 lights whose
attenuation is a0 only, a D3D9 light with att0 = 1/a0 and range R is close but not equal.)

**Texture handles.** A handle → Surface* map per device (or global: XP's DP2 handles are the
surface's kernel handle, identical on every device); TEXTUREHANDLE → `GpuContext::SetTexture(0,
surface)` plus the legacy blend mapping; handle 0 → no texture. Hook surface destruction to
invalidate handles, zero materials' hTexture and reset TEXTUREHANDLE.

**Clear.** `GpuContext::Clear` treats n = 0 as "whole viewport": **don't call it when
ProcessRects leaves 0 rects** (XP clears nothing). Pass the rects already intersected (with the
render target, not the viewport, for version-1 devices — GpuContext must then not clip to the
viewport again, or the device must temporarily widen the D3D9 viewport). Colour: compute ARGB with
truncation and without clamping, as XP. Textured background: draw one pre-transformed textured fan per rect exactly as
TriFillRectsTex (§1.11: texture stretched over the viewport, z 0, Z test/write off, blending,
fog, colour key, stencil off, the game's texture blend kept), restoring the states afterwards. Background depth surfaces: ignore for clearing (XP's DP2 path does).

**TransformVertices.** Pure CPU (no D3D9 involvement); reuse the PV transform with Mclip.

**DX6 vertex buffers.** Reuse VertexBuffer7's storage (system-memory pointer stable for the VB's
life; D3D9 dynamic VB upload on draw) with the §7.4 deltas. Its ProcessVertices must use the DX6
viewport mapping (Mclip) and DX5/6 lighting, so share the execute-buffer CPU front end.

**Pitfalls.**
* dwVertexOffset is ignored by XP; a game that sets it non-zero and also places its vertices at
  offset 0 works on XP — emulate XP, not the documentation.
* D3DLVERTEX.dwReserved is D3D9's PSIZE position if an FVF 0x1E2 ever reaches D3D9 (strip the
  RESERVED1 bit; see d3d7-draw-vb.md).
* Record sizes are fixed (bSize ignored except SPAN); some games put garbage in bSize.
* Execute must not require BeginScene (no check seen in Execute/ExecuteI).

## Not determined

* Whether TL-buffer sizes above 4096 vertices overflow (GrowTLandHbuffers clamp, §6.2 step 6) or
  CBufferVB keeps a larger earlier allocation; the driver's dwMaxVertexCount on XP DP2 HALs.
* The exact D3DSPAN format handling (driver side).
* Directional1C vs Directional2 differences (not read); the v1 light specular details; the
  behaviour of D3DLIGHT dltType 5/invalid when lit (probable fault).
* Whether the TRANSFORMLIGHT light flag really leaks into later TRANSFORM batches of the same
  Execute (§6.4) — needs a test.
* The device's viewport method at vtable +0x78 (what it sends to the driver) and SetClipStatus's
  exact fields beyond the extents (device scope).
* D3DOP_TEXTURELOAD's behaviour for mismatched mip chains / sizes (the loop that walks the chains
  was read only roughly).
* CreateMatrix/SetMatrix/GetMatrix/DeleteMatrix (device scope): the 17-DWORD copy in MATRIXLOAD
  suggests a 68-byte handle block (matrix + one DWORD); not read.
* IDirect3DDevice2/3 DrawPrimitive with lit vertex types and their use of viewport lights,
  SwapTextureHandles, Pick/GetPickRecords: out of scope.
