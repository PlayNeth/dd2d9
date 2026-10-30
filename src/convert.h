// Rows of pixels to A8R8G8B8, exactly as PixelToARGB converts one (util.cpp), with SSE2 for
// the common layouts: 8-bit palettized (a table), 16-bit 565/555/1555/4444 and 32-bit RGB,
// with an optional colour key (keyed pixels become 0, transparent black). Used for textures
// and render targets on their way to the GPU (gpures.cpp).
#pragma once
#include "common.h"

namespace dd2d9 {

class RowConverter {
public:
    // pal: a palettized format's colours (null: grey ramp, as PixelToARGB). keyed/key: pixels
    // whose colour bits equal the key's become 0.
    RowConverter(const Format& f, const PALETTEENTRY* pal, bool keyed = false, DWORD key = 0);
    void Run(const uint8_t* src, uint32_t* dst, int n) const;

private:
    enum Kind { kGeneric, kPal8, k16, k32 };
    struct Channel {
        int shift = 0, bits = 0;  // bits 0: absent (alpha: 0xFF)
    };
    void Run16(const uint16_t* src, uint32_t* dst, int n) const;
    void Run32(const uint32_t* src, uint32_t* dst, int n) const;
    uint32_t One(uint32_t v) const;

    Kind kind_ = kGeneric;
    Format f_;
    const PALETTEENTRY* pal_;
    bool keyed_;
    uint32_t key_, keyMask_;
    Channel ch_[4];  // red, green, blue, alpha
    uint32_t lut_[256];
};

}  // namespace dd2d9
