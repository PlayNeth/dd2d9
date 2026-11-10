// Surface access: Lock / Unlock, GetDC / ReleaseDC.
#include "objects.h"

#include <algorithm>

namespace dd2d9 {

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
        if (p != buf->dcPal || (p && p->version != buf->dcPalVersion) || !p) LoadDcColorTable(dc, p);
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

void Surface::LoadDcColorTable(HDC dc, const Palette* p) {
    const PALETTEENTRY* e = EffectivePalette();
    RGBQUAD q[256];
    for (int i = 0; i < 256; i++) q[i] = {e[i].peBlue, e[i].peGreen, e[i].peRed, 0};
    SetDIBColorTable(dc, 0, 256, q);
    buf->dcPal = p;
    buf->dcPalVersion = p ? p->version : 0;
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
    // null pointer. Not the back buffers, which fail as with ddraw.dll: NFS Porsche
    // Unleashed, Alt+Tabbed, stopped with "D3D pageflip called in while locked" while its
    // back buffer could be locked there.
    const bool background = dd->DevState() == DDraw::kDevLost && dd->FocusLossOnly() && (!lost || drawableLost) &&
                            (isPrimary || !InPrimaryChain());
    if (CantDraw() && !background) return DDERR_SURFACELOST;
    auto* d = static_cast<DDSURFACEDESC2*>(outDesc);
    const DWORD caps = desc.ddsCaps.dwCaps;
    // XP's fast path for system-memory textures of a DirectDraw object made by
    // DirectDrawCreateEx (spec section 1.4: DDRAWILCL_DIRECTDRAW7, the object's flag, not the
    // interface the surface was made through); managed textures, kept in system memory too,
    // get it as well (Wine's test on Windows).
    const bool managed = (desc.ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)) != 0;
    const bool fast = dd->dd7 && (sysmemRequested || managed) && (caps & DDSCAPS_TEXTURE);
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
    // What Direct3D drew is read back first. lockwhole=on: the write may be anywhere (a game
    // writing past the rectangle it locked, which XP's memory let it).
    CpuAccess(!(flags & DDLOCK_READONLY), g_config.lockWhole ? nullptr : &area);
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
    if (g_config.lockWhole) {
        lockArea = {0, 0, buf->w, buf->h};
    } else if (lockCount == 0 || (dcOut && lockCount == 1)) {
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
    if (g_config.lockWhole && lockCount > 0) lockArea = {0, 0, buf->w, buf->h};
    if (!readOnly) {
        BumpUniqueness();
        MarkDirtyIfVisible(g_config.lockWhole ? nullptr : &area);
    }
    return CantDraw() ? DDERR_SURFACELOST : DD_OK;  // unlocked either way, as ddraw.dll
}

// PageLock and PageUnlock: nothing to pin here; ddraw.dll checks only these.
HRESULT Surface::PageLock(DWORD flags) {
    if (flags) return DDERR_INVALIDPARAMS;
    Guard l;
    return IsLost() ? DDERR_SURFACELOST : DD_OK;
}

}  // namespace dd2d9
