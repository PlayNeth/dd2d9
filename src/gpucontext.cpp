// The Direct3D 9 side of a Direct3D 7 device: its states, targets and textures (see gpures.h,
// gpuint.h; its draws: gpudraw.cpp).
#include "gpuint.h"

#include <algorithm>
#include <cmath>

namespace dd2d9 {

// Counts drawing on every render target: a copy made at serial n is of that drawing alone.
unsigned g_drawSerial = 0;
// Emulated W buffering's scale: 1/w times this is at most 1 for every vertex so far (it only
// shrinks, when a nearer one comes). Gpu lock.
float g_wScale = 1.0f;
// drawtrace=on: one whole frame's draws logged every 600 scenes (about 10 s).
unsigned g_traceScene = 0;

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
            static bool logged;
            if (p_->nativeW < 0) {  // asked once (the game sets ZENABLE per draw, some of them)
                D3DCAPS9 caps = {};
                p_->nativeW = SUCCEEDED(p_->dev->GetDeviceCaps(&caps)) && (caps.RasterCaps & D3DPRASTERCAPS_WBUFFER);
            }
            const bool native = p_->nativeW > 0;
            if (!native) {
                p_->wEmulate = true;
                value = D3DZB_TRUE;
            }
            if (!logged) {
                logged = true;
                DD2D9_LOG("Direct3D: the game draws with W buffering (%s)", native ? "the GPU's" : "emulated");
            }
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
    if (state == D3DRS_DEPTHBIAS) {  // (from ZBIAS): the game's, before the overlay tolerance
        memcpy(&p_->gameBias, &value, 4);
        p_->biasSet = -1.0f;  // re-sent at the next draw
        p_->slopeSet = -2.0f;
    }
    if (state == D3DRS_DEPTHBIAS && (value & 0x7FFFFFFF)) {  // (not -0: ZBIAS 0)
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
        p_->projDirty = g_config.pixelCenter || p_->fold[0] != 1.0f || p_->fold[1] != 0.0f || p_->fold[2] != 1.0f ||
                        p_->fold[3] != 0.0f;
        // The depth this projection gives a point at a given w (emulated W buffering of
        // transformed vertices drawn among untransformed ones, Draw): with w = z * _34 + _44
        // it is (z * _33 + _43) / w = far + perRhw / w. A perspective projection only (w grows
        // with z), nearer points getting the smaller depth.
        const float m33 = m16[10], m34 = m16[11], m43 = m16[14], m44 = m16[15];
        const float wFar = m34 != 0.0f ? m33 / m34 : 0.0f, perRhw = m43 - m44 * wFar;
        p_->wProj = m34 > 0.0f && std::isfinite(wFar) && std::isfinite(perRhw) && wFar > 0.0f && perRhw < 0.0f;
        p_->wDepthFar = wFar;
        p_->wDepthPerRhw = perRhw;
    }
    if (p_->dev) p_->dev->SetTransform(D3DTRANSFORMSTATETYPE(type), reinterpret_cast<const D3DMATRIX*>(m16));
}

void GpuContext::SetViewport(DWORD x, DWORD y, DWORD w, DWORD h, float minZ, float maxZ) {
    p_->vp = {x, y, w, h, minZ, maxZ};
    if (g_config.pixelCenter) p_->projDirty = true;
    p_->vpDirty = true;  // sent with the targets (BindTargets), which it is checked against
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

// D3D9 refuses a viewport reaching past the render target, or a Z range outside [0, 1]
// (and keeps the one before). Direct3D 7 takes them: XP's driver drew with the game's
// mapping and the target's edges cut what fell outside. So D3D9 gets the part within the
// target, and the projection (Draw) maps onto it as the game's viewport would: the same
// pixels. The Z range is only clamped.
void GpuContext::Impl::SendViewport() {
    const int64_t w = rt->buf->w, h = rt->buf->h;
    const int64_t x0 = std::min<int64_t>(vp.X, w - 1), y0 = std::min<int64_t>(vp.Y, h - 1);
    const int64_t x1 = std::clamp<int64_t>(int64_t(vp.X) + vp.Width, x0 + 1, w);
    const int64_t y1 = std::clamp<int64_t>(int64_t(vp.Y) + vp.Height, y0 + 1, h);
    auto unit = [](float z) { return !(z > 0.0f) ? 0.0f : z > 1.0f ? 1.0f : z; };
    vpSent = {DWORD(x0), DWORD(y0), DWORD(x1 - x0), DWORD(y1 - y0), unit(vp.MinZ), unit(vp.MaxZ)};
    // (D3D9 gets the target's last column or row then, which Draw and Clear leave alone: the
    // fold keeps untransformed vertices off it, but not transformed ones nor a clear.)
    vpEmpty = vp.X >= w || vp.Y >= h;
    float f[4] = {1.0f, 0.0f, 1.0f, 0.0f};
    if (vpSent.X != vp.X || vpSent.Y != vp.Y || vpSent.Width != vp.Width || vpSent.Height != vp.Height) {
        // Screen x = X + (x / w + 1) W / 2 for both viewports, solved for D3D9's x; y alike,
        // downwards.
        const double gw = std::max<DWORD>(vp.Width, 1), gh = std::max<DWORD>(vp.Height, 1);
        const double sw = vpSent.Width, sh = vpSent.Height;
        const double dx = double(vp.X) - double(vpSent.X), dy = double(vp.Y) - double(vpSent.Y);
        f[0] = float(gw / sw);
        f[1] = float((2.0 * dx + gw - sw) / sw);
        f[2] = float(gh / sh);
        f[3] = float((sh - gh - 2.0 * dy) / sh);
    }
    if (memcmp(f, fold, sizeof(f)) != 0) {
        memcpy(fold, f, sizeof(f));
        projDirty = true;
    }
    dev->SetViewport(&vpSent);
    vpDirty = false;
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
        vpDirty = true;  // D3D9 resets it to the whole target; Direct3D 7 keeps its own
    }
    if (vpDirty) SendViewport();
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
            D3DVIEWPORT9 whole = {0, 0, DWORD(z->buf->w), DWORD(z->buf->h), 0.0f, 1.0f};
            if (DWORD(rt->buf->w) < whole.Width) whole.Width = rt->buf->w;
            if (DWORD(rt->buf->h) < whole.Height) whole.Height = rt->buf->h;
            dev->SetViewport(&whole);
            for (const GpuSurf::Fill& f : zs->fills) {
                D3DRECT rect = {f.r.left, f.r.top, f.r.right, f.r.bottom};
                dev->Clear(1, &rect, D3DCLEAR_ZBUFFER | ((zs->fmt == D3DFMT_D24S8 || zs->fmt == kIntz) ? D3DCLEAR_STENCIL : 0), 0, f.z, 0);
            }
            dev->SetViewport(&vpSent);
        }
        zs->fills.clear();
    }
    return true;
}

void GpuContext::Impl::BindTextures() {
    // Each stage's texture as this function set it last (Gpu::lastTex): set again only when
    // it changes (all 8 every draw before, most of them null).
    auto known = [this] {
        if (gpu.lastTexEpoch == g_deviceStateEpoch) return;
        gpu.lastTexEpoch = g_deviceStateEpoch;
        for (auto& e : gpu.lastTex) e = reinterpret_cast<IDirect3DBaseTexture9*>(~uintptr_t(0));  // (unknown)
    };
    known();
    for (DWORD i = 0; i < 8; i++) {
        IDirect3DBaseTexture9* t = tex[i] ? EnsureTex(gpu, dev, tex[i], colorKey) : nullptr;
        known();  // (EnsureTex may draw on the GPU itself, RtTexture's copy: its SetTexture9 bumps the epoch)
        if (t != gpu.lastTex[i]) {
            dev->SetTexture(i, t);
            gpu.lastTex[i] = t;
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
    if (!p.BindTargets(whole && (flags & D3DCLEAR_TARGET), whole && (flags & D3DCLEAR_ZBUFFER)) || p.vpEmpty) return;
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

void GpuContext::EndScene() {
    if (TracingFrame()) DD2D9_LOG("drawtrace: end of scene %u", g_traceScene);
    g_traceScene++;
    p_->gpu.CloseScene();
}

}  // namespace dd2d9
