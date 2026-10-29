# IDirectDrawSurface: Lock, Unlock, GetDC, ReleaseDC, IsLost, Restore, PageLock, PageUnlock

Reference: Windows XP SP3 ddraw.dll 5.3.2600.5512. One function body implements every
interface version (1/2/3/4/7) of each method, except Unlock (v1–3 take an LPVOID, v4/v7 an
LPRECT; both wrappers call the same worker). Where the version matters the code compares
`This->lpVtbl` with `&vtbl_IDirectDrawSurface..4`.

File references `X:<file>:<line>` are to the Ghidra output in `$S/decomp-annotated/`
(S = the audit scratchpad); `D:<file>:<line>` are to dd2d9's `src/` as of 2026-10-29 10:43 (src/ was being
edited during the audit; if the numbers have moved, the function names still apply).
Every XP method takes the DirectDraw critical section first, and at the outermost level
checks the display-settings uniqueness (`GdiEntry13` = DdQueryDisplaySettingsUniqueness);
when it has changed it refetches the display data (FUN_7373cc76 → FUN_7373c175 → …), which
is where a mode change made by anyone loses the surfaces (see §9). Lock, GetDC, IsLost,
Restore, PageLock and ReleaseDC run inside an SEH frame whose handler returns
DDERR_INVALIDPARAMS (scope tables at 0x73733698, 0x737380f0, 0x73738310, 0x7375fea0,
0x737617d8, 0x737616d8: the handlers all `mov $0x80070057,%eax`), so a bad pointer that is
dereferenced inside the frame gives DDERR_INVALIDPARAMS rather than a crash.

Names: LCL/GBL/MORE = `_DDRAWI_DDRAWSURFACE_LCL/_GBL/_MORE`; GBL_MORE = the structure whose
pointer sits at `GBL - 4`. LCL flags used here (ddrawi.h names, values checked against the
code): INVALID 0x10000000 (= "lost"), HASDC 0x80, IMPLICITCREATE 0x2, IMPLICITROOT 0x10,
ISFREE 0x4, PARTOFPRIMARYCHAIN 0x20, HASPIXELFORMAT 0x2000, FRONTBUFFER 0x04000000,
HELCB 0x02000000, DRIVERMANAGED 0x40000000, DCILOCK 0x80000000. GBL flags: MEMFREE 0x1,
SYSMEMREQUESTED 0x2, ISGDISURFACE 0x4, LOCKNOTHOLDINGWIN16LOCK 0x10, FASTLOCKHELD 0x8000,
READONLYLOCKHELD 0x10000. "Usage" = `GBL->dwUsageCount` (offset 0x20), "rect list" =
`GBL->lpRectList` (offset 8, a list of ACCESSRECTLIST nodes: next, rDest, lpOwner =
DirectDraw LCL, lpSurfaceData, dwFlags, …).

`0x8876028F` (code 655) is returned for DDSCAPS_OPTIMIZED surfaces. Neither mingw's nor
Wine's ddraw.h names it (it is **not** DDERR_NOTLOADED, which is 601 = 0x88760259 and is what
IsLost returns). dd2d9 never has optimized surfaces, so these rows never apply.

---------------------------------------------------------------------------------------------

## 1. Lock(lpRect, lpDesc, dwFlags, hEvent): X:IDirectDrawSurface7_Lock.c

`hEvent` is never read.

### 1.1 Validation, in XP's order

| # | Condition | Result | Line |
|---|-----------|--------|------|
| 1 | surface lost | DDERR_SURFACELOST (before lpDesc is touched: a NULL desc on a lost surface gives SURFACELOST) | 49–52 |
| — | **Fast path** (see 1.4): the DirectDraw object is a DirectDraw7 one (`DDRAWILCL_DIRECTDRAW7`), GBL has SYSMEMREQUESTED, caps have DDSCAPS_TEXTURE, not DDSCAPS_VIDEOPORT, and no hardware op pending (`lpDDCB+0x2a8 && GBL & 0x600`, driver). Jumps past everything below. | | 53–62 |
| 2 | DDSCAPS_OPTIMIZED | 0x8876028F | 63–66 |
| 3 | `dwFlags & 0xFFFE87CE`: any bit other than WAIT 0x1, READONLY 0x10, WRITEONLY 0x20, NOSYSLOCK 0x800, NOOVERWRITE 0x1000, DISCARDCONTENTS 0x2000, DONOTWAIT 0x4000, NODIRTYUPDATE 0x10000. So DDLOCK_EVENT (0x2), 0x4, 0x8, 0x40–0x400, 0x8000 (HASVOLUMETEXTUREBOXRECT) and ≥0x20000 are refused, on every version. | DDERR_INVALIDPARAMS | 67 → 216–217 |
| — | IDirectDrawSurface7 only: DONOTWAIT set → WAIT cleared; else WAIT set (v7 waits by default). v1–4: flags unchanged (DONOTWAIT accepted, no effect). | — | 68–77 |
| 4 | `lpDesc->dwSize` neither 0x6C (DDSURFACEDESC) nor 0x7C (DDSURFACEDESC2): either size is accepted on every interface version. NULL lpDesc faults here → SEH → DDERR_INVALIDPARAMS. | DDERR_INVALIDPARAMS | 78 → 216 |
| — | `lpDesc->lpSurface = NULL` from here on: every later failure returns with lpSurface zeroed (Wine test_lockrect_invalid checks this). | — | 79 |
| 5 | `LCL->dwProcessId != GetCurrentProcessId()` | DDERR_SURFACEBUSY | 80–84 |
| 6 | lpRect and DDSCAPS_EXECUTEBUFFER | DDERR_INVALIDPARAMS | 86 |
| 7 | lpRect with `left < 0`, `top < 0`, `right < left`, `bottom < top`, `bottom > height` or `right > width`. **Empty rectangles (left == right or top == bottom) are valid.** | DDERR_INVALIDPARAMS | 87–93 |
| 8 | internal lock (FUN_737336a9, flags \| 0x440) fails — see 1.2 | its code | 98, 197–213 |

### 1.2 Internal lock (FUN_737336a9), normal path

X:FUN_737336a9_737336a9.c

* 54–56: if a hardware blit involving the surface is pending (GBL 0x600), wait for it (driver).
* 57–76: video-port surfaces: wait for the port; without WAIT → DDERR_WASSTILLDRAWING (driver).
* 78–82: "hold the lock" (`local_18`) is decided: the caller passed 0x40 (Lock always does)
  and GBL lacks SYSMEMREQUESTED, and the DirectDraw object is the display driver
  (DDRAWI_DISPLAYDRV). I.e. any surface not explicitly created in system memory.
* 90–154, **busy rules** (skipped entirely for D3D-managed textures: DDRAWILCL 0x200000 and
  DDSCAPS2_TEXTUREMANAGE, line 90–91):
  * rect lock: if the rect intersects (IntersectRect non-empty) any rectangle already in the
    rect list → DDERR_SURFACEBUSY (93–97). An empty rectangle never intersects anything.
  * rect list empty and usage ≠ 0 (a whole-surface lock, or a GetDC, is outstanding)
    → DDERR_SURFACEBUSY (99, 102–105).
  * whole-surface lock (lpRect NULL) and (usage ≠ 0 or rect list non-empty) → DDERR_SURFACEBUSY (100).
  * So: rect locks nest as long as they don't overlap; a whole lock excludes everything;
    a rect lock is refused while a whole lock or a DC is out.
  * A rect lock pushes a node (rect, owner = DirectDraw LCL) on the rect list (115–126);
    allocation failure → DDERR_OUTOFMEMORY.
* 158–161: usage++; if not SYSMEMREQUESTED, `DD_GBL+0x428` (dwSurfaceLockCount) ++.
* 162–180: callback choice: HAL Lock for non-system-memory surfaces without LCL HELCB, else
  the HEL Lock (`lpDDCB+0x144`, = FUN_73751ce8).
* 185: flags passed to the callback: `dwFlags & 0x3830` (READONLY, WRITEONLY, NOSYSLOCK,
  NOOVERWRITE, DISCARDCONTENTS).
* 198–281: call loop: callback returns DDERR_WASSTILLDRAWING and WAIT is set → retry; without
  WAIT → that code is returned. Callback not handled → `*lpSurfData = 0x80000000`, undo,
  DDERR_SURFACEBUSY (205–218). Callback error → undo, return it (242–256).
  HEL returning 0xFFBADBAD (primary not lockable through DCI) with internal flag 0x400 →
  DDERR_CANTLOCKSURFACE (238–240).
* 261–263: the rect node remembers the returned pointer (used by v1–3 Unlock).
* 264–275: **if "hold the lock" and DDLOCK_NOSYSLOCK is not set, the function returns with the
  DirectDraw critical section still entered** (and `DD_GBL+0x464`++): it is released by the
  matching Unlock (X:FUN_737333e1_737333e1.c:211–215 → FUN_73738008). Other threads calling
  any DirectDraw method block until then. With NOSYSLOCK the node (or GBL, for a whole lock)
  is marked LOCKNOTHOLDINGWIN16LOCK and the section is released normally.

HEL Lock (X:FUN_73751ce8_73751ce8.c, FUN_73751391): base = `GBL->fpVidMem` for system-memory
surfaces; for others the NT alias (`GBL_MORE->fpNTAlias`); for the emulated primary
(fpVidMem == 0xFFBADBAD) DCIBeginAccess on the rect (or the whole surface) — failure →
DDERR_CANTLOCKSURFACE (driver/DCI-dependent; LCL DCILOCK set while held). With a rect:
`base + top * lPitch + xoff`, `xoff` = left·bpp/8 for bpp ∈ {1, 2, 4, 8, 16, 24, 32}
(bpp = the surface's DDPIXELFORMAT dwRGBBitCount when it has one, else the display's).
For any other bpp (FourCC: DXTn, YUV) xoff is garbage (the compiler reuses the LOCKDATA
pointer, disassembly 0x73751da1), so rect locks of FourCC surfaces through the HEL return
nonsense on XP; real hardware does this in the driver.

### 1.3 Returned description (FUN_73733184), both paths

X:FUN_73733184_73733184.c. The caller's structure is **not cleared first** (unlike
GetSurfaceDesc, which zeroes it: FUN_73734fe9). Fields Lock does not write keep whatever the
caller had there: dwAlphaBitDepth, dwReserved, dwBackBufferCount and dwMipMapCount/
dwRefreshRate (unless their flags are set), ddckCKDestBlt/ddckCKSrcBlt (unless keyed).
dwSize is left alone.

* dwFlags starts as DDSD_CAPS (12); ddsCaps.dwCaps = LCL caps (13).
* dwSize > 0x7B (DDSURFACEDESC2): dwCaps2/3/4 from MORE (16–18); dwTextureStage written (0, or
  the stage with DDSD_TEXTURESTAGE when bit 0x100000 is set — note this branch *replaces*
  dwFlags with CAPS|TEXTURESTAGE, 19–25); dwFVF with DDSD_FVF when non-zero (26–29).
* lpSurface = NULL (31), set by Lock afterwards (IDirectDrawSurface7_Lock.c:208).
* DDSD_CKDESTBLT / DDSD_CKSRCBLT and the key when the surface has one (32–41).
* DDSD_BACKBUFFERCOUNT and the count only on the surface created as the front buffer
  (LCL FRONTBUFFER, 42–45). DDSD_MIPMAPCOUNT and the count for DDSCAPS_MIPMAP (46–49).
* Not FourCC: DDSD_WIDTH|HEIGHT|PITCH (52–55) (execute buffers: WIDTH|PITCH, height 0, width =
  size). FourCC: WIDTH|HEIGHT and LINEARSIZE for DXT1–5, PITCH for other FourCCs (63–76).
  lPitch = GBL lPitch (77) — ddraw.dll's pitch, or the linear size.
* Pixel format: DDSD_PIXELFORMAT with the surface's format, or the display format when the
  surface has none (79–94). Z buffers: format copied, and with a 0x6C structure
  DDSD_ZBUFFERBITDEPTH + dwZBufferBitDepth **instead of** DDSD_PIXELFORMAT (95–111).
* Overlays only: CKDESTOVERLAY/CKSRCOVERLAY (126–145); non-overlays get those keys zeroed.
* DDSD_LPSURFACE is never set.

Lock then sets, on success: GBL READONLYLOCKHELD = (dwFlags & DDLOCK_READONLY) (199–206).

### 1.4 Fast path (DirectDraw7 system-memory textures)

IDirectDrawSurface7_Lock.c:95–196: for the textures of 1.1's fast-path condition, Lock skips
the OPTIMIZED, flag, descriptor-size, process-id and **rectangle** checks (Wine
test_lockrect_invalid: invalid rects succeed on sysmem and managed textures, "todo_wine"):

* usage ≠ 0 → DDERR_SURFACEBUSY (102–105): no nesting at all, not even disjoint rects, and
  not while a DC is out.
* pointer = fpVidMem, or `fpVidMem + top·pitch + xoff` with xoff from bpp as above (YUY2/UYVY:
  left rounded down to even) — out-of-range rects give out-of-range pointers.
* usage++, GBL flags: clear 0x600, set FASTLOCKHELD (185–194); no rect node, no lock held.
* NULL lpDesc crashes (line 101 is outside the SEH frame; Wine notes "Crashes in ddraw7").

### 1.5 Flags summary

| Flag | XP effect (emulated / system-memory path) |
|------|------|
| WAIT / DONOTWAIT | only matters for DDERR_WASSTILLDRAWING from a driver/video port; v7 implies WAIT |
| READONLY | sets READONLYLOCKHELD → Unlock leaves the contents stamp (GetUniquenessValue) alone; skips texture dirty tracking |
| WRITEONLY, NOOVERWRITE, DISCARDCONTENTS | passed to the driver only |
| NOSYSLOCK | lock does not keep the DirectDraw critical section (non-sysmem surfaces) |
| NODIRTYUPDATE | accepted, no effect in ddraw.dll |
| EVENT and every other bit | DDERR_INVALIDPARAMS |

---------------------------------------------------------------------------------------------

## 2. Unlock: X:IDirectDrawSurface7_Unlock.c (v4/v7, LPRECT), X:IDirectDrawSurface3_Unlock.c (v1–3, LPVOID)

Both call FUN_73733308(This, lpSurfaceData, lpRect) (v1–3 pass (ptr, NULL), v4/7 (0, rect)).

### 2.1 Validation, in XP's order (X:FUN_73733308_73733308.c)

| # | Condition | Result | Line |
|---|-----------|--------|------|
| — | GBL FASTLOCKHELD (fast-path lock): no checks; usage--, flag cleared, DD_OK (then 2.3) | — | 17, 37–41 |
| 1 | DDSCAPS_OPTIMIZED | 0x8876028F | 18–20 |
| 2 | other process, or usage == 0 | DDERR_NOTLOCKED | 21–24 |
| 3 | usage > 1 and neither pointer nor rect given (NULL) | DDERR_INVALIDRECT | 25–27 |
| 4 | LCL HASDC, not DDSCAPS_OWNDC, interface v3/v4/v7 | DDERR_GENERIC | 28–32 |
| 5 | FUN_737333e1 (2.2) | its code | 35 |

Usage counts the GetDC too, so: Lock + GetDC + Unlock(NULL) → DDERR_INVALIDRECT (any
version); GetDC + Unlock → DDERR_GENERIC on v3+, but on v1/v2 it **succeeds and removes the
DC's share of the usage count** (HASDC stays set; a later ReleaseDC decrements again).

### 2.2 FUN_737333e1 (X:FUN_737333e1_737333e1.c)

* usage == 0 → DDERR_NOTLOCKED (89–91).
* rect list empty (whole-surface lock or DC): **any pointer/rect is accepted** (100–105);
  GBL LOCKNOTHOLDINGWIN16LOCK/LOCKVRAMSTYLE/LOCKBROKEN cleared.
* rect list non-empty:
  * NULL pointer and NULL rect: more than one node → DDERR_INVALIDRECT; one node → it (if the
    owner is this DirectDraw object, else DDERR_NOTLOCKED) (106–115).
  * v1–3 pointer: the node whose returned lpSurface equals it (119–126).
  * v4/7 rect: the node whose rectangle is identical (memcmp of the 4 LONGs) (127–146).
  * no match, or owner is another DirectDraw object → DDERR_NOTLOCKED (148–153, 155).
* node unlinked and freed; usage--; dwSurfaceLockCount-- (non-SYSMEMREQUESTED) (155–176).
* driver/HEL Unlock callback; its ddRVal is the return value (177–210).
* critical section held by the Lock released (211–215).

### 2.3 After the unlock (FUN_73733308)

* Not a READONLY lock → GBL_MORE dwContentsStamp++ (skipping 0) (42–53); READONLYLOCKHELD cleared.
* **Surface lost → DDERR_SURFACELOST**, after having unlocked it (55–57).

Wine (test_lockrect_invalid): locking a second, different rect succeeds and then
Unlock(NULL) fails — consistent with row 3.

---------------------------------------------------------------------------------------------

## 3. GetDC(lphDC): X:IDirectDrawSurface7_GetDC.c, FUN_73738101

### 3.1 Validation, in order

| # | Condition | Result | Line |
|---|-----------|--------|------|
| 1 | DDSCAPS_OPTIMIZED or DDSCAPS_EXECUTEBUFFER | DDERR_INVALIDSURFACETYPE | 21, 47–49 |
| 2 | surface lost | DDERR_SURFACELOST | 22, 43–45 |
| 3 | HASDC and not OWNDC | DDERR_DCALREADYCREATED (*lphDC untouched) | 23–25 |
| — | `*lphDC = NULL` (NULL lphDC faults → SEH → DDERR_INVALIDPARAMS) | — | 27 |
| 4 | OWNDC with a DC already made → that DC, DD_OK (no lock taken) | | 29, 32–35 |
| 5 | FUN_73738101 | | 30 |

**No check of the lock state**: GetDC on a locked surface works (Wine test_getdc: Lock,
GetDC, ReleaseDC, Unlock all succeed). No process-id check.

### 3.2 FUN_73738101 (X:FUN_73738101_73738101.c)

* Ordinary surfaces (not a system-memory primary): kernel surface created on demand
  (CompleteCreateSysmemSurface, 25); `GdiEntry7` = DdGetDC(LCL, colour table) where the
  colour table is **the surface's own palette's lpColorTable, or NULL** — a primary never
  passes its palette (27–33). With NULL, win32k chooses the DIB colour table (kernel-
  dependent; Wine's tests show the primary's / system palette on an 8-bit surface without
  one, and the surface's palette overriding it).
* Emulated (system-memory) primary: the DirectDraw object's screen DC (recreated with
  CreateDC on the device name when DDRAWILCL_DIRTYDC, with the primary palette's HPALETTE
  selected and realized) — GDI draws on the screen, not in the surface (36–47).
* No DC → DDERR_CANTCREATEDC (51–52). Registering the DC fails → DdReleaseDC,
  *lphDC = NULL, DDERR_OUTOFMEMORY (55–61).
* Success: LCL HASDC (63). Not OWNDC: **usage++** (and dwSurfaceLockCount for non-SYSMEMREQUESTED)
  **and the DirectDraw critical section is entered once more and kept until ReleaseDC**
  (64–78). OWNDC: `LCL->hDC` = the DC, no usage (80).
* The DC is recorded in a global (hdc, LCL) list used by ReleaseDC and GetSurfaceFromDC
  (FUN_737381ed).

Lock while a DC is out: refused by the usage rules (1.2), DDERR_SURFACEBUSY — except on OWNDC
surfaces, whose DC does not count.

---------------------------------------------------------------------------------------------

## 4. ReleaseDC(hDC): X:IDirectDrawSurface7_ReleaseDC.c, FUN_73738321

| # | Condition | Result | Line |
|---|-----------|--------|------|
| 1 | LCL lacks HASDC | DDERR_NODC (regardless of any lock) | 81–83 |
| 2 | DDSCAPS_OPTIMIZED | 0x8876028F | 84, 123–125 |
| 3 | hDC not in the DC list or registered for another surface, and the interface is v3/v4/v7 | DDERR_INVALIDPARAMS | 86–96, 121 |
| — | **v1/v2: a wrong (or NULL) hDC is accepted and the surface's real DC is released** | | 95–96 |
| 4 | OWNDC: hDC == LCL->hDC → DD_OK (DC kept), else DDERR_INVALIDPARAMS | | 97, 111–116 |
| 5 | FUN_73738321: usage--, critical section left (the one GetDC kept); DdReleaseDC failure → DDERR_GENERIC; HASDC cleared; list entry removed (by LCL or hDC) | | X:FUN_73738321:107–126 |
| — | GBL_MORE dwContentsStamp++ | | 99–109 |

No lost or process-id check.

---------------------------------------------------------------------------------------------

## 5. IsLost(): X:IDirectDrawSurface7_IsLost.c

* lost → DDERR_SURFACELOST (23, 30–34).
* OPTIMIZED with GBL MEMFREE → DDERR_NOTLOADED (0x88760259) (24–26).
* else DD_OK. Nothing else is consulted: loss is a flag set by the events of §9 (the
  uniqueness check at entry can set it on this very call).

---------------------------------------------------------------------------------------------

## 6. Restore(): X:IDirectDrawSurface7_Restore.c

### 6.1 Validation, in order

| # | Condition | Result | Line |
|---|-----------|--------|------|
| 1 | LCL ISFREE (released) | DDERR_INVALIDOBJECT | 51–57 |
| 2 | **not lost → DD_OK immediately** (no mode or exclusivity check) | DD_OK | 58–61 |
| 3 | DDSCAPS_OPTIMIZED | 0x8876028F | 63–66 |
| 4 | LCL IMPLICITCREATE (back buffers, mip sublevels, cube faces created with their root) | DDERR_IMPLICITLYCREATED | 67–70 |
| 5 | the display mode the surface was created in (8-byte record at MORE+0x64: width, height, bpp, + refresh/other bytes, copied from `DD_GBL+0x65C` at creation, X:FUN_73737235:597–598) differs from the current one (`DD_GBL+0x65C`) — **for every surface type, not only the primary** | DDERR_WRONGMODE | 71–88, 166–167 |
| 6 | another DirectDraw object holds exclusive mode | DDERR_WRONGMODE | 89, 161–167 |
| 7 | nobody holds exclusive mode but this object's LCL still has HASEXCLUSIVEMODE (exclusive app in the background) | DDERR_WRONGMODE | 91–93 → 166 |
| 8 | a mode change is in progress (DD_GBL 0x400000): v3/v4/v7 → DDERR_WRONGMODE; v1/v2 proceed (debug warning) | DDERR_WRONGMODE | 95–99 |
| 9 | primary, no exclusive owner, and the primary's description is not compatible with the current mode (FUN_7373ab89) | DDERR_INCOMPATIBLEPRIMARY | 101–110 |
| 10 | primary, exclusive mode exists but not ours | DDERR_NOEXCLUSIVEMODE (unreachable after 6) | 112–115 |

(FUN_73734625 = "who owns exclusive mode": `local_20` = someone does, `local_2c` = it's us.)

### 6.2 Effect

* Primary: if this surface is not the GDI surface (ISGDISURFACE), the flip chain is walked and
  the kernel surface / flags of the GDI surface are swapped onto it, so the front buffer is
  the GDI surface again (119–140) — flip order reset (driver-level; contents are undefined
  anyway).
* FUN_73761201 (X:FUN_73761201_73761201.c): repeats the mode comparison (38–53); if the
  surface kept its memory (GBL not MEMFREE, or a system-memory GDI surface, or optimized)
  only the lost flag is cleared (61–65) — **contents survive**; otherwise video memory is
  reallocated (pitch recomputed, driver CreateSurface or the heap) and the flag cleared only
  on success (66–113): contents undefined.
* LCL IMPLICITROOT → the implicitly created attachments are restored recursively
  (FUN_737613c1). A restored root therefore restores its back buffers/mip levels; restoring a
  back buffer directly is refused (row 4).
* `lpDDCBtmp+0x378` set (D3D driver hook) → FUN_7375febe instead (driver).

---------------------------------------------------------------------------------------------

## 7. PageLock(dwFlags) / PageUnlock(dwFlags): X:IDirectDrawSurface7_PageLock.c

One function for both, all versions:

| # | Condition | Result | Line |
|---|-----------|--------|------|
| 1 | dwFlags ≠ 0 | DDERR_INVALIDPARAMS | 59, 70–72 |
| 2 | surface lost | DDERR_SURFACELOST | 60, 68 |
| 3 | DDSCAPS_OPTIMIZED | 0x8876028F | 61–63 |
| — | else DD_OK; nothing is counted (no DDERR_NOTPAGELOCKED on NT) | DD_OK | 58, 64–66 |

---------------------------------------------------------------------------------------------

## 8. Process-id checks

Only Lock (→ DDERR_SURFACEBUSY, X:IDirectDrawSurface7_Lock.c:80–84) and Unlock
(→ DDERR_NOTLOCKED, X:FUN_73733308:21–24) compare `LCL->dwProcessId` with the caller, and
Unlock also matches the rect node's owner DirectDraw object. Surfaces can't cross processes
in dd2d9: not applicable.

---------------------------------------------------------------------------------------------

## 9. Surface loss (what sets the lost flag)

InvalidateSurface = X:FUN_73760541_73760541.c (only if not already lost):

* video-memory surface (not a system-memory execute buffer): driver-managed surfaces without
  DDSCAPS2_DONOTPERSIST are handed to the driver and **not** lost (11–14); others have their
  video memory freed (FUN_73734478, 15).
* the lost flag is set (and the contents stamp bumped) when `(GBL lacks SYSMEMREQUESTED or LCL
  PARTOFPRIMARYCHAIN) and not a sysmem execute buffer` (17–29). So: every surface not
  explicitly created in system memory, plus every member of a primary flip chain even in
  system memory. Explicit system-memory surfaces (textures, offscreen, Z buffers) are never
  lost.

Callers: InvalidateAllSurfaces (FUN_73760648: every surface of the DirectDraw global object)
from the display-data refresh after a mode/uniqueness change (FUN_737391bd ← FUN_7373c175, run
from any method's entry check), from exclusive-mode activation handling (FUN_7375ac27,
FUN_7375d5aa ← SetCooperativeLevel / app deactivation); InvalidatePrimary (FUN_73760607:
the primary and its implicit chain) when a new primary is created in exclusive mode
(X:FUN_73737235:245).

---------------------------------------------------------------------------------------------

## Differences from dd2d9

Deliberate differences (lost surfaces keep contents, window mode not losing surfaces,
GetDC refusing red-low 32-bit, no WASSTILLDRAWING) are not repeated. XP (§9, §6.2) itself
keeps the contents of surfaces that weren't in video memory and returns no WASSTILLDRAWING
on the emulated path, so those two choices are close to XP anyway.

1. **Lock doesn't validate dwFlags** — edge case.
   XP: any bit outside 0x00017831 → DDERR_INVALIDPARAMS, before the size check
   (X:IDirectDrawSurface7_Lock.c:67, 216). dd2d9: `flags` only traced
   (D:surface.cpp:888–920). Fix: in Surface::Lock (normal path only, after the lost check):
   `if (flags & ~0x17831u) return DDERR_INVALIDPARAMS;`. A game passing DDLOCK_EVENT or
   garbage gets the error on XP.

2. **Overlapping rectangle locks are allowed** — edge case (Wine-tested).
   XP: a rect lock intersecting an outstanding rect lock → DDERR_SURFACEBUSY; disjoint ones
   nest; empty rects never conflict (X:FUN_737336a9:92–98). Wine test_lockrect_invalid:
   locking the same rect twice → SURFACEBUSY. dd2d9: a second rect lock is refused only if
   the *bounding box* of the earlier locks covers the whole surface (D:surface.cpp:894–896),
   so the same rect twice succeeds, and three disjoint rects can be wrongly refused.
   Fix: keep a list of outstanding lock rects (whole-surface lock = a flag) and apply XP's
   rules; it also serves 3 and 4.

3. **Unlock ignores its argument** — edge case.
   XP (X:FUN_73733308:25–27, X:FUN_737333e1:100–153): with rect locks outstanding, v4/7
   Unlock(rect) must match a locked rect exactly and v1–3 Unlock(ptr) the pointer that Lock
   returned, else DDERR_NOTLOCKED; NULL with more than one lock (counting a DC) →
   DDERR_INVALIDRECT. With a whole-surface lock any argument is accepted. dd2d9:
   `Unlock(typename T::UL)` drops it (D:wrappers.h:121), Surface::Unlock just decrements
   (D:surface.cpp:922–929). Fix: pass the rect/pointer down and match against the list from 2.

4. **Interaction of locks and DCs** — edge case.
   XP counts a non-OWNDC DC as one more lock (X:FUN_73738101:64–70, FUN_73738321:107–113):
   (a) Unlock while a DC is out: DDERR_INVALIDRECT when the surface is also locked and the
   argument is NULL; otherwise DDERR_GENERIC on v3/v4/v7; on v1/v2 it succeeds and drops the
   DC's share (X:FUN_73733308:25–35). dd2d9: unlocks a lock if any, else DDERR_NOTLOCKED
   (D:surface.cpp:922–929).
   (b) ReleaseDC with no DC out always returns DDERR_NODC (X:IDirectDrawSurface7_ReleaseDC.c:81–83).
   dd2d9 instead releases an outstanding *lock* and returns DD_OK (D:surface.cpp:761–768). The
   comment there says this is ddraw.dll's behavior; the XP code doesn't do it. The nearest
   XP sequence is v1/v2: GetDC, Unlock (succeeds and drops the DC's lock), Lock (now allowed),
   ReleaseDC (the lock count goes back to 0). *Uncertain:* check which game needed the current
   code before changing it.
   Fix: give the surface XP's single usage count (locks + non-OWNDC DC), apply rows 3–4 of
   §2.1 and §4 row 1, and drop the special case.

5. **ReleaseDC with a wrong DC on v1/v2** — game-visible (old games).
   XP: v1/v2 accept any hDC (NULL, stale, another surface's) and release this surface's DC
   (X:IDirectDrawSurface7_ReleaseDC.c:95–96); v3+ → DDERR_INVALIDPARAMS. dd2d9: always
   DDERR_INVALIDPARAMS (D:surface.cpp:770), so the DC stays out and every later Lock/Blt on the
   surface fails busy. Fix: pass V into ReleaseDC (D:wrappers.h:115) and, for V ≤ 2, ignore a
   mismatch.

6. **GetDC on a lost surface succeeds** — game-visible (low).
   XP: DDERR_SURFACELOST after the type check and before DCALREADYCREATED
   (X:IDirectDrawSurface7_GetDC.c:22, 43–45). dd2d9 has no lost check (D:surface.cpp:733–757).
   Games that draw text with GDI each frame and restore on that error never see it.
   Fix: `if (IsLost()) return DDERR_SURFACELOST;` before the dcOut check (contents are kept,
   so only the return code changes).

7. **Unlock on a lost surface returns DD_OK** — edge case.
   XP unlocks, then returns DDERR_SURFACELOST (X:FUN_73733308:55–57). dd2d9: DD_OK
   (D:surface.cpp:922–929). Fix: after the decrement, `return IsLost() ? DDERR_SURFACELOST : DD_OK;`.

8. **Lock failures leave lpSurface as it was** — edge case (Wine-tested).
   XP zeroes lpDesc->lpSurface once the size check passes, so every later failure
   (invalid rect, SURFACEBUSY, …) returns it NULL (X:IDirectDrawSurface7_Lock.c:79; Wine
   test_lockrect_invalid `ok(!locked_desc.lpSurface)`). dd2d9 doesn't touch the structure on
   failure (D:wrappers.h:109–113). Fix: in the wrapper, after the size check, set
   `d->lpSurface = nullptr` before calling Surface::Lock (not for SURFACELOST, which XP
   returns earlier; see 9).

9. **Lock check order** — edge case.
   XP order: lost → (fast path) → flags → dwSize (NULL desc faults → INVALIDPARAMS) → rect
   (X:IDirectDrawSurface7_Lock.c:49–93). dd2d9 checks NULL/dwSize in the wrapper first
   (D:wrappers.h:101–108), then lost (D:surface.cpp:890), then busy, then the rect
   (D:surface.cpp:894–903). XP checks the rect before busy, dd2d9 after. Visible
   only with several errors at once (lost + bad desc → XP SURFACELOST, dd2d9 INVALIDPARAMS;
   busy + invalid rect → XP INVALIDPARAMS, dd2d9 SURFACEBUSY). Fix: reorder as XP.

10. **Empty lock rectangles are refused** — edge case (Wine-tested).
    XP accepts left == right or top == bottom (X:IDirectDrawSurface7_Lock.c:87–89; Wine
    `valid[]` includes {60,60,60,68} and {60,60,68,60}). dd2d9: `r->left >= r->right ||
    r->top >= r->bottom` → INVALIDPARAMS (D:surface.cpp:901–903). Fix: use `>`.

11. **Fast path for DirectDraw7 system-memory textures is only partly modelled** — edge case.
    XP (§1.4): no rect validation (invalid rects succeed: out-of-range pointer; Wine
    "todo_wine" DD_OK), no flag check, and **no nesting** (any second lock or a lock while a
    DC is out → SURFACEBUSY). dd2d9 relaxes only the descriptor size (D:wrappers.h:103–108),
    still validates the rect and allows disjoint rect locks. Fix: in Surface::Lock, when
    (ddVersion == 7 && TEXTURE && SYSTEMMEMORY), skip the flag/rect checks and refuse any
    lock while lockCount or dcOut. Clamp the pointer so it stays inside the buffer, or keep
    the rect check and accept the mismatch. *Uncertain:* XP's condition is GBL SYSMEMREQUESTED
    (explicit system memory). Whether D3D-managed textures get it on XP is not shown here
    (Wine says they behave this way on modern Windows).

12. **Lock overwrites fields of the caller's description that XP leaves alone** — edge case.
    XP fills the structure in place without clearing it (X:FUN_73733184, no memset; compare
    GetSurfaceDesc's FUN_73734fe9). dwAlphaBitDepth, dwReserved, and dwBackBufferCount /
    dwMipMapCount / colour keys when their flag isn't set keep the caller's values. dd2d9
    memsets a DDSURFACEDESC and copies a whole DDSURFACEDESC2 (D:util.cpp:605–627 via
    D:wrappers.h:111). It also derives dwFlags from the stored creation description
    (D:surface.cpp:821–845), while XP computes them (CAPS|WIDTH|HEIGHT|PITCH|PIXELFORMAT,
    + BACKBUFFERCOUNT only on the front buffer, MIPMAPCOUNT only for mipmaps, CK* when keyed,
    TEXTURESTAGE/FVF for 0x7C). Fix: give Lock its own fill (FUN_73733184's rules) instead of
    GetSurfaceDesc + DescOut. The flags question belongs to the GetSurfaceDesc audit too.

13. **READONLY locks change the uniqueness value** — edge case.
    XP: Unlock of a DDLOCK_READONLY lock doesn't bump the contents stamp
    (X:FUN_73733308:42–54) that GetUniquenessValue reports. dd2d9: `uniqueness++` on every
    Unlock (D:surface.cpp:927). Fix: remember READONLY per lock and skip the increment and
    MarkDirtyIfVisible for it (this also saves re-presenting the primary after read-backs).

14. **Restore: mode check for every lost surface; non-lost surfaces return DD_OK first** — game-visible (low).
    XP: not lost → DD_OK before any check; lost surfaces of *any* type created in another
    display mode (width, height, bpp, refresh) → DDERR_WRONGMODE
    (X:IDirectDrawSurface7_Restore.c:58–88). dd2d9: only the primary is compared, against its
    buffer's size/format, and that comparison runs before the lost check, so a non-lost primary
    can return WRONGMODE (D:surface.cpp:391–398). After a mode change on XP, Restore on old
    offscreen/vidmem surfaces fails and the game must recreate them, picking up the new display
    format. On dd2d9 they restore with the old format, so a game that assumes the new
    format (made without DDSD_PIXELFORMAT) can draw garbage. Fix: record (w, h, bpp, refresh)
    at creation; in Restore: `if (!IsLost()) return DD_OK;`, then compare for all surfaces.

15. **Restore of an implicitly created surface succeeds** — edge case.
    XP: back buffers, mip sublevels and cube faces made with their root → DDERR_IMPLICITLYCREATED;
    only the root restores them (X:IDirectDrawSurface7_Restore.c:67–70, FUN_737613c1). dd2d9
    restores them and, for a flip-chain member, *every* member including the primary
    (D:surface.cpp:403–408). Wine (ddraw1.c:980) accepts both. Fix: if the surface was
    created implicitly (has a root, not attached explicitly), return DDERR_IMPLICITLYCREATED
    when lost.

16. **Which surfaces can be lost** — edge case, *partly uncertain*.
    XP: not lost if GBL SYSMEMREQUESTED, unless part of the primary chain
    (X:FUN_73760541:17–19). dd2d9: lost = not SYSTEMMEMORY, or primary, or Z buffer
    (D:surface.cpp:382–385). Two mismatches: (a) back buffers of a system-memory primary chain
    are lost on XP but not in dd2d9; (b) a Z buffer the game *explicitly* created with
    DDSCAPS_SYSTEMMEMORY is never lost on XP (if SYSMEMREQUESTED tracks explicit requests,
    which this audit didn't confirm), whereas dd2d9 loses it. The EMULATIONONLY test cited in
    the dd2d9 comment is consistent with XP if those Z buffers got SYSTEMMEMORY implicitly.
    Fix: track "explicit system memory" at creation and use XP's rule.

17. **PageLock/PageUnlock always succeed** — edge case.
    XP: dwFlags ≠ 0 → DDERR_INVALIDPARAMS; lost → DDERR_SURFACELOST (X:IDirectDrawSurface7_PageLock.c:59–72).
    dd2d9: DD_OK (D:wrappers.h:127–128). Fix: `if (f) return DDERR_INVALIDPARAMS; Guard l;
    return s->IsLost() ? DDERR_SURFACELOST : DD_OK;`.

18. **DDSCAPS_OWNDC isn't implemented** — edge case.
    XP: an OWNDC surface keeps its DC: a second GetDC returns the same DC, ReleaseDC only
    checks it (wrong one → INVALIDPARAMS), and the DC doesn't lock the surface (Lock works
    while it's out) (X:IDirectDrawSurface7_GetDC.c:23–35, ReleaseDC.c:97–116). dd2d9 accepts
    the cap at creation (nothing in src/ refers to DDSCAPS_OWNDC) but treats it as an ordinary
    DC: second GetDC → DDERR_DCALREADYCREATED, Lock while out → SURFACEBUSY. Fix: honour the cap
    in GetDC/ReleaseDC/Lock, or refuse it at CreateSurface if that is what XP's HEL does
    (not checked).

19. **The lock doesn't hold DirectDraw's critical section** — edge case, no change suggested.
    XP keeps the critical section from Lock to Unlock for non-system-memory surfaces without
    DDLOCK_NOSYSLOCK (X:FUN_737336a9:264–268). It keeps it from GetDC to ReleaseDC for every
    non-OWNDC surface (X:FUN_73738101:71–77). Other threads' DirectDraw calls block meanwhile.
    dd2d9 releases its lock on return. That is safer (no cross-thread deadlocks); it differs
    only for multi-threaded games that rely on the blocking.

20. **GetDC's NULL check comes first** — edge case.
    XP checks type, lost and DCALREADYCREATED before writing *lphDC (a NULL pointer then
    faults → INVALIDPARAMS) (X:IDirectDrawSurface7_GetDC.c:21–27). dd2d9 returns INVALIDPARAMS
    first (D:surface.cpp:734). Only matters with NULL plus another error.

### Agreements checked (no change)

* Lock: either descriptor size on every version; DDSD_LPSURFACE not set; Z buffers in a
  DDSURFACEDESC get DDSD_ZBUFFERBITDEPTH; whole lock while locked → SURFACEBUSY; Lock while a
  DC is out → SURFACEBUSY; GetDC while locked succeeds (Wine test_getdc).
* GetDC: DCALREADYCREATED leaves *lphDC alone; CANTCREATEDC sets it NULL; palette choice
  (own palette, else primary's/system) matches Wine's observations and XP's NULL-table call;
  the colour table is refreshed on each GetDC.
* ReleaseDC with no DC and no lock → DDERR_NODC.
* IsLost: SURFACELOST / DD_OK; Restore of a system-memory surface → DD_OK; Restore while the
  exclusive game is in the background → DDERR_WRONGMODE (Wine test_lost_device).
* Driver-dependent XP behaviour not modelled, correctly: HAL lock callbacks, DCI primary
  access (CANTLOCKSURFACE), WASSTILLDRAWING, video ports, the screen-DC GetDC of an emulated
  primary, the flip-order reset in Restore, FourCC rect-lock offsets.
