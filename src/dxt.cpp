// DXT1-5 and YUY2/UYVY codecs (see dxt.h). DXT: Layout as Buffer::Create makes it: blocks of 4x4 pixels, a block
// row of (width rounded up to 4) / 4 blocks, 8 bytes each for DXT1, 16 for DXT2-5; the
// buffer's pitch is a block row's bytes / 4.
#include "dxt.h"

#include "objects.h"

#include <emmintrin.h>

#include <algorithm>
#include <cstring>

namespace dd2d9 {

namespace {

constexpr DWORD kDxt1 = MAKEFOURCC('D', 'X', 'T', '1'), kDxt2 = MAKEFOURCC('D', 'X', 'T', '2'),
                kDxt3 = MAKEFOURCC('D', 'X', 'T', '3'), kDxt4 = MAKEFOURCC('D', 'X', 'T', '4'),
                kDxt5 = MAKEFOURCC('D', 'X', 'T', '5');

// R5G6B5 to and from 8-bit channels (the top bits repeated below, as hardware expands them).
inline void From565(uint16_t c, int* r, int* g, int* b) {
    const int r5 = c >> 11, g6 = (c >> 5) & 63, b5 = c & 31;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g6 << 2) | (g6 >> 4);
    *b = (b5 << 3) | (b5 >> 2);
}
inline uint16_t To565(int r, int g, int b) {
    return uint16_t((((r * 31 + 127) / 255) << 11) | (((g * 63 + 127) / 255) << 5) | ((b * 31 + 127) / 255));
}

// The four colours of a colour block (DXT1 with c0 <= c1: three, the fourth transparent black).
void Palette(const uint8_t* blk, bool dxt1, uint32_t pal[4]) {
    const uint16_t c0 = uint16_t(blk[0] | (blk[1] << 8)), c1 = uint16_t(blk[2] | (blk[3] << 8));
    int r[4], g[4], b[4];
    From565(c0, &r[0], &g[0], &b[0]);
    From565(c1, &r[1], &g[1], &b[1]);
    uint32_t a3 = 0xFF000000u;
    if (!dxt1 || c0 > c1) {
        for (int k = 0; k < 3; k++) {
            int* ch[3] = {r, g, b};
            ch[k][2] = (2 * ch[k][0] + ch[k][1] + 1) / 3;
            ch[k][3] = (ch[k][0] + 2 * ch[k][1] + 1) / 3;
        }
    } else {
        r[2] = (r[0] + r[1]) / 2, g[2] = (g[0] + g[1]) / 2, b[2] = (b[0] + b[1]) / 2;
        r[3] = g[3] = b[3] = 0;
        a3 = 0;
    }
    for (int i = 0; i < 4; i++) pal[i] = 0xFF000000u | (uint32_t(r[i]) << 16) | (uint32_t(g[i]) << 8) | uint32_t(b[i]);
    pal[3] = (pal[3] & 0x00FFFFFFu) | a3;
}

void DecodeBlock(const uint8_t* blk, DWORD fourCC, uint32_t px[16]) {
    const bool dxt1 = fourCC == kDxt1;
    const uint8_t* colour = dxt1 ? blk : blk + 8;
    uint32_t pal[4];
    Palette(colour, dxt1, pal);
    const uint32_t idx = uint32_t(colour[4]) | (uint32_t(colour[5]) << 8) | (uint32_t(colour[6]) << 16) | (uint32_t(colour[7]) << 24);
    for (int i = 0; i < 16; i++) px[i] = pal[(idx >> (2 * i)) & 3];
    if (fourCC == kDxt2 || fourCC == kDxt3) {  // explicit 4-bit alpha
        for (int i = 0; i < 16; i++) {
            const uint32_t a4 = (blk[i / 2] >> ((i & 1) * 4)) & 15;
            px[i] = (px[i] & 0x00FFFFFFu) | ((a4 * 17) << 24);
        }
    } else if (fourCC == kDxt4 || fourCC == kDxt5) {  // interpolated alpha
        const int a0 = blk[0], a1 = blk[1];
        int a[8] = {a0, a1};
        if (a0 > a1) {
            for (int k = 1; k < 7; k++) a[k + 1] = ((7 - k) * a0 + k * a1 + 3) / 7;
        } else {
            for (int k = 1; k < 5; k++) a[k + 1] = ((5 - k) * a0 + k * a1 + 2) / 5;
            a[6] = 0;
            a[7] = 255;
        }
        uint64_t bits = 0;
        for (int k = 0; k < 6; k++) bits |= uint64_t(blk[2 + k]) << (8 * k);
        for (int i = 0; i < 16; i++) px[i] = (px[i] & 0x00FFFFFFu) | (uint32_t(a[(bits >> (3 * i)) & 7]) << 24);
    }
}

inline int Dist(uint32_t x, uint32_t y) {
    const int dr = int((x >> 16) & 255) - int((y >> 16) & 255), dg = int((x >> 8) & 255) - int((y >> 8) & 255),
              db = int(x & 255) - int(y & 255);
    return dr * dr + dg * dg + db * db;
}

// A colour block for 16 pixels: the two pixels farthest apart as the endpoints (as the
// encoders of Windows' drivers: two colours come back exactly, as 565, which Wine's
// test_surface_format_conversion_alpha checks), each pixel the nearest of the palette.
// DXT1 with a transparent pixel (alpha below 128): the three-colour mode, index 3 for those.
void EncodeColour(const uint32_t px[16], bool dxt1, uint8_t* out) {
    bool transparent = false;
    int ia = -1, ib = -1, farthest = -1;
    for (int i = 0; i < 16; i++) {
        if (dxt1 && (px[i] >> 24) < 128) {
            transparent = true;
            continue;
        }
        if (ia < 0) ia = ib = i, farthest = 0;
        for (int j = 0; j < i; j++) {
            if (dxt1 && (px[j] >> 24) < 128) continue;
            if (const int dd = Dist(px[i], px[j]); dd > farthest) farthest = dd, ia = i, ib = j;
        }
    }
    int hi[3] = {0, 0, 0}, lo[3] = {0, 0, 0};  // all transparent: black
    if (ia >= 0) {
        for (int k = 0; k < 3; k++) {
            hi[k] = int((px[ia] >> (16 - 8 * k)) & 255);
            lo[k] = int((px[ib] >> (16 - 8 * k)) & 255);
        }
    }
    uint16_t c0 = To565(hi[0], hi[1], hi[2]), c1 = To565(lo[0], lo[1], lo[2]);
    // DXT1: four colours need c0 > c1, three (with transparency) c0 <= c1 (one colour: c0 ==
    // c1, every pixel index 0). DXT2-5 always decode four, whatever the order.
    if (transparent ? c0 > c1 : c0 < c1) std::swap(c0, c1);
    out[0] = uint8_t(c0), out[1] = uint8_t(c0 >> 8), out[2] = uint8_t(c1), out[3] = uint8_t(c1 >> 8);
    uint32_t pal[4];
    Palette(out, dxt1, pal);
    const int colours = (dxt1 && c0 <= c1) ? 3 : 4;
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        int best = 0;
        if (dxt1 && transparent && (px[i] >> 24) < 128) {
            best = 3;
        } else {
            int bd = Dist(px[i], pal[0]);
            for (int k = 1; k < colours; k++) {
                const int dd = Dist(px[i], pal[k]);
                if (dd < bd) bd = dd, best = k;
            }
        }
        idx |= uint32_t(best) << (2 * i);
    }
    out[4] = uint8_t(idx), out[5] = uint8_t(idx >> 8), out[6] = uint8_t(idx >> 16), out[7] = uint8_t(idx >> 24);
}

void EncodeBlock(const uint32_t px[16], DWORD fourCC, uint8_t* out) {
    if (fourCC == kDxt1) {
        EncodeColour(px, true, out);
        return;
    }
    if (fourCC == kDxt2 || fourCC == kDxt3) {
        memset(out, 0, 8);
        for (int i = 0; i < 16; i++) out[i / 2] |= uint8_t((((px[i] >> 24) * 15 + 127) / 255) << ((i & 1) * 4));
    } else {
        int a0 = 0, a1 = 255;
        for (int i = 0; i < 16; i++) a0 = std::max(a0, int(px[i] >> 24)), a1 = std::min(a1, int(px[i] >> 24));
        int a[8] = {a0, a1};
        if (a0 > a1) {
            for (int k = 1; k < 7; k++) a[k + 1] = ((7 - k) * a0 + k * a1 + 3) / 7;
        } else {
            for (int k = 2; k < 8; k++) a[k] = a0;  // one alpha: every index gives it
        }
        uint64_t bits = 0;
        for (int i = 0; i < 16; i++) {
            const int v = int(px[i] >> 24);
            int best = 0, bd = 256;
            for (int k = 0; k < 8; k++)
                if (std::abs(a[k] - v) < bd) bd = std::abs(a[k] - v), best = k;
            bits |= uint64_t(best) << (3 * i);
        }
        out[0] = uint8_t(a0), out[1] = uint8_t(a1);
        for (int k = 0; k < 6; k++) out[2 + k] = uint8_t(bits >> (8 * k));
    }
    EncodeColour(px, false, out + 8);
}

inline int BlockBytes(DWORD fourCC) { return fourCC == kDxt1 ? 8 : 16; }

}  // namespace

// Rows y0..y1 (multiples of 4, or y1 the height), the blocks over columns x0..x1.
static void DecodeDxt(const Buffer* b, uint8_t* argb, LONG pitch, int y0, int y1, int x0, int x1) {
    const DWORD fc = b->fmt.fourCC;
    const int bs = BlockBytes(fc), bx1 = (x1 + 3) / 4;
    for (int by = y0 / 4; by * 4 < y1; by++) {
        const uint8_t* row = b->bits + size_t(by) * 4 * size_t(b->pitch);
        const int rows = std::min(4, y1 - by * 4);
        for (int bx = x0 / 4; bx < bx1; bx++) {
            uint32_t px[16];
            DecodeBlock(row + size_t(bx) * bs, fc, px);
            const int cols = std::min(4, b->w - bx * 4);
            for (int y = 0; y < rows; y++)
                memcpy(argb + size_t(by * 4 + y) * pitch + size_t(bx) * 16, px + y * 4, size_t(cols) * 4);
        }
    }
}

static void EncodeDxt(const uint8_t* argb, LONG pitch, Buffer* b, const RECT& r) {
    const DWORD fc = b->fmt.fourCC;
    const int bs = BlockBytes(fc);
    const int bx0 = std::max<LONG>(r.left, 0) / 4, by0 = std::max<LONG>(r.top, 0) / 4;
    const int bx1 = (std::min<LONG>(r.right, b->w) + 3) / 4, by1 = (std::min<LONG>(r.bottom, b->h) + 3) / 4;
    for (int by = by0; by < by1; by++) {
        uint8_t* row = b->bits + size_t(by) * 4 * size_t(b->pitch);
        for (int bx = bx0; bx < bx1; bx++) {
            uint32_t px[16];
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    const int xx = std::min(bx * 4 + x, b->w - 1), yy = std::min(by * 4 + y, b->h - 1);
                    px[y * 4 + x] = reinterpret_cast<const uint32_t*>(argb + size_t(yy) * pitch)[xx];
                }
            EncodeBlock(px, fc, row + size_t(bx) * bs);
        }
    }
}

// ---- YUY2 (Y0 U Y1 V) and UYVY (U Y0 V Y1): ITU-R BT.601 with video levels (Y 16-235,
// chroma 16-240), as video hardware converts them.

namespace {

inline uint8_t Clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

inline uint32_t YuvToArgb(int y, int u, int v) {
    const int c = 298 * (y - 16), d = u - 128, e = v - 128;
    const int r = (c + 409 * e + 128) >> 8, g = (c - 100 * d - 208 * e + 128) >> 8, b = (c + 516 * d + 128) >> 8;
    return 0xFF000000u | (uint32_t(Clamp8(r)) << 16) | (uint32_t(Clamp8(g)) << 8) | Clamp8(b);
}

// Eight pixels at once, exactly as YuvToArgb: y, u, v (16-bit lanes, 0-255) into eight
// A8R8G8B8 at `out`. The sums in 32 bits through pmaddwd (pairs of 16-bit products), shifted,
// then saturated to 0-255 as Clamp8 does.
struct Sum4x2 {  // eight 32-bit sums: pixels 0-3, 4-7
    __m128i lo, hi;
};
// a * k.low16 + b * k.high16 for each of the eight pixels (16-bit lanes in, 32-bit sums out).
inline Sum4x2 MulAdd(__m128i a, __m128i b, __m128i k) {
    return {_mm_madd_epi16(_mm_unpacklo_epi16(a, b), k), _mm_madd_epi16(_mm_unpackhi_epi16(a, b), k)};
}
inline Sum4x2 Add(Sum4x2 a, __m128i v) { return {_mm_add_epi32(a.lo, v), _mm_add_epi32(a.hi, v)}; }
inline Sum4x2 Add(Sum4x2 a, Sum4x2 b) { return {_mm_add_epi32(a.lo, b.lo), _mm_add_epi32(a.hi, b.hi)}; }
inline __m128i Shift8(Sum4x2 s) { return _mm_packs_epi32(_mm_srai_epi32(s.lo, 8), _mm_srai_epi32(s.hi, 8)); }
inline __m128i K(int lowFactor, int highFactor) {
    return _mm_set1_epi32(int((uint32_t(uint16_t(highFactor)) << 16) | uint16_t(lowFactor)));
}

inline void YuvToArgb8(__m128i y, __m128i u, __m128i v, uint8_t* out) {
    const __m128i c = _mm_sub_epi16(y, _mm_set1_epi16(16)), d = _mm_sub_epi16(u, _mm_set1_epi16(128)),
                  e = _mm_sub_epi16(v, _mm_set1_epi16(128)), one = _mm_set1_epi16(1), round = _mm_set1_epi32(128);
    const __m128i r = Shift8(Add(MulAdd(c, e, K(298, 409)), round));
    const __m128i g = Shift8(Add(MulAdd(c, d, K(298, -100)), MulAdd(e, one, K(-208, 128))));  // (the 128 rounding there)
    const __m128i b = Shift8(Add(MulAdd(c, d, K(298, 516)), round));
    const __m128i b8 = _mm_packus_epi16(b, b), g8 = _mm_packus_epi16(g, g), r8 = _mm_packus_epi16(r, r);
    const __m128i bg = _mm_unpacklo_epi8(b8, g8), ra = _mm_unpacklo_epi8(r8, _mm_set1_epi8(char(0xFF)));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out), _mm_unpacklo_epi16(bg, ra));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16), _mm_unpackhi_epi16(bg, ra));
}

// A pixel pair's byte offsets of Y0, U, Y1, V.
struct YuvLayout {
    int y0, u, y1, v;
};
inline YuvLayout Layout(DWORD fourCC) {
    return fourCC == MAKEFOURCC('U', 'Y', 'V', 'Y') ? YuvLayout{1, 0, 3, 2} : YuvLayout{0, 1, 2, 3};
}

}  // namespace

static void DecodeYuv(const Buffer* b, uint8_t* argb, LONG pitch, int y0, int y1, int x0, int x1) {
    const YuvLayout l = Layout(b->fmt.fourCC);
    // A last pixel alone (odd width) whose pair's second half lies past a game's row: grey
    // chroma for it.
    const bool lastAlone = (b->w & 1) && b->pitch < LONG(b->w + 1) * 2;
    const bool uyvy = b->fmt.fourCC == MAKEFOURCC('U', 'Y', 'V', 'Y');
    const __m128i lowBytes = _mm_set1_epi16(0x00FF);
    for (int y = y0; y < y1; y++) {
        const uint8_t* row = b->bits + size_t(y) * b->pitch;
        uint32_t* out = reinterpret_cast<uint32_t*>(argb + size_t(y) * pitch);
        // Eight pixels (four pairs, 16 bytes) at a time: Y in the even bytes (YUY2) or odd
        // (UYVY), the chroma pairs in the others, each pair's U and V spread over its pixels.
        int x = x0 & ~1;  // (whole pairs)
        for (; x + 8 <= x1; x += 8) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + size_t(x) * 2));
            const __m128i ys = uyvy ? _mm_srli_epi16(v, 8) : _mm_and_si128(v, lowBytes);
            const __m128i uv = uyvy ? _mm_and_si128(v, lowBytes) : _mm_srli_epi16(v, 8);  // U0 V0 U1 V1 ...
            const __m128i us = _mm_shufflehi_epi16(_mm_shufflelo_epi16(uv, _MM_SHUFFLE(2, 2, 0, 0)), _MM_SHUFFLE(2, 2, 0, 0));
            const __m128i vs = _mm_shufflehi_epi16(_mm_shufflelo_epi16(uv, _MM_SHUFFLE(3, 3, 1, 1)), _MM_SHUFFLE(3, 3, 1, 1));
            YuvToArgb8(ys, us, vs, reinterpret_cast<uint8_t*>(out + x));
        }
        for (; x < x1; x += 2) {
            const uint8_t* p = row + size_t(x) * 2;
            if (x + 1 >= b->w && lastAlone) {
                out[x] = YuvToArgb(p[l.y0], 128, 128);  // (its Y lies in the first two bytes either way)
                continue;
            }
            out[x] = YuvToArgb(p[l.y0], p[l.u], p[l.v]);
            if (x + 1 < b->w) out[x + 1] = YuvToArgb(p[l.y1], p[l.u], p[l.v]);
        }
    }
}

static void EncodeYuv(const uint8_t* argb, LONG pitch, Buffer* b, const RECT& r) {
    const YuvLayout l = Layout(b->fmt.fourCC);
    const bool lastAlone = (b->w & 1) && b->pitch < LONG(b->w + 1) * 2;
    const int x0 = std::max<LONG>(r.left, 0) & ~1, x1 = std::min<LONG>(r.right, b->w);
    const int y0 = std::max<LONG>(r.top, 0), y1 = std::min<LONG>(r.bottom, b->h);
    auto luma = [](uint32_t c, int* y, int* u, int* v) {
        const int R = int((c >> 16) & 255), G = int((c >> 8) & 255), B = int(c & 255);
        *y = ((66 * R + 129 * G + 25 * B + 128) >> 8) + 16;
        *u = ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
        *v = ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
    };
    for (int y = y0; y < y1; y++) {
        const uint32_t* in = reinterpret_cast<const uint32_t*>(argb + size_t(y) * pitch);
        uint8_t* row = b->bits + size_t(y) * b->pitch;
        for (int x = x0; x < x1; x += 2) {
            uint8_t* p = row + size_t(x) * 2;
            int ya, ua, va, yb, ub, vb;
            luma(in[x], &ya, &ua, &va);
            if (x + 1 >= b->w) {
                if (lastAlone) {
                    p[l.y0] = Clamp8(ya);
                    continue;
                }
                yb = ya, ub = ua, vb = va;
            } else {
                luma(in[x + 1], &yb, &ub, &vb);
            }
            p[l.y0] = Clamp8(ya);
            p[l.y1] = Clamp8(yb);
            p[l.u] = Clamp8((ua + ub + 1) / 2);
            p[l.v] = Clamp8((va + vb + 1) / 2);
        }
    }
}

// YV12 / I420: the luma plane, then the chroma planes (half the pitch, half the rows rounded
// up): V first in YV12, U first in I420 (IYUV).
struct Planes {
    uint8_t *y, *u, *v;
    LONG cp;  // the chroma planes' pitch
};
static Planes PlanesOf(const Buffer* b) {
    Planes p;
    p.cp = b->pitch / 2;
    p.y = b->bits;
    uint8_t* first = b->bits + size_t(b->pitch) * b->h;
    uint8_t* second = first + size_t(p.cp) * ((b->h + 1) / 2);
    const bool yv12 = b->fmt.fourCC == MAKEFOURCC('Y', 'V', '1', '2');
    p.v = yv12 ? first : second;
    p.u = yv12 ? second : first;
    return p;
}

static void DecodePlanar(const Buffer* b, uint8_t* argb, LONG pitch, int y0, int y1, int x0, int x1) {
    const Planes p = PlanesOf(b);
    for (int y = y0; y < y1; y++) {
        const uint8_t* yr = p.y + size_t(y) * b->pitch;
        const uint8_t* ur = p.u + size_t(y / 2) * p.cp;
        const uint8_t* vr = p.v + size_t(y / 2) * p.cp;
        uint32_t* out = reinterpret_cast<uint32_t*>(argb + size_t(y) * pitch);
        // Eight pixels (four chroma samples, each over two of them) at a time.
        int x = x0 & ~1;  // (even: a chroma sample's first pixel)
        const __m128i zero = _mm_setzero_si128();
        for (; x + 8 <= x1; x += 8) {
            const __m128i ys = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(yr + x)), zero);
            uint32_t u4, v4;
            memcpy(&u4, ur + x / 2, 4);
            memcpy(&v4, vr + x / 2, 4);
            const __m128i u = _mm_unpacklo_epi8(_mm_cvtsi32_si128(int(u4)), zero), v = _mm_unpacklo_epi8(_mm_cvtsi32_si128(int(v4)), zero);
            YuvToArgb8(ys, _mm_unpacklo_epi16(u, u), _mm_unpacklo_epi16(v, v), reinterpret_cast<uint8_t*>(out + x));
        }
        for (; x < x1; x++) out[x] = YuvToArgb(yr[x], ur[x / 2], vr[x / 2]);
    }
}

// The 2x2 blocks `r` touches: luma per pixel, chroma the block's average (its pixels inside
// the surface).
static void EncodePlanar(const uint8_t* argb, LONG pitch, Buffer* b, const RECT& r) {
    const Planes p = PlanesOf(b);
    const int x0 = std::max<LONG>(r.left, 0) & ~1, x1 = std::min<LONG>(r.right, b->w);
    const int y0 = std::max<LONG>(r.top, 0) & ~1, y1 = std::min<LONG>(r.bottom, b->h);
    auto px = [&](int x, int y) { return reinterpret_cast<const uint32_t*>(argb + size_t(y) * pitch)[x]; };
    for (int y = y0; y < y1; y += 2) {
        for (int x = x0; x < x1; x += 2) {
            int us = 0, vs = 0, n = 0;
            for (int dy = 0; dy < 2 && y + dy < b->h; dy++)
                for (int dx = 0; dx < 2 && x + dx < b->w; dx++) {
                    const uint32_t c = px(x + dx, y + dy);
                    const int R = int((c >> 16) & 255), G = int((c >> 8) & 255), B = int(c & 255);
                    p.y[size_t(y + dy) * b->pitch + x + dx] = Clamp8(((66 * R + 129 * G + 25 * B + 128) >> 8) + 16);
                    us += ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
                    vs += ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
                    n++;
                }
            p.u[size_t(y / 2) * p.cp + x / 2] = Clamp8((us + n / 2) / n);
            p.v[size_t(y / 2) * p.cp + x / 2] = Clamp8((vs + n / 2) / n);
        }
    }
}

void CopyPlanar(Buffer* dst, const RECT& d, const Buffer* src, const RECT& s) {
    const Planes dp = PlanesOf(dst), sp = PlanesOf(src);
    const LONG w = d.right - d.left, h = d.bottom - d.top;
    for (LONG y = 0; y < h; y++)
        memmove(dp.y + size_t(d.top + y) * dst->pitch + d.left, sp.y + size_t(s.top + y) * src->pitch + s.left, size_t(w));
    const LONG cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (LONG y = 0; y < ch; y++) {
        memmove(dp.u + size_t(d.top / 2 + y) * dp.cp + d.left / 2, sp.u + size_t(s.top / 2 + y) * sp.cp + s.left / 2, size_t(cw));
        memmove(dp.v + size_t(d.top / 2 + y) * dp.cp + d.left / 2, sp.v + size_t(s.top / 2 + y) * sp.cp + s.left / 2, size_t(cw));
    }
}

uint32_t FourCCKeyToArgb(DWORD fourCC, DWORD key) {
    if (fourCC == MAKEFOURCC('Y', 'V', '1', '2') || fourCC == MAKEFOURCC('I', '4', '2', '0') ||
        fourCC == MAKEFOURCC('I', 'Y', 'U', 'V'))
        return YuvToArgb(int(key & 255), int((key >> 8) & 255), int((key >> 16) & 255));  // Y, U, V bytes
    if (fourCC == MAKEFOURCC('Y', 'U', 'Y', '2') || fourCC == MAKEFOURCC('U', 'Y', 'V', 'Y')) {
        const YuvLayout l = Layout(fourCC);
        const uint8_t p[4] = {uint8_t(key), uint8_t(key >> 8), uint8_t(key >> 16), uint8_t(key >> 24)};
        return YuvToArgb(p[l.y0], p[l.u], p[l.v]);
    }
    int r, g, b;
    From565(uint16_t(key), &r, &g, &b);
    return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

// The Y, U and V a YUV key names (as FourCCKeyToArgb reads it).
static void KeyYuv(DWORD fourCC, DWORD key, int* y, int* u, int* v) {
    if (fourCC == MAKEFOURCC('Y', 'U', 'Y', '2') || fourCC == MAKEFOURCC('U', 'Y', 'V', 'Y')) {
        const YuvLayout l = Layout(fourCC);
        const uint8_t p[4] = {uint8_t(key), uint8_t(key >> 8), uint8_t(key >> 16), uint8_t(key >> 24)};
        *y = p[l.y0], *u = p[l.u], *v = p[l.v];
    } else {
        *y = int(key & 255), *u = int((key >> 8) & 255), *v = int((key >> 16) & 255);
    }
}

void MarkYuvKey(const Buffer* b, uint8_t* argb, LONG pitch, DWORD lo, DWORD hi, int y0, int y1, int x0, int x1) {
    int ly, lu, lv, hy, hu, hv;
    KeyYuv(b->fmt.fourCC, lo, &ly, &lu, &lv);
    KeyYuv(b->fmt.fourCC, hi, &hy, &hu, &hv);
    hy = std::max(hy, ly), hu = std::max(hu, lu), hv = std::max(hv, lv);
    y0 = std::max(y0, 0), y1 = std::min(y1, b->h), x0 = std::max(x0, 0), x1 = std::min(x1, b->w);
    auto in = [&](int y, int u, int v) { return y >= ly && y <= hy && u >= lu && u <= hu && v >= lv && v <= hv; };
    auto set = [](uint32_t* p, bool keyed) { *p = (*p & 0x00FFFFFFu) | (keyed ? 0u : 0xFF000000u); };
    if (b->fmt.Planar()) {
        const Planes p = PlanesOf(b);
        for (int y = y0; y < y1; y++) {
            const uint8_t* yr = p.y + size_t(y) * b->pitch;
            const uint8_t* ur = p.u + size_t(y / 2) * p.cp;
            const uint8_t* vr = p.v + size_t(y / 2) * p.cp;
            uint32_t* out = reinterpret_cast<uint32_t*>(argb + size_t(y) * pitch);
            for (int x = x0; x < x1; x++) set(out + x, in(yr[x], ur[x / 2], vr[x / 2]));
        }
        return;
    }
    const YuvLayout l = Layout(b->fmt.fourCC);
    const bool lastAlone = (b->w & 1) && b->pitch < LONG(b->w + 1) * 2;  // (as DecodeYuv: grey chroma)
    for (int y = y0; y < y1; y++) {
        const uint8_t* row = b->bits + size_t(y) * b->pitch;
        uint32_t* out = reinterpret_cast<uint32_t*>(argb + size_t(y) * pitch);
        for (int x = x0; x < x1; x++) {
            const uint8_t* p = row + size_t(x & ~1) * 2;
            const int yy = (x & 1) ? p[l.y1] : p[l.y0];
            const bool alone = (x | 1) >= b->w && lastAlone;
            set(out + x, alone ? in(p[l.y0], 128, 128) : in(yy, p[l.u], p[l.v]));
        }
    }
}

void DecodeFourCC(const Buffer* b, uint8_t* argb, LONG pitch, int y0, int y1, int x0, int x1) {
    y0 = std::max(y0, 0);
    y1 = std::min(y1, b->h);
    x0 = std::max(x0, 0);
    x1 = std::min(x1, b->w);
    if (x0 >= x1) return;
    if (b->fmt.Yuv()) DecodeYuv(b, argb, pitch, y0, y1, x0, x1);
    else if (b->fmt.Planar()) DecodePlanar(b, argb, pitch, y0, y1, x0, x1);
    else DecodeDxt(b, argb, pitch, y0 & ~3, y1, x0, x1);
}

void EncodeFourCC(const uint8_t* argb, LONG pitch, Buffer* b, const RECT& r) {
    if (b->fmt.Yuv()) EncodeYuv(argb, pitch, b, r);
    else if (b->fmt.Planar()) EncodePlanar(argb, pitch, b, r);
    else EncodeDxt(argb, pitch, b, r);
}

}  // namespace dd2d9
