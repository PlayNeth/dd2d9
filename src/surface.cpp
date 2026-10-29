// Surfaces, surface memory, palettes, clippers and gamma control.
#include "wrappers.h"

#include "d3dproxy.h"

#include <new>

namespace dd2d9 {

// ================================================================ Buffer

Buffer* Buffer::Create(int w, int h, const Format& f) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return nullptr;
    auto* b = new (std::nothrow) Buffer();
    if (!b) return nullptr;
    b->w = w;
    b->h = h;
    b->fmt = f;

    bool dibOk = !f.zbuf && !f.fourcc && (f.bpp == 8 || f.bpp == 16 || f.bpp == 24 || f.bpp == 32);
    if (dibOk) {
        struct {
            BITMAPINFOHEADER h;
            DWORD colors[256];
        } bi = {};
        bi.h.biSize = sizeof(BITMAPINFOHEADER);
        bi.h.biWidth = w;
        bi.h.biHeight = -h;  // top-down
        bi.h.biPlanes = 1;
        bi.h.biBitCount = WORD(f.bpp);
        if (f.pal8) {
            bi.h.biCompression = BI_RGB;
            bi.h.biClrUsed = 256;
            for (int i = 0; i < 256; i++) bi.colors[i] = DWORD(i) * 0x010101u;
        } else if (f.bpp == 24) {
            bi.h.biCompression = BI_RGB;
        } else {
            bi.h.biCompression = BI_BITFIELDS;
            bi.colors[0] = f.r;
            bi.colors[1] = f.g;
            bi.colors[2] = f.b;
        }
        void* bits = nullptr;
        b->dib = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
        if (b->dib) {
            b->bits = static_cast<uint8_t*>(bits);
            b->pitch = ((w * LONG(f.bpp) + 31) / 32) * 4;
            memset(b->bits, 0, size_t(b->pitch) * h);
            return b;
        }
        DD2D9_LOG("CreateDIBSection failed for %dx%d %lubpp, falling back to plain memory", w, h, f.bpp);
    }

    if (f.BlockCompressed()) {
        // DXT1 is 4 bits/pixel, the others 8.
        int bw = (w + 3) & ~3, bh = (h + 3) & ~3;
        size_t size = f.fourCC == MAKEFOURCC('D', 'X', 'T', '1') ? size_t(bw) * bh / 2 : size_t(bw) * bh;
        b->pitch = LONG(size / bh);
        b->bits = static_cast<uint8_t*>(calloc(1, size));
    } else {
        b->pitch = ((w * LONG(f.Bytes())) + 3) & ~3;
        b->bits = static_cast<uint8_t*>(calloc(1, size_t(b->pitch) * h));
    }
    if (!b->bits) {
        delete b;
        return nullptr;
    }
    b->ownsMemory = true;
    return b;
}

Buffer* Buffer::CreateExternal(void* mem, int w, int h, LONG pitch, const Format& f) {
    auto* b = new (std::nothrow) Buffer();
    if (!b) return nullptr;
    b->w = w;
    b->h = h;
    b->fmt = f;
    b->pitch = pitch;
    b->bits = static_cast<uint8_t*>(mem);
    return b;
}

void Buffer::Release() {
    if (InterlockedDecrement(&ref) != 0) return;
    if (dc) DeleteDC(dc);
    if (dib) DeleteObject(dib);
    if (ownsMemory) free(bits);
    delete this;
}

HDC Buffer::GetDC() {
    if (!dib) return nullptr;
    if (!dc) {
        dc = CreateCompatibleDC(nullptr);
        if (dc) SelectObject(dc, dib);
    }
    return dc;
}

// ================================================================ Surface

Surface::Surface(DDraw* d, const DDSURFACEDESC2& sd, Buffer* b, int ver) : dd(d), desc(sd), buf(b), version(ver) {
    wraps_[0] = new SurfaceWrap<1>(this);
    wraps_[1] = new SurfaceWrap<2>(this);
    wraps_[2] = new SurfaceWrap<3>(this);
    wraps_[3] = new SurfaceWrap<4>(this);
    wraps_[4] = new SurfaceWrap<7>(this);
    static const DWORD kCkFlags[4] = {DDSD_CKDESTBLT, DDSD_CKDESTOVERLAY, DDSD_CKSRCBLT, DDSD_CKSRCOVERLAY};
    const DDCOLORKEY* keys[4] = {&sd.ddckCKDestBlt, &sd.ddckCKDestOverlay, &sd.ddckCKSrcBlt, &sd.ddckCKSrcOverlay};
    for (int i = 0; i < 4; i++) {
        if (sd.dwFlags & kCkFlags[i]) {
            hasCk[i] = true;
            ck[i] = *keys[i];
        }
    }
    desc.dwFlags &= ~(DDSD_CKDESTBLT | DDSD_CKDESTOVERLAY | DDSD_CKSRCBLT | DDSD_CKSRCOVERLAY | DDSD_LPSURFACE);
    dd->IntAddRef();
    dd->surfaces.push_back(this);
}

Surface::~Surface() {
    // Called with g_lock held.
    Surface* family = root ? root : this;
    for (Surface* a : attached) {
        // Members of the same complex surface (mip levels) are owned by the root.
        if (a != family && a->root != family) a->Release();
    }
    for (Surface* c : children) delete c;
    if (palette) palette->Release();
    if (clipper) clipper->Release();
    for (auto& p : priv_)
        if (p.unk) p.unk->Release();
    D3DProxySurfaceDestroyed(this);
    buf->Release();
    delete static_cast<SurfaceWrap<1>*>(wraps_[0]);
    delete static_cast<SurfaceWrap<2>*>(wraps_[1]);
    delete static_cast<SurfaceWrap<3>*>(wraps_[2]);
    delete static_cast<SurfaceWrap<4>*>(wraps_[3]);
    delete static_cast<SurfaceWrap<7>*>(wraps_[4]);
    delete gamma;
    if (dd->primary == this) {
        dd->primary = nullptr;
        dd->MarkDirty();
    }
    for (size_t i = 0; i < dd->surfaces.size(); i++) {
        if (dd->surfaces[i] == this) {
            dd->surfaces.erase(dd->surfaces.begin() + i);
            break;
        }
    }
    dd->IntRelease();
}

void* Surface::Iface(int v) {
    switch (v) {
    case 1: return wraps_[0];
    case 2: return wraps_[1];
    case 3: return wraps_[2];
    case 4: return wraps_[3];
    default: return wraps_[4];
    }
}

HRESULT Surface::QueryInterface(REFIID r, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    int v = 0;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirectDrawSurface)) v = 1;
    else if (IsEqualGUID(r, IID_IDirectDrawSurface2)) v = 2;
    else if (IsEqualGUID(r, IID_IDirectDrawSurface3)) v = 3;
    else if (IsEqualGUID(r, IID_IDirectDrawSurface4)) v = 4;
    else if (IsEqualGUID(r, IID_IDirectDrawSurface7)) v = 7;
    if (v) {
        AddRef();
        *out = Iface(v);
        return S_OK;
    }
    if (IsEqualGUID(r, IID_IDirectDrawGammaControl)) {
        Guard l;
        if (!gamma) gamma = new GammaControl(this);
        AddRef();
        *out = gamma;
        return S_OK;
    }
    HRESULT hr = D3DProxyQuerySurface(this, r, out);  // textures and Direct3D 1 devices for a wrapper
    if (hr != E_NOINTERFACE) return hr;
    DD2D9_LOG("Surface::QueryInterface(%s): not supported", GuidName(r));
    return E_NOINTERFACE;
}

ULONG Surface::AddRef() {
    if (root) return root->AddRef();
    return ULONG(InterlockedIncrement(&ref_));
}

ULONG Surface::Release() {
    if (root) return root->Release();
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) {
        Guard l;
        delete this;
    }
    return ULONG(r < 0 ? 0 : r);
}

HRESULT Surface::AddAttachedSurface(Surface* s) {
    if (!s || s == this) return DDERR_INVALIDPARAMS;
    // Parts of one complex surface are already attached to each other; a
    // reference to our own root would also keep the chain alive forever.
    if ((s->root ? s->root : s) == (root ? root : this)) return DDERR_CANNOTATTACHSURFACE;
    for (Surface* a : attached)
        if (a == s) return DDERR_SURFACEALREADYATTACHED;
    s->AddRef();
    attached.push_back(s);
    return DD_OK;
}

HRESULT Surface::DeleteAttachedSurface(DWORD, Surface* s) {
    Surface* family = root ? root : this;
    auto ownMember = [family](const Surface* a) { return (a->root ? a->root : a) == family; };
    if (!s) {  // detach everything that was explicitly attached
        std::vector<Surface*> keep, drop;
        for (Surface* a : attached) (ownMember(a) ? keep : drop).push_back(a);
        attached.swap(keep);
        for (Surface* a : drop) a->Release();
        return DD_OK;
    }
    for (size_t i = 0; i < attached.size(); i++) {
        Surface* a = attached[i];
        if (a != s) continue;
        // Levels of our own complex surface (mipmaps) can't be detached.
        if (ownMember(a)) return DDERR_CANNOTDETACHSURFACE;
        attached.erase(attached.begin() + i);
        a->Release();
        return DD_OK;
    }
    return DDERR_SURFACENOTATTACHED;
}

HRESULT Surface::EnumAttachedSurfaces(int v, void* ctx, void* callback) {
    if (!callback) return DDERR_INVALIDPARAMS;
    using CB = HRESULT(WINAPI*)(void*, void*, void*);
    std::vector<Surface*> list;
    {
        Guard l;
        if (chainNext && chainNext != this) list.push_back(chainNext);
        for (Surface* a : attached) list.push_back(a);
        for (Surface* s : list) s->AddRef();  // the callback releases it
    }
    size_t i = 0;
    for (; i < list.size(); i++) {
        DDSURFACEDESC2 d2;
        list[i]->GetSurfaceDesc(&d2);
        union {
            DDSURFACEDESC v1;
            DDSURFACEDESC2 v2;
        } out;
        out.v1.dwSize = v >= 4 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
        DescOut(d2, &out);
        if (reinterpret_cast<CB>(callback)(list[i]->Iface(v), &out, ctx) == DDENUMRET_CANCEL) {
            i++;
            break;
        }
    }
    for (; i < list.size(); i++) list[i]->Release();  // never handed to the callback
    return DD_OK;
}

HRESULT Surface::GetAttachedSurface(const DDSCAPS2& caps, Surface** out) {
    auto matches = [&](Surface* s) {
        return (s->desc.ddsCaps.dwCaps & caps.dwCaps) == caps.dwCaps &&
               (s->desc.ddsCaps.dwCaps2 & caps.dwCaps2) == caps.dwCaps2;
    };
    Surface* found = nullptr;
    if (chainNext && chainNext != this && matches(chainNext)) found = chainNext;
    for (Surface* a : attached) {
        if (!found && matches(a)) found = a;
    }
    if (!found) return DDERR_NOTFOUND;
    found->AddRef();
    *out = found;
    return DD_OK;
}

HRESULT Surface::GetCaps(DDSCAPS2* caps) {
    *caps = desc.ddsCaps;
    return DD_OK;
}

HRESULT Surface::GetClipper(IDirectDrawClipper** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (!clipper) return DDERR_NOCLIPPERATTACHED;
    clipper->AddRef();
    *out = clipper;
    return DD_OK;
}

HRESULT Surface::SetClipper(IDirectDrawClipper* c) {
    Clipper* nc = c ? Clipper::From(c) : nullptr;
    if (c && !nc) return DDERR_INVALIDPARAMS;
    if (nc) nc->AddRef();
    if (clipper) clipper->Release();
    clipper = nc;
    MarkDirtyIfVisible();
    return DD_OK;
}

static int CkIndex(DWORD flags) {
    switch (flags & ~DDCKEY_COLORSPACE) {
    case DDCKEY_DESTBLT: return kCkDestBlt;
    case DDCKEY_DESTOVERLAY: return kCkDestOverlay;
    case DDCKEY_SRCBLT: return kCkSrcBlt;
    case DDCKEY_SRCOVERLAY: return kCkSrcOverlay;
    default: return -1;
    }
}

HRESULT Surface::GetColorKey(DWORD flags, DDCOLORKEY* key) {
    int i = CkIndex(flags);
    if (i < 0 || !key) return DDERR_INVALIDPARAMS;
    if (!hasCk[i]) return DDERR_NOCOLORKEY;
    *key = ck[i];
    return DD_OK;
}

HRESULT Surface::SetColorKey(DWORD flags, DDCOLORKEY* key) {
    int i = CkIndex(flags);
    if (i < 0) return DDERR_INVALIDPARAMS;
    if (!key) {
        hasCk[i] = false;
        return DD_OK;
    }
    ck[i] = *key;
    if (!(flags & DDCKEY_COLORSPACE)) ck[i].dwColorSpaceHighValue = ck[i].dwColorSpaceLowValue;
    hasCk[i] = true;
    return DD_OK;
}

const PALETTEENTRY* Surface::EffectivePalette() const {
    if (palette) return palette->entries;
    if (dd->primary && dd->primary->palette) return dd->primary->palette->entries;
    return dd->defaultPalette;
}

HRESULT Surface::GetDC(HDC* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    if (dcOut) return DDERR_DCALREADYCREATED;
    HDC dc = buf->GetDC();
    if (!dc) return DDERR_CANTCREATEDC;
    if (buf->fmt.pal8) {
        const Palette* p = palette ? palette : (dd->primary ? dd->primary->palette : nullptr);
        if (p != buf->dcPal || (p && p->version != buf->dcPalVersion) || !p) {
            const PALETTEENTRY* e = EffectivePalette();
            RGBQUAD q[256];
            for (int i = 0; i < 256; i++) q[i] = {e[i].peBlue, e[i].peGreen, e[i].peRed, 0};
            SetDIBColorTable(dc, 0, 256, q);
            buf->dcPal = p;
            buf->dcPalVersion = p ? p->version : 0;
        }
    }
    dcOut = dc;
    *out = dc;
    DD2D9_TRACE("GetDC %p%s", static_cast<void*>(this), isPrimary ? " (primary)" : "");
    return DD_OK;
}

HRESULT Surface::ReleaseDC(HDC dc) {
    Guard l;
    if (!dcOut || dc != dcOut) return DDERR_INVALIDPARAMS;
    GdiFlush();
    dcOut = nullptr;
    DD2D9_TRACE("ReleaseDC %p%s", static_cast<void*>(this), isPrimary ? " (primary)" : "");
    uniqueness++;
    MarkDirtyIfVisible();
    return DD_OK;
}

HRESULT Surface::GetPalette(IDirectDrawPalette** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (!palette) return DDERR_NOPALETTEATTACHED;
    palette->AddRef();
    *out = palette;
    return DD_OK;
}

HRESULT Surface::SetPalette(IDirectDrawPalette* p) {
    Palette* np = p ? Palette::From(p) : nullptr;
    if (p && !np) return DDERR_INVALIDPARAMS;
    if (np && !buf->fmt.pal8) return DDERR_INVALIDPIXELFORMAT;
    DD2D9_TRACE("SetPalette %p%s <- %p", static_cast<void*>(this), isPrimary ? " (primary)" : "", static_cast<void*>(np));
    if (np) np->AddRef();
    if (palette) palette->Release();
    palette = np;
    // Palettes set on a flip chain apply to the whole chain.
    if (root == nullptr) {
        for (Surface* s : flipChain) {
            if (s == this) continue;
            if (np) np->AddRef();
            if (s->palette) s->palette->Release();
            s->palette = np;
        }
    }
    MarkDirtyIfVisible();
    return DD_OK;
}

HRESULT Surface::GetPixelFormat(DDPIXELFORMAT* pf) {
    if (!pf) return DDERR_INVALIDPARAMS;
    buf->fmt.ToDDPF(pf);
    return DD_OK;
}

HRESULT Surface::GetSurfaceDesc(DDSURFACEDESC2* d) {
    *d = desc;
    d->dwSize = sizeof(DDSURFACEDESC2);
    d->dwWidth = DWORD(buf->w);
    d->dwHeight = DWORD(buf->h);
    d->dwFlags |= DDSD_WIDTH | DDSD_HEIGHT | DDSD_CAPS | DDSD_PIXELFORMAT;
    buf->fmt.ToDDPF(&d->ddpfPixelFormat);
    if (buf->fmt.BlockCompressed()) {
        d->dwFlags = (d->dwFlags & ~DDSD_PITCH) | DDSD_LINEARSIZE;
        d->dwLinearSize = DWORD(buf->pitch) * ((buf->h + 3) & ~3);
    } else {
        d->dwFlags |= DDSD_PITCH;
        d->lPitch = buf->pitch;
    }
    d->lpSurface = nullptr;
    static const DWORD kCkFlags[4] = {DDSD_CKDESTBLT, DDSD_CKDESTOVERLAY, DDSD_CKSRCBLT, DDSD_CKSRCOVERLAY};
    DDCOLORKEY* keys[4] = {&d->ddckCKDestBlt, &d->ddckCKDestOverlay, &d->ddckCKSrcBlt, &d->ddckCKSrcOverlay};
    for (int i = 0; i < 4; i++) {
        if (hasCk[i]) {
            d->dwFlags |= kCkFlags[i];
            *keys[i] = ck[i];
        }
    }
    return DD_OK;
}

HRESULT Surface::SetSurfaceDesc(const DDSURFACEDESC2& d, DWORD) {
    // Only client-memory surfaces are supported (DDSD_LPSURFACE, optionally
    // with a new size, pitch and pixel format), as in real DirectDraw.
    if (!(d.dwFlags & DDSD_LPSURFACE) || !d.lpSurface) return DDERR_INVALIDPARAMS;
    if (d.dwFlags & ~(DDSD_LPSURFACE | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_CAPS))
        return DDERR_INVALIDPARAMS;
    if (isPrimary || lockCount || dcOut) return DDERR_INVALIDSURFACETYPE;
    int w = (d.dwFlags & DDSD_WIDTH) ? int(d.dwWidth) : buf->w;
    int h = (d.dwFlags & DDSD_HEIGHT) ? int(d.dwHeight) : buf->h;
    Format f = buf->fmt;
    if ((d.dwFlags & DDSD_PIXELFORMAT) && !Format::FromDDPF(d.ddpfPixelFormat, &f)) return DDERR_INVALIDPIXELFORMAT;
    LONG pitch = (d.dwFlags & DDSD_PITCH) ? d.lPitch : LONG(((w * f.Bytes()) + 3) & ~3u);
    if (w <= 0 || h <= 0 || pitch < LONG(w * f.Bytes())) return DDERR_INVALIDPARAMS;
    Buffer* nb = Buffer::CreateExternal(d.lpSurface, w, h, pitch, f);
    if (!nb) return DDERR_OUTOFMEMORY;
    ReplaceBuffer(nb);
    desc.dwWidth = DWORD(w);
    desc.dwHeight = DWORD(h);
    uniqueness++;
    return DD_OK;
}

void Surface::ReplaceBuffer(Buffer* b) {
    buf->Release();
    buf = b;
}

HRESULT Surface::Lock(RECT* r, DDSURFACEDESC2* out, DWORD flags) {
    Guard l;
    GdiFlush();
    GetSurfaceDesc(out);
    uint8_t* p = buf->bits;
    if (r && !buf->fmt.BlockCompressed()) {
        if (r->left < 0 || r->top < 0 || r->right > buf->w || r->bottom > buf->h || r->left >= r->right ||
            r->top >= r->bottom)
            return DDERR_INVALIDPARAMS;
        p += r->top * buf->pitch + r->left * LONG(buf->fmt.Bytes());
    }
    out->dwFlags |= DDSD_LPSURFACE;
    out->lpSurface = p;
    // While locked, the presenter treats this area as changing (see Presenter::Loop).
    const RECT area = r && !buf->fmt.BlockCompressed() ? *r : RECT{0, 0, buf->w, buf->h};
    if (lockCount == 0) {
        lockArea = area;
    } else {
        lockArea = {std::min(lockArea.left, area.left), std::min(lockArea.top, area.top),
                    std::max(lockArea.right, area.right), std::max(lockArea.bottom, area.bottom)};
    }
    lockCount++;
    DD2D9_TRACE("Lock %p%s rect=%s%ld,%ld-%ld,%ld flags=0x%lx -> %p pitch=%ld", static_cast<void*>(this),
               isPrimary ? " (primary)" : "", r ? "" : "full ", r ? r->left : 0L, r ? r->top : 0L,
               r ? r->right : LONG(buf->w), r ? r->bottom : LONG(buf->h), flags, static_cast<void*>(p), buf->pitch);
    return DD_OK;
}

HRESULT Surface::Unlock() {
    Guard l;
    DD2D9_TRACE("Unlock %p%s", static_cast<void*>(this), isPrimary ? " (primary)" : "");
    if (lockCount <= 0) return DDERR_NOTLOCKED;
    lockCount--;
    uniqueness++;
    MarkDirtyIfVisible(&lockArea);
    return DD_OK;
}

HRESULT Surface::GetDDInterface(void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (!dd->alive) return DDERR_INVALIDOBJECT;  // the DirectDraw object was released
    *out = dd->Iface(version);
    dd->AddRef();
    return DD_OK;
}

HRESULT Surface::SetPrivateData(REFGUID tag, void* data, DWORD size, DWORD flags) {
    if (!data && size) return DDERR_INVALIDPARAMS;
    FreePrivateData(tag);
    PrivateData p = {tag, {}, nullptr};
    if (flags & DDSPD_IUNKNOWNPOINTER) {
        if (size != sizeof(IUnknown*)) return DDERR_INVALIDPARAMS;
        p.unk = static_cast<IUnknown*>(data);
        p.unk->AddRef();
    } else {
        p.data.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    }
    priv_.push_back(std::move(p));
    return DD_OK;
}

HRESULT Surface::GetPrivateData(REFGUID tag, void* data, DWORD* size) {
    if (!size) return DDERR_INVALIDPARAMS;
    for (auto& p : priv_) {
        if (!IsEqualGUID(p.tag, tag)) continue;
        DWORD need = p.unk ? sizeof(IUnknown*) : DWORD(p.data.size());
        if (!data || *size < need) {
            *size = need;
            return DDERR_MOREDATA;
        }
        if (p.unk) memcpy(data, &p.unk, sizeof(IUnknown*));
        else if (need) memcpy(data, p.data.data(), need);
        *size = need;
        return DD_OK;
    }
    return DDERR_NOTFOUND;
}

HRESULT Surface::FreePrivateData(REFGUID tag) {
    for (size_t i = 0; i < priv_.size(); i++) {
        if (IsEqualGUID(priv_[i].tag, tag)) {
            if (priv_[i].unk) priv_[i].unk->Release();
            priv_.erase(priv_.begin() + i);
            return DD_OK;
        }
    }
    return DDERR_NOTFOUND;
}

void Surface::FillRect(const RECT& r, DWORD value) {
    const RECT bounds = {0, 0, buf->w, buf->h};
    RECT c;
    if (buf->fmt.BlockCompressed() || !IntersectRect(&c, &r, &bounds)) return;
    FillCore(&c, value);
    uniqueness++;
    MarkDirtyIfVisible(&c);
}

Surface* Surface::NextMipLevel() const {
    const Surface* family = root ? root : this;
    for (Surface* a : attached)
        if ((a->root ? a->root : a) == family && (a->desc.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL)) return a;
    return nullptr;
}

void Surface::MarkDirtyIfVisible(const RECT* area) {
    if (!isPrimary) return;
    if (area) dd->MarkDirtyRect(*area);
    else dd->MarkDirty();
}

HRESULT Surface::Flip(Surface* target, DWORD flags) {
    uint64_t seq = 0;
    bool wait = false;
    {
        Guard l;
        Surface* r = root ? root : this;
        if (!(desc.ddsCaps.dwCaps & DDSCAPS_FLIP) || r->flipChain.size() < 2) return DDERR_NOTFLIPPABLE;
        if (r != this) return DDERR_NOTFLIPPABLE;  // must be called on the front buffer
        if (isPrimary && !dd->Exclusive()) return DDERR_NOEXCLUSIVEMODE;
        DD2D9_TRACE("Flip %p target=%p flags=0x%lx", static_cast<void*>(this), static_cast<void*>(target), flags);
        auto& chain = r->flipChain;
        for (Surface* s : chain)
            if (s->lockCount || s->dcOut) return DDERR_SURFACEBUSY;
        if (target) {
            size_t k = 0;
            while (k < chain.size() && chain[k] != target) k++;
            if (k == 0 || k == chain.size()) return DDERR_INVALIDPARAMS;
            std::swap(chain[0]->buf, chain[k]->buf);
        } else {
            Buffer* first = chain[0]->buf;
            for (size_t i = 0; i + 1 < chain.size(); i++) chain[i]->buf = chain[i + 1]->buf;
            chain.back()->buf = first;
        }
        for (Surface* s : chain) s->uniqueness++;
        if (isPrimary) {
            Presenter* p = dd->GetPresenter();
            wait = g_config.vsync == kVSyncOn || (g_config.vsync == kVSyncAuto && !(flags & DDFLIP_NOVSYNC));
            seq = p ? p->ConvertSeq() : 0;
            dd->MarkDirty();
        }
    }
    if (wait) {
        int intervals = int((flags >> 24) & 0xF);
        Presenter* p = dd->GetPresenter();
        if (p && p->Active()) p->WaitPresented(seq, 100);
        dd->PaceFlip();
        for (int i = 1; i < intervals; i++) dd->PaceFlip();
    }
    return DD_OK;
}

// ================================================================ Palette

Palette::Palette(DDraw* dd, DWORD c, const PALETTEENTRY* e) : caps(c), dd_(dd) {
    if (dd_) dd_->IntAddRef();
    int n = Count();
    if (e) {
        if (caps & DDPCAPS_8BITENTRIES) {
            const uint8_t* idx = reinterpret_cast<const uint8_t*>(e);
            for (int i = 0; i < n; i++) entries[i] = {idx[i], 0, 0, 0};
        } else {
            memcpy(entries, e, sizeof(PALETTEENTRY) * n);
        }
    }
}

Palette::~Palette() {
    if (dd_) dd_->IntRelease();
}

int Palette::Count() const {
    if (caps & DDPCAPS_1BIT) return 2;
    if (caps & DDPCAPS_2BIT) return 4;
    if (caps & DDPCAPS_4BIT) return 16;
    return 256;
}

HRESULT Palette::QueryInterface(REFIID r, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirectDrawPalette)) {
        AddRef();
        *out = this;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG Palette::AddRef() { return ULONG(InterlockedIncrement(&ref_)); }

ULONG Palette::Release() {
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) {
        Guard l;
        delete this;
    }
    return ULONG(r < 0 ? 0 : r);
}

HRESULT Palette::GetCaps(LPDWORD c) {
    if (!c) return DDERR_INVALIDPARAMS;
    *c = caps;
    return DD_OK;
}

HRESULT Palette::GetEntries(DWORD, DWORD base, DWORD count, LPPALETTEENTRY out) {
    const DWORD n = DWORD(Count());
    if (!out || base > n || count > n - base) return DDERR_INVALIDPARAMS;
    Guard l;
    if (caps & DDPCAPS_8BITENTRIES) {
        auto* idx = reinterpret_cast<uint8_t*>(out);
        for (DWORD i = 0; i < count; i++) idx[i] = entries[base + i].peRed;
    } else {
        memcpy(out, entries + base, sizeof(PALETTEENTRY) * count);
    }
    return DD_OK;
}

HRESULT Palette::Initialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) { return DDERR_ALREADYINITIALIZED; }

HRESULT Palette::SetEntries(DWORD, DWORD start, DWORD count, LPPALETTEENTRY in) {
    const DWORD n = DWORD(Count());
    if (!in || start > n || count > n - start) return DDERR_INVALIDPARAMS;
    Guard l;
    if (caps & DDPCAPS_8BITENTRIES) {
        auto* idx = reinterpret_cast<const uint8_t*>(in);
        for (DWORD i = 0; i < count; i++) entries[start + i] = {idx[i], 0, 0, 0};
    } else {
        memcpy(entries + start, in, sizeof(PALETTEENTRY) * count);
    }
    version++;
    DD2D9_TRACE("Palette %p SetEntries start=%lu count=%lu%s", static_cast<void*>(this), start, count,
               dd_ && dd_->primary && dd_->primary->palette == this ? " (primary)" : "");
    if (dd_ && dd_->primary && dd_->primary->palette == this) dd_->MarkDirty();
    return DD_OK;
}

// ================================================================ Clipper

Clipper::Clipper(DDraw* dd) : dd_(dd) {
    if (dd_) dd_->IntAddRef();
}

Clipper::~Clipper() {
    if (dd_) dd_->IntRelease();
}

HRESULT Clipper::QueryInterface(REFIID r, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirectDrawClipper)) {
        AddRef();
        *out = this;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG Clipper::AddRef() { return ULONG(InterlockedIncrement(&ref_)); }

ULONG Clipper::Release() {
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) {
        Guard l;
        delete this;
    }
    return ULONG(r < 0 ? 0 : r);
}

HRESULT Clipper::GetClipList(LPRECT rect, LPRGNDATA data, LPDWORD size) {
    if (!size) return DDERR_INVALIDPARAMS;
    Guard l;
    std::vector<uint8_t> list;
    if (hwnd) {
        RECT cr;
        if (!GetClientRect(hwnd, &cr)) return DDERR_INVALIDPARAMS;
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        if (rect && !IntersectRect(&cr, &cr, rect)) SetRectEmpty(&cr);
        list.resize(sizeof(RGNDATAHEADER) + sizeof(RECT));
        auto* rd = reinterpret_cast<RGNDATA*>(list.data());
        rd->rdh.dwSize = sizeof(RGNDATAHEADER);
        rd->rdh.iType = RDH_RECTANGLES;
        rd->rdh.nCount = IsRectEmpty(&cr) ? 0 : 1;
        rd->rdh.nRgnSize = sizeof(RECT);
        rd->rdh.rcBound = cr;
        memcpy(rd->Buffer, &cr, sizeof(RECT));
    } else {
        if (clipList_.empty()) return DDERR_NOCLIPLIST;
        list = clipList_;
    }
    if (!data) {
        *size = DWORD(list.size());
        return DD_OK;
    }
    if (*size < list.size()) {
        *size = DWORD(list.size());
        return DDERR_REGIONTOOSMALL;
    }
    memcpy(data, list.data(), list.size());
    return DD_OK;
}

std::vector<RECT> Clipper::Region(const Surface* target) const {
    std::vector<RECT> out;
    if (hwnd) {
        if (!IsWindow(hwnd)) return out;
        if (target->isPrimary && target->dd->Exclusive()) {
            // The window covers the (virtual) screen, whatever its real position.
            out.push_back(RECT{0, 0, target->W(), target->H()});
            return out;
        }
        RECT cr;
        GetClientRect(hwnd, &cr);
        if (target->isPrimary) MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);  // screen coordinates
        out.push_back(cr);
        return out;
    }
    if (clipList_.size() >= sizeof(RGNDATAHEADER)) {
        const auto* rd = reinterpret_cast<const RGNDATA*>(clipList_.data());
        const auto* r = reinterpret_cast<const RECT*>(rd->Buffer);
        const size_t n = (clipList_.size() - sizeof(RGNDATAHEADER)) / sizeof(RECT);
        for (size_t i = 0; i < rd->rdh.nCount && i < n; i++) out.push_back(r[i]);
    }
    return out;
}

HRESULT Clipper::GetHWnd(HWND* out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = hwnd;
    return DD_OK;
}

HRESULT Clipper::Initialize(LPDIRECTDRAW, DWORD) { return DD_OK; }

HRESULT Clipper::IsClipListChanged(WINBOOL* changed) {
    if (!changed) return DDERR_INVALIDPARAMS;
    *changed = FALSE;
    return DD_OK;
}

HRESULT Clipper::SetClipList(LPRGNDATA data, DWORD) {
    Guard l;
    if (hwnd) return DDERR_CLIPPERISUSINGHWND;
    if (!data) {
        clipList_.clear();
        return DD_OK;
    }
    if (data->rdh.nCount > 65536) return DDERR_INVALIDPARAMS;  // also keeps the size below from overflowing
    size_t size = sizeof(RGNDATAHEADER) + size_t(data->rdh.nCount) * sizeof(RECT);
    clipList_.assign(reinterpret_cast<uint8_t*>(data), reinterpret_cast<uint8_t*>(data) + size);
    return DD_OK;
}

HRESULT Clipper::SetHWnd(DWORD, HWND h) {
    Guard l;
    hwnd = h;
    if (dd_) dd_->MarkDirty();
    return DD_OK;
}

// ================================================================ GammaControl

HRESULT GammaControl::QueryInterface(REFIID r, void** out) { return surface->QueryInterface(r, out); }
ULONG GammaControl::AddRef() { return surface->AddRef(); }
ULONG GammaControl::Release() { return surface->Release(); }

HRESULT GammaControl::GetGammaRamp(DWORD, LPDDGAMMARAMP ramp) {
    if (!ramp) return DDERR_INVALIDPARAMS;
    Presenter* p = surface->dd->GetPresenter();
    if (!p) return DDERR_GENERIC;
    p->GetGamma(ramp);
    return DD_OK;
}

HRESULT GammaControl::SetGammaRamp(DWORD, LPDDGAMMARAMP ramp) {
    if (!ramp) return DDERR_INVALIDPARAMS;
    Presenter* p = surface->dd->GetPresenter();
    if (!p) return DDERR_GENERIC;
    p->SetGamma(*ramp);
    surface->dd->MarkDirty();
    return DD_OK;
}

}  // namespace dd2d9
