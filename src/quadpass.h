// dd2d9's own draws on the shared device (gpu.h): a quad textured from one to three
// textures, through a pixel shader or stage 0's texture as it is, into a render target. The
// antialiasing copy into a multisampled image, the readable Z buffers' reads and writes, the
// colour key of a render-target texture, the presenter's gamma and sharp-bilinear passes and
// the overlays all draw so, in the middle of a game's Direct3D frame: the device state a pass
// changes is put back at its end (SavedDeviceState), so the game's Direct3D context finds its
// state as it left it. The render states SavedDeviceState keeps are all set to what a plain
// copy needs (lighting aside: transformed vertices aren't lit). Gpu lock held, device usable.
#pragma once

#include "gpu.h"

#include <d3d9.h>
#include "devstate.h"

namespace dd2d9 {

class QuadPass {
public:
    // ps: the pixel shader (null: stage 0's texture as it is); fvf: the vertices' format.
    QuadPass(Gpu& gpu, IDirect3DDevice9* d, IDirect3DPixelShader9* ps, DWORD fvf = D3DFVF_XYZRHW | D3DFVF_TEX1);
    QuadPass(const QuadPass&) = delete;
    QuadPass& operator=(const QuadPass&) = delete;

    // The render target (the viewport all of it) and the depth-stencil surface.
    void Target(IDirect3DSurface9* rt, IDirect3DSurface9* depth = nullptr);
    // A stage's texture (0 to 2), clamped, without mipmaps, filtered with `filter`.
    void Texture(DWORD stage, IDirect3DBaseTexture9* t, DWORD filter = D3DTEXF_POINT);
    void State(D3DRENDERSTATETYPE s, DWORD v) { d_->SetRenderState(s, v); }
    // `dst` of the target, from u0,v0-u1,v1 of the texture, pixel centres on texel centres
    // (D3D9's half-pixel offset). The scene opens; the caller closes it where it must.
    bool Draw(const RECT& dst, float u0, float v0, float u1, float v1);
    // From `src` of a texW x texH texture: texel for pixel when they are the same size.
    bool Draw(const RECT& dst, const RECT& src, int texW, int texH);
    // Vertices of the pass's own format.
    bool DrawUP(D3DPRIMITIVETYPE type, UINT prims, const void* verts, UINT stride);

private:
    Gpu& gpu_;
    IDirect3DDevice9* d_;
    const SavedDeviceState saved_;  // first made, last gone: the game's state put back
};

}  // namespace dd2d9
