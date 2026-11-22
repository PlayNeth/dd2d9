// The software blitter's parts shared by blit.cpp (Blt and BltFast: ddraw.dll's checks, the
// clipping, colour fills), blitfourcc.cpp (blits with a FourCC side) and blitcore.cpp (the
// pixels). Every blit runs with g_lock held.
#pragma once
#include "objects.h"

#include <vector>

namespace dd2d9 {

// Reports the union of the drawn rectangles to the presenter (primary surface only).
template <class Clips>
inline void MarkClipsDirty(Surface* s, const Clips& clips) {
    if (!s->isPrimary) return;
    for (const RECT& c : clips) s->MarkDirtyIfVisible(&c);
}

// A colour key as a blit compares it: a pixel matches when its value & mask lies in lo..hi; a
// range of colours (DDCKEY_COLORSPACE) when each of its colour channels does (chan: their
// masks), as ddraw.dll's blits compare one, measured on XP (5-6-5 0x0841-0x2104 keys red 1-4,
// green 2-8 and blue 1-4: 0x1000, between the two as a number, isn't keyed).
struct BlitKey {
    bool on = false;
    uint32_t lo = 0, hi = 0, mask = 0;
    uint32_t chan[3] = {};  // none: the value as a whole (one colour, an index, a depth)
};

// The colour key Blt's flags ask for, as values in the keyed surface's format: the override's
// low value, or the surface's, a range as the blit into `dst` compares it (see blit.cpp).
struct KeyRange {
    bool on = false;
    DWORD lo = 0, hi = 0;
};
KeyRange SourceKeyOf(DWORD flags, const DDBLTFX* fx, const Surface* src, const Surface* dst);
// DDBLT_KEYDEST's key comes from `owner`: the destination, or with an old interface's source
// passed, the source (which then may have none: its last value is used).
KeyRange DestKeyOf(DWORD flags, const DDBLTFX* fx, const Surface* owner, const Surface* dst);
// In `f`: a range of an RGB format compared channel by channel, a channel's high end below its
// low one taken as the low one; otherwise as a number.
inline BlitKey Masked(const KeyRange& k, const Format& f) {
    if (!k.on) return {};
    const uint32_t mask = f.KeyMask();
    BlitKey b;
    b.on = true;
    b.mask = mask;
    b.lo = k.lo & mask;
    b.hi = k.hi & mask;
    const bool rgb = !(f.Palettized() || f.zbuf || f.fourcc || f.bump || f.alphaOnly) && f.r && f.g && f.b;
    if (b.lo == b.hi || !rgb) {
        if (b.hi < b.lo) b.hi = b.lo;
        return b;
    }
    const uint32_t chan[3] = {f.r, f.g, f.b};
    for (int c = 0; c < 3; c++) {
        b.chan[c] = chan[c];
        if ((b.hi & chan[c]) < (b.lo & chan[c])) b.hi = (b.hi & ~chan[c]) | (b.lo & chan[c]);
    }
    return b;
}

// v (a pixel of the key's format) matches the key: each in-range test lo <= x <= hi in one
// unsigned comparison.
inline bool KeyHits(const BlitKey& k, uint32_t v) {
    if (!k.chan[0]) return (v & k.mask) - k.lo <= k.hi - k.lo;
    for (uint32_t m : k.chan)
        if ((v & m) - (k.lo & m) > (k.hi & m) - (k.lo & m)) return false;
    return true;
}

// What a blit draws with besides its buffers and rectangles.
struct BlitParams {
    BlitKey srcKey, dstKey;
    bool mirrorX = false, mirrorY = false;
    uint32_t orMask = 0;                 // set on every pixel written
    const PALETTEENTRY* spal = nullptr;  // a palettized source's colours
    const PALETTEENTRY* dpal = nullptr;  // an 8-bit destination's (conversions pick from it)
};

// DDBLT_DDFX's mirroring (the other effects Blt lets through change nothing).
void Mirroring(DWORD flags, const DDBLTFX* fx, BlitParams* p);

// Where a blit to `s` within `d` may draw: the clipper's region (when it is used), or d
// itself, which must then lie inside the surface.
HRESULT ClipsFor(const Surface* s, const RECT& d, bool useClipper, std::vector<RECT>* out);
// The clip rectangles of the blit under way (kept from blit to blit).
extern std::vector<RECT> g_clips;

bool SameLayout(const Format& a, const Format& b);

// The pixels of a blit from `s` of sb into `d` of db, within the clip rectangles (inside db):
// any formats but FourCC ones, keys, stretching and mirroring. Callers have checked the
// rectangles, and that a conversion doesn't involve a bump map.
void DrawBlit(Buffer* db, const RECT& d, const Buffer* sb, const RECT& s, const BlitParams& p,
              const std::vector<RECT>& clips);

}  // namespace dd2d9
