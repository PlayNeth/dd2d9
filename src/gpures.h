// Direct3D's use of the shared Direct3D 9 device (gpu.h): the GPU copies of surfaces and
// the drawing context of a Direct3D 7 device. The Direct3D 7 side (d3d7.cpp) includes
// d3d.h, which conflicts with d3d9.h, so this interface uses dd2d9's own types and passes
// Direct3D 9 enumeration values as plain numbers (d3d7.cpp maps the Direct3D 7 ones).
// Implemented in gpures.cpp. See re/d3d-design.md.
//
// Locking: g_lock first (surfaces are read), then the Gpu lock, for every call here.
#pragma once
#include "common.h"

namespace dd2d9 {

class Gpu;
class Surface;
struct Buffer;
struct GpuSurf;
struct GpuTex;

// ---------------------------------------------------------------- surfaces' GPU copies
// A surface's pixels live in its Buffer. A render target or Z buffer Direct3D draws into
// gets a GPU copy there too (Buffer::gpu, travelling with the pixels when Flip swaps
// buffers); a texture gets one on its top-level surface (Surface::gpuTex), covering its
// mipmap levels. Buffer::gpuStale: the CPU copy changed since the GPU one was made;
// Buffer::cpuStale: the GPU copy changed since the CPU one was read back.

// The CPU copy is about to be read (and written, with `write`): read the GPU copy back if
// it is newer. Surfaces without a GPU copy cost nothing. g_lock held; takes the Gpu lock.
void GpuCpuAccess(Surface* s, bool write);
// A Blt DDBLT_DEPTHFILL into a Z buffer with a GPU copy: done there (true), or not (false).
bool GpuDepthFill(Surface* s, const RECT& r, DWORD value);
// Buffer::Release / the Surface destructor: their GPU copies go. g_lock held.
void GpuBufferGone(Buffer* b);
void GpuSurfaceGone(Surface* s);

// ---------------------------------------------------------------- caps
// What Direct3D 7 reports for its hardware devices, from the Direct3D 9 adapter.
struct GpuCaps {
    bool valid = false;
    DWORD devCaps = 0, primMiscCaps = 0, rasterCaps = 0, zCmpCaps = 0, srcBlendCaps = 0, destBlendCaps = 0;
    DWORD alphaCmpCaps = 0, shadeCaps = 0, textureCaps = 0, textureFilterCaps = 0, textureAddressCaps = 0;
    DWORD maxTextureWidth = 0, maxTextureHeight = 0, maxTextureRepeat = 0, maxTextureAspectRatio = 0;
    DWORD maxAnisotropy = 0;
    float guardBandLeft = 0, guardBandTop = 0, guardBandRight = 0, guardBandBottom = 0, extentsAdjust = 0;
    float maxVertexW = 0;
    DWORD stencilCaps = 0, fvfCaps = 0, textureOpCaps = 0;
    DWORD maxTextureBlendStages = 0, maxSimultaneousTextures = 0, maxActiveLights = 0;
    DWORD maxUserClipPlanes = 0, maxVertexBlendMatrices = 0, vertexProcessingCaps = 0;
    bool hwTnL = false;
    bool depth16 = false, depth24x8 = false, depth24s8 = false, depth32 = false;
    bool dxt[5] = {};  // DXT1-5 textures
};
GpuCaps GpuGetCaps();  // cached after the first call; any thread

// ---------------------------------------------------------------- drawing
// The Direct3D 9 side of one Direct3D 7 device. Every call with g_lock and the Gpu lock
// held. Values are Direct3D 9's (render states, stage and sampler states, transform types).
class GpuContext {
public:
    explicit GpuContext(Gpu& gpu);
    ~GpuContext();
    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;

    // The device, usable now. false while it is lost or can't be made: the call does
    // nothing (Direct3D 7 still reports success, as its surfaces will be lost anyway).
    // *fresh: the device is new or was reset, so every state must be sent again.
    bool Begin(bool* fresh);

    void SetRenderState(DWORD state, DWORD value);
    void SetTextureStageState(DWORD stage, DWORD type, DWORD value);
    void SetSamplerState(DWORD stage, DWORD type, DWORD value);
    void SetTransform(DWORD type, const float* m16);
    void SetViewport(DWORD x, DWORD y, DWORD w, DWORD h, float minZ, float maxZ);
    void SetMaterial(const void* material);  // D3DMATERIAL7 = D3DMATERIAL9
    void SetLight(DWORD index, const void* light);  // D3DLIGHT7 = D3DLIGHT9
    void LightEnable(DWORD index, bool on);
    void SetClipPlane(DWORD index, const float* plane);
    void SetTexture(DWORD stage, Surface* top);  // bound (and uploaded) at the next draw
    void SetTargets(Surface* rt, Surface* z);
    void SetColorKeyEnable(bool on);  // D3DRENDERSTATE_COLORKEYENABLE: textures' keys become alpha

    // flags: D3DCLEAR_TARGET 1, ZBUFFER 2, STENCIL 4. rects as RECT (= D3DRECT), clipped
    // to the viewport already; n = 0: the whole viewport.
    void Clear(DWORD n, const RECT* rects, DWORD flags, DWORD color, float z, DWORD stencil);
    // A Direct3D 7 FVF (D3DFVF_RESERVED1 is skipped, not D3D9's PSIZE). indices: 16-bit.
    void Draw(DWORD primType, DWORD fvf, const void* verts, UINT stride, UINT vertexCount, const WORD* indices,
              UINT indexCount, UINT primCount, bool zEnabled);
    void EndScene();  // the game's EndScene: D3D9's scene closes (presentation may follow)

private:
    struct Impl;
    Impl* p_;
};

}  // namespace dd2d9
