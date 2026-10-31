// dd2d9's Direct3D 3-6: the Direct3D object and its descriptors, lights, materials,
// textures, execute buffers, vertex buffers, and the hand-off from DirectDraw (A).
#include "d3d6int.h"

#include <cfloat>
#include <new>

namespace dd2d9 {
namespace d6 {

const GUID kIID_NullDevice = {0x8767df22, 0xbacc, 0x11d1, {0x89, 0x69, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
const GUID kIID_NewRGBDevice = {0x15b29400, 0x2abf, 0x11d1, {0x89, 0xaa, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29}};

// IsValidD3DDeviceGuid (A §Direct3DCreateDevice step 1): T&L HAL isn't one.
DevKind KindOf(REFCLSID g) {
    if (IsBadReadPtr(&g, sizeof(GUID))) return kBadGuid;
    if (IsEqualGUID(g, IID_IDirect3DRampDevice)) return kRamp;
    if (IsEqualGUID(g, IID_IDirect3DRGBDevice)) return kRGB;
    if (IsEqualGUID(g, IID_IDirect3DMMXDevice)) return kMMX;
    if (IsEqualGUID(g, IID_IDirect3DHALDevice)) return kHAL;
    if (IsEqualGUID(g, IID_IDirect3DRefDevice)) return kRef;
    if (IsEqualGUID(g, kIID_NullDevice)) return kNull;
    if (IsEqualGUID(g, kIID_NewRGBDevice)) return kNewRGB;
    return kBadGuid;
}

// ---------------------------------------------------------------- descriptors (A)

static void BaseDesc(D3DDEVICEDESC* d) {
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(D3DDEVICEDESC);
    d->dtcTransformCaps.dwSize = sizeof(D3DTRANSFORMCAPS);
    d->dlcLightingCaps.dwSize = sizeof(D3DLIGHTINGCAPS);
    d->dpcLineCaps.dwSize = d->dpcTriCaps.dwSize = sizeof(D3DPRIMCAPS);
}

void NullDesc(D3DDEVICEDESC* d) { BaseDesc(d); }

void HalHalDesc(D3DDEVICEDESC* d) {
    const D3DDEVICEDESC7 h = HalDesc(false);
    BaseDesc(d);
    d->dwFlags = 0x7FF;
    d->dcmColorModel = D3DCOLOR_RGB;
    d->dwDevCaps = h.dwDevCaps & ~DWORD(D3DDEVCAPS_HWTRANSFORMANDLIGHT);
    d->dtcTransformCaps.dwCaps = D3DTRANSFORMCAPS_CLIP;
    d->bClipping = TRUE;
    d->dlcLightingCaps.dwCaps = 0xF;
    d->dlcLightingCaps.dwLightingModel = D3DLIGHTINGMODEL_RGB;
    d->dlcLightingCaps.dwNumLights = 8;
    d->dpcLineCaps = h.dpcLineCaps;
    d->dpcTriCaps = h.dpcTriCaps;
    d->dwDeviceRenderBitDepth = DDBD_16 | DDBD_32;
    d->dwDeviceZBufferBitDepth = h.dwDeviceZBufferBitDepth;
    d->dwMaxBufferSize = 0;
    d->dwMaxVertexCount = 0x400;  // XP's value for a driver's 0
    d->dwMinTextureWidth = h.dwMinTextureWidth;
    d->dwMinTextureHeight = h.dwMinTextureHeight;
    d->dwMaxTextureWidth = h.dwMaxTextureWidth;
    d->dwMaxTextureHeight = h.dwMaxTextureHeight;
    d->dwMaxTextureRepeat = h.dwMaxTextureRepeat;
    d->dwMaxTextureAspectRatio = h.dwMaxTextureAspectRatio;
    d->dwMaxAnisotropy = h.dwMaxAnisotropy;
    d->dvGuardBandLeft = h.dvGuardBandLeft;
    d->dvGuardBandTop = h.dvGuardBandTop;
    d->dvGuardBandRight = h.dvGuardBandRight;
    d->dvGuardBandBottom = h.dvGuardBandBottom;
    d->dvExtentsAdjust = h.dvExtentsAdjust;
    d->dwStencilCaps = h.dwStencilCaps;
    d->dwFVFCaps = h.dwFVFCaps;
    d->dwTextureOpCaps = h.dwTextureOpCaps;
    d->wMaxTextureBlendStages = h.wMaxTextureBlendStages;
    d->wMaxSimultaneousTextures = h.wMaxSimultaneousTextures;
}

void HalHelDesc(D3DDEVICEDESC* d) {
    BaseDesc(d);
    d->dwFlags = 0x1F;
    d->dwDevCaps = D3DDEVCAPS_FLOATTLVERTEX;
    d->dtcTransformCaps.dwCaps = D3DTRANSFORMCAPS_CLIP;
    d->bClipping = TRUE;
    d->dlcLightingCaps.dwCaps = 0xF;
    d->dlcLightingCaps.dwLightingModel = D3DLIGHTINGMODEL_RGB;
    d->dwMaxVertexCount = 0x400;
}

void SoftHelDesc(DevKind k, int version, D3DDEVICEDESC* d) {
    BaseDesc(d);
    const bool ramp = k == kRamp, v3 = version >= 3 && !ramp;
    d->dwFlags = v3 ? 0x7FF : 0x7DF;
    d->dcmColorModel = ramp ? D3DCOLOR_MONO : D3DCOLOR_RGB;
    d->dwDevCaps = v3 ? 0x551 : 0x55B;
    d->dtcTransformCaps.dwCaps = D3DTRANSFORMCAPS_CLIP;
    d->bClipping = TRUE;
    d->dlcLightingCaps.dwCaps = 0xF;
    d->dlcLightingCaps.dwLightingModel = ramp ? D3DLIGHTINGMODEL_MONO : D3DLIGHTINGMODEL_RGB;
    D3DPRIMCAPS& p = d->dpcTriCaps;
    p.dwMiscCaps = 0x70;
    p.dwZCmpCaps = 0xFF;
    p.dwStippleWidth = p.dwStippleHeight = 4;
    if (v3) {
        p.dwRasterCaps = 0x121F7;
        p.dwSrcBlendCaps = 0x1FFF;
        p.dwDestBlendCaps = 0x7FF;
        p.dwAlphaCmpCaps = 0xFF;
        p.dwShadeCaps = 0xC528A;
        p.dwTextureCaps = 0xDF;
        p.dwTextureFilterCaps = 0x0303033F;
        p.dwTextureBlendCaps = 0xCF;
        p.dwTextureAddressCaps = 0x1F;
    } else if (!ramp) {
        p.dwRasterCaps = 0x1D1;
        p.dwShadeCaps = 0xCA28A;
        p.dwTextureCaps = 0xB;
        p.dwTextureFilterCaps = 0x1F;
        p.dwTextureBlendCaps = 0x42;
        p.dwTextureAddressCaps = 0x1;
    } else {
        p.dwRasterCaps = 0x51;
        p.dwShadeCaps = 0xA145;
        p.dwTextureCaps = 0xB;
        p.dwTextureFilterCaps = 0x5;
        p.dwTextureBlendCaps = 0x42;
        p.dwTextureAddressCaps = 0x1;
    }
    d->dpcLineCaps = p;
    d->dwDeviceRenderBitDepth = 0xF00;
    d->dwDeviceZBufferBitDepth = 0x400;
    d->dwMaxVertexCount = 2021;
    if (v3) {
        d->dwMinTextureWidth = d->dwMinTextureHeight = 1;
        d->dwMaxTextureWidth = d->dwMaxTextureHeight = 1024;
        d->dwMaxTextureRepeat = 256;
        d->dwMaxAnisotropy = 1;
        d->dvGuardBandLeft = d->dvGuardBandTop = -32768.0f;
        d->dvGuardBandRight = d->dvGuardBandBottom = 32767.0f;
        d->dwStencilCaps = 0xFF;
        d->dwFVFCaps = 2;
        d->dwTextureOpCaps = 0x78FF;
        d->wMaxTextureBlendStages = d->wMaxSimultaneousTextures = 2;
    }
}

// The device table (A §EnumDevices): writable strings that stay valid, as XP's.
namespace {
GUID g_guids[4] = {IID_IDirect3DRampDevice, IID_IDirect3DRGBDevice, IID_IDirect3DHALDevice, IID_IDirect3DMMXDevice};
char g_names[4][32] = {"Ramp Emulation", "RGB Emulation", "Direct3D HAL", "MMX Emulation"};
char g_descs[4][96] = {"Microsoft Direct3D Mono(Ramp) Software Emulation", "Microsoft Direct3D RGB Software Emulation",
                       "Microsoft Direct3D Hardware acceleration through Direct3D HAL",
                       "Microsoft Direct3D MMX Software Emulation"};
const DevKind kTableKind[4] = {kRamp, kRGB, kHAL, kMMX};

// The devices a version lists, in order (A: IDirect3D Ramp, RGB, HAL; IDirect3D2 also MMX,
// as XP's Drivers registry key exists there (Wine's test_find_device); IDirect3D3 RGB, HAL).
std::vector<int> DeviceList(int version) {
    std::vector<int> l;
    if (version < 3) l.push_back(0);
    l.push_back(1);
    if (GpuGetCaps().valid) l.push_back(2);
    if (version == 2) l.push_back(3);
    return l;
}

void DescsFor(int i, int version, D3DDEVICEDESC* hal, D3DDEVICEDESC* hel) {
    if (kTableKind[i] == kHAL) {
        HalHalDesc(hal);
        HalHelDesc(hel);
    } else {
        NullDesc(hal);
        SoftHelDesc(kTableKind[i], version, hel);
    }
    const DWORD size = version == 1 ? 0xAC : version == 2 ? 0xCC : 0xFC;
    hal->dwSize = hel->dwSize = size;
}

bool CodeOk(const void* cb) { return cb && !IsBadCodePtr(reinterpret_cast<FARPROC>(const_cast<void*>(cb))); }

// The surface interface version of a pointer the game passed.
int VersionOfIface(Surface* s, void* iface) {
    for (int v : {1, 2, 3, 4, 7})
        if (s->Iface(v) == iface) return v;
    return 1;
}
}  // namespace

// ---------------------------------------------------------------- Direct3D object

D3D::D3D(DDraw* dd, int outer) : dd(dd), outer(outer) {
    f1.o = f2.o = f3.o = this;
}

D3D::~D3D() = default;

HRESULT D3D::InnerQuery(REFIID r, void** out) {
    void* p = nullptr;
    if (IsEqualGUID(r, IID_IDirect3D)) p = static_cast<IDirect3D*>(&f1);
    else if (IsEqualGUID(r, IID_IDirect3D2)) p = static_cast<IDirect3D2*>(&f2);
    else if (IsEqualGUID(r, IID_IDirect3D3)) p = static_cast<IDirect3D3*>(&f3);
    if (!p) return E_NOINTERFACE;
    dd->AddRef(outer);
    *out = p;
    return S_OK;
}

HRESULT D3D::EnumDevices(LPD3DENUMDEVICESCALLBACK cb, void* ctx, int version) {
    if (!CodeOk(reinterpret_cast<void*>(cb))) return DDERR_INVALIDPARAMS;
    for (int i : DeviceList(version)) {
        D3DDEVICEDESC hal, hel;
        DescsFor(i, version, &hal, &hel);
        if (cb(&g_guids[i], g_descs[i], g_names[i], &hal, &hel, ctx) != D3DENUMRET_OK) break;
    }
    return D3D_OK;
}

// A §FindDevice.
HRESULT D3D::FindDevice(D3DFINDDEVICESEARCH* s, D3DFINDDEVICERESULT* r, int version) {
    if (!s || s->dwSize != sizeof(D3DFINDDEVICESEARCH) || !r ||
        (r->dwSize != 0x16C && r->dwSize != 0x1AC && r->dwSize != 0x20C))
        return DDERR_INVALIDPARAMS;
    Guard l;
    int found = -1;
    bool foundHw = false;
    D3DDEVICEDESC fh = {}, fs = {};
    for (int i : DeviceList(version)) {
        D3DDEVICEDESC hal, hel;
        DescsFor(i, version, &hal, &hel);
        const bool hw = hal.dcmColorModel != 0;
        const DWORD f = s->dwFlags;
        if ((f & D3DFDS_GUID) && !IsEqualGUID(s->guid, g_guids[i])) continue;
        if ((f & D3DFDS_HARDWARE) && bool(s->bHardware) != hw) continue;
        if ((f & D3DFDS_COLORMODEL) && !(hal.dcmColorModel & s->dcmColorModel) && !(hel.dcmColorModel & s->dcmColorModel))
            continue;
        auto capsOk = [&](const D3DPRIMCAPS& c) {
            const D3DPRIMCAPS& w = s->dpcPrimCaps;
            auto t = [&](DWORD flag, DWORD dev, DWORD want) { return !(f & flag) || (dev & want) == want; };
            return t(D3DFDS_MISCCAPS, c.dwMiscCaps, w.dwMiscCaps) && t(D3DFDS_RASTERCAPS, c.dwRasterCaps, w.dwRasterCaps) &&
                   t(D3DFDS_ZCMPCAPS, c.dwZCmpCaps, w.dwZCmpCaps) &&
                   t(D3DFDS_ALPHACMPCAPS, c.dwAlphaCmpCaps, w.dwAlphaCmpCaps) &&
                   t(D3DFDS_SRCBLENDCAPS, c.dwSrcBlendCaps, w.dwSrcBlendCaps) &&
                   t(D3DFDS_DSTBLENDCAPS, c.dwDestBlendCaps, w.dwDestBlendCaps) &&
                   t(D3DFDS_SHADECAPS, c.dwShadeCaps, w.dwShadeCaps) &&
                   t(D3DFDS_TEXTURECAPS, c.dwTextureCaps, w.dwTextureCaps) &&
                   t(D3DFDS_TEXTUREFILTERCAPS, c.dwTextureFilterCaps, w.dwTextureFilterCaps) &&
                   t(D3DFDS_TEXTUREBLENDCAPS, c.dwTextureBlendCaps, w.dwTextureBlendCaps) &&
                   t(D3DFDS_TEXTUREADDRESSCAPS, c.dwTextureAddressCaps, w.dwTextureAddressCaps);
        };
        if ((f & D3DFDS_TRIANGLES) && !capsOk(hal.dpcTriCaps) && !capsOk(hel.dpcTriCaps)) continue;
        if ((f & D3DFDS_LINES) && !capsOk(hal.dpcLineCaps) && !capsOk(hel.dpcLineCaps)) continue;
        if (hw || !foundHw) {  // a hardware match always wins; software only while none did
            found = i;
            foundHw = foundHw || hw;
            fh = hal;
            fs = hel;
        }
    }
    if (found < 0) return DDERR_NOTFOUND;
    r->guid = g_guids[found];
    if (r->dwSize == 0x20C) {
        r->ddHwDesc = fh;
        r->ddSwDesc = fs;
    } else {
        const DWORD n = (r->dwSize - 0x14) / 2;
        uint8_t* p = reinterpret_cast<uint8_t*>(r) + 0x14;
        memcpy(p, &fh, n);
        memcpy(p + n, &fs, n);
    }
    return D3D_OK;
}

HRESULT D3D::CreateLight(IDirect3DLight** out, IUnknown* outerUnk) {
    if (outerUnk) return CLASS_E_NOAGGREGATION;
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    *out = nullptr;
    Light* light = new (std::nothrow) Light(this);
    if (!light) return E_OUTOFMEMORY;
    lights.push_back(light);
    *out = light;
    return D3D_OK;
}

HRESULT D3D::CreateMaterial(Material** out, IUnknown* outerUnk) {
    if (outerUnk) return CLASS_E_NOAGGREGATION;
    if (!out) return DDERR_INVALIDPARAMS;  // v1/v2 fault on XP
    Guard l;
    *out = nullptr;
    Material* m = new (std::nothrow) Material(this);
    if (!m) return E_OUTOFMEMORY;
    materials.push_back(m);
    *out = m;
    return D3D_OK;
}

HRESULT D3D::F1::CreateMaterial(IDirect3DMaterial** out, IUnknown* outerUnk) {
    if (!out) return DDERR_INVALIDPARAMS;  // (XP v1/v2 fault; nothing is made)
    Material* m;
    const HRESULT hr = o->CreateMaterial(&m, outerUnk);
    if (SUCCEEDED(hr)) *out = &m->f1;
    return hr;
}
HRESULT D3D::F2::CreateMaterial(IDirect3DMaterial2** out, IUnknown* outerUnk) {
    if (!out) return DDERR_INVALIDPARAMS;  // (XP v1/v2 fault; nothing is made)
    Material* m;
    const HRESULT hr = o->CreateMaterial(&m, outerUnk);
    if (SUCCEEDED(hr)) *out = &m->f2;
    return hr;
}
HRESULT D3D::F3::CreateMaterial(IDirect3DMaterial3** out, IUnknown* outerUnk) {
    if (outerUnk) return CLASS_E_NOAGGREGATION;
    if (!out) return DDERR_INVALIDPARAMS;  // (XP v1/v2 fault; nothing is made)
    Material* m;
    const HRESULT hr = o->CreateMaterial(&m, outerUnk);
    if (SUCCEEDED(hr)) *out = &m->f3;
    return hr;
}

HRESULT D3D::CreateViewport(Viewport** out, IUnknown* outerUnk) {
    if (outerUnk) return CLASS_E_NOAGGREGATION;
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    *out = nullptr;
    Viewport* v = new (std::nothrow) Viewport(this);
    if (!v) return E_OUTOFMEMORY;
    viewports.push_back(v);
    *out = v;
    return D3D_OK;
}

// IDirect3D and IDirect3D2 ignore the outer unknown (A §CreateViewport).
HRESULT D3D::F1::CreateViewport(IDirect3DViewport** out, IUnknown*) {
    if (!out) return DDERR_INVALIDPARAMS;  // (would leak a viewport no one can reach)
    Viewport* v;
    const HRESULT hr = o->CreateViewport(&v, nullptr);
    if (out) *out = SUCCEEDED(hr) ? v : nullptr;
    return hr;
}
HRESULT D3D::F2::CreateViewport(IDirect3DViewport2** out, IUnknown*) {
    if (!out) return DDERR_INVALIDPARAMS;
    Viewport* v;
    const HRESULT hr = o->CreateViewport(&v, nullptr);
    if (out) *out = SUCCEEDED(hr) ? v : nullptr;
    return hr;
}
HRESULT D3D::F3::CreateViewport(IDirect3DViewport3** out, IUnknown* outerUnk) {
    if (outerUnk) return CLASS_E_NOAGGREGATION;
    if (!out) return DDERR_INVALIDPARAMS;
    Viewport* v;
    const HRESULT hr = o->CreateViewport(&v, outerUnk);
    if (out && hr != CLASS_E_NOAGGREGATION) *out = SUCCEEDED(hr) ? v : nullptr;
    return hr;
}

// Direct3DCreateDevice and Init (A).
HRESULT D3D::CreateDeviceI(REFCLSID guid, Surface* rt, int rtVersion, int version, Device** out) {
    *out = nullptr;
    const DevKind kind = KindOf(guid);
    if (kind == kBadGuid) return DDERR_INVALIDPARAMS;
    if (version >= 3 && kind == kRamp) return DDERR_INVALIDPARAMS;
    const DWORD caps = rt->desc.ddsCaps.dwCaps;
    if (!(caps & DDSCAPS_3DDEVICE) || (caps & DDSCAPS_ZBUFFER)) return DDERR_INVALIDCAPS;
    if (rt->W() > 2048 || rt->H() > 2048) return DDERR_INVALIDOBJECT;
    const Format& f = rt->buf->fmt;
    if (!rt->palette && f.bpp < 16) return DDERR_NOPALETTEATTACHED;
    Surface* z = AttachedZ(rt);
    // The HAL provider (A Init 9): Ref and Null need d3dref.dll, which XP doesn't have.
    if (kind == kRef || kind == kNull) return E_NOINTERFACE;
    if (kind == kHAL && !GpuGetCaps().valid) return E_NOINTERFACE;
    const bool hw = kind == kHAL;
    // checkDeviceSurface (A Init 11).
    if (hw && !(caps & DDSCAPS_VIDEOMEMORY)) return D3DERR_SURFACENOTINVIDMEM;
    if (f.fourcc || f.zbuf) return DDERR_INVALIDPIXELFORMAT;
    {
        D3DDEVICEDESC hal, hel;
        if (hw) {
            HalHalDesc(&hal);
            HalHelDesc(&hel);
        } else {
            NullDesc(&hal);
            SoftHelDesc(kind, version, &hel);
        }
        const DWORD bd = BitsToDDBD(f.bpp);
        if (!bd || !((hal.dwDeviceRenderBitDepth | hel.dwDeviceRenderBitDepth) & bd)) return DDERR_INVALIDPIXELFORMAT;
    }
    if (z) {
        const DWORD zc = z->desc.ddsCaps.dwCaps;
        if (!hw && !(zc & DDSCAPS_SYSTEMMEMORY)) return D3DERR_ZBUFF_NEEDS_SYSTEMMEMORY;
        if (kind == kRamp && z->buf->fmt.a) return DDERR_INVALIDPARAMS;  // stencil
        if (hw && !(zc & DDSCAPS_VIDEOMEMORY)) return D3DERR_ZBUFF_NEEDS_VIDEOMEMORY;
    }
    Device* d = new (std::nothrow) Device(this, kind, guid, version, rt, rtVersion, z, rt->palette);
    if (!d) return E_OUTOFMEMORY;
    devices.push_back(d);
    // The x87 control word only with DDSCL_FPUSETUP (A step 8), on this thread.
    if (dd->CoopFlags() & DDSCL_FPUSETUP) _controlfp(_PC_24 | _RC_NEAR | _MCW_EM, _MCW_PC | _MCW_RC | _MCW_EM);
    DD2D9_LOG("IDirect3DDevice%s created (%s, %dx%d, %u bpp)", version == 1 ? "" : version == 2 ? "2" : "3",
              kind == kHAL ? "HAL" : kind == kRamp ? "Ramp" : "RGB", rt->W(), rt->H(), unsigned(f.bpp));
    *out = d;
    return D3D_OK;
}

HRESULT D3D::F2::CreateDevice(REFCLSID g, IDirectDrawSurface* s, IDirect3DDevice2** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (!s) return DDERR_INVALIDOBJECT;
    Guard l;
    *out = nullptr;
    Surface* rt = Surface::From(s);
    if (!rt) return DDERR_INVALIDOBJECT;
    Device* d;
    const HRESULT hr = o->CreateDeviceI(g, rt, VersionOfIface(rt, s), 2, &d);
    if (FAILED(hr)) return hr;
    *out = &d->f2;  // refcount 1
    return D3D_OK;
}

HRESULT D3D::F3::CreateDevice(REFCLSID g, IDirectDrawSurface4* s, IDirect3DDevice3** out, IUnknown* outerUnk) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (!s) return DDERR_INVALIDOBJECT;
    if (outerUnk) return CLASS_E_NOAGGREGATION;
    Guard l;
    *out = nullptr;
    Surface* rt = Surface::From(s);
    if (!rt) return DDERR_INVALIDOBJECT;
    Device* d;
    const HRESULT hr = o->CreateDeviceI(g, rt, 4, 3, &d);
    if (FAILED(hr)) return hr;
    *out = &d->f3;
    return D3D_OK;
}

HRESULT D3D::F3::CreateVertexBuffer(D3DVERTEXBUFFERDESC* desc, IDirect3DVertexBuffer** out, DWORD flags,
                                    IUnknown* outerUnk) {
    if (outerUnk) return CLASS_E_NOAGGREGATION;
    if (!desc || !out) return DDERR_INVALIDPARAMS;  // XP faults
    Guard l;
    *out = nullptr;
    if (desc->dwNumVertices >= 0x10000) return D3DERR_TOOMANYVERTICES;
    if (desc->dwCaps & D3DVBCAPS_OPTIMIZED) return DDERR_INVALIDPARAMS;
    if ((desc->dwFVF & 0xFFFFF000) || !FvfSize6(desc->dwFVF)) return D3DERR_INVALIDVERTEXFORMAT;
    VBuf* vb = new (std::nothrow) VBuf(o, *desc, flags);
    if (!vb) return E_OUTOFMEMORY;
    o->vbufs.push_back(vb);
    *out = vb;
    return D3D_OK;
}

HRESULT D3D::F3::EnumZBufferFormats(REFCLSID g, LPD3DENUMPIXELFORMATSCALLBACK cb, void* ctx) {
    if (!CodeOk(reinterpret_cast<void*>(cb))) return DDERR_INVALIDPARAMS;
    const DevKind k = KindOf(g);
    if (k == kBadGuid) return DDERR_INVALIDPARAMS;
    std::vector<DDPIXELFORMAT> list;
    auto add = [&](DWORD depth, DWORD zmask, DWORD stencilBits, DWORD smask) {
        DDPIXELFORMAT f = {};
        f.dwSize = sizeof(f);
        f.dwFlags = DDPF_ZBUFFER | (stencilBits ? DDPF_STENCILBUFFER : 0);
        f.dwZBufferBitDepth = depth;
        f.dwStencilBitDepth = stencilBits;
        f.dwZBitMask = zmask;
        f.dwStencilBitMask = smask;
        list.push_back(f);
    };
    if (k == kHAL) {
        const GpuCaps c = GpuGetCaps();
        if (c.depth16) add(16, 0xFFFF, 0, 0);
        if (c.depth24x8) add(32, 0xFFFFFF, 0, 0);
        if (c.depth32) add(32, 0xFFFFFFFF, 0, 0);
        if (c.depth24s8) add(32, 0xFFFFFF, 8, 0xFF000000);
        if (list.empty()) return DDERR_NOZBUFFERHW;
    } else if (k == kRamp) {
        add(16, 0xFFFF, 0, 0);
    } else if (k == kRef || k == kNull) {
        return D3D_OK;  // none without d3dref.dll
    } else {
        add(16, 0xFFFF, 0, 0);
        add(32, 0xFFFFFF00, 8, 0xFF);
    }
    for (DDPIXELFORMAT& f : list)
        if (cb(&f, ctx) != D3DENUMRET_OK) break;
    return D3D_OK;
}

// A §~DIRECT3DI: viewports, lights, devices and materials go. Objects the game still holds
// are cut loose instead of freed under it (XP frees them; using them after is undefined).
void D3D::Teardown() {
    for (Viewport* v : std::vector<Viewport*>(viewports)) v->Orphan();
    for (Light* l : lights) l->Orphan();
    for (Device* d : std::vector<Device*>(devices)) d->Orphan();
    for (Material* m : std::vector<Material*>(materials)) m->Orphan();
    for (VBuf* vb : vbufs) vb->d3d_ = nullptr;
    viewports.clear();
    lights.clear();
    g_lightGen++;
    devices.clear();
    materials.clear();
    vbufs.clear();
}

template <class T> static void Unlink(std::vector<T*>& v, T* x) {
    auto it = std::find(v.begin(), v.end(), x);
    if (it != v.end()) v.erase(it);
}

// ---------------------------------------------------------------- light (C §2)

HRESULT Light::QueryInterface(REFIID r, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirect3DLight)) {
        AddRef();
        *out = this;
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG Light::Release() {
    const LONG r = InterlockedDecrement(&ref_);
    if (r == 0) {
        Guard l;
        if (vp) {  // only after a teardown cut it loose
            Unlink(vp->lights, this);
            vp->lightsDirty = true, g_lightGen++;
        }
        if (d3d_) Unlink(d3d_->lights, this);
        delete this;
    }
    return ULONG(std::max<LONG>(r, 0));
}

HRESULT Light::SetLight(D3DLIGHT* in) {
    if (!in || (in->dwSize != sizeof(D3DLIGHT) && in->dwSize != sizeof(D3DLIGHT2))) return DDERR_INVALIDPARAMS;
    if (in->dwSize == sizeof(D3DLIGHT2)) {  // D3DLIGHT itself isn't checked at all
        const D3DLIGHT2* l = reinterpret_cast<const D3DLIGHT2*>(in);
        if (l->dltType < D3DLIGHT_POINT || l->dltType > D3DLIGHT_PARALLELPOINT) return DDERR_INVALIDPARAMS;
        if (!(l->dvRange >= 0.0f) || l->dvRange > sqrtf(FLT_MAX)) return DDERR_INVALIDPARAMS;
        if (l->dltType == D3DLIGHT_SPOT || l->dltType == D3DLIGHT_DIRECTIONAL) {
            const D3DVECTOR& d = l->dvDirection;
            if (d.x * d.x + d.y * d.y + d.z * d.z < 1e-5f) return DDERR_INVALIDPARAMS;
        }
        if (l->dltType == D3DLIGHT_SPOT &&
            !(l->dvPhi >= 0.0f && l->dvPhi <= 3.14159265f && l->dvTheta >= 0.0f && l->dvTheta <= l->dvPhi))
            return DDERR_INVALIDPARAMS;
    }
    Guard g;
    memcpy(&data, in, in->dwSize);
    if (vp) vp->lightsDirty = true, g_lightGen++;
    return D3D_OK;
}

// The stored structure is copied with its own size (C §2.1); not past the caller's here.
HRESULT Light::GetLight(D3DLIGHT* out) {
    if (!out || (out->dwSize != sizeof(D3DLIGHT) && out->dwSize != sizeof(D3DLIGHT2))) return DDERR_INVALIDPARAMS;
    Guard g;
    memcpy(out, &data, std::min<DWORD>(data.dwSize, out->dwSize));
    return D3D_OK;
}

// ---------------------------------------------------------------- material (C §3)

namespace {
std::unordered_map<DWORD, MatHandle*> g_matHandles;
DWORD g_nextMatHandle = 1;
}  // namespace

MatHandle* FindMatHandle(DWORD h) {
    auto it = g_matHandles.find(h);
    return it == g_matHandles.end() ? nullptr : it->second;
}

Material::Material(D3D* d3d) : d3d_(d3d) {
    f1.o = f2.o = f3.o = this;
    data.dwSize = sizeof(D3DMATERIAL);
}

Material::~Material() {
    for (MatHandle* h : std::vector<MatHandle*>(handles)) DestroyHandle(h);
    if (d3d_) Unlink(d3d_->materials, this);
}

void Material::DestroyHandle(MatHandle* h) {
    if (h->dev) {
        Unlink(h->dev->matHandles, h);
        if (h->dev->ls.material == h->value) h->dev->SetMaterialHandle(0);
    }
    Unlink(handles, h);
    g_matHandles.erase(h->value);
    delete h;
}

void Material::Orphan() {
    if (d3d_) Unlink(d3d_->materials, this);
    d3d_ = nullptr;
}

ULONG Material::Release() {
    const LONG r = InterlockedDecrement(&ref);
    if (r == 0) {
        Guard l;
        delete this;
    }
    return ULONG(std::max<LONG>(r, 0));
}

HRESULT Material::QI(REFIID r, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirect3DMaterial3)) *out = static_cast<IDirect3DMaterial3*>(&f3);
    else if (IsEqualGUID(r, IID_IDirect3DMaterial2)) *out = static_cast<IDirect3DMaterial2*>(&f2);
    else if (IsEqualGUID(r, IID_IDirect3DMaterial)) *out = static_cast<IDirect3DMaterial*>(&f1);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

HRESULT Material::Set(D3DMATERIAL* m) {
    if (!m || m->dwSize != sizeof(D3DMATERIAL)) return DDERR_INVALIDPARAMS;
    Guard l;
    if (!memcmp(m, &data, sizeof(data))) return D3D_OK;
    data = *m;
    for (MatHandle* h : handles) {
        h->data = data;
        if (h->dev && h->dev->ls.material == h->value) h->dev->curMat = data;  // re-applied at once
    }
    return D3D_OK;
}

HRESULT Material::Get(D3DMATERIAL* m) {
    if (!m || m->dwSize != sizeof(D3DMATERIAL)) return DDERR_INVALIDPARAMS;
    Guard l;
    *m = data;
    return D3D_OK;
}

HRESULT Material::GetHandle(Device* dev, D3DMATERIALHANDLE* out) {
    if (!dev) return DDERR_INVALIDOBJECT;
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    for (MatHandle* h : handles)
        if (h->dev == dev) {
            *out = h->value;
            return D3D_OK;
        }
    MatHandle* h = new (std::nothrow) MatHandle{g_nextMatHandle++, this, dev, data};
    if (!h) return D3DERR_MATERIAL_CREATE_FAILED;
    g_matHandles[h->value] = h;
    handles.push_back(h);
    dev->matHandles.push_back(h);
    *out = h->value;
    return D3D_OK;
}

HRESULT Material::F1::GetHandle(IDirect3DDevice* d, D3DMATERIALHANDLE* h) { return o->GetHandle(d ? DevFrom(d) : nullptr, h); }
HRESULT Material::F2::GetHandle(IDirect3DDevice2* d, D3DMATERIALHANDLE* h) { return o->GetHandle(d ? DevFrom(d) : nullptr, h); }
HRESULT Material::F3::GetHandle(IDirect3DDevice3* d, D3DMATERIALHANDLE* h) { return o->GetHandle(d ? DevFrom(d) : nullptr, h); }

// ---------------------------------------------------------------- texture handles

namespace {
std::unordered_map<DWORD, Surface*> g_texHandles;
DWORD g_nextTexHandle = 1;
}  // namespace

SurfaceInfo* Info(Surface* s) {
    if (!s->d3dLegacy) s->d3dLegacy = new SurfaceInfo;
    return static_cast<SurfaceInfo*>(s->d3dLegacy);
}

DWORD TexHandleFor(Surface* s) {
    SurfaceInfo* i = Info(s);
    if (!i->texHandle) {
        i->texHandle = g_nextTexHandle++;
        g_texHandles[i->texHandle] = s;
    }
    return i->texHandle;
}

Surface* SurfaceForHandle(DWORD h) {
    if (!h) return nullptr;
    auto it = g_texHandles.find(h);
    return it == g_texHandles.end() ? nullptr : it->second;
}

void SwapHandles(Surface* a, Surface* b) {
    SurfaceInfo* ia = Info(a);
    SurfaceInfo* ib = Info(b);
    std::swap(ia->texHandle, ib->texHandle);
    g_texHandles[ia->texHandle] = a;
    g_texHandles[ib->texHandle] = b;
}

// ---------------------------------------------------------------- texture (C §4)

Texture* TexFrom(IDirect3DTexture* p) { return p ? static_cast<Texture::F1*>(p)->o : nullptr; }
Texture* TexFrom(IDirect3DTexture2* p) { return p ? static_cast<Texture::F2*>(p)->o : nullptr; }

HRESULT Texture::InnerQuery(REFIID r, void** out) {
    if (IsEqualGUID(r, IID_IDirect3DTexture)) *out = static_cast<IDirect3DTexture*>(&f1);
    else if (IsEqualGUID(r, IID_IDirect3DTexture2)) *out = static_cast<IDirect3DTexture2*>(&f2);
    else return E_NOINTERFACE;
    s->AddRef(Surface::SlotFor(version));
    return S_OK;
}

void Texture::SurfaceGone() { delete this; }

// QueryInterface, AddRef and Release are the surface's (C §4).
HRESULT Texture::F1::QueryInterface(REFIID r, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    return o->s->QueryInterface(r, out, o->version);
}
ULONG Texture::F1::AddRef() { return o->s->AddRef(Surface::SlotFor(o->version)); }
ULONG Texture::F1::Release() { return o->s->Release(Surface::SlotFor(o->version)); }
HRESULT Texture::F2::QueryInterface(REFIID r, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    return o->s->QueryInterface(r, out, o->version);
}
ULONG Texture::F2::AddRef() { return o->s->AddRef(Surface::SlotFor(o->version)); }
ULONG Texture::F2::Release() { return o->s->Release(Surface::SlotFor(o->version)); }

HRESULT Texture::GetHandle(Device* dev, D3DTEXTUREHANDLE* h, bool v2) {
    if (!dev || !h) return DDERR_INVALIDOBJECT;
    Guard l;
    if (s->desc.ddsCaps.dwCaps2 & DDSCAPS2_TEXTUREMANAGE) return DDERR_INVALIDOBJECT;
    const DWORD value = TexHandleFor(s);
    const bool known = std::find(dev->texHandles.begin(), dev->texHandles.end(), value) != dev->texHandles.end();
    *h = 0;
    if (!known) {
        if (v2) {  // VerifyTextureCaps
            const DWORD tc = (dev->kind == kHAL ? dev->hal : dev->hel).dpcTriCaps.dwTextureCaps;
            const auto pow2 = [](int x) { return x > 0 && !(x & (x - 1)); };
            if ((tc & D3DPTEXTURECAPS_POW2) && !(tc & D3DPTEXTURECAPS_NONPOW2CONDITIONAL) && (!pow2(s->W()) || !pow2(s->H())))
                return D3DERR_TEXTURE_BADSIZE;
            if ((tc & D3DPTEXTURECAPS_SQUAREONLY) && s->W() != s->H()) return D3DERR_TEXTURE_BADSIZE;
        }
        dev->texHandles.push_back(value);
    }
    *h = value;
    return D3D_OK;
}

HRESULT Texture::F1::GetHandle(IDirect3DDevice* d, D3DTEXTUREHANDLE* h) {
    return o->GetHandle(d ? DevFrom(d) : nullptr, h, false);
}
HRESULT Texture::F2::GetHandle(IDirect3DDevice2* d, D3DTEXTUREHANDLE* h) {
    return o->GetHandle(d ? DevFrom(d) : nullptr, h, true);
}

// Load: d3dim700's CopySurface without rectangles (C §4.2): the whole source, level by
// level, with its palette and colour keys. Also D3DOP_TEXTURELOAD's.
HRESULT CopyTexture(Surface* dst, Surface* src) {
    if ((dst->desc.ddsCaps.dwCaps2 & 0xFC00) != (src->desc.ddsCaps.dwCaps2 & 0xFC00)) return DDERR_INVALIDPARAMS;
    if (src->palette && !dst->palette && dst->buf->fmt.pal8) return DDERR_NOPALETTEATTACHED;  // Wine's d3d TextureLoadTest
    if (dst->palette && src->palette) {
        if ((dst->palette->caps & 0x7) != (src->palette->caps & 0x7)) return DDERR_INVALIDPARAMS;
        memcpy(dst->palette->entries, src->palette->entries, sizeof(dst->palette->entries));
        dst->palette->version++;
    }
    for (int k : {kCkDestBlt, kCkSrcBlt}) {  // a key only the destination has stays (Wine's test_texture_load_ckey)
        if (!src->hasCk[k]) continue;
        dst->hasCk[k] = true;
        dst->ck[k] = src->ck[k];
    }
    Surface* sl = src;
    while (sl && sl->W() > dst->W()) sl = sl->NextMipLevel();
    if (!sl || sl->W() != dst->W() || sl->H() != dst->H()) return DDERR_INVALIDPARAMS;
    for (Surface* d = dst; d; d = d->NextMipLevel(), sl = sl ? sl->NextMipLevel() : nullptr) {
        if (!sl) return DDERR_INVALIDPARAMS;
        if (d->lockCount || sl->lockCount) return DDERR_SURFACEBUSY;
        RECT r = {0, 0, d->W(), d->H()};
        RECT sr = {0, 0, sl->W(), sl->H()};
        const HRESULT hr = d->BltLocked(&r, sl, &sr, 0, nullptr, false);
        if (FAILED(hr)) return hr;
        d->BumpUniqueness();
    }
    return D3D_OK;
}

HRESULT Texture::Load(Texture* srcTex) {
    if (!srcTex) return DDERR_INVALIDOBJECT;
    Guard l;
    return CopyTexture(s, srcTex->s);
}

HRESULT Texture::F1::Load(IDirect3DTexture* src) { return o->Load(TexFrom(src)); }
HRESULT Texture::F2::Load(IDirect3DTexture2* src) { return o->Load(TexFrom(src)); }

// ---------------------------------------------------------------- execute buffer (C §5)

ExecBuf::ExecBuf(Device* dev, DWORD size) : dev(dev), mem(size, 0) {}

ExecBuf::~ExecBuf() {
    if (dev) Unlink(dev->execBufs, this);
}

HRESULT ExecBuf::QueryInterface(REFIID r, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirect3DExecuteBuffer)) {
        AddRef();
        *out = this;
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG ExecBuf::Release() {
    const LONG r = InterlockedDecrement(&ref_);
    if (r == 0) {
        Guard l;
        delete this;
    }
    return ULONG(std::max<LONG>(r, 0));
}

HRESULT ExecBuf::Lock(D3DEXECUTEBUFFERDESC* d) {
    if (!d || d->dwSize != sizeof(D3DEXECUTEBUFFERDESC)) return DDERR_INVALIDPARAMS;
    Guard l;
    if (locked) return D3DERR_EXECUTE_LOCKED;
    locked = true;
    d->dwSize = sizeof(D3DEXECUTEBUFFERDESC);
    d->dwFlags = D3DDEB_LPDATA;
    d->dwCaps = 0;
    d->dwBufferSize = DWORD(mem.size());
    d->lpData = mem.data();
    return D3D_OK;
}

HRESULT ExecBuf::Unlock() {
    Guard l;
    if (!locked) return D3DERR_EXECUTE_NOT_LOCKED;
    locked = false;
    return D3D_OK;
}

HRESULT ExecBuf::SetExecuteData(D3DEXECUTEDATA* d) {
    if (!d || d->dwSize != sizeof(D3DEXECUTEDATA)) return DDERR_INVALIDPARAMS;
    Guard l;
    data = *d;
    return D3D_OK;
}

HRESULT ExecBuf::GetExecuteData(D3DEXECUTEDATA* d) {
    if (!d) return DDERR_INVALIDPARAMS;
    Guard l;
    *d = data;
    return D3D_OK;
}

// ---------------------------------------------------------------- vertex buffer (C §7)

UINT FvfSize6(DWORD fvf) {
    UINT n;
    switch (fvf & 0x0E) {
    case 0: n = 0; break;
    case 0x04: n = 16; break;  // XYZRHW
    case 0x06: n = 16; break;
    case 0x08: n = 20; break;
    case 0x0A: n = 24; break;
    case 0x0C: n = 28; break;
    case 0x0E: n = 32; break;
    default: n = 12; break;
    }
    if (fvf & D3DFVF_NORMAL) n += 12;
    if (fvf & D3DFVF_RESERVED1) n += 4;
    if (fvf & D3DFVF_DIFFUSE) n += 4;
    if (fvf & D3DFVF_SPECULAR) n += 4;
    return n + 8 * ((fvf >> 8) & 0xF);
}

VBuf::VBuf(D3D* d3d, const D3DVERTEXBUFFERDESC& d, DWORD flags)
    : d3d_(d3d), caps(d.dwCaps), fvf(d.dwFVF), count(d.dwNumVertices) {
    stride = FvfSize6(fvf);
    mem.assign(size_t(stride) * std::max<DWORD>(count, 1), 0);
    if ((fvf & 0x0E) == D3DFVF_XYZRHW && !(flags & D3DDP_DONOTCLIP)) clip.assign(std::max<DWORD>(count, 1), 0);
}

VBuf::~VBuf() {
    if (d3d_) Unlink(d3d_->vbufs, this);
}

HRESULT VBuf::QueryInterface(REFIID r, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirect3DVertexBuffer)) {
        AddRef();
        *out = this;
        return S_OK;
    }
    return E_NOINTERFACE;  // *out untouched (C §7.2)
}

ULONG VBuf::Release() {
    const LONG r = InterlockedDecrement(&ref_);
    if (r == 0) {
        Guard l;
        delete this;
    }
    return ULONG(std::max<LONG>(r, 0));
}

HRESULT VBuf::Lock(DWORD, void** data, DWORD* size) {
    Guard l;
    if (optimized) return D3DERR_VERTEXBUFFEROPTIMIZED;
    if (!data) return DDERR_INVALIDPARAMS;  // XP faults
    if (size) *size = stride * count;
    locks++;
    *data = mem.data();
    return D3D_OK;
}

HRESULT VBuf::Unlock() {
    Guard l;
    if (locks) locks--;
    return D3D_OK;
}

HRESULT VBuf::GetVertexBufferDesc(D3DVERTEXBUFFERDESC* d) {
    if (!d) return DDERR_INVALIDPARAMS;  // XP faults
    d->dwCaps = caps;
    d->dwFVF = fvf;
    d->dwNumVertices = count;
    return D3D_OK;
}

HRESULT VBuf::Optimize(IDirect3DDevice3* dev, DWORD flags) {
    if (!dev) return DDERR_INVALIDOBJECT;
    if (flags) return DDERR_INVALIDPARAMS;
    Guard l;
    if (optimized) return D3DERR_VERTEXBUFFEROPTIMIZED;
    if (locks) return D3DERR_VERTEXBUFFERLOCKED;
    optimized = true;
    caps |= D3DVBCAPS_OPTIMIZED;
    return D3D_OK;
}

}  // namespace d6

// ---------------------------------------------------------------- DirectDraw's side (A)

using namespace d6;

bool D3DLegacyEnabled() { return g_config.d3d != kD3DOff; }

static D3D* EnsureD3D(DDraw* dd, int outer) {
    auto* d3d = static_cast<D3D*>(dd->d3dLegacy);
    if (!d3d) {
        d3d = new (std::nothrow) D3D(dd, outer ? outer : 1);
        if (!d3d) return nullptr;
        dd->d3dLegacy = d3d;
        DD2D9_LOG("Direct3D 3-6 object created (dd2d9's own)");
    }
    return d3d;
}

HRESULT D3DLegacyQuery(DDraw* dd, REFIID riid, void** out, int ddVersion) {
    if (!IsEqualGUID(riid, IID_IDirect3D) && !IsEqualGUID(riid, IID_IDirect3D2) && !IsEqualGUID(riid, IID_IDirect3D3))
        return E_NOINTERFACE;
    if (dd->dd7 || !D3DLegacyEnabled()) return E_NOINTERFACE;
    Guard l;
    D3D* d3d = EnsureD3D(dd, ddVersion);
    if (!d3d) return E_OUTOFMEMORY;
    return d3d->InnerQuery(riid, out);
}

// A §Surface hand-off.
HRESULT D3DLegacyQuerySurface(Surface* s, REFIID riid, void** out, int version) {
    if (s->dd->dd7 || !D3DLegacyEnabled()) return E_NOINTERFACE;
    Guard l;
    SurfaceInfo* info = Info(s);
    for (const SurfaceInfo::Entry& e : info->cache)
        if (IsEqualGUID(e.iid, riid)) return SUCCEEDED(e.obj->InnerQuery(riid, out)) ? S_OK : E_NOINTERFACE;
    // A surface outliving its DirectDraw object: no new Direct3D object on a dead one
    // (nothing would tear it down, and its Gpu is gone).
    if (!s->dd->alive) return E_NOINTERFACE;
    D3D* d3d = EnsureD3D(s->dd, s->ddVersion);
    if (!d3d) return E_OUTOFMEMORY;
    Inner* obj = nullptr;
    if (IsEqualGUID(riid, IID_IDirect3DTexture) || IsEqualGUID(riid, IID_IDirect3DTexture2)) {
        obj = new (std::nothrow) Texture(s, version, IsEqualGUID(riid, IID_IDirect3DTexture2) ? 2 : 1);
        if (!obj) return E_OUTOFMEMORY;
    } else {
        if (KindOf(riid) == kBadGuid) return E_INVALIDARG;  // Direct3DCreateDevice's answer
        Device* d;
        const HRESULT hr = d3d->CreateDeviceI(riid, s, version, 1, &d);
        if (FAILED(hr)) return hr;
        d->outerSlot = Surface::SlotFor(version);
        obj = d;
    }
    info->cache.push_back({riid, obj});
    return SUCCEEDED(obj->InnerQuery(riid, out)) ? S_OK : E_NOINTERFACE;
}

void D3DLegacySurfaceGone(Surface* s) {
    auto* info = static_cast<SurfaceInfo*>(s->d3dLegacy);
    if (info) {
        for (const SurfaceInfo::Entry& e : info->cache) e.obj->SurfaceGone();
    }
    // Devices of this DirectDraw object let go of it (Z buffers, stage textures, handles),
    // and viewports of a background depth surface kept without a reference (XP dangles).
    if (auto* d3d = static_cast<D3D*>(s->dd->d3dLegacy)) {
        for (Device* d : d3d->devices) d->ForgetSurface(s);
        for (Viewport* v : d3d->viewports)
            if (v->bgDepth == s) {
                v->bgDepth = nullptr;
                v->bgDepthIface = nullptr;
                v->bgDepthSlot = -1;
            }
    }
    if (info) {
        if (info->texHandle) {
            const DWORD h = info->texHandle;
            // Materials whose hTexture was this handle (C §4.1).
            for (auto& m : g_matHandles)
                if (m.second->data.hTexture == h) m.second->data.hTexture = 0;
            if (auto* d3d = static_cast<D3D*>(s->dd->d3dLegacy))
                for (Material* mat : d3d->materials)
                    if (mat->data.hTexture == h) mat->data.hTexture = 0;
            g_texHandles.erase(h);
        }
        delete info;
        s->d3dLegacy = nullptr;
    }
}

void D3DLegacyDestroy(DDraw* dd) {
    auto* d3d = static_cast<D3D*>(dd->d3dLegacy);
    if (!d3d) return;
    d3d->Teardown();
    delete d3d;
    dd->d3dLegacy = nullptr;
}

}  // namespace dd2d9
