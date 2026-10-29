// Software blitter: Blt / BltFast / color fill.
#include "objects.h"

#include <cstdio>

namespace dd2d9 {

// "x0,y0-x1,y1" or "full" for trace logging.
struct RectText {
    char s[48];
    explicit RectText(const RECT* r) {
        if (r) snprintf(s, sizeof(s), "%ld,%ld-%ld,%ld", r->left, r->top, r->right, r->bottom);
        else strcpy(s, "full");
    }
};

static inline uint32_t ReadPx(const uint8_t* p, int bytes) {
    switch (bytes) {
    case 1: return *p;
    case 2: return *reinterpret_cast<const uint16_t*>(p);
    case 3: return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
    default: return *reinterpret_cast<const uint32_t*>(p);
    }
}

static inline void WritePx(uint8_t* p, int bytes, uint32_t v) {
    switch (bytes) {
    case 1: *p = uint8_t(v); break;
    case 2: *reinterpret_cast<uint16_t*>(p) = uint16_t(v); break;
    case 3: p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); break;
    default: *reinterpret_cast<uint32_t*>(p) = v; break;
    }
}

void Surface::FillCore(RECT* r, DWORD value) {
    const int bytes = int(buf->fmt.Bytes());
    value &= buf->fmt.ValueMask();
    const int n = r->right - r->left;
    for (int y = r->top; y < r->bottom; y++) {
        uint8_t* row = buf->Row(y) + r->left * bytes;
        switch (bytes) {
        case 1: memset(row, int(value), size_t(n)); break;
        case 2: for (int x = 0; x < n; x++) reinterpret_cast<uint16_t*>(row)[x] = uint16_t(value); break;
        case 4: for (int x = 0; x < n; x++) reinterpret_cast<uint32_t*>(row)[x] = value; break;
        default: for (int x = 0; x < n; x++) WritePx(row + x * 3, 3, value); break;
        }
    }
}

namespace {

struct BlitJob {
    uint8_t* dst;
    LONG dpitch;
    const uint8_t* src;
    LONG spitch;
    int dbytes, sbytes;
    RECT d0;  // full destination rectangle (may extend past the surface)
    RECT s0;  // source rectangle
    RECT dc;  // destination rectangle clipped to the surface
    bool mirrorX, mirrorY;
    bool srcKey;
    uint32_t skLo, skHi, skMask;
    bool dstKey;
    uint32_t dkLo, dkHi, dkMask;
    bool convert;
    const Format* sf;
    const Format* df;
    const PALETTEENTRY* spal;
};

// Maps each clipped destination column to a source column.
// Destination rectangles can be huge when a clipper is attached (only the
// clipped part is drawn), so the mapping is done in 64-bit arithmetic.
static void BuildXMap(const BlitJob& j, std::vector<int>& xmap) {
    const int64_t dw = int64_t(j.d0.right) - j.d0.left, sw = int64_t(j.s0.right) - j.s0.left;
    xmap.resize(size_t(j.dc.right - j.dc.left));
    for (int x = j.dc.left; x < j.dc.right; x++) {
        int64_t rx = int64_t(x) - j.d0.left;
        if (j.mirrorX) rx = dw - 1 - rx;
        xmap[size_t(x - j.dc.left)] = int(j.s0.left + rx * sw / dw);
    }
}

static inline int MapY(const BlitJob& j, int y) {
    const int64_t dh = int64_t(j.d0.bottom) - j.d0.top, sh = int64_t(j.s0.bottom) - j.s0.top;
    int64_t ry = int64_t(y) - j.d0.top;
    if (j.mirrorY) ry = dh - 1 - ry;
    return int(j.s0.top + ry * sh / dh);
}

template <typename T>
static void BlitSameFormat(const BlitJob& j) {
    const int dw = j.d0.right - j.d0.left, sw = j.s0.right - j.s0.left;
    const bool rowCopy = !j.mirrorX && dw == sw && !j.srcKey && !j.dstKey;
    std::vector<int> xmap;
    if (!rowCopy) BuildXMap(j, xmap);
    const int n = j.dc.right - j.dc.left;
    for (int y = j.dc.top; y < j.dc.bottom; y++) {
        T* drow = reinterpret_cast<T*>(j.dst + y * j.dpitch) + j.dc.left;
        const T* srow = reinterpret_cast<const T*>(j.src + MapY(j, y) * j.spitch);
        if (rowCopy) {
            memcpy(drow, srow + j.s0.left + (j.dc.left - j.d0.left), size_t(n) * sizeof(T));
            continue;
        }
        for (int i = 0; i < n; i++) {
            uint32_t v = srow[xmap[size_t(i)]];
            if (j.srcKey) {
                uint32_t k = v & j.skMask;
                if (k >= j.skLo && k <= j.skHi) continue;
            }
            if (j.dstKey) {
                uint32_t k = uint32_t(drow[i]) & j.dkMask;
                if (k < j.dkLo || k > j.dkHi) continue;
            }
            drow[i] = T(v);
        }
    }
}

static void BlitGeneric(const BlitJob& j) {
    std::vector<int> xmap;
    BuildXMap(j, xmap);
    const int n = j.dc.right - j.dc.left;
    for (int y = j.dc.top; y < j.dc.bottom; y++) {
        uint8_t* drow = j.dst + y * j.dpitch + j.dc.left * j.dbytes;
        const uint8_t* srow = j.src + MapY(j, y) * j.spitch;
        for (int i = 0; i < n; i++) {
            uint32_t v = ReadPx(srow + xmap[size_t(i)] * j.sbytes, j.sbytes);
            if (j.srcKey) {
                uint32_t k = v & j.skMask;
                if (k >= j.skLo && k <= j.skHi) continue;
            }
            uint8_t* dp = drow + i * j.dbytes;
            if (j.dstKey) {
                uint32_t k = ReadPx(dp, j.dbytes) & j.dkMask;
                if (k < j.dkLo || k > j.dkHi) continue;
            }
            if (j.convert) v = ARGBToPixel(*j.df, PixelToARGB(*j.sf, v, j.spal));
            WritePx(dp, j.dbytes, v);
        }
    }
}

static bool SameLayout(const Format& a, const Format& b) {
    if (a.bpp != b.bpp || a.pal8 != b.pal8 || a.zbuf != b.zbuf) return false;
    if (a.pal8 || a.zbuf) return true;
    return a.r == b.r && a.g == b.g && a.b == b.b;  // alpha vs. X bits copy unchanged
}

}  // namespace

HRESULT Surface::Blt(RECT* dstRect, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx) {
    Guard l;
    DD2D9_TRACE("Blt %p[%s] <- %p[%s] flags=0x%lx%s fx=0x%lx fill=0x%lx", static_cast<void*>(this),
               RectText(dstRect).s, static_cast<void*>(src), RectText(srcRect).s, flags, isPrimary ? " (primary)" : "",
               fx ? fx->dwDDFX : 0, fx ? fx->dwFillColor : 0);
    return BltLocked(dstRect, src, srcRect, flags, fx, true);
}

// Rectangle rules follow ddraw.dll: an empty rectangle, or a source rectangle
// outside the source surface, is DDERR_INVALIDRECT. Without a clipper the
// destination rectangle must lie inside the destination surface; with one, the
// blit is clipped to the clipper's region.
HRESULT Surface::BltLocked(RECT* dstRect, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx, bool useClipper) {
    GdiFlush();
    if (buf->fmt.fourcc) return DDERR_UNSUPPORTED;
    const RECT bounds = {0, 0, buf->w, buf->h};
    RECT d = dstRect ? *dstRect : bounds;
    if (d.right <= d.left || d.bottom <= d.top) return DDERR_INVALIDRECT;

    std::vector<RECT> clips;
    if (useClipper && clipper) {
        for (const RECT& c : clipper->Region(this)) {
            RECT r;
            if (IntersectRect(&r, &c, &bounds) && IntersectRect(&r, &r, &d)) clips.push_back(r);
        }
    } else {
        if (d.left < 0 || d.top < 0 || d.right > buf->w || d.bottom > buf->h) return DDERR_INVALIDRECT;
        clips.push_back(d);
    }

    if (flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) {
        if (!fx) return DDERR_INVALIDPARAMS;
        const DWORD value = (flags & DDBLT_COLORFILL) ? fx->dwFillColor : fx->dwFillDepth;
        for (RECT& c : clips) FillCore(&c, value);
        uniqueness++;
        MarkDirtyIfVisible();
        return DD_OK;
    }
    if (flags & DDBLT_ROP) {
        if (!fx) return DDERR_INVALIDPARAMS;
        if (fx->dwROP == BLACKNESS || fx->dwROP == WHITENESS) {
            for (RECT& c : clips) FillCore(&c, fx->dwROP == BLACKNESS ? 0 : 0xFFFFFFFFu);
            uniqueness++;
            MarkDirtyIfVisible();
            return DD_OK;
        }
        if (fx->dwROP != SRCCOPY) {
            DD2D9_LOG("Blt: unsupported ROP 0x%08lx", fx->dwROP);
            return DD_OK;
        }
    }
    if (!src) return DDERR_INVALIDPARAMS;
    Buffer* sb = src->buf;
    if (sb->fmt.fourcc) return DDERR_UNSUPPORTED;
    RECT s = srcRect ? *srcRect : RECT{0, 0, sb->w, sb->h};
    if (s.right <= s.left || s.bottom <= s.top || s.left < 0 || s.top < 0 || s.right > sb->w || s.bottom > sb->h)
        return DDERR_INVALIDRECT;

    BlitJob j = {};
    j.dst = buf->bits;
    j.dpitch = buf->pitch;
    j.src = sb->bits;
    j.spitch = sb->pitch;
    j.dbytes = int(buf->fmt.Bytes());
    j.sbytes = int(sb->fmt.Bytes());
    j.d0 = d;
    j.s0 = s;
    j.sf = &sb->fmt;
    j.df = &buf->fmt;
    j.spal = src->EffectivePalette();
    j.convert = !SameLayout(sb->fmt, buf->fmt);
    if (j.convert && buf->fmt.pal8) DD2D9_STUB("Blt: conversion to an 8-bit surface (writes index 0)");

    const DDCOLORKEY* sk = nullptr;
    if (flags & DDBLT_KEYSRCOVERRIDE) {
        if (!fx) return DDERR_INVALIDPARAMS;
        sk = &fx->ddckSrcColorkey;
    } else if ((flags & DDBLT_KEYSRC) && src->hasCk[kCkSrcBlt]) {
        sk = &src->ck[kCkSrcBlt];
    }
    if (sk) {
        // An override key is a single color: games often leave the high value uninitialized.
        const bool range = !(flags & DDBLT_KEYSRCOVERRIDE);
        j.srcKey = true;
        j.skMask = sb->fmt.KeyMask();
        j.skLo = sk->dwColorSpaceLowValue & j.skMask;
        j.skHi = range ? sk->dwColorSpaceHighValue & j.skMask : j.skLo;
        if (j.skHi < j.skLo) j.skHi = j.skLo;
    }
    const DDCOLORKEY* dk = nullptr;
    if (flags & DDBLT_KEYDESTOVERRIDE) {
        if (!fx) return DDERR_INVALIDPARAMS;
        dk = &fx->ddckDestColorkey;
    } else if ((flags & DDBLT_KEYDEST) && hasCk[kCkDestBlt]) {
        dk = &ck[kCkDestBlt];
    }
    if (dk) {
        const bool range = !(flags & DDBLT_KEYDESTOVERRIDE);
        j.dstKey = true;
        j.dkMask = buf->fmt.KeyMask();
        j.dkLo = dk->dwColorSpaceLowValue & j.dkMask;
        j.dkHi = range ? dk->dwColorSpaceHighValue & j.dkMask : j.dkLo;
        if (j.dkHi < j.dkLo) j.dkHi = j.dkLo;
    }
    if ((flags & DDBLT_DDFX) && fx) {
        j.mirrorX = (fx->dwDDFX & DDBLTFX_MIRRORLEFTRIGHT) != 0;
        j.mirrorY = (fx->dwDDFX & DDBLTFX_MIRRORUPDOWN) != 0;
        if (fx->dwDDFX & ~(DDBLTFX_MIRRORLEFTRIGHT | DDBLTFX_MIRRORUPDOWN | DDBLTFX_NOTEARING))
            DD2D9_LOG("Blt: unsupported DDFX 0x%08lx", fx->dwDDFX);
    }

    // Copy the source first when it overlaps the destination in the same memory.
    std::vector<uint8_t> temp;
    if (sb == buf) {
        RECT ov;
        if (IntersectRect(&ov, &s, &d)) {
            const int sh = s.bottom - s.top;
            temp.resize(size_t(sh) * size_t(sb->pitch));
            memcpy(temp.data(), sb->Row(s.top), temp.size());
            j.src = temp.data();
            j.s0.top = 0;
            j.s0.bottom = sh;
        }
    }

    for (const RECT& c : clips) {
        j.dc = c;
        if (!j.convert && j.dbytes == j.sbytes) {
            switch (j.dbytes) {
            case 1: BlitSameFormat<uint8_t>(j); break;
            case 2: BlitSameFormat<uint16_t>(j); break;
            case 4: BlitSameFormat<uint32_t>(j); break;
            default: BlitGeneric(j); break;
            }
        } else {
            BlitGeneric(j);
        }
    }
    uniqueness++;
    MarkDirtyIfVisible();
    return DD_OK;
}

// As in ddraw.dll, BltFast never clips: both rectangles must lie inside their
// surfaces, and a requested color key must exist.
HRESULT Surface::BltFast(DWORD x, DWORD y, Surface* src, RECT* srcRect, DWORD trans) {
    Guard l;
    const RECT s = srcRect ? *srcRect : RECT{0, 0, src->W(), src->H()};
    DD2D9_TRACE("BltFast %p (%lu,%lu) <- %p[%s] trans=0x%lx", static_cast<void*>(this), x, y, static_cast<void*>(src),
               RectText(&s).s, trans);
    if (s.right <= s.left || s.bottom <= s.top || s.left < 0 || s.top < 0 || s.right > src->W() ||
        s.bottom > src->H())
        return DDERR_INVALIDRECT;
    const int64_t w = s.right - s.left, h = s.bottom - s.top;
    if (int64_t(x) + w > W() || int64_t(y) + h > H()) return DDERR_INVALIDRECT;
    DWORD flags = 0;
    switch (trans & 0xF) {
    case DDBLTFAST_NOCOLORKEY:
        break;
    case DDBLTFAST_SRCCOLORKEY:
        if (!src->hasCk[kCkSrcBlt]) return DDERR_INVALIDPARAMS;
        flags = DDBLT_KEYSRC;
        break;
    case DDBLTFAST_DESTCOLORKEY:
        if (!hasCk[kCkDestBlt]) return DDERR_INVALIDPARAMS;
        flags = DDBLT_KEYDEST;
        break;
    default:
        return DDERR_INVALIDPARAMS;
    }
    RECT d = {LONG(x), LONG(y), LONG(int64_t(x) + w), LONG(int64_t(y) + h)};
    RECT sr = s;
    return BltLocked(&d, src, &sr, flags, nullptr, false);
}

}  // namespace dd2d9
