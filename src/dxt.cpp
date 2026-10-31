// DXT1-5 and YUY2/UYVY codecs (see dxt.h). DXT: Layout as Buffer::Create makes it: blocks of 4x4 pixels, a block
// row of (width rounded up to 4) / 4 blocks, 8 bytes each for DXT1, 16 for DXT2-5; the
// buffer's pitch is a block row's bytes / 4.
#include "dxt.h"

#include "objects.h"

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

bool DxtFormat(DWORD fourCC) {
    return fourCC == kDxt1 || fourCC == kDxt2 || fourCC == kDxt3 || fourCC == kDxt4 || fourCC == kDxt5;
}

static void DecodeDxt(const Buffer* b, uint8_t* argb, LONG pitch) {
    const DWORD fc = b->fmt.fourCC;
    const int bs = BlockBytes(fc), bw = (b->w + 3) / 4, bh = (b->h + 3) / 4;
    for (int by = 0; by < bh; by++) {
        const uint8_t* row = b->bits + size_t(by) * 4 * size_t(b->pitch);
        for (int bx = 0; bx < bw; bx++) {
            uint32_t px[16];
            DecodeBlock(row + size_t(bx) * bs, fc, px);
            for (int y = 0; y < 4; y++) {
                const int yy = by * 4 + y;
                if (yy >= b->h) break;
                for (int x = 0; x < 4; x++) {
                    const int xx = bx * 4 + x;
                    if (xx < b->w) reinterpret_cast<uint32_t*>(argb + size_t(yy) * pitch)[xx] = px[y * 4 + x];
                }
            }
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

// A pixel pair's byte offsets of Y0, U, Y1, V.
struct YuvLayout {
    int y0, u, y1, v;
};
inline YuvLayout Layout(DWORD fourCC) {
    return fourCC == MAKEFOURCC('U', 'Y', 'V', 'Y') ? YuvLayout{1, 0, 3, 2} : YuvLayout{0, 1, 2, 3};
}

}  // namespace

static void DecodeYuv(const Buffer* b, uint8_t* argb, LONG pitch) {
    const YuvLayout l = Layout(b->fmt.fourCC);
    // A last pixel alone (odd width) whose pair's second half lies past a game's row: grey
    // chroma for it.
    const bool lastAlone = (b->w & 1) && b->pitch < LONG(b->w + 1) * 2;
    for (int y = 0; y < b->h; y++) {
        const uint8_t* row = b->bits + size_t(y) * b->pitch;
        uint32_t* out = reinterpret_cast<uint32_t*>(argb + size_t(y) * pitch);
        for (int x = 0; x < b->w; x += 2) {
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

uint32_t FourCCKeyToArgb(DWORD fourCC, DWORD key) {
    if (fourCC == MAKEFOURCC('Y', 'U', 'Y', '2') || fourCC == MAKEFOURCC('U', 'Y', 'V', 'Y')) {
        const YuvLayout l = Layout(fourCC);
        const uint8_t p[4] = {uint8_t(key), uint8_t(key >> 8), uint8_t(key >> 16), uint8_t(key >> 24)};
        return YuvToArgb(p[l.y0], p[l.u], p[l.v]);
    }
    int r, g, b;
    From565(uint16_t(key), &r, &g, &b);
    return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
}

void DecodeFourCC(const Buffer* b, uint8_t* argb, LONG pitch) {
    if (b->fmt.Yuv()) DecodeYuv(b, argb, pitch);
    else DecodeDxt(b, argb, pitch);
}

void EncodeFourCC(const uint8_t* argb, LONG pitch, Buffer* b, const RECT& r) {
    if (b->fmt.Yuv()) EncodeYuv(argb, pitch, b, r);
    else EncodeDxt(argb, pitch, b, r);
}

}  // namespace dd2d9
