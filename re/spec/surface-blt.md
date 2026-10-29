# IDirectDrawSurface: Blt, BltFast, BltBatch, GetBltStatus

Reference: Windows XP SP3 ddraw.dll 5.3.2600.5512. One function body implements all five
interface versions (1/2/3/4/7) of each method; where the version matters the code compares an
interface's `lpVtbl` with the vtables (Surface 0x7376f100, Surface2 0x7376f300,
Surface3 0x7376f260, Surface4 0x7376f1a0, Surface7 0x7376f020).

File references: `X:<file>:<line>` = Ghidra output in `$S/decomp-annotated/` (S = the audit
scratchpad); `A:<addr>` = raw disassembly of the DLL (`objdump -d`), used where the decompiler
lost the return code (in Blt every error path is `goto LAB_73747b22` and the HRESULT is set in
`%esi` by a cold block, so all Blt return codes below were read from the disassembly);
`D:<file>:<line>` = dd2d9 `src/` as of 2026-10-29 (blit.cpp md5 de9ba46a…; other agents were
editing src/ during the audit, so if line numbers moved, the function names still apply).

Names: LCL/GBL = `_DDRAWI_DDRAWSURFACE_LCL/_GBL`; pdrv = `_DDRAWI_DIRECTDRAW_GBL`. LCL flags:
INVALID 0x10000000 (= lost), HASCKEYDESTBLT 0x200, HASCKEYSRCBLT 0x800, HASPIXELFORMAT 0x2000,
HELCB 0x02000000. "Usage" = `GBL->dwUsageCount` (raised by Lock and by GetDC on non-OWNDC
surfaces, see surface-lock-dc.md §3). `0x8876028F` (code 655, unnamed in public headers) is
returned for DDSCAPS_OPTIMIZED surfaces; dd2d9 has none, so those rows never apply.

Every method enters the DirectDraw critical section and, at the outermost level, refreshes the
display data if the display-settings uniqueness changed (that is where a mode change loses
surfaces, surface-lock-dc.md §9).

## 0. How XP decides HAL vs. HEL, and the HEL's capabilities

* FUN_73735ff9 (X:FUN_73735ff9_73735ff9.c) picks the caps sets used for validation: pointers to
  {dwCaps, dwFXCaps, dwCKeyCaps, dwRops} for the driver (HAL), the emulation layer (HEL) and the
  "both" set. If either surface is in system memory and the driver lacks DDCAPS_CANBLTSYSMEM, the
  blit is emulated; with CANBLTSYSMEM the SVB/VSB/SSB caps are used and the driver does it.
* Each feature check below follows the same pattern ("caps check"): if the *both* set has the
  bit → OK; else if the driver has it → the blit becomes hardware-only; else if the HEL has it →
  the blit becomes emulated; else → the feature's error code. A blit that ends up both
  hardware-only and emulated → DDERR_NOBLTHW (X:IDirectDrawSurface7_Blt.c:1010, A:737488ee).
* If the driver has no Blt callback (`lpDDCB+0xcc == NULL`) the blit is emulated
  (X:…Blt.c:1007–1009).
* HEL caps (X:FUN_7373bdf9_7373bdf9.c:101–118, when emulation is available):
  dwCaps 0xF4C08241 (BLT, BLTSTRETCH, COLORKEY, ALPHA, BLTCOLORFILL, BLTDEPTHFILL, CANCLIP,
  CANCLIPSTRETCHED, CANBLTSYSMEM, …), **dwCKeyCaps 0x200 = DDCKEYCAPS_SRCBLT only** (no dest
  keys), dwFXCaps 0x3FCE3 (stretch/shrink, arith-stretch-Y, mirror LR/UD; **no rotation**),
  dwRops (table at 0x737700b8) = **BLACKNESS, SRCCOPY, WHITENESS only**.
* The HEL Blt (DDHAL callback 0x73752889, X:FUN_73752889_73752889.c):
  * dest formats: RGB / palettized at 8/16/24/32 bpp, or Z buffers (110–146); anything else
    (FourCC/YUV) → "not handled" → Blt returns DDERR_UNSUPPORTED (X:…Blt.c:1594–1596).
  * ROP SRCCOPY requires the **source format to equal the dest format** (pixel-format flags, bit
    count, RGB masks, alpha mask) (162–209), else "not handled" → DDERR_UNSUPPORTED. The HEL
    never converts formats (P8→RGB included).
  * COLORFILL writes `bltFX.dwFillColor`; ROP BLACKNESS writes 0, **WHITENESS writes 0x00FFFFFF**
    (alpha/X byte 0 at 32 bpp) (210–224). DEPTHFILL writes dwFillDepth the same way (285–296).
  * Source key: only `ddckSrcColorkey.dwColorSpaceLowValue` is used (single color, 331–333).
    The compare mask is 0xFFFFFF, or R|G|B for 32-bit bitfield sources (X:FUN_7375892b:33–36),
    i.e. X/alpha ignored at 32 bpp but **bit 15 of a 16-bit X1R5G5B5 pixel is compared**.
  * Dest keys (KEYDESTOVERRIDE) are ignored by the HEL entirely (it has no code for them).
  * Mirroring: MIRRORLEFTRIGHT/UPDOWN swap the dest rect's edges and the core blitter mirrors
    (319–330). The stretch sampling of the core blitters (FUN_737567de etc.) was not decoded.

With dd2d9's reported driver caps (D:ddraw.cpp:987–996: BLT, BLTCOLORFILL, BLTSTRETCH,
COLORKEY, CANBLTSYSMEM, BLTDEPTHFILL, CANCLIP, CANCLIPSTRETCHED; CKey SRCBLT|DESTBLT; FX
stretch/shrink + mirror; **dwRops all zero**) XP's validation would pass every color-key, stretch
and mirror blit, and refuse every ROP except BLACKNESS/WHITENESS (via the HEL) and SRCCOPY.

---------------------------------------------------------------------------------------------

## 1. Blt(lpDestRect, lpSrcSurface, lpSrcRect, dwFlags, lpDDBltFX) — X:IDirectDrawSurface7_Blt.c

SEH frame (scope table 0x73735fd0): a fault while reading parameters or validating (states 0/1,
i.e. everything up to the rectangle checks) → **DDERR_INVALIDPARAMS** (handlers A:73749a76,
A:73749ab7); a fault inside the driver/HEL call → DDERR_EXCEPTION after cleanup (A:73749a4a).
So bad This/src/rect/fx pointers give DDERR_INVALIDPARAMS, not a crash.

### 1.1 Validation, in XP's order

| # | Condition | Result | Where |
|---|-----------|--------|-------|
| 1 | dest LCL INVALID (lost) | DDERR_SURFACELOST | 198–203, A:73747afd |
| 2 | src given and src LCL INVALID | DDERR_SURFACELOST | 1644–1646, A:73747b1d |
| — | dwFlags & 0x80000000 (private flag of the D3D8/9 runtime): window-clip the dest rect, optional vblank wait; then clears 0x8000000B | — | 208–275 |
| 3 | dwFlags & (DDBLT_LAST_PRESENTATION 0x20000000 \| 0x80000000) | DDERR_INVALIDPARAMS | 277, A:73749a86 |
| 4 | src and dest on different display drivers (both pdrv DISPLAYDRV, different pdrv) | DDERR_DEVICEDOESNTOWNSURFACE | 278–283, A:73747dcd |
| 5 | dest or src DDSCAPS_OPTIMIZED | DDERR_INVALIDPARAMS | 284–286 |
| 6 | **IDirectDrawSurface7 only**: DONOTWAIT and WAIT both set → error; else no DONOTWAIT → WAIT is added. v1–4: no check, DONOTWAIT ignored | DDERR_INVALIDPARAMS | 296–308, A:73747de1 |
| 7 | device busy bit already set (another thread inside a blit) | DDERR_SURFACEBUSY | 314–318, A:73747dfd |
| — | src is a video-port surface: wait for the port (WASSTILLDRAWING without WAIT) | driver | 319–346 |
| 8 | **dest or src Usage > 0 (locked, or a DC out on a non-OWNDC surface)** | DDERR_SURFACEBUSY | 354–360, A:73749a5e |
| — | dest GBL_MORE dwContentsStamp bumped (GetUniquenessValue changes **even if the blit then fails**) | — | 361–370 |
| 9 | a display mode change is in progress (pdrv flag 0x400000, set around ChangeDisplaySettings) and dest not in system memory | **DD_OK, nothing drawn** | 371–376, A:73747ed2 |
| 10 | no BLT cap in driver nor HEL | DDERR_NOBLTHW | 393–408, A:73748902 |
| — | FourCC (DXT) and HELCB surfaces force emulation | — | 412–441 |
| 11 | lpDDBltFX NULL and dwFlags & 0x02F75D4A (DEPTHFILL, ZBUFFER*OVERRIDE, ZBUFFER*CONSTOVERRIDE, ROTATIONANGLE, ROP, KEYSRCOVERRIDE, KEYDESTOVERRIDE, DDROPS, DDFX, COLORFILL, ALPHA{SRC,DEST}{CONST,SURFACE}OVERRIDE) | DDERR_INVALIDPARAMS | 443–450, A:7373ed14 |
| 12 | **lpDDBltFX non-NULL and `dwSize != sizeof(DDBLTFX)` (100)** — whatever the flags | DDERR_INVALIDPARAMS | 447–450 |
| 13 | flags (§1.2) | | |
| 14 | rectangles (§1.3) | | |
| 15 | blit needs both a hardware-only feature and emulation | DDERR_NOBLTHW | 1010 |
| 16 | display settings changed since step 0 (mode change) | DDERR_SURFACELOST | 1011–1012, A:73748922 |
| 17 | lazily-created sysmem surface can't be completed | DDERR_GENERIC | 1016–1025, A:73748971 |
| 18 | locking dest/src for the HEL fails (not WASSTILLDRAWING) | that code | 1064–1092 |
| 19 | clip list (§1.4) can't be had (no hwnd and no list → DDERR_NOCLIPLIST; GetDC/region failure → DDERR_GENERIC) | that code | 1187–1215 |
| 20 | driver/HEL returns "not handled" | DDERR_UNSUPPORTED (or, with KEYSRCOVERRIDE and code 2, an emulated retry; failing that DDERR_NOCOLORKEYHW) | 1593–1614 |
| 21 | otherwise the driver/HEL's ddRVal (WAIT retries WASSTILLDRAWING; the HEL never returns it) | | 1130–1148 |

Everything up to 16 happens before any pixel is written. With a clipper the blit runs once per
clip rectangle and stops at the first failure (earlier rectangles stay drawn, 1557).

### 1.2 Flags (step 13), in order (X:…Blt.c:452–907)

If `dwFlags & ~DDBLT_WAIT == 0` (also flags 0): source required, else DDERR_INVALIDPARAMS
(884–890, A:73735bd0 → 7373ed14); the blit is ROP SRCCOPY. Otherwise:

**a. Color keys** (if any of KEYDEST 0x2000, KEYDESTOVERRIDE 0x4000, KEYSRC 0x8000,
KEYSRCOVERRIDE 0x10000):
1. KEYSRCOVERRIDE or KEYDESTOVERRIDE: caps check DDCAPS_COLORKEY → DDERR_NOCOLORKEYHW
   (A:737482bf). KEYSRCOVERRIDE: caps check DDCKEYCAPS_SRCBLT → NOCOLORKEYHW; key =
   `fx->ddckSrcColorkey`. KEYDESTOVERRIDE: caps check DDCKEYCAPS_DESTBLT → NOCOLORKEYHW; key =
   `fx->ddckDestColorkey` (453–518).
2. KEYSRC: src NULL → **DDERR_INVALIDOBJECT** (A:73748191); KEYSRCOVERRIDE also set →
   DDERR_INVALIDPARAMS; src lacks HASCKEYSRCBLT → DDERR_INVALIDPARAMS; caps SRCBLT →
   NOCOLORKEYHW; then KEYSRC is turned into KEYSRCOVERRIDE with the source's CKSrcBlt
   (550–588, A:7374816a–7374822e).
3. KEYDEST: KEYDESTOVERRIDE also set → DDERR_INVALIDPARAMS; **dest** lacks HASCKEYDESTBLT →
   DDERR_INVALIDPARAMS; caps DESTBLT → NOCOLORKEYHW; then the key value is taken from **the
   source surface's CKDestBlt if the *source interface pointer* is an IDirectDrawSurface or
   IDirectDrawSurface2** (vtables 0x7376f100 / 0x7376f300), otherwise from the dest's CKDestBlt
   (v3, v4, v7 sources) (589–628, A:737482c9–7374830f). The source needs no dest key of its own
   (its stored value, possibly stale or zero, is used). With src NULL and KEYDEST: v1/v2 test
   dereferences NULL → SEH → DDERR_INVALIDPARAMS; otherwise falls to "source required" →
   DDERR_INVALIDPARAMS.

**b. Operation** — the first of these that is set decides (631–883):
| Flag | Checks, in order | Result |
|------|------------------|--------|
| ROP 0x20000 | also COLORFILL/DDFX/DDROPS/DEPTHFILL/ROTATIONANGLE → DDERR_INVALIDPARAMS; `fx->dwROP != SRCCOPY` and its bit not in the rops caps → **DDERR_NORASTEROPHW** (A:737483eb); ROP uses a source (table 0x73771568 bit 0) and src NULL → DDERR_INVALIDPARAMS; ROP uses a pattern (bit 1): `fx->lpDDSPattern` lost → DDERR_SURFACELOST, not 8×8 → DDERR_INVALIDPARAMS (892–907) | |
| COLORFILL 0x400 | src non-NULL → DDERR_INVALIDPARAMS; dest is DDSCAPS_ZBUFFER → DDERR_INVALIDPARAMS; caps BLTCOLORFILL → DDERR_UNSUPPORTED (A:73748532). Then **writes back into the caller's DDBLTFX**: `dwFillColor &= 0xFF` at ≤8 bpp, high word cleared at 16 bpp, top byte cleared at 24 bpp (dest's own pixel format if HASPIXELFORMAT, else the display mode's) (815–832) | fill |
| DEPTHFILL 0x2000000 | src non-NULL → DDERR_INVALIDPARAMS; dest **not** ZBUFFER → DDERR_INVALIDPARAMS; caps BLTDEPTHFILL → DDERR_UNSUPPORTED | fill with dwFillDepth |
| DDROPS 0x1000 | also DEPTHFILL/ROTATIONANGLE/DDFX/COLORFILL → INVALIDPARAMS; else **DDERR_NODDROPSHW** always | |
| DDFX 0x800 | also DEPTHFILL/ROTATIONANGLE/COLORFILL → INVALIDPARAMS; dwDDFX & ARITHSTRETCHY → **DDERR_NOSTRETCHHW** always; MIRRORLEFTRIGHT / MIRRORUPDOWN: caps BLTMIRRORLEFTRIGHT / BLTMIRRORUPDOWN → DDERR_NOMIRRORHW; ROTATE90/180/270 (0x70): caps BLTROTATION90 → **DDERR_NOROTATIONHW**; other dwDDFX bits (NOTEARING, ZBUFFERRANGE, …) not checked. Then ROP SRCCOPY. **No source check** (a NULL source reaches the HEL, which faults → DDERR_EXCEPTION; not verified at run time) | copy |
| ROTATIONANGLE 0x40000 | also DEPTHFILL/COLORFILL → INVALIDPARAMS; else **DDERR_NOROTATIONHW** | |
| none of these | src NULL → DDERR_INVALIDPARAMS; ROP SRCCOPY | copy |

Flags not mentioned (ASYNC, WAIT, PRESENTATION, EXTENDED_FLAGS, ZBUFFER, the ALPHA*/ZBUFFER*
flags given an fx) are accepted and passed to the driver; the HEL ignores them (Wine
test_blt_z_alpha: plain copy). Unknown bits are not rejected.

### 1.3 Rectangles (step 14) (X:…Blt.c:909–1001, A:73735bf9–73735d97)

* Dest rect = lpDestRect or the whole dest; src rect = lpSrcRect or the whole source. Without a
  source, lpSrcRect is ignored entirely (Wine test_color_fill "unused source rectangle": OK).
* Dest width or height ≤ 0 (empty or inverted) → DDERR_INVALIDRECT (A:737487ce → 73748918).
* Dest **without a clipper** (`LCL->lpDDClipper`): left < 0, top < 0, right > width or
  bottom > height → DDERR_INVALIDRECT. (Primary on a multi-monitor virtual desktop: checked
  against the desktop rect instead.) **With a clipper: no bounds check** — the clip list does it.
* Source: width or height ≤ 0 → DDERR_INVALIDRECT; outside the source surface →
  DDERR_INVALIDRECT. Always checked when a source is given, whatever the operation (so a
  BLACKNESS/WHITENESS ROP or a DDFX blit with a source still validates lpSrcRect; Wine
  test_color_fill: ROP + source + inverted/negative/out-of-bounds lpSrcRect → INVALIDRECT).
* Sizes differ → stretch: stretch caps (FUN_73762800: BLTSTRETCH, then FX stretch/shrink
  X/Y, integer-ratio variants) → DDERR_NOSTRETCHHW. Mirroring needs no size difference.
* Inverted rectangles are never a way to mirror: they fail as above (Wine test_blt).

### 1.4 Clipper (X:…Blt.c:1160–1576, X:FUN_737363fc_737363fc.c)

* The clip list is fetched with the dest rect: an explicit clip list (SetClipList) is used in
  surface coordinates; an hwnd clipper gives the window's **visible region (GetRandomRgn
  SYSRGN) ∩ client rect, in screen coordinates — for any dest surface, offscreen ones
  included** (no translation to the window's origin). The result is ∩ dest rect, then ∩ the
  surface rectangle (FUN_737364ff with {0,0,w,h}).
* Empty region → DD_OK, nothing drawn (1216–1220).
* Each clip rectangle is blitted with the matching part of the source: offset for 1:1
  blits; for stretches FUN_73762b4c scales the edges with 16.16 factors computed from the whole
  rects; for mirrored blits the sub-source is reflected inside the full source rect
  (1495–1507).

### 1.5 Overlap, primary, lost/busy

* Source == dest: only the dest is locked; nothing in Blt copies the overlapping source. Whether
  the HEL core blitters handle overlap was not decoded; Wine test_blit_to_self (modern Windows)
  expects a correct overlapped result.
* Blits to the primary through the HEL lock the primary directly; on failure the HEL uses GDI
  (StretchBlt/PatBlt on a DC) (X:FUN_73752889:232–284, 336–620). Driver-dependent.
* No DDERR_WASSTILLDRAWING from the HEL.

---------------------------------------------------------------------------------------------

## 2. BltFast(dwX, dwY, lpSrcSurface, lpSrcRect, dwTrans) — X:IDirectDrawSurface7_BltFast.c

**No SEH frame** (A:73762c01): a NULL or bad source pointer is dereferenced at line 96 and
faults in the caller (with the DirectDraw lock held).

| # | Condition | Result | Line |
|---|-----------|--------|------|
| 1 | dest lost | DDERR_SURFACELOST | 95, 566 |
| 2 | src lost | DDERR_SURFACELOST | 99, 566 |
| 3 | `dwTrans & 0xFFFFFFCC` (anything but SRCCOLORKEY 1, DESTCOLORKEY 2, WAIT 0x10, DONOTWAIT 0x20), or src/dest OPTIMIZED | DDERR_INVALIDPARAMS | 100–101, 560–561 |
| 4 | different display drivers | DDERR_DEVICEDOESNTOWNSURFACE | 104–108 |
| 5 | IDirectDrawSurface7 only: DONOTWAIT+WAIT → error; no DONOTWAIT → WAIT added | DDERR_INVALIDPARAMS | 115–123 |
| 6 | device busy | DDERR_SURFACEBUSY | 127–130 |
| — | dest contents stamp bumped | — | 155–162 |
| 7 | **src or dest Usage > 0 (locked / DC out)** | DDERR_SURFACEBUSY | 163, 554 |
| 8 | mode change in progress, dest not sysmem | DD_OK, nothing drawn | 164–165 |
| 9 | no BLT cap | DDERR_NOBLTHW | 193–208 |
| 10 | src rect (NULL = whole source): width/height ≤ 0, or outside the source | DDERR_INVALIDRECT | 238–277 |
| 11 | dest rect = (x, y, x+w, y+h): x < 0 or y < 0 (signed), right > width or bottom > height | DDERR_INVALIDRECT | 279–297 |
| 12 | `dwTrans & 0xF`: 0 → ROP SRCCOPY. 1 → src lacks HASCKEYSRCBLT → INVALIDPARAMS; caps SRCBLT → NOCOLORKEYHW; key = src's CKSrcBlt (KEYSRCOVERRIDE). 2 → **dest** lacks HASCKEYDESTBLT → INVALIDPARAMS; caps DESTBLT → NOCOLORKEYHW; key = **dest's** CKDestBlt on every version (no v1/v2 quirk). **3 (both bits) is accepted** and passes a blit with no operation flag at all to the driver/HEL (result not determined) | | 299–349 |
| 13 | hardware-only + emulation | DDERR_NOBLTHW | 353 |
| 14 | display settings changed | DDERR_SURFACELOST | 354–358 |
| 15 | lazy sysmem completion fails | DDERR_GENERIC | 361–367 |
| 16 | **dest has a clipper** (`LCL->lpDDClipper`), on both paths | DDERR_BLTFASTCANTCLIP | 435, 468–471 |
| 17 | driver/HEL "not handled" → DDERR_UNSUPPORTED (SRCCOLORKEY: emulated retry or NOCOLORKEYHW) | | 484–490 |

The blit itself is a Blt with ROP SRCCOPY (+ the key override); same HEL behaviour as §0. No
stretching, no mirroring, never clipped.

---------------------------------------------------------------------------------------------

## 3. BltBatch(lpDDBltBatch, dwCount, dwFlags) — X:IDirectDrawSurface7_BltBatch.c

Never blits. No SEH frame; dwFlags never read.
1. dest lost → DDERR_SURFACELOST (22, 68). 2. dest Usage > 0 → DDERR_SURFACEBUSY (23, 64).
3. For each entry i < (int)dwCount: src (if non-NULL) lost → SURFACELOST; src Usage > 0 →
   SURFACEBUSY; src OPTIMIZED → 0x8876028F; dest OPTIMIZED → 0x8876028F; `lpDDBltFx` NULL with
   `dwFlags & 0x02F75D4A`, or non-NULL with dwSize != 100 → DDERR_INVALIDPARAMS (25–59).
   lpDDBltBatch NULL with dwCount > 0 faults.
4. Then **DDERR_UNSUPPORTED** (61) — also for dwCount ≤ 0.

## 4. GetBltStatus(dwFlags) — X:IDirectDrawSurface7_GetBltStatus.c

SEH frame. In order:
1. dwFlags not exactly DDGBS_CANBLT (1) or DDGBS_ISBLTDONE (2) (0, 3, other bits) →
   DDERR_INVALIDPARAMS (24, 51).
2. surface lost → DDERR_SURFACELOST (27, 47).
3. device busy bit set → DDERR_SURFACEBUSY (28, 43).
4. system-memory surface and the driver lacks CANBLTSYSMEM → DD_OK (29, 40).
5. OPTIMIZED → 0x8876028F; else the driver's GetBltStatus callback (FUN_7375f5f9; DD_OK when
   there is none, which is the HEL case). Driver-dependent: a pending hardware blit gives
   DDERR_WASSTILLDRAWING for ISBLTDONE.

---------------------------------------------------------------------------------------------

## Differences from dd2d9

Deliberate differences (no WASSTILLDRAWING, palette-based 8-bit→RGB, no color-key ranges, no
overlays, lost surfaces keep contents, BltFast + clipper → BLTFASTCANTCLIP) are not repeated.
Related note: the XP HEL converts no formats at all (§0: any SRCCOPY between different formats
is DDERR_UNSUPPORTED when emulated); dd2d9 converting is a superset and needs no change.

1. **Blt and BltFast never report lost surfaces** — game-visible.
   XP: dest lost or src lost → DDERR_SURFACELOST, first check of both methods
   (X:…Blt.c:198, 1646; X:…BltFast.c:95, 99, 566); also a pattern surface (§1.2) and a mode
   change during the call. dd2d9: Surface::Blt / BltLocked / BltFast never call IsLost
   (D:blit.cpp:221–410), while Lock/Flip do (D:surface.cpp Lock, Flip). A game that blits its
   back buffer to the primary (no Flip) and restores on Blt's DDERR_SURFACELOST never restores;
   dd2d9's presenter doesn't show a lost primary (D:presenter.cpp:419), so the screen stays black
   after a mode change or Alt+Tab. Fix: at the top of Surface::Blt and Surface::BltFast (after
   `Guard`): `if (IsLost() || (src && src->IsLost())) return DDERR_SURFACELOST;` (the contents
   are still kept; nothing is drawn, as on XP). Same for GetBltStatus/BltBatch (items 11, 12).

2. **DDBLT_ROP with an unsupported ROP returns DD_OK and draws nothing** — game-visible.
   XP: any ROP other than SRCCOPY not in the caps → DDERR_NORASTEROPHW (X:…Blt.c:839–870,
   A:737483eb); with dd2d9's caps only SRCCOPY/BLACKNESS/WHITENESS pass (via the HEL's rops).
   Wine test_color_fill (modern Windows): SRCPAINT, SRCAND, SRCINVERT, SRCERASE, NOTSRCCOPY,
   NOTSRCERASE, MERGECOPY, MERGEPAINT, PATCOPY, PATPAINT, PATINVERT, DSTINVERT, 0xAA0029 →
   NORASTEROPHW. dd2d9: logs and returns DD_OK (D:blit.cpp:273–276), so a game that falls back
   when a ROP fails silently loses that drawing. Fix: return DDERR_NORASTEROPHW there (before
   the "source required" check, as XP orders it). Related: GetCaps reports `dwRops` all zero for
   both the driver and the HEL (D:ddraw.cpp:984–1031); XP's HEL reports the SRCCOPY, BLACKNESS
   and WHITENESS bits (0x737700b8: dwRops[0]=0x1, [6]=0x1000, [7]=0x80000000). Set those three
   bits in `c.dwRops` (GetCaps is outside this audit; noted because games read it before Blt).

3. **DDBLT_KEYDEST takes the source's key for IDirectDrawSurface3 too** — game-visible (rare:
   dest keys with the v3 interface).
   XP: the key comes from the source's CKDestBlt only when the **source interface pointer** is
   IDirectDrawSurface or IDirectDrawSurface2 (A:737482c9–737482eb; vtables 0x7376f100,
   0x7376f300); v3/v4/v7 sources use the dest's own key. dd2d9: `version < 4` of the **dest**
   interface (D:blit.cpp:227, D:wrappers.h:46), so v3 → source key. Fix: in
   SurfaceWrap<V>::Blt compute `bool srcOld = src && (static_cast<void*>(src) == ss->Iface(1) ||
   static_cast<void*>(src) == ss->Iface(2));` and pass that instead of `version < 4`. (Wine's
   ddraw1/2/4/7 tests only cover v1+v1, v2+v2, v4+v4 and v7+v7, which both rules agree on.)

4. **Validation order in Blt: rectangles before flags** — edge case (Wine-tested).
   XP validates flags, fx and "source required" (§1.2, steps 11–13) before any rectangle
   (step 14). dd2d9 checks the dest rect first (D:blit.cpp:240–255), then fills/ROPs, then the
   source and the keys. Visible results: NULL source with no fill flag + empty/inverted dest
   rect → XP DDERR_INVALIDPARAMS, dd2d9 DDERR_INVALIDRECT (Wine test_blt expects
   INVALIDPARAMS for every NULL-source call, including rows 6–7 with flipped/mirrored dest
   rects, which dd2d9 gets wrong); COLORFILL/DEPTHFILL + source + bad dest rect → XP INVALIDPARAMS (Wine
   test_color_fill); key or fx errors + bad rect → XP INVALIDPARAMS/NOCOLORKEYHW. The Wine tests
   that cover this (test_blt, test_color_fill) are skipped in dd2d9's runs because they need a
   3D device, so the baseline doesn't show it. Fix: restructure BltLocked as XP orders it:
   (a) fx checks, (b) key flags, (c) operation flags incl. source requirements, (d) rects.

5. **Fill flags don't validate their surfaces** — edge case (Wine-tested).
   XP (§1.2b): COLORFILL with a source → INVALIDPARAMS; COLORFILL on a Z buffer →
   INVALIDPARAMS; DEPTHFILL with a source or on a non-Z surface → INVALIDPARAMS;
   ROP together with COLORFILL/DDFX/DDROPS/DEPTHFILL/ROTATIONANGLE → INVALIDPARAMS;
   COLORFILL|DEPTHFILL on a color surface = color fill, on a Z buffer → INVALIDPARAMS.
   dd2d9 fills in all these cases (D:blit.cpp:257–264). DXT dest with DEPTHFILL: XP
   INVALIDPARAMS, dd2d9 DDERR_UNSUPPORTED (fourcc test first, D:blit.cpp:238). Fix: in the
   operation step: ROP first (reject the combinations), then COLORFILL (`src` → INVALIDPARAMS,
   `buf->fmt.zbuf` → INVALIDPARAMS), then DEPTHFILL (`src` or `!buf->fmt.zbuf` →
   INVALIDPARAMS); move the fourcc test after validation (XP: DDERR_UNSUPPORTED from the HEL).

6. **BLACKNESS/WHITENESS skip the source rectangle check** — edge case (Wine-tested).
   XP validates lpSrcRect whenever a source is given (§1.3); Wine test_color_fill: WHITENESS +
   source + inverted, negative or out-of-bounds lpSrcRect → INVALIDRECT. dd2d9 fills and returns
   before looking at the source (D:blit.cpp:267–271). Fix: falls out of item 4's reordering.

7. **WHITENESS writes 0xFFFFFFFF** — edge case.
   XP HEL: 0x00FFFFFF (X:FUN_73752889:218–220), so the alpha/X byte of a 32-bit surface is 0
   (Wine test_color_fill's comment says the same of modern Windows). dd2d9:
   `0xFFFFFFFFu` & ValueMask (D:blit.cpp:268). Fix: use 0x00FFFFFF (same result below 32 bpp).

8. **lpDDBltFX not validated** — edge case.
   XP: fx non-NULL with dwSize != 100 → INVALIDPARAMS whatever the flags; fx NULL with any of
   0x02F75D4A → INVALIDPARAMS (X:…Blt.c:443–450). dd2d9 never reads dwSize and requires fx only
   for COLORFILL/DEPTHFILL/ROP/KEY*OVERRIDE (D:blit.cpp:258, 266, 311, 327); DDBLT_DDFX without
   fx blits normally (D:blit.cpp:340). Fix: at the top of the validation:
   `if (fx ? fx->dwSize != sizeof(DDBLTFX) : (flags & 0x02F75D4A)) return DDERR_INVALIDPARAMS;`.
   (A game that leaves dwSize uninitialized fails on XP; matching is the faithful choice.)

9. **Flags XP refuses are accepted** — edge case.
   (a) DDBLT_DDROPS → XP DDERR_NODDROPSHW; DDBLT_ROTATIONANGLE → DDERR_NOROTATIONHW; DDFX with
   ROTATE90/180/270 → DDERR_NOROTATIONHW (dd2d9's caps have no rotation); DDFX with
   ARITHSTRETCHY → DDERR_NOSTRETCHHW (X:…Blt.c:669–779, A:7374858f, 73748736, 737485ce).
   dd2d9 ignores DDROPS/ROTATIONANGLE and logs the DDFX bits but blits (D:blit.cpp:340–345).
   (b) DDBLT_LAST_PRESENTATION (0x20000000) → XP INVALIDPARAMS (277); dd2d9 ignores. (Bit
   0x80000000 alone is XP's private D3D-runtime flag, consumed at 208–275, not an error.)
   (c) IDirectDrawSurface7: DDBLT_WAIT|DDBLT_DONOTWAIT → XP INVALIDPARAMS (296–308); BltFast
   v7: DDBLTFAST_WAIT|DONOTWAIT → INVALIDPARAMS (X:…BltFast.c:115–123). dd2d9 no check.
   (d) KEYSRC with a NULL source → XP DDERR_INVALIDOBJECT (A:73748191); dd2d9
   DDERR_INVALIDPARAMS (D:blit.cpp:278).
   Fix: add these checks in the order of §1.1/§1.2 (they need the interface version, which the
   wrapper already passes).

10. **Locked surfaces and surfaces with a DC out can be blitted** — edge case.
    XP: dest or src Usage > 0 → DDERR_SURFACEBUSY, before any flag/rect check (Blt step 8,
    BltFast step 7); GetDC raises Usage for non-OWNDC surfaces. dd2d9: GdiFlush and blit
    (D:blit.cpp:237); `lockCount` is never consulted. Fix: in Blt and BltFast after the lost
    check: `if (lockCount || (dcOut && !(desc.ddsCaps.dwCaps & DDSCAPS_OWNDC)) || (src && (…same
    for src…))) return DDERR_SURFACEBUSY;`. Being lenient here hurts nothing a working XP game
    does, so this is low priority; matching makes broken code paths behave as on XP.

11. **BltFast validation order and dwTrans** — edge case.
    XP: dwTrans bits outside 0x33 → INVALIDPARAMS before any rectangle (X:…BltFast.c:100);
    SRCCOLORKEY|DESTCOLORKEY (3) is accepted (passed on with no operation flag; outcome not
    determined); the clipper test is the **last** check (435). dd2d9: clipper first
    (D:blit.cpp:383), rects, then `switch (trans & 0xF)` — higher bits ignored, 3 →
    INVALIDPARAMS (D:blit.cpp:393–406). E.g. a clipper plus an out-of-bounds rect: XP
    INVALIDRECT, dd2d9 BLTFASTCANTCLIP. Fix: `if (trans & ~0x33u) return DDERR_INVALIDPARAMS;`
    right after the lost check; move the clipper test to just before the blit; for 3, leave
    INVALIDPARAMS unless a game is found that uses it (XP's result is undetermined).

12. **GetBltStatus does no validation** — edge case.
    XP: flags not exactly 1 or 2 → INVALIDPARAMS; lost → SURFACELOST; else DD_OK (§4). dd2d9:
    always DD_OK (D:wrappers.h:74). Fix: `if (f != DDGBS_CANBLT && f != DDGBS_ISBLTDONE) return
    DDERR_INVALIDPARAMS; Guard l; return s->IsLost() ? DDERR_SURFACELOST : DD_OK;`.

13. **BltBatch skips XP's checks before DDERR_UNSUPPORTED** — edge case.
    XP returns SURFACELOST / SURFACEBUSY / INVALIDPARAMS per §3 before UNSUPPORTED; dd2d9
    returns DDERR_UNSUPPORTED at once (D:wrappers.h:48). Fix (optional): dest lost → SURFACELOST,
    dest locked → SURFACEBUSY, per-entry src lost/locked and fx checks, then DDERR_UNSUPPORTED.

14. **hwnd clipper on an offscreen dest is clipped in client coordinates** — edge case,
    uncertain impact.
    XP uses the window's visible region in **screen** coordinates for every dest surface
    (X:FUN_737363fc:56–98), so on XP a windowed game with a window clipper on its back buffer
    draws only where the surface coordinates fall inside the window's on-screen rectangle (no
    difference when the window sits at 0,0, e.g. in exclusive mode). Wine's
    ddraw_surface_blt_clipped also uses the screen-space list unchanged. dd2d9 clips an
    offscreen dest to `GetClientRect` at the origin (D:surface.cpp Clipper::Region, non-primary
    branch). This lives in Clipper::Region (Clipper scope); match only if a game turns up that
    depends on it, since dd2d9's behaviour is the one such games expect to look right.

15. **Color-key compare mask at 16 bpp** — edge case.
    XP HEL compares all 16 bits (mask 0xFFFFFF) for 16-bit sources (X:FUN_7375892b:33–36), so
    in X1R5G5B5 a pixel with bit 15 set never matches a key with bit 15 clear. dd2d9 masks with
    r|g|b (D:common.h:140), ignoring bit 15. At 32 bpp both ignore X/alpha. Driver-dependent on
    the hardware path (Wine test_ck_operation shows vendors disagree), so low priority; the fix,
    if wanted, is to use ValueMask() as the key mask for 16-bit formats.

16. **GetUniquenessValue after a failed blit** — edge case.
    XP bumps the dest's contents stamp before validating flags and rects (X:…Blt.c:361–370,
    X:…BltFast.c:155–162), so even a failed Blt/BltFast (past the busy checks) changes it.
    dd2d9 bumps only on success (D:blit.cpp:261, 270, 374). Harmless; mention only.

### Agreements checked (no change)
* NULL rects = whole surface; dest rect bounds only without a clipper; source rect always
  bounded; empty/inverted rects → INVALIDRECT; lpSrcRect ignored without a source.
* KEYSRC needs HASCKEYSRCBLT on the source, KEYDEST needs HASCKEYDESTBLT on the dest (even when
  the value comes from the source); key + override of the same kind → INVALIDPARAMS; override
  keys use the low value only.
* BltFast: source rect then dest rect, dest key from the dest on every version, any clipper →
  DDERR_BLTFASTCANTCLIP (deliberate in dd2d9, and XP's result too).
* COLORFILL masks the fill to the dest bit depth (dd2d9 masks a copy; XP also writes the masked
  value back into the caller's DDBLTFX at ≤24 bpp — not worth copying).
* Override keys, mirror flags, stretch, colour fill, depth fill and SRCCOPY copies with the
  ALPHA*/ZBUFFER* flags behave as the HEL does (Wine test_blt_z_alpha, test_ck_operation pass).
* Bad interface pointers → DDERR_INVALIDPARAMS (XP via SEH; dd2d9 via Surface::From). BltFast
  with a NULL source crashes on XP; dd2d9's DDERR_INVALIDPARAMS is fine.

### Not determined
* Stretch sampling of the HEL core blitters (FUN_737567de, FUN_7375816b, …) — dd2d9's
  `s0 + rx*sw/dw` nearest-neighbour was not compared pixel for pixel; nor the per-clip-rect
  source edges of clipped stretches (XP: 16.16 factors, FUN_73762b4c).
* Overlapping source/dest in the HEL (dd2d9 copies the source first, which Wine's
  test_blit_to_self confirms is the right visible result on modern Windows).
* BltFast with dwTrans = 3, and Blt with DDBLT_DDFX and a NULL source (both reach the HEL
  with inputs it doesn't expect).
