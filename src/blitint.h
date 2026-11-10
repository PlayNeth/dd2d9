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

// A colour key as a blit compares it: a pixel matches when its value & mask lies in lo..hi.
struct BlitKey {
    bool on = false;
    uint32_t lo = 0, hi = 0, mask = 0;
};

// The colour key Blt's flags ask for, as values in the keyed surface's format: an override's
// one value (games often leave its high value uninitialized) or the surface's range.
struct KeyRange {
    bool on = false;
    DWORD lo = 0, hi = 0;
};
KeyRange SourceKeyOf(DWORD flags, const DDBLTFX* fx, const Surface* src);
// DDBLT_KEYDEST's key comes from `owner`: the destination, or with an old interface's source
// passed, the source (which then may have none: its last value is used).
KeyRange DestKeyOf(DWORD flags, const DDBLTFX* fx, const Surface* owner);
inline BlitKey Masked(const KeyRange& k, uint32_t mask) {
    if (!k.on) return {};
    const uint32_t lo = k.lo & mask, hi = k.hi & mask;
    return {true, lo, hi < lo ? lo : hi, mask};
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
