// Software blitter: Blt / BltFast / color fill.
#include "objects.h"
#include "gpures.h"

#include <emmintrin.h>

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

// Scratch buffers reused from blit to blit; every blit runs with g_lock held.
std::vector<RECT> g_clips;
std::vector<int> g_xmap;
std::vector<uint8_t> g_temp;

// Maps each clipped destination column to a source column.
// Destination rectangles can be huge when a clipper is attached (only the
// clipped part is drawn), so the mapping is done in 64-bit arithmetic.
static const int* BuildXMap(const BlitJob& j) {
    const int64_t dw = int64_t(j.d0.right) - j.d0.left, sw = int64_t(j.s0.right) - j.s0.left;
    g_xmap.resize(size_t(j.dc.right - j.dc.left));
    for (int x = j.dc.left; x < j.dc.right; x++) {
        int64_t rx = int64_t(x) - j.d0.left;
        if (j.mirrorX) rx = dw - 1 - rx;
        g_xmap[size_t(x - j.dc.left)] = int(j.s0.left + rx * sw / dw);
    }
    return g_xmap.data();
}

static inline int MapY(const BlitJob& j, int y) {
    const int64_t dh = int64_t(j.d0.bottom) - j.d0.top, sh = int64_t(j.s0.bottom) - j.s0.top;
    int64_t ry = int64_t(y) - j.d0.top;
    if (j.mirrorY) ry = dh - 1 - ry;
    return int(j.s0.top + ry * sh / dh);
}

// In-range test for color keys: lo <= k <= hi in one unsigned comparison.
static inline bool InKey(uint32_t k, uint32_t lo, uint32_t hi) { return k - lo <= hi - lo; }

// SSE2 helpers per pixel size.
template <typename T> struct Simd;
template <> struct Simd<uint8_t> {
    static __m128i Set(uint32_t v) { return _mm_set1_epi8(char(v)); }
    static __m128i Eq(__m128i a, __m128i b) { return _mm_cmpeq_epi8(a, b); }
};
template <> struct Simd<uint16_t> {
    static __m128i Set(uint32_t v) { return _mm_set1_epi16(short(v)); }
    static __m128i Eq(__m128i a, __m128i b) { return _mm_cmpeq_epi16(a, b); }
};
template <> struct Simd<uint32_t> {
    static __m128i Set(uint32_t v) { return _mm_set1_epi32(int(v)); }
    static __m128i Eq(__m128i a, __m128i b) { return _mm_cmpeq_epi32(a, b); }
};

// One row of a 1:1 copy with a single-color source key (the usual sprite blit):
// pixels whose masked value equals `key` are left alone.
template <typename T>
static void KeyRow(T* d, const T* s, int n, uint32_t mask, uint32_t key) {
    constexpr int kPerVector = 16 / sizeof(T);
    const __m128i vmask = Simd<T>::Set(mask), vkey = Simd<T>::Set(key);
    int i = 0;
    for (; i + kPerVector <= n; i += kPerVector) {
        const __m128i sv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i));
        const __m128i keyed = Simd<T>::Eq(_mm_and_si128(sv, vmask), vkey);
        const int bits = _mm_movemask_epi8(keyed);
        if (bits == 0xFFFF) continue;  // all transparent
        __m128i* dp = reinterpret_cast<__m128i*>(d + i);
        if (bits == 0) {  // all opaque
            _mm_storeu_si128(dp, sv);
            continue;
        }
        const __m128i dv = _mm_loadu_si128(dp);
        _mm_storeu_si128(dp, _mm_or_si128(_mm_and_si128(keyed, dv), _mm_andnot_si128(keyed, sv)));
    }
    for (; i < n; i++) {
        const uint32_t v = s[i];
        if ((v & mask) != key) d[i] = T(v);
    }
}

// Any combination of keys, 1:1 (Direct) or stretched / mirrored columns.
template <typename T, bool Direct, bool SrcKey, bool DstKey>
static void KeyRows(const BlitJob& j, const int* xmap) {
    const int n = j.dc.right - j.dc.left;
    const int sx = j.s0.left + (j.dc.left - j.d0.left);  // first source column when Direct
    for (int y = j.dc.top; y < j.dc.bottom; y++) {
        T* drow = reinterpret_cast<T*>(j.dst + y * j.dpitch) + j.dc.left;
        const T* srow = reinterpret_cast<const T*>(j.src + MapY(j, y) * j.spitch);
        for (int i = 0; i < n; i++) {
            const uint32_t v = Direct ? srow[sx + i] : srow[xmap[i]];
            if (SrcKey && InKey(v & j.skMask, j.skLo, j.skHi)) continue;
            if (DstKey && !InKey(uint32_t(drow[i]) & j.dkMask, j.dkLo, j.dkHi)) continue;
            drow[i] = T(v);
        }
    }
}

template <typename T, bool Direct>
static void KeyRowsDispatch(const BlitJob& j, const int* xmap) {
    if (j.srcKey && j.dstKey) KeyRows<T, Direct, true, true>(j, xmap);
    else if (j.srcKey) KeyRows<T, Direct, true, false>(j, xmap);
    else if (j.dstKey) KeyRows<T, Direct, false, true>(j, xmap);
    else KeyRows<T, Direct, false, false>(j, xmap);
}

template <typename T>
static void BlitSameFormat(const BlitJob& j) {
    const int dw = j.d0.right - j.d0.left, sw = j.s0.right - j.s0.left;
    const bool direct = !j.mirrorX && dw == sw;
    const int n = j.dc.right - j.dc.left;
    if (direct && !j.dstKey && (!j.srcKey || j.skLo == j.skHi)) {
        const int sx = j.s0.left + (j.dc.left - j.d0.left);
        for (int y = j.dc.top; y < j.dc.bottom; y++) {
            T* drow = reinterpret_cast<T*>(j.dst + y * j.dpitch) + j.dc.left;
            const T* srow = reinterpret_cast<const T*>(j.src + MapY(j, y) * j.spitch) + sx;
            if (j.srcKey) KeyRow(drow, srow, n, j.skMask, j.skLo);
            else memcpy(drow, srow, size_t(n) * sizeof(T));
        }
    } else if (direct) {
        KeyRowsDispatch<T, true>(j, nullptr);
    } else {
        KeyRowsDispatch<T, false>(j, BuildXMap(j));
    }
}

static void BlitGeneric(const BlitJob& j) {
    const int* xmap = BuildXMap(j);
    const int n = j.dc.right - j.dc.left;
    for (int y = j.dc.top; y < j.dc.bottom; y++) {
        uint8_t* drow = j.dst + y * j.dpitch + j.dc.left * j.dbytes;
        const uint8_t* srow = j.src + MapY(j, y) * j.spitch;
        for (int i = 0; i < n; i++) {
            uint32_t v = ReadPx(srow + xmap[i] * j.sbytes, j.sbytes);
            if (j.srcKey && InKey(v & j.skMask, j.skLo, j.skHi)) continue;
            uint8_t* dp = drow + i * j.dbytes;
            if (j.dstKey && !InKey(ReadPx(dp, j.dbytes) & j.dkMask, j.dkLo, j.dkHi)) continue;
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

// Reports the union of the drawn rectangles to the presenter (primary surface only).
template <class Clips>
static void MarkClipsDirty(Surface* s, const Clips& clips) {
    if (!s->isPrimary) return;
    for (const RECT& c : clips) s->MarkDirtyIfVisible(&c);
}

}  // namespace

// Checks in ddraw.dll's order (spec: re/spec/surface-blt.md, section 1); BltLocked then
// draws. As ddraw.dll's software blitter: ROPs SRCCOPY, BLACKNESS and WHITENESS only, no
// rotation, no DDROPS.
HRESULT Surface::Blt(RECT* dstRect, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx, int version,
                     bool srcOldIface) {
    Guard l;
    DD2D9_TRACE("Blt %p[%s] <- %p[%s] flags=0x%lx%s fx=0x%lx fill=0x%lx", static_cast<void*>(this),
               RectText(dstRect).s, static_cast<void*>(src), RectText(srcRect).s, flags, isPrimary ? " (primary)" : "",
               fx ? fx->dwDDFX : 0, fx ? fx->dwFillColor : 0);
    if (CantDraw() || (src && src->CantDraw(false))) return DDERR_SURFACELOST;
    // 0x80000000 is the Direct3D 8/9 runtime's own flag (window clipping, with the
    // destination-alpha bits); ddraw.dll takes it off first.
    if (flags & 0x80000000) flags &= ~0x8000000Bu;
    if (flags & DDBLT_LAST_PRESENTATION) return DDERR_INVALIDPARAMS;
    if (version == 7 && (flags & DDBLT_WAIT) && (flags & DDBLT_DONOTWAIT)) return DDERR_INVALIDPARAMS;
    if (lockCount || (src && src->lockCount)) return DDERR_SURFACEBUSY;  // locked, or a DC out
    BumpUniqueness();  // as ddraw.dll: even if the blit fails from here on
    // The flags that read the DDBLTFX (DEPTHFILL, the overrides, ROP, DDROPS, DDFX, COLORFILL, ...).
    const DWORD kNeedFx = 0x02F75D4A;
    if (fx ? fx->dwSize != sizeof(DDBLTFX) : (flags & kNeedFx)) return DDERR_INVALIDPARAMS;

    // Color keys.
    if ((flags & DDBLT_KEYSRC) && !src) return DDERR_INVALIDOBJECT;
    if ((flags & DDBLT_KEYSRC) && ((flags & DDBLT_KEYSRCOVERRIDE) || !src->hasCk[kCkSrcBlt])) return DDERR_INVALIDPARAMS;
    if ((flags & DDBLT_KEYDEST) && ((flags & DDBLT_KEYDESTOVERRIDE) || !hasCk[kCkDestBlt])) return DDERR_INVALIDPARAMS;
    // The operation: the first of these flags decides.
    const DWORD fills = DDBLT_COLORFILL | DDBLT_DEPTHFILL;
    if (flags & DDBLT_ROP) {
        if (flags & (fills | DDBLT_DDFX | DDBLT_DDROPS | DDBLT_ROTATIONANGLE)) return DDERR_INVALIDPARAMS;
        const DWORD rop = fx->dwROP;
        if (rop != SRCCOPY && rop != BLACKNESS && rop != WHITENESS) return DDERR_NORASTEROPHW;
        if (rop == SRCCOPY && !src) return DDERR_INVALIDPARAMS;
    } else if (flags & DDBLT_COLORFILL) {
        if (src || buf->fmt.zbuf) return DDERR_INVALIDPARAMS;
    } else if (flags & DDBLT_DEPTHFILL) {
        if (src || !buf->fmt.zbuf) return DDERR_INVALIDPARAMS;
    } else if (flags & DDBLT_DDROPS) {
        if (flags & (DDBLT_ROTATIONANGLE | DDBLT_DDFX)) return DDERR_INVALIDPARAMS;
        return DDERR_NODDROPSHW;
    } else if (flags & DDBLT_DDFX) {
        if (flags & DDBLT_ROTATIONANGLE) return DDERR_INVALIDPARAMS;
        if (fx->dwDDFX & DDBLTFX_ARITHSTRETCHY) return DDERR_NOSTRETCHHW;
        if (fx->dwDDFX & (DDBLTFX_ROTATE90 | DDBLTFX_ROTATE180 | DDBLTFX_ROTATE270)) return DDERR_NOROTATIONHW;
        if (!src) return DDERR_INVALIDPARAMS;  // ddraw.dll's blitter faults on it
    } else if (flags & DDBLT_ROTATIONANGLE) {
        return DDERR_NOROTATIONHW;
    } else if (!src) {
        return DDERR_INVALIDPARAMS;
    }

    // Rectangles: the destination's bounds only without a clipper (the clip list does it).
    const RECT d = dstRect ? *dstRect : RECT{0, 0, buf->w, buf->h};
    if (d.right <= d.left || d.bottom <= d.top) return DDERR_INVALIDRECT;
    if (!clipper && (d.left < 0 || d.top < 0 || d.right > buf->w || d.bottom > buf->h)) return DDERR_INVALIDRECT;
    if (src) {
        const RECT s = srcRect ? *srcRect : RECT{0, 0, src->W(), src->H()};
        if (s.right <= s.left || s.bottom <= s.top || s.left < 0 || s.top < 0 || s.right > src->W() ||
            s.bottom > src->H())
            return DDERR_INVALIDRECT;
    }
    // A fill takes no source rectangle.
    const bool usesSource = !(flags & (DDBLT_ROP | fills)) || ((flags & DDBLT_ROP) && fx->dwROP == SRCCOPY);
    const HRESULT hr = BltLocked(dstRect, usesSource ? src : nullptr, srcRect, flags, fx, true, srcOldIface);
    // fpslimit: a game that shows its frames by blitting them to the primary (windowed, or
    // without a flip chain) is paced at successful blits covering half of its screen or
    // more: the primary in exclusive mode, else the clipper's window (a windowed game's
    // primary is the whole desktop).
    if (g_config.fpsLimit > 0 && SUCCEEDED(hr) && isPrimary && src && src != this) {
        int64_t whole = int64_t(buf->w) * buf->h;
        RECT c;
        if (!dd->Exclusive() && clipper && clipper->hwnd && GetClientRect(clipper->hwnd, &c))
            whole = int64_t(c.right - c.left) * (c.bottom - c.top);
        const int64_t area = dstRect ? int64_t(dstRect->right - dstRect->left) * (dstRect->bottom - dstRect->top)
                                     : int64_t(buf->w) * buf->h;
        if (whole > 0 && area * 2 >= whole) RunWhenUnlocked([](void*) { LimitFrame(); }, nullptr);
    }
    return hr;
}

// The drawing (callers have checked what ddraw.dll checks; the contents stamp is theirs
// to change). Rectangle rules follow ddraw.dll: an empty rectangle, or a source rectangle
// outside the source surface, is DDERR_INVALIDRECT. Without a clipper the
// destination rectangle must lie inside the destination surface; with one, the
// blit is clipped to the clipper's region.
HRESULT Surface::BltLocked(RECT* dstRect, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx, bool useClipper,
                           bool destKeyFromSource) {
    // GDI may still be drawing on a surface whose DC is out (ReleaseDC flushes).
    if (dcOut || (src && src->dcOut)) GdiFlush();
    if (buf->fmt.fourcc) return DDERR_UNSUPPORTED;
    // What Direct3D drew into either surface is read back first.
    if (src && src != this) src->CpuAccess(false);
    CpuAccess(true);
    const RECT bounds = {0, 0, buf->w, buf->h};
    RECT d = dstRect ? *dstRect : bounds;
    if (d.right <= d.left || d.bottom <= d.top) return DDERR_INVALIDRECT;

    std::vector<RECT>& clips = g_clips;
    clips.clear();
    if (useClipper && clipper) {
        // A clipper with nothing to clip to fails the blit, as its GetClipList would.
        if (HRESULT hr = clipper->Usable(); FAILED(hr)) return hr;
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
        for (RECT& c : clips) {
            FillCore(&c, value);
            if (flags & DDBLT_DEPTHFILL) GpuDepthFill(this, c, value);  // a Z buffer Direct3D uses
        }
        MarkClipsDirty(this, clips);
        return DD_OK;
    }
    if (flags & DDBLT_ROP) {
        if (!fx) return DDERR_INVALIDPARAMS;
        if (fx->dwROP == BLACKNESS || fx->dwROP == WHITENESS) {
            // As ddraw.dll's blitter: white leaves the top byte of a 32-bit pixel 0.
            for (RECT& c : clips) FillCore(&c, fx->dwROP == BLACKNESS ? 0 : 0x00FFFFFFu);
                MarkClipsDirty(this, clips);
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

    // As in ddraw.dll: a key and its override together, or a key the surface doesn't
    // have, are errors.
    if ((flags & DDBLT_KEYSRC) && (flags & DDBLT_KEYSRCOVERRIDE)) return DDERR_INVALIDPARAMS;
    if ((flags & DDBLT_KEYDEST) && (flags & DDBLT_KEYDESTOVERRIDE)) return DDERR_INVALIDPARAMS;
    Surface* destKeyOwner = destKeyFromSource ? src : this;
    if ((flags & DDBLT_KEYSRC) && !src->hasCk[kCkSrcBlt]) return DDERR_INVALIDPARAMS;
    // DDBLT_KEYDEST needs a destination key on the destination even when the value
    // comes from the source, which then may have none (its last value is used).
    if ((flags & DDBLT_KEYDEST) && !hasCk[kCkDestBlt]) return DDERR_INVALIDPARAMS;
    const DDCOLORKEY* sk = nullptr;
    if (flags & DDBLT_KEYSRCOVERRIDE) {
        if (!fx) return DDERR_INVALIDPARAMS;
        sk = &fx->ddckSrcColorkey;
    } else if (flags & DDBLT_KEYSRC) {
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
    } else if (flags & DDBLT_KEYDEST) {
        dk = &destKeyOwner->ck[kCkDestBlt];
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
    if (sb == buf) {
        RECT ov;
        if (IntersectRect(&ov, &s, &d)) {
            const int sh = s.bottom - s.top;
            std::vector<uint8_t>& temp = g_temp;
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
    MarkClipsDirty(this, clips);
    return DD_OK;
}

// Checks in ddraw.dll's order (spec: re/spec/surface-blt.md, section 2). BltFast never
// clips: both rectangles must lie inside their surfaces, and a dest with a clipper fails.
HRESULT Surface::BltFast(DWORD x, DWORD y, Surface* src, RECT* srcRect, DWORD trans, int version) {
    Guard l;
    DD2D9_TRACE("BltFast %p (%lu,%lu) <- %p[%s] trans=0x%lx", static_cast<void*>(this), x, y, static_cast<void*>(src),
               RectText(srcRect).s, trans);
    if (CantDraw() || src->CantDraw(false)) return DDERR_SURFACELOST;
    if (trans & ~0x33u) return DDERR_INVALIDPARAMS;  // NO/SRC/DESTCOLORKEY, WAIT, DONOTWAIT
    if (version == 7 && (trans & DDBLTFAST_WAIT) && (trans & DDBLTFAST_DONOTWAIT)) return DDERR_INVALIDPARAMS;
    if (lockCount || src->lockCount) return DDERR_SURFACEBUSY;
    BumpUniqueness();
    const RECT s = srcRect ? *srcRect : RECT{0, 0, src->W(), src->H()};
    if (s.right <= s.left || s.bottom <= s.top || s.left < 0 || s.top < 0 || s.right > src->W() ||
        s.bottom > src->H())
        return DDERR_INVALIDRECT;
    const int64_t w = s.right - s.left, h = s.bottom - s.top;
    if (LONG(x) < 0 || LONG(y) < 0 || int64_t(x) + w > W() || int64_t(y) + h > H()) return DDERR_INVALIDRECT;
    DWORD flags = 0;
    switch (trans & 0xF) {
    case DDBLTFAST_NOCOLORKEY:
        break;
    case DDBLTFAST_SRCCOLORKEY:
        if (!src->hasCk[kCkSrcBlt]) return DDERR_INVALIDPARAMS;
        flags = DDBLT_KEYSRC;
        break;
    case DDBLTFAST_DESTCOLORKEY:  // the destination's own key, whatever the interface
        if (!hasCk[kCkDestBlt]) return DDERR_INVALIDPARAMS;
        flags = DDBLT_KEYDEST;
        break;
    default:  // both keys: what ddraw.dll's blitter makes of it wasn't determined
        return DDERR_INVALIDPARAMS;
    }
    if (clipper) return DDERR_BLTFASTCANTCLIP;
    RECT d = {LONG(x), LONG(y), LONG(int64_t(x) + w), LONG(int64_t(y) + h)};
    RECT sr = s;
    return BltLocked(&d, src, &sr, flags, nullptr, false);
}

}  // namespace dd2d9
