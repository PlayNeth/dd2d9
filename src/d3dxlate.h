// Direct3D 3-7 state and clip codes as dd2d9's Direct3D 7 (d3d7*.cpp) and Direct3D 3-6
// (d3d6*.cpp) both need them: the translation to Direct3D 9 of what D3D9 has under other
// names or values (sampler states, ZBIAS, the colour key's alpha test), the bump map formats
// a hardware device lists, and the front end's clip codes and clip status
// (re/spec/d3d7-draw-vb.md §7). Includes d3d.h (never with d3d9.h).
#pragma once
#include "common.h"

#include <d3d.h>

#include <vector>

namespace dd2d9 {

class Surface;

// A texture stage state that is a sampler state in D3D9 (filters, LOD bias, mip level,
// anisotropy, border colour): its D3D9 sampler state and value; false for the others. The
// address modes aren't translated here, as their callers decide which of ADDRESS, ADDRESSU
// and ADDRESSV is in force.
bool TssToSampler(DWORD type, DWORD value, DWORD* sampler, DWORD* value9);

// The bump map formats a hardware device lists where the GPU has them with the bump-mapping
// operations (DirectX 6 and 7: environment-mapped bump mapping), appended to list.
void AddBumpFormats(std::vector<DDPIXELFORMAT>* list);

// ZBIAS (0-16) as D3D9's DEPTHBIAS, a fraction of the depth range: the factor Wine's ddraw
// used until 2011 (its zbias_factor). XP leaves the scale to the driver (S, ZBIAS: not
// determined); one unit is about a third of a 16-bit depth step and 84 steps of a 24-bit one.
// (Wine has since made one unit one depth step of the bound format.)
float ZBiasToDepthBias(DWORD zbias);

// COLORKEYENABLE as D3D9 gets it: the keyed texels were uploaded with alpha 0, and an alpha
// test drops them. That test runs on the stage output's alpha, while XP's key works on the
// texel alone, so stage 0's alpha op is changed where the game's ops would let a keyed texel
// through or drop the rest (Wine's test_colorkey_precision). rs: the render states by number;
// tss0: stage 0's texture stage states by type; tex0: stage 0's texture; alphaUsed:
// Stage0AlphaUsed's answer.
struct KeyedAlpha {
    DWORD alphaOp, alphaArg1, alphaArg2;  // stage 0's
    DWORD test[3];                        // ALPHATESTENABLE, ALPHAREF, ALPHAFUNC
};
KeyedAlpha KeyedAlphaState(const DWORD* rs, const DWORD* tss0, const Surface* tex0, bool alphaUsed);

// Whether stage 0's alpha shows other than through blending and the alpha test: a later stage
// reads it into its colour (BLENDCURRENTALPHA, CURRENT with ALPHAREPLICATE, the
// MODULATEALPHA_ADDCOLOR family), or it reaches a render target with alpha (targetAlpha).
// later: stages 1-7's texture stage states by type, null from the first stage that doesn't
// run; 0xFFFFFFFF (never set) is Direct3D's default.
bool Stage0AlphaUsed(const DWORD* const later[7], bool targetAlpha);

// The clip code of a point in the front end's 0..w clip volume (§7.3), given as x and y in
// that volume ((c + w) / 2 of D3D's), z and w: D3DCLIP_LEFT to D3DCLIP_BACK.
inline DWORD ClipCode01(float x, float y, float z, float w) {
    DWORD code = 0;
    if (x < 0) code |= D3DCLIP_LEFT;
    if (x > w) code |= D3DCLIP_RIGHT;
    if (y > w) code |= D3DCLIP_TOP;
    if (y < 0) code |= D3DCLIP_BOTTOM;
    if (z < 0) code |= D3DCLIP_FRONT;
    if (z > w) code |= D3DCLIP_BACK;
    return code;
}

// The clip code of a transformed vertex (§7.1): against the viewport's rectangle, and z
// against 0..1 only with a Z buffer on. x, y and z are negated first where rhw is negative;
// the caller's x and y are left so (the extents grow by them).
inline DWORD TlClipCode(float* x, float* y, float z, float rhw, const D3DVIEWPORT7& vp, bool zenable) {
    if (rhw < 0) *x = -*x, *y = -*y, z = -z;
    DWORD code = 0;
    if (*x < float(vp.dwX)) code |= D3DCLIP_LEFT;
    if (*x >= float(vp.dwX + vp.dwWidth)) code |= D3DCLIP_RIGHT;
    if (*y < float(vp.dwY)) code |= D3DCLIP_TOP;
    if (*y >= float(vp.dwY + vp.dwHeight)) code |= D3DCLIP_BOTTOM;
    if (zenable) {
        if (z < 0) code |= D3DCLIP_FRONT;
        if (z >= 1) code |= D3DCLIP_BACK;
    }
    return code;
}

// D3DFE_UpdateClipStatus (§7.4): the codes' union ORed into the status, their intersection
// ANDed into its bits 12 up; the guard band's bits dropped.
inline DWORD MergeClipStatus(DWORD status, DWORD orCodes, DWORD andCodes) {
    return (status | (orCodes & 0xFFF)) & ((andCodes << 12) | 0xFF000FFF);
}

}  // namespace dd2d9
