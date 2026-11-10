// Attached surfaces and flip chains: AddAttachedSurface and its kin, Flip.
#include "wrappers.h"

#include <algorithm>

namespace dd2d9 {

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
        {
            Guard l;  // (its buffer: SetSurfaceDesc or a Flip on another thread changes it)
            list[i]->GetSurfaceDesc(&d2);
        }
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
    // A DDSCAPS2 with only dwCaps set and the rest left as it was on the stack (Rockman X5:
    // DDSCAPS_BACKBUFFER through IDirectDrawSurface4, the other words not cleared; it stopped
    // with "GetAttachSurface error"): matched on dwCaps alone, as the game expects, and logged.
    // Only when the other words can't be meant: dwCaps3 or dwCaps4 set (reserved in DirectX 7),
    // or dwCaps2 bits DirectX 7 doesn't define. (A real dwCaps2 that matches nothing, a cube
    // map face that isn't there, stays DDERR_NOTFOUND.)
    constexpr DWORD kCaps2Dx7 = 0x000FFF9E;
    if (caps.dwCaps && ((caps.dwCaps2 & ~kCaps2Dx7) || caps.dwCaps3 || caps.dwCaps4)) {
        for (const AttachEntry& e : AttachList()) {
            if (!has(e.s->desc.ddsCaps.dwCaps, caps.dwCaps)) continue;
            static bool logged;
            if (!logged) {
                logged = true;
                DD2D9_LOG("GetAttachedSurface: caps 0x%lx with 0x%lx 0x%lx 0x%lx in the other words, no surface has those: "
                          "matched on the first word alone",
                          static_cast<unsigned long>(caps.dwCaps), static_cast<unsigned long>(caps.dwCaps2),
                          static_cast<unsigned long>(caps.dwCaps3), static_cast<unsigned long>(caps.dwCaps4));
            }
            e.s->AddRef(SlotFor(version));
            *out = e.s;
            return DD_OK;
        }
    }
    return DDERR_NOTFOUND;
}

HRESULT Surface::Flip(Surface* target, DWORD flags) {
    uint64_t seq = 0;
    bool wait = false;
    {
        Guard l;
        // Checks in ddraw.dll's order (spec: re/spec/surface-flip-key-palette.md, section 1).
        // DDFLIP_EVEN/ODD only for overlays (and then like any flip); no stereo surfaces here.
        const DWORD kIntervals = 0x07000000;
        const bool overlay = (desc.ddsCaps.dwCaps & DDSCAPS_OVERLAY) != 0;
        if ((flags & ~(0x3Fu | kIntervals)) || ((flags & (DDFLIP_EVEN | DDFLIP_ODD)) && !overlay) ||
            ((flags & DDFLIP_NOVSYNC) && (flags & kIntervals)))
            return DDERR_INVALIDPARAMS;
        if (IsLost() || (target && target->IsLost(false))) return DDERR_SURFACELOST;
        if (flags & DDFLIP_STEREO) return DDERR_INVALIDPARAMS;  // (after the lost check, row 6)
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
        if (OverlayShown()) overlayDest->dd->MarkOverlayDirty();
        if (isPrimary) {
            Presenter* p = dd->GetPresenter();
            wait = g_config.vsync == kVSyncOn || (g_config.vsync == kVSyncAuto && !(flags & DDFLIP_NOVSYNC));
            // (A buffer Direct3D draws into, which has a GPU copy: only its frames wait for the
            // presents, on the device they share. A 2D game's flips never do.)
            const Buffer* shown = chain[0]->buf;
            if (g_config.vsync == kVSyncAuto) dd->CountFlip(!wait && shown && shown->gpu);
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
    if (isPrimary) RunWhenUnlocked([](void*) { LimitFrame(); }, nullptr);  // not under a game's thread lock
    return DD_OK;
}

// No flip is ever pending here: valid calls succeed (spec section 2).
HRESULT Surface::GetFlipStatus(DWORD flags) {
    if (flags != DDGFS_CANFLIP && flags != DDGFS_ISFLIPDONE) return DDERR_INVALIDPARAMS;
    if (IsLost()) return DDERR_SURFACELOST;
    return DD_OK;
}

}  // namespace dd2d9
