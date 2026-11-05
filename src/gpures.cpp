// Direct3D's use of the shared Direct3D 9 device: surfaces' GPU copies and the drawing
// context of a Direct3D 7 device (see gpures.h).
#include "gpures.h"

#include "convert.h"

#include "gpu.h"
#include "objects.h"

#include <d3d9.h>
#include "devstate.h"

#include <algorithm>
#include <climits>
#include <cfloat>
#include <cmath>
#include <emmintrin.h>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace dd2d9 {

// ---------------------------------------------------------------- formats

// The Direct3D 9 format with this memory layout, or D3DFMT_UNKNOWN (converted to
// A8R8G8B8 on the way to the GPU and back).
static D3DFORMAT NativeFormat(const Format& f) {
    if (f.fourcc) {
        switch (f.fourCC) {
        case MAKEFOURCC('D', 'X', 'T', '1'): return D3DFMT_DXT1;
        case MAKEFOURCC('D', 'X', 'T', '2'): return D3DFMT_DXT2;
        case MAKEFOURCC('D', 'X', 'T', '3'): return D3DFMT_DXT3;
        case MAKEFOURCC('D', 'X', 'T', '4'): return D3DFMT_DXT4;
        case MAKEFOURCC('D', 'X', 'T', '5'): return D3DFMT_DXT5;
        default: return D3DFMT_UNKNOWN;
        }
    }
    if (f.bump) {
        if (f.bpp == 24) return D3DFMT_X8L8V8U8;  // (widened: see WriteLevel)
        return f.b ? D3DFMT_L6V5U5 : D3DFMT_V8U8;
    }
    if (f.pal8 || f.zbuf) return D3DFMT_UNKNOWN;
    if (f.bpp == 16) {
        if (f.r == 0xF800 && f.g == 0x07E0 && f.b == 0x001F && !f.a) return D3DFMT_R5G6B5;
        if (f.r == 0x7C00 && f.g == 0x03E0 && f.b == 0x001F) return f.a == 0x8000 ? D3DFMT_A1R5G5B5 : f.a ? D3DFMT_UNKNOWN : D3DFMT_X1R5G5B5;
        if (f.r == 0x0F00 && f.g == 0x00F0 && f.b == 0x000F) return f.a == 0xF000 ? D3DFMT_A4R4G4B4 : f.a ? D3DFMT_UNKNOWN : D3DFMT_X4R4G4B4;
    }
    if (f.bpp == 32 && f.r == 0xFF0000 && f.g == 0xFF00 && f.b == 0xFF)
        return f.a == 0xFF000000u ? D3DFMT_A8R8G8B8 : f.a ? D3DFMT_UNKNOWN : D3DFMT_X8R8G8B8;
    return D3DFMT_UNKNOWN;
}

static bool IsRenderTargetFormat(D3DFORMAT f) {
    return f == D3DFMT_R5G6B5 || f == D3DFMT_X1R5G5B5 || f == D3DFMT_A1R5G5B5 || f == D3DFMT_X8R8G8B8 ||
           f == D3DFMT_A8R8G8B8;
}

static inline uint32_t ReadPixel(const uint8_t* p, DWORD bytes) {
    switch (bytes) {
    case 1: return p[0];
    case 2: return *reinterpret_cast<const uint16_t*>(p);
    case 3: return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
    default: return *reinterpret_cast<const uint32_t*>(p);
    }
}

static inline void WritePixel(uint8_t* p, DWORD bytes, uint32_t v) {
    switch (bytes) {
    case 1: p[0] = uint8_t(v); break;
    case 2: *reinterpret_cast<uint16_t*>(p) = uint16_t(v); break;
    case 3: p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); break;
    default: *reinterpret_cast<uint32_t*>(p) = v; break;
    }
}

// One row of the CPU copy to the GPU's memory: as is (same layout), or to A8R8G8B8 through
// `conv` (the palette; a colour-keyed texel becomes transparent black, as DX7 hardware treated it).
static void RowToGpu(const Buffer* b, int y, uint8_t* dst, const RowConverter* conv) {
    if (!conv) memcpy(dst, b->Row(y), size_t(b->w) * b->fmt.Bytes());
    else conv->Run(b->Row(y), reinterpret_cast<uint32_t*>(dst), b->w);
}

// A palettized render target: Direct3D 9 draws in X8R8G8B8, and each pixel read back
// becomes the palette's entry for its colour: the exact one when the palette has it, else
// the nearest (by RGB distance, cached per 15-bit colour).
struct PaletteMap {
    PALETTEENTRY pal[256] = {};
    std::unordered_map<uint32_t, uint8_t> exact;
    std::vector<int16_t> nearest;  // 32768 entries, -1 until computed
    void Set(const PALETTEENTRY* p) {
        if (!memcmp(pal, p, sizeof(pal)) && !exact.empty()) return;
        memcpy(pal, p, sizeof(pal));
        exact.clear();
        for (int i = 255; i >= 0; i--) exact[(uint32_t(p[i].peRed) << 16) | (uint32_t(p[i].peGreen) << 8) | p[i].peBlue] = uint8_t(i);
        nearest.assign(32768, -1);
    }
    uint8_t Index(uint32_t argb) {
        const uint32_t rgb = argb & 0xFFFFFF;
        auto it = exact.find(rgb);
        if (it != exact.end()) return it->second;
        const uint32_t key = ((rgb >> 9) & 0x7C00) | ((rgb >> 6) & 0x3E0) | ((rgb >> 3) & 0x1F);
        if (nearest[key] < 0) {  // from the bucket's centre: the same answer whatever comes first
            const int r = int((key >> 10) << 3 | 4), g = int(((key >> 5) & 31) << 3 | 4), b = int((key & 31) << 3 | 4);
            int best = 0, bestD = INT_MAX;
            for (int i = 0; i < 256; i++) {
                const int dr = r - pal[i].peRed, dg = g - pal[i].peGreen, db = b - pal[i].peBlue;
                const int dist = dr * dr + dg * dg + db * db;
                if (dist < bestD) bestD = dist, best = i;
            }
            nearest[key] = int16_t(best);
        }
        return uint8_t(nearest[key]);
    }
};

// A8R8G8B8 to an RGB layout, as ARGBToPixel converts (rounded to nearest), by table: one
// entry per channel value, OR-ed together.
struct ToPixel {
    uint32_t lut[4][256];  // blue, green, red, alpha
    explicit ToPixel(const Format& f) {
        for (uint32_t i = 0; i < 256; i++) {
            lut[0][i] = ARGBToPixel(f, i);
            lut[1][i] = ARGBToPixel(f, i << 8);
            lut[2][i] = ARGBToPixel(f, i << 16);
            lut[3][i] = ARGBToPixel(f, i << 24);
        }
    }
    uint32_t operator()(uint32_t c) const {
        return lut[0][c & 0xFF] | lut[1][(c >> 8) & 0xFF] | lut[2][(c >> 16) & 0xFF] | lut[3][c >> 24];
    }
};

// One row of a render target back into the CPU copy.
static void RowFromGpu(Buffer* b, int y, const uint8_t* src, bool convert, PaletteMap* pal, const ToPixel* to) {
    uint8_t* dst = b->Row(y);
    const DWORD bytes = b->fmt.Bytes();
    if (!convert) {
        memcpy(dst, src, size_t(b->w) * bytes);
        return;
    }
    const auto* in = reinterpret_cast<const uint32_t*>(src);
    if (b->fmt.pal8) {
        for (int x = 0; x < b->w; x++) dst[x] = pal ? pal->Index(in[x]) : 0;
        return;
    }
    if (to && bytes == 2) {
        auto* d16 = reinterpret_cast<uint16_t*>(dst);
        for (int x = 0; x < b->w; x++) d16[x] = uint16_t((*to)(in[x]));
        return;
    }
    for (int x = 0; x < b->w; x++, dst += bytes) WritePixel(dst, bytes, to ? (*to)(in[x]) : ARGBToPixel(b->fmt, in[x]));
}

// The Z buffer's Direct3D 9 format: closest depth, with stencil if it has some. depth24=on:
// 24 bits for a 16-bit one too (less z-fighting in the distance).
static D3DFORMAT DepthFormat(const Format& f) {
    // (wbuffer=force: 24 bits too, the emulation's depths are 1/w, crowded near 1 in the distance.)
    if (f.bpp == 16) return g_config.depth24 || g_config.wbuffer == 2 ? D3DFMT_D24X8 : D3DFMT_D16;
    if (f.a) return D3DFMT_D24S8;  // Format keeps a Z buffer's stencil mask in `a`
    return D3DFMT_D24X8;
}

// ---------------------------------------------------------------- render targets and Z

// Counts drawing on every render target: a copy made at serial n is of that drawing alone.
static unsigned g_drawSerial = 0;
// Emulated W buffering's scale: 1/w times this is at most 1 for every vertex so far (it only
// shrinks, when a nearer one comes). Gpu lock.
static float g_wScale = 1.0f;
// drawtrace=on: one whole frame's draws logged every 600 scenes (about 10 s).
static unsigned g_traceScene = 0;
static bool TracingFrame() { return g_config.drawTrace && g_traceScene % 600 == 300; }

struct GpuSurf final : Gpu::Owner {
    Gpu* gpu;
    Buffer* buf;
    bool depth;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    bool convert = false;
    IDirect3DSurface9* surf = nullptr;     // the render target or depth-stencil surface (antialiasing:
                                           // a Z buffer's is multisampled; a render target's is the resolved image)
    // antialiasing=N, render targets: the multisampled surface Direct3D draws into, resolved
    // into `surf` before anything reads that; and the CPU's changes on their way into it.
    IDirect3DSurface9* ms = nullptr;
    IDirect3DTexture9* msCopy = nullptr;
    bool msNewer = false;  // `ms` has drawing `surf` lacks
    bool msStale = false;  // `surf` has the CPU's changes `ms` lacks, in msDirty
    RECT msDirty = {};
    IDirect3DSurface9* sysmem = nullptr;   // read-back staging
    IDirect3DSurface9* upload = nullptr;   // upload staging (D3DPOOL_DEFAULT, lockable)
    // Z buffers DirectDraw can read and write (where the GPU has INTZ, without antialiasing):
    // `surf` is the depth texture's level, read by drawing it into the R32F target `zrt`
    // (read back through `zsys`), written by drawing `zup` (filled through `zupSys`) with a
    // shader writing depth. Without, as before: only DDBLT_DEPTHFILL reaches the GPU.
    IDirect3DTexture9* ztex = nullptr;
    IDirect3DSurface9* zrt = nullptr;
    IDirect3DSurface9* zsys = nullptr;
    IDirect3DTexture9* zup = nullptr;
    IDirect3DTexture9* zupSys = nullptr;
    IDirect3DPixelShader9* zreadPs = nullptr;
    IDirect3DPixelShader9* zwritePs = nullptr;
    // A small lock's read back (a lens flare's occlusion test, every frame): only its
    // rectangle, drawn into this tile and read through `ztileSys` (kZTile pixels square).
    IDirect3DSurface9* ztile = nullptr;
    IDirect3DSurface9* ztileSys = nullptr;
    // DDBLT_DEPTHFILLs waiting for the Z buffer's next use (it can't be written directly),
    // in order: a second one before a draw (a split screen's two halves) doesn't replace
    // the first. A new Z buffer starts with one over it all, from the surface's memory.
    struct Fill {
        RECT r;
        float z;
    };
    std::vector<Fill> fills;
    unsigned failedGen = 0;  // creation failed on the device of generation failedGen - 1
    // A palettized render target's palette (its surface's when last bound or read back).
    PaletteMap* pal = nullptr;
    ToPixel* toPixel = nullptr;  // a converted RGB render target's read-back table
    unsigned serial = 0;  // counts what Direct3D drew into it (a texture copy of it is current while equal)

    GpuSurf(Gpu* g, Buffer* b, bool d) : gpu(g), buf(b), depth(d) {}
    ~GpuSurf() {
        delete pal;
        delete toPixel;
    }
    void SetPalette(const Surface* s) {
        if (!buf->fmt.pal8) return;
        const PALETTEENTRY* p = s->EffectivePalette();
        if (!p) return;
        if (!pal) pal = new PaletteMap;
        pal->Set(p);
    }
    void Drop() {
        if (surf) surf->Release();
        if (sysmem) sysmem->Release();
        if (upload) upload->Release();
        if (ms) ms->Release();
        if (msCopy) msCopy->Release();
        surf = sysmem = upload = ms = nullptr;
        msCopy = nullptr;
        for (IUnknown** u : {reinterpret_cast<IUnknown**>(&ztex), reinterpret_cast<IUnknown**>(&zrt),
                             reinterpret_cast<IUnknown**>(&zsys), reinterpret_cast<IUnknown**>(&zup),
                             reinterpret_cast<IUnknown**>(&zupSys), reinterpret_cast<IUnknown**>(&zreadPs),
                             reinterpret_cast<IUnknown**>(&zwritePs), reinterpret_cast<IUnknown**>(&ztile),
                             reinterpret_cast<IUnknown**>(&ztileSys)}) {
            if (*u) (*u)->Release();
            *u = nullptr;
        }
        msNewer = msStale = false;
    }
    // Device reset: D3DPOOL_DEFAULT goes, and what only the GPU had with it.
    void ReleaseGpuResources() override {
        Drop();
        buf->cpuStale = false;
        buf->gpuStale = true;
        buf->gpuDirty = Buffer::kAllRect;
    }
    void GpuReleased() override {
        ReleaseGpuResources();
        gpu = nullptr;  // an orphan now: GpuBufferGone only frees it
    }
};

// antialiasing=N: the multisample type every render target and Z buffer gets (they are made
// 32-bit and 24-bit then, the formats every GPU multisamples), the largest the GPU has up
// to N; D3DMULTISAMPLE_NONE without the option or support. Decided once.
static D3DMULTISAMPLE_TYPE MsaaType(IDirect3DDevice9* d) {
    static int known = -1;  // Gpu lock
    if (known >= 0) return D3DMULTISAMPLE_TYPE(known);
    known = 0;
    if (g_config.msaa < 2) return D3DMULTISAMPLE_NONE;
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp;
    if (FAILED(d->GetCreationParameters(&cp)) || FAILED(d->GetDirect3D(&d3d))) return D3DMULTISAMPLE_NONE;
    const D3DFORMAT fmts[] = {D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_D24X8, D3DFMT_D24S8};
    for (int n = std::min(g_config.msaa, 16); n >= 2 && !known; n--) {
        bool all = true;
        for (D3DFORMAT f : fmts)
            if (FAILED(d3d->CheckDeviceMultiSampleType(cp.AdapterOrdinal, cp.DeviceType, f, TRUE, D3DMULTISAMPLE_TYPE(n), nullptr)))
                all = false;
        if (all) known = n;
    }
    d3d->Release();
    DD2D9_LOG("antialiasing: %dx%s", known, known ? "" : " (none: the GPU has no multisampling for these formats)");
    return D3DMULTISAMPLE_TYPE(known);
}

// Z buffers DirectDraw can read and write: the GPU samples INTZ depth textures, renders to
// R32F and runs ps_2_0 (most do, DXVK too), and zaccess isn't off. Decided once. Gpu lock held.
constexpr D3DFORMAT kIntz = D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z'));
static bool ReadableDepth(IDirect3DDevice9* d) {
    static int known = -1;
    if (known >= 0) return known != 0;
    known = 0;
    if (!g_config.zAccess) {  // zaccess=off
        DD2D9_LOG("Direct3D: Z buffers on the GPU only (zaccess=off)");
        return false;
    }
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp;
    D3DCAPS9 caps;
    if (FAILED(d->GetCreationParameters(&cp)) || FAILED(d->GetDirect3D(&d3d)) || FAILED(d->GetDeviceCaps(&caps))) {
        if (d3d) d3d->Release();
        return false;
    }
    known = SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
                                             D3DRTYPE_TEXTURE, kIntz)) &&
            SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET,
                                             D3DRTYPE_SURFACE, D3DFMT_R32F)) &&
            SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE,
                                             D3DFMT_R32F)) &&
            caps.PixelShaderVersion >= D3DPS_VERSION(2, 0);
    d3d->Release();
    DD2D9_LOG("Direct3D: Z buffers %s", known ? "readable and writable by DirectDraw (INTZ)" : "on the GPU only (no INTZ)");
    return known != 0;
}

// The buffer's render-target (or Z) surface, made if needed. Gpu lock held, device usable.
static GpuSurf* EnsureSurf(Gpu& gpu, IDirect3DDevice9* d, Buffer* b, bool depth) {
    if (b->gpu && b->gpu->gpu != &gpu) return nullptr;  // another object's device (not expected)
    if (!b->gpu) {
        b->gpu = new GpuSurf(&gpu, b, depth);
        gpu.AddOwner(b->gpu);
    }
    GpuSurf* g = b->gpu;
    if (g->surf) return g;
    if (g->failedGen == gpu.Generation() + 1) return nullptr;  // failed on this device already
    HRESULT hr;
    const D3DMULTISAMPLE_TYPE msaa = MsaaType(d);
    if (depth) {
        g->fmt = DepthFormat(b->fmt);
        if (msaa) g->fmt = b->fmt.a ? D3DFMT_D24S8 : D3DFMT_D24X8;
        hr = E_FAIL;
        if (!msaa && ReadableDepth(d) &&
            SUCCEEDED(d->CreateTexture(b->w, b->h, 1, D3DUSAGE_DEPTHSTENCIL, kIntz, D3DPOOL_DEFAULT, &g->ztex, nullptr))) {
            hr = g->ztex->GetSurfaceLevel(0, &g->surf);
            if (SUCCEEDED(hr)) g->fmt = kIntz;
            else g->ztex->Release(), g->ztex = nullptr;
        }
        if (FAILED(hr)) hr = d->CreateDepthStencilSurface(b->w, b->h, g->fmt, msaa, 0, FALSE, &g->surf, nullptr);
        if (FAILED(hr) && g->fmt != D3DFMT_D16 && !msaa) {
            g->fmt = D3DFMT_D16;
            hr = d->CreateDepthStencilSurface(b->w, b->h, g->fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &g->surf, nullptr);
        }
        b->gpuStale = false;  // nothing to upload: a Z buffer starts cleared by the game
        if (SUCCEEDED(hr) && g->ztex) {
            // Writable: uploaded as the surface's memory has it (zero-filled by DirectDraw,
            // which Wine's test_multiple_devices relies on: nothing passes).
            b->gpuStale = true;
            b->gpuDirty = Buffer::kAllRect;
        } else if (SUCCEEDED(hr)) {
            // Starts as the surface's memory says (DirectDraw zero-fills a new one, which
            // Wine's test_multiple_devices relies on: nothing passes; or what a fill on the
            // CPU copy made it), not as the driver leaves it.
            const Format& f = b->fmt;
            const DWORD zmask = f.r ? f.r : f.ValueMask();
            const DWORD shift = zmask ? DWORD(__builtin_ctz(zmask)) : 0, maxv = zmask >> shift;
            const DWORD v = ReadPixel(b->Row(0), f.Bytes());
            g->fills.assign(1, GpuSurf::Fill{RECT{0, 0, b->w, b->h}, maxv ? float(double((v & zmask) >> shift) / maxv) : 0.0f});
        }
    } else {
        const D3DFORMAT native = NativeFormat(b->fmt);
        g->convert = !IsRenderTargetFormat(native);
        // render32=on: a 16-bit render target drawn in 32 bits (blending, fog and gradients
        // without 16-bit banding), converted when the CPU reads it.
        if ((g_config.render32 || msaa) && b->fmt.bpp == 16 && !b->fmt.pal8) g->convert = true;
        g->fmt = g->convert ? (b->fmt.a && !b->fmt.pal8 ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8) : native;
        hr = d->CreateRenderTarget(b->w, b->h, g->fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &g->surf, nullptr);
        if (SUCCEEDED(hr) && msaa &&
            FAILED(d->CreateRenderTarget(b->w, b->h, g->fmt, msaa, 0, FALSE, &g->ms, nullptr))) {
            g->surf->Release();  // the Z buffers are multisampled: no target without it
            g->surf = g->ms = nullptr;
            hr = E_FAIL;
        }
        DD2D9_TRACE("Direct3D: render target %dx%d, %d-bit, as D3D9 format %d", b->w, b->h, int(b->fmt.bpp), int(g->fmt));
    }
    if (FAILED(hr)) {
        DD2D9_LOG("Direct3D: %s %dx%d (format %d) failed: 0x%08lx", depth ? "CreateDepthStencilSurface" : "CreateRenderTarget",
                  b->w, b->h, int(g->fmt), hr);
        g->surf = nullptr;
        g->failedGen = gpu.Generation() + 1;  // not tried again (and logged) at every draw
        return nullptr;
    }
    return g;
}

// antialiasing: the CPU's changes (in `surf`) into the multisampled surface, drawn as a
// quad from a texture copy of `surf` (StretchRect can't write a multisampled surface), over
// the part changed (msDirty) only. The device state it sets is put back after
// (SavedDeviceState): it happens in the middle of the game's drawing. Gpu lock held.
static void CopyToMs(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g) {
    const Buffer* b = g->buf;
    const RECT bounds = {0, 0, b->w, b->h};
    RECT rc;
    if (!IntersectRect(&rc, &g->msDirty, &bounds)) {
        g->msStale = false;
        return;
    }
    if (TracingFrame()) DD2D9_LOG("drawtrace: copy to the multisampled image %ld,%ld-%ld,%ld", rc.left, rc.top, rc.right, rc.bottom);
    if (!g->msCopy && FAILED(d->CreateTexture(b->w, b->h, 1, D3DUSAGE_RENDERTARGET, g->fmt, D3DPOOL_DEFAULT, &g->msCopy, nullptr))) {
        g->msCopy = nullptr;
        return;
    }
    IDirect3DSurface9* level = nullptr;
    if (FAILED(g->msCopy->GetSurfaceLevel(0, &level))) return;
    gpu.CloseScene();
    const HRESULT hr = d->StretchRect(g->surf, &rc, level, &rc, D3DTEXF_NONE);
    level->Release();
    if (FAILED(hr)) return;
    const SavedDeviceState saved(d);  // the game's context state, put back at the return
    d->SetRenderTarget(0, g->ms);
    d->SetDepthStencilSurface(nullptr);
    const D3DVIEWPORT9 vp = {0, 0, DWORD(b->w), DWORD(b->h), 0.0f, 1.0f};
    d->SetViewport(&vp);
    d->SetPixelShader(nullptr);
    d->SetVertexShader(nullptr);
    SetFvf9(d, D3DFVF_XYZRHW | D3DFVF_TEX1);
    SetTexture9(d, 0, g->msCopy);
    SetTexture9(d, 1, nullptr);
    const DWORD tss[][2] = {{D3DTSS_COLOROP, D3DTOP_SELECTARG1}, {D3DTSS_COLORARG1, D3DTA_TEXTURE},
                            {D3DTSS_ALPHAOP, D3DTOP_SELECTARG1}, {D3DTSS_ALPHAARG1, D3DTA_TEXTURE},
                            {D3DTSS_TEXCOORDINDEX, 0}, {D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE}};
    for (const auto& t : tss) d->SetTextureStageState(0, D3DTEXTURESTAGESTATETYPE(t[0]), t[1]);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    const DWORD ss[][2] = {{D3DSAMP_MINFILTER, D3DTEXF_POINT}, {D3DSAMP_MAGFILTER, D3DTEXF_POINT},
                           {D3DSAMP_MIPFILTER, D3DTEXF_NONE}, {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP},
                           {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP}, {D3DSAMP_SRGBTEXTURE, FALSE}};
    for (const auto& t : ss) d->SetSamplerState(0, D3DSAMPLERSTATETYPE(t[0]), t[1]);
    const DWORD rs[][2] = {{D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE}, {D3DRS_CULLMODE, D3DCULL_NONE},
                           {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_FOGENABLE, FALSE},
                           {D3DRS_STENCILENABLE, FALSE}, {D3DRS_SCISSORTESTENABLE, FALSE},
                           {D3DRS_COLORWRITEENABLE, 0xF}, {D3DRS_FILLMODE, D3DFILL_SOLID}, {D3DRS_SRGBWRITEENABLE, FALSE},
                           {D3DRS_CLIPPLANEENABLE, 0}, {D3DRS_SPECULARENABLE, FALSE}, {D3DRS_WRAP0, 0},
                           {D3DRS_TEXTUREFACTOR, 0xFFFFFFFF}};
    for (const auto& t : rs) d->SetRenderState(D3DRENDERSTATETYPE(t[0]), t[1]);
    // The dirty rectangle only, texel for pixel (pixel centres on texel centres).
    const float iw = 1.0f / float(b->w), ih = 1.0f / float(b->h);
    const float x0 = float(rc.left) - 0.5f, y0 = float(rc.top) - 0.5f, x1 = float(rc.right) - 0.5f, y1 = float(rc.bottom) - 0.5f;
    const float u0 = rc.left * iw, v0 = rc.top * ih, u1 = rc.right * iw, v1 = rc.bottom * ih;
    const float q[4][6] = {{x0, y0, 0, 1, u0, v0}, {x1, y0, 0, 1, u1, v0}, {x0, y1, 0, 1, u0, v1}, {x1, y1, 0, 1, u1, v1}};
    gpu.OpenScene();
    if (SUCCEEDED(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0])))) g->msStale = false;  // (else tried again)
}

// antialiasing: what Direct3D drew, into the resolved image (before it is read).
static void Resolve(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g) {
    if (!g->ms || !g->msNewer || !g->surf) return;
    if (TracingFrame()) DD2D9_LOG("drawtrace: resolve%s", g->msStale ? " (the resolved image had newer parts: lost)" : "");
    gpu.CloseScene();
    d->StretchRect(g->ms, nullptr, g->surf, nullptr, D3DTEXF_NONE);
    g->msNewer = false;
}

static void UploadDepth(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g);

// The CPU copy into a render target: the part written since (Buffer::gpuDirty). gpuStale
// stays set until the copy is made (tried again at the next use).
static void UploadSurf(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g) {
    Buffer* b = g->buf;
    if (g->depth) {  // only DDBLT_DEPTHFILL reaches one that can't be written (GpuSurf::fills)
        if (g->ztex) UploadDepth(gpu, d, g);
        else b->gpuStale = false;
        return;
    }
    const RECT bounds = {0, 0, b->w, b->h};
    RECT rc;
    if (!IntersectRect(&rc, &b->gpuDirty, &bounds)) {
        b->gpuStale = false;
        return;
    }
    if (!g->upload &&
        FAILED(d->CreateOffscreenPlainSurface(b->w, b->h, g->fmt, D3DPOOL_DEFAULT, &g->upload, nullptr))) {
        g->upload = nullptr;
        return;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(g->upload->LockRect(&lr, &rc, 0))) return;
    const RowConverter conv(b->fmt, g->pal ? g->pal->pal : nullptr);
    const DWORD bytes = b->fmt.Bytes(), width = DWORD(rc.right - rc.left);
    for (LONG y = rc.top; y < rc.bottom; y++) {
        const uint8_t* from = b->Row(y) + size_t(rc.left) * bytes;
        uint8_t* to = static_cast<uint8_t*>(lr.pBits) + size_t(y - rc.top) * lr.Pitch;
        if (g->convert) conv.Run(from, reinterpret_cast<uint32_t*>(to), int(width));
        else memcpy(to, from, size_t(width) * bytes);
    }
    g->upload->UnlockRect();
    gpu.CloseScene();
    if (FAILED(d->StretchRect(g->upload, &rc, g->surf, &rc, D3DTEXF_NONE))) return;
    b->gpuStale = false;
    if (g->ms) {  // copied on into the multisampled one when drawn into
        if (!g->msStale) g->msDirty = rc;
        else UnionRect(&g->msDirty, &g->msDirty, &rc);
        g->msStale = true;
    }
}

// ---- Z buffers DirectDraw reads and writes (GpuSurf::ztex)

// texld r0, t0, s0; mov r1, r0.x; mov oC0, r1
static const DWORD kZReadPs[] = {0xffff0200, 0x0200001f, 0x80000000, 0xb0030000, 0x0200001f, 0x90000000,
                                 0xa00f0800, 0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x02000001,
                                 0x800f0001, 0x80000000, 0x02000001, 0x800f0800, 0x80e40001, 0x0000ffff};
// texld r0, t0, s0; mov oDepth, r0.x; mov r1, r0.x; mov oC0, r1
static const DWORD kZWritePs[] = {0xffff0200, 0x0200001f, 0x80000000, 0xb0030000, 0x0200001f, 0x90000000,
                                  0xa00f0800, 0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x02000001,
                                  0x900f0800, 0x80000000, 0x02000001, 0x800f0001, 0x80000000, 0x02000001,
                                  0x800f0800, 0x80e40001, 0x0000ffff};

// The Z buffer's bits in its CPU format: mask, shift, largest value.
static void ZBits(const Format& f, DWORD* mask, DWORD* shift, DWORD* maxv) {
    *mask = f.r ? f.r : f.ValueMask();
    *shift = *mask ? DWORD(__builtin_ctz(*mask)) : 0;
    *maxv = *mask >> *shift;
}

// The R32F target the depth is drawn into (also the colour target, not written, of uploads)
// and the shaders. false: not available.
static bool ZTargets(IDirect3DDevice9* d, GpuSurf* g) {
    const Buffer* b = g->buf;
    if (!g->zrt && FAILED(d->CreateRenderTarget(b->w, b->h, D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &g->zrt, nullptr)))
        g->zrt = nullptr;
    if (!g->zreadPs && FAILED(d->CreatePixelShader(kZReadPs, &g->zreadPs))) g->zreadPs = nullptr;
    if (!g->zwritePs && FAILED(d->CreatePixelShader(kZWritePs, &g->zwritePs))) g->zwritePs = nullptr;
    return g->zrt && g->zreadPs && g->zwritePs;
}

// A quad over `dst` of the target (tw x th), textured with `src` of stage 0's texture (texW x
// texH, pixel for texel) through `ps`, everything else in the device's state as a plain
// copy needs it. The caller has the target, depth-stencil surface and texture set (and a
// SavedDeviceState).
static bool DrawZQuad(Gpu& gpu, IDirect3DDevice9* d, IDirect3DPixelShader9* ps, int tw, int th, const RECT& dst,
                      int texW, int texH, const RECT& src, bool writeDepth) {
    const D3DVIEWPORT9 vp = {0, 0, DWORD(tw), DWORD(th), 0.0f, 1.0f};
    d->SetViewport(&vp);
    d->SetPixelShader(ps);
    d->SetVertexShader(nullptr);
    SetFvf9(d, D3DFVF_XYZRHW | D3DFVF_TEX1);
    SetTexture9(d, 1, nullptr);
    const DWORD tss[][2] = {{D3DTSS_TEXCOORDINDEX, 0}, {D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE}};
    for (const auto& t : tss) d->SetTextureStageState(0, D3DTEXTURESTAGESTATETYPE(t[0]), t[1]);
    const DWORD ss[][2] = {{D3DSAMP_MINFILTER, D3DTEXF_POINT}, {D3DSAMP_MAGFILTER, D3DTEXF_POINT},
                           {D3DSAMP_MIPFILTER, D3DTEXF_NONE}, {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP},
                           {D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP}, {D3DSAMP_SRGBTEXTURE, FALSE}};
    for (const auto& t : ss) d->SetSamplerState(0, D3DSAMPLERSTATETYPE(t[0]), t[1]);
    const DWORD rs[][2] = {{D3DRS_ZENABLE, writeDepth}, {D3DRS_ZWRITEENABLE, writeDepth}, {D3DRS_ZFUNC, D3DCMP_ALWAYS},
                           {D3DRS_CULLMODE, D3DCULL_NONE}, {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE},
                           {D3DRS_FOGENABLE, FALSE}, {D3DRS_STENCILENABLE, FALSE}, {D3DRS_SCISSORTESTENABLE, FALSE},
                           {D3DRS_COLORWRITEENABLE, writeDepth ? 0u : 0xFu}, {D3DRS_FILLMODE, D3DFILL_SOLID},
                           {D3DRS_SRGBWRITEENABLE, FALSE}, {D3DRS_CLIPPLANEENABLE, 0}, {D3DRS_WRAP0, 0},
                           {D3DRS_DEPTHBIAS, 0}, {D3DRS_SLOPESCALEDEPTHBIAS, 0}};
    for (const auto& t : rs) d->SetRenderState(D3DRENDERSTATETYPE(t[0]), t[1]);
    const float iw = 1.0f / float(texW), ih = 1.0f / float(texH);
    const float x0 = float(dst.left) - 0.5f, y0 = float(dst.top) - 0.5f, x1 = float(dst.right) - 0.5f, y1 = float(dst.bottom) - 0.5f;
    const float u0 = src.left * iw, v0 = src.top * ih, u1 = src.right * iw, v1 = src.bottom * ih;
    const float q[4][6] = {{x0, y0, 0, 1, u0, v0}, {x1, y0, 0, 1, u1, v0}, {x0, y1, 0, 1, u0, v1}, {x1, y1, 0, 1, u1, v1}};
    gpu.OpenScene();
    return SUCCEEDED(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0])));
}

// The largest lock read back alone (ReadBackDepth's `part`).
constexpr int kZTile = 64;

// The depth the GPU has, into the CPU copy (in its format, its stencil bits kept): the
// pending depth fills applied first. `part`: only that rectangle (at most kZTile square),
// the rest of the CPU copy left as it was (still behind). false: not read. Gpu lock held.
static bool ReadBackDepth(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g, const RECT* part = nullptr) {
    Buffer* b = g->buf;
    if (!g->ztex || !ZTargets(d, g)) return false;
    const RECT all = {0, 0, b->w, b->h};
    RECT src = all;  // (a Blt's rectangle comes here before Blt checks it: kept inside)
    if (part && !IntersectRect(&src, part, &all)) return false;
    const int w = src.right - src.left, h = src.bottom - src.top;
    if (part && (w > kZTile || h > kZTile)) return false;
    IDirect3DSurface9*& rt = part ? g->ztile : g->zrt;
    IDirect3DSurface9*& sys = part ? g->ztileSys : g->zsys;
    if (part && !rt && FAILED(d->CreateRenderTarget(kZTile, kZTile, D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr))) {
        rt = nullptr;
        return false;
    }
    if (!sys && FAILED(d->CreateOffscreenPlainSurface(part ? kZTile : b->w, part ? kZTile : b->h, D3DFMT_R32F,
                                                      D3DPOOL_SYSTEMMEM, &sys, nullptr))) {
        sys = nullptr;
        return false;
    }
    {
        const SavedDeviceState saved(d);
        d->SetRenderTarget(0, g->zrt);  // (as large as the Z buffer, which D3D9 needs for the fills)
        if (!g->fills.empty()) {
            d->SetDepthStencilSurface(g->surf);
            const D3DVIEWPORT9 whole = {0, 0, DWORD(b->w), DWORD(b->h), 0.0f, 1.0f};
            d->SetViewport(&whole);
            for (const GpuSurf::Fill& f : g->fills) {
                const D3DRECT rect = {f.r.left, f.r.top, f.r.right, f.r.bottom};
                d->Clear(1, &rect, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, f.z, 0);
            }
            g->fills.clear();
        }
        d->SetDepthStencilSurface(nullptr);  // (sampled now: not bound as well)
        d->SetRenderTarget(0, rt);
        SetTexture9(d, 0, g->ztex);
        const RECT dst = part ? RECT{0, 0, w, h} : all;
        if (!DrawZQuad(gpu, d, g->zreadPs, part ? kZTile : b->w, part ? kZTile : b->h, dst, b->w, b->h, src, false))
            return false;
    }
    gpu.CloseScene();
    if (FAILED(d->GetRenderTargetData(rt, sys))) return false;
    D3DLOCKED_RECT lr;
    if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return false;
    DWORD mask, shift, maxv;
    ZBits(b->fmt, &mask, &shift, &maxv);
    const DWORD bytes = b->fmt.Bytes();
    for (int y = 0; y < h; y++) {
        const float* from = reinterpret_cast<const float*>(static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch);
        uint8_t* row = b->Row(src.top + y) + size_t(src.left) * bytes;
        for (int x = 0; x < w; x++) {
            const float z = std::clamp(from[x], 0.0f, 1.0f);
            const DWORD v = (DWORD(double(z) * maxv + 0.5) << shift) & mask;
            uint8_t* p = row + size_t(x) * bytes;
            const DWORD old = ReadPixel(p, bytes);
            WritePixel(p, bytes, (old & ~mask) | v);
        }
    }
    sys->UnlockRect();
    return true;
}

// The CPU copy's depth (the part written since, Buffer::gpuDirty) into the GPU's: drawn
// with a shader writing depth. gpuStale stays set if it can't be. Gpu lock held.
static void UploadDepth(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g) {
    Buffer* b = g->buf;
    const RECT bounds = {0, 0, b->w, b->h};
    RECT rc;
    if (!IntersectRect(&rc, &b->gpuDirty, &bounds)) {
        b->gpuStale = false;
        return;
    }
    if (!ZTargets(d, g)) return;
    if (!g->zupSys && FAILED(d->CreateTexture(b->w, b->h, 1, 0, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &g->zupSys, nullptr)))
        g->zupSys = nullptr;
    if (!g->zup && FAILED(d->CreateTexture(b->w, b->h, 1, 0, D3DFMT_R32F, D3DPOOL_DEFAULT, &g->zup, nullptr)))
        g->zup = nullptr;
    if (!g->zupSys || !g->zup) return;
    D3DLOCKED_RECT lr;
    if (FAILED(g->zupSys->LockRect(0, &lr, &rc, 0))) return;
    DWORD mask, shift, maxv;
    ZBits(b->fmt, &mask, &shift, &maxv);
    const DWORD bytes = b->fmt.Bytes();
    const float inv = maxv ? 1.0f / float(maxv) : 0.0f;
    for (LONG y = rc.top; y < rc.bottom; y++) {
        float* dst = reinterpret_cast<float*>(static_cast<uint8_t*>(lr.pBits) + size_t(y - rc.top) * lr.Pitch);
        const uint8_t* row = b->Row(y);
        for (LONG x = rc.left; x < rc.right; x++) dst[x - rc.left] = float((ReadPixel(row + size_t(x) * bytes, bytes) & mask) >> shift) * inv;
    }
    g->zupSys->UnlockRect(0);  // (its dirty rectangle: UpdateTexture copies that)
    gpu.CloseScene();
    if (FAILED(d->UpdateTexture(g->zupSys, g->zup))) return;
    bool drawn;
    {
        const SavedDeviceState saved(d);
        d->SetRenderTarget(0, g->zrt);
        d->SetDepthStencilSurface(g->surf);
        SetTexture9(d, 0, g->zup);
        drawn = DrawZQuad(gpu, d, g->zwritePs, b->w, b->h, rc, b->w, b->h, rc, true);
    }
    if (drawn) b->gpuStale = false;
}

bool GpuDepthWritable(const Buffer* b) { return b->gpu && b->gpu->depth && b->gpu->ztex; }

static void ReadBack(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g);

bool GpuBlt(Surface* dst, const RECT& d, Surface* src, const RECT& s, const std::vector<RECT>& clips) {
    Buffer* sb = src->buf;
    Buffer* db = dst->buf;
    GpuSurf* sg = sb->gpu;
    // The source: a render target whose newest pixels Direct3D drew (the CPU copy behind).
    if (!sg || !sg->gpu || sg->depth || !sg->surf || !sb->cpuStale || sb->gpuStale) return false;
    if (sb->fmt.pal8 || db->fmt.pal8 || !(sb->fmt == db->fmt) || clips.empty()) return false;
    // The destination: a render target too, or the primary (given a GPU copy: the presenter
    // then shows it from there, as when Direct3D draws into it).
    if (db->gpu && (db->gpu->depth || db->gpu->gpu != sg->gpu)) return false;
    if (!db->gpu && !dst->isPrimary) return false;
    if (&dst->dd->GetGpu() != sg->gpu) return false;
    if (dst->isPrimary) {
        // Not when the presenter couldn't show it from there: the part of a windowed primary
        // shown (the window) partly off it would be read back every frame instead.
        const RECT shown = dst->dd->PresentSource(dst->dd->PresentWindow());
        if (shown.left < 0 || shown.top < 0 || shown.right > db->w || shown.bottom > db->h) return false;
    }
    // Stretched: whole (StretchRect takes whole rectangles: a clipped part's source edge
    // would fall between pixels).
    const LONG dw = d.right - d.left, dh = d.bottom - d.top, sw = s.right - s.left, sh = s.bottom - s.top;
    const bool stretched = dw != sw || dh != sh;
    if (stretched && (clips.size() != 1 || !EqualRect(&clips[0], &d))) return false;
    Gpu& gpu = *sg->gpu;
    GpuGuard l(gpu);
    IDirect3DDevice9* dev = gpu.Ensure();
    if (!dev) return false;
    if (!sg->surf || !sb->cpuStale) return false;  // (Ensure reset the device: read back meanwhile)
    // A primary's GPU copy made here is never drawn into multisampled: none made (the
    // antialiasing setting would make one).
    if (!db->gpu && MsaaType(dev)) return false;
    GpuSurf* dg = EnsureSurf(gpu, dev, db, false);
    if (!dg || !dg->surf || dg->fmt != sg->fmt) return false;
    Resolve(gpu, dev, sg);
    Resolve(gpu, dev, dg);  // (what Direct3D drew there, in `surf` before it is written)
    if (db->gpuStale) {
        // The CPU's newer parts first, unless the blit covers them all.
        RECT cover;
        const RECT bounds = {0, 0, db->w, db->h};
        if (IntersectRect(&cover, &db->gpuDirty, &bounds)) {
            bool covered = false;
            for (const RECT& c : clips) {
                RECT u;
                UnionRect(&u, &c, &cover);
                covered |= EqualRect(&u, &c) != 0;
            }
            if (!covered) UploadSurf(gpu, dev, dg);
            else db->gpuStale = false;
        } else {
            db->gpuStale = false;
        }
    }
    if (db->gpuStale) return false;
    gpu.CloseScene();
    for (const RECT& c : clips) {
        const RECT sr = stretched ? s : RECT{s.left + (c.left - d.left), s.top + (c.top - d.top),
                                             s.left + (c.right - d.left), s.top + (c.bottom - d.top)};
        if (FAILED(dev->StretchRect(sg->surf, &sr, dg->surf, &c, D3DTEXF_POINT))) {
            // (Nothing written, or a part: the CPU copy can't be trusted for the rest either way.)
            ReadBack(gpu, dev, dg);
            return false;
        }
        if (dg->ms) {  // the multisampled image lacks it now
            if (!dg->msStale) dg->msDirty = c;
            else UnionRect(&dg->msDirty, &dg->msDirty, &c);
            dg->msStale = true;
        }
    }
    db->cpuStale = true;
    dg->serial = ++g_drawSerial;
    static bool logged;  // Gpu lock
    if (!logged) {
        logged = true;
        DD2D9_LOG("Blt from a render target copied on the GPU (%s)", dst->isPrimary ? "to the primary" : "between render targets");
    }
    return true;
}

// A render target back into the CPU copy.
static void ReadBack(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g) {
    Buffer* b = g->buf;
    b->cpuStale = false;
    if (g->depth) {
        if (g->surf) ReadBackDepth(gpu, d, g);
        return;
    }
    if (!g->surf) return;
    Resolve(gpu, d, g);
    if (!g->sysmem &&
        FAILED(d->CreateOffscreenPlainSurface(b->w, b->h, g->fmt, D3DPOOL_SYSTEMMEM, &g->sysmem, nullptr))) {
        g->sysmem = nullptr;
        return;
    }
    gpu.CloseScene();
    if (FAILED(d->GetRenderTargetData(g->surf, g->sysmem))) return;
    D3DLOCKED_RECT lr;
    if (FAILED(g->sysmem->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
    if (g->convert && !b->fmt.pal8 && !g->toPixel) g->toPixel = new ToPixel(b->fmt);
    for (int y = 0; y < b->h; y++)
        RowFromGpu(b, y, static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch, g->convert, g->pal, g->toPixel);
    g->sysmem->UnlockRect();
    b->cpuSerial = NextCpuSerial();  // the CPU copy changed: a texture made from it is behind
}

void GpuCpuAccess(Surface* s, bool write, const RECT* area) {
    Buffer* b = s->buf;
    GpuSurf* g = b->gpu;
    if (g && g->gpu && b->cpuStale) {
        Gpu& gpu = *g->gpu;
        GpuGuard l(gpu);
        g->SetPalette(s);  // a palettized target maps back through the palette it has now
        bool partly = false;
        if (IDirect3DDevice9* d = gpu.Ensure(); d && g->surf) {
            // A writable Z buffer: what the CPU wrote into a part read back alone goes up
            // first (the GPU's copy is the newer one elsewhere); a small area is read back
            // alone, the rest staying behind (cpuStale).
            if (g->ztex && b->gpuStale) UploadDepth(gpu, d, g);
            partly = g->ztex && area && ReadBackDepth(gpu, d, g, area);
            if (!partly) ReadBack(gpu, d, g);
        }
        if (!partly) b->cpuStale = false;
    }
    // A writable Z buffer written with depth fills still pending: the CPU copy has them (the
    // fill drew there too), so the upload carries them, in order with the write.
    if (write && g && g->depth && g->ztex && !g->fills.empty()) {
        for (const GpuSurf::Fill& f : g->fills) b->MarkGpuStale(&f.r);
        g->fills.clear();
    }
    if (write) b->MarkGpuStale(area);
}

IDirect3DSurface9* GpuNewestSurface(Buffer* b) {
    GpuSurf* g = b->gpu;
    if (!g || !g->gpu || g->depth || !g->surf || !b->cpuStale) return nullptr;
    if (IDirect3DDevice9* d = g->gpu->Device()) Resolve(*g->gpu, d, g);
    g->gpu->CloseScene();
    return g->surf;
}

bool GpuDepthFill(Surface* s, const RECT& r, DWORD value) {
    GpuSurf* g = s->buf->gpu;
    if (!g || !g->gpu || !g->depth) return false;
    GpuGuard l(*g->gpu);
    const Format& f = s->buf->fmt;
    DWORD zmask = f.r ? f.r : f.ValueMask();
    const DWORD shift = zmask ? DWORD(__builtin_ctz(zmask)) : 0;
    const DWORD maxv = zmask >> shift;
    if (g->fills.size() >= 64) g->fills.erase(g->fills.begin());  // (a game filling without drawing)
    g->fills.push_back({r, maxv ? float(double((value & zmask) >> shift) / maxv) : 1.0f});
    return true;
}

void GpuBufferGone(Buffer* b) {
    GpuSurf* g = b->gpu;
    b->gpu = nullptr;
    if (g->gpu) {
        GpuGuard l(*g->gpu);
        g->Drop();
        g->gpu->RemoveOwner(g);
    }
    delete g;
}

// ---------------------------------------------------------------- textures

struct GpuTex final : Gpu::Owner {
    Gpu* gpu;
    Surface* top;
    IDirect3DTexture9* tex = nullptr;
    IDirect3DCubeTexture9* cube = nullptr;  // a cube map: its six faces (levelBufs: face by face)
    UINT levels = 0;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    bool convert = false;
    // What the GPU copy was made with: a change means uploading again.
    const Palette* pal = nullptr;
    DWORD palVersion = 0;
    bool keyed = false;
    DWORD key = 0;
    bool shownKeyed = false;  // the texture last handed out (managed or rtTex) has the key as alpha
    bool autogen = false;  // mipmaps=on: one level from the game, the rest made by the GPU
    int w = 0, h = 0;      // the top level's size and layout when made (SetSurfaceDesc may change them)
    Format made = {};
    std::vector<Buffer*> levelBufs;
    std::vector<unsigned> levelSerials;  // each level's Buffer::cpuSerial when uploaded
    // A texture whose levels Direct3D drew into (render targets used as textures) while the
    // GPU has their newest pixels: a copy made on the GPU (no read back through the CPU), each
    // level from its render target (`rtLevelSrc` the GpuSurf as its serial was) or, for one
    // the GPU doesn't have newer, its CPU copy (the Buffer as its cpuSerial was) through
    // `rtSys`. Colour-keyed: the key made transparent by a shader pass from `rtTmp`.
    IDirect3DTexture9* rtTex = nullptr;
    IDirect3DTexture9* rtSys = nullptr;  // SYSTEMMEM, the CPU levels on their way up
    IDirect3DTexture9* rtTmp = nullptr;  // a level's render target as a texture (keyed only)
    IDirect3DPixelShader9* keyPs = nullptr;
    std::vector<const void*> rtLevelSrc;
    std::vector<unsigned> rtLevelSerial;
    UINT rtW = 0, rtH = 0, rtLevels = 0, tmpW = 0, tmpH = 0;
    D3DFORMAT rtFmt = D3DFMT_UNKNOWN, tmpFmt = D3DFMT_UNKNOWN;
    bool rtKeyed = false;
    DWORD rtKey = 0;
    void DropRt() {
        if (rtTex) rtTex->Release();
        if (rtSys) rtSys->Release();
        if (rtTmp) rtTmp->Release();
        rtTex = rtSys = rtTmp = nullptr;
        rtLevelSrc.clear();
        rtLevelSerial.clear();
    }

    GpuTex(Gpu* g, Surface* s) : gpu(g), top(s) {}
    void Drop() {
        if (tex) tex->Release();
        tex = nullptr;
        if (cube) cube->Release();
        cube = nullptr;
        levelBufs.clear();
        levelSerials.clear();
        DropRt();
        if (keyPs) keyPs->Release();
        keyPs = nullptr;
    }
    void ReleaseGpuResources() override { Drop(); }
    void GpuReleased() override {
        Drop();
        gpu = nullptr;
    }
};

// The palette a P8 texture is shown with: its own, or the primary's (as DX7 drivers did).
static const Palette* TexturePalette(const Surface* s) {
    if (s->palette) return s->palette;
    if (s->dd->primary && s->dd->primary->palette) return s->dd->primary->palette;
    return nullptr;
}

// An 8-bit texture's palette made with DDPCAPS_ALPHA: each entry's peFlags is its alpha
// (D3DPTEXTURECAPS_ALPHAPALETTE, which dd2d9 reports). PopCap's SexyApp games (Bejeweled 2)
// keep their translucent images this way.
static bool PaletteHasAlpha(const Palette* pal) { return pal && (pal->caps & DDPCAPS_ALPHA); }

bool TextureHasAlpha(const Surface* s) {
    return s->buf->fmt.a || (s->buf->fmt.pal8 && PaletteHasAlpha(TexturePalette(s)));
}

// mipmaps=on: whether the GPU can make a texture of this format's smaller levels itself.
static bool CanAutogen(IDirect3DDevice9* d, D3DFORMAT fmt) {
    static std::unordered_map<int, bool> known;  // Gpu lock
    auto it = known.find(int(fmt));
    if (it != known.end()) return it->second;
    bool ok = false;
    IDirect3D9* d3d = nullptr;
    if (SUCCEEDED(d->GetDirect3D(&d3d))) {
        D3DDEVICE_CREATION_PARAMETERS cp;
        D3DDISPLAYMODE dm;
        if (SUCCEEDED(d->GetCreationParameters(&cp)) && SUCCEEDED(d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &dm)))
            ok = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, dm.Format, D3DUSAGE_AUTOGENMIPMAP,
                                        D3DRTYPE_TEXTURE, fmt) == D3D_OK;  // D3DOK_NOAUTOGEN: no
        d3d->Release();
    }
    known[int(fmt)] = ok;
    return ok;
}

// One level of a texture's CPU copy into its locked GPU level.
static void WriteLevel(const Buffer* b, const D3DLOCKED_RECT& lr, const PALETTEENTRY* entries, bool convert, bool keyed,
                       DWORD key, bool palAlpha = false) {
    if (b->fmt.bump && b->fmt.bpp == 24) {
        // U8V8L8 widened to X8L8V8U8 (the same bytes, a fourth unused): four pixels from three
        // 32-bit words at a time.
        for (int y = 0; y < b->h; y++) {
            const uint8_t* src = b->Row(y);
            uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
            int x = 0;
            for (; x + 4 <= b->w; x += 4) {
                uint32_t w[3];
                memcpy(w, src + size_t(x) * 3, 12);
                dst[x] = w[0] | 0xFF000000u;
                dst[x + 1] = (w[0] >> 24) | (w[1] << 8) | 0xFF000000u;
                dst[x + 2] = (w[1] >> 16) | (w[2] << 16) | 0xFF000000u;
                dst[x + 3] = (w[2] >> 8) | 0xFF000000u;
            }
            for (; x < b->w; x++) {
                const uint8_t* p = src + size_t(x) * 3;
                dst[x] = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | 0xFF000000u;
            }
        }
        return;
    }
    if (b->fmt.BlockCompressed()) {
        // Rows of 4x4 blocks: the CPU copy's pitch is a quarter of a block row.
        const int rows = (b->h + 3) / 4;
        const size_t rowBytes = size_t(b->pitch) * 4;
        for (int y = 0; y < rows; y++)
            memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, b->bits + y * rowBytes,
                   std::min<size_t>(rowBytes, size_t(lr.Pitch)));
    } else {
        const RowConverter conv(b->fmt, entries, keyed, key, palAlpha);
        for (int y = 0; y < b->h; y++)
            RowToGpu(b, y, static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, convert ? &conv : nullptr);
    }
}

// A cube map's GPU copy: a D3D9 cube texture, each face's levels uploaded where their CPU
// copies changed (faces Direct3D drew into read back first). Gpu lock held.
static IDirect3DCubeTexture9* EnsureCube(Gpu& gpu, IDirect3DDevice9* d, Surface* s, bool colorKey) {
    GpuTex* t = s->gpuTex;
    static const DWORD kFaces[6] = {DDSCAPS2_CUBEMAP_POSITIVEX, DDSCAPS2_CUBEMAP_NEGATIVEX, DDSCAPS2_CUBEMAP_POSITIVEY,
                                    DDSCAPS2_CUBEMAP_NEGATIVEY, DDSCAPS2_CUBEMAP_POSITIVEZ, DDSCAPS2_CUBEMAP_NEGATIVEZ};
    // The faces under the cube map's root (a face bound: its own cube map). A face the game
    // didn't make (DirectX 7 allows that) stays unwritten.
    Surface* const root = s->root ? s->root : s;
    Surface* chains[6][16] = {};
    UINT levels = 0;
    for (int f = 0; f < 6; f++) {
        Surface* face = CubeFace(root, kFaces[f]);
        if (!face) continue;
        UINT n = 0;
        for (Surface* l = face; l && n < 16; l = l->NextMipLevel()) chains[f][n++] = l;
        if (!levels) levels = n;
        else if (n != levels) return nullptr;
    }
    if (!levels) return nullptr;
    const Palette* pal = s->buf->fmt.pal8 ? TexturePalette(s) : nullptr;
    const bool keyed = colorKey && s->hasCk[kCkSrcBlt] && !s->buf->fmt.fourcc && !s->buf->fmt.bump;
    const DWORD key = keyed ? s->ck[kCkSrcBlt].dwColorSpaceLowValue : 0;
    bool all = false;
    if (!t->cube || t->levels != levels || keyed != t->keyed || t->w != s->buf->w || !(t->made == s->buf->fmt)) {
        t->Drop();
        const D3DFORMAT native = NativeFormat(s->buf->fmt);
        t->convert = native == D3DFMT_UNKNOWN || keyed;
        t->fmt = t->convert ? D3DFMT_A8R8G8B8 : native;
        HRESULT hr = d->CreateCubeTexture(s->buf->w, levels, 0, t->fmt, D3DPOOL_MANAGED, &t->cube, nullptr);
        if (FAILED(hr) && !t->convert && !s->buf->fmt.fourcc && !s->buf->fmt.bump) {
            t->convert = true;
            t->fmt = D3DFMT_A8R8G8B8;
            hr = d->CreateCubeTexture(s->buf->w, levels, 0, t->fmt, D3DPOOL_MANAGED, &t->cube, nullptr);
        }
        if (FAILED(hr)) {
            DD2D9_LOG("Direct3D: cube texture %d, %u levels (format %d) failed: 0x%08lx", s->buf->w, levels, int(t->fmt), hr);
            t->cube = nullptr;
            return nullptr;
        }
        static bool logged;  // Gpu lock
        if (!logged) {
            logged = true;
            DD2D9_LOG("Direct3D: the game uses cube maps");
        }
        t->levels = levels;
        t->w = t->h = s->buf->w;
        t->made = s->buf->fmt;
        t->autogen = false;
        all = true;
    }
    if (pal != t->pal || (pal && pal->version != t->palVersion) || key != t->key) all = true;
    t->levelBufs.resize(size_t(6) * levels, nullptr);
    t->levelSerials.resize(size_t(6) * levels, 0);
    const PALETTEENTRY* entries = pal ? pal->entries : s->dd->defaultPalette;
    for (int f = 0; f < 6; f++)
        for (UINT i = 0; i < levels && chains[f][0]; i++) {
            Buffer* b = chains[f][i]->buf;
            if (b->gpu && b->cpuStale && b->gpu->surf) ReadBack(gpu, d, b->gpu);  // a face drawn into
            const size_t k = size_t(f) * levels + i;
            if (!all && t->levelBufs[k] == b && t->levelSerials[k] == b->cpuSerial) continue;
            D3DSURFACE_DESC ld;
            if (FAILED(t->cube->GetLevelDesc(i, &ld)) || UINT(b->w) > ld.Width || UINT(b->h) > ld.Height ||
                !(b->fmt == s->buf->fmt))
                continue;
            D3DLOCKED_RECT lr;
            if (FAILED(t->cube->LockRect(D3DCUBEMAP_FACES(f), i, &lr, nullptr, 0))) continue;
            WriteLevel(b, lr, entries, t->convert, keyed, key, PaletteHasAlpha(pal));
            t->cube->UnlockRect(D3DCUBEMAP_FACES(f), i);
            t->levelBufs[k] = b;
            t->levelSerials[k] = b->cpuSerial;
        }
    t->pal = pal;
    t->palVersion = pal ? pal->version : 0;
    t->keyed = keyed;
    t->key = key;
    t->shownKeyed = keyed;
    return t->cube;
}

// ps_2_0: the texel transparent black where its colour, in the texture's own format, is the
// key (as the CPU path's RowConverter makes it). c0 = each channel's largest value (r, g, b), c3 = minus
// the key's channels. Each channel quantized as ARGBToPixel rounds: floor(c * max + 0.498).
// Assembled (psasm) from:
//   def c2, 0.498, -0.5, 0, 0 / dcl t0 / dcl_2d s0 / mov r1..r3, c2.z / texld r0, t0, s0
//   r1.xyz = r0 * c0 + c2.x / r2.xy = frc(r1) / r3.x = r1.z, r3.xy = frc(r3), r2.z = r3.x
//   r1.xyz = abs(r1 - r2 + c3) / r1.x = max(r1.x, r1.y, r1.z) + c2.y
//   r0 = r1.x >= 0 ? r0 : 0 / oC0 = r0
// Written to Windows' ps_2_0 rules as the presenter's shaders are (registers set whole
// first, no negated constants, one constant per instruction, frc on .xy).
static const DWORD kKeyPs[] = {
    0xffff0200, 0x0200001f, 0x80000000, 0xb00f0000, 0x0200001f, 0x90000000, 0xa00f0800, 0x05000051, 0xa00f0002,
    0x3efef9db, 0xbf000000, 0x00000000, 0x00000000, 0x02000001, 0x800f0001, 0xa0aa0002, 0x02000001, 0x800f0002,
    0xa0aa0002, 0x02000001, 0x800f0003, 0xa0aa0002, 0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x03000005,
    0x80070001, 0x80e40000, 0xa0e40000, 0x03000002, 0x80070001, 0x80e40001, 0xa0000002, 0x02000013, 0x80030002,
    0x80e40001, 0x02000001, 0x80010003, 0x80aa0001, 0x02000013, 0x80030003, 0x80e40003, 0x02000001, 0x80040002,
    0x80000003, 0x03000002, 0x80070001, 0x80e40001, 0x81e40002, 0x03000002, 0x80070001, 0x80e40001, 0xa0e40003,
    0x02000023, 0x80070001, 0x80e40001, 0x0300000b, 0x80010001, 0x80000001, 0x80550001, 0x0300000b, 0x80010001,
    0x80000001, 0x80aa0001, 0x03000002, 0x80010001, 0x80000001, 0xa0550002, 0x04000058, 0x800f0000, 0x80000001,
    0x80e40000, 0xa0aa0002, 0x02000001, 0x800f0800, 0x80e40000, 0x0000ffff};

// A texture with levels Direct3D drew into, made on the GPU (see GpuTex::rtTex); null when
// none of its levels is newer on the GPU, or this can't be done here (the CPU copy is used:
// read back, then uploaded). Palettized textures go through the CPU (the palette is applied
// there). Gpu lock held.
static IDirect3DTexture9* RtTexture(Gpu& gpu, IDirect3DDevice9* d, GpuTex* t, Surface* const* chain, size_t n,
                                    const Format& fmt, bool keyed, DWORD key) {
    auto newest = [](const Buffer* b) { return b->gpu && b->gpu->surf && b->cpuStale && !b->gpuStale; };
    D3DFORMAT gfmt = D3DFMT_UNKNOWN;
    for (size_t i = 0; i < n; i++) {
        const Buffer* b = chain[i]->buf;
        if (!newest(b)) continue;
        if (gfmt == D3DFMT_UNKNOWN) gfmt = b->gpu->fmt;
        else if (b->gpu->fmt != gfmt) return nullptr;  // (levels drawn in different formats)
    }
    if (gfmt == D3DFMT_UNKNOWN || fmt.pal8 || fmt.fourcc || fmt.bump) return nullptr;
    // The texture's format: the render targets', or with a key A8R8G8B8 (alpha for it). The
    // levels the CPU has newer go up as they are (the render targets' layout is the
    // texture's) or converted to 32-bit colour.
    const D3DFORMAT T = keyed ? D3DFMT_A8R8G8B8 : gfmt;
    const bool cpuConvert = keyed || T != NativeFormat(fmt);
    if (cpuConvert && T != D3DFMT_A8R8G8B8 && T != D3DFMT_X8R8G8B8) return nullptr;
    if (keyed && !t->keyPs && FAILED(d->CreatePixelShader(kKeyPs, &t->keyPs))) {
        t->keyPs = nullptr;
        return nullptr;
    }
    const Buffer* top = chain[0]->buf;
    if (!t->rtTex || t->rtW != UINT(top->w) || t->rtH != UINT(top->h) || t->rtFmt != T || t->rtLevels != n) {
        t->DropRt();
        if (FAILED(d->CreateTexture(top->w, top->h, UINT(n), D3DUSAGE_RENDERTARGET, T, D3DPOOL_DEFAULT, &t->rtTex, nullptr))) {
            t->rtTex = nullptr;
            return nullptr;
        }
        t->rtW = top->w;
        t->rtH = top->h;
        t->rtFmt = T;
        t->rtLevels = UINT(n);
    }
    if (t->rtLevelSrc.size() != n || keyed != t->rtKeyed || key != t->rtKey) {
        t->rtLevelSrc.assign(n, nullptr);
        t->rtLevelSerial.assign(n, 0);
        t->rtKeyed = keyed;
        t->rtKey = key;
    }
    for (size_t i = 0; i < n; i++) {
        Buffer* b = chain[i]->buf;
        IDirect3DSurface9* level = nullptr;
        D3DSURFACE_DESC ld;
        if (FAILED(t->rtTex->GetLevelDesc(UINT(i), &ld)) || UINT(b->w) != ld.Width || UINT(b->h) != ld.Height ||
            !(b->fmt == fmt))
            return nullptr;  // (a level given another size or format since: the CPU path copes)
        if (newest(b)) {
            GpuSurf* g = b->gpu;
            if (t->rtLevelSrc[i] == g && t->rtLevelSerial[i] == g->serial) continue;
            Resolve(gpu, d, g);
            if (FAILED(t->rtTex->GetSurfaceLevel(UINT(i), &level))) return nullptr;
            gpu.CloseScene();  // StretchRect: outside a scene
            bool ok;
            if (!keyed) {
                ok = SUCCEEDED(d->StretchRect(g->surf, nullptr, level, nullptr, D3DTEXF_NONE));
            } else {
                // The render target copied into a texture, drawn through the key shader.
                if (!t->rtTmp || t->tmpW != UINT(b->w) || t->tmpH != UINT(b->h) || t->tmpFmt != g->fmt) {
                    if (t->rtTmp) t->rtTmp->Release();
                    if (FAILED(d->CreateTexture(b->w, b->h, 1, D3DUSAGE_RENDERTARGET, g->fmt, D3DPOOL_DEFAULT, &t->rtTmp,
                                                nullptr)))
                        t->rtTmp = nullptr;
                    t->tmpW = b->w;
                    t->tmpH = b->h;
                    t->tmpFmt = g->fmt;
                }
                IDirect3DSurface9* tmp = nullptr;
                ok = t->rtTmp && SUCCEEDED(t->rtTmp->GetSurfaceLevel(0, &tmp)) &&
                     SUCCEEDED(d->StretchRect(g->surf, nullptr, tmp, nullptr, D3DTEXF_NONE));
                if (tmp) tmp->Release();
                if (ok) {
                    auto bits = [](DWORD mask) { return mask ? float(mask >> __builtin_ctz(mask)) : 0.0f; };
                    auto chan = [](DWORD v, DWORD mask) { return mask ? -float((v & mask) >> __builtin_ctz(mask)) : 0.0f; };
                    const float maxes[4] = {bits(fmt.r), bits(fmt.g), bits(fmt.b), 0};
                    const float minusKey[4] = {chan(key, fmt.r), chan(key, fmt.g), chan(key, fmt.b), 0};
                    const SavedDeviceState saved(d);
                    d->SetRenderTarget(0, level);
                    d->SetDepthStencilSurface(nullptr);
                    SetTexture9(d, 0, t->rtTmp);
                    d->SetPixelShaderConstantF(0, maxes, 1);
                    d->SetPixelShaderConstantF(3, minusKey, 1);
                    const RECT all = {0, 0, b->w, b->h};
                    ok = DrawZQuad(gpu, d, t->keyPs, b->w, b->h, all, b->w, b->h, all, false);
                    gpu.CloseScene();
                }
            }
            level->Release();
            if (!ok) return nullptr;
            t->rtLevelSrc[i] = g;
            t->rtLevelSerial[i] = g->serial;
        } else {
            // A level the CPU has newer (or that nothing drew into): its CPU copy, through a
            // system-memory texture (what Direct3D drew there merged in first, if both changed).
            if (b->gpu && b->cpuStale && b->gpu->surf) ReadBack(gpu, d, b->gpu);
            if (t->rtLevelSrc[i] == b && t->rtLevelSerial[i] == b->cpuSerial) continue;
            if (!t->rtSys && FAILED(d->CreateTexture(top->w, top->h, UINT(n), 0, T, D3DPOOL_SYSTEMMEM, &t->rtSys, nullptr))) {
                t->rtSys = nullptr;
                return nullptr;
            }
            D3DLOCKED_RECT lr;
            if (FAILED(t->rtSys->LockRect(UINT(i), &lr, nullptr, 0))) return nullptr;
            WriteLevel(b, lr, chain[0]->dd->defaultPalette, cpuConvert, keyed, key);
            t->rtSys->UnlockRect(UINT(i));
            IDirect3DSurface9* sys = nullptr;
            bool ok = SUCCEEDED(t->rtSys->GetSurfaceLevel(UINT(i), &sys)) && SUCCEEDED(t->rtTex->GetSurfaceLevel(UINT(i), &level));
            gpu.CloseScene();
            ok = ok && SUCCEEDED(d->UpdateSurface(sys, nullptr, level, nullptr));
            if (sys) sys->Release();
            if (level) level->Release();
            if (!ok) return nullptr;
            t->rtLevelSrc[i] = b;
            t->rtLevelSerial[i] = b->cpuSerial;
        }
    }
    static bool loggedKeyed, loggedMip;  // Gpu lock
    if (keyed && !loggedKeyed) {
        loggedKeyed = true;
        DD2D9_LOG("Direct3D: a colour-keyed render-target texture copied on the GPU");
    }
    if (n > 1 && !loggedMip) {
        loggedMip = true;
        DD2D9_LOG("Direct3D: a mipmapped render-target texture copied on the GPU");
    }
    return t->rtTex;
}

// The texture's GPU copy, uploaded where the CPU copy changed. Gpu lock held.
static IDirect3DBaseTexture9* EnsureTex(Gpu& gpu, IDirect3DDevice9* d, Surface* s, bool colorKey) {
    if (s->gpuTex && s->gpuTex->gpu != &gpu) return nullptr;
    if (!s->gpuTex) {
        s->gpuTex = new GpuTex(&gpu, s);
        gpu.AddOwner(s->gpuTex);
    }
    GpuTex* t = s->gpuTex;
    if (s->desc.ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP) return EnsureCube(gpu, d, s, colorKey);
    struct Chain {  // the mip levels, at most 16 (no allocation per bound texture per draw)
        Surface* l[16];
        size_t n = 0;
        size_t size() const { return n; }
        Surface* operator[](size_t i) const { return l[i]; }
    } chain;
    for (Surface* l = s; l && chain.n < 16; l = l->NextMipLevel()) chain.l[chain.n++] = l;
    const Palette* pal = s->buf->fmt.pal8 ? TexturePalette(s) : nullptr;
    // The colour key becomes alpha only while D3DRENDERSTATE_COLORKEYENABLE is on.
    const bool keyed = colorKey && s->hasCk[kCkSrcBlt] && !s->buf->fmt.fourcc && !s->buf->fmt.bump;
    const DWORD key = keyed ? s->ck[kCkSrcBlt].dwColorSpaceLowValue : 0;
    // Levels rendered into by Direct3D and not touched by the CPU since: copied on the GPU.
    if (IDirect3DTexture9* rt = RtTexture(gpu, d, t, chain.l, chain.n, s->buf->fmt, keyed, key)) {
        t->levelBufs.assign(chain.size(), nullptr);  // the managed copy is behind now
        t->shownKeyed = keyed;
        return rt;
    }
    bool all = false;
    // (Made with GPU mipmaps and given alpha since, by a palette with alpha: made again without.)
    if (!t->tex || t->levels != chain.size() || keyed != t->keyed || t->w != s->buf->w || t->h != s->buf->h ||
        !(t->made == s->buf->fmt) || (t->autogen && TextureHasAlpha(s))) {
        t->Drop();
        const D3DFORMAT native = NativeFormat(s->buf->fmt);
        t->convert = native == D3DFMT_UNKNOWN || keyed;
        t->fmt = t->convert ? D3DFMT_A8R8G8B8 : native;
        // mipmaps=on: a texture the game gave one level, made with the rest by the GPU. Not
        // cut-outs, colour-keyed or with an alpha channel (the transparent texels blur into
        // the smaller levels: dark fringes and lines in the distance, NFS III's corn), not
        // block-compressed ones (D3D9 can't), not 1x1.
        HRESULT hr = E_FAIL;
        t->autogen = false;
        if (g_config.mipmaps && chain.size() == 1 && !keyed && !TextureHasAlpha(s) && !s->buf->fmt.fourcc && !s->buf->fmt.bump &&
            (s->buf->w > 1 || s->buf->h > 1) &&
            CanAutogen(d, t->fmt)) {
            hr = d->CreateTexture(s->buf->w, s->buf->h, 0, D3DUSAGE_AUTOGENMIPMAP, t->fmt, D3DPOOL_MANAGED, &t->tex, nullptr);
            t->autogen = SUCCEEDED(hr);
            static bool logged;  // Gpu lock
            if (t->autogen && !logged) {
                logged = true;
                DD2D9_LOG("mipmaps: the GPU makes the smaller levels of textures that have none");
            }
        }
        if (!t->autogen)
            hr = d->CreateTexture(s->buf->w, s->buf->h, UINT(chain.size()), 0, t->fmt, D3DPOOL_MANAGED, &t->tex, nullptr);
        if (FAILED(hr) && !t->convert && !s->buf->fmt.fourcc && !s->buf->fmt.bump) {  // a layout this GPU lacks: convert it
            t->autogen = false;
            t->convert = true;
            t->fmt = D3DFMT_A8R8G8B8;
            hr = d->CreateTexture(s->buf->w, s->buf->h, UINT(chain.size()), 0, t->fmt, D3DPOOL_MANAGED, &t->tex,
                                  nullptr);
        }
        if (FAILED(hr)) {
            DD2D9_LOG("Direct3D: texture %dx%d, %u levels (format %d) failed: 0x%08lx", s->buf->w, s->buf->h,
                      unsigned(chain.size()), int(t->fmt), hr);
            t->tex = nullptr;
            return nullptr;
        }
        t->levels = UINT(chain.size());
        t->w = s->buf->w;
        t->h = s->buf->h;
        t->made = s->buf->fmt;
        all = true;
    }
    if (pal != t->pal || (pal && pal->version != t->palVersion) || key != t->key) all = true;
    t->levelBufs.resize(chain.size(), nullptr);
    t->levelSerials.resize(chain.size(), 0);
    const PALETTEENTRY* entries = pal ? pal->entries : s->dd->defaultPalette;
    for (UINT i = 0; i < chain.size(); i++) {
        Buffer* b = chain[i]->buf;
        // Rendered into by Direct3D (a render target used as a texture): read back first.
        if (b->gpu && b->cpuStale && b->gpu->surf) ReadBack(gpu, d, b->gpu);
        if (!all && t->levelBufs[i] == b && t->levelSerials[i] == b->cpuSerial) continue;
        D3DSURFACE_DESC ld;
        // A level given another size since (SetSurfaceDesc on a lower level): not written
        // past the GPU level's rows.
        if (FAILED(t->tex->GetLevelDesc(i, &ld)) || UINT(b->w) > ld.Width || UINT(b->h) > ld.Height ||
            !(b->fmt == s->buf->fmt))
            continue;
        D3DLOCKED_RECT lr;
        if (FAILED(t->tex->LockRect(i, &lr, nullptr, 0))) continue;
        WriteLevel(b, lr, entries, t->convert, keyed, key, PaletteHasAlpha(pal));
        t->tex->UnlockRect(i);
        t->levelBufs[i] = b;
        t->levelSerials[i] = b->cpuSerial;
        if (t->autogen) t->tex->GenerateMipSubLevels();
    }
    t->pal = pal;
    t->palVersion = pal ? pal->version : 0;
    t->keyed = keyed;
    t->key = key;
    t->shownKeyed = keyed;
    return t->tex;
}

void GpuSurfaceGone(Surface* s) {
    GpuTex* t = s->gpuTex;
    s->gpuTex = nullptr;
    if (t->gpu) {
        GpuGuard l(*t->gpu);
        t->Drop();
        t->gpu->RemoveOwner(t);
    }
    delete t;
}

// ---------------------------------------------------------------- caps

GpuCaps GpuGetCaps() {
    static GpuCaps caps;
    static LONG done = 0;
    if (InterlockedCompareExchange(&done, 0, 0) == 2) return caps;
    static CRITICAL_SECTION* cs = [] {
        static CRITICAL_SECTION c;
        InitializeCriticalSection(&c);
        return &c;
    }();
    EnterCriticalSection(cs);
    if (done != 2) {
        if (IDirect3D9* d3d = CreateD3D9()) {
            D3DCAPS9 c = {};
            if (SUCCEEDED(d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &c))) {
                caps.valid = true;
                caps.devCaps = c.DevCaps;
                caps.primMiscCaps = c.PrimitiveMiscCaps;
                caps.rasterCaps = c.RasterCaps;
                caps.zCmpCaps = c.ZCmpCaps;
                caps.srcBlendCaps = c.SrcBlendCaps;
                caps.destBlendCaps = c.DestBlendCaps;
                caps.alphaCmpCaps = c.AlphaCmpCaps;
                caps.shadeCaps = c.ShadeCaps;
                caps.textureCaps = c.TextureCaps;
                caps.textureFilterCaps = c.TextureFilterCaps;
                caps.textureAddressCaps = c.TextureAddressCaps;
                caps.maxTextureWidth = c.MaxTextureWidth;
                caps.maxTextureHeight = c.MaxTextureHeight;
                caps.maxTextureRepeat = c.MaxTextureRepeat;
                caps.maxTextureAspectRatio = c.MaxTextureAspectRatio;
                caps.maxAnisotropy = c.MaxAnisotropy;
                caps.guardBandLeft = c.GuardBandLeft;
                caps.guardBandTop = c.GuardBandTop;
                caps.guardBandRight = c.GuardBandRight;
                caps.guardBandBottom = c.GuardBandBottom;
                caps.extentsAdjust = c.ExtentsAdjust;
                caps.maxVertexW = c.MaxVertexW;
                caps.stencilCaps = c.StencilCaps;
                caps.fvfCaps = c.FVFCaps;
                caps.textureOpCaps = c.TextureOpCaps;
                caps.maxTextureBlendStages = c.MaxTextureBlendStages;
                caps.maxSimultaneousTextures = c.MaxSimultaneousTextures;
                caps.maxActiveLights = c.MaxActiveLights;
                caps.maxUserClipPlanes = c.MaxUserClipPlanes;
                caps.maxVertexBlendMatrices = c.MaxVertexBlendMatrices;
                caps.vertexProcessingCaps = c.VertexProcessingCaps;
                caps.hwTnL = (c.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0;
            }
            D3DDISPLAYMODE mode = {};
            d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode);
            const D3DFORMAT disp = mode.Format ? mode.Format : D3DFMT_X8R8G8B8;
            auto depthOk = [&](D3DFORMAT f) {
                return SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, disp,
                                                        D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, f));
            };
            caps.depth16 = depthOk(D3DFMT_D16);
            caps.depth24x8 = depthOk(D3DFMT_D24X8);
            caps.depth24s8 = depthOk(D3DFMT_D24S8);
            caps.depth32 = depthOk(D3DFMT_D32);
            const D3DFORMAT dxt[5] = {D3DFMT_DXT1, D3DFMT_DXT2, D3DFMT_DXT3, D3DFMT_DXT4, D3DFMT_DXT5};
            for (int i = 0; i < 5; i++)
                caps.dxt[i] = SUCCEEDED(
                    d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, disp, 0, D3DRTYPE_TEXTURE, dxt[i]));
            auto texOk = [&](D3DFORMAT f) {
                return SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, disp, 0, D3DRTYPE_TEXTURE, f));
            };
            // Bump maps only where the GPU also does the bump-mapping operations.
            const bool bumpOps = (caps.textureOpCaps & (D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE)) != 0;
            caps.bumpV8U8 = bumpOps && texOk(D3DFMT_V8U8);
            caps.bumpL6V5U5 = bumpOps && texOk(D3DFMT_L6V5U5);
            caps.bumpX8L8V8U8 = bumpOps && texOk(D3DFMT_X8L8V8U8);
            d3d->Release();
        }
        InterlockedExchange(&done, 2);
    }
    LeaveCriticalSection(cs);
    return caps;
}

// ---------------------------------------------------------------- drawing context

struct GpuContext::Impl final : Gpu::Owner {
    Gpu& gpu;
    IDirect3DDevice9* dev = nullptr;  // valid between Begin and the end of the call
    unsigned generation = 0;
    Surface* rt = nullptr;
    Surface* z = nullptr;
    IDirect3DSurface9* boundRt = nullptr;  // what D3D9 has (compared only)
    IDirect3DSurface9* boundZ = nullptr;
    bool rebind = true;  // what D3D9 has bound is unknown (new device, or another context's)
    // antialias2d=off: this draw is 2D (Draw: screen-aligned, the depth test passing
    // always): drawn into the resolved, single-sampled image, not the multisampled one.
    bool flat2d = false;
    Surface* tex[8] = {};
    bool colorKey = false;
    // Each stage's mip filter as the game set it, and as D3D9 has it: mipmaps=on turns it on
    // (linear) for textures whose levels the GPU made while the game asks for none.
    DWORD mipFilter[8] = {};
    DWORD mipFilterSet[8] = {};
    // anisotropy=N: each stage's minification filter and anisotropy as the game set them, and
    // as D3D9 has them (linear minification becomes anisotropic). D3D9's defaults: point, 1.
    DWORD minFilter[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    DWORD minFilterSet[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    DWORD magFilter[8] = {1, 1, 1, 1, 1, 1, 1, 1};  // texfilter=linear (D3D9's default: point)
    DWORD magFilterSet[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    DWORD maxAniso[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    // antialiasing: each stage's addressing as the game set it and as D3D9 has it, its
    // texture coordinate set, and whether its texture is a cut-out (see ClampCutouts).
    DWORD addr[8][2] = {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}};
    DWORD addrSet[8][2] = {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}};
    DWORD tci[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    bool cutoutStage[8] = {};
    bool wEmulate = false;  // ZENABLE = D3DZB_USEW on a GPU without W buffering
    int nativeW = -1;       // the GPU's own W buffering: -1 not asked yet
    DWORD rsMirror[256] = {};  // render states as last set through this context (drawtrace)
    bool zWrite = true;     // ZWRITEENABLE as the game set it; zWriteSet: as D3D9 has it
    bool zWriteSet = true;
    bool alphaBlend = false;  // ALPHABLENDENABLE
    float gameBias = 0.0f;  // the game's depth bias (ZBIAS); biasSet: as D3D9 has it
    float biasSet = 0.0f;
    float slopeSet = 0.0f;  // SLOPESCALEDEPTHBIAS as D3D9 has it (only the overlay slack sets it)
    // CLIPPING as the game set it, and as D3D9 has it (2: unknown): off for every draw of
    // transformed vertices (Draw).
    DWORD gameClipping = TRUE;
    DWORD clippingSet = 2;
    DWORD maxAnisoSet[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    DWORD anisoLimit = 0;  // anisotropy= within what the GPU can (0: not known yet, or none)
    // alternatepixelcenter: the game's projection, sent moved half a pixel at the next draw
    // of untransformed vertices (it depends on the viewport too).
    D3DMATRIX proj = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
    bool projDirty = g_config.pixelCenter;
    D3DVIEWPORT9 vp = {0, 0, 1, 1, 0.0f, 1.0f};
    std::unordered_map<DWORD, IDirect3DVertexDeclaration9*> decls;
    DWORD lastFvf = 0xFFFFFFFF;  // Decl's last answer (most draws use the FVF of the one before)
    IDirect3DVertexDeclaration9* lastDecl = nullptr;
    std::vector<uint8_t> scratch;

    explicit Impl(Gpu& g) : gpu(g) {
        GpuGuard l(gpu);  // the presenter thread walks the owners with only the Gpu lock
        gpu.AddOwner(this);
    }
    ~Impl() {
        GpuGuard l(gpu);
        if (gpu.stateOwner == this) gpu.stateOwner = nullptr;
        ReleaseGpuResources();
        gpu.RemoveOwner(this);
    }
    void ReleaseGpuResources() override {
        for (auto& e : decls)
            if (e.second) e.second->Release();  // a failed one is cached as nullptr
        decls.clear();
        lastFvf = 0xFFFFFFFF;
        lastDecl = nullptr;
        DeviceStateChanged();  // (a declaration Draw set may be gone)
        boundRt = boundZ = nullptr;
    }

    IDirect3DVertexDeclaration9* Decl(DWORD fvf);
    bool BindTargets(bool overwriteColor, bool overwriteZ);
    void BindTextures();
    // The address modes ClampCutouts sets for these vertices (each stage's U and V).
    void CutoutAddr(DWORD fvf, const void* verts, UINT stride, UINT count, DWORD (&want)[8][2]) const;
    void ClampCutouts(DWORD fvf, const void* verts, UINT stride, UINT count);
    void SetAddr(const DWORD (&want)[8][2]);  // the sampler address modes, where they differ
    bool CoversTarget() const;
};

GpuContext::GpuContext(Gpu& gpu) : p_(new Impl(gpu)) {}
GpuContext::~GpuContext() { delete p_; }

bool GpuContext::Begin(bool* fresh) {
    *fresh = false;
    p_->dev = p_->gpu.Ensure();
    if (!p_->dev) return false;
    if (p_->generation != p_->gpu.Generation() || p_->gpu.stateOwner != p_) {
        p_->generation = p_->gpu.Generation();
        p_->gpu.stateOwner = p_;
        p_->boundRt = p_->boundZ = nullptr;
        p_->rebind = true;
        // The device's own copies of what dd2d9 sets beside the game's states (which the
        // caller sends all again): unknown now, sent at the next draw.
        p_->biasSet = p_->slopeSet = -2.0f;
        p_->clippingSet = 2;
        *fresh = true;
    }
    return true;
}

void GpuContext::SetRenderState(DWORD state, DWORD value) {
    if (state < 256) p_->rsMirror[state] = value;  // as the game set it (drawtrace)
    if (state == D3DRS_CLIPPING) {  // sent per draw (Draw)
        p_->gameClipping = value;
        return;
    }
    if (state == D3DRS_ZENABLE) {
        // W buffering: D3D9's own where the GPU has it, else emulated for transformed
        // vertices (Draw); the depth test itself is an ordinary Z buffer's.
        p_->wEmulate = false;
        if (value == D3DZB_USEW && p_->dev) {
            static int logged;
            if (p_->nativeW < 0) {  // asked once (the game sets ZENABLE per draw, some of them)
                D3DCAPS9 caps = {};
                p_->nativeW = SUCCEEDED(p_->dev->GetDeviceCaps(&caps)) && (caps.RasterCaps & D3DPRASTERCAPS_WBUFFER);
            }
            const bool native = p_->nativeW > 0;
            if (!native) {
                p_->wEmulate = true;
                value = D3DZB_TRUE;
            }
            if (!logged++) DD2D9_LOG("Direct3D: the game draws with W buffering (%s)", native ? "the GPU's" : "emulated");
        } else if (value == D3DZB_TRUE && g_config.wbuffer == 2) {
            p_->wEmulate = true;  // wbuffer=force: Z buffering taken as W buffering too
            static bool logged;
            if (!logged) {
                logged = true;
                DD2D9_LOG("Direct3D: the game draws with Z buffering (wbuffer=force: emulated W buffering instead)");
            }
        }
    }
    if (state == D3DRS_ZWRITEENABLE) p_->zWrite = p_->zWriteSet = value != 0;
    if (state == D3DRS_ALPHABLENDENABLE) p_->alphaBlend = value != 0;
    if (state == 195) {  // D3DRS_DEPTHBIAS (from ZBIAS): the game's, before the overlay tolerance
        memcpy(&p_->gameBias, &value, 4);
        p_->biasSet = -1.0f;  // re-sent at the next draw
        p_->slopeSet = -2.0f;
    }
    if (state == 195 && (value & 0x7FFFFFFF)) {  // (not -0: ZBIAS 0)
        static bool logged;
        if (!logged) {
            logged = true;
            float f;
            memcpy(&f, &value, 4);
            DD2D9_LOG("Direct3D: the game uses ZBIAS (depth bias %g)", f);
        }
    }
    if (p_->dev) p_->dev->SetRenderState(D3DRENDERSTATETYPE(state), value);
}

void GpuContext::SetTextureStageState(DWORD stage, DWORD type, DWORD value) {
    if (type == D3DTSS_TEXCOORDINDEX && stage < 8) p_->tci[stage] = value;
    if (p_->dev) p_->dev->SetTextureStageState(stage, D3DTEXTURESTAGESTATETYPE(type), value);
}

void GpuContext::SetSamplerState(DWORD stage, DWORD type, DWORD value) {
    if (stage < 8) {
        if (type == D3DSAMP_MIPFILTER) p_->mipFilter[stage] = p_->mipFilterSet[stage] = value;
        if (type == D3DSAMP_MINFILTER) p_->minFilter[stage] = p_->minFilterSet[stage] = value;
        if (type == D3DSAMP_MAGFILTER) p_->magFilter[stage] = p_->magFilterSet[stage] = value;
        if (type == D3DSAMP_ADDRESSU) p_->addr[stage][0] = p_->addrSet[stage][0] = value;
        if (type == D3DSAMP_ADDRESSV) p_->addr[stage][1] = p_->addrSet[stage][1] = value;
        if (type == D3DSAMP_MAXANISOTROPY) p_->maxAniso[stage] = p_->maxAnisoSet[stage] = value;
    }
    if (p_->dev) p_->dev->SetSamplerState(stage, D3DSAMPLERSTATETYPE(type), value);
}

void GpuContext::SetTransform(DWORD type, const float* m16) {
    if (type == D3DTS_PROJECTION) {
        memcpy(&p_->proj, m16, sizeof(p_->proj));
        p_->projDirty = g_config.pixelCenter;
    }
    if (p_->dev) p_->dev->SetTransform(D3DTRANSFORMSTATETYPE(type), reinterpret_cast<const D3DMATRIX*>(m16));
}

void GpuContext::SetViewport(DWORD x, DWORD y, DWORD w, DWORD h, float minZ, float maxZ) {
    p_->vp = {x, y, w, h, minZ, maxZ};
    if (g_config.pixelCenter) p_->projDirty = true;
    if (p_->dev) p_->dev->SetViewport(&p_->vp);
}

void GpuContext::SetMaterial(const void* m) {
    if (p_->dev) p_->dev->SetMaterial(static_cast<const D3DMATERIAL9*>(m));
}

void GpuContext::SetLight(DWORD i, const void* l) {
    if (p_->dev) p_->dev->SetLight(i, static_cast<const D3DLIGHT9*>(l));
}

void GpuContext::LightEnable(DWORD i, bool on) {
    if (p_->dev) p_->dev->LightEnable(i, on);
}

void GpuContext::SetClipPlane(DWORD i, const float* plane) {
    if (p_->dev) p_->dev->SetClipPlane(i, plane);
}

void GpuContext::SetTexture(DWORD stage, Surface* top) {
    if (stage < 8) p_->tex[stage] = top;
}

void GpuContext::SetColorKeyEnable(bool on) { p_->colorKey = on; }

void GpuContext::SetTargets(Surface* rt, Surface* z) {
    p_->rt = rt;
    p_->z = z;
}

// The viewport covers the whole render target (a clear of it replaces every pixel).
bool GpuContext::Impl::CoversTarget() const {
    return rt && vp.X == 0 && vp.Y == 0 && vp.Width >= DWORD(rt->buf->w) && vp.Height >= DWORD(rt->buf->h);
}

// The render target and Z buffer on the device, uploaded where the CPU copy is newer
// (unless about to be overwritten whole). false: no render target.
bool GpuContext::Impl::BindTargets(bool overwriteColor, bool overwriteZ) {
    if (!rt) return false;
    GpuSurf* r = EnsureSurf(gpu, dev, rt->buf, false);
    if (!r) return false;
    r->SetPalette(rt);
    if (rt->buf->gpuStale) {
        if (overwriteColor) rt->buf->gpuStale = false;
        else UploadSurf(gpu, dev, r);
    }
    const bool single = flat2d && r->ms;  // into `surf`: the 3D drawn so far resolved into it first
    // (`surf` already the newest image for a 2D draw: nothing to copy into the multisampled one yet)
    if (r->ms && r->msStale && !(single && !r->msNewer)) {
        if (overwriteColor) r->msStale = false;
        else CopyToMs(gpu, dev, r);
        rebind = true;  // CopyToMs drew into it with no Z buffer
    }
    if (single) Resolve(gpu, dev, r);
    IDirect3DSurface9* target = r->ms && !single ? r->ms : r->surf;
    if (rebind || boundRt != target) {
        dev->SetRenderTarget(0, target);
        boundRt = target;
        dev->SetViewport(&vp);  // D3D9 resets it to the whole target; Direct3D 7 keeps its own
    }
    GpuSurf* zs = z ? EnsureSurf(gpu, dev, z->buf, true) : nullptr;
    if (zs && z->buf->gpuStale) {  // DirectDraw wrote into a writable Z buffer
        if (overwriteZ) z->buf->gpuStale = false;
        else UploadSurf(gpu, dev, zs);
        rebind = true;  // (drawn with its own targets, the device's put back)
    }
    IDirect3DSurface9* zsurf = zs && !single ? zs->surf : nullptr;  // (multisampled: not with `surf`)
    if (rebind || boundZ != zsurf) {  // also null: another context may have left its Z bound
        dev->SetDepthStencilSurface(zsurf);
        boundZ = zsurf;
    }
    rebind = false;
    if (zs && !single && !zs->fills.empty()) {  // (with `surf`: at the next 3D draw)
        if (!overwriteZ) {
            // Clear works inside the viewport only: the whole surface's for the fills.
            const D3DVIEWPORT9 keep = vp;
            D3DVIEWPORT9 whole = {0, 0, DWORD(z->buf->w), DWORD(z->buf->h), 0.0f, 1.0f};
            if (DWORD(rt->buf->w) < whole.Width) whole.Width = rt->buf->w;
            if (DWORD(rt->buf->h) < whole.Height) whole.Height = rt->buf->h;
            dev->SetViewport(&whole);
            for (const GpuSurf::Fill& f : zs->fills) {
                D3DRECT rect = {f.r.left, f.r.top, f.r.right, f.r.bottom};
                dev->Clear(1, &rect, D3DCLEAR_ZBUFFER | ((zs->fmt == D3DFMT_D24S8 || zs->fmt == kIntz) ? D3DCLEAR_STENCIL : 0), 0, f.z, 0);
            }
            dev->SetViewport(&keep);
        }
        zs->fills.clear();
    }
    return true;
}

void GpuContext::Impl::BindTextures() {
    // Each stage's texture as this function set it last, on which device, and the epoch then
    // (gpu.h): set again only when it changes (all 8 every draw before, most of them null).
    static IDirect3DDevice9* texDev = nullptr;
    static LONG texEpoch = 0;
    static IDirect3DBaseTexture9* texSet[8] = {};
    if (texDev != dev || texEpoch != g_deviceStateEpoch) {
        texDev = dev, texEpoch = g_deviceStateEpoch;
        for (auto& e : texSet) e = reinterpret_cast<IDirect3DBaseTexture9*>(~uintptr_t(0));  // (unknown)
    }
    for (DWORD i = 0; i < 8; i++) {
        IDirect3DBaseTexture9* t = tex[i] ? EnsureTex(gpu, dev, tex[i], colorKey) : nullptr;
        // (EnsureTex may draw on the GPU itself, RtTexture's copy: its SetTexture9 bumps the epoch.)
        if (texEpoch != g_deviceStateEpoch) {
            texEpoch = g_deviceStateEpoch;
            for (auto& e : texSet) e = reinterpret_cast<IDirect3DBaseTexture9*>(~uintptr_t(0));
        }
        if (t != texSet[i]) {
            dev->SetTexture(i, t);
            texSet[i] = t;
        }
        cutoutStage[i] = t && (TextureHasAlpha(tex[i]) || (tex[i]->gpuTex && tex[i]->gpuTex->shownKeyed));
        if (g_config.anisotropy > 1 || g_config.texLinear) {
            if (g_config.anisotropy > 1 && !anisoLimit) {
                D3DCAPS9 caps = {};
                anisoLimit = 1;
                if (SUCCEEDED(dev->GetDeviceCaps(&caps)) && (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC))
                    anisoLimit = std::max<DWORD>(1, std::min<DWORD>(DWORD(g_config.anisotropy), caps.MaxAnisotropy));
                DD2D9_LOG("anisotropy: %lu (the GPU: up to %lu, %s)", static_cast<unsigned long>(anisoLimit),
                          static_cast<unsigned long>(caps.MaxAnisotropy),
                          (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) ? "anisotropic minification" : "none");
            }
            // Cut-outs (colour-keyed or with alpha: trees, fences, sprites) keep the game's
            // filtering: made linear, their transparent texels blend into the edges (dark
            // fringes); made anisotropic, the longer footprint reaches past their edges, wraps
            // to the other side of the texture and draws a line of it into the transparent part
            // (NFS III's trees), and they are mostly seen face-on, where it changes nothing.
            const bool cutout = cutoutStage[i];
            DWORD wantMin = minFilter[i], wantMag = magFilter[i];
            // texfilter=linear: point-filtered textures drawn linear.
            if (g_config.texLinear && t && !cutout) {
                if (wantMin == D3DTEXF_POINT) wantMin = D3DTEXF_LINEAR;
                if (wantMag == D3DTEXF_POINT) wantMag = D3DTEXF_LINEAR;
            }
            // anisotropy=N: only where the filtering is smooth: point (pixel art, sharp on
            // purpose) stays.
            const bool smooth = t && anisoLimit > 1 && !cutout &&
                                (wantMin == D3DTEXF_LINEAR || wantMin == D3DTEXF_ANISOTROPIC);
            if (smooth) wantMin = D3DTEXF_ANISOTROPIC;
            const DWORD wantAniso = smooth ? std::max(anisoLimit, maxAniso[i]) : maxAniso[i];
            if (wantMin != minFilterSet[i]) {
                dev->SetSamplerState(i, D3DSAMP_MINFILTER, wantMin);
                minFilterSet[i] = wantMin;
            }
            if (wantMag != magFilterSet[i]) {
                dev->SetSamplerState(i, D3DSAMP_MAGFILTER, wantMag);
                magFilterSet[i] = wantMag;
            }
            if (wantAniso != maxAnisoSet[i]) {
                dev->SetSamplerState(i, D3DSAMP_MAXANISOTROPY, wantAniso);
                maxAnisoSet[i] = wantAniso;
            }
        }
        if (!g_config.mipmaps) continue;
        const bool autogen = t && tex[i]->gpuTex && tex[i]->gpuTex->autogen && t == tex[i]->gpuTex->tex;
        const DWORD want = autogen && mipFilter[i] == D3DTEXF_NONE ? DWORD(D3DTEXF_LINEAR) : mipFilter[i];
        if (want != mipFilterSet[i]) {
            dev->SetSamplerState(i, D3DSAMP_MIPFILTER, want);
            mipFilterSet[i] = want;
        }
    }
}

// antialiasing: a cut-out texture (trees, sprites) drawn within its own coordinates (0 to 1,
// not tiled) is addressed clamped for that draw. Multisampling takes a pixel's texture
// coordinate at its centre even where the polygon only covers part of it: past the
// sprite's edge, a little outside 0..1, where wrapping reaches the other side of the
// texture and draws a line of it at the edge (NFS III's trees and corn). Clamped, it reads
// the edge's own transparent texels; inside, nothing changes. Tiled ones keep the game's.
void GpuContext::Impl::CutoutAddr(DWORD fvf, const void* verts, UINT stride, UINT count, DWORD (&out)[8][2]) const {
    // Where each texture coordinate set sits in the vertex (as Decl lays it out).
    const DWORD pos = fvf & 0x0E;
    UINT off = pos == 0x04 ? 16 : 12 + (pos >= 0x06 ? (pos - 0x04) / 2 : 0) * 4;
    if (fvf & 0x10) off += 12;
    if (fvf & 0x20) off += 4;
    if (fvf & 0x40) off += 4;
    if (fvf & 0x80) off += 4;
    const DWORD sets = std::min<DWORD>((fvf >> 8) & 0xF, 8);
    UINT setOff[8] = {};
    bool set2d[8] = {};
    static const UINT kSize[] = {8, 12, 16, 4};
    for (DWORD k = 0; k < sets; k++) {
        const DWORD f = (fvf >> (16 + 2 * k)) & 3;
        setOff[k] = off;
        set2d[k] = f == 0;
        off += kSize[f];
    }
    for (DWORD i = 0; i < 8; i++) {
        DWORD want[2] = {addr[i][0], addr[i][1]};
        const DWORD k = tci[i];  // generated coordinates (high bits) and missing sets: the game's
        if (cutoutStage[i] && k < sets && set2d[k] && setOff[k] + 8 <= stride && verts && count) {
            float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
            const auto* base = static_cast<const uint8_t*>(verts) + setOff[k];
            for (UINT v = 0; v < count; v++) {
                const float* uv = reinterpret_cast<const float*>(base + size_t(v) * stride);
                for (int a = 0; a < 2; a++) {
                    lo[a] = std::min(lo[a], uv[a]);
                    hi[a] = std::max(hi[a], uv[a]);
                }
            }
            for (int a = 0; a < 2; a++)
                if ((want[a] == D3DTADDRESS_WRAP || want[a] == D3DTADDRESS_MIRROR) && lo[a] >= -0.001f && hi[a] <= 1.001f)
                    want[a] = D3DTADDRESS_CLAMP;
        }
        out[i][0] = want[0], out[i][1] = want[1];
    }
}

void GpuContext::Impl::ClampCutouts(DWORD fvf, const void* verts, UINT stride, UINT count) {
    DWORD want[8][2];
    CutoutAddr(fvf, verts, stride, count, want);
    SetAddr(want);
}

void GpuContext::Impl::SetAddr(const DWORD (&want)[8][2]) {
    for (DWORD i = 0; i < 8; i++)
        for (int a = 0; a < 2; a++)
            if (want[i][a] != addrSet[i][a]) {
                dev->SetSamplerState(i, a ? D3DSAMP_ADDRESSV : D3DSAMP_ADDRESSU, want[i][a]);
                addrSet[i][a] = want[i][a];
            }
}

void GpuContext::Clear(DWORD n, const RECT* rects, DWORD flags, DWORD color, float z, DWORD stencil) {
    Impl& p = *p_;
    if (!p.dev) return;
    // Whole: no rectangles with a viewport over the target, or one rectangle over all of it
    // (Direct3D 7 games often pass the viewport's rectangle).
    const bool whole = p.rt && (n == 0 ? p.CoversTarget()
                                       : n == 1 && rects[0].left <= 0 && rects[0].top <= 0 &&
                                             rects[0].right >= p.rt->buf->w && rects[0].bottom >= p.rt->buf->h);
    if (!p.BindTargets(whole && (flags & D3DCLEAR_TARGET), whole && (flags & D3DCLEAR_ZBUFFER))) return;
    if (!p.boundZ) flags &= ~DWORD(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
    if (!flags) return;
    // A Z buffer that fell back to D16 has no stencil: D3D9 would refuse the whole clear.
    if (p.z && p.z->buf->gpu && p.z->buf->gpu->fmt != D3DFMT_D24S8 && p.z->buf->gpu->fmt != kIntz)
        flags &= ~DWORD(D3DCLEAR_STENCIL);
    if (!flags) return;
    p.dev->Clear(n, reinterpret_cast<const D3DRECT*>(rects), flags, color, z, stencil);
    // Emulated W buffering's scale starts again with each whole depth clear (a frame): one
    // stray near vertex shrinks it for that frame only, not for good.
    if ((flags & D3DCLEAR_ZBUFFER) && whole) g_wScale = 1.0f;
    if (p.rt->isPrimary) p.rt->MarkDirtyIfVisible();  // drawn into the front surface itself
    if (flags & D3DCLEAR_TARGET) {
        p.rt->buf->cpuStale = true;
        p.rt->buf->gpu->msNewer = p.rt->buf->gpu->ms != nullptr;
        p.rt->buf->gpu->serial = ++g_drawSerial;
        p.rt->buf->gpuStale = false;
    }
    if ((flags & D3DCLEAR_ZBUFFER) && p.z && p.z->buf->gpu && p.z->buf->gpu->ztex) {
        p.z->buf->cpuStale = true;  // a writable Z buffer: read back when DirectDraw reads it
        if (whole) p.z->buf->gpuStale = false;
    }
}

// Direct3D 7 FVF to a Direct3D 9 vertex declaration: the same offsets, but D3DFVF_RESERVED1
// (D3DLVERTEX's spare DWORD, D3D9's PSIZE) skipped, and five blend weights cut to four.
IDirect3DVertexDeclaration9* GpuContext::Impl::Decl(DWORD fvf) {
    if (fvf == lastFvf) return lastDecl;
    auto it = decls.find(fvf);
    if (it != decls.end()) {
        lastFvf = fvf;
        lastDecl = it->second;
        return it->second;
    }
    std::vector<D3DVERTEXELEMENT9> e;
    WORD off = 0;
    auto add = [&](BYTE type, BYTE usage, BYTE index, WORD size) {
        e.push_back({0, off, type, D3DDECLMETHOD_DEFAULT, usage, index});
        off += size;
    };
    const DWORD pos = fvf & 0x0E;
    if (pos == 0x04) {
        add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0, 16);
    } else {
        add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12);  // XYZ, the blends, and 0 (taken as XYZ)
        const int weights = pos >= 0x06 ? (pos - 0x04) / 2 : 0;
        if (weights) {
            static const BYTE kType[] = {0, D3DDECLTYPE_FLOAT1, D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4};
            const int used = std::min(weights, 4);
            add(kType[used], D3DDECLUSAGE_BLENDWEIGHT, 0, WORD(used * 4));
            off += WORD((weights - used) * 4);
        }
    }
    if (fvf & 0x10) add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0, 12);
    if (fvf & 0x20) off += 4;  // D3DFVF_RESERVED1
    if (fvf & 0x40) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
    if (fvf & 0x80) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);
    const DWORD texCount = std::min<DWORD>((fvf >> 8) & 0xF, 8);
    for (DWORD i = 0; i < texCount; i++) {
        static const BYTE kType[] = {D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4, D3DDECLTYPE_FLOAT1};
        static const WORD kSize[] = {8, 12, 16, 4};
        const DWORD fmt = (fvf >> (16 + 2 * i)) & 3;
        add(kType[fmt], D3DDECLUSAGE_TEXCOORD, BYTE(i), kSize[fmt]);
    }
    e.push_back(D3DDECL_END());
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (FAILED(dev->CreateVertexDeclaration(e.data(), &decl))) {
        DD2D9_LOG("Direct3D: no vertex declaration for FVF 0x%lx", fvf);
        decl = nullptr;
    }
    decls[fvf] = decl;
    lastFvf = fvf;
    lastDecl = decl;
    return decl;
}

// One pass over transformed vertices (x, y, z, rhw first; stride 16 or more), each vertex
// one vector, in vertex order: minps and maxps with the vertex first give std::min's and
// std::max's answers (NaN, and the zeros' signs, included), so the loops this replaces decide
// the same.
struct TlScan {
    float x0, y0, x1, y1;  // x and y from lowest to highest
    bool flat;             // every vertex at vertex 0's z and rhw
    bool zOut;             // a z below 0 or above 1
    bool badRhw;           // an rhw that isn't a positive normal number (0, negative, a denormal, NaN, infinite)
    float minRhw, maxRhw;  // the others' (0: none)
};
static TlScan ScanTl(const uint8_t* src, UINT stride, UINT n) {
    const __m128 v0 = _mm_loadu_ps(reinterpret_cast<const float*>(src));
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f), inf = _mm_set1_ps(INFINITY);
    const __m128 normMin = _mm_set1_ps(FLT_MIN), normMax = _mm_set1_ps(FLT_MAX);
    __m128 lo = v0, hi = v0, eq = _mm_cmpeq_ps(zero, zero), out = zero, bad = zero, gMin = inf, gMax = zero;
    for (UINT i = 0; i < n; i++) {
        const __m128 v = _mm_loadu_ps(reinterpret_cast<const float*>(src + size_t(i) * stride));
        if (i) {
            lo = _mm_min_ps(v, lo);  // v < lo ? v : lo, as std::min(lo, v)
            hi = _mm_max_ps(v, hi);  // v > hi ? v : hi, as std::max(hi, v)
            eq = _mm_and_ps(eq, _mm_cmpeq_ps(v, v0));
        }
        out = _mm_or_ps(out, _mm_or_ps(_mm_cmplt_ps(v, zero), _mm_cmpgt_ps(v, one)));
        const __m128 good = _mm_and_ps(_mm_cmpge_ps(v, normMin), _mm_cmple_ps(v, normMax));
        bad = _mm_or_ps(bad, _mm_andnot_ps(good, _mm_cmpeq_ps(zero, zero)));
        gMin = _mm_min_ps(_mm_or_ps(_mm_and_ps(good, v), _mm_andnot_ps(good, inf)), gMin);
        gMax = _mm_max_ps(_mm_and_ps(good, v), gMax);
    }
    alignas(16) float l[4], h[4], mn[4], mx[4];
    _mm_store_ps(l, lo), _mm_store_ps(h, hi), _mm_store_ps(mn, gMin), _mm_store_ps(mx, gMax);
    TlScan r;
    r.x0 = l[0], r.y0 = l[1], r.x1 = h[0], r.y1 = h[1];
    r.flat = (_mm_movemask_ps(eq) & 0xC) == 0xC;
    r.zOut = (_mm_movemask_ps(out) & 4) != 0;
    r.badRhw = (_mm_movemask_ps(bad) & 8) != 0;
    r.minRhw = mn[3] == INFINITY ? 0.0f : mn[3];
    r.maxRhw = mx[3];
    return r;
}

void GpuContext::Draw(DWORD primType, DWORD fvf, const void* verts, UINT stride, UINT vertexCount,
                      const WORD* indices, UINT indexCount, UINT primCount, bool zEnabled, const DrawSeg* segs,
                      UINT segCount) {
    Impl& p = *p_;
    if (!p.dev || !primCount || !verts || !vertexCount) return;
    if (segCount < 2) segs = nullptr, segCount = 0;
    // Several draws (segs), one by one: as each would have been drawn on its own.
    auto drawEach = [&] {
        std::vector<WORD> own;
        for (UINT k = 0; k < segCount; k++) {
            const DrawSeg& g = segs[k];
            if (g.indexed) {
                own.assign(indices + g.firstIndex, indices + g.firstIndex + g.indexCount);
                for (WORD& i : own) i = WORD(i - g.firstVertex);
            }
            Draw(primType, fvf, static_cast<const uint8_t*>(verts) + size_t(g.firstVertex) * stride, stride,
                 g.vertexCount, g.indexed ? own.data() : nullptr, g.indexed ? g.indexCount : 0, g.primCount, zEnabled);
        }
    };
    // antialias2d=off: a 2D draw (a HUD, a menu, text) skips antialiasing: drawn into the
    // resolved image after the 3D so far, pixel for pixel, its edges as the game drew them
    // (multisampled, a HUD's edges came out soft). 2D: transformed vertices at one depth
    // and one rhw (screen-aligned, no perspective: a sky dome has it), the depth test off,
    // or passing always (ALWAYS) without writing depth: `surf` has no Z buffer, and a 2D
    // draw that writes depth hides what is drawn behind it later (NFS Porsche Unleashed's
    // menu labels, written so, were covered by the translucent buttons drawn after: dim).
    // The next 3D draw copies the part drawn (msDirty) into the multisampled image first,
    // as for the CPU's drawing.
    RECT flatRect = {};
    p.flat2d = false;
    const bool tl = (fvf & 0x0E) == 0x04 && stride >= 16;
    // Several draws (segs): their vertices only, each scanned once (between their ranges may
    // lie vertices none of them uses).
    static std::vector<std::pair<UINT, UINT>> spans;  // (every caller holds g_lock)
    TlScan scan{};
    if (tl && !segs) {
        scan = ScanTl(static_cast<const uint8_t*>(verts), stride, vertexCount);
    } else if (tl) {
        spans.clear();
        for (UINT k = 0; k < segCount; k++) spans.emplace_back(segs[k].firstVertex, segs[k].firstVertex + segs[k].vertexCount);
        std::sort(spans.begin(), spans.end());
        bool firstSpan = true;
        for (size_t k = 0; k < spans.size();) {
            UINT a = spans[k].first, b = spans[k].second;
            for (k++; k < spans.size() && spans[k].first <= b; k++) b = std::max(b, spans[k].second);
            const TlScan t = ScanTl(static_cast<const uint8_t*>(verts) + size_t(a) * stride, stride, b - a);
            if (firstSpan) {
                scan = t;
            } else {
                scan.zOut |= t.zOut;
                scan.badRhw |= t.badRhw;
                scan.minRhw = scan.minRhw == 0.0f ? t.minRhw : t.minRhw == 0.0f ? scan.minRhw : std::min(scan.minRhw, t.minRhw);
                scan.maxRhw = std::max(scan.maxRhw, t.maxRhw);
            }
            firstSpan = false;
        }
        scan.flat = false;  // (2D: decided draw by draw below)
    }
    const bool depthNeither = !zEnabled || !p.z || (p.rsMirror[D3DRS_ZFUNC] == D3DCMP_ALWAYS && !p.zWrite);
    const bool flatTest = !g_config.aa2d && tl && depthNeither && p.rsMirror[D3DRS_STENCILENABLE] != TRUE && p.rt &&
                          p.rt->buf->gpu && p.rt->buf->gpu->ms;  // (no depth-stencil with `surf`)
    // Several draws: each one's own scan, for what's decided from it below.
    static std::vector<TlScan> segScan;  // (every caller holds g_lock)
    if (segs) {
        if (!tl || scan.badRhw) return drawEach();  // (DrawPVerts' are transformed; bad rhw: the draw's smallest valid one)
        if (flatTest || p.wEmulate) {  // (only what's decided from them below)
            segScan.resize(segCount);
            for (UINT k = 0; k < segCount; k++) {
                segScan[k] = ScanTl(static_cast<const uint8_t*>(verts) + size_t(segs[k].firstVertex) * stride, stride,
                                    segs[k].vertexCount);
                if (flatTest && segScan[k].flat) return drawEach();  // a 2D draw: drawn into `surf`
            }
        }
    }
    if (flatTest) {
        const float x0 = scan.x0, y0 = scan.y0, x1 = scan.x1, y1 = scan.y1;
        if (scan.flat && std::isfinite(x0) && std::isfinite(x1) && std::isfinite(y0) && std::isfinite(y1)) {
            const float w = float(p.rt->buf->w), h = float(p.rt->buf->h);
            flatRect = {LONG(std::clamp(std::floor(x0) - 1.0f, 0.0f, w)), LONG(std::clamp(std::floor(y0) - 1.0f, 0.0f, h)),
                        LONG(std::clamp(std::ceil(x1) + 1.0f, 0.0f, w)), LONG(std::clamp(std::ceil(y1) + 1.0f, 0.0f, h))};
            p.flat2d = true;
        }
    }
    const bool bound = p.BindTargets(false, false);
    const bool flat2d = p.flat2d;
    p.flat2d = false;
    if (!bound) return;
    p.BindTextures();
    if (segs) {
        // Untextured rhw flattened (below): the whole draw's or none.
        bool apart = (scan.minRhw > 0.0f && scan.maxRhw > scan.minRhw * 10000.0f) && (!p.tex[0] || scan.minRhw == 0.0f);
        if (p.wEmulate) {  // the W scale each would have been drawn with, the scale as one
            const float all = scan.maxRhw * g_wScale > 1.0f ? 0.5f / scan.maxRhw : g_wScale;
            float s = g_wScale;
            for (UINT k = 0; k < segCount && !apart; k++) {
                if (segScan[k].maxRhw * s > 1.0f) s = 0.5f / segScan[k].maxRhw;
                apart = s != all;
            }
        }
        if (apart) return drawEach();
    }
    if (MsaaType(p.dev) != D3DMULTISAMPLE_NONE) {
        if (!segs) {
            p.ClampCutouts(fvf, verts, stride, vertexCount);
        } else {  // each draw's address modes for the cut-outs: the same, or one by one
            DWORD all[8][2], own[8][2];
            for (UINT k = 0; k < segCount; k++) {
                p.CutoutAddr(fvf, static_cast<const uint8_t*>(verts) + size_t(segs[k].firstVertex) * stride, stride,
                              segs[k].vertexCount, k ? own : all);
                if (k && memcmp(all, own, sizeof all) != 0) return drawEach();
            }
            p.SetAddr(all);
        }
    }
    IDirect3DVertexDeclaration9* decl = p.Decl(fvf);
    if (!decl) return;
    // The declaration this function set last, on which device, and the epoch then (gpu.h).
    static IDirect3DDevice9* declDev = nullptr;
    static IDirect3DVertexDeclaration9* declSet = nullptr;
    static LONG declEpoch = 0;
    if (decl != declSet || p.dev != declDev || declEpoch != g_deviceStateEpoch) {
        p.dev->SetVertexDeclaration(decl);
        declDev = p.dev, declSet = decl, declEpoch = g_deviceStateEpoch;
    }
    // Transformed vertices: Direct3D 7 clips their z to [0, 1] only with ZENABLE on, so it
    // can't matter with it off, but D3D9 always clips: clamp it then. rhw 0 on every vertex
    // (2D drawn through Direct3D) is 1: the same picture, without DXVK's infinities.
    // alternatepixelcenter: everything half a pixel up and left, so that pixel centres fall
    // where the drivers that offered it put them (texel centres for 2D drawn 1:1, no lines at
    // texture edges): transformed vertices moved, the projection of the others too.
    // overlaybias: a pass laid on a surface already drawn is pulled forward by N 16-bit
    // depth steps (on: one) and a slope-scaled one pixel's depth change. Its depth equals
    // the surface's only to within rounding (the polygons are cut differently), which the
    // 16-bit depth buffers of the era rounded the same way, and the overlay won; with
    // today's 24 bits less-or-equal fails on half its pixels, in a fine hatch (NFS III's
    // headlights and light maps on the road). The games' depth maths was made for 16 bits,
    // whatever Z buffer they ask for now (NFS3+: 32 bits). An overlay is a pass that
    // doesn't write depth, or one that adds or multiplies (D3DRS_DESTBLEND ONE or a colour
    // factor, or SRCBLEND ZERO or DESTCOLOR): ordinary alpha blending doesn't count (NFS III
    // blends everything). Overlays that would write depth don't, meanwhile: the next light
    // laid on the same spot would fight the first's depth as the first fought the road's.
    {
        const DWORD src = p.rsMirror[D3DRS_SRCBLEND], dst = p.rsMirror[D3DRS_DESTBLEND];
        const bool combines = p.alphaBlend && (dst == D3DBLEND_ONE || dst == D3DBLEND_SRCCOLOR ||
                                               dst == D3DBLEND_INVSRCCOLOR || dst == D3DBLEND_DESTCOLOR ||
                                               src == D3DBLEND_ZERO || src == D3DBLEND_DESTCOLOR);
        const bool overlay = g_config.overlayBias > 0 && zEnabled && (!p.zWrite || combines);
        const float bias = p.gameBias - (overlay ? float(g_config.overlayBias) / 65535.0f : 0.0f);
        const float slope = overlay ? -1.0f : 0.0f;  // D3DRS_SLOPESCALEDEPTHBIAS: a pixel's depth change
        const bool write = p.zWrite && !overlay;
        if (bias != p.biasSet) {
            DWORD b;
            memcpy(&b, &bias, 4);
            p.dev->SetRenderState(D3DRENDERSTATETYPE(195), b);
            p.biasSet = bias;
        }
        if (slope != p.slopeSet) {
            DWORD b;
            memcpy(&b, &slope, 4);
            p.dev->SetRenderState(D3DRENDERSTATETYPE(175), b);
            p.slopeSet = slope;
        }
        if (write != p.zWriteSet) {
            p.dev->SetRenderState(D3DRS_ZWRITEENABLE, write);
            p.zWriteSet = write;
        }
    }
    // Transformed vertices aren't clipped: the hardware of the era clamped their depth to
    // the depth range per pixel, and so does D3D9 without clipping. With it, D3D9 cuts
    // triangles where their z leaves [0, 1] (NFS Porsche Unleashed's sky haze, z up to 11,
    // was cut in wedges on Intel's driver; Wine's own Direct3D clamps). Their x and y stay
    // within the guard band: the game's or dd2d9's own clipper (Direct3D 3-6) sees to it.
    const DWORD clipping = (fvf & 0x0E) == 0x04 ? DWORD(FALSE) : p.gameClipping;
    if (clipping != p.clippingSet) {
        p.dev->SetRenderState(D3DRS_CLIPPING, clipping);
        p.clippingSet = clipping;
    }
    const bool shift = g_config.pixelCenter;
    if (shift && (fvf & 0x0E) != 0x04 && p.projDirty) {
        D3DMATRIX m = p.proj;
        const float kx = -1.0f / float(std::max<DWORD>(p.vp.Width, 1)), ky = 1.0f / float(std::max<DWORD>(p.vp.Height, 1));
        for (int r = 0; r < 4; r++) {  // clip x += kx * w, clip y += ky * w
            m.m[r][0] += m.m[r][3] * kx;
            m.m[r][1] += m.m[r][3] * ky;
        }
        p.dev->SetTransform(D3DTS_PROJECTION, &m);
        p.projDirty = false;
    }
    if (tl) {
        const auto* src = static_cast<const uint8_t*>(verts);
        // Bad rhw: hardware of the era interpolated colour across the screen without rhw and
        // used it for texture perspective only; Direct3D 9 weighs every attribute by it, and
        // a triangle whose rhw are all 0 comes out black (Moto Racer's menu letters:
        // untextured, rhw 0 or near it on part of the vertices). Bad: 0, negative, not a
        // number, or a denormal (the GPU takes it as 0). A draw with any bad rhw gets rhw 1 on
        // every vertex when untextured (colour linear on the screen, as then), else the bad
        // ones the draw's smallest valid rhw. An untextured draw whose rhw differ over 10000
        // times is flattened too: the smallest weigh nothing and flush to 0 as easily.
        const bool clampZ = !zEnabled && scan.zOut;  // (ScanTl)
        bool badRhw = scan.badRhw;
        const float minRhw = scan.minRhw, maxRhw = scan.maxRhw;
        const bool flatRhw = (badRhw || (minRhw > 0.0f && maxRhw > minRhw * 10000.0f)) && (!p.tex[0] || minRhw == 0.0f);
        badRhw = badRhw || flatRhw;
        const bool wz = p.wEmulate;
        // The scale for the whole draw at once, from its nearest valid w: shrunk vertex by
        // vertex, a triangle's corners before the nearest one got another scale than those
        // after (its depth warped); an infinite rhw (bad, above) made it 0 and every depth
        // after it 1 until the next whole clear.
        if (wz && maxRhw * g_wScale > 1.0f) g_wScale = 0.5f / maxRhw;
        if (clampZ || badRhw || shift || wz) {
            p.scratch.assign(src, src + size_t(vertexCount) * stride);
            for (UINT i = 0; i < vertexCount; i++) {
                float* v = reinterpret_cast<float*>(p.scratch.data() + size_t(i) * stride);
                if (clampZ) v[2] = std::clamp(v[2], 0.0f, 1.0f);
                // Emulated W buffering: depth from 1/w (rhw), which varies linearly across
                // the screen over any flat surface, as the Z buffer interpolates it: front to
                // back as a W buffer orders, and coplanar polygons agree however they are cut
                // up (the game's own z may not: NFS III's light maps fought with the road).
                // The scale keeps the nearest w seen within [0, 1], 24 bits for the rest.
                if (wz) {
                    const float rhw = v[3] >= FLT_MIN && std::isfinite(v[3]) ? v[3] : 0.0f;  // (bad: as far as can be)
                    v[2] = 1.0f - rhw * g_wScale;
                }
                if (flatRhw) v[3] = 1.0f;
                else if (badRhw && (!(v[3] >= FLT_MIN) || !std::isfinite(v[3]))) v[3] = minRhw;
                if (shift) {
                    v[0] -= 0.5f;
                    v[1] -= 0.5f;
                }
            }
            verts = p.scratch.data();
        }
    }
    if (TracingFrame()) {
        float zlo = 1e30f, zhi = -1e30f, wlo = 1e30f, whi = -1e30f;
        for (UINT i = 0; tl && i < vertexCount; i++) {
            const float* v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(verts) + size_t(i) * stride);
            zlo = std::min(zlo, v[2]);
            zhi = std::max(zhi, v[2]);
            wlo = std::min(wlo, v[3]);
            whi = std::max(whi, v[3]);
        }
        const DWORD* rs = p.rsMirror;
        Surface* t0 = p.tex[0];
        float bias, slope;
        memcpy(&bias, &p.biasSet, 4);
        slope = p.slopeSet;
        DD2D9_LOG("drawtrace: prim %lu x%u v%u fvf 0x%lx z(en %lu wr %lu fn %lu) blend(%lu %lu>%lu) atest(%lu %lu ref %lu) "
                  "tex0 %p %dbpp%s%s bias %g slope %g fog(en %lu v %lu t %lu) | z %.6f..%.6f rhw %g..%g%s",
                  primType, primCount, vertexCount, fvf, rs[7], rs[14], rs[23], rs[27], rs[19], rs[20], rs[15], rs[25], rs[24],
                  static_cast<void*>(t0), t0 ? int(t0->buf->fmt.bpp) : 0, t0 && TextureHasAlpha(t0) ? " alpha" : "",
                  t0 && t0->gpuTex && t0->gpuTex->shownKeyed ? " keyed" : "", bias, slope, rs[28], rs[140], rs[35],
                  tl ? zlo : 0.0f, tl ? zhi : 0.0f, tl ? wlo : 0.0f, tl ? whi : 0.0f, flat2d ? " FLAT2D" : "");
    }
    p.gpu.OpenScene();
    if (indices) {
        // Only the vertices the indices use are copied to the GPU (D3D9 takes them from the
        // lowest one): a few triangles from a big vertex buffer don't send all of it.
        WORD lo = 0xFFFF, hi = 0;
        for (UINT i = 0; i < indexCount; i++) {
            lo = std::min(lo, indices[i]);
            hi = std::max(hi, indices[i]);
        }
        if (lo > hi || hi >= vertexCount) lo = 0, hi = WORD(std::min<UINT>(vertexCount, 0x10000) - 1);  // (no indices)
        p.dev->DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE(primType), lo, UINT(hi - lo) + 1, primCount, indices,
                                      D3DFMT_INDEX16, verts, stride);
    } else {
        p.dev->DrawPrimitiveUP(D3DPRIMITIVETYPE(primType), primCount, verts, stride);
    }
    p.rt->buf->cpuStale = true;
    p.rt->buf->gpuStale = false;
    if (p.z && p.boundZ && zEnabled && p.zWriteSet && p.z->buf->gpu && p.z->buf->gpu->ztex)
        p.z->buf->cpuStale = true;  // a writable Z buffer: read back when DirectDraw reads it
    GpuSurf* g = p.rt->buf->gpu;
    if (flat2d && g->ms) {  // drawn into `surf`: the multisampled image lacks it there
        if (!g->msStale) g->msDirty = flatRect;
        else UnionRect(&g->msDirty, &g->msDirty, &flatRect);
        g->msStale = true;
        g->msNewer = false;
    } else {
        g->msNewer = g->ms != nullptr;
    }
    g->serial = ++g_drawSerial;
    if (p.rt->isPrimary) p.rt->MarkDirtyIfVisible();  // drawn into the front surface itself
}

void GpuContext::EndScene() {
    if (TracingFrame()) DD2D9_LOG("drawtrace: end of scene %u", g_traceScene);
    g_traceScene++;
    p_->gpu.CloseScene();
}

}  // namespace dd2d9
