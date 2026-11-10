// What the DirectDraw object reports: display modes, caps, video memory, the device, and the
// surfaces that exist or could be made (EnumSurfaces).
#include "ddint.h"

#include <algorithm>

namespace dd2d9 {

HRESULT DDraw::GetDisplayMode(void* desc, int version) {
    if (!DescSizeOk(desc)) return DDERR_INVALIDPARAMS;
    Guard l;
    DWORD w, h, bpp;
    DisplayMode(&w, &h, &bpp);
    Format f = Format::ForDisplayBpp(bpp);
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    d.dwWidth = w;
    d.dwHeight = h;
    d.lPitch = LONG(w * f.Bytes());  // as ddraw.dll: not rounded
    // The display's own report, as ddraw.dll gives it (0 for the driver default). Asked again
    // when the mode answered changes, else at most every half second (g_lock): games ask
    // every frame, and it is a call into the kernel.
    static DWORD rate, rateAt, rateFor[3];
    const DWORD now = GetTickCount();
    if (!rateAt || now - rateAt >= 500 || rateFor[0] != w || rateFor[1] != h || rateFor[2] != bpp) {
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        rate = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
        rateAt = now | 1;  // (never 0: "not asked yet")
        rateFor[0] = w, rateFor[1] = h, rateFor[2] = bpp;
    }
    d.dwRefreshRate = rate;
    f.ToDDPF(&d.ddpfPixelFormat);
    static DWORD lastLogged[3];  // g_lock: logged when the answer changes (games ask every frame)
    if (lastLogged[0] != w || lastLogged[1] != h || lastLogged[2] != bpp) {
        DD2D9_TRACE("GetDisplayMode -> %lux%lu, %lu bpp%s", w, h, bpp,
                    modeSet_ ? " (the game's mode)"
                    : g_exclusiveDD && g_exclusiveDD != this && g_exclusiveDD->ShowsMode() ? " (another object's mode)"
                    : g_config.desktopBpp != 32 ? " (the desktop, desktopbpp=)" : " (the desktop)");
        lastLogged[0] = w, lastLogged[1] = h, lastLogged[2] = bpp;
    }
    DescOut(d, desc);  // only as much as the caller's structure holds
    // ddraw.dll then reports the size of this interface version's structure.
    *static_cast<DWORD*>(desc) = version >= 4 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
    return DD_OK;
}

// The display sizes offered: the classic modes plus whatever the monitor offers. A
// classic mode the monitor lacks still works: it falls back to a scaled borderless window.
static std::vector<SIZE> ModeSizes() {
    static const SIZE kSizes[] = {{320, 200}, {320, 240}, {400, 300}, {512, 384}, {640, 400}, {640, 480},
                                  {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 960},
                                  {1280, 1024}, {1600, 1200}, {1920, 1080}};
    std::vector<SIZE> sizes;
    LONG maxW = GetSystemMetrics(SM_CXSCREEN), maxH = GetSystemMetrics(SM_CYSCREEN);
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
        SIZE s = {LONG(dm.dmPelsWidth), LONG(dm.dmPelsHeight)};
        if (s.cx < 320 || s.cy < 200) continue;
        maxW = std::max(maxW, s.cx);
        maxH = std::max(maxH, s.cy);
        sizes.push_back(s);
    }
    for (const SIZE& s : kSizes)
        if (s.cx <= maxW && s.cy <= maxH) sizes.push_back(s);
    sizes.push_back({GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)});
    std::sort(sizes.begin(), sizes.end(),
              [](const SIZE& a, const SIZE& b) { return a.cx != b.cx ? a.cx < b.cx : a.cy < b.cy; });
    sizes.erase(std::unique(sizes.begin(), sizes.end(),
                            [](const SIZE& a, const SIZE& b) { return a.cx == b.cx && a.cy == b.cy; }),
                sizes.end());
    return sizes;
}

HRESULT DDraw::EnumDisplayModes(DWORD flags, const void* filter, void* ctx, void* callback, int descVersion) {
    if (!callback || (flags & ~DWORD(DDEDM_REFRESHRATES | DDEDM_STANDARDVGAMODES))) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 f = {};
    if (filter && !DescIn(filter, &f)) return DDERR_INVALIDPARAMS;
    const std::vector<SIZE> sizes = ModeSizes();
    using CB = HRESULT(WINAPI*)(void*, void*);
    static const DWORD kBpps[] = {8, 16, 32};
    for (DWORD bpp : kBpps) {
        if (filter && (f.dwFlags & DDSD_PIXELFORMAT) && f.ddpfPixelFormat.dwRGBBitCount != bpp) continue;
        for (const SIZE& s : sizes) {
            if (filter && (f.dwFlags & DDSD_WIDTH) && f.dwWidth != DWORD(s.cx)) continue;
            if (filter && (f.dwFlags & DDSD_HEIGHT) && f.dwHeight != DWORD(s.cy)) continue;
            if (filter && (f.dwFlags & DDSD_REFRESHRATE) && f.dwRefreshRate != refresh_) continue;
            Format pf = Format::ForDisplayBpp(bpp);
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
            d.dwWidth = DWORD(s.cx);
            d.dwHeight = DWORD(s.cy);
            d.lPitch = LONG(DWORD(s.cx) * pf.Bytes());  // as ddraw.dll: not rounded
            d.dwRefreshRate = (flags & DDEDM_REFRESHRATES) ? refresh_ : 0;
            pf.ToDDPF(&d.ddpfPixelFormat);
            union {
                DDSURFACEDESC v1;
                DDSURFACEDESC2 v2;
            } out;
            out.v1.dwSize = descVersion >= 2 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
            DescOut(d, &out);
            if (reinterpret_cast<CB>(callback)(&out, ctx) == DDENUMRET_CANCEL) return DD_OK;
        }
    }
    return DD_OK;
}

HRESULT DDraw::GetCaps(DDCAPS* drv, DDCAPS* hel) {
    // As ddraw.dll: the structure of DirectX 1, 3, 5 or 6, both checked before either is written.
    auto sizeOk = [](const DDCAPS* c) {
        return !c || c->dwSize == 0xAC || c->dwSize == 0x13C || c->dwSize == 0x16C || c->dwSize == sizeof(DDCAPS);
    };
    if ((!drv && !hel) || !sizeOk(drv) || !sizeOk(hel)) return DDERR_INVALIDPARAMS;
    DDCAPS c = {};
    c.dwSize = sizeof(c);
    c.dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_PALETTE |
               DDCAPS_CANBLTSYSMEM | DDCAPS_BLTDEPTHFILL | DDCAPS_CANCLIP | DDCAPS_CANCLIPSTRETCHED |
               DDCAPS_BLTFOURCC;  // (GetFourCCCodes' formats: converted by Blt)
    c.dwNumFourCCCodes = DWORD(std::size(kFourCCCodes));  // as GetFourCCCodes lists them
    c.dwCaps2 = DDCAPS2_WIDESURFACES | DDCAPS2_NOPAGELOCKREQUIRED | DDCAPS2_CANRENDERWINDOWED |
                DDCAPS2_PRIMARYGAMMA | DDCAPS2_FLIPNOVSYNC | DDCAPS2_FLIPINTERVAL | DDCAPS2_CERTIFIED;
    c.dwCKeyCaps = DDCKEYCAPS_SRCBLT | DDCKEYCAPS_DESTBLT;  // no key ranges (see SetColorKey)
    c.dwFXCaps = DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY | DDFXCAPS_BLTSHRINKX | DDFXCAPS_BLTSHRINKY |
                 DDFXCAPS_BLTSTRETCHXN | DDFXCAPS_BLTSTRETCHYN | DDFXCAPS_BLTSHRINKXN | DDFXCAPS_BLTSHRINKYN |
                 DDFXCAPS_BLTMIRRORLEFTRIGHT | DDFXCAPS_BLTMIRRORUPDOWN;
    c.dwPalCaps = DDPCAPS_8BIT | DDPCAPS_PRIMARYSURFACE | DDPCAPS_ALLOW256;
    // The raster operations Blt does, as ddraw.dll's software blitter: BLACKNESS,
    // SRCCOPY, WHITENESS (bit n of the array is the ROP with n in its high word).
    for (DWORD rop : {DWORD(BLACKNESS), DWORD(SRCCOPY), DWORD(WHITENESS)}) {
        const DWORD n = (rop >> 16) & 0xFF;
        c.dwRops[n / 32] |= 1u << (n % 32);
    }
    {
        Guard l;
        c.dwVidMemTotal = DWORD(VidMemTotal());
        c.dwVidMemFree = VidMemFree();
    }
    c.ddsCaps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER |
                       DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PALETTE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY |
                       DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_VISIBLE;
    if (g_config.d3d != kD3DOff) {
        // A 3D accelerator, as ddraw does with a 3D driver: dd2d9's own Direct3D (3-7).
        // Games check this before they try Direct3D.
        c.dwCaps |= DDCAPS_3D;
        c.ddsCaps.dwCaps |= DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP;
        c.dwZBufferBitDepths = DDBD_16 | DDBD_24 | DDBD_32;
    }
    if (OverlaysOffered(this)) {
        // Overlays (overlay.cpp): emulated, any stretch, mirroring, single colour keys.
        c.dwCaps |= DDCAPS_OVERLAY | DDCAPS_OVERLAYSTRETCH | DDCAPS_OVERLAYFOURCC;
        c.dwCKeyCaps |= DDCKEYCAPS_DESTOVERLAY | DDCKEYCAPS_SRCOVERLAY;
        c.dwFXCaps |= kOverlayFxCaps;
        c.ddsCaps.dwCaps |= DDSCAPS_OVERLAY;
        c.dwMaxVisibleOverlays = 16;  // (overlay.cpp's kMaxVisibleOverlays)
        c.dwMinOverlayStretch = 1;    // thousandths (overlay.cpp's kMinStretch, kMaxStretch)
        c.dwMaxOverlayStretch = 100000;
        Guard l;
        for (Surface* s : OverlayZOrder())
            if (s->desc.ddsCaps.dwCaps & DDSCAPS_VISIBLE) c.dwCurrVisibleOverlays++;
    }
    c.ddsOldCaps.dwCaps = c.ddsCaps.dwCaps;  // the DirectX 5 field, same value
    c.dwSVBCaps = c.dwCaps;
    c.dwSVBCKeyCaps = c.dwCKeyCaps;
    c.dwSVBFXCaps = c.dwFXCaps;
    c.dwVSBCaps = c.dwCaps;
    c.dwSSBCaps = c.dwCaps;
    for (int i = 0; i < DD_ROP_SPACE; i++) c.dwSVBRops[i] = c.dwVSBRops[i] = c.dwSSBRops[i] = c.dwRops[i];
    DD2D9_TRACE("GetCaps(%p, %p) -> dwCaps 0x%08lx, ddsCaps 0x%08lx%s", static_cast<void*>(drv),
               static_cast<void*>(hel), c.dwCaps, c.ddsCaps.dwCaps, (c.dwCaps & DDCAPS_3D) ? " (3D)" : "");
    // The emulation layer's (HEL) surface caps are a fixed set in ddraw.dll.
    DDCAPS helCaps = c;
    helCaps.ddsCaps.dwCaps = DDSCAPS_FLIP | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PALETTE | DDSCAPS_PRIMARYSURFACE |
                             DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP;
    helCaps.ddsOldCaps.dwCaps = helCaps.ddsCaps.dwCaps;
    helCaps.dwZBufferBitDepths = DDBD_16;
    // No overlays in the emulation layer.
    helCaps.dwCaps &= ~DWORD(DDCAPS_OVERLAY | DDCAPS_OVERLAYSTRETCH | DDCAPS_OVERLAYFOURCC);
    helCaps.dwCKeyCaps &= ~DWORD(DDCKEYCAPS_DESTOVERLAY | DDCKEYCAPS_SRCOVERLAY);
    helCaps.dwFXCaps &= ~kOverlayFxCaps;
    helCaps.dwMaxVisibleOverlays = helCaps.dwCurrVisibleOverlays = 0;
    helCaps.dwMinOverlayStretch = helCaps.dwMaxOverlayStretch = 0;
    helCaps.dwVidMemFree = 0;  // as ddraw.dll
    // An object limited to one side reports no surface caps for the other.
    if (capsSide == kHardwareOnly) helCaps.ddsCaps.dwCaps = helCaps.ddsOldCaps.dwCaps = 0;
    if (capsSide == kEmulationOnly) c.ddsCaps.dwCaps = c.ddsOldCaps.dwCaps = 0;
    for (DDCAPS* out : {drv, hel}) {
        if (!out) continue;
        const DDCAPS& src = out == hel ? helCaps : c;
        const DWORD size = out->dwSize;
        memcpy(out, &src, size);
        out->dwSize = size;
    }
    return DD_OK;
}

HRESULT DDraw::GetAvailableVidMem(const DDSCAPS2* caps, DWORD* total, DWORD* free) {
    // As ddraw.dll: the kinds of memory a game can ask about.
    if (!caps) return DDERR_INVALIDPARAMS;
    if (caps->dwCaps & kCapsReserved) return DDERR_INVALIDCAPS;
    const DWORD notMemory = DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_PALETTE |
                            DDSCAPS_SYSTEMMEMORY | DDSCAPS_VISIBLE | DDSCAPS_WRITEONLY | DDSCAPS_OWNDC;
    if ((caps->dwCaps & notMemory) || (!total && !free)) return DDERR_INVALIDPARAMS;
    Guard l;
    if (total) *total = DWORD(VidMemTotal());
    if (free) *free = VidMemFree();
    return DD_OK;
}

HRESULT DDraw::GetDeviceIdentifier(void* id, DWORD flags, bool v2) {
    if (!id) return DDERR_INVALIDPARAMS;
    if (flags & ~DWORD(DDGDI_GETHOSTIDENTIFIER)) {  // as ddraw.dll: the structure zeroed first
        memset(id, 0, v2 ? sizeof(DDDEVICEIDENTIFIER2) : sizeof(DDDEVICEIDENTIFIER));
        return DDERR_INVALIDPARAMS;
    }
    DDDEVICEIDENTIFIER2 d = {};
    if (!AdapterIdentifier(&d)) {  // no Direct3D 9: something that still looks like a driver
        strcpy(d.szDriver, "dd2d9.dll");
        strcpy(d.szDescription, "dd2d9: DirectDraw on Direct3D 9");
        d.liDriverVersion.QuadPart = 0x0006000E00000001LL;
    }
    memcpy(id, &d, v2 ? sizeof(DDDEVICEIDENTIFIER2) : sizeof(DDDEVICEIDENTIFIER));
    return DD_OK;
}

// Does surface description `have` match what `want` asks for (the fields it flags)?
// ddraw.dll's EnumSurfaces matching (FUN_737637e9): only these fields count, and the
// filter's flags needn't be among the surface's. g_lock held.
static bool DescMatches(const DDSURFACEDESC2& want, const Surface* s) {
    const DWORD F = want.dwFlags;
    const DDSCAPS2& c = s->desc.ddsCaps;
    if ((F & DDSD_CAPS) && (want.ddsCaps.dwCaps != c.dwCaps || want.ddsCaps.dwCaps2 != c.dwCaps2 ||
                            want.ddsCaps.dwCaps3 != c.dwCaps3 || want.ddsCaps.dwCaps4 != c.dwCaps4))
        return false;
    if ((F & DDSD_HEIGHT) && want.dwHeight != DWORD(s->H())) return false;
    if ((F & DDSD_WIDTH) && want.dwWidth != DWORD(s->W())) return false;
    if ((F & DDSD_LPSURFACE) && want.lpSurface != s->buf->bits) return false;
    // A key the surface hasn't set compares as 0.
    auto key = [&](int i, const DDCOLORKEY& k) {
        const DDCOLORKEY have = s->hasCk[i] ? s->ck[i] : DDCOLORKEY{};
        return k.dwColorSpaceLowValue == have.dwColorSpaceLowValue && k.dwColorSpaceHighValue == have.dwColorSpaceHighValue;
    };
    if ((F & DDSD_CKDESTBLT) && !key(kCkDestBlt, want.ddckCKDestBlt)) return false;
    if ((F & DDSD_CKSRCBLT) && !key(kCkSrcBlt, want.ddckCKSrcBlt)) return false;
    if (F & (DDSD_CKDESTOVERLAY | DDSD_CKSRCOVERLAY)) return false;  // no overlay data here
    const bool hasCount = (s->desc.dwFlags & DDSD_BACKBUFFERCOUNT) != 0;
    if ((F & DDSD_BACKBUFFERCOUNT) && want.dwBackBufferCount != (hasCount ? s->desc.dwBackBufferCount : 0)) return false;
    const bool mip = (c.dwCaps & DDSCAPS_MIPMAP) != 0;
    if ((F & DDSD_MIPMAPCOUNT) && want.dwMipMapCount != (mip ? s->desc.dwMipMapCount : 0)) return false;
    if (F & DDSD_PIXELFORMAT) {
        // Only a surface created with a pixel format of its own has one to compare.
        if (!s->ownFormat) return false;
        DDPIXELFORMAT pf;
        s->buf->fmt.ToDDPF(&pf);
        if (memcmp(&want.ddpfPixelFormat, &pf, sizeof(pf)) != 0) return false;
    }
    return true;
}

// Checks in ddraw.dll's order (spec: re/spec/ddraw-object.md, EnumSurfaces). The wrapper
// checked the filter's size for the interface version.
HRESULT DDraw::EnumSurfaces(DWORD flags, const void* filter, void* ctx, void* callback, int version, int ddVersion) {
    if (!callback || IsBadCodePtr(reinterpret_cast<FARPROC>(callback)) || (flags & ~0x1Fu)) return DDERR_INVALIDPARAMS;
    using CB = HRESULT(WINAPI*)(void*, void*, void*);
    DDSURFACEDESC2 want = {};
    if (filter && !DescIn(filter, &want)) return DDERR_INVALIDPARAMS;
    const bool all = flags & DDENUMSURFACES_ALL, match = flags & DDENUMSURFACES_MATCH;
    const bool nomatch = flags & DDENUMSURFACES_NOMATCH, create = flags & DDENUMSURFACES_CANBECREATED;
    if ((all && (match || nomatch)) || (!all && !filter) || (match && nomatch)) return DDERR_INVALIDPARAMS;
    if (create && (!match || all || nomatch)) return DDERR_INVALIDPARAMS;
    if (filter) {
        const DWORD keys = DDSD_CKDESTBLT | DDSD_CKSRCBLT | DDSD_CKDESTOVERLAY | DDSD_CKSRCOVERLAY;
        if ((want.dwFlags & DDSD_LPSURFACE) || (want.ddsCaps.dwCaps & kCapsReserved)) return DDERR_INVALIDPARAMS;
        if (want.ddsCaps.dwCaps & DDSCAPS_OPTIMIZED) return DD_OK;  // there are none
        if ((want.ddsCaps.dwCaps2 & 1) || (want.ddsCaps.dwCaps3 & 0xFFFFE000) || want.ddsCaps.dwCaps4)
            return DDERR_INVALIDCAPS;
        if (want.ddsCaps.dwCaps & kCapsExecuteBuffer) return DDERR_INVALIDPARAMS;
        if (!(want.dwFlags & DDSD_HEIGHT) != !(want.dwFlags & DDSD_WIDTH)) return DDERR_INVALIDPARAMS;
        if (create && (want.dwFlags & keys)) return DDERR_INVALIDPARAMS;
    }
    if (!create && !(flags & DDENUMSURFACES_DOESEXIST)) return DDERR_INVALIDPARAMS;
    auto call = [&](Surface* s, const DDSURFACEDESC2& d) {
        union {
            DDSURFACEDESC v1;
            DDSURFACEDESC2 v2;
        } out;
        if (version >= 4) {
            out.v2.dwSize = sizeof(DDSURFACEDESC2);
            DescOut(d, &out);
        } else {
            DescOutEnum(d, &out.v1);  // as ddraw.dll: the new structure cut short
        }
        return reinterpret_cast<CB>(callback)(s ? s->Iface(version) : nullptr, &out, ctx);
    };
    if (flags & DDENUMSURFACES_CANBECREATED) {
        // The descriptions surfaces could be created with (no surface handed out): the one
        // asked for, or without a size, one for each display mode. As in ddraw.dll, the
        // callback's answer doesn't stop it.
        auto tryCreate = [&](const DDSURFACEDESC2& d) {
            if (d.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) {
                // Not really created (that would start presenting): the display's description.
                DDSURFACEDESC2 got = {};
                got.dwSize = sizeof(got);
                got.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
                DWORD w, h, bpp;
                {
                    Guard l;
                    DisplayMode(&w, &h, &bpp);
                }
                const Format f = Format::ForDisplayBpp(bpp);
                got.dwWidth = w;
                got.dwHeight = h;
                got.lPitch = LONG(((w * f.Bytes()) + 7) & ~7u);
                got.ddsCaps.dwCaps = d.ddsCaps.dwCaps | DDSCAPS_VISIBLE | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
                f.ToDDPF(&got.ddpfPixelFormat);
                call(nullptr, got);
                return;
            }
            union {
                DDSURFACEDESC v1;
                DDSURFACEDESC2 v2;
            } in;
            in.v1.dwSize = version >= 4 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
            DescOut(d, &in);
            void* iface = nullptr;
            DDSURFACEDESC2 got;
            // With the rules of the interface called (IDirectDraw2's, not IDirectDraw's).
            if (FAILED(CreateSurfaceImpl(&in, &iface, nullptr, ddVersion, &got))) return;
            call(nullptr, got);
        };
        if (want.dwFlags & (DDSD_WIDTH | DDSD_HEIGHT)) {
            tryCreate(want);
        } else {  // in the display's format, unless the game asks for another
            for (const SIZE& sz : ModeSizes()) {
                DDSURFACEDESC2 d = want;
                d.dwFlags |= DDSD_WIDTH | DDSD_HEIGHT;
                d.dwWidth = DWORD(sz.cx);
                d.dwHeight = DWORD(sz.cy);
                tryCreate(d);
            }
        }
        return DD_OK;
    }
    // The surfaces that exist, newest first.
    std::vector<Surface*> list;
    std::vector<DDSURFACEDESC2> descs;
    {
        Guard l;
        for (size_t k = surfaces.size(); k-- > 0;) {
            Surface* s = surfaces[k];
            if (!all && nomatch == DescMatches(want, s)) continue;
            DDSURFACEDESC2 d;
            s->GetSurfaceDesc(&d);
            s->AddRef(Surface::SlotFor(version));  // the callback releases it
            list.push_back(s);
            descs.push_back(d);
        }
    }
    size_t i = 0;
    for (; i < list.size(); i++) {
        if (call(list[i], descs[i]) == DDENUMRET_CANCEL) {
            i++;
            break;
        }
    }
    for (; i < list.size(); i++) list[i]->Release(Surface::SlotFor(version));  // never handed to the callback
    return DD_OK;
}

}  // namespace dd2d9
