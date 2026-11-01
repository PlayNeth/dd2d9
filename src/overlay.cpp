// Overlay surfaces (DDSCAPS_OVERLAY). Cards of the era showed them in hardware, over the
// primary surface, without touching its pixels: video players and some games' movies use
// them. Here they are emulated: UpdateOverlay and its siblings keep ddraw.dll's state and
// checks (spec: re/spec/surface-other.md, "Overlays"), and the presenter lays the visible
// overlays over the frame it converts from the primary (stretched, mirrored, colour keyed).
#include "objects.h"
#include "convert.h"
#include "devstate.h"
#include "dxt.h"
#include "gpu.h"
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

bool OverlaysOffered(const DDraw*) { return g_config.overlays; }

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
            overlayDest->dd->MarkOverlayDirty();  // (whose image it left)
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
    dest->dd->MarkOverlayDirty();
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
    overlayDest->dd->MarkOverlayDirty();
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
    if (overlayDest) overlayDest->dd->MarkOverlayDirty();
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
        if (OverlayShown()) overlayDest->dd->MarkOverlayDirty();
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
    unsigned serial;  // buf->cpuSerial when taken: the GPU path's copy is current while equal
    PALETTEENTRY pal[256];
    RECT src, dst;
    DWORD flags, ddfx;
    DDCOLORKEY destKey, srcKey;
    // The GPU path's keys, as the colours they show (the destination's through the primary's
    // format and palette, a FourCC source key as it decodes), and its tolerance.
    uint32_t destKeyArgb, srcKeyArgb;
    float destEps;
    bool unkeyed = false;  // (PrepareOverlays) a windowed key shown nowhere under it: drawn whole
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
        l->serial = s->buf->cpuSerial;
        const Format& pf = primary->buf->fmt;
        const Palette* pp = pf.pal8 ? primary->ShownPalette() : nullptr;
        l->destKeyArgb = PixelToARGB(pf, l->destKey.dwColorSpaceLowValue, pp ? pp->entries : primary->dd->defaultPalette);
        // (16-bit and smaller: the frame's colours come out of the GPU's own widening.)
        l->destEps = (pf.bpp <= 16 && !pf.pal8 ? 4.5f : 0.5f) / 255.0f;
        const Format& f = l->buf->fmt;
        l->srcKeyArgb = f.fourcc ? FourCCKeyToArgb(f.fourCC, l->srcKey.dwColorSpaceLowValue)
                                 : PixelToARGB(f, l->srcKey.dwColorSpaceLowValue, l->pal);
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

bool OverlaysKeyedOnDest(const std::vector<OverlayLayer*>& layers) {
    for (const OverlayLayer* l : layers)
        if (l->flags & DDOVER_KEYDESTOVERRIDE) return true;
    return false;
}

void DrawOverlays(const std::vector<OverlayLayer*>& layers, const Buffer* primaryBits, const RECT& area, uint32_t* frame,
                  size_t pitch, const OverlayGdi* gdi) {
    std::vector<uint32_t> decoded;
    const DWORD destMask = primaryBits->fmt.KeyMask(), destBytes = primaryBits->fmt.Bytes();
    for (const OverlayLayer* l : layers) {
        RECT vis;
        if (!IntersectRect(&vis, &l->dst, &area)) continue;
        const Buffer* b = l->buf;
        const Format& f = b->fmt;
        // FourCC overlays (YUY2, UYVY, YV12) decoded whole; their source key is compared as
        // the colour it decodes to.
        if (f.fourcc) {
            decoded.resize(size_t(b->w) * b->h);
            DecodeFourCC(b, reinterpret_cast<uint8_t*>(decoded.data()), LONG(b->w) * 4);
        }
        const bool srcKeyed = (l->flags & DDOVER_KEYSRCOVERRIDE) != 0;
        bool destKeyed = (l->flags & DDOVER_KEYDESTOVERRIDE) != 0;
        // The destination key: the primary's raw pixel, or (a windowed program) what GDI drew
        // in its window there, as the screen the hardware compared held both.
        auto keyAt = [&](LONG x, LONG y) {
            if (y >= 0 && y < primaryBits->h && x >= 0 && x < primaryBits->w &&
                KeyMatch(RawPixel(primaryBits->Row(int(y)) + size_t(x) * destBytes, destBytes), destMask, l->destKey))
                return true;
            if (gdi && !primaryBits->fmt.pal8) {  // (no colour-to-index lookup for an 8-bit desktop)
                const LONG gx = x - gdi->left, gy = y - gdi->top;
                if (gx >= 0 && gy >= 0 && gx < gdi->w && gy < gdi->h &&
                    KeyMatch(ARGBToPixel(primaryBits->fmt, 0xFF000000u | gdi->px[size_t(gy) * gdi->w + gx]), destMask,
                             l->destKey))
                    return true;
            }
            return false;
        };
        // A windowed program's key that shows nowhere under the overlay (painted where dd2d9
        // doesn't see it): the overlay shown whole rather than not at all.
        if (destKeyed && gdi) {
            bool any = false;
            for (LONG y = vis.top; y < vis.bottom && !any; y++)
                for (LONG x = vis.left; x < vis.right && !any; x++) any = keyAt(x, y);
            if (!any) destKeyed = false;
        }
        const uint32_t fourccKey = f.fourcc && srcKeyed ? FourCCKeyToArgb(f.fourCC, l->srcKey.dwColorSpaceLowValue) & 0xFFFFFF : 0;
        const DWORD srcMask = f.KeyMask(), srcBytes = f.Bytes();
        const LONG dw = l->dst.right - l->dst.left, dh = l->dst.bottom - l->dst.top;
        const LONG sw = l->src.right - l->src.left, sh = l->src.bottom - l->src.top;
        for (LONG y = vis.top; y < vis.bottom; y++) {
            LONG sy = LONG((int64_t(2 * (y - l->dst.top) + 1) * sh) / (2 * dh));
            if (l->ddfx & DDOVERFX_MIRRORUPDOWN) sy = sh - 1 - sy;
            sy += l->src.top;
            uint32_t* row = frame + size_t(y - area.top) * pitch - area.left;
            for (LONG x = vis.left; x < vis.right; x++) {
                if (destKeyed && !keyAt(x, y)) continue;  // shown only where the destination has its key colour
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

// ---------------------------------------------------------------- on the GPU
// The overlays drawn over the presented frame in the back buffer, as hardware laid them over
// the screen: each one's pixels uploaded only when they changed, its keys and mirroring
// applied by kOverlayPs, stretched with filtering (point sampled when source-keyed). The
// destination key is compared with the frame as converted (a copy of the source), and for a
// windowed program with what GDI drew in its window too.

// ps_2_0. t0.xy: the overlay's texture coordinates, t0.zw: the frame's (and GDI's). s0 the
// overlay, s1 the frame, s2 the GDI picture. c0 = -source key, c3 = (0, -its tolerance,
// source keyed, 0); c1 = -destination key as the frame has it (c5 as GDI's picture has it),
// c2 = ((no GDI picture) * 10, destination keyed, -its tolerance, 0). Killed
// where the source key matches (keyed) or the destination key matches neither the frame nor
// the GDI picture (keyed). Written to the presenter's shaders' rules (registers set whole
// first, one constant per instruction, no negated constants). Assembled (psasm) from:
//   def c4, 1, 0, 10, -1 / dcl t0 / dcl_2d s0..s2 / mov r3..r8, c4.y / mov r9, c4.x
//   r5.xy = t0.zw / texld r0, t0, s0 / texld r1, r5, s1 / texld r2, r5, s2
//   r3.x = max|r0.rgb + c0| + c3.y / r4.x = (r3.x + c4.w) * c3.z + 1
//   r6.x = max|r1.rgb + c1| / r7.x = max|r2.rgb + c5| + c2.x / r6.x = min(r6.x, r7.x) + c2.z
//   r8.x = (-r6.x + c4.w) * c2.y + 1 / r4 = min(r4.x, r8.x) / texkill r4 / r0.w = 1 / oC0 = r0
static const DWORD kOverlayPs[] = {
    0xffff0200, 0x0200001f, 0x80000000, 0xb00f0000, 0x0200001f, 0x90000000, 0xa00f0800, 0x0200001f, 0x90000000, 0xa00f0801, 0x0200001f, 0x90000000, 0xa00f0802, 0x05000051, 0xa00f0004, 0x3f800000, 0x00000000, 0x41200000, 0xbf800000, 0x02000001, 0x800f0003, 0xa0550004, 0x02000001, 0x800f0004, 0xa0550004, 0x02000001, 0x800f0005, 0xa0550004, 0x02000001, 0x800f0006, 0xa0550004, 0x02000001, 0x800f0007, 0xa0550004, 0x02000001, 0x800f0008, 0xa0550004, 0x02000001, 0x800f0009, 0xa0000004, 0x02000001, 0x80030005, 0xb0ee0000, 0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x03000042, 0x800f0001, 0x80e40005, 0xa0e40801, 0x03000042, 0x800f0002, 0x80e40005, 0xa0e40802, 0x03000002, 0x80070003, 0x80e40000, 0xa0e40000, 0x02000023, 0x80070003, 0x80e40003, 0x0300000b, 0x80010003, 0x80000003, 0x80550003, 0x0300000b, 0x80010003, 0x80000003, 0x80aa0003, 0x03000002, 0x80010003, 0x80000003, 0xa0550003, 0x03000002, 0x80010004, 0x80000003, 0xa0ff0004, 0x04000004, 0x80010004, 0x80000004, 0xa0aa0003, 0x80000009, 0x03000002, 0x80070006, 0x80e40001, 0xa0e40001, 0x02000023, 0x80070006, 0x80e40006, 0x0300000b, 0x80010006, 0x80000006, 0x80550006, 0x0300000b, 0x80010006, 0x80000006, 0x80aa0006, 0x03000002, 0x80070007, 0x80e40002, 0xa0e40005, 0x02000023, 0x80070007, 0x80e40007, 0x0300000b, 0x80010007, 0x80000007, 0x80550007, 0x0300000b, 0x80010007, 0x80000007, 0x80aa0007, 0x03000002, 0x80010007, 0x80000007, 0xa0000002, 0x0300000a, 0x80010006, 0x80000006, 0x80000007, 0x03000002, 0x80010006, 0x80000006, 0xa0aa0002, 0x03000002, 0x80010008, 0x81000006, 0xa0ff0004, 0x04000004, 0x80010008, 0x80000008, 0xa0550002, 0x80000009, 0x0300000a, 0x80010004, 0x80000004, 0x80000008, 0x02000001, 0x800f0004, 0x80000004, 0x01000041, 0x800f0004, 0x02000001, 0x80080000, 0x80000009, 0x02000001, 0x800f0800, 0x80e40000, 0x0000ffff};

class OverlayGpu {
public:
    // Each overlay's pixels as decoded: the presenter thread's alone (PrepareOverlays, no lock;
    // DrawOverlaysGpu reads them).
    struct Layer {
        const Buffer* buf = nullptr;
        unsigned serial = 0;
        int w = 0, h = 0;
        PALETTEENTRY pal[256];       // (a palettized overlay's colours when decoded)
        std::vector<uint32_t> argb;
        unsigned decoded = 0;        // which decoding argb holds (0: none)
    };
    std::vector<Layer> layers;
    // Their textures: Gpu lock only (Release can come from the game's thread on a device reset).
    struct Tex {
        IDirect3DTexture9* tex = nullptr;
        int w = 0, h = 0;
        unsigned uploaded = 0;  // the decoding it holds
    };
    std::vector<Tex> texs;
    unsigned decodings = 0;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DTexture9* frameTex = nullptr;
    UINT frameW = 0, frameH = 0;
    IDirect3DTexture9* gdiTex = nullptr;
    LONG gdiW = 0, gdiH = 0;
    unsigned gdiSeq = 0;
    bool failed = false;  // no pixel shaders: the CPU path from now on

    void Release() {
        for (Tex& t : texs) {
            if (t.tex) t.tex->Release();
            t = Tex();
        }
        if (ps) ps->Release();
        if (frameTex) frameTex->Release();
        if (gdiTex) gdiTex->Release();
        ps = nullptr;
        frameTex = gdiTex = nullptr;
        frameW = frameH = 0;
        gdiW = gdiH = 0;
        gdiSeq = 0;
    }
};

OverlayGpu* NewOverlayGpu() { return new OverlayGpu; }
void DeleteOverlayGpu(OverlayGpu* g) { delete g; }
void ReleaseOverlayGpu(OverlayGpu* g) { g->Release(); }
bool OverlayGpuUsable(const OverlayGpu* g) { return !g->failed; }

bool OverlayKeyShows(const OverlayLayer* l, const Buffer* primaryBits, const RECT& area, const OverlayGdi* gdi);

void PrepareOverlays(OverlayGpu* g, const std::vector<OverlayLayer*>& layers, const Buffer* keyPrimary, const RECT& area,
                     const OverlayGdi* gdi) {
    if (g->layers.size() < layers.size()) g->layers.resize(layers.size());
    for (size_t i = 0; i < layers.size(); i++) {
        OverlayLayer* l = layers[i];
        OverlayGpu::Layer& c = g->layers[i];
        if (gdi && keyPrimary && (l->flags & DDOVER_KEYDESTOVERRIDE))
            l->unkeyed = !OverlayKeyShows(l, keyPrimary, area, gdi);
        const bool pal8 = l->buf->fmt.pal8;
        if (c.decoded && c.buf == l->buf && c.serial == l->serial && c.w == l->buf->w && c.h == l->buf->h &&
            (!pal8 || memcmp(c.pal, l->pal, sizeof(c.pal)) == 0))
            continue;
        const Buffer* b = l->buf;
        c.buf = b;
        c.serial = l->serial;
        c.w = b->w;
        c.h = b->h;
        if (pal8) memcpy(c.pal, l->pal, sizeof(c.pal));
        c.argb.resize(size_t(b->w) * b->h);
        if (b->fmt.fourcc) {
            DecodeFourCC(b, reinterpret_cast<uint8_t*>(c.argb.data()), LONG(b->w) * 4);
        } else {
            const RowConverter conv(b->fmt, l->pal);
            for (int y = 0; y < b->h; y++) conv.Run(b->Row(y), c.argb.data() + size_t(y) * b->w, b->w);
        }
        if (++g->decodings == 0) g->decodings = 1;  // (0: none)
        c.decoded = g->decodings;
    }
}

bool DrawOverlaysGpu(OverlayGpu* g, Gpu& gpu, IDirect3DDevice9* d, const std::vector<OverlayLayer*>& layers,
                     IDirect3DSurface9* bb, const RECT& src, const RECT& dst, IDirect3DSurface9* keySrc,
                     const RECT& keyRect, const OverlayGdi* gdi, unsigned gdiSeq, const uint8_t (*keyGamma)[256]) {
    if (g->failed) return false;
    if (!g->ps && FAILED(d->CreatePixelShader(kOverlayPs, &g->ps))) {
        g->ps = nullptr;
        g->failed = true;
        DD2D9_LOG("overlays: no pixel shader for them: laid over the frame on the CPU");
        return false;
    }
    const LONG sw = src.right - src.left, sh = src.bottom - src.top;
    if (sw <= 0 || sh <= 0 || g->layers.size() < layers.size()) return true;
    if (g->texs.size() < layers.size()) g->texs.resize(layers.size());
    gpu.CloseScene();  // (StretchRect, uploads: outside a scene)
    // The overlays' pixels that changed.
    for (size_t i = 0; i < layers.size(); i++) {
        const OverlayGpu::Layer& c = g->layers[i];
        OverlayGpu::Tex& t = g->texs[i];
        if (!c.decoded) continue;
        if (!t.tex || t.w != c.w || t.h != c.h) {
            if (t.tex) t.tex->Release();
            t = OverlayGpu::Tex();
            if (FAILED(d->CreateTexture(c.w, c.h, 1, D3DUSAGE_DYNAMIC, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &t.tex, nullptr))) {
                t.tex = nullptr;
                continue;
            }
            t.w = c.w;
            t.h = c.h;
        }
        if (t.uploaded == c.decoded) continue;
        D3DLOCKED_RECT lr;
        if (FAILED(t.tex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) continue;
        for (int y = 0; y < c.h; y++)
            memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, c.argb.data() + size_t(y) * c.w, size_t(c.w) * 4);
        t.tex->UnlockRect(0);
        t.uploaded = c.decoded;
    }
    bool anyDest = false;
    for (const OverlayLayer* l : layers) anyDest |= (l->flags & DDOVER_KEYDESTOVERRIDE) && !l->unkeyed;
    // The frame as converted, for the destination key (point sampled, its own pixels).
    if (anyDest && keySrc) {
        const UINT fw = UINT(keyRect.right - keyRect.left), fh = UINT(keyRect.bottom - keyRect.top);
        if (!g->frameTex || g->frameW != fw || g->frameH != fh) {
            if (g->frameTex) g->frameTex->Release();
            g->frameTex = nullptr;
            if (FAILED(d->CreateTexture(fw, fh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g->frameTex, nullptr)))
                g->frameTex = nullptr;
            g->frameW = fw;
            g->frameH = fh;
        }
        IDirect3DSurface9* level = nullptr;
        if (g->frameTex && SUCCEEDED(g->frameTex->GetSurfaceLevel(0, &level))) {
            d->StretchRect(keySrc, &keyRect, level, nullptr, D3DTEXF_POINT);
            level->Release();
        }
    }
    const bool useGdi = anyDest && gdi && gdi->px && gdi->w == sw && gdi->h == sh;
    if (useGdi && (!g->gdiTex || g->gdiW != gdi->w || g->gdiH != gdi->h || g->gdiSeq != gdiSeq)) {
        if (!g->gdiTex || g->gdiW != gdi->w || g->gdiH != gdi->h) {
            if (g->gdiTex) g->gdiTex->Release();
            g->gdiTex = nullptr;
            if (FAILED(d->CreateTexture(gdi->w, gdi->h, 1, D3DUSAGE_DYNAMIC, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g->gdiTex, nullptr)))
                g->gdiTex = nullptr;
            g->gdiW = gdi->w;
            g->gdiH = gdi->h;
        }
        D3DLOCKED_RECT lr;
        if (g->gdiTex && SUCCEEDED(g->gdiTex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) {
            for (LONG y = 0; y < gdi->h; y++)
                memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, gdi->px + size_t(y) * gdi->w, size_t(gdi->w) * 4);
            g->gdiTex->UnlockRect(0);
            g->gdiSeq = gdiSeq;
        }
    }
    const SavedDeviceState saved(d);
    d->SetRenderTarget(0, bb);
    d->SetDepthStencilSurface(nullptr);
    D3DSURFACE_DESC bd;
    bb->GetDesc(&bd);
    const D3DVIEWPORT9 vp = {0, 0, bd.Width, bd.Height, 0.0f, 1.0f};
    d->SetViewport(&vp);
    d->SetPixelShader(g->ps);
    d->SetVertexShader(nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE4(0));
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetRenderState(D3DRS_WRAP0, 0);
    const DWORD rs[][2] = {{D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE}, {D3DRS_CULLMODE, D3DCULL_NONE},
                           {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_FOGENABLE, FALSE},
                           {D3DRS_STENCILENABLE, FALSE}, {D3DRS_SCISSORTESTENABLE, FALSE},
                           {D3DRS_COLORWRITEENABLE, 0xF}, {D3DRS_FILLMODE, D3DFILL_SOLID}, {D3DRS_SRGBWRITEENABLE, FALSE},
                           {D3DRS_CLIPPLANEENABLE, 0}};
    for (const auto& r : rs) d->SetRenderState(D3DRENDERSTATETYPE(r[0]), r[1]);
    for (DWORD st = 0; st < 3; st++) {
        d->SetSamplerState(st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetSamplerState(st, D3DSAMP_SRGBTEXTURE, FALSE);
        if (st) {
            d->SetSamplerState(st, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            d->SetSamplerState(st, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        }
    }
    const float dw = float(dst.right - dst.left), dh = float(dst.bottom - dst.top);
    const float kx = dw / float(sw), ky = dh / float(sh);
    gpu.OpenScene();
    for (size_t i = 0; i < layers.size(); i++) {
        const OverlayLayer* l = layers[i];
        const OverlayGpu::Tex& c = g->texs[i];
        RECT vis;
        if (!c.tex || !c.uploaded || !IntersectRect(&vis, &l->dst, &src)) continue;
        const bool srcKeyed = (l->flags & DDOVER_KEYSRCOVERRIDE) != 0;
        const bool destKeyed = (l->flags & DDOVER_KEYDESTOVERRIDE) && !l->unkeyed && g->frameTex;
        const DWORD filter = srcKeyed ? D3DTEXF_POINT : D3DTEXF_LINEAR;  // (a key blurred by filtering would fringe)
        d->SetSamplerState(0, D3DSAMP_MINFILTER, filter);
        d->SetSamplerState(0, D3DSAMP_MAGFILTER, filter);
        d->SetTexture(0, c.tex);
        d->SetTexture(1, destKeyed ? static_cast<IDirect3DBaseTexture9*>(g->frameTex) : c.tex);
        d->SetTexture(2, destKeyed && useGdi && g->gdiTex ? static_cast<IDirect3DBaseTexture9*>(g->gdiTex) : c.tex);
        auto neg = [](uint32_t argb, float* out) {
            out[0] = -float((argb >> 16) & 255) / 255.0f;
            out[1] = -float((argb >> 8) & 255) / 255.0f;
            out[2] = -float(argb & 255) / 255.0f;
            out[3] = 0;
        };
        float k[6][4] = {};  // (c4: the shader's own)
        neg(l->srcKeyArgb, k[0]);
        const uint32_t dk = l->destKeyArgb;
        neg(keyGamma ? (uint32_t(keyGamma[0][(dk >> 16) & 255]) << 16) | (uint32_t(keyGamma[1][(dk >> 8) & 255]) << 8) |
                           keyGamma[2][dk & 255]
                     : dk,
            k[1]);
        neg(dk, k[5]);
        k[2][0] = destKeyed && useGdi && g->gdiTex ? 0.0f : 10.0f;
        k[2][1] = destKeyed ? 1.0f : 0.0f;
        k[2][2] = -l->destEps;
        k[3][1] = -0.5f / 255.0f;
        k[3][2] = srcKeyed ? 1.0f : 0.0f;
        d->SetPixelShaderConstantF(0, &k[0][0], 4);
        d->SetPixelShaderConstantF(5, k[5], 1);
        // Where the visible part goes in the back buffer, and the coordinates at its corners.
        const float x0 = float(dst.left) + float(vis.left - src.left) * kx - 0.5f;
        const float y0 = float(dst.top) + float(vis.top - src.top) * ky - 0.5f;
        const float x1 = float(dst.left) + float(vis.right - src.left) * kx - 0.5f;
        const float y1 = float(dst.top) + float(vis.bottom - src.top) * ky - 0.5f;
        const float osw = float(l->src.right - l->src.left), osh = float(l->src.bottom - l->src.top);
        const float odw = float(l->dst.right - l->dst.left), odh = float(l->dst.bottom - l->dst.top);
        auto ou = [&](LONG x) {
            const float t = float(x - l->dst.left) * osw / odw;
            return ((l->ddfx & DDOVERFX_MIRRORLEFTRIGHT) ? float(l->src.right) - t : float(l->src.left) + t) / float(c.w);
        };
        auto ov = [&](LONG y) {
            const float t = float(y - l->dst.top) * osh / odh;
            return ((l->ddfx & DDOVERFX_MIRRORUPDOWN) ? float(l->src.bottom) - t : float(l->src.top) + t) / float(c.h);
        };
        const float u0 = ou(vis.left), u1 = ou(vis.right), v0 = ov(vis.top), v1 = ov(vis.bottom);
        const float f0 = float(vis.left - src.left) / float(sw), f1 = float(vis.right - src.left) / float(sw);
        const float g0 = float(vis.top - src.top) / float(sh), g1 = float(vis.bottom - src.top) / float(sh);
        const float q[4][8] = {{x0, y0, 0, 1, u0, v0, f0, g0}, {x1, y0, 0, 1, u1, v0, f1, g0},
                               {x0, y1, 0, 1, u0, v1, f0, g1}, {x1, y1, 0, 1, u1, v1, f1, g1}};
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
    }
    gpu.CloseScene();
    d->SetPixelShader(nullptr);
    for (DWORD st = 0; st < 3; st++) d->SetTexture(st, nullptr);
    return true;
}

// Whether a windowed overlay's destination key shows anywhere under it (in the primary or
// the GDI picture): if not, it is drawn whole (see DrawOverlays).
bool OverlayKeyShows(const OverlayLayer* l, const Buffer* primaryBits, const RECT& area, const OverlayGdi* gdi) {
    RECT vis;
    if (!IntersectRect(&vis, &l->dst, &area)) return true;
    const DWORD destMask = primaryBits->fmt.KeyMask(), destBytes = primaryBits->fmt.Bytes();
    for (LONG y = vis.top; y < vis.bottom; y++)
        for (LONG x = vis.left; x < vis.right; x++) {
            if (y < primaryBits->h && x < primaryBits->w && y >= 0 && x >= 0 &&
                KeyMatch(RawPixel(primaryBits->Row(int(y)) + size_t(x) * destBytes, destBytes), destMask, l->destKey))
                return true;
            if (gdi && gdi->px && !primaryBits->fmt.pal8) {
                const LONG gx = x - gdi->left, gy = y - gdi->top;
                if (gx >= 0 && gy >= 0 && gx < gdi->w && gy < gdi->h &&
                    KeyMatch(ARGBToPixel(primaryBits->fmt, 0xFF000000u | gdi->px[size_t(gy) * gdi->w + gx]), destMask,
                             l->destKey))
                    return true;
            }
        }
    return false;
}

}  // namespace dd2d9
