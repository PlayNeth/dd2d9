// The FourCC formats Blt converts through A8R8G8B8 (as the drivers under ddraw.dll did):
// DXT1-5 (S3TC) blocks (Wine's test_compressed_surface_stretch on Windows) and YUY2/UYVY
// (video decoded into an offscreen surface and blitted to the screen).
#pragma once

#include <cstdint>

#include <windows.h>

namespace dd2d9 {

struct Buffer;

bool DxtFormat(DWORD fourCC);  // DXT1-5
// The whole of `b` (DXT or YUV) into `argb` (b's size, `pitch` bytes a row).
void DecodeFourCC(const Buffer* b, uint8_t* argb, LONG pitch);
// The part of `b` that `r` touches, from `argb` (b's size, `pitch` bytes a row): whole DXT
// blocks (pixels past the surface's edge repeat its last row and column) or whole YUV
// pixel pairs (chroma the pair's average).
void EncodeFourCC(const uint8_t* argb, LONG pitch, Buffer* b, const RECT& r);
// A colour key of a FourCC surface as the A8R8G8B8 its pixels decode to: DXT keys are
// R5G6B5 (the blocks' colours), YUY2/UYVY keys a pixel pair as the surface packs it (the
// first pixel's colour).
uint32_t FourCCKeyToArgb(DWORD fourCC, DWORD key);

}  // namespace dd2d9
