// The presenter's GPU passes over a frame: the gamma ramp, sharp bilinear scaling, and the
// draw call for overlay tools (see presenter.h).
#include "objects.h"
#include "gpu.h"

#include <d3d9.h>
#include "quadpass.h"

namespace dd2d9 {

// ps_2_0: each channel of s0 looked up in s1, a 256x1 ramp (texel i = the ramp's red, green
// and blue for i). Assembled from:
//   def c0, 255/256, 0.5/256, 0.5, 1 / dcl t0.xy / dcl_2d s0 / dcl_2d s1 / texld r0, t0, s0
//   r1..r3 = (r0.x|y|z * c0.x + c0.y, 0.5) / texld r4..r6, r1..r3, s1
//   oC0 = (r4.x, r5.y, r6.z, 1)
static const DWORD kGammaPs[] = {0xffff0200, 0x0200001f, 0x80000000, 0xb0030000, 0x0200001f, 0x90000000, 0xa00f0800, 0x0200001f, 0x90000000, 0xa00f0801, 0x05000051, 0xa00f0000, 0x3f7f0000, 0x3b000000, 0x3f000000, 0x3f800000, 0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x02000001, 0x800f0001, 0xa0aa0000, 0x02000001, 0x800f0002, 0xa0aa0000, 0x02000001, 0x800f0003, 0xa0aa0000, 0x04000004, 0x80010001, 0x80000000, 0xa0000000, 0xa0550000, 0x04000004, 0x80010002, 0x80550000, 0xa0000000, 0xa0550000, 0x04000004, 0x80010003, 0x80aa0000, 0xa0000000, 0xa0550000, 0x03000042, 0x800f0004, 0x80e40001, 0xa0e40801, 0x03000042, 0x800f0005, 0x80e40002, 0xa0e40801, 0x03000042, 0x800f0006, 0x80e40003, 0xa0e40801, 0x02000001, 0x800f0007, 0xa0ff0000, 0x02000001, 0x80010007, 0x80000004, 0x02000001, 0x80020007, 0x80550005, 0x02000001, 0x80040007, 0x80aa0006, 0x02000001, 0x800f0800, 0x80e40007, 0x0000ffff};

// The frame (src's rectangle r) with the gamma ramp applied, or null (shown without it).
// Gpu lock held, outside a scene. The device's state is a GpuContext's: what this changes
// is put back after (SavedDeviceState).
IDirect3DSurface9* Presenter::ApplyGammaGpu(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT& r) {
    if (gammaFailed_ || gammaLutFailed_) return nullptr;
    const UINT w = UINT(r.right - r.left), h = UINT(r.bottom - r.top);
    if (!gammaPs_ && FAILED(d->CreatePixelShader(kGammaPs, &gammaPs_))) {
        gammaPs_ = nullptr;
        gammaFailed_ = true;
        DD2D9_LOG("gamma: no pixel shader (the ramp isn't applied to Direct3D frames)");
        return nullptr;
    }
    if (!gammaLutTex_ && FAILED(d->CreateTexture(256, 1, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &gammaLutTex_, nullptr))) {
        gammaLutTex_ = nullptr;
        gammaLutFailed_ = true;
        DD2D9_LOG("gamma: no ramp texture (the ramp isn't applied to Direct3D frames)");
        return nullptr;
    }
    if (gammaW_ != w || gammaH_ != h || !gammaSrc_ || !gammaOut_) {
        if (gammaFail_.Is(w, h)) return nullptr;  // (not tried again every frame)
        if (gammaSrc_) gammaSrc_->Release();
        if (gammaOut_) gammaOut_->Release();
        gammaSrc_ = nullptr;
        gammaOut_ = nullptr;
        gammaW_ = gammaH_ = 0;
        if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &gammaSrc_, nullptr)) ||
            FAILED(d->CreateRenderTarget(w, h, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &gammaOut_, nullptr))) {
            if (gammaSrc_) gammaSrc_->Release();
            gammaSrc_ = nullptr;
            gammaOut_ = nullptr;
            gammaFail_.Set(w, h);
            return nullptr;
        }
        gammaW_ = w;
        gammaH_ = h;
    }
    EnterCriticalSection(&gammaLock_);
    if (gammaTexVersion_ != gammaVersion_) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(gammaLutTex_->LockRect(0, &lr, nullptr, 0))) {
            auto* t = static_cast<uint32_t*>(lr.pBits);
            for (int i = 0; i < 256; i++)
                t[i] = 0xFF000000u | (uint32_t(gammaLut_[0][i]) << 16) | (uint32_t(gammaLut_[1][i]) << 8) | gammaLut_[2][i];
            gammaLutTex_->UnlockRect(0);
            gammaTexVersion_ = gammaVersion_;
        }
    }
    LeaveCriticalSection(&gammaLock_);
    IDirect3DSurface9* level = nullptr;
    if (FAILED(gammaSrc_->GetSurfaceLevel(0, &level))) return nullptr;
    d->StretchRect(src, &r, level, nullptr, D3DTEXF_POINT);
    level->Release();
    Gpu& gpu = dd_->GetGpu();
    QuadPass pass(gpu, d, gammaPs_);
    pass.Target(gammaOut_);
    pass.Texture(0, gammaSrc_);
    pass.Texture(1, gammaLutTex_);
    pass.Draw(RECT{0, 0, LONG(w), LONG(h)}, 0, 0, 1, 1);
    gpu.CloseScene();
    return gammaOut_;
}

// filter=sharp without pixel shaders: the frame enlarged by a whole factor (a render
// target: StretchRect filters only into one).
bool Presenter::EnsurePrescaled(IDirect3DDevice9* d, UINT w, UINT h) {
    if (prescaled_ && preW_ == w && preH_ == h) return true;
    if (preFail_.Is(w, h)) return false;  // (not tried again every frame)
    if (prescaled_) prescaled_->Release();
    prescaled_ = nullptr;
    preW_ = preH_ = 0;
    if (FAILED(d->CreateRenderTarget(w, h, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &prescaled_, nullptr))) {
        prescaled_ = nullptr;
        preFail_.Set(w, h);
        return false;
    }
    preW_ = w;
    preH_ = h;
    return true;
}

// ps_2_0, sharp bilinear: within each source pixel, flat colour, blended with the neighbour
// only over the last output pixel at its edge (libretro's sharp-bilinear-simple). Any
// enlargement, whole or not. c0 = source size, c1 = 0.5 - 0.5/scale (the flat region's
// half-width), c5 = -c1, c2 = scale, c3 = 1/source size. Assembled from:
//   def c4, 0.5, 0, -0.5, 0 / dcl t0 / dcl_2d s0 / mov r0..r3, c4.y
//   r0.xy = t0 * c0 (the texel position) / r1.xy = frc(r0) / r0.xy -= r1 / r1.xy += c4.z
//   r2.xy = min(max(r1, c5), c1) / r1.xy = (r1 - r2) * c2 + c4.x / r3.xy = (r0 + r1) * c3
//   texld r0, r3, s0 / oC0 = r0
// Written to stay clear of everything Windows' ps_2_0 validator may refuse (Wine checks
// none of it): every register set whole before it is read, no negated constants, one
// constant per instruction, the texture coordinate declared whole.
static const DWORD kSharpPs[] = {0xffff0200,0x0200001f,0x80000000,0xb00f0000,0x0200001f,0x90000000,
0xa00f0800,0x05000051,0xa00f0004,0x3f000000,0x00000000,0xbf000000,
0x00000000,0x02000001,0x800f0000,0xa0550004,0x02000001,0x800f0001,
0xa0550004,0x02000001,0x800f0002,0xa0550004,0x02000001,0x800f0003,
0xa0550004,0x03000005,0x80030000,0xb0e40000,0xa0e40000,0x02000013,
0x80030001,0x80e40000,0x03000002,0x80030000,0x80e40000,0x81e40001,
0x03000002,0x80030001,0x80e40001,0xa0aa0004,0x0300000b,0x80030002,
0x80e40001,0xa0e40005,0x0300000a,0x80030002,0x80e40002,0xa0e40001,
0x03000002,0x80030001,0x80e40001,0x81e40002,0x03000005,0x80030001,
0x80e40001,0xa0e40002,0x03000002,0x80030001,0x80e40001,0xa0000004,
0x03000002,0x80030000,0x80e40000,0x80e40001,0x03000005,0x80030003,
0x80e40000,0xa0e40003,0x03000042,0x800f0000,0x80e40003,0xa0e40800,
0x02000001,0x800f0800,0x80e40000,0x0000ffff,
};

// The frame (src's rectangle r) drawn into bb's rectangle dst through kSharpPs; false when it
// can't be (no pixel shaders), for plain linear filtering. Gpu lock held, outside a scene.
bool Presenter::DrawSharp(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT& r, IDirect3DSurface9* bb, const RECT& dst) {
    if (sharpFailed_) return false;
    const UINT w = UINT(r.right - r.left), h = UINT(r.bottom - r.top);
    if (!sharpPs_) {
        const HRESULT hr = d->CreatePixelShader(kSharpPs, &sharpPs_);
        if (FAILED(hr)) {
            sharpPs_ = nullptr;
            sharpFailed_ = true;
            D3DCAPS9 caps = {};
            d->GetDeviceCaps(&caps);
            DD2D9_LOG("filter=sharp: no pixel shader (0x%08lx; the device: pixel shaders %lu.%lu), point scaling then "
                      "linear instead",
                      hr, static_cast<unsigned long>((caps.PixelShaderVersion >> 8) & 0xFF),
                      static_cast<unsigned long>(caps.PixelShaderVersion & 0xFF));
            return false;
        }
    }
    if (!sharpTex_ || sharpW_ != w || sharpH_ != h) {
        if (sharpFail_.Is(w, h)) return false;  // (not tried again every frame)
        if (sharpTex_) sharpTex_->Release();
        sharpTex_ = nullptr;
        sharpW_ = sharpH_ = 0;
        if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &sharpTex_, nullptr))) {
            sharpTex_ = nullptr;
            sharpFail_.Set(w, h);
            return false;
        }
        sharpW_ = w;
        sharpH_ = h;
    }
    IDirect3DSurface9* level = nullptr;
    if (FAILED(sharpTex_->GetSurfaceLevel(0, &level))) return false;
    d->StretchRect(src, &r, level, nullptr, D3DTEXF_POINT);
    level->Release();
    Gpu& gpu = dd_->GetGpu();
    QuadPass pass(gpu, d, sharpPs_);
    pass.Target(bb);
    pass.Texture(0, sharpTex_, D3DTEXF_LINEAR);
    const float dw = float(dst.right - dst.left), dh = float(dst.bottom - dst.top);
    const float sx = dw / float(w), sy = dh / float(h);
    const float rx = 0.5f - 0.5f / sx, ry = 0.5f - 0.5f / sy;
    const float c[6][4] = {{float(w), float(h), 0, 0}, {rx, ry, 0, 0}, {sx, sy, 0, 0},
                           {1.0f / float(w), 1.0f / float(h), 0, 0}, {0.5f, 0, -0.5f, 0}, {-rx, -ry, 0, 0}};
    d->SetPixelShaderConstantF(0, &c[0][0], 6);
    pass.Draw(dst, 0, 0, 1, 1);
    gpu.CloseScene();
    return true;
}

// One draw call in a frame that had none, which draws nothing: RivaTuner (and other overlay
// tools) draws its overlay only into frames that had one, and a 2D game's are only copied into
// the back buffer (StretchRect): it showed in Direct3D games and with filter=sharp (a draw),
// not otherwise. A triangle of no area, colour writes off. Gpu lock held, outside a scene.
void DrawCall(Gpu& gpu, IDirect3DDevice9* d) {
    IDirect3DSurface9* bb = gpu.TargetBackBuffer();
    if (!bb) return;
    QuadPass pass(gpu, d, nullptr, D3DFVF_XYZRHW);
    pass.Target(bb);
    bb->Release();
    pass.State(D3DRS_COLORWRITEENABLE, 0);
    const float t[3][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
    pass.DrawUP(D3DPT_TRIANGLELIST, 1, t, sizeof(t[0]));
    gpu.CloseScene();
}

}  // namespace dd2d9
