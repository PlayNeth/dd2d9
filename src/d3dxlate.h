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

// ValidateDevice (S §ValidateDevice, §For dd2d9 17): XP asks the driver, and answers
// E_NOTIMPL without one, which games take for a setup that can't be drawn. dd2d9 has no
// driver to ask and can draw what Direct3D 7 can express, so its answer is its own: one pass,
// unless the stages in use go past what the device reports (GetCaps) or hold values that
// name nothing. tss: the 8 stages' states by Direct3D 7 number; textures: those with a
// texture set. *passes is 0 where it fails, as XP leaves it.
inline HRESULT ValidateStages(const DWORD (&tss)[8][64], DWORD textures, DWORD opCaps, DWORD maxStages, DWORD maxTextures,
                              DWORD* passes) {
    *passes = 0;
    // An operation the device has: 1 (DISABLE) to 24 (DOTPRODUCT3), each a bit of its caps.
    const auto has = [opCaps](DWORD op) { return op >= 1 && op <= 24 && (opCaps & (1u << (op - 1))); };
    // An argument: DIFFUSE to SPECULAR, with COMPLEMENT and ALPHAREPLICATE at most.
    const auto arg = [](DWORD a) { return (a & D3DTA_SELECTMASK) <= D3DTA_SPECULAR && !(a & ~DWORD(0x3F)); };
    // The arguments an operation reads: the first, the second, or both.
    const auto args = [&arg](DWORD op, DWORD a1, DWORD a2) {
        return (op == D3DTOP_SELECTARG2 || arg(a1)) && (op == D3DTOP_SELECTARG1 || arg(a2));
    };
    DWORD used = 0;
    for (DWORD s = 0; s < 8; s++) {
        const DWORD* t = tss[s];
        const DWORD colorOp = t[D3DTSS_COLOROP], alphaOp = t[D3DTSS_ALPHAOP];
        if (colorOp == D3DTOP_DISABLE) break;  // this stage and those after it aren't used
        if (s >= maxStages) return D3DERR_TOOMANYOPERATIONS;
        if (!has(colorOp)) return D3DERR_UNSUPPORTEDCOLOROPERATION;
        if (!args(colorOp, t[D3DTSS_COLORARG1], t[D3DTSS_COLORARG2])) return D3DERR_UNSUPPORTEDCOLORARG;
        if (!has(alphaOp)) return D3DERR_UNSUPPORTEDALPHAOPERATION;
        if (alphaOp != D3DTOP_DISABLE && !args(alphaOp, t[D3DTSS_ALPHAARG1], t[D3DTSS_ALPHAARG2])) return D3DERR_UNSUPPORTEDALPHAARG;
        if (textures & (1u << s)) used++;
    }
    if (used > maxTextures) return D3DERR_TOOMANYOPERATIONS;
    *passes = 1;
    return D3D_OK;
}

// D3DFE_UpdateClipStatus (§7.4): the codes' union ORed into the status, their intersection
// ANDed into its bits 12 up; the guard band's bits dropped.
inline DWORD MergeClipStatus(DWORD status, DWORD orCodes, DWORD andCodes) {
    return (status | (orCodes & 0xFFF)) & ((andCodes << 12) | 0xFF000FFF);
}

}  // namespace dd2d9
