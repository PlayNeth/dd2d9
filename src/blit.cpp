// Software blitter: Blt / BltFast / color fill.
#include "objects.h"
#include "convert.h"
#include "dxt.h"
#include "gpures.h"

#include <emmintrin.h>

#include <algorithm>
#include <cstdio>
#include <new>
#include <vector>

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
        default: {  // 24-bit: four pixels (three words) at a time
            uint32_t w[3];
            for (int k = 0; k < 12; k++) reinterpret_cast<uint8_t*>(w)[k] = uint8_t(value >> (8 * (k % 3)));
            int x = 0;
            for (; x + 4 <= n; x += 4) memcpy(row + x * 3, w, 12);
            for (; x < n; x++) WritePx(row + x * 3, 3, value);
            break;
        }
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
    const PALETTEENTRY* dpal;  // an 8-bit destination's palette (conversions pick from it)
    uint32_t orMask;           // set on every pixel written (g_orMask)
};

// Colour to palette index for conversions into an 8-bit surface: the nearest entry (squared
// RGB distance, the first of equals), looked up by 15-bit colour and filled on demand; made
// again when the palette's colours change. g_lock held. Without a palette: index 0.
struct NearestIndex {
    PALETTEENTRY pal[256] = {};
    uint16_t lut[32768];
    bool valid = false;
    // Once per blit: the palette the lookups pick from.
    void Prepare(const PALETTEENTRY* p) {
        if (p && (!valid || memcmp(pal, p, sizeof(pal)) != 0)) {
            memcpy(pal, p, sizeof(pal));
            memset(lut, 0xFF, sizeof(lut));
            valid = true;
        }
    }
    uint8_t operator()(const PALETTEENTRY* p, uint32_t argb) {
        if (!p) return 0;
        const uint32_t key = ((argb >> 9) & 0x7C00) | ((argb >> 6) & 0x03E0) | ((argb >> 3) & 0x001F);
        if (lut[key] == 0xFFFF) {
            const int r = int((key >> 10) << 3 | (key >> 12)), g = int(((key >> 5) & 31) << 3 | ((key >> 7) & 7)),
                      b = int((key & 31) << 3 | ((key >> 2) & 7));
            int best = 0, bd = INT32_MAX;
            for (int i = 0; i < 256; i++) {
                const int dr = r - pal[i].peRed, dg = g - pal[i].peGreen, db = b - pal[i].peBlue;
                const int dd = dr * dr + dg * dg + db * db;
                if (dd < bd) bd = dd, best = i;
            }
            lut[key] = uint16_t(best);
        }
        return uint8_t(lut[key]);
    }
};
NearestIndex g_nearest;

// Scratch buffers reused from blit to blit; every blit runs with g_lock held.
std::vector<RECT> g_clips;
std::vector<int> g_xmap;
std::vector<uint8_t> g_temp;
std::vector<uint8_t> g_keyRow;  // a stretched / mirrored keyed row's source pixels, gathered
// Set on every pixel a blit writes (BltFourCC: alpha 255 from a source without alpha).
uint32_t g_orMask;
// The source and destination keys' masks when not 0 (BltFourCC: alpha alone, which marks the
// pixels a YUV key matched).
uint32_t g_skMaskOverride, g_dkMaskOverride;

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
// (Kept out of line: inlined, the loop came out up to a fifth slower, measured with ddbench.)
__attribute__((noinline)) static void KeyRow(T* d, const T* s, int n, uint32_t mask, uint32_t key) {
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

// lo <= (v & mask) <= hi for each pixel (all ones where so), as InKey.
template <typename T> struct InRange;
template <> struct InRange<uint8_t> {
    static __m128i Test(__m128i v, __m128i lo, __m128i range) {
        return _mm_cmpeq_epi8(_mm_subs_epu8(_mm_sub_epi8(v, lo), range), _mm_setzero_si128());
    }
    static __m128i Bias(uint32_t v) { return Simd<uint8_t>::Set(v); }
};
template <> struct InRange<uint16_t> {
    static __m128i Test(__m128i v, __m128i lo, __m128i range) {
        return _mm_cmpeq_epi16(_mm_subs_epu16(_mm_sub_epi16(v, lo), range), _mm_setzero_si128());
    }
    static __m128i Bias(uint32_t v) { return Simd<uint16_t>::Set(v); }
};
template <> struct InRange<uint32_t> {
    // (No unsigned compare: both sides with the top bit flipped, compared signed; `range` comes
    // flipped already, from Bias.)
    static __m128i Test(__m128i v, __m128i lo, __m128i range) {
        const __m128i d = _mm_xor_si128(_mm_sub_epi32(v, lo), _mm_set1_epi32(INT32_MIN));
        return _mm_xor_si128(_mm_cmpgt_epi32(d, range), _mm_set1_epi32(-1));
    }
    static __m128i Bias(uint32_t v) { return _mm_set1_epi32(int(v ^ 0x80000000u)); }
};

// One row, 1:1, any combination of keys (ranges too) and the OR mask: a source pixel is
// written where its key doesn't match and the destination's key does.
template <typename T, bool SrcKey, bool DstKey>
__attribute__((noinline)) static void KeyRowAny(T* d, const T* s, int n, const BlitJob& j) {
    constexpr int kPerVector = 16 / sizeof(T);
    const __m128i skMask = Simd<T>::Set(j.skMask), skLo = Simd<T>::Set(j.skLo), skRange = InRange<T>::Bias(j.skHi - j.skLo);
    const __m128i dkMask = Simd<T>::Set(j.dkMask), dkLo = Simd<T>::Set(j.dkLo), dkRange = InRange<T>::Bias(j.dkHi - j.dkLo);
    const __m128i orMask = Simd<T>::Set(j.orMask);
    int i = 0;
    for (; i + kPerVector <= n; i += kPerVector) {
        const __m128i sv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + i));
        __m128i* dp = reinterpret_cast<__m128i*>(d + i);
        const __m128i dv = _mm_loadu_si128(dp);
        __m128i write = _mm_set1_epi32(-1);
        if (SrcKey) write = _mm_andnot_si128(InRange<T>::Test(_mm_and_si128(sv, skMask), skLo, skRange), write);
        if (DstKey) write = _mm_and_si128(InRange<T>::Test(_mm_and_si128(dv, dkMask), dkLo, dkRange), write);
        const int bits = _mm_movemask_epi8(write);
        if (bits == 0) continue;
        const __m128i out = _mm_or_si128(sv, orMask);
        _mm_storeu_si128(dp, bits == 0xFFFF ? out : _mm_or_si128(_mm_and_si128(write, out), _mm_andnot_si128(write, dv)));
    }
    for (; i < n; i++) {
        const uint32_t v = s[i];
        if (SrcKey && InKey(v & j.skMask, j.skLo, j.skHi)) continue;
        if (DstKey && !InKey(uint32_t(d[i]) & j.dkMask, j.dkLo, j.dkHi)) continue;
        d[i] = T(v | j.orMask);
    }
}

// Any combination of keys, 1:1 (Direct) or stretched / mirrored columns (out of line, as KeyRow):
// the row's source pixels (gathered by column first when not Direct) through KeyRowAny.
template <typename T, bool Direct, bool SrcKey, bool DstKey>
__attribute__((noinline)) static void KeyRows(const BlitJob& j, const int* xmap) {
    const int n = j.dc.right - j.dc.left;
    const int sx = j.s0.left + (j.dc.left - j.d0.left);  // first source column when Direct
    const uint32_t orMask = j.orMask;
    T* gathered = nullptr;
    if (!Direct) {
        g_keyRow.resize(size_t(n) * sizeof(T));
        gathered = reinterpret_cast<T*>(g_keyRow.data());
    }
    int lastSy = -1;
    for (int y = j.dc.top; y < j.dc.bottom; y++) {
        T* drow = reinterpret_cast<T*>(j.dst + y * j.dpitch) + j.dc.left;
        const int sy = MapY(j, y);
        if (!SrcKey && !DstKey && sy == lastSy) {  // stretched taller: the row above again
            memcpy(drow, reinterpret_cast<const uint8_t*>(drow) - j.dpitch, size_t(n) * sizeof(T));
            continue;
        }
        lastSy = sy;
        const T* srow = reinterpret_cast<const T*>(j.src + sy * j.spitch);
        const T* from = srow + sx;
        if (!Direct) {
            for (int i = 0; i < n; i++) gathered[i] = srow[xmap[i]];
            from = gathered;
        }
        if (!SrcKey && !DstKey && !orMask) memcpy(drow, from, size_t(n) * sizeof(T));
        else KeyRowAny<T, SrcKey, DstKey>(drow, from, n, j);
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
    if (direct && !j.dstKey && (!j.srcKey || j.skLo == j.skHi) && !j.orMask) {
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

RowPacker g_packer;  // (g_lock held)
std::vector<uint32_t> g_row, g_gathered;

// Any formats, keys, stretching and mirroring, a converting blit's source rows made
// A8R8G8B8 first (RowConverter: the part the columns read). Without keys into a format that
// isn't palettized, a row at a time (RowPacker, the columns gathered first when stretched or
// mirrored); otherwise a pixel at a time.
static void BlitGeneric(const BlitJob& j) {
    const int* xmap = BuildXMap(j);
    const int n = j.dc.right - j.dc.left;
    if (n <= 0) return;
    const int lo = std::min(xmap[0], xmap[n - 1]), span = std::max(xmap[0], xmap[n - 1]) + 1 - lo;
    const bool keys = j.srcKey || j.dstKey;
    const bool rows = j.convert && !keys && !j.df->pal8;
    // (Its table only for a palettized source: built at every blit.)
    const RowConverter conv(*j.sf, j.spal);
    if (j.convert) {
        g_row.resize(size_t(span));
        if (!j.df->pal8) g_packer.Prepare(*j.df);
    }
    // 1:1 columns: the converted row is the destination's, in order.
    const bool direct = !j.mirrorX && j.d0.right - j.d0.left == j.s0.right - j.s0.left;
    if (rows && !direct) g_gathered.resize(size_t(n));
    int lastSy = -1;
    for (int y = j.dc.top; y < j.dc.bottom; y++) {
        uint8_t* drow = j.dst + y * j.dpitch + j.dc.left * j.dbytes;
        const int sy = MapY(j, y);
        if (!keys && sy == lastSy) {  // stretched taller: the row above again
            memcpy(drow, drow - j.dpitch, size_t(n) * j.dbytes);
            continue;
        }
        lastSy = sy;
        const uint8_t* srow = j.src + sy * j.spitch;
        if (j.convert) conv.Run(srow + lo * j.sbytes, g_row.data(), span);
        if (rows) {
            const uint32_t* from = g_row.data();
            if (!direct) {
                for (int i = 0; i < n; i++) g_gathered[size_t(i)] = g_row[size_t(xmap[i] - lo)];
                from = g_gathered.data();
            }
            g_packer.Run(from, drow, n, j.orMask);
            continue;
        }
        for (int i = 0; i < n; i++) {
            uint32_t v = 0;
            if (j.srcKey || !j.convert) {
                v = ReadPx(srow + xmap[i] * j.sbytes, j.sbytes);
                if (j.srcKey && InKey(v & j.skMask, j.skLo, j.skHi)) continue;
            }
            uint8_t* dp = drow + i * j.dbytes;
            if (j.dstKey && !InKey(ReadPx(dp, j.dbytes) & j.dkMask, j.dkLo, j.dkHi)) continue;
            if (j.convert) {
                const uint32_t argb = g_row[size_t(xmap[i] - lo)];
                v = j.df->pal8 ? g_nearest(j.dpal, argb) : g_packer.One(argb);
            }
            WritePx(dp, j.dbytes, v | j.orMask);
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

// BltLocked with a FourCC side (DXT or YUV): each such surface swapped for its decoding
// (A8R8G8B8) for the blit itself, the destination's part the rectangle touches encoded back.
// A plain copy between two surfaces of one YUV format, on whole pixel pairs, is copied as
// it is (a video player's double buffering: no loss).
HRESULT Surface::BltFourCC(RECT* dstRect, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx, bool useClipper,
                           bool destKeyFromSource) {
    static const Format argb = [] {
        DDPIXELFORMAT pf = {sizeof(pf), DDPF_RGB | DDPF_ALPHAPIXELS, 0, {32}, {0x00FF0000}, {0x0000FF00}, {0x000000FF}, {0xFF000000}};
        Format f;
        Format::FromDDPF(pf, &f);
        return f;
    }();
    // Planar (YV12, I420) likewise, by 2x2 block.
    if (src && buf->fmt.Planar() && src->buf->fmt == buf->fmt && !(flags & ~DWORD(DDBLT_WAIT | DDBLT_ASYNC | DDBLT_DONOTWAIT)) &&
        !(useClipper && clipper)) {
        const RECT d = dstRect ? *dstRect : RECT{0, 0, buf->w, buf->h};
        const RECT sr = srcRect ? *srcRect : RECT{0, 0, src->buf->w, src->buf->h};
        const LONG w = d.right - d.left, h = d.bottom - d.top;
        RECT overlap;  // (within one surface: overlapping blocks would copy in the wrong order)
        // (An odd size only up to both edges: the last block goes whole.)
        const bool blocks = (!(w & 1) || (d.right == buf->w && sr.right == src->buf->w)) &&
                            (!(h & 1) || (d.bottom == buf->h && sr.bottom == src->buf->h));
        if (w > 0 && h > 0 && w == sr.right - sr.left && h == sr.bottom - sr.top && !((d.left | d.top | sr.left | sr.top) & 1) &&
            blocks && d.left >= 0 && d.top >= 0 && d.right <= buf->w && d.bottom <= buf->h && sr.left >= 0 && sr.top >= 0 &&
            sr.right <= src->buf->w && sr.bottom <= src->buf->h && (src->buf != buf || (d.left == sr.left && d.top == sr.top) ||
                                                                    !IntersectRect(&overlap, &d, &sr))) {
            src->CpuAccess(false);
            CpuAccess(true, &d);
            CopyPlanar(buf, d, src->buf, sr);
            return DD_OK;
        }
    }
    if (src && buf->fmt.Yuv() && src->buf->fmt == buf->fmt && !(flags & ~DWORD(DDBLT_WAIT | DDBLT_ASYNC | DDBLT_DONOTWAIT)) &&
        !(useClipper && clipper)) {
        const RECT d = dstRect ? *dstRect : RECT{0, 0, buf->w, buf->h};
        const RECT sr = srcRect ? *srcRect : RECT{0, 0, src->buf->w, src->buf->h};
        const LONG w = d.right - d.left, h = d.bottom - d.top;
        // (An odd width only up to both right edges: the last pixel's pair goes whole.)
        const bool pairs = !(w & 1) || (d.right == buf->w && sr.right == src->buf->w);
        if (w > 0 && h > 0 && w == sr.right - sr.left && h == sr.bottom - sr.top && !(d.left & 1) && !(sr.left & 1) && pairs &&
            d.left >= 0 && d.top >= 0 && d.right <= buf->w && d.bottom <= buf->h && sr.left >= 0 && sr.top >= 0 &&
            sr.right <= src->buf->w && sr.bottom <= src->buf->h) {
            src->CpuAccess(false);
            CpuAccess(true, &d);
            const size_t bytes = size_t((w + 1) & ~1) * 2;
            const bool down = src->buf == buf && sr.top < d.top;  // overlapping: rows in the safe order
            for (LONG i = 0; i < h; i++) {
                const LONG y = down ? h - 1 - i : i;
                memmove(buf->Row(d.top + y) + size_t(d.left) * 2, src->buf->Row(sr.top + y) + size_t(sr.left) * 2,
                        std::min<size_t>(bytes, size_t(std::min(buf->pitch - d.left * 2, src->buf->pitch - sr.left * 2))));
            }
            return DD_OK;
        }
    }
    // Each FourCC side decoded into a scratch buffer of its size (kept from blit to blit, g_lock
    // held): only the rows the blit reads or the encoding after it rewrites (whole DXT blocks,
    // whole 2x2 YV12 blocks).
    Buffer* const realDst = buf;
    Buffer* const realSrc = src ? src->buf : nullptr;
    const bool dstConv = realDst->fmt.fourcc, srcConv = realSrc && realSrc->fmt.fourcc;
    const RECT dr = dstRect ? *dstRect : RECT{0, 0, realDst->w, realDst->h};
    const RECT sr = src ? (srcRect ? *srcRect : RECT{0, 0, realSrc->w, realSrc->h}) : RECT{};
    // Colour keys on YUV sides: compared in the surface's own Y, U and V (MarkYuvKey), the
    // decoded pixels' alpha then 0 where the key matched, which the blit's key tests alone
    // (g_skMaskOverride, g_dkMaskOverride). A surface that is both sides with both keys has
    // one decoding for the two: its keys are converted end by end instead (as DXT keys).
    Surface* const keyDst = destKeyFromSource ? src : this;
    auto yuv = [](const Buffer* b) { return b->fmt.Yuv() || b->fmt.Planar(); };
    const bool shared = dstConv && src && (src == this || src->buf == buf);
    const bool srcKeyOn = src && (flags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE)) && (fx || !(flags & DDBLT_KEYSRCOVERRIDE));
    const bool dstKeyOn = keyDst && (flags & (DDBLT_KEYDEST | DDBLT_KEYDESTOVERRIDE)) && (fx || !(flags & DDBLT_KEYDESTOVERRIDE));
    bool markSrc = srcKeyOn && srcConv && yuv(realSrc), markDst = dstKeyOn && dstConv && yuv(realDst);
    if (shared && markSrc && dstKeyOn) markSrc = markDst = false;
    DWORD srcMark[2] = {}, dstMark[2] = {};  // (an override key is one value: BltLocked's rule)
    if (markSrc) {
        const DDCOLORKEY k = (flags & DDBLT_KEYSRCOVERRIDE) ? fx->ddckSrcColorkey : src->ck[kCkSrcBlt];
        srcMark[0] = k.dwColorSpaceLowValue;
        srcMark[1] = (flags & DDBLT_KEYSRCOVERRIDE) ? k.dwColorSpaceLowValue : k.dwColorSpaceHighValue;
    }
    if (markDst) {
        const DDCOLORKEY k = (flags & DDBLT_KEYDESTOVERRIDE) ? fx->ddckDestColorkey : keyDst->ck[kCkDestBlt];
        dstMark[0] = k.dwColorSpaceLowValue;
        dstMark[1] = (flags & DDBLT_KEYDESTOVERRIDE) ? k.dwColorSpaceLowValue : k.dwColorSpaceHighValue;
    }
    // (Only the columns too: a sprite from a big texture atlas decodes its own blocks.)
    auto decode = [&](Surface* s, Buffer* t, std::vector<uint8_t>& mem, LONG top, LONG bottom, LONG left, LONG right,
                      const DWORD* mark) -> bool {
        s->CpuAccess(false);
        const Buffer* b = s->buf;
        t->w = b->w;
        t->h = b->h;
        t->fmt = argb;
        t->pitch = LONG(b->w) * 4;
        const size_t size = size_t(t->pitch) * b->h;
        if (mem.size() < size) {
            try {
                mem.resize(size);
            } catch (const std::bad_alloc&) {
                return false;
            }
        }
        t->bits = mem.data();
        const int align = b->fmt.BlockCompressed() ? 4 : b->fmt.Planar() ? 2 : 1;
        const int y0 = int(std::max<LONG>(top, 0)) / align * align;
        const int y1 = int(std::min<LONG>((std::min<LONG>(bottom, b->h) + align - 1) / align * align, b->h));
        const int across = b->fmt.BlockCompressed() ? 4 : 2;  // (YUV: pairs, 2x2 blocks)
        const int x0 = int(std::max<LONG>(left, 0)) / across * across;
        const int x1 = int(std::min<LONG>((std::min<LONG>(right, b->w) + across - 1) / across * across, b->w));
        if (y0 < y1 && x0 < x1) {
            DecodeFourCC(b, t->bits, t->pitch, y0, y1, x0, x1);
            if (mark) MarkYuvKey(b, t->bits, t->pitch, mark[0], mark[1], y0, y1, x0, x1);
        }
        return true;
    };
    static std::vector<uint8_t> dstMem, srcMem;
    Buffer dstTemp, srcTemp;
    Buffer* td = nullptr;
    Buffer* ts = nullptr;
    if (dstConv) {
        // (Its own source too: the rows of both.)
        const bool both = src == this || (src && src->buf == buf);
        const LONG top = both ? std::min(dr.top, sr.top) : dr.top, bottom = both ? std::max(dr.bottom, sr.bottom) : dr.bottom;
        const LONG left = both ? std::min(dr.left, sr.left) : dr.left, right = both ? std::max(dr.right, sr.right) : dr.right;
        const DWORD* mark = markDst ? dstMark : both && markSrc ? srcMark : nullptr;
        if (!decode(this, &dstTemp, dstMem, top, bottom, left, right, mark)) return DDERR_OUTOFMEMORY;
        td = &dstTemp;
        if (both) ts = td;
    }
    if (srcConv && !ts) {
        if (!decode(src, &srcTemp, srcMem, sr.top, sr.bottom, sr.left, sr.right, markSrc ? srcMark : nullptr))
            return DDERR_OUTOFMEMORY;
        ts = &srcTemp;
    }
    // A source without alpha is opaque (Wine's test_surface_format_conversion_alpha, RGBX
    // into DXT1-3): the pixels the blit writes get alpha 255, the others (keyed, clipped)
    // keep theirs.
    const uint32_t opaque = td && !srcConv && (!realSrc || !realSrc->fmt.a) ? 0xFF000000u : 0;
    // Colour keys of the FourCC sides, as the decoded pixels they match (a source key is in
    // the source's format, a destination key in the destination's): set for the blit, put
    // back after.
    DDBLTFX fxConv{};
    if (fx) fxConv = *fx;
    const DDCOLORKEY savedSrc = src ? src->ck[kCkSrcBlt] : DDCOLORKEY{}, savedDst = keyDst ? keyDst->ck[kCkDestBlt] : DDCOLORKEY{};
    auto conv = [](DWORD fc, DDCOLORKEY k) {
        return DDCOLORKEY{FourCCKeyToArgb(fc, k.dwColorSpaceLowValue), FourCCKeyToArgb(fc, k.dwColorSpaceHighValue)};
    };
    if (markSrc) {  // alpha 0: matched
        src->ck[kCkSrcBlt] = fxConv.ddckSrcColorkey = DDCOLORKEY{0, 0};
        g_skMaskOverride = 0xFF000000u;
    } else if (srcConv) {
        src->ck[kCkSrcBlt] = conv(realSrc->fmt.fourCC, savedSrc);
        fxConv.ddckSrcColorkey = conv(realSrc->fmt.fourCC, fxConv.ddckSrcColorkey);
    }
    if (keyDst && markDst) {
        keyDst->ck[kCkDestBlt] = fxConv.ddckDestColorkey = DDCOLORKEY{0, 0};
        g_dkMaskOverride = 0xFF000000u;
    } else if (keyDst && dstConv) {  // compared with the destination's pixels: in its format, wherever it is kept
        keyDst->ck[kCkDestBlt] = conv(realDst->fmt.fourCC, savedDst);
        fxConv.ddckDestColorkey = conv(realDst->fmt.fourCC, fxConv.ddckDestColorkey);
    }
    if (td) buf = td;
    if (ts) src->buf = ts;
    g_orMask = opaque;
    const HRESULT hr = BltLocked(dstRect, src, srcRect, flags, fx ? &fxConv : nullptr, useClipper, destKeyFromSource);
    g_orMask = 0;
    g_skMaskOverride = g_dkMaskOverride = 0;
    buf = realDst;
    if (src) src->buf = realSrc;
    if (src) src->ck[kCkSrcBlt] = savedSrc;
    if (keyDst) keyDst->ck[kCkDestBlt] = savedDst;
    if (SUCCEEDED(hr) && td) {
        EncodeFourCC(td->bits, td->pitch, realDst, dr);
        CpuAccess(true, &dr);  // (a texture's GPU copy made again from it)
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
    // FourCC surfaces (DXT, YUY2, UYVY): copies (stretched, keyed, mirrored as any) through
    // an A8R8G8B8 decoding of either side, the destination's part encoded again after
    // (BltFourCC). A colour fill of a YUV surface writes the value as it is, as the drivers
    // did (Wine's test_color_fill); other fills of FourCC surfaces aren't done.
    const bool fill = (flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) ||
                      ((flags & DDBLT_ROP) && fx && (fx->dwROP == BLACKNESS || fx->dwROP == WHITENESS));
    if (buf->fmt.fourcc && fill && !(buf->fmt.Yuv() && (flags & DDBLT_COLORFILL))) return DDERR_UNSUPPORTED;
    if ((buf->fmt.fourcc && !fill) || (!fill && src && src->buf->fmt.fourcc))
        return BltFourCC(dstRect, src, srcRect, flags, fx, useClipper, destKeyFromSource);
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
    // A plain copy out of a render target whose newest pixels are on the GPU (a 3D frame
    // blitted to the screen or another render target): done there, without reading it back.
    const DWORD kPlain = DDBLT_WAIT | DDBLT_ASYNC | DDBLT_DONOTWAIT | DDBLT_ROP;
    if (!fill && src && src != this && src->buf != buf && !(flags & ~kPlain) &&
        (!(flags & DDBLT_ROP) || (fx && fx->dwROP == SRCCOPY))) {
        const RECT s = srcRect ? *srcRect : RECT{0, 0, src->buf->w, src->buf->h};
        if (s.right > s.left && s.bottom > s.top && s.left >= 0 && s.top >= 0 && s.right <= src->buf->w &&
            s.bottom <= src->buf->h && GpuBlt(this, d, src, s, clips)) {
            MarkClipsDirty(this, clips);
            return DD_OK;
        }
    }
    // What Direct3D drew into either surface is read back first.
    if (src && src != this) src->CpuAccess(false);
    // A depth fill into a Z buffer DirectDraw can write on the GPU: no read back first (games
    // fill every frame); the GPU's copy gets the fill (GpuDepthFill), or, with writes of the
    // CPU's pending upload, the fill joins them.
    const bool zfill = (flags & DDBLT_DEPTHFILL) && GpuDepthWritable(buf);
    if (!zfill) CpuAccess(true, dstRect ? dstRect : &bounds);  // (the rectangle before clipping: at most that)
    else if (buf->gpuStale) buf->MarkGpuStale(dstRect ? dstRect : &bounds);

    if (flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) {
        if (!fx) return DDERR_INVALIDPARAMS;
        const DWORD value = (flags & DDBLT_COLORFILL) ? fx->dwFillColor : fx->dwFillDepth;
        for (RECT& c : clips) {
            FillCore(&c, value);
            if ((flags & DDBLT_DEPTHFILL) && !(zfill && buf->gpuStale)) GpuDepthFill(this, c, value);  // a Z buffer Direct3D uses
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
    j.dpal = buf->fmt.pal8 ? EffectivePalette() : nullptr;
    j.convert = !SameLayout(sb->fmt, buf->fmt);
    j.orMask = g_orMask;
    // Bump maps (du/dv, not colours) only copy as they are, as the emulation layer's blits.
    if (j.convert && (sb->fmt.bump || buf->fmt.bump)) return DDERR_UNSUPPORTED;
    if (j.convert && j.dpal) g_nearest.Prepare(j.dpal);

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
        j.skMask = g_skMaskOverride ? g_skMaskOverride : sb->fmt.KeyMask();
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
        j.dkMask = g_dkMaskOverride ? g_dkMaskOverride : buf->fmt.KeyMask();
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

    // A plain copy (any pixel size): a row at a time; overlapping in the same memory (a
    // scroll), in the order that reads each row before it is written (one clip rectangle:
    // with more, one could overwrite what another reads).
    const bool plain = !j.convert && j.dbytes == j.sbytes && !j.srcKey && !j.dstKey && !j.mirrorX && !j.mirrorY &&
                       !j.orMask && d.right - d.left == s.right - s.left && d.bottom - d.top == s.bottom - s.top &&
                       (sb != buf || clips.size() == 1);
    if (plain) {
        const bool up = sb == buf && d.top > s.top;
        for (const RECT& c : clips) {
            const size_t bytes = size_t(c.right - c.left) * j.dbytes;
            for (LONG i = 0, h = c.bottom - c.top; i < h; i++) {
                const LONG y = up ? c.bottom - 1 - i : c.top + i;
                memmove(buf->Row(y) + size_t(c.left) * j.dbytes,
                        sb->Row(s.top + (y - d.top)) + size_t(s.left + (c.left - d.left)) * j.sbytes, bytes);
            }
        }
        MarkClipsDirty(this, clips);
        return DD_OK;
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
