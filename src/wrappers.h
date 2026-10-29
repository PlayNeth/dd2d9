// COM interface wrappers. One template covers every version of an interface:
// methods that a version lacks become harmless non-virtual extras, and any
// missing or mistyped override leaves the class abstract, so the compiler
// checks every vtable slot against the SDK headers.
#pragma once
#include "objects.h"

namespace dd2d9 {

// ================================================================ surfaces
template <int V> struct ST;
template <> struct ST<1> { using I = IDirectDrawSurface;  using Desc = DDSURFACEDESC;  using Caps = DDSCAPS;  using CB = LPDDENUMSURFACESCALLBACK;  using UL = LPVOID; };
template <> struct ST<2> { using I = IDirectDrawSurface2; using Desc = DDSURFACEDESC;  using Caps = DDSCAPS;  using CB = LPDDENUMSURFACESCALLBACK;  using UL = LPVOID; };
template <> struct ST<3> { using I = IDirectDrawSurface3; using Desc = DDSURFACEDESC;  using Caps = DDSCAPS;  using CB = LPDDENUMSURFACESCALLBACK;  using UL = LPVOID; };
template <> struct ST<4> { using I = IDirectDrawSurface4; using Desc = DDSURFACEDESC2; using Caps = DDSCAPS2; using CB = LPDDENUMSURFACESCALLBACK2; using UL = LPRECT; };
template <> struct ST<7> { using I = IDirectDrawSurface7; using Desc = DDSURFACEDESC2; using Caps = DDSCAPS2; using CB = LPDDENUMSURFACESCALLBACK7; using UL = LPRECT; };

inline DDSCAPS2 ToCaps2(const DDSCAPS& c) { DDSCAPS2 r = {}; r.dwCaps = c.dwCaps; return r; }
inline DDSCAPS2 ToCaps2(const DDSCAPS2& c) { return c; }
inline void FromCaps2(const DDSCAPS2& c, DDSCAPS* out) { out->dwCaps = c.dwCaps; }
inline void FromCaps2(const DDSCAPS2& c, DDSCAPS2* out) { *out = c; }

template <int V>
class SurfaceWrap final : public ST<V>::I {
    using T = ST<V>;
    using I = typename T::I;

public:
    uint32_t magic = kMagic;
    Surface* s;
    explicit SurfaceWrap(Surface* impl) : s(impl) { WritableVtbl(this); }
    // Unlock takes the locked pointer (IDirectDrawSurface 1-3) or rectangle (4 and 7).
    HRESULT UnlockAs(LPVOID p) { return s->Unlock(p, nullptr, V); }
    HRESULT UnlockAs(LPRECT r) { return s->Unlock(nullptr, r, V); }

    static Surface* S(void* iface) { return Surface::From(iface); }

    STDMETHOD(QueryInterface)(REFIID r, void** p) override { return s->QueryInterface(r, p); }
    STDMETHOD_(ULONG, AddRef)() override { return s->AddRef(Surface::SlotFor(V)); }
    STDMETHOD_(ULONG, Release)() override { return s->Release(Surface::SlotFor(V)); }
    STDMETHOD(AddAttachedSurface)(I* a) { Guard l; return s->AddAttachedSurface(S(a), V); }
    // No overlays here: the overlay methods give what ddraw.dll gives for other surfaces
    // (spec: re/spec/surface-other.md, "Overlay methods on non-overlay surfaces").
    STDMETHOD(AddOverlayDirtyRect)(LPRECT r) {
        Guard l;
        if (!r || r->left < 0 || r->top < 0 || r->left > r->right || r->top > r->bottom || r->bottom > s->H() ||
            r->right > s->W())
            return DDERR_INVALIDPARAMS;
        return (s->desc.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) ? DDERR_INVALIDPARAMS : DD_OK;
    }
    STDMETHOD(Blt)(LPRECT d, I* src, LPRECT sr, DWORD f, LPDDBLTFX fx) {
        Surface* ss = S(src);
        if (src && !ss) return DDERR_INVALIDPARAMS;
        const bool srcOld = ss && (static_cast<void*>(src) == ss->Iface(1) || static_cast<void*>(src) == ss->Iface(2));
        return TraceFail("Blt", s, s->Blt(d, ss, sr, f, fx, V, srcOld));
    }
    // ddraw.dll checks the batch, then doesn't do it.
    STDMETHOD(BltBatch)(LPDDBLTBATCH b, DWORD n, DWORD) {
        DD2D9_STUB("BltBatch");
        Guard l;
        if (s->IsLost()) return DDERR_SURFACELOST;
        if (s->lockCount) return DDERR_SURFACEBUSY;
        for (LONG i = 0; i < LONG(n); i++) {
            if (!b) return DDERR_INVALIDPARAMS;
            Surface* src = b[i].lpDDSSrc ? S(b[i].lpDDSSrc) : nullptr;
            if (src && src->IsLost()) return DDERR_SURFACELOST;
            if (src && src->lockCount) return DDERR_SURFACEBUSY;
            const LPDDBLTFX fx = b[i].lpDDBltFx;
            if (fx ? fx->dwSize != sizeof(DDBLTFX) : (b[i].dwFlags & 0x02F75D4A)) return DDERR_INVALIDPARAMS;
        }
        return DDERR_UNSUPPORTED;
    }
    STDMETHOD(BltFast)(DWORD x, DWORD y, I* src, LPRECT sr, DWORD t) {
        Surface* ss = S(src);
        if (!ss) return DDERR_INVALIDPARAMS;
        return TraceFail("BltFast", s, s->BltFast(x, y, ss, sr, t, V));
    }
    STDMETHOD(DeleteAttachedSurface)(DWORD f, I* a) { Guard l; return s->DeleteAttachedSurface(f, S(a), V); }
    STDMETHOD(EnumAttachedSurfaces)(LPVOID ctx, typename T::CB cb) {
        return s->EnumAttachedSurfaces(V, ctx, reinterpret_cast<void*>(cb));
    }
    STDMETHOD(EnumOverlayZOrders)(DWORD f, LPVOID, typename T::CB cb) {
        if (!cb || f > 1) return DDERR_INVALIDPARAMS;  // DDENUMOVERLAYZ_BACKTOFRONT (0) or _FRONTTOBACK (1)
        return DD_OK;  // no overlays to list
    }
    STDMETHOD(Flip)(I* target, DWORD f) {
        Surface* t = S(target);
        if (target && !t) return DDERR_INVALIDPARAMS;
        return TraceFail("Flip", s, s->Flip(t, f));
    }
    STDMETHOD(GetAttachedSurface)(typename T::Caps* caps, I** out) {
        if (!caps || !out) return DDERR_INVALIDPARAMS;
        Guard l;
        Surface* a = nullptr;
        DDSCAPS2 c = ToCaps2(*caps);
        if (V < 4) c.dwCaps2 = c.dwCaps3 = c.dwCaps4 = 0;  // IDirectDrawSurface 1-3: only dwCaps
        HRESULT hr = s->GetAttachedSurface(c, &a, V);
        *out = SUCCEEDED(hr) ? static_cast<I*>(a->Iface(V)) : nullptr;
        return hr;
    }
    STDMETHOD(GetBltStatus)(DWORD f) {  // blits are done when Blt returns
        if (f != DDGBS_CANBLT && f != DDGBS_ISBLTDONE) return DDERR_INVALIDPARAMS;
        Guard l;
        return s->IsLost() ? DDERR_SURFACELOST : DD_OK;
    }
    STDMETHOD(GetCaps)(typename T::Caps* caps) {
        if (!caps) return DDERR_INVALIDPARAMS;
        DDSCAPS2 c;
        s->GetCaps(&c);
        FromCaps2(c, caps);
        return DD_OK;
    }
    STDMETHOD(GetClipper)(LPDIRECTDRAWCLIPPER* c) { Guard l; return s->GetClipper(c); }
    STDMETHOD(GetColorKey)(DWORD f, LPDDCOLORKEY k) { Guard l; return s->GetColorKey(f, k); }
    STDMETHOD(GetDC)(HDC* dc) { return TraceFail("GetDC", s, s->GetDC(dc)); }
    STDMETHOD(GetFlipStatus)(DWORD f) { Guard l; return s->GetFlipStatus(f); }
    STDMETHOD(GetOverlayPosition)(LPLONG x, LPLONG y) {
        if (!x || !y) return DDERR_INVALIDPARAMS;
        *x = *y = 0;
        Guard l;
        return s->IsLost() ? DDERR_SURFACELOST : DDERR_NOTAOVERLAYSURFACE;
    }
    STDMETHOD(GetPalette)(LPDIRECTDRAWPALETTE* p) { Guard l; return s->GetPalette(p); }
    STDMETHOD(GetPixelFormat)(LPDDPIXELFORMAT pf) { Guard l; return s->GetPixelFormat(pf); }
    STDMETHOD(GetSurfaceDesc)(typename T::Desc* d) {
        // Only the description of this interface version, as in ddraw.dll.
        if (!d || d->dwSize != sizeof(typename T::Desc)) return DDERR_INVALIDPARAMS;
        Guard l;
        DDSURFACEDESC2 t;
        s->GetSurfaceDesc(&t);
        DescOut(t, d);
        return DD_OK;
    }
    STDMETHOD(Initialize)(LPDIRECTDRAW, typename T::Desc*) { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(IsLost)() { Guard l; return TraceFail("IsLost", s, s->IsLost() ? DDERR_SURFACELOST : DD_OK); }
    STDMETHOD(Lock)(LPRECT r, typename T::Desc* d, DWORD f, HANDLE) { return TraceFail("Lock", s, s->Lock(r, d, f)); }
    STDMETHOD(ReleaseDC)(HDC dc) { return s->ReleaseDC(dc, V); }
    STDMETHOD(Restore)() { Guard l; return TraceFail("Restore", s, s->Restore()); }
    STDMETHOD(SetClipper)(LPDIRECTDRAWCLIPPER c) { Guard l; return s->SetClipper(c); }
    STDMETHOD(SetColorKey)(DWORD f, LPDDCOLORKEY k) { Guard l; return s->SetColorKey(f, k, V); }
    STDMETHOD(SetOverlayPosition)(LONG, LONG) {
        Guard l;
        return s->IsLost() ? DDERR_SURFACELOST : DDERR_NOTAOVERLAYSURFACE;
    }
    STDMETHOD(SetPalette)(LPDIRECTDRAWPALETTE p) { Guard l; return s->SetPalette(p, V); }
    STDMETHOD(Unlock)(typename T::UL u) { return UnlockAs(u); }
    STDMETHOD(UpdateOverlay)(LPRECT, I* dest, LPRECT, DWORD f, LPDDOVERLAYFX fx) {
        DD2D9_STUB("UpdateOverlay");
        Guard l;
        if (s->IsLost()) return DDERR_SURFACELOST;
        Surface* d = S(dest);
        if (!d) return DDERR_INVALIDPARAMS;
        if (d->IsLost()) return DDERR_SURFACELOST;
        if ((f & 0xF8000000) || ((f & DDOVER_DDFX) && (!fx || fx->dwSize != sizeof(DDOVERLAYFX))))
            return DDERR_INVALIDPARAMS;
        return DDERR_NOTAOVERLAYSURFACE;
    }
    STDMETHOD(UpdateOverlayDisplay)(DWORD f) {
        if (f & ~DWORD(DDOVER_REFRESHDIRTYRECTS | DDOVER_REFRESHALL)) return DDERR_INVALIDPARAMS;
        return (s->desc.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) ? DDERR_UNSUPPORTED : DD_OK;
    }
    STDMETHOD(UpdateOverlayZOrder)(DWORD, I*) { return DDERR_NOTAOVERLAYSURFACE; }
    // IDirectDrawSurface2+
    STDMETHOD(GetDDInterface)(LPVOID* out) { Guard l; return s->GetDDInterface(out); }
    STDMETHOD(PageLock)(DWORD f) { return s->PageLock(f); }
    STDMETHOD(PageUnlock)(DWORD f) { return s->PageLock(f); }
    // IDirectDrawSurface3+
    STDMETHOD(SetSurfaceDesc)(typename T::Desc* d, DWORD f) {
        // Only this interface version's structure, as in ddraw.dll.
        DDSURFACEDESC2 t;
        if (!d || d->dwSize != sizeof(typename T::Desc) || !DescIn(d, &t)) return DDERR_INVALIDPARAMS;
        Guard l;
        return s->SetSurfaceDesc(t, f);
    }
    // IDirectDrawSurface4+
    STDMETHOD(SetPrivateData)(REFGUID g, LPVOID d, DWORD sz, DWORD f) { Guard l; return s->SetPrivateData(g, d, sz, f); }
    STDMETHOD(GetPrivateData)(REFGUID g, LPVOID d, LPDWORD sz) { Guard l; return s->GetPrivateData(g, d, sz); }
    STDMETHOD(FreePrivateData)(REFGUID g) { Guard l; return s->FreePrivateData(g); }
    STDMETHOD(GetUniquenessValue)(LPDWORD v) {
        if (!v) return DDERR_INVALIDPARAMS;
        *v = s->uniqueness;
        return DD_OK;
    }
    STDMETHOD(ChangeUniquenessValue)() { Guard l; s->BumpUniqueness(); return DD_OK; }
    // IDirectDrawSurface7
    // Managed textures only. The checks come before the pointer is used, as in ddraw.dll
    // (where a NULL one faults into DDERR_INVALIDPARAMS).
    // Managed textures only. The checks come before the pointer is used, as in ddraw.dll
    // (where a NULL one faults into DDERR_INVALIDPARAMS).
    STDMETHOD(SetPriority)(DWORD p) { return SetManaged(&s->priority, p); }
    STDMETHOD(GetPriority)(LPDWORD p) { return GetManaged(s->priority, p); }
    STDMETHOD(SetLOD)(DWORD v) { return SetManaged(&s->lod, v); }
    STDMETHOD(GetLOD)(LPDWORD v) { return GetManaged(s->lod, v); }

private:
    HRESULT SetManaged(DWORD* field, DWORD v) {
        Guard l;
        const HRESULT hr = s->ManagedTextureCheck();
        if (SUCCEEDED(hr)) *field = v;
        return hr;
    }
    HRESULT GetManaged(const DWORD& field, DWORD* out) {
        Guard l;
        const HRESULT hr = s->ManagedTextureCheck();
        if (FAILED(hr)) return hr;
        if (!out) return DDERR_INVALIDPARAMS;
        *out = field;
        return DD_OK;
    }
};

// ================================================================ DirectDraw
template <int V> struct DT;
template <> struct DT<1> { using I = IDirectDraw;  using SI = IDirectDrawSurface;  using Desc = DDSURFACEDESC;  using ModesCB = LPDDENUMMODESCALLBACK;  using SurfCB = LPDDENUMSURFACESCALLBACK;  using Caps = DDSCAPS;  using DevId = DDDEVICEIDENTIFIER;  static constexpr int SV = 1; };
template <> struct DT<2> { using I = IDirectDraw2; using SI = IDirectDrawSurface;  using Desc = DDSURFACEDESC;  using ModesCB = LPDDENUMMODESCALLBACK;  using SurfCB = LPDDENUMSURFACESCALLBACK;  using Caps = DDSCAPS;  using DevId = DDDEVICEIDENTIFIER;  static constexpr int SV = 1; };
template <> struct DT<4> { using I = IDirectDraw4; using SI = IDirectDrawSurface4; using Desc = DDSURFACEDESC2; using ModesCB = LPDDENUMMODESCALLBACK2; using SurfCB = LPDDENUMSURFACESCALLBACK2; using Caps = DDSCAPS2; using DevId = DDDEVICEIDENTIFIER;  static constexpr int SV = 4; };
template <> struct DT<7> { using I = IDirectDraw7; using SI = IDirectDrawSurface7; using Desc = DDSURFACEDESC2; using ModesCB = LPDDENUMMODESCALLBACK2; using SurfCB = LPDDENUMSURFACESCALLBACK7; using Caps = DDSCAPS2; using DevId = DDDEVICEIDENTIFIER2; static constexpr int SV = 7; };

template <int V>
class DDWrap final : public DT<V>::I {
    using T = DT<V>;
    using SI = typename T::SI;

public:
    uint32_t magic = kMagic;
    DDraw* d;
    explicit DDWrap(DDraw* impl) : d(impl) { WritableVtbl(this); }

    STDMETHOD(QueryInterface)(REFIID r, void** p) override { return d->QueryInterface(r, p); }
    STDMETHOD_(ULONG, AddRef)() override { return d->AddRef(V); }
    STDMETHOD_(ULONG, Release)() override { return d->Release(V); }
    STDMETHOD(Compact)() { return d->Exclusive() ? DD_OK : DDERR_NOEXCLUSIVEMODE; }  // as ddraw.dll
    STDMETHOD(CreateClipper)(DWORD f, LPDIRECTDRAWCLIPPER* c, IUnknown* o) { return d->CreateClipper(f, c, o, V); }
    STDMETHOD(CreatePalette)(DWORD f, LPPALETTEENTRY e, LPDIRECTDRAWPALETTE* p, IUnknown* o) {
        return d->CreatePalette(f, e, p, o, V);
    }
    STDMETHOD(CreateSurface)(typename T::Desc* desc, SI** out, IUnknown* o) {
        return TraceFail("CreateSurface", d, d->CreateSurface(desc, reinterpret_cast<void**>(out), o, V));
    }
    STDMETHOD(DuplicateSurface)(SI* src, SI** out) { return d->DuplicateSurface(src, reinterpret_cast<void**>(out), V); }
    STDMETHOD(EnumDisplayModes)(DWORD f, typename T::Desc* desc, LPVOID ctx, typename T::ModesCB cb) {
        // A filter of this interface version's size only, as in ddraw.dll.
        if (desc && desc->dwSize != sizeof(typename T::Desc)) return DDERR_INVALIDPARAMS;
        return d->EnumDisplayModes(f, desc, ctx, reinterpret_cast<void*>(cb), V >= 4 ? 2 : 1);
    }
    STDMETHOD(EnumSurfaces)(DWORD f, typename T::Desc* desc, LPVOID ctx, typename T::SurfCB cb) {
        return d->EnumSurfaces(f, desc, ctx, reinterpret_cast<void*>(cb), T::SV);
    }
    STDMETHOD(FlipToGDISurface)() { return d->FlipToGDISurface(); }
    STDMETHOD(GetCaps)(LPDDCAPS drv, LPDDCAPS hel) { return d->GetCaps(drv, hel); }
    STDMETHOD(GetDisplayMode)(typename T::Desc* desc) { return d->GetDisplayMode(desc, V); }
    STDMETHOD(GetFourCCCodes)(LPDWORD n, LPDWORD) {
        if (!n) return DDERR_INVALIDPARAMS;
        *n = 0;
        return DD_OK;
    }
    STDMETHOD(GetGDISurface)(SI** out) { return d->GetGDISurface(reinterpret_cast<void**>(out), T::SV); }
    STDMETHOD(GetMonitorFrequency)(LPDWORD f) { return d->GetMonitorFrequency(f); }
    STDMETHOD(GetScanLine)(LPDWORD l) { return d->GetScanLine(l); }
    STDMETHOD(GetVerticalBlankStatus)(WINBOOL* b) { return d->GetVerticalBlankStatus(b); }
    STDMETHOD(Initialize)(GUID* g) { return d->Initialize(g); }
    STDMETHOD(RestoreDisplayMode)() { return d->RestoreDisplayMode(); }
    STDMETHOD(SetCooperativeLevel)(HWND h, DWORD f) { return TraceFail("SetCooperativeLevel", d, d->SetCooperativeLevel(h, f, V)); }
    STDMETHOD(SetDisplayMode)(DWORD w, DWORD h, DWORD bpp) { return TraceFail("SetDisplayMode", d, d->SetDisplayMode(w, h, bpp, 0, 0)); }
    STDMETHOD(SetDisplayMode)(DWORD w, DWORD h, DWORD bpp, DWORD rr, DWORD f) { return TraceFail("SetDisplayMode", d, d->SetDisplayMode(w, h, bpp, rr, f)); }
    STDMETHOD(WaitForVerticalBlank)(DWORD f, HANDLE e) { return d->WaitForVerticalBlank(f, e); }
    // IDirectDraw2+
    STDMETHOD(GetAvailableVidMem)(typename T::Caps* c, LPDWORD total, LPDWORD free) {
        if (!c) return DDERR_INVALIDPARAMS;
        const DDSCAPS2 caps = ToCaps2(*c);
        return d->GetAvailableVidMem(&caps, total, free);
    }
    // IDirectDraw4+
    // IDirectDraw4's hands out the IDirectDrawSurface interface (as ddraw.dll does).
    STDMETHOD(GetSurfaceFromDC)(HDC dc, SI** out) {
        return d->GetSurfaceFromDC(dc, reinterpret_cast<void**>(out), V == 4 ? 1 : T::SV);
    }
    STDMETHOD(RestoreAllSurfaces)() { return TraceFail("RestoreAllSurfaces", d, d->RestoreAllSurfaces()); }
    STDMETHOD(TestCooperativeLevel)() { return TraceFail("TestCooperativeLevel", d, d->TestCooperativeLevel()); }
    STDMETHOD(GetDeviceIdentifier)(typename T::DevId* id, DWORD f) { return d->GetDeviceIdentifier(id, f, V == 7); }
    // IDirectDraw7
    STDMETHOD(StartModeTest)(LPSIZE, DWORD, DWORD) { return DDERR_TESTFINISHED; }
    STDMETHOD(EvaluateMode)(DWORD, DWORD*) { return DDERR_INVALIDPARAMS; }  // as ddraw.dll (no mode test running)
};

}  // namespace dd2d9
