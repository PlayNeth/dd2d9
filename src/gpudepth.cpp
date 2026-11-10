// Direct3D's Z buffers that DirectDraw reads and writes (GpuSurf::ztex), and its depth fills
// (see gpures.h, gpuint.h).
#include "gpuint.h"

#include "quadpass.h"

#include <algorithm>

namespace dd2d9 {

// texld r0, t0, s0; mov r1, r0.x; mov oC0, r1
static const DWORD kZReadPs[] = {0xffff0200, 0x0200001f, 0x80000000, 0xb0030000, 0x0200001f, 0x90000000,
                                 0xa00f0800, 0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x02000001,
                                 0x800f0001, 0x80000000, 0x02000001, 0x800f0800, 0x80e40001, 0x0000ffff};
// texld r0, t0, s0; mov oDepth, r0.x; mov r1, r0.x; mov oC0, r1
static const DWORD kZWritePs[] = {0xffff0200, 0x0200001f, 0x80000000, 0xb0030000, 0x0200001f, 0x90000000,
                                  0xa00f0800, 0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x02000001,
                                  0x900f0800, 0x80000000, 0x02000001, 0x800f0001, 0x80000000, 0x02000001,
                                  0x800f0800, 0x80e40001, 0x0000ffff};

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

// The largest lock read back alone (ReadBackDepth's `part`).
constexpr int kZTile = 64;

// The depth the GPU has, into the CPU copy (in its format, its stencil bits kept): the
// pending depth fills applied first. `part`: only that rectangle (at most kZTile square),
// the rest of the CPU copy left as it was (still behind). false: not read. Gpu lock held.
bool ReadBackDepth(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g, const RECT* part) {
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
        QuadPass pass(gpu, d, g->zreadPs);
        if (!g->fills.empty()) {
            pass.Target(g->zrt, g->surf);  // (as large as the Z buffer, which D3D9 needs for the fills)
            for (const GpuSurf::Fill& f : g->fills) {
                const D3DRECT rect = {f.r.left, f.r.top, f.r.right, f.r.bottom};
                d->Clear(1, &rect, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, f.z, 0);
            }
            g->fills.clear();
        }
        pass.Target(rt);  // (the Z buffer sampled now: not bound as well)
        pass.Texture(0, g->ztex);
        if (!pass.Draw(part ? RECT{0, 0, w, h} : all, src, b->w, b->h)) return false;
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
void UploadDepth(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g) {
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
        QuadPass pass(gpu, d, g->zwritePs);
        pass.Target(g->zrt, g->surf);
        pass.Texture(0, g->zup);
        pass.State(D3DRS_ZENABLE, TRUE);  // (written whatever is there: ZFUNC ALWAYS)
        pass.State(D3DRS_ZWRITEENABLE, TRUE);
        pass.State(D3DRS_COLORWRITEENABLE, 0);
        drawn = pass.Draw(rc, rc, b->w, b->h);
    }
    if (drawn) b->gpuStale = false;
}

bool GpuDepthWritable(const Buffer* b) { return b->gpu && b->gpu->depth && b->gpu->ztex; }

bool GpuDepthFill(Surface* s, const RECT& r, DWORD value) {
    GpuSurf* g = s->buf->gpu;
    if (!g || !g->gpu || !g->depth) return false;
    GpuGuard l(*g->gpu);
    // A fill covering an earlier one replaces it: a game filling every frame without drawing
    // in between (a paused half of a split screen) keeps one per viewport, in order.
    auto covered = [&r](const GpuSurf::Fill& f) {
        return r.left <= f.r.left && r.top <= f.r.top && r.right >= f.r.right && r.bottom >= f.r.bottom;
    };
    g->fills.erase(std::remove_if(g->fills.begin(), g->fills.end(), covered), g->fills.end());
    // A safety net only: as many different rectangles, none covering another, never drawn.
    if (g->fills.size() >= 256) {
        static bool logged;  // Gpu lock
        if (!logged) {
            logged = true;
            DD2D9_LOG("Direct3D: 256 depth fills of different rectangles without a draw: the oldest dropped");
        }
        g->fills.erase(g->fills.begin());
    }
    g->fills.push_back({r, ZToFloat(s->buf->fmt, value)});
    return true;
}

}  // namespace dd2d9
