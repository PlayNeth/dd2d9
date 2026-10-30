// Surfaces, surface memory, palettes, clippers and gamma control.
#include "wrappers.h"
#include "gpures.h"

#include "d3d6.h"

#include <algorithm>
#include <new>

namespace dd2d9 {

// ================================================================ Buffer

static bool DibFormat(const Format& f) {
    // Red in the low byte (A8B8G8R8, X8B8G8R8): ddraw.dll on Vista and later has no DC for it.
    if (f.bpp == 32 && f.r == 0xFF && f.b == 0xFF0000) return false;
    return !f.zbuf && !f.fourcc && (f.bpp == 8 || f.bpp == 16 || f.bpp == 24 || f.bpp == 32);
}

// A top-down DIB section for format f (DibFormat(f)). As ddraw.dll's: 8-bit with a
// colour table, 24- and 32-bit as BI_RGB (whatever the 32-bit masks), 16-bit with its masks.
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
    } else if (f.bpp == 24 || f.bpp == 32) {
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
    if (f.bpp == 32 && f.r == 0x3FF00000 && f.g == 0xFFC00 && f.b == 0x3FF) return 35;  // A2R10G10B10
    if (f.bpp == 24 && f.r == 0xFF0000 && f.g == 0xFF00 && f.b == 0xFF) return 20;
    if (f.bpp == 16 && f.r == 0xF800 && f.g == 0x7E0 && f.b == 0x1F) return 23;
    if (f.bpp == 16 && f.r == 0x7C00 && f.g == 0x3E0 && f.b == 0x1F) return f.a == 0x8000 ? 25 : 24;
    if (f.bpp == 16 && f.r == 0xF00 && f.g == 0xF0 && f.b == 0xF) return f.a == 0xF000 ? 26 : 30;
    return 0;
}
}  // namespace

void Buffer::Release() {
    if (InterlockedDecrement(&ref) != 0) return;
    if (gpu) GpuBufferGone(this);
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
    dd->DisplayMode(&modeW, &modeH, &modeBpp);
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
    D3DLegacySurfaceGone(this);
    if (gpuTex) GpuSurfaceGone(this);
    buf->Release();
    delete static_cast<SurfaceWrap<1>*>(wraps_[0]);
    delete static_cast<SurfaceWrap<2>*>(wraps_[1]);
    delete static_cast<SurfaceWrap<3>*>(wraps_[2]);
    delete static_cast<SurfaceWrap<4>*>(wraps_[3]);
    delete static_cast<SurfaceWrap<7>*>(wraps_[4]);
    delete gamma;
    delete gammaRamp;
    dd->vidMemUsed -= vidMemBytes;
    if (dd->primary == this) {
        DD2D9_LOG("primary surface %p released", static_cast<void*>(this));
        dd->primary = nullptr;
        dd->ApplyGamma();  // its ramp goes with it
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

HRESULT Surface::QueryInterface(REFIID r, void** out, int version) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (IsBadReadPtr(&r, sizeof(IID))) return DDERR_INVALIDPARAMS;
    int v = 0;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirectDrawSurface)) v = 1;
    else if (IsEqualGUID(r, IID_IDirectDrawSurface2)) v = 2;
    else if (IsEqualGUID(r, IID_IDirectDrawSurface3)) v = 3;
    else if (IsEqualGUID(r, IID_IDirectDrawSurface4)) v = 4;
    else if (IsEqualGUID(r, IID_IDirectDrawSurface7)) v = 7;
    if (v) {
        DD2D9_TRACE("QueryInterface %p -> IDirectDrawSurface v%d", static_cast<void*>(this), v);
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
    if (IsEqualGUID(r, IID_IDirectDrawColorControl)) return E_NOINTERFACE;  // ddraw's own; not on these surfaces
    // Textures and Direct3D 1 devices (DirectDraw 1-4 objects).
    HRESULT hr = D3DLegacyQuerySurface(this, r, out, version);
    if (hr != E_NOINTERFACE) return hr;
    DD2D9_LOG("Surface::QueryInterface(%s): not supported", GuidName(r));
    return E_NOINTERFACE;
}

// A member of a complex surface also holds its root's reference (counted through the
// interface it was created with, as ddraw.dll counts it), and lives until the root is
// destroyed.
ULONG Surface::AddRef(int slot) {
    InterlockedIncrement(&total_);
    const LONG r = InterlockedIncrement(&refs_[slot]);
    TraceRef("AddRef", slot, r);
    return ULONG(r);
}

// log=2: reference changes on the primary's flip chain (when a game lets go of its old
// primary decides whether a new one can be created).
void Surface::TraceRef(const char* what, int slot, LONG r) const {
    if (LogLevel() < 2 || !(isPrimary || (root && root->isPrimary))) return;
    static const char* const kSlot[kRefSlots] = {"v1", "v2", "v3", "v4", "v7", "gamma", "dd2d9"};
    DD2D9_TRACE("%s %p%s through %s -> %ld (total %ld)", what, static_cast<const void*>(this),
                isPrimary ? " (primary)" : " (primary chain)", slot >= 0 && slot < kRefSlots ? kSlot[slot] : "?", r,
                total_);
}

ULONG Surface::Release(int slot) {
    // A member of a complex surface with only its root's reference left: ddraw.dll
    // ignores the call (and returns 0).
    if (root && total_ <= 1) {
        TraceRef("Release (ignored: only the root's reference left)", slot, refs_[slot]);
        return 0;
    }
    // Released more often than referenced through this interface: ignored.
    LONG r;
    do {
        r = refs_[slot];
        if (r <= 0) return 0;
    } while (InterlockedCompareExchange(&refs_[slot], r - 1, r) != r);
    r--;
    const LONG left = InterlockedDecrement(&total_);
    TraceRef("Release", slot, r);
    if (left == 0) {
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
// As ddraw.dll (spec: re/spec/surface-lock-dc.md, section 9): every surface but those
// created in system memory on request; the primary's flip chain always.
bool Surface::CanBeLost() const {
    return !sysmemRequested || isPrimary || (root && root->isPrimary);
}

bool Surface::IsLost(bool poll) const {
    if (poll && !dd->Exclusive()) dd->PollDisplayChange();
    if (!CanBeLost()) return false;
    return lost || dd->DevState() != DDraw::kDevOk;  // in the background, or about to be marked lost
}

// Lost as IsLost says, except a surface lost only by Alt+Tab outside the primary's flip
// chain: its pixels are kept, so drawing with it still works (see DDraw::UpdateLostSurfaces).
bool Surface::CantDraw(bool poll) const {
    if (!IsLost(poll)) return false;
    if (lost) return !drawableLost;
    // Not marked yet: in the background everything is, back in front what UpdateLostSurfaces will mark.
    return dd->DevState() == DDraw::kDevLost || !dd->FocusLossOnly() || InPrimaryChain();
}

// Checks in ddraw.dll's order (spec: re/spec/surface-lock-dc.md, section 6). A surface that
// isn't lost needs nothing; one made with its root (back buffer, mip level, cube face) is
// restored through it; any other only in the display mode it was created in.
HRESULT Surface::Restore() {
    dd->UpdateLostSurfaces();
    if (!IsLost()) return DD_OK;
    if (root) return DDERR_IMPLICITLYCREATED;
    DWORD w, h, bpp;
    dd->DisplayMode(&w, &h, &bpp);
    if (w != modeW || h != modeH || bpp != modeBpp) return DDERR_WRONGMODE;
    if (dd->OtherExclusive() || dd->DevState() == DDraw::kDevLost) return DDERR_WRONGMODE;
    // With the surfaces of its complex structure.
    lost = drawableLost = false;
    for (Surface* c : children) c->lost = c->drawableLost = false;
    return DD_OK;
}

// ddraw.dll's attach list, newest first (spec: re/spec/surface-other.md): what
// AddAttachedSurface added, in the order it was added, then what the surface was
// created with (the next member of its flip chain, its mip level, other cube faces).
// The link to the next ring member counts as explicit when AddAttachedSurface made it
// (that member then holds an attachment reference).
std::vector<Surface::AttachEntry> Surface::AttachList() const {
    std::vector<AttachEntry> list;
    const bool ringExplicit = chainNext && chainNext != this && chainNext->attachSlot >= 0;
    if (ringExplicit) list.push_back({chainNext, chainNext->attachSlot, chainNext->ringSeq});
    for (const AttachRef& r : attachRefs) list.push_back({r.s, r.slot, r.seq});
    std::sort(list.begin(), list.end(), [](const AttachEntry& x, const AttachEntry& y) { return x.seq > y.seq; });
    if (chainNext && chainNext != this && !ringExplicit) list.push_back({chainNext, -1, 0});
    for (Surface* a : attached) {
        bool expl = false;
        for (const AttachRef& r : attachRefs) expl |= r.s == a;
        if (!expl) list.push_back({a, -1, 0});
    }
    return list;
}

static uint32_t NextAttachSeq() {
    static uint32_t seq = 0;  // g_lock held
    return ++seq;
}

// ddraw.dll's "flip-compatible" (FUN_73733941): the same Z buffer, texture, overlay and
// alpha bits, and not two mipmaps. The interface version and pixel format don't matter.
static bool FlipCompatible(const Surface* a, const Surface* b) {
    const DWORD ac = a->desc.ddsCaps.dwCaps, bc = b->desc.ddsCaps.dwCaps;
    const DWORD kinds = DDSCAPS_ZBUFFER | DDSCAPS_TEXTURE | DDSCAPS_OVERLAY | DDSCAPS_ALPHA;
    if ((ac ^ bc) & kinds) return false;
    return !((ac & bc & DDSCAPS_TEXTURE) && (ac & bc & DDSCAPS_MIPMAP));
}

// Checks in ddraw.dll's order (spec: re/spec/surface-other.md, AddAttachedSurface).
// `version` is the interface called, `attacheeVersion` that of the pointer passed (whose
// reference the attachment holds).
HRESULT Surface::AddAttachedSurface(Surface* s, int version, int attacheeVersion) {
    if (!s) return DDERR_INVALIDPARAMS;
    const DWORD tc = desc.ddsCaps.dwCaps, ac = s->desc.ddsCaps.dwCaps;
    // IDirectDrawSurface7 attaches Z buffers only (to anything but a Z buffer, or the other way round).
    if (version == 7 && !((tc ^ ac) & DDSCAPS_ZBUFFER)) return DDERR_CANNOTATTACHSURFACE;
    if ((desc.ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP) && !(ac & DDSCAPS_ZBUFFER)) return DDERR_CANNOTATTACHSURFACE;
    if ((tc & DDSCAPS_PRIMARYSURFACE) && !dd->Exclusive()) return DDERR_NOEXCLUSIVEMODE;
    if (s == this) return DDERR_CANNOTATTACHSURFACE;
    if (IsLost() || s->IsLost()) return DDERR_SURFACELOST;
    const int slot = SlotFor(attacheeVersion);
    // The same interface pointer attached already (or made with this surface). Another
    // pointer to the same surface goes on to the checks below, as in ddraw.dll.
    for (const AttachEntry& e : AttachList())
        if (e.s == s && (e.slot < 0 || e.slot == slot)) return DDERR_SURFACEALREADYATTACHED;
    // Sizes: a mipmap level no bigger than this one; a texture and a non-texture of any
    // sizes (a Z buffer on a texture); otherwise equal.
    const DWORD mipTex = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP;
    if ((tc & ac & mipTex) == mipTex) {
        if (W() < s->W() || H() < s->H()) return DDERR_CANNOTATTACHSURFACE;
    } else if (!((tc ^ ac) & DDSCAPS_TEXTURE) && (W() != s->W() || H() != s->H())) {
        return DDERR_CANNOTATTACHSURFACE;
    }
    // One of each kind: a second Z buffer, alpha surface or (mip)texture is refused.
    const DWORD kinds = ac & (DDSCAPS_MIPMAP | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_ALPHA);
    if (kinds) {
        for (const AttachEntry& e : AttachList()) {
            const DWORD ec = e.s->desc.ddsCaps.dwCaps, shared = ec & kinds;
            if (shared && (!(shared & DDSCAPS_TEXTURE) || !((ec ^ kinds) & DDSCAPS_MIPMAP)))
                return DDERR_CANNOTATTACHSURFACE;
        }
    }
    const bool flipPair = FlipCompatible(this, s);
    if ((ac & DDSCAPS_FLIP) && flipPair) return DDERR_CANNOTATTACHSURFACE;  // already in a ring
    // System and video memory don't mix, but for a system-memory Z buffer.
    const bool tSys = (tc & DDSCAPS_SYSTEMMEMORY) != 0, aSys = (ac & DDSCAPS_SYSTEMMEMORY) != 0;
    if (tSys != aSys && !(aSys && (ac & DDSCAPS_ZBUFFER))) return DDERR_CANNOTATTACHSURFACE;
    // dd2d9's own limits, against reference cycles that would keep surfaces alive forever:
    // a part of our own complex surface, or a surface this one is attached to.
    if ((s->root ? s->root : s) == (root ? root : this)) return DDERR_CANNOTATTACHSURFACE;
    for (Surface* a : s->attached)
        if (a == this) return DDERR_SURFACEALREADYATTACHED;
    if (flipPair) return AttachForFlip(s, slot);
    s->AddRef(slot);
    attached.push_back(s);
    attachRefs.push_back({s, slot, NextAttachSeq()});
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

// s joins this surface's flip ring right after it (forming one if there is none), as
// ddraw.dll links it.
HRESULT Surface::AttachForFlip(Surface* s, int slot) {
    // Flip swaps the memory of the members: dd2d9 can't put two formats in one ring.
    if (s->chainNext || !(s->buf->fmt == buf->fmt)) {
        DD2D9_LOG("AddAttachedSurface: a flip ring of two pixel formats isn't supported");
        return DDERR_CANNOTATTACHSURFACE;
    }
    const DWORD roles = DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER;
    s->attachSavedCaps = s->desc.ddsCaps.dwCaps & ~DDSCAPS_FRONTBUFFER;
    s->flipStart = false;
    s->desc.ddsCaps.dwCaps = (s->desc.ddsCaps.dwCaps & ~roles) | DDSCAPS_FLIP;
    if (!chainNext) {
        flipStart = true;
        chainNext = s;
        s->chainNext = this;
        attachSavedCaps = desc.ddsCaps.dwCaps & ~DDSCAPS_FRONTBUFFER;
        desc.ddsCaps.dwCaps |= DDSCAPS_FLIP;
        if (desc.ddsCaps.dwCaps & DDSCAPS_BACKBUFFER) {
            s->desc.ddsCaps.dwCaps |= DDSCAPS_FRONTBUFFER;
        } else {
            desc.ddsCaps.dwCaps |= DDSCAPS_FRONTBUFFER;
            s->desc.ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER;
        }
    } else {
        // Right after the front, the new surface is the back buffer.
        if (desc.ddsCaps.dwCaps & DDSCAPS_FRONTBUFFER) {
            chainNext->desc.ddsCaps.dwCaps &= ~DDSCAPS_BACKBUFFER;
            s->desc.ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER;
        }
        s->chainNext = chainNext;
        chainNext = s;
    }
    UpdateBackBufferCount(this, 1);
    s->AddRef(slot);
    s->attachSlot = slot;
    s->ringSeq = NextAttachSeq();
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

// One entry of the attach list, as ddraw.dll's FUN_7373545a detaches it.
HRESULT Surface::DetachOne(Surface* s, int slot) {
    if (chainNext == s && s != this) {
        if (s->attachSlot < 0) return DDERR_CANNOTDETACHSURFACE;  // a link the ring was made with
        if (s->attachSlot != slot) return DDERR_SURFACENOTATTACHED;
        UpdateBackBufferCount(this, -1);
        const DWORD roles = DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER;
        const DWORD sc = s->desc.ddsCaps.dwCaps;
        Surface* next = chainNext = s->chainNext;
        if (next == this) {  // a ring of two: this one is on its own again
            flipStart = false;
            chainNext = nullptr;
            desc.ddsCaps.dwCaps = attachSavedCaps;
        } else if (sc & DDSCAPS_BACKBUFFER) {
            next->desc.ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER;
        } else if (sc & DDSCAPS_FRONTBUFFER) {
            next->desc.ddsCaps.dwCaps = (next->desc.ddsCaps.dwCaps & ~roles) | DDSCAPS_FRONTBUFFER;
            if (next->chainNext && next->chainNext != next) next->chainNext->desc.ddsCaps.dwCaps |= DDSCAPS_BACKBUFFER;
        }
        s->desc.ddsCaps.dwCaps = s->attachSavedCaps;
        s->chainNext = nullptr;
        s->DropAttachRef();
        return DD_OK;
    }
    for (size_t k = 0; k < attachRefs.size(); k++) {
        if (attachRefs[k].s != s) continue;
        // Only through the interface pointer it was attached with.
        if (attachRefs[k].slot != slot) return DDERR_SURFACENOTATTACHED;
        attachRefs.erase(attachRefs.begin() + k);
        attached.erase(std::find(attached.begin(), attached.end(), s));
        s->Release(slot);
        return DD_OK;
    }
    // A member of our own complex surface (a mip level, a cube face).
    if (std::find(attached.begin(), attached.end(), s) != attached.end()) return DDERR_CANNOTDETACHSURFACE;
    return DDERR_SURFACENOTATTACHED;
}

// As ddraw.dll: without a surface, everything attached goes, newest first, up to the
// first that can't (a part of the surface's own complex structure).
HRESULT Surface::DeleteAttachedSurface(DWORD flags, Surface* s, int ifaceVersion) {
    if (flags) return DDERR_INVALIDPARAMS;
    if (IsLost() || (s && s->IsLost())) return DDERR_SURFACELOST;
    if (s) return DetachOne(s, SlotFor(ifaceVersion));
    for (const AttachEntry& e : AttachList()) {
        if (e.slot < 0) return DDERR_CANNOTDETACHSURFACE;
        const HRESULT hr = DetachOne(e.s, e.slot);
        if (FAILED(hr)) return hr;
    }
    return DD_OK;
}

HRESULT Surface::EnumAttachedSurfaces(int v, void* ctx, void* callback) {
    if (!callback) return DDERR_INVALIDPARAMS;
    using CB = HRESULT(WINAPI*)(void*, void*, void*);
    std::vector<Surface*> list;
    {
        Guard l;
        if (IsLost()) return DDERR_SURFACELOST;
        for (const AttachEntry& e : AttachList()) list.push_back(e.s);
        for (Surface* s : list) s->AddRef(SlotFor(HandedOut(v)));  // the callback releases it
    }
    size_t i = 0;
    for (; i < list.size(); i++) {
        DDSURFACEDESC2 d2;
        list[i]->GetSurfaceDesc(&d2);
        union {
            DDSURFACEDESC v1;
            DDSURFACEDESC2 v2;
        } out;
        if (v >= 4) {
            out.v1.dwSize = sizeof(DDSURFACEDESC2);
            DescOut(d2, &out);
        } else {
            DescOutEnum(d2, &out.v1);
        }
        const HRESULT r = reinterpret_cast<CB>(callback)(list[i]->Iface(HandedOut(v)), &out, ctx);
        DD2D9_TRACE("EnumAttachedSurfaces %p (v%d): %p (caps 0x%lx) -> callback returned %ld", static_cast<void*>(this),
                    v, static_cast<void*>(list[i]), static_cast<unsigned long>(d2.ddsCaps.dwCaps), static_cast<long>(r));
        if (r == DDENUMRET_CANCEL) {
            i++;
            break;
        }
    }
    for (; i < list.size(); i++) list[i]->Release(SlotFor(HandedOut(v)));  // never handed to the callback
    return DD_OK;
}

// As ddraw.dll: the newest attachment that has every requested bit (at least one).
HRESULT Surface::GetAttachedSurface(const DDSCAPS2& caps, Surface** out, int version) {
    if (IsLost()) return DDERR_SURFACELOST;
    if (version >= 4 && (caps.dwCaps & 0x03000000)) return DDERR_INVALIDCAPS;  // reserved bits
    if (!(caps.dwCaps | caps.dwCaps2 | caps.dwCaps3 | caps.dwCaps4)) return DDERR_NOTFOUND;
    auto has = [](DWORD have, DWORD want) { return (have & want) == want; };
    for (const AttachEntry& e : AttachList()) {
        const DDSCAPS2& c = e.s->desc.ddsCaps;
        if (has(c.dwCaps, caps.dwCaps) && has(c.dwCaps2, caps.dwCaps2) && has(c.dwCaps3, caps.dwCaps3) &&
            has(c.dwCaps4, caps.dwCaps4)) {
            e.s->AddRef(SlotFor(version));
            *out = e.s;
            return DD_OK;
        }
    }
    return DDERR_NOTFOUND;
}

HRESULT Surface::GetCaps(DDSCAPS2* caps) {
    *caps = desc.ddsCaps;
    return DD_OK;
}

// As ddraw.dll (spec: re/spec/surface-flip-key-palette.md): the out pointer is cleared first.
HRESULT Surface::GetClipper(IDirectDrawClipper** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (!clipper) return DDERR_NOCLIPPERATTACHED;
    clipper->AddRef();
    *out = clipper;
    return DD_OK;
}

static bool LiveClipper(const Clipper* c);
bool LivePalette(const Palette* p);

HRESULT Surface::SetClipper(IDirectDrawClipper* c) {
    Clipper* nc = c ? Clipper::From(c) : nullptr;
    if (c && (!nc || !LiveClipper(nc))) return DDERR_INVALIDPARAMS;
    if (!nc && !clipper) return DDERR_NOCLIPPERATTACHED;  // nothing to detach
    if (nc) nc->AddRef();
    if (clipper) clipper->Release();
    clipper = nc;
    MarkDirtyIfVisible();
    return DD_OK;
}

static constexpr DWORD kCkTypes = DDCKEY_DESTBLT | DDCKEY_DESTOVERLAY | DDCKEY_SRCBLT | DDCKEY_SRCOVERLAY;

static int CkIndex(DWORD type) {
    switch (type) {
    case DDCKEY_DESTBLT: return kCkDestBlt;
    case DDCKEY_DESTOVERLAY: return kCkDestOverlay;
    case DDCKEY_SRCBLT: return kCkSrcBlt;
    case DDCKEY_SRCOVERLAY: return kCkSrcOverlay;
    default: return -1;
    }
}

static bool OneBit(DWORD v) { return v && !(v & (v - 1)); }

// Checks in ddraw.dll's order (spec: re/spec/surface-flip-key-palette.md, section 4).
HRESULT Surface::GetColorKey(DWORD flags, DDCOLORKEY* key) {
    if (IsLost()) return DDERR_SURFACELOST;
    if (desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) return DDERR_NOCOLORKEY;
    if (flags & ~(kCkTypes | DDCKEY_COLORSPACE)) return DDERR_INVALIDPARAMS;
    const DWORD type = flags & kCkTypes;
    if (type && !OneBit(type)) return DDERR_INVALIDPARAMS;  // two types at once
    if ((type & DDCKEY_SRCOVERLAY) && !(desc.ddsCaps.dwCaps & DDSCAPS_OVERLAY)) return DDERR_INVALIDPARAMS;
    if (!type || !key) return DDERR_INVALIDPARAMS;
    const int i = CkIndex(type);
    if (!hasCk[i]) return DDERR_NOCOLORKEY;
    *key = ck[i];
    return DD_OK;
}

// Checks in ddraw.dll's order (spec section 3). dd2d9 reports colour keys for blits only
// (DDCKEYCAPS_SRCBLT | DDCKEYCAPS_DESTBLT): overlay keys and key ranges are refused as on
// hardware without them.
HRESULT Surface::SetColorKey(DWORD flags, DDCOLORKEY* key, int version) {
    if (IsLost()) return DDERR_SURFACELOST;
    if (desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) return DDERR_INVALIDSURFACETYPE;
    if (version == 7 && (desc.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL)) return DDERR_NOTONMIPMAPSUBLEVEL;
    if (flags & ~(kCkTypes | DDCKEY_COLORSPACE)) return DDERR_INVALIDPARAMS;
    if (!key) {
        // Removes the key of the first type named, in ddraw.dll's order; none named: nothing.
        if (flags & DDCKEY_DESTOVERLAY) return DDERR_NOOVERLAYHW;
        if (flags & DDCKEY_DESTBLT) hasCk[kCkDestBlt] = false;
        else if (flags & DDCKEY_SRCOVERLAY) hasCk[kCkSrcOverlay] = false;
        else if (flags & DDCKEY_SRCBLT) hasCk[kCkSrcBlt] = false;
        return DD_OK;
    }
    const DWORD type = flags & kCkTypes;
    if (!OneBit(type)) return DDERR_INVALIDPARAMS;  // none, or two at once
    // Without DDCKEY_COLORSPACE the key is the low value.
    const DWORD low = key->dwColorSpaceLowValue;
    const DWORD high = (flags & DDCKEY_COLORSPACE) ? key->dwColorSpaceHighValue : low;
    if (type & (DDCKEY_SRCOVERLAY | DDCKEY_DESTOVERLAY)) return DDERR_NOCOLORKEYHW;
    if (low != high) return DDERR_NOCOLORKEYHW;
    const int i = CkIndex(type);
    ck[i].dwColorSpaceLowValue = ck[i].dwColorSpaceHighValue = low;
    hasCk[i] = true;
    return DD_OK;
}

const Palette* Surface::ShownPalette() const {
    if (palette) return palette;
    for (Surface* s : flipChain)
        if (s->palette) return s->palette;
    return nullptr;
}

const PALETTEENTRY* Surface::EffectivePalette() const {
    if (palette) return palette->entries;
    if (const Palette* p = dd->primary ? dd->primary->ShownPalette() : nullptr) return p->entries;
    return dd->defaultPalette;
}

// Checks in ddraw.dll's order (spec: re/spec/surface-lock-dc.md, sections 3 and 4). A DC
// out counts as one more lock: the surface can't be locked meanwhile, but a locked one can
// still give a DC.
HRESULT Surface::GetDC(HDC* out) {
    Guard l;
    if (CantDraw()) return DDERR_SURFACELOST;
    if (dcOut) return DDERR_DCALREADYCREATED;  // *out left alone, as ddraw.dll
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    CpuAccess(true);  // what Direct3D drew is read back first
    HDC dc = buf->GetDC();
    if (!dc) return DDERR_CANTCREATEDC;
    if (buf->fmt.pal8) {
        const Palette* p = palette ? palette : (dd->primary ? dd->primary->ShownPalette() : nullptr);
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
    dcCounted = true;
    lockCount++;
    *out = dc;
    DD2D9_TRACE("GetDC %p%s", static_cast<void*>(this), isPrimary ? " (primary)" : "");
    return DD_OK;
}

// IDirectDrawSurface 1 and 2 take any DC (even null) and release the surface's own.
HRESULT Surface::ReleaseDC(HDC dc, int version) {
    Guard l;
    if (!dcOut) return DDERR_NODC;
    if (dc != dcOut && version >= 3) return DDERR_INVALIDPARAMS;
    GdiFlush();
    buf->EndDC();
    dcOut = nullptr;
    if (dcCounted) lockCount--;
    dcCounted = false;
    DD2D9_TRACE("ReleaseDC %p%s", static_cast<void*>(this), isPrimary ? " (primary)" : "");
    BumpUniqueness();
    MarkDirtyIfVisible();
    return DD_OK;
}

HRESULT Surface::GetPalette(IDirectDrawPalette** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;  // first, as ddraw.dll
    if (IsLost()) return DDERR_SURFACELOST;
    if (!palette) return DDERR_NOPALETTEATTACHED;
    palette->AddRef();
    *out = palette;
    return DD_OK;
}

// Checks in ddraw.dll's order (spec: re/spec/surface-flip-key-palette.md, section 5). The
// palette goes to this surface only, not the rest of its flip chain.
HRESULT Surface::SetPalette(IDirectDrawPalette* p, int version) {
    Palette* np = p ? Palette::From(p) : nullptr;
    if (p && (!np || !LivePalette(np))) return DDERR_INVALIDPARAMS;
    if (desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) return DDERR_INVALIDSURFACETYPE;
    if (version == 7 && (desc.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL)) return DDERR_NOTONMIPMAPSUBLEVEL;
    if (IsLost()) return DDERR_SURFACELOST;
    if (np && (np->caps & DDPCAPS_ALPHA) && !(desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE)) return DDERR_INVALIDSURFACETYPE;
    if (isPrimary && dd->OtherExclusive()) return DDERR_NOEXCLUSIVEMODE;
    if (!np && !palette) return DDERR_NOPALETTEATTACHED;
    // An 8-bit palette (256 entries of colours) for an 8-bit surface only.
    if (np && (!buf->fmt.pal8 || !(np->caps & DDPCAPS_8BIT) || (np->caps & DDPCAPS_8BITENTRIES)))
        return DDERR_INVALIDPIXELFORMAT;
    DD2D9_TRACE("SetPalette %p%s <- %p", static_cast<void*>(this), isPrimary ? " (primary)" : "", static_cast<void*>(np));
    if (np) np->AddRef();
    if (palette) palette->Release();
    palette = np;
    BumpUniqueness();
    MarkDirtyIfVisible();
    // A back buffer's palette is shown while the front has none (see ShownPalette).
    if (!isPrimary && dd->primary && !dd->primary->palette) dd->MarkDirty();
    return DD_OK;
}

// The primary chain has no pixel format of its own: ddraw.dll reports the display's,
// which differs from the memory's only once a mode change has made the chain lost.
Format Surface::ReportedFormat() const {
    if (!isPrimary && !(root && root->isPrimary)) return buf->fmt;
    DWORD w, h, bpp;
    dd->DisplayMode(&w, &h, &bpp);
    return Format::ForDisplayBpp(bpp);
}

HRESULT Surface::GetPixelFormat(DDPIXELFORMAT* pf) {
    if (!pf || pf->dwSize != sizeof(DDPIXELFORMAT)) return DDERR_INVALIDPARAMS;
    ReportedFormat().ToDDPF(pf);
    return DD_OK;
}

// Built the way ddraw.dll builds it (FUN_73733184), not from the creation flags: no
// alpha depth or refresh rate, overlay keys never (no overlay data here), no FVF (vertex
// buffers only; dwFVF shares its place with the pixel format).
HRESULT Surface::GetSurfaceDesc(DDSURFACEDESC2* d) {
    *d = {};
    d->dwSize = sizeof(DDSURFACEDESC2);
    d->dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d->ddsCaps = desc.ddsCaps;
    d->dwWidth = DWORD(buf->w);
    d->dwHeight = DWORD(buf->h);
    ReportedFormat().ToDDPF(&d->ddpfPixelFormat);
    if (buf->fmt.BlockCompressed()) {
        d->dwFlags |= DDSD_LINEARSIZE;
        d->dwLinearSize = DWORD(buf->pitch) * ((buf->h + 3) & ~3);
    } else {
        d->dwFlags |= DDSD_PITCH;
        d->lPitch = buf->pitch;
    }
    if (hasCk[kCkDestBlt]) {
        d->dwFlags |= DDSD_CKDESTBLT;
        d->ddckCKDestBlt = ck[kCkDestBlt];
    }
    if (hasCk[kCkSrcBlt]) {
        d->dwFlags |= DDSD_CKSRCBLT;
        d->ddckCKSrcBlt = ck[kCkSrcBlt];
    }
    // The back buffer count only on a created chain's front.
    if (desc.dwFlags & DDSD_BACKBUFFERCOUNT) {
        d->dwFlags |= DDSD_BACKBUFFERCOUNT;
        d->dwBackBufferCount = desc.dwBackBufferCount;
    }
    if (desc.ddsCaps.dwCaps & DDSCAPS_MIPMAP) {
        d->dwFlags |= DDSD_MIPMAPCOUNT;
        d->dwMipMapCount = desc.dwMipMapCount;
    }
    if (desc.dwFlags & DDSD_TEXTURESTAGE) {
        d->dwFlags |= DDSD_TEXTURESTAGE;
        d->dwTextureStage = desc.dwTextureStage;
    }
    return DD_OK;
}

HRESULT Surface::SetSurfaceDesc(const DDSURFACEDESC2& d, DWORD flags) {
    // Points a system-memory surface at client memory (DDSD_LPSURFACE), optionally with a new
    // size, pitch and pixel format. Checks in ddraw.dll's order (spec: re/spec/surface-other.md);
    // the wrapper checked the structure's size.
    if (flags) return DDERR_INVALIDPARAMS;
    if (IsLost()) return DDERR_SURFACELOST;
    if (lockCount || dcOut) return DDERR_SURFACEBUSY;
    const DWORD caps = desc.ddsCaps.dwCaps, caps2 = desc.ddsCaps.dwCaps2;
    if (!sysmemRequested ||
        (caps & (DDSCAPS_ALLOCONLOAD | DDSCAPS_PRIMARYSURFACE | DDSCAPS_PRIMARYSURFACELEFT | DDSCAPS_OVERLAY)) ||
        (caps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)) || (root && root->isPrimary))
        return DDERR_INVALIDSURFACETYPE;
    if (d.dwFlags & ~(DDSD_LPSURFACE | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_CAPS))
        return DDERR_INVALIDPARAMS;
    if ((d.dwFlags & DDSD_WIDTH) && !d.dwWidth) return DDERR_INVALIDPARAMS;
    if ((d.dwFlags & DDSD_HEIGHT) && !d.dwHeight) return DDERR_INVALIDPARAMS;
    if ((d.dwFlags & DDSD_PITCH) && (d.lPitch <= 0 || (d.lPitch & 3))) return DDERR_INVALIDPARAMS;
    // A new pixel format only for a surface that was created with one.
    if ((d.dwFlags & DDSD_PIXELFORMAT) && !ownFormat) return DDERR_INVALIDPARAMS;
    // Width and pitch go together.
    if (!(d.dwFlags & DDSD_WIDTH) != !(d.dwFlags & DDSD_PITCH)) return DDERR_INVALIDPARAMS;
    if (!(d.dwFlags & DDSD_LPSURFACE) || !d.lpSurface) return DDERR_INVALIDPARAMS;
    if ((d.dwFlags & DDSD_CAPS) && (d.ddsCaps.dwCaps || d.ddsCaps.dwCaps3 || d.ddsCaps.dwCaps4))
        return DDERR_INVALIDCAPS;
    const int w = (d.dwFlags & DDSD_WIDTH) ? int(d.dwWidth) : buf->w;
    const int h = (d.dwFlags & DDSD_HEIGHT) ? int(d.dwHeight) : buf->h;
    const LONG pitch = (d.dwFlags & DDSD_PITCH) ? d.lPitch : buf->pitch;
    if (w <= 0 || h <= 0) return DDERR_INVALIDPARAMS;  // above INT_MAX
    Format f = buf->fmt;
    if ((d.dwFlags & DDSD_PIXELFORMAT) && !Format::FromDDPF(d.ddpfPixelFormat, &f)) return DDERR_INVALIDPIXELFORMAT;
    const bool reshaped = w != buf->w || h != buf->h || !(f == buf->fmt);
    // dd2d9's own limit: Flip swaps memory between the members of a flip ring, so a
    // member can't change shape (ddraw.dll allows it outside the primary's chain).
    if (reshaped && chainNext) return DDERR_INVALIDPARAMS;
    Buffer* nb = Buffer::CreateExternal(d.lpSurface, w, h, pitch, f);
    if (!nb) return DDERR_OUTOFMEMORY;
    ReplaceBuffer(nb);
    desc.dwWidth = DWORD(w);
    desc.dwHeight = DWORD(h);
    uniqueness = 0;  // as ddraw.dll: no longer counted
    return DD_OK;
}

void Surface::ReplaceBuffer(Buffer* b) {
    buf->Release();
    buf = b;
}

// Fills the caller's description for Lock in place, as ddraw.dll does: what Lock doesn't
// set keeps the caller's values; dwSize stays as given.
void Surface::FillLockDesc(void* outDesc, uint8_t* bits) const {
    auto* d = static_cast<DDSURFACEDESC2*>(outDesc);  // the two structures share their layout up to ddsCaps
    const bool v2 = d->dwSize >= sizeof(DDSURFACEDESC2);  // as ddraw.dll: the DirectX 7 fields need the whole structure
    const DWORD caps = desc.ddsCaps.dwCaps;
    DWORD flags = DDSD_CAPS;
    d->ddsCaps.dwCaps = caps;
    if (v2) {
        d->ddsCaps.dwCaps2 = desc.ddsCaps.dwCaps2;
        d->ddsCaps.dwCaps3 = desc.ddsCaps.dwCaps3;
        d->ddsCaps.dwCaps4 = desc.ddsCaps.dwCaps4;
        d->dwTextureStage = 0;
        if (desc.dwFlags & DDSD_TEXTURESTAGE) {
            d->dwTextureStage = desc.dwTextureStage;
            flags = DDSD_CAPS | DDSD_TEXTURESTAGE;
        }
    }
    d->lpSurface = bits;
    if (hasCk[kCkDestBlt]) {
        flags |= DDSD_CKDESTBLT;
        d->ddckCKDestBlt = ck[kCkDestBlt];
    }
    if (hasCk[kCkSrcBlt]) {
        flags |= DDSD_CKSRCBLT;
        d->ddckCKSrcBlt = ck[kCkSrcBlt];
    }
    if (!root && (caps & DDSCAPS_FRONTBUFFER) && (desc.dwFlags & DDSD_BACKBUFFERCOUNT)) {
        flags |= DDSD_BACKBUFFERCOUNT;
        d->dwBackBufferCount = desc.dwBackBufferCount;
    }
    if (caps & DDSCAPS_MIPMAP) {
        flags |= DDSD_MIPMAPCOUNT;
        d->dwMipMapCount = desc.dwMipMapCount;
    }
    d->dwWidth = DWORD(buf->w);
    d->dwHeight = DWORD(buf->h);
    if (buf->fmt.BlockCompressed()) {
        flags |= DDSD_WIDTH | DDSD_HEIGHT | DDSD_LINEARSIZE;
        d->dwLinearSize = DWORD(buf->pitch) * ((buf->h + 3) & ~3);
    } else {
        flags |= DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH;
        d->lPitch = buf->pitch;
    }
    buf->fmt.ToDDPF(&d->ddpfPixelFormat);
    if (buf->fmt.zbuf && !v2) {
        // The old structure describes a Z buffer by its depth.
        flags |= DDSD_ZBUFFERBITDEPTH;
        d->dwMipMapCount = d->ddpfPixelFormat.dwZBufferBitDepth;  // dwZBufferBitDepth's slot
    } else {
        flags |= DDSD_PIXELFORMAT;
    }
    d->ddckCKDestOverlay = {};  // no overlays here
    d->ddckCKSrcOverlay = {};
    d->dwFlags = flags;
}

// Checks in ddraw.dll's order (spec: re/spec/surface-lock-dc.md, section 1). Rectangle
// locks nest as long as they don't overlap; a whole-surface lock (or a DC out) excludes
// the rest. System-memory textures made through IDirectDraw7 take ddraw.dll's fast path:
// any descriptor size, no checks, no nesting.
HRESULT Surface::Lock(RECT* r, void* outDesc, DWORD flags) {
    Guard l;
    // Lost, as in ddraw.dll, except while the game is in the background after Alt+Tab
    // only: dd2d9 still has the pixels, so the lock works (a deliberate difference).
    // Captain Claw locks its primary there, ignores the failure and writes through the
    // null pointer.
    const bool background = dd->DevState() == DDraw::kDevLost && dd->FocusLossOnly() && (!lost || drawableLost);
    if (CantDraw() && !background) return DDERR_SURFACELOST;
    auto* d = static_cast<DDSURFACEDESC2*>(outDesc);
    const DWORD caps = desc.ddsCaps.dwCaps;
    // XP's fast path for DirectDraw 7 system-memory textures (spec section 1.4); managed
    // textures, kept in system memory too, get it as well (Wine's test on Windows).
    const bool managed = (desc.ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)) != 0;
    const bool fast = ddVersion == 7 && (sysmemRequested || managed) && (caps & DDSCAPS_TEXTURE);
    RECT area = {0, 0, buf->w, buf->h};
    if (fast) {
        if (!d) return DDERR_INVALIDPARAMS;
        if (lockCount) return DDERR_SURFACEBUSY;
        if (r && !buf->fmt.BlockCompressed()) {  // unchecked by ddraw.dll; kept inside the surface here
            area = {std::clamp<LONG>(r->left, 0, buf->w), std::clamp<LONG>(r->top, 0, buf->h),
                    std::clamp<LONG>(r->right, 0, buf->w), std::clamp<LONG>(r->bottom, 0, buf->h)};
            area.right = std::max(area.right, area.left);
            area.bottom = std::max(area.bottom, area.top);
        }
        fastLocked = true;
        wholeReadOnly = (flags & DDLOCK_READONLY) != 0;
    } else {
        if (flags & ~0x17831u) return DDERR_INVALIDPARAMS;
        if (!d || !DescSizeOk(d)) return DDERR_INVALIDPARAMS;
        d->lpSurface = nullptr;  // from here on, a failure leaves it null
        if (r) {
            if (r->left < 0 || r->top < 0 || r->right < r->left || r->bottom < r->top || r->right > buf->w ||
                r->bottom > buf->h)
                return DDERR_INVALIDPARAMS;  // an empty rectangle is fine
            if (!buf->fmt.BlockCompressed()) area = *r;
            RECT x;
            for (const LockRec& k : rectLocks)
                if (IntersectRect(&x, &k.r, r)) return DDERR_SURFACEBUSY;
            if (rectLocks.empty() && lockCount) return DDERR_SURFACEBUSY;  // locked whole, or a DC out
        } else if (lockCount) {
            return DDERR_SURFACEBUSY;
        }
    }
    GdiFlush();
    CpuAccess(!(flags & DDLOCK_READONLY));  // what Direct3D drew is read back first
    uint8_t* p = buf->bits + area.top * buf->pitch + area.left * LONG(buf->fmt.Bytes());
    if (fast) {
    } else if (r) {
        rectLocks.push_back({*r, p, (flags & DDLOCK_READONLY) != 0});
    } else {
        wholeLocked = true;
        wholeReadOnly = (flags & DDLOCK_READONLY) != 0;
    }
    FillLockDesc(d, p);
    // While locked, the presenter treats this area as changing (see Presenter::Loop).
    if (lockCount == 0 || (dcOut && lockCount == 1)) {
        lockArea = area;
    } else {
        lockArea = {std::min(lockArea.left, area.left), std::min(lockArea.top, area.top),
                    std::max(lockArea.right, area.right), std::max(lockArea.bottom, area.bottom)};
    }
    lockCount++;
    DD2D9_TRACE("Lock %p%s rect=%s%ld,%ld-%ld,%ld flags=0x%lx -> %p pitch=%ld", static_cast<void*>(this),
               isPrimary ? " (primary)" : "", r ? "" : "full ", area.left, area.top, area.right, area.bottom, flags,
               static_cast<void*>(p), buf->pitch);
    return DD_OK;
}

// Checks in ddraw.dll's order (spec section 2): with rectangle locks out, the one to undo
// is named by its rectangle (4, 7) or pointer (1-3); no name, only if it's the one lock.
HRESULT Surface::Unlock(void* pointer, const RECT* rect, int version) {
    Guard l;
    DD2D9_TRACE("Unlock %p%s", static_cast<void*>(this), isPrimary ? " (primary)" : "");
    bool readOnly = false;
    RECT area = lockArea;
    if (fastLocked) {
        fastLocked = false;
        readOnly = wholeReadOnly;
        lockCount--;
    } else {
        if (lockCount <= 0) return DDERR_NOTLOCKED;
        const bool named = pointer || rect;
        if (lockCount > 1 && !named) return DDERR_INVALIDRECT;
        if (dcOut && version >= 3) return DDERR_GENERIC;
        if (rectLocks.empty()) {
            if (wholeLocked) {  // whatever is named
                wholeLocked = false;
                readOnly = wholeReadOnly;
            } else if (dcCounted) {  // only a DC out (IDirectDrawSurface 1/2): its share goes
                dcCounted = false;
            } else {
                return DDERR_NOTLOCKED;
            }
        } else {
            size_t k = rectLocks.size();
            if (!named) {
                k = 0;  // the one lock (checked above)
            } else {
                for (size_t i = 0; i < rectLocks.size(); i++) {
                    const LockRec& lr = rectLocks[i];
                    if (version <= 3 ? lr.ptr == pointer : (rect && EqualRect(&lr.r, rect))) {
                        k = i;
                        break;
                    }
                }
            }
            if (k == rectLocks.size()) return DDERR_NOTLOCKED;
            area = rectLocks[k].r;
            readOnly = rectLocks[k].readOnly;
            rectLocks.erase(rectLocks.begin() + k);
        }
        lockCount--;
    }
    // The area still locked, for the presenter.
    lockArea = wholeLocked || fastLocked ? RECT{0, 0, buf->w, buf->h} : RECT{0, 0, 0, 0};
    for (const LockRec& k : rectLocks) UnionRect(&lockArea, &lockArea, &k.r);
    if (!readOnly) {
        BumpUniqueness();
        MarkDirtyIfVisible(&area);
    }
    return CantDraw() ? DDERR_SURFACELOST : DD_OK;  // unlocked either way, as ddraw.dll
}

// PageLock and PageUnlock: nothing to pin here; ddraw.dll checks only these.
HRESULT Surface::PageLock(DWORD flags) {
    if (flags) return DDERR_INVALIDPARAMS;
    Guard l;
    return IsLost() ? DDERR_SURFACELOST : DD_OK;
}

HRESULT Surface::GetDDInterface(void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (!dd->alive) return DDERR_INVALIDOBJECT;  // the DirectDraw object was released
    *out = dd->Iface(ddVersion);  // the interface it was created through
    dd->AddRef(ddVersion);
    return DD_OK;
}

// As ddraw.dll (spec: re/spec/surface-other.md), but for one thing: the new object is
// referenced before the old entry goes, so setting the same one again keeps it alive.
HRESULT Surface::SetPrivateData(REFGUID tag, void* data, DWORD size, DWORD flags) {
    if (!size || (flags & ~(DDSPD_IUNKNOWNPOINTER | DDSPD_VOLATILE))) return DDERR_INVALIDPARAMS;
    if (!data || ((flags & DDSPD_IUNKNOWNPOINTER) && size != sizeof(IUnknown*))) {
        FreePrivateData(tag);  // ddraw.dll has freed the old entry by then
        return DDERR_INVALIDPARAMS;
    }
    PrivateData p = {tag, {}, nullptr, (flags & DDSPD_VOLATILE) != 0, uniqueness};
    if (flags & DDSPD_IUNKNOWNPOINTER) {
        // The pointer the game passes, not what it points to (a copy of the pointer).
        p.unk = static_cast<IUnknown*>(data);
        p.unk->AddRef();
    } else {
        p.data.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
    }
    FreePrivateData(tag);
    priv_.push_back(std::move(p));
    return DD_OK;
}

HRESULT Surface::GetPrivateData(REFGUID tag, void* data, DWORD* size) {
    for (auto& p : priv_) {
        if (!IsEqualGUID(p.tag, tag)) continue;
        if (!size) return DDERR_INVALIDPARAMS;
        // Volatile data is gone once the contents change (or aren't counted).
        if (p.isVolatile && (!uniqueness || uniqueness != p.stamp)) {
            *size = 0;
            return DDERR_EXPIRED;
        }
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
    BumpUniqueness();
    MarkDirtyIfVisible(&c);
}

void Surface::CpuAccess(bool write) {
    if (buf->gpu) GpuCpuAccess(this, write);  // reads back what Direct3D drew, if newer
    else if (write) buf->gpuStale = true;
    if (write) buf->cpuSerial++;              // a texture's GPU copy is made from its levels' buffers
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
        // Checks in ddraw.dll's order (spec: re/spec/surface-flip-key-palette.md, section 1).
        // No overlays or stereo surfaces here: DDFLIP_EVEN/ODD and DDFLIP_STEREO never apply.
        const DWORD kIntervals = 0x07000000;
        if ((flags & ~(0x3Fu | kIntervals)) || (flags & (DDFLIP_EVEN | DDFLIP_ODD | DDFLIP_STEREO)) ||
            ((flags & DDFLIP_NOVSYNC) && (flags & kIntervals)))
            return DDERR_INVALIDPARAMS;
        if (IsLost() || (target && target->IsLost(false))) return DDERR_SURFACELOST;
        // Called on the front buffer of a chain (created, or put together with AddAttachedSurface).
        const DWORD caps = desc.ddsCaps.dwCaps;
        if (!(caps & DDSCAPS_FLIP) || !(caps & DDSCAPS_FRONTBUFFER)) return DDERR_NOTFLIPPABLE;
        if (lockCount || dcOut) return DDERR_SURFACEBUSY;
        if (isPrimary && !dd->Exclusive()) return DDERR_NOEXCLUSIVEMODE;
        if (!chainNext) return DDERR_NOTFLIPPABLE;
        DD2D9_TRACE("Flip %p target=%p flags=0x%lx", static_cast<void*>(this), static_cast<void*>(target), flags);
        // The chain's order is the ring's (back buffers attached later sit right after the front).
        std::vector<Surface*> chain{this};
        for (Surface* m = chainNext; m && m != this && chain.size() < 4096; m = m->chainNext) chain.push_back(m);
        // Only the buffers that change hands have to be free: with a target, the others may be locked.
        size_t k = 0;
        for (size_t i = 1; i < chain.size(); i++) {
            Surface* s = chain[i];
            if (s->IsLost(false)) return DDERR_SURFACELOST;
            if ((!target || s == target) && (s->lockCount || s->dcOut)) return DDERR_SURFACEBUSY;
            if (s == target) k = i;
        }
        if (target && !k) return DDERR_NOTFLIPPABLE;  // the front buffer, or not in the chain
        if (target) {
            std::swap(chain[0]->buf, chain[k]->buf);
        } else {
            Buffer* first = chain[0]->buf;
            for (size_t i = 0; i + 1 < chain.size(); i++) chain[i]->buf = chain[i + 1]->buf;
            chain.back()->buf = first;
        }
        // As ddraw.dll: only the front and the buffer it swapped with count as changed.
        chain[0]->BumpUniqueness();
        chain[target ? k : 1]->BumpUniqueness();
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
    if (isPrimary) LimitFrame();
    return DD_OK;
}

// No flip is ever pending here: valid calls succeed (spec section 2).
HRESULT Surface::GetFlipStatus(DWORD flags) {
    if (flags != DDGFS_CANFLIP && flags != DDGFS_ISFLIPDONE) return DDERR_INVALIDPARAMS;
    if (IsLost()) return DDERR_SURFACELOST;
    return DD_OK;
}

// ================================================================ Palette

// As for clippers (see LiveClipper): a released or made-up palette pointer gets 0 from
// AddRef/Release, as in ddraw.dll, and is refused by SetPalette, instead of a crash.
namespace {
std::vector<const Palette*>* g_palettes;  // live palettes (g_lock)
const void* g_paletteVtbl;                // the genuine vtable
}

bool LivePalette(const Palette* p) {
    Guard l;
    return g_palettes && std::find(g_palettes->begin(), g_palettes->end(), p) != g_palettes->end();
}

Palette::Palette(DDraw* dd, DWORD c, const PALETTEENTRY* e) : caps(c), dd_(dd) {
    WritableVtbl(this);
    if (dd_) dd_->IntAddRef();
    {
        Guard l;
        if (!g_palettes) g_palettes = new std::vector<const Palette*>;
        g_palettes->push_back(this);
        g_paletteVtbl = *reinterpret_cast<void* const*>(this);
    }
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
        // Only the colors: ddraw.dll leaves the game's peFlags.
        entries[0].peRed = entries[0].peGreen = entries[0].peBlue = 0;
        entries[255].peRed = entries[255].peGreen = entries[255].peBlue = 255;
    }
}

// Released more often than referenced (the surfaces it's attached to hold references):
// ddraw.dll detaches it, so GetPalette then says DDERR_NOPALETTEATTACHED instead of
// handing out freed memory. g_lock held.
Palette::~Palette() {
    g_palettes->erase(std::find(g_palettes->begin(), g_palettes->end(), this));
    if (dd_) {
        for (Surface* s : dd_->surfaces)
            if (s->palette == this) s->palette = nullptr;
        dd_->IntRelease();
    }
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

ULONG Palette::AddRef() {
    if (!LivePalette(this)) return 0;
    return ULONG(InterlockedIncrement(&ref_));
}

ULONG Palette::Release() {
    DDraw* owner = nullptr;
    int ownerRef = 0;
    LONG r;
    {
        Guard l;  // the check and the decrement go together
        if (!LivePalette(this) || *reinterpret_cast<void* const*>(this) != g_paletteVtbl) return 0;
        r = InterlockedDecrement(&ref_);
        if (r == 0) {
            owner = dd_;
            ownerRef = ddRef;
            if (owner) owner->IntAddRef();  // see Surface::Release
            delete this;
        }
    }
    if (ownerRef) owner->Release(ownerRef);
    if (owner) owner->IntRelease();
    return ULONG(r < 0 ? 0 : r);
}

HRESULT Palette::GetCaps(LPDWORD c) {
    if (!c) return DDERR_INVALIDPARAMS;
    Guard l;
    // As ddraw.dll: the size and entry layout (not DDPCAPS_ALPHA or the creation-only
    // flags), and PRIMARYSURFACE while the palette is the primary surface's.
    *c = caps & (DDPCAPS_1BIT | DDPCAPS_2BIT | DDPCAPS_4BIT | DDPCAPS_8BIT | DDPCAPS_8BITENTRIES | DDPCAPS_ALLOW256);
    if (dd_ && dd_->primary && dd_->primary->palette == this) *c |= DDPCAPS_PRIMARYSURFACE;
    return DD_OK;
}

HRESULT Palette::GetEntries(DWORD flags, DWORD base, DWORD count, LPPALETTEENTRY out) {
    const DWORD n = DWORD(Count());
    if (flags || !out || !count || base >= n || count > n - base) return DDERR_INVALIDPARAMS;
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

HRESULT Palette::SetEntries(DWORD flags, DWORD start, DWORD count, LPPALETTEENTRY in) {
    const DWORD n = DWORD(Count());
    if (flags || !in || !count || start >= n || count > n - start) return DDERR_INVALIDPARAMS;
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
               dd_ && dd_->primary && dd_->primary->ShownPalette() == this ? " (primary)" : "");
    if (dd_ && dd_->primary && dd_->primary->ShownPalette() == this) dd_->MarkDirty();
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
    WritableVtbl(this);
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

// A clipper the class factory made answers only Initialize until it gets one.
static HRESULT ClipperReady(const Clipper* c) {
    if (!LiveClipper(c)) return DDERR_INVALIDPARAMS;
    return c->initialized ? DD_OK : DDERR_NOTINITIALIZED;
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
    DDraw* owner = nullptr;
    int ownerRef = 0;
    LONG r;
    {
        Guard l;  // the check and the decrement go together
        // Also a no-op with the clipper's vtable overwritten (as ddraw.dll).
        if (!LiveClipper(this) || *reinterpret_cast<void* const*>(this) != g_clipperVtbl) return 0;
        r = InterlockedDecrement(&ref_);
        if (r == 0) {
            owner = dd_;
            ownerRef = ddRef;
            if (owner) owner->IntAddRef();  // see Surface::Release
            delete this;
        }
    }
    // The DirectDraw interface it referenced (made through IDirectDraw4/7): released
    // last and without g_lock, as it may tear the object down.
    if (ownerRef) owner->Release(ownerRef);
    if (owner) owner->IntRelease();
    return ULONG(r < 0 ? 0 : r);
}

// The rectangles in `in` that meet `clip`, clipped to it (ddraw.dll's FUN_737364ff).
static std::vector<RECT> ClipRects(const RECT* in, size_t n, const RECT& clip) {
    std::vector<RECT> out;
    for (size_t i = 0; i < n; i++) {
        RECT r;
        if (IntersectRect(&r, &in[i], &clip)) out.push_back(r);
    }
    return out;
}

// As ddraw.dll (spec: re/spec/palette-clipper-exports.md): with a window, the part of
// its client area that is visible on the screen, in screen coordinates; otherwise the
// list SetClipList gave. Either clipped to `rect`.
HRESULT Clipper::GetClipList(LPRECT rect, LPRGNDATA data, LPDWORD size) {
    if (HRESULT hr = ClipperReady(this); FAILED(hr)) return hr;
    if (data) data->rdh.nCount = 0;  // ddraw.dll's first step, whatever follows
    Guard l;
    std::vector<uint8_t> list;
    if (hwnd) {
        RECT cr;
        if (!GetClientRect(hwnd, &cr)) return E_FAIL;  // the window is gone
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        std::vector<RECT> rects;
        HRGN rgn = CreateRectRgn(0, 0, 0, 0);
        HDC dc = GetDC(hwnd);
        // The window's visible region (SYSRGN is in screen coordinates).
        if (dc && rgn && GetRandomRgn(dc, rgn, SYSRGN) == 1) {
            const DWORD bytes = GetRegionData(rgn, 0, nullptr);
            std::vector<uint8_t> raw(bytes);
            if (bytes && GetRegionData(rgn, bytes, reinterpret_cast<RGNDATA*>(raw.data()))) {
                const auto* rd = reinterpret_cast<const RGNDATA*>(raw.data());
                rects = ClipRects(reinterpret_cast<const RECT*>(rd->Buffer), rd->rdh.nCount, cr);
            }
        } else {
            // No region to be had: the client area on the screen.
            RECT screen = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)};
            screen.right = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
            screen.bottom = screen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
            rects = ClipRects(&cr, 1, screen);
        }
        if (dc) ReleaseDC(hwnd, dc);
        if (rgn) DeleteObject(rgn);
        if (rect) rects = ClipRects(rects.data(), rects.size(), *rect);
        list.resize(sizeof(RGNDATAHEADER) + rects.size() * sizeof(RECT));
        auto* rd = reinterpret_cast<RGNDATA*>(list.data());
        rd->rdh.dwSize = sizeof(RGNDATAHEADER);
        rd->rdh.iType = RDH_RECTANGLES;
        rd->rdh.nCount = DWORD(rects.size());
        rd->rdh.nRgnSize = DWORD(rects.size() * sizeof(RECT));
        SetRectEmpty(&rd->rdh.rcBound);
        for (const RECT& r : rects) UnionRect(&rd->rdh.rcBound, &rd->rdh.rcBound, &r);
        if (!rects.empty()) memcpy(rd->Buffer, rects.data(), rects.size() * sizeof(RECT));
    } else {
        if (clipList_.empty()) return DDERR_NOCLIPLIST;
        list = clipList_;
        if (rect) {
            // Clipped to `rect`; the header's bounds and size stay as they were.
            auto* rd = reinterpret_cast<RGNDATA*>(list.data());
            const size_t n = std::min<size_t>(rd->rdh.nCount, (list.size() - sizeof(RGNDATAHEADER)) / sizeof(RECT));
            const std::vector<RECT> rects = ClipRects(reinterpret_cast<const RECT*>(rd->Buffer), n, *rect);
            rd->rdh.nCount = DWORD(rects.size());
            if (!rects.empty()) memcpy(rd->Buffer, rects.data(), rects.size() * sizeof(RECT));
        }
    }
    if (!size) return DDERR_INVALIDPARAMS;
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
    if (HRESULT hr = ClipperReady(this); FAILED(hr)) return hr;
    if (!out) return DDERR_INVALIDPARAMS;
    *out = hwnd;
    return DD_OK;
}

// Only a clipper from the class factory needs it: it then belongs to that DirectDraw
// object (without a reference, as in ddraw.dll).
HRESULT Clipper::Initialize(LPDIRECTDRAW dd, DWORD flags) {
    if (!LiveClipper(this)) return DDERR_INVALIDPARAMS;
    Guard l;
    if (initialized) return DDERR_ALREADYINITIALIZED;
    if (flags) return DDERR_INVALIDPARAMS;
    if (DDraw* d = DDraw::From(dd)) {
        dd_ = d;
        d->IntAddRef();  // keeps dd2d9's object allocated, not a game reference
    }
    initialized = true;
    return DD_OK;
}

// ddraw.dll keeps no track of changes: always "changed".
HRESULT Clipper::IsClipListChanged(WINBOOL* changed) {
    if (HRESULT hr = ClipperReady(this); FAILED(hr)) return hr;
    if (!changed) return DDERR_INVALIDPARAMS;
    *changed = TRUE;
    return DD_OK;
}

// Checks in ddraw.dll's order.
HRESULT Clipper::SetClipList(LPRGNDATA data, DWORD flags) {
    if (HRESULT hr = ClipperReady(this); FAILED(hr)) return hr;
    if (flags) return DDERR_INVALIDPARAMS;
    if (data) {
        if (!data->rdh.nCount) return DDERR_INVALIDCLIPLIST;
        // A clipper made through IDirectDraw takes any header.
        if (!fromDD1 && (data->rdh.dwSize < sizeof(RGNDATAHEADER) || data->rdh.iType != RDH_RECTANGLES))
            return DDERR_INVALIDCLIPLIST;
    }
    Guard l;
    if (hwnd) return DDERR_CLIPPERISUSINGHWND;
    if (!data) {
        clipList_.clear();
        return DD_OK;
    }
    // dd2d9's own limit, which also keeps the size below from overflowing.
    if (data->rdh.nCount > 65536) return DDERR_INVALIDCLIPLIST;
    size_t size = sizeof(RGNDATAHEADER) + size_t(data->rdh.nCount) * sizeof(RECT);
    clipList_.assign(reinterpret_cast<uint8_t*>(data), reinterpret_cast<uint8_t*>(data) + size);
    return DD_OK;
}

HRESULT Clipper::SetHWnd(DWORD flags, HWND h) {
    if (HRESULT hr = ClipperReady(this); FAILED(hr)) return hr;
    if (flags || (h && !IsWindow(h))) return DDERR_INVALIDPARAMS;
    Guard l;
    hwnd = h;
    if (dd_) dd_->MarkDirty();
    return DD_OK;
}

// Set/GetPriority and Set/GetLOD: managed textures only, at their top level (ddraw.dll
// hands them to Direct3D's texture manager).
HRESULT Surface::ManagedTextureCheck() const {
    const DDSCAPS2& c = desc.ddsCaps;
    if ((c.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL) || (root && (c.dwCaps2 & DDSCAPS2_CUBEMAP))) return DDERR_INVALIDPARAMS;
    if (!(c.dwCaps & DDSCAPS_TEXTURE)) return DDERR_INVALIDOBJECT;
    if (!(c.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE))) return DDERR_INVALIDPARAMS;
    return DD_OK;
}

// ================================================================ GammaControl

HRESULT GammaControl::QueryInterface(REFIID r, void** out) { return surface->QueryInterface(r, out); }
ULONG GammaControl::AddRef() { return surface->AddRef(Surface::kRefGamma); }
ULONG GammaControl::Release() { return surface->Release(Surface::kRefGamma); }

// As ddraw.dll (spec: re/spec/gamma-colorcontrol.md): the primary's only. Without a ramp
// of its own, the display's. Where Windows has none to give, ddraw.dll returns
// DDERR_UNSUPPORTED; dd2d9 applies ramps itself (to the game's image), so it always
// has one: the identity (deliberate).
HRESULT GammaControl::GetGammaRamp(DWORD flags, LPDDGAMMARAMP ramp) {
    Guard l;
    if (!ramp || flags || !(surface->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) return DDERR_INVALIDPARAMS;
    if (surface->IsLost()) return DDERR_SURFACELOST;
    if (surface->gammaRamp) {
        *ramp = *surface->gammaRamp;
        return DD_OK;
    }
    HDC dc = GetDC(nullptr);
    const bool ok = dc && GetDeviceGammaRamp(dc, ramp);
    if (dc) ReleaseDC(nullptr, dc);
    if (!ok)
        for (int i = 0; i < 256; i++) ramp->red[i] = ramp->green[i] = ramp->blue[i] = WORD(i * 0x101);
    return DD_OK;
}

// The ramp is the primary's and shows only while its DirectDraw object is exclusive;
// NULL drops it. No calibrator here: DDSGR_CALIBRATE (1) is unsupported.
HRESULT GammaControl::SetGammaRamp(DWORD flags, LPDDGAMMARAMP ramp) {
    Guard l;
    const DWORD kCalibrate = 1;  // DDSGR_CALIBRATE
    if (flags & ~kCalibrate) return DDERR_INVALIDPARAMS;
    if (flags & kCalibrate) return DDERR_UNSUPPORTED;
    if (!(surface->desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE)) return DDERR_INVALIDPARAMS;
    if (surface->IsLost()) return DDERR_SURFACELOST;
    if (!ramp) {
        delete surface->gammaRamp;
        surface->gammaRamp = nullptr;
    } else {
        if (!surface->gammaRamp) surface->gammaRamp = new DDGAMMARAMP;
        *surface->gammaRamp = *ramp;
    }
    if (surface->dd->primary == surface) {
        surface->dd->ApplyGamma();
        surface->dd->MarkDirty();
    }
    return DD_OK;
}

}  // namespace dd2d9
