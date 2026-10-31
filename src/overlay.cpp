// Overlay surfaces (DDSCAPS_OVERLAY). Cards of the era showed them in hardware, over the
// primary surface, without touching its pixels: video players and some games' movies use
// them. Here they are emulated: UpdateOverlay and its siblings keep ddraw.dll's state and
// checks (spec: re/spec/surface-other.md, "Overlays"), and the presenter lays the visible
// overlays over the frame it converts from the primary (stretched, mirrored, colour keyed).
#include "objects.h"
#include "dxt.h"
#include "gpures.h"

#include <algorithm>
#include <vector>

namespace dd2d9 {

namespace {
// As many overlays shown at once as dwMaxVisibleOverlays says (ddraw.cpp, GetCaps).
constexpr int kMaxVisibleOverlays = 16;
// The stretch factors reported (thousandths): from 1/1000 to 100 times.
constexpr DWORD kMinStretch = 1, kMaxStretch = 100000;
// UpdateOverlayZOrder's and EnumOverlayZOrders' flags (not in mingw-w64's ddraw.h).
enum : DWORD {
    kZSendToFront, kZSendToBack, kZMoveForward, kZMoveBackward, kZInsertInFrontOf, kZInsertInBackOf
};
constexpr DWORD kEnumBackToFront = 0;  // kEnumBackToFront (1: _FRONTTOBACK)

std::vector<Surface*>* g_overlayZ;  // g_lock

// The members of an overlay's flip chain, starting with `s` (a single overlay alone).
std::vector<Surface*> OverlayChain(Surface* s) {
    std::vector<Surface*> chain{s};
    for (Surface* m = s->chainNext; m && m != s && chain.size() < 4096; m = m->chainNext) chain.push_back(m);
    return chain;
}

void ReleaseLater(Surface* s) {
    // Not under g_lock: the last reference may take the DirectDraw object with it.
    RunWhenUnlocked([](void* p) { static_cast<Surface*>(p)->Release(Surface::kRefInternal); }, s);
}

int VisibleOverlays() {
    int n = 0;
    for (Surface* s : OverlayZOrder())
        if (s->desc.ddsCaps.dwCaps & DDSCAPS_VISIBLE) n++;
    return n;
}

bool InSurface(const RECT& r, int w, int h) {
    // (bounds first: the game's coordinates may be anything, and right - left could overflow)
    return r.left >= 0 && r.top >= 0 && r.right <= w && r.bottom <= h && r.right > r.left && r.bottom > r.top;
}
}  // namespace

bool OverlaysOffered(const DDraw* dd) { return g_config.overlays && dd && dd->Exclusive(); }

std::vector<Surface*>& OverlayZOrder() {
    if (!g_overlayZ) g_overlayZ = new std::vector<Surface*>;
    return *g_overlayZ;
}

bool Surface::HasOverlayData() const {
    return (desc.ddsCaps.dwCaps & DDSCAPS_OVERLAY) || ((desc.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) && OverlaysOffered(dd));
}

// ddraw.dll's checks in its order (IDirectDrawSurface7_UpdateOverlay.c and its helpers
// FUN_7373d771, FUN_7373d81a, FUN_7373d8a1), g_lock held.
HRESULT Surface::UpdateOverlay(RECT* srcRect, Surface* dest, RECT* dstRect, DWORD flags, DDOVERLAYFX* fx) {
    if (IsLost()) return DDERR_SURFACELOST;
    if (!dest) return DDERR_INVALIDPARAMS;  // (ddraw.dll faults reading it)
    if (dest->IsLost(false)) return DDERR_SURFACELOST;
    if (flags & 0xF8000000) return DDERR_INVALIDPARAMS;
    if (fx ? fx->dwSize != sizeof(DDOVERLAYFX) : (flags & DDOVER_DDFX) != 0) return DDERR_INVALIDPARAMS;
    if (!(desc.ddsCaps.dwCaps & DDSCAPS_OVERLAY)) return DDERR_NOTAOVERLAYSURFACE;
    // An overlay over an overlay: no hardware did that (and two holding each other would
    // never be freed).
    if (dest->desc.ddsCaps.dwCaps & DDSCAPS_OVERLAY) return DDERR_INVALIDSURFACETYPE;
    // Either one in system memory: the emulation layer's overlays, which it has none of.
    if ((desc.ddsCaps.dwCaps | dest->desc.ddsCaps.dwCaps) & DDSCAPS_SYSTEMMEMORY) return DDERR_UNSUPPORTED;
    // No rectangle: the whole surface (the whole destination: an overlay over the whole screen).
    const RECT d = dstRect ? *dstRect : RECT{0, 0, dest->W(), dest->H()};
    const RECT sr = srcRect ? *srcRect : RECT{0, 0, W(), H()};
    if (!InSurface(d, dest->W(), dest->H()) || !InSurface(sr, W(), H())) return DDERR_INVALIDRECT;
    // The stretch factor (the width's) within what GetCaps reports; any stretch or shrink
    // between them (arbitrary stretching caps). A hidden overlay isn't checked.
    if (!(flags & DDOVER_HIDE)) {
        const uint64_t dw = uint64_t(d.right - d.left) * 1000, sw = uint64_t(sr.right - sr.left);
        if (dw < kMinStretch * sw || kMaxStretch * sw < dw) return DDERR_INVALIDPARAMS;
    }
    DWORD f = flags;
    DDOVERLAYFX keep = fx ? *fx : DDOVERLAYFX{};
    if (f & 0x00703C00) {  // keys, DDOVER_AUTOFLIP, DDOVER_BOB, DDOVER_OVERRIDEBOBWEAVE
        // No video port to flip it, no bob caps.
        if (f & (DDOVER_AUTOFLIP | DDOVER_BOB | DDOVER_BOBHARDWARE)) return DDERR_INVALIDPARAMS;
        if ((f & (DDOVER_KEYDESTOVERRIDE | DDOVER_KEYSRCOVERRIDE)) && !fx) return DDERR_INVALIDPARAMS;  // (a fault)
        // DDOVER_KEYSRC and DDOVER_KEYDEST become overrides with the surfaces' keys as they
        // are now, as ddraw.dll hands them to the driver (later changes need another call).
        if (f & DDOVER_KEYSRC) {
            if ((f & DDOVER_KEYSRCOVERRIDE) || !hasCk[kCkSrcOverlay]) return DDERR_INVALIDPARAMS;
            keep.dckSrcColorkey = ck[kCkSrcOverlay];
            f = (f & ~DWORD(DDOVER_KEYSRC)) | DDOVER_KEYSRCOVERRIDE;
        }
        if (f & DDOVER_KEYDEST) {
            if ((f & DDOVER_KEYDESTOVERRIDE) || !dest->hasCk[kCkDestOverlay]) return DDERR_INVALIDPARAMS;
            keep.dckDestColorkey = dest->ck[kCkDestOverlay];
            f = (f & ~DWORD(DDOVER_KEYDEST)) | DDOVER_KEYDESTOVERRIDE;
        }
    }
    const bool wasVisible = (desc.ddsCaps.dwCaps & DDSCAPS_VISIBLE) != 0;
    if ((f & DDOVER_SHOW) && !wasVisible && VisibleOverlays() >= kMaxVisibleOverlays) return DDERR_OUTOFCAPS;
    if (f & DDOVER_SHOW) desc.ddsCaps.dwCaps |= DDSCAPS_VISIBLE;
    else if (f & DDOVER_HIDE) desc.ddsCaps.dwCaps &= ~DWORD(DDSCAPS_VISIBLE);
    // Every member of the chain takes the position (only this one becomes visible).
    for (Surface* m : OverlayChain(this)) {
        m->overlaySrc = sr;
        m->overlayDst = d;
        m->overlayFlags = f;
        m->overlayFx = keep;
    }
    if (overlayDest != dest) {
        if (overlayDest) {
            overlayDest->dd->MarkDirty();  // (whose image it left)
            ReleaseLater(overlayDest);
        }
        overlayDest = dest;
        dest->AddRef(kRefInternal);
    }
    static bool logged;
    if (!logged) {
        logged = true;
        DD2D9_LOG("the game uses overlays (laid over the primary as it is presented)");
    }
    DD2D9_TRACE("UpdateOverlay %p: on %p, %ld,%ld-%ld,%ld from %ld,%ld-%ld,%ld, flags 0x%lx%s", static_cast<void*>(this),
              static_cast<void*>(dest), d.left, d.top, d.right, d.bottom, sr.left, sr.top, sr.right, sr.bottom, f,
              (desc.ddsCaps.dwCaps & DDSCAPS_VISIBLE) ? " (shown)" : "");
    dest->dd->MarkDirty();
    return DD_OK;
}

// IDirectDrawSurface7_GetOverlayPosition.c: the caller zeroed x and y.
HRESULT Surface::GetOverlayPosition(LONG* x, LONG* y) {
    if (IsLost()) return DDERR_SURFACELOST;
    if (!(desc.ddsCaps.dwCaps & DDSCAPS_OVERLAY)) return DDERR_NOTAOVERLAYSURFACE;
    if (!(desc.ddsCaps.dwCaps & DDSCAPS_VISIBLE)) return DDERR_OVERLAYNOTVISIBLE;
    if (!overlayDest) return DDERR_NOOVERLAYDEST;
    *x = overlayDst.left;
    *y = overlayDst.top;
    return DD_OK;
}

// IDirectDrawSurface7_SetOverlayPosition.c: the destination rectangle moves, its size stays.
HRESULT Surface::SetOverlayPosition(LONG x, LONG y) {
    if (IsLost()) return DDERR_SURFACELOST;
    if (!(desc.ddsCaps.dwCaps & DDSCAPS_OVERLAY)) return DDERR_NOTAOVERLAYSURFACE;
    if (!(desc.ddsCaps.dwCaps & DDSCAPS_VISIBLE)) return DDERR_OVERLAYNOTVISIBLE;
    if (!overlayDest) return DDERR_NOOVERLAYDEST;
    if (y > LONG(overlayDest->H()) - (overlayDst.bottom - overlayDst.top) ||
        x > LONG(overlayDest->W()) - (overlayDst.right - overlayDst.left) || y < 0 || x < 0)
        return DDERR_INVALIDPOSITION;
    if ((desc.ddsCaps.dwCaps | overlayDest->desc.ddsCaps.dwCaps) & DDSCAPS_SYSTEMMEMORY) return DDERR_UNSUPPORTED;
    OffsetRect(&overlayDst, x - overlayDst.left, y - overlayDst.top);
    overlayDest->dd->MarkDirty();
    return DD_OK;
}

// IDirectDrawSurface7_UpdateOverlayZOrder.c. No lost check there.
HRESULT Surface::UpdateOverlayZOrder(DWORD flags, Surface* ref) {
    if (!(desc.ddsCaps.dwCaps & DDSCAPS_OVERLAY)) return DDERR_NOTAOVERLAYSURFACE;
    std::vector<Surface*>& z = OverlayZOrder();
    auto at = [&z](const Surface* s) { return size_t(std::find(z.begin(), z.end(), s) - z.begin()); };
    size_t i = at(this);
    if (i == z.size()) return DDERR_NOTAOVERLAYSURFACE;  // can't happen: every overlay is listed
    switch (flags) {
    case kZSendToFront:
        z.erase(z.begin() + i);
        z.insert(z.begin(), this);
        break;
    case kZSendToBack:
        z.erase(z.begin() + i);
        z.push_back(this);
        break;
    case kZMoveForward:
        if (i > 0) std::swap(z[i], z[i - 1]);
        break;
    case kZMoveBackward:
        if (i + 1 < z.size()) std::swap(z[i], z[i + 1]);
        break;
    case kZInsertInFrontOf:
    case kZInsertInBackOf: {
        if (!ref) return DDERR_INVALIDPARAMS;  // (ddraw.dll faults reading it)
        if (!(ref->desc.ddsCaps.dwCaps & DDSCAPS_OVERLAY)) return DDERR_NOTAOVERLAYSURFACE;
        if (at(ref) == z.size()) return DDERR_INVALIDPARAMS;
        if (ref == this) break;  // (ddraw.dll would unlink the node it inserts next to)
        z.erase(z.begin() + i);
        const size_t r = at(ref);
        z.insert(z.begin() + (flags == kZInsertInFrontOf ? r : r + 1), this);
        break;
    }
    default:
        return DDERR_INVALIDPARAMS;
    }
    if (overlayDest) overlayDest->dd->MarkDirty();
    return DD_OK;
}

// IDirectDrawSurface7_EnumOverlayZOrders.c: every overlay of the device, whichever surface
// is asked, through the caller's interface version, referenced (the callback releases it).
HRESULT Surface::EnumOverlayZOrders(DWORD flags, int v, void* ctx, void* callback) {
    using CB = HRESULT(WINAPI*)(void*, void*, void*);
    std::vector<Surface*> list;
    {
        Guard l;
        list = OverlayZOrder();
        if (flags == kEnumBackToFront) std::reverse(list.begin(), list.end());
        for (Surface* s : list) s->AddRef(SlotFor(HandedOut(v)));
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
        if (r == DDENUMRET_CANCEL) {
            i++;
            break;
        }
    }
    for (; i < list.size(); i++) list[i]->Release(SlotFor(HandedOut(v)));  // never handed out
    return DD_OK;
}

// g_lock held.
void Surface::OverlayGone() {
    if (desc.ddsCaps.dwCaps & DDSCAPS_OVERLAY) {
        std::vector<Surface*>& z = OverlayZOrder();
        z.erase(std::remove(z.begin(), z.end(), this), z.end());
        if (OverlayShown()) overlayDest->dd->MarkDirty();
    }
    if (overlayDest) {
        ReleaseLater(overlayDest);
        overlayDest = nullptr;
    }
    // (Overlays on this surface hold references to it: none is left.)
    if (g_overlayZ)
        for (Surface* s : *g_overlayZ)
            if (s->overlayDest == this) s->overlayDest = nullptr;
}

// ---------------------------------------------------------------- the presenter's part

struct OverlayLayer {
    Buffer* buf;
    PALETTEENTRY pal[256];
    RECT src, dst;
    DWORD flags, ddfx;
    DDCOLORKEY destKey, srcKey;
};

void TakeOverlays(const Surface* primary, std::vector<OverlayLayer*>* out) {
    if (!g_overlayZ) return;
    for (auto it = g_overlayZ->rbegin(); it != g_overlayZ->rend(); ++it) {
        Surface* s = *it;
        if (!s->OverlayShown() || s->overlayDest != primary || s->IsLost(false)) continue;
        // (Within the surface as UpdateOverlay saw it: SetSurfaceDesc may have changed its size.)
        const RECT& sr = s->overlaySrc;
        if (sr.left < 0 || sr.top < 0 || sr.right > s->buf->w || sr.bottom > s->buf->h || sr.right <= sr.left ||
            sr.bottom <= sr.top)
            continue;
        if (s->buf->gpu && s->buf->cpuStale) GpuCpuAccess(s, false);
        auto* l = new OverlayLayer;
        l->buf = s->buf;
        l->buf->AddRef();
        if (l->buf->fmt.pal8) {
            const Palette* p = s->palette ? s->palette : primary->ShownPalette();
            memcpy(l->pal, p ? p->entries : s->dd->defaultPalette, sizeof(l->pal));
        }
        l->src = s->overlaySrc;
        l->dst = s->overlayDst;
        l->flags = s->overlayFlags;
        l->ddfx = (s->overlayFlags & DDOVER_DDFX) ? s->overlayFx.dwDDFX : 0;
        l->destKey = s->overlayFx.dckDestColorkey;
        l->srcKey = s->overlayFx.dckSrcColorkey;
        out->push_back(l);
    }
}

static uint32_t RawPixel(const uint8_t* p, DWORD bytes) {
    switch (bytes) {
    case 1: return p[0];
    case 2: return uint32_t(p[0]) | uint32_t(p[1]) << 8;
    case 3: return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16;
    default: return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
}

static bool KeyMatch(uint32_t v, DWORD mask, const DDCOLORKEY& k) {
    v &= mask;
    return v >= (k.dwColorSpaceLowValue & mask) && v <= (k.dwColorSpaceHighValue & mask);
}

void DrawOverlays(const std::vector<OverlayLayer*>& layers, const Buffer* primaryBits, const RECT& area, uint32_t* frame,
                  size_t pitch) {
    std::vector<uint32_t> decoded;
    for (const OverlayLayer* l : layers) {
        RECT vis;
        if (!IntersectRect(&vis, &l->dst, &area)) continue;
        const Buffer* b = l->buf;
        const Format& f = b->fmt;
        // FourCC overlays (YUY2, UYVY) decoded whole; their source key is a pixel pair,
        // compared as the colour it decodes to.
        if (f.fourcc) {
            decoded.resize(size_t(b->w) * b->h);
            DecodeFourCC(b, reinterpret_cast<uint8_t*>(decoded.data()), LONG(b->w) * 4);
        }
        const bool srcKeyed = (l->flags & DDOVER_KEYSRCOVERRIDE) != 0;
        const bool destKeyed = (l->flags & DDOVER_KEYDESTOVERRIDE) != 0;
        const uint32_t fourccKey = f.fourcc && srcKeyed ? FourCCKeyToArgb(f.fourCC, l->srcKey.dwColorSpaceLowValue) & 0xFFFFFF : 0;
        const DWORD srcMask = f.KeyMask(), destMask = primaryBits->fmt.KeyMask();
        const DWORD srcBytes = f.Bytes(), destBytes = primaryBits->fmt.Bytes();
        const LONG dw = l->dst.right - l->dst.left, dh = l->dst.bottom - l->dst.top;
        const LONG sw = l->src.right - l->src.left, sh = l->src.bottom - l->src.top;
        for (LONG y = vis.top; y < vis.bottom; y++) {
            LONG sy = LONG((int64_t(2 * (y - l->dst.top) + 1) * sh) / (2 * dh));
            if (l->ddfx & DDOVERFX_MIRRORUPDOWN) sy = sh - 1 - sy;
            sy += l->src.top;
            uint32_t* row = frame + size_t(y - area.top) * pitch - area.left;
            const uint8_t* destRow =
                (y >= 0 && y < primaryBits->h) ? primaryBits->Row(int(y)) : nullptr;
            for (LONG x = vis.left; x < vis.right; x++) {
                if (destKeyed) {
                    // Shown only where the destination has its key colour.
                    if (!destRow || x < 0 || x >= primaryBits->w) continue;
                    if (!KeyMatch(RawPixel(destRow + size_t(x) * destBytes, destBytes), destMask, l->destKey)) continue;
                }
                LONG sx = LONG((int64_t(2 * (x - l->dst.left) + 1) * sw) / (2 * dw));
                if (l->ddfx & DDOVERFX_MIRRORLEFTRIGHT) sx = sw - 1 - sx;
                sx += l->src.left;
                uint32_t argb;
                if (f.fourcc) {
                    argb = decoded[size_t(sy) * b->w + sx];
                    if (srcKeyed && (argb & 0xFFFFFF) == fourccKey) continue;
                } else {
                    const uint32_t raw = RawPixel(b->Row(int(sy)) + size_t(sx) * srcBytes, srcBytes);
                    if (srcKeyed && KeyMatch(raw, srcMask, l->srcKey)) continue;
                    argb = PixelToARGB(f, raw, l->pal);
                }
                row[x] = argb | 0xFF000000;
            }
        }
    }
}

void DropOverlays(std::vector<OverlayLayer*>* layers) {
    for (OverlayLayer* l : *layers) {
        l->buf->Release();
        delete l;
    }
    layers->clear();
}

}  // namespace dd2d9
