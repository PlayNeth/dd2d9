// Direct3D's GPU copies of render targets and Z buffers: made, uploaded, read back, copied
// on the GPU (see gpures.h, gpuint.h).
#include "gpuint.h"

#include "quadpass.h"

#include <algorithm>

namespace dd2d9 {

// antialiasing=N: the multisample type every render target and Z buffer gets (they are made
// 32-bit and 24-bit then, the formats every GPU multisamples), the largest the GPU has up
// to N; D3DMULTISAMPLE_NONE without the option or support. Decided once.
D3DMULTISAMPLE_TYPE MsaaType(IDirect3DDevice9* d) {
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
bool ReadableDepth(IDirect3DDevice9* d) {
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
GpuSurf* EnsureSurf(Gpu& gpu, IDirect3DDevice9* d, Buffer* b, bool depth) {
    if (b->gpu && b->gpu->gpu != &gpu) return nullptr;  // another object's device (not expected)
    if (!b->gpu) {
        b->gpu = new GpuSurf(&gpu, b, depth);
        gpu.AddOwner(b->gpu);
    }
    GpuSurf* g = b->gpu;
    g->TakeLoss();
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
            g->fills.assign(1, GpuSurf::Fill{RECT{0, 0, b->w, b->h}, ZToFloat(b->fmt, ReadPixel(b->Row(0), b->fmt.Bytes()))});
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
void CopyToMs(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g) {
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
    QuadPass pass(gpu, d, nullptr);
    pass.Target(g->ms);
    pass.Texture(0, g->msCopy);
    // The dirty rectangle only, texel for pixel.
    if (pass.Draw(rc, rc, b->w, b->h)) g->msStale = false;  // (else tried again)
}

// antialiasing: what Direct3D drew, into the resolved image (before it is read).
void Resolve(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g) {
    if (!g->ms || !g->msNewer || !g->surf) return;
    if (TracingFrame()) DD2D9_LOG("drawtrace: resolve%s", g->msStale ? " (the resolved image had newer parts: lost)" : "");
    gpu.CloseScene();
    d->StretchRect(g->ms, nullptr, g->surf, nullptr, D3DTEXF_NONE);
    g->msNewer = false;
}

// An upload surface of g's, locked over `rc`: one the GPU no longer copies from if there is
// one (or can be one more), else the least recently used, waited for. Null if none can be.
static IDirect3DSurface9* LockUpload(IDirect3DDevice9* d, GpuSurf* g, const RECT& rc, D3DLOCKED_RECT* lr) {
    int n = 0;
    while (n < GpuSurf::kUploads && g->upload[n]) n++;
    for (int k = 0; k < n; k++) {
        const int i = (g->uploadNext + k) % n;
        const HRESULT hr = g->upload[i]->LockRect(lr, &rc, D3DLOCK_DONOTWAIT);
        if (SUCCEEDED(hr)) {
            g->uploadNext = (i + 1) % n;
            return g->upload[i];
        }
        if (hr != D3DERR_WASSTILLDRAWING) {
            // A driver refusing D3DLOCK_DONOTWAIT here (it is valid, but the one flag this path
            // added): the waiting lock, as before, else CPU writes never reach the target.
            static bool logged;  // (the Gpu lock)
            if (!logged) {
                logged = true;
                DD2D9_LOG("upload surface: LockRect with D3DLOCK_DONOTWAIT failed (0x%08lx): waited for instead", hr);
            }
            if (FAILED(g->upload[i]->LockRect(lr, &rc, 0))) return nullptr;
            g->uploadNext = (i + 1) % n;
            return g->upload[i];
        }
    }
    if (n < GpuSurf::kUploads) {
        IDirect3DSurface9*& u = g->upload[n];
        const Buffer* b = g->buf;
        if (SUCCEEDED(d->CreateOffscreenPlainSurface(b->w, b->h, g->fmt, D3DPOOL_DEFAULT, &u, nullptr)) &&
            SUCCEEDED(u->LockRect(lr, &rc, 0))) {
            g->uploadNext = 0;  // (the others, busy a moment ago, are the oldest now)
            return u;
        }
        if (u) u->Release();
        u = nullptr;
        if (!n) return nullptr;
    }
    const int i = g->uploadNext % n;
    if (FAILED(g->upload[i]->LockRect(lr, &rc, 0))) return nullptr;
    g->uploadNext = (i + 1) % n;
    return g->upload[i];
}

// The CPU copy into a render target: the part written since (Buffer::gpuDirty). gpuStale
// stays set until the copy is made (tried again at the next use).
void UploadSurf(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g) {
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
    D3DLOCKED_RECT lr;
    IDirect3DSurface9* const up = LockUpload(d, g, rc, &lr);
    if (!up) return;
    const RowConverter conv(b->fmt, g->pal ? g->pal->pal : nullptr);
    const DWORD bytes = b->fmt.Bytes(), width = DWORD(rc.right - rc.left);
    for (LONG y = rc.top; y < rc.bottom; y++) {
        const uint8_t* from = b->Row(y) + size_t(rc.left) * bytes;
        uint8_t* to = static_cast<uint8_t*>(lr.pBits) + size_t(y - rc.top) * lr.Pitch;
        if (g->convert) conv.Run(from, reinterpret_cast<uint32_t*>(to), int(width));
        else memcpy(to, from, size_t(width) * bytes);
    }
    up->UnlockRect();
    gpu.CloseScene();
    if (FAILED(d->StretchRect(up, &rc, g->surf, &rc, D3DTEXF_NONE))) return;
    b->gpuStale = false;
    if (g->ms) g->MsStale(rc);  // copied on into the multisampled one when drawn into
}

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
    sg->TakeLoss();
    if (!sg->surf || !sb->cpuStale) return false;  // (the device was reset: the CPU copy is all there is)
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
        if (dg->ms) dg->MsStale(c);  // the multisampled image lacks it now
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
void ReadBack(Gpu& gpu, IDirect3DDevice9* d, GpuSurf* g) {
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
        IDirect3DDevice9* d = gpu.Ensure();
        g->TakeLoss();  // (after Ensure, which may reset the device)
        if (d && g->surf) {
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
    if (IDirect3DDevice9* d = g->gpu->CurrentDevice()) Resolve(*g->gpu, d, g);
    g->gpu->CloseScene();
    return g->surf;
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

}  // namespace dd2d9
