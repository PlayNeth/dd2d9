// The software blitter's pixels: copies with any formats, keys, stretching and mirroring
// (DrawBlit), and colour fills (Surface::FillCore).
#include "blitint.h"
#include "convert.h"

#include <emmintrin.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace dd2d9 {

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
    uint32_t orMask;           // set on every pixel written
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
std::vector<int> g_xmap;
std::vector<uint8_t> g_temp;
std::vector<uint8_t> g_keyRow;  // a stretched / mirrored keyed row's source pixels, gathered

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

// A stretched or mirrored copy without keys or the OR mask: each row's source columns gathered
// straight into the destination row (out of line, as KeyRows; kept apart from it, which the
// keyed sprites' speed depends on), a row repeated where the stretch makes it taller.
template <typename T>
__attribute__((noinline)) static void StretchRows(const BlitJob& j, const int* __restrict xmap) {
    const int n = j.dc.right - j.dc.left;
    int lastSy = -1;
    for (int y = j.dc.top; y < j.dc.bottom; y++) {
        T* __restrict drow = reinterpret_cast<T*>(j.dst + y * j.dpitch) + j.dc.left;
        const int sy = MapY(j, y);
        if (sy == lastSy) {
            memcpy(drow, reinterpret_cast<const uint8_t*>(drow) - j.dpitch, size_t(n) * sizeof(T));
            continue;
        }
        lastSy = sy;
        // (drow never overlaps the source: an overlapping blit's source is copied aside first.)
        const T* __restrict srow = reinterpret_cast<const T*>(j.src + sy * j.spitch);
        for (int i = 0; i < n; i++) drow[i] = srow[xmap[i]];
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
    } else if (!j.srcKey && !j.dstKey && !j.orMask) {
        StretchRows<T>(j, BuildXMap(j));
    } else {
        KeyRowsDispatch<T, false>(j, BuildXMap(j));
    }
}

RowPacker g_packer;  // (g_lock held)
std::vector<uint32_t> g_row, g_gathered;

// A converting blit's source rows to A8R8G8B8, kept from blit to blit (g_lock held): made
// again only for another format or other colours. (A palettized format's table of 256
// conversions took longer than a small sprite's pixels when it was made for every blit.)
class ConverterCache {
public:
    const RowConverter& For(const Format& f, const PALETTEENTRY* pal) {
        const bool colours = f.pal8 && pal;
        if (!conv_ || !(f == fmt_) || colours != colours_ || (colours && memcmp(pal_, pal, sizeof(pal_)))) {
            fmt_ = f;
            colours_ = colours;
            if (colours) memcpy(pal_, pal, sizeof(pal_));
            conv_.emplace(f, colours ? pal_ : nullptr);
        }
        return *conv_;
    }

private:
    std::optional<RowConverter> conv_;
    Format fmt_;
    bool colours_ = false;
    PALETTEENTRY pal_[256];
};
ConverterCache g_converter;

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
    const RowConverter* conv = nullptr;
    if (j.convert) {
        conv = &g_converter.For(*j.sf, j.spal);
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
        if (j.convert) conv->Run(srow + lo * j.sbytes, g_row.data(), span);
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


}  // namespace

bool SameLayout(const Format& a, const Format& b) {
    if (a.bpp != b.bpp || a.pal8 != b.pal8 || a.zbuf != b.zbuf) return false;
    if (a.pal8 || a.zbuf) return true;
    return a.r == b.r && a.g == b.g && a.b == b.b;  // alpha vs. X bits copy unchanged
}

void DrawBlit(Buffer* db, const RECT& d, const Buffer* sb, const RECT& s, const BlitParams& p,
              const std::vector<RECT>& clips) {
    BlitJob j = {};
    j.dst = db->bits;
    j.dpitch = db->pitch;
    j.src = sb->bits;
    j.spitch = sb->pitch;
    j.dbytes = int(db->fmt.Bytes());
    j.sbytes = int(sb->fmt.Bytes());
    j.d0 = d;
    j.s0 = s;
    j.sf = &sb->fmt;
    j.df = &db->fmt;
    j.spal = p.spal;
    j.dpal = p.dpal;
    j.convert = !SameLayout(sb->fmt, db->fmt);
    j.orMask = p.orMask;
    j.mirrorX = p.mirrorX;
    j.mirrorY = p.mirrorY;
    j.srcKey = p.srcKey.on;
    j.skLo = p.srcKey.lo, j.skHi = p.srcKey.hi, j.skMask = p.srcKey.mask;
    j.dstKey = p.dstKey.on;
    j.dkLo = p.dstKey.lo, j.dkHi = p.dstKey.hi, j.dkMask = p.dstKey.mask;
    if (j.convert && j.dpal) g_nearest.Prepare(j.dpal);

    // A plain copy (any pixel size): a row at a time; overlapping in the same memory (a
    // scroll), in the order that reads each row before it is written (one clip rectangle:
    // with more, one could overwrite what another reads).
    const bool plain = !j.convert && j.dbytes == j.sbytes && !j.srcKey && !j.dstKey && !j.mirrorX && !j.mirrorY &&
                       !j.orMask && d.right - d.left == s.right - s.left && d.bottom - d.top == s.bottom - s.top &&
                       (sb != db || clips.size() == 1);
    if (plain) {
        const bool up = sb == db && d.top > s.top;
        for (const RECT& c : clips) {
            const size_t bytes = size_t(c.right - c.left) * j.dbytes;
            for (LONG i = 0, h = c.bottom - c.top; i < h; i++) {
                const LONG y = up ? c.bottom - 1 - i : c.top + i;
                memmove(db->Row(y) + size_t(c.left) * j.dbytes,
                        sb->Row(s.top + (y - d.top)) + size_t(s.left + (c.left - d.left)) * j.sbytes, bytes);
            }
        }
        return;
    }
    // Copy the source rectangle first when it overlaps the destination in the same memory.
    if (sb == db) {
        RECT ov;
        if (IntersectRect(&ov, &s, &d)) {
            const LONG sw = s.right - s.left, sh = s.bottom - s.top;
            const size_t row = size_t(sw) * j.sbytes;
            std::vector<uint8_t>& temp = g_temp;
            temp.resize(row * size_t(sh));
            for (LONG y = 0; y < sh; y++) memcpy(temp.data() + row * y, sb->Row(s.top + y) + size_t(s.left) * j.sbytes, row);
            j.src = temp.data();
            j.spitch = LONG(row);
            j.s0 = {0, 0, sw, sh};
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
}

}  // namespace dd2d9
