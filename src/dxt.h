// The FourCC formats Blt converts through A8R8G8B8 (as the drivers under ddraw.dll did):
// DXT1-5 (S3TC) blocks (Wine's test_compressed_surface_stretch on Windows), packed YUY2/UYVY
// and planar YV12/I420 (video decoded into an offscreen surface and blitted to the screen).
#pragma once

#include <cstdint>

#include <windows.h>

namespace dd2d9 {

struct Buffer;

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
// A YUV surface's (YUY2, UYVY, YV12, I420) colour key lo..hi (one value but for a range),
// compared in its own Y, U and V (the key's bytes as FourCCKeyToArgb reads them), component
// by component: YUV to RGB clamps and rounds (every Y below 16 is black), so a decoded pixel
// can match a key it doesn't hold, and a box of YUV values isn't a range of A8R8G8B8 ones.
// Sets the alpha of each pixel of `argb` (b decoded by DecodeFourCC, the same rows and
// columns) to 0 where the key matches, 255 elsewhere. A component's high value below its low
// value is the low value.
void MarkYuvKey(const Buffer* b, uint8_t* argb, LONG pitch, DWORD lo, DWORD hi, int y0, int y1, int x0, int x1);
// Planar (YV12 / I420) to planar of the same format, no stretching: d and s the same size,
// their left and top even (the chroma planes copied by 2x2 block).
void CopyPlanar(Buffer* dst, const RECT& d, const Buffer* src, const RECT& s);

}  // namespace dd2d9
