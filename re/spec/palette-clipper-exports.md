# IDirectDrawPalette, IDirectDrawClipper and the ddraw.dll exports

Reference: ddraw.dll 5.3.2600.5512 (XP SP3, the Swedish build in `$S/dll/ddraw.dll`). Decompilation in
`$S/decomp-annotated/` (`$S` = the session scratchpad). Line numbers are those of the plain-named
file (`IDirectDrawPalette_X.c`), which is identical to its `_<addr>.c` twin (checked with `cmp`). Helpers
are cited as `FUN_<addr>_<addr>.c`. Where Ghidra lost a return value, the code was read from the
disassembly (`objdump -d`), cited by address.
dd2d9 references are to `src/` at the time of writing.

## Conventions used in XP's code (they apply to every method below)

- Same as surface-other.md: no pointer validation. Methods run inside an SEH frame and a fault returns
  `DDERR_INVALIDPARAMS` (0x80070057) **at the point of the first dereference**. Explicit checks are only
  `IsBadReadPtr(riid)` (QI), `IsBadCodePtr(callback)` (enumeration) and `IsBadWritePtr(This)` + vtable (Release).
  **Exception: the exports `DirectDrawCreate`, `DirectDrawCreateEx` and `DirectDrawCreateClipper` have no SEH
  frame** (prologue at 0x7375e921 is a plain `push ebp`). A bad pointer there is a real access violation, unless
  the helper checks it (see below).
- Every method takes the global DirectDraw lock (`EnterCriticalSection(DAT_7376f190)`, recursion counter
  `DAT_7376f000`). This is the same lock as AcquireDDThreadLock.
- Palette structures: INT `{vtbl, lpLcl, lpLink, dwIntRefCnt}`. The LCL (0x28 bytes) is followed by its GBL in
  the same allocation. GBL `dwFlags` uses `DDRAWIPAL_*`: 256=0x1, 16=0x2, INHEL=0x80, DIRTY=0x100,
  ALLOW256=0x200, 4=0x400, 2=0x800, STORED_8INDEX=0x1000, ALPHA=0x2000, EXCLUSIVE=0x40.
  Entry count N = 2/4/16/256. Entry size is 1 byte with STORED_8INDEX (DDPCAPS_8BITENTRIES), else 4.
- Clipper structures (missing from `types/ddrawi_flat.h`; reconstructed from FUN_7373ce21 and the DDK
  `ddrawi.h`): INT `{vtbl, lpLcl, lpLink, dwIntRefCnt}`. LCL at +0 of a 0x3c block: `+8 lpDD_lcl`,
  `+0xc dwLocalRefCnt`, `+0x14 lpDD_int`, `+0x1c` owner interface (AddRef'd). GBL at +0x20: `+0 dwRefCnt`,
  `+4 dwFlags` (2 = DDRAWICLIP_ISINITIALIZED), `+8 lpDD (GBL)`, `+0xc dwProcessId`, `+0x14 hWnd`,
  `+0x18 lpStaticClipList`. There are two vtables: the normal one and an "Uninit" one (see Initialize).
- Driver callbacks. Palette and DD callbacks go to the HAL (`lpDDCB+0xa0` CreatePalette, `+0xfc` SetEntries,
  `+0xf8` DestroyPalette) when the driver supplied them, else to the HEL (`+0x120`, `+0x17c`, `+0x178`).
  The static HEL tables are at 0x73770018 (DD: CreatePalette = FUN_7375231c), 0x737700d8 (palette:
  DestroyPalette = FUN_73752394, SetEntries = FUN_7375205b) and 0x737700e8 (surface: SetPalette =
  FUN_73751e14 at 0x73770124). **It is assumed that on XP the HEL handles palettes**: NT display drivers have no
  palette entry points that ddraw.dll can reach (see Not determined).

---

## IDirectDrawPalette

### QueryInterface — `IDirectDrawPalette_QueryInterface.c`

1. `IsBadReadPtr(riid, 16)` → DDERR_INVALIDPARAMS (lines 23-28). **`*ppv` is not cleared first.**
2. riid is IID_IUnknown (0x73733298) or IID_IDirectDrawPalette (0x7374b838, 6C14DB84-…) → `*ppv = This`,
   AddRef, S_OK (lines 56-69). There is only one palette vtable, so This is always returned. A NULL ppv faults
   → INVALIDPARAMS.
3. **Any other IID → `0x80004005` E_FAIL (DDERR_GENERIC), with `*ppv` untouched** (line 52; confirmed in the
   disassembly at 0x737671f1 `mov esi,0x80004005`). E_NOINTERFACE (line 64) only happens if allocating an INT
   fails, which is unreachable here.

dd2d9 (`Palette::QueryInterface`, surface.cpp:1367-1376): NULL ppv → INVALIDPARAMS. Unknown IID →
`*out = NULL`, E_NOINTERFACE. No riid check. See difference 5.

### AddRef — `IDirectDrawPalette_AddRef.c`

It increments the GBL, LCL and INT counts and returns the INT count (lines 20-28). There are no checks.
dd2d9 (surface.cpp:1378) matches.

### Release — `IDirectDrawPalette_Release.c`, `FUN_73767289_73767289.c`

1. `IsBadWritePtr(This, 0x18)` fails, or the vtable isn't the palette vtable → 0 (Release.c:18-29).
2. FUN_73767289 decrements the GBL, LCL and INT counts (lines 32-34). There is no "already 0" check.
3. LCL count (ignoring bit 31) reaches 0 → the owner interface (LCL+0x20) is remembered (lines 39-45).
4. **GBL count reaches 0: if this INT is the palette of the creating DirectDraw object's primary surface, it is
   detached from it (`FUN_73766da1(primary, NULL)`)** (lines 46-50). Nothing else is detached: a palette that is
   over-released while on another surface leaves a dangling pointer (Wine test_primary_palette notes the access
   violation). Then the D3D handle is freed (51), the driver/HEL DestroyPalette runs (52-87) and the memory is freed.
5. INT count reaches 0: the INT is unlinked from the DirectDraw object's palette list, **its vtable is zeroed** and it is
   freed (96-116). A second Release therefore usually returns 0 through check 1.
6. The owner DirectDraw interface (set only for palettes made through IDirectDraw4/7) is Released (117-120).
7. It returns the INT count (121).

dd2d9 (surface.cpp:1380-1394, destructor 1352-1358): returns the count, releases the owner interface (`ddRef`). On
destruction it detaches the palette from **every** surface of its DirectDraw object (deliberately safer than XP).
There is no liveness or vtable check. See difference 6.

### GetCaps — `IDirectDrawPalette_GetCaps.c`

1. `*caps = 0` (line 25). NULL → INVALIDPARAMS through SEH.
2. The caps are **rebuilt from the GBL flags**, not copied from the creation flags (lines 26-51):
   - size: DDRAWIPAL_2 → DDPCAPS_1BIT, _4 → DDPCAPS_2BIT, _16 → DDPCAPS_4BIT, otherwise DDPCAPS_8BIT;
   - `| DDPCAPS_PRIMARYSURFACE` while this exact INT is `lpDD_lcl->lpPrimary->lpLcl->lpDDPalette` (lines 39-45);
   - `| DDPCAPS_ALLOW256` if ALLOW256, `| DDPCAPS_8BITENTRIES` if STORED_8INDEX.
   - **Never reported: DDPCAPS_ALPHA** (DDRAWIPAL_ALPHA exists but isn't mapped), DDPCAPS_INITIALIZE,
     DDPCAPS_VSYNC, DDPCAPS_PRIMARYSURFACELEFT.
3. Returns DD_OK.

Wine test_primary_palette (ddraw7.c:8665-8702) confirms 8BIT|ALLOW256, plus PRIMARYSURFACE only while attached.
dd2d9 (surface.cpp:1396-1403) returns the creation flags plus the dynamic PRIMARYSURFACE. See difference 2.

### GetEntries — `IDirectDrawPalette_GetEntries.c`

1. **`flags != 0` → DDERR_INVALIDPARAMS** (line 23 → 59).
2. **`count == 0`**, `count > N`, `start >= N` or `start + count > N` → DDERR_INVALIDPARAMS (lines 37-38).
3. It copies `count * entrysize` bytes from `table + start * entrysize` (39-51). A NULL buffer faults →
   INVALIDPARAMS. Returns DD_OK. The bytes are the stored table, so the peFlags rewrites listed under SetEntries
   are visible here.

dd2d9 (surface.cpp:1405-1416): flags ignored. `count == 0` (and `start == N` with `count == 0`) succeed. See difference 3.

### SetEntries — `IDirectDrawPalette_SetEntries.c`, HEL `FUN_7375205b_7375205b.c`

Validation, in order:
1. **`flags != 0` → DDERR_INVALIDPARAMS** (line 49 → 135).
2. Same range rule as GetEntries, **`count == 0` included** (lines 52-65).

Effect:
3. The entries are **copied into the palette's table first** (66-80), with 1 byte per entry for 8BITENTRIES. A NULL
   pointer faults here → INVALIDPARAMS, with nothing changed.
4. The SetEntries callback runs: HAL unless the palette is DDRAWIPAL_INHEL, else HEL (82-90). With no callback
   the result is DDERR_UNSUPPORTED, **after the table was already changed** (91-92). If the driver handles the call
   with an error, that error is returned (107-111).
5. The palette's contents stamp is incremented if non-zero, skipping 0 on wrap-around (112-118). D3D gets a
   palette-changed notification (119-125). Returns DD_OK (126).

HEL SetEntries (FUN_7375205b), which is what runs on XP (see Conventions):
- **8-bit palette (DDRAWIPAL_256) without ALLOW256, and `start == 0 || start + count == 256`: entry 0's RGB
  bytes are set to 0,0,0 and entry 255's to 255,255,255. Their peFlags bytes are not touched** (lines 23-31). Creation
  does the same (HEL CreatePalette FUN_7375231c:12-19). So entries 0 and 255 always read back black and white
  unless ALLOW256 is set. This holds for any 8-bit palette, not only the primary's.
- **Not the primary's palette** (the creating object's `lpPrimary` doesn't hold this palette GBL): the DIRTY flag is set and
  nothing else happens (32-37). Attached offscreen surfaces see the new table the next time it is read (blit, GetDC).
- The primary's palette, but the display isn't palettized (`GetDeviceCaps(RASTERCAPS) & RC_PALETTE == 0`,
  i.e. not an 8-bpp mode): only DIRTY (53-56).
- The primary's palette in an 8-bpp mode (58-121):
  - EXCLUSIVE is set if the palette's object is the exclusive owner, else cleared. A change sets DIRTY.
  - Not exclusive: `SetPaletteEntries` (all 256 if DIRTY) + `SelectPalette`, and `RealizePalette` unless the primary is lost (70-81).
  - Exclusive, not dirty: `SelectPalette` + `RealizePalette`, then the changed entries' **stored peFlags become
    `(peFlags & 0xF0) | PC_RESERVED | PC_NOCOLLAPSE` (0x05)**, and `AnimatePalette` (83-94).
  - Exclusive and dirty: the same peFlags rewrite on all 256 entries, entries 0/255 forced again (their peFlags low nibble cleared),
    `SetPaletteEntries(0,256)`, `SetSystemPaletteUse(SYSPAL_NOSTATIC256)`, realize, DIRTY cleared (95-121).
  - So the visible screen changes immediately, without a Flip or Blt.
- It returns DDHAL_DRIVER_HANDLED with DD_OK (135-136).

dd2d9 (surface.cpp:1420-1436): flags ignored, `count == 0` accepted. It copies, then forces 0/255 to
`{0,0,0,0}`/`{255,255,255,0}` (surface.cpp:1342-1347, peFlags zeroed), bumps `version`, and marks the display dirty
when the palette is the one the primary shows (including dd2d9's own back-buffer fallback, `ShownPalette`). The
visible result (the screen updates on the next present) matches. See differences 3 and 4.

### Initialize — `IDirectDrawPalette_Initialize.c:5`

Always DDERR_ALREADYINITIALIZED, with no checks. There is no uninitialized palette. dd2d9 (surface.cpp:1418) matches.

### CreatePalette (IDirectDraw/2/4/7) — `IDirectDraw7_CreatePalette.c` (one function for all versions)

Checks in order:
1. `pUnkOuter != NULL` → CLASS_E_NOAGGREGATION, before taking the lock (lines 51-53).
2. **`*ppPalette = NULL`** (line 65; NULL → INVALIDPARAMS through SEH).
3. **Flags outside 0x7FF (above DDPCAPS_ALPHA) → DDERR_INVALIDPARAMS** (66 → 231).
4. No SetCooperativeLevel yet (`dwLocalFlags & DDRAWILCL_SETCOOPCALLED`) → DDERR_NOCOOPERATIVELEVELSET (67-70).
5. **DDPCAPS_PRIMARYSURFACE (0x10), DDPCAPS_PRIMARYSURFACELEFT (0x20) or DDPCAPS_VSYNC (0x80) → DDERR_INVALIDPARAMS**
   (71 → 231; disassembly 0x73766e5a `test dl,0xb0`).
6. **Not exactly one of 1BIT/2BIT/4BIT/8BIT → INVALIDPARAMS. ALPHA together with 8BITENTRIES → INVALIDPARAMS** (72-90).
7. **8BIT with 8BITENTRIES → INVALIDPARAMS. ALLOW256 without 8BIT → INVALIDPARAMS** (91-97).
   DDPCAPS_INITIALIZE (0x8) is accepted and ignored.
8. **`entries[N-1]` is read** (120-129), so a NULL table faults → INVALIDPARAMS.
9. The objects are allocated (OOM → DDERR_OUTOFMEMORY, 224). The GBL flags are the size, ALLOW256, STORED_8INDEX (8BITENTRIES) and ALPHA
   (140-148). `N * entrysize` bytes of the table are copied (150-170).
10. Neither the HAL nor the HEL caps have DDSCAPS_PALETTE → DDERR_UNSUPPORTED (173-174, 217). The driver or HEL CreatePalette
    runs. If it handles the call with an error, that error is returned (175-199). It also returns DDERR_UNSUPPORTED when the
    call isn't handled. The HEL marks INHEL|DIRTY and forces 0/255 (FUN_7375231c).
11. Success: count 1, `*ppPalette` set. **Through IDirectDraw4/7 (any vtable but v1/v2), the DirectDraw interface is AddRef'd and
    stored as owner** (208-212). DD_OK.

dd2d9 (`DDraw::CreatePalette`, ddraw.cpp:1155-1169; wrappers.h:234-235): outer, then `!out`, then coop, then "at least one size bit".
The DD4/7 owner reference matches. See difference 1.

---

## IDirectDrawClipper

### QueryInterface — `IDirectDrawClipper_QueryInterface.c` (both vtables)

1. `IsBadReadPtr(riid, 16)` → DDERR_INVALIDPARAMS (23-28).
2. `*ppv = NULL` (29).
3. IID_IUnknown or IID_IDirectDrawClipper (0x7375ebd4, 6C14DB85-…) → AddRef, `*ppv = This`, S_OK (57-59). Otherwise
   E_NOINTERFACE (53).

dd2d9 (surface.cpp:1468-1477) matches, apart from the riid check. `LiveClipper` makes a dead or made-up This return
INVALIDPARAMS, which XP produces through SEH for unmapped pointers.

### AddRef / Release — `IDirectDrawClipper_AddRef.c`, `IDirectDrawClipper_Release.c`, `FUN_73733fb9_73733fb9.c`

- AddRef: GBL, LCL and INT counts +1, returns the INT count (AddRef.c:19-27). There are no checks.
- Release: `IsBadWritePtr(This, 0x18)` or a vtable other than the two clipper vtables → 0 (Release.c:20-33). Otherwise
  FUN_73733fb9:
  - decrements all three counts (19-22);
  - INT count 0: unlinked from its list (the DirectDraw GBL's `clipperList` at +0x17c, or the global driverless list `DAT_73770090`),
    vtable zeroed, freed (26-31);
  - GBL count 0: the static clip list is freed and so is the block (36-45);
  - LCL count 0: the owner DirectDraw interface is Released (32-35, 47-49);
  - **no surface is told**. A surface still holding an over-released clipper dangles (Wine test_clipper_refcount);
  - it returns the INT count (50).

dd2d9 (surface.cpp:1479-1491) matches this, including the vtable check (`g_clipperVtbl`) and the bad-pointer result 0.

### GetClipList — `IDirectDrawClipper_GetClipList.c`, `FUN_737363fc_737363fc.c`, `FUN_737364ff_737364ff.c`

1. **If `lpClipList != NULL`, `lpClipList->rdh.nCount = 0` first** (GetClipList.c:18-20), whatever happens next.
2. **No hWnd (static list):**
   - no list → **DDERR_NOCLIPLIST, before `lpdwSize` is touched** (FUN_737363fc:25-28);
   - `need = (list.nCount + 2) * 16`, i.e. header plus rectangles (30);
   - `lpClipList == NULL` → `*lpdwSize = need`, DD_OK (31-33, 53);
   - `*lpdwSize >= need` → the stored bytes (header as the game passed it) are copied, then **clipped to `lpRect`** by
     FUN_737364ff, DD_OK. **`*lpdwSize` is not updated** (35-49);
   - otherwise `*lpdwSize = need`, DDERR_REGIONTOOSMALL (51-53).
3. **hWnd set:** the list is the window's **visible region**: `GetDC(hWnd)` + `GetRandomRgn(hdc, rgn, SYSRGN)`, in screen
   coordinates (56-69). On NT this excludes the parts covered by other windows and the parts off the screen, and it can be any
   number of rectangles. A failure (an invalid hWnd makes GetDC fail) → E_FAIL (DDERR_GENERIC) (135).
   - `lpClipList == NULL` → `*lpdwSize = GetRegionData(rgn, 0, NULL)`, which is DD_OK if non-zero, else E_FAIL (70-76).
   - `GetRegionData(rgn, *lpdwSize, data)` fails and the region needs more → `*lpdwSize = needed`, DDERR_REGIONTOOSMALL (77-85).
   - Then the rectangles are clipped to the window's client rectangle (screen coordinates) (89-92) and to `lpRect` (screen
     coordinates) (130 → 48), DD_OK. `*lpdwSize` is not updated.
   - Multi-monitor (the object's GBL has more than one device and DDRAWI_DISPLAYDRV): the list is also clipped to the device's
     rectangle, and if the device isn't at (0,0) it is **translated to device-relative coordinates** (93-129).
4. FUN_737364ff (the clipping): if `lpRect` and the data are non-NULL and `lpRect` doesn't contain `rdh.rcBound`, each rectangle is
   intersected with `lpRect`. Empty results are dropped and `nCount` decremented (16-58). **`rcBound` and `nRgnSize` are left
   as they were.**

Wine test_clipper_blt (ddraw7.c:1370-1385): with an hWnd, `dwSize` is 32, the type is RDH_RECTANGLES, `nCount >= 1`, and `rcBound` is
the client rectangle in screen coordinates.

dd2d9 (surface.cpp:1493-1524): `!size` → INVALIDPARAMS first. hWnd → **one rectangle**: the client rectangle in screen coordinates
intersected with `lpRect`, with `rcBound` set to the same rectangle. Static list → copied, **`lpRect` ignored**. The sizes and the
REGIONTOOSMALL/NOCLIPLIST logic otherwise match. See differences 8, 9 and 10.

### GetHWnd — `IDirectDrawClipper_GetHWnd.c:17`

`*lphWnd = gbl->hWnd`, DD_OK. There are no checks. dd2d9 (surface.cpp:1555-1559) matches.

### Initialize — `IDirectDrawClipper_Initialize.c` (both vtables)

1. **Already initialized (GBL `dwFlags & 2`) → DDERR_ALREADYINITIALIZED** (29, 48). **Every clipper made by
   CreateClipper or DirectDrawCreateClipper is created initialized** (FUN_7373ce21:44-46). Only CoCreateInstance clippers
   (class factory, see below) aren't.
2. `flags != 0` → DDERR_INVALIDPARAMS (30, 45).
3. With `lpDD`: moved from the global list to that object's list, and `lpDD`, `lpDD_lcl` and `lpDD_int` are set. **The DirectDraw object isn't
   AddRef'd** (31-38).
4. It is marked initialized and gets the normal vtable (39-40), DD_OK.

The **Uninit vtable** (used until then) returns **DDERR_NOTINITIALIZED (0x800401F0)** from GetClipList, GetHWnd,
IsClipListChanged, SetClipList and SetHWnd (`IDirectDrawClipper_Uninit_*.c`). QI, AddRef, Release and Initialize are shared.

dd2d9 (surface.cpp:1561): DD_OK for any live clipper. See difference 14.

### IsClipListChanged — `IDirectDrawClipper_IsClipListChanged.c`

**`*lpbChanged = FALSE; *lpbChanged = TRUE;` → always TRUE, DD_OK** (lines 17-18; disassembly 0x7375e8b9-0x7375e8c0
`and [eax],0` / `mov [eax],1`). There are no other checks.

dd2d9 (surface.cpp:1563-1567) always returns FALSE. See difference 7.

### SetClipList — `IDirectDrawClipper_SetClipList.c`

1. **`flags != 0` → DDERR_INVALIDPARAMS** (25-26).
2. List non-NULL:
   - **`rdh.nCount == 0` → DDERR_INVALIDCLIPLIST** (33-36);
   - **unless the clipper was created through IDirectDraw (v1 vtable of `lpDD_int`)**: `rdh.dwSize < 32`,
     `rdh.iType != RDH_RECTANGLES`, or `IsBadReadPtr(list, (nCount + 2) * 16)` → DDERR_INVALIDCLIPLIST (37-45).
     Clippers from DirectDrawCreateClipper (no `lpDD_int`) are checked.
3. **hWnd set → DDERR_CLIPPERISUSINGHWND**, for a NULL list too, but only after the checks above (48, 77).
4. NULL → the stored list is freed, DD_OK (49-52). Otherwise exactly `(nCount + 2) * 16` bytes are copied as given (header included)
   and replace the old list (54-72), DD_OK. OOM → DDERR_OUTOFMEMORY (57).

dd2d9 (surface.cpp:1569-1581): no flag, count or header checks. The hWnd check comes first. `nCount > 65536` →
INVALIDPARAMS is dd2d9's own. It stores the same bytes. See difference 11.

### SetHWnd — `IDirectDrawClipper_SetHWnd.c`

1. **`flags != 0`, or `hWnd != NULL && !IsWindow(hWnd)` → DDERR_INVALIDPARAMS** (line 19 → 25-27).
2. `gbl->hWnd = hWnd`, DD_OK (20-23). **The static clip list is kept.** While an hWnd is set it is ignored by GetClipList
   and protected from SetClipList (CLIPPERISUSINGHWND). After SetHWnd(NULL) it is used again.

dd2d9 (surface.cpp:1583-1589): no checks, otherwise the same model (hwnd wins, the list survives). See difference 12.

### Creation: IDirectDraw::CreateClipper, DirectDrawCreateClipper — `IDirectDraw7_CreateClipper.c:20`, `DirectDrawCreateClipper.c:16`, `FUN_7373ce21_7373ce21.c`

FUN_7373ce21(ddGbl, flags, out, outer, initialized, ddLcl, ddInt):
1. `outer != NULL` → CLASS_E_NOAGGREGATION (16, 67).
2. `*out = NULL` (17).
3. **`flags != 0` → DDERR_INVALIDPARAMS** (18, 63).
4. The objects are allocated and linked into the DirectDraw object's list, or into the global list when there is no object (19-35). OOM → DDERR_OUTOFMEMORY.
5. Both callers pass initialized = 1 → normal vtable, ISINITIALIZED (41-47). The count is 1 (48).
6. **Created through IDirectDraw4/7 (a vtable other than v1/v2): the DirectDraw interface is AddRef'd and kept as owner**,
   released with the clipper (50-55). Through v1/v2, and from DirectDrawCreateClipper, there is no reference.

DirectDrawCreateClipper itself has **no SEH frame**: a NULL `out` with `outer == NULL` is an access violation inside the lock.
IDirectDraw7_CreateClipper has one (NULL → INVALIDPARAMS).

dd2d9: `DDraw::CreateClipper` (ddraw.cpp:1171-1177) and `DD2D9_DirectDrawCreateClipper` (main.cpp:96-101) check
`outer`/`!out` (main.cpp in the order `!out`, then outer), ignore flags, and take only an internal reference (`IntAddRef`,
surface.cpp:1455). See difference 13.

---

## Exports

### Export table

XP SP3 exports exactly 22 functions (pefile on `$S/dll/ddraw.dll`):

| Ord | Name | XP address | dd2d9 |
|---|---|---|---|
| 1 | AcquireDDThreadLock | 0x7373475a | implemented |
| 2 | CheckFullscreen | 0x7375a9d9 | stub → 0 |
| 3 | CompleteCreateSysmemSurface | 0x7375b960 | stub → DDERR_UNSUPPORTED |
| 4 | D3DParseUnknownCommand | 0x7375c8da | stub → DDERR_UNSUPPORTED |
| 5 | DDGetAttachedSurfaceLcl | 0x7375cf19 | stub → DDERR_UNSUPPORTED |
| 6 | DDInternalLock | 0x7375a63f | stub → DDERR_UNSUPPORTED |
| 7 | DDInternalUnlock | 0x7375a5f4 | stub → DDERR_UNSUPPORTED |
| 8 | DSoundHelp | 0x7375e4b6 | stub → 0 |
| 9 | DirectDrawCreate | 0x7375cca3 | implemented |
| 10 | DirectDrawCreateClipper | 0x7375e921 | implemented |
| 11 | DirectDrawCreateEx | 0x7373ccf6 | implemented |
| 12 | DirectDrawEnumerateA | 0x7375cb1b | implemented |
| 13 | DirectDrawEnumerateExA | 0x73731a57 | implemented |
| 14 | DirectDrawEnumerateExW | 0x7375c629 | implemented |
| 15 | DirectDrawEnumerateW | 0x7375caf6 | implemented |
| 16 | DllCanUnloadNow | 0x7375f32d | always S_FALSE |
| 17 | DllGetClassObject | 0x7375f1bf | implemented |
| 18 | GetDDSurfaceLocal | 0x7375b7e9 | stub → DDERR_UNSUPPORTED |
| 19 | GetOLEThunkData | 0x7375b619 | stub → NULL |
| 20 | GetSurfaceFromDC | 0x7376089f | implemented (not audited here) |
| 21 | RegisterSpecialCase | 0x7375009c | stub → DD_OK |
| 22 | ReleaseDDThreadLock | 0x73734789 | implemented |

`src/ddraw.def` has the same 22 names **with the same ordinals and stack sizes**. Nothing is missing relative to XP.
Relative to later Windows (`$S/dll/win11/ddraw.dll`, 10.0.26100): Windows 11 drops CheckFullscreen, renumbers the rest
(e.g. DirectDrawCreate is @8 there), and adds **`SetAppCompatData` (@22, stdcall, 2 DWORDs)**. That is the only
export dd2d9 lacks compared with any Windows version. It is called by compatibility layers (e.g. DDrawCompat calls it on the
system ddraw), not by games, and D7VK 2.7.1 neither exports nor calls it (`$S/d7vk-src/src/ddraw/ddraw.def`, ddraw_main.cpp).

What the internal ones do on XP (for reference; none is used by games):
- CheckFullscreen (CheckFullscreen.c): if another process holds the "DDrawCheckFullscreenSemaphore" (an exclusive-mode app), it calls
  `ChangeDisplaySettingsA(NULL, CDS_RESET)` and returns 1. Otherwise it returns 0.
- DSoundHelp: registers DirectSound's window for activation tracking (FUN_7375e236).
- GetOLEThunkData(n): returns internal pointers for n = 1-5 (n = 5 → FUN_73734625). n = 6 releases a mutex.
- DDInternalLock/Unlock: the surface Lock/Unlock internals with flags 0x180/0x80, used by d3dim.
- RegisterSpecialCase: an app-compatibility table (GUID pairs) filled by the shim engine.

D7VK interop note (**not a dd2d9 difference**): D7VK 2.7.1's exported `ReleaseDDThreadLock`
(`$S/d7vk-src/src/ddraw/ddraw_main.cpp:520-538`) resolves **`"AcquireDDThreadLock"`** from the proxied ddraw. With dd2d9
behind D7VK, every ReleaseDDThreadLock a game makes enters `g_lock` once more instead of leaving it. dd2d9's presenter
thread then blocks forever. It would be worth reporting upstream.

### AcquireDDThreadLock / ReleaseDDThreadLock — `AcquireDDThreadLock_7373475a.c`, `ReleaseDDThreadLock_73734789.c`

Acquire: EnterCriticalSection on the global lock, recursion counter +1, and on the outermost entry a mode-change check
(`GdiEntry13` uniqueness → FUN_7373cc76). Release: counter -1, LeaveCriticalSection. There are no return values and no checks.
dd2d9 (main.cpp:146-150) uses the same lock as its methods, and runs deferred work on the last release. This matches.

### DirectDrawCreate — `DirectDrawCreate.c`, `FUN_7373ad42_7373ad42.c`

1. `outer != NULL` → CLASS_E_NOAGGREGATION (DirectDrawCreate.c:9, 18). This comes **before** the out-pointer check.
2. `GetProfileInt("DirectDraw", "reloadreg")` (10-14).
3. FUN_7373ad42(guid, out, 0, flags = 0, NULL):
   - `IsBadWritePtr(out, 4)` → DDERR_INVALIDPARAMS (38-39, 0x73745cad), then `*out = NULL` (40);
   - the desktop is under 8 bpp (`BITSPIXEL * PLANES < 8`, FUN_73738bbc) → DDERR_NODIRECTDRAWSUPPORT (41-42, 0x73745cb7);
   - registry settings (ModeXOnly, EmulationOnly, …) are read (51-132);
   - GUID: NULL → the primary display. `DDCREATE_EMULATIONONLY` (2) / `DDCREATE_HARDWAREONLY` (1) → primary with that
     restriction. Otherwise **`IsBadReadPtr(guid, 16)` → DDERR_INVALIDPARAMS** (182-185; 0x7373e135 → 0x73745e2d). An all-zero GUID
     (0x7373ca60) = primary. **Any other GUID is looked up by FUN_7373e42e. It must be a display-device GUID
     `{67685559+n-3106-11D0-B971-00AA00342F9F}`, n = an attached EnumDisplayDevices index, or a `Hardware\DirectDrawDrivers`
     entry. Otherwise DDERR_INVALIDDIRECTDRAWGUID** (169-172; 0x7373e3ed → 0x73745e37);
   - after that come the driver and object creation errors (E_FAIL, OUTOFMEMORY, etc.).

dd2d9 (main.cpp:81-86, 9-21): `!out` → INVALIDPARAMS **before** the outer check. Special GUIDs 1/2 are handled. **Any other
GUID pointer is accepted unchecked and gives the primary.** See difference 15.

### DirectDrawCreateEx — `DirectDrawCreateEx.c`

1. `outer != NULL` → CLASS_E_NOAGGREGATION (11, 42).
2. `riid != IID_IDirectDraw7` (0x73737c34) → DDERR_INVALIDPARAMS (12-22, 38). `*out` is untouched. A NULL riid crashes (no SEH).
3. FUN_7373ad42 with `DDRAWILCL_DIRECTDRAW7` (0x20000) into a local (28). On failure `*out = NULL` and its error is returned (33-34). On
   success it QIs for IID_IDirectDraw7 into `*out` (a bad out faults inside QI → INVALIDPARAMS, and the object is destroyed by the
   following Release), then Releases the local (30-31).

dd2d9 (main.cpp:88-94): order `!out`, outer, riid. The same GUID handling as DirectDrawCreate. See difference 15.

### DirectDrawEnumerateA/W, ExA/ExW — `DirectDrawEnumerateA.c`, `…W.c`, `…ExA.c`, `…ExW.c`

ExA (`DirectDrawEnumerateExA.c`; the return values are from the disassembly at 0x73731aff-0x73731b7c and 0x7373e194):
1. It reads the `EnumerateAttachedSecondaries` registry value (52-62).
2. `IsBadCodePtr(cb)` or `flags & ~7` → DDERR_INVALIDPARAMS (63-64). **DDENUM_DETACHEDSECONDARYDEVICES is accepted and
   then cleared** (0x73731b0c `and [flags],~2`): detached devices are never listed.
3. First call: `cb(NULL, <string resource 0x67>, "display", ctx, NULL)` (65-67). Resource 0x67 is the localized "Primary
   Display Driver" (this Swedish build has "Primär bildskärmsdrivrutin"). If the callback returns 0, enumeration stops.
4. With DDENUM_NONDISPLAYDEVICES (4): every `HKLM\Hardware\DirectDrawDrivers` subkey with a Description and a DriverName whose driver
   loads (3dfx-type add-on cards) is listed with its GUID and a NULL hMonitor. A callback returning 0 stops (70-119).
5. If there are at least 2 non-mirroring display devices (FUN_73731bb1 returns 1, checked in the disassembly at 0x73731c10-0x73731c19): for each
   EnumDisplayDevicesA entry that isn't a mirroring driver and is attached to the desktop, and if (flags & DDENUM_ATTACHEDSECONDARYDEVICES)
   or (the registry value is set and it isn't the primary device), `cb(&{67685559+n-…}, DeviceString, DeviceName, ctx, hMonitor)`.
   **With the flag, the primary device is listed a second time under its own GUID.** A callback returning 0 stops (120-157).
6. Returns DD_OK (0x73731b6d), whatever the callbacks returned.

- EnumerateA: `IsBadCodePtr(cb)` → INVALIDPARAMS. Otherwise `ExA(thunk, {cb, ctx}, DDENUM_NONDISPLAYDEVICES)` (EnumerateA.c:13-18), so it lists
  the primary entry and non-display devices, plus attached secondaries only through the registry value.
- EnumerateW: `IsBadCodePtr(cb)` ? DDERR_INVALIDPARAMS : DDERR_UNSUPPORTED (EnumerateW.c:8-9).
- ExW: always DDERR_UNSUPPORTED, with no checks (ExW.c:6).

dd2d9 (main.cpp:103-130): the primary entry only, with the same flag validation (NULL instead of IsBadCodePtr) and English text. W and ExW match.
See difference 16.

### DllGetClassObject / class factory / DllCanUnloadNow — `DllGetClassObject_7375f1bf.c`, `FUN_7375eed1_7375eed1.c`, `DllCanUnloadNow_7375f32d.c`

DllGetClassObject:
1. `IsBadWritePtr(ppv, 4)` → **DDERR_INVALIDPARAMS** (14, 131), then `*ppv = NULL` (16).
2. Bad rclsid or riid → INVALIDPARAMS (17-18, 127).
3. The CLSID must be CLSID_DirectDraw, CLSID_DirectDraw7, CLSID_DirectDrawClipper or the undocumented
   {B9DC4790-4AF1-11D1-8C4C-00C04FD930C5}. **Anything else → E_FAIL** (19-68).
4. riid not IUnknown/IClassFactory → E_NOINTERFACE (72-97).
5. A factory is allocated (OOM → E_OUTOFMEMORY) and QI'd (106-121).

Factory CreateInstance (FUN_7375eed1):
1. Bad This/vtable, bad riid or bad ppv → INVALIDPARAMS (18-23).
2. outer → CLASS_E_NOAGGREGATION (24-26).
3. Desktop under 8 bpp → E_FAIL (27-30).
4. CLSID_DirectDrawClipper → an **uninitialized** clipper (Uninit vtable), QI'd for riid (73-77).
5. CLSID_DirectDraw or CLSID_DirectDraw7 → an **uninitialized** DirectDraw object whose vtable is chosen by riid (IUnknown/IDirectDraw → v1,
   IDirectDraw2/4/7 → their Uninit tables, anything else → E_NOINTERFACE) (81-172). The CLSID doesn't decide the version.
6. The fourth CLSID matches none of these and returns S_OK with `*ppv` untouched (78).

DllCanUnloadNow: S_OK when there is no DirectDraw object, no driverless clipper and none of two other internal lists has entries (`DAT_7376f9a4`,
`DAT_73773220`). Otherwise S_FALSE (17-20).

dd2d9 (main.cpp:23-60, 132-144): NULL ppv → E_POINTER, unknown CLSID → CLASS_E_CLASSNOTAVAILABLE, and the factory makes a working
clipper. DllCanUnloadNow → S_FALSE always (the DLL pins itself, ddraw.cpp:360). See differences 14 and 17.

---

## Differences from dd2d9

Severity: **game-visible** means a plausible game call sequence behaves differently. **Edge case** means
it only shows up with invalid parameters, unusual sequences or exact-return-code checks.

1. **CreatePalette accepts flag combinations XP refuses, and checks in another order** (edge case).
   - XP (IDirectDraw7_CreatePalette.c:51-129), in this order:
     - outer;
     - `*out = NULL`;
     - flags above 0x7FF;
     - cooperative level;
     - PRIMARYSURFACE/PRIMARYSURFACELEFT/VSYNC;
     - exactly one size bit;
     - ALPHA+8BITENTRIES, 8BIT+8BITENTRIES, ALLOW256 without 8BIT;
     - a NULL table (fault).
     Each of these gives DDERR_INVALIDPARAMS, except the cooperative level (NOCOOPERATIVELEVELSET) and outer (CLASS_E_NOAGGREGATION).
   - dd2d9 (ddraw.cpp:1157-1160) accepts all of them. A NULL table gives a black palette. Several size bits pick the smallest
     (`Count()`, surface.cpp:1360-1365). `*out` isn't cleared on failure, and the cooperative level is checked before the flags.
   - Fix: copy XP's sequence: `*out = NULL` after the outer check, then `if (flags & ~0x7FF) INVALIDPARAMS; if (!coop_) NOCOOPERATIVELEVELSET;
     if (flags & 0xB0) INVALIDPARAMS;` then the bit-count and combination rules, then `if (!entries) INVALIDPARAMS`.
     **Caution:** XP refusing DDPCAPS_PRIMARYSURFACE could break Win9x-era games that pass it and never ran on NT. If such a
     game turns up, keep accepting it (log it) as a deliberate difference.
2. **GetCaps returns the creation flags instead of rebuilding them** (edge case).
   - XP (GetCaps.c:25-51) reports only the size bit, 8BITENTRIES, ALLOW256 and the dynamic PRIMARYSURFACE. **DDPCAPS_ALPHA is never
     reported**, nor INITIALIZE.
   - dd2d9 (surface.cpp:1399) also returns ALPHA, INITIALIZE and any other bit the game passed (with fix 1, only ALPHA and INITIALIZE can remain).
   - Fix: `*c = caps & (DDPCAPS_1BIT|DDPCAPS_2BIT|DDPCAPS_4BIT|DDPCAPS_8BIT|DDPCAPS_8BITENTRIES|DDPCAPS_ALLOW256)` plus PRIMARYSURFACE.
3. **GetEntries/SetEntries ignore `flags` and accept `count == 0`** (edge case).
   - XP: `flags != 0` → INVALIDPARAMS (GetEntries.c:23, SetEntries.c:49). `count == 0` or `start >= N` → INVALIDPARAMS
     (GetEntries.c:37, SetEntries.c:64).
   - dd2d9 (surface.cpp:1407, 1422): `start > n || count > n - start` only.
   - Fix: `if (flags || !in || !count || start >= n || count > n - start) return DDERR_INVALIDPARAMS;` in both.
4. **peFlags of the stored table** (edge case).
   - XP: the forced entries 0/255 get only their RGB bytes set, and **keep the game's peFlags** (FUN_7375205b:23-31,
     FUN_7375231c:12-19). On the primary's palette in an 8-bpp exclusive mode, the entries changed (or all 256 when dirty) get
     `peFlags = (peFlags & 0xF0) | 0x05`, and GetEntries returns that (FUN_7375205b:83-121; SetPalette's HEL FUN_73751e14:66-103 does
     the same on attach).
   - dd2d9 (surface.cpp:1344-1345) zeroes peFlags of 0/255 and never rewrites peFlags.
   - Fix: in `ReserveSystemEntries`, set only peRed/peGreen/peBlue. The 0x05 rewrite needs a real 8-bpp display and is not worth copying
     (dd2d9 never runs in a palettized mode). Treat it as a deliberate difference.
5. **Palette QueryInterface for an unknown IID** (edge case).
   - XP: **E_FAIL with `*ppv` untouched**. A bad riid → INVALIDPARAMS (QI.c:23-27, 52; 0x737671f1).
   - dd2d9 (surface.cpp:1374-1375): `*out = NULL`, E_NOINTERFACE.
   - Fix: `return E_FAIL;` without touching `*out`. The Wine-like E_NOINTERFACE is arguably saner. If kept, record it as deliberate.
6. **No liveness check in palette Release/AddRef** (edge case: over-release).
   - XP: Release on a freed palette (vtable zeroed at FUN_73767289:114) or a bad pointer returns 0 (Release.c:18-29).
   - dd2d9 (surface.cpp:1380-1394) is a use-after-free after the last Release. The clipper has a `LiveClipper` registry for this
     case (surface.cpp:1443-1451), the palette doesn't.
   - Fix: give Palette the same live-set + vtable check in Release (and AddRef/the other methods, as for clippers).
   - Detaching from every surface at destruction (surface.cpp:1352-1358) instead of only from the primary (FUN_73767289:46-50) is a
     deliberate safety difference. Keep it.
7. **IsClipListChanged always says FALSE; XP always says TRUE** (game-visible, rare).
   - XP: `*changed = TRUE`, DD_OK (IsClipListChanged.c:17-18; 0x7375e8c0).
   - dd2d9 (surface.cpp:1565) returns FALSE. A windowed game that caches its clip list and refreshes it only when this returns TRUE
     never sees the window move or resize under dd2d9, and keeps drawing at the old position.
   - Fix: `*changed = TRUE;`.
8. **GetClipList with an hWnd returns the bare client rectangle** (game-visible, rare).
   - XP (FUN_737363fc:56-130) returns the window's visible region (GetRandomRgn SYSRGN): parts covered by other windows are excluded,
     there can be several rectangles, and **parts outside the screen are cut off**. It is then clipped to the client area and `lpRect`.
     `rcBound`/`nRgnSize` come from GetRegionData before the clipping. A size query (`lpClipList == NULL`) returns that region's real size.
   - dd2d9 (surface.cpp:1497-1509) returns one rectangle, the client rectangle in screen coordinates, not clipped to the screen. Its
     `rcBound` is the clipped rectangle, and the size is always 48.
   - The game-visible part is a window partly off the desktop. A game that Locks the primary and writes only inside the GetClipList rectangles
     (its own clipping) gets rectangles with negative or out-of-range coordinates. That writes outside dd2d9's primary
     buffer, where XP's list would have been clipped to the screen.
     Occlusion by other windows doesn't matter for dd2d9, which presents into the game's own window anyway.
   - Fix: at least intersect with the desktop (primary) rectangle, and report an empty list (`nCount = 0`, size 32) when nothing is left.
     Optionally use `GetRandomRgn(GetDC(hwnd), rgn, SYSRGN)` + `GetRegionData` as XP does, and keep `rcBound` unclipped.
9. **GetClipList ignores `lpRect` for a static clip list** (edge case).
   - XP clips the copied list to `lpRect` (FUN_737363fc:48 → FUN_737364ff), dropping empty rectangles and lowering `nCount`. It leaves
     `rcBound`/`nRgnSize` and the reported size as they were.
   - dd2d9 (surface.cpp:1510-1513, 1522) copies it unchanged.
   - Fix: after the memcpy, run the same intersect-and-compact over `data->Buffer` when `rect` doesn't contain `rcBound`.
10. **GetClipList's early steps** (edge case).
    - XP zeroes `lpClipList->rdh.nCount` before anything (GetClipList.c:18-20), even when it then fails. NOCLIPLIST comes before
      `lpdwSize` is used, so `GetClipList(NULL, NULL, NULL)` on an empty clipper → DDERR_NOCLIPLIST
      (FUN_737363fc:27-28). Wine test_clipper_blt calls it with a valid size.
    - dd2d9 (surface.cpp:1494): `!size` → INVALIDPARAMS first, and doesn't touch `nCount`.
    - Fix: `if (data) data->rdh.nCount = 0;`, then the list/hwnd logic, then validate `size` where it is first used.
11. **SetClipList validation** (edge case).
    - XP (SetClipList.c:25-48), in this order:
      - `flags != 0` → INVALIDPARAMS;
      - `nCount == 0` → DDERR_INVALIDCLIPLIST;
      - `rdh.dwSize < 32`, `iType != RDH_RECTANGLES` or unreadable → INVALIDCLIPLIST, except for clippers made through IDirectDraw (v1);
      - then hWnd → CLIPPERISUSINGHWND.
    - dd2d9 (surface.cpp:1569-1580) checks the hWnd first and none of the rest (an empty list is stored and clips everything away). Its own
      `nCount > 65536` → INVALIDPARAMS is fine to keep as a deliberate allocation guard, or turn into INVALIDCLIPLIST.
    - Fix: add the checks in XP's order. Remember whether the clipper came from a v1 `CreateClipper` (pass the version through
      wrappers.h:233).
12. **SetHWnd doesn't validate** (edge case).
    - XP: `flags != 0` or a non-NULL hWnd that isn't a window → INVALIDPARAMS (SetHWnd.c:19).
    - dd2d9 (surface.cpp:1583-1589) stores anything.
    - Fix: `if (flags || (h && !IsWindow(h))) return DDERR_INVALIDPARAMS;`.
13. **Clipper creation: flags, order, and the DirectDraw reference** (edge case).
    - XP (FUN_7373ce21:16-55):
      - outer → NOAGGREGATION before anything;
      - `*out = NULL`;
      - **`flags != 0` → INVALIDPARAMS**;
      - **a clipper made through IDirectDraw4/7 holds a public reference on that interface**, released with the clipper. The
        game sees it in `IDirectDraw7::Release`'s return value, and the object outlives a premature Release.
    - dd2d9 (ddraw.cpp:1171-1177, main.cpp:96-101): flags ignored, `!out` checked before outer in DirectDrawCreateClipper, and only an
      internal reference (surface.cpp:1455).
    - Fix: check outer first, clear `*out`, refuse flags. Pass the version to `CreateClipper` and, for V >= 4, `AddRef(version)` and remember it,
      as `CreatePalette` already does with `ddRef`.
    - (dd2d9 returning INVALIDPARAMS for a NULL `out` in DirectDrawCreateClipper, where XP crashes, is fine.)
14. **Clipper Initialize and CoCreateInstance clippers** (edge case).
    - XP: every clipper from CreateClipper or DirectDrawCreateClipper is already initialized → Initialize returns
      DDERR_ALREADYINITIALIZED (Initialize.c:29, 48). Only the class factory makes an uninitialized clipper. It answers
      DDERR_NOTINITIALIZED from GetClipList/GetHWnd/IsClipListChanged/SetClipList/SetHWnd until `Initialize(lpDD or NULL, 0)`
      (flags ≠ 0 → INVALIDPARAMS), which binds it to `lpDD` without a reference (Initialize.c:30-40).
    - dd2d9 (surface.cpp:1561, main.cpp:46-51): Initialize → DD_OK always, and factory clippers work at once.
    - Fix: add an `initialized` flag. False only for factory clippers. While false, the five methods return DDERR_NOTINITIALIZED.
      Initialize: initialized → ALREADYINITIALIZED, flags → INVALIDPARAMS, else bind `dd_` (IntAddRef only) and set it.
15. **DirectDrawCreate/Ex: check order and GUID validation** (edge case).
    - XP:
      - outer is checked first (DirectDrawCreate.c:9, DirectDrawCreateEx.c:11);
      - then riid (Ex);
      - then the out pointer (FUN_7373ad42:38);
      - a GUID pointer that can't be read → INVALIDPARAMS;
      - **an unknown GUID → DDERR_INVALIDDIRECTDRAWGUID** (0x73745e37). An all-zero GUID means the primary.
    - dd2d9 (main.cpp:83-93) checks `!out` first and accepts any GUID as the primary.
    - Fix: reorder (outer, [riid], out). Accept NULL, 1, 2, GUID_NULL and whatever GUIDs dd2d9's own enumeration hands out
      (see 16). Refuse other readable GUIDs with DDERR_INVALIDDIRECTDRAWGUID.
    - **Caution:** a game that stored a device GUID from a real Windows install would then fail where dd2d9 now works. Keeping the leniency
      (logged) may be the better deliberate choice.
16. **DirectDrawEnumerate(Ex)A lists only the primary entry** (edge case: device menus).
    - XP (EnumerateExA.c:70-157): with DDENUM_ATTACHEDSECONDARYDEVICES on a machine with at least two display devices, it also lists every
      attached device, **including the primary device again**, as `{67685559+n-3106-11D0-B971-00AA00342F9F}`, DeviceString,
      `\\.\DISPLAYn` and an hMonitor. With DDENUM_NONDISPLAYDEVICES (which EnumerateA always passes), it also lists the
      `HKLM\Hardware\DirectDrawDrivers` devices. The primary entry's description is localized.
    - dd2d9 (main.cpp:103-125) gives the "Primary Display Driver" entry alone.
    - Fix (optional): with the flag and two or more monitors, list the attached devices as XP does. DirectDrawCreate must then accept those
      GUIDs (create on the primary, or on that monitor if dd2d9 ever supports it). Otherwise record it as deliberate: dd2d9 drives one display.
17. **DllGetClassObject / DllCanUnloadNow codes** (edge case).
    - XP: NULL/bad ppv → DDERR_INVALIDPARAMS (not E_POINTER). An unknown CLSID → **E_FAIL** (not CLASS_E_CLASSNOTAVAILABLE) (DllGetClassObject.c:14-67).
      The factory's CreateInstance checks a bad riid/ppv (INVALIDPARAMS) before outer (FUN_7375eed1:18-26). DllCanUnloadNow → S_OK once no
      objects remain.
    - dd2d9 (main.cpp:134-144, 42-45, 132).
    - Fix: change the two codes, and the factory's check order. DllCanUnloadNow staying S_FALSE is deliberate (dd2d9 pins itself, ddraw.cpp:360).
      Keep it.

### Matches (verified)

- Palette: AddRef/Release returning the INT count. The DD4/7 owner reference of palettes (none through v1/v2). Initialize →
  ALREADYINITIALIZED. GetCaps' dynamic PRIMARYSURFACE (only while this palette is the primary's). The 8BITENTRIES byte layout. Entries 0/255
  forced to black/white for 8-bit palettes without ALLOW256, at creation and on every SetEntries. SetEntries touching only the table and
  the display (no per-surface work).
- Clipper: QI/AddRef/Release (including the vtable check in Release). GetHWnd. The hWnd-wins model with a surviving static list.
  GetClipList's size protocol (size query → DD_OK, REGIONTOOSMALL with the needed size, `*size` untouched on success, stored header returned
  as given). SetClipList(NULL) frees the list. CLIPPERISUSINGHWND.
- Exports: the full XP export table with XP's ordinals. Acquire/ReleaseDDThreadLock sharing the method lock. EnumerateExA flag validation and the
  primary entry (NULL GUID, "display", NULL hMonitor). EnumerateW and EnumerateExW results. DirectDrawCreateEx's IID_IDirectDraw7 requirement
  (INVALIDPARAMS, `*out` untouched). DDCREATE_HARDWAREONLY/EMULATIONONLY.

### Not determined

- Whether any XP display driver supplies HAL palette callbacks (CreatePalette `lpDDCB+0xa0`, SetEntries `+0xfc`). The analysis assumes
  the HEL (FUN_7375231c, FUN_7375205b) runs, because NT's kernel DirectDraw interface has no palette entry points as far as is known.
  This wasn't verified against win32k. If a HAL did run, the 0/255 forcing and the peFlags rewrites might not happen.
- Whether the HEL caps always include DDSCAPS_PALETTE (CreatePalette's DDERR_UNSUPPORTED path, IDirectDraw7_CreatePalette.c:173-174).
  It is presumably always set. The caps setup wasn't traced.
- The English XP string for resource 0x67. The reference binary is Swedish. "Primary Display Driver" is assumed from English Windows and Wine.
- Whether the EnumerateA thunk (FUN_7375c60a) returns the A callback's result. Ghidra shows it as void. If it does, a callback returning 0 stops the enumeration as it
  does through ExA.
- What CLSID {B9DC4790-4AF1-11D1-8C4C-00C04FD930C5} was meant for (it gets a factory whose CreateInstance returns S_OK and creates nothing).
- GetSurfaceFromDC (export 20) and the internal stubs (CompleteCreateSysmemSurface, DDGetAttachedSurfaceLcl, GetDDSurfaceLocal,
  DDInternalLock/Unlock, D3DParseUnknownCommand) weren't audited beyond their purpose. They are only used by d3dim, ddrawex and dxdiag.
- The exact multi-monitor GBL fields used by GetClipList (offsets 0x5c8-0x5d8, 0x608: device rectangle and device count). The reading
  above is inferred from the arithmetic, not from a structure definition.
