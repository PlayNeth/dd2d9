// Pixel formats and surface descriptions.
#include "common.h"

namespace dd2d9 {

// ---------------------------------------------------------------- formats

Format Format::ForDisplayBpp(DWORD bpp) {
    Format f;
    f.bpp = bpp;
    switch (bpp) {
    case 8: f.pal8 = true; break;
    case 15: f.bpp = 16; f.r = 0x7C00; f.g = 0x03E0; f.b = 0x001F; break;
    case 16: f.r = 0xF800; f.g = 0x07E0; f.b = 0x001F; break;
    default: f.bpp = bpp == 24 ? 24 : 32; f.r = 0xFF0000; f.g = 0xFF00; f.b = 0xFF; break;
    }
    return f;
}

bool Format::FromDDPF(const DDPIXELFORMAT& pf, Format* out) {
    Format f;
    if (pf.dwFlags & DDPF_FOURCC) {
        // Only formats whose memory layout we know; GetFourCCCodes reports none,
        // so games that check first never ask for these.
        f.fourcc = true;
        f.fourCC = pf.dwFourCC;
        switch (pf.dwFourCC) {
        case MAKEFOURCC('D', 'X', 'T', '1'): case MAKEFOURCC('D', 'X', 'T', '2'):
        case MAKEFOURCC('D', 'X', 'T', '3'): case MAKEFOURCC('D', 'X', 'T', '4'):
        case MAKEFOURCC('D', 'X', 'T', '5'):
            f.bpp = 8;  // marks block compression (see BlockCompressed)
            break;
        case MAKEFOURCC('Y', 'U', 'Y', '2'): case MAKEFOURCC('U', 'Y', 'V', 'Y'):
            f.bpp = 16;  // packed 4:2:2, two bytes per pixel
            break;
        case MAKEFOURCC('Y', 'V', '1', '2'): case MAKEFOURCC('I', '4', '2', '0'): case MAKEFOURCC('I', 'Y', 'U', 'V'):
            f.bpp = 12;  // planar 4:2:0 (see Planar)
            break;
        default:
            return false;
        }
    } else if (pf.dwFlags & DDPF_ZBUFFER) {
        f.zbuf = true;
        f.bpp = pf.dwZBufferBitDepth;
        if (f.bpp != 16 && f.bpp != 24 && f.bpp != 32) return false;
        // Depth and stencil masks, kept only when not the plain all-depth layout (r: depth,
        // a: stencil), so that the usual Z formats compare equal however they were described.
        const DWORD all = f.bpp >= 32 ? 0xFFFFFFFFu : ((1u << f.bpp) - 1);
        if ((pf.dwFlags & DDPF_STENCILBUFFER) && pf.dwStencilBitMask) f.a = pf.dwStencilBitMask;
        if (pf.dwZBitMask && (pf.dwZBitMask != all || f.a)) f.r = pf.dwZBitMask;
    } else if (pf.dwFlags & DDPF_BUMPDUDV) {
        // The layouts Direct3D 9 has for fixed-function bump mapping (U8V8L8 widened on the
        // way to the GPU).
        f.bump = true;
        f.bpp = pf.dwBumpBitCount;
        f.r = pf.dwBumpDuBitMask;
        f.g = pf.dwBumpDvBitMask;
        f.b = (pf.dwFlags & DDPF_BUMPLUMINANCE) ? pf.dwBumpLuminanceBitMask : 0;
        const bool u8v8 = f.bpp == 16 && f.r == 0xFF && f.g == 0xFF00 && !f.b;
        const bool u5v5l6 = f.bpp == 16 && f.r == 0x1F && f.g == 0x3E0 && f.b == 0xFC00;
        const bool u8v8l8 = f.bpp == 24 && f.r == 0xFF && f.g == 0xFF00 && f.b == 0xFF0000;
        if (!u8v8 && !u5v5l6 && !u8v8l8) return false;
    } else if (pf.dwFlags & DDPF_PALETTEINDEXED8) {
        f.pal8 = true;
        f.bpp = 8;
    } else if (pf.dwFlags & (DDPF_PALETTEINDEXED1 | DDPF_PALETTEINDEXED2 | DDPF_PALETTEINDEXED4)) {
        return false;
    } else if (pf.dwFlags & DDPF_RGB) {
        f.bpp = pf.dwRGBBitCount;
        f.r = pf.dwRBitMask;
        f.g = pf.dwGBitMask;
        f.b = pf.dwBBitMask;
        if (pf.dwFlags & DDPF_ALPHAPIXELS) f.a = pf.dwRGBAlphaBitMask;
        if (f.bpp != 8 && f.bpp != 16 && f.bpp != 24 && f.bpp != 32) return false;
        if (f.bpp == 8 && !f.r) f.pal8 = true;  // some games omit PALETTEINDEXED8
    } else {
        return false;
    }
    *out = f;
    return true;
}

void Format::ToDDPF(DDPIXELFORMAT* pf) const {
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(DDPIXELFORMAT);
    if (fourcc) {
        pf->dwFlags = DDPF_FOURCC;
        pf->dwFourCC = fourCC;
        // The bits per pixel of the YUV formats, as the drivers report them (and the video
        // renderers read them: VMR-7 sizes its frames with them, and with none shows none).
        if (Yuv() || Planar()) pf->dwYUVBitCount = bpp;
    } else if (bump) {
        pf->dwFlags = DDPF_BUMPDUDV | (b ? DDPF_BUMPLUMINANCE : 0);
        pf->dwBumpBitCount = bpp;
        pf->dwBumpDuBitMask = r;
        pf->dwBumpDvBitMask = g;
        pf->dwBumpLuminanceBitMask = b;
    } else if (zbuf) {
        pf->dwFlags = DDPF_ZBUFFER;
        pf->dwZBufferBitDepth = bpp;
        pf->dwZBitMask = r ? r : bpp >= 32 ? 0xFFFFFFFFu : ((1u << bpp) - 1);
        if (a) {
            pf->dwFlags |= DDPF_STENCILBUFFER;
            pf->dwStencilBitDepth = DWORD(__builtin_popcount(a));
            pf->dwStencilBitMask = a;
        }
    } else if (pal8) {
        pf->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        pf->dwRGBBitCount = 8;
    } else {
        pf->dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
        pf->dwRGBBitCount = bpp;
        pf->dwRBitMask = r;
        pf->dwGBitMask = g;
        pf->dwBBitMask = b;
        pf->dwRGBAlphaBitMask = a;
    }
}

static inline uint32_t Extract8(uint32_t v, uint32_t mask) {
    if (!mask) return 0;
    uint32_t shift = __builtin_ctz(mask);
    uint32_t max = mask >> shift;
    uint32_t c = (v & mask) >> shift;
    return max == 255 ? c : (c * 255 + max / 2) / max;
}

static inline uint32_t Insert8(uint32_t c8, uint32_t mask) {
    if (!mask) return 0;
    uint32_t shift = __builtin_ctz(mask);
    uint32_t max = mask >> shift;
    uint32_t c = max == 255 ? c8 : (c8 * max + 127) / 255;
    return (c << shift) & mask;
}

uint32_t PixelToARGB(const Format& f, uint32_t v, const PALETTEENTRY* pal) {
    if (f.pal8) {
        if (!pal) return 0xFF000000u | (v & 0xFF) * 0x010101u;
        const PALETTEENTRY& e = pal[v & 0xFF];
        return 0xFF000000u | (uint32_t(e.peRed) << 16) | (uint32_t(e.peGreen) << 8) | e.peBlue;
    }
    uint32_t a = f.a ? Extract8(v, f.a) : 0xFF;
    return (a << 24) | (Extract8(v, f.r) << 16) | (Extract8(v, f.g) << 8) | Extract8(v, f.b);
}

uint32_t ARGBToPixel(const Format& f, uint32_t argb) {
    if (f.pal8) return 0;  // no reverse palette lookup
    return Insert8((argb >> 16) & 0xFF, f.r) | Insert8((argb >> 8) & 0xFF, f.g) | Insert8(argb & 0xFF, f.b) |
           Insert8(argb >> 24, f.a);
}

// ---------------------------------------------------------------- descriptions

bool DescSizeOk(const void* desc) {
    if (!desc) return false;
    DWORD size = *static_cast<const DWORD*>(desc);
    return size == sizeof(DDSURFACEDESC) || size == sizeof(DDSURFACEDESC2);
}

bool DescIn(const void* in, DDSURFACEDESC2* out) {
    if (!DescSizeOk(in)) return false;
    if (*static_cast<const DWORD*>(in) == sizeof(DDSURFACEDESC2)) {
        *out = *static_cast<const DDSURFACEDESC2*>(in);
        return true;
    }
    const auto* d = static_cast<const DDSURFACEDESC*>(in);
    memset(out, 0, sizeof(*out));
    out->dwSize = sizeof(DDSURFACEDESC2);
    out->dwFlags = d->dwFlags;
    out->dwHeight = d->dwHeight;
    out->dwWidth = d->dwWidth;
    out->lPitch = d->lPitch;
    out->dwBackBufferCount = d->dwBackBufferCount;
    out->dwMipMapCount = d->dwMipMapCount;  // same slot as dwZBufferBitDepth / dwRefreshRate
    out->dwAlphaBitDepth = d->dwAlphaBitDepth;
    out->lpSurface = d->lpSurface;
    out->ddckCKDestOverlay = d->ddckCKDestOverlay;
    out->ddckCKDestBlt = d->ddckCKDestBlt;
    out->ddckCKSrcOverlay = d->ddckCKSrcOverlay;
    out->ddckCKSrcBlt = d->ddckCKSrcBlt;
    out->ddpfPixelFormat = d->ddpfPixelFormat;
    out->ddsCaps.dwCaps = d->ddsCaps.dwCaps;
    return true;
}

// The enumerations through the old interfaces (EnumAttachedSurfaces v1-3, EnumSurfaces
// through IDirectDraw/2) hand out ddraw.dll's DDSURFACEDESC2 cut to the old size: the
// fields are where the old structure has them, a Z buffer keeps its pixel format
// (no DDSD_ZBUFFERBITDEPTH; spec: re/spec/legacy-interfaces.md, difference 2).
void DescOutEnum(const DDSURFACEDESC2& in, DDSURFACEDESC* out) {
    static_assert(sizeof(DDSURFACEDESC) == 0x6c && offsetof(DDSURFACEDESC2, ddsCaps) == offsetof(DDSURFACEDESC, ddsCaps),
                  "the old structure is the new one's start");
    memcpy(out, &in, sizeof(DDSURFACEDESC));
    out->dwSize = sizeof(DDSURFACEDESC);
}

void DescOut(const DDSURFACEDESC2& in, void* out) {
    if (*static_cast<DWORD*>(out) == sizeof(DDSURFACEDESC2)) {
        *static_cast<DDSURFACEDESC2*>(out) = in;
        static_cast<DDSURFACEDESC2*>(out)->dwSize = sizeof(DDSURFACEDESC2);
        return;
    }
    auto* d = static_cast<DDSURFACEDESC*>(out);
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(DDSURFACEDESC);
    d->dwFlags = in.dwFlags & ~(DDSD_TEXTURESTAGE | DDSD_FVF | DDSD_SRCVBHANDLE | DDSD_DEPTH);
    d->dwHeight = in.dwHeight;
    d->dwWidth = in.dwWidth;
    d->lPitch = in.lPitch;
    d->dwBackBufferCount = in.dwBackBufferCount;
    d->dwMipMapCount = in.dwMipMapCount;
    d->dwAlphaBitDepth = in.dwAlphaBitDepth;
    d->lpSurface = in.lpSurface;
    d->ddckCKDestOverlay = in.ddckCKDestOverlay;
    d->ddckCKDestBlt = in.ddckCKDestBlt;
    d->ddckCKSrcOverlay = in.ddckCKSrcOverlay;
    d->ddckCKSrcBlt = in.ddckCKSrcBlt;
    d->ddpfPixelFormat = in.ddpfPixelFormat;
    d->ddsCaps.dwCaps = in.ddsCaps.dwCaps;
    // The old structure describes a Z buffer by its depth rather than a pixel format.
    if ((in.dwFlags & DDSD_PIXELFORMAT) && (in.ddsCaps.dwCaps & DDSCAPS_ZBUFFER)) {
        d->dwFlags = (d->dwFlags & ~DDSD_PIXELFORMAT) | DDSD_ZBUFFERBITDEPTH;
        d->dwZBufferBitDepth = in.ddpfPixelFormat.dwZBufferBitDepth;
    }
}

}  // namespace dd2d9
