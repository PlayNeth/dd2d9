// The parts of Direct3D's use of the shared device (gpures.h) that its files share: the
// surfaces' GPU copies (GpuSurf), the textures' (GpuTex) and the Direct3D 7 drawing context
// (GpuContext::Impl), and the functions between them. gpuformat.cpp: formats and pixel
// conversion; gpusurf.cpp: render targets; gpudepth.cpp: Z buffers; gputex.cpp: textures;
// gpucontext.cpp and gpudraw.cpp: the drawing context. Private to those files.
#pragma once
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

// ---------------------------------------------------------------- formats (gpuformat.cpp)

D3DFORMAT NativeFormat(const Format& f);
bool IsRenderTargetFormat(D3DFORMAT f);
void RowToGpu(const Buffer* b, int y, uint8_t* dst, const RowConverter* conv);
// A palettized render target: Direct3D 9 draws in X8R8G8B8, and each pixel read back
// becomes the palette's entry for its colour: the exact one when the palette has it (the
// lowest), else the nearest (by RGB distance, cached per 15-bit colour). Every pixel of the
// frame is looked up (under g_lock, by the presenter, for an 8-bit primary Direct3D drew
// into): the exact colours are an open-addressed table, a few times faster than a
// std::unordered_map (640x480: 0.7 ms rather than 5.5).
struct PaletteMap {
    PALETTEENTRY pal[256] = {};
    bool valid = false;
    static constexpr uint32_t kSlots = 1024, kEmpty = 0xFFFFFFFF;  // (no colour is: 24 bits)
    uint32_t slotRgb[kSlots];
    uint8_t slotIndex[kSlots];
    std::vector<int16_t> nearest;  // 32768 entries, -1 until computed
    static uint32_t Slot(uint32_t rgb) { return (rgb * 0x9E3779B1u) >> 22; }
    void Set(const PALETTEENTRY* p) {
        if (valid && !memcmp(pal, p, sizeof(pal))) return;
        memcpy(pal, p, sizeof(pal));
        valid = true;
        std::fill(slotRgb, slotRgb + kSlots, kEmpty);
        for (int i = 255; i >= 0; i--) {  // (from the top: the lowest entry of a colour stays)
            const uint32_t rgb = (uint32_t(p[i].peRed) << 16) | (uint32_t(p[i].peGreen) << 8) | p[i].peBlue;
            uint32_t k = Slot(rgb);
            while (slotRgb[k] != kEmpty && slotRgb[k] != rgb) k = (k + 1) % kSlots;
            slotRgb[k] = rgb;
            slotIndex[k] = uint8_t(i);
        }
        nearest.assign(32768, -1);
    }
    uint8_t Index(uint32_t argb) {
        const uint32_t rgb = argb & 0xFFFFFF;
        for (uint32_t k = Slot(rgb); slotRgb[k] != kEmpty; k = (k + 1) % kSlots)
            if (slotRgb[k] == rgb) return slotIndex[k];
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

void RowFromGpu(Buffer* b, int y, const uint8_t* src, bool convert, PaletteMap* pal, const ToPixel* to);
void ZBits(const Format& f, DWORD* mask, DWORD* shift, DWORD* maxv);
float ZToFloat(const Format& f, DWORD v);
D3DFORMAT DepthFormat(const Format& f);
void WriteLevel(const Buffer* b, const D3DLOCKED_RECT& lr, const PALETTEENTRY* entries, bool convert, bool keyed,
                DWORD key, bool palAlpha = false);

// ---------------------------------------------------------------- render targets and Z

// Defined in gpucontext.cpp.
extern unsigned g_drawSerial;
extern float g_wScale;
extern unsigned g_traceScene;
inline bool TracingFrame() { return g_config.drawTrace && g_traceScene % 600 == 300; }

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
    // Upload staging (D3DPOOL_DEFAULT, lockable), up to kUploads used in turn: locked while
    // the GPU still copies from it, a surface waits for that copy, and a game writing into
    // its back buffer between draws (HUD text through Lock or GetDC, every frame) would drain
    // the GPU at each write. Another is made only when every one there is still busy.
    static constexpr int kUploads = 3;
    IDirect3DSurface9* upload[kUploads] = {};
    int uploadNext = 0;  // the one to try first (the least recently used)
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
    bool lost = false;    // the device was reset since: see ReleaseGpuResources

    GpuSurf(Gpu* g, Buffer* b, bool d) : gpu(g), buf(b), depth(d) {}
    ~GpuSurf() {
        delete pal;
        delete toPixel;
    }
    // `surf` got what `ms` lacks over `r` (the CPU's changes, a GPU blit, a 2D draw): copied
    // into `ms` before Direct3D next draws there (CopyToMs).
    void MsStale(const RECT& r) {
        if (!msStale) msDirty = r;
        else UnionRect(&msDirty, &msDirty, &r);
        msStale = true;
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
        for (IDirect3DSurface9*& u : upload) {
            if (u) u->Release();
            u = nullptr;
        }
        uploadNext = 0;
        if (ms) ms->Release();
        if (msCopy) msCopy->Release();
        surf = sysmem = ms = nullptr;
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
    // Device reset: D3DPOOL_DEFAULT goes, and what only the GPU had with it. A reset may come
    // from the presenter thread, which holds the Gpu lock alone, while the Buffer's stale
    // flags are g_lock's (a game thread may be marking a write at that moment): only `lost`
    // is set here, and TakeLoss moves it into the Buffer where both locks are held.
    void ReleaseGpuResources() override {
        Drop();
        lost = true;
    }
    // The CPU copy is all there is now: nothing newer on the GPU, all of it to upload again.
    // g_lock and the Gpu lock held.
    void TakeLoss() {
        if (!lost) return;
        lost = false;
        buf->cpuStale = false;
        buf->gpuStale = true;
        buf->gpuDirty = Buffer::kAllRect;
    }
    // Only the DirectDraw object's destruction releases a device that still has surfaces
    // (Gpu::Release otherwise runs once it has no owners): no game thread uses its buffers.
    void GpuReleased() override {
        ReleaseGpuResources();
        TakeLoss();
        gpu = nullptr;  // an orphan now: GpuBufferGone only frees it
    }
};

// The depth texture format of the Z buffers DirectDraw reads and writes (ReadableDepth).
constexpr D3DFORMAT kIntz = D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z'));

// gpusurf.cpp
D3DMULTISAMPLE_TYPE MsaaType(IDirect3DDevice9* d);
bool ReadableDepth(IDirect3DDevice9* d);
GpuSurf* EnsureSurf(Gpu& gpu, IDirect3DDevice9* d, Buffer* b, bool depth);
void CopyToMs(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g);
void Resolve(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g);
void UploadSurf(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g);
void ReadBack(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g);
// gpudepth.cpp
bool ReadBackDepth(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g, const RECT* part = nullptr);
void UploadDepth(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g);

// ---------------------------------------------------------------- textures (gputex.cpp)

// A texture's managed GPU copy and what it was made from.
struct GpuTexCopy {
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
    int w = 0, h = 0;      // the top level's size and layout when made (SetSurfaceDesc may change them)
    Format made = {};
    std::vector<Buffer*> levelBufs;
    std::vector<unsigned> levelSerials;  // each level's Buffer::cpuSerial when uploaded
    // The format for `f`: its own where Direct3D 9 has it, else (or colour-keyed: the key
    // made alpha) converted to A8R8G8B8.
    void PickFormat(const Format& f, bool withKey) {
        const D3DFORMAT native = NativeFormat(f);
        convert = native == D3DFMT_UNKNOWN || withKey;
        fmt = convert ? D3DFMT_A8R8G8B8 : native;
    }
    // A layout the GPU lacks after all: converted, if it can be (not block-compressed or bump).
    bool FallBack(const Format& f) {
        if (convert || f.fourcc || f.bump) return false;
        convert = true;
        fmt = D3DFMT_A8R8G8B8;
        return true;
    }
};

struct GpuTex final : Gpu::Owner, GpuTexCopy {
    Gpu* gpu;
    Surface* top;
    IDirect3DCubeTexture9* cube = nullptr;  // a cube map: its six faces (levelBufs: face by face)
    bool shownKeyed = false;  // the texture last handed out (managed or rtTex) has the key as alpha
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

    // The other managed copy of a texture drawn with the colour key on and off by turns: the
    // key made alpha in one and not in the other, both kept and switched (SwapAlt) instead of
    // made again at every change. N.I.C.E. 2 draws two of its race textures so, several
    // times a frame: a new texture and a whole upload each time cost it 4-8 ms a frame.
    GpuTexCopy alt;
    void SwapAlt() { std::swap(static_cast<GpuTexCopy&>(*this), alt); }
    void DropAlt() {
        if (alt.tex) alt.tex->Release();
        alt = GpuTexCopy();
    }

    GpuTex(Gpu* g, Surface* s) : gpu(g), top(s) {}
    void DropAll() {
        Drop();
        DropAlt();
    }
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
    void ReleaseGpuResources() override { DropAll(); }
    void GpuReleased() override {
        DropAll();
        gpu = nullptr;
    }
};

IDirect3DBaseTexture9* EnsureTex(Gpu& gpu, IDirect3DDevice9* d, Surface* s, bool colorKey);

// ---------------------------------------------------------------- drawing context (gpucontext.cpp, gpudraw.cpp)

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
    // Emulated W buffering where the game also draws untransformed vertices (wMixed, set by
    // the first such draw): a transformed vertex's depth is wDepthFar + wDepthPerRhw * rhw,
    // what the projection matrix gives an untransformed one at the same w (wProj: it is a
    // perspective projection; see SetTransform).
    bool wProj = false, wMixed = false;
    float wDepthFar = 1.0f, wDepthPerRhw = 0.0f;
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
    // The game's projection, sent changed at the next draw of untransformed vertices where
    // it has to be: alternatepixelcenter moves it half a pixel, and a viewport reaching past
    // the render target needs `fold` (both depend on the viewport).
    D3DMATRIX proj = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
    bool projDirty = g_config.pixelCenter;
    // The viewport as the game set it, kept as it is (Direct3D 7 keeps one larger than a
    // newer, smaller target, and doesn't check its Z range), and as D3D9 has it: within the
    // target and Z within [0, 1], which D3D9 demands (SendViewport).
    D3DVIEWPORT9 vp = {0, 0, 1, 1, 0.0f, 1.0f};
    D3DVIEWPORT9 vpSent = {0, 0, 1, 1, 0.0f, 1.0f};
    bool vpDirty = true;
    bool vpEmpty = false;  // the game's viewport wholly past the target: nothing drawn or cleared
    // What keeps the game's mapping with the viewport D3D9 has: clip x' = fold[0] x +
    // fold[1] w, clip y' = fold[2] y + fold[3] w. Identity unless the viewport was cut.
    float fold[4] = {1.0f, 0.0f, 1.0f, 0.0f};
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
    void SendViewport();  // vp as D3D9 takes it, for the bound target
    void BindTextures();
    // The address modes ClampCutouts sets for these vertices (each stage's U and V).
    void CutoutAddr(DWORD fvf, const void* verts, UINT stride, UINT count, DWORD (&want)[8][2]) const;
    void ClampCutouts(DWORD fvf, const void* verts, UINT stride, UINT count);
    void SetAddr(const DWORD (&want)[8][2]);  // the sampler address modes, where they differ
    bool CoversTarget() const;
};

}  // namespace dd2d9
