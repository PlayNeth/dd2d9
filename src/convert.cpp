// See convert.h.
#include "convert.h"

#include <emmintrin.h>

#include <cstring>

namespace dd2d9 {

namespace {

// A channel mask as (shift, width), or false if it isn't one SSE2 path below handles.
bool ChannelOf(uint32_t mask, int* shift, int* bits) {
    if (!mask) {
        *shift = *bits = 0;
        return true;
    }
    *shift = __builtin_ctz(mask);
    const uint32_t m = mask >> *shift;
    if (m & (m + 1)) return false;  // not contiguous
    *bits = __builtin_popcount(m);
    return *bits == 1 || *bits == 4 || *bits == 5 || *bits == 6;
}

// c (width `bits`, in 16-bit lanes) scaled to 0..255 with Extract8's rounding: c*255/max,
// rounded to nearest. 5 bits: (c*527 + 23) >> 6; 6 bits: (c*259 + 33) >> 6; 4: c*17; 1: c*255
// (all exact, checked against Extract8 for every value).
inline __m128i Scale(__m128i c, int bits) {
    switch (bits) {
    case 5: return _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(c, _mm_set1_epi16(527)), _mm_set1_epi16(23)), 6);
    case 6: return _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(c, _mm_set1_epi16(259)), _mm_set1_epi16(33)), 6);
    case 4: return _mm_mullo_epi16(c, _mm_set1_epi16(17));
    case 1: return _mm_mullo_epi16(c, _mm_set1_epi16(255));
    default: return _mm_set1_epi16(255);  // absent alpha
    }
}

inline __m128i Field(__m128i v, int shift, int bits) {
    if (!bits) return _mm_setzero_si128();
    return _mm_and_si128(_mm_srl_epi16(v, _mm_cvtsi32_si128(shift)), _mm_set1_epi16(short((1 << bits) - 1)));
}

}  // namespace

RowConverter::RowConverter(const Format& f, const PALETTEENTRY* pal, bool keyed, DWORD key)
    : f_(f), pal_(pal), keyed_(keyed), key_(key), keyMask_(f.KeyMask()) {
    if (f.fourcc || f.zbuf) return;
    if (f.pal8) {
        kind_ = kPal8;
        for (uint32_t i = 0; i < 256; i++) lut_[i] = One(i);
        return;
    }
    if (f.bpp == 16) {
        const uint32_t masks[4] = {f.r, f.g, f.b, f.a};
        bool ok = true;
        for (int c = 0; c < 4 && ok; c++) ok = ChannelOf(masks[c], &ch_[c].shift, &ch_[c].bits);
        if (ok && ch_[0].bits && ch_[1].bits && ch_[2].bits) kind_ = k16;
        return;
    }
    if (f.bpp == 24 && f.r == 0xFF0000 && f.g == 0xFF00 && f.b == 0xFF && !f.a && !keyed) kind_ = k24;
    if (f.bpp == 32 && f.r == 0xFF0000 && f.g == 0xFF00 && f.b == 0xFF && (!f.a || f.a == 0xFF000000u)) kind_ = k32;
}

uint32_t RowConverter::One(uint32_t v) const {
    if (keyed_ && (v & keyMask_) == (key_ & keyMask_)) return 0;
    return PixelToARGB(f_, v, pal_);
}

void RowConverter::Run(const uint8_t* src, uint32_t* dst, int n) const {
    switch (kind_) {
    case kPal8:
        for (int i = 0; i < n; i++) dst[i] = lut_[src[i]];
        return;
    case k16: Run16(reinterpret_cast<const uint16_t*>(src), dst, n); return;
    case k24: Run24(src, dst, n); return;
    case k32: Run32(reinterpret_cast<const uint32_t*>(src), dst, n); return;
    default: {
        const DWORD bytes = f_.Bytes();
        for (int i = 0; i < n; i++, src += bytes) {
            uint32_t v;
            switch (bytes) {
            case 1: v = src[0]; break;
            case 2: v = *reinterpret_cast<const uint16_t*>(src); break;
            case 3: v = uint32_t(src[0]) | (uint32_t(src[1]) << 8) | (uint32_t(src[2]) << 16); break;
            default: v = *reinterpret_cast<const uint32_t*>(src); break;
            }
            dst[i] = One(v);
        }
        return;
    }
    }
}

void RowConverter::Run16(const uint16_t* src, uint32_t* dst, int n) const {
    int i = 0;
    const __m128i keyMask = _mm_set1_epi16(short(keyMask_)), key = _mm_set1_epi16(short(key_ & keyMask_));
    for (; i + 8 <= n; i += 8) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        const __m128i r = Scale(Field(v, ch_[0].shift, ch_[0].bits), ch_[0].bits);
        const __m128i g = Scale(Field(v, ch_[1].shift, ch_[1].bits), ch_[1].bits);
        const __m128i b = Scale(Field(v, ch_[2].shift, ch_[2].bits), ch_[2].bits);
        const __m128i a = Scale(Field(v, ch_[3].shift, ch_[3].bits), ch_[3].bits);
        const __m128i bg = _mm_or_si128(b, _mm_slli_epi16(g, 8));  // bytes b, g
        const __m128i ra = _mm_or_si128(r, _mm_slli_epi16(a, 8));  // bytes r, a
        __m128i lo = _mm_unpacklo_epi16(bg, ra);                    // 4 pixels: b, g, r, a = A8R8G8B8
        __m128i hi = _mm_unpackhi_epi16(bg, ra);
        if (keyed_) {
            const __m128i k = _mm_cmpeq_epi16(_mm_and_si128(v, keyMask), key);  // 0xFFFF where keyed
            lo = _mm_andnot_si128(_mm_unpacklo_epi16(k, k), lo);
            hi = _mm_andnot_si128(_mm_unpackhi_epi16(k, k), hi);
        }
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), lo);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 4), hi);
    }
    for (; i < n; i++) dst[i] = One(src[i]);
}

void RowConverter::Run24(const uint8_t* src, uint32_t* dst, int n) const {
    int i = 0;
    for (; i + 4 <= n; i += 4, src += 12) {
        uint32_t w[3];
        memcpy(w, src, 12);
        dst[i] = w[0] | 0xFF000000u;
        dst[i + 1] = (w[0] >> 24) | (w[1] << 8) | 0xFF000000u;
        dst[i + 2] = (w[1] >> 16) | (w[2] << 16) | 0xFF000000u;
        dst[i + 3] = (w[2] >> 8) | 0xFF000000u;
    }
    for (; i < n; i++, src += 3) dst[i] = 0xFF000000u | uint32_t(src[0]) | (uint32_t(src[1]) << 8) | (uint32_t(src[2]) << 16);
}

void RowConverter::Run32(const uint32_t* src, uint32_t* dst, int n) const {
    int i = 0;
    const __m128i alpha = _mm_set1_epi32(f_.a ? 0 : int(0xFF000000u));
    const __m128i keyMask = _mm_set1_epi32(int(keyMask_)), key = _mm_set1_epi32(int(key_ & keyMask_));
    for (; i + 4 <= n; i += 4) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        const __m128i k = _mm_cmpeq_epi32(_mm_and_si128(v, keyMask), key);
        v = _mm_or_si128(v, alpha);
        if (keyed_) v = _mm_andnot_si128(k, v);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), v);
    }
    for (; i < n; i++) dst[i] = One(src[i]);
}

}  // namespace dd2d9
