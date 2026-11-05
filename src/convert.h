// Rows of pixels to A8R8G8B8, exactly as PixelToARGB converts one (util.cpp), with SSE2 for
// the common layouts: 8-bit palettized (a table), 16-bit 565/555/1555/4444 and 32-bit RGB
// (24-bit RGB unkeyed: four pixels from three words),
// with an optional colour key (keyed pixels become 0, transparent black). Used for textures
// and render targets on their way to the GPU (gpures.cpp).
#pragma once
#include "common.h"

namespace dd2d9 {

class RowConverter {
public:
    // pal: a palettized format's colours (null: grey ramp, as PixelToARGB). keyed/key: pixels
    // whose colour bits equal the key's become 0. palAlpha: the palette has DDPCAPS_ALPHA, each
    // entry's peFlags is its alpha (textures only).
    RowConverter(const Format& f, const PALETTEENTRY* pal, bool keyed = false, DWORD key = 0, bool palAlpha = false);
    void Run(const uint8_t* src, uint32_t* dst, int n) const;

private:
    enum Kind { kGeneric, kPal8, k16, k24, k32 };
    struct Channel {
        int shift = 0, bits = 0;  // bits 0: absent (alpha: 0xFF)
    };
    void Run16(const uint16_t* src, uint32_t* dst, int n) const;
    void Run24(const uint8_t* src, uint32_t* dst, int n) const;
    void Run32(const uint32_t* src, uint32_t* dst, int n) const;
    uint32_t One(uint32_t v) const;

    Kind kind_ = kGeneric;
    Format f_;
    const PALETTEENTRY* pal_;
    bool keyed_;
    bool palAlpha_;
    uint32_t key_, keyMask_;
    Channel ch_[4];  // red, green, blue, alpha
    uint32_t lut_[256];
};

// Rows of A8R8G8B8 into a format's pixels, exactly as ARGBToPixel makes one (util.cpp), with
// SSE2 for 16-bit formats of contiguous channels and 32-bit X8R8G8B8 / A8R8G8B8; others
// through a table per channel. `orMask` is set on every pixel written. Not palettized.
class RowPacker {
public:
    // For format `f` (kept until another is asked for: the tables are made once).
    void Prepare(const Format& f);
    void Run(const uint32_t* src, uint8_t* dst, int n, uint32_t orMask) const;
    uint32_t One(uint32_t c) const { return t_[0][c & 255] | t_[1][(c >> 8) & 255] | t_[2][(c >> 16) & 255] | t_[3][c >> 24]; }

private:
    enum Kind { kTable, k16, k32 };
    bool valid_ = false;
    Kind kind_ = kTable;
    Format f_;
    int bytes_ = 0;
    int shift_[4] = {}, bits_[4] = {};  // k16: blue, green, red, alpha (bits 0: absent)
    uint32_t mask32_ = 0;               // k32: the bits kept
    uint32_t t_[4][256];                // kTable: blue, green, red, alpha
};

}  // namespace dd2d9
