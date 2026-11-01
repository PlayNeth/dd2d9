// dd2d9's own draws on the shared device in the middle of a game's Direct3D frame (the
// presenter's gamma and sharp-bilinear passes, CopyToMs): the device state they change,
// saved first and put back after, so the game's Direct3D context finds its state as it
// left it (rather than sending all of it again, some 450 calls, every frame). Only the
// states those draws set are kept: render target 0 and the depth buffer, the viewport,
// the shaders and vertex declaration, stages 0-1 (textures, samplers, a few stage states)
// and the render states in kStates. Include after d3d9.h.
#pragma once

#include <d3d9.h>

namespace dd2d9 {

class SavedDeviceState {
public:
    explicit SavedDeviceState(IDirect3DDevice9* d) : d_(d) {
        d->GetRenderTarget(0, &rt_);
        d->GetDepthStencilSurface(&z_);
        d->GetViewport(&vp_);
        d->GetPixelShader(&ps_);
        d->GetVertexShader(&vs_);
        d->GetVertexDeclaration(&decl_);
        d->GetFVF(&fvf_);
        for (DWORD s = 0; s < kTexStages; s++) {
            d->GetTexture(s, &tex_[s]);
            for (DWORD i = 0; i < kSamplerCount; i++) d->GetSamplerState(s, kSamplers[i], &samp_[s][i]);
        }
        for (DWORD i = 0; i < kStageCount; i++) d->GetTextureStageState(kStages[i].stage, kStages[i].type, &stage_[i]);
        for (DWORD i = 0; i < kStateCount; i++) d->GetRenderState(kStates[i], &rs_[i]);
    }
    ~SavedDeviceState() {
        IDirect3DDevice9* d = d_;
        for (DWORD i = 0; i < kStateCount; i++) d->SetRenderState(kStates[i], rs_[i]);
        for (DWORD i = 0; i < kStageCount; i++) d->SetTextureStageState(kStages[i].stage, kStages[i].type, stage_[i]);
        for (DWORD s = 0; s < kTexStages; s++) {
            for (DWORD i = 0; i < kSamplerCount; i++) d->SetSamplerState(s, kSamplers[i], samp_[s][i]);
            d->SetTexture(s, tex_[s]);
            if (tex_[s]) tex_[s]->Release();
        }
        d->SetPixelShader(ps_);
        d->SetVertexShader(vs_);
        if (decl_) d->SetVertexDeclaration(decl_);
        else d->SetFVF(fvf_);
        if (ps_) ps_->Release();
        if (vs_) vs_->Release();
        if (decl_) decl_->Release();
        if (rt_) {
            d->SetRenderTarget(0, rt_);  // (resets the viewport: set again below)
            rt_->Release();
        }
        d->SetDepthStencilSurface(z_);
        if (z_) z_->Release();
        d->SetViewport(&vp_);
    }
    SavedDeviceState(const SavedDeviceState&) = delete;
    SavedDeviceState& operator=(const SavedDeviceState&) = delete;

private:
    static constexpr D3DRENDERSTATETYPE kStates[] = {
        D3DRS_ZENABLE,         D3DRS_ZWRITEENABLE,      D3DRS_CULLMODE,         D3DRS_ALPHABLENDENABLE,
        D3DRS_ALPHATESTENABLE, D3DRS_FOGENABLE,         D3DRS_STENCILENABLE,    D3DRS_SCISSORTESTENABLE,
        D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE,         D3DRS_SRGBWRITEENABLE,  D3DRS_CLIPPLANEENABLE,
        D3DRS_WRAP0,           D3DRS_SPECULARENABLE,    D3DRS_TEXTUREFACTOR,    D3DRS_LIGHTING,
        D3DRS_DEPTHBIAS,       D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_ZFUNC};
    static constexpr DWORD kStateCount = sizeof(kStates) / sizeof(kStates[0]);
    static constexpr D3DSAMPLERSTATETYPE kSamplers[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER,  D3DSAMP_MIPFILTER,
                                                        D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,   D3DSAMP_SRGBTEXTURE};
    static constexpr DWORD kSamplerCount = sizeof(kSamplers) / sizeof(kSamplers[0]);
    struct Stage {
        DWORD stage;
        D3DTEXTURESTAGESTATETYPE type;
    };
    static constexpr Stage kStages[] = {{0, D3DTSS_COLOROP},       {0, D3DTSS_COLORARG1}, {0, D3DTSS_ALPHAOP},
                                        {0, D3DTSS_ALPHAARG1},     {0, D3DTSS_TEXCOORDINDEX},
                                        {0, D3DTSS_TEXTURETRANSFORMFLAGS}, {1, D3DTSS_COLOROP}, {1, D3DTSS_ALPHAOP}};
    static constexpr DWORD kTexStages = 3;  // textures and samplers kept (the overlays' pass uses three)
    static constexpr DWORD kStageCount = sizeof(kStages) / sizeof(kStages[0]);

    IDirect3DDevice9* d_;
    IDirect3DSurface9* rt_ = nullptr;
    IDirect3DSurface9* z_ = nullptr;
    D3DVIEWPORT9 vp_ = {};
    IDirect3DPixelShader9* ps_ = nullptr;
    IDirect3DVertexShader9* vs_ = nullptr;
    IDirect3DVertexDeclaration9* decl_ = nullptr;
    DWORD fvf_ = 0;
    IDirect3DBaseTexture9* tex_[kTexStages] = {};
    DWORD samp_[kTexStages][kSamplerCount] = {};
    DWORD stage_[kStageCount] = {};
    DWORD rs_[kStateCount] = {};
};

}  // namespace dd2d9
