// dd2d9's Direct3D 7: the Direct3D object (devices and Z buffer formats enumerated, devices
// and vertex buffers made) and DirectDraw's side of it (d3d7int.h).
#include "d3d7int.h"

#include <float.h>
#include <new>

namespace dd2d9 {
namespace d7 {

namespace {

// Not in mingw's dxguid.
const GUID kIID_NullDevice = {0x8767df22, 0xbacc, 0x11d1, {0x89, 0x69, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};

}  // namespace

// ---------------------------------------------------------------- caps (D: EnumDevices)

D3DDEVICEDESC7 RgbDesc() {
    D3DDEVICEDESC7 d = {};
    d.dwDevCaps = 0x551;
    D3DPRIMCAPS p = {};
    p.dwSize = sizeof(D3DPRIMCAPS);
    p.dwMiscCaps = 0x70;
    p.dwRasterCaps = 0x2121F7;
    p.dwZCmpCaps = 0xFF;
    p.dwSrcBlendCaps = 0x1FFF;
    p.dwDestBlendCaps = 0x7FF;
    p.dwAlphaCmpCaps = 0xFF;
    p.dwShadeCaps = 0xC528A;
    p.dwTextureCaps = 0xDF;
    p.dwTextureFilterCaps = 0x0303033F;
    p.dwTextureBlendCaps = 0xCF;
    p.dwTextureAddressCaps = 0x1F;
    p.dwStippleWidth = p.dwStippleHeight = 4;
    d.dpcLineCaps = d.dpcTriCaps = p;
    d.dwDeviceRenderBitDepth = 0xF00;
    d.dwDeviceZBufferBitDepth = 0x400;
    d.dwMinTextureWidth = d.dwMinTextureHeight = 1;
    d.dwMaxTextureWidth = d.dwMaxTextureHeight = 1024;
    d.dwMaxTextureRepeat = 256;
    d.dwMaxAnisotropy = 1;
    d.dvGuardBandLeft = d.dvGuardBandTop = -4096.0f;
    d.dvGuardBandRight = d.dvGuardBandBottom = 4095.0f;
    d.dwStencilCaps = 0xFF;
    d.dwFVFCaps = 8;
    d.dwTextureOpCaps = 0x78FF;
    d.wMaxTextureBlendStages = d.wMaxSimultaneousTextures = 8;
    d.dwMaxActiveLights = 0xFFFFFFFF;
    d.dvMaxVertexW = 10000.0f;
    d.deviceGUID = IID_IDirect3DRGBDevice;
    d.wMaxUserClipPlanes = 6;
    d.wMaxVertexBlendMatrices = 4;
    d.dwVertexProcessingCaps = 0x3F;
    return d;
}


// The device table, as XP's static one: the strings handed to the callback stay valid
// after the call and are writable, and what a game writes there stays (Wine's test).
static char g_devName[3][32] = {"RGB Emulation", "Direct3D HAL", "Direct3D T&L HAL"};
static char g_devDesc[3][96] = {"Microsoft Direct3D RGB Software Emulation",
                                "Microsoft Direct3D Hardware acceleration through Direct3D HAL",
                                "Microsoft Direct3D Hardware Transform and Lighting acceleration capable device"};

HRESULT Direct3D7::EnumDevices(LPD3DENUMDEVICESCALLBACK7 cb, void* ctx) {
    if (!cb || IsBadCodePtr(reinterpret_cast<FARPROC>(reinterpret_cast<void*>(cb)))) return DDERR_INVALIDPARAMS;
    const GpuCaps c = GpuGetCaps();
    for (int i = 0; i < 3; i++) {
        if (i > 0 && !c.valid) break;
        if (i == 2 && !c.hwTnL) break;
        D3DDEVICEDESC7 caps = i == 0 ? RgbDesc() : HalDesc(i == 2);
        if (cb(g_devDesc[i], g_devName[i], &caps, ctx) != D3DENUMRET_OK) break;
    }
    return D3D_OK;
}

HRESULT Direct3D7::EnumZBufferFormats(REFCLSID iid, LPD3DENUMPIXELFORMATSCALLBACK cb, void* ctx) {
    if (!cb || IsBadCodePtr(reinterpret_cast<FARPROC>(reinterpret_cast<void*>(cb)))) return DDERR_INVALIDPARAMS;
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
    if (IsEqualGUID(iid, IID_IDirect3DRGBDevice) || IsEqualGUID(iid, IID_IDirect3DMMXDevice)) {
        add(16, 0xFFFF, 0, 0);
        add(32, 0xFFFFFF00, 8, 0xFF);
    } else if (IsEqualGUID(iid, IID_IDirect3DHALDevice) || IsEqualGUID(iid, IID_IDirect3DTnLHalDevice)) {
        const GpuCaps c = GpuGetCaps();
        if (c.depth16) add(16, 0xFFFF, 0, 0);
        if (c.depth24x8) add(32, 0xFFFFFF, 0, 0);
        if (c.depth32) add(32, 0xFFFFFFFF, 0, 0);
        if (c.depth24s8) add(32, 0xFFFFFF, 8, 0xFF000000);
        if (list.empty()) return DDERR_NOZBUFFERHW;
    } else if (IsEqualGUID(iid, IID_IDirect3DRampDevice) || IsEqualGUID(iid, IID_IDirect3DRefDevice) ||
               IsEqualGUID(iid, kIID_NullDevice)) {
        return D3D_OK;  // none (Ramp; Ref and Null without d3dref.dll)
    } else {
        return DDERR_INVALIDPARAMS;
    }
    for (DDPIXELFORMAT& f : list)
        if (cb(&f, ctx) != D3DENUMRET_OK) break;
    return D3D_OK;
}

HRESULT Direct3D7::CreateVertexBuffer(D3DVERTEXBUFFERDESC* desc, IDirect3DVertexBuffer7** out, DWORD) {
    if (!desc || !out) return DDERR_INVALIDPARAMS;  // XP faults
    Guard l;
    *out = nullptr;
    if (desc->dwNumVertices > 0xFFFF) return D3DERR_TOOMANYVERTICES;
    if (desc->dwCaps & D3DVBCAPS_OPTIMIZED) return DDERR_INVALIDPARAMS;
    auto* vb = new (std::nothrow) VertexBuffer7(this, *desc);
    if (!vb) return E_OUTOFMEMORY;
    *out = vb;
    return D3D_OK;
}

// ---------------------------------------------------------------- CreateDevice (D)

HRESULT Direct3D7::CreateDevice(REFCLSID rclsid, IDirectDrawSurface7* surface, IDirect3DDevice7** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (!surface) return DDERR_INVALIDOBJECT;
    *out = nullptr;
    Guard l;
    Surface* rt = Surf(surface);
    if (!rt) return DDERR_INVALIDPARAMS;
    DevKind kind;
    const GpuCaps c = GpuGetCaps();
    if (IsEqualGUID(rclsid, IID_IDirect3DRGBDevice) || IsEqualGUID(rclsid, IID_IDirect3DMMXDevice)) {
        kind = kRGB;
    } else if (IsEqualGUID(rclsid, IID_IDirect3DHALDevice)) {
        if (!c.valid) return E_NOINTERFACE;
        kind = kHAL;
    } else if (IsEqualGUID(rclsid, IID_IDirect3DTnLHalDevice)) {
        if (!c.valid || !c.hwTnL) return E_OUTOFMEMORY;  // no device class for it (D, CreateDevice 3.4)
        kind = kTnLHAL;
    } else if (IsEqualGUID(rclsid, IID_IDirect3DRampDevice) || IsEqualGUID(rclsid, IID_IDirect3DRefDevice) ||
               IsEqualGUID(rclsid, kIID_NullDevice)) {
        return E_NOINTERFACE;
    } else {
        return DDERR_INVALIDPARAMS;
    }
    // The render target (Init 3.3-3.7, checkDeviceSurface).
    const DWORD caps = rt->desc.ddsCaps.dwCaps;
    if (!(caps & DDSCAPS_3DDEVICE) || (caps & DDSCAPS_ZBUFFER)) return DDERR_INVALIDCAPS;
    if (rt->W() > 2048 || rt->H() > 2048) return DDERR_INVALIDOBJECT;
    if (!rt->palette && rt->buf->fmt.bpp < 16) return DDERR_NOPALETTEATTACHED;
    Surface* z = AttachedZ(rt);
    const bool hw = kind != kRGB;
    if (hw && !(caps & DDSCAPS_VIDEOMEMORY)) return D3DERR_SURFACENOTINVIDMEM;
    const Format& f = rt->buf->fmt;
    if (f.fourcc || f.zbuf) return DDERR_INVALIDPIXELFORMAT;
    // d3dim700's RGB device doesn't render to palettized surfaces (Wine's test_rt_caps):
    // refused by the format check in video memory, failing later in system memory.
    if (f.pal8) return (caps & DDSCAPS_VIDEOMEMORY) ? DDERR_INVALIDPIXELFORMAT : E_FAIL;
    const D3DDEVICEDESC7 desc = kind == kRGB ? RgbDesc() : HalDesc(kind == kTnLHAL);
    if (!(BitsToDDBD(f.bpp) & desc.dwDeviceRenderBitDepth)) return DDERR_INVALIDPIXELFORMAT;
    if (z) {
        const DWORD zc = z->desc.ddsCaps.dwCaps;
        if (hw && !(zc & DDSCAPS_VIDEOMEMORY)) return D3DERR_ZBUFF_NEEDS_VIDEOMEMORY;
        if (!hw && !(zc & DDSCAPS_SYSTEMMEMORY)) return D3DERR_ZBUFF_NEEDS_SYSTEMMEMORY;
    }
    Device7* dev = new (std::nothrow) Device7(this, kind, rt, z, rt->palette);
    if (!dev) return E_OUTOFMEMORY;
    // The x87 control word: single precision, round to nearest, exceptions masked, unless
    // the game asked to keep its own (D, CreateDevice 3.7). On this thread only.
    if (!(dd_->CoopFlags() & DDSCL_FPUPRESERVE)) _controlfp(_PC_24 | _RC_NEAR | _MCW_EM, _MCW_PC | _MCW_RC | _MCW_EM);
    *out = dev;
    return D3D_OK;
}

}  // namespace d7

// ---------------------------------------------------------------- DirectDraw's side

HRESULT D3D7Query(DDraw* dd, REFIID riid, void** out, int ddVersion) {
    if (!IsEqualGUID(riid, IID_IDirect3D7)) {
        // Direct3D 1-3 are d3dim.dll's (not yet implemented); a DirectDraw 7 object has none.
        return E_NOINTERFACE;
    }
    if (!dd->dd7) return E_NOINTERFACE;  // d3dim.dll's object: IDirect3D7 isn't among its interfaces
    if (g_config.d3d == kD3DOff) return E_NOINTERFACE;
    Guard l;
    auto* d3d = static_cast<d7::Direct3D7*>(dd->d3d7);
    if (!d3d) {
        d3d = new (std::nothrow) d7::Direct3D7(dd, ddVersion ? ddVersion : 7);
        if (!d3d) return E_OUTOFMEMORY;
        dd->d3d7 = d3d;
        DD2D9_LOG("Direct3D 7 object created (dd2d9's own)");
    }
    d3d->AddRef();
    *out = static_cast<IDirect3D7*>(d3d);
    return S_OK;
}

void D3D7Destroy(DDraw* dd) {
    delete static_cast<d7::Direct3D7*>(dd->d3d7);
    dd->d3d7 = nullptr;
}

}  // namespace dd2d9
