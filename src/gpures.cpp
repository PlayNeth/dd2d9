// Direct3D's use of the shared Direct3D 9 device: surfaces' GPU copies and the drawing
// context of a Direct3D 7 device (see gpures.h).
#include "gpures.h"

#include "convert.h"

#include "gpu.h"
#include "objects.h"

#include <d3d9.h>

#include <algorithm>
#include <climits>
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
    bool msStale = false;  // `surf` has the CPU's changes `ms` lacks
    IDirect3DSurface9* sysmem = nullptr;   // read-back staging
    IDirect3DSurface9* upload = nullptr;   // upload staging (D3DPOOL_DEFAULT, lockable)
    // A DDBLT_DEPTHFILL waiting for the Z buffer's next use (it can't be written directly).
    bool pendingClear = false;
    RECT clearRect = {};
    float clearZ = 1.0f;
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
        msNewer = msStale = false;
    }
    // Device reset: D3DPOOL_DEFAULT goes, and what only the GPU had with it.
    void ReleaseGpuResources() override {
        Drop();
        buf->cpuStale = false;
        buf->gpuStale = true;
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

// The buffer's render-target (or Z) surface, made if needed. Gpu lock held, device usable.
static GpuSurf* EnsureSurf(Gpu& gpu, IDirect3DDevice9* d, Buffer* b, bool depth) {
    if (b->gpu && b->gpu->gpu != &gpu) return nullptr;  // another object's device (not expected)
    if (!b->gpu) {
        b->gpu = new GpuSurf(&gpu, b, depth);
        gpu.AddOwner(b->gpu);
    }
    GpuSurf* g = b->gpu;
    if (g->surf) return g;
    HRESULT hr;
    const D3DMULTISAMPLE_TYPE msaa = MsaaType(d);
    if (depth) {
        g->fmt = DepthFormat(b->fmt);
        if (msaa) g->fmt = b->fmt.a ? D3DFMT_D24S8 : D3DFMT_D24X8;
        hr = d->CreateDepthStencilSurface(b->w, b->h, g->fmt, msaa, 0, FALSE, &g->surf, nullptr);
        if (FAILED(hr) && g->fmt != D3DFMT_D16 && !msaa) {
            g->fmt = D3DFMT_D16;
            hr = d->CreateDepthStencilSurface(b->w, b->h, g->fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &g->surf, nullptr);
        }
        b->gpuStale = false;  // nothing to upload: a Z buffer starts cleared by the game
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
        return nullptr;
    }
    return g;
}

// antialiasing: the CPU's changes (in `surf`) into the multisampled surface, drawn as a
// quad from a texture copy of `surf` (StretchRect can't write a multisampled surface). The
// device's state is kept around it (a state block): it happens in the middle of the game's
// drawing. Gpu lock held.
static void CopyToMs(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g) {
    g->msStale = false;
    const Buffer* b = g->buf;
    if (!g->msCopy && FAILED(d->CreateTexture(b->w, b->h, 1, D3DUSAGE_RENDERTARGET, g->fmt, D3DPOOL_DEFAULT, &g->msCopy, nullptr))) {
        g->msCopy = nullptr;
        return;
    }
    IDirect3DSurface9* level = nullptr;
    if (FAILED(g->msCopy->GetSurfaceLevel(0, &level))) return;
    gpu.CloseScene();
    const HRESULT hr = d->StretchRect(g->surf, nullptr, level, nullptr, D3DTEXF_NONE);
    level->Release();
    if (FAILED(hr)) return;
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(d->CreateStateBlock(D3DSBT_ALL, &sb))) return;
    d->SetRenderTarget(0, g->ms);
    d->SetDepthStencilSurface(nullptr);
    const D3DVIEWPORT9 vp = {0, 0, DWORD(b->w), DWORD(b->h), 0.0f, 1.0f};
    d->SetViewport(&vp);
    d->SetPixelShader(nullptr);
    d->SetVertexShader(nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetTexture(0, g->msCopy);
    for (DWORD i = 1; i < 8; i++) d->SetTexture(i, nullptr);
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
    const float w = float(b->w), h = float(b->h);
    const float q[4][6] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {w - 0.5f, -0.5f, 0, 1, 1, 0},
                           {-0.5f, h - 0.5f, 0, 1, 0, 1}, {w - 0.5f, h - 0.5f, 0, 1, 1, 1}};
    gpu.OpenScene();
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
    sb->Apply();
    sb->Release();
}

// antialiasing: what Direct3D drew, into the resolved image (before it is read).
static void Resolve(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g) {
    if (!g->ms || !g->msNewer || !g->surf) return;
    gpu.CloseScene();
    d->StretchRect(g->ms, nullptr, g->surf, nullptr, D3DTEXF_NONE);
    g->msNewer = false;
}

// The CPU copy into a render target.
static void UploadSurf(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g) {
    Buffer* b = g->buf;
    b->gpuStale = false;
    if (g->depth) return;  // can't be written: only DDBLT_DEPTHFILL reaches it (pendingClear)
    if (!g->upload &&
        FAILED(d->CreateOffscreenPlainSurface(b->w, b->h, g->fmt, D3DPOOL_DEFAULT, &g->upload, nullptr))) {
        g->upload = nullptr;
        return;
    }
    D3DLOCKED_RECT lr;
    if (FAILED(g->upload->LockRect(&lr, nullptr, 0))) return;
    const RowConverter conv(b->fmt, g->pal ? g->pal->pal : nullptr);
    for (int y = 0; y < b->h; y++) RowToGpu(b, y, static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, g->convert ? &conv : nullptr);
    g->upload->UnlockRect();
    gpu.CloseScene();
    d->StretchRect(g->upload, nullptr, g->surf, nullptr, D3DTEXF_NONE);
    if (g->ms) g->msStale = true;  // copied on into the multisampled one when drawn into
}

// A render target back into the CPU copy.
static void ReadBack(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g) {
    Buffer* b = g->buf;
    b->cpuStale = false;
    if (g->depth || !g->surf) return;
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
    b->cpuSerial++;  // the CPU copy changed: a texture made from it is behind
}

void GpuCpuAccess(Surface* s, bool write) {
    Buffer* b = s->buf;
    GpuSurf* g = b->gpu;
    if (g && g->gpu && b->cpuStale) {
        Gpu& gpu = *g->gpu;
        GpuGuard l(gpu);
        if (IDirect3DDevice9* d = gpu.Ensure(); d && g->surf) ReadBack(gpu, d, g);
        b->cpuStale = false;
    }
    if (write) b->gpuStale = true;
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
    g->pendingClear = true;
    g->clearRect = r;
    g->clearZ = maxv ? float(double((value & zmask) >> shift) / maxv) : 1.0f;
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
    UINT levels = 0;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    bool convert = false;
    // What the GPU copy was made with: a change means uploading again.
    const Palette* pal = nullptr;
    DWORD palVersion = 0;
    bool keyed = false;
    DWORD key = 0;
    bool autogen = false;  // mipmaps=on: one level from the game, the rest made by the GPU
    std::vector<Buffer*> levelBufs;
    std::vector<unsigned> levelSerials;  // each level's Buffer::cpuSerial when uploaded
    // A render target used as a texture while the GPU has its newest pixels: a copy made on
    // the GPU (no read back through the CPU), of `rtSrc` as its serial was `rtSerial`.
    IDirect3DTexture9* rtTex = nullptr;
    const void* rtSrc = nullptr;
    unsigned rtSerial = 0;
    UINT rtW = 0, rtH = 0;
    D3DFORMAT rtFmt = D3DFMT_UNKNOWN;

    GpuTex(Gpu* g, Surface* s) : gpu(g), top(s) {}
    void Drop() {
        if (tex) tex->Release();
        tex = nullptr;
        levelBufs.clear();
        levelSerials.clear();
        if (rtTex) rtTex->Release();
        rtTex = nullptr;
        rtSrc = nullptr;
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

// The texture's GPU copy, uploaded where the CPU copy changed. Gpu lock held.
static IDirect3DTexture9* EnsureTex(Gpu& gpu, IDirect3DDevice9* d, Surface* s, bool colorKey) {
    if (s->gpuTex && s->gpuTex->gpu != &gpu) return nullptr;
    if (!s->gpuTex) {
        s->gpuTex = new GpuTex(&gpu, s);
        gpu.AddOwner(s->gpuTex);
    }
    GpuTex* t = s->gpuTex;
    std::vector<Surface*> chain;
    for (Surface* l = s; l && chain.size() < 16; l = l->NextMipLevel()) chain.push_back(l);
    const Palette* pal = s->buf->fmt.pal8 ? TexturePalette(s) : nullptr;
    // The colour key becomes alpha only while D3DRENDERSTATE_COLORKEYENABLE is on.
    const bool keyed = colorKey && s->hasCk[kCkSrcBlt] && !s->buf->fmt.fourcc;
    const DWORD key = keyed ? s->ck[kCkSrcBlt].dwColorSpaceLowValue : 0;
    // Rendered into by Direct3D and not touched by the CPU since: copied on the GPU. (Mipmapped
    // or colour-keyed ones go through the CPU copy below.)
    if (chain.size() == 1 && !keyed) {
        Buffer* b = s->buf;
        GpuSurf* g = b->gpu;
        if (g && g->surf && b->cpuStale && !b->gpuStale) {
            if (!t->rtTex || t->rtW != UINT(b->w) || t->rtH != UINT(b->h) || t->rtFmt != g->fmt) {
                if (t->rtTex) t->rtTex->Release();
                t->rtTex = nullptr;
                if (FAILED(d->CreateTexture(b->w, b->h, 1, D3DUSAGE_RENDERTARGET, g->fmt, D3DPOOL_DEFAULT, &t->rtTex,
                                            nullptr)))
                    t->rtTex = nullptr;
                t->rtW = b->w;
                t->rtH = b->h;
                t->rtFmt = g->fmt;
                t->rtSrc = nullptr;
            }
            if (t->rtTex) {
                if (t->rtSrc != g || t->rtSerial != g->serial) {
                    Resolve(gpu, d, g);
                    IDirect3DSurface9* level = nullptr;
                    if (SUCCEEDED(t->rtTex->GetSurfaceLevel(0, &level))) {
                        gpu.CloseScene();  // StretchRect: outside a scene
                        if (SUCCEEDED(d->StretchRect(g->surf, nullptr, level, nullptr, D3DTEXF_NONE))) {
                            t->rtSrc = g;
                            t->rtSerial = g->serial;
                        }
                        level->Release();
                    }
                }
                if (t->rtSrc == g) {
                    t->levelBufs.assign(chain.size(), nullptr);  // the managed copy is behind now
                    return t->rtTex;
                }
            }
        }
    }
    bool all = false;
    if (!t->tex || t->levels != chain.size() || keyed != t->keyed) {
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
        if (g_config.mipmaps && chain.size() == 1 && !keyed && !s->buf->fmt.a && !s->buf->fmt.fourcc &&
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
        if (FAILED(hr) && !t->convert && !s->buf->fmt.fourcc) {  // a layout this GPU lacks: convert it
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
        D3DLOCKED_RECT lr;
        if (FAILED(t->tex->LockRect(i, &lr, nullptr, 0))) continue;
        if (b->fmt.BlockCompressed()) {
            // Rows of 4x4 blocks: the CPU copy's pitch is a quarter of a block row.
            const int rows = (b->h + 3) / 4;
            const size_t rowBytes = size_t(b->pitch) * 4;
            for (int y = 0; y < rows; y++)
                memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, b->bits + y * rowBytes,
                       std::min<size_t>(rowBytes, size_t(lr.Pitch)));
        } else {
            const RowConverter conv(b->fmt, entries, keyed, key);
            for (int y = 0; y < b->h; y++)
                RowToGpu(b, y, static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, t->convert ? &conv : nullptr);
        }
        t->tex->UnlockRect(i);
        t->levelBufs[i] = b;
        t->levelSerials[i] = b->cpuSerial;
        if (t->autogen) t->tex->GenerateMipSubLevels();
    }
    t->pal = pal;
    t->palVersion = pal ? pal->version : 0;
    t->keyed = keyed;
    t->key = key;
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
    bool zWrite = true;     // ZWRITEENABLE
    bool alphaBlend = false;  // ALPHABLENDENABLE
    float gameBias = 0.0f;  // the game's depth bias (ZBIAS); biasSet: as D3D9 has it
    float biasSet = 0.0f;
    float slopeSet = 0.0f;  // SLOPESCALEDEPTHBIAS as D3D9 has it (only the overlay slack sets it)
    DWORD maxAnisoSet[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    DWORD anisoLimit = 0;  // anisotropy= within what the GPU can (0: not known yet, or none)
    // alternatepixelcenter: the game's projection, sent moved half a pixel at the next draw
    // of untransformed vertices (it depends on the viewport too).
    D3DMATRIX proj = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
    bool projDirty = g_config.pixelCenter;
    D3DVIEWPORT9 vp = {0, 0, 1, 1, 0.0f, 1.0f};
    std::unordered_map<DWORD, IDirect3DVertexDeclaration9*> decls;
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
        for (auto& e : decls) e.second->Release();
        decls.clear();
        boundRt = boundZ = nullptr;
    }

    IDirect3DVertexDeclaration9* Decl(DWORD fvf);
    bool BindTargets(bool overwriteColor, bool overwriteZ);
    void BindTextures();
    void ClampCutouts(DWORD fvf, const void* verts, UINT stride, UINT count);
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
        *fresh = true;
    }
    return true;
}

void GpuContext::SetRenderState(DWORD state, DWORD value) {
    if (state == D3DRS_ZENABLE) {
        // W buffering: D3D9's own where the GPU has it, else emulated for transformed
        // vertices (Draw); the depth test itself is an ordinary Z buffer's.
        p_->wEmulate = false;
        if (value == D3DZB_USEW && p_->dev) {
            static int logged;
            D3DCAPS9 caps = {};
            const bool native = SUCCEEDED(p_->dev->GetDeviceCaps(&caps)) && (caps.RasterCaps & D3DPRASTERCAPS_WBUFFER);
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
    if (state == D3DRS_ZWRITEENABLE) p_->zWrite = value != 0;
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
    if (r->ms && r->msStale) {
        if (overwriteColor) r->msStale = false;
        else CopyToMs(gpu, dev, r);
        rebind = true;  // CopyToMs drew into it with no Z buffer
    }
    IDirect3DSurface9* target = r->ms ? r->ms : r->surf;
    if (rebind || boundRt != target) {
        dev->SetRenderTarget(0, target);
        boundRt = target;
        dev->SetViewport(&vp);  // D3D9 resets it to the whole target; Direct3D 7 keeps its own
    }
    GpuSurf* zs = z ? EnsureSurf(gpu, dev, z->buf, true) : nullptr;
    IDirect3DSurface9* zsurf = zs ? zs->surf : nullptr;
    if (rebind || boundZ != zsurf) {  // also null: another context may have left its Z bound
        dev->SetDepthStencilSurface(zsurf);
        boundZ = zsurf;
    }
    rebind = false;
    if (zs && zs->pendingClear) {
        zs->pendingClear = false;
        if (!overwriteZ) {
            // Inside the viewport only, as Clear works: the pending fill's rectangle in it.
            D3DRECT rect = {zs->clearRect.left, zs->clearRect.top, zs->clearRect.right, zs->clearRect.bottom};
            const D3DVIEWPORT9 keep = vp;
            D3DVIEWPORT9 whole = {0, 0, DWORD(z->buf->w), DWORD(z->buf->h), 0.0f, 1.0f};
            if (DWORD(rt->buf->w) < whole.Width) whole.Width = rt->buf->w;
            if (DWORD(rt->buf->h) < whole.Height) whole.Height = rt->buf->h;
            dev->SetViewport(&whole);
            dev->Clear(1, &rect, D3DCLEAR_ZBUFFER | (zs->fmt == D3DFMT_D24S8 ? D3DCLEAR_STENCIL : 0), 0, zs->clearZ, 0);
            dev->SetViewport(&keep);
        }
    }
    return true;
}

void GpuContext::Impl::BindTextures() {
    for (DWORD i = 0; i < 8; i++) {
        IDirect3DTexture9* t = tex[i] ? EnsureTex(gpu, dev, tex[i], colorKey) : nullptr;
        dev->SetTexture(i, t);
        cutoutStage[i] = t && (tex[i]->buf->fmt.a || (tex[i]->gpuTex && tex[i]->gpuTex->keyed));
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
void GpuContext::Impl::ClampCutouts(DWORD fvf, const void* verts, UINT stride, UINT count) {
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
        for (int a = 0; a < 2; a++)
            if (want[a] != addrSet[i][a]) {
                dev->SetSamplerState(i, a ? D3DSAMP_ADDRESSV : D3DSAMP_ADDRESSU, want[a]);
                addrSet[i][a] = want[a];
            }
    }
}

void GpuContext::Clear(DWORD n, const RECT* rects, DWORD flags, DWORD color, float z, DWORD stencil) {
    Impl& p = *p_;
    if (!p.dev) return;
    const bool whole = n == 0 && p.CoversTarget();
    if (!p.BindTargets(whole && (flags & D3DCLEAR_TARGET), whole && (flags & D3DCLEAR_ZBUFFER))) return;
    if (!p.boundZ) flags &= ~DWORD(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
    if (!flags) return;
    // A Z buffer that fell back to D16 has no stencil: D3D9 would refuse the whole clear.
    if (p.z && p.z->buf->gpu && p.z->buf->gpu->fmt != D3DFMT_D24S8) flags &= ~DWORD(D3DCLEAR_STENCIL);
    if (!flags) return;
    p.dev->Clear(n, reinterpret_cast<const D3DRECT*>(rects), flags, color, z, stencil);
    if (p.rt->isPrimary) p.rt->MarkDirtyIfVisible();  // drawn into the front surface itself
    if (flags & D3DCLEAR_TARGET) {
        p.rt->buf->cpuStale = true;
        p.rt->buf->gpu->msNewer = p.rt->buf->gpu->ms != nullptr;
        p.rt->buf->gpu->serial = ++g_drawSerial;
        p.rt->buf->gpuStale = false;
    }
}

// Direct3D 7 FVF to a Direct3D 9 vertex declaration: the same offsets, but D3DFVF_RESERVED1
// (D3DLVERTEX's spare DWORD, D3D9's PSIZE) skipped, and five blend weights cut to four.
IDirect3DVertexDeclaration9* GpuContext::Impl::Decl(DWORD fvf) {
    auto it = decls.find(fvf);
    if (it != decls.end()) return it->second;
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
    return decl;
}

void GpuContext::Draw(DWORD primType, DWORD fvf, const void* verts, UINT stride, UINT vertexCount,
                      const WORD* indices, UINT indexCount, UINT primCount, bool zEnabled) {
    Impl& p = *p_;
    if (!p.dev || !primCount || !verts || !vertexCount) return;
    if (!p.BindTargets(false, false)) return;
    p.BindTextures();
    if (MsaaType(p.dev) != D3DMULTISAMPLE_NONE) p.ClampCutouts(fvf, verts, stride, vertexCount);
    IDirect3DVertexDeclaration9* decl = p.Decl(fvf);
    if (!decl) return;
    p.dev->SetVertexDeclaration(decl);
    // Transformed vertices: Direct3D 7 clips their z to [0, 1] only with ZENABLE on, so it
    // can't matter with it off, but D3D9 always clips: clamp it then. rhw 0 on every vertex
    // (2D drawn through Direct3D) is 1: the same picture, without DXVK's infinities.
    // alternatepixelcenter: everything half a pixel up and left, so that pixel centres fall
    // where the drivers that offered it put them (texel centres for 2D drawn 1:1, no lines at
    // texture edges): transformed vertices moved, the projection of the others too.
    // overlaybias: a pass laid on a surface already drawn (it doesn't write depth, or it
    // blends: light maps, decals) is pulled forward by one step of the depth precision the
    // game asked for. Its depth equals the surface's only to within rounding (the polygons
    // are cut differently), which the 16-bit depth buffers of the era rounded the same way,
    // and the overlay won; with today's 24 bits less-or-equal fails on half its pixels, in
    // a fine hatch (NFS III's headlights and light maps on the road). overlaybias=N 16-bit
    // steps (on: one) whatever the Z buffer (the games' depth maths was made for that precision, NFS3+
    // asks for 32 bits), and a slope-scaled one pixel's depth change: the polygons'
    // mismatch grows with the depth gradient (a road at a grazing angle).
    {
        const bool overlay = g_config.overlayBias > 0 && zEnabled && (!p.zWrite || p.alphaBlend);
        const float bias = p.gameBias - (overlay ? float(g_config.overlayBias) / 65535.0f : 0.0f);
        const float slope = overlay ? -1.0f : 0.0f;  // D3DRS_SLOPESCALEDEPTHBIAS: a pixel's depth change
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
    if ((fvf & 0x0E) == 0x04 && stride >= 16) {
        const auto* src = static_cast<const uint8_t*>(verts);
        bool clampZ = false, allRhwZero = true;
        for (UINT i = 0; i < vertexCount; i++) {
            const float* v = reinterpret_cast<const float*>(src + size_t(i) * stride);
            if (!zEnabled && (v[2] < 0.0f || v[2] > 1.0f)) clampZ = true;
            if (v[3] != 0.0f) allRhwZero = false;
        }
        const bool wz = p.wEmulate;
        if (clampZ || allRhwZero || shift || wz) {
            p.scratch.assign(src, src + size_t(vertexCount) * stride);
            for (UINT i = 0; i < vertexCount; i++) {
                float* v = reinterpret_cast<float*>(p.scratch.data() + size_t(i) * stride);
                if (clampZ) v[2] = std::clamp(v[2], 0.0f, 1.0f);
                if (allRhwZero) v[3] = 1.0f;
                // Emulated W buffering: depth from 1/w (rhw), which varies linearly across
                // the screen over any flat surface, as the Z buffer interpolates it: front to
                // back as a W buffer orders, and coplanar polygons agree however they are cut
                // up (the game's own z may not: NFS III's light maps fought with the road).
                // The scale keeps the nearest w seen within [0, 1], 24 bits for the rest.
                if (wz) {
                    const float rhw = v[3] > 0.0f ? v[3] : 0.0f;
                    if (rhw * g_wScale > 1.0f) g_wScale = 0.5f / rhw;
                    v[2] = 1.0f - rhw * g_wScale;
                }
                if (shift) {
                    v[0] -= 0.5f;
                    v[1] -= 0.5f;
                }
            }
            verts = p.scratch.data();
        }
    }
    p.gpu.OpenScene();
    if (indices)
        p.dev->DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE(primType), 0, vertexCount, primCount, indices, D3DFMT_INDEX16,
                                      verts, stride);
    else
        p.dev->DrawPrimitiveUP(D3DPRIMITIVETYPE(primType), primCount, verts, stride);
    (void)indexCount;
    p.rt->buf->cpuStale = true;
    p.rt->buf->gpuStale = false;
    p.rt->buf->gpu->msNewer = p.rt->buf->gpu->ms != nullptr;
    p.rt->buf->gpu->serial = ++g_drawSerial;
    if (p.rt->isPrimary) p.rt->MarkDirtyIfVisible();  // drawn into the front surface itself
}

void GpuContext::EndScene() { p_->gpu.CloseScene(); }

}  // namespace dd2d9
