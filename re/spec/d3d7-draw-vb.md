# Direct3D 7: drawing (DrawPrimitive family) and vertex buffers

Reference: Windows XP SP3 `d3dim700.dll` 5.3.2600.5512 (retail build, stripped PDB: names but no
types). File references `File.c:line` are to the Ghidra output in
`$S/d3d-decomp/d3dim700-named/` (S = the session scratchpad; one file per function, named
`<method>_<address>.c`). Several classes share method names; the address in the file name picks
the class (see `$S/d3d-decomp/d3dim700-functions.txt`). Vtables were dumped from the binary
(`??_7…` symbols): CDirect3DDevice7 at 0x739186f8, CDirect3DDeviceTL at 0x739194f0,
CDirect3DDeviceIDP2 at 0x739185a0, CDirect3DVertexBuffer at 0x739198c4, D3DFE_PVFUNCS at
0x73919c3c.

Ghidra mislabels COM parameters: `param_1` is `This` and every later parameter is shifted one
place (the decompile's `param_2` is the first real argument, the last argument shows up as
`in_stack_000000xx`). Below, arguments are named by the real API.

**Big picture, read first.** The retail DX7 runtime validates almost nothing on the draw path.
There is no primitive-type check, no vertex-count check, no BeginScene check, no flags check, no
NULL check, no FVF validation (ValidateFVF is called only from VB::Optimize), no "VB is locked"
check. D3DDP_DONOTCLIP, D3DDP_DONOTUPDATEEXTENTS, D3DDP_DONOTLIGHT and D3DDP_WAIT have no effect.
Clipping, extents and lighting follow only the render states CLIPPING, EXTENTS and LIGHTING. The
debug runtime had the checks; games shipped against retail. dd2d9 must not be stricter than this
where a game could notice.

## 0. Common names

Device object offsets (DIRECT3DDEVICEI; the front-end block D3DFE_PROCESSVERTICES starts at
device+4, so PV-relative offsets used by the PV functions are 4 less):

| Offset | Meaning (inferred from use) |
|---|---|
| 0x1a40 | primitive type |
| 0x1a44 | vertex count |
| 0x1a48 | draw flags: the game's dwFlags ORed with internal D3DPV_* bits |
| 0x1a50 / 0x1a54 | index count / index pointer |
| 0x1a58 | primitive count (GetNumPrim) |
| 0x1a60 | input FVF of the last setup (0 = "re-setup needed") |
| 0x1a64 | device flags: 0x2 guard band in use, 0x4 RANGEFOGENABLE, 0x8 vertex fog, 0x10 driver takes FVF, 0x40 driver wants full TL vertices (D3DFVFCAPS_DONOTSTRIPELEMENTS), 0x80000 CLIPPING off, 0x100000 EXTENTS off, 0x800 SPECULARENABLE, 0x1000000 LOCALVIEWER, 0x2000000 NORMALIZENORMALS, 0x8000000 strided input, 0x10000000 COLORVERTEX, 0x80000000 LIGHTING on (UpdateInternalState_739204e2.c:36–148) |
| 0x1a68 / 0x1a70 / 0x1a74 | output vertex size / output FVF / output pointer |
| 0x1a7c / 0x1a80 | input position pointer / input stride |
| 0x1a84 | clip-code array (one WORD per vertex) |
| 0x1b20, 0x1b24, 0x1b28, 0x1b2c | extents minX, minY, maxX, maxY (GetClipStatus_739202de.c:16–19) |
| 0x27e0 | clip status dwStatus |
| 0x2270 | 0x1 in scene, 0x2 multithreaded, 0x8 fast paths allowed |
| 0x2284 | DDI level: 3 = CDirect3DDeviceIDP2 (DX6 DDI), 4 = CDirect3DDevice7 (DX7 DDI), 5 = CDirect3DDeviceTL (T&L HAL) (CDirect3DDeviceIDP2_7391ce98.c:10, CDirect3DDevice7_7391cee8.c:10, CDirect3DDeviceTL_73927143.c:10) |
| 0x2998 | render-state array (also at 0x1b1c, Init_7391b66b.c:169–170) |

Vertex buffer object (CDirect3DVertexBuffer, 0xa4 bytes):

| Offset | Meaning |
|---|---|
| +0x04 | private copy of the vtable (the interface pointer points here; slot 3 = Lock is patched) |
| +0x2c | draw-flag bits ORed into every draw from this VB (0x1000 "optimized SOA", 0x800000 "has clip codes", 0x20000000 "clip codes valid") |
| +0x30 | device that last drew from it (not AddRef'd) |
| +0x34 | lock count |
| +0x38 | pointer to the vertex memory (NULL while a video-memory VB is unlocked) |
| +0x3c / +0x40 / +0x44 / +0x48 | stride / dwNumVertices / dwFVF / dwCaps |
| +0x4c | 0x4000 = lives in video memory |
| +0x50 bit 31 | VB holds transformed (XYZRHW) vertices |
| +0x58 / +0x5c / +0x7c | texture-coordinate count / per-set sizes / total size |
| +0x80 | reference count |
| +0x84 | clip-code array (XYZRHW VBs without DONOTCLIP) |
| +0x88 / +0x8c | the DirectDraw execute-buffer surface holding the vertices (IDirectDrawSurface7 / IDirectDrawSurface) |
| +0x90 | 1 = optimized into a PSGP-specific layout |
| +0x94 | owning DIRECT3DI |

HRESULTs seen: 0x80070057 DDERR_INVALIDPARAMS, 0x8007000E E_OUTOFMEMORY, 0x80004005
E_FAIL/DDERR_GENERIC, 0x80004002 E_NOINTERFACE, 0x887602E1 D3DERR_INVALIDVERTEXTYPE (737),
0x88760800 D3DERR_INVALIDVERTEXFORMAT (2048), 0x8876080C D3DERR_VERTEXBUFFEROPTIMIZED (2060),
0x8876080E D3DERR_VERTEXBUFFERLOCKED (2062), 0x88760825 D3DERR_TOOMANYVERTICES (2085),
0x88760082 DDERR_INVALIDOBJECT, 0x887601AE DDERR_SURFACEBUSY, 0x887601C2 DDERR_SURFACELOST.

### 0.1 Locking

`CLockD3DMT` takes DirectDraw's lock only if the device is multithreaded, i.e. the DirectDraw
object was set up with DDSCL_MULTITHREADED (CLockD3DMT_7391f55e.c:8–14,
Init_7391b66b.c:55–57). Every Draw*, ProcessVertices* and Optimize uses it. VB Lock, AddRef,
Release and QueryInterface always take the lock (Lock_73929c67.c:23, AddRef_73929ab3.c:11,
Release_7392b11e.c:19).

### 0.2 Per-device vtable and fast paths

At device creation the vtable is copied into the device (0x57 entries at +0x31f4; the original
is kept at +0x31f0) and the object's vtable pointer is moved to the copy
(Init_7391b66b.c:257–265). The runtime then patches the DrawPrimitive, DrawIndexedPrimitive,
DrawPrimitiveVB and DrawIndexedPrimitiveVB slots (+0x3258, +0x325c, +0x3270, +0x3274) with
fast paths (`…TL` for transformed vertices, `…FE` for untransformed, `…VBTL` and `…VBFE`) when
the previous call left suitable state, and puts back the generic method whenever the state no
longer fits (e.g. DrawPrimitiveTL_73929000.c:16–17, 45–47; StartPrimVB_73925639.c:38–51;
BeginScene_7391bd87.c:70–75). Fast paths are only installed for single-threaded devices with
device flag 0x8 (Direct3DCreateDevice_7391cf86.c:181–184: DDI ≥ 3 and a DirectDraw-local bit
clear, probably "FPU setup"; *uncertain*). The fast paths have the same visible results as the
generic paths, so dd2d9 needs only the generic behaviour below. Note that a game can therefore
see a vtable pointer that is different per device and changes between calls (relevant only to
hooking tools).

---------------------------------------------------------------------------------------------

## 1. DrawPrimitive(type, fvf, lpvVertices, dwVertexCount, dwFlags)

DrawPrimitive_73928e32.c (vtable slot 0x64; same body for every device class).

| # | Step | Line |
|---|---|---|
| 1 | Lock (multithreaded only). | 24–26 |
| 2 | Store type, count, flags (**unvalidated**: unknown flag bits flow straight into the internal flags word), vertex pointer and stride. | 28–32 |
| 3 | If `fvf` differs from the cached FVF, or the previous call was strided: cache it, then vtable `SetupFVFData` (+0xf4), which computes the input stride and output format. A failure is returned **but the FVF stays cached**: an identical second call skips setup and goes ahead (quirk). | 33–43 |
| 4 | `GetNumPrim`: POINTLIST n, LINELIST n/2, LINESTRIP n−1 (0 if n<2), TRIANGLELIST n/3, STRIP/FAN n−2 (0 if n<3); **any other type leaves 0 and is not rejected.** | GetNumPrim_73928ae9.c:10–38 |
| 5 | vtable `ProcessPrimitive(draw = 1)` (+0xd4): §5 (HAL/RGB devices) or §6 (T&L HAL). Its result is returned. | 45–51 |

No error for: type 0 or > 6, count 0, NULL pointer (faults), outside BeginScene/EndScene (no
check of device+0x2270 bit 1 anywhere on the draw path; only BeginScene/EndScene/CopySurface read
it: grep), D3DDP_* flags.

SetupFVFData failures (only these):
* HAL/RGB device whose driver takes FVF (device flag 0x10): `SetupFVFDataCommon` returns
  DDERR_INVALIDPARAMS if the *output* FVF has texture-coordinate format bits (non-2D texcoords)
  and DDI < 4 (SetupFVFDataCommon_73925cde.c:133–135), or an error from EvalTextureTransforms
  (81–84).
* A driver without FVF support (DX6 DDI, output always D3DFVF_TLVERTEX 0x1c4, 32 bytes):
  DDERR_INVALIDPARAMS when texture transforms need a non-2D output it can't take
  (SetupFVFData_73920f46.c:30–46).
* T&L HAL: never fails (SetupFVFData_7392747e.c).

For dd2d9 (a DX7-level driver with FVF support) **SetupFVFData never fails**, so DrawPrimitive
returns D3D_OK unless the driver/flush fails or memory runs out (E_OUTOFMEMORY from growing the
internal buffers, ProcessPrimitive_739262bc.c:16–19, 66–76).

## 2. DrawIndexedPrimitive(type, fvf, lpvVertices, dwVertexCount, lpwIndices, dwIndexCount, dwFlags)

DrawIndexedPrimitive_73928f10.c. Same as §1 with index pointer/count stored (31–37),
`GetNumPrim(dwIndexCount)` (50) and `ProcessPrimitive(indexed = 0)` (51). Indices are
**not** checked against dwVertexCount; only dwVertexCount vertices are transformed/copied
(ProcessPrimitive_739262bc.c:62–66, DrawIndexedPrimitiveFE_739293bf.c:31), so an index ≥
dwVertexCount reads whatever is in the internal buffer.

## 3. DrawPrimitiveStrided / DrawIndexedPrimitiveStrided

DrawPrimitiveStrided_73928b60.c, DrawIndexedPrimitiveStrided_73928cc0.c.

| # | Step | Line |
|---|---|---|
| 1 | Lock. | 27–29 |
| 2 | **`(fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW` → D3DERR_INVALIDVERTEXTYPE (0x887602E1)** (disassembly 0x73928b91–0x73928ba8: `mov $0x887602e1,%eax`). This is the only argument check on any draw call. | 32–38 |
| 3 | Copy position ptr/stride, normal, diffuse, specular from the D3DDRAWPRIMITIVESTRIDEDDATA (offsets 0x00–0x1c). | 39–47 |
| 4 | Always re-setup (strided bit set, SetupFVFData), cached FVF = fvf. | 50–62 |
| 5 | Copy `texcount` texture coordinate pointer/stride pairs (offset 0x20 + 8·i), texcount from the FVF (max 15, no check against 8). | 63–66 |
| 6 | GetNumPrim, ProcessPrimitive. | 67–68 |

The strided data are gathered into the internal TL buffer (HAL: the transform loop reads each
stream; T&L HAL: copied into an interleaved buffer, ProcessPrimitive_7392725d.c:40–126, including
a zero DWORD for D3DFVF_RESERVED1, 85–88).

## 4. DrawPrimitiveVB / DrawIndexedPrimitiveVB

DrawPrimitiveVB_7392a641.c (args: type, lpVB, dwStartVertex, dwNumVertices, dwFlags);
DrawIndexedPrimitiveVB_7392a2f0.c (type, lpVB, dwStartVertex, dwNumVertices, lpwIndices,
dwIndexCount, dwFlags). Indices are relative to dwStartVertex.

| # | Step | Line (DPVB / DIPVB) |
|---|---|---|
| 1 | Lock. | 31–33 / 32–35 |
| 2 | flags = VB+0x2c \| dwFlags; store type, counts, indices; GetNumPrim. No check of lpVB (NULL faults), of the VB's lock count, of dwStartVertex+dwNumVertices ≤ VB size, or whether the VB is optimized. | 35–39 / 37–44 |
| 3a | **Transformed VB** (VB+0x50 bit 31, i.e. created with D3DFVF_XYZRHW) **or T&L HAL device**: output = the VB's own vertices (stride, FVF). Clip only if CLIPPING is on **and** (DDI < 5 **or** the VB has clip codes from ProcessVertices(D3DVOP_CLIP)) (46–52 / 51–57). | 40–138 / 45–143 |
| | — no clip: the driver draws straight from the VB (StartPrimVB, the device now references the VB; §8.6), no extents, no clip status. | 126–137, 115–116 / 131–142, 120–121 |
| | — clip: the VB's stored clip codes (written by ProcessVertices) are used, the OR-of-codes is forced to all-ones so the clipper always runs (73, 74 / 78, 79); if the VB's memory is not mapped (video-memory VB, §8.2) it is locked for the call with DDLOCK_WAIT\|READONLY\|NOSYSLOCK (0x811) and unlocked afterwards (78–106 / 83–111). | |
| 3b | **Untransformed VB, HAL/RGB device**: input = VB memory + start·stride (or the optimized SOA layout, 148–152 / 152–156); re-setup if the FVF changed; then ProcessPrimitive (§5) exactly as DrawPrimitive. | 139–164 / 144–168 |

Game-visible consequence of 3a: **transformed vertices in a vertex buffer are never clipped or
counted in the extents/clip status by the runtime unless they came from
ProcessVertices(D3DVOP_CLIP)**, unlike the same vertices passed through DrawPrimitive (§5.1).

## 5. ProcessPrimitive on HAL / RGB devices (DDI 3 and 4)

CDirect3DDeviceIDP2::ProcessPrimitive_739262bc.c (also used by CDirect3DDevice7).

### 5.1 Pre-transformed input (D3DFVF_XYZRHW)

| Condition | Action | Line |
|---|---|---|
| CLIPPING on | Compute clip codes for every vertex (D3DFE_GenClipFlags, §7.1), merge into the clip status (§7.3). If the AND of all codes ≠ 0 (all vertices beyond one plane) **nothing is drawn**, D3D_OK. Else draw through the clipper if any vertex needs it (§7.2). | 28–39 |
| CLIPPING off, EXTENTS on | Extents grow to every vertex's x,y (D3DFE_updateExtents_739288f1.c:10–25; no clamp); draw unclipped. | 42–48 |
| CLIPPING off, EXTENTS off | Draw unclipped (vertex data copied to the internal buffer if < 96 vertices, else drawn from the game's memory with a flush before and after, StartPrimUserMem_73926169.c:23–71, EndPrim_7392575a.c:10–15). | 50–58 |

The clip status is **not** updated when CLIPPING is off.

### 5.2 Untransformed input

The software front end (PV functions; one of four implementations: C reference
`D3DFE_PVFUNCS`, AMD 3DNow! `X3D_PVFUNCS`, SSE `sse_`, SSE2 `wmt_`, picked by CPU unless the
registry value DisablePSGP is set, Init_7391b66b.c:171–176) transforms, lights, fogs and computes
clip codes into the internal TL buffer, then draws (ProcessPrimitive_739262bc.c:61–107):

* `DoUpdateState` refreshes matrices, lights, fog flags (DoUpdateState_73991876.c). Lighting is
  done iff LIGHTING is on **and** the output FVF has diffuse or specular (34–36). Vertex fog is
  done iff FOGVERTEXMODE ≠ NONE and FOGENABLE (SetFogFlags_7391f798.c:10–17) **and** the output
  has specular (DoUpdateState_73991876.c:37–39).
* Every vertex is transformed (and clip-coded unless CLIPPING is off). If the AND of all clip
  codes ≠ 0, **nothing is drawn** (D3DFE_PVFUNCS ProcessPrimitive_73992737.c:45–51,
  ProcessIndexedPrimitive_739927ec.c:11–17).
* Clip status merged afterwards (ProcessPrimitive_739262bc.c:106).
* There is no maximum: the internal buffer grows to dwVertexCount·outputStride
  (TLVbuf_Grow_73925849.c:35–58, minimum 64 KB, rounded to 32 bytes). The DP2 command's
  primitive count and vertex start are WORDs (DrawPrim_73924d9d.c:35–42,
  DrawIndexPrim_7392495b.c:44–47): more than 65535 primitives in one call are truncated by the
  driver interface (*driver-level; not checked by test*).

Output vertex format sent to the driver (SetupFVFDataCommon_73925cde.c:51–132): XYZRHW, plus
DIFFUSE|SPECULAR if lighting is on or the driver asked for full vertices, else the input's
DIFFUSE/SPECULAR, with SPECULAR forced on when FOGENABLE is on and kept when SPECULARENABLE is
on and the input has it (60–67); the texture coordinates of the input (or those the texture
transforms/TEXCOORDINDEX generate, 77–122); a dummy texcoord set if the driver needs one (126–131).
Default colours when the input has none: diffuse 0xFFFFFFFF, specular 0
(ProcessVerticesGen_73992dc0.c:121–122). *This is driver-facing; what the game sees is the
rendered result.*

### 5.3 Primitive types and flags in the clipper

`DoDrawPrimitive`/`DoDrawIndexedPrimitive` (DoDrawPrimitive_73928981.c,
DoDrawIndexedPrimitive_73928a03.c): if no vertex needs clipping (§7.2) the batch goes to the
driver unchanged; otherwise per-type clippers (points, lines, line strips, lists, strips, fans).
An invalid primitive type that reaches the clipper returns E_FAIL (DoDrawPrimitive_73928981.c:28–29);
indexed point lists or invalid types return garbage (in_ECX, DoDrawIndexedPrimitive_73928a03.c:8,
31). An invalid type that does not need clipping is sent to the driver with an opcode read past
the end of a 7-entry table (DrawPrim_73924d9d.c:21, 28). **Undefined; dd2d9 should return
DDERR_INVALIDPARAMS for type ∉ 1..6 (the debug runtime's behaviour) rather than emulate this.**

## 6. ProcessPrimitive on the T&L HAL (DDI 5)

CDirect3DDeviceTL::ProcessPrimitive_7392725d.c: the vertices (or the gathered strided data) go
to the driver as they are, transformed or not. **No clipping, no lighting, no extents, no clip
status by the runtime for any draw on a T&L HAL device** (the driver does T&L and clipping).
GetClipStatus on such a device only reflects ProcessVertices calls and SetClipStatus.

## 7. Clipping, guard band, extents, clip status

### 7.1 Pre-transformed vertices (D3DFE_GenClipFlags_73995c89.c)

Viewport rectangle (UpdateViewportCache_73927838.c:26–70): X, Y, X+W, Y+H as floats. Guard band
(lines 71–98) from the driver's D3DHAL extended caps dvGuardBandLeft/Top/Right/Bottom; it is used
iff any of the four is non-zero and the registry value DisableGB is not set
(D3DFE_Create_7391eaa5.c:47–56); otherwise the "guard band" equals the viewport (99–102).

For each vertex (x, y, z, rhw), if rhw < 0 then x, y, z are negated first (60–69); then

| Code | Condition |
|---|---|
| 0x01 LEFT | x < X |
| 0x02 RIGHT | x ≥ X+W |
| 0x04 TOP | y < Y |
| 0x08 BOTTOM | y ≥ Y+H |
| 0x10 FRONT | z < 0, **only if ZENABLE ≠ 0** (41, 86–93; state 7 of the array at 0x1b1c) |
| 0x20 BACK | z ≥ 1, only if ZENABLE ≠ 0 |
| 0x1000/0x2000/0x4000/0x8000 | x < gbLeft / x ≥ gbRight / y < gbTop / y ≥ gbBottom (only computed if the vertex has some other code) (100–117) |

The viewport's MinZ/MaxZ are **not** used for TL vertices: their z is clipped to [0, 1].
Extents (if EXTENTS on) grow by vertices that are inside, or, with a guard band, only outside
the viewport but inside the guard band and z range (94–131). The codes are stored per vertex;
the function returns the AND of all codes (205–207).

### 7.2 When the clipper runs

CheckIfNeedClipping_739288bd.c: if any code ≠ 0; with a guard band, if any code other than the
four viewport bits ≠ 0, so triangles that leave the viewport but stay in the guard band are sent
unclipped (the driver scissors). After clipping, with a guard band and EXTENTS on, the extents
are clamped to the viewport (ClampExtents_7392882f.c:7–19).

### 7.3 Untransformed vertices

The front end folds the viewport into the projection, so clip space is 0 ≤ x ≤ w, 0 ≤ y ≤ w,
0 ≤ z ≤ w (TransformClip_739a8d23.c:102–105, sign-bit tests: LEFT x<0, RIGHT x>w, BOTTOM y<0,
TOP y>w, FRONT z<0, BACK z>w; ComputeClipCode_739959ce.c:10–22). User clip planes
(CLIPPLANEENABLE) give GEN0–5 = 0x40…0x800 when dot(plane, clip-space vertex) < 0
(TransformClip_739a8d23.c:106–119, ComputeClipCodeUserPlanes_739958c9.c). Guard band codes as in
7.1, tested in clip space (TransformClip_739a8d23.c:149–159). Visible vertices get screen
coordinates (lines 120–146):

    sx = x/w · W + X + 1e-6,  sy = −y/w · H + (Y+H) + 1e-6,
    sz = z/w · (MaxZ − MinZ) + MinZ,  rhw = 1/w

(UpdateViewportCache_73927838.c:46–57; the 1e-6 is the float at 0x739196ac). Extents grow by
these (127–140). Clipped-vertex screen coordinates are clamped to the guard band
(ComputeScreenCoordinates_73995738.c:26–67).

### 7.4 Clip status and GetClipStatus/SetClipStatus

D3DFE_UpdateClipStatus_7392464f.c:7–9:
`status = (status | (OR of codes & 0xfff)) & ((AND of codes << 12) | 0xff000fff)`, i.e. union
bits D3DSTATUS_CLIPUNION* accumulate, intersection bits D3DSTATUS_CLIPINTERSECTION* are ANDed;
guard-band bits are dropped. Initial status 0x01FFF000 (all intersection bits +
D3DSTATUS_ZNOTVISIBLE), initial extents minX = minY = 2048.0, maxX = maxY = 0
(D3DFE_Create_7391eaa5.c:117–126). Nothing resets them per frame: they accumulate until the game
calls SetClipStatus.

GetClipStatus (GetClipStatus_739202de.c:14–19): dwFlags = D3DCLIPSTATUS_EXTENTS2, dwStatus,
minx/maxx/miny/maxy; minz/maxz untouched; always D3D_OK; no NULL check.
SetClipStatus (SetClipStatus_73920258.c:15–34): D3DCLIPSTATUS_EXTENTS3 (4) → DDERR_INVALIDPARAMS;
STATUS (1) sets dwStatus; EXTENTS2 sets the four extents.

## 8. CDirect3DVertexBuffer (IDirect3DVertexBuffer7)

d3dim700 implements only IDirect3DVertexBuffer7 (vtable at 0x739198c4: QI, AddRef, Release,
Lock, Unlock, ProcessVertices, GetVertexBufferDesc, Optimize, ProcessVerticesStrided).
IDirect3DVertexBuffer (DX6) is d3dim.dll's (not covered here).

### 8.1 Creation (IDirect3D7::CreateVertexBuffer, for reference)

CreateVertexBuffer_7392b223.c → CreateVertexBufferI_7392b1bd.c → Init_73929b01.c:

| # | Check / action | Line |
|---|---|---|
| 1 | `*lplpVB = NULL`; allocation failure → E_OUTOFMEMORY. | CreateVertexBufferI:16–22 |
| 2 | dwNumVertices ≥ 0x10000 → **D3DERR_TOOMANYVERTICES**. | Init:33, 93–94 |
| 3 | dwCaps has D3DVBCAPS_OPTIMIZED → DDERR_INVALIDPARAMS. No other caps bit, no dwSize, no FVF validation. | Init:34, 89–90 |
| 4 | stride = vertex size of the FVF (position 12/16/16/20/24/28/32 for XYZ/RHW/B1–B5, position 0 counts 12; +12 normal, +4 RESERVED1 0x20, +4 diffuse, +4 specular; texcoords 8 each, or 4/8/12/16 per D3DFVF_TEXCOORDSIZEn) (GetVertexSizeFVF_7391f626.c, GetPositionSizeFVF_7391f59b.c, ComputeTextureCoordSize). stride 0 → D3DERR_INVALIDVERTEXFORMAT (unreachable: position size is never 0). | Init:35–43 |
| 5 | Memory: an execute-buffer DirectDraw surface. Video memory (DDSCAPS_EXECUTEBUFFER\|VIDEOMEMORY, + WRITEONLY if asked and the VB is DONOTCLIP or the driver supports VBs) is tried when SYSTEMMEMORY is not requested and (the driver supports video-memory VBs or the FVF is XYZRHW) and DirectDraw is not emulation-only; otherwise / on failure system memory. The surface is locked once (DDLOCK_WAIT\|NOSYSLOCK) and the pointer kept at +0x38. | CreateMemoryBuffer_739297ca.c:39–68 |
| 6 | XYZRHW without D3DVBCAPS_DONOTCLIP: a zeroed WORD clip-code array (dwNumVertices entries). | Init:51–70 |
| 7 | The VB is linked into the Direct3D object's VB list and a counter there is incremented; **no AddRef of the Direct3D object** (hookVertexBufferToD3D_73929713.c). Its surfaces belong to DirectDraw. | |

### 8.2 Lock(dwFlags, lplpData, lpdwSize) — Lock_73929c67.c, LockI_7392990b.c

| # | Step | Line |
|---|---|---|
| 1 | DirectDraw lock (always). | 23 |
| 2 | **Optimized → D3DERR_VERTEXBUFFEROPTIMIZED.** | 25–28 |
| 3 | If the memory pointer is NULL (video-memory VB after the runtime unmapped it): unlock the surface if it is still locked; flush the device that drew from it last; lock the surface with `dwFlags \| DDLOCK_NOSYSLOCK`; its error is returned. | 29–46 |
| 4 | Single-threaded DirectDraw: patch the VB's own vtable so later Lock calls go straight to LockI (no lock, no optimized check) (Optimize/UnlockI/BreakLock put Lock back). | 47–49 |
| 5 | LockI: lock count++. If neither DDLOCK_READONLY (0x10) nor DDLOCK_NOOVERWRITE (0x1000) and a device is using the VB: DDLOCK_DISCARDCONTENTS (0x2000) → the device starts a new batch buffer (FlushStatesReq); else the device flushes (FlushStates). | LockI:11–23 |
| 6 | `*lplpData = memory`; `*lpdwSize = stride·count` if lpdwSize ≠ NULL. **No flag validation, no NULL check on lplpData, D3D_OK.** | LockI:24–28 |

* Locks nest (count only). DDLOCK_WAIT/DONOTWAIT/NOSYSLOCK/WRITEONLY are not looked at (other
  than being passed to the surface Lock in step 3).
* **System-memory VBs return the same pointer every time, valid for the VB's life, also after
  Unlock** (it was obtained at creation, §8.1 step 5; only a video-memory VB is ever unmapped:
  UnlockI_73929d57.c:7–11). Games that keep writing through an old pointer work on XP.
* Locking does not stop drawing: Draw*VB and ProcessVertices never check the lock count; the
  only users of the count are Optimize (§8.6) and UnlockI.

### 8.3 Unlock — Unlock_7392996e.c

`if (count) count--; return D3D_OK` (9–12). No lock taken, never fails (no
D3DERR_VERTEXBUFFERUNLOCKFAILED, no "not locked" error). A video-memory VB is unmapped later,
when a device flush finds its count at 0 (FlushStates_7392526b.c:77–83 → UnlockI). DirectDraw can
also break the mapping on mode changes through the exported `D3DBreakVBLock`
(D3DBreakVBLock_7391dd8d.c → BreakLock_73929d93.c: pointer = NULL, Lock un-patched).

### 8.4 GetVertexBufferDesc — GetVertexBufferDesc_73929a1b.c

Writes dwCaps (as created, plus D3DVBCAPS_OPTIMIZED after Optimize; SYSTEMMEMORY is not added when
video memory failed), dwFVF, dwNumVertices (10–12). dwSize neither checked nor written; NULL
faults; D3D_OK.

### 8.5 ProcessVertices(dwVertexOp, dwDestIndex, dwCount, lpSrcBuffer, dwSrcIndex, lpD3DDevice, dwFlags)

ProcessVertices_73929fe9.c → DoProcessVertices_73929da9.c. `this` is the destination.

| # | Step | Line |
|---|---|---|
| 1 | Lock (device's MT lock); count stored in the device. lpD3DDevice is not checked (NULL faults). | 27–32 |
| 2 | Lock the destination through its vtable with DDLOCK_WAIT: optimized destination → **D3DERR_VERTEXBUFFEROPTIMIZED**. | 33, 103–105 |
| 3 | **lpSrcBuffer == NULL** (undocumented): no processing. If the destination is XYZRHW: D3DVOP_CLIP → clip codes for dest[dwDestIndex…+dwCount] (§7.1) into its clip-code array + clip status, mark "codes valid"; D3DVOP_EXTENTS → extents from those vertices. Non-RHW destination with D3DVOP_CLIP: clip status from those vertices (CheckClipStatus). Unlock, D3D_OK. | 35–70 |
| 4 | Lock the source via LockI with DDLOCK_WAIT\|READONLY (**an optimized source is accepted**); its failure is returned (and the destination stays locked: count leaks). | 72–73, 94–99 |
| 5 | Input FVF = source FVF, flags = (src+0x2c & 0x1000) \| internal "ProcessVertices" bit. | 75–79 |
| 6 | DoProcessVertices: output format computed from the source as for a draw (§5.2); then **D3DERR_INVALIDVERTEXFORMAT** unless: (no D3DVOP_LIGHT, or SPECULARENABLE and FOGENABLE both off, or the destination has SPECULAR) and dest texcoord count ≥ the output's and the destination's texcoord-size bits include the output's. The destination's FVF is otherwise not checked (in particular it is not required to be XYZRHW, and diffuse may be absent: then none is written). | DoProcessVertices:17–24, 92–95 |
| 7 | Output goes to dest + dwDestIndex·stride **in the destination's FVF and stride**. dwDestIndex+dwCount and dwSrcIndex+dwCount are not checked against either VB. | 25–28, 50–63 |
| 8 | Options, independent of the render states: D3DVOP_CLIP → clip codes, "has clip codes" set on the destination; else no clipping. **D3DVOP_LIGHT → lighting, else none, whatever D3DRENDERSTATE_LIGHTING says.** D3DVOP_EXTENTS → extents; else not. D3DVOP_TRANSFORM is not looked at (always transformed). Fog follows FOGENABLE/FOGVERTEXMODE (when the output has specular). | 29–49 |
| 9 | dwFlags & D3DPV_DONOTCOPYDATA (1): diffuse, specular and texture coordinates the pipeline doesn't compute are left untouched in the destination (diffuse/specular are written when lit, specular when fogged, texcoords when generated/transformed). Other dwFlags bits ignored. | 64–78 |
| 10 | Process; merge clip status if clipping. Then the device's CLIPPING/EXTENTS/LIGHTING flags are restored and its FVF cache invalidated (no lasting state change). Unlock destination, then source. | 79–91; PV:84–89 |

Vertices that are outside (with D3DVOP_CLIP) are written in **clip space, not screen space**:
x, y, z of the DX7 0…w clip volume and w in the rhw slot (TransformClip_739a8d23.c:163–172).
Inside ones get screen x, y, z and rhw = 1/w.

ProcessVerticesStrided(op, dwDestIndex, dwCount, lpStrided, dwVertexTypeDesc, lpD3DDevice,
dwFlags) (ProcessVerticesStrided_7392a1ce.c): lock destination (33; OPTIMIZED error as above),
copy the strided pointers, **with the texture-coordinate pair count taken from the destination's
texcoord count, not from dwVertexTypeDesc** (49–59), then DoProcessVertices as above (60–61). An
XYZRHW dwVertexTypeDesc is not rejected here (*result not determined*).

### 8.6 Optimize(lpD3DDevice, dwFlags) — Optimize_7392a986.c

| # | Check | Line |
|---|---|---|
| 1 | lpD3DDevice NULL → **DDERR_INVALIDOBJECT** (0x88760082). (The decompile also tests `this` first → DDERR_INVALIDPARAMS.) | 31–36 |
| 2 | dwFlags ≠ 0 → DDERR_INVALIDPARAMS. | 37–39 |
| 3 | **ValidateFVF(VB's FVF) fails → DDERR_INVALIDPARAMS** (rules in §8.8). | 45–54 |
| 4 | Already optimized → D3DERR_VERTEXBUFFEROPTIMIZED. | 55–61 |
| 5 | **Locked (count ≠ 0) → D3DERR_VERTEXBUFFERLOCKED.** | 62–68 |
| 6 | Non-T&L device or system-memory VB, untransformed FVF: the CPU front end may rewrite the data into its own layout (e.g. structure-of-arrays) in a new buffer (71–98); E_NOTIMPL from it is ignored. T&L HAL, video memory: the driver makes an optimized copy from the source VB handle (106–135). | 69–136 |
| 7 | Mark D3DVBCAPS_OPTIMIZED, put the checking Lock back, reset the device's VB fast paths. D3D_OK. | 137–150 |

After Optimize: Lock → D3DERR_VERTEXBUFFEROPTIMIZED; GetVertexBufferDesc shows OPTIMIZED; drawing
and use as a ProcessVertices **source** work; use as a ProcessVertices destination fails.

### 8.7 QueryInterface, AddRef, Release; references

* QueryInterface (QueryInterface_73929a43.c): `*ppv = NULL` first (25), IUnknown or
  IID_IDirect3DVertexBuffer7 → AddRef, S_OK; anything else (including IID_IDirect3DVertexBuffer)
  → E_NOINTERFACE.
* AddRef/Release: one count (+0x80), starting at 1 (CDirect3DVertexBuffer_7392b16b.c:9); Release
  to 0 destroys (Release_7392b11e.c:20–27): unlinks from the Direct3D list, frees the clip codes,
  releases its surfaces (~CDirect3DVertexBuffer_73929753.c).
* **A device holds a reference to the last VB it drew from directly** (StartPrimVB_73925639.c:26–31:
  Release of the previous one, AddRef of the new one). That happens for DrawPrimitiveVB /
  DrawIndexedPrimitiveVB of a transformed VB that isn't clipped, and for every VB draw on a T&L HAL
  (§4 step 3a). The reference is dropped when the device draws from another VB or its own buffer
  (StartPrimUserMem_73926169.c:54–64, TLVbuf_Grow_73925849.c:42–49) or is destroyed. So
  `vb->Release()` right after such a draw returns 1, not 0, and the VB lives on until the device
  moves on. Draws of untransformed VBs on HAL/RGB devices take no reference.
* The VB→device pointer (+0x30) is weak; the VB holds no reference on the Direct3D object.

### 8.8 ValidateFVF (only for Optimize) — ValidateFVF_7391f4dc.c

Valid iff all of: no texcoord-size bits beyond the declared texcoord count (table at 0x73918a44;
for counts 9–15 the "table" is unrelated data, effectively undefined) (12); not (texcoord-size bits
and XYZRHW and DDI ≤ 3) (13); no D3DFVF_RESERVED0 (0x1) or bits 0xF000 (13); position XYZ,
XYZB1–B5, or XYZRHW without NORMAL (15–24). Position 0 is invalid. Failure =
DDERR_INVALIDPARAMS.

## 9. Lighting and fog (front end), as far as results show

All per-vertex in camera space (normals through the inverse-transpose of world·view):

* Ambient + emissive = AMBIENT render state × material ambient + material emissive, precomputed in
  0–255 units and clamped (D3DFE_UpdateLights_73994488.c:92–124). Output alpha = material diffuse
  alpha (clamped, ×255) unless a colour-vertex source supplies it (75–85; ProcessVerticesGen
  lines 543–547).
* Directional/point/spot: diffuse term N·L when > 0 (_Directional7_739a7808.c:114–136).
  Specular uses the halfway vector: non-local viewer H = L + (0,0,1) precomputed per light;
  LOCALVIEWER H = normalize(eye−P) + L, divided by |H| through a table inverse square root
  (186–212). The power is **looked up**: 256-entry table of pow(i/255, Power) with linear
  interpolation, dot ≥ 1 → last entry, built per distinct material power when Power > 0.001
  (CreateSpecularTable_73994415.c, _Directional7:213–224, UpdateLights:22–56); dots below a
  per-material threshold give no specular (_Directional7:213). NORMALIZENORMALS uses a table
  normalize (_Directional7:107–110). Results match the D3D9 fixed-function formula to within
  these approximations (*the exact pow/threshold arguments are hidden behind `__CIpow` in the
  decompile: uncertain*).
* COLORVERTEX with DIFFUSE/SPECULAR/AMBIENT/EMISSIVEMATERIALSOURCE picks vertex colour 1/2 when
  the vertex has it (ProcessVerticesGen_73992dc0.c:129–167).
* Vertex fog (ComputeFog_73992a7c.c, ComputeFogFactor_739929dd.c): distance = eye-space z (or
  |eye-space position| with RANGEFOGENABLE) (24–114); LINEAR: d < start → 255, d ≥ end → 0, else
  round((end−d)·255/(end−start)) (with end = start the factor term is 0, UpdateFogFactor_7391f11a.c);
  EXP/EXP2 through exp() (arguments hidden; presumably the usual e^(−d·density),
  e^(−(d·density)²)). The factor goes into the specular alpha.
* Vertex blending: number of weights read = min(n of the FVF's XYZBn, D3DRENDERSTATE_VERTEXBLEND)
  (DoUpdateState_73991876.c:40–58, render state at array offset 0x25c = 151); one more matrix
  gets weight 1 − sum (TransformClip_739a8d23.c:62–100).

---------------------------------------------------------------------------------------------

## For dd2d9

1. **No draw-time validation beyond XP's.** DrawPrimitive/DrawIndexedPrimitive/*VB return D3D_OK
   for count 0, unknown flags, outside BeginScene/EndScene, a locked VB, start+count past the VB
   end (don't read out of bounds: clamp/skip and still return D3D_OK). Exceptions: Strided with
   XYZRHW → D3DERR_INVALIDVERTEXTYPE (§3). Invalid primitive type: undefined on XP → return
   DDERR_INVALIDPARAMS (§5.3). NULL pointers: return DDERR_INVALIDPARAMS instead of faulting.
2. Ignore D3DDP_WAIT, DONOTCLIP, DONOTUPDATEEXTENTS, DONOTLIGHT, but mask the game's dwFlags so
   stray high bits can't switch on internal behaviour (on XP they can; not worth emulating).
3. Clipping/extents/lighting follow D3DRENDERSTATE_CLIPPING/EXTENTS/LIGHTING only (§5, §7).
4. Clip status and extents: maintain them on the CPU as in §7 for HAL/RGB devices if a game reads
   them (GetClipStatus). They accumulate until SetClipStatus. Initial status 0x01FFF000, extents
   (2048, 2048, 0, 0). GetClipStatus writes D3DCLIPSTATUS_EXTENTS2 and leaves minz/maxz.
   SetClipStatus with EXTENTS3 → DDERR_INVALIDPARAMS. On a T&L HAL device draws don't touch them.
5. Trivial rejection (all vertices beyond one plane → nothing drawn, still D3D_OK) is invisible
   except through the clip status; D3D9 gets the same picture by clipping.
6. VB creation: dwNumVertices ≥ 65536 → D3DERR_TOOMANYVERTICES; OPTIMIZED in caps →
   DDERR_INVALIDPARAMS; no FVF validation.
7. VB Lock: OPTIMIZED → D3DERR_VERTEXBUFFEROPTIMIZED; otherwise always D3D_OK, nested, the same
   stable pointer for system-memory VBs (valid after Unlock), lpdwSize optional. Unlock always
   D3D_OK. Draws from locked VBs allowed.
8. ProcessVertices: §8.5 exactly; notable: D3DVOP_LIGHT overrides the LIGHTING state; destination
   format = destination FVF; D3DERR_INVALIDVERTEXFORMAT rule of step 6; D3DPV_DONOTCOPYDATA;
   NULL source = clip/extents of existing destination vertices; outside vertices left in clip
   space with D3DVOP_CLIP; no lasting state change.
9. Optimize: §8.6 error order (NULL device → DDERR_INVALIDOBJECT, flags → INVALIDPARAMS, FVF →
   INVALIDPARAMS, already → VERTEXBUFFEROPTIMIZED, locked → VERTEXBUFFERLOCKED); then just mark it
   (keep the data in the original layout: no game can see the layout once Lock is refused).
10. QueryInterface: only IUnknown and IID_IDirect3DVertexBuffer7. The device's reference on the
    last directly-drawn VB (§8.7) changes Release's return value; emulate it at least for
    transformed VBs (cheap: an AddRef'd "current VB" pointer swapped on each VB draw).

### Not determined

* Which class (IDP2 / Device7 / TL) XP uses for the RGB and reference rasterizers (decided in
  Direct3DCreateDevice; CreateDevice spec). It decides whether §5 or §6 applies.
* ProcessVertices with D3DVOP_CLIP into a D3DVBCAPS_DONOTCLIP destination (no clip-code array:
  probably writes through a NULL-based pointer).
* ProcessVertices/ProcessVerticesStrided with an XYZRHW source.
* Exact specular threshold and pow/exp arguments (§9).
* With a guard band, extents of draws that needed no clipping can extend past the viewport into
  the guard band (no ClampExtents on that path, TransformClip_739a8d23.c:149–158); whether
  anything clamps them later was not found.
* An SSE front-end fog path tests draw-flag bit 4 (= D3DDP_DONOTCLIP) where the C code tests
  RANGEFOGENABLE (FUN_7394c3e0_7394c3e0.c:75 vs ComputeFog_73992a7c.c:26); possibly a vendor
  quirk making D3DDP_DONOTCLIP select range fog on SSE CPUs. Not confirmed.
* Texcoord counts 9–15 in an FVF: nothing rejects them on the draw path; behaviour undefined.

---------------------------------------------------------------------------------------------

## Implementation notes for dd2d9 (on D3D9)

**Device setup.** ProcessVertices needs software vertex processing. A device created with
D3DCREATE_MIXED_VERTEXPROCESSING lets dd2d9 draw in hardware and switch
`SetSoftwareVertexProcessing(TRUE)` only around ProcessVertices, but a D3D9 ProcessVertices
source buffer then has to be D3DUSAGE_SOFTWAREPROCESSING. Simpler and closer to XP: **do
ProcessVertices on the CPU in dd2d9** (the transform/light/fog/clip code above is a few hundred
lines). D3D9's ProcessVertices doesn't match XP: lighting follows D3DRS_LIGHTING (there is no
D3DVOP_LIGHT), it has no extents or clip codes, clipped-vertex output differs, and DXVK
implements it with a transform-feedback shader that doesn't reproduce XP's clip-space output.
The same CPU code then also gives clip status/extents (§7) when a game asks for them.

**DrawPrimitive / DrawIndexedPrimitive.** `SetFVF(fvf)` + `DrawPrimitiveUP` /
`DrawIndexedPrimitiveUP(type, 0, dwVertexCount, primCount, indices, D3DFMT_INDEX16, verts,
stride)`, with primCount from GetNumPrim. Skip the D3D9 call when primCount is 0 (D3D9 rejects 0)
and return D3D_OK. D3D9 has a cap MaxPrimitiveCount/MaxVertexIndex (DXVK: large): split big
batches if needed. Better throughput: append to a D3DUSAGE_DYNAMIC|WRITEONLY vertex buffer with
D3DLOCK_NOOVERWRITE and DISCARD on wrap, the way XP's 64 KB TL buffer works (§5.2). For the
UP path an index ≥ dwVertexCount reads unrelated data, as on XP.

**FVF translation (pitfalls).**
* D3DFVF_RESERVED1 (0x20, D3DLVERTEX's reserved DWORD) **is D3DFVF_PSIZE in D3D9.** With it, a
  point list reads the reserved DWORD (usually 0) as the point size. Use a vertex declaration
  that skips those 4 bytes instead of the FVF (or clear the bit and keep the stride by declaring
  the same offsets).
* 0x1000 (D3DFVF_LASTBETA_UBYTE4 in D3D9) and 0x8000 (LASTBETA_D3DCOLOR) are reserved in DX7 and
  accepted unchecked by XP's draw path: strip them.
* Position 0 (no position) is 12 bytes on XP; D3D9 rejects it: treat as XYZ.
* XYZB4/XYZB5: D3D9 FFP blends at most 3 weights + last; map by declaration (BLENDWEIGHT with the
  right count) and clamp D3DRS_VERTEXBLEND.
* Texcoord count > 8: undefined on XP; clamp to 8.

**Strided.** Gather into an interleaved buffer in FVF order (position, weights, normal, [reserved],
diffuse, specular, texcoords), or bind one stream per element with a vertex declaration
(D3D9 allows up to 16 streams; each D3DDRAWPRIMITIVESTRIDEDDATA element becomes a stream with its
own stride; DrawPrimitiveUP can't do that, so a dynamic VB per stream is needed). Gathering on the
CPU is simpler and what XP does on the T&L HAL.

**Vertex buffers.** Keep the authoritative copy in system memory (the pointer Lock returns, stable
for the VB's life: §8.2) and a D3DPOOL_DEFAULT D3DUSAGE_DYNAMIC|WRITEONLY D3D9 VB; upload the
dirty range before a draw that uses it. Mark dirty on every Lock without DDLOCK_READONLY and keep
it dirty while the lock count is > 0 at draw time (games draw from locked VBs and write after
Unlock through the saved pointer, which works on XP for system-memory VBs). Map
DDLOCK_DISCARDCONTENTS → D3DLOCK_DISCARD and DDLOCK_NOOVERWRITE → D3DLOCK_NOOVERWRITE on the
upload. Don't use D3DPOOL_SYSTEMMEM VBs for drawing under DXVK: they are re-uploaded on every
draw. D3DVBCAPS_WRITEONLY/DONOTCLIP need no D3D9 counterpart (D3DUSAGE_DONOTCLIP exists but only
matters for D3D9 ProcessVertices).

**DrawPrimitiveVB / DrawIndexedPrimitiveVB.** `SetStreamSource(0, vb9, 0, stride)` + `SetFVF` +
`DrawPrimitive(type, dwStartVertex, primCount)` / indexed: indices are relative to dwStartVertex,
so copy them into a dynamic 16-bit index buffer and `DrawIndexedPrimitive(type,
BaseVertexIndex = dwStartVertex, 0, dwNumVertices, startIndex, primCount)` (or
DrawIndexedPrimitiveUP from the CPU copy, with vertices offset by dwStartVertex).

**Pre-transformed vertices (XYZRHW) and the viewport.**
* D3D9 passes XYZRHW straight to the rasterizer with the viewport as the clip rectangle and
  without applying MinZ/MaxZ to z. XP clips TL vertices to the viewport (or the guard band, then
  the driver scissors to the viewport), so x/y match.
* Z: XP clips TL z to [0, 1] **only when ZENABLE is on** (§7.1). D3D9 hardware and DXVK
  always clip (depth clip) z outside [0, 1]. A game that draws 2D overlays with ZENABLE off and
  z < 0 or > 1 would lose them on D3D9. Fix: when ZENABLE is off,
  clamp TL z into [0, 1] while copying (it can't affect the result). With ZENABLE on, leave z.
* rhw ≤ 0 or 0: XP flips x,y,z for clipping when rhw < 0 and otherwise passes rhw to the driver.
  DXVK rebuilds clip-space w = 1/rhw: rhw = 0 gives infinities and the primitive vanishes; on
  Windows drivers it usually draws. When every vertex of the call has rhw = 0 (2D games), replace
  it with 1 (all equal, so interpolation is unchanged); otherwise leave it.
* D3D9's half-pixel convention is the same as DX7 (pixel centres at integers): no 0.5 offset.
* XP adds 1e-6 to screen x and y of transformed vertices (§7.3): not worth emulating.

**Clipping differences.** XP clips untransformed geometry in software against the viewport (or
guard band) and user planes. D3D9/DXVK clip in hardware with the same results to rasterization
precision. CLIPPING = FALSE on XP means "don't clip"; on D3D9 hardware it only removes the
runtime's own clipping, so geometry outside the viewport is still cut at the viewport. Nobody
relies on XP drawing outside the viewport (the driver scissored), so this is fine.
TL VBs drawn without ProcessVertices(D3DVOP_CLIP) are unclipped on XP; D3D9 clips them to the
viewport: same picture.

**Lighting and fog.** D3D9 fixed-function lighting is the DX7 model (§9): copy lights, material,
AMBIENT, COLORVERTEX and the material sources, LOCALVIEWER, NORMALIZENORMALS, SPECULARENABLE
straight across. XP's specular power table and table square roots differ in the last bits only.
Vertex fog: D3D9 computes it when FOGVERTEXMODE ≠ NONE and FOGTABLEMODE = NONE, from eye z
(or range with RANGEFOGENABLE). XP's runtime computes vertex fog even when FOGTABLEMODE is set
(SetFogFlags tests only FOGVERTEXMODE and FOGENABLE) and the driver chooses; set D3D9's
FOGTABLEMODE as the game did and let D3D9 choose: pixel fog takes precedence there as it did in
DX7 HALs. For XYZRHW vertices fog comes from the specular alpha the game supplied (D3D9: same
when FOGVERTEXMODE is NONE and table fog off; DXVK honours it).

**Scenes.** XP doesn't require BeginScene for drawing. Keep D3D9 inside a scene of dd2d9's own
(BeginScene once, EndScene before Present) and treat the game's BeginScene/EndScene as
bookkeeping, as Wine does.

**Threads.** XP serialises only DDSCL_MULTITHREADED devices (§0.1); dd2d9's device lock around
every call is a superset and fine.

**DXVK pitfalls in summary:** FVF 0x20 = PSIZE; z outside [0,1] on TL vertices clipped; rhw = 0;
system-memory pool VBs slow; ProcessVertices emulation differs from XP; Get/SetClipStatus are,
as far as known, stubs in DXVK (not re-checked against current DXVK source), so dd2d9 must keep
its own (§7.4); DrawPrimitiveUP with primCount 0 is an error. The D3D9-side statements in this
section (XYZRHW z not remapped by MinZ/MaxZ, DXVK's rhw and ProcessVertices handling) come from
D3D9 documentation and general knowledge, not from this decompile: verify with tests.
