# IDirectDrawSurface (1/2/3/4/7) and IDirectDrawGammaControl: the methods other than Blt/Lock/Flip/DC/palette/clipper/color keys

Reference: ddraw.dll 5.3.2600.5512 (XP SP3). Decompilation in
`$S/decomp-annotated/` (`$S` = the session scratchpad). Line numbers are those of the plain-named
file (`IDirectDrawSurface7_X.c`), which is identical to its `_<addr>.c` twin.
dd2d9 references are to `src/` at the time of writing.

## Conventions used in XP's code (they apply to every method below)

- **No pointer validation in this retail build.** The methods run inside an SEH frame. A NULL or bad
  pointer argument (or This) faults, and the handler (e.g. 0x7374644b for AddAttachedSurface) returns
  `0x80070057` = `DDERR_INVALIDPARAMS` = `E_INVALIDARG`. So a NULL out-pointer yields
  DDERR_INVALIDPARAMS **at the point it is first dereferenced**, which can come after other checks or
  side effects (noted where it matters). The only explicit checks are `IsBadReadPtr(riid)` (QI),
  `IsBadCodePtr(callback)` (the Enum methods) and `IsBadWritePtr(This)` plus a vtable check (Release).
- "lost" = `lcl->dwFlags & DDRAWISURF_INVALID (0x10000000)`. The error is DDERR_SURFACELOST.
- `0x8876028F` (an undocumented DDHRESULT 655) is returned for `DDSCAPS_OPTIMIZED` (0x80000000)
  surfaces. dd2d9 cannot create those, so that check is ignored below.
- The surface "attach list" (`lcl->lpAttachList`) is a singly linked list and **new entries go at its
  head** (`FUN_7373530c` lines 28-30). GetAttachedSurface, EnumAttachedSurfaces and
  DeleteAttachedSurface(NULL) all walk it newest first.
- Reference counts: each interface pointer (INT) has its own `dwIntRefCnt`. The surface (LCL) keeps
  the sum, and the GBL keeps a count shared by every LCL. AddRef/Release return the INT count.
- Interface versions: one function usually serves several vtables and tells them apart by
  `This->lpVtbl`. **Surface/Surface2/Surface3 hand out IDirectDrawSurface (v1) interfaces** from
  GetAttachedSurface, EnumAttachedSurfaces and EnumOverlayZOrders. They call `QueryInterface`
  with `IID_IDirectDrawSurface` (data at 0x73734f44 decodes to 6C14DB81-A733-11CE-A521-0020AF0BE560).
  Surface4 hands out v4 interfaces and Surface7 hands out v7 ones.

---

## AddRef — `IDirectDrawSurface7_AddRef.c` (all surface versions, gamma, color control, kernel)

1. If the LCL has `DDRAWISURF_ISFREE (4)`, the method returns 0 and changes nothing (line 23).
2. Otherwise it increments the GBL, LCL and INT counts and returns the **INT** count (lines 24-29).

dd2d9 (`Surface::AddRef`, surface.cpp:322) counts per slot (kRefV1..V7, kRefGamma). This matches.

## Release — `IDirectDrawSurface7_Release.c`

1. If `IsBadWritePtr(This, 0x18)` fails, or the vtable is not one of the surface, color control,
   gamma or kernel tables, the method returns 0 (lines 23-28, 47-50).
2. If the LCL is ISFREE or the INT count is 0, it returns 0 (line 31).
3. Otherwise `FUN_73733099(This, 0, 0)` (internal release) does the work and its result is the
   return value (the new INT count).
   - `FUN_73733099_73733099.c:45`: **if the GBL count is 1 and the LCL has
     `DDRAWISURF_IMPLICITCREATE (2)` (it is a member of a complex surface), the method returns 0 without
     decrementing anything.** The root's reference can't be released by the game.
   - When the LCL count reaches 0, the surface is torn down (lines 60-182). Every attach-list entry is
     detached (`FUN_7373545a(this, x, 1, 1)`). Implicit members are destroyed by releasing their INT as
     many times as its count (lines 134-148). That is the "members die with the root" behavior dd2d9
     deliberately doesn't copy. Every "attached-from" link is also removed (line 155). Gamma is restored
     (`FUN_73734478` → `FUN_737343fb`).
   - For a MIPMAP surface, the chain's mip counts are recomputed once it is destroyed
     (Release.c:33-38, `FUN_7375ccf7`).

## QueryInterface — `IDirectDrawSurface7_QueryInterface.c` (all surface versions, gamma, color control, kernel)

Order:
1. `*ppv = NULL` (line 41). It runs before anything else, so a NULL ppv gives DDERR_INVALIDPARAMS through SEH.
2. `IsBadReadPtr(riid, 16)` → DDERR_INVALIDPARAMS (lines 42-47).
3. Private GUID {1bab8e96-9cfe-4ce3-bc72-d7e9e99a2175} (0x73735174) → a **new** v1 INT each time
   (`FUN_7373e7ca`), unless This is v1 (lines 63-66). It is internal and dd2d9 can ignore it.
4. `IID_IUnknown` or `IID_IDirectDrawSurface` → the v1 INT (This if This is v1, else found or created
   by `FUN_7373e5c8`), AddRef'd → S_OK (lines 89-101, 114).
5. `IID_IDirectDrawSurface2`, `3`, `4`, `7` → that version's INT, AddRef'd (lines 115-174).
6. `IID_IDirectDrawColorControl` → only if (primary and `DDCAPS2_COLORCONTROLPRIMARY`) or (overlay and
   `DDCAPS2_COLORCONTROLOVERLAY`), else E_NOINTERFACE (lines 278-287).
7. `IID_IDirectDrawGammaControl` → the gamma INT for **any** surface (not only the primary) (lines 198-200).
8. `IID_IDirectDrawSurfaceKernel` (60755da0-…) → only with kernel-mode support (`FUN_73765d79`),
   else E_NOINTERFACE (lines 266-276).
9. Otherwise, a Direct3D IID:
   - The already-created D3D objects (`lpSurfMore->lpIUnknowns`) are searched for the IID first
     (lines 214-232). A second QI for a texture/device IID reuses the same object.
   - **If the DirectDraw object is a DirectDraw7 one (`dwLocalFlags & DDRAWILCL_DIRECTDRAW7
     0x20000`), the result is E_NOINTERFACE** (line 233).
   - d3dim is loaded. If that fails, its error is returned (lines 234-235). Then `Direct3DCreateTexture`
     is tried, then `Direct3DCreateDevice`. An error other than E_NOINTERFACE from either is returned.
     The new object is remembered, and the QI is forwarded to it (lines 236-264).
10. → E_NOINTERFACE (line 290).

dd2d9 (`Surface::QueryInterface`, surface.cpp:292-317) matches for 1, 4, 5 and 7. See differences 15 and 16.

## GetCaps — `IDirectDrawSurface7_GetCaps.c`, `IDirectDrawSurface3_GetCaps.c`

- There are no checks: no lost check, and a NULL pointer gives DDERR_INVALIDPARAMS through SEH. Returns DD_OK.
- v4/v7 copy `dwCaps` plus caps2/3/4 from `lpSurfMore->ddsCapsEx` (7_GetCaps.c:21-25).
- v1/2/3 fill a local DDSCAPS2 and copy only `dwCaps` (3_GetCaps.c:19).

dd2d9 (wrappers.h:72-78) matches.

## GetSurfaceDesc — `IDirectDrawSurface7_GetSurfaceDesc.c` (v4/v7), `IDirectDrawSurface3_GetSurfaceDesc.c` (v1/2/3)

- v4/v7: requires `dwSize == 0x7c`, v1-3 require `0x6c`, otherwise DDERR_INVALIDPARAMS
  (7:19-28, 3:19-28). A NULL pointer gives DDERR_INVALIDPARAMS through SEH. There is no lost check.
- The structure is zeroed to its own size and filled by `FUN_73733184(lcl, desc)`:
  - `dwFlags = DDSD_CAPS`, and `ddsCaps.dwCaps` is the LCL caps.
  - Size ≥ 0x7c only: caps2/3/4. `dwTextureStage` plus **dwFlags reset to CAPS|DDSD_TEXTURESTAGE** if
    the stored stage has bit 0x100000. DDSD_FVF and `dwFVF` if an FVF is set.
  - `lpSurface = NULL` and DDSD_LPSURFACE is never set.
  - DDSD_CKDESTBLT / DDSD_CKSRCBLT and the key, if the LCL has HASCKEYDESTBLT / HASCKEYSRCBLT.
  - **DDSD_BACKBUFFERCOUNT only if the LCL was created as a chain's front buffer**
    (`dwFlags & 0x04000000`). The value is `lcl->dwBackBufferCount`.
  - DDSD_MIPMAPCOUNT with `lpSurfMore->dwMipMapCount` for every surface with DDSCAPS_MIPMAP
    (each level reports its own remaining count).
  - Size flags: normally HEIGHT|WIDTH|PITCH. A FourCC surface gets HEIGHT|WIDTH plus LINEARSIZE (DXT1-5)
    or PITCH (any other FourCC). An execute buffer gets WIDTH|PITCH.
  - DDSD_PIXELFORMAT with the surface's own format, or the display format when it has none (not for a
    Z buffer through v1, see below).
  - **Z buffer, 0x6c structure: DDSD_ZBUFFERBITDEPTH and `dwZBufferBitDepth`, no DDSD_PIXELFORMAT**
    (the pixel format bytes are still copied). Through 0x7c: DDSD_PIXELFORMAT.
  - DDSD_CKDESTOVERLAY / DDSD_CKSRCOVERLAY **only when the LCL has overlay data
    (`DDRAWISURF_HASOVERLAYDATA 0x4000`)**. Otherwise those keys are zeroed.
  - Never set: DDSD_ALPHABITDEPTH, DDSD_REFRESHRATE.

dd2d9: wrappers.h:86-94 (size per version, matches) and `Surface::GetSurfaceDesc` surface.cpp:785-809.
See difference 13.

## SetSurfaceDesc — `IDirectDrawSurface3_SetSurfaceDesc.c` (v3), `IDirectDrawSurface7_SetSurfaceDesc.c` (v4/v7)

v3 (3_SetSurfaceDesc.c:20-32): `flags != 0` or `dwSize != 0x6c` → DDERR_INVALIDPARAMS. Otherwise the
0x6c bytes are copied into a zeroed 0x7c structure (so caps2/3/4 are 0), which is passed to the v7 code.

v4/v7, in order:
1. OPTIMIZED → 0x8876028F (line 71).
2. `flags != 0` or `dwSize != 0x7c` → DDERR_INVALIDPARAMS (line 75 → 274).
3. Lost → DDERR_SURFACELOST (line 77).
4. **Locked (`gbl->dwUsageCount != 0`, which includes an outstanding GetDC) → DDERR_SURFACEBUSY**
   (line 82). This comes before the surface-type check.
5. DDERR_INVALIDSURFACETYPE (lines 86-90) if any of these holds:
   - the game didn't request DDSCAPS_SYSTEMMEMORY at creation (`!(gbl->dwGlobalFlags & DDRAWISURFGBL_SYSMEMREQUESTED)`);
   - the caps include ALLOCONLOAD, PRIMARYSURFACE, PRIMARYSURFACELEFT or OVERLAY;
   - caps2 has TEXTUREMANAGE or D3DTEXTUREMANAGE;
   - the LCL is `DDRAWISURF_PARTOFPRIMARYCHAIN (0x20)`.
6. DDERR_INVALIDPARAMS (lines 93-102 → 274) unless all of these hold:
   - `dwFlags & ~(CAPS|HEIGHT|WIDTH|PITCH|LPSURFACE|PIXELFORMAT) == 0`;
   - WIDTH → `dwWidth != 0`; HEIGHT → `dwHeight != 0`;
   - PITCH → `lPitch > 0 && lPitch % 4 == 0`;
   - **PIXELFORMAT → the surface has its own pixel format (`DDRAWISURF_HASPIXELFORMAT`, i.e. it was
     created with DDSD_PIXELFORMAT)**;
   - WIDTH and PITCH are both given or both absent;
   - DDSD_LPSURFACE is given and `lpSurface != NULL`.
7. DDSD_CAPS: `ddsCaps.dwCaps != 0` → DDERR_INVALIDCAPS. `dwCaps3 != 0 || dwCaps4 != 0` → DDERR_INVALIDCAPS
   (`dwCaps2` is not checked) (lines 103-112).
8. The current description with the new width/height/pitch/format is checked by the CreateSurface
   validator `FUN_73736cad` (line 139). Pixel format errors give DDERR_INVALIDPIXELFORMAT or
   DDERR_INVALIDPARAMS, DXT sizes must be multiples of 4, and mip levels must have power-of-two sizes
   that fit the mip count. **There is no minimum-pitch check.**
9. The driver/HEL CanCreate and Create callbacks run. If one handles the call with an error, that error is returned (lines 142-182).
10. Commit: the old memory is freed. W/H/pitch/format are stored, `fpVidMem = lpSurface`, and the GBL
    gets `DDRAWISURFGBL_ISCLIENTMEM` (lines 183-211). **The contents stamp (GetUniquenessValue) is set to 0**
    (line 214). The kernel-mode surface is recreated (GdiEntry4). A failure there on a
    non-system-memory surface gives DDERR_INVALIDPARAMS (lines 240-243).

dd2d9: wrappers.h:130-135 plus surface.cpp:811-845. The "flip-chain member can't be reshaped" rule
(surface.cpp:837) is dd2d9's own. XP allows it (only primary chains are refused, by step 5). See difference 12.

## GetPixelFormat — `IDirectDrawSurface7_GetPixelFormat.c`

1. **`ddpf->dwSize != 32` → DDERR_INVALIDPARAMS** (line 23 → 43). A NULL pointer gives the same result through SEH.
2. Execute buffer → DDERR_INVALIDSURFACETYPE (line 40).
3. It copies 32 bytes: the surface's own format if HASPIXELFORMAT, else the display format (lines 24-38).

dd2d9 (surface.cpp:779-783) doesn't check dwSize. See difference 11.

## AddAttachedSurface — `IDirectDrawSurface7_AddAttachedSurface.c` (all versions)

Validation, in order:
1. CANNOTATTACH (lines 74-88) if any of these holds:
   - **v7 (any vtable other than 1/2/3/4) and not exactly one of the two is a Z buffer**
     (`(a.caps ^ t.caps) & DDSCAPS_ZBUFFER == 0`);
   - either surface is an execute buffer;
   - This is a cube map (`caps2 & DDSCAPS2_CUBEMAP`) and the attachee is not a Z buffer;
   - either surface is OPTIMIZED.
2. **This is a primary and the caller is not the exclusive owner, or the DirectDraw object isn't
   DDRAWI_FULLSCREEN → DDERR_NOEXCLUSIVEMODE** (lines 89-94). This holds for any attachment, Z buffers
   included.
3. This == attachee → CANNOTATTACH (line 96).
4. Either surface lost → DDERR_SURFACELOST (lines 97-100).
5. This's attach list already holds **this interface pointer** of the attachee → DDERR_SURFACEALREADYATTACHED (lines 101-107).
   Only that direction is checked.
6. Different DirectDraw GBLs on two display drivers → DDERR_DEVICEDOESNTOWNSURFACE (lines 108-113).
7. Size (lines 114-125):
   - both TEXTURE|MIPMAP: This must be at least as big as the attachee;
   - otherwise, if both or neither are textures, the sizes must be equal → CANNOTATTACH;
   - **if exactly one is a texture, there is no size check.**
8. One of each kind (lines 126-137): if the attachee has any of MIPMAP|TEXTURE|ZBUFFER|ALPHA, and This
   already has an attachment sharing one of those bits (for TEXTURE: with the same MIPMAP-ness), the result is
   CANNOTATTACH. **So a second Z buffer can't be attached.**
9. The attachee has DDSCAPS_FLIP and is "flip-compatible" with This (below) → CANNOTATTACH (line 138).
10. Memory types (lines 141-171): system memory and video memory can't be mixed (CANNOTATTACH), except
    a system-memory Z buffer on a video-memory surface.
11. The driver/HEL AddAttachedSurface callback runs. If it handles the call with an error, that error is returned (lines 172-189).

"Flip-compatible" is `FUN_73733941(This, attachee)`: same ZBUFFER, TEXTURE, OVERLAY and ALPHA bits, and
not both MIPMAP textures. **It is independent of the interface version (1-4) and of pixel format.**

The attachment is made like this (lines 190-252):
- Not flip-compatible, or the attachee has FLIP → a plain attachment This→attachee, which AddRefs **the
  attachee interface pointer passed**. For a MIPMAP attachee, the mip counts are recomputed (`FUN_7375ccf7`).
  The method returns DD_OK.
- Flip-compatible and This has no FLIP (a new two-surface ring): links are made This→attachee
  (AddRef) and attachee→This (implicit, no AddRef). If This lacks BACKBUFFER, the attachee gets
  BACKBUFFER, This gets FRONTBUFFER and `This.dwBackBufferCount = 1`. Otherwise the attachee gets
  FRONTBUFFER and its count is 1. Both get FLIP.
- Flip-compatible and This already has FLIP: the attachee is inserted right after This. The
  attachee's FRONT/BACK bits are cleared. **If This is the FRONTBUFFER, the attachee gets BACKBUFFER and
  the old next surface loses BACKBUFFER.** The attachee gets FLIP and the ring's front buffer
  `dwBackBufferCount++`. Links: This→next is removed, then This→attachee (AddRef) and attachee→next (implicit
  if the old link was) are added. The net change in the old next's references is zero.

## DeleteAttachedSurface — `IDirectDrawSurface7_DeleteAttachedSurface.c` (all versions)

1. **`dwFlags != 0` → DDERR_INVALIDPARAMS** (line 25).
2. This lost → SURFACELOST. The attachee (if non-NULL) lost → SURFACELOST (lines 29-35).
3. OPTIMIZED → 0x8876028F (line 62).
4. **Attachee NULL → every entry of This's attach list is detached, newest first. The method stops at the
   first error and returns it** (lines 38-49). An implicit entry gives CANNOTDETACH, after the explicit
   ones attached later than it have already gone.
5. Otherwise `FUN_7373545a(This, attachee, 1, 0)`:
   - this interface pointer isn't in This's list → **DDERR_SURFACENOTATTACHED** (line 124);
   - the entry is implicit (part of a created complex surface, or the back link of a ring) → DDERR_CANNOTDETACHSURFACE (line 41);
   - if the attachee is This's next ring member, the ring is repaired (lines 43-108). The ring's front buffer
     `dwBackBufferCount--`.
     - If the count reaches 0 (two-surface ring): This loses FRONT|BACK|FLIP and gets back
       the FRONT/BACK it was created with.
     - Otherwise: if the attachee was BACKBUFFER, the next surface gets BACKBUFFER. If the attachee was
       FRONTBUFFER, the next surface becomes FRONTBUFFER (loses BACK) and takes over the count, the one after
       it gets BACKBUFFER, and the old front's count becomes 0. **If the attachee was neither, the
       other surfaces are left alone.** The attachee loses FRONT|BACK|FLIP and gets back its creation bits.
   - Both links are removed. The attachee is Released if the forward link held a reference
     (`FUN_737355d3`). This is where the documented DirectDraw 1 leak comes from (the AddRef/Release pairs at lines 70-81, 112-120).
6. For a MIPMAP This, the mip counts are recomputed. The method returns DD_OK.

## EnumAttachedSurfaces — `IDirectDrawSurface7_EnumAttachedSurfaces.c` (all versions)

1. `IsBadCodePtr(callback)` → DDERR_INVALIDPARAMS (lines 41, 72-73).
2. Lost → DDERR_SURFACELOST (line 45 → 70).
3. For each attach-list entry, **newest first** (implicit links included, so the last back buffer of a
   ring lists the front), it builds a description (size 0x6c for v1-3, 0x7c for v4/v7) and gets an
   interface through QueryInterface (**v1 for Surface/2/3**, v4, v7), which AddRefs it for the callback.
   It then calls the callback, and stops when the callback returns 0 (DDENUMRET_CANCEL) (lines 47-65).
4. Returns DD_OK.

## GetAttachedSurface — `IDirectDrawSurface3_GetAttachedSurface.c` (v1/2/3), `IDirectDrawSurface4_…` (v4), `IDirectDrawSurface7_…` (v7)

1. Lost → DDERR_SURFACELOST (3:34/44, 4:29/45, 7:19/30).
2. v4/v7: **`caps.dwCaps & 0x03000000` (reserved bits) → DDERR_INVALIDCAPS** (4:30/42, 7:20/27), then
   `*out = NULL`. v1-3: only `dwCaps` is used and caps2/3/4 are 0 (3:20-23, 35).
3. Search (`FUN_73735269_73735269.c`): `*out = NULL` (line 17), then the attach list is walked **newest
   first**. An entry matches if **all four** requested caps words are fully present in its caps
   **and at least one requested bit overlaps** (lines 28-34). So **all-zero caps never match →
   DDERR_NOTFOUND**. The first match wins. DDERR_MOREDATA is never returned.
4. On a match it calls QueryInterface on the attachee with IID_IDirectDrawSurface (**for v1, v2 and v3**),
   v4 or v7, which AddRefs that interface. Returns DD_OK. No match → DDERR_NOTFOUND (line 25).

## GetDDInterface — `IDirectDrawSurface7_GetDDInterface.c` (v2+)

It has no checks (NULL gives DDERR_INVALIDPARAMS through SEH). `*out = lpSurfMore->lpDD_int` (the interface the
surface was created through), which is AddRef'd. Returns DD_OK (lines 20-25). dd2d9 (surface.cpp:896) matches.

## Initialize — `IDirectDrawSurface7_Initialize.c:8`

Always DDERR_ALREADYINITIALIZED, with no checks. dd2d9 (wrappers.h:95) matches.

## Private data — `IDirectDrawSurface7_{Set,Get,Free}PrivateData.c` (v4/v7)

SetPrivateData (`FUN_7376c7d4_7376c7d4.c`):
1. **`size == 0` or `flags & ~(DDSPD_IUNKNOWNPOINTER|DDSPD_VOLATILE)` → DDERR_INVALIDPARAMS** (line 14).
2. **Any existing entry for the GUID is freed first** (its IUnknown is Released) (line 17).
3. Not IUNKNOWNPOINTER: `size` bytes are copied (NULL data faults → DDERR_INVALIDPARAMS, with the old entry already gone).
   IUNKNOWNPOINTER: `size != 4` → DDERR_INVALIDPARAMS (old entry already gone, lines 41, 56). Otherwise
   `data` itself is the IUnknown*, which is AddRef'd (lines 42-43).
4. The flags and the **current contents stamp** (uniqueness value) are stored, and the entry goes at the list head.

GetPrivateData (`FUN_7376c8d9_7376c8d9.c`):
1. GUID not found → DDERR_NOTFOUND (`size` isn't touched).
2. **DDSPD_VOLATILE and (the current stamp is 0 or differs from the stored one) → `*size = 0`, DDERR_EXPIRED** (lines 31-34).
3. `*size < stored` → `*size = stored`, DDERR_MOREDATA (lines 36-38).
4. The data is copied (the pointer value for IUNKNOWNPOINTER, with no AddRef), `*size = stored`, DD_OK. A NULL size
   or data faults → DDERR_INVALIDPARAMS.

FreePrivateData (`FUN_7376c781`): not found → DDERR_NOTFOUND. Otherwise the entry is unlinked, its IUnknown Released, DD_OK.

## GetUniquenessValue / ChangeUniquenessValue (v4/v7)

- The value is `gbl_more->dwContentsStamp`. At creation it is 1, or **0 for DDSCAPS_OWNDC and DDSCAPS_VIDEOPORT
  surfaces** (`FUN_73737235_73737235.c:535-539`). SetSurfaceDesc sets it to 0 (see above).
- GetUniquenessValue writes it, with no checks (7_GetUniquenessValue.c:19).
- ChangeUniquenessValue (7_ChangeUniquenessValue.c:21-26), and likewise every internal change (Unlock,
  Blt, Flip): **if the stamp is 0 it stays 0**. Otherwise it is incremented, skipping 0 on wrap-around. Returns DD_OK.

## SetPriority / GetPriority / SetLOD / GetLOD (v7)

They are forwarded to Direct3D texture-manager callbacks stored after the DirectDraw LCL, with
`lpSurfMore->pCreatedDDSurfaceDesc2` as the texture handle.
- Get/SetLOD and GetPriority (e.g. 7_GetPriority.c:24-36):
  - `FUN_7375fab9` fails (a mip sublevel `DDSCAPS2_MIPMAPSUBLEVEL`, or a cube face other than the root)
    → DDERR_INVALIDPARAMS;
  - no texture handle (any non-texture) → DDERR_INVALIDOBJECT;
  - no callback → DDERR_D3DNOTINITIALIZED;
  - otherwise the callback's result.
- SetPriority (7_SetPriority.c:15/25) has no checks and calls the callback directly (Windows crashes for
  non-textures and sublevels).
- Wine tests (ddraw7.c test_resource_priority) on real Windows: a managed texture → DD_OK (priority
  starts at 0); a non-managed texture → DDERR_INVALIDPARAMS (Get and Set); an offscreen plain →
  DDERR_INVALIDOBJECT. Wine returns DDERR_INVALIDOBJECT from Set/GetLOD on non-managed textures. **The XP
  callback's code for that case is in d3dim700.dll and was not checked.**

## GammaControl::GetGammaRamp — `IDirectDrawGammaControl_GetGammaRamp.c`

1. Ramp NULL, `flags != 0`, the surface not a primary, or no DDCAPS2_PRIMARYGAMMA → DDERR_INVALIDPARAMS (lines 25-27).
2. Lost → DDERR_SURFACELOST (line 33).
3. The surface's stored ramp is copied if set. Otherwise the result is `GetDeviceGammaRamp(dd->hDC)` (failure → DDERR_UNSUPPORTED) (lines 37-52).

## GammaControl::SetGammaRamp — `IDirectDrawGammaControl_SetGammaRamp.c`

1. `flags & ~DDSGR_CALIBRATE` → DDERR_INVALIDPARAMS (line 29 → 117). CALIBRATE without a calibrator → DDERR_UNSUPPORTED (lines 30-32).
2. Not a primary, or no DDCAPS2_PRIMARYGAMMA → DDERR_INVALIDPARAMS (line 34 → 117).
3. Lost → DDERR_SURFACELOST (line 36).
4. **Ramp NULL → the original device ramp is restored and the stored one dropped (`FUN_737343c3`), DD_OK** (lines 39-41).
5. Otherwise the ramp is stored on the surface (`lpSurfMore->lpGammaRamp`). **It is applied to the device only if the
   DirectDraw object has exclusive mode (`dwLocalFlags & DDRAWILCL_HASEXCLUSIVEMODE`)** (line 64). The result of
   applying it is ignored, and DD_OK is returned.
6. Elsewhere: the ramp is removed from the device when the exclusive application deactivates (`FUN_7375aa94`) and
   applied again when it activates (`FUN_7375ac27`). It is restored when the primary is destroyed (`FUN_73734478`).

## Overlay methods on non-overlay surfaces

| Method | XP checks in order (non-overlay surface) | Result |
|---|---|---|
| AddOverlayDirtyRect (7_AddOverlayDirtyRect.c) | execute buffer → INVALIDSURFACETYPE (90). Rect NULL/`left<0`/`top<0`/`left>right`/`top>bottom`/`bottom>height`/`right>width` → INVALIDPARAMS (47-50, 87) | **not system memory → DD_OK** (55). System memory → the HEL callback's result, or INVALIDPARAMS (81) |
| GetOverlayPosition (7_GetOverlayPosition.c) | `*x = *y = 0` first (25-26, NULL → INVALIDPARAMS). Lost → SURFACELOST (52) | DDERR_NOTAOVERLAYSURFACE (48) |
| SetOverlayPosition (7_SetOverlayPosition.c) | lost → SURFACELOST (133) | DDERR_NOTAOVERLAYSURFACE (129) |
| UpdateOverlay (7_UpdateOverlay.c) | This lost → SURFACELOST (331). Dest NULL → INVALIDPARAMS (SEH). Dest lost → SURFACELOST. `flags & 0xF8000000`, or fx NULL with DDOVER_DDFX, or `fx->dwSize != 0x38` → INVALIDPARAMS (88-95, 327) | DDERR_NOTAOVERLAYSURFACE (99) |
| UpdateOverlayDisplay (7_UpdateOverlayDisplay.c) | `flags & ~(0x30000)` → INVALIDPARAMS (48/88). Execute buffer → INVALIDSURFACETYPE (84) | **not system memory → DD_OK**. System memory → HEL callback, or DDERR_UNSUPPORTED |
| UpdateOverlayZOrder (7_UpdateOverlayZOrder.c) | — | DDERR_NOTAOVERLAYSURFACE (35) |
| EnumOverlayZOrders (7_EnumOverlayZOrders.c) | `IsBadCodePtr(cb)` → INVALIDPARAMS (44-48). Flags not 0/1 → INVALIDPARAMS (78-79) | **enumerates the device's overlay z-order list (empty without overlays) → DD_OK** |

---

## Differences from dd2d9

Severity: **game-visible** means a plausible game call sequence behaves differently. **Edge case** means
it only shows up with invalid parameters, unusual sequences or exact-return-code checks.

1. **Attaching a back buffer to a chain's front buffer doesn't update FRONT/BACK caps** (game-visible, rare).
   - XP: in an existing ring, if This is the FRONTBUFFER, the new surface gets BACKBUFFER and the old back buffer
     loses it (AddAttachedSurface.c:231-236).
   - dd2d9: `AttachForFlip` else-branch (surface.cpp:488-491) only sets FLIP. Afterwards
     `GetAttachedSurface(front, DDSCAPS_BACKBUFFER)` returns DDERR_NOTFOUND (chainNext lacks BACKBUFFER)
     instead of the new surface, and the old back buffer still claims BACKBUFFER.
   - Fix: in the else-branch, if `desc.ddsCaps.dwCaps & DDSCAPS_FRONTBUFFER`, set BACKBUFFER on `s` and clear it on
     the previous `chainNext`.
2. **Flip-chain building by AddAttachedSurface is limited to surfaces created through IDirectDraw/IDirectDraw2**
   (game-visible, rare).
   - XP: through Surface/2/3/4, any two "flip-compatible" surfaces form a ring: same ZBUFFER, TEXTURE, OVERLAY and
     ALPHA bits, not both mip textures (`FUN_73733941`). The interface version and pixel format don't matter.
     Examples: IDirectDraw4 offscreen plains or primaries, two same-size non-mip textures, two Z buffers.
   - dd2d9: `FlipAttachable` (surface.cpp:416-419) requires `version == 1`, excludes
     Z/texture/overlay, and `AttachForFlip` (475) requires the same format. A DirectDraw4 game doing this gets a
     plain attachment: no FLIP/FRONT/BACK caps, and Flip fails with DDERR_NOTFLIPPABLE.
   - Fix: make `FlipAttachable` use XP's rule (both surfaces, any interface 1-4) and drop the format test
     (or keep it only where Flip's buffer swap needs it, and log it).
3. **The v1-4 attachment rules are narrower than XP's** (game-visible for some engines).
   - XP accepts any pair that passes the size, one-per-kind and memory-type rules (AddAttachedSurface.c:114-171).
     For example: a Z buffer on a back buffer or offscreen plain without DDSCAPS_3DDEVICE, a Z buffer on a
     plain texture of any size, or a manually built mip chain (a smaller TEXTURE|MIPMAP attached to a
     bigger one, with the mip count updated).
   - dd2d9 (surface.cpp:428-433) refuses all of these with DDERR_CANNOTATTACHSURFACE.
   - Fix: implement XP's checks 7-10 instead of the caps whitelist. For manual mip chains, also make
     `NextMipLevel` accept attached MIPMAP levels that are outside the root's family (the D3D texture path relies on it).
4. **XP refuses some attachments that dd2d9 allows** (edge case).
   - XP: a second Z buffer (or second texture/alpha surface) on the same surface → CANNOTATTACH (126-137).
     Mixed system/video memory → CANNOTATTACH, except a system-memory Z buffer on a video-memory surface (141-171).
     A lost surface → SURFACELOST (97-100). Any attachment to a primary without exclusive fullscreen ownership
     → NOEXCLUSIVEMODE, Z buffers included (89-94). Through v7, Z to Z, or anything where neither or both are Z
     → CANNOTATTACH (74-77).
   - dd2d9 (surface.cpp:426-454) allows all of these, only checks exclusive mode for flip attachments (473),
     and through v7 only requires the attachee to be a Z buffer. It also refuses a Z buffer (This) getting a render target
     attached through v7, which XP allows.
   - The reverse attachment check is also different. dd2d9 returns SURFACEALREADYATTACHED if `s` already has This
     attached (446-447). XP only checks This's own list, so for a non-flip pair the reverse attachment succeeds.
   - Fix: add XP's checks in XP's order: caps, primary exclusivity, self, lost, already attached, size, one-per-kind,
     flip-member, memory.
5. **DeleteAttachedSurface ignores flags and mishandles NULL and unknown surfaces** (edge case. NULL could matter).
   - XP: `flags != 0` → INVALIDPARAMS (Delete.c:25). NULL attachee → detaches every explicit attachment, newest first,
     until an implicit one gives CANNOTDETACH (38-49). A surface that isn't attached → SURFACENOTATTACHED
     (FUN_7373545a:124). It also checks lost (29-35).
   - dd2d9 (surface.cpp:540-586): flags are ignored, NULL → CANNOTDETACHSURFACE with nothing detached (541),
     and not attached → CANNOTDETACHSURFACE (583/585).
   - Fix: check the flags first. Implement NULL as "detach all `attachRefs` plus attached flip-ring links,
     newest first, and return CANNOTDETACH if an implicit member remains". Return DDERR_SURFACENOTATTACHED when
     `s` is neither in `attached` nor `chainNext`.
6. **Detaching a plain ring member mislabels the next surface** (edge case: manual chains of 4 or more).
   - XP: if the detached surface was neither FRONT nor BACK, nothing else changes. If it was FRONT, the BACKBUFFER bit goes to
     the surface after the new front (FUN_7373545a:82-96).
   - dd2d9 (surface.cpp:552-561) gives `next` BACKBUFFER whenever `next` isn't the front, and in the FRONT case
     sets BACKBUFFER on `this` (right only for a 3-surface ring).
   - Fix: copy XP's three cases exactly. The FRONT case should also move the back buffer count to the new front.
7. **GetAttachedSurface with all-zero caps** (edge case).
   - XP: NOTFOUND (FUN_73735269:32).
   - dd2d9: `matches` (surface.cpp:619-622) accepts anything, so it returns the first link.
   - Fix: also require `(caps & s.caps) != 0` over all words. Compare dwCaps3/dwCaps4 too (v7).
8. **GetAttachedSurface v4/v7 don't refuse the reserved caps bits** (edge case).
   - XP: `dwCaps & 0x03000000` → DDERR_INVALIDCAPS (4_GetAttachedSurface.c:30, 7_…:20), checked after the lost check.
   - dd2d9: wrappers.h:61-70 doesn't check them.
   - Fix: add the check for V >= 4.
9. **Search and enumeration order** (edge case).
   - XP: attach lists are newest first (FUN_7373530c:28-30). GetAttachedSurface returns the newest matching
     attachment, and EnumAttachedSurfaces lists, for example, an attached Z buffer before the back buffer.
   - dd2d9 (surface.cpp:594-595, 624-627): ring next first, then `attached` oldest first.
   - Fix: walk `attached` (explicit part) in reverse before `chainNext`, if the creation-time order of
     complex members is kept as ddraw.dll builds it. **Uncertain**: the creation-time list order of complex
     surfaces wasn't audited here (see the creation spec).
10. **Surface2/Surface3 GetAttachedSurface and EnumAttachedSurfaces return the v2/v3 interface** (edge case).
    - XP returns and AddRefs the **IDirectDrawSurface (v1)** interface (3_GetAttachedSurface.c:37 with
      0x73734f44, EnumAttachedSurfaces.c:51-55; also EnumOverlayZOrders).
    - dd2d9 (wrappers.h:68, surface.cpp:596/608) uses `Iface(V)` and `SlotFor(V)`. Pointer identity and Release
      return values differ: for a created back buffer through v1 on XP, the count includes the root's.
    - Fix (optional): use `Iface(V >= 4 ? V : 1)` and `SlotFor(V >= 4 ? V : 1)`. XP games can't call
      v2/v3-only methods on the result anyway (on XP that would run past the v1 vtable).
11. **GetPixelFormat doesn't check `dwSize`** (edge case).
    - XP: `dwSize != sizeof(DDPIXELFORMAT)` → INVALIDPARAMS (GetPixelFormat.c:23).
    - dd2d9: surface.cpp:779-783.
    - Fix: `if (!pf || pf->dwSize != sizeof(DDPIXELFORMAT)) return DDERR_INVALIDPARAMS;`.
12. **SetSurfaceDesc check order and rules** (edge case).
    - XP (7_SetSurfaceDesc.c:71-112) differs from dd2d9 (wrappers.h:130-135, surface.cpp:811-837) in these ways:
      - **size per interface**: v3 only 0x6c, v4/v7 only 0x7c. dd2d9's `DescIn` takes either on any version;
      - **SURFACEBUSY before the surface type and parameter checks** (dd2d9 checks it last);
      - INVALIDSURFACETYPE also for ALLOCONLOAD and members of a primary chain;
      - **DDSD_PIXELFORMAT is refused (INVALIDPARAMS) on a surface created without an explicit pixel format**;
      - all INVALIDPARAMS conditions come before DDSD_CAPS's INVALIDCAPS (dd2d9 checks CAPS before width/pitch);
      - `dwCaps3/dwCaps4 != 0` → INVALIDCAPS;
      - a bad pixel format → DDERR_INVALIDPIXELFORMAT, not INVALIDPARAMS;
      - **afterwards the uniqueness value is 0, not incremented** (line 214).
    - Fix: reorder the checks, enforce the size per version in the wrapper, and remember "created with DDSD_PIXELFORMAT".
      Set `uniqueness = 0` (see 14).
13. **GetSurfaceDesc returns creation flags XP doesn't** (edge case).
    - dd2d9 copies the creation description (ddraw.cpp:1443-1448 keeps DDSD_ALPHABITDEPTH, DDSD_REFRESHRATE and their
      fields) and reports overlay color keys on any surface (surface.cpp:800-807).
    - XP builds the flags from scratch: CAPS|H|W|PITCH (or LINEARSIZE)|PIXELFORMAT, plus BLT keys,
      BACKBUFFERCOUNT (created front only), MIPMAPCOUNT, TEXTURESTAGE and FVF. Overlay keys need overlay data
      (FUN_73733184:126-145).
    - Fix: build `dwFlags` the way FUN_73733184 does instead of inheriting it, and zero fields that aren't reported.
14. **Uniqueness value semantics** (edge case).
    - XP: OWNDC/VIDEOPORT surfaces start at 0. 0 is sticky (Change/Unlock/Blt don't move it). Increments skip 0, and
      SetSurfaceDesc sets 0 (see above). DDSPD_VOLATILE private data then always reports DDERR_EXPIRED.
    - dd2d9 (objects.h:227, wrappers.h:140-145, all `uniqueness++` sites) starts at 1 and always increments.
    - Fix: start OWNDC surfaces at 0, use a helper `BumpUniqueness()` (`if (u && !++u) u = 1;`), and set 0 in SetSurfaceDesc.
15. **QueryInterface details** (edge case).
    - XP: a bad `riid` → INVALIDPARAMS (QI.c:42). D3D texture/device IIDs → E_NOINTERFACE on surfaces of a DirectDraw7
      object (233). A second QI for the same D3D IID returns the same object (214-232).
    - dd2d9 (surface.cpp:292-316, d3dproxy.cpp:563-585): no riid check, and texture QIs allowed on DD7 surfaces.
      Each device QI creates a new device.
    - Fix: check `s->dd`'s DirectDraw-7-ness (an object made by DirectDrawCreateEx, not the IDirectDraw7 QI) before
      D3DProxyQuerySurface, and cache device objects per IID.
16. **Over-releasing a complex member** (edge case).
    - XP: when the GBL count is 1 and the surface is implicit, Release is a no-op returning 0
      (FUN_73733099:45), so a later AddRef returns 2.
    - dd2d9 (surface.cpp:327-340) takes the slot to 0, keeps the surface (`root` set) and returns 0. A later AddRef returns 1.
    - Fix: in `Release`, if `root && total_ == 1`, return 0 without decrementing.
17. **Private data validation and VOLATILE** (edge case).
    - XP (FUN_7376c7d4:14-56, FUN_7376c8d9:31-34) differs in these ways:
      - `size == 0` → INVALIDPARAMS;
      - unknown flags → INVALIDPARAMS;
      - the old entry is freed **before** the IUNKNOWNPOINTER size check, so a failed set erases it (Wine's test notes this
        too);
      - DDSPD_VOLATILE → DDERR_EXPIRED with `*size = 0` once the uniqueness value changed or is 0.
    - dd2d9 (surface.cpp:905-948) has none of these. It also AddRefs before freeing, which is safer when the same object is set again.
    - Fix: add the size and flag checks and VOLATILE (store the stamp). The "erase on failed IUNKNOWNPOINTER set"
      behavior is optional.
18. **Priority/LOD on surfaces that aren't managed textures** (edge case).
    - XP (and real Windows per Wine's test_resource_priority): GetPriority/SetPriority on a non-managed texture →
      INVALIDPARAMS, on a non-texture → INVALIDOBJECT, on a mip sublevel or non-root cube face → INVALIDPARAMS.
      Set/GetLOD follow the same pattern (7_GetLOD.c:24-36). The exact code for non-managed textures comes from
      d3dim and is **uncertain**; Wine uses INVALIDOBJECT.
    - dd2d9 (wrappers.h:147-158) always returns DD_OK.
    - Fix: check DDSCAPS2_TEXTUREMANAGE|D3DTEXTUREMANAGE, MIPMAPSUBLEVEL, cube faces and TEXTURE as above.
19. **Gamma control** (game-visible for windowed games, otherwise edge case).
    - **Superseded by gamma-colorcontrol.md** (dd2d9 has changed since; the fault codes below are DDERR_EXCEPTION on XP).
    - XP (GetGammaRamp.c:25-52, SetGammaRamp.c:29-64):
      - only on the primary (others → INVALIDPARAMS);
      - flags checked;
      - lost → SURFACELOST;
      - Set(NULL) → restore, DD_OK;
      - the ramp is kept per primary, **applied only while the object is exclusive** and active, and dropped when the primary is
        destroyed.
    - dd2d9 (surface.cpp:1280-1295, presenter.cpp:148-167):
      - any surface;
      - flags ignored;
      - Set(NULL) → INVALIDPARAMS;
      - the ramp is global to the DirectDraw object and applied in windowed mode too;
      - it survives a new primary.
    - Fix: check primary/flags/lost. Treat NULL as a reset to identity. Reset the ramp when the primary is
      destroyed. Optionally apply it only while exclusive (a config switch, since applying it windowed is arguably nicer).
20. **Overlay methods on non-overlay surfaces** (edge case).
    - XP:
      - AddOverlayDirtyRect and UpdateOverlayDisplay return **DD_OK** on video-memory surfaces after validating the rect or flags;
      - EnumOverlayZOrders returns **DD_OK** after checking the callback and flags (0/1);
      - GetOverlayPosition zeroes `*x/*y` and checks lost first;
      - Set/GetOverlayPosition and UpdateOverlay check lost, and UpdateOverlay also checks dest/flags/fx before NOTAOVERLAYSURFACE.
    - dd2d9 (wrappers.h:39, 55, 83, 119, 122-124) returns DDERR_NOTAOVERLAYSURFACE unconditionally.
    - Fix: follow the table above. Only UpdateOverlayZOrder is right as it is.
21. **No lost-surface checks** in EnumAttachedSurfaces, DeleteAttachedSurface, AddAttachedSurface, SetSurfaceDesc and the
    overlay methods (edge case). XP returns DDERR_SURFACELOST in each (cited above). dd2d9 checks lost only in
    GetAttachedSurface (surface.cpp:618). Fix: add `IsLost()` checks at XP's positions.

### Matches (verified)

GetCaps (per version), GetSurfaceDesc size check per version, AddRef/Release returning the per-interface count,
QI for IUnknown/v1-v7/gamma, Initialize, GetDDInterface, GetAttachedSurface's v1-3 caps truncation and AddRef,
EnumAttachedSurfaces's description size per version and CANCEL handling, SetSurfaceDesc's parameter rules
(flags allowed, width/pitch pairing, pitch > 0 and DWORD-aligned, LPSURFACE required), GetPrivateData's
MOREDATA/NOTFOUND/INVALIDPARAMS, the IUNKNOWNPOINTER copy semantics, the attachment AddRef through the interface
used, and detaching only through the attaching interface (SURFACENOTATTACHED).

### Not determined

- What d3dim700's texture-manager callbacks return for Set/GetLOD and SetPriority on non-managed textures (see 18).
- The HEL overlay callback's result for AddOverlayDirtyRect/UpdateOverlayDisplay on system-memory non-overlay surfaces.
- The attach-list order of complex surfaces as CreateSurface builds them. It affects difference 9, and belongs to the creation audit.
