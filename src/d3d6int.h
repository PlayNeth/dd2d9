// dd2d9's Direct3D 3-6 (d3dim.dll's interfaces), internal declarations shared by d3d6obj.cpp
// (the Direct3D object, lights, materials, textures, execute and vertex buffers, the
// hand-off from DirectDraw), d3d6vp.cpp (viewports), d3d6dev.cpp (devices) and d3d6pipe.cpp
// (the CPU vertex pipeline, Execute and the draws). Matched to XP SP3's d3dim.dll
// 5.1.2600.0; section numbers cite re/spec/: A = d3d6-direct3d.md, B = d3d6-device.md,
// C = d3d6-objects.md.
//
// Locking: every method takes g_lock (Guard), then the Gpu lock (GpuGuard) to draw.
// Interfaces whose methods share names across versions (IDirect3D/2/3, IDirect3DDevice/2/3,
// IDirect3DMaterial/2/3, IDirect3DTexture/2) are facets: small objects, one per interface,
// forwarding to the one object behind them, as d3dim's objects carry several vtables.
#pragma once
#include "d3d6.h"
#include "d3dcommon.h"
#include "gpu.h"

#include <bitset>
#include <unordered_map>
#include <vector>

namespace dd2d9 {
namespace d6 {

class D3D;
class Device;
class Viewport;
class Light;
class Material;
class Texture;
class ExecBuf;
class VBuf;

// Not in mingw's dxguid.
extern const GUID kIID_NullDevice;
extern const GUID kIID_NewRGBDevice;

enum DevKind { kRamp, kRGB, kMMX, kHAL, kRef, kNull, kNewRGB, kBadGuid };
DevKind KindOf(REFCLSID guid);

// A float stored in a DWORD state.
inline float F(DWORD v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}
inline DWORD D(float f) {
    DWORD v;
    memcpy(&v, &f, 4);
    return v;
}

// The descriptors EnumDevices, FindDevice and GetCaps report (A §The descriptors).
void HalHalDesc(D3DDEVICEDESC* d);      // the hardware device's HAL desc, from Direct3D 9
void HalHelDesc(D3DDEVICEDESC* d);      // the hardware device's HEL desc (fixed)
void SoftHelDesc(DevKind k, int version, D3DDEVICEDESC* d);  // RGB/MMX/Ramp HEL desc
void NullDesc(D3DDEVICEDESC* d);        // the software devices' HAL desc

// ---------------------------------------------------------------- inner objects
// What a surface's Direct3D cache holds (A §Surface hand-off): textures and the devices
// made by a surface QueryInterface. Destroyed with the surface.
class Inner {
public:
    virtual ~Inner() = default;
    virtual HRESULT InnerQuery(REFIID riid, void** out) = 0;
    virtual void SurfaceGone() = 0;  // the surface is being destroyed: go too
};

// Per surface (Surface::d3dLegacy): the cache, and its texture handle.
struct SurfaceInfo {
    struct Entry {
        GUID iid;
        Inner* obj;
    };
    std::vector<Entry> cache;
    DWORD texHandle = 0;  // 0 until a GetHandle (one value per surface, as XP's DP2 handles)
};
SurfaceInfo* Info(Surface* s);  // created on demand

// Texture handles: one value per surface, the same on every device (XP: the surface's
// kernel handle). Resolved at draw time; 0 or a dead handle draws untextured.
DWORD TexHandleFor(Surface* s);
Surface* SurfaceForHandle(DWORD h);
void SwapHandles(Surface* a, Surface* b);

// ---------------------------------------------------------------- Direct3D object (A)
template <class I> struct D3DFacet : I {
    D3D* o;
};

class D3D {
public:
    D3D(DDraw* dd, int outer);
    ~D3D();
    DDraw* dd;
    int outer;  // the DirectDraw interface version its IUnknown forwards to (A §Aggregation)
    LONG innerRef = 1;  // DirectDraw's reference (released at teardown)
    DWORD nextViewportId = 1;
    std::vector<Device*> devices;
    std::vector<Viewport*> viewports;
    std::vector<Light*> lights;
    std::vector<Material*> materials;
    std::vector<VBuf*> vbufs;

    struct F1 final : D3DFacet<IDirect3D> {
        STDMETHOD(QueryInterface)(REFIID r, void** out) override { return o->QI(r, out); }
        STDMETHOD_(ULONG, AddRef)() override { return o->dd->AddRef(o->outer); }
        STDMETHOD_(ULONG, Release)() override { return o->dd->Release(o->outer); }
        STDMETHOD(Initialize)(REFIID) override { return DDERR_ALREADYINITIALIZED; }
        STDMETHOD(EnumDevices)(LPD3DENUMDEVICESCALLBACK cb, void* ctx) override { return o->EnumDevices(cb, ctx, 1); }
        STDMETHOD(CreateLight)(IDirect3DLight** l, IUnknown* outer) override { return o->CreateLight(l, outer); }
        STDMETHOD(CreateMaterial)(IDirect3DMaterial** m, IUnknown* outer) override;
        STDMETHOD(CreateViewport)(IDirect3DViewport** v, IUnknown*) override;
        STDMETHOD(FindDevice)(D3DFINDDEVICESEARCH* s, D3DFINDDEVICERESULT* r) override { return o->FindDevice(s, r, 1); }
    } f1;
    struct F2 final : D3DFacet<IDirect3D2> {
        STDMETHOD(QueryInterface)(REFIID r, void** out) override { return o->QI(r, out); }
        STDMETHOD_(ULONG, AddRef)() override { return o->dd->AddRef(o->outer); }
        STDMETHOD_(ULONG, Release)() override { return o->dd->Release(o->outer); }
        STDMETHOD(EnumDevices)(LPD3DENUMDEVICESCALLBACK cb, void* ctx) override { return o->EnumDevices(cb, ctx, 2); }
        STDMETHOD(CreateLight)(IDirect3DLight** l, IUnknown* outer) override { return o->CreateLight(l, outer); }
        STDMETHOD(CreateMaterial)(IDirect3DMaterial2** m, IUnknown* outer) override;
        STDMETHOD(CreateViewport)(IDirect3DViewport2** v, IUnknown*) override;
        STDMETHOD(FindDevice)(D3DFINDDEVICESEARCH* s, D3DFINDDEVICERESULT* r) override { return o->FindDevice(s, r, 2); }
        STDMETHOD(CreateDevice)(REFCLSID g, IDirectDrawSurface* s, IDirect3DDevice2** d) override;
    } f2;
    struct F3 final : D3DFacet<IDirect3D3> {
        STDMETHOD(QueryInterface)(REFIID r, void** out) override { return o->QI(r, out); }
        STDMETHOD_(ULONG, AddRef)() override { return o->dd->AddRef(o->outer); }
        STDMETHOD_(ULONG, Release)() override { return o->dd->Release(o->outer); }
        STDMETHOD(EnumDevices)(LPD3DENUMDEVICESCALLBACK cb, void* ctx) override { return o->EnumDevices(cb, ctx, 3); }
        STDMETHOD(CreateLight)(IDirect3DLight** l, IUnknown* outer) override { return o->CreateLight(l, outer); }
        STDMETHOD(CreateMaterial)(IDirect3DMaterial3** m, IUnknown* outer) override;
        STDMETHOD(CreateViewport)(IDirect3DViewport3** v, IUnknown* outer) override;
        STDMETHOD(FindDevice)(D3DFINDDEVICESEARCH* s, D3DFINDDEVICERESULT* r) override { return o->FindDevice(s, r, 3); }
        STDMETHOD(CreateDevice)(REFCLSID g, IDirectDrawSurface4* s, IDirect3DDevice3** d, IUnknown* outer) override;
        STDMETHOD(CreateVertexBuffer)(D3DVERTEXBUFFERDESC* d, IDirect3DVertexBuffer** vb, DWORD flags,
                                      IUnknown* outer) override;
        STDMETHOD(EnumZBufferFormats)(REFCLSID g, LPD3DENUMPIXELFORMATSCALLBACK cb, void* ctx) override;
        STDMETHOD(EvictManagedTextures)() override { return D3D_OK; }
    } f3;

    HRESULT QI(REFIID r, void** out) {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = nullptr;
        return dd->QueryInterface(r, out, outer);
    }
    // The inner IUnknown's QueryInterface (A §Aggregation): AddRefs DirectDraw's outer.
    HRESULT InnerQuery(REFIID r, void** out);
    HRESULT EnumDevices(LPD3DENUMDEVICESCALLBACK cb, void* ctx, int version);
    HRESULT FindDevice(D3DFINDDEVICESEARCH* s, D3DFINDDEVICERESULT* r, int version);
    HRESULT CreateLight(IDirect3DLight** out, IUnknown* outer);
    HRESULT CreateMaterial(Material** out, IUnknown* outer);
    HRESULT CreateViewport(Viewport** out, IUnknown* outer);
    // Direct3DCreateDevice (A): version 1 from a surface QI (surface, slot set), 2 or 3 from
    // CreateDevice. rtVersion: the surface interface version the game passed.
    HRESULT CreateDeviceI(REFCLSID guid, Surface* rt, int rtVersion, int version, Device** out);
    // DirectDraw's last reference went: everything left is released (A §~DIRECT3DI).
    void Teardown();
};

// ---------------------------------------------------------------- light (C §2)
class Light final : public IDirect3DLight {
public:
    explicit Light(D3D* d3d) : d3d_(d3d) {}
    STDMETHOD(QueryInterface)(REFIID r, void** out) override;
    STDMETHOD_(ULONG, AddRef)() override { return ULONG(InterlockedIncrement(&ref_)); }
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Initialize)(IDirect3D*) override { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(SetLight)(D3DLIGHT* l) override;
    STDMETHOD(GetLight)(D3DLIGHT* l) override;

    D3D* d3d_;
    Viewport* vp = nullptr;  // the viewport it was added to (holding a reference)
    D3DLIGHT2 data = {};     // as set (D3DLIGHT: the first 0x4c bytes); dwSize 0 until then
    bool Active() const { return data.dwSize == sizeof(D3DLIGHT) || (data.dwSize && (data.dwFlags & D3DLIGHT_ACTIVE)); }
    void Orphan() { d3d_ = nullptr; }

private:
    LONG ref_ = 1;
};

// ---------------------------------------------------------------- material (C §3)
// A per-device copy of a material; the handle value names it (C §3.2).
struct MatHandle {
    DWORD value;
    Material* mat;
    Device* dev;
    D3DMATERIAL data;
};
MatHandle* FindMatHandle(DWORD h);  // null for 0 or an unknown handle

template <class I> struct MatFacet : I {
    Material* o;
};

class Material {
public:
    explicit Material(D3D* d3d);
    ~Material();
    D3D* d3d_;
    D3DMATERIAL data = {};
    std::vector<MatHandle*> handles;
    LONG ref = 1;
    ULONG AddRef() { return ULONG(InterlockedIncrement(&ref)); }
    ULONG Release();
    HRESULT QI(REFIID r, void** out);
    HRESULT Set(D3DMATERIAL* m);
    HRESULT Get(D3DMATERIAL* m);
    HRESULT GetHandle(Device* dev, D3DMATERIALHANDLE* h);

    struct F1 final : MatFacet<IDirect3DMaterial> {
        STDMETHOD(QueryInterface)(REFIID r, void** out) override { return o->QI(r, out); }
        STDMETHOD_(ULONG, AddRef)() override { return o->AddRef(); }
        STDMETHOD_(ULONG, Release)() override { return o->Release(); }
        STDMETHOD(Initialize)(IDirect3D*) override { return DDERR_ALREADYINITIALIZED; }
        STDMETHOD(SetMaterial)(D3DMATERIAL* m) override { return o->Set(m); }
        STDMETHOD(GetMaterial)(D3DMATERIAL* m) override { return o->Get(m); }
        STDMETHOD(GetHandle)(IDirect3DDevice* d, D3DMATERIALHANDLE* h) override;
        STDMETHOD(Reserve)() override { return E_NOTIMPL; }
        STDMETHOD(Unreserve)() override { return E_NOTIMPL; }
    } f1;
    struct F2 final : MatFacet<IDirect3DMaterial2> {
        STDMETHOD(QueryInterface)(REFIID r, void** out) override { return o->QI(r, out); }
        STDMETHOD_(ULONG, AddRef)() override { return o->AddRef(); }
        STDMETHOD_(ULONG, Release)() override { return o->Release(); }
        STDMETHOD(SetMaterial)(D3DMATERIAL* m) override { return o->Set(m); }
        STDMETHOD(GetMaterial)(D3DMATERIAL* m) override { return o->Get(m); }
        STDMETHOD(GetHandle)(IDirect3DDevice2* d, D3DMATERIALHANDLE* h) override;
    } f2;
    struct F3 final : MatFacet<IDirect3DMaterial3> {
        STDMETHOD(QueryInterface)(REFIID r, void** out) override { return o->QI(r, out); }
        STDMETHOD_(ULONG, AddRef)() override { return o->AddRef(); }
        STDMETHOD_(ULONG, Release)() override { return o->Release(); }
        STDMETHOD(SetMaterial)(D3DMATERIAL* m) override { return o->Set(m); }
        STDMETHOD(GetMaterial)(D3DMATERIAL* m) override { return o->Get(m); }
        STDMETHOD(GetHandle)(IDirect3DDevice3* d, D3DMATERIALHANDLE* h) override;
    } f3;
    void DestroyHandle(MatHandle* h);  // removes it from its device too
    void Orphan();
};

// ---------------------------------------------------------------- texture (C §4)
template <class I> struct TexFacet : I {
    Texture* o;
};

class Texture final : public Inner {
public:
    Texture(Surface* s, int version, int iid) : s(s), version(version), iid(iid) {
        f1.o = this;
        f2.o = this;
    }
    Surface* s;
    int version;  // the surface interface it was queried through (AddRef/Release go there)
    int iid;      // 1: IDirect3DTexture, 2: IDirect3DTexture2
    HRESULT InnerQuery(REFIID r, void** out) override;
    void SurfaceGone() override;
    HRESULT GetHandle(Device* dev, D3DTEXTUREHANDLE* h, bool v2);
    HRESULT Load(Texture* src);

    struct F1 final : TexFacet<IDirect3DTexture> {
        STDMETHOD(QueryInterface)(REFIID r, void** out) override;
        STDMETHOD_(ULONG, AddRef)() override;
        STDMETHOD_(ULONG, Release)() override;
        STDMETHOD(Initialize)(IDirect3DDevice*, IDirectDrawSurface*) override { return DDERR_ALREADYINITIALIZED; }
        STDMETHOD(GetHandle)(IDirect3DDevice* d, D3DTEXTUREHANDLE* h) override;
        STDMETHOD(PaletteChanged)(DWORD, DWORD) override { return D3D_OK; }
        STDMETHOD(Load)(IDirect3DTexture* src) override;
        STDMETHOD(Unload)() override { return D3D_OK; }
    } f1;
    struct F2 final : TexFacet<IDirect3DTexture2> {
        STDMETHOD(QueryInterface)(REFIID r, void** out) override;
        STDMETHOD_(ULONG, AddRef)() override;
        STDMETHOD_(ULONG, Release)() override;
        STDMETHOD(GetHandle)(IDirect3DDevice2* d, D3DTEXTUREHANDLE* h) override;
        STDMETHOD(PaletteChanged)(DWORD, DWORD) override { return D3D_OK; }
        STDMETHOD(Load)(IDirect3DTexture2* src) override;
    } f2;
};
Texture* TexFrom(IDirect3DTexture* p);
HRESULT CopyTexture(Surface* dst, Surface* src);  // Load / D3DOP_TEXTURELOAD (C §4.2, §6.6)
Texture* TexFrom(IDirect3DTexture2* p);

// ---------------------------------------------------------------- execute buffer (C §5)
class ExecBuf final : public IDirect3DExecuteBuffer {
public:
    ExecBuf(Device* dev, DWORD size);
    ~ExecBuf();
    STDMETHOD(QueryInterface)(REFIID r, void** out) override;
    STDMETHOD_(ULONG, AddRef)() override { return ULONG(InterlockedIncrement(&ref_)); }
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Initialize)(IDirect3DDevice*, D3DEXECUTEBUFFERDESC*) override { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(Lock)(D3DEXECUTEBUFFERDESC* d) override;
    STDMETHOD(Unlock)() override;
    STDMETHOD(SetExecuteData)(D3DEXECUTEDATA* d) override;
    STDMETHOD(GetExecuteData)(D3DEXECUTEDATA* d) override;
    STDMETHOD(Validate)(DWORD*, LPD3DVALIDATECALLBACK, void*, DWORD) override { return E_NOTIMPL; }
    STDMETHOD(Optimize)(DWORD flags) override { return flags ? DDERR_INVALIDPARAMS : E_NOTIMPL; }

    Device* dev;
    std::vector<uint8_t> mem;
    D3DEXECUTEDATA data = {};
    bool locked = false;
    LONG ref_ = 1;
};

// ---------------------------------------------------------------- vertex buffer (C §7)
class VBuf final : public IDirect3DVertexBuffer {
public:
    VBuf(D3D* d3d, const D3DVERTEXBUFFERDESC& d, DWORD flags);
    ~VBuf();
    STDMETHOD(QueryInterface)(REFIID r, void** out) override;
    STDMETHOD_(ULONG, AddRef)() override { return ULONG(InterlockedIncrement(&ref_)); }
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Lock)(DWORD flags, void** data, DWORD* size) override;
    STDMETHOD(Unlock)() override;
    STDMETHOD(ProcessVertices)(DWORD op, DWORD dst, DWORD count, IDirect3DVertexBuffer* src, DWORD srcIndex,
                               IDirect3DDevice3* dev, DWORD flags) override;
    STDMETHOD(GetVertexBufferDesc)(D3DVERTEXBUFFERDESC* d) override;
    STDMETHOD(Optimize)(IDirect3DDevice3* dev, DWORD flags) override;

    D3D* d3d_;
    DWORD caps, fvf, count;
    UINT stride;
    std::vector<uint8_t> mem;
    std::vector<WORD> clip;  // XYZRHW without D3DDP_DONOTCLIP (C §7.1 step 7)
    bool optimized = false;
    DWORD locks = 0;
    LONG ref_ = 1;
};

// ---------------------------------------------------------------- viewport (C §1)
class Viewport final : public IDirect3DViewport3 {
public:
    explicit Viewport(D3D* d3d);
    ~Viewport();
    STDMETHOD(QueryInterface)(REFIID r, void** out) override;
    STDMETHOD_(ULONG, AddRef)() override { return ULONG(InterlockedIncrement(&ref_)); }
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(Initialize)(IDirect3D*) override { return DDERR_ALREADYINITIALIZED; }
    STDMETHOD(GetViewport)(D3DVIEWPORT* d) override;
    STDMETHOD(SetViewport)(D3DVIEWPORT* d) override;
    STDMETHOD(TransformVertices)(DWORD n, D3DTRANSFORMDATA* d, DWORD flags, DWORD* offscreen) override;
    STDMETHOD(LightElements)(DWORD, D3DLIGHTDATA*) override { return E_NOTIMPL; }
    STDMETHOD(SetBackground)(D3DMATERIALHANDLE h) override;
    STDMETHOD(GetBackground)(D3DMATERIALHANDLE* h, WINBOOL* valid) override;
    STDMETHOD(SetBackgroundDepth)(IDirectDrawSurface* s) override;
    STDMETHOD(GetBackgroundDepth)(IDirectDrawSurface** s, WINBOOL* valid) override;
    STDMETHOD(Clear)(DWORD n, D3DRECT* rects, DWORD flags) override;
    STDMETHOD(AddLight)(IDirect3DLight* l) override;
    STDMETHOD(DeleteLight)(IDirect3DLight* l) override;
    STDMETHOD(NextLight)(IDirect3DLight* ref, IDirect3DLight** out, DWORD flags) override;
    STDMETHOD(GetViewport2)(D3DVIEWPORT2* d) override;
    STDMETHOD(SetViewport2)(D3DVIEWPORT2* d) override;
    STDMETHOD(SetBackgroundDepth2)(IDirectDrawSurface4* s) override;
    STDMETHOD(GetBackgroundDepth2)(IDirectDrawSurface4** s, WINBOOL* valid) override;
    STDMETHOD(Clear2)(DWORD n, D3DRECT* rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil) override;

    D3D* d3d_;
    Device* dev = nullptr;          // AddViewport's device
    DWORD id;                       // per Direct3D object, from 1 (C §1)
    D3DVIEWPORT2 data = {};
    bool dataSet = false;
    bool bgSet = false;
    DWORD bg = 0;                   // background material handle
    Surface* bgDepth = nullptr;     // held through bgDepthSlot (-1: no reference, SetBackgroundDepth)
    int bgDepthSlot = -1;
    void* bgDepthIface = nullptr;   // the interface pointer the game gave / GetBackgroundDepth returns
    std::vector<Light*> lights;     // in the order added (XP's list walked from the oldest)
    bool lightsDirty = true;
    void Orphan();

private:
    HRESULT ProcessRects(DWORD n, const D3DRECT* rects, std::vector<RECT>* out) const;
    LONG ref_ = 1;
};
Viewport* VpFrom(IUnknown* p);  // any IDirect3DViewport version (one pointer)

// ---------------------------------------------------------------- the CPU pipeline (d3d6pipe.cpp)
// A vertex through the front end: XP's clip space (0 <= x', y', z' <= w inside, C §1.6),
// the screen form (for outside vertices of a clipped batch, XP's TL buffer holds the clip
// coordinates there instead), colours and texture coordinates.
struct PVert {
    float cx, cy, cz, cw;     // clip space
    float sx, sy, sz, rhw;    // screen
    DWORD diffuse, specular;
    float tex[8][2];
    WORD code;                // D3DCLIP_* bits; 0 inside
};

// The viewport's mapping (C §1.6), recomputed when a viewport is downloaded.
struct VpMap {
    bool valid = false;
    float X = 0, Y = 0, W = 1, H = 1;          // the viewport rectangle
    D3DMATRIX mclip = kIdentity;             // projected point -> XP's clip space
    float cx = -1, cy = 1, cw = 2, ch = 2, zmin = 0, zmax = 1;
};

std::unordered_map<DWORD, D3DMATRIX>& Matrices();
DWORD NewMatrixHandle();

// ---------------------------------------------------------------- device (B)
template <class I> struct DevFacet : I {
    Device* o;
};

class Device final : public Inner {
public:
    Device(D3D* d3d, DevKind kind, const GUID& guid, int version, Surface* rt, int rtVersion, Surface* z, Palette* pal);
    ~Device() override;

    // IUnknown (B §10): version 1 is the surface's aggregate.
    HRESULT QI(REFIID r, void** out);
    ULONG AddRef();
    ULONG Release();
    HRESULT InnerQuery(REFIID r, void** out) override;
    void SurfaceGone() override;
    void Orphan();  // the Direct3D object went (A §~DIRECT3DI)

    D3D* d3d;
    DDraw* dd;
    DevKind kind;
    GUID guid;
    int version;
    Surface* rt;
    int rtVersion;  // the render target's interface version held (v2) / handed out
    Surface* z;
    Palette* pal;
    D3DDEVICEDESC hal = {}, hel = {};
    LONG ref = 1;
    int outerSlot = -1;  // version 1: the surface slot AddRef/Release go to
    int zSlot = -1;
    bool rtUnheld = false;  // SetRenderTarget to a Z buffer: set without a reference (B §7.1)      // version 2: the Z buffer's slot after SetRenderTarget (creation: rtVersion's)

    // ---- front-end state (B §1.3), answered by every Get
    DWORD rs[256];
    DWORD tss[8][64];
    Texture* stageTex[8] = {};  // Device3::SetTexture (holding the surface)
    struct LightStates {
        DWORD material = 0, ambient = 0, colorModel = 0, fogMode = 0;
        DWORD fogStart = 0, fogEnd = 0, fogDensity = 0;  // floats
        bool colorVertex = true;
    } ls;
    std::bitset<512> overRS, overLS, overXS;  // execute-buffer override bits (C §6.2)
    D3DMATRIX world = kIdentity, view = kIdentity, proj = kIdentity;
    D3DMATERIAL curMat = {};  // the material handle's data, for lighting
    // v1 matrix handles (B §4.2): XP's are pointers, good on any device (Wine's test_multiple_devices).
    std::unordered_map<DWORD, D3DMATRIX>& matrices = Matrices();
    DWORD bound[3] = {};  // matrix handles bound by STATETRANSFORM: world, view, projection
    std::vector<MatHandle*> matHandles;
    std::vector<DWORD> texHandles;  // texture handles GetHandle gave for this device
    std::vector<ExecBuf*> execBufs;
    std::vector<Viewport*> viewports;  // in the order added
    Viewport* cur = nullptr;           // the current viewport
    DWORD downloaded = 0;              // id of the viewport whose data the device has (0: none)
    VpMap map;
    DWORD status = 0x01FFF000;         // clip status / D3DSTATUS (C §6.2)
    float ext[4] = {2048.0f, 2048.0f, 0.0f, 0.0f};  // extents minx, miny, maxx, maxy
    D3DSTATS stats = {sizeof(D3DSTATS), 0, 0, 0, 0, 0};
    bool inScene = false, lostInScene = false;
    // Begin/End (B §8.5)
    bool inBegin = false;
    D3DPRIMITIVETYPE beginType = D3DPT_POINTLIST;
    DWORD beginFvf = 0, beginFlags = 0;
    bool beginIndexed = false;
    const uint8_t* beginVerts = nullptr;
    DWORD beginCount = 0;
    std::vector<uint8_t> batchV;
    std::vector<WORD> batchI;
    // Execute (C §6)
    std::vector<PVert> tl;       // the TL buffer, indices of execute-buffer primitives
    DWORD pickCount = 0;

    // ---- the emulated driver (B §1.4): what a DX6 HAL had, pushed to Direct3D 9
    GpuContext ctx;
    DWORD dtss[8][32];        // driver stage states (Direct3D 6 numbers)
    Surface* dtex[8] = {};    // driver stage textures (Device3 SetTexture)
    DWORD dTexHandle = 0;     // driver TEXTUREHANDLE
    DWORD dWrap0 = 0;
    struct Use {
        GpuGuard g;
        bool ok;
        explicit Use(Device* d) : g(d->dd->GetGpu()) {
            bool fresh = false;
            ok = d->ctx.Begin(&fresh);
            if (ok && fresh) d->PushAll();
        }
    };
    void PushAll();
    void DrvRS(DWORD s, DWORD v);            // a render state reaches the driver
    void DrvTSS(DWORD stage, DWORD t, DWORD v);
    void DrvTexture(DWORD stage, Surface* s);
    void PushRS9(DWORD s);                   // to Direct3D 9 (driver values)
    void PushStage9(DWORD stage, DWORD t);
    void PushAlpha9();                       // alpha test with COLORKEYENABLE's emulation
    void PushStageOps9();                    // stage 0/1 ops (legacy texture, MODULATE's alpha)
    Surface* StageSurface(DWORD stage) const;
    void BindTextures9();

    // ---- front-end operations
    HRESULT SetRS(DWORD s, DWORD v, bool fromExecute);
    HRESULT SetLS(DWORD s, DWORD v);   // STATELIGHT (B §2.1)
    HRESULT SetTSS(DWORD stage, DWORD t, DWORD v);
    HRESULT SetTex(DWORD stage, Texture* t);
    void SetMaterialHandle(DWORD h);
    D3DMATRIX* Xform(DWORD t);         // WORLD, VIEW, PROJECTION or null
    void MatrixChanged(DWORD handle);  // a bound matrix handle was written
    HRESULT Download(Viewport* vp);    // downloadView (C §1.6)
    HRESULT CheckViewport();           // the current viewport, downloaded (B §5.6)
    void ApplyViewport9();             // takes the Gpu lock
    void SetViewport9();               // Use held
    void RemoveViewport(Viewport* vp); // DeleteViewport's unlinking
    bool TargetsBusy() const;
    HRESULT DrawCommon(D3DPRIMITIVETYPE t, DWORD fvf, const uint8_t* v, UINT stride, DWORD n, const WORD* idx,
                       DWORD ni, DWORD flags, const WORD* vbClip);
    HRESULT ExecuteI(ExecBuf* eb, DWORD flags);
    HRESULT FlushBegin(bool end);
    void ForgetSurface(Surface* s);     // a surface it refers to is being destroyed
    void InvalidateTexHandles();        // SetRenderTarget (B §3.5)
    HRESULT SetRenderTargetI(Surface* s, int sVersion);
    HRESULT GetCapsI(D3DDEVICEDESC* hal, D3DDEVICEDESC* hel);
    HRESULT EnumTexFormats(void* cb, void* ctx, bool v3);
    HRESULT BeginSceneI();
    HRESULT EndSceneI();
    HRESULT AddViewportI(Viewport* vp);
    HRESULT DeleteViewportI(Viewport* vp);
    HRESULT NextViewportI(Viewport* ref, Viewport** out, DWORD flags);
    HRESULT SetCurrentViewportI(Viewport* vp);
    HRESULT GetCurrentViewportI(Viewport** out);
    HRESULT SetTransformI(DWORD t, const D3DMATRIX* m);
    HRESULT GetTransformI(DWORD t, D3DMATRIX* m);
    HRESULT MultiplyTransformI(DWORD t, const D3DMATRIX* m);
    HRESULT GetLightStateI(DWORD s, DWORD* v);
    HRESULT SetClipStatusI(const D3DCLIPSTATUS* cs);
    HRESULT GetClipStatusI(D3DCLIPSTATUS* cs);
    HRESULT GetStatsI(D3DSTATS* s);
    HRESULT BeginI(D3DPRIMITIVETYPE t, DWORD fvf, const void* verts, DWORD n, DWORD flags, bool indexed);
    HRESULT VertexI(const void* v);
    HRESULT IndexI(WORD i);
    HRESULT EndI(DWORD flags);
    HRESULT SwapTextureHandlesI(Texture* a, Texture* b);
    HRESULT ExecuteE(ExecBuf* eb, Viewport* vp, DWORD flags);
    HRESULT CreateExecuteBufferI(D3DEXECUTEBUFFERDESC* d, IDirect3DExecuteBuffer** out, IUnknown* outer);
    HRESULT DrawPrimitiveVBI(D3DPRIMITIVETYPE t, VBuf* vb, DWORD start, DWORD n, const WORD* idx, DWORD ni,
                             DWORD flags);
    HRESULT ComputeSphereVisibilityI(const D3DVECTOR* c, const D3DVALUE* r, DWORD n, DWORD* ret);

    struct F1 final : DevFacet<IDirect3DDevice> {
        STDMETHOD(QueryInterface)(REFIID r, void** out) override { return o->QI(r, out); }
        STDMETHOD_(ULONG, AddRef)() override { return o->AddRef(); }
        STDMETHOD_(ULONG, Release)() override { return o->Release(); }
        STDMETHOD(Initialize)(IDirect3D*, GUID*, D3DDEVICEDESC*) override { return DDERR_ALREADYINITIALIZED; }
        STDMETHOD(GetCaps)(D3DDEVICEDESC* h, D3DDEVICEDESC* s) override { return o->GetCapsI(h, s); }
        STDMETHOD(SwapTextureHandles)(IDirect3DTexture* a, IDirect3DTexture* b) override;
        STDMETHOD(CreateExecuteBuffer)(D3DEXECUTEBUFFERDESC* d, IDirect3DExecuteBuffer** b, IUnknown* outer) override {
            return o->CreateExecuteBufferI(d, b, outer);
        }
        STDMETHOD(GetStats)(D3DSTATS* s) override { return o->GetStatsI(s); }
        STDMETHOD(Execute)(IDirect3DExecuteBuffer* b, IDirect3DViewport* v, DWORD flags) override;
        STDMETHOD(AddViewport)(IDirect3DViewport* v) override;
        STDMETHOD(DeleteViewport)(IDirect3DViewport* v) override;
        STDMETHOD(NextViewport)(IDirect3DViewport* ref, IDirect3DViewport** out, DWORD flags) override;
        STDMETHOD(Pick)(IDirect3DExecuteBuffer* b, IDirect3DViewport* v, DWORD flags, D3DRECT* r) override;
        STDMETHOD(GetPickRecords)(DWORD* n, D3DPICKRECORD* r) override;
        STDMETHOD(EnumTextureFormats)(LPD3DENUMTEXTUREFORMATSCALLBACK cb, void* ctx) override {
            return o->EnumTexFormats(reinterpret_cast<void*>(cb), ctx, false);
        }
        STDMETHOD(CreateMatrix)(D3DMATRIXHANDLE* h) override;
        STDMETHOD(SetMatrix)(D3DMATRIXHANDLE h, D3DMATRIX* m) override;
        STDMETHOD(GetMatrix)(D3DMATRIXHANDLE h, D3DMATRIX* m) override;
        STDMETHOD(DeleteMatrix)(D3DMATRIXHANDLE h) override;
        STDMETHOD(BeginScene)() override { return o->BeginSceneI(); }
        STDMETHOD(EndScene)() override { return o->EndSceneI(); }
        STDMETHOD(GetDirect3D)(IDirect3D** out) override;
    } f1;
    struct F2 final : DevFacet<IDirect3DDevice2> {
        STDMETHOD(QueryInterface)(REFIID r, void** out) override { return o->QI(r, out); }
        STDMETHOD_(ULONG, AddRef)() override { return o->AddRef(); }
        STDMETHOD_(ULONG, Release)() override { return o->Release(); }
        STDMETHOD(GetCaps)(D3DDEVICEDESC* h, D3DDEVICEDESC* s) override { return o->GetCapsI(h, s); }
        STDMETHOD(SwapTextureHandles)(IDirect3DTexture2* a, IDirect3DTexture2* b) override;
        STDMETHOD(GetStats)(D3DSTATS* s) override { return o->GetStatsI(s); }
        STDMETHOD(AddViewport)(IDirect3DViewport2* v) override;
        STDMETHOD(DeleteViewport)(IDirect3DViewport2* v) override;
        STDMETHOD(NextViewport)(IDirect3DViewport2* ref, IDirect3DViewport2** out, DWORD flags) override;
        STDMETHOD(EnumTextureFormats)(LPD3DENUMTEXTUREFORMATSCALLBACK cb, void* ctx) override {
            return o->EnumTexFormats(reinterpret_cast<void*>(cb), ctx, false);
        }
        STDMETHOD(BeginScene)() override { return o->BeginSceneI(); }
        STDMETHOD(EndScene)() override { return o->EndSceneI(); }
        STDMETHOD(GetDirect3D)(IDirect3D2** out) override;
        STDMETHOD(SetCurrentViewport)(IDirect3DViewport2* v) override;
        STDMETHOD(GetCurrentViewport)(IDirect3DViewport2** v) override;
        STDMETHOD(SetRenderTarget)(IDirectDrawSurface* s, DWORD flags) override;
        STDMETHOD(GetRenderTarget)(IDirectDrawSurface** s) override;
        STDMETHOD(Begin)(D3DPRIMITIVETYPE t, D3DVERTEXTYPE vt, DWORD flags) override;
        STDMETHOD(BeginIndexed)(D3DPRIMITIVETYPE t, D3DVERTEXTYPE vt, void* v, DWORD n, DWORD flags) override;
        STDMETHOD(Vertex)(void* v) override { return o->VertexI(v); }
        STDMETHOD(Index)(WORD i) override { return o->IndexI(i); }
        STDMETHOD(End)(DWORD flags) override { return o->EndI(flags); }
        STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE s, LPDWORD v) override;
        STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE s, DWORD v) override;
        STDMETHOD(GetLightState)(D3DLIGHTSTATETYPE s, LPDWORD v) override { return o->GetLightStateI(s, v); }
        STDMETHOD(SetLightState)(D3DLIGHTSTATETYPE s, DWORD v) override;
        STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) override { return o->SetTransformI(t, m); }
        STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) override { return o->GetTransformI(t, m); }
        STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) override {
            return o->MultiplyTransformI(t, m);
        }
        STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE t, D3DVERTEXTYPE vt, void* v, DWORD n, DWORD flags) override;
        STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE t, D3DVERTEXTYPE vt, void* v, DWORD n, WORD* idx, DWORD ni,
                                        DWORD flags) override;
        STDMETHOD(SetClipStatus)(D3DCLIPSTATUS* cs) override { return o->SetClipStatusI(cs); }
        STDMETHOD(GetClipStatus)(D3DCLIPSTATUS* cs) override { return o->GetClipStatusI(cs); }
    } f2;
    struct F3 final : DevFacet<IDirect3DDevice3> {
        STDMETHOD(QueryInterface)(REFIID r, void** out) override { return o->QI(r, out); }
        STDMETHOD_(ULONG, AddRef)() override { return o->AddRef(); }
        STDMETHOD_(ULONG, Release)() override { return o->Release(); }
        STDMETHOD(GetCaps)(D3DDEVICEDESC* h, D3DDEVICEDESC* s) override { return o->GetCapsI(h, s); }
        STDMETHOD(GetStats)(D3DSTATS* s) override { return o->GetStatsI(s); }
        STDMETHOD(AddViewport)(IDirect3DViewport3* v) override;
        STDMETHOD(DeleteViewport)(IDirect3DViewport3* v) override;
        STDMETHOD(NextViewport)(IDirect3DViewport3* ref, IDirect3DViewport3** out, DWORD flags) override;
        STDMETHOD(EnumTextureFormats)(LPD3DENUMPIXELFORMATSCALLBACK cb, void* ctx) override {
            return o->EnumTexFormats(reinterpret_cast<void*>(cb), ctx, true);
        }
        STDMETHOD(BeginScene)() override { return o->BeginSceneI(); }
        STDMETHOD(EndScene)() override { return o->EndSceneI(); }
        STDMETHOD(GetDirect3D)(IDirect3D3** out) override;
        STDMETHOD(SetCurrentViewport)(IDirect3DViewport3* v) override;
        STDMETHOD(GetCurrentViewport)(IDirect3DViewport3** v) override;
        STDMETHOD(SetRenderTarget)(IDirectDrawSurface4* s, DWORD flags) override;
        STDMETHOD(GetRenderTarget)(IDirectDrawSurface4** s) override;
        STDMETHOD(Begin)(D3DPRIMITIVETYPE t, DWORD fvf, DWORD flags) override;
        STDMETHOD(BeginIndexed)(D3DPRIMITIVETYPE t, DWORD fvf, void* v, DWORD n, DWORD flags) override;
        STDMETHOD(Vertex)(void* v) override { return o->VertexI(v); }
        STDMETHOD(Index)(WORD i) override { return o->IndexI(i); }
        STDMETHOD(End)(DWORD flags) override { return o->EndI(flags); }
        STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE s, LPDWORD v) override { return o->f2.GetRenderState(s, v); }
        STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE s, DWORD v) override { return o->f2.SetRenderState(s, v); }
        STDMETHOD(GetLightState)(D3DLIGHTSTATETYPE s, LPDWORD v) override { return o->GetLightStateI(s, v); }
        STDMETHOD(SetLightState)(D3DLIGHTSTATETYPE s, DWORD v) override { return o->f2.SetLightState(s, v); }
        STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) override { return o->SetTransformI(t, m); }
        STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) override { return o->GetTransformI(t, m); }
        STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) override {
            return o->MultiplyTransformI(t, m);
        }
        STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE t, DWORD fvf, void* v, DWORD n, DWORD flags) override;
        STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE t, DWORD fvf, void* v, DWORD n, WORD* idx, DWORD ni,
                                        DWORD flags) override;
        STDMETHOD(SetClipStatus)(D3DCLIPSTATUS* cs) override { return o->SetClipStatusI(cs); }
        STDMETHOD(GetClipStatus)(D3DCLIPSTATUS* cs) override { return o->GetClipStatusI(cs); }
        STDMETHOD(DrawPrimitiveStrided)(D3DPRIMITIVETYPE t, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA* d, DWORD n,
                                        DWORD flags) override;
        STDMETHOD(DrawIndexedPrimitiveStrided)(D3DPRIMITIVETYPE t, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA* d, DWORD n,
                                               WORD* idx, DWORD ni, DWORD flags) override;
        STDMETHOD(DrawPrimitiveVB)(D3DPRIMITIVETYPE t, IDirect3DVertexBuffer* vb, DWORD start, DWORD n,
                                   DWORD flags) override;
        STDMETHOD(DrawIndexedPrimitiveVB)(D3DPRIMITIVETYPE t, IDirect3DVertexBuffer* vb, WORD* idx, DWORD ni,
                                          DWORD flags) override;
        STDMETHOD(ComputeSphereVisibility)(D3DVECTOR* c, D3DVALUE* r, DWORD n, DWORD flags, DWORD* ret) override {
            return o->ComputeSphereVisibilityI(c, r, n, ret);
        }
        STDMETHOD(GetTexture)(DWORD stage, IDirect3DTexture2** t) override;
        STDMETHOD(SetTexture)(DWORD stage, IDirect3DTexture2* t) override;
        STDMETHOD(GetTextureStageState)(DWORD stage, D3DTEXTURESTAGESTATETYPE t, LPDWORD v) override;
        STDMETHOD(SetTextureStageState)(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) override;
        STDMETHOD(ValidateDevice)(LPDWORD passes) override;
    } f3;
};
Device* DevFrom(IDirect3DDevice* p);
Device* DevFrom(IDirect3DDevice2* p);
Device* DevFrom(IDirect3DDevice3* p);

// ---- the pipeline (d3d6pipe.cpp)
// Transforms (and lights, fogs) untransformed vertices of a Direct3D 6 FVF into PVerts
// with the device's current transforms and viewport mapping. clip: compute clip codes
// (and merge them into the clip status); light: as B §2.3 decided.
void TransformVerts(Device* d, const uint8_t* src, UINT stride, DWORD fvf, DWORD n, bool clip, bool light,
                    bool extents, PVert* out);
// Lights one vertex position/normal (model space) into diffuse/specular (C §2.2).
void SetupLights(Device* d);
// Draws PVerts (indexed or not): screen coordinates as they are where clip is false or every
// vertex is inside, else clipped against XP's volume first.
void DrawPVerts(Device* d, D3DPRIMITIVETYPE t, const PVert* v, DWORD n, const WORD* idx, DWORD ni, bool clip,
                DWORD ntex);
// Direct3D 6 vertex size: texture coordinates are always two floats (no size bits in DX6).
UINT FvfSize6(DWORD fvf);
// A viewport's mapping (C §1.6); false (and *m untouched) for degenerate data.
bool MakeMap(const D3DVIEWPORT2& v, VpMap* m);
// Viewport::Clear's pieces: a clear of rectangles already clipped (D3D9's viewport widened
// to the render target for it), and the textured background fill (TriFillRectsTex).
void ClearRects(Device* d, const std::vector<RECT>& rects, DWORD flags, DWORD color, float z, DWORD stencil);
void FillRectsTex(Device* d, const std::vector<RECT>& rects, Surface* tex, const Viewport* vp);
inline D3DMATRIX Mul(const D3DMATRIX& a, const D3DMATRIX& b) {
    D3DMATRIX r;
    MatMul(&r, a, b);
    return r;
}

}  // namespace d6
}  // namespace dd2d9
