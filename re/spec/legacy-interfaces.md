# IDirectDraw, IDirectDraw2, IDirectDrawSurface 1/2/3: where the old interfaces differ from v4/v7

Reference: ddraw.dll 5.3.2600.5512 (XP SP3). Decompilation in `$S/decomp-annotated/` (`$S` = the session
scratchpad). Line numbers are those of the plain-named file (`IDirectDraw2_X.c`, `FUN_xxxxxxxx_xxxxxxxx.c`), which
is identical to its `_<addr>.c` twin. `xp.dis` is `objdump -d` of the same DLL (`$S/dll/ddraw.dll`, SHA-256
3006f1c8…, matching `re/ddraw_xpsp3_symbols.json`). A bare method file name (`Flip.c`, `EnumAttachedSurfaces.c`)
means the `IDirectDraw7_`/`IDirectDrawSurface7_` one. dd2d9 references are to `src/` at the time of writing.

Scope: only behaviour that depends on the interface version (1/2 vs 4/7 for the object, 1/2/3 vs 4/7 for surfaces).
Most of it is already settled in the other specs; those items are listed with a pointer and not repeated:
`ddraw-object.md` (DO), `surface-other.md` (SO), `surface-lock-dc.md` (SL), `surface-blt.md` (SB),
`surface-flip-key-palette.md` (SF), `palette-clipper-exports.md` (PC).

## Conventions

- **The version is the vtable of the interface the method was called through** (`This->lpVtbl ==
  &vtbl_IDirectDraw…`). There is no per-object "version" for these rules. (A separate, object-level flag,
  `DDRAWILCL_DIRECTDRAW7` 0x20000, is set only by DirectDrawCreateEx (DirectDrawCreateEx.c:28,
  FUN_7373ad42:129) and is consulted by surface QI for Direct3D IIDs (SO difference 15), a Lock fast path
  (IDirectDrawSurface7_Lock.c:53) and a D3D palette hook (IDirectDrawSurface7_SetPalette.c:200). Those aren't
  interface-version rules and are out of scope.)
- Interfaces handed out are found or made per version: `FUN_73737e41(lcl, vtbl)` for the object
  (FUN_73737e41.c:9-25), `FUN_7373e5c8`/`FUN_7373e7ca` for surfaces (DO conventions). Each interface pointer
  (INT) has its own count; AddRef/Release return it (DO AddRef, SO AddRef). dd2d9 models the INT as one count
  slot per version (`DDraw::ifaceRef_`, ddraw.cpp:87-112; `Surface::refs_`, objects.h:141-143). Matches.
- **Surfaces made through IDirectDraw or IDirectDraw2 get the IDirectDrawSurface (v1) vtable; through
  IDirectDraw4 the v4 one; otherwise v7** (FUN_73737235:564-573; the same pattern in FUN_7373dd96:354-361,
  FUN_7373e9fe:449-456, FUN_73744537:531-538). `lpSurfMore->lpDD_int` is the object interface called
  (FUN_73737235:591).

---

## 1. Which methods are different code (vtable map)

From `InitInterfaceVtables_73731cd2.c` (the tables are filled at load time; their file bytes are zero). Every
slot not listed here points at the v7 implementation, which branches on `lpVtbl` where section 2 says so.

| Interface | Slot | Method | Implementation | Init line |
|---|---|---|---|---|
| IDirectDraw, IDirectDraw2 | 6 | CreateSurface | `IDirectDraw2_CreateSurface` (DDSURFACEDESC in) | 16, 136 |
| IDirectDraw, IDirectDraw2 | 8 | EnumDisplayModes | `IDirectDraw2_EnumDisplayModes` | 18, 138 |
| IDirectDraw, IDirectDraw2 | 9 | EnumSurfaces | `IDirectDraw2_EnumSurfaces` | 19, 139 |
| IDirectDraw | 21 | SetDisplayMode(w,h,bpp) | `IDirectDraw_SetDisplayMode` → v7 with rate 0, flags 0 | 31 |
| IDirectDraw2 | 21 | SetDisplayMode(w,h,bpp,rate,flags) | the v7 one | — |
| IDirectDraw2 | 23 | GetAvailableVidMem(DDSCAPS) | `IDirectDraw2_GetAvailableVidMem` (IDirectDraw has no slot 23) | 141 |
| IDirectDrawSurface/2/3 | 12 | GetAttachedSurface(DDSCAPS) | `IDirectDrawSurface3_GetAttachedSurface` | 224, 260, 299 |
| IDirectDrawSurface/2/3 | 14 | GetCaps(DDSCAPS) | `IDirectDrawSurface3_GetCaps` | 226, 262, 301 |
| IDirectDrawSurface/2/3 | 22 | GetSurfaceDesc(DDSURFACEDESC) | `IDirectDrawSurface3_GetSurfaceDesc` | 234, 271, 310 |
| IDirectDrawSurface/2/3 | 32 | Unlock(LPVOID) | `IDirectDrawSurface3_Unlock` | 244, 281, 320 |
| IDirectDrawSurface2+ | 36 | GetDDInterface | the v7 one (IDirectDrawSurface has 36 slots, no GetDDInterface) | 266, 305 |
| IDirectDrawSurface2+ | 37, 38 | PageLock, **PageUnlock** | **both slots point at the same function** (`IDirectDrawSurface7_PageLock`, 0x737616e9) | 285-286, 324-325 |
| IDirectDrawSurface3 | 39 | SetSurfaceDesc(DDSURFACEDESC) | `IDirectDrawSurface3_SetSurfaceDesc` | 326 |

dd2d9: one template per interface (`SurfaceWrap<V>`, `DDWrap<V>`, wrappers.h:23-212, 221-301); methods a version
lacks are non-virtual extras, so the vtable shapes match the SDK. PageUnlock calls `PageLock` (wrappers.h:168), as
XP does. **Matches.**

---

## 2. Version-dependent behaviour, per method

### 2.1 IDirectDraw / IDirectDraw2 (object)

**QueryInterface** (`IDirectDraw7_QueryInterface.c`, all versions): IID_IUnknown and IID_IDirectDraw → the v1 INT,
IID_IDirectDraw2/4/7 → that version's INT; This itself if it already is that version, else found or made by
`FUN_73737e41` (lines 64-87 for v1, 112-153 for the others, as numbered in DO). dd2d9 `DDraw::QueryInterface`
(ddraw.cpp:63-80). Matches. On an object from CoCreateInstance, This keeps its Uninit vtable for "its own" IID,
but **another IID gets an INT with the real vtable** on the still-uninitialized object (`pdVar10 = &vtbl_IDirectDraw…`
then `FUN_73737e41`, QueryInterface.c:66-76 and the IID_IDirectDraw2/4/7 branches). See DO difference 28 for the
Uninit state; dd2d9's per-object `initialized` (wrappers.h:232-235) is close enough.

**Initialize** (`IDirectDraw7_Initialize.c:38-53`): only the interface called is switched: Uninit → IDirectDraw,
UninitC → IDirectDraw2, UninitB → IDirectDraw4, UninitA → IDirectDraw7 (+ DIRECTDRAW7 flag). Settled in DO
(Initialize, difference 28).

**The Uninit tables' extra slots** (resolves the DO "Not determined" item). UninitC (IDirectDraw2's, 0x7376f8e0) has
24 slots; slots 0-22 are set at InitInterfaceVtables.c:53-75, **slot 23 (GetAvailableVidMem) is never written**.
UninitB (IDirectDraw4's, 0x7376f860): slot 23 never written; 24 GetSurfaceFromDC → `IDirectDrawClipper_Uninit_SetClipList`
(109), 25 RestoreAllSurfaces and 26 TestCooperativeLevel → `IDirectDraw_Uninit_Compact` (83-84), 27 GetDeviceIdentifier
→ `IDirectDrawClipper_Uninit_GetHWnd` (110). UninitA (IDirectDraw7's, 0x7376f7e0): the same for 24-27 (91-92,
128-129); **23, 28 (StartModeTest) and 29 (EvaluateMode) never written**. No instruction in xp.dis references those
addresses (the only writes to the tables are the absolute `mov`s of InitInterfaceVtables, e.g. xp.dis:1376, 1404,
1446), and the .data bytes are 0. So on XP, **IDirectDraw2::GetAvailableVidMem on an uninitialized object calls
through a NULL pointer** (the game's call jumps to address 0 before any ddraw code runs, so no ddraw SEH frame catches it). The other filled
Uninit slots return DDERR_NOTINITIALIZED (DO). dd2d9 returns DDERR_NOTINITIALIZED (wrappers.h:284). No fix wanted.

**CreateSurface** (`IDirectDraw2_CreateSurface.c`, v1/v2):
1. `!SETCOOPCALLED` → DDERR_NOCOOPERATIVELEVELSET (37-38).
2. `dwSize != 0x6c` → DDERR_INVALIDPARAMS (40, 51-52).
3. No current mode in the table and no SYSTEMMEMORY → DDERR_UNSUPPORTEDMODE (41, 49).
4. `FUN_7375baac`: the 0x6c bytes are copied into a zeroed 0x7c buffer **whose `dwSize` stays 0x6c**; caps2-4 and
   `dwTextureStage` are 0 (FUN_7375baac.c:9-17). Then `FUN_73736f21(…)` (44): CLASS_E_NOAGGREGATION only now (DO 20).
5. Version rules inside the shared code:
   - DD1/DD2: DDSD_PITCH and DDSD_LINEARSIZE are dropped (FUN_73737051:34-36), so client memory always fails on the
     missing pitch (DO 19).
   - **Anything but DD1**: DDSCAPS_FRONTBUFFER or DDSCAPS_BACKBUFFER requested → INVALIDCAPS (FUN_73736a8e:29).
     DD1 may ask for them.
   - Z buffer rules follow the **structure size** (0x6c: DDSD_ZBUFFERBITDEPTH or a Z pixel format; 0x7c: pixel format
     only, no COMPLEX), FUN_73736cad:81-183 (DO).
   - Default mip count from the shorter side for DD1/DD2/DD4 (FUN_73737235:106-107); cube maps only through DD7
     (FUN_73736cad:327-332); DD7-only stereo/WHQL bits are not legacy concerns.
   - The new surface's vtable: v1 for DD1/DD2 (FUN_73737235:564-573). No DirectDraw reference through DD1/DD2
     (FUN_73736f21:59-64).
- dd2d9 (`CreateSurfaceImpl`, ddraw.cpp:2006-2021; `CheckSurfaceDesc` 1892-1894, 1947-1963; `CheckCaps` 1852; mip
  count 2108; vtable 2007; reference 2225): **matches** every item above (UNSUPPORTEDMODE can't arise: the mode list is
  virtual).

**EnumDisplayModes** (`IDirectDraw2_EnumDisplayModes.c`, v1/v2): a non-NULL filter must be 0x6c (29-30); it is copied
into a 0x7c buffer with `dwSize = 0x7c` and caps2/3/4 + `dwTextureStage` zeroed (32-44), then the v7 code runs
(46) with all its checks (flags, callback). Each mode is reported in a 0x6c structure through DD1/DD2
(FUN_73761f69:21-24); STEREOSURFACELEFT only through DD7 (38-39). dd2d9: wrappers.h:251-256 (size check first, then
`descVersion` 1 for v1/v2) and ddraw.cpp:1353-1385. **Matches.**

**EnumSurfaces** (`IDirectDraw2_EnumSurfaces.c`, v1/v2): filter `dwSize != 0x6c` → INVALIDPARAMS (70-73); otherwise
copied into a **local** zeroed 0x7c structure with `dwSize = 0x7c` (74-88) and the v7 code runs (95). Consequences:
- The filter's caps2-4 are 0, so CAPS matching (all four words equal, DO §EnumSurfaces 8) compares against 0.
- **CANBECREATED's write-back into the filter (DO §EnumSurfaces 9) lands in the local copy**: the caller's
  DDSURFACEDESC is never modified through DD1/DD2.
- DOESEXIST: each matching **interface pointer** gets a QI for IID_IDirectDrawSurface (v1)
  (IDirectDraw7_EnumSurfaces.c:155-160) and the callback. Because `dsList` holds every INT, a surface that
  the game QI'd to IDirectDrawSurface2/3 is reported once per INT, each time with the **same** v1 pointer (DO 23;
  whether INTs whose count fell to 0 stay listed is DO "Not determined").
- **The description is filled as a 0x7c structure and only then given `dwSize = 0x6c`** (`FUN_73734fe9` zeroes 0x7c and
  sets 0x7c before `FUN_73733184`, FUN_73734fe9.c:9-14; EnumSurfaces.c:155, 160 for DOESEXIST and 389-393 for
  CANBECREATED). See 2.3 for what that changes.
- dd2d9: wrappers.h:257-262 (size first), ddraw.cpp:1672-1780 with `version = SV = 1` for DD1/DD2 (wrappers.h:216-217).
  Caller's filter untouched (match). See differences 2 and 4.

**SetDisplayMode**: IDirectDraw's slot calls the v7 code with rate 0 and flags 0 (IDirectDraw_SetDisplayMode.c:7);
IDirectDraw2 has the full v7 signature (flags `& ~DDSDM_STANDARDVGAMODE` → INVALIDPARAMS, DO). dd2d9: wrappers.h:279-280,
ddraw.cpp:1094. **Matches.**

**GetAvailableVidMem** (`IDirectDraw2_GetAvailableVidMem.c`, v2 only):
1. Device without hardware (`gbl->dwFlags & 0x100`) → DDERR_NODIRECTDRAWHW, **before** the caps pointer is read (36, 43-46).
2. `local_38 = {*caps, 0, 0, 0}`: only `dwCaps` is read; caps2-4 are 0 (21-24, 37). A NULL caps faults → SEH →
   DDERR_INVALIDPARAMS.
3. The v7 code runs (39) with its checks (DO §GetAvailableVidMem).
dd2d9: wrappers.h:283-288 (`ToCaps2`, wrappers.h:18) and ddraw.cpp:1488-1499. No hardware-less device exists there.
**Matches.**

**GetDisplayMode** (`IDirectDraw7_GetDisplayMode.c`, all versions): 0x6c or 0x7c accepted on any interface, `dwSize`
then set to 0x6c through DD1/DD2 (40-46); DD7-only stereo bit (57-62). dd2d9 ddraw.cpp:1302-1323. **Matches** (DO).

**GetCaps** (`IDirectDraw7_GetCaps.c`): the DDCAPS sizes 0xac/0x13c/0x16c/0x17c are accepted on **every** interface
version; nothing depends on the vtable. dd2d9 ddraw.cpp:1389-1392. **Matches.**

**GetGDISurface** (`IDirectDraw7_GetGDISurface.c:31-38, 53-60`): through DD1/DD2 the interface pointer found is handed
out as it is (the primary's own INT, or the chain link's); through DD4/DD7 a v4/v7 one, made anew when needed (DO).
dd2d9 hands out `Iface(SV)` (ddraw.cpp:1573-1582). Same for every normal case; see difference 7.

**GetSurfaceFromDC** (`IDirectDraw7_GetSurfaceFromDC.c:9-17`, v4/v7 only): **through IDirectDraw4 the result is an
IDirectDrawSurface (v1)**, through IDirectDraw7 a v7. dd2d9 wrappers.h:291-294. **Matches.**

**DuplicateSurface** (`IDirectDraw7_DuplicateSurface.c`): the duplicate's INT gets **the source pointer's vtable**
(line 37 as numbered in DO; `FUN_7373e7ca(lcl, *param_2)`), and its `lpDD_int` is the object interface called
(`*(lpSurfMore + 0x14) = This`, plain file line 36), so its GetDDInterface returns that. dd2d9 ddraw.cpp:1791-1804.
**Matches.**

**CreateClipper / CreatePalette**: no DirectDraw reference through DD1/DD2 (FUN_7373ce21:50-55,
IDirectDraw7_CreatePalette.c:208-212). **A clipper made through IDirectDraw (v1 only) takes any RGNDATA header in
SetClipList** (IDirectDrawClipper_SetClipList.c:37-41). dd2d9 ddraw.cpp:1536, 1551-1556, surface.cpp:1699. **Matches**
(PC).

**SetCooperativeLevel** (`IDirectDraw7_SetCooperativeLevel.c:81-83`): a call through IDirectDraw sets
`DDRAWILCL_V1SCLBEHAVIOUR` (0x80) for good, once validation (45-78) has passed. It is read in two places only:
Release (IDirectDraw7_Release.c:109-118) and the deactivation `FUN_7375aa94:45` (no mode restore). dd2d9
`ddraw1Coop_` (ddraw.cpp:998, 1021, 1029). Matches but for difference 6.

### 2.2 IDirectDrawSurface / 2 / 3

**Surface methods handing out interfaces** (v1-3 → IDirectDrawSurface; v4 → v4; v7 → v7):
GetAttachedSurface (`IDirectDrawSurface3_GetAttachedSurface.c:37`, QI with IID_IDirectDrawSurface at 0x73734f44),
EnumAttachedSurfaces (`IDirectDrawSurface7_EnumAttachedSurfaces.c:51-62`), EnumOverlayZOrders
(`IDirectDrawSurface7_EnumOverlayZOrders.c:60-73, 86-99`). Settled in SO; still open in dd2d9 (difference 3).

**GetAttachedSurface** (`IDirectDrawSurface3_GetAttachedSurface.c`, v1-3):
1. Lost → DDERR_SURFACELOST, **before the caps are read and without touching `*out`** (34, 41-44).
2. `caps = {lpDDSCaps->dwCaps, 0, 0, 0}` (20-23, 35); a NULL caps faults → SEH → INVALIDPARAMS. **No reserved-bits check**
   (v4/v7 have it, SO 8).
3. `FUN_73735269(&IID_IDirectDrawSurface, This, caps, out)` (37): `*out = NULL`, newest-first search, QI (SO).
dd2d9 wrappers.h:91-102, surface.cpp:702-717 (`version >= 4` for the reserved bits, 704). Matches except difference 3
and the order of difference 5.

**GetCaps** (`IDirectDrawSurface3_GetCaps.c`): the v7 code fills a local DDSCAPS2 (17) and only `dwCaps` is copied out
(18-19). A NULL pointer faults in the copy → DDERR_INVALIDPARAMS (handler at xp.dis 0x737611d8-0x737611df).
dd2d9 wrappers.h:108-114. **Matches.**

**GetSurfaceDesc** (`IDirectDrawSurface3_GetSurfaceDesc.c`): `dwSize` must be 0x6c (19, 25-28); `FUN_737637be` zeroes
0x6c bytes, sets 0x6c and calls `FUN_73733184` (FUN_737637be.c:8-14), which **sees 0x6c**: no caps2-4, no texture
stage/FVF, and a Z buffer gets DDSD_ZBUFFERBITDEPTH + `dwZBufferBitDepth` instead of DDSD_PIXELFORMAT
(FUN_73733184.c:14-30, 95-111). dd2d9 wrappers.h:127-135 + `DescOut` (util.cpp:616-644). **Matches.**

**SetSurfaceDesc** (`IDirectDrawSurface3_SetSurfaceDesc.c`, v3 only): `flags != 0` or `dwSize != 0x6c` →
INVALIDPARAMS (20, 31-32); otherwise the 0x6c bytes go into a zeroed 0x7c buffer, `dwSize = 0x7c` (16-28), and the v7
code runs (29), so caps2-4 are 0. dd2d9 wrappers.h:170-176 (size per version) + `DescIn` (util.cpp:590-614).
**Matches** (SO 12's size bullet is fixed).

**Unlock** (`IDirectDrawSurface3_Unlock.c:17`, v1-3): `FUN_73733308(This, lpSurfaceData, NULL)`; the pointer must match
the one a rect lock returned (SL §2). GetDC + Unlock: DDERR_GENERIC only on v3/v4/v7, **v1/v2 succeed**
(FUN_73733308:28-32). dd2d9 surface.cpp:1127-1160 (`version >= 3` at 1140, `version <= 3` pointer match at 1157).
**Matches.**

**ReleaseDC** (`IDirectDrawSurface7_ReleaseDC.c:41-42`): v1/v2 accept a wrong or NULL hDC (SL §4). dd2d9
surface.cpp:850-853. **Matches.**

**Restore** (`IDirectDrawSurface7_Restore.c:95-99`): while a mode change is in progress (`gbl->dwFlags & 0x400000`), v1/v2
go on (debug string only) where v3+ return DDERR_WRONGMODE (SL §6 row 8). dd2d9 changes modes synchronously under its
lock (no such window, surface.cpp:421-433): not applicable.

**Blt** (`IDirectDrawSurface7_Blt.c`): v7-only WAIT/DONOTWAIT rule (297-301); **with DDBLT_KEYSRC and a v1/v2 source
interface, the key is taken from the destination's CKDestBlt** (617-625, `local_234` = `param_3`, the source; also
FUN_73747f2e:113-116, FUN_737487db:1154-1156) (SB). dd2d9 wrappers.h:51-56 (`srcOld` by pointer identity), blit.cpp:235.
**Matches.** BltFast: v7-only WAIT/DONOTWAIT (BltFast.c:117-121); blit.cpp:442. **Matches.**

**Flip / Lock**: through v1-4 the flags are left alone; only v7 adds WAIT unless DONOTWAIT (Flip.c:102-111,
Lock.c:68-77) (SF, SL). Without WAIT, a driver's DDERR_WASSTILLDRAWING is returned to the caller (Flip.c:301-307,
FUN_737336a9:65-70, 280). dd2d9 never reports WASSTILLDRAWING, so the version doesn't matter there (see Notes).

**SetColorKey / SetPalette**: DDERR_NOTONMIPMAPSUBLEVEL only through v7 (SetColorKey.c:80-86, SetPalette.c:68-74) (SF).
dd2d9 surface.cpp:783, 881. **Matches.**

**AddAttachedSurface** (`IDirectDrawSurface7_AddAttachedSurface.c:74-77`): through v7, CANNOTATTACH unless exactly one of
the two is a Z buffer; v1-4 have no such rule (SO). dd2d9 surface.cpp:474. Matches; **but see difference 1 for which
interface pointer the attachment belongs to.**

**DeleteAttachedSurface / attachment keying** (FUN_7373530c, FUN_7373545a): the attach list belongs to This's **LCL**
(FUN_7373530c.c:10, 30; FUN_7373545a.c:31-36), and each entry is **the attachee interface pointer that was passed**:
`FUN_7373530c(This, attachee, 0)` AddRefs that INT (20) and stores it (26); AddAttachedSurface's duplicate check compares
it (AddAttachedSurface.c:101-107); DeleteAttachedSurface finds the entry by it (FUN_7373545a.c:35-36; not found →
DDERR_SURFACENOTATTACHED, 124). **The version of This plays no part.** See difference 1.

**GetDDInterface** (v2+): `*out = lpSurfMore->lpDD_int`, AddRef'd (GetDDInterface.c:20-22): for a surface made through
IDirectDraw2 that is the IDirectDraw2 pointer, although the surface is a v1 one. dd2d9 surface.cpp:1187-1194
(`ddVersion`). **Matches.**

**PageLock / PageUnlock** (v2+): one function for both (§1); flags ≠ 0 → INVALIDPARAMS, lost → SURFACELOST, else DD_OK
(SL §7). dd2d9 wrappers.h:167-168, surface.cpp:1181-1185. **Matches.**

**QueryInterface / AddRef / Release / Initialize / IsLost / the rest**: one function for every version (SO).
**Matches.**

### 2.3 How descriptions handed to old-interface callbacks are built

Two different paths make a DDSURFACEDESC (0x6c) for an old interface:

| Path | Fill | Z buffer | Other |
|---|---|---|---|
| GetSurfaceDesc v1-3 (`FUN_737637be`), Lock with a 0x6c structure | `FUN_73733184` with `dwSize = 0x6c` | **DDSD_ZBUFFERBITDEPTH**, `dwZBufferBitDepth` set, no DDSD_PIXELFORMAT (FUN_73733184.c:103-107) | no stage/FVF |
| EnumAttachedSurfaces v1-3 (EnumAttachedSurfaces.c:49-53), EnumOverlayZOrders v1-3 (EnumOverlayZOrders.c:59-63, 85-89), EnumSurfaces through DD1/DD2 (EnumSurfaces.c:155-160, 389-393) | `FUN_73734fe9`: filled **as 0x7c** (FUN_73734fe9.c:13-14), then `dwSize = 0x6c` | **DDSD_PIXELFORMAT with the Z format, no DDSD_ZBUFFERBITDEPTH, `dwZBufferBitDepth` (offset 0x18) left 0** unless the surface is a mipmap (FUN_73733184.c:46-49, 108-110) | DDSD_TEXTURESTAGE (flags reset to CAPS\|TEXTURESTAGE) and DDSD_FVF can appear in `dwFlags` for surfaces that have them (14-29); their fields lie past 0x6c, in ddraw's own buffer |

dd2d9 builds every 0x6c description with `DescOut`, which always uses the first form (util.cpp:639-643). See
difference 2.

---

## Differences from dd2d9

Severity: **game-visible** means a plausible game call sequence behaves differently. **Edge case** means it only shows up
with invalid parameters, unusual sequences or exact-return-code/pointer checks. None of the differences found here is
clearly game-visible; 1 is the most plausible for DirectX 3/5 games, which mix v1 pointers with QI'd v2/v3 ones.

1. **Attachments belong to This's interface version instead of the attachee pointer passed** (edge case, plausible in
   DirectX 5 games that mix pointer versions).
   - XP: AddAttachedSurface AddRefs and records **the attachee interface pointer it was given**, on This's LCL; the duplicate
     check and DeleteAttachedSurface match that pointer, whatever interface This is (FUN_7373530c.c:20-30,
     AddAttachedSurface.c:101-107, 193, FUN_7373545a.c:31-36, 124).
   - dd2d9: the wrappers pass only This's version (wrappers.h:41, 78). `AddAttachedSurface` takes `slot = SlotFor(version)`
     (surface.cpp:479) for the AddRef (515), the record (517) and flip rings (514); `DetachOne` requires the same slot
     (621, 644); the duplicate check turns "same surface, other slot" into CANNOTATTACH (482-485). So:
     - `back3->AddAttachedSurface((LPDIRECTDRAWSURFACE3)z1)` (z1 a v1 pointer): XP counts the reference on z1's v1
       interface, dd2d9 on z's v3 slot. `z1->Release()` then returns 0 on dd2d9 and 1 on XP (a `while (Release());` loop
       stops early on dd2d9: the Z buffer is kept alive, harmless);
     - Add through `back3`, Delete through `back1` (same z1 pointer): XP DD_OK; dd2d9 DDERR_SURFACENOTATTACHED, and the Z
       buffer stays attached, so attaching a new one later fails with CANNOTATTACH (one of each kind);
     - attaching the same pointer again through another version of This: XP SURFACEALREADYATTACHED, dd2d9
       CANNOTATTACH; the same surface through a **different** pointer: XP goes on to the later checks (it can even
       succeed, e.g. an offscreen plain on a texture, AddAttachedSurface.c:114-171), dd2d9 CANNOTATTACH.
   - Fix: in `SurfaceWrap::AddAttachedSurface`/`DeleteAttachedSurface`, find the attachee's version by pointer identity
     (as the Blt wrapper does for `srcOld`, wrappers.h:54: `for v in {1,2,3,4,7} if (a == S(a)->Iface(v))`) and pass
     `SlotFor(thatVersion)` as the slot for the AddRef, the record, `AttachForFlip` and `DetachOne`. Make the duplicate check
     "same surface and same slot → SURFACEALREADYATTACHED" and let other pairs fall through to the size/kind/flip checks.

2. **Z buffers in old-interface enumeration callbacks are described by depth instead of by pixel format** (edge case).
   - XP: EnumAttachedSurfaces through v1-3 and EnumSurfaces through DD1/DD2 fill the description as a DDSURFACEDESC2 and
     then set `dwSize = 0x6c` (FUN_73734fe9.c:13-14; EnumAttachedSurfaces.c:49-53; EnumSurfaces.c:155-160, 389-393). A Z
     buffer is reported with **DDSD_PIXELFORMAT** (its Z format) and **without DDSD_ZBUFFERBITDEPTH; `dwZBufferBitDepth`
     is 0** (FUN_73733184.c:95-111). GetSurfaceDesc through v1-3 does give DDSD_ZBUFFERBITDEPTH (2.3).
   - dd2d9: the enumerations build their 0x6c structure with `DescOut` (surface.cpp:687-688; ddraw.cpp:1697-1698, 1729-1730
     feeds CreateSurface, not the callback), which converts a Z format into DDSD_ZBUFFERBITDEPTH (util.cpp:639-643).
   - Fix: give `DescOut` a mode that copies the first 0x6c bytes of the DDSURFACEDESC2 form unchanged (only `dwSize` and
     `ddsCaps` beyond `dwCaps` differ) and use it in `Surface::EnumAttachedSurfaces` for v < 4 and in `DDraw::EnumSurfaces`'s
     `call` for version < 4. Keep the depth form for GetSurfaceDesc and Lock.

3. **IDirectDrawSurface2/3 GetAttachedSurface and EnumAttachedSurfaces hand out v2/v3 interfaces** (edge case; SO 10,
   still open).
   - XP: the IDirectDrawSurface (v1) interface, AddRef'd on its own count (IDirectDrawSurface3_GetAttachedSurface.c:37,
     EnumAttachedSurfaces.c:51-55).
   - dd2d9: `Iface(V)` and `SlotFor(V)` (wrappers.h:98; surface.cpp:677, 689, 711).
   - Visible only through pointer identity and the counts Release returns. On XP a v2/v3-only method called through
     the returned pointer reads past the 36-slot v1 vtable (slot 36 is `DAT_7376f190`, a data pointer), so no game working
     on XP does that; dd2d9's v2/v3 pointer is safe for it.
   - Fix (optional): `Iface(V >= 4 ? V : 1)` and `SlotFor(V >= 4 ? V : 1)` in those three places. The `I**` out type
     stays as declared (the vtable prefix is shared).

4. **EnumSurfaces(CANBECREATED) through IDirectDraw2 tries creation under IDirectDraw 1 rules** (edge case).
   - XP: the test creation `FUN_73737051` runs with the interface called (EnumSurfaces.c, CANBECREATED branch, DO §9), so
     through IDirectDraw2 a filter with DDSCAPS_FRONTBUFFER/BACKBUFFER can't be created (FUN_73736a8e:29).
   - dd2d9: `DDWrap<2>` passes `SV = 1` (wrappers.h:217, 261), and `tryCreate` calls `CreateSurfaceImpl(&in, …, version)`
     with it (ddraw.cpp:1732), so the DD1 rules apply (CheckCaps, ddraw.cpp:1852) and such a filter is reported as creatable.
   - Fix: pass both the surface version and the DirectDraw version to `DDraw::EnumSurfaces` (e.g. `V` and `T::SV`), and
     give `CreateSurfaceImpl` the DirectDraw version.

5. **GetAttachedSurface v1-3: order of the lost check and `*out`** (edge case).
   - XP: lost → DDERR_SURFACELOST first, `*out` untouched; the caps are read afterwards (NULL → INVALIDPARAMS);
     `*out = NULL` only inside the search (IDirectDrawSurface3_GetAttachedSurface.c:34-44; FUN_73735269:17). v4/v7 likewise
     leave `*out` alone on SURFACELOST/INVALIDCAPS (SO §GetAttachedSurface).
   - dd2d9: NULL `caps`/`out` → INVALIDPARAMS before the lost check, and `*out` is written on every failure
     (wrappers.h:92, 98).
   - Fix: check lost first (move `IsLost()` ahead of the NULL check for `caps`), and write `*out` only when the search ran.
     Optional.

6. **IDirectDraw's V1SCLBEHAVIOUR is set even when SetCooperativeLevel later fails with HWNDALREADYSET** (edge case).
   - XP sets the flag right after validation (SetCooperativeLevel.c:81-83), before the window hook can fail with
     HWNDALREADYSET (FUN_7375e236:57-58).
   - dd2d9 returns HWNDALREADYSET (ddraw.cpp:1007) before setting `ddraw1Coop_` (1021).
   - Fix: set `ddraw1Coop_` for `version == 1` just before the HWNDALREADYSET check. Optional.

7. **GetGDISurface through IDirectDraw/IDirectDraw2 always hands out a v1 pointer** (edge case, cross-version only).
   - XP hands out the primary's own interface pointer (or the chain link's) unchanged through DD1/DD2
     (GetGDISurface.c:31-38, 53-60): if the game made the primary through IDirectDraw4/7 of the same object, that is a
     v4/v7 pointer.
   - dd2d9 `Iface(1)` (ddraw.cpp:1579-1580).
   - Fix: none needed (a game using the result as IDirectDrawSurface works either way).

### Notes (no fix)

- **DDERR_WASSTILLDRAWING from Flip/Lock/Blt without WAIT** is possible on XP through v1-4 (the driver's answer is passed
  on, Flip.c:301-307, FUN_737336a9:65-70); v7 waits by default. dd2d9 completes these at once and never returns it, which
  is what a v7 caller sees on XP and what a v1-4 caller's retry loop handles.
- **Uninitialized objects**: IDirectDraw2::GetAvailableVidMem (and IDirectDraw4/7's slot 23, IDirectDraw7's 28/29) are
  NULL in XP's Uninit tables (crash); QI from an uninitialized object to another version returns a real-vtable INT
  (section 2.1). dd2d9 answers DDERR_NOTINITIALIZED per object (wrappers.h:232-235). Safer; keep.
- Restore's v1/v2 exception during a mode change has no counterpart in dd2d9 (no "change in progress" state).

### Matches (verified)

- The vtable shapes, including PageUnlock = PageLock and no GetAvailableVidMem on IDirectDraw / no GetDDInterface,
  PageLock, PageUnlock on IDirectDrawSurface.
- Description sizes per version: CreateSurface, EnumSurfaces and EnumDisplayModes filters, GetSurfaceDesc, SetSurfaceDesc
  (v3 0x6c only); GetDisplayMode and Lock accepting either size; GetDisplayMode's `dwSize` per interface; mode
  descriptions 0x6c through DD1/DD2.
- DDSCAPS → DDSCAPS2 conversions (GetCaps, GetAttachedSurface, GetAvailableVidMem: only `dwCaps`, rest 0).
- Interfaces handed out: v1 surfaces from IDirectDraw/IDirectDraw2 CreateSurface and EnumSurfaces, and from
  IDirectDraw4 GetSurfaceFromDC; the source pointer's version from DuplicateSurface; GetDDInterface's creating interface.
- DirectDraw references only through IDirectDraw4/7 (surfaces, palettes, clippers); the v1-only SetClipList leniency.
- IDirectDraw's SetDisplayMode (rate 0, flags 0) and V1SCLBEHAVIOUR's effect on leaving exclusive mode and on Release.
- CreateSurface through DD1/DD2: check order, pitch dropping, FRONT/BACKBUFFER allowed only through DD1, Z rules by
  structure size, mip count from the shorter side, cube maps only through DD7.
- Surface rules by version: Unlock(LPVOID) pointer matching and the GetDC+Unlock exception on v1/v2; ReleaseDC's
  wrong-DC leniency on v1/v2; Blt's v1/v2 source key rule; v7-only WAIT/DONOTWAIT checks, mip-sublevel refusals and
  the Z-only AddAttachedSurface rule; GetAttachedSurface's reserved-caps check only on v4/v7.
- QueryInterface/AddRef/Release per interface for both objects.

### Not determined

- Whether interface pointers whose count fell to 0 stay in `dsList`; it decides how often EnumSurfaces(DOESEXIST) through
  DD1/DD2 reports a surface that the game QI'd to v2/v3 and released again (section 2.1, DO "Not determined").
- What happens on XP after QI from an uninitialized object to another version and calls through that real-vtable
  pointer (`lpGbl` is NULL; each method would fault or misbehave differently). Not traced.
- Whether any shipped game passes a pointer of one surface version to another version's AddAttachedSurface or
  DeleteAttachedSurface (difference 1) or reads `dwZBufferBitDepth` from an enumeration callback (difference 2). A
  trace of DirectX 5-era titles would settle how much these matter.
