// Legacy Direct3D objects for a wrapper (D7VK) on top of dd2d9; see d3dproxy.h.
#include "d3dproxy.h"

#include "d3dstubs.h"
#include "objects.h"

#include <new>

namespace dd2d9 {

namespace {

constexpr uint32_t kD3DMagic = 0x50443344;  // "D3DP": distinguishes these from DirectDraw objects

enum Kind { kD3D, kDevice, kViewport, kMaterial, kTexture };

// Reference-counted implementation object shared by the interface versions.
class RefObj {
public:
    explicit RefObj(Kind k) : kind(k) {}
    virtual ~RefObj() = default;
    virtual HRESULT QueryInterface(REFIID riid, void** out) = 0;
    virtual ULONG AddRef() { return ULONG(InterlockedIncrement(&ref_)); }
    virtual ULONG Release() {
        // Under g_lock: D3DProxyQueryDDraw hands out the cached Direct3D object
        // under it, and must not AddRef one whose count already reached zero.
        Guard l;
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return ULONG(r < 0 ? 0 : r);
    }
    const Kind kind;

private:
    LONG ref_ = 1;
};

// One interface version of an object: { vtable, magic, object } like the
// DirectDraw wrappers, so incoming pointers can be checked and mapped back.
template <class Stub>
class Facet : public Stub {
public:
    uint32_t magic = kD3DMagic;
    RefObj* obj;
    explicit Facet(RefObj* o) : obj(o) {}
    STDMETHOD(QueryInterface)(REFIID r, void** p) override { return obj->QueryInterface(r, p); }
    STDMETHOD_(ULONG, AddRef)() override { return obj->AddRef(); }
    STDMETHOD_(ULONG, Release)() override { return obj->Release(); }
};

struct FacetHeader {
    void* vtbl;
    uint32_t magic;
    RefObj* obj;
};

template <class T>
T* FromIface(void* iface, Kind kind) {
    if (!iface) return nullptr;
    auto* h = static_cast<FacetHeader*>(iface);
    if (h->magic != kD3DMagic || h->obj->kind != kind) return nullptr;
    return static_cast<T*>(h->obj);
}

// mingw-w64 has no __uuidof registrations for these interfaces.
template <class T> const GUID& IidOf();
template <> const GUID& IidOf<IDirect3DMaterial>() { return IID_IDirect3DMaterial; }
template <> const GUID& IidOf<IDirect3DMaterial2>() { return IID_IDirect3DMaterial2; }
template <> const GUID& IidOf<IDirect3DMaterial3>() { return IID_IDirect3DMaterial3; }
template <> const GUID& IidOf<IDirect3DViewport>() { return IID_IDirect3DViewport; }
template <> const GUID& IidOf<IDirect3DViewport2>() { return IID_IDirect3DViewport2; }
template <> const GUID& IidOf<IDirect3DViewport3>() { return IID_IDirect3DViewport3; }

D3DCOLOR ColorFromValue(const D3DCOLORVALUE& c) {
    auto ch = [](float v) { return DWORD(v <= 0.0f ? 0 : v >= 1.0f ? 255 : v * 255.0f + 0.5f); };
    return (ch(c.a) << 24) | (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
}

// Copies a surface (and its mip levels) as IDirect3DDevice7::Load and
// IDirect3DTexture::Load do. Called with g_lock held.
HRESULT CopyChain(Surface* dst, const POINT* point, Surface* src, const RECT* rect) {
    if (!dst || !src) return DDERR_INVALIDPARAMS;
    RECT sr = rect ? *rect : RECT{0, 0, src->W(), src->H()};
    POINT p = point ? *point : POINT{0, 0};
    for (int level = 0; dst && src; level++) {
        RECT dr = {p.x, p.y, p.x + (sr.right - sr.left), p.y + (sr.bottom - sr.top)};
        HRESULT hr = dst->BltLocked(&dr, src, &sr, 0, nullptr, false);
        if (FAILED(hr)) return level == 0 ? hr : DD_OK;  // smaller levels may simply not line up
        dst = dst->NextMipLevel();
        src = src->NextMipLevel();
        sr = {sr.left / 2, sr.top / 2, std::max<LONG>(sr.right / 2, sr.left / 2 + 1),
              std::max<LONG>(sr.bottom / 2, sr.top / 2 + 1)};
        p = {p.x / 2, p.y / 2};
    }
    return DD_OK;
}

// ---------------------------------------------------------------- materials

class Material;
std::vector<Material*> g_materials;  // handle - 1 -> material; guarded by g_lock

class Material final : public RefObj {
public:
    Material() : RefObj(kMaterial), f1(this), f2(this), f3(this) {
        Guard l;
        g_materials.push_back(this);
        handle = DWORD(g_materials.size());
    }
    ~Material() override { g_materials[handle - 1] = nullptr; }  // destroyed with g_lock held

    static Material* FromHandle(DWORD h) { return h && h <= g_materials.size() ? g_materials[h - 1] : nullptr; }

    HRESULT QueryInterface(REFIID r, void** out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        void* f = nullptr;
        if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirect3DMaterial)) f = &f1;
        else if (IsEqualGUID(r, IID_IDirect3DMaterial2)) f = &f2;
        else if (IsEqualGUID(r, IID_IDirect3DMaterial3)) f = &f3;
        *out = f;
        if (!f) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    template <class Stub, class Dev>
    struct F : Facet<Stub> {
        using Facet<Stub>::Facet;
        Material* M() { return static_cast<Material*>(this->obj); }
        STDMETHOD(SetMaterial)(D3DMATERIAL* data) override {
            if (!data) return DDERR_INVALIDPARAMS;
            Guard l;
            M()->color = ColorFromValue(data->diffuse);
            return D3D_OK;
        }
        STDMETHOD(GetHandle)(Dev* device, D3DMATERIALHANDLE* h) override {
            if (!h) return DDERR_INVALIDPARAMS;
            *h = M()->handle;
            return D3D_OK;
        }
    };
    struct F1 : F<IDirect3DMaterialStub, IDirect3DDevice> {
        using F::F;
        STDMETHOD(Initialize)(IDirect3D*) override { return D3D_OK; }
    };

    DWORD handle = 0;
    D3DCOLOR color = 0;  // diffuse, used as a viewport background
    F1 f1;
    F<IDirect3DMaterial2Stub, IDirect3DDevice2> f2;
    F<IDirect3DMaterial3Stub, IDirect3DDevice3> f3;
};

// ---------------------------------------------------------------- devices and viewports

class Device;

class Viewport final : public RefObj {
public:
    Viewport() : RefObj(kViewport), f1(this), f2(this), f3(this) {}

    HRESULT QueryInterface(REFIID r, void** out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        void* f = nullptr;
        if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirect3DViewport)) f = &f1;
        else if (IsEqualGUID(r, IID_IDirect3DViewport2)) f = &f2;
        else if (IsEqualGUID(r, IID_IDirect3DViewport3)) f = &f3;
        *out = f;
        if (!f) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    HRESULT Clear(DWORD count, D3DRECT* rects, DWORD flags, bool haveColor, D3DCOLOR color, float z);

    // Methods shared by the three versions.
    template <class Stub>
    struct F : Facet<Stub> {
        using Facet<Stub>::Facet;
        Viewport* V() { return static_cast<Viewport*>(this->obj); }
        STDMETHOD(Initialize)(IDirect3D*) override { return D3D_OK; }
        STDMETHOD(SetViewport)(D3DVIEWPORT* d) override {
            if (!d) return DDERR_INVALIDPARAMS;
            Guard l;
            V()->area = {LONG(d->dwX), LONG(d->dwY), LONG(d->dwX + d->dwWidth), LONG(d->dwY + d->dwHeight)};
            return D3D_OK;
        }
        STDMETHOD(SetBackground)(D3DMATERIALHANDLE h) override {
            Guard l;
            V()->background = h;
            return D3D_OK;
        }
        STDMETHOD(Clear)(DWORD count, D3DRECT* rects, DWORD flags) override {
            Guard l;
            Material* m = Material::FromHandle(V()->background);
            return V()->Clear(count, rects, flags, m != nullptr, m ? m->color : 0, 1.0f);
        }
        STDMETHOD(AddLight)(IDirect3DLight*) override { return D3D_OK; }
        STDMETHOD(DeleteLight)(IDirect3DLight*) override { return D3D_OK; }
    };
    template <class Stub>
    struct F2 : F<Stub> {
        using F<Stub>::F;
        STDMETHOD(SetViewport2)(D3DVIEWPORT2* d) override {
            if (!d) return DDERR_INVALIDPARAMS;
            Guard l;
            this->V()->area = {LONG(d->dwX), LONG(d->dwY), LONG(d->dwX + d->dwWidth), LONG(d->dwY + d->dwHeight)};
            return D3D_OK;
        }
    };
    struct F3 : F2<IDirect3DViewport3Stub> {
        using F2::F2;
        STDMETHOD(Clear2)(DWORD count, D3DRECT* rects, DWORD flags, DWORD color, D3DVALUE z, DWORD) override {
            Guard l;
            return V()->Clear(count, rects, flags, true, color, z);
        }
    };

    Device* device = nullptr;  // set by AddViewport, not referenced
    RECT area = {0, 0, 0, 0};
    DWORD background = 0;
    F<IDirect3DViewportStub> f1;
    F2<IDirect3DViewport2Stub> f2;
    F3 f3;
};

class Device final : public RefObj {
public:
    Device(DDraw* d, Surface* target) : RefObj(kDevice), dd(d), rt(target), f1(this), f2(this), f3(this), f7(this) {
        dd->IntAddRef();
        rt->AddRef();
        dd->Add3DDevice();  // dd2d9 stops presenting: the wrapper shows the frames now
    }
    ~Device() override {  // destroyed with g_lock held
        for (Viewport* v : viewports) {
            v->device = nullptr;
            v->Release();
        }
        rt->Release();
        dd->Remove3DDevice();
        dd->IntRelease();
    }

    HRESULT QueryInterface(REFIID r, void** out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        void* f = nullptr;
        if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirect3DDevice)) f = &f1;
        else if (IsEqualGUID(r, IID_IDirect3DDevice2)) f = &f2;
        else if (IsEqualGUID(r, IID_IDirect3DDevice3)) f = &f3;
        else if (IsEqualGUID(r, IID_IDirect3DDevice7)) f = &f7;
        *out = f;
        if (!f) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    // Fills `rects` (or the viewport) of the render target and its z-buffer.
    // Called with g_lock held.
    HRESULT Clear(DWORD count, const D3DRECT* rects, DWORD flags, bool haveColor, D3DCOLOR color, float z,
                  const RECT& view) {
        if (count && !rects) return DDERR_INVALIDPARAMS;
        Surface* zbuf = nullptr;
        for (Surface* a : rt->attached)
            if (a->buf->fmt.zbuf) zbuf = a;
        const DWORD zmax = zbuf ? zbuf->buf->fmt.ValueMask() : 0;
        const DWORD depth = DWORD(double(z < 0 ? 0 : z > 1 ? 1 : z) * zmax);
        for (DWORD i = 0; i < (count ? count : 1); i++) {
            RECT r = count ? RECT{rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2} : view;
            if (count && !IntersectRect(&r, &r, &view)) continue;
            if ((flags & D3DCLEAR_TARGET) && haveColor) rt->FillRect(r, ARGBToPixel(rt->buf->fmt, color));
            if ((flags & D3DCLEAR_ZBUFFER) && zbuf) zbuf->FillRect(r, depth);
        }
        return D3D_OK;
    }

    RECT FullTarget() const { return RECT{0, 0, rt->W(), rt->H()}; }

    void SetTarget(Surface* s) {
        s->AddRef();
        rt->Release();
        rt = s;
    }

    HRESULT AddViewport(Viewport* v) {
        if (!v) return DDERR_INVALIDPARAMS;
        for (Viewport* x : viewports)
            if (x == v) return DDERR_INVALIDPARAMS;
        v->AddRef();
        v->device = this;
        viewports.push_back(v);
        return D3D_OK;
    }
    HRESULT DeleteViewport(Viewport* v) {
        for (size_t i = 0; i < viewports.size(); i++) {
            if (viewports[i] != v) continue;
            viewports.erase(viewports.begin() + i);
            v->device = nullptr;
            v->Release();
            return D3D_OK;
        }
        return DDERR_INVALIDPARAMS;
    }

    // Methods shared by versions 1-3 (viewport objects differ per version).
    template <class Stub, class VP>
    struct F : Facet<Stub> {
        using Facet<Stub>::Facet;
        Device* D() { return static_cast<Device*>(this->obj); }
        STDMETHOD(AddViewport)(VP* v) override {
            Guard l;
            return D()->AddViewport(FromIface<Viewport>(v, kViewport));
        }
        STDMETHOD(DeleteViewport)(VP* v) override {
            Guard l;
            return D()->DeleteViewport(FromIface<Viewport>(v, kViewport));
        }
        STDMETHOD(BeginScene)() override { return D3D_OK; }
        STDMETHOD(EndScene)() override { return D3D_OK; }
    };
    struct F1 : F<IDirect3DDeviceStub, IDirect3DViewport> {
        using F::F;
        STDMETHOD(Initialize)(IDirect3D*, GUID*, D3DDEVICEDESC*) override { return D3D_OK; }
    };
    template <class Stub, class VP, class DDS>
    struct F23 : F<Stub, VP> {
        using F<Stub, VP>::F;
        STDMETHOD(SetCurrentViewport)(VP* v) override { return FromIface<Viewport>(v, kViewport) ? D3D_OK : DDERR_INVALIDPARAMS; }
        STDMETHOD(SetRenderTarget)(DDS* surface, DWORD) override {
            Surface* s = Surface::From(surface);
            if (!s) return DDERR_INVALIDPARAMS;
            Guard l;
            this->D()->SetTarget(s);
            return D3D_OK;
        }
    };
    struct F7 : Facet<IDirect3DDevice7Stub> {
        using Facet::Facet;
        Device* D() { return static_cast<Device*>(obj); }
        STDMETHOD(BeginScene)() override { return D3D_OK; }
        STDMETHOD(EndScene)() override { return D3D_OK; }
        STDMETHOD(SetRenderTarget)(IDirectDrawSurface7* surface, DWORD) override {
            Surface* s = Surface::From(surface);
            if (!s) return DDERR_INVALIDPARAMS;
            Guard l;
            D()->SetTarget(s);
            return D3D_OK;
        }
        STDMETHOD(SetViewport)(D3DVIEWPORT7* v) override {
            if (!v) return DDERR_INVALIDPARAMS;
            Guard l;
            D()->view7 = {LONG(v->dwX), LONG(v->dwY), LONG(v->dwX + v->dwWidth), LONG(v->dwY + v->dwHeight)};
            D()->haveView7 = true;
            return D3D_OK;
        }
        STDMETHOD(Clear)(DWORD count, D3DRECT* rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD) override {
            Guard l;
            RECT view = D()->haveView7 ? D()->view7 : D()->FullTarget();
            return D()->Clear(count, rects, flags, true, color, z, view);
        }
        STDMETHOD(Load)(IDirectDrawSurface7* dst, POINT* point, IDirectDrawSurface7* src, RECT* rect, DWORD) override {
            Guard l;
            return CopyChain(Surface::From(dst), point, Surface::From(src), rect);
        }
    };

    DDraw* dd;
    Surface* rt;
    std::vector<Viewport*> viewports;
    RECT view7 = {};
    bool haveView7 = false;
    F1 f1;
    F23<IDirect3DDevice2Stub, IDirect3DViewport2, IDirectDrawSurface> f2;
    F23<IDirect3DDevice3Stub, IDirect3DViewport3, IDirectDrawSurface4> f3;
    F7 f7;
};

HRESULT Viewport::Clear(DWORD count, D3DRECT* rects, DWORD flags, bool haveColor, D3DCOLOR color, float z) {
    if (!device) return D3DERR_VIEWPORTHASNODEVICE;
    RECT view = IsRectEmpty(&area) ? device->FullTarget() : area;
    return device->Clear(count, rects, flags, haveColor, color, z, view);
}

// ---------------------------------------------------------------- textures

class Texture final : public RefObj {
public:
    explicit Texture(Surface* s) : RefObj(kTexture), surface(s), f1(this), f2(this) {}
    // Same object and lifetime as the surface: its references are those of the
    // surface interface it came from (as in ddraw.dll, the one it was created through).
    ULONG AddRef() override { return surface->AddRef(Surface::SlotFor(surface->version)); }
    ULONG Release() override { return surface->Release(Surface::SlotFor(surface->version)); }
    HRESULT QueryInterface(REFIID r, void** out) override { return surface->QueryInterface(r, out); }

    template <class Stub, class Dev, class Tex>
    struct F : Facet<Stub> {
        using Facet<Stub>::Facet;
        Texture* T() { return static_cast<Texture*>(this->obj); }
        STDMETHOD(GetHandle)(Dev*, D3DTEXTUREHANDLE* h) override {
            if (!h) return DDERR_INVALIDPARAMS;
            *h = D3DTEXTUREHANDLE(reinterpret_cast<uintptr_t>(T()->surface));
            return D3D_OK;
        }
        STDMETHOD(PaletteChanged)(DWORD, DWORD) override { return D3D_OK; }
        STDMETHOD(Load)(Tex* src) override {
            Texture* s = FromIface<Texture>(src, kTexture);
            if (!s) return DDERR_INVALIDPARAMS;
            Guard l;
            return CopyChain(T()->surface, nullptr, s->surface, nullptr);
        }
    };
    struct F1 : F<IDirect3DTextureStub, IDirect3DDevice, IDirect3DTexture> {
        using F::F;
        STDMETHOD(Initialize)(IDirect3DDevice*, IDirectDrawSurface*) override { return D3D_OK; }
        STDMETHOD(Unload)() override { return D3D_OK; }
    };

    Surface* surface;
    F1 f1;
    F<IDirect3DTexture2Stub, IDirect3DDevice2, IDirect3DTexture2> f2;
};

// ---------------------------------------------------------------- Direct3D object

class D3D final : public RefObj {
public:
    explicit D3D(DDraw* d) : RefObj(kD3D), dd(d), f1(this), f2(this), f3(this), f7(this) { dd->IntAddRef(); }
    ~D3D() override {  // destroyed with g_lock held
        if (dd->d3dObject == this) dd->d3dObject = nullptr;
        dd->IntRelease();
    }

    HRESULT QueryInterface(REFIID r, void** out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        void* f = nullptr;
        if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirect3D)) f = &f1;
        else if (IsEqualGUID(r, IID_IDirect3D2)) f = &f2;
        else if (IsEqualGUID(r, IID_IDirect3D3)) f = &f3;
        else if (IsEqualGUID(r, IID_IDirect3D7)) f = &f7;
        else return dd->QueryInterface(r, out);  // back to the DirectDraw interfaces
        *out = f;
        AddRef();
        return S_OK;
    }

    template <class Out, class Obj>
    static HRESULT Create(Obj* o, REFIID iid, Out** out) {
        HRESULT hr = o->QueryInterface(iid, reinterpret_cast<void**>(out));
        o->Release();  // drop the creation reference: the interface holds one now
        return hr;
    }

    HRESULT CreateDevice(void* surface, REFIID iid, void** out) {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = nullptr;
        Surface* s = Surface::From(surface);
        if (!s) return DDERR_INVALIDPARAMS;
        auto* d = new (std::nothrow) Device(dd, s);  // not under g_lock: Add3DDevice waits for the presenter
        if (!d) return DDERR_OUTOFMEMORY;
        DD2D9_LOG("Direct3D device created on surface %p (%dx%d)", static_cast<void*>(s), s->W(), s->H());
        return Create(d, iid, out);
    }

    template <class Stub, class MatOut, class VpOut>
    struct F : Facet<Stub> {
        using Facet<Stub>::Facet;
        D3D* O() { return static_cast<D3D*>(this->obj); }
        STDMETHOD(CreateMaterial)(MatOut** out, IUnknown* outer) override {
            if (!out) return DDERR_INVALIDPARAMS;
            if (outer) return CLASS_E_NOAGGREGATION;
            auto* m = new (std::nothrow) Material();
            if (!m) return DDERR_OUTOFMEMORY;
            return Create(m, IidOf<MatOut>(), out);
        }
        STDMETHOD(CreateViewport)(VpOut** out, IUnknown* outer) override {
            if (!out) return DDERR_INVALIDPARAMS;
            if (outer) return CLASS_E_NOAGGREGATION;
            auto* v = new (std::nothrow) Viewport();
            if (!v) return DDERR_OUTOFMEMORY;
            return Create(v, IidOf<VpOut>(), out);
        }
    };
    struct F1 : F<IDirect3DStub, IDirect3DMaterial, IDirect3DViewport> {
        using F::F;
        STDMETHOD(Initialize)(REFIID) override { return D3D_OK; }
    };
    struct F2 : F<IDirect3D2Stub, IDirect3DMaterial2, IDirect3DViewport2> {
        using F::F;
        STDMETHOD(CreateDevice)(REFCLSID, IDirectDrawSurface* s, IDirect3DDevice2** out) override {
            return O()->CreateDevice(s, IID_IDirect3DDevice2, reinterpret_cast<void**>(out));
        }
    };
    struct F3 : F<IDirect3D3Stub, IDirect3DMaterial3, IDirect3DViewport3> {
        using F::F;
        STDMETHOD(CreateDevice)(REFCLSID, IDirectDrawSurface4* s, IDirect3DDevice3** out, IUnknown* outer) override {
            if (outer) return CLASS_E_NOAGGREGATION;
            return O()->CreateDevice(s, IID_IDirect3DDevice3, reinterpret_cast<void**>(out));
        }
        STDMETHOD(EvictManagedTextures)() override { return D3D_OK; }
    };
    struct F7 : Facet<IDirect3D7Stub> {
        using Facet::Facet;
        STDMETHOD(CreateDevice)(REFCLSID, IDirectDrawSurface7* s, IDirect3DDevice7** out) override {
            return static_cast<D3D*>(obj)->CreateDevice(s, IID_IDirect3DDevice7, reinterpret_cast<void**>(out));
        }
        STDMETHOD(EvictManagedTextures)() override { return D3D_OK; }
    };

    DDraw* dd;
    F1 f1;
    F2 f2;
    F3 f3;
    F7 f7;
};

bool IsDirect3DIID(REFIID r) {
    static const GUID* kIIDs[] = {&IID_IDirect3D, &IID_IDirect3D2, &IID_IDirect3D3, &IID_IDirect3D7};
    for (const GUID* g : kIIDs)
        if (IsEqualGUID(r, *g)) return true;
    return false;
}

// Querying a surface for one of these creates a Direct3D 1 device on it.
bool IsDeviceClass(REFIID r) {
    static const GUID* kIIDs[] = {&IID_IDirect3DHALDevice, &IID_IDirect3DRGBDevice, &IID_IDirect3DMMXDevice,
                                  &IID_IDirect3DRampDevice, &IID_IDirect3DRefDevice, &IID_IDirect3DTnLHalDevice};
    for (const GUID* g : kIIDs)
        if (IsEqualGUID(r, *g)) return true;
    return false;
}

}  // namespace

bool D3DProxyEnabled() {
    if (g_config.d3d == kD3DOff) return false;
    return g_config.d3d == kD3DOn || LoadedBehindWrapper();
}

HRESULT D3DProxyQueryDDraw(DDraw* dd, REFIID riid, void** out) {
    if (!IsDirect3DIID(riid)) return E_NOINTERFACE;
    if (!D3DProxyEnabled()) {
        DD2D9_LOG("DirectDraw::QueryInterface(Direct3D %lx): not offered, dd2d9 isn't loaded behind a Direct3D "
                  "wrapper such as D7VK", riid.Data1);
        return E_NOINTERFACE;
    }
    D3D* d3d;
    {
        Guard l;
        d3d = static_cast<D3D*>(dd->d3dObject);
        if (d3d) {
            d3d->AddRef();
        } else {
            d3d = new (std::nothrow) D3D(dd);
            if (!d3d) return DDERR_OUTOFMEMORY;
            dd->d3dObject = d3d;
            DD2D9_LOG("Direct3D object created for a wrapper on top of dd2d9");
        }
    }
    HRESULT hr = d3d->QueryInterface(riid, out);
    d3d->Release();
    return hr;
}

HRESULT D3DProxyQuerySurface(Surface* s, REFIID riid, void** out) {
    const bool texture = IsEqualGUID(riid, IID_IDirect3DTexture) || IsEqualGUID(riid, IID_IDirect3DTexture2);
    const bool device = IsDeviceClass(riid);
    if (!texture && !device) return E_NOINTERFACE;
    if (!D3DProxyEnabled()) return E_NOINTERFACE;
    if (device) {
        // Direct3D 1 style: the device is created by querying the render target.
        auto* d = new (std::nothrow) Device(s->dd, s);
        if (!d) return DDERR_OUTOFMEMORY;
        DD2D9_LOG("Direct3D device created through surface %p", static_cast<void*>(s));
        return D3D::Create(d, IID_IDirect3DDevice, out);
    }
    Guard l;
    auto* t = static_cast<Texture*>(s->d3dTexture);
    if (!t) {
        t = new (std::nothrow) Texture(s);
        if (!t) return DDERR_OUTOFMEMORY;
        s->d3dTexture = t;
    }
    *out = IsEqualGUID(riid, IID_IDirect3DTexture2) ? static_cast<void*>(&t->f2) : static_cast<void*>(&t->f1);
    t->AddRef();
    return S_OK;
}

void D3DProxySurfaceDestroyed(Surface* s) {
    delete static_cast<Texture*>(s->d3dTexture);  // its references were the surface's
    s->d3dTexture = nullptr;
}

}  // namespace dd2d9
