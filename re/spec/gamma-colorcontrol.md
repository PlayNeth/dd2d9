# IDirectDrawGammaControl and IDirectDrawColorControl

Reference: Windows XP SP3 ddraw.dll 5.3.2600.5512 (image base 0x73730000). Both interfaces
are extra INTs on a surface's LCL: GammaControl and ColorControl have their own vtables, but
QueryInterface, AddRef and Release on them are the surface functions
(X:InitInterfaceVtables_73731cd2.c:477-485 for ColorControl, :479, 507-510 for GammaControl).

File references `X:<file>:<line>` are to the Ghidra output in `$S/decomp-annotated/`
(S = the audit scratchpad). For API methods the plain-named file (`IDirectDrawGammaControl_SetGammaRamp.c`)
was checked to be byte-identical to its `_<addr>.c` twin. Internal helpers exist only as `FUN_<addr>_<addr>.c`.
`A:<addr>` is the disassembly in `$S/xp.dis`. `D:<file>:<line>` are to dd2d9's `src/` as of 2026-10-29.

Names and offsets (ddrawi.h names, values checked against the code and `$S/types/layouts.txt`):
- Surface LCL: `lpSurfMore` +0, `lpGbl` +4, `dwFlags` +0x1c, `ddsCaps` +0x20.
  LCL flags: `DDRAWISURF_SETGAMMA` 0x8000 (a ramp is on the device and the original was saved),
  `DDRAWISURF_INVALID` 0x10000000 (lost).
- Surface MORE: `lpDD_lcl` +8, `lpGammaRamp` +0x58 (the game's ramp), `lpOriginalGammaRamp` +0x5c
  (the device ramp saved before the first apply). Both are 0x600-byte (DDGAMMARAMP) LocalAlloc blocks.
- Surface GBL_MORE (pointer at `GBL - 4`): `lpColorInfo` +0x2c (the color controls saved before the first SetColorControls).
- DirectDraw LCL: `lpGbl` +4, `dwLocalFlags` +0xc (`DDRAWILCL_HASEXCLUSIVEMODE` 0x1), `dwProcessId` +0x14,
  `hDC` +0x24, `lpPrimary` +0x2c, `lpDDCB` +0x40. XP's LCL is longer than the header type Ghidra used: the
  calibrator module handle and `CalibrateGammaRamp` pointer are at **+0x68/+0x6c** (X:FUN_73764449_73764449.c:9,12),
  which Ghidra prints as `p_Var3[1].lpDDMore` / `p_Var3[1].lpGbl` in SetGammaRamp. The `hGammaCalibrator` /
  `lpGammaCalibrator` names in X:IDirectDraw7_SetDisplayMode.c:51-66 are the same overlay artifact
  (header offsets +0x60/+0x64 hold the requested mode there) and have nothing to do with gamma.
- DirectDraw GBL `ddCaps.dwCaps2` is at GBL+0x14. Ghidra types `ddCaps` as a byte array starting at
  +0xc, so `ddCaps[10] & 2` (X:IDirectDrawGammaControl_GetGammaRamp.c:26) is `dwCaps2 & 0x20000` =
  **DDCAPS2_PRIMARYGAMMA** (confirmed in A:73764395 `testb $0x2,0x16(%esi)`).
- `GdiEntry15` = DdSetGammaRamp(lpDD_gbl, hdc, ramp). `GetDeviceGammaRamp` is the GDI call.

IIDs at the QI comparison addresses (decoded from `$S/dll/ddraw.dll`): 0x7374a254 =
{69C11C3E-B46B-11D1-AD7A-00C04FC29B4E} = IID_IDirectDrawGammaControl; 0x7374a264 =
{4B9F0EE0-0D7E-11D0-9B06-00A0C903A3B8} = IID_IDirectDrawColorControl; 0x7374a244 = IID_IDirectDrawSurfaceKernel.

**Exception handling.** All four methods (Get/SetGammaRamp, Get/SetColorControls) take the DirectDraw critical
section, check the display uniqueness (FUN_7373cc76 when it changed), and run their first checks inside an SEH
try block. **The handler returns `0x88760037` = DDERR_EXCEPTION, not DDERR_INVALIDPARAMS** (A:7376442b,
A:737646c3, A:7376c57b, A:7376c6f0). The try block covers only the `This → lpLcl → lpSurfMore → lpDD_lcl → lpGbl`
dereferences and the argument checks (A:73764364-737643be, A:737644e6-7376455d, A:7376c4aa-7376c4ce,
A:7376c5da-7376c608). In ColorControl that includes reading `lpDDColorControl->dwSize`. **The ramp copies run after
the try level is set back to -1** (A:737643be/737643ce and A:7376455d, then SetGammaRamp.c:59-63 / 95-99), so a
non-NULL invalid ramp pointer is an unhandled access violation (the application crashes). This corrects
surface-other.md's general "faults → DDERR_INVALIDPARAMS" note for these four methods.

---

## 1. QueryInterface for the two IIDs: X:IDirectDrawSurface7_QueryInterface.c

The same function serves the surface versions, GammaControl, ColorControl and SurfaceKernel (header comment, lines 4-7).
The steps before these IIDs are in surface-other.md (`*ppv = NULL` line 41, `IsBadReadPtr(riid)` lines 42-47,
then IUnknown/v1-v7).

1. **IID_IDirectDrawColorControl** (compare at lines 175-186, which falls through to line 278 on a match):
   - surface is a primary (`ddsCaps & DDSCAPS_PRIMARYSURFACE` 0x200): needs `dwCaps2 & DDCAPS2_COLORCONTROLPRIMARY`
     (0x80, `local_20 + 0x14` = surface GBL → `lpDD` → `ddCaps.dwCaps2`) (lines 279, 287);
   - otherwise: needs `ddsCaps & DDSCAPS_OVERLAY` (0x80, `(char)ddsCaps < 0`) **and** `dwCaps2 & DDCAPS2_COLORCONTROLOVERLAY`
     (0x40) (line 280);
   - else E_NOINTERFACE (line 281 → 289-290).
   - If allowed: This itself when This is already the ColorControl INT (line 283), else the LCL's ColorControl INT,
     found or created by `FUN_7373e5c8` (lines 284-285 → 96). AddRef'd → S_OK (lines 69-74).
2. **IID_IDirectDrawGammaControl** (lines 190-200): **no caps or surface-type check**. Every surface (texture, Z buffer,
   offscreen, back buffer) gets one. This if This is already the gamma INT, else `FUN_7373e5c8(gbl, lcl, &vtbl_IDirectDrawGammaControl)`
   (lines 93-97), AddRef'd → S_OK. The surface-type and caps checks are in Get/SetGammaRamp.
3. From a GammaControl or ColorControl INT, any other IID resolves exactly as from a surface (same function), e.g.
   IID_IDirectDrawSurface7 → the LCL's v7 INT (lines 171-173).

## 2. AddRef / Release on the two interfaces

- AddRef = X:IDirectDrawSurface7_AddRef.c (header line 4): if the LCL is ISFREE (4) → 0 (line 23). Otherwise it
  increments the GBL, LCL and **this INT's** count and returns the INT count (lines 24-29). So the gamma/color INT
  has its own count. Its references are also LCL references, and they keep the surface alive.
- Release = X:IDirectDrawSurface7_Release.c (header line 4): `IsBadWritePtr(This, 0x18)` or a vtable that isn't a
  surface/ColorControl/GammaControl/Kernel table → 0 (lines 23-29, 47-51). ISFREE or INT count 0 → 0 (line 31).
  Otherwise `FUN_73733099(This, 0, 0)` (line 36) decrements INT/LCL/GBL (X:FUN_73733099_73733099.c:53-57) and returns
  the new INT count. A complex-surface member with GBL count 1 and IMPLICITCREATE returns 0 untouched (line 45).
- Releasing the last LCL reference through **any** INT (gamma included) tears the surface down. Gamma: §5.

## 3. GetGammaRamp(dwFlags, lpRampData): X:IDirectDrawGammaControl_GetGammaRamp.c

In order:
1. One combined check → **DDERR_INVALIDPARAMS** (lines 25-27):
   `lpRampData == NULL` || `dwFlags != 0` || surface not `DDSCAPS_PRIMARYSURFACE` || no `DDCAPS2_PRIMARYGAMMA`.
2. Lost (`DDRAWISURF_INVALID`) → **DDERR_SURFACELOST** (lines 33-35).
3. If `lpSurfMore->lpGammaRamp` is set, its 0x600 bytes are copied out → DD_OK (lines 37, 45-52). That happens
   **whether or not the ramp is on the device** (not exclusive, deactivated).
4. Else `GetDeviceGammaRamp(lpDD_lcl->hDC, lpRampData)` (line 39). If it fails → **DDERR_UNSUPPORTED** (0x80004001,
   lines 40-42), else DD_OK. `lpDD_lcl->hDC` is the DirectDraw object's device DC, so on a secondary monitor it is that monitor's.
5. No exclusive-mode check. There is no `dwFlags` value that is accepted other than 0.

## 4. SetGammaRamp(dwFlags, lpRampData): X:IDirectDrawGammaControl_SetGammaRamp.c

In order:
1. `dwFlags & ~1` → **DDERR_INVALIDPARAMS** (lines 29, 116-117).
2. `dwFlags & DDSGR_CALIBRATE (1)` and no calibrator registered (`DAT_737707f8 == 0`) → **DDERR_UNSUPPORTED** (lines 30-32).
   This runs **before** the primary check. `DAT_737707f8` is set to 1 at load time only if
   `HKLM\Software\Microsoft\DirectDraw\GammaCalibrator` has a non-empty REG_SZ `Path` (X:FUN_7373b5cc_7373b5cc.c:113-121).
   The same flag gates `DDCAPS2_CANCALIBRATEGAMMA` (0x100000): GetCaps sets it only with the flag and PRIMARYGAMMA, otherwise
   it clears it (X:FUN_73738eaf_73738eaf.c:5-9).
3. Surface not a primary, or no DDCAPS2_PRIMARYGAMMA → **DDERR_INVALIDPARAMS** (lines 34, 116-117).
4. Lost → **DDERR_SURFACELOST** (lines 35-37).
5. `lpRampData == NULL` → `FUN_737343c3(lcl)` → **DD_OK** (lines 39-42). The helper restores the saved original ramp to the
   device if this surface had applied one (`FUN_737343fb`, §5), then frees both `lpGammaRamp` and `lpOriginalGammaRamp`
   (X:FUN_737343c3_737343c3.c:5-13). After this, GetGammaRamp reads the device again. The NULL check comes before the CALIBRATE
   branch, so `Set(CALIBRATE, NULL)` with a calibrator present is a plain reset.
6. CALIBRATE path (lines 84-107): the first time, the calibrator DLL named by the registry path is loaded and
   `CalibrateGammaRamp` is resolved into DD LCL +0x68/+0x6c (X:FUN_73764449_73764449.c:8-18; on failure +0x68 = -1).
   If it is unavailable, `DAT_737707f8` is cleared (calibration is off for the rest of the process) → DDERR_UNSUPPORTED (lines 85-90).
   Otherwise the ramp is copied into a temporary block, and `CalibrateGammaRamp(tmp, lpDD_gbl->cDriverName)` is called (line 100).
   A non-zero return is **returned as the HRESULT** (lines 102-104). Otherwise the calibrated copy is what gets stored (line 101 → 47).
7. Store (lines 47-71): `lpOriginalGammaRamp` and `lpGammaRamp` are allocated (zero-filled, X:FUN_7373134f_7373134f.c:5) if absent.
   If either allocation fails, both are freed → **DDERR_OUTOFMEMORY** (lines 72-82, 109). Otherwise the ramp is copied into `lpGammaRamp`.
8. **Applied to the device only if the DirectDraw object has exclusive mode** (`dwLocalFlags & DDRAWILCL_HASEXCLUSIVEMODE`):
   `FUN_737642d1(lcl, dd_lcl)` (lines 64-66), whose result is **ignored**. The method returns DD_OK (line 70 → 42).
   - X:FUN_737642d1_737642d1.c:8-17. If SETGAMMA is not yet set, the current device ramp is saved first:
     `GetDeviceGammaRamp(dd_lcl->hDC, lpOriginalGammaRamp)`. **If that fails, nothing is applied**, the flag stays clear,
     and the result is DDERR_UNSUPPORTED (ignored by the caller). Otherwise `DdSetGammaRamp(dd_gbl, hDC, lpGammaRamp)`
     runs and SETGAMMA is set **even if DdSetGammaRamp fails** (line 12).
   - Consequences: in DDSCL_NORMAL the ramp is stored and returned by GetGammaRamp but has no visible effect. With no
     gamma hardware, SetGammaRamp returns DD_OK and nothing changes.

## 5. When the ramp is put on and taken off the device

Helpers:
- **Restore**: `FUN_737343fb(lcl, dd_lcl)` (X:FUN_737343fb_737343fb.c). If SETGAMMA is set and `lpOriginalGammaRamp` is set,
  `DdSetGammaRamp(original)` runs (lines 26-28). SETGAMMA is always cleared (line 30). The buffers are **kept**. When the
  caller is not the DirectDraw object's process (process-exit cleanup), a temporary DC for the device is created for the
  call and then deleted (lines 12-25, 31-34).
- **Restore and drop**: `FUN_737343c3` = restore, then free both buffers (X:FUN_737343c3_737343c3.c:5-13).
- **Surface hardware teardown/invalidate**: `FUN_73734478(lcl)` calls `FUN_7373454c` (color-control restore, §7) and then
  the restore (X:FUN_73734478_73734478.c:78-79). The buffers are kept.
- **Apply**: `FUN_737642d1` (§4 step 8).

Events:
| Event | What happens to the device ramp | Stored ramp |
|---|---|---|
| SetGammaRamp(ramp) while exclusive | applied (SetGammaRamp.c:64-66) | replaced |
| SetGammaRamp(ramp) not exclusive | untouched | replaced |
| SetGammaRamp(NULL) | original restored if applied (SetGammaRamp.c:40) | freed |
| Primary's last LCL reference released (any INT, gamma included) | restored: `FUN_737343c3` (X:FUN_73733099_73733099.c:60, 131), and again harmlessly via `FUN_73734478` when the GBL count reaches 0 (line 192) | freed |
| Exclusive mode ends: SetCooperativeLevel without EXCLUSIVE | `FUN_7375aa94` (X:IDirectDraw7_SetCooperativeLevel.c:191-194) restores if the primary exists and has an original saved (X:FUN_7375aa94_7375aa94.c:48-50). HASEXCLUSIVEMODE is cleared after it (SetCooperativeLevel.c:194). | **kept** |
| Exclusive mode entered: SetCooperativeLevel EXCLUSIVE | `FUN_7375ac27` (SetCooperativeLevel.c:235) applies the ramp if the DD LCL has a primary with `lpGammaRamp` (X:FUN_7375ac27_7375ac27.c:44-46). It does not check lost. This also covers a primary kept from an earlier exclusive period. | kept |
| Application deactivated (alt-tab, FUN_7375d5aa with param_3 = 0) | the surfaces are invalidated (X:FUN_7375d5aa_7375d5aa.c:40-41 → `FUN_73760648` → `FUN_73760541`:15 → `FUN_73734478`:79), which restores the original. `FUN_7375aa94` follows (line 82). HASEXCLUSIVEMODE is **not** cleared there. | kept |
| Application reactivated (param_3 != 0) | `FUN_7375ac27` (X:FUN_7375d5aa_7375d5aa.c:48) re-applies the stored ramp | kept |
| Display mode change: SetDisplayMode → `FUN_73761aa2` → `ChangeDisplaySettingsExA` → `FUN_7373cc84(0)` (X:FUN_73761aa2_73761aa2.c:62, 84). The same path runs when another process changes the mode (`FUN_7373cc76`). | Before the driver re-init it records, per DirectDraw LCL, whether its primary had a ramp **on the device** (`lpGammaRamp` and SETGAMMA) (X:FUN_7373cc84_7373cc84.c:35-41). The re-init invalidates the surfaces (probably `FUN_737391bd`:288 → `FUN_73760648`, which restores the original). Unless the driver is NOHARDWARE (0x100), it **re-applies** the ramp afterwards (lines 74-77). Net: the ramp stays on across mode changes, even though the primary is now lost. | kept |
| Surface restored (`Restore`) | nothing gamma-related | kept |
| DirectDraw object released while exclusive | `FUN_7375aa94` (X:IDirectDraw7_Release.c:115) restores | freed with the surfaces |

While the primary is lost, GetGammaRamp and SetGammaRamp both return DDERR_SURFACELOST, even though the ramp stays on the device.

## 6. GetColorControls(lpColorControl): X:IDirectDrawColorControl_GetColorControls.c

1. Inside the try block: `lcl = This->lpLcl`, `dd_lcl = lcl->lpSurfMore->lpDD_lcl` (lines 31-33).
2. `lpColorControl == NULL` or `lpColorControl->dwSize != 0x28` (sizeof DDCOLORCONTROL) → **DDERR_INVALIDPARAMS**
   (lines 34-38). A bad pointer here → DDERR_EXCEPTION (handler).
3. No lost check, no exclusive check, no primary/overlay check (QI already gates the interface), and no dwFlags check.
4. HAL call: `dd_lcl->lpDDCB + 0x278` (line 40). This is presumably the ColorControl callback (DDHAL_DDCOLORCONTROLCALLBACKS);
   the offset is not checked against a header. It gets `{lpDD = dd_lcl->lpGbl, lpDDSurface = lcl, lpColorData = lpColorControl,
   dwFlags = 1 (DDRAWI_GETCOLOR), ddRVal}` (lines 42-46). If the callback is missing or doesn't return DDHAL_DRIVER_HANDLED (1)
   → **DDERR_UNSUPPORTED** (line 57). If handled → `ddRVal` (lines 47-55); the driver fills the structure.

## 7. SetColorControls(lpColorControl): X:IDirectDrawColorControl_SetColorControls.c

1. Same pointers and the same NULL/`dwSize == 0x28` check → DDERR_INVALIDPARAMS (lines 36-46).
2. **First call on this surface only** (`GBL_MORE->lpColorInfo == 0`, lines 48-49): GetColorControls is called into a local
   (dwSize 0x28) (lines 50-51). If that succeeds, the result is saved in a new 0x28-byte block at `lpColorInfo` (lines 52-62).
   A failed Get just skips the save. The save happens before the Set and is not undone if the Set fails.
3. HAL call with `dwFlags = 2` (DDRAWI_SETCOLOR) (lines 65-71). Not handled or no callback → DDERR_UNSUPPORTED (line 82).
   Handled → `ddRVal` (lines 72-79).
4. Restore: `FUN_7373454c(lcl)` sends `lpColorInfo` back with SETCOLOR and frees it (X:FUN_7373454c_7373454c.c:19-30).
   It runs from `FUN_73734478` (X:FUN_73734478_73734478.c:78), i.e. on surface teardown, invalidation (deactivate,
   mode change) and the other `FUN_73734478` callers. So the original color controls come back whenever the surface
   loses its hardware.

---

## dd2d9 as it stands

- QI: D:surface.cpp:311-316. Any surface gets the single per-surface `GammaControl` object, AddRef'd through slot
  `kRefGamma`. ColorControl is not handled; it falls to `D3DProxyQuerySurface` (d3dproxy.cpp:564-567), which returns
  E_NOINTERFACE (D:surface.cpp:318-321).
- AddRef/Release/QI on the gamma object forward to the surface with `kRefGamma` (D:surface.cpp:1736-1738). The per-slot count
  is returned and over-release is ignored (D:surface.cpp:327-331, 344-360).
- GetGammaRamp: D:surface.cpp:1742-1756. SetGammaRamp: D:surface.cpp:1760-1779. The ramp is per surface (`gammaRamp`,
  D:objects.h:272). It is applied in software by the presenter (D:presenter.cpp:148-161, 352-405) through
  `DDraw::ApplyGamma`: the primary's ramp when `Exclusive()`, identity otherwise (D:ddraw.cpp:1096-1105). ApplyGamma
  is called on SetCooperativeLevel (D:ddraw.cpp:1047), on exclusive takeover (D:ddraw.cpp:1216), when a primary is
  created (D:ddraw.cpp:2217), and when the primary is destroyed (D:surface.cpp:270-273).
- GetCaps advertises DDCAPS2_PRIMARYGAMMA and neither COLORCONTROL cap nor CANCALIBRATEGAMMA (D:ddraw.cpp:1369-1370).

Note: surface-other.md item 19 describes an older dd2d9 (surface.cpp:1280-1295: any surface, flags ignored,
Set(NULL) → INVALIDPARAMS, applied windowed). Those points have since been fixed and that item is superseded by this file.

## Differences from dd2d9

Ranked game-visible first. No difference found is likely to break a game. The current implementation follows XP closely.

1. **GetGammaRamp when the device can't report a ramp** (edge case; visible only where `GetDeviceGammaRamp` fails, e.g.
   some VMs, RDP, or Wine drivers without gamma).
   - XP: no stored ramp and `GetDeviceGammaRamp(dd_lcl->hDC)` fails → DDERR_UNSUPPORTED (X:IDirectDrawGammaControl_GetGammaRamp.c:38-42).
     The DC is the DirectDraw object's device DC.
   - dd2d9 (D:surface.cpp:1750-1755): `GetDeviceGammaRamp(GetDC(NULL))`. On failure it fills in an identity ramp and returns DD_OK.
   - Game effect: a game that probes GetGammaRamp to decide whether to offer a brightness slider shows it under dd2d9 where XP
     would hide it. Since dd2d9's software gamma always works, this deviation helps the game.
   - Fix (strict): `if (!ok) return DDERR_UNSUPPORTED;` instead of the identity loop. Recommended: keep the identity fallback and
     replace the comment at D:surface.cpp:1740-1741 with one stating it is a deliberate deviation from XP's UNSUPPORTED.
     The DC choice (screen vs the object's monitor) matters only for a DirectDraw object created on a secondary-monitor GUID.
     Fix if multi-monitor is ever modelled: use the DC of the object's device.

2. **The ramp stays on while a window-mode exclusive game is in the background** (cosmetic edge case).
   - XP: deactivation takes the ramp off the device (X:FUN_7375d5aa_7375d5aa.c:41 → … → X:FUN_73734478_73734478.c:79, and
     X:FUN_7375aa94_7375aa94.c:48-50). Reactivation puts it back (X:FUN_7375ac27_7375ac27.c:44-46).
   - dd2d9: `OnActivate(false)` (D:ddraw.cpp:554-577) doesn't touch gamma. With `display = window` the inactive window stays
     visible (D:ddraw.cpp:572-573) and keeps showing the game's ramp. With switch/borderless the window is minimized, so nothing differs.
   - Fix (optional): in `OnActivate`, call `presenter_->SetGamma(identity)` when going inactive in window display mode, and
     `ApplyGamma()` on reactivation. The simplest form is a `backgrounded_` term in `ApplyGamma`'s condition plus a call at both
     ends of OnActivate. Arguably not worth it: XP had no "visible inactive exclusive window".

3. **Faults inside the methods** (edge case, no change suggested).
   - XP: a bad This (or a bad DDCOLORCONTROL pointer) → DDERR_EXCEPTION 0x88760037 through SEH. A bad non-NULL ramp pointer
     crashes, because the copy is outside the try block (see the exception-handling note at the top).
   - dd2d9: no SEH. A bad This or a bad ramp pointer crashes.
   - Fix: none. Only programs that pass garbage on purpose (test suites) could tell the difference.

4. **DDSGR_CALIBRATE with a registered calibrator** (edge case, no change suggested).
   - XP: if `HKLM\Software\Microsoft\DirectDraw\GammaCalibrator\Path` exists, GetCaps reports DDCAPS2_CANCALIBRATEGAMMA and
     `SetGammaRamp(CALIBRATE, ramp)` runs the ramp through the calibrator DLL (X:FUN_7373b5cc_7373b5cc.c:113-121,
     X:FUN_73738eaf_73738eaf.c:5-9, SetGammaRamp.c:84-107).
   - dd2d9 behaves like XP without the key: no CANCALIBRATEGAMMA, CALIBRATE → DDERR_UNSUPPORTED before the primary check
     (D:surface.cpp:1763-1764), the same order as XP.
   - Fix: none. Stock XP had no calibrator registered.

5. **Device-side failure modes not modelled** (no change suggested).
   - XP: with no gamma hardware, or if saving the original ramp fails, SetGammaRamp still returns DD_OK, but nothing is shown
     (X:FUN_737642d1_737642d1.c:8-17).
   - dd2d9 always applies the ramp in software. Games get the effect they asked for. Keep.

### Agreements checked (no change)

- QI: GammaControl for **any** surface, no caps check, the same object returned on every QI including QI from the gamma
  object itself (XP lines 190-200, 93-101). ColorControl → E_NOINTERFACE, which is what XP does without the
  COLORCONTROLPRIMARY/OVERLAY caps that dd2d9 doesn't advertise (XP lines 278-290). IUnknown/surface IIDs from the gamma
  object resolve like from the surface.
- AddRef/Release on the gamma object return the gamma interface's own count, keep the surface alive and ignore over-release
  (X:IDirectDrawSurface7_AddRef.c:24-29, X:IDirectDrawSurface7_Release.c:31).
- GetGammaRamp check order and codes: NULL/flags/non-primary → INVALIDPARAMS, then lost → SURFACELOST, then the stored ramp,
  then the device ramp (XP lines 25-52 vs D:surface.cpp:1744-1751). DDCAPS2_PRIMARYGAMMA is always advertised, so XP's caps
  term is always true here.
- SetGammaRamp check order and codes: flags → INVALIDPARAMS, CALIBRATE → UNSUPPORTED, non-primary → INVALIDPARAMS, lost →
  SURFACELOST, NULL → drop the ramp and DD_OK, else store → DD_OK (XP lines 29-70 vs D:surface.cpp:1762-1778).
- The ramp is shown only while the object is exclusive. It is stored and returned by Get but invisible in DDSCL_NORMAL. It
  survives leaving and re-entering exclusive mode and display mode changes, it goes away when the primary is destroyed, and a
  new primary starts without one (§5 vs D:ddraw.cpp:1047, 1096-1105, 1216, 2217, D:surface.cpp:270-273). XP only puts the
  device's own ramp back. Since dd2d9 never touches the device ramp, the identity in its window is the same visible result.
- The ramp lives with the primary surface object, not the DirectDraw object. Flips don't move it: in XP it is on the LCL, and
  in dd2d9 on the `Surface`.
- The 8-bit palette path applies the ramp to the palette entries (D:presenter.cpp:365-369), which is what a hardware LUT does.

## Not determined

- Whether XP's `DdSetGammaRamp` (GdiEntry15 → win32k/dxg) rejects "extreme" ramps the way GDI's `SetDeviceGammaRamp` does on XP.
  If it does, games that fade to black through the gamma ramp would show no fade on XP but a fade under dd2d9. That code is in
  the kernel, not ddraw.dll.
- Whether `FUN_737391bd`:288 (the surface invalidation inside the mode-change re-init) is reached on every SetDisplayMode. It is
  in the `param_2 != 0` branch, and `FUN_7373cc84`:49 passes 1, but the conditions at `FUN_737391bd`:228-234 weren't fully
  resolved. Either way the ramp ends up re-applied (`FUN_7373cc84`:74-77). Only the intermediate restore is in doubt.
- The DD LCL's initial value at +0x68. SetGammaRamp loads the calibrator only when it is -1 (A:73764578). If creation leaves it 0,
  CALIBRATE would always fail even with the registry key. Irrelevant to dd2d9.
- The DDHAL callback-table offset 0x278 used by Get/SetColorControls, and the DDRAWI_GETCOLOR/SETCOLOR values 1/2, were taken from
  the call shape and DDK naming. They were not verified against a header in `$S/types` (its ddrawi.h lacks the ColorControl types).
- What an emulation-only DirectDraw object (DDCREATE_EMULATIONONLY) reports in `ddCaps.dwCaps2`. If PRIMARYGAMMA is absent there,
  XP's Get/SetGammaRamp would return INVALIDPARAMS for such objects. Not checked.
- The other `FUN_73734478` callers (X:FUN_7376017f_7376017f.c:147 via BltFast/`FUN_73764b29`, and
  X:IDirectDrawSurface7_SetSurfaceDesc.c:185) would also restore gamma and color controls. Whether they can ever run on a primary
  was not traced (probably not).
