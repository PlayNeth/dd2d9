// Surfaces: creation and references, lost surfaces, colour keys, palettes and clippers set on
// them, descriptions, private data; the gamma control.
#include "wrappers.h"
#include "gpures.h"
#include "surfaceint.h"

#include "d3d6.h"

#include <algorithm>

namespace dd2d9 {

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
    if (desc.ddsCaps.dwCaps & DDSCAPS_OVERLAY) OverlayZOrder().insert(OverlayZOrder().begin(), this);  // in front
    if (desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY) {  // called with g_lock held
        vidMemBytes = buf->fmt.Planar() ? uint64_t(buf->pitch) * buf->h + uint64_t(buf->pitch / 2) * ((buf->h + 1) / 2) * 2
                                        : uint64_t(buf->pitch) * uint64_t(buf->fmt.BlockCompressed() ? (buf->h + 3) & ~3 : buf->h);
        dd->vidMemUsed += vidMemBytes;
    }
}

Surface::~Surface() {
    // Called with g_lock held.
    OverlayGone();
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
    if (palette && dd->primary == this) dd->KeepHardwarePalette(palette->entries);
    if (palette) palette->Release();
    if (clipper) {
        clipper->users.erase(std::find(clipper->users.begin(), clipper->users.end(), this));
        clipper->Release();
    }
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
    for (std::vector<Surface*>* list : {&dd->surfaces, &dd->detached})
        list->erase(std::remove(list->begin(), list->end(), this), list->end());
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
            // Referenced again before the lock was ours: a thread holding it found the surface
            // in dd->surfaces (EnumSurfaces, GetSurfaceFromDC) and took a reference. It lives.
            if (total_ != 0) return ULONG(r);
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
    dd->detached.push_back(this);
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
    // Back in front after Alt+Tab in dd2d9's own display mode: kept, not lost (see
    // UpdateLostSurfaces), from the first call on.
    if (dd->DevState() == DDraw::kDevNotRestored && dd->FocusLossOnly() && dd->KeepsSurfacesOnFocus()) {
        Guard l;
        dd->UpdateLostSurfaces();
    }
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

HRESULT Surface::SetClipper(IDirectDrawClipper* c) {
    Clipper* nc = c ? Clipper::From(c) : nullptr;
    if (c && (!nc || !LiveClipper(nc))) return DDERR_INVALIDPARAMS;
    if (!nc && !clipper) return DDERR_NOCLIPPERATTACHED;  // nothing to detach
    if (nc) {
        nc->AddRef();
        nc->users.push_back(this);
    }
    if (clipper) {
        clipper->users.erase(std::find(clipper->users.begin(), clipper->users.end(), this));
        clipper->Release();
    }
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

// Checks in ddraw.dll's order (spec section 3). dd2d9 reports single colour keys (no
// ranges): for blits, and for overlays while they are offered (videooverlay=on).
HRESULT Surface::SetColorKey(DWORD flags, DDCOLORKEY* key, int version) {
    if (IsLost()) return DDERR_SURFACELOST;
    if (desc.ddsCaps.dwCaps & DDSCAPS_ZBUFFER) return DDERR_INVALIDSURFACETYPE;
    if (version == 7 && (desc.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL)) return DDERR_NOTONMIPMAPSUBLEVEL;
    if (flags & ~(kCkTypes | DDCKEY_COLORSPACE)) return DDERR_INVALIDPARAMS;
    const bool overlays = OverlaysOffered(dd);
    // A destination overlay key: on a surface that overlays go on or are (the primary, an overlay).
    auto destOverlayCheck = [&]() -> HRESULT {
        if (!overlays) return DDERR_NOOVERLAYHW;
        if (!HasOverlayData()) return DDERR_NOTAOVERLAYSURFACE;
        return DD_OK;
    };
    if (!key) {
        // Removes the key of the first type named, in ddraw.dll's order; none named: nothing.
        // An overlay key also leaves the overlay's own use of it (UpdateOverlay's flags).
        if (flags & DDCKEY_DESTOVERLAY) {
            if (HRESULT hr = destOverlayCheck(); FAILED(hr)) return hr;
            hasCk[kCkDestOverlay] = false;
            overlayFlags &= ~DWORD(DDOVER_KEYDEST | DDOVER_KEYDESTOVERRIDE);
            if (OverlayShown()) overlayDest->dd->MarkOverlayDirty();
        } else if (flags & DDCKEY_DESTBLT) {
            hasCk[kCkDestBlt] = false;
        } else if (flags & DDCKEY_SRCOVERLAY) {
            hasCk[kCkSrcOverlay] = false;
            overlayFlags &= ~DWORD(DDOVER_KEYSRC | DDOVER_KEYSRCOVERRIDE);
            if (OverlayShown()) overlayDest->dd->MarkOverlayDirty();
        } else if (flags & DDCKEY_SRCBLT) {
            hasCk[kCkSrcBlt] = false;
        }
        return DD_OK;
    }
    const DWORD type = flags & kCkTypes;
    if (!OneBit(type)) return DDERR_INVALIDPARAMS;  // none, or two at once
    // Without DDCKEY_COLORSPACE the key is the low value.
    const DWORD low = key->dwColorSpaceLowValue;
    const DWORD high = (flags & DDCKEY_COLORSPACE) ? key->dwColorSpaceHighValue : low;
    if ((type & (DDCKEY_SRCOVERLAY | DDCKEY_DESTOVERLAY)) && !overlays) return DDERR_NOCOLORKEYHW;
    if (low != high) return DDERR_NOCOLORKEYHW;
    if (type & DDCKEY_DESTOVERLAY)
        if (HRESULT hr = destOverlayCheck(); FAILED(hr)) return hr;
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
    if (palette && isPrimary) dd->KeepHardwarePalette(palette->entries);  // (shown until another is)
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
        // In the game's memory: ~0, as ddraw.dll reports it (Wine's test_block_formats_creation).
        d->dwLinearSize = buf->ownsMemory ? DWORD(buf->pitch) * ((buf->h + 3) & ~3) : ~0u;
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
    if (hasCk[kCkDestOverlay]) {
        d->dwFlags |= DDSD_CKDESTOVERLAY;
        d->ddckCKDestOverlay = ck[kCkDestOverlay];
    }
    if (hasCk[kCkSrcOverlay]) {
        d->dwFlags |= DDSD_CKSRCOVERLAY;
        d->ddckCKSrcOverlay = ck[kCkSrcOverlay];
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
    if (w <= 0 || h <= 0) return DDERR_INVALIDPARAMS;  // above INT_MAX
    Format f = buf->fmt;
    if ((d.dwFlags & DDSD_PIXELFORMAT) && !Format::FromDDPF(d.ddpfPixelFormat, &f)) return DDERR_INVALIDPIXELFORMAT;
    // DXT: the pitch of dd2d9's own layout, as CreateSurface makes it for the game's memory (a
    // DXT description's lPitch is its linear size, and the old pitch another format's).
    const LONG pitch = f.BlockCompressed() ? Buffer::BlockPitch(w, f) : (d.dwFlags & DDSD_PITCH) ? d.lPitch : buf->pitch;
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

void Surface::CpuAccess(bool write, const RECT* area) {
    if (buf->gpu) GpuCpuAccess(this, write, area);  // reads back what Direct3D drew, if newer
    else if (write) buf->MarkGpuStale(area);
    if (write) buf->cpuSerial = NextCpuSerial();              // a texture's GPU copy is made from its levels' buffers
}

Surface* CubeFace(Surface* root, DWORD face) {
    if (root->desc.ddsCaps.dwCaps2 & face) return root;
    for (Surface* c : root->children)
        if ((c->desc.ddsCaps.dwCaps2 & face) && !(c->desc.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL)) return c;
    return nullptr;
}

Surface* Surface::NextMipLevel() const {
    const Surface* family = root ? root : this;
    for (Surface* a : attached)
        if ((a->root ? a->root : a) == family && (a->desc.ddsCaps.dwCaps2 & DDSCAPS2_MIPMAPSUBLEVEL)) return a;
    return nullptr;
}

void Surface::MarkDirtyIfVisible(const RECT* area) {
    if (!isPrimary) {
        if (OverlayShown() && overlayDest->isPrimary) overlayDest->dd->MarkOverlayDirty();  // laid over the primary
        return;
    }
    if (area) dd->MarkDirtyRect(*area);
    else dd->MarkDirty();
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
