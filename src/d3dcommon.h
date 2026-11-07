// Helpers shared by dd2d9's Direct3D 7 (d3d7.cpp) and Direct3D 3-6 (d3d6*.cpp): vertex
// formats, primitive counts, matrices, the hardware device's caps from Direct3D 9, and the
// small vector types of the CPU vertex pipelines. Includes d3d.h (never with d3d9.h).
#pragma once
#include "common.h"
#include "gpures.h"
#include "objects.h"

#include <d3d.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <xmmintrin.h>

namespace dd2d9 {

// The Z buffer attached to a render target, or null.
inline Surface* AttachedZ(Surface* rt) {
    for (Surface* a : rt->attached)
        if (a->desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) return a;
    for (const Surface::AttachRef& r : rt->attachRefs)
        if (r.s->desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) return r.s;
    return nullptr;
}

inline DWORD BitsToDDBD(DWORD bpp) {
    switch (bpp) {
    case 8: return DDBD_8;
    case 16: return DDBD_16;
    case 24: return DDBD_24;
    case 32: return DDBD_32;
    default: return 0;
    }
}

// Vertex size of an FVF as d3dim700 computes it (V §8.1 step 4).
inline UINT FvfSize(DWORD fvf) {
    UINT n;
    switch (fvf & 0x0E) {
    case 0x04: n = 16; break;          // XYZRHW
    case 0x06: n = 16; break;          // XYZB1
    case 0x08: n = 20; break;
    case 0x0A: n = 24; break;
    case 0x0C: n = 28; break;
    case 0x0E: n = 32; break;          // XYZB5
    default: n = 12; break;            // XYZ, and position 0
    }
    if (fvf & D3DFVF_NORMAL) n += 12;
    if (fvf & D3DFVF_RESERVED1) n += 4;
    if (fvf & D3DFVF_DIFFUSE) n += 4;
    if (fvf & D3DFVF_SPECULAR) n += 4;
    const DWORD tex = std::min<DWORD>((fvf >> 8) & 0xF, 8);
    for (DWORD i = 0; i < tex; i++) {
        static const UINT kSize[] = {8, 12, 16, 4};
        n += kSize[(fvf >> (16 + 2 * i)) & 3];
    }
    return n;
}

// GetNumPrim (V §1 step 4); 0 for an unknown type.
inline UINT PrimCount(D3DPRIMITIVETYPE t, DWORD n) {
    switch (t) {
    case D3DPT_POINTLIST: return n;
    case D3DPT_LINELIST: return n / 2;
    case D3DPT_LINESTRIP: return n >= 2 ? n - 1 : 0;
    case D3DPT_TRIANGLELIST: return n / 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return n >= 3 ? n - 2 : 0;
    default: return 0;
    }
}

inline bool ValidPrim(D3DPRIMITIVETYPE t) { return t >= D3DPT_POINTLIST && t <= D3DPT_TRIANGLEFAN; }

inline const D3DMATRIX kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

inline void MatMul(D3DMATRIX* out, const D3DMATRIX& a, const D3DMATRIX& b) {  // out = a x b
    const float* x = &a._11;
    const float* y = &b._11;
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = x[i * 4] * y[j] + x[i * 4 + 1] * y[4 + j] + x[i * 4 + 2] * y[8 + j] + x[i * 4 + 3] * y[12 + j];
    memcpy(&out->_11, r, sizeof(r));
}

// The hardware devices from the Direct3D 9 adapter (D, implementation notes).
inline D3DDEVICEDESC7 HalDesc(bool tnl) {
    const GpuCaps c = GpuGetCaps();
    D3DDEVICEDESC7 d = {};
    d.dwDevCaps = (c.devCaps & 0x000BFFF0) | D3DDEVCAPS_FLOATTLVERTEX | D3DDEVCAPS_DRAWPRIMITIVES2 |
                  D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWRASTERIZATION;
    if (tnl) d.dwDevCaps |= D3DDEVCAPS_HWTRANSFORMANDLIGHT;
    else d.dwDevCaps &= ~DWORD(D3DDEVCAPS_HWTRANSFORMANDLIGHT);
    D3DPRIMCAPS p = {};
    p.dwSize = sizeof(D3DPRIMCAPS);
    p.dwMiscCaps = c.primMiscCaps & (D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW);
    p.dwRasterCaps = (c.rasterCaps & 0x0037A091) | D3DPRASTERCAPS_SUBPIXEL;  // DITHER ZTEST FOG* LODBIAS FOGRANGE ANISOTROPY WBUFFER WFOG ZFOG
    if (c.rasterCaps & 0x04000000) p.dwRasterCaps |= D3DPRASTERCAPS_ZBIAS;  // D3DPRASTERCAPS_DEPTHBIAS
    if (g_config.wbuffer) p.dwRasterCaps |= D3DPRASTERCAPS_WBUFFER;  // emulated where D3D9 lacks it (gpures.cpp)
    p.dwZCmpCaps = c.zCmpCaps & 0xFF;
    p.dwSrcBlendCaps = c.srcBlendCaps & 0x1FFF;
    p.dwDestBlendCaps = c.destBlendCaps & 0x1FFF;
    p.dwAlphaCmpCaps = c.alphaCmpCaps & 0xFF;
    p.dwShadeCaps = c.shadeCaps | 0xC528A;
    // Cube maps where D3D9 has them (gpures.cpp: EnsureCube).
    p.dwTextureCaps = (c.textureCaps & (0x5E7 | D3DPTEXTURECAPS_CUBEMAP)) | D3DPTEXTURECAPS_TRANSPARENCY;
    if (!g_config.palTextures) p.dwTextureCaps &= ~DWORD(D3DPTEXTURECAPS_ALPHAPALETTE);  // (no palettized textures)
    DWORD f = c.textureFilterCaps & 0x07030700;
    if (f & D3DPTFILTERCAPS_MINFPOINT) f |= D3DPTFILTERCAPS_NEAREST;
    if (f & D3DPTFILTERCAPS_MINFLINEAR) f |= D3DPTFILTERCAPS_LINEAR;
    if (f & D3DPTFILTERCAPS_MIPFPOINT) f |= D3DPTFILTERCAPS_MIPNEAREST | D3DPTFILTERCAPS_LINEARMIPNEAREST;
    if (f & D3DPTFILTERCAPS_MIPFLINEAR) f |= D3DPTFILTERCAPS_MIPLINEAR | D3DPTFILTERCAPS_LINEARMIPLINEAR;
    p.dwTextureFilterCaps = f;
    p.dwTextureBlendCaps = 0xCF;
    p.dwTextureAddressCaps = c.textureAddressCaps & 0x1F;
    d.dpcLineCaps = d.dpcTriCaps = p;
    d.dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
    d.dwDeviceZBufferBitDepth = (c.depth16 ? DDBD_16 : 0) | (c.depth24x8 || c.depth24s8 ? DDBD_24 : 0) |
                                (c.depth32 || c.depth24x8 ? DDBD_32 : 0);
    d.dwMinTextureWidth = d.dwMinTextureHeight = 1;
    d.dwMaxTextureWidth = std::min<DWORD>(c.maxTextureWidth, 4096);
    d.dwMaxTextureHeight = std::min<DWORD>(c.maxTextureHeight, 4096);
    d.dwMaxTextureRepeat = c.maxTextureRepeat;
    d.dwMaxTextureAspectRatio = c.maxTextureAspectRatio;
    d.dwMaxAnisotropy = c.maxAnisotropy;
    d.dvGuardBandLeft = c.guardBandLeft;
    d.dvGuardBandTop = c.guardBandTop;
    d.dvGuardBandRight = c.guardBandRight;
    d.dvGuardBandBottom = c.guardBandBottom;
    d.dvExtentsAdjust = c.extentsAdjust;
    d.dwStencilCaps = c.stencilCaps & 0xFF;
    d.dwFVFCaps = std::min<DWORD>(c.fvfCaps & 0xFFFF, 8) | (c.fvfCaps & D3DFVFCAPS_DONOTSTRIPELEMENTS);
    d.dwTextureOpCaps = c.textureOpCaps & 0x00FFFFFF;
    d.wMaxTextureBlendStages = WORD(std::min<DWORD>(c.maxTextureBlendStages, 8));
    d.wMaxSimultaneousTextures = WORD(std::min<DWORD>(c.maxSimultaneousTextures, 8));
    d.dvMaxVertexW = c.maxVertexW;
    if (tnl) {
        d.deviceGUID = IID_IDirect3DTnLHalDevice;
        d.dwMaxActiveLights = c.maxActiveLights;
        d.wMaxUserClipPlanes = WORD(c.maxUserClipPlanes);
        d.wMaxVertexBlendMatrices = WORD(c.maxVertexBlendMatrices == 1 ? 0 : c.maxVertexBlendMatrices);
        d.dwVertexProcessingCaps = c.vertexProcessingCaps & 0x3F;
    } else {
        d.deviceGUID = IID_IDirect3DHALDevice;
        d.dwMaxActiveLights = 0xFFFFFFFF;
        d.wMaxUserClipPlanes = 6;
        d.wMaxVertexBlendMatrices = 4;
        d.dwVertexProcessingCaps = 0x3F;
    }
    return d;
}

struct Vec3 {
    float x, y, z;
};
inline Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Norm(Vec3 a) {
    const float l = sqrtf(Dot(a, a));
    return l > 0 ? Vec3{a.x / l, a.y / l, a.z / l} : a;
}
inline Vec3 XformPoint(Vec3 p, const D3DMATRIX& m) {
    return {p.x * m._11 + p.y * m._21 + p.z * m._31 + m._41, p.x * m._12 + p.y * m._22 + p.z * m._32 + m._42,
            p.x * m._13 + p.y * m._23 + p.z * m._33 + m._43};
}
inline Vec3 XformDir(Vec3 p, const D3DMATRIX& m) {
    return {p.x * m._11 + p.y * m._21 + p.z * m._31, p.x * m._12 + p.y * m._22 + p.z * m._32,
            p.x * m._13 + p.y * m._23 + p.z * m._33};
}
struct Color {
    float r, g, b, a;
};
inline Color FromArgb(DWORD c) {
    return {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, (c >> 24) / 255.0f};
}
inline Color FromCv(const D3DCOLORVALUE& c) { return {c.r, c.g, c.b, c.a}; }
// A colour value (0 to 255.5) to its integer, truncated, as x86 converts: a NaN (which
// std::clamp lets through) gives 0x80000000. (DWORD(NaN) is undefined: compilers give that on
// x86 today, but needn't.)
inline DWORD TruncToDword(float v) { return DWORD(_mm_cvtt_ss2si(_mm_set_ss(v))); }

inline DWORD ToArgb(Color c) {
    auto b = [](float v) { return TruncToDword(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (b(c.a) << 24) | (b(c.r) << 16) | (b(c.g) << 8) | b(c.b);
}

// Where each element of an FVF vertex is (offsets in bytes; -1: absent).
struct Layout {
    int pos = 0, normal = -1, diffuse = -1, specular = -1;
    int tex[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    UINT texSize[8] = {};
    DWORD texCount = 0;
    explicit Layout(DWORD fvf) {
        int o = int(FvfSize(fvf & 0x0E));
        if (fvf & D3DFVF_NORMAL) { normal = o; o += 12; }
        if (fvf & D3DFVF_RESERVED1) o += 4;
        if (fvf & D3DFVF_DIFFUSE) { diffuse = o; o += 4; }
        if (fvf & D3DFVF_SPECULAR) { specular = o; o += 4; }
        texCount = std::min<DWORD>((fvf >> 8) & 0xF, 8);
        for (DWORD i = 0; i < texCount; i++) {
            static const UINT kSize[] = {8, 12, 16, 4};
            tex[i] = o;
            texSize[i] = kSize[(fvf >> (16 + 2 * i)) & 3];
            o += int(texSize[i]);
        }
    }
};

}  // namespace dd2d9
