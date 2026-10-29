# IDirectDrawSurface: Flip, GetFlipStatus, SetColorKey, GetColorKey, SetPalette, GetPalette, SetClipper, GetClipper

Reference: Windows XP SP3 ddraw.dll 5.3.2600.5512. One function body implements every
interface version (1/2/3/4/7) of each method; the version is only distinguished where noted
(compare of `This->lpVtbl` with `&vtbl_IDirectDrawSurface..4`).

File references `X:<file>:<line>` are to the Ghidra output in
`$S/decomp-annotated/` (S = the audit scratchpad); `D:<file>:<line>` are to dd2d9's `src/`.
All XP methods take the DirectDraw critical section first; none validates `This` or the
pointer arguments explicitly (bad pointers fault inside the method's SEH frame, or crash).

Names used below: LCL = `_DDRAWI_DDRAWSURFACE_LCL`, GBL = `..._GBL`, GBL_MORE = the
structure whose pointer sits at `GBL - 4` (Ghidra prints it as `lpGbl[-1].ddpfSurface + 0x1c`;
the disassembly is `mov -0x4(%edx),%edx`). "Lost" = `LCL->dwFlags & DDRAWISURF_INVALID`
(0x10000000). "Locked" = `GBL->dwUsageCount != 0`; XP's GetDC (FUN_73738101, via
IDirectDrawSurface7_GetDC.c:30) increments `dwUsageCount` too unless the surface is
DDSCAPS_OWNDC, so "DC out" counts as locked everywhere below.
`DDERR_NOTLOADED` (0x8876028F) is returned for DDSCAPS_OPTIMIZED surfaces (never exist in dd2d9).

---------------------------------------------------------------------------------------------

## 1. Flip(lpTarget, dwFlags) — X:IDirectDrawSurface7_Flip.c

### 1.1 Validation, in XP's order

The target's LCL is read before anything else (`param_2[1]`, line 98): a garbage target
faults (SEH), there is no explicit pointer check.

| # | Condition | Result | Line |
|---|-----------|--------|------|
| 1 | `dwFlags & ~(0x3F | 0x07000000)` (any bit outside WAIT, EVEN, ODD, NOVSYNC, STEREO, DONOTWAIT, INTERVAL bits) | DDERR_INVALIDPARAMS | 101 → 517 |
| — | IDirectDrawSurface7 only: if DONOTWAIT, clear WAIT; else set WAIT (v7 waits by default). v1–4: flags unchanged (DONOTWAIT accepted, meaningless). | — | 102–111 |
| 2 | (EVEN or ODD) and surface not DDSCAPS_OVERLAY | DDERR_INVALIDPARAMS | 113 → 517 |
| 3 | NOVSYNC together with any INTERVAL bit (0x07000000) | DDERR_INVALIDPARAMS | 114 → 517 |
| 4 | this surface lost, or target non-NULL and lost | DDERR_SURFACELOST | 118–122 |
| 5 | Win16 lock busy (`*lpwPDeviceFlags & BUSY`) | DDERR_SURFACEBUSY | 124–126 |
| 6 | STEREO and this LCL lacks DDRAWISURF_STEREOSURFACELEFT (0x20000000) | DDERR_INVALIDPARAMS | 129 |
| 7 | caps lack DDSCAPS_FRONTBUFFER **or** DDSCAPS_FLIP (so Flip on a back buffer, or on any plain surface) | DDERR_NOTFLIPPABLE | 131–133 |
| 8 | this surface locked / DC out | DDERR_SURFACEBUSY | 135 |
| 9 | DDSCAPS_PRIMARYSURFACE and this DirectDraw object is not the exclusive owner | DDERR_NOEXCLUSIVEMODE | 136–138 |
| — | DDSCAPS_MIPMAP (not OPTIMIZED): handled by FUN_7375f7a9 (flips every mip level's chain in parallel, same checks per level: lost, NOTFLIPPABLE, busy, sys/vid mismatch → INVALIDPARAMS) | — | 141–143 |
| — | primary and DD-global flag 0x20000: driver callback (lpDDCB+0xAC) may complete the call (driver-dependent; not reached by an emulated driver) | — | 145–168 |
| 10 | no next surface in the chain (FUN_737338f6 returns NULL: no attached surface with DDSCAPS_FLIP of the same kind) | DDERR_NOTFLIPPABLE | 172–178 → 510 |
| 11 | chain walk, from the next surface around to (not including) this one — except ODD/EVEN with target == this, which starts at this. For each member: lost → DDERR_SURFACELOST (181–183); locked **and** (target NULL or this member is the target) → DDERR_SURFACEBUSY (185–190); GBL_MORE+0x24 ≠ 0 → DDERR_SURFACEBUSY (188; field is `hKernelSurface` in the public header layout — meaning on XP not confirmed) | | 178–200 |
| 12 | target non-NULL and not found in the walk (includes target == this surface for non-overlays, and any surface of another chain) | DDERR_NOTFLIPPABLE | 202–203 → 510 |
| 13 | target (or next) is SYSTEMMEMORY while this is VIDEOMEMORY, or target VIDEOMEMORY while this is SYSTEMMEMORY (only possible in rings built with AddAttachedSurface) | DDERR_INVALIDPARAMS | 207–217 → 505 (asm 0x73746b0b) |

Notes on #11: with an explicit target, a *non-target* back buffer may be locked and the flip
still succeeds (only the front and the target are swapped, see 1.3). With a NULL target any
locked member fails the call.

FUN_737338f6 (next in chain): first entry of this LCL's attach list whose LCL has
DDSCAPS_FLIP and passes FUN_73733941 (same ZBUFFER/TEXTURE/OVERLAY/ALPHA caps as this
surface). Attached Z-buffers are therefore never part of the ring.

### 1.2 Driver call and flags (driver-dependent)

Lines 225–315. The driver (HAL lpDDCB+0x3C/0xBC, or HEL +0x13C when both are system memory)
Flip callback is only called when this surface is the **primary**, or a visible overlay with
hardware overlay caps (254–256). Offscreen/texture chains never call the driver: they are a
pure pointer swap (1.3).

- No callback for the primary → DDERR_NOFLIPHW (257–259).
- Flags passed to the driver (local_58): WAIT removed; STEREO removed if no stereo pair;
  registry "force no vsync" (DAT_7376f3a0 bit) adds NOVSYNC and clears INTERVAL;
  without DDCAPS2_FLIPINTERVAL the INTERVAL bits are removed, with it and no INTERVAL /
  NOVSYNC given, DDFLIP_INTERVAL1 (0x01000000) is added; without DDCAPS2_FLIPNOVSYNC,
  NOVSYNC is removed (281–292). dd2d9 reports both caps.
- Driver returns DDERR_WASSTILLDRAWING and WAIT is set → retried in a loop; any other
  error (or WASSTILLDRAWING without WAIT) is returned **before** the swap (294–308).
- Every 500 primary flips the screen-saver timeout is re-set (SPI_SETSCREENSAVETIMEOUT,
  227–233). Not observable by games.

### 1.3 The swap (side effects)

Lines 316–488. What moves is only the video memory identity of each surface: `fpVidMem`,
`lpVidMemHeap`, GBL_MORE `fpPhysicalVidMem` and `dwDriverReserved`, `GBL->dwReserved1`,
the DDRAWISURFGBL_ISGDISURFACE flag, `LCL->hDDSurface` and the surface handle
(`lpSurfMore->qwDDrawReserved8[1]`, with the handle table entry updated). Nothing else
moves: caps (FRONTBUFFER/BACKBUFFER/VISIBLE), palette, clipper, color keys, attachments,
pixel format, lock state and private data stay with the surface object.

- target NULL (350–414): rotate — each member receives the memory of the next one; the last
  member receives the old front's memory. Front shows old back1; back1 gets old back2; ...;
  backN gets the old front.
- target given (415–458): swap front and target only; the other back buffers keep theirs.
- Stereo chains: the attached left-eye surfaces rotate the same way (377–411, 436–457, 471–487).
- Primary: `vmiData.fpPrimary` = new front memory (347–349).
- Attached Z-buffers: untouched (not in the ring, no z swap).
- GBL_MORE `dwContentsStamp` (what GetUniquenessValue returns) is incremented on the front
  and on the target / next surface only — not on back2..backN — and a stamp of 0 stays 0
  (234–251).
- D3D flip notification (`lpDD_lcl[1].dwProcessId` callback, 489–493).
- Returns DD_OK (501).

## 2. GetFlipStatus(dwFlags) — X:IDirectDrawSurface7_GetFlipStatus.c

| # | Condition | Result | Line |
|---|-----------|--------|------|
| 1 | `dwFlags` not exactly DDGFS_CANFLIP or exactly DDGFS_ISFLIPDONE (0, both, or other bits) | DDERR_INVALIDPARAMS | 37 → 85 |
| 2 | DDSCAPS_RESERVED2 (0x00800000, execute buffer) | DDERR_INVALIDSURFACETYPE | 42 → 81 |
| 3 | lost | DDERR_SURFACELOST | 43 → 77 |
| 4 | Win16 lock busy | DDERR_SURFACEBUSY | 44 → 73 |
| 5 | DDSCAPS_SYSTEMMEMORY | DD_OK | 45 → 70 |
| 6 | driver GetFlipStatus callback (lpDDCB+0x5C/0xDC) if any and handled → its ddRVal (e.g. DDERR_WASSTILLDRAWING); otherwise DD_OK | 47–68 |

No check that the surface is flippable or part of a chain. Same for all versions.

## 3. SetColorKey(dwFlags, lpKey) — X:IDirectDrawSurface7_SetColorKey.c, FUN_7373e021

### 3.1 Validation, in order

| # | Condition | Result | Line |
|---|-----------|--------|------|
| 1 | lost | DDERR_SURFACELOST | 67–69 |
| 2 | DDSCAPS_OPTIMIZED | DDERR_NOTLOADED | 72–74 |
| 3 | DDSCAPS_ZBUFFER or execute buffer (0x00820000) | DDERR_INVALIDSURFACETYPE | 76–78 |
| 4 | **IDirectDrawSurface7 only**: DDSCAPS2_MIPMAPSUBLEVEL | DDERR_NOTONMIPMAPSUBLEVEL | 80–85 |
| 5 | `dwFlags & ~0x1F` | DDERR_INVALIDPARAMS | 87–89 |
| 6 | neither driver nor HEL has DDCAPS_COLORKEY | DDERR_UNSUPPORTED | 103–115 |
| — | *Only when lpKey != NULL*: copy the key; without DDCKEY_COLORSPACE, **high := low** | — | 131–137 |
| 7 | (key != NULL) flags name no key type, i.e. 0 or COLORSPACE only | DDERR_INVALIDPARAMS | FUN_7373e021:17–18 |
| 8 | (key != NULL) DESTOVERLAY with DESTBLT/SRCBLT/SRCOVERLAY; DESTBLT with SRCBLT/SRCOVERLAY; SRCOVERLAY with SRCBLT | DDERR_INVALIDPARAMS | FUN_7373e021:143, 101, 59 |
| 9 | (key != NULL) the needed DDCKEYCAPS bit is in neither the driver's nor (as fallback) the HEL's caps: SRCBLT→0x200 (range: SRCBLTCLRSPACE 0x400), DESTBLT→0x1 (0x2), SRCOVERLAY→0x2000 (0x4000), DESTOVERLAY→0x10 (0x20). A "range" is low ≠ high after the collapse above. | DDERR_NOCOLORKEYHW | FUN_7373e021 |
| 10 | DESTOVERLAY (with or without key): driver lacks DDCAPS_OVERLAY | DDERR_NOOVERLAYHW | 182–183 |
| 11 | DESTOVERLAY: surface has no overlay data (DDRAWISURF_HASOVERLAYDATA) | DDERR_NOTAOVERLAYSURFACE | 189–191 |

With a NULL key, steps 7–9 do not run: flags are not validated beyond step 5.

### 3.2 Effect

The key type is chosen by priority DESTOVERLAY > DESTBLT > SRCOVERLAY > SRCBLT (142–203).
- Key non-NULL: store low/high in `ddckCK*`, set DDRAWISURF_HASCKEY* flag. The value is
  stored as given (no masking to the pixel format).
- Key NULL: clear the HASCKEY* flag of the chosen type only; the stored value stays. With
  flags 0 or COLORSPACE alone nothing is changed and DD_OK is returned. With several types,
  only the highest-priority one is removed.
- Key NULL and an overlay type on a visible overlay: UpdateOverlay is re-issued (216–235).
- Then (key non-NULL) the driver/HEL SetColorKey callback is called (240–272); an error
  (or DDHAL "no ckey hw" for non-SRCBLT) restores all four keys and the flags (274–284).
- Keys are per surface: they do not move on Flip, and setting one on the front does not
  touch the back buffers (Wine ddraw7.c:2422–2457 shows the back buffer starts without one).

Wine test_range_colorkey (ddraw7.c:13079) confirms on modern Windows: without COLORSPACE
a range collapses to low; COLORSPACE with low == high is fine; a real range → NOCOLORKEYHW for
SRCBLT and DESTBLT; a failed call leaves the key unchanged.

The HEL's own dwCKeyCaps could not be located (filled outside the decompiled set); with the
caps dd2d9 reports (DDCKEYCAPS_SRCBLT | DDCKEYCAPS_DESTBLT, D:ddraw.cpp:986) and assuming the
HEL adds no overlay/range bits, XP gives NOCOLORKEYHW for SRCOVERLAY/DESTOVERLAY keys and for
ranges.

## 4. GetColorKey(dwFlags, lpKey) — X:IDirectDrawSurface7_GetColorKey.c

| # | Condition | Result | Line |
|---|-----------|--------|------|
| 1 | lost | DDERR_SURFACELOST | 26–28 |
| 2 | ZBUFFER or execute buffer | DDERR_NOCOLORKEY (not INVALIDSURFACETYPE) | 30 → 85–86 |
| 3 | `dwFlags & ~0x1F` | DDERR_INVALIDPARAMS | 31 |
| 4 | OPTIMIZED | DDERR_NOTLOADED | 32–34 |
| 5 | no DDCAPS_COLORKEY in driver nor HEL | DDERR_NOCOLORKEYHW | 36–39 |
| 6 | DESTOVERLAY with DESTBLT/SRCBLT/SRCOVERLAY | DDERR_INVALIDPARAMS | 78 |
| 7 | DESTBLT with SRCBLT/SRCOVERLAY | DDERR_INVALIDPARAMS | 69 |
| 8 | SRCOVERLAY with SRCBLT, or SRCOVERLAY on a surface without DDSCAPS_OVERLAY | DDERR_INVALIDPARAMS | 52 |
| 9 | no type flag (0 or COLORSPACE alone) | DDERR_INVALIDPARAMS | 44 |
| 10 | requested key not set | DDERR_NOCOLORKEY | 86 |

COLORSPACE is accepted alongside any type. lpKey is not checked (NULL faults). On success
both low and high are copied. Same for all versions; no mip-sublevel restriction (Wine
ddraw7.c:2400: sublevels report NOCOLORKEY).

## 5. SetPalette(lpPalette) — X:IDirectDrawSurface7_SetPalette.c

### 5.1 Validation, in order

| # | Condition | Result | Line |
|---|-----------|--------|------|
| 1 | OPTIMIZED | DDERR_NOTLOADED | 63–65 |
| 2 | ZBUFFER or execute buffer | DDERR_INVALIDSURFACETYPE | 67 → 257 |
| 3 | **IDirectDrawSurface7 only**: DDSCAPS2_MIPMAPSUBLEVEL | DDERR_NOTONMIPMAPSUBLEVEL | 68–74 |
| 4 | lost | DDERR_SURFACELOST | 75–77 |
| 5 | palette is DDPCAPS_ALPHA and surface not DDSCAPS_TEXTURE | DDERR_INVALIDSURFACETYPE | 92 |
| 6 | palette from a different DirectDraw global object, both display drivers | DDERR_INVALIDPARAMS | 93–98 |
| 7 | primary, some DirectDraw object holds exclusive mode, and it is not this one | DDERR_NOEXCLUSIVEMODE | 101–103 → 253 (FUN_73734625: [0]=someone exclusive, [1]=we are) |
| 8 | palette NULL and none attached | DDERR_NOPALETTEATTACHED | 106–111 |
| 9 | (palette non-NULL) palette size vs. the surface's pixel format (its own, or the display mode's if it has none): 1-bit needs DDPF_PALETTEINDEXED1, 2-bit → INDEXED2, 4-bit → INDEXED4, 8-bit → INDEXED8; DDPCAPS_8BITENTRIES palette needs DDPF_PALETTEINDEXEDTO8 and a normal palette must not be put on an INDEXEDTO8 surface | DDERR_INVALIDPIXELFORMAT | 118–137, 250 |

A NULL palette (detach) skips #5, #6 and #9. Thus an 8-bit palette can only go on an
8-bit indexed surface; mip sublevels are allowed in v1–4 (Wine ddraw1/2/4 test_palette_complex);
alpha palettes only on textures. Mipmap sublevel check happens before the pixel-format check
(Wine ddraw7.c:9902–9921).

### 5.2 Effect

- Driver/HEL SetPalette callback (HAL lpDDCB+0x6C/0xEC unless the palette is DDRAWIPAL_INHEL,
  else HEL +0x16C; 140–163). The primary's palette realization (hardware palette / GDI
  palette in exclusive mode) happens inside this callback — driver-/HEL-dependent.
  No callback → DD_OK without attaching (149–150, 248); callback not handled → DDERR_UNSUPPORTED (244).
- On ddRVal == 0 (164–239):
  - detach: `lpDDPalette = NULL`;
  - attach: if different from the current one, store it and **AddRef the new palette once**;
    re-setting the same palette changes no reference count;
  - the old palette (if any and different) gets DDRAWIPAL_EXCLUSIVE cleared when this is the
    primary and we own exclusive mode, then **Release** once;
  - this surface's contents stamp is incremented; D3D palette notifications.
- Returns the callback's ddRVal.
- **Only this surface gets the palette.** Back buffers of a flip chain have none
  (Wine ddraw7.c test_primary_palette: refcount 2 after SetPalette on a primary with a back
  buffer, GetPalette(backbuffer) → DDERR_NOPALETTEATTACHED). Flip does not move palettes.
- Related (outside this scope, noted for the palette spec): IDirectDrawPalette::GetCaps adds
  DDPCAPS_PRIMARYSURFACE dynamically while the palette is the primary's (X:IDirectDrawPalette_GetCaps.c:38–43);
  the palette's last Release detaches it from the primary (FUN_73767289:46–49).

## 6. GetPalette(lplpPalette) — X:IDirectDrawSurface7_GetPalette.c

1. `*lplpPalette = NULL` **first, before any check** (line 20; no NULL check of the pointer).
2. lost → DDERR_SURFACELOST (22, 37).
3. OPTIMIZED → DDERR_NOTLOADED (23, 33).
4. no palette → DDERR_NOPALETTEATTACHED (24, 30).
5. `IDirectDrawPalette_QueryInterface(palette, IID_IDirectDrawPalette, out)` → one AddRef (25).

Wine test_palette_complex checks that the out pointer is NULL after NOPALETTEATTACHED.

## 7. SetClipper(lpClipper) — X:IDirectDrawSurface7_SetClipper.c

1. OPTIMIZED or execute buffer → DDERR_INVALIDSURFACETYPE (24–27). No lost check, no
   restriction to the primary: any surface (offscreen, back buffer, texture, Z) may hold one.
2. NULL and none attached → DDERR_NOCLIPPERATTACHED (30–35).
3. NULL: clear `lpDDClipper`/`lpDDIClipper`, Release the old clipper (52–55) → DD_OK.
4. non-NULL, different from current: Release old (if any), store, AddRef new (39–47).
   Same clipper again: nothing changes (refcount unchanged). → DD_OK.

No pointer validation (Wine: an invalid pointer crashes).

## 8. GetClipper(lplpClipper) — X:IDirectDrawSurface7_GetClipper.c

1. `*lplpClipper = NULL` first (line 22).
2. none → DDERR_NOCLIPPERATTACHED (24–25).
3. OPTIMIZED → DDERR_NOTLOADED (28, 35).
4. AddRef, return it (29–30). No lost check.

---------------------------------------------------------------------------------------------

## Differences from dd2d9

Severity: **game-visible** = plausible in real games; **edge case** = only odd/invalid usage.

1. **SetPalette on a flip chain's front sets every member's palette** — game-visible.
   XP: only the surface called (X:SetPalette.c:166–172); GetPalette on a back buffer →
   DDERR_NOPALETTEATTACHED; palette refcount +1 (Wine test_primary_palette expects 2).
   dd2d9: D:surface.cpp:766–774 gives the palette (and a reference) to every member of
   `flipChain`, so refcount is 1 + chain length, GetPalette(back) succeeds, and a palette a
   game set on a back buffer is overwritten / released by SetPalette(NULL) on the front.
   Fix: remove the propagation loop. Rendering does not need it for the primary chain
   (EffectivePalette, D:surface.cpp:693–697, falls back to the primary's palette); if an
   offscreen 8-bit chain needs its front's palette for display/GetDC, fall back to it
   internally inside EffectivePalette without a reference. (Uncertain why it was added; check
   git history for a game that needed it.)

2. **GetPalette / GetClipper leave the out pointer untouched on failure** — game-visible.
   XP clears it first (X:GetPalette.c:20, X:GetClipper.c:22), for every error.
   dd2d9: D:surface.cpp:746–753 and 639–645 only write it on success. A game doing
   `GetPalette(&p); if (p) p->Release();` with an uninitialized `p` crashes.
   Fix: `*out = nullptr;` right after the NULL check in both.

3. **SetPalette on a mip sublevel refused for every interface version** — game-visible (rare;
   DX5/6 games with palettized mipmaps). XP refuses only through IDirectDrawSurface7
   (X:SetPalette.c:68–74; Wine ddraw1/2/4 tests accept it). dd2d9: D:surface.cpp:758 checks
   regardless of version; the wrapper does not pass it (D:wrappers.h:120).
   Fix: pass V from the wrapper and apply the check only for V == 7.

4. **SetColorKey on a mip sublevel through IDirectDrawSurface7 is accepted** — edge case.
   XP: DDERR_NOTONMIPMAPSUBLEVEL for v7 only (X:SetColorKey.c:80–85; Wine ddraw7.c:2405, 2514).
   dd2d9: no check (D:surface.cpp:677–691). Fix: pass V; for V == 7 and DDSCAPS2_MIPMAPSUBLEVEL
   return DDERR_NOTONMIPMAPSUBLEVEL (after the lost/Z-buffer checks, before flag checks).

5. **SetColorKey with a NULL key validates flags** — edge case.
   XP: with NULL, only `flags & ~0x1F` is rejected; the highest-priority named type is
   cleared, flags 0 / COLORSPACE alone is a no-op, DD_OK (X:SetColorKey.c:87, 142–203, 215–237).
   dd2d9: CkIndex rejects 0 and combinations with DDERR_INVALIDPARAMS (D:surface.cpp:678–683).
   Fix: when `!key`, reject only unknown bits; clear the first of DESTOVERLAY, DESTBLT,
   SRCOVERLAY, SRCBLT present; return DD_OK (but see 6 for DESTOVERLAY).

6. **Overlay color keys accepted** — edge case. XP with dd2d9's caps: SRCOVERLAY/DESTOVERLAY
   with a key → DDERR_NOCOLORKEYHW (no DDCKEYCAPS_*OVERLAY; FUN_7373e021), DESTOVERLAY with
   NULL → DDERR_NOOVERLAYHW (X:SetColorKey.c:182). GetColorKey(SRCOVERLAY) on a non-overlay
   surface → DDERR_INVALIDPARAMS (X:GetColorKey.c:52). dd2d9 stores/returns them
   (D:surface.cpp:659–691). Fix: SetColorKey overlay types → NOCOLORKEYHW (key) /
   NOOVERLAYHW (DESTOVERLAY, NULL) / DD_OK (SRCOVERLAY, NULL); GetColorKey SRCOVERLAY →
   INVALIDPARAMS. (HEL ckey caps not confirmed — see 3.2.) Consistent with dd2d9's "overlays refused".

7. **Color keys on lost surfaces and Z-buffers** — edge case.
   XP SetColorKey: lost → SURFACELOST (first check), ZBUFFER → INVALIDSURFACETYPE
   (X:SetColorKey.c:67, 76). XP GetColorKey: lost → SURFACELOST, ZBUFFER → NOCOLORKEY
   (X:GetColorKey.c:26, 30). dd2d9 checks neither (D:surface.cpp:669–691).
   Fix: add `IsLost()` and DDSCAPS_ZBUFFER checks in XP's order.

8. **SetPalette error codes/order** — edge case. dd2d9 (D:surface.cpp:755–761):
   a. SetPalette(NULL) with no palette attached → DD_OK on an 8-bit surface or
      DDERR_INVALIDPIXELFORMAT otherwise; XP → DDERR_NOPALETTEATTACHED (X:SetPalette.c:110),
      and a NULL detach never checks the pixel format.
   b. Z-buffer → INVALIDPIXELFORMAT; XP → INVALIDSURFACETYPE before anything else (line 67).
   c. Palette size not checked against the format: a 1/2/4-bit palette on an 8-bit surface is
      accepted (EffectivePalette then reads 256 entries); DDPCAPS_8BITENTRIES palettes on a
      plain 8-bit surface accepted. XP → DDERR_INVALIDPIXELFORMAT (lines 125–137).
   d. Primary while another DirectDraw object holds exclusive mode → XP NOEXCLUSIVEMODE (line 103); dd2d9 none.
   Fix: order as in 5.1: Z-buffer → (v7) sublevel → lost → alpha/non-texture → [NULL:
   none attached → NOPALETTEATTACHED, else detach] → size/format match.

9. **Flip flag validation missing** — edge case. XP rejects unknown bits, EVEN/ODD on
   non-overlays, NOVSYNC with INTERVALn, STEREO on non-stereo surfaces with
   DDERR_INVALIDPARAMS before any other check (X:Flip.c:101, 113–114, 129).
   dd2d9 ignores flags apart from NOVSYNC/INTERVAL timing (D:surface.cpp:972–1013).
   Fix: `if (flags & ~0x0700003F || (flags & (DDFLIP_EVEN|DDFLIP_ODD)) || ((flags & DDFLIP_NOVSYNC) && (flags & 0x07000000)) || (flags & DDFLIP_STEREO)) return DDERR_INVALIDPARAMS;`
   (dd2d9 has no overlays or stereo surfaces), placed first.

10. **Flip check order and lock rule** — edge case.
    XP order: flags → this/target lost → NOTFLIPPABLE caps → this locked → NOEXCLUSIVEMODE →
    no next → per member (lost, locked) → target not found → sys/vid mismatch.
    dd2d9 (D:surface.cpp:977–991): lost(this) → caps and `!chainNext` together → NOEXCLUSIVEMODE
    → any member locked/DC → target not found. Visible differences:
    a. front locked while not exclusive: XP SURFACEBUSY, dd2d9 NOEXCLUSIVEMODE;
    b. single FLIP|FRONTBUFFER surface without chain while not exclusive: XP NOEXCLUSIVEMODE, dd2d9 NOTFLIPPABLE;
    c. explicit target with another (non-target) back buffer locked: XP flips, dd2d9 SURFACEBUSY;
    d. lost target outside the chain: XP SURFACELOST, dd2d9 NOTFLIPPABLE; lost member: XP SURFACELOST, dd2d9 only checks the front;
    e. ring mixing system and video memory (AddAttachedSurface): XP INVALIDPARAMS, dd2d9 flips
       (not verified whether dd2d9's AddAttachedSurface can build such a ring).
    Fix: reorder as in 1.1; in the member loop, count a member as busy only if the target is
    NULL or it is the target (the front is checked earlier); check `IsLost()` per member and on the target.

11. **GetFlipStatus is a constant DD_OK** — edge case. XP: flags must be exactly DDGFS_CANFLIP or
    DDGFS_ISFLIPDONE (else INVALIDPARAMS), lost → SURFACELOST (X:GetFlipStatus.c:37, 43).
    dd2d9: D:wrappers.h:82. With flips never pending, DD_OK is right for valid calls.
    Fix: add the flag check and `IsLost()` (under the lock).

12. **SetClipper(NULL) with none attached** — edge case. XP DDERR_NOCLIPPERATTACHED
    (X:SetClipper.c:30–35); dd2d9 DD_OK (D:surface.cpp:649–657). Fix: return
    NOCLIPPERATTACHED when `!c && !clipper`. (XP has no lost check or surface-type
    restriction here — dd2d9 matches.)

13. **Uniqueness after Flip** — edge case (IDirectDrawSurface7::GetUniquenessValue only).
    XP increments the contents stamp of the front and the target/next surface only
    (X:Flip.c:234–251); dd2d9 increments every member (D:surface.cpp:998). Only visible on
    chains of 3+ buffers. Fix (optional): bump `chain[0]` and the target / `chain[1]` only.

Matches (verified): Flip on a back buffer / non-flippable surface → NOTFLIPPABLE; target ==
front → NOTFLIPPABLE; target outside the chain → NOTFLIPPABLE; primary without exclusive
mode → NOEXCLUSIVEMODE while offscreen chains flip; rotation order and explicit-target swap
(Wine test_flip); palettes, keys, clippers and Z-buffers stay with the surface objects;
DC out counts as busy; color-key collapse without COLORSPACE and range rejection;
SetClipper/SetPalette/GetPalette/GetClipper reference counting for the single surface.

Could not determine: the XP HEL's own dwCKeyCaps (whether emulation would accept overlay or
range keys on system-memory surfaces); the meaning of GBL_MORE+0x24 in Flip's busy test;
what XP returns for garbage pointers (no explicit checks — SEH fault handling varies by method).
