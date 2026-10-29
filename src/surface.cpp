// Surfaces, surface memory, palettes, clippers and gamma control.
#include "wrappers.h"

#include "d3dproxy.h"

#include <algorithm>
#include <new>

namespace dd2d9 {

// ================================================================ Buffer

static bool DibFormat(const Format& f) {
    return !f.zbuf && !f.fourcc && (f.bpp == 8 || f.bpp == 16 || f.bpp == 24 || f.bpp == 32);
}

// A top-down DIB section for format f (DibFormat(f)). As ddraw.dll's: 8-bit with a
// colour table, 24-bit and standard 32-bit as BI_RGB, others with their masks.
static HBITMAP MakeDib(int w, int h, const Format& f, uint8_t** bits) {
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
    } else if (f.bpp == 24 || (f.bpp == 32 && f.r == 0xFF0000 && f.g == 0xFF00 && f.b == 0xFF)) {
        bi.h.biCompression = BI_RGB;
    } else {
        bi.h.biCompression = BI_BITFIELDS;
        bi.colors[0] = f.r;
        bi.colors[1] = f.g;
        bi.colors[2] = f.b;
    }
    void* p = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &p, nullptr, 0);
    *bits = static_cast<uint8_t*>(p);
    return dib;
}

static LONG DibPitch(int w, const Format& f) { return ((w * LONG(f.bpp) + 31) / 32) * 4; }

// Surface memory is plain memory with ddraw.dll's pitch (rows aligned to 8 bytes in a
// 32-bit process); GetDC puts a DC right on it (see Buffer::GetDC).
Buffer* Buffer::Create(int w, int h, const Format& f) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return nullptr;
    auto* b = new (std::nothrow) Buffer();
    if (!b) return nullptr;
    b->w = w;
    b->h = h;
    b->fmt = f;
    size_t size;
    if (f.BlockCompressed()) {
        // DXT1 is 4 bits/pixel, the others 8.
        int bw = (w + 3) & ~3, bh = (h + 3) & ~3;
        size = f.fourCC == MAKEFOURCC('D', 'X', 'T', '1') ? size_t(bw) * bh / 2 : size_t(bw) * bh;
        b->pitch = LONG(size / bh);
    } else {
        b->pitch = ((w * LONG(f.Bytes())) + 7) & ~7;
        size = size_t(b->pitch) * h;
    }
    b->bits = static_cast<uint8_t*>(_aligned_malloc(size, 16));
    if (!b->bits) {
        delete b;
        return nullptr;
    }
    memset(b->bits, 0, size);
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

// gdi32's D3DKMTCreateDCFromMemory (Vista and later, Wine): a DC drawing straight
// into memory of any pitch, as ddraw.dll's surface DCs do.
namespace {
struct KmtCreateDC {
    void* pMemory;
    UINT Format;  // D3DDDIFORMAT
    UINT Width, Height, Pitch;
    HDC hDeviceDc;
    PALETTEENTRY* pColorTable;
    HDC hDc;         // out
    HANDLE hBitmap;  // out
};
struct KmtDestroyDC {
    HDC hDc;
    HANDLE hBitmap;
};
using KmtCreateFn = LONG(WINAPI*)(KmtCreateDC*);
using KmtDestroyFn = LONG(WINAPI*)(const KmtDestroyDC*);
KmtCreateFn g_kmtCreate;
KmtDestroyFn g_kmtDestroy;

void LoadKmt() {
    static bool done;
    if (done) return;
    done = true;
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) return;
    g_kmtCreate = reinterpret_cast<KmtCreateFn>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTCreateDCFromMemory")));
    g_kmtDestroy = reinterpret_cast<KmtDestroyFn>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTDestroyDCFromMemory")));
    if (!g_kmtCreate || !g_kmtDestroy) g_kmtCreate = nullptr;
}

// The D3DDDIFORMAT for f, or 0 if GDI can't take it.
UINT KmtFormat(const Format& f) {
    if (f.zbuf || f.fourcc) return 0;
    if (f.pal8) return 41;  // P8
    if (f.bpp == 32 && f.r == 0xFF0000 && f.g == 0xFF00 && f.b == 0xFF) return f.a == 0xFF000000u ? 21 : 22;
    if (f.bpp == 24 && f.r == 0xFF0000 && f.g == 0xFF00 && f.b == 0xFF) return 20;
    if (f.bpp == 16 && f.r == 0xF800 && f.g == 0x7E0 && f.b == 0x1F) return 23;
    if (f.bpp == 16 && f.r == 0x7C00 && f.g == 0x3E0 && f.b == 0x1F) return f.a == 0x8000 ? 25 : 24;
    if (f.bpp == 16 && f.r == 0xF00 && f.g == 0xF0 && f.b == 0xF) return f.a == 0xF000 ? 26 : 30;
    return 0;
}
}  // namespace

void Buffer::Release() {
    if (InterlockedDecrement(&ref) != 0) return;
    if (kmtBitmap) {
        KmtDestroyDC d = {dc, kmtBitmap};
        g_kmtDestroy(&d);
        dc = nullptr;
    }
    if (dc) DeleteDC(dc);
    if (shadow) DeleteObject(shadow);
    if (ownsMemory) _aligned_free(bits);
    delete this;
}

// A DC on the memory itself where GDI can do that; otherwise (no D3DKMT, or a format
// GDI only takes as a DIB) through a shadow DIB: the pixels are copied in here and
// back in EndDC().
HDC Buffer::GetDC() {
    if (dc) {
        if (shadow) CopyToShadow();
        return dc;
    }
    LoadKmt();
    const UINT kmt = KmtFormat(fmt);
    if (g_kmtCreate && kmt && pitch > 0) {
        PALETTEENTRY gray[256];
        for (int i = 0; i < 256; i++) gray[i] = {BYTE(i), BYTE(i), BYTE(i), 0};
        HDC screen = CreateCompatibleDC(nullptr);
        KmtCreateDC c = {bits, kmt, UINT(w), UINT(h), UINT(pitch), screen, fmt.pal8 ? gray : nullptr, nullptr, nullptr};
        const LONG status = g_kmtCreate(&c);
        if (screen) DeleteDC(screen);
        if (status == 0 && c.hDc) {
            dc = c.hDc;
            kmtBitmap = c.hBitmap;
            return dc;
        }
        DD2D9_LOG("D3DKMTCreateDCFromMemory(%dx%d, format %u, pitch %ld) failed (0x%lx): using a DIB copy", w, h,
                  kmt, pitch, status);
    }
    if (!DibFormat(fmt)) return nullptr;
    if (!(shadow = MakeDib(w, h, fmt, &shadowBits))) return nullptr;
    if (!(dc = CreateCompatibleDC(nullptr))) {
        DeleteObject(shadow);
        shadow = nullptr;
        return nullptr;
    }
    SelectObject(dc, shadow);
    CopyToShadow();
    return dc;
}

void Buffer::CopyToShadow() {
    const LONG sp = DibPitch(w, fmt), row = LONG(w * fmt.Bytes());
    for (int y = 0; y < h; y++) memcpy(shadowBits + y * sp, Row(y), size_t(row));
}

void Buffer::EndDC() {
    if (!shadow) return;
    GdiFlush();
    const LONG sp = DibPitch(w, fmt), row = LONG(w * fmt.Bytes());
    for (int y = 0; y < h; y++) memcpy(Row(y), shadowBits + y * sp, size_t(row));
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
    if (desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY) {  // called with g_lock held
        vidMemBytes = uint64_t(buf->pitch) * uint64_t(buf->fmt.BlockCompressed() ? (buf->h + 3) & ~3 : buf->h);
        dd->vidMemUsed += vidMemBytes;
    }
}

Surface::~Surface() {
    // Called with g_lock held.
    // What AddAttachedSurface attached here holds a reference: unlink everything first,
    // then release those (which may destroy them).
    std::vector<Surface*> drop;
    const std::vector<AttachRef> refs = attachRefs;
    attachRefs.clear();
    attached.clear();
    LeaveFlipRing(&drop);
    for (const AttachRef& a : refs) a.s->Release(a.slot);
    for (Surface* a : drop) a->DropAttachRef();
    // The members of a complex surface (back buffers, mip levels) go with it, as
    // in ddraw. ddraw destroys them even if the game still holds references, leaving
    // it dangling pointers; here such a member stays alive as a standalone surface
    // until it is released. Members link to each other without references: those
    // links go first, so that no member's destructor releases a detached one.
    for (Surface* c : children) c->ForgetMembers(this);
    std::vector<Surface*> doomed;
    for (Surface* c : children) {
        // Drop the root's own reference (unless the game released it already, see Release).
        LONG& held = c->refs_[SlotFor(c->version)];
        LONG r;
        do {
            r = held;
        } while (r > 0 && InterlockedCompareExchange(&held, r - 1, r) != r);
        const LONG left = r > 0 ? InterlockedDecrement(&c->total_) : c->total_;
        if (left > 0) c->Detach(this);
        else doomed.push_back(c);
    }
    for (Surface* c : doomed) delete c;
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
    dd->vidMemUsed -= vidMemBytes;
    if (dd->primary == this) {
        DD2D9_LOG("primary surface %p released", static_cast<void*>(this));
        dd->primary = nullptr;
        dd->MarkDirty();
        dd->OnPrimaryReleased();
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
        AddRef(SlotFor(v));
        *out = Iface(v);
        return S_OK;
    }
    if (IsEqualGUID(r, IID_IDirectDrawGammaControl)) {
        Guard l;
        if (!gamma) gamma = new GammaControl(this);
        AddRef(kRefGamma);
        *out = gamma;
        return S_OK;
    }
    HRESULT hr = D3DProxyQuerySurface(this, r, out);  // textures and Direct3D 1 devices for a wrapper
    if (hr != E_NOINTERFACE) return hr;
    DD2D9_LOG("Surface::QueryInterface(%s): not supported", GuidName(r));
    return E_NOINTERFACE;
}

// A member of a complex surface also holds its root's reference (counted through the
// interface it was created with, as ddraw.dll counts it), and lives until the root is
// destroyed.
ULONG Surface::AddRef(int slot) {
    InterlockedIncrement(&total_);
    return ULONG(InterlockedIncrement(&refs_[slot]));
}

ULONG Surface::Release(int slot) {
    // Released more often than referenced through this interface: ignored.
    LONG r;
    do {
        r = refs_[slot];
        if (r <= 0) return 0;
    } while (InterlockedCompareExchange(&refs_[slot], r - 1, r) != r);
    r--;
    if (InterlockedDecrement(&total_) == 0) {
        DDraw* owner = nullptr;
        int ownerRef = 0;
        {
            Guard l;
            if (root) return ULONG(r);  // can't happen: the root holds a reference
            owner = dd;
            ownerRef = ddRef;
            owner->IntAddRef();  // the object stays allocated until its Release below is done
            delete this;
        }
        // The DirectDraw interface this surface referenced (created through
        // IDirectDraw4/7): released last, it may tear the object down.
        if (ownerRef) owner->Release(ownerRef);
        owner->IntRelease();
    }
    return ULONG(r < 0 ? 0 : r);
}

void Surface::ForgetMembers(const Surface* oldRoot) {
    auto member = [oldRoot](const Surface* a) {
        return a == oldRoot ||
               std::find(oldRoot->children.begin(), oldRoot->children.end(), a) != oldRoot->children.end();
    };
    attached.erase(std::remove_if(attached.begin(), attached.end(), member), attached.end());
    chainNext = nullptr;
}

void Surface::Detach(const Surface* oldRoot) {
    // ddraw.dll destroys the members with their root, references or not, leaving the
    // game a dangling pointer (Jazz Jackrabbit 2 still uses its old back buffer). Here
    // the surface lives on, usable, until released; but as far as ddraw.dll's
    // bookkeeping goes it is gone: EnumSurfaces doesn't list it and it no longer takes
    // video memory, so leaked references don't make the game run out of it.
    root = nullptr;
    desc.ddsCaps.dwCaps &= ~(DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX);
    desc.ddsCaps.dwCaps2 &= ~DDSCAPS2_MIPMAPSUBLEVEL;
    dd->vidMemUsed -= vidMemBytes;
    vidMemBytes = 0;
    auto& list = dd->surfaces;
    list.erase(std::remove(list.begin(), list.end(), this), list.end());
    DD2D9_LOG("surface %p outlives its complex surface %p (still referenced): kept as a standalone surface",
              static_cast<void*>(this), static_cast<const void*>(oldRoot));
}

// Testing ddraw.dll with DDCREATE_EMULATIONONLY showed the primary and Z buffers can be
// lost even in system memory.
bool Surface::CanBeLost() const {
    const DWORD caps = desc.ddsCaps.dwCaps;
    return !(caps & DDSCAPS_SYSTEMMEMORY) || (caps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_ZBUFFER));
}

bool Surface::IsLost() const {
    return CanBeLost() && (dd->DevState() != DDraw::kDevOk || lost);
}

HRESULT Surface::Restore() {
    if (isPrimary) {  // only in the display mode it was made for
        DWORD w, h, bpp;
        dd->DisplayMode(&w, &h, &bpp);
        if (int(w) != buf->w || int(h) != buf->h || !(Format::ForDisplayBpp(bpp) == buf->fmt))
            return DDERR_WRONGMODE;
    }
    if (!CanBeLost()) return DD_OK;
    dd->UpdateLostSurfaces();
    if (dd->DevState() == DDraw::kDevLost) return DDERR_WRONGMODE;
    // With the surfaces of its complex structure below it.
    lost = false;
    if (!root) {
        for (Surface* c : children) c->lost = false;
    } else {
        for (Surface* s = NextMipLevel(); s; s = s->NextMipLevel()) s->lost = false;
        for (Surface* s : root->flipChain) s->lost = false;
    }
    return DD_OK;
}

// What may be attached to what, as ddraw.dll decides it. IDirectDrawSurface7 takes only
// Z buffers; before it, offscreen surfaces and primaries of one size go together, Z
// buffers go on primaries and 3D devices, and IDirectDrawSurface (1) surfaces build flip
// chains by attaching their back buffers.
static bool FlipAttachable(const Surface* s) {
    return s->version == 1 && !s->root &&
           !(s->desc.ddsCaps.dwCaps & (DDSCAPS_ZBUFFER | DDSCAPS_TEXTURE | DDSCAPS_OVERLAY));
}

HRESULT Surface::AddAttachedSurface(Surface* s, int version) {
    if (!s) return DDERR_INVALIDPARAMS;
    const DWORD tc = desc.ddsCaps.dwCaps, ac = s->desc.ddsCaps.dwCaps;
    const DWORD screen = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN;
    const bool forFlip = FlipAttachable(s);
    if (version == 7) {
        if (!(ac & DDSCAPS_ZBUFFER)) return DDERR_CANNOTATTACHSURFACE;
    } else if ((tc & screen) && (ac & screen)) {
        if (s->W() != W() || s->H() != H()) return DDERR_CANNOTATTACHSURFACE;
    } else if (!((tc & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_3DDEVICE)) && (ac & DDSCAPS_ZBUFFER)) &&
               !(this->version == 1 && !(tc & (DDSCAPS_ZBUFFER | DDSCAPS_TEXTURE | DDSCAPS_OVERLAY)) && forFlip)) {
        return DDERR_CANNOTATTACHSURFACE;
    }
    if (s == this) return DDERR_CANNOTATTACHSURFACE;
    const int slot = SlotFor(version);
    if (forFlip) return AttachForFlip(s, slot);
    // Parts of one complex surface are already attached to each other; a
    // reference to our own root would also keep the chain alive forever.
    if ((s->root ? s->root : s) == (root ? root : this)) return DDERR_CANNOTATTACHSURFACE;
    for (Surface* a : attached)
        if (a == s) return DDERR_SURFACEALREADYATTACHED;
    for (Surface* a : s->attached)
        if (a == this) return DDERR_SURFACEALREADYATTACHED;  // the other way round
    // A Z buffer has to be the size of what it serves.
    if ((ac & DDSCAPS_ZBUFFER) && !(tc & DDSCAPS_ZBUFFER) && (s->W() != W() || s->H() != H()))
        return DDERR_CANNOTATTACHSURFACE;
    s->AddRef(slot);
    attached.push_back(s);
    attachRefs.push_back({s, slot});
    return DD_OK;
}

// A flip ring's back buffer count is kept by its front (the one created with
// DDSD_BACKBUFFERCOUNT), as in ddraw.dll.
static void UpdateBackBufferCount(Surface* start, int delta) {
    Surface* s = start;
    for (int guard = 0; s && guard < 4096; guard++) {
        if (!s->root && (s->desc.dwFlags & DDSD_BACKBUFFERCOUNT)) {
            s->desc.dwBackBufferCount = DWORD(LONG(s->desc.dwBackBufferCount) + delta);
            return;
        }
        s = s->chainNext;
        if (s == start) return;
    }
}

// s joins this surface's flip ring right after it (forming one if there is none).
HRESULT Surface::AttachForFlip(Surface* s, int slot) {
    if (isPrimary && !dd->Exclusive()) return DDERR_NOEXCLUSIVEMODE;
    if (chainNext == s) return DDERR_SURFACEALREADYATTACHED;
    if (s->chainNext || s->W() != W() || s->H() != H() || !(s->buf->fmt == buf->fmt))
        return DDERR_CANNOTATTACHSURFACE;
    s->attachSavedCaps = s->desc.ddsCaps.dwCaps & ~DDSCAPS_FRONTBUFFER;
    s->flipStart = false;
    s->desc.ddsCaps.dwCaps |= DDSCAPS_FLIP;
    if (!chainNext) {
        flipStart = true;
        chainNext = s;
        s->chainNext = this;
        attachSavedCaps = desc.ddsCaps.dwCaps & ~DDSCAPS_FRONTBUFFER;
        desc.ddsCaps.dwCaps |= DDSCAPS_FLIP;
        if (!(desc.ddsCaps.dwCaps & (DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER))) desc.ddsCaps.dwCaps |= DDSCAPS_FRONTBUFFER;
        s->desc.ddsCaps.dwCaps |= (desc.ddsCaps.dwCaps & DDSCAPS_BACKBUFFER) ? DDSCAPS_FRONTBUFFER : DDSCAPS_BACKBUFFER;
    } else {
        s->chainNext = chainNext;
        chainNext = s;
    }
    UpdateBackBufferCount(this, 1);
    s->AddRef(slot);
    s->attachSlot = slot;
    return DD_OK;
}

void Surface::DropAttachRef() {
    const int slot = attachSlot;
    attachSlot = -1;
    if (slot >= 0) Release(slot);
}

// Being destroyed (g_lock held): leave any flip ring built with AddAttachedSurface.
// The front takes the ring apart; the surfaces whose attachment references need
// releasing go to *drop.
void Surface::LeaveFlipRing(std::vector<Surface*>* drop) {
    if (!chainNext) return;
    if (flipStart || (!root && !flipChain.empty())) {
        Surface* m = chainNext;
        for (int guard = 0; m && m != this && guard < 4096; guard++) {
            Surface* next = m->chainNext;
            if (!m->root) {  // attached, not a member of a created chain
                m->desc.ddsCaps.dwCaps = m->attachSavedCaps;
                m->flipStart = false;
                m->chainNext = nullptr;
                if (m->attachSlot >= 0) drop->push_back(m);
            }
            m = next;
        }
        if (flipStart) {
            flipStart = false;
            chainNext = nullptr;
        }
        return;
    }
    if (root) return;  // a created chain's member: its root takes care of the links
    // Another surface's attached back buffer: take it out of the ring.
    Surface* p = chainNext;
    for (int guard = 0; p->chainNext != this && guard < 4096; guard++) p = p->chainNext;
    p->chainNext = chainNext;
    if (p->chainNext == p) {  // only the front is left
        p->chainNext = nullptr;
        p->flipStart = false;
        p->desc.ddsCaps.dwCaps = p->attachSavedCaps;
    }
    chainNext = nullptr;
}

HRESULT Surface::DeleteAttachedSurface(DWORD, Surface* s, int ifaceVersion) {
    if (!s) return DDERR_CANNOTDETACHSURFACE;
    if (FlipAttachable(s)) {
        // A back buffer attached for flipping: only from the surface before it in the ring.
        if (version != 1 || chainNext != s) return DDERR_SURFACENOTATTACHED;
        if (s->root || s->flipStart) return DDERR_CANNOTDETACHSURFACE;
        UpdateBackBufferCount(this, -1);
        Surface* next = chainNext = s->chainNext;
        if (next == this) {
            flipStart = false;
            chainNext = nullptr;
            desc.ddsCaps.dwCaps = attachSavedCaps;
        } else if (!(next->desc.ddsCaps.dwCaps & DDSCAPS_FRONTBUFFER) || (s->desc.ddsCaps.dwCaps & DDSCAPS_FRONTBUFFER)) {
            // The surface after it takes its role (the front keeps its own).
            next->desc.ddsCaps.dwCaps &= ~(DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER);
            if (s->desc.ddsCaps.dwCaps & DDSCAPS_FRONTBUFFER) {
                desc.ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER;
                next->desc.ddsCaps.dwCaps |= DDSCAPS_FRONTBUFFER;
            } else {
                next->desc.ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER;
            }
        }
        s->desc.ddsCaps.dwCaps = s->attachSavedCaps;
        s->chainNext = nullptr;
        s->DropAttachRef();
        return DD_OK;
    }
    Surface* family = root ? root : this;
    for (size_t i = 0; i < attached.size(); i++) {
        Surface* a = attached[i];
        if (a != s) continue;
        // Levels of our own complex surface (mipmaps) can't be detached.
        if ((a->root ? a->root : a) == family) return DDERR_CANNOTDETACHSURFACE;
        for (size_t k = 0; k < attachRefs.size(); k++) {
            if (attachRefs[k].s != s) continue;
            const int slot = attachRefs[k].slot;
            // Only through the interface version it was attached with.
            if (slot != SlotFor(ifaceVersion)) return DDERR_SURFACENOTATTACHED;
            attachRefs.erase(attachRefs.begin() + k);
            attached.erase(attached.begin() + i);
            s->Release(slot);
            return DD_OK;
        }
        return DDERR_CANNOTDETACHSURFACE;
    }
    return DDERR_CANNOTDETACHSURFACE;
}

HRESULT Surface::EnumAttachedSurfaces(int v, void* ctx, void* callback) {
    if (!callback) return DDERR_INVALIDPARAMS;
    using CB = HRESULT(WINAPI*)(void*, void*, void*);
    std::vector<Surface*> list;
    {
        Guard l;
        if (chainNext && chainNext != this) list.push_back(chainNext);
        for (Surface* a : attached) list.push_back(a);
        for (Surface* s : list) s->AddRef(SlotFor(v));  // the callback releases it
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
    for (; i < list.size(); i++) list[i]->Release(SlotFor(v));  // never handed to the callback
    return DD_OK;
}

HRESULT Surface::GetAttachedSurface(const DDSCAPS2& caps, Surface** out, int version) {
    if (IsLost()) return DDERR_SURFACELOST;
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
    found->AddRef(SlotFor(version));
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

static bool LiveClipper(const Clipper* c);

HRESULT Surface::SetClipper(IDirectDrawClipper* c) {
    Clipper* nc = c ? Clipper::From(c) : nullptr;
    if (c && (!nc || !LiveClipper(nc))) return DDERR_INVALIDPARAMS;
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
    // Without DDCKEY_COLORSPACE the key is the low value; key ranges need hardware
    // support, which ddraw.dll reports none of.
    if ((flags & DDCKEY_COLORSPACE) && key->dwColorSpaceLowValue != key->dwColorSpaceHighValue)
        return DDERR_NOCOLORKEYHW;
    ck[i].dwColorSpaceLowValue = ck[i].dwColorSpaceHighValue = key->dwColorSpaceLowValue;
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
    if (!dc) {
        *out = nullptr;  // as ddraw.dll (DDERR_DCALREADYCREATED leaves it alone)
        return DDERR_CANTCREATEDC;
    }
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
    if (!dcOut) {
        // ddraw.dll's GetDC locks the surface and ReleaseDC unlocks it: with no DC out, a
        // lock taken before GetDC is what goes.
        if (lockCount <= 0) return DDERR_NODC;
        lockCount--;
        uniqueness++;
        MarkDirtyIfVisible(&lockArea);
        return DD_OK;
    }
    if (dc != dcOut) return DDERR_INVALIDPARAMS;
    GdiFlush();
    buf->EndDC();
    dcOut = nullptr;
    DD2D9_TRACE("ReleaseDC %p%s", static_cast<void*>(this), isPrimary ? " (primary)" : "");
    uniqueness++;
    MarkDirtyIfVisible();
    return DD_OK;
}

HRESULT Surface::GetPalette(IDirectDrawPalette** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (IsLost()) return DDERR_SURFACELOST;
    if (!palette) return DDERR_NOPALETTEATTACHED;
    palette->AddRef();
    *out = palette;
    return DD_OK;
}

HRESULT Surface::SetPalette(IDirectDrawPalette* p) {
    Palette* np = p ? Palette::From(p) : nullptr;
    if (p && !np) return DDERR_INVALIDPARAMS;
    if (desc.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL) return DDERR_NOTONMIPMAPSUBLEVEL;  // the top level's applies
    if (IsLost()) return DDERR_SURFACELOST;
    if (np && (np->caps & DDPCAPS_ALPHA) && !(desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE)) return DDERR_INVALIDSURFACETYPE;
    if (!buf->fmt.pal8) return DDERR_INVALIDPIXELFORMAT;
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

HRESULT Surface::SetSurfaceDesc(const DDSURFACEDESC2& d, DWORD flags) {
    // Points a system-memory surface at client memory (DDSD_LPSURFACE), optionally with a new
    // size, pitch and pixel format. Checks in the order ddraw.dll makes them.
    if (flags) return DDERR_INVALIDPARAMS;
    const DWORD caps = desc.ddsCaps.dwCaps, caps2 = desc.ddsCaps.dwCaps2;
    if (!(caps & DDSCAPS_SYSTEMMEMORY) || (caps & DDSCAPS_PRIMARYSURFACE) ||
        (caps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)))
        return DDERR_INVALIDSURFACETYPE;
    if (d.dwFlags & ~(DDSD_LPSURFACE | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_CAPS))
        return DDERR_INVALIDPARAMS;
    if (!(d.dwFlags & DDSD_LPSURFACE) || !d.lpSurface) return DDERR_INVALIDPARAMS;
    if ((d.dwFlags & DDSD_CAPS) && d.ddsCaps.dwCaps) return DDERR_INVALIDCAPS;  // caps 0 is ignored
    // Width and pitch go together. The pitch only has to be positive and DWORD-aligned.
    if (!(d.dwFlags & DDSD_WIDTH) != !(d.dwFlags & DDSD_PITCH)) return DDERR_INVALIDPARAMS;
    if ((d.dwFlags & DDSD_WIDTH) && (!d.dwWidth || d.lPitch <= 0 || (d.lPitch & 3))) return DDERR_INVALIDPARAMS;
    if ((d.dwFlags & DDSD_HEIGHT) && !d.dwHeight) return DDERR_INVALIDPARAMS;
    if (lockCount || dcOut) return DDERR_SURFACEBUSY;
    const int w = (d.dwFlags & DDSD_WIDTH) ? int(d.dwWidth) : buf->w;
    const int h = (d.dwFlags & DDSD_HEIGHT) ? int(d.dwHeight) : buf->h;
    const LONG pitch = (d.dwFlags & DDSD_PITCH) ? d.lPitch : buf->pitch;
    if (w <= 0 || h <= 0) return DDERR_INVALIDPARAMS;  // above INT_MAX
    Format f = buf->fmt;
    if ((d.dwFlags & DDSD_PIXELFORMAT) && !Format::FromDDPF(d.ddpfPixelFormat, &f)) return DDERR_INVALIDPARAMS;
    const bool reshaped = w != buf->w || h != buf->h || !(f == buf->fmt);
    // ddraw.dll lets mipmap levels change shape independently. A flip-chain member can't:
    // Flip swaps buffers between the members.
    if (reshaped && chainNext) return DDERR_INVALIDPARAMS;
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
    if (IsLost()) return DDERR_SURFACELOST;
    // Busy with a DC out, or locked as a whole already (rectangles may still be locked
    // side by side).
    if (dcOut) return DDERR_SURFACEBUSY;
    if (lockCount && (!r || (lockArea.left <= 0 && lockArea.top <= 0 && lockArea.right >= buf->w &&
                             lockArea.bottom >= buf->h)))
        return DDERR_SURFACEBUSY;
    GdiFlush();
    GetSurfaceDesc(out);
    uint8_t* p = buf->bits;
    if (r && !buf->fmt.BlockCompressed()) {
        if (r->left < 0 || r->top < 0 || r->right > buf->w || r->bottom > buf->h || r->left >= r->right ||
            r->top >= r->bottom)
            return DDERR_INVALIDPARAMS;
        p += r->top * buf->pitch + r->left * LONG(buf->fmt.Bytes());
    }
    out->lpSurface = p;  // ddraw.dll doesn't set DDSD_LPSURFACE in the returned description
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
    *out = dd->Iface(ddVersion);  // the interface it was created through
    dd->AddRef(ddVersion);
    return DD_OK;
}

HRESULT Surface::SetPrivateData(REFGUID tag, void* data, DWORD size, DWORD flags) {
    if (!data) return DDERR_INVALIDPARAMS;
    if ((flags & DDSPD_IUNKNOWNPOINTER) && size != sizeof(IUnknown*)) return DDERR_INVALIDPARAMS;
    PrivateData p = {tag, {}, nullptr};
    if (flags & DDSPD_IUNKNOWNPOINTER) {
        // The pointer the game passes, not what it points to (a copy of the pointer).
        p.unk = static_cast<IUnknown*>(data);
        p.unk->AddRef();
    } else {
        p.data.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    }
    FreePrivateData(tag);  // after the AddRef: setting the same object again keeps it alive
    priv_.push_back(std::move(p));
    return DD_OK;
}

HRESULT Surface::GetPrivateData(REFGUID tag, void* data, DWORD* size) {
    for (auto& p : priv_) {
        if (!IsEqualGUID(p.tag, tag)) continue;
        if (!size) return DDERR_INVALIDPARAMS;
        const DWORD need = p.unk ? sizeof(IUnknown*) : DWORD(p.data.size());
        if (*size < need) {
            *size = need;
            return DDERR_MOREDATA;
        }
        if (!data) return DDERR_INVALIDPARAMS;
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
        if (IsLost()) return DDERR_SURFACELOST;
        // Called on the front buffer of a chain (created, or put together with AddAttachedSurface).
        const DWORD caps = desc.ddsCaps.dwCaps;
        if (!(caps & DDSCAPS_FLIP) || !(caps & DDSCAPS_FRONTBUFFER) || !chainNext) return DDERR_NOTFLIPPABLE;
        if (isPrimary && !dd->Exclusive()) return DDERR_NOEXCLUSIVEMODE;
        DD2D9_TRACE("Flip %p target=%p flags=0x%lx", static_cast<void*>(this), static_cast<void*>(target), flags);
        // The chain's order is the ring's (back buffers attached later sit right after the front).
        std::vector<Surface*> chain{this};
        for (Surface* m = chainNext; m && m != this && chain.size() < 4096; m = m->chainNext) chain.push_back(m);
        for (Surface* s : chain)
            if (s->lockCount || s->dcOut) return DDERR_SURFACEBUSY;
        if (target) {
            size_t k = 0;
            while (k < chain.size() && chain[k] != target) k++;
            if (k == 0 || k == chain.size()) return DDERR_NOTFLIPPABLE;  // the front buffer, or not in the chain
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
    ReserveSystemEntries();
}

// Without DDPCAPS_ALLOW256, an 8-bit palette's first and last entries belong to
// the system: always black and white, whatever the game sets.
void Palette::ReserveSystemEntries() {
    if ((caps & DDPCAPS_8BIT) && !(caps & (DDPCAPS_ALLOW256 | DDPCAPS_8BITENTRIES))) {
        entries[0] = {0, 0, 0, 0};
        entries[255] = {255, 255, 255, 0};
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
        DDraw* owner = dd_;
        const int ownerRef = ddRef;
        if (owner) owner->IntAddRef();  // see Surface::Release
        {
            Guard l;
            delete this;
        }
        if (ownerRef) owner->Release(ownerRef);
        if (owner) owner->IntRelease();
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
    ReserveSystemEntries();
    version++;
    DD2D9_TRACE("Palette %p SetEntries start=%lu count=%lu%s", static_cast<void*>(this), start, count,
               dd_ && dd_->primary && dd_->primary->palette == this ? " (primary)" : "");
    if (dd_ && dd_->primary && dd_->primary->palette == this) dd_->MarkDirty();
    return DD_OK;
}

// ================================================================ Clipper

// ddraw.dll checks that a clipper method is called on a live clipper: a released or
// made-up pointer gets an error (or 0 from AddRef/Release) instead of a crash. Games
// that release a clipper once too often survive that here as well.
namespace {
std::vector<const Clipper*>* g_clippers;  // live clippers (g_lock)
const void* g_clipperVtbl;                // the genuine vtable
}

static bool LiveClipper(const Clipper* c) {
    Guard l;
    return g_clippers && std::find(g_clippers->begin(), g_clippers->end(), c) != g_clippers->end();
}

Clipper::Clipper(DDraw* dd) : dd_(dd) {
    if (dd_) dd_->IntAddRef();
    Guard l;
    if (!g_clippers) g_clippers = new std::vector<const Clipper*>;
    g_clippers->push_back(this);
    g_clipperVtbl = *reinterpret_cast<void* const*>(this);
}

Clipper::~Clipper() {
    // Called with g_lock held.
    g_clippers->erase(std::find(g_clippers->begin(), g_clippers->end(), this));
    if (dd_) dd_->IntRelease();
}

HRESULT Clipper::QueryInterface(REFIID r, void** out) {
    if (!out || !LiveClipper(this)) return DDERR_INVALIDPARAMS;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirectDrawClipper)) {
        AddRef();
        *out = this;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG Clipper::AddRef() {
    if (!LiveClipper(this)) return 0;
    return ULONG(InterlockedIncrement(&ref_));
}

ULONG Clipper::Release() {
    Guard l;  // the check and the decrement go together
    // Also a no-op with the clipper's vtable overwritten (as ddraw.dll).
    if (!LiveClipper(this) || *reinterpret_cast<void* const*>(this) != g_clipperVtbl) return 0;
    LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return ULONG(r < 0 ? 0 : r);
}

HRESULT Clipper::GetClipList(LPRECT rect, LPRGNDATA data, LPDWORD size) {
    if (!size || !LiveClipper(this)) return DDERR_INVALIDPARAMS;
    Guard l;
    std::vector<uint8_t> list;
    if (hwnd) {
        RECT cr;
        if (!GetClientRect(hwnd, &cr)) return E_FAIL;  // the window is gone
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

HRESULT Clipper::Usable() const {
    if (hwnd) return IsWindow(hwnd) ? DD_OK : E_FAIL;
    return clipList_.empty() ? DDERR_NOCLIPLIST : DD_OK;
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
    if (!out || !LiveClipper(this)) return DDERR_INVALIDPARAMS;
    *out = hwnd;
    return DD_OK;
}

HRESULT Clipper::Initialize(LPDIRECTDRAW, DWORD) { return LiveClipper(this) ? DD_OK : DDERR_INVALIDPARAMS; }

HRESULT Clipper::IsClipListChanged(WINBOOL* changed) {
    if (!changed || !LiveClipper(this)) return DDERR_INVALIDPARAMS;
    *changed = FALSE;
    return DD_OK;
}

HRESULT Clipper::SetClipList(LPRGNDATA data, DWORD) {
    if (!LiveClipper(this)) return DDERR_INVALIDPARAMS;
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
    if (!LiveClipper(this)) return DDERR_INVALIDPARAMS;
    Guard l;
    hwnd = h;
    if (dd_) dd_->MarkDirty();
    return DD_OK;
}

// ================================================================ GammaControl

HRESULT GammaControl::QueryInterface(REFIID r, void** out) { return surface->QueryInterface(r, out); }
ULONG GammaControl::AddRef() { return surface->AddRef(Surface::kRefGamma); }
ULONG GammaControl::Release() { return surface->Release(Surface::kRefGamma); }

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
