// The FourCC formats Blt converts through A8R8G8B8 (as the drivers under ddraw.dll did):
// DXT1-5 (S3TC) blocks (Wine's test_compressed_surface_stretch on Windows), packed YUY2/UYVY
// and planar YV12/I420 (video decoded into an offscreen surface and blitted to the screen).
#pragma once

#include <cstdint>

#include <windows.h>

namespace dd2d9 {

struct Buffer;

bool DxtFormat(DWORD fourCC);  // DXT1-5
// `b` (DXT or YUV) into `argb` (b's size, `pitch` bytes a row): rows y0 to y1 (DXT: from
// y0's block row on), columns x0 to x1 (whole DXT blocks, whole YUV pairs), or all of it.
void DecodeFourCC(const Buffer* b, uint8_t* argb, LONG pitch, int y0 = 0, int y1 = 0x7FFFFFFF, int x0 = 0,
                  int x1 = 0x7FFFFFFF);
// The part of `b` that `r` touches, from `argb` (b's size, `pitch` bytes a row): whole DXT
// blocks (pixels past the surface's edge repeat its last row and column) or whole YUV
// pixel pairs (chroma the pair's average).
void EncodeFourCC(const uint8_t* argb, LONG pitch, Buffer* b, const RECT& r);
// A colour key of a FourCC surface as the A8R8G8B8 its pixels decode to: DXT keys are
// R5G6B5 (the blocks' colours), YUY2/UYVY keys a pixel pair as the surface packs it (the
// first pixel's colour).
uint32_t FourCCKeyToArgb(DWORD fourCC, DWORD key);
// Planar (YV12 / I420) to planar of the same format, no stretching: d and s the same size,
// their left and top even (the chroma planes copied by 2x2 block).
void CopyPlanar(Buffer* dst, const RECT& d, const Buffer* src, const RECT& s);

}  // namespace dd2d9
