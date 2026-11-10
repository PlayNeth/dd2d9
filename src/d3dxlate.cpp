// Direct3D 3-7 state as Direct3D 9 gets it (see d3dxlate.h).
#include "d3dxlate.h"

#include "gpures.h"
#include "objects.h"

namespace dd2d9 {

bool TssToSampler(DWORD type, DWORD v, DWORD* sampler, DWORD* value9) {
    switch (type) {
    case D3DTSS_BORDERCOLOR: *sampler = 4, *value9 = v; return true;
    case D3DTSS_MAGFILTER: {
        static const DWORD kMap[] = {0, 1, 2, 2, 2, 3};  // FLAT/GAUSSIANCUBIC: linear; ANISOTROPIC 5 -> 3
        *sampler = 5, *value9 = v < 6 ? kMap[v] : 2;
        return true;
    }
    case D3DTSS_MINFILTER: *sampler = 6, *value9 = v <= 3 ? v : 2; return true;
    case D3DTSS_MIPFILTER: *sampler = 7, *value9 = v >= 2 && v <= 3 ? v - 1 : 0; return true;
    case D3DTSS_MIPMAPLODBIAS: *sampler = 8, *value9 = v; return true;
    case D3DTSS_MAXMIPLEVEL: *sampler = 9, *value9 = v; return true;
    case D3DTSS_MAXANISOTROPY: *sampler = 10, *value9 = v; return true;
    default: return false;
    }
}

void AddBumpFormats(std::vector<DDPIXELFORMAT>* list) {
    const GpuCaps c = GpuGetCaps();
    auto bump = [&](bool have, DWORD bits, DWORD du, DWORD dv, DWORD lum) {
        if (!have) return;
        DDPIXELFORMAT f = {};
        f.dwSize = sizeof(f);
        f.dwFlags = DDPF_BUMPDUDV | (lum ? DDPF_BUMPLUMINANCE : 0);
        f.dwBumpBitCount = bits;
        f.dwBumpDuBitMask = du;
        f.dwBumpDvBitMask = dv;
        f.dwBumpLuminanceBitMask = lum;
        list->push_back(f);
    };
    bump(c.bumpV8U8, 16, 0xFF, 0xFF00, 0);
    bump(c.bumpL6V5U5, 16, 0x1F, 0x3E0, 0xFC00);
    bump(c.bumpX8L8V8U8, 24, 0xFF, 0xFF00, 0xFF0000);
}

float ZBiasToDepthBias(DWORD zbias) { return -0.000005f * float(LONG(zbias)); }

bool Stage0AlphaUsed(const DWORD* const later[7], bool targetAlpha) {
    bool fromStage0 = true;  // whether the alpha so far (CURRENT's) still depends on stage 0's
    for (int i = 0; i < 7 && later[i]; i++) {
        const DWORD* s = later[i];
        auto get = [&](DWORD t, DWORD def) { return s[t] == 0xFFFFFFFF ? def : s[t]; };
        const DWORD cop = get(D3DTSS_COLOROP, D3DTOP_DISABLE);
        if (cop == D3DTOP_DISABLE) break;
        const DWORD c1 = get(D3DTSS_COLORARG1, D3DTA_TEXTURE), c2 = get(D3DTSS_COLORARG2, D3DTA_CURRENT);
        const DWORD aop = get(D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        const DWORD a1 = get(D3DTSS_ALPHAARG1, D3DTA_TEXTURE), a2 = get(D3DTSS_ALPHAARG2, D3DTA_CURRENT);
        auto current = [](DWORD arg) { return (arg & D3DTA_SELECTMASK) == D3DTA_CURRENT; };
        // (SELECTARG1 and SELECTARG2 read one argument, the rest both.)
        auto reads = [](DWORD op, DWORD x1, DWORD x2, auto test) {
            return (op != D3DTOP_SELECTARG2 && test(x1)) || (op != D3DTOP_SELECTARG1 && test(x2));
        };
        if (fromStage0) {
            auto currentAlpha = [&](DWORD arg) { return current(arg) && (arg & D3DTA_ALPHAREPLICATE); };
            if (cop == D3DTOP_BLENDCURRENTALPHA || reads(cop, c1, c2, currentAlpha) ||
                (cop >= D3DTOP_MODULATEALPHA_ADDCOLOR && cop <= D3DTOP_MODULATEINVCOLOR_ADDALPHA && current(c1)))
                return true;
        }
        // An alpha op DISABLE with the colour on is undefined in Direct3D: taken as passing the
        // alpha on.
        if (aop != D3DTOP_DISABLE)
            fromStage0 = fromStage0 && (aop == D3DTOP_BLENDCURRENTALPHA || reads(aop, a1, a2, current));
    }
    return fromStage0 && targetAlpha;
}

KeyedAlpha KeyedAlphaState(const DWORD* rs, const DWORD* tss0, const Surface* tex0, bool alphaUsed) {
    KeyedAlpha k;
    k.alphaOp = tss0[D3DTSS_ALPHAOP];
    k.alphaArg1 = tss0[D3DTSS_ALPHAARG1];
    k.alphaArg2 = tss0[D3DTSS_ALPHAARG2];
    const bool test = rs[D3DRENDERSTATE_ALPHATESTENABLE] == TRUE;
    const DWORD func = rs[D3DRENDERSTATE_ALPHAFUNC] == 0xFFFFFFFF ? DWORD(D3DCMP_ALWAYS) : rs[D3DRENDERSTATE_ALPHAFUNC];
    k.test[0] = test;
    k.test[1] = rs[D3DRENDERSTATE_ALPHAREF] & 0xFF;
    k.test[2] = func;
    // An untextured pass (COLOROP DISABLE) with a keyed texture still bound samples nothing:
    // no key.
    const bool key = rs[D3DRENDERSTATE_COLORKEYENABLE] == TRUE && tex0 && tex0->hasCk[kCkSrcBlt] &&
                     tss0[D3DTSS_COLOROP] != D3DTOP_DISABLE;
    if (!key) return k;
    if (rs[D3DRENDERSTATE_ALPHABLENDENABLE] != TRUE && !test && !alphaUsed) {
        // The game's alpha is unused (no blending, no alpha test, no later stage or render
        // target reading it): the test gets the texel's.
        k.alphaOp = D3DTOP_SELECTARG1;
        k.alphaArg1 = D3DTA_TEXTURE;
    } else if (k.alphaOp == D3DTOP_SELECTARG2 && !TextureHasAlpha(tex0)) {
        // The other alpha kept, keyed texels still dropped: the texture's alpha is 1 except
        // there, so MODULATE gives the same alpha everywhere else.
        k.alphaOp = D3DTOP_MODULATE;
        k.alphaArg1 = D3DTA_TEXTURE;
    }
    // A game test that lets alpha 0 through as a whole (ALWAYS, GREATEREQUAL 0) becomes the
    // key's, which drops only alpha 0. Any other game test is kept, as one stage can't AND two
    // tests: where it too drops alpha 0 (GREATER, NOTEQUAL 0, GREATEREQUAL above 0, EQUAL above
    // 0, NEVER) the key still works, but LESS, LESSEQUAL, EQUAL 0 and NOTEQUAL above 0 let keyed
    // texels through.
    if (!test || func == D3DCMP_ALWAYS || (func == D3DCMP_GREATEREQUAL && k.test[1] == 0)) {
        k.test[0] = TRUE;
        k.test[1] = 0;
        k.test[2] = D3DCMP_GREATER;
    }
    return k;
}

}  // namespace dd2d9
