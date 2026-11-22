// Software blitter: Blt / BltFast / color fill.
#include "blitint.h"
#include "gpures.h"

#include <cstdio>
#include <new>
#include <vector>

namespace dd2d9 {

// "x0,y0-x1,y1" or "full" for trace logging.
struct RectText {
    char s[48];
    explicit RectText(const RECT* r) {
        if (r) snprintf(s, sizeof(s), "%ld,%ld-%ld,%ld", r->left, r->top, r->right, r->bottom);
        else strcpy(s, "full");
    }
};

std::vector<RECT> g_clips;

// A surface's key as the blit into `dst` compares it: the driver's, into video memory, the
// whole range; the emulation layer's (into system memory) the low value alone.
static KeyRange KeyAsCompared(const DDCOLORKEY& k, const Surface* dst) {
    const DWORD lo = k.dwColorSpaceLowValue;
    return {true, lo, (dst->desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY) ? k.dwColorSpaceHighValue : lo};
}
// An override's key (DDBLTFX's): its low value alone, wherever the blit goes. Nothing says an
// override is a range (a surface's key has DDCKEY_COLORSPACE for that), and games leave the high
// value uninitialized: most drivers had no ranges and never looked at it. Deliberately not as
// XP with the Voodoo3's driver, which compared the range into video memory and, for a
// destination override into system memory, refused one (DDERR_NOCOLORKEYHW); 1.9.0 did both.
static KeyRange OverrideKey(const DDCOLORKEY& k) { return {true, k.dwColorSpaceLowValue, k.dwColorSpaceLowValue}; }
KeyRange SourceKeyOf(DWORD flags, const DDBLTFX* fx, const Surface* src, const Surface* dst) {
    if (flags & DDBLT_KEYSRCOVERRIDE) return OverrideKey(fx->ddckSrcColorkey);
    if (flags & DDBLT_KEYSRC) return KeyAsCompared(src->ck[kCkSrcBlt], dst);
    return {};
}
KeyRange DestKeyOf(DWORD flags, const DDBLTFX* fx, const Surface* owner, const Surface* dst) {
    if (flags & DDBLT_KEYDESTOVERRIDE) return OverrideKey(fx->ddckDestColorkey);
    if (flags & DDBLT_KEYDEST) return KeyAsCompared(owner->ck[kCkDestBlt], dst);
    return {};
}

void Mirroring(DWORD flags, const DDBLTFX* fx, BlitParams* p) {
    if (!(flags & DDBLT_DDFX)) return;
    p->mirrorX = (fx->dwDDFX & DDBLTFX_MIRRORLEFTRIGHT) != 0;
    p->mirrorY = (fx->dwDDFX & DDBLTFX_MIRRORUPDOWN) != 0;
    if (fx->dwDDFX & ~(DDBLTFX_MIRRORLEFTRIGHT | DDBLTFX_MIRRORUPDOWN | DDBLTFX_NOTEARING))
        DD2D9_LOG("Blt: unsupported DDFX 0x%08lx", fx->dwDDFX);
}

HRESULT ClipsFor(const Surface* s, const RECT& d, bool useClipper, std::vector<RECT>* out) {
    out->clear();
    const RECT bounds = {0, 0, s->buf->w, s->buf->h};
    if (useClipper && s->clipper) {
        // A clipper with nothing to clip to fails the blit, as its GetClipList would.
        if (HRESULT hr = s->clipper->Usable(); FAILED(hr)) return hr;
        for (const RECT& c : s->clipper->Region(s)) {
            RECT r;
            if (IntersectRect(&r, &c, &bounds) && IntersectRect(&r, &r, &d)) out->push_back(r);
        }
        return DD_OK;
    }
    if (d.left < 0 || d.top < 0 || d.right > bounds.right || d.bottom > bounds.bottom) return DDERR_INVALIDRECT;
    out->push_back(d);
    return DD_OK;
}

// Checks in ddraw.dll's order (spec: re/spec/surface-blt.md, section 1); BltLocked then
// draws. As ddraw.dll's software blitter: ROPs SRCCOPY, BLACKNESS and WHITENESS only, no
// rotation, no DDROPS.
HRESULT Surface::Blt(RECT* dstRect, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx, int version,
                     bool srcOldIface) {
    Guard l;
    DD2D9_TRACE("Blt %p[%s] <- %p[%s] flags=0x%lx%s fx=0x%lx fill=0x%lx", static_cast<void*>(this),
               RectText(dstRect).s, static_cast<void*>(src), RectText(srcRect).s, flags, isPrimary ? " (primary)" : "",
               fx ? fx->dwDDFX : 0, fx ? fx->dwFillColor : 0);
    if (CantDraw() || (src && src->CantDraw(false))) return DDERR_SURFACELOST;
    // 0x80000000 is the Direct3D 8/9 runtime's own flag (window clipping, with the
    // destination-alpha bits); ddraw.dll takes it off first.
    if (flags & 0x80000000) flags &= ~0x8000000Bu;
    if (flags & DDBLT_LAST_PRESENTATION) return DDERR_INVALIDPARAMS;
    if (version == 7 && (flags & DDBLT_WAIT) && (flags & DDBLT_DONOTWAIT)) return DDERR_INVALIDPARAMS;
    if (lockCount || (src && src->lockCount)) return DDERR_SURFACEBUSY;  // locked, or a DC out
    BumpUniqueness();  // as ddraw.dll: even if the blit fails from here on
    // The flags that read the DDBLTFX (DEPTHFILL, the overrides, ROP, DDROPS, DDFX, COLORFILL, ...).
    const DWORD kNeedFx = 0x02F75D4A;
    if (fx ? fx->dwSize != sizeof(DDBLTFX) : (flags & kNeedFx)) return DDERR_INVALIDPARAMS;

    // Color keys.
    if ((flags & DDBLT_KEYSRC) && !src) return DDERR_INVALIDOBJECT;
    if ((flags & DDBLT_KEYSRC) && ((flags & DDBLT_KEYSRCOVERRIDE) || !src->hasCk[kCkSrcBlt])) return DDERR_INVALIDPARAMS;
    if ((flags & DDBLT_KEYDEST) && ((flags & DDBLT_KEYDESTOVERRIDE) || !hasCk[kCkDestBlt])) return DDERR_INVALIDPARAMS;
    // The operation: the first of these flags decides.
    const DWORD fills = DDBLT_COLORFILL | DDBLT_DEPTHFILL;
    if (flags & DDBLT_ROP) {
        if (flags & (fills | DDBLT_DDFX | DDBLT_DDROPS | DDBLT_ROTATIONANGLE)) return DDERR_INVALIDPARAMS;
        const DWORD rop = fx->dwROP;
        if (rop != SRCCOPY && rop != BLACKNESS && rop != WHITENESS) return DDERR_NORASTEROPHW;
        if (rop == SRCCOPY && !src) return DDERR_INVALIDPARAMS;
    } else if (flags & DDBLT_COLORFILL) {
        if (src || buf->fmt.zbuf) return DDERR_INVALIDPARAMS;
    } else if (flags & DDBLT_DEPTHFILL) {
        if (src || !buf->fmt.zbuf) return DDERR_INVALIDPARAMS;
    } else if (flags & DDBLT_DDROPS) {
        if (flags & (DDBLT_ROTATIONANGLE | DDBLT_DDFX)) return DDERR_INVALIDPARAMS;
        return DDERR_NODDROPSHW;
    } else if (flags & DDBLT_DDFX) {
        if (flags & DDBLT_ROTATIONANGLE) return DDERR_INVALIDPARAMS;
        if (fx->dwDDFX & DDBLTFX_ARITHSTRETCHY) return DDERR_NOSTRETCHHW;
        if (fx->dwDDFX & (DDBLTFX_ROTATE90 | DDBLTFX_ROTATE180 | DDBLTFX_ROTATE270)) return DDERR_NOROTATIONHW;
        if (!src) return DDERR_EXCEPTION;  // ddraw.dll's blitter faults on it, and that is what it returns
    } else if (flags & DDBLT_ROTATIONANGLE) {
        return DDERR_NOROTATIONHW;
    } else if (!src) {
        return DDERR_INVALIDPARAMS;
    }

    // Rectangles: the destination's bounds only without a clipper (the clip list does it).
    const RECT d = dstRect ? *dstRect : RECT{0, 0, buf->w, buf->h};
    if (d.right <= d.left || d.bottom <= d.top) return DDERR_INVALIDRECT;
    if (!clipper && (d.left < 0 || d.top < 0 || d.right > buf->w || d.bottom > buf->h)) return DDERR_INVALIDRECT;
    if (src) {
        const RECT s = srcRect ? *srcRect : RECT{0, 0, src->W(), src->H()};
        if (s.right <= s.left || s.bottom <= s.top || s.left < 0 || s.top < 0 || s.right > src->W() ||
            s.bottom > src->H())
            return DDERR_INVALIDRECT;
    }
    // A fill takes no source rectangle.
    const bool usesSource = !(flags & (DDBLT_ROP | fills)) || ((flags & DDBLT_ROP) && fx->dwROP == SRCCOPY);
    const HRESULT hr = BltLocked(dstRect, usesSource ? src : nullptr, srcRect, flags, fx, true, srcOldIface);
    // fpslimit: a game that shows its frames by blitting them to the primary (windowed, or
    // without a flip chain) is paced at successful blits covering half of its screen or
    // more: the primary in exclusive mode, else the clipper's window (a windowed game's
    // primary is the whole desktop).
    if (g_config.fpsLimit > 0 && SUCCEEDED(hr) && isPrimary && src && src != this) {
        int64_t whole = int64_t(buf->w) * buf->h;
        RECT c;
        if (!dd->Exclusive() && clipper && clipper->hwnd && GetClientRect(clipper->hwnd, &c))
            whole = int64_t(c.right - c.left) * (c.bottom - c.top);
        const int64_t area = dstRect ? int64_t(dstRect->right - dstRect->left) * (dstRect->bottom - dstRect->top)
                                     : int64_t(buf->w) * buf->h;
        if (whole > 0 && area * 2 >= whole) RunWhenUnlocked([](void*) { LimitFrame(); }, nullptr);
    }
    return hr;
}

// The drawing (callers have checked what ddraw.dll checks, the rectangles among it; the
// contents stamp is theirs to change). Out of memory for the blitter's scratch buffers (a
// clip list, a large overlapping blit's copy) is DDERR_OUTOFMEMORY, not an exception through
// the COM method. Without a clipper the destination rectangle must lie inside the
// destination surface; with one, the blit is clipped to the clipper's region.
HRESULT Surface::BltLocked(RECT* dstRect, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx, bool useClipper,
                           bool destKeyFromSource) {
    try {
        const HRESULT hr = BltCore(dstRect, src, srcRect, flags, fx, useClipper, destKeyFromSource);
        KeptDcWritten();
        return hr;
    } catch (const std::bad_alloc&) {
        return DDERR_OUTOFMEMORY;
    }
}

HRESULT Surface::BltCore(RECT* dstRect, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx, bool useClipper,
                         bool destKeyFromSource) {
    // GDI may still be drawing on a surface whose DC is out (ReleaseDC flushes).
    if (dcOut || (src && src->dcOut)) GdiFlush();
    // 1-, 2- and 4-bit surfaces: ddraw.dll's emulation layer takes none of them, as source or
    // destination, for a copy or a fill (re/spec/surface-blt.md, section 0).
    if (buf->fmt.palLow || (src && src->buf->fmt.palLow)) return DDERR_UNSUPPORTED;
    // FourCC surfaces (DXT, YUY2, UYVY): copies (stretched, keyed, mirrored as any) through
    // an A8R8G8B8 decoding of either side, the destination's part encoded again after
    // (BltFourCC). A colour fill of a YUV surface in video memory writes the value as it is, as
    // the drivers did (Wine's test_color_fill; XP's Voodoo3); other fills of FourCC surfaces
    // aren't done, nor by ddraw.dll's emulation layer any of a texture in system memory (XP).
    const bool fill = (flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) ||
                      ((flags & DDBLT_ROP) && (fx->dwROP == BLACKNESS || fx->dwROP == WHITENESS));
    if (buf->fmt.fourcc && fill &&
        !(buf->fmt.Yuv() && (flags & DDBLT_COLORFILL) && (desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY)))
        return DDERR_UNSUPPORTED;
    if (!fill && (buf->fmt.fourcc || src->buf->fmt.fourcc))
        return BltFourCC(dstRect, src, srcRect, flags, fx, useClipper, destKeyFromSource);
    const RECT bounds = {0, 0, buf->w, buf->h};
    const RECT d = dstRect ? *dstRect : bounds;
    std::vector<RECT>& clips = g_clips;
    if (HRESULT hr = ClipsFor(this, d, useClipper, &clips); FAILED(hr)) return hr;
    Buffer* const sb = fill ? nullptr : src->buf;
    const RECT s = !sb ? RECT{} : srcRect ? *srcRect : RECT{0, 0, sb->w, sb->h};
    // A plain copy out of a render target whose newest pixels are on the GPU (a 3D frame
    // blitted to the screen or another render target): done there, without reading it back.
    const DWORD kPlain = DDBLT_WAIT | DDBLT_ASYNC | DDBLT_DONOTWAIT | DDBLT_ROP;
    if (sb && src != this && sb != buf && !(flags & ~kPlain) && GpuBlt(this, d, src, s, clips)) {
        MarkClipsDirty(this, clips);
        return DD_OK;
    }
    // Bump maps (du/dv, not colours), luminance and alpha-only surfaces only copy as they are,
    // as the emulation layer's blits.
    if (sb && !SameLayout(sb->fmt, buf->fmt) && (sb->fmt.CopyOnly() || buf->fmt.CopyOnly())) return DDERR_UNSUPPORTED;

    // What Direct3D drew into either surface is read back first.
    if (sb && src != this) src->CpuAccess(false);
    // A depth fill into a Z buffer on the GPU: no read back first (games fill every frame); the
    // GPU's copy gets the fill, or, with writes of the CPU's pending upload, the fill joins
    // them (GpuDepthFill). (Nor is it a Z lock: an antialiased one stays so, see GpuCpuAccess.)
    const bool zfill = (flags & DDBLT_DEPTHFILL) && GpuDepthFillable(buf);
    if (!zfill) CpuAccess(true, dstRect ? dstRect : &bounds);  // (the rectangle before clipping: at most that)
    else if (buf->gpuStale) buf->MarkGpuStale(dstRect ? dstRect : &bounds);

    if (flags & (DDBLT_COLORFILL | DDBLT_DEPTHFILL)) {
        const DWORD value = (flags & DDBLT_COLORFILL) ? fx->dwFillColor : fx->dwFillDepth;
        for (RECT& c : clips) {
            FillCore(&c, value);
            if (flags & DDBLT_DEPTHFILL) GpuDepthFill(this, c, value);  // a Z buffer Direct3D uses
        }
        MarkClipsDirty(this, clips);
        return DD_OK;
    }
    if (fill) {  // BLACKNESS, WHITENESS
        // As ddraw.dll's blitter: white leaves the top byte of a 32-bit pixel 0.
        for (RECT& c : clips) FillCore(&c, fx->dwROP == BLACKNESS ? 0 : 0x00FFFFFFu);
        MarkClipsDirty(this, clips);
        return DD_OK;
    }

    BlitParams p;
    p.srcKey = Masked(SourceKeyOf(flags, fx, src, this), sb->fmt);
    p.dstKey = Masked(DestKeyOf(flags, fx, destKeyFromSource ? src : this, this), buf->fmt);
    Mirroring(flags, fx, &p);
    p.spal = src->EffectivePalette();
    p.dpal = buf->fmt.pal8 ? EffectivePalette() : nullptr;
    DrawBlit(buf, d, sb, s, p, clips);
    MarkClipsDirty(this, clips);
    return DD_OK;
}

// The whole of `src` copied row by row: what Direct3D's texture loads do for the 1-, 2- and
// 4-bit surfaces Blt doesn't take (re/spec/d3d7-device-state.md, Load; d3d6-objects.md §6.6).
// The same size and format only.
HRESULT Surface::CopyRowsFrom(Surface* src) {
    if (!(src->buf->fmt == buf->fmt) || src->W() != W() || src->H() != H()) return DDERR_UNSUPPORTED;
    src->CpuAccess(false);
    CpuAccess(true);
    const size_t row = buf->fmt.RowBytes(DWORD(W()));
    for (int y = 0; y < H(); y++) memcpy(buf->Row(y), src->buf->Row(y), row);
    return DD_OK;
}

// Checks in ddraw.dll's order (spec: re/spec/surface-blt.md, section 2). BltFast never
// clips: both rectangles must lie inside their surfaces, and a dest with a clipper fails.
HRESULT Surface::BltFast(DWORD x, DWORD y, Surface* src, RECT* srcRect, DWORD trans, int version) {
    Guard l;
    DD2D9_TRACE("BltFast %p (%lu,%lu) <- %p[%s] trans=0x%lx", static_cast<void*>(this), x, y, static_cast<void*>(src),
               RectText(srcRect).s, trans);
    if (CantDraw() || src->CantDraw(false)) return DDERR_SURFACELOST;
    if (trans & ~0x33u) return DDERR_INVALIDPARAMS;  // NO/SRC/DESTCOLORKEY, WAIT, DONOTWAIT
    if (version == 7 && (trans & DDBLTFAST_WAIT) && (trans & DDBLTFAST_DONOTWAIT)) return DDERR_INVALIDPARAMS;
    if (lockCount || src->lockCount) return DDERR_SURFACEBUSY;
    BumpUniqueness();
    const RECT s = srcRect ? *srcRect : RECT{0, 0, src->W(), src->H()};
    if (s.right <= s.left || s.bottom <= s.top || s.left < 0 || s.top < 0 || s.right > src->W() ||
        s.bottom > src->H())
        return DDERR_INVALIDRECT;
    const int64_t w = s.right - s.left, h = s.bottom - s.top;
    if (LONG(x) < 0 || LONG(y) < 0 || int64_t(x) + w > W() || int64_t(y) + h > H()) return DDERR_INVALIDRECT;
    DWORD flags = 0;
    switch (trans & 0xF) {
    case DDBLTFAST_NOCOLORKEY:
        break;
    case DDBLTFAST_SRCCOLORKEY:
        if (!src->hasCk[kCkSrcBlt]) return DDERR_INVALIDPARAMS;
        flags = DDBLT_KEYSRC;
        break;
    case DDBLTFAST_DESTCOLORKEY:  // the destination's own key, whatever the interface
        if (!hasCk[kCkDestBlt]) return DDERR_INVALIDPARAMS;
        flags = DDBLT_KEYDEST;
        break;
    default:  // both at once: ddraw.dll sets up neither key and copies
        break;
    }
    if (clipper) return DDERR_BLTFASTCANTCLIP;
    RECT d = {LONG(x), LONG(y), LONG(int64_t(x) + w), LONG(int64_t(y) + h)};
    RECT sr = s;
    return BltLocked(&d, src, &sr, flags, nullptr, false);
}

}  // namespace dd2d9
