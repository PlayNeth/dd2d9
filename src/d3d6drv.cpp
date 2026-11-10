// dd2d9's Direct3D 3-6: the emulated DX6 driver behind the devices, which turns their state
// into Direct3D 9's (B §1.4).
#include "d3d6int.h"

namespace dd2d9 {
namespace d6 {

// ---------------------------------------------------------------- the emulated driver (B §1.4)

// Pushes everything to a new (or reset, or shared) Direct3D 9 device. Use held.
void Device::PushAll() {
    tssSentMask[0] = tssSentMask[1] = 0;
    alphaSentMask = 0;
    // What the vertices bring: Direct3D 9 gets transformed vertices only (d3d6xform.cpp),
    // with the fog factor in specular alpha.
    ctx.SetRenderState(137, FALSE);  // LIGHTING
    ctx.SetRenderState(140, 0);      // FOGVERTEXMODE NONE
    ctx.SetRenderState(136, TRUE);   // CLIPPING
    ctx.SetRenderState(151, 0);      // VERTEXBLEND
    ctx.SetRenderState(152, 0);      // CLIPPLANEENABLE
    for (DWORD s = 1; s < 256; s++) PushRS9(s);
    for (DWORD st = 0; st < 8; st++)
        for (DWORD t = 1; t < 24; t++) PushStage9(st, t);
    PushStageOps9();
    ctx.SetRenderState(128, dWrap0);
    ctx.SetTargets(rtUnheld ? nullptr : rt, z);  // a Z buffer set as the target is no colour target
    SetViewport9();
}

// Render states Direct3D 9 has with the same meaning (B §Implementation notes).
void Device::PushRS9(DWORD s) {
    const DWORD v = rs[s];
    switch (s) {
    case 7: {  // ZENABLE: W buffering only where D3D9 has it
        const bool w = (hal.dpcTriCaps.dwRasterCaps & D3DPRASTERCAPS_WBUFFER) != 0 && kind == kHAL;
        ctx.SetRenderState(7, v == D3DZB_USEW && !w ? DWORD(D3DZB_TRUE) : v);
        break;
    }
    case 41:  // COLORKEYENABLE: the textures' keys become alpha, which an alpha test drops
        ctx.SetColorKeyEnable(v == TRUE);
        PushStageOps9();
        break;
    case 15: case 24: case 25:  // the alpha test, and COLORKEYENABLE's emulation of it
        PushStageOps9();
        break;
    case 27:  // ALPHABLENDENABLE: the colour key's emulation depends on it
        ctx.SetRenderState(27, v);
        PushStageOps9();
        break;
    case 47:  // ZBIAS: D3D9's DEPTHBIAS (195), a float
        ctx.SetRenderState(195, D(ZBiasToDepthBias(v)));
        break;
    case 8: case 9: case 14: case 16: case 19: case 20: case 22: case 23: case 26: case 28: case 29:
    case 34: case 35: case 36: case 37: case 38: case 48: case 52: case 53: case 54: case 55: case 56: case 57:
    case 58: case 59: case 60:
        ctx.SetRenderState(s, v);
        break;
    default:
        if (s >= 129 && s <= 135) ctx.SetRenderState(s, v);  // WRAP1-7 (WRAP0: the driver's copy)
        break;
    }
}

void Device::PushStage9(DWORD stage, DWORD t) {
    const DWORD v = dtss[stage][t];
    if (v == 0xFFFFFFFF && t != D3DTSS_BORDERCOLOR) return;  // never set
    DWORD sampler, v9;
    if (TssToSampler(t, v, &sampler, &v9)) {
        ctx.SetSamplerState(stage, sampler, v9);
        return;
    }
    switch (t) {
    case D3DTSS_COLOROP: case D3DTSS_COLORARG1: case D3DTSS_COLORARG2:
    case D3DTSS_ALPHAOP: case D3DTSS_ALPHAARG1: case D3DTSS_ALPHAARG2:
        if (stage <= 1) PushStageOps9();
        else ctx.SetTextureStageState(stage, t, v);
        break;
    case D3DTSS_ADDRESS:
        ctx.SetSamplerState(stage, 1, v);
        ctx.SetSamplerState(stage, 2, v);
        break;
    case D3DTSS_ADDRESSU: ctx.SetSamplerState(stage, 1, v); break;
    case D3DTSS_ADDRESSV: ctx.SetSamplerState(stage, 2, v); break;
    default:
        if ((t >= 7 && t <= 11) || t == 22 || t == 23) ctx.SetTextureStageState(stage, t, v);
        break;
    }
}

// A hardware device textures only from texture surfaces (Wine's test_texture_wrong_caps).
Surface* Device::StageSurface(DWORD stage) const {
    Surface* s = dTexHandle ? (stage == 0 ? SurfaceForHandle(dTexHandle) : nullptr) : dtex[stage];
    if (s && kind == kHAL && !(s->desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE)) return nullptr;
    return s;
}

// Stage 0 and 1's ops and arguments and the alpha test, as PushStageOps9 last sent them (it
// runs at every draw): only changes go to D3D9. Forgotten in PushAll.
void Device::SetTSS9(DWORD stage, DWORD t, DWORD v) {
    const DWORD bit = 1u << t;
    if ((tssSentMask[stage] & bit) && tssSent[stage][t] == v) return;
    tssSentMask[stage] |= bit;
    tssSent[stage][t] = v;
    ctx.SetTextureStageState(stage, t, v);
}

void Device::SetAlphaRS9(int i, DWORD v) {
    static const DWORD kStates[3] = {15, 24, 25};
    if ((alphaSentMask & (1u << i)) && alphaSent[i] == v) return;
    alphaSentMask |= 1u << i;
    alphaSent[i] = v;
    ctx.SetRenderState(kStates[i], v);
}

// Stages 0 and 1: while TEXTUREHANDLE is set only stage 0 runs; legacy MODULATE takes alpha
// from the texture when it has alpha, else from the vertex (B §1.4: the reference
// rasterizer's span choice for MODULATE(TEXTURE, DIFFUSE) / SELECTARG1(TEXTURE)). Then
// COLORKEYENABLE's emulation (KeyedAlphaState) may change stage 0's alpha op and the alpha
// test, so the alpha test is sent here too.
// The stage-0 alpha values are worked out first and each sent once: sending the game's and
// then the replacement would change D3D9's state twice at every draw.
void Device::PushStageOps9() {
    const DWORD* s0 = dtss[0];
    for (DWORD t : {D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2})
        if (s0[t] != 0xFFFFFFFF) SetTSS9(0, t, s0[t]);
    const DWORD op1 = dTexHandle ? DWORD(D3DTOP_DISABLE) : dtss[1][D3DTSS_COLOROP];
    DWORD tss0[32];
    memcpy(tss0, s0, sizeof(tss0));
    Surface* tex = StageSurface(0);
    if (s0[D3DTSS_ALPHAOP] == D3DTOP_SELECTARG1 && s0[D3DTSS_ALPHAARG1] == D3DTA_TEXTURE && s0[D3DTSS_COLOROP] == D3DTOP_MODULATE &&
        s0[D3DTSS_COLORARG1] == D3DTA_TEXTURE && (s0[D3DTSS_COLORARG2] == D3DTA_DIFFUSE || s0[D3DTSS_COLORARG2] == D3DTA_CURRENT) &&
        op1 == D3DTOP_DISABLE && tex && !TextureHasAlpha(tex) && !tex->buf->fmt.fourcc) {
        // Texture alpha is 1 (or 0 where colour-keyed): MODULATE gives the vertex alpha.
        tss0[D3DTSS_ALPHAOP] = D3DTOP_MODULATE;
        tss0[D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
    }
    const DWORD* later[7] = {};
    if (op1 != D3DTOP_DISABLE)
        for (int i = 0; i < 7; i++) later[i] = dtss[i + 1];
    const KeyedAlpha k = KeyedAlphaState(rs, tss0, tex, Stage0AlphaUsed(later, rt->buf->fmt.a != 0));
    if (k.alphaArg1 != 0xFFFFFFFF) SetTSS9(0, D3DTSS_ALPHAARG1, k.alphaArg1);
    if (k.alphaArg2 != 0xFFFFFFFF) SetTSS9(0, D3DTSS_ALPHAARG2, k.alphaArg2);
    if (k.alphaOp != 0xFFFFFFFF) SetTSS9(0, D3DTSS_ALPHAOP, k.alphaOp);
    for (DWORD t : {D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2})
        if (dtss[1][t] != 0xFFFFFFFF) SetTSS9(1, t, dtss[1][t]);
    if (op1 != 0xFFFFFFFF) SetTSS9(1, D3DTSS_COLOROP, op1);
    for (int i = 0; i < 3; i++) SetAlphaRS9(i, k.test[i]);
}

// At each draw: the stage textures (a handle resolves now), and what depends on them.
void Device::BindTextures9() {
    for (DWORD i = 0; i < 8; i++) ctx.SetTexture(i, StageSurface(i));
    PushStageOps9();
}

// A render state as the driver gets it (B §1.4).
void Device::DrvRS(DWORD s, DWORD v) {
    switch (s) {
    case 1:  // TEXTUREHANDLE: the stage-0 texture (0: none)
        dTexHandle = v;
        dtex[0] = nullptr;
        PushStageOps9();
        return;
    case 3:  // TEXTUREADDRESS
        dtss[0][D3DTSS_ADDRESS] = dtss[0][D3DTSS_ADDRESSU] = dtss[0][D3DTSS_ADDRESSV] = v;
        PushStage9(0, D3DTSS_ADDRESS);
        return;
    case 5: case 6: {  // WRAPU / WRAPV
        const DWORD bit = s == 5 ? D3DWRAP_U : D3DWRAP_V;
        dWrap0 = v ? (dWrap0 | bit) : (dWrap0 & ~bit);
        ctx.SetRenderState(128, dWrap0);
        return;
    }
    case 128:
        dWrap0 = v;
        ctx.SetRenderState(128, v);
        return;
    case 17:  // TEXTUREMAG
        dtss[0][D3DTSS_MAGFILTER] = v == D3DFILTER_LINEAR ? D3DTFG_LINEAR : D3DTFG_POINT;
        PushStage9(0, D3DTSS_MAGFILTER);
        return;
    case 18: {  // TEXTUREMIN
        static const DWORD kMin[7][2] = {{0, 0},
                                         {D3DTFN_POINT, D3DTFP_NONE},
                                         {D3DTFN_LINEAR, D3DTFP_NONE},
                                         {D3DTFN_POINT, D3DTFP_POINT},
                                         {D3DTFN_LINEAR, D3DTFP_POINT},
                                         {D3DTFN_POINT, D3DTFP_LINEAR},
                                         {D3DTFN_LINEAR, D3DTFP_LINEAR}};
        if (v < 1 || v > 6) return;  // other values: no change
        dtss[0][D3DTSS_MINFILTER] = kMin[v][0];
        dtss[0][D3DTSS_MIPFILTER] = kMin[v][1];
        PushStage9(0, D3DTSS_MINFILTER);
        PushStage9(0, D3DTSS_MIPFILTER);
        return;
    }
    case 21: {  // TEXTUREMAPBLEND
        DWORD* t = dtss[0];
        switch (v) {
        case D3DTBLEND_MODULATE: case D3DTBLEND_MODULATEMASK:
            t[D3DTSS_COLOROP] = D3DTOP_MODULATE;
            t[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
            t[D3DTSS_COLORARG2] = D3DTA_DIFFUSE;
            t[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
            t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            t[D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
            break;
        case D3DTBLEND_DECALALPHA:
            t[D3DTSS_COLOROP] = D3DTOP_BLENDTEXTUREALPHA;
            t[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
            t[D3DTSS_COLORARG2] = D3DTA_DIFFUSE;
            t[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG2;
            t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            t[D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
            break;
        case D3DTBLEND_MODULATEALPHA:
            t[D3DTSS_COLOROP] = t[D3DTSS_ALPHAOP] = D3DTOP_MODULATE;
            t[D3DTSS_COLORARG1] = t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            t[D3DTSS_COLORARG2] = t[D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
            break;
        case D3DTBLEND_ADD:
            t[D3DTSS_COLOROP] = D3DTOP_ADD;
            t[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
            t[D3DTSS_COLORARG2] = D3DTA_DIFFUSE;
            t[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG2;
            t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            t[D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
            break;
        default:  // DECAL, DECALMASK, COPY and anything else
            t[D3DTSS_COLOROP] = t[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
            t[D3DTSS_COLORARG1] = t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            break;
        }
        dtss[1][D3DTSS_COLOROP] = D3DTOP_DISABLE;
        PushStageOps9();
        return;
    }
    case 43:
        dtss[0][D3DTSS_BORDERCOLOR] = v;
        PushStage9(0, D3DTSS_BORDERCOLOR);
        return;
    case 44:
        dtss[0][D3DTSS_ADDRESSU] = v;
        PushStage9(0, D3DTSS_ADDRESSU);
        return;
    case 45:
        dtss[0][D3DTSS_ADDRESSV] = v;
        PushStage9(0, D3DTSS_ADDRESSV);
        return;
    case 46:
        dtss[0][D3DTSS_MIPMAPLODBIAS] = v;
        PushStage9(0, D3DTSS_MIPMAPLODBIAS);
        return;
    default:
        PushRS9(s);  // ANISOTROPY and the other legacy states: nothing (only stored)
        return;
    }
}

void Device::DrvTSS(DWORD stage, DWORD t, DWORD v) {
    if (t >= 32) return;
    dtss[stage][t] = v;
    if (t == D3DTSS_ADDRESS) dtss[stage][D3DTSS_ADDRESSU] = dtss[stage][D3DTSS_ADDRESSV] = v;
    PushStage9(stage, t);
}

void Device::DrvTexture(DWORD stage, Surface* s) {
    dtex[stage] = s;
    if (stage <= 1 && s) dTexHandle = 0;  // binding a texture on stage 0 or 1 ends TEXTUREHANDLE
}

}  // namespace d6
}  // namespace dd2d9
