# IDirectDraw, IDirectDraw2, IDirectDraw4, IDirectDraw7: the object's methods

Reference: ddraw.dll 5.3.2600.5512 (XP SP3). Decompilation in
`$S/decomp-annotated/` (`$S` = the session scratchpad). Line numbers are those of the plain-named
file (`IDirectDraw7_X.c`, `FUN_xxxxxxxx_xxxxxxxx.c`). Return codes that the decompiler lost were
read from the disassembly; those cite an address.
dd2d9 references are to `src/` at the time of writing (ddraw.cpp, wrappers.h, objects.h).

## Conventions used in XP's code (they apply to every method below)

- **One GBL per display device, shared by every DirectDraw object in the process.** Each
  DirectDrawCreate makes a new LCL (with its own INTs, one per interface version). Refcounts: INT
  (`dwIntRefCnt`), LCL (`dwLocalRefCnt`, the sum of the object's INTs), GBL (`dwRefCnt`, all
  objects). AddRef/Release return the **INT** count.
- The exclusive owner is `gbl->lpExclusiveOwner` (+0x3f8), one LCL per device. `FUN_73734625(lcl,
  &any, &mine, ...)` reads it: `any` = some LCL owns it, `mine` = this LCL does. "Exclusive" below
  means that, not the LCL flag `DDRAWILCL_HASEXCLUSIVEMODE`, unless said otherwise.
- Every method first compares `GdiEntry13()` (the display-settings uniqueness) with the last value
  seen, and runs the mode-change handler `FUN_7373cc84` when it changed (e.g. QueryInterface.c:35-39).
  dd2d9 has the same idea in `DDraw::PollDisplayChange` (ddraw.cpp:853).
- **No pointer checks.** A NULL or bad out pointer faults inside the SEH frame, which returns
  DDERR_INVALIDPARAMS, at the point it is dereferenced. Explicit checks: `IsBadReadPtr(riid)` (QI),
  `IsBadCodePtr(callback)` (Enum methods), `IsBadWritePtr` (GetDisplayMode, GetDeviceIdentifier).
- Interface version comes from `This->lpVtbl`. "via DD1/DD2" means `vtbl_IDirectDraw` or
  `vtbl_IDirectDraw2`, and so on. IDirectDraw2 hands out IDirectDrawSurface (v1) interfaces.
- `FUN_7373e5c8(lcl, vtbl)` finds the surface's INT with that vtable or makes one;
  `FUN_7373e7ca(lcl, vtbl)` **always makes a new INT**. Both link it at the head of `gbl->dsList`
  (FUN_7373e7ca:17-19), so `dsList` holds every interface pointer of every surface, newest first.
- **Objects from CoCreateInstance start on "Uninit" vtables** (`vtbl_IDirectDraw_Uninit`,
  `vtbl_IDirectDrawX_UninitA/B/C`). There, every method except QueryInterface, AddRef, Release
  and Initialize returns `0x800401F0` (DDERR_NOTINITIALIZED) (IDirectDraw_Uninit_Compact.c,
  IDirectDraw_Uninit_CreatePalette.c, IDirectDrawX_UninitA_SetDisplayMode.c).

---

## QueryInterface — `IDirectDraw7_QueryInterface.c` (all versions)

1. `IsBadReadPtr(riid, 16)` → DDERR_INVALIDPARAMS (41-46). It comes **before** `*ppv = NULL` (47),
   so a NULL ppv gives DDERR_INVALIDPARAMS through SEH.
2. GUIDs (decoded from the DLL's data):
   - IID_IDirectDraw (0x73737c64) or IID_IUnknown (0x73733298) → the v1 INT (64-87);
   - IID_IDirectDraw2, 4, 7 → that version's INT (112-153).
   An existing INT of that version is reused, otherwise one is made (`FUN_73737e41`). It is AddRef'd → S_OK.
3. IID_IDDVideoPortContainer (6c142760-…, 0x73737e2c) → an interface is always handed out (165-169).
   IID_IDirectDrawKernel (8d56c120-…) → only with kernel-mode support (181-193).
   IID_IDDVideoAcceleratorContainer (aca12120-…) → only if `FUN_73737c79` allows it (225-235).
4. **Any other IID** (WOW64 aside) loads d3dim (`FUN_7373d0b5`) if needed. The QI is forwarded to its
   IUnknown, and that result is returned (204-221).

dd2d9 (`DDraw::QueryInterface`, ddraw.cpp:61): NULL out → INVALIDPARAMS, then IUnknown/1/2/4/7, then
`D3DProxyQueryDDraw` (Direct3D IIDs only, through D7VK), else E_NOINTERFACE. Forwarding to its own
Direct3D isn't possible here (deliberate: D7VK provides Direct3D). See difference 26.

## AddRef — `IDirectDraw7_AddRef.c` (all versions)

The method increments the GBL, LCL and INT counts and returns the INT count (24-34). dd2d9
(`DDraw::AddRef`, ddraw.cpp:82) keeps a count per version. **Matches.**

## Release — `IDirectDraw7_Release.c` (all versions)

1. INT count 0 → returns 0, nothing changes (66-70).
2. The INT, LCL and GBL counts are decremented (74-84). If the LCL count is still > 0, nothing else happens (87).
3. LCL count 0 (the object is gone):
   - the window is unhooked (88-91), and the Direct3D object is released (94-99);
   - **every surface, palette, clipper, video port and video accelerator this object created is released
     until it dies** (`FUN_737347f6`, `FUN_7373481e`, `FUN_73734890`, `FUN_737347c1`, `FUN_737348af`,
     `FUN_737349c1`, 102-107). Their interface pointers become dead;
   - display mode (108-118):
     - without V1SCLBEHAVIOUR: if nobody is exclusive, the mode is restored (`FUN_73734f22`).
       If this object is exclusive, it is deactivated (`FUN_7375aa94`: mode restored, owner cleared,
       cursor unclipped);
     - with V1SCLBEHAVIOUR: the mode is restored if this was the last object on the device, nobody
       is exclusive, or this object is exclusive. `FUN_73734f22` only acts if this object changed the mode
       (`DDRAWILCL_MODEHASBEENCHANGED`);
   - a window DirectDraw created (DDSCL_CREATEDEVICEWINDOW) is destroyed (119-125).
   - **The game window is not made non-topmost** (that only happens in SetCooperativeLevel(NORMAL)).
4. Returns the new INT count (249).

dd2d9 (`DDraw::Release`/`TearDown`, ddraw.cpp:87-129): per-version count, and over-release returns 0
(matches). The teardown restores the mode unless another object is exclusive, gives the window back
its style and non-topmost state, and **keeps surfaces, palettes and clippers usable**: they report
DDERR_INVALIDOBJECT where they need the object (surface.cpp:1175). See difference 27.

## Compact — `IDirectDraw7_Compact.c` (all versions)

The method returns DD_OK if this object is the exclusive owner, otherwise DDERR_NOEXCLUSIVEMODE (19-28).
dd2d9 (wrappers.h:232) always returns DD_OK. See difference 13.

## CreateClipper — `IDirectDraw7_CreateClipper.c` → `FUN_7373ce21`

1. outer ≠ NULL → CLASS_E_NOAGGREGATION (16, 66-67).
2. `*out = NULL` (17).
3. **flags ≠ 0 → DDERR_INVALIDPARAMS** (18, 62-63).
4. The clipper is created on the object's clipper list.
5. **Through anything other than DD1/DD2, the DirectDraw interface is AddRef'd** and remembered, to be
   released with the clipper (50-55).

dd2d9 (`DDraw::CreateClipper`, ddraw.cpp:1188): outer, NULL out. It ignores flags and takes no DirectDraw
reference. See difference 14.

## CreatePalette — `IDirectDraw7_CreatePalette.c` (all versions)

1. outer ≠ NULL → CLASS_E_NOAGGREGATION, before anything else (51-53).
2. `*out = NULL` (65).
3. `flags & 0xFFFFF800` (unknown bits) → DDERR_INVALIDPARAMS (66 → 231).
4. SetCooperativeLevel never succeeded (`!DDRAWILCL_SETCOOPCALLED`) → DDERR_NOCOOPERATIVELEVELSET (67-69).
5. **`flags & (DDPCAPS_PRIMARYSURFACE | DDPCAPS_PRIMARYSURFACELEFT | DDPCAPS_VSYNC)` → DDERR_INVALIDPARAMS** (71 → 231).
6. **Not exactly one of 1BIT/2BIT/4BIT/8BIT**, or ALPHA together with 8BITENTRIES → DDERR_INVALIDPARAMS (72-90).
7. 8BIT with 8BITENTRIES, or ALLOW256 without 8BIT → DDERR_INVALIDPARAMS (91-97).
8. The entries are read (count × 4 bytes, or count bytes with 8BITENTRIES). NULL → DDERR_INVALIDPARAMS through SEH (120-129).
9. The driver/HEL CreatePalette callback runs. None → DDERR_UNSUPPORTED (173-218).
10. Through anything other than DD1/DD2, the DirectDraw interface is AddRef'd (208-212).

dd2d9 (`DDraw::CreatePalette`, ddraw.cpp:1172): outer, NULL out, `!coop_` → NOCOOPERATIVELEVELSET, no
size flag → INVALIDPARAMS. Through v4/v7 it takes a DirectDraw reference (matches 10). See difference 15.

## CreateSurface — `IDirectDraw2_CreateSurface.c` (v1/v2), `IDirectDraw7_CreateSurface.c` (v4/v7) → `FUN_73736f21`

Entry:
- v1/v2: `!SETCOOPCALLED` → NOCOOPERATIVELEVELSET (47-49). `dwSize ≠ 0x6c` → INVALIDPARAMS (50, 62).
  The current mode isn't in the mode table (`dwModeIndex == -1`) and the caps lack SYSTEMMEMORY →
  DDERR_UNSUPPORTEDMODE (51, 59). The description is converted to 0x7c (`FUN_7375baac`).
  **CLASS_E_NOAGGREGATION comes only after these** (FUN_73736f21:17).
- v4/v7 (`FUN_73736f21`): outer → CLASS_E_NOAGGREGATION (17-19). `!SETCOOPCALLED` → NOCOOPERATIVELEVELSET
  (28-30). `dwSize ≠ 0x7c` → INVALIDPARAMS (31, 74). The UNSUPPORTEDMODE rule (32-33, 71).
- `*out = NULL` (34). Then `FUN_73737051`, the core (43).
- On success, through anything other than DD1/DD2, the DirectDraw interface is AddRef'd and remembered (59-64).

`FUN_73737051`, in order:
1. SYSTEMMEMORY and VIDEOMEMORY → INVALIDCAPS (31-33). **This is the first caps check.**
2. Through DD1/DD2, the DDSD_PITCH and DDSD_LINEARSIZE flags are dropped (34-36). Without DDSD_LPSURFACE they are
   dropped too (38-42): a pitch is only used with client memory.
3. **DDSD_LPSURFACE** (43-111), checked here, before every other caps rule:
   - no SYSTEMMEMORY → INVALIDCAPS;
   - `IsBadReadPtr(lpSurface, 4)` → INVALIDPARAMS;
   - OPTIMIZED, COMPLEX, 0x00800000, OVERLAY/PRIMARYSURFACE/PRIMARYSURFACELEFT, or VIDEOPORT → INVALIDCAPS;
   - WIDTH and HEIGHT not both given → INVALIDPARAMS;
   - **`lpSurface & 3` and the interface isn't DD4 → INVALIDPARAMS** (DD4 allows an unaligned pointer, DD7 doesn't);
   - non-FourCC: no DDSD_PITCH (always the case through DD1/DD2, see 2), `lPitch < (bpp·width+7)/8`, or
     `lPitch & 3` → INVALIDPARAMS;
   - DXTn: DDSD_PITCH is turned into DDSD_LINEARSIZE (DD4 only, otherwise INVALIDPARAMS). The linear size
     must cover the blocks;
   - another FourCC → UNSUPPORTED.
4. `caps2 & 1` → INVALIDCAPS (112-114). LOCALVIDMEM/NONLOCALVIDMEM without VIDEOMEMORY → INVALIDCAPS (115-117).
   **OWNDC without SYSTEMMEMORY → DDERR_UNSUPPORTED** (118-120).
5. Memory placement:
   - managed textures with any of SYSTEMMEMORY/VIDEOMEMORY/LOCAL/NONLOCAL → INVALIDCAPS (123-124, 180-181);
   - neither SYSTEMMEMORY nor VIDEOMEMORY: video memory is tried, then system memory if that failed with
     something other than INVALIDCAPS or INVALIDPARAMS (143-163).
6. After creation, the color keys are set with `IDirectDrawSurface7_SetColorKey` in the order SRCBLT, DESTBLT,
   SRCOVERLAY, DESTOVERLAY. If one fails, the surface is released, `*out = NULL`, and that error is returned (223-250).

`FUN_73737235` (the creation itself):
1. COMPLEX MIPMAP without DDSD_MIPMAPCOUNT (`FUN_7375b844`, 104-117): the sizes must be powers of two
   (INVALIDPARAMS). The level count follows the shorter side, or the longer side through DD7 when the D3D condition at
   107 holds.
2. caps2 VOLUME → INVALIDCAPS (118-120).
3. The validator `FUN_73736cad` (122-126), below.
4. Hardware support: ALPHA without HW → NOALPHAHW, STEREO → NOSTEREOHARDWARE/NOSURFACELEFT, ZBUFFER → NOZBUFFERHW,
   TEXTURE → NOTEXTUREHW, MIPMAP → NOMIPMAPHW, OVERLAY → NOOVERLAYHW, VIDEOPORT → INVALIDPARAMS,
   NONLOCALVIDMEM → NONONLOCALVIDMEM (181-223).
5. **Primary surface** (225-247):
   - FLIP primary and (not the exclusive owner, or the device not DDRAWI_FULLSCREEN) → **DDERR_NOEXCLUSIVEMODE**.
     This comes first;
   - **`lcl->lpPrimary ≠ NULL` → DDERR_PRIMARYSURFACEALREADYEXISTS, lost or not**. `lpPrimary` is only cleared when the
     primary surface is destroyed (`FUN_73733099_73733099.c:244-248`). Destroying the root also destroys
     its implicit members, even ones the game still references (see surface-other.md, Release);
   - nobody exclusive: `FUN_7373ab89` → DDERR_INCOMPATIBLEPRIMARY on failure;
   - **another object is exclusive → DDERR_NOEXCLUSIVEMODE**;
   - this object is exclusive: `FUN_73760607(gbl)` **makes every other DirectDraw object's primary (and its
     implicit members) lost** (FUN_73760607_73760607.c:26-31).
6. Pixel format support → INVALIDPIXELFORMAT (262-267).

`FUN_73736cad` (validator), in order. F = dwFlags, C = dwCaps, C2-C4 = dwCaps2-4.
1. Without COMPLEX: BACKBUFFERCOUNT or MIPMAPCOUNT → INVALIDCAPS. With COMPLEX: both → INVALIDPARAMS (40-50).
2. FLIP: not COMPLEX, no BACKBUFFERCOUNT, or a count of 0 → INVALIDCAPS (51-64).
3. `C2 & 2` (RESERVED4) → INVALIDCAPS, except a kernel-mode video-port overlay (65-70).
4. Caps rules, `FUN_73736a8e` (all INVALIDCAPS unless noted):
   - C = 0, `C & 0x03000000`, `C2 & 1`, `C3 & 0xFFFFE000`, `C4 ≠ 0` (not volume) (12-15);
   - **PALETTE or VISIBLE requested** (16);
   - WRITEONLY without managed;
   - DONOTPERSIST without managed;
   - managed without TEXTURE;
   - both TEXTUREMANAGE and D3DTEXTUREMANAGE;
   - HINTDYNAMIC/HINTSTATIC/OPAQUE without TEXTURE, or any two of them;
   - OPTIMIZED;
   - LOCAL/NONLOCALVIDMEM with SYSTEMMEMORY, or both of them;
   - **FRONTBUFFER/BACKBUFFER through anything but DD1**;
   - 0x00800000 with other caps (16-30);
   - **COMPLEX** (31-37): FRONTBUFFER; BACKBUFFER unless (ALPHA or ZBUFFER) and not FLIP; none of
     TEXTURE/PRIMARYSURFACE/OVERLAY/OFFSCREENPLAIN/BACKBUFFER; none of MIPMAP/ZBUFFER/FLIP/ALPHA and not a cube map;
   - BACKBUFFER with FRONTBUFFER or ALPHA (38);
   - FLIP without COMPLEX (39);
   - **PRIMARYSURFACE with TEXTURE/OVERLAY/OFFSCREENPLAIN/BACKBUFFER, or with ZBUFFER without FLIP** (40-41);
   - **OFFSCREENPLAIN with TEXTURE/OVERLAY/BACKBUFFER** (42);
   - OVERLAY with TEXTURE/OFFSCREENPLAIN/BACKBUFFER, or with ZBUFFER without FLIP (43-44);
   - ZBUFFER+BACKBUFFER without COMPLEX (47-51);
   - MIPMAP without TEXTURE, or with ZBUFFER (52-59);
   - ALLOCONLOAD without TEXTURE (60-62);
   - cube faces without CUBEMAP (63-66);
   - CUBEMAP (67-90): other caps (anything but COMPLEX, SYSTEMMEMORY/VIDEOMEMORY, TEXTURE, 3DDEVICE, OWNDC,
     MIPMAP, ALLOCONLOAD, LOCAL/NONLOCAL) → INVALIDPARAMS; neither TEXTURE nor 3DDEVICE → INVALIDPARAMS;
     not COMPLEX → **DDERR_DDSCAPSCOMPLEXREQUIRED**; no face → INVALIDPARAMS; no driver cube support → UNSUPPORTED;
   - STEREOSURFACELEFT without a primary video-memory flip chain → UNSUPPORTED (91-95).
5. **DDSCAPS_ALPHA → INVALIDPARAMS** (the `test cl,2` on the caps at 0x73736d3a, 75-77).
   **DDSD_ALPHABITDEPTH → INVALIDPARAMS** (78-80).
6. Z buffers (81-183):
   - not ZBUFFER: DDSD_ZBUFFERBITDEPTH, or a pixel format with DDPF_ZBUFFER or DDPF_STENCILBUFFER → INVALIDPARAMS;
   - ZBUFFER, 0x6c structure (v1/v2): without DDSD_PIXELFORMAT, DDSD_ZBUFFERBITDEPTH is required (INVALIDPARAMS) and a
     Z pixel format is built from it. With one, it must be `dwSize == 32`, DDPF_ZBUFFER and no stencil (INVALIDPARAMS);
   - ZBUFFER, 0x7c structure (v4/v7): **COMPLEX → INVALIDPARAMS, DDSD_ZBUFFERBITDEPTH → INVALIDPARAMS**, and a pixel
     format is required with `dwSize == 32`, DDPF_ZBUFFER and a non-zero Z mask (INVALIDPARAMS). Stencil without
     driver support → UNSUPPORTED;
   - depth < 8 → INVALIDPARAMS. With stencil, its depth and mask must be non-zero.
7. Size (186-216):
   - **a primary with DDSD_HEIGHT or DDSD_WIDTH → INVALIDPARAMS**;
   - not a primary: height and width both required and non-zero (INVALIDPARAMS). A width above the driver's
     limit (`FUN_7375b6a5`), unless the device has DDCAPS2_WIDESURFACES → INVALIDPARAMS.
8. MIPMAP (217-237): MIPMAPCOUNT of 0, **a non-power-of-two width or height, or more levels than
   log2(max side)+1 → INVALIDPARAMS**.
9. DDSD_PIXELFORMAT (238-310):
   - **on a primary → INVALIDPARAMS**;
   - DXT1-5 with a width or height not a multiple of 4 → INVALIDPARAMS;
   - other formats go through `FUN_73736ead`: bad bit counts or flag combinations → INVALIDPIXELFORMAT
     (some → INVALIDPARAMS or UNSUPPORTED).
10. Color keys **validated** with SetColorKey's checker `FUN_7373e021` in the order DESTOVERLAY, DESTBLT, SRCOVERLAY,
    SRCBLT. Its error is returned (311-326). With dd2d9's caps, overlay keys and ranges fail there
    (NOCOLORKEYHW / NOOVERLAYHW, see surface-flip-key-palette.md §3).
11. CUBEMAP: **only through DD7, square, power of two, without BACKBUFFERCOUNT**, otherwise INVALIDPARAMS (327-336).

dd2d9 (`DDraw::CreateSurfaceImpl`, ddraw.cpp:1431-1714):
- order: outer → coop → description size → NULL out;
- its own caps list (1453-1480);
- primary rules (1487-1500);
- size, format and Z (1506-1524);
- mip count, clamped (1546-1563);
- OVERLAY → NOOVERLAYHW (1565);
- color key ranges → NOCOLORKEYHW (1567-1573);
- client memory (1576-1585);
- **a lost primary is replaced by the new one** (1487-1497, 1700).
It matches the v1/v2/v4/v7 size rule, the FRONT/BACKBUFFER rule by version, the managed rules it has,
NOEXCLUSIVEMODE for a non-exclusive flip primary, and the DirectDraw reference through v4/v7.
See differences 2, 3, 16-20.

## DuplicateSurface — `IDirectDraw7_DuplicateSurface.c` (all versions)

1. Source lost → **DDERR_SURFACELOST** (25, 58). A NULL source gives INVALIDPARAMS through SEH.
2. `*out = NULL` (26).
3. Source is a primary → CANTDUPLICATE (27, 55). Source is an implicit member (IMPLICITCREATE) → CANTDUPLICATE
   (28-32). **The root of a non-primary complex surface (offscreen flip chain front, mip root) can be duplicated.**
4. A new LCL sharing the GBL is made, owned by the calling object (35-36). **The interface has the source
   interface's vtable** (`FUN_7373e7ca(lcl, *src)`, 37), whatever the DirectDraw version. **No DirectDraw
   reference is taken** (surfMore+0x4c isn't set, unlike FUN_73736f21:63). AddRef → DD_OK.

dd2d9 (`DDraw::DuplicateSurface`, ddraw.cpp:1389): primary, member or flip-chain root → CANTDUPLICATE. No lost
check. The interface version comes from the DirectDraw version, and it takes a DirectDraw reference through v4/v7.
See difference 21.

## EnumDisplayModes — `IDirectDraw7_EnumDisplayModes.c` (v4/v7), `IDirectDraw2_EnumDisplayModes.c` (v1/v2)

1. v1/v2: a non-NULL filter must have `dwSize == 0x6c`, else INVALIDPARAMS (2_…:29-30). It is copied to a 0x7c structure.
   v4/v7: the filter must be NULL or `0x7c`, `flags & ~3` → INVALIDPARAMS, `IsBadCodePtr(cb)` → INVALIDPARAMS
   (54-55, the value at 0x737624b8). An allocation failure, or a fault in the callback, → DDERR_GENERIC (0x7376212b, 0x7376248a).
2. Modes come from `EnumDisplaySettings`, **in the driver's order**, < 8 bpp skipped, 15 bpp reported as 16 (66-141).
   The filter matches WIDTH, HEIGHT, PIXELFORMAT (`dwRGBBitCount` only) and REFRESHRATE (the mode's own rate) (89-93).
   Duplicates (w, h, bpp, and the rate with DDEDM_REFRESHRATES) are dropped. **With DDEDM_REFRESHRATES each rate is a
   separate entry.** Otherwise the rate is 0 (112).
3. Per mode (`FUN_73761f69`):
   - `dwSize` 0x6c through DD1/DD2, otherwise 0x7c;
   - `dwFlags = 0x4100e` (WIDTH|HEIGHT|PITCH|PIXELFORMAT|REFRESHRATE);
   - **`lPitch = width·bpp/8`, not rounded** (26);
   - DD7 only: STEREOSURFACELEFT in the caps for stereo modes.
   The callback returning 0 (DDENUMRET_CANCEL) stops the enumeration (151).
4. With DDSCL_ALLOWMODEX set and 640×480×8 listed, 320×200×8 and 320×240×8 are added if they're missing, with
   **DDSCAPS_MODEX** in their caps (156-160, FUN_73761f69:48).
5. Returns DD_OK.

dd2d9 (`DDraw::EnumDisplayModes`, ddraw.cpp:1019, wrapper wrappers.h:241): a deliberately virtual list (classic
sizes plus the monitor's, in 8/16/32 bpp), sorted by bpp then size. The pitch is rounded to 4. One entry per size,
with the current rate. NULL callback → INVALIDPARAMS, and the filter can be either size. See difference 22.

## EnumSurfaces — `IDirectDraw7_EnumSurfaces.c` (v4/v7), `IDirectDraw2_EnumSurfaces.c` (v1/v2)

Flags: ALL 1, MATCH 2, NOMATCH 4, CANBECREATED 8, DOESEXIST 0x10.
1. v1/v2: filter `dwSize ≠ 0x6c` → INVALIDPARAMS (2_…). It is copied to 0x7c.
2. `IsBadCodePtr(cb)`, or `flags & ~0x1F` → INVALIDPARAMS (86-88). v4/v7: filter `dwSize ≠ 0x7c` → INVALIDPARAMS (91).
3. ALL with MATCH or NOMATCH → INVALIDPARAMS. **Not ALL and no filter → INVALIDPARAMS.** MATCH with NOMATCH →
   INVALIDPARAMS (asm 0x737639df-0x737639f3).
4. CANBECREATED requires MATCH, and neither ALL nor NOMATCH (0x737639f5-0x73763a02).
5. Filter (0x73763a04-0x73763a62):
   - DDSD_LPSURFACE → INVALIDPARAMS;
   - caps `& 0x03000000` → INVALIDPARAMS;
   - **caps OPTIMIZED → DD_OK with nothing enumerated**;
   - `caps2 & 1`, `caps3 & 0xFFFFE000`, or caps4 ≠ 0 → INVALIDCAPS;
   - caps `& 0x00800000` → INVALIDPARAMS;
   - HEIGHT without WIDTH or the reverse → INVALIDPARAMS;
   - CANBECREATED with any color key flag → INVALIDPARAMS;
   - DDSD_CAPS is added if it is missing.
6. Neither CANBECREATED nor DOESEXIST → INVALIDPARAMS (asm 0x737640d1).
7. DOESEXIST (132-177): walks `gbl->dsList`, the interface pointers of this object's surfaces **newest first,
   one entry per interface pointer** (so a surface with v1 and v7 interfaces is reported twice).
   - Created members are included. 0x00800000 surfaces are skipped.
   - ALL matches everything. Otherwise `FUN_737637e9` decides, inverted by NOMATCH.
   - Each match gets a QueryInterface (v1 for DD1/DD2, v4, v7), so the callback must Release it, plus a 0x6c/0x7c
     description (`FUN_73734fe9`).
   - The callback returning 0 stops the walk.
8. `FUN_737637e9` matching compares only these fields:
   - CAPS: all four words **equal**;
   - HEIGHT, WIDTH;
   - LPSURFACE;
   - the CKDESTBLT/CKSRCBLT keys, against the stored key, set or not;
   - BACKBUFFERCOUNT;
   - MIPMAPCOUNT;
   - PIXELFORMAT: **only surfaces created with an explicit pixel format** (HASPIXELFORMAT), 32 bytes compared;
   - the overlay keys: no match unless the surface has overlay data.
   PITCH, ALPHABITDEPTH, REFRESHRATE, LINEARSIZE, TEXTURESTAGE and ZBUFFERBITDEPTH are ignored. **The filter's flags
   don't have to be a subset of the surface's.**
9. CANBECREATED (179-402):
   - sizes: the filter's, or, without DDSD_HEIGHT (and not a primary), every distinct display size;
   - formats: with DDPF_RGB, every distinct display format; with DDPF_FOURCC, every driver FourCC; otherwise the filter's;
   - each combination is **really created and destroyed**, a primary included (`FUN_73737051`), and successes are
     passed to the callback with a NULL surface;
   - **the callback's return value is ignored** (394);
   - through v4/v7 **the caller's filter is modified** (WIDTH|HEIGHT flags, size, format; 357-385).
10. Returns DD_OK (0x737641c6).

dd2d9 (`DDraw::EnumSurfaces`, ddraw.cpp:1295, `DescMatches` 1271): CANBECREATED needs exactly MATCH and a filter
(matches 4). A primary is faked. DOESEXIST enumerates `surfaces` in creation order. The filter can be either size,
and a missing filter with no ALL enumerates everything. See difference 23.

## EvaluateMode / StartModeTest — `IDirectDraw7_EvaluateMode.c`, `IDirectDraw7_StartModeTest.c`

StartModeTest → DDERR_TESTFINISHED. **EvaluateMode → DDERR_INVALIDPARAMS.** Neither has checks.
dd2d9 (wrappers.h:276-277): both DDERR_TESTFINISHED. See difference 28.

## FlipToGDISurface — `IDirectDraw7_FlipToGDISurface.c` → `FUN_73734deb`

1. No primary → DDERR_NOTFOUND. The primary lacks DDSCAPS_FLIP → DDERR_NOTFLIPPABLE (20-26).
2. A system-memory primary → DD_OK (FUN_73734deb:16). The driver's FlipToGDISurface callback, if it handles the
   call, decides. The primary is already the GDI surface (`DDRAWISURFGBL_ISGDISURFACE`) → DD_OK (36).
3. Otherwise the chain is walked to the GDI surface and `Flip(primary, gdi, DDFLIP_WAIT)` runs. Its result is
   returned (41-50).

dd2d9 (`DDraw::FlipToGDISurface`, ddraw.cpp:1221) does the same. **Matches.**

## GetAvailableVidMem — `IDirectDraw7_GetAvailableVidMem.c` (v4/v7), `IDirectDraw2_GetAvailableVidMem.c` (v2)

1. A device without hardware → DDERR_NODIRECTDRAWHW (47).
2. **NULL caps → DDERR_INVALIDPARAMS** (dereferenced, SEH). v2 copies only `dwCaps` (2_…:167-169).
3. `caps & 0x03000000`, `caps2 & 1`, `caps3 & 0xFFFFE000`, or caps4 ≠ 0 → DDERR_INVALIDCAPS (48-49, 120).
4. **caps with BACKBUFFER, COMPLEX, FLIP, FRONTBUFFER, PALETTE, SYSTEMMEMORY, VISIBLE, WRITEONLY or OWNDC
   (0x5893c) → DDERR_INVALIDPARAMS** (50, 117).
5. Both out pointers NULL → INVALIDPARAMS (51-55). Otherwise each non-NULL one is set to 0, then to the driver's
   figure for the requested memory type (61-112).

dd2d9 (`DDraw::GetAvailableVidMem`, ddraw.cpp:1153): ignores caps, and both NULL → DD_OK. The figures are a fixed
512 MB total minus its video-memory surfaces (deliberate). See difference 24.

## GetCaps — `IDirectDraw7_GetCaps.c` (all versions)

1. Both NULL → INVALIDPARAMS. **Each non-NULL structure's `dwSize` must be 0xac, 0x13c, 0x16c or 0x17c**
   (DX1, DX3, DX5, DX6), checked for both **before** either is written (39-48).
2. Driver caps: the structure is zeroed to its size, then that many bytes are copied. DDSCAPS 0x00800000 is
   cleared. `dwVidMemTotal`/`dwVidMemFree` come from GetAvailableVidMem(VIDEOMEMORY) (50-82).
3. HEL caps: copied the same way, with **`dwVidMemFree = 0`** (83-113).

dd2d9 (`DDraw::GetCaps`, ddraw.cpp:1053-1115): accepts any size from 4 to sizeof(DDCAPS), checks each structure as
it copies it (so `drv` is already written when `hel` turns out bad), and the HEL gets the driver's memory figures.
The caps contents are dd2d9's own (deliberate). See difference 25.

## GetDeviceIdentifier — `IDirectDraw7_GetDeviceIdentifier.c` (v7), `IDirectDraw4_GetDeviceIdentifier.c` (v4) → `FUN_737655bf`

1. `IsBadWritePtr(id, 0x430)` → INVALIDPARAMS (42, asm 0x73765822). v4 checks 0x428 bytes and copies those
   (4_…). A bad pointer → INVALIDPARAMS (0x73765908).
2. **`flags & ~DDGDI_GETHOSTIDENTIFIER` → INVALIDPARAMS, but the zeroed structure is still copied out**
   (52, asm 0x737657da → 0x737657e4).
3. The driver strings and version are looked up. Without DDGDI_GETHOSTIDENTIFIER the identity of an add-on 3D card
   is reported (`FUN_7375bca1`). `dwWHQLLevel` is filled for v7 only (107-111).

dd2d9 (`DDraw::GetDeviceIdentifier`, ddraw.cpp:1160): NULL → INVALIDPARAMS, flags ignored, Direct3D 9's adapter
identifier. See difference 25.

## GetDisplayMode — `IDirectDraw7_GetDisplayMode.c` (all versions)

1. `dwSize` must be 0x7c or 0x6c (writable) on any interface, else INVALIDPARAMS (28-30, 71).
2. The structure is zeroed to its own size. **`dwSize` is then set to the interface's size** (0x6c for DD1/DD2,
   0x7c otherwise) (32-46).
3. `dwFlags = 0x4100e`. Width, height and refresh rate come from the current mode, plus the display pixel format.
   **`lPitch = width·bpp/8`, not rounded** (47-55). DD7 only: STEREOSURFACELEFT for stereo modes (57-62).

dd2d9 (`DDraw::GetDisplayMode`, ddraw.cpp:968): size check, flags, `dwSize` per interface (match). The mode is the
game's (virtual) mode, with the pitch rounded up to 4 (979). See difference 22.

## GetFourCCCodes — `IDirectDraw7_GetFourCCCodes.c` (all versions)

Codes NULL → `*count = N`. Otherwise `*count = min(*count, N)` and that many codes are copied. DD_OK (22-38).
NULL count → INVALIDPARAMS through SEH. dd2d9 (wrappers.h:250) reports N = 0 (deliberate: it offers no FourCC
formats), and so gives the same results. **Matches.**

## GetGDISurface — `IDirectDraw7_GetGDISurface.c` (all versions)

1. `*out = NULL` (61). No primary → DDERR_NOTFOUND (105).
2. The surface whose GBL has ISGDISURFACE: the primary, or the first one found walking its flip chain (63-102).
   None → NOTFOUND.
3. The interface handed out:
   - DD1/DD2: the primary's own interface pointer, or the chain link's;
   - DD4: an IDirectDrawSurface4 one;
   - DD7: an IDirectDrawSurface7 one.
   **When the found interface isn't already that version, a new interface pointer is made on every call**
   (`FUN_7373e7ca`, 72, 94). AddRef → DD_OK.

dd2d9 (`DDraw::GetGDISurface`, ddraw.cpp:1209, `GdiSurface` 1198): the surface holding the primary's first memory,
or **the primary if none does** (1206). It returns the shared per-version interface. See difference 29.

## GetMonitorFrequency — `IDirectDraw7_GetMonitorFrequency.c`

The device's frequency is 0 → DDERR_UNSUPPORTED (out not touched). Otherwise it is written → DD_OK (61-72).
dd2d9 (ddraw.cpp:1117) writes the current rate (default 60). **Matches** (driver-dependent).

## GetScanLine — `IDirectDraw7_GetScanLine.c`

The driver's GetScanLine callback is used: `*line` is set to the driver's line and **the driver's result is returned,
DDERR_VERTICALBLANKINPROGRESS included, with the line written** (110-126). Without a callback, `*line = 0` →
DDERR_UNSUPPORTED (128-131).
dd2d9 (ddraw.cpp:1123) emulates it. During the emulated blank it returns VERTICALBLANKINPROGRESS **without writing
`*line`** (1132). See difference 30.

## GetSurfaceFromDC — `IDirectDraw7_GetSurfaceFromDC.c` (v4/v7) → `FUN_73760741`

1. `*out = NULL` (19). The DC is looked up in **the process-wide list of surface DCs, any DirectDraw object's**
   (22-30). Not found (NULL included) → DDERR_NOTFOUND.
2. The interface: v1 (IDirectDrawSurface) through DD4, v7 through DD7. It is found or made (`FUN_7373e5c8`) and
   AddRef'd → DD_OK. Failing to make one → OUTOFMEMORY (31-40).

dd2d9 (`DDraw::GetSurfaceFromDC`, ddraw.cpp:1238, wrapper wrappers.h:269): the same versions, but it searches only
this object's surfaces with a DC out. See difference 29.

## GetVerticalBlankStatus — `IDirectDraw7_GetVerticalBlankStatus.c`

The driver's WaitForVerticalBlank callback (GETSTATUS) is used, and its result returned (38-55). Without one, the
answer **alternates TRUE/FALSE** on each call → DD_OK (58-62). dd2d9 (ddraw.cpp:1137) uses its emulated phase.
**Matches** (driver-dependent).

## Initialize — `IDirectDraw7_Initialize.c` (all versions, Uninit included)

- `lpGbl ≠ NULL` (already initialized) → DDERR_ALREADYINITIALIZED (34, 65).
- Otherwise **only the interface called** is switched from its Uninit vtable to the real one (UninitA → v7 also
  sets DDRAWILCL_DIRECTDRAW7). The object is then created for the GUID (`FUN_7373ad42`). On failure the vtable is
  put back (35-58).

dd2d9 (`DDraw::Initialize`, ddraw.cpp:688): CoCreateInstance objects (`initialized = false`) → DD_OK once, then
ALREADYINITIALIZED. The GUID is ignored, and every method works before Initialize. See difference 28.

## RestoreAllSurfaces — `IDirectDraw7_RestoreAllSurfaces.c` (v4/v7) → `FUN_7376d953`

The method walks `gbl->dsList` for this object's interface pointers, **skipping implicit members**, restoring
**oldest first** (recursion to the tail). It **stops at the first failure and returns it** (FUN_7376d953:9-22).
DD_OK otherwise.
dd2d9 (`DDraw::RestoreAllSurfaces`, ddraw.cpp:961-966) calls `Restore` on every surface, members included,
**ignores the results** and returns DD_OK. See difference 4.

## RestoreDisplayMode — `IDirectDraw7_RestoreDisplayMode.c` (all versions)

1. **Another object is exclusive → DDERR_NOEXCLUSIVEMODE**, even when there is nothing to restore (45 → 54).
2. A video-memory surface of the device is locked → DDERR_SURFACEBUSY (19-28).
3. `FUN_73734f22(lcl, 1)`. This object never changed the mode → DD_OK. Otherwise the registry mode is set
   again: failure → DDERR_UNSUPPORTED. Success → `MODEHASBEENCHANGED` is cleared, the mode change is handled
   (`FUN_7373cc84`), and every window is redrawn (FUN_73734f22:13-46).

dd2d9 (`DDraw::RestoreDisplayMode`, ddraw.cpp:928): nothing set → DD_OK **first**, then another object exclusive →
NOEXCLUSIVEMODE. There is no SURFACEBUSY. See difference 12.

## SetCooperativeLevel — `IDirectDraw7_SetCooperativeLevel.c` (all versions)

Flags: FULLSCREEN 1, ALLOWREBOOT 2, NOWINDOWCHANGES 4, NORMAL 8, EXCLUSIVE 0x10, (0x20 internal), ALLOWMODEX 0x40,
SETFOCUSWINDOW 0x80, SETDEVICEWINDOW 0x100, CREATEDEVICEWINDOW 0x200, MULTITHREADED 0x400, FPUSETUP 0x800,
FPUPRESERVE 0x1000.

Validation, in order (all DDERR_INVALIDPARAMS unless noted):
1. `flags & 0xFFFFC000`, or **FPUSETUP with FPUPRESERVE** (45).
2. The device isn't attached to the desktop → NOWINDOWCHANGES is forced (46-48).
3. None of NORMAL/EXCLUSIVE/SETFOCUSWINDOW; EXCLUSIVE or ALLOWMODEX without FULLSCREEN; hwnd not NULL and not a
   window; 0x20 with EXCLUSIVE (49-53).
4. SETFOCUSWINDOW (54-60): without CREATEDEVICEWINDOW, anything but NOWINDOWCHANGES (and 0x2000) alongside it.
   The object has HASEXCLUSIVEMODE → **DDERR_HWNDALREADYSET**.
5. SETDEVICEWINDOW (61, 266-271): with SETFOCUSWINDOW or CREATEDEVICEWINDOW → INVALIDPARAMS. hwnd given and no
   focus window set → **DDERR_NOFOCUSWINDOW**.
6. CREATEDEVICEWINDOW (63-69): without EXCLUSIVE, or the object has a window it didn't create → INVALIDPARAMS.
   Without SETFOCUSWINDOW: hwnd not NULL → INVALIDPARAMS, and no focus window → NOFOCUSWINDOW.
7. EXCLUSIVE without CREATEDEVICEWINDOW: hwnd NULL or WS_CHILD (70-78).

Then:
8. Called through DD1 → `DDRAWILCL_V1SCLBEHAVIOUR`, permanently (81-83).
9. **hwnd given, no SETDEVICEWINDOW/CREATEDEVICEWINDOW, and its style has WS_DLGFRAME without WS_BORDER →
   NOWINDOWCHANGES is forced** (84-87).
10. **EXCLUSIVE: another object (any object in the process; on another monitor only if it uses another focus
    window) holds exclusive mode → DDERR_EXCLUSIVEMODEALREADYSET. There is no takeover** (89-101). The holder
    loses ownership only by:
    - SetCooperativeLevel(NORMAL);
    - its Release;
    - **losing activation**: the window hook's WM_ACTIVATEAPP(FALSE) handler `FUN_7375d5aa` calls `FUN_7375aa94`,
      which sets the owner to NULL (FUN_7375aa94:63). It takes ownership back when reactivated (`FUN_7375ac27`).
11. SETFOCUSWINDOW: `hFocusWnd = hwnd` (**NULL too**) → DD_OK, without SETCOOPCALLED (103-127).
12. CREATEDEVICEWINDOW: a "DirectDrawDeviceWnd" popup over the device is made, owned by the focus window (129-166).
    A window DirectDraw made earlier is destroyed if the window changes (167-173).
13. **`lcl->hWnd = hwnd` (NULL too)**. Without SETDEVICEWINDOW/CREATEDEVICEWINDOW, `hFocusWnd = hwnd` too (174-183).
    The ALLOWMODEX state is stored (184-189).
14. Not exclusive (190-218): if the object had HASEXCLUSIVEMODE:
    - **FlipToGDISurface**;
    - `FUN_7375aa94`: mode restored unless V1SCLBEHAVIOUR, owner cleared, cursor unclipped, gamma reset,
      WM_PALETTECHANGED broadcast;
    - HASEXCLUSIVEMODE and ISFULLSCREEN cleared;
    - the window unhooked;
    - unless NOWINDOWCHANGES, the old window gets `SetWindowPos(HWND_NOTOPMOST, SWP_NOSIZE|SWP_NOMOVE|SWP_NOACTIVATE)`.
15. Exclusive (219-243):
    - FULLSCREEN marks the device DDRAWI_FULLSCREEN;
    - the window is hooked (`FUN_7375e3ef`). Unless NOWINDOWCHANGES, this first moves the window over the device
      rectangle (`SetWindowPos(.., SWP_NOZORDER|SWP_NOACTIVATE)`, style untouched). If the focus window is in front,
      it is shown, made topmost and un-maximized (`FUN_7375d40c`);
    - **the hook is per process: one already on another window → DDERR_HWNDALREADYSET** (FUN_7375e236:57-58),
      after the window changes above. This happens when the owner calls again with a new window without going
      NORMAL, or when an earlier hook couldn't be removed because something subclassed the window after DirectDraw;
    - **only if the object wasn't already the owner**: `FUN_7375ac27` (owner = this object, **every surface of the
      device not in system memory is lost**, `FUN_73760648`; gamma applied), ACTIVEYES,
      `SetForegroundWindow(hwnd)` (`FUN_7375d557`), HASEXCLUSIVEMODE, and **`ClipCursor(device rectangle)`**
      (`FUN_7375d3c8`).
16. SETCOOPCALLED. MULTITHREADED/FPUSETUP/FPUPRESERVE are added to the object's flags and never cleared (244-256).
    DD_OK.

dd2d9 (`DDraw::SetCooperativeLevel`, ddraw.cpp:694-796; window handling `ApplyWindow`/`CoverMonitor`/`HookWindow`
434-554):
- the same validation for flags 1-6;
- CREATEDEVICEWINDOW makes the DirectDrawDeviceWnd window (same class, window procedure and focus/window
  bookkeeping: `lclWnd_`, `focusWnd_`, `deviceWnd_`); since 1.2.0 it covers the monitor in exclusive mode while the
  game is in front (`UpdateDeviceWindow`) and the picture is presented into it; its mouse messages are passed on to
  the focus window (1.0.3: hidden, the picture in the focus window);
- DirectDraw 1 mode retention (`ddraw1Coop_`), as XP's V1SCLBEHAVIOUR;
- non-topmost on leaving;
- **a second object takes exclusive mode over** (`TakeOverExclusive`, 898);
- entering exclusive mode loses this object's surfaces, also when only the window changed;
- a re-entrant call from the window procedure is ignored.
The window covers the monitor with its frame off-screen, and the display can be a scaled window: deliberate.
See differences 1, 6, 7, 31.

## SetDisplayMode — `IDirectDraw7_SetDisplayMode.c` (v2/v4/v7), `IDirectDraw_SetDisplayMode.c` (v1: rate 0, flags 0)

1. `flags & ~DDSDM_STANDARDVGAMODE` → INVALIDPARAMS (32, 74).
2. A video-memory surface of the device is locked → DDERR_SURFACEBUSY (35-47).
3. **Another object is exclusive → DDERR_NOEXCLUSIVEMODE** (49-50, 71). No cooperative level is needed, and normal
   mode may change the mode.
4. `FUN_73761aa2`:
   - **the width/height/bpp aren't validated by ddraw**. They go to `ChangeDisplaySettingsEx(CDS_FULLSCREEN)`;
   - 16 bpp becomes 15 on 555 hardware;
   - rate 0 → the registry rate if the mode fits, else 75 Hz, else 60 Hz (`FUN_7376198b`);
   - failure: with ALLOWMODEX, 320×200/240×8 retries as 640×480. Otherwise **DDERR_UNSUPPORTED** (0x73761c70);
   - success → `MODEHASBEENCHANGED`, mode-change handling, and the object's expected mode is updated
     (SetDisplayMode.c:63-66, used by TestCooperativeLevel).

dd2d9 (`DDraw::SetDisplayMode`, ddraw.cpp:798):
- w/h of 0 or > 16384 → INVALIDPARAMS, bpp not 8/16/24/32 → DDERR_INVALIDMODE;
- another object exclusive → NOEXCLUSIVEMODE (match);
- flags ignored, no SURFACEBUSY;
- any mode accepted (virtual modes, deliberate).
See difference 11.

## TestCooperativeLevel — `IDirectDraw7_TestCooperativeLevel.c` (v4/v7)

1. The display device is unavailable (pdevice flag 0x10): DDERR_NOEXCLUSIVEMODE if the object has HASEXCLUSIVEMODE,
   else DDERR_EXCLUSIVEMODEALREADYSET (38-43).
2. Object without HASEXCLUSIVEMODE (46-69):
   - **some object is exclusive, or the desktop isn't visible (`FUN_7375b43a`, an empty system region) →
     DDERR_EXCLUSIVEMODEALREADYSET**;
   - **the current mode differs from the one the object expects (lcl+0x60, updated by its SetDisplayMode) →
     DDERR_WRONGMODE**;
   - else DD_OK.
3. Object with HASEXCLUSIVEMODE: the exclusive owner (it isn't while deactivated, see SetCooperativeLevel 10) and
   the desktop visible → DD_OK, else DDERR_NOEXCLUSIVEMODE (71-76).

dd2d9 (`DDraw::TestCooperativeLevel`, ddraw.cpp:954): NOEXCLUSIVEMODE while an exclusive game is in the background
(`kDevLost`), otherwise DD_OK. In display=window mode an exclusive game is never "in the background" (deliberate).
See difference 5.

## WaitForVerticalBlank — `IDirectDraw7_WaitForVerticalBlank.c` (all versions)

1. **DDWAITVB_BLOCKBEGINEVENT, or a non-NULL event → DDERR_UNSUPPORTED** (38, 69).
2. **flags not exactly DDWAITVB_BLOCKBEGIN (1) or DDWAITVB_BLOCKEND (4) → DDERR_INVALIDPARAMS** (39, 66). 0 is invalid.
3. Mode X, or no driver callback → UNSUPPORTED. Otherwise the driver's result (41-64).

dd2d9 (`DDraw::WaitForVerticalBlank`, ddraw.cpp:1143): BLOCKBEGINEVENT → UNSUPPORTED. Every other flag value, and
any event, waits for the next emulated blank → DD_OK. See difference 10.

---

## Differences from dd2d9

Severity: **game-visible** means a plausible game call sequence behaves differently. **Edge case** means it only
shows up with invalid parameters, unusual sequences or exact-return-code checks.

1. **A second DirectDraw object asking for exclusive mode takes it over** (game-visible, deliberate).
   - XP: DDERR_EXCLUSIVEMODEALREADYSET while another object of the process is the active owner
     (SetCooperativeLevel.c:94-100). The holder keeps everything. Ownership is free only after the holder goes
     NORMAL, is released, or **loses activation** (`FUN_7375d5aa` → `FUN_7375aa94`:63).
   - dd2d9: `TakeOverExclusive` (ddraw.cpp:898) demotes the holder to NORMAL and inherits its mode and window state.
     CHANGELOG 0.7.1 (Type Wild) and the comment at ddraw.cpp:735-737 say this is "as in ddraw.dll". **The XP code
     doesn't support that** (Wine does it this way).
   - Fix: keep the takeover (it's a game fix), but correct the comment and changelog. To be closer to XP, return
     EXCLUSIVEMODEALREADYSET while the holder is in front, and take over only when the holder is in the background,
     its window is gone or hidden, or it was released. Test with Type Wild (see Not determined).
2. **A lost primary is replaced instead of DDERR_PRIMARYSURFACEALREADYEXISTS** (game-visible, deliberate workaround).
   - XP: any existing primary of the object, lost or not, → PRIMARYSURFACEALREADYEXISTS (FUN_73737235:232-233).
     `lpPrimary` is cleared only when the primary dies (FUN_73733099:244-248), and the root dies with its members.
     That is why Jazz Jackrabbit 2 (release the front, keep a back-buffer reference, create again) works on XP.
   - dd2d9 (ddraw.cpp:1487-1497, 1700): members keep the root alive (deliberate, surface-other.md), so a lost primary
     is replaced instead. **A game that tries CreateSurface on purpose while holding a lost primary** (instead of
     Restore) gets a new primary, where XP fails.
   - Fix: replace the lost-primary rule with "the root's last game reference is gone". When the primary root's own
     interface counts reach 0 while only members keep it, clear `primary` (and `isPrimary`) as XP's destruction would.
     Then return PRIMARYSURFACEALREADYEXISTS for any primary the game still holds.
3. **Primary creation across DirectDraw objects** (game-visible, rare: games with two objects).
   - XP:
     - a primary from an object that isn't exclusive while another object is → DDERR_NOEXCLUSIVEMODE (FUN_73737235:241-244);
     - a primary created by the exclusive object **makes every other object's primary lost** (FUN_73760607);
     - a FLIP primary without ownership → NOEXCLUSIVEMODE **before** the ALREADYEXISTS check (226-233).
   - dd2d9 (ddraw.cpp:1487-1500) has none of these, and checks ALREADYEXISTS first. Instead it keeps the superseded
     object off the screen (`Superseded`).
   - Fix: in the primary branch, check `flip && !Exclusive()` first, then `OtherExclusive()` → NOEXCLUSIVEMODE. When
     `Exclusive()`, mark other objects' primaries (and members) lost.
4. **RestoreAllSurfaces ignores failures** (game-visible).
   - XP: restores this object's non-implicit surfaces oldest first, **stops at the first failure and returns it**
     (FUN_7376d953:15-22): DDERR_WRONGMODE after a mode change, or the Restore error while in the background.
   - dd2d9 (ddraw.cpp:961-966) always returns DD_OK, so a game that recreates its surfaces only when this fails keeps
     lost surfaces.
   - Fix: iterate `surfaces` in creation order, skip members (`s->root`), and return the first failing `Restore()`.
5. **TestCooperativeLevel in normal mode** (game-visible for windowed DirectDraw 4/7 games).
   - XP (TestCooperativeLevel.c:46-69):
     - DDERR_WRONGMODE once the display mode differs from the one the object expects;
     - DDERR_EXCLUSIVEMODEALREADYSET while another object is exclusive, or the desktop isn't visible.
   - dd2d9 (ddraw.cpp:954-959): DD_OK. A windowed game polling it after the user changes the desktop resolution never
     sees WRONGMODE, though its surfaces are lost (`PollDisplayChange`).
   - Fix: for a non-exclusive object, return EXCLUSIVEMODEALREADYSET if `OtherExclusive()`. Otherwise remember the
     display mode at creation and at each successful SetDisplayMode/RestoreDisplayMode, and return WRONGMODE when the
     current mode differs (see Not determined for the initial value).
6. **The cursor isn't confined in exclusive mode** (game-visible on multi-monitor systems only).
   - XP: `ClipCursor(device rectangle)` when exclusive mode is taken and on reactivation. It is undone on deactivation
     and on leaving (`FUN_7375d3c8`, SetCooperativeLevel.c:241, FUN_7375aa94:67).
   - dd2d9: no ClipCursor. The mouse can leave for another monitor, and a click there deactivates the game.
   - Fix: in switch/borderless display modes, `ClipCursor(&monitorRect)` after `ApplyWindow` while exclusive and in
     front. `ClipCursor(nullptr)` in `OnActivate(false)`, when leaving exclusive mode, and in `TearDown`. Skip it for
     display=window.
7. **Other SetCooperativeLevel rules** (edge case).
   - XP (SetCooperativeLevel.c):
     - FPUSETUP|FPUPRESERVE → INVALIDPARAMS (45);
     - SETDEVICEWINDOW: refused with SETFOCUSWINDOW/CREATEDEVICEWINDOW, and NOFOCUSWINDOW without a focus window
       (266-271). It sets only the device window;
     - CREATEDEVICEWINDOW without SETFOCUSWINDOW needs hwnd NULL (66-67);
     - a WS_DLGFRAME-without-WS_BORDER window gets NOWINDOWCHANGES (84-87);
     - NORMAL with hwnd NULL clears the object's window, and SETFOCUSWINDOW with NULL clears the focus window (103, 176);
     - **the owner calling EXCLUSIVE again doesn't lose any surface** (234). On another window it fails with
       HWNDALREADYSET (FUN_7375e236:57-58);
     - entering exclusive mode loses the surfaces of **every** object on the device (FUN_73760648);
     - an exclusive call on a window whose earlier hook couldn't be removed → HWNDALREADYSET.
   - dd2d9 (ddraw.cpp:709-796) has none of these: `acquire` (745) also fires on a window change, and losing only
     touches this object.
   - Fix: add the FPU and SETDEVICEWINDOW checks. Make `acquire` mean `!Exclusive()` only, and return HWNDALREADYSET
     for a window change while exclusive. Let NULL hwnd clear `hwnd_` in NORMAL. Lose other objects' surfaces in
     `acquire` (a loop over `g_ddraws`). The DLGFRAME rule is optional.
8. **Leaving exclusive mode doesn't flip to the GDI surface** (edge case).
   - XP calls `FlipToGDISurface` before deactivating (SetCooperativeLevel.c:192).
   - dd2d9 doesn't, so the front buffer the game sees afterwards (and GetAttachedSurface order) can differ.
   - Fix: in the `leaving` branch, call `FlipToGDISurface()` before `RestoreDisplay()` (outside g_displayLock if Flip
     needs g_lock only; check the lock order).
9. **Window placement details on entering exclusive mode** (edge case, besides the deliberate frame-off-screen cover).
   - XP un-maximizes a maximized focus window (`ShowWindow(SW_RESTORE)`, FUN_7375d40c:68-71), sets a 1.5 s timer 0x4242
     on it, and calls SetForegroundWindow (`FUN_7375d557`).
   - dd2d9 covers the monitor and activates with SetWindowPos.
   - Fix: none needed unless a maximized-window game misbehaves. Mentioned for completeness.
10. **WaitForVerticalBlank flags and event** (edge case; pacing-visible for a game that passes 0).
    - XP: an event handle → UNSUPPORTED. Flags other than exactly 1 or 4 (0 included) → INVALIDPARAMS, returned at
      once (WaitForVerticalBlank.c:38-39).
    - dd2d9 (ddraw.cpp:1143-1151) waits and returns DD_OK.
    - Fix: `if (event || (flags & BLOCKBEGINEVENT)) return UNSUPPORTED; if (flags != 1 && flags != 4) return INVALIDPARAMS;`.
11. **SetDisplayMode validation** (edge case).
    - XP: `flags & ~1` → INVALIDPARAMS. A locked video-memory surface → SURFACEBUSY (SetDisplayMode.c:32-47). Bad sizes
      or depths reach ChangeDisplaySettings → DDERR_UNSUPPORTED.
    - dd2d9 (ddraw.cpp:800-801): no flag or busy check, and INVALIDPARAMS/INVALIDMODE for 0, huge or odd depths.
      Accepting modes the monitor lacks is deliberate.
    - Fix: add the flags and SURFACEBUSY (any surface with `lockCount` that `CanBeLost()`) checks. Return
      DDERR_UNSUPPORTED instead of INVALIDPARAMS/INVALIDMODE for impossible modes.
12. **RestoreDisplayMode check order** (edge case).
    - XP: another object exclusive → NOEXCLUSIVEMODE first, then SURFACEBUSY, and only then "nothing to restore → DD_OK"
      (RestoreDisplayMode.c:45-54).
    - dd2d9 (ddraw.cpp:930-931) returns DD_OK first.
    - Fix: swap the two checks and add SURFACEBUSY.
13. **Compact needs exclusive mode** (edge case).
    - XP: NOEXCLUSIVEMODE unless the object is the exclusive owner (Compact.c:19-28).
    - dd2d9 (wrappers.h:232): always DD_OK.
    - Fix: `return d->Exclusive() ? DD_OK : DDERR_NOEXCLUSIVEMODE;`.
14. **CreateClipper flags and reference** (edge case).
    - XP: flags ≠ 0 → INVALIDPARAMS. Through DD4/DD7 the clipper holds a DirectDraw reference (FUN_7373ce21:18, 50-55),
      which changes Release's return values.
    - dd2d9 (ddraw.cpp:1188): neither.
    - Fix: check flags. Through v4/v7, `AddRef(version)` and release it with the clipper, as `Palette::ddRef` does.
15. **CreatePalette flag validation** (edge case).
    - XP (CreatePalette.c:66-129), in this order:
      - unknown bits → INVALIDPARAMS;
      - NOCOOPERATIVELEVELSET;
      - PRIMARYSURFACE/PRIMARYSURFACELEFT/VSYNC → INVALIDPARAMS;
      - exactly one size flag;
      - ALPHA with 8BITENTRIES, 8BIT with 8BITENTRIES, or ALLOW256 without 8BIT → INVALIDPARAMS;
      - NULL entries → INVALIDPARAMS.
    - dd2d9 (ddraw.cpp:1172-1186): only "a size flag" and coop. NULL entries make an all-black palette.
    - Fix: add the checks in XP's order (flags before coop).
16. **CreateSurface caps rules missing in dd2d9** (edge case). XP's `FUN_73736a8e`/`FUN_73736cad` refuse these, which
    dd2d9 (ddraw.cpp:1453-1480) accepts:
    - PALETTE or VISIBLE requested (INVALIDCAPS);
    - OPTIMIZED, reserved bits 0x03000000, `caps2 & 3`, high `caps3` bits, `caps4` (INVALIDCAPS);
    - OFFSCREENPLAIN with TEXTURE/OVERLAY/BACKBUFFER;
    - PRIMARYSURFACE with OFFSCREENPLAIN/OVERLAY/BACKBUFFER, or with ZBUFFER without FLIP;
    - COMPLEX without TEXTURE/PRIMARYSURFACE/OVERLAY/OFFSCREENPLAIN/BACKBUFFER (e.g. COMPLEX|FLIP|3DDEVICE);
    - BACKBUFFER with FRONTBUFFER or ALPHA, or COMPLEX|FLIP|BACKBUFFER through DD1;
    - LOCAL/NONLOCALVIDMEM without VIDEOMEMORY, or with SYSTEMMEMORY;
    - managed with LOCAL/NONLOCAL, or with both manage flags;
    - HINT/OPAQUE combinations, DONOTPERSIST without managed;
    - **OWNDC without SYSTEMMEMORY (DDERR_UNSUPPORTED)**;
    - BACKBUFFERCOUNT together with MIPMAPCOUNT (INVALIDPARAMS);
    - **DDSCAPS_ALPHA and DDSD_ALPHABITDEPTH (INVALIDPARAMS)**;
    - **a primary with DDSD_WIDTH/HEIGHT/PIXELFORMAT (INVALIDPARAMS)** (dd2d9 ignores them, 1501-1502);
    - a non-power-of-two MIPMAP, or more levels than the size allows (INVALIDPARAMS; dd2d9 clamps, 1554, as its
      comment says);
    - DXT sizes not a multiple of 4;
    - a cube map through DD4, non-square, non-power-of-two, or with BACKBUFFERCOUNT; and a cube map without COMPLEX,
      which is DDERR_DDSCAPSCOMPLEXREQUIRED.
    And dd2d9 refuses, where XP accepts, COMPLEX|ZBUFFER or COMPLEX|ALPHA without FLIP/MIPMAP (1453, XP lines 31-37).
    BACKBUFFERCOUNT without COMPLEX on a cube is INVALIDCAPS on XP, not INVALIDPARAMS (1459; XP 40-46).
    - Fix: port `FUN_73736a8e`'s table as one function and call it where XP does (after the SYSTEMMEMORY+VIDEOMEMORY
      and client-memory checks). Keep the mip clamp only if a game needs it (log it).
17. **Z buffer descriptions** (edge case).
    - XP (FUN_73736cad:81-183):
      - v1/v2 need DDSD_ZBUFFERBITDEPTH or a Z pixel format;
      - v4/v7 refuse DDSD_ZBUFFERBITDEPTH and COMPLEX, and need a Z pixel format with a non-zero mask;
      - a depth below 8 is INVALIDPARAMS;
      - a non-Z surface with DDSD_ZBUFFERBITDEPTH or a Z/stencil pixel format is INVALIDPARAMS.
    - dd2d9 (ddraw.cpp:1519-1524): defaults to 16 bits without either, accepts ZBUFFERBITDEPTH on any version, makes a
      Z-format non-Z surface, and refuses depths other than 16/24/32 with INVALIDPIXELFORMAT (8 is valid on XP).
    - Fix: follow XP's per-structure-size rules. Keep INVALIDPIXELFORMAT for depths the HEL/driver can't do (see Not
      determined).
18. **Color keys in CreateSurface** (edge case).
    - XP validates all four keys with SetColorKey's checker before creating (DESTOVERLAY, DESTBLT, SRCOVERLAY, SRCBLT;
      FUN_73736cad:311-326), and sets them afterwards with SetColorKey (FUN_73737051:223-250). So overlay keys fail
      (NOCOLORKEYHW/NOOVERLAYHW), a Z buffer with a key fails (INVALIDSURFACETYPE), and a set failure releases the new
      surface.
    - dd2d9 (ddraw.cpp:1567-1573) refuses only ranges and stores overlay keys.
    - Fix: run `SetColorKey`'s own validation for each flagged key in XP's order, before allocating.
19. **Client memory (DDSD_LPSURFACE)** (edge case).
    - XP (FUN_73737051:43-111) checks client memory **before all other caps rules**:
      - SYSTEMMEMORY must be requested (else INVALIDCAPS);
      - COMPLEX, PRIMARYSURFACE, OVERLAY or VIDEOPORT → INVALIDCAPS;
      - `IsBadReadPtr` → INVALIDPARAMS;
      - **DD7 needs a DWORD-aligned pointer (DD4 doesn't)**;
      - through DD1/DD2 it fails with INVALIDPARAMS (no pitch) after the caps checks;
      - DXTn with a linear size is allowed (pitch → linear size on DD4 only).
    - dd2d9 (ddraw.cpp:1576-1585):
      - refuses only VIDEOMEMORY (adding SYSTEMMEMORY otherwise);
      - ignores LPSURFACE on a primary;
      - checks the version first;
      - refuses FourCC;
      - doesn't check alignment.
    - Fix: move these checks up, right after SYSTEMMEMORY+VIDEOMEMORY, in XP's order.
20. **CreateSurface entry order** (edge case).
    - XP through DD1/DD2: NOCOOPERATIVELEVELSET, then size, then CLASS_E_NOAGGREGATION (2_CreateSurface.c:47-62 then
      FUN_73736f21:17). Through DD4/DD7 the aggregation check is first.
    - dd2d9 (ddraw.cpp:1433) always checks the outer first.
    - Fix: for `ddVersion < 4`, check outer after the description size.
21. **DuplicateSurface** (edge case).
    - XP (DuplicateSurface.c):
      - a lost source → SURFACELOST first (25);
      - the root of an offscreen flip chain can be duplicated (27-32);
      - the duplicate's interface has **the source interface's version** (37);
      - **no DirectDraw reference** is taken.
    - dd2d9 (ddraw.cpp:1389-1411): no lost check, refuses chain roots (1394), uses the DirectDraw version, and takes a
      reference through v4/v7.
    - Fix: check `IsLost()` first, drop the `flipChain` test (duplicate the front alone), take the version from `src`'s
      interface, and don't AddRef the DirectDraw object.
22. **Display mode reports** (edge case).
    - XP (GetDisplayMode.c:55, FUN_73761f69:26):
      - `lPitch = width·bpp/8`, not rounded;
      - EnumDisplayModes lists modes in the driver's order;
      - one entry per refresh rate with DDEDM_REFRESHRATES;
      - Mode X entries carry DDSCAPS_MODEX and appear only with DDSCL_ALLOWMODEX;
      - `flags & ~3` → INVALIDPARAMS;
      - the filter must be the interface's size (0x6c for v1/v2, 0x7c for v4/v7).
    - dd2d9 (ddraw.cpp:979, 1019-1051): rounds the pitch to 4, sorts, one rate, no flag or filter-size check.
      Its virtual mode list is deliberate.
    - Fix: drop the `+3 & ~3` rounding (it only differs for odd widths at 8/24 bpp). Check flags and the filter size
      per version in the wrapper. Keep the sorted virtual list.
23. **EnumSurfaces validation and matching** (edge case).
    - XP:
      - without ALL a filter is required, and ALL with MATCH/NOMATCH or MATCH with NOMATCH is refused (INVALIDPARAMS);
      - unknown flags, LPSURFACE, and reserved caps in the filter are refused;
      - caps OPTIMIZED → DD_OK with nothing enumerated;
      - the filter must be the interface's size;
      - DOESEXIST walks interface pointers newest first, one callback per interface pointer;
      - matching compares only CAPS (exact), WIDTH, HEIGHT, BLT keys, BACKBUFFERCOUNT, MIPMAPCOUNT and PIXELFORMAT
        (only on surfaces with their own format) (FUN_737637e9);
      - CANBECREATED really creates (a primary too), ignores DDENUMRET_CANCEL, tries every display format with
        DDPF_RGB, and through v4/v7 writes into the caller's filter.
    - dd2d9 (ddraw.cpp:1271-1387):
      - accepts a NULL filter without ALL (enumerating everything);
      - requires the filter's flags to be a subset of the surface's (so a CKSRCBLT filter never matches an unkeyed
        surface, while XP compares against key 0);
      - compares PITCH/REFRESHRATE/ALPHABITDEPTH;
      - matches PIXELFORMAT on surfaces without their own format;
      - enumerates oldest first, once per surface;
      - honours CANCEL in CANBECREATED.
    - Fix: add the flag rules, then rewrite `DescMatches` as `FUN_737637e9` (use `ownFormat` for PIXELFORMAT, stored
      keys or zero for the keys). Reverse the order. The CANBECREATED differences can stay unless a game shows them.
24. **GetAvailableVidMem validation** (edge case).
    - XP: NULL caps → INVALIDPARAMS. Reserved caps → INVALIDCAPS. BACKBUFFER, COMPLEX, FLIP, FRONTBUFFER, PALETTE,
      SYSTEMMEMORY, VISIBLE, WRITEONLY or OWNDC → INVALIDPARAMS. Both outputs NULL → INVALIDPARAMS
      (GetAvailableVidMem.c:48-55).
    - dd2d9 (wrappers.h:266, ddraw.cpp:1153) ignores caps and accepts two NULLs.
    - Fix: pass the caps through and check them in XP's order.
25. **GetCaps sizes and GetDeviceIdentifier flags** (edge case).
    - XP:
      - GetCaps accepts only the DX1/3/5/6 sizes (0xac, 0x13c, 0x16c, 0x17c), checks both structures before writing,
        and gives the HEL `dwVidMemFree = 0` (GetCaps.c:39-48, 112);
      - GetDeviceIdentifier: unknown flags → INVALIDPARAMS (after zeroing the output).
    - dd2d9 (ddraw.cpp:1106-1113, 1160): any size 4…sizeof(DDCAPS), checked one structure at a time; HEL memory equal
      to the driver's; flags ignored.
    - Fix: validate both sizes against the four values first. Zero the HEL's free memory. In GetDeviceIdentifier, if
      `flags & ~1`, zero the structure and return INVALIDPARAMS.
26. **QueryInterface details** (edge case).
    - XP: a bad riid → INVALIDPARAMS before `*ppv` is touched, and a NULL ppv after that check. IID_IDDVideoPortContainer
      always gives an interface (QueryInterface.c:41-47, 165-169).
    - dd2d9 (ddraw.cpp:61-78) checks `out` first and reads riid unchecked. It has no video-port container (no video
      ports: deliberate, like overlays).
    - Fix: check `riid` (e.g. `IsBadReadPtr`) before `out`. Optional.
27. **Releasing the object** (edge case; partly deliberate).
    - XP: the last Release destroys every surface, palette and clipper the object made (their pointers become dead), and
      leaves the window topmost if the game never called SetCooperativeLevel(NORMAL) (Release.c:102-125).
    - dd2d9 keeps them usable (INVALIDOBJECT where needed), restores the window's style, and makes it non-topmost
      (ddraw.cpp:105-129).
    - Fix: none. dd2d9's way is safer, and games that use objects after this crash on XP. Noted so tests don't expect
      XP's dead pointers.
28. **Uninitialized objects and EvaluateMode** (edge case).
    - XP: a CoCreateInstance object's methods return DDERR_NOTINITIALIZED until Initialize, which fixes only the
      interface it was called on and validates the GUID (Initialize.c:34-58). EvaluateMode → INVALIDPARAMS.
    - dd2d9: every method works before Initialize, the GUID is ignored, and EvaluateMode → TESTFINISHED
      (wrappers.h:277).
    - Fix: EvaluateMode → `DDERR_INVALIDPARAMS`. For uninitialized objects, return DDERR_NOTINITIALIZED from the
      methods other than QI/AddRef/Release/Initialize while `!initialized` (per object is close enough).
29. **GetGDISurface and GetSurfaceFromDC lookups** (edge case).
    - XP:
      - GetGDISurface returns NOTFOUND when no chain member is the GDI surface, and through DD4/DD7 hands out a new
        interface pointer on each call when the primary's isn't of that version (GetGDISurface.c:72, 94, 105);
      - GetSurfaceFromDC finds DCs of any DirectDraw object in the process (FUN_73760741:21-30).
    - dd2d9: GetGDISurface falls back to the primary (ddraw.cpp:1206) and returns the shared interface.
      GetSurfaceFromDC searches only this object (1243).
    - Fix: search `g_ddraws` in GetSurfaceFromDC (as `FindSurfaceFromDC` already does). The GetGDISurface pointer
      identity can stay.
30. **GetScanLine during the blank** (edge case).
    - XP writes the driver's line even when it returns VERTICALBLANKINPROGRESS (GetScanLine.c:121-125).
    - dd2d9 leaves `*line` alone (ddraw.cpp:1132).
    - Fix: `*line = pos` before returning VERTICALBLANKINPROGRESS (a line ≥ height is what drivers report).
31. **The "ignore a re-entrant SetCooperativeLevel" rule** (edge case).
    - dd2d9 returns DD_OK without doing anything for a call made from the window procedure during SetCooperativeLevel
      (ddraw.cpp:699-702), "as in ddraw.dll".
    - XP's SetCooperativeLevel has no such guard: its critical section is re-entrant and the inner call runs.
    - Fix: keep it if a game needs it, but drop the "as in ddraw.dll" claim (see Not determined).

### Deliberate (noted, not fixes)

- Exclusive mode is a window covering the monitor with its frame off-screen, or a scaled window (display=window /
  borderless); XP moves the window itself over the device rectangle (FUN_7375d40c).
- Virtual display modes: any SetDisplayMode size is accepted and EnumDisplayModes offers a fixed list.
- No overlays or video ports (NOOVERLAYHW, no IDDVideoPortContainer), no FourCC codes reported.
- No Direct3D of its own: Direct3D IIDs go to D7VK through `D3DProxyQueryDDraw`, not to d3dim.
- The caps contents, the fixed 512 MB video memory, the emulated scan line and vertical-blank status.
- DDSCL_CREATEDEVICEWINDOW's window is made hidden (WS_POPUP without WS_VISIBLE): the picture goes to the focus
  window.
- Members of complex surfaces don't die with their root (surface-other.md). That is the reason for difference 2's workaround.

### Matches (verified)

- AddRef/Release returning per-interface counts, with over-release returning 0.
- The QueryInterface versions and AddRef.
- CreateSurface:
  - the description size per interface and v2 handing out v1 surfaces;
  - the FRONTBUFFER/BACKBUFFER rule by version (DD1 vs DD2+);
  - SYSTEMMEMORY+VIDEOMEMORY and the managed/WRITEONLY rules it has;
  - the NOCOOPERATIVELEVELSET rule (SETFOCUSWINDOW alone doesn't count);
  - the DirectDraw reference through v4/v7;
  - the default mip count (shorter side, longer through DD7).
- CreatePalette's v4/v7 reference.
- FlipToGDISurface.
- GetDisplayMode's flags and `dwSize` per interface.
- GetFourCCCodes, GetMonitorFrequency, GetVerticalBlankStatus (driver-dependent).
- SetCooperativeLevel's main validation (flags 1-6, 7), DirectDraw 1 mode retention, and non-topmost on leaving.
- SetDisplayMode's NOEXCLUSIVEMODE when another object is exclusive, and no cooperative level needed.
- StartModeTest.
- EnumSurfaces' CANBECREATED/DOESEXIST flag rule and per-version callback interfaces.
- EnumDisplayModes' `dwFlags` and callback CANCEL.

### Not determined

- ~~Why Type Wild works on XP~~ **Settled (0.7.15 traces):** the game releases its DirectDraw object properly; on XP
  it dies and exclusive mode is free. Behind D7VK it never dies: D7VK's `DirectDrawCreate`
  (d7vk src/ddraw/ddraw_main.cpp:115/156) wraps the pointer dd2d9 returns in a `Com<>`, which AddRefs it, and the
  original reference is never released. The takeover (difference 1) only compensates for that leak; dd2d9 alone can
  refuse as XP does. Likewise difference 2 (JJ2's "second primary"): alone (0.6.2/0.6.3 logs) JJ2 releases its
  primary before each mode change; behind D7VK, D7VK's per-surface `m_attachedSurfaces` caches (ddraw_surface.cpp,
  GetAttachedSurface) chain front→back1→back2→front, so the back-buffer references JJ2 leaves keep a reference on the
  old front. Both workarounds go once dd2d9 implements Direct3D itself (no D7VK).
- **How XP marks surfaces lost on its own mode changes** (SetDisplayMode, RestoreDisplayMode, leaving exclusive mode):
  `FUN_7373cc84` doesn't set `DDRAWISURF_INVALID` in the code read. The only setter found is `FUN_73760541`, reached
  from activation and primary creation. The kernel-side loss may be what games see. So whether leaving exclusive mode
  loses surfaces on XP (dd2d9: only after a background spell, ddraw.cpp:768) wasn't settled.
- The initial value of the object's "expected mode" (lcl+0x60) before any SetDisplayMode, which decides when
  TestCooperativeLevel first says WRONGMODE (difference 5).
- What the Uninit vtables hold in the IDirectDraw2/4/7-only slots (no `UninitX_GetAvailableVidMem` etc. is named).
- The driver width limit `FUN_7375b6a5` in CreateSurface, and the refresh-rate pick for an explicit rate
  (`FUN_7376198b` lost its return value).
- Whether an interface pointer whose count reached 0 stays in `dsList` (and so is still enumerated by EnumSurfaces).
- The meaning of DDSCL flag 0x20 (internal; refused with EXCLUSIVE, skips the window hook).
- Z depths the XP HEL accepts (dd2d9 allows 16/24/32; XP's validator allows any depth ≥ 8 and leaves the rest to the
  driver/HEL).
