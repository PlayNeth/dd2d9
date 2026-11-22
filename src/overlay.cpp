// Overlay surfaces (DDSCAPS_OVERLAY). Cards of the era showed them in hardware, over the
// primary surface, without touching its pixels: video players and some games' movies use
// them. Here they are emulated: UpdateOverlay and its siblings keep ddraw.dll's state and
// checks (spec: re/spec/surface-other.md, "Overlays"), and the presenter lays the visible
// overlays over the frame it converts from the primary (stretched, mirrored, colour keyed).
#include "objects.h"
#include "blitint.h"
#include "convert.h"
#include "devstate.h"
#include "fourcc.h"
#include "gpu.h"
#include "gpures.h"
#include "quadpass.h"

#include <algorithm>
#include <vector>

namespace dd2d9 {

namespace {
// As many overlays shown at once as dwMaxVisibleOverlays says (ddcaps.cpp, GetCaps).
constexpr int kMaxVisibleOverlays = 16;
// The stretch factors reported (thousandths): from 1/1000 to 100 times.
constexpr DWORD kMinStretch = 1, kMaxStretch = 100000;
// UpdateOverlayZOrder's and EnumOverlayZOrders' flags (not in mingw-w64's ddraw.h).
enum : DWORD {
    kZSendToFront, kZSendToBack, kZMoveForward, kZMoveBackward, kZInsertInFrontOf, kZInsertInBackOf
};
constexpr DWORD kEnumBackToFront = 0;  // DDENUMOVERLAYZ_BACKTOFRONT (1: _FRONTTOBACK)

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

// The surface of `dd` that `iface` is an interface of, by identity (nothing is read through
// it: the field it comes from holds a constant too); null if none.
Surface* SurfaceNamed(DDraw* dd, const void* iface) {
    if (!iface) return nullptr;
    for (const std::vector<Surface*>* list : {&dd->surfaces, &dd->detached})
        for (Surface* s : *list)
            for (int v : {1, 2, 3, 4, 7})
                if (s->Iface(v) == iface) return s;
    return nullptr;
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
        // No video port feeds it: nothing to flip it, and no bob of the hardware's (with any of
        // these, as ddraw.dll checks it: alone, DDOVER_BOBHARDWARE is taken). Bob itself (of
        // interleaved fields or of one field a surface, shown by the flips naming the field,
        // DDCAPS2_CANBOBINTERLEAVED | CANBOBNONINTERLEAVED | CANFLIPODDEVEN): see DrawOverlays.
        if (f & (DDOVER_AUTOFLIP | DDOVER_BOBHARDWARE)) return DDERR_INVALIDPARAMS;
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
    // The alpha surfaces (the DirectX 7 SDK's, which ddraw.dll checks nothing of: see TakeAlpha).
    Surface* alphaSrc = (f & DDOVER_ALPHASRCSURFACEOVERRIDE) ? SurfaceNamed(dd, keep.lpDDSAlphaSrc) : nullptr;
    Surface* alphaDest = (f & DDOVER_ALPHADESTSURFACEOVERRIDE) ? SurfaceNamed(dd, keep.lpDDSAlphaDest) : nullptr;
    // Every member of the chain takes the position (only this one becomes visible).
    for (Surface* m : OverlayChain(this)) {
        m->overlaySrc = sr;
        m->overlayDst = d;
        m->overlayFlags = f;
        m->overlayFx = keep;
        m->overlayAlphaSrc = alphaSrc;
        m->overlayAlphaDest = alphaDest;
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
    // (Overlays on this surface hold references to it: none is left.) One that took its alpha
    // from it has none from it now.
    if (g_overlayZ)
        for (Surface* s : *g_overlayZ) {
            if (s->overlayDest == this) s->overlayDest = nullptr;
            if (s->overlayAlphaSrc != this && s->overlayAlphaDest != this) continue;
            if (s->overlayAlphaSrc == this) s->overlayAlphaSrc = nullptr;
            if (s->overlayAlphaDest == this) s->overlayAlphaDest = nullptr;
            if (s->OverlayShown()) s->overlayDest->dd->MarkOverlayDirty();
        }
}

void OverlayAlphaDrawn(const Surface* s) {
    if (g_overlayZ)
        for (const Surface* o : *g_overlayZ)
            if ((o->overlayAlphaSrc == s || o->overlayAlphaDest == s) && o->OverlayShown())
                o->overlayDest->dd->MarkOverlayDirty();
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
    // format and palette, a FourCC source key as it decodes): the ends of a range (of an RGB
    // format: an index range or a YUV one compares its low end alone there), and its tolerance.
    uint32_t destKeyArgb, srcKeyArgb, destKeyHiArgb, srcKeyHiArgb;
    float destEps;
    bool unkeyed = false;  // (PrepareOverlays) a windowed key shown nowhere under it: drawn whole
    // Bob (DDOVER_BOB, a field named by the last flip): the field shown (0 even, 1 odd; -1 the
    // whole surface), its own lines alone when the surface interleaves both (src then in rows of
    // the field), as the odd lines of a frame half a line of the field lower.
    int field = -1;
    bool interleaved = false;
    ColorTransform color;  // its colour controls (IDirectDrawColorControl)
    bool colored = false;  // ...not the defaults
    // Alpha (DDOVER_ALPHA*, as the DirectX 7 SDK has it; the Voodoo3 has none): where it's shown,
    // the overlay over the frame by the weight w = (alpha * alphaScale + alphaBias) * alphaDest
    // (SourceWeight, DestWeight). Its alpha: an alpha surface's (ALPHASRCSURFACEOVERRIDE: any
    // surface with alpha in its format, its whole area over the overlay's), the pixel's own (its
    // format's: ALPHASRC) or a constant (ALPHASRCCONSTOVERRIDE); edge blending's constant
    // (ALPHAEDGEBLEND) instead where it borders its source key. The destination's, the frame's
    // opacity over it: an alpha surface's (ALPHADESTSURFACEOVERRIDE, over the primary's whole
    // area) or a constant (ALPHADESTCONSTOVERRIDE). Each a transparency with its NEG flag. (The
    // primary's format has no alpha, and no alpha channel can be attached to it: DDOVER_ALPHADEST
    // gives none.)
    float alphaScale = 0, alphaBias = 1, alphaDest = 1;
    Buffer* alphaSrcBuf = nullptr;   // the source alpha surface's pixels (referenced)
    Buffer* alphaDestBuf = nullptr;  // ...the destination's
    unsigned alphaSrcSerial = 0, alphaDestSerial = 0;  // (as serial)
    bool alphaDestNeg = false;
    LONG destW = 0, destH = 0;  // the primary's size, which the destination's covers
    float edgeAlpha = -1;       // edge blending's weight (-1: none)
    bool Opaque() const { return alphaScale == 0 && alphaBias == 1 && alphaDest == 1 && !alphaDestBuf && edgeAlpha < 0; }
    // The weight varies from pixel to pixel beyond the pixels' own alpha: the GPU path puts it in
    // the texels' alpha (PrepareOverlays).
    bool TexelWeights() const { return alphaSrcBuf || edgeAlpha >= 0; }
    // The source row (of the field) at t rows down the source rectangle (FieldRow: the surface's).
    float BobShift() const { return field > 0 ? 0.5f : 0.0f; }
    LONG FieldRow(LONG row) const { return interleaved && field >= 0 ? 2 * row + field : row; }
};

// An alpha constant of `bits` bits as 0 to 1 (its largest value 1); -1: no bits given.
static float AlphaConstant(DWORD value, DWORD bits) {
    if (!bits || bits > 32) return -1.0f;
    const DWORD most = bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
    return float(double(std::min(value, most)) / most);
}

// An alpha surface's pixels, referenced, with their serial (as OverlayLayer's); null if it has no
// alpha in its format (none taken from it, as if not named) or is lost.
static Buffer* TakeAlphaSurface(Surface* a, unsigned* serial) {
    if (!a || a->IsLost(false)) return nullptr;
    const Format& f = a->buf->fmt;
    if (!f.a || f.fourcc || f.Palettized() || f.zbuf || f.bump) return nullptr;
    if (a->buf->gpu && a->buf->cpuStale) GpuCpuAccess(a, false);
    *serial = (a->lockCount || a->dcOut) ? NextCpuSerial() : a->buf->cpuSerial;  // (see TakeOverlays)
    a->buf->AddRef();
    return a->buf;
}

// The layer's alpha from overlay `s`'s flags, UpdateOverlay's DDOVERLAYFX and alpha surfaces
// (see OverlayLayer); the alpha surfaces before the constants, which share their fields.
static void TakeAlpha(OverlayLayer* l, const Surface* s, const Surface* primary) {
    const DDOVERLAYFX& fx = s->overlayFx;
    const Format& f = s->buf->fmt;
    const DWORD fl = l->flags;
    const bool neg = (fl & DDOVER_ALPHASRCNEG) != 0;
    if (fl & DDOVER_ALPHASRCSURFACEOVERRIDE) l->alphaSrcBuf = TakeAlphaSurface(s->overlayAlphaSrc, &l->alphaSrcSerial);
    const float c = !l->alphaSrcBuf && (fl & DDOVER_ALPHASRCCONSTOVERRIDE)
                        ? AlphaConstant(fx.dwAlphaSrcConst, fx.dwAlphaSrcConstBitDepth)
                        : -1.0f;
    if (c >= 0) {
        l->alphaBias = neg ? 1 - c : c;
    } else if (l->alphaSrcBuf || ((fl & DDOVER_ALPHASRC) && f.a && !f.fourcc && !f.Palettized())) {
        l->alphaScale = neg ? -1.0f : 1.0f;
        l->alphaBias = neg ? 1.0f : 0.0f;
    }
    if ((fl & DDOVER_ALPHAEDGEBLEND) && (fl & DDOVER_KEYSRCOVERRIDE))
        l->edgeAlpha = AlphaConstant(fx.dwAlphaEdgeBlend, fx.dwAlphaEdgeBlendBitDepth);
    l->alphaDestNeg = (fl & DDOVER_ALPHADESTNEG) != 0;
    if (fl & DDOVER_ALPHADESTSURFACEOVERRIDE) l->alphaDestBuf = TakeAlphaSurface(s->overlayAlphaDest, &l->alphaDestSerial);
    const float d = !l->alphaDestBuf && (fl & DDOVER_ALPHADESTCONSTOVERRIDE)
                        ? AlphaConstant(fx.dwAlphaDestConst, fx.dwAlphaDestConstBitDepth)
                        : -1.0f;
    if (d >= 0) l->alphaDest = l->alphaDestNeg ? d : 1 - d;
    l->destW = primary->buf->w;
    l->destH = primary->buf->h;
}

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
        l->field = (l->flags & DDOVER_BOB) ? s->overlayField : -1;
        l->interleaved = (l->flags & DDOVER_INTERLEAVED) != 0;
        if (l->interleaved && l->field >= 0) {
            // The source rectangle in rows of the field (those of its lines inside it).
            auto toField = [&](LONG y) { return (y - l->field + 1) / 2; };
            const LONG rows = toField(s->buf->h);
            l->src.top = std::min(toField(l->src.top), rows - 1);
            l->src.bottom = std::max(toField(l->src.bottom), l->src.top + 1);
        }
        l->colored = s->colorControls && ColorTransformOf(*s->colorControls, &l->color);
        TakeAlpha(l, s, primary);
        l->ddfx = (s->overlayFlags & DDOVER_DDFX) ? s->overlayFx.dwDDFX : 0;
        l->destKey = s->overlayFx.dckDestColorkey;
        l->srcKey = s->overlayFx.dckSrcColorkey;
        // Locked or with a DC out, the memory may be half written: taken now, but not cached
        // as that serial (the serial changes at Lock, not at Unlock, and the finished picture
        // would never be decoded again: a torn overlay until the game next touched it).
        l->serial = (s->lockCount || s->dcOut) ? NextCpuSerial() : s->buf->cpuSerial;
        const Format& pf = primary->buf->fmt;
        const Palette* pp = pf.pal8 ? primary->ShownPalette() : nullptr;
        const PALETTEENTRY* ppe = pp ? pp->entries : primary->dd->defaultPalette;
        l->destKeyArgb = PixelToARGB(pf, l->destKey.dwColorSpaceLowValue, ppe);
        l->destKeyHiArgb = pf.pal8 ? l->destKeyArgb : PixelToARGB(pf, l->destKey.dwColorSpaceHighValue, ppe);
        // (16-bit and smaller: the frame's colours come out of the GPU's own widening.)
        l->destEps = (pf.bpp <= 16 && !pf.pal8 ? 4.5f : 0.5f) / 255.0f;
        const Format& f = l->buf->fmt;
        l->srcKeyArgb = f.fourcc ? FourCCKeyToArgb(f.fourCC, l->srcKey.dwColorSpaceLowValue)
                                 : PixelToARGB(f, l->srcKey.dwColorSpaceLowValue, l->pal);
        l->srcKeyHiArgb = f.fourcc || f.Palettized() ? l->srcKeyArgb : PixelToARGB(f, l->srcKey.dwColorSpaceHighValue, l->pal);
        out->push_back(l);
    }
}

// A pixel of format `f` in an overlay key: a range compared colour by colour, as blits do.
static bool KeyMatch(uint32_t v, const Format& f, const DDCOLORKEY& k) {
    return KeyHits(Masked({true, k.dwColorSpaceLowValue, k.dwColorSpaceHighValue}, f), v);
}

bool OverlaysKeyedOnDest(const std::vector<OverlayLayer*>& layers) {
    for (const OverlayLayer* l : layers)
        if (l->flags & DDOVER_KEYDESTOVERRIDE) return true;
    return false;
}

// The destination key at x, y of the primary: its raw pixel, or (a windowed program) what GDI
// drew in its window there, as the screen the hardware compared held both.
static bool DestKeyAt(const OverlayLayer* l, const Buffer* primaryBits, const OverlayGdi* gdi, LONG x, LONG y) {
    const Format& pf = primaryBits->fmt;
    const DWORD destBytes = pf.Bytes();
    if (y >= 0 && y < primaryBits->h && x >= 0 && x < primaryBits->w &&
        KeyMatch(ReadPixel(primaryBits->Row(int(y)) + size_t(x) * destBytes, destBytes), pf, l->destKey))
        return true;
    if (gdi && gdi->px && !pf.pal8) {  // (no colour-to-index lookup for an 8-bit desktop)
        const LONG gx = x - gdi->left, gy = y - gdi->top;
        if (gx >= 0 && gy >= 0 && gx < gdi->w && gy < gdi->h &&
            KeyMatch(ARGBToPixel(pf, 0xFF000000u | gdi->px[size_t(gy) * gdi->w + gx]), pf, l->destKey))
            return true;
    }
    return false;
}

// Whether a windowed overlay's destination key shows anywhere under it (in the primary or
// the GDI picture): if not, it is drawn whole (painted where dd2d9 doesn't see it, the
// overlay is shown rather than not at all).
static bool OverlayKeyShows(const OverlayLayer* l, const Buffer* primaryBits, const RECT& area, const OverlayGdi* gdi) {
    RECT vis;
    if (!IntersectRect(&vis, &l->dst, &area)) return true;
    for (LONG y = vis.top; y < vis.bottom; y++)
        for (LONG x = vis.left; x < vis.right; x++)
            if (DestKeyAt(l, primaryBits, gdi, x, y)) return true;
    return false;
}

// The alpha (0 to 255) of alpha surface `a` at x, y of the w by h picture it covers, its whole
// area over the picture's.
static uint32_t AlphaAt(const Buffer* a, LONG x, LONG y, LONG w, LONG h) {
    const LONG ax = std::min(LONG(int64_t(2 * x + 1) * a->w / (2 * w)), LONG(a->w) - 1);
    const LONG ay = std::min(LONG(int64_t(2 * y + 1) * a->h / (2 * h)), LONG(a->h) - 1);
    const DWORD bytes = a->fmt.Bytes();
    return PixelToARGB(a->fmt, ReadPixel(a->Row(int(ay)) + size_t(ax) * bytes, bytes), nullptr) >> 24;
}

// Whether the overlay's pixel at x, y of its surface is its source key's (`decoded`: a FourCC
// overlay's pixels, compared as the colour they decode to).
static bool SrcKeyedAt(const OverlayLayer* l, const uint32_t* decoded, LONG x, LONG y) {
    const Buffer* b = l->buf;
    if (b->fmt.fourcc) return ((decoded[size_t(y) * b->w + x] ^ l->srcKeyArgb) & 0xFFFFFF) == 0;
    const DWORD bytes = b->fmt.Bytes();
    return KeyMatch(ReadPixel(b->Row(int(y)) + size_t(x) * bytes, bytes), b->fmt, l->srcKey);
}

// The overlay's own weight at its pixel x, y (`argb`, as decoded) where it's shown: edge
// blending's beside a pixel of the source key (left, right, above or below it; for a bobbed
// field of an interleaving surface, in the field's lines), else its alpha (the alpha surface's
// or the pixel's) by the layer's scale and bias.
static float SourceWeight(const OverlayLayer* l, const uint32_t* decoded, LONG x, LONG y, uint32_t argb) {
    if (l->edgeAlpha >= 0) {
        const LONG step = l->interleaved && l->field >= 0 ? 2 : 1;
        if ((x > 0 && SrcKeyedAt(l, decoded, x - 1, y)) || (x + 1 < l->buf->w && SrcKeyedAt(l, decoded, x + 1, y)) ||
            (y >= step && SrcKeyedAt(l, decoded, x, y - step)) || (y + step < l->buf->h && SrcKeyedAt(l, decoded, x, y + step)))
            return l->edgeAlpha;
    }
    const uint32_t a = l->alphaSrcBuf ? AlphaAt(l->alphaSrcBuf, x, y, l->buf->w, l->buf->h) : argb >> 24;
    return float(a) / 255.0f * l->alphaScale + l->alphaBias;
}

// The frame's transparency over the overlay at x, y of the primary (alphaDest): from the
// destination alpha surface there, else the constant's.
static float DestWeight(const OverlayLayer* l, LONG x, LONG y) {
    if (!l->alphaDestBuf) return l->alphaDest;
    const float a = float(AlphaAt(l->alphaDestBuf, x, y, l->destW, l->destH)) / 255.0f;
    return l->alphaDestNeg ? a : 1 - a;
}

// An overlay's pixel `argb` as shown over the frame's `under`: through its colour controls, then
// by its weight `w` (as the GPU path's shader and blending make it).
static uint32_t Shade(const OverlayLayer* l, uint32_t argb, uint32_t under, float w) {
    float c[3] = {float((argb >> 16) & 255) / 255.0f, float((argb >> 8) & 255) / 255.0f, float(argb & 255) / 255.0f};
    if (l->colored) {
        const float in[3] = {c[0], c[1], c[2]};
        for (int k = 0; k < 3; k++) {
            const float* m = l->color.m[k];
            c[k] = m[0] * in[0] + m[1] * in[1] + m[2] * in[2] + m[3];
        }
    }
    w = std::clamp(w, 0.0f, 1.0f);
    uint32_t out = 0xFF000000u;
    for (int k = 0; k < 3; k++) {
        const int shift = 16 - 8 * k;
        const float v = std::clamp(c[k], 0.0f, 1.0f) * w + float((under >> shift) & 255) / 255.0f * (1 - w);
        out |= uint32_t(v * 255.0f + 0.5f) << shift;
    }
    return out;
}

void DrawOverlays(const std::vector<OverlayLayer*>& layers, const Buffer* primaryBits, const RECT& area, uint32_t* frame,
                  size_t pitch, const OverlayGdi* gdi) {
    std::vector<uint32_t> decoded;
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
        const bool destKeyed = (l->flags & DDOVER_KEYDESTOVERRIDE) && (!gdi || OverlayKeyShows(l, primaryBits, area, gdi));
        const uint32_t* const dec = f.fourcc ? decoded.data() : nullptr;
        const DWORD srcBytes = f.Bytes();
        const bool plain = !l->colored && l->Opaque();
        const LONG dw = l->dst.right - l->dst.left, dh = l->dst.bottom - l->dst.top;
        const LONG sw = l->src.right - l->src.left, sh = l->src.bottom - l->src.top;
        for (LONG y = vis.top; y < vis.bottom; y++) {
            // (Bob's odd field half a line lower: a row less down where it starts, clamped.)
            LONG sy = std::max<LONG>(LONG((int64_t(2 * (y - l->dst.top) + 1) * sh - (l->field > 0 ? dh : 0)) / (2 * dh)), 0);
            if (l->ddfx & DDOVERFX_MIRRORUPDOWN) sy = sh - 1 - sy;
            sy = l->FieldRow(sy + l->src.top);
            uint32_t* row = frame + size_t(y - area.top) * pitch - area.left;
            for (LONG x = vis.left; x < vis.right; x++) {
                // Shown only where the destination has its key colour.
                if (destKeyed && !DestKeyAt(l, primaryBits, gdi, x, y)) continue;
                LONG sx = LONG((int64_t(2 * (x - l->dst.left) + 1) * sw) / (2 * dw));
                if (l->ddfx & DDOVERFX_MIRRORLEFTRIGHT) sx = sw - 1 - sx;
                sx += l->src.left;
                if (srcKeyed && SrcKeyedAt(l, dec, sx, sy)) continue;
                const uint32_t argb =
                    f.fourcc ? dec[size_t(sy) * b->w + sx]
                             : PixelToARGB(f, ReadPixel(b->Row(int(sy)) + size_t(sx) * srcBytes, srcBytes), l->pal);
                row[x] = plain ? argb | 0xFF000000
                               : Shade(l, argb, row[x], SourceWeight(l, dec, sx, sy, argb) * DestWeight(l, x, y));
            }
        }
    }
}

void DropOverlays(std::vector<OverlayLayer*>* layers) {
    for (OverlayLayer* l : *layers) {
        for (Buffer* b : {l->buf, l->alphaSrcBuf, l->alphaDestBuf})
            if (b) b->Release();
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

// ps_2_0. t0: the overlay's texture coordinates, t1: the frame's (and GDI's), t2: the destination
// alpha surface's. s0 the overlay, s1 the frame, s2 the GDI picture, s3 the destination alpha
// surface. A key is the middle of its range (a colour: the colour), compared channel by channel
// within half its width: c0 = -source key, c6 = -half its width, c3 = (0, -its tolerance, source
// keyed, 0); c1 = -destination key as the frame has it and c7 = -half its width (c5, c8 as GDI's
// picture has them), c2 = ((no GDI picture) * 10, destination keyed, -its tolerance, 0). Killed
// where the source key matches (keyed) or the destination key matches neither the frame nor the
// GDI picture (keyed). What's drawn goes through the overlay's colour controls last: c9..c11, the
// transform's rows (ColorTransform); its alpha the overlay's weight (blended by it): (texel's alpha
// * c12.x + c12.y) * (s3's alpha * c13.x + c13.y), SourceWeight's and DestWeight's. Written to the
// presenter's shaders' rules (registers set whole first, one constant per instruction, no negated
// constants, only the swizzles ps_2_0 has). Assembled (psasm) from:
//   def c4, 1, 0, 10, -1 / dcl t0.xy..t2.xy / dcl_2d s0..s3 / r3, r4, r6..r8, r11 = c4.y / r9 = c4.x
//   texld r0, t0, s0 / texld r1, t1, s1 / texld r2, t1, s2 / texld r5, t2, s3
//   r3.x = max(|r0.rgb + c0| + c6) + c3.y / r4.x = (r3.x + c4.w) * c3.z + 1
//   r6.x = max(|r1.rgb + c1| + c7) / r7.x = max(|r2.rgb + c5| + c8) + c2.x / r6.x = min(r6.x, r7.x) + c2.z
//   r8.x = (-r6.x + c4.w) * c2.y + 1 / r4 = min(r4.x, r8.x) / texkill r4
//   r11.x = (r0.w * c12.x + c12.y) * (r5.w * c13.x + c13.y) / r0.w = 1
//   mov r10, c4.y / r10.x..z = dp4(r0, c9..c11) / r10.w = r11.x / oC0 = r10
static const DWORD kOverlayPs[] = {
    0xffff0200, 0x0200001f, 0x80000000, 0xb0030000, 0x0200001f, 0x80000000, 0xb0030001, 0x0200001f, 0x80000000, 0xb0030002, 0x0200001f, 0x90000000, 0xa00f0800, 0x0200001f, 0x90000000, 0xa00f0801, 0x0200001f, 0x90000000, 0xa00f0802, 0x0200001f, 0x90000000, 0xa00f0803, 0x05000051, 0xa00f0004, 0x3f800000, 0x00000000, 0x41200000, 0xbf800000, 0x02000001, 0x800f0003, 0xa0550004, 0x02000001, 0x800f0004, 0xa0550004, 0x02000001, 0x800f0006, 0xa0550004, 0x02000001, 0x800f0007, 0xa0550004, 0x02000001, 0x800f0008, 0xa0550004, 0x02000001, 0x800f0009, 0xa0000004, 0x02000001, 0x800f000b, 0xa0550004, 0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x03000042, 0x800f0001, 0xb0e40001, 0xa0e40801, 0x03000042, 0x800f0002, 0xb0e40001, 0xa0e40802, 0x03000042, 0x800f0005, 0xb0e40002, 0xa0e40803, 0x03000002, 0x80070003, 0x80e40000, 0xa0e40000, 0x02000023, 0x80070003, 0x80e40003, 0x03000002, 0x80070003, 0x80e40003, 0xa0e40006, 0x0300000b, 0x80010003, 0x80000003, 0x80550003, 0x0300000b, 0x80010003, 0x80000003, 0x80aa0003, 0x03000002, 0x80010003, 0x80000003, 0xa0550003, 0x03000002, 0x80010004, 0x80000003, 0xa0ff0004, 0x04000004, 0x80010004, 0x80000004, 0xa0aa0003, 0x80000009, 0x03000002, 0x80070006, 0x80e40001, 0xa0e40001, 0x02000023, 0x80070006, 0x80e40006, 0x03000002, 0x80070006, 0x80e40006, 0xa0e40007, 0x0300000b, 0x80010006, 0x80000006, 0x80550006, 0x0300000b, 0x80010006, 0x80000006, 0x80aa0006, 0x03000002, 0x80070007, 0x80e40002, 0xa0e40005, 0x02000023, 0x80070007, 0x80e40007, 0x03000002, 0x80070007, 0x80e40007, 0xa0e40008, 0x0300000b, 0x80010007, 0x80000007, 0x80550007, 0x0300000b, 0x80010007, 0x80000007, 0x80aa0007, 0x03000002, 0x80010007, 0x80000007, 0xa0000002, 0x0300000a, 0x80010006, 0x80000006, 0x80000007, 0x03000002, 0x80010006, 0x80000006, 0xa0aa0002, 0x03000002, 0x80010008, 0x81000006, 0xa0ff0004, 0x04000004, 0x80010008, 0x80000008, 0xa0550002, 0x80000009, 0x0300000a, 0x80010004, 0x80000004, 0x80000008, 0x02000001, 0x800f0004, 0x80000004, 0x01000041, 0x800f0004, 0x04000004, 0x8001000b, 0x80ff0000, 0xa000000c, 0xa055000c, 0x04000004, 0x80010005, 0x80ff0005, 0xa000000d, 0xa055000d, 0x03000005, 0x8001000b, 0x8000000b, 0x80000005, 0x02000001, 0x80080000, 0x80000009, 0x02000001, 0x800f000a, 0xa0550004, 0x03000009, 0x8001000a, 0x80e40000, 0xa0e40009, 0x03000009, 0x8002000a, 0x80e40000, 0xa0e4000a, 0x03000009, 0x8004000a, 0x80e40000, 0xa0e4000b, 0x02000001, 0x8008000a, 0x8000000b, 0x02000001, 0x800f0800, 0x80e4000a, 0x0000ffff};

// What an overlay's texels' alpha holds beyond its pixels' (OverlayLayer::TexelWeights): the
// decoding is current while it's the same.
struct TexelAlpha {
    const Buffer* surf = nullptr;
    unsigned serial = 0;
    float scale = 0, bias = 0, edge = -1;
    DWORD keyLo = 0, keyHi = 0;
    bool operator==(const TexelAlpha& o) const {
        return surf == o.surf && serial == o.serial && scale == o.scale && bias == o.bias && edge == o.edge &&
               keyLo == o.keyLo && keyHi == o.keyHi;
    }
};

static TexelAlpha TexelAlphaOf(const OverlayLayer* l) {
    TexelAlpha t;
    if (l->TexelWeights())
        t = {l->alphaSrcBuf, l->alphaSrcSerial, l->alphaScale, l->alphaBias, l->edgeAlpha, l->srcKey.dwColorSpaceLowValue,
             l->srcKey.dwColorSpaceHighValue};
    return t;
}

class OverlayGpu {
public:
    // Each overlay's pixels as decoded: the presenter thread's alone (PrepareOverlays, no lock;
    // DrawOverlaysGpu reads them).
    struct Layer {
        const Buffer* buf = nullptr;
        unsigned serial = 0;
        int w = 0, h = 0;
        PALETTEENTRY pal[256];       // (a palettized overlay's colours when decoded)
        int rows = -1;               // bob: the field of an interleaving surface decoded alone (-1: all)
        TexelAlpha texelAlpha;       // (the weights in argb's alpha: see TexelWeights)
        std::vector<uint32_t> argb;
        unsigned decoded = 0;        // which decoding argb holds (0: none)
        // The destination alpha surface's alpha (in the texels' alpha), at its own size.
        const Buffer* destBuf = nullptr;
        unsigned destSerial = 0;
        int destW = 0, destH = 0;
        std::vector<uint32_t> destArgb;
        unsigned destDecoded = 0;
    };
    std::vector<Layer> layers;
    // Their textures: Gpu lock only (Release can come from the game's thread on a device reset).
    struct Tex {
        IDirect3DTexture9* tex = nullptr;
        int w = 0, h = 0;
        unsigned uploaded = 0;  // the decoding it holds
        // `px` (pw by ph) uploaded, unless it holds decoding `decoding` already; false: no texture.
        bool Hold(IDirect3DDevice9* d, const std::vector<uint32_t>& px, int pw, int ph, unsigned decoding) {
            if (!tex || w != pw || h != ph) {
                Release();
                if (FAILED(d->CreateTexture(pw, ph, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr))) {
                    tex = nullptr;
                    return false;
                }
                w = pw;
                h = ph;
            }
            D3DLOCKED_RECT lr;
            if (uploaded != decoding && SUCCEEDED(tex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) {
                for (int y = 0; y < h; y++)
                    memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, px.data() + size_t(y) * w, size_t(w) * 4);
                tex->UnlockRect(0);
                uploaded = decoding;
            }
            return uploaded != 0;
        }
        void Release() {
            if (tex) tex->Release();
            *this = Tex();
        }
    };
    std::vector<Tex> texs, destTexs;  // each overlay's, and its destination alpha surface's
    unsigned decodings = 0;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DTexture9* frameTex = nullptr;
    UINT frameW = 0, frameH = 0;
    IDirect3DTexture9* gdiTex = nullptr;
    LONG gdiW = 0, gdiH = 0;
    unsigned gdiSeq = 0;
    bool failed = false;  // no pixel shaders: the CPU path from now on

    unsigned NextDecoding() {
        if (++decodings == 0) decodings = 1;  // (0: none)
        return decodings;
    }
    void Release() {
        for (Tex& t : texs) t.Release();
        for (Tex& t : destTexs) t.Release();
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

void PrepareOverlays(OverlayGpu* g, const std::vector<OverlayLayer*>& layers, const Buffer* keyPrimary, const RECT& area,
                     const OverlayGdi* gdi) {
    if (g->layers.size() < layers.size()) g->layers.resize(layers.size());
    for (size_t i = 0; i < layers.size(); i++) {
        OverlayLayer* l = layers[i];
        OverlayGpu::Layer& c = g->layers[i];
        if (gdi && keyPrimary && (l->flags & DDOVER_KEYDESTOVERRIDE))
            l->unkeyed = !OverlayKeyShows(l, keyPrimary, area, gdi);
        // The destination alpha surface's alpha, as AlphaAt reads it, when it changed.
        const Buffer* a = l->alphaDestBuf;
        if (a && !(c.destDecoded && c.destBuf == a && c.destSerial == l->alphaDestSerial && c.destW == a->w &&
                   c.destH == a->h)) {
            c.destBuf = a;
            c.destSerial = l->alphaDestSerial;
            c.destW = a->w;
            c.destH = a->h;
            c.destArgb.resize(size_t(a->w) * a->h);
            const DWORD bytes = a->fmt.Bytes();
            for (int y = 0; y < a->h; y++)
                for (int x = 0; x < a->w; x++)
                    c.destArgb[size_t(y) * a->w + x] =
                        PixelToARGB(a->fmt, ReadPixel(a->Row(y) + size_t(x) * bytes, bytes), nullptr) & 0xFF000000u;
            c.destDecoded = g->NextDecoding();
        }
        const bool pal8 = l->buf->fmt.pal8;
        // (Bob's field of an interleaving surface: its lines alone, as the source rectangle has them.)
        const int rows = l->interleaved ? l->field : -1;
        const int h = rows >= 0 ? (l->buf->h - rows + 1) / 2 : l->buf->h;
        const TexelAlpha texelAlpha = TexelAlphaOf(l);
        if (c.decoded && c.buf == l->buf && c.serial == l->serial && c.w == l->buf->w && c.h == h && c.rows == rows &&
            c.texelAlpha == texelAlpha && (!pal8 || memcmp(c.pal, l->pal, sizeof(c.pal)) == 0))
            continue;
        const Buffer* b = l->buf;
        c.buf = b;
        c.serial = l->serial;
        c.w = b->w;
        c.h = h;
        c.rows = rows;
        c.texelAlpha = texelAlpha;
        if (pal8) memcpy(c.pal, l->pal, sizeof(c.pal));
        c.argb.resize(size_t(b->w) * b->h);
        if (b->fmt.fourcc) {
            DecodeFourCC(b, reinterpret_cast<uint8_t*>(c.argb.data()), LONG(b->w) * 4);
        } else {
            const RowConverter conv(b->fmt, l->pal);
            for (int y = 0; y < b->h; y++) conv.Run(b->Row(y), c.argb.data() + size_t(y) * b->w, b->w);
        }
        // The weights in the texels' alpha (the shader takes them as they are), the field's lines alone.
        if (l->TexelWeights()) {
            const uint32_t* const dec = b->fmt.fourcc ? c.argb.data() : nullptr;  // (the keys don't see the alpha)
            for (int y = std::max(rows, 0); y < b->h; y += rows >= 0 ? 2 : 1)
                for (int x = 0; x < b->w; x++) {
                    uint32_t& p = c.argb[size_t(y) * b->w + x];
                    const float w = std::clamp(SourceWeight(l, dec, x, y, p), 0.0f, 1.0f);
                    p = (p & 0xFFFFFFu) | uint32_t(w * 255.0f + 0.5f) << 24;
                }
        }
        if (rows >= 0) {
            for (int y = 0; y < h; y++)
                memmove(c.argb.data() + size_t(y) * b->w, c.argb.data() + size_t(2 * y + rows) * b->w, size_t(b->w) * 4);
            c.argb.resize(size_t(b->w) * h);
        }
        c.decoded = g->NextDecoding();
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
    if (g->destTexs.size() < layers.size()) g->destTexs.resize(layers.size());
    gpu.CloseScene();  // (StretchRect, uploads: outside a scene)
    // The overlays' pixels that changed, and their destination alpha surfaces'.
    for (size_t i = 0; i < layers.size(); i++) {
        const OverlayGpu::Layer& c = g->layers[i];
        if (c.decoded) g->texs[i].Hold(d, c.argb, c.w, c.h, c.decoded);
        if (layers[i]->alphaDestBuf && c.destDecoded) g->destTexs[i].Hold(d, c.destArgb, c.destW, c.destH, c.destDecoded);
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
    QuadPass pass(gpu, d, g->ps, D3DFVF_XYZRHW | D3DFVF_TEX3);  // (t0, t1, t2: two coordinates each)
    pass.Target(bb);
    const float dw = float(dst.right - dst.left), dh = float(dst.bottom - dst.top);
    const float kx = dw / float(sw), ky = dh / float(sh);
    for (size_t i = 0; i < layers.size(); i++) {
        const OverlayLayer* l = layers[i];
        const OverlayGpu::Tex& c = g->texs[i];
        RECT vis;
        if (!c.tex || !c.uploaded || !IntersectRect(&vis, &l->dst, &src)) continue;
        const bool srcKeyed = (l->flags & DDOVER_KEYSRCOVERRIDE) != 0;
        const bool destKeyed = (l->flags & DDOVER_KEYDESTOVERRIDE) && !l->unkeyed && g->frameTex;
        // (Point sampled when source-keyed: a key blurred by filtering would fringe.)
        pass.Texture(0, c.tex, srcKeyed ? D3DTEXF_POINT : D3DTEXF_LINEAR);
        pass.Texture(1, destKeyed ? static_cast<IDirect3DBaseTexture9*>(g->frameTex) : c.tex);
        pass.Texture(2, destKeyed && useGdi && g->gdiTex ? static_cast<IDirect3DBaseTexture9*>(g->gdiTex) : c.tex);
        const OverlayGpu::Tex& da = g->destTexs[i];
        const bool destAlpha = l->alphaDestBuf && da.tex && da.uploaded;
        pass.Texture(3, destAlpha ? da.tex : c.tex);
        // A key from lo to hi, as its middle (negated) and half its width (negated); a channel's
        // high end below its low one taken as the low one, as on the CPU (Masked).
        auto key = [](uint32_t lo, uint32_t hi, float* mid, float* half) {
            for (int c = 0; c < 3; c++) {
                const float a = float((lo >> (16 - 8 * c)) & 255) / 255.0f, b = float((hi >> (16 - 8 * c)) & 255) / 255.0f;
                const float h = b > a ? (b - a) / 2 : 0.0f;
                mid[c] = -(a + h);
                half[c] = -h;
            }
            mid[3] = half[3] = 0;
        };
        auto gamma = [&](uint32_t v) {
            return keyGamma ? (uint32_t(keyGamma[0][(v >> 16) & 255]) << 16) | (uint32_t(keyGamma[1][(v >> 8) & 255]) << 8) |
                                  keyGamma[2][v & 255]
                            : v;
        };
        float k[9][4] = {};  // (c4: the shader's own)
        key(l->srcKeyArgb, l->srcKeyHiArgb, k[0], k[6]);
        key(gamma(l->destKeyArgb), gamma(l->destKeyHiArgb), k[1], k[7]);
        key(l->destKeyArgb, l->destKeyHiArgb, k[5], k[8]);
        k[2][0] = destKeyed && useGdi && g->gdiTex ? 0.0f : 10.0f;
        k[2][1] = destKeyed ? 1.0f : 0.0f;
        k[2][2] = -l->destEps;
        k[3][1] = -0.5f / 255.0f;
        k[3][2] = srcKeyed ? 1.0f : 0.0f;
        d->SetPixelShaderConstantF(0, &k[0][0], 4);
        d->SetPixelShaderConstantF(5, k[5], 4);
        d->SetPixelShaderConstantF(9, &l->color.m[0][0], 3);
        // The weight: the texel's alpha by the layer's scale and bias (as it is where the decoding
        // put the weights there), times the destination's factor: 1 - s3's alpha (with NEG, s3's
        // alpha), or without a destination alpha surface the constant's.
        const bool texelWeights = l->TexelWeights();
        const float alpha[2][4] = {{texelWeights ? 1.0f : l->alphaScale, texelWeights ? 0.0f : l->alphaBias, 0, 0},
                                   {destAlpha ? (l->alphaDestNeg ? 1.0f : -1.0f) : 0.0f,
                                    destAlpha ? (l->alphaDestNeg ? 0.0f : 1.0f) : l->alphaDest, 0, 0}};
        d->SetPixelShaderConstantF(12, alpha[0], 2);
        // (Blended by its weight, the shader's alpha, unless it has none.)
        pass.State(D3DRS_ALPHABLENDENABLE, !l->Opaque());
        if (!l->Opaque()) {
            pass.State(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            pass.State(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            pass.State(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        }
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
            const float t = float(y - l->dst.top) * osh / odh - l->BobShift();
            return ((l->ddfx & DDOVERFX_MIRRORUPDOWN) ? float(l->src.bottom) - t : float(l->src.top) + t) / float(c.h);
        };
        const float u0 = ou(vis.left), u1 = ou(vis.right), v0 = ov(vis.top), v1 = ov(vis.bottom);
        const float f0 = float(vis.left - src.left) / float(sw), f1 = float(vis.right - src.left) / float(sw);
        const float g0 = float(vis.top - src.top) / float(sh), g1 = float(vis.bottom - src.top) / float(sh);
        // (The destination alpha surface: over the whole primary.)
        const float a0 = float(vis.left) / float(l->destW), a1 = float(vis.right) / float(l->destW);
        const float b0 = float(vis.top) / float(l->destH), b1 = float(vis.bottom) / float(l->destH);
        const float q[4][10] = {{x0, y0, 0, 1, u0, v0, f0, g0, a0, b0}, {x1, y0, 0, 1, u1, v0, f1, g0, a1, b0},
                                {x0, y1, 0, 1, u0, v1, f0, g1, a0, b1}, {x1, y1, 0, 1, u1, v1, f1, g1, a1, b1}};
        pass.DrawUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
    }
    gpu.CloseScene();
    return true;
}

}  // namespace dd2d9
