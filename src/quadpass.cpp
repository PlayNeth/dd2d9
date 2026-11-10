// dd2d9's own textured quads on the shared device (see quadpass.h).
#include "quadpass.h"

namespace dd2d9 {

QuadPass::QuadPass(Gpu& gpu, IDirect3DDevice9* d, IDirect3DPixelShader9* ps, DWORD fvf) : gpu_(gpu), d_(d), saved_(d) {
    d->SetPixelShader(ps);
    d->SetVertexShader(nullptr);
    SetFvf9(d, fvf);
    // Only what the pass sets is bound (a texture left bound may be the target itself).
    for (DWORD s = 0; s < 3; s++) SetTexture9(d, s, nullptr);
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    if (!ps) {
        const DWORD tss[][2] = {{D3DTSS_COLOROP, D3DTOP_SELECTARG1}, {D3DTSS_COLORARG1, D3DTA_TEXTURE},
                                {D3DTSS_ALPHAOP, D3DTOP_SELECTARG1}, {D3DTSS_ALPHAARG1, D3DTA_TEXTURE}};
        for (const auto& t : tss) d->SetTextureStageState(0, D3DTEXTURESTAGESTATETYPE(t[0]), t[1]);
        d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
    const DWORD rs[][2] = {{D3DRS_ZENABLE, FALSE},        {D3DRS_ZWRITEENABLE, FALSE},      {D3DRS_ZFUNC, D3DCMP_ALWAYS},
                           {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_ALPHABLENDENABLE, FALSE},  {D3DRS_ALPHATESTENABLE, FALSE},
                           {D3DRS_FOGENABLE, FALSE},      {D3DRS_STENCILENABLE, FALSE},     {D3DRS_SCISSORTESTENABLE, FALSE},
                           {D3DRS_COLORWRITEENABLE, 0xF}, {D3DRS_FILLMODE, D3DFILL_SOLID},  {D3DRS_SRGBWRITEENABLE, FALSE},
                           {D3DRS_CLIPPLANEENABLE, 0},    {D3DRS_WRAP0, 0},                 {D3DRS_SPECULARENABLE, FALSE},
                           {D3DRS_TEXTUREFACTOR, 0xFFFFFFFF}, {D3DRS_DEPTHBIAS, 0},         {D3DRS_SLOPESCALEDEPTHBIAS, 0}};
    for (const auto& r : rs) d->SetRenderState(D3DRENDERSTATETYPE(r[0]), r[1]);
}

void QuadPass::Target(IDirect3DSurface9* rt, IDirect3DSurface9* depth) {
    d_->SetRenderTarget(0, rt);
    d_->SetDepthStencilSurface(depth);
    D3DSURFACE_DESC desc;
    if (FAILED(rt->GetDesc(&desc))) return;  // (SetRenderTarget made the viewport all of it already)
    const D3DVIEWPORT9 vp = {0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
    d_->SetViewport(&vp);
}

void QuadPass::Texture(DWORD stage, IDirect3DBaseTexture9* t, DWORD filter) {
    SetTexture9(d_, stage, t);
    const DWORD ss[][2] = {{D3DSAMP_MINFILTER, filter},          {D3DSAMP_MAGFILTER, filter},
                           {D3DSAMP_MIPFILTER, D3DTEXF_NONE},    {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP},
                           {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP}, {D3DSAMP_SRGBTEXTURE, FALSE}};
    for (const auto& s : ss) d_->SetSamplerState(stage, D3DSAMPLERSTATETYPE(s[0]), s[1]);
}

bool QuadPass::Draw(const RECT& dst, float u0, float v0, float u1, float v1) {
    const float x0 = float(dst.left) - 0.5f, y0 = float(dst.top) - 0.5f, x1 = float(dst.right) - 0.5f,
                y1 = float(dst.bottom) - 0.5f;
    const float q[4][6] = {{x0, y0, 0, 1, u0, v0}, {x1, y0, 0, 1, u1, v0}, {x0, y1, 0, 1, u0, v1}, {x1, y1, 0, 1, u1, v1}};
    return DrawUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
}

bool QuadPass::Draw(const RECT& dst, const RECT& src, int texW, int texH) {
    const float iw = 1.0f / float(texW), ih = 1.0f / float(texH);
    return Draw(dst, src.left * iw, src.top * ih, src.right * iw, src.bottom * ih);
}

bool QuadPass::DrawUP(D3DPRIMITIVETYPE type, UINT prims, const void* verts, UINT stride) {
    gpu_.OpenScene();
    return SUCCEEDED(d_->DrawPrimitiveUP(type, prims, verts, stride));
}

}  // namespace dd2d9
