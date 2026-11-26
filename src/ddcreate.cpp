// CreateSurface (ddraw.dll's checks of the description and caps) and DuplicateSurface.
#include "ddint.h"

#include <algorithm>

namespace dd2d9 {

HRESULT DDraw::DuplicateSurface(void* src, void** out, int ddVersion) {
    Surface* s = Surface::From(src);
    if (!s || !out) return DDERR_INVALIDPARAMS;
    Guard l;
    if (s->IsLost()) return DDERR_SURFACELOST;
    *out = nullptr;  // (after the lost check: spec step 2)
    // A flip chain's front can be duplicated (alone); its members and the primary can't.
    if (s->isPrimary || s->root) return DDERR_CANTDUPLICATE;
    // As ddraw.dll: the duplicate is handed out through the source's interface version,
    // and holds no reference on the DirectDraw object.
    int version = 1;
    for (int v : {2, 3, 4, 7})
        if (src == s->Iface(v)) version = v;
    Surface* n;
    try {
        n = new Surface(this, s->desc, s->buf, version);
    } catch (const std::bad_alloc&) {
        return DDERR_OUTOFMEMORY;
    }
    s->buf->AddRef();
    n->sysmemRequested = s->sysmemRequested;
    n->ownFormat = s->ownFormat;
    n->uniqueness = s->uniqueness;  // ddraw.dll keeps one for the memory both share
    n->modeW = s->modeW;
    n->modeH = s->modeH;
    n->modeBpp = s->modeBpp;
    vidMemUsed -= n->vidMemBytes;  // the memory is the source's: counted once
    n->vidMemBytes = 0;
    n->AddRef(Surface::SlotFor(version));
    n->ddVersion = ddVersion;
    *out = n->Iface(version);
    return DD_OK;
}

static void FinishCaps(DDSURFACEDESC2* d) {
    // Managed textures live in system memory as far as the game can tell.
    if (d->ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE))
        d->ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
    // Video memory is local unless the game asked for non-local (AGP) memory.
    if (!(d->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY))
        d->ddsCaps.dwCaps |= DDSCAPS_VIDEOMEMORY | ((d->ddsCaps.dwCaps & DDSCAPS_NONLOCALVIDMEM) ? 0 : DDSCAPS_LOCALVIDMEM);
    if (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) d->ddsCaps.dwCaps |= DDSCAPS_VISIBLE;
    d->dwFlags |= DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
}

HRESULT DDraw::CreateSurface(const void* inDesc, void** out, IUnknown* outer, int ddVersion) {
    HRESULT hr = CreateSurfaceImpl(inDesc, out, outer, ddVersion);
    // A windowed primary refused for another object's exclusive mode, that being dd2d9's own
    // gdifullscreen mode (a game that switched the display with ChangeDisplaySettings before
    // making its DirectDraw object): the mode makes way, and the primary is made. (Out here:
    // making way takes the locks CreateSurfaceImpl holds.)
    if (hr == DDERR_NOEXCLUSIVEMODE && !Exclusive() && GdiFullscreenYield(this))
        hr = CreateSurfaceImpl(inDesc, out, outer, ddVersion);
    if (FAILED(hr)) DD2D9_LOG("CreateSurface failed: 0x%08lx", hr);
    return hr;
}

static bool PowerOfTwo(DWORD v) { return v && !(v & (v - 1)); }

static DWORD Log2(DWORD v) {
    DWORD n = 0;
    while (v >>= 1) n++;
    return n;
}

// ddraw.dll's caps rules (FUN_73736a8e; spec: re/spec/ddraw-object.md, CreateSurface).
// C has its memory type filled in already (video memory unless system memory was asked for).
static HRESULT CheckCaps(DWORD C, DWORD C2, DWORD C3, DWORD C4, int ddVersion) {
    const DWORD managed = DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE;
    const bool complex = C & DDSCAPS_COMPLEX, flipping = C & DDSCAPS_FLIP, cube = C2 & DDSCAPS2_CUBEMAP;
    const bool z = C & DDSCAPS_ZBUFFER;
    auto all = [](DWORD v, DWORD bits) { return (v & bits) == bits; };
    if (!C || (C & kCapsReserved) || (C2 & 1) || (C3 & 0xFFFFE000)) return DDERR_INVALIDCAPS;  // bit 0: reserved
    if ((!(C2 & DDSCAPS2_VOLUME) && C4) || (C & (DDSCAPS_PALETTE | DDSCAPS_VISIBLE)) ||
        ((C & DDSCAPS_WRITEONLY) && !(C & kCapsExecuteBuffer) && !(C2 & managed)) ||
        ((C2 & DDSCAPS2_DONOTPERSIST) && !(C2 & managed)) || ((C2 & managed) && !(C & DDSCAPS_TEXTURE)) ||
        all(C2, managed) ||
        ((C2 & (DDSCAPS2_HINTDYNAMIC | DDSCAPS2_HINTSTATIC | DDSCAPS2_OPAQUE)) && !(C & DDSCAPS_TEXTURE)) ||
        all(C2, DDSCAPS2_HINTDYNAMIC | DDSCAPS2_HINTSTATIC) || all(C2, DDSCAPS2_HINTDYNAMIC | DDSCAPS2_OPAQUE) ||
        all(C2, DDSCAPS2_HINTSTATIC | DDSCAPS2_OPAQUE) || (C & DDSCAPS_OPTIMIZED) ||
        ((C & (DDSCAPS_LOCALVIDMEM | DDSCAPS_NONLOCALVIDMEM)) &&
         ((C & DDSCAPS_SYSTEMMEMORY) || all(C, DDSCAPS_LOCALVIDMEM | DDSCAPS_NONLOCALVIDMEM))) ||
        (ddVersion > 1 && (C & (DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER))) || ((C & kCapsExecuteBuffer) && (C & 0xFF7EB7FF)))
        return DDERR_INVALIDCAPS;
    if (complex &&
        ((C & DDSCAPS_FRONTBUFFER) ||
         ((C & DDSCAPS_BACKBUFFER) && (!(C & (DDSCAPS_ALPHA | DDSCAPS_ZBUFFER)) || flipping)) ||
         !(C & (DDSCAPS_TEXTURE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_OVERLAY | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_BACKBUFFER)) ||
         (!(C & (DDSCAPS_MIPMAP | DDSCAPS_ZBUFFER | DDSCAPS_FLIP | DDSCAPS_ALPHA)) && !cube)))
        return DDERR_INVALIDCAPS;
    if (((C & DDSCAPS_BACKBUFFER) && (C & (DDSCAPS_FRONTBUFFER | DDSCAPS_ALPHA))) || (flipping && !complex) ||
        ((C & DDSCAPS_PRIMARYSURFACE) &&
         ((C & (DDSCAPS_TEXTURE | DDSCAPS_OVERLAY | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_BACKBUFFER)) || (z && !flipping))) ||
        ((C & DDSCAPS_OFFSCREENPLAIN) && (C & (DDSCAPS_TEXTURE | DDSCAPS_OVERLAY | DDSCAPS_BACKBUFFER))) ||
        ((C & DDSCAPS_OVERLAY) &&
         ((C & (DDSCAPS_TEXTURE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_BACKBUFFER)) || (z && !flipping))))
        return DDERR_INVALIDCAPS;
    if (C & DDSCAPS_MIPMAP) {
        if (!(C & DDSCAPS_TEXTURE) || z) return DDERR_INVALIDCAPS;
    } else if (z && (C & DDSCAPS_BACKBUFFER) && !complex) {
        return DDERR_INVALIDCAPS;
    }
    if ((C & DDSCAPS_ALLOCONLOAD) && !(C & DDSCAPS_TEXTURE)) return DDERR_INVALIDCAPS;
    if ((C2 & DDSCAPS2_CUBEMAP_ALLFACES) && !cube) return DDERR_INVALIDCAPS;
    if (cube) {
        if ((C & 0xC3BA87F7) || (C2 & 0xF7A90060) || (C3 & 0xFFFFE5FF) || C4 ||
            !(C & (DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE)))
            return DDERR_INVALIDPARAMS;
        if (!complex) return DDERR_DDSCAPSCOMPLEXREQUIRED;
        if (!(C2 & DDSCAPS2_CUBEMAP_ALLFACES)) return DDERR_INVALIDPARAMS;
    }
    if (C2 & DDSCAPS2_STEREOSURFACELEFT) return DDERR_UNSUPPORTED;  // no stereo here
    return DD_OK;
}

// CreateSurface's checks of the description, in ddraw.dll's order (FUN_73737051,
// FUN_73737235 and its validator FUN_73736cad). `in` loses what the interface version
// doesn't take (the pitch through IDirectDraw/2, or without client memory).
// overlays: OverlaysOffered for the object.
static HRESULT CheckSurfaceDesc(DDSURFACEDESC2& in, int ddVersion, DWORD displayBpp, bool overlays, bool emulationOnly) {
    DWORD& F = in.dwFlags;
    const DWORD C2 = in.ddsCaps.dwCaps2, C3 = in.ddsCaps.dwCaps3, C4 = in.ddsCaps.dwCaps4;
    DWORD C = in.ddsCaps.dwCaps;
    const bool v1 = ddVersion < 4;  // IDirectDraw and IDirectDraw2: the DDSURFACEDESC
    if ((C & DDSCAPS_SYSTEMMEMORY) && (C & DDSCAPS_VIDEOMEMORY)) return DDERR_INVALIDCAPS;
    if (v1 || !(F & DDSD_LPSURFACE)) F &= ~(DDSD_PITCH | DDSD_LINEARSIZE);
    if (F & DDSD_LPSURFACE) {
        // Client memory (IDirectDrawSurface4+ SetSurfaceDesc's sibling).
        if (!(C & DDSCAPS_SYSTEMMEMORY)) return DDERR_INVALIDCAPS;
        if (!in.lpSurface || IsBadReadPtr(in.lpSurface, 4)) return DDERR_INVALIDPARAMS;
        if (C & (DDSCAPS_OPTIMIZED | DDSCAPS_COMPLEX | kCapsExecuteBuffer | DDSCAPS_OVERLAY | DDSCAPS_PRIMARYSURFACE |
                 DDSCAPS_PRIMARYSURFACELEFT | DDSCAPS_VIDEOPORT))
            return DDERR_INVALIDCAPS;
        if (!(F & DDSD_WIDTH) || !(F & DDSD_HEIGHT)) return DDERR_INVALIDPARAMS;
        if ((reinterpret_cast<uintptr_t>(in.lpSurface) & 3) && ddVersion != 4) return DDERR_INVALIDPARAMS;
        if ((F & DDSD_PIXELFORMAT) && (in.ddpfPixelFormat.dwFlags & DDPF_FOURCC)) {
            // Block-compressed (DXT): by its linear size, which must hold every block (a
            // pitch isn't taken, with or without it; Wine's test_block_formats_creation).
            Format f;
            if (!Format::FromDDPF(in.ddpfPixelFormat, &f) || !f.BlockCompressed()) {
                DD2D9_LOG("CreateSurface: FourCC surfaces in client memory aren't supported but DXT");
                return DDERR_UNSUPPORTED;
            }
            // IDirectDraw4 takes a pitch flag as enough, whatever the value (and with it, the
            // linear size unchecked); IDirectDraw7 refuses it (the same test, both ways).
            const bool v4pitch = ddVersion == 4 && (F & DDSD_PITCH);
            if (!v4pitch) {
                if (!(F & DDSD_LINEARSIZE) || (F & DDSD_PITCH)) return DDERR_INVALIDPARAMS;
                const uint64_t blocks = ((uint64_t(in.dwWidth) + 3) / 4) * ((uint64_t(in.dwHeight) + 3) / 4);
                if (in.dwLinearSize < blocks * (f.fourCC == MAKEFOURCC('D', 'X', 'T', '1') ? 8 : 16))
                    return DDERR_INVALIDPARAMS;
            }
        } else {
            const DWORD bpp = (F & DDSD_PIXELFORMAT) ? in.ddpfPixelFormat.dwRGBBitCount : displayBpp;
            // (In 64 bits: a huge width would wrap to a small pitch and pass.)
            if (!(F & DDSD_PITCH) || in.lPitch < 0 || uint64_t(in.lPitch) < (uint64_t(bpp) * in.dwWidth + 7) / 8 ||
                (in.lPitch & 3))
                return DDERR_INVALIDPARAMS;
        }
    }
    const DWORD managed = DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE;
    if (C2 & 1) return DDERR_INVALIDCAPS;  // reserved
    if ((C & (DDSCAPS_LOCALVIDMEM | DDSCAPS_NONLOCALVIDMEM)) && !(C & DDSCAPS_VIDEOMEMORY)) return DDERR_INVALIDCAPS;
    if ((C & DDSCAPS_OWNDC) && !(C & DDSCAPS_SYSTEMMEMORY)) return DDERR_UNSUPPORTED;
    if ((C2 & managed) &&
        (C & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_NONLOCALVIDMEM)))
        return DDERR_INVALIDCAPS;
    if (!(C & DDSCAPS_SYSTEMMEMORY) && !(C2 & managed)) C |= DDSCAPS_VIDEOMEMORY;  // ddraw.dll tries it first

    const bool complex = C & DDSCAPS_COMPLEX, primary = C & DDSCAPS_PRIMARYSURFACE, z = C & DDSCAPS_ZBUFFER;
    // A complex mipmap without a level count: as many levels as the size halves.
    if (complex && (C & DDSCAPS_MIPMAP) && !(F & DDSD_MIPMAPCOUNT) &&
        (((F & DDSD_WIDTH) && !PowerOfTwo(in.dwWidth)) || ((F & DDSD_HEIGHT) && !PowerOfTwo(in.dwHeight))))
        return DDERR_INVALIDPARAMS;
    if (C2 & DDSCAPS2_VOLUME) return DDERR_INVALIDCAPS;

    // The validator.
    if (!complex) {
        if (F & (DDSD_BACKBUFFERCOUNT | DDSD_MIPMAPCOUNT)) return DDERR_INVALIDCAPS;  // even a count of 1
    } else if ((F & DDSD_BACKBUFFERCOUNT) && (F & DDSD_MIPMAPCOUNT)) {
        return DDERR_INVALIDPARAMS;
    }
    if ((C & DDSCAPS_FLIP) && (!complex || !(F & DDSD_BACKBUFFERCOUNT) || !in.dwBackBufferCount))
        return DDERR_INVALIDCAPS;
    if (C2 & 2) return DDERR_INVALIDCAPS;  // DX7's DDSCAPS2_RESERVED4 (kernel-mode video ports only)
    if (HRESULT hr = CheckCaps(C, C2, C3, C4, ddVersion); FAILED(hr)) return hr;
    if ((C & DDSCAPS_ALPHA) || (F & DDSD_ALPHABITDEPTH)) return DDERR_INVALIDPARAMS;
    // Z buffers: through the old structure by depth or format, through the new one by format.
    DDPIXELFORMAT& pf = in.ddpfPixelFormat;
    if (!z) {
        if ((F & DDSD_ZBUFFERBITDEPTH) ||
            ((F & DDSD_PIXELFORMAT) && (pf.dwFlags & (DDPF_ZBUFFER | DDPF_STENCILBUFFER))))
            return DDERR_INVALIDPARAMS;
    } else {
        DWORD depth;
        if (v1) {
            if (!(F & DDSD_PIXELFORMAT)) {
                if (!(F & DDSD_ZBUFFERBITDEPTH)) return DDERR_INVALIDPARAMS;
                depth = in.dwMipMapCount;  // dwZBufferBitDepth
            } else {
                if (pf.dwSize != sizeof(DDPIXELFORMAT) || (pf.dwFlags & DDPF_STENCILBUFFER) ||
                    !(pf.dwFlags & DDPF_ZBUFFER))
                    return DDERR_INVALIDPARAMS;
                depth = pf.dwZBufferBitDepth;
            }
        } else {
            if (complex || (F & DDSD_ZBUFFERBITDEPTH) || !(F & DDSD_PIXELFORMAT) || pf.dwSize != sizeof(DDPIXELFORMAT) ||
                !(pf.dwFlags & DDPF_ZBUFFER) || !pf.dwZBitMask)
                return DDERR_INVALIDPARAMS;
            // With stencil: its depth and mask given, and then any layout (which memory takes
            // it is CreateSurface's business): depth and stencil sharing bits too, or the stencil
            // outside the word, as ddraw.dll takes them (measured on XP).
            if ((pf.dwFlags & DDPF_STENCILBUFFER) && (!pf.dwStencilBitDepth || !pf.dwStencilBitMask))
                return DDERR_INVALIDPARAMS;
            depth = pf.dwZBufferBitDepth;
        }
        if (depth < 8) return DDERR_INVALIDPARAMS;
    }
    // Size: none for the primary (it is the screen's), both for anything else.
    if (primary) {
        if (F & (DDSD_WIDTH | DDSD_HEIGHT)) return DDERR_INVALIDPARAMS;
    } else if (!(F & DDSD_WIDTH) || !(F & DDSD_HEIGHT) || !in.dwWidth || !in.dwHeight) {
        return DDERR_INVALIDPARAMS;
    }
    if (C & DDSCAPS_MIPMAP) {
        if ((F & DDSD_MIPMAPCOUNT) && !in.dwMipMapCount) return DDERR_INVALIDPARAMS;
        if (!PowerOfTwo(in.dwWidth) || !PowerOfTwo(in.dwHeight)) return DDERR_INVALIDPARAMS;
        if ((F & DDSD_MIPMAPCOUNT) && Log2(std::max(in.dwWidth, in.dwHeight)) + 1 < in.dwMipMapCount)
            return DDERR_INVALIDPARAMS;
    }
    if (F & DDSD_PIXELFORMAT) {
        if (primary) return DDERR_INVALIDPARAMS;
        // DDPF_ALPHA goes with none of these (ddraw.dll's check, FUN_73736cad: mask 0x27e6d;
        // XP SP3 returns this for each one tried): ALPHAPIXELS, FOURCC, the palettized flags,
        // RGB, YUV, ZBUFFER, ZPIXELS, STENCILBUFFER, LUMINANCE.
        if ((pf.dwFlags & DDPF_ALPHA) && (pf.dwFlags & 0x27E6D)) return DDERR_INVALIDPARAMS;
        Format f;
        if (!z && !Format::FromDDPF(pf, &f)) {
            DD2D9_LOG("CreateSurface: unsupported pixel format (flags 0x%lx, %lu bpp)", pf.dwFlags, pf.dwRGBBitCount);
            return DDERR_INVALIDPIXELFORMAT;
        }
        // Formats that aren't colours (luminance, alpha-only, bump maps) are textures' alone:
        // ddraw.dll makes no other surface of them (XP SP3 and Windows 98: every luminance
        // and alpha-only layout refused as a plain surface, a bump map as a plain render target).
        // 8-bit RGB (3-3-2) as well (XP SP3 at 8-, 16- and 32-bit displays).
        const bool rgb8 = f.bpp == 8 && !f.Palettized() && !f.CopyOnly() && !f.fourcc && !f.zbuf;
        if ((f.CopyOnly() || rgb8) && !(C & DDSCAPS_TEXTURE)) return DDERR_INVALIDPIXELFORMAT;
        if (f.BlockCompressed() && ((in.dwWidth | in.dwHeight) & 3)) return DDERR_INVALIDPARAMS;
        // Planar video (YV12, I420): offscreen and overlay surfaces only (no texture format lists it).
        if (f.Planar() && (C & (DDSCAPS_3DDEVICE | DDSCAPS_ZBUFFER))) return DDERR_INVALIDPIXELFORMAT;
        // Video formats where XP's ddraw.dll takes them (fmtprobe section 33, the Voodoo3's
        // driver): an offscreen surface or overlay of one in video memory (the emulation layer
        // makes none: in system memory DDERR_INVALIDPIXELFORMAT, as Wine's
        // test_block_formats_creation has it on other Windows too); a texture of one not there
        // (the device takes no textures of them: DDERR_UNSUPPORTEDFORMAT), in system memory YUY2
        // and UYVY only (where no memory is named, there: see CreateSurfaceImpl). I420 and IYUV,
        // which GetFourCCCodes doesn't list, not at all.
        if (f.Yuv() || f.Planar()) {
            const bool i420 = f.fourCC == MAKEFOURCC('I', '4', '2', '0') || f.fourCC == MAKEFOURCC('I', 'Y', 'U', 'V');
            if (C & DDSCAPS_TEXTURE) {
                if (C & DDSCAPS_VIDEOMEMORY) return DDERR_UNSUPPORTEDFORMAT;
                if (f.Planar()) return DDERR_INVALIDPIXELFORMAT;
            } else if (i420 || (C & DDSCAPS_SYSTEMMEMORY)) {
                return DDERR_INVALIDPIXELFORMAT;
            }
        }
    }
    // Color keys, as SetColorKey takes them: overlay keys only while overlays are offered, ranges
    // only in video memory, where the driver compares them (the surface keeps the low value
    // alone, as measured on XP: the emulation layer has no ranges).
    static const DWORD kCkFlags[4] = {DDSD_CKDESTOVERLAY, DDSD_CKDESTBLT, DDSD_CKSRCOVERLAY, DDSD_CKSRCBLT};
    DDCOLORKEY* keys[4] = {&in.ddckCKDestOverlay, &in.ddckCKDestBlt, &in.ddckCKSrcOverlay, &in.ddckCKSrcBlt};
    for (int i = 0; i < 4; i++) {
        if (!(F & kCkFlags[i])) continue;
        if ((kCkFlags[i] & (DDSD_CKDESTOVERLAY | DDSD_CKSRCOVERLAY)) && !overlays) return DDERR_NOCOLORKEYHW;
        if (keys[i]->dwColorSpaceLowValue != keys[i]->dwColorSpaceHighValue) {
            if (!(C & DDSCAPS_VIDEOMEMORY) || emulationOnly) return DDERR_NOCOLORKEYHW;
            keys[i]->dwColorSpaceHighValue = keys[i]->dwColorSpaceLowValue;
        }
        // A destination overlay key where SetColorKey takes one: overlays and the primary.
        if (kCkFlags[i] == DDSD_CKDESTOVERLAY && !(C & (DDSCAPS_OVERLAY | DDSCAPS_PRIMARYSURFACE)))
            return DDERR_NOTAOVERLAYSURFACE;
        if (z) return DDERR_INVALIDSURFACETYPE;  // SetColorKey's answer for a Z buffer
    }
    // Cube maps: through IDirectDraw7, square, a power of two, no back buffers.
    if ((C2 & DDSCAPS2_CUBEMAP) &&
        (ddVersion != 7 || in.dwWidth != in.dwHeight || !PowerOfTwo(in.dwWidth) || (F & DDSD_BACKBUFFERCOUNT)))
        return DDERR_INVALIDPARAMS;
    // Overlays: offered in video memory (overlay.cpp): RGB, palettized, YUY2, UYVY, YV12, I420.
    if (C & DDSCAPS_OVERLAY) {
        if (!overlays || (C & DDSCAPS_SYSTEMMEMORY)) return DDERR_NOOVERLAYHW;
        if (F & DDSD_PIXELFORMAT) {
            Format f;
            if (Format::FromDDPF(pf, &f) && (f.BlockCompressed() || f.palLow || f.CopyOnly())) return DDERR_INVALIDPIXELFORMAT;
        }
    }
    // What this implementation doesn't have.
    if (C & DDSCAPS_VIDEOPORT) return DDERR_INVALIDPARAMS;
    return DD_OK;
}

HRESULT DDraw::CreateSurfaceImpl(const void* inDesc, void** out, IUnknown* outer, int ddVersion, DDSURFACEDESC2* describe) {
    const int version = ddVersion == 2 ? 1 : ddVersion;  // IDirectDraw2 hands out IDirectDrawSurface
    // In ddraw.dll's order: through IDirectDraw4/7 the aggregation check comes first,
    // through IDirectDraw/2 after the cooperative level and the description's size.
    if (outer && ddVersion >= 4) return CLASS_E_NOAGGREGATION;
    Guard l;
    if (!(coop_ & (DDSCL_NORMAL | DDSCL_EXCLUSIVE))) return DDERR_NOCOOPERATIVELEVELSET;
    DDSURFACEDESC2 in;
    if (!inDesc || IsBadReadPtr(inDesc, sizeof(DWORD))) return DDERR_INVALIDPARAMS;
    // Each interface takes exactly its own description.
    if (static_cast<const DDSURFACEDESC*>(inDesc)->dwSize != (ddVersion < 4 ? sizeof(DDSURFACEDESC) : sizeof(DDSURFACEDESC2)))
        return DDERR_INVALIDPARAMS;
    if (!DescIn(inDesc, &in)) return DDERR_INVALIDPARAMS;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!out || IsBadWritePtr(out, sizeof(*out))) return DDERR_INVALIDPARAMS;  // XP faults inside its handler
    *out = nullptr;
    UpdateLostSurfaces();  // the surfaces lost so far, not this new one
    DD2D9_LOG("CreateSurface(flags 0x%lx, caps 0x%lx, %lux%lu, backbuffers %lu)", in.dwFlags, in.ddsCaps.dwCaps,
             (in.dwFlags & DDSD_WIDTH) ? in.dwWidth : 0, (in.dwFlags & DDSD_HEIGHT) ? in.dwHeight : 0,
             (in.dwFlags & DDSD_BACKBUFFERCOUNT) ? in.dwBackBufferCount : 0);
    DWORD displayW, displayH, displayBpp;
    DisplayMode(&displayW, &displayH, &displayBpp);
    if (HRESULT hr = CheckSurfaceDesc(in, ddVersion, displayBpp, OverlaysOffered(this), capsSide == kEmulationOnly); FAILED(hr))
        return hr;
    // ddraw.dll reads the caps even without DDSD_CAPS. None at all makes an offscreen
    // surface, whose caps still say nothing but where its memory is.
    const DWORD asked = in.ddsCaps.dwCaps;
    const DWORD caps2 = in.ddsCaps.dwCaps2;
    // DDCREATE_EMULATIONONLY: the emulation layer's surfaces, all in system memory; video
    // memory there is none of (measured on XP).
    if (capsSide == kEmulationOnly && (asked & DDSCAPS_VIDEOMEMORY)) return DDERR_NODIRECTDRAWHW;
    const bool emulated = capsSide == kEmulationOnly && !(caps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE));
    DWORD caps = emulated ? asked | DDSCAPS_SYSTEMMEMORY : asked;
    if ((caps & DDSCAPS_OVERLAY) && (caps & DDSCAPS_SYSTEMMEMORY)) return DDERR_NOOVERLAYHW;
    const bool flip = (caps & DDSCAPS_FLIP) != 0;
    const bool complex = (caps & DDSCAPS_COMPLEX) != 0;
    const bool cube = (caps2 & DDSCAPS2_CUBEMAP) != 0;

    int w, h;
    Format fmt;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        // As ddraw.dll: a flip chain needs exclusive mode, and while another object has
        // it, this one can't have a primary at all.
        if (flip && !Exclusive()) return DDERR_NOEXCLUSIVEMODE;
        if (!Exclusive() && OtherExclusive()) return DDERR_NOEXCLUSIVEMODE;
        if (primary) {  // lost or not, as ddraw.dll
            DD2D9_LOG("CreateSurface: the primary surface %p still exists", static_cast<void*>(primary));
            return DDERR_PRIMARYSURFACEALREADYEXISTS;
        }
        w = int(displayW);
        h = int(displayH);
        fmt = Format::ForDisplayBpp(displayBpp);
    } else {
        // CheckSurfaceDesc has seen the size, and the format or Z depth. ddraw.dll keeps
        // a surface's size in 16 bits and refuses what doesn't fit.
        if (in.dwWidth > 0xFFFF || in.dwHeight > 0xFFFF) return DDERR_INVALIDPARAMS;
        w = int(in.dwWidth);
        h = int(in.dwHeight);
        if (in.dwFlags & DDSD_PIXELFORMAT) {
            if (!Format::FromDDPF(in.ddpfPixelFormat, &fmt)) {
                DD2D9_LOG("CreateSurface: unsupported pixel format (flags 0x%lx, %lu bpp)",
                         in.ddpfPixelFormat.dwFlags, in.ddpfPixelFormat.dwRGBBitCount);
                return DDERR_INVALIDPIXELFORMAT;
            }
        } else if (caps & DDSCAPS_ZBUFFER) {
            const DWORD depth = in.dwMipMapCount;  // dwZBufferBitDepth (IDirectDraw/2)
            fmt.zbuf = true;
            fmt.bpp = depth;
            if (depth != 16 && depth != 24 && depth != 32) return DDERR_INVALIDPIXELFORMAT;
        } else {
            fmt = Format::ForDisplayBpp(displayBpp);
        }
    }

    // A Z buffer with stencil: in video memory any 16-bit layout, as the Voodoo3's driver takes
    // them on XP (15+1, depth and stencil sharing bits, the stencil outside the word; VMware's
    // refused 15+1), and in 32 bits 24 with 8 of stencil apart (as Direct3D enumerates it:
    // EnumZBufferFormats). Any other is the emulation layer's: in system memory, there too when
    // no memory is named (ddraw.dll tries the driver first), and refused in video memory
    // (measured on XP: 16+16, 24+9 sharing a bit, 8 of stencil inside 24 or 32 of depth).
    if (fmt.zbuf && fmt.a && fmt.bpp != 16 &&
        !(fmt.bpp == 32 && __builtin_popcount(fmt.a) == 8 && __builtin_popcount(fmt.r) == 24 && !(fmt.r & fmt.a))) {
        if (asked & DDSCAPS_VIDEOMEMORY) return DDERR_INVALIDPIXELFORMAT;
        caps |= DDSCAPS_SYSTEMMEMORY;
    }
    // A texture of YUY2 or UYVY where no memory is named: the emulation layer's, in system
    // memory, as ddraw.dll goes there once the driver has none (see CheckSurfaceDesc).
    if (fmt.Yuv() && (caps & DDSCAPS_TEXTURE) && !(caps & DDSCAPS_VIDEOMEMORY)) caps |= DDSCAPS_SYSTEMMEMORY;
    // The emulation layer's Z buffers, which those in system memory are: 16 and 32 bits only
    // (Wine's test_caps; measured on XP).
    if ((caps & DDSCAPS_SYSTEMMEMORY) && fmt.zbuf && fmt.bpp != 16 && fmt.bpp != 32) return DDERR_INVALIDPIXELFORMAT;
    DDSURFACEDESC2 base = in;
    base.dwFlags &= ~(DDSD_LPSURFACE | DDSD_PITCH | DDSD_ZBUFFERBITDEPTH | DDSD_LINEARSIZE);
    // Fields the game didn't flag hold whatever its structure did: not taken over.
    if (!(in.dwFlags & DDSD_BACKBUFFERCOUNT)) base.dwBackBufferCount = 0;
    if (!(in.dwFlags & (DDSD_MIPMAPCOUNT | DDSD_REFRESHRATE))) base.dwMipMapCount = 0;
    if (!(in.dwFlags & DDSD_ALPHABITDEPTH)) base.dwAlphaBitDepth = 0;
    base.lpSurface = nullptr;
    base.dwWidth = DWORD(w);
    base.dwHeight = DWORD(h);
    base.ddsCaps.dwCaps = caps;
    fmt.ToDDPF(&base.ddpfPixelFormat);

    // Mip levels: as many as asked for (CheckSurfaceDesc saw they fit), or (without
    // DDSD_MIPMAPCOUNT) down to a side of 1: the longer side through IDirectDraw7, the
    // shorter one before it.
    DWORD levels = 1;
    if (caps & DDSCAPS_MIPMAP) {
        const int longest = std::max(w, h), shortest = std::min(w, h);
        if (complex) {
            if (in.dwFlags & DDSD_MIPMAPCOUNT) {
                levels = in.dwMipMapCount;
            } else {
                int n = 1;
                while (((ddVersion == 7 ? longest : shortest) >> n) > 0) n++;
                levels = DWORD(n);
            }
        }
        base.dwFlags |= DDSD_MIPMAPCOUNT;
        base.dwMipMapCount = levels;
    }
    // Main surface memory: client memory when DDSD_LPSURFACE is given (CheckSurfaceDesc
    // saw it is system memory with a pitch that holds a row).
    const bool client = (in.dwFlags & DDSD_LPSURFACE) != 0;
    Buffer* b;
    if (client) {
        // DXT: the pitch of dd2d9's own layout (Buffer::Create), the game's memory holding
        // every block (CheckSurfaceDesc).
        const LONG pitch = fmt.BlockCompressed() ? Buffer::BlockPitch(w, fmt) : in.lPitch;
        b = Buffer::CreateExternal(in.lpSurface, w, h, pitch, fmt);
    } else if (describe) {
        b = Buffer::CreateLayout(w, h, fmt);
    } else {
        b = Buffer::Create(w, h, fmt);
    }
    if (!b) return DDERR_OUTOFMEMORY;

    const DWORD backs = flip ? in.dwBackBufferCount : 0;  // validated above

    // A cube map's root is its first face; the other faces hang off it, each with its own
    // mip levels. ddraw.dll lists the faces on the root from the last one back.
    static const DWORD kFaces[6] = {DDSCAPS2_CUBEMAP_POSITIVEX, DDSCAPS2_CUBEMAP_NEGATIVEX, DDSCAPS2_CUBEMAP_POSITIVEY,
                                    DDSCAPS2_CUBEMAP_NEGATIVEY, DDSCAPS2_CUBEMAP_POSITIVEZ, DDSCAPS2_CUBEMAP_NEGATIVEZ};
    DWORD faces[6];
    size_t faceCount = 0;
    if (cube)
        for (DWORD f : kFaces)
            if (caps2 & f) faces[faceCount++] = f;
    auto faceDesc = [&](DWORD face) {
        DDSURFACEDESC2 d = base;
        if (face) d.ddsCaps.dwCaps2 = (caps2 & ~DDSCAPS2_CUBEMAP_ALLFACES) | face;
        return d;
    };

    DDSURFACEDESC2 frontDesc = faceDesc(cube ? faces[0] : 0);
    if (flip) frontDesc.ddsCaps.dwCaps |= DDSCAPS_FRONTBUFFER;
    FinishCaps(&frontDesc);
    Surface* front;
    try {
        front = new Surface(this, frontDesc, b, version);
    } catch (const std::bad_alloc&) {
        b->Release();
        return DDERR_OUTOFMEMORY;
    }
    front->sysmemRequested = (asked & DDSCAPS_SYSTEMMEMORY) != 0;
    front->ownFormat = !(caps & DDSCAPS_PRIMARYSURFACE) && (in.dwFlags & (DDSD_PIXELFORMAT | DDSD_ZBUFFERBITDEPTH));
    if (caps & DDSCAPS_OWNDC) front->uniqueness = 0;
    const int frontSlot = Surface::SlotFor(version);
    front->AddRef(frontSlot);
    if (describe) {
        // Only the description: the members (back buffers, mip levels) don't change it.
        // The surface goes again under the lock that made it, so no other thread can
        // find it without memory in `surfaces`.
        front->GetSurfaceDesc(describe);
        front->Release(frontSlot);
        return DD_OK;
    }

    // A member of the complex surface, owned by the root (front).
    auto addMember = [&](const DDSURFACEDESC2& d, int mw, int mh) -> Surface* {
        Buffer* mb = Buffer::Create(mw, mh, fmt);
        if (!mb) return nullptr;
        DDSURFACEDESC2 md = d;
        md.dwWidth = DWORD(mw);
        md.dwHeight = DWORD(mh);
        FinishCaps(&md);
        Surface* m = nullptr;
        try {
            if (Room(front->children)) m = new Surface(this, md, mb, version);
        } catch (const std::bad_alloc&) {
        }
        if (!m) {
            mb->Release();
            return nullptr;
        }
        m->sysmemRequested = front->sysmemRequested;
        m->ownFormat = front->ownFormat;
        m->uniqueness = front->uniqueness;
        m->AddRef(Surface::SlotFor(version));  // the root's reference, counted as ddraw.dll does
        m->root = front;
        front->children.push_back(m);
        return m;
    };
    // Mip levels 1.. below a face (or a plain texture), each attached to the one above.
    auto addLevels = [&](Surface* top, const DDSURFACEDESC2& d) -> bool {
        Surface* prev = top;
        int mw = w, mh = h;
        for (DWORD i = 1; i < levels; i++) {
            mw = std::max(mw / 2, 1);
            mh = std::max(mh / 2, 1);
            DDSURFACEDESC2 md = d;
            md.dwMipMapCount = levels - i;
            md.ddsCaps.dwCaps2 |= DDSCAPS2_MIPMAPSUBLEVEL;
            Surface* level = addMember(md, mw, mh);
            if (!level || !Room(prev->attached)) return false;
            prev->attached.push_back(level);
            prev = level;
        }
        return true;
    };

    const auto fail = [&] {  // the root takes the members made so far with it
        front->Release(frontSlot);
        return DDERR_OUTOFMEMORY;
    };
    if (flip) {
        try {
            front->flipChain.reserve(size_t(backs) + 1);
        } catch (const std::bad_alloc&) {
            return fail();
        }
        front->flipChain.push_back(front);
        Surface* prev = front;
        for (DWORD i = 0; i < backs; i++) {
            DDSURFACEDESC2 bd = base;
            bd.dwFlags &= ~DDSD_BACKBUFFERCOUNT;
            bd.dwBackBufferCount = 0;
            bd.ddsCaps.dwCaps = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE)) |
                                (i == 0 ? DDSCAPS_BACKBUFFER : 0);
            Surface* back = addMember(bd, w, h);
            if (!back) return fail();
            front->flipChain.push_back(back);
            prev->chainNext = back;
            prev = back;
        }
        prev->chainNext = front;
    } else {
        Surface* others[5];  // the other cube faces, in order
        size_t otherCount = 0;
        for (size_t i = 1; i < faceCount; i++) {
            const DDSURFACEDESC2 fd = faceDesc(faces[i]);
            Surface* face = addMember(fd, w, h);
            if (!face || !addLevels(face, fd)) return fail();
            others[otherCount++] = face;
        }
        try {
            front->attached.reserve(otherCount + 1);  // (and the first mip level)
        } catch (const std::bad_alloc&) {
            return fail();
        }
        for (size_t i = otherCount; i-- > 0;) front->attached.push_back(others[i]);
        if (!addLevels(front, frontDesc)) return fail();
    }

    front->ddVersion = ddVersion;
    if (version >= 4) {  // made through IDirectDraw4/7: references it, as in ddraw.dll
        AddRef(version);
        front->ddRef = version;
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        front->isPrimary = true;
        primary = front;
        // The exclusive object's new primary makes the other objects' primaries lost, as
        // in ddraw.dll.
        if (Exclusive()) {
            for (DDraw* d : g_ddraws) {
                if (d == this || !d->primary) continue;
                d->primary->lost = true;
                d->primary->drawableLost = false;
                for (Surface* c : d->primary->children) c->lost = true, c->drawableLost = false;
            }
        }
        ApplyGamma();  // a new primary starts without a ramp
        gdiBuf_ = front->buf;
        presenter_->Start();
        MarkDirty();
    }
    // (The kind of format named: a log shows whether a game took a luminance or alpha-only
    // texture format from the list, or made a 1-, 2- or 4-bit surface.)
    DD2D9_LOG("  -> %p %dx%d %lubpp%s%s", static_cast<void*>(front), w, h, fmt.bpp,
              fmt.lum ? (fmt.a ? " luminance with alpha" : " luminance") : fmt.alphaOnly ? " alpha-only" : fmt.palLow ? " palettized" : "",
              front->isPrimary ? " (primary)" : "");
    for (size_t i = 1; i < front->flipChain.size(); i++)
        DD2D9_LOG("  -> back buffer %p", static_cast<void*>(front->flipChain[i]));
    *out = front->Iface(version);
    return DD_OK;
}

}  // namespace dd2d9
