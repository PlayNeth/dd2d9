// Direct3D's GPU copies: formats, and pixels between the CPU copies and the GPU's (see gpuint.h).
#include "gpuint.h"

#include <algorithm>

namespace dd2d9 {

// The Direct3D 9 format with this memory layout, or D3DFMT_UNKNOWN (converted to
// A8R8G8B8 on the way to the GPU and back).
D3DFORMAT NativeFormat(const Format& f) {
    if (f.fourcc) {
        switch (f.fourCC) {
        case MAKEFOURCC('D', 'X', 'T', '1'): return D3DFMT_DXT1;
        case MAKEFOURCC('D', 'X', 'T', '2'): return D3DFMT_DXT2;
        case MAKEFOURCC('D', 'X', 'T', '3'): return D3DFMT_DXT3;
        case MAKEFOURCC('D', 'X', 'T', '4'): return D3DFMT_DXT4;
        case MAKEFOURCC('D', 'X', 'T', '5'): return D3DFMT_DXT5;
        default: return D3DFMT_UNKNOWN;
        }
    }
    if (f.bump) {
        if (f.bpp == 24) return D3DFMT_X8L8V8U8;  // (widened: see WriteLevel)
        return f.b ? D3DFMT_L6V5U5 : D3DFMT_V8U8;
    }
    if (f.pal8 || f.zbuf) return D3DFMT_UNKNOWN;
    if (f.bpp == 16) {
        if (f.r == 0xF800 && f.g == 0x07E0 && f.b == 0x001F && !f.a) return D3DFMT_R5G6B5;
        if (f.r == 0x7C00 && f.g == 0x03E0 && f.b == 0x001F) return f.a == 0x8000 ? D3DFMT_A1R5G5B5 : f.a ? D3DFMT_UNKNOWN : D3DFMT_X1R5G5B5;
        if (f.r == 0x0F00 && f.g == 0x00F0 && f.b == 0x000F) return f.a == 0xF000 ? D3DFMT_A4R4G4B4 : f.a ? D3DFMT_UNKNOWN : D3DFMT_X4R4G4B4;
    }
    if (f.bpp == 32 && f.r == 0xFF0000 && f.g == 0xFF00 && f.b == 0xFF)
        return f.a == 0xFF000000u ? D3DFMT_A8R8G8B8 : f.a ? D3DFMT_UNKNOWN : D3DFMT_X8R8G8B8;
    return D3DFMT_UNKNOWN;
}

bool IsRenderTargetFormat(D3DFORMAT f) {
    return f == D3DFMT_R5G6B5 || f == D3DFMT_X1R5G5B5 || f == D3DFMT_A1R5G5B5 || f == D3DFMT_X8R8G8B8 ||
           f == D3DFMT_A8R8G8B8;
}

// One row of the CPU copy to the GPU's memory: as is (same layout), or to A8R8G8B8 through
// `conv` (the palette; a colour-keyed texel becomes transparent black, as DX7 hardware treated it).
void RowToGpu(const Buffer* b, int y, uint8_t* dst, const RowConverter* conv) {
    if (!conv) memcpy(dst, b->Row(y), size_t(b->w) * b->fmt.Bytes());
    else conv->Run(b->Row(y), reinterpret_cast<uint32_t*>(dst), b->w);
}

// One row of a render target back into the CPU copy.
void RowFromGpu(Buffer* b, int y, const uint8_t* src, bool convert, PaletteMap* pal, const ToPixel* to) {
    uint8_t* dst = b->Row(y);
    const DWORD bytes = b->fmt.Bytes();
    if (!convert) {
        memcpy(dst, src, size_t(b->w) * bytes);
        return;
    }
    const auto* in = reinterpret_cast<const uint32_t*>(src);
    if (b->fmt.pal8) {
        for (int x = 0; x < b->w; x++) dst[x] = pal ? pal->Index(in[x]) : 0;
        return;
    }
    if (to && bytes == 2) {
        auto* d16 = reinterpret_cast<uint16_t*>(dst);
        for (int x = 0; x < b->w; x++) d16[x] = uint16_t((*to)(in[x]));
        return;
    }
    for (int x = 0; x < b->w; x++, dst += bytes) WritePixel(dst, bytes, to ? (*to)(in[x]) : ARGBToPixel(b->fmt, in[x]));
}

// The Z buffer's bits in its CPU format: mask, shift, largest value.
void ZBits(const Format& f, DWORD* mask, DWORD* shift, DWORD* maxv) {
    *mask = f.r ? f.r : f.ValueMask();
    *shift = *mask ? DWORD(__builtin_ctz(*mask)) : 0;
    *maxv = *mask >> *shift;
}

// A depth value in the Z buffer's CPU format as Direct3D 9's 0 to 1.
float ZToFloat(const Format& f, DWORD v) {
    DWORD mask, shift, maxv;
    ZBits(f, &mask, &shift, &maxv);
    return maxv ? float(double((v & mask) >> shift) / maxv) : 0.0f;
}

// The Z buffer's Direct3D 9 format: closest depth, with stencil if it has some. depth24=on:
// 24 bits for a 16-bit one too (less z-fighting in the distance).
D3DFORMAT DepthFormat(const Format& f) {
    // (wbuffer=force: 24 bits too, the emulation's depths are 1/w, crowded near 1 in the distance.)
    if (f.bpp == 16) return g_config.depth24 || g_config.wbuffer == 2 ? D3DFMT_D24X8 : D3DFMT_D16;
    if (f.a) return D3DFMT_D24S8;  // Format keeps a Z buffer's stencil mask in `a`
    return D3DFMT_D24X8;
}

// One level of a texture's CPU copy into its locked GPU level.
void WriteLevel(const Buffer* b, const D3DLOCKED_RECT& lr, const PALETTEENTRY* entries, bool convert, bool keyed,
                DWORD key, bool palAlpha) {
    if (b->fmt.bump && b->fmt.bpp == 24) {
        // U8V8L8 widened to X8L8V8U8 (the same bytes, a fourth unused): four pixels from three
        // 32-bit words at a time.
        for (int y = 0; y < b->h; y++) {
            const uint8_t* src = b->Row(y);
            uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
            int x = 0;
            for (; x + 4 <= b->w; x += 4) {
                uint32_t w[3];
                memcpy(w, src + size_t(x) * 3, 12);
                dst[x] = w[0] | 0xFF000000u;
                dst[x + 1] = (w[0] >> 24) | (w[1] << 8) | 0xFF000000u;
                dst[x + 2] = (w[1] >> 16) | (w[2] << 16) | 0xFF000000u;
                dst[x + 3] = (w[2] >> 8) | 0xFF000000u;
            }
            for (; x < b->w; x++) {
                const uint8_t* p = src + size_t(x) * 3;
                dst[x] = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | 0xFF000000u;
            }
        }
        return;
    }
    if (b->fmt.BlockCompressed()) {
        // Rows of 4x4 blocks: the CPU copy's pitch is a quarter of a block row.
        const int rows = (b->h + 3) / 4;
        const size_t rowBytes = size_t(b->pitch) * 4;
        for (int y = 0; y < rows; y++)
            memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, b->bits + y * rowBytes,
                   std::min<size_t>(rowBytes, size_t(lr.Pitch)));
    } else {
        const RowConverter conv(b->fmt, entries, keyed, key, palAlpha);
        for (int y = 0; y < b->h; y++)
            RowToGpu(b, y, static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, convert ? &conv : nullptr);
    }
}

}  // namespace dd2d9
