// dd2d9's own Direct3D 7 (see d3d7.h). The Direct3D 7 state lives here, in Direct3D 7's own
// terms, and answers every Get; the Direct3D 9 device (gpures.h) gets it translated. The
// section numbers cited are those of the specs in re/spec/: D = d3d7-direct3d.md,
// S = d3d7-device-state.md, V = d3d7-draw-vb.md.
#include "d3d7.h"

#include "d3dcommon.h"
#include "gpu.h"
#include "gpures.h"
#include "objects.h"

#include <d3d.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <float.h>
#include <new>
#include <vector>

namespace dd2d9 {

namespace {

// Not in mingw's dxguid.
const GUID kIID_NullDevice = {0x8767df22, 0xbacc, 0x11d1, {0x89, 0x69, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};

enum DevKind { kRGB, kHAL, kTnLHAL };

class Direct3D7;
class Device7;
class VertexBuffer7;

// ---------------------------------------------------------------- small helpers

Surface* Surf(IDirectDrawSurface7* p) { return p ? Surface::From(p) : nullptr; }

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


// ---------------------------------------------------------------- Direct3D object

class Direct3D7 final : public IDirect3D7 {
public:
    Direct3D7(DDraw* dd, int outer) : dd_(dd), outer_(outer) {}
    DDraw* dd_;
    int outer_;  // the DirectDraw interface version AddRef/Release go to (D, aggregation)

    STDMETHOD(QueryInterface)(REFIID r, void** out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = nullptr;
        return dd_->QueryInterface(r, out, outer_);
    }
    STDMETHOD_(ULONG, AddRef)() override { return dd_->AddRef(outer_); }
    STDMETHOD_(ULONG, Release)() override { return dd_->Release(outer_); }
    STDMETHOD(EnumDevices)(LPD3DENUMDEVICESCALLBACK7 cb, void* ctx) override;
    STDMETHOD(CreateDevice)(REFCLSID rclsid, IDirectDrawSurface7* surface, IDirect3DDevice7** device) override;
    STDMETHOD(CreateVertexBuffer)(D3DVERTEXBUFFERDESC* desc, IDirect3DVertexBuffer7** vb, DWORD flags) override;
    STDMETHOD(EnumZBufferFormats)(REFCLSID iid, LPD3DENUMPIXELFORMATSCALLBACK cb, void* ctx) override;
    STDMETHOD(EvictManagedTextures)() override { return D3D_OK; }
};

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

// ---------------------------------------------------------------- vertex buffer

class VertexBuffer7 final : public IDirect3DVertexBuffer7 {
public:
    VertexBuffer7(Direct3D7* d3d, const D3DVERTEXBUFFERDESC& d) : d3d_(d3d), caps_(d.dwCaps), fvf_(d.dwFVF), count_(d.dwNumVertices) {
        stride_ = FvfSize(fvf_);
        // 16 bytes more: vertex blending reads weights past the FVF, as XP (V §9).
        mem_.assign(size_t(stride_) * std::max<DWORD>(count_, 1) + 16, 0);
        if ((fvf_ & 0x0E) == D3DFVF_XYZRHW && !(caps_ & D3DVBCAPS_DONOTCLIP)) clip_.assign(count_, 0);
        d3d_->AddRef();  // its buffer surface holds the DirectDraw interface (D §CreateVertexBuffer)
    }
    ~VertexBuffer7() { d3d_->Release(); }

    STDMETHOD(QueryInterface)(REFIID r, void** out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = nullptr;
        if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirect3DVertexBuffer7)) {
            AddRef();
            *out = this;
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ULONG(InterlockedIncrement(&ref_)); }
    STDMETHOD_(ULONG, Release)() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) {
            Guard l;
            delete this;
        }
        return ULONG(std::max<LONG>(r, 0));
    }
    // V §8.2: the same pointer for the buffer's life, valid after Unlock too.
    STDMETHOD(Lock)(DWORD, void** data, DWORD* size) override {
        Guard l;
        if (optimized_) return D3DERR_VERTEXBUFFEROPTIMIZED;
        if (!data) return DDERR_INVALIDPARAMS;
        locks_++;
        *data = mem_.data();
        if (size) *size = stride_ * count_;
        return D3D_OK;
    }
    STDMETHOD(Unlock)() override {
        Guard l;
        if (locks_) locks_--;
        return D3D_OK;
    }
    STDMETHOD(ProcessVertices)(DWORD op, DWORD dst, DWORD count, IDirect3DVertexBuffer7* src, DWORD srcIndex,
                               IDirect3DDevice7* device, DWORD flags) override;
    STDMETHOD(GetVertexBufferDesc)(D3DVERTEXBUFFERDESC* desc) override {
        if (!desc) return DDERR_INVALIDPARAMS;
        desc->dwCaps = caps_ | (optimized_ ? D3DVBCAPS_OPTIMIZED : 0);
        desc->dwFVF = fvf_;
        desc->dwNumVertices = count_;
        return D3D_OK;
    }
    STDMETHOD(Optimize)(IDirect3DDevice7* device, DWORD flags) override;
    STDMETHOD(ProcessVerticesStrided)(DWORD op, DWORD dst, DWORD count, D3DDRAWPRIMITIVESTRIDEDDATA* data, DWORD fvf,
                                      IDirect3DDevice7* device, DWORD flags) override;

    Direct3D7* d3d_;
    DWORD caps_, fvf_, count_;
    UINT stride_ = 0;
    std::vector<uint8_t> mem_;
    std::vector<WORD> clip_;   // clip codes from ProcessVertices(D3DVOP_CLIP)
    bool hasClipCodes_ = false;
    bool optimized_ = false;
    DWORD locks_ = 0;

private:
    LONG ref_ = 1;
};

VertexBuffer7* VB(IDirect3DVertexBuffer7* p) { return static_cast<VertexBuffer7*>(p); }

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

HRESULT VertexBuffer7::Optimize(IDirect3DDevice7* device, DWORD flags) {
    if (!device) return DDERR_INVALIDOBJECT;
    if (flags) return DDERR_INVALIDPARAMS;
    Guard l;
    // ValidateFVF (V §8.8).
    const DWORD tex = (fvf_ >> 8) & 0xF;
    const DWORD pos = fvf_ & 0x0E;
    const DWORD sizeBits = fvf_ >> 16;
    const bool sizeOk = tex >= 8 || !(sizeBits >> (2 * tex));
    if (!sizeOk || (fvf_ & 0xF001) || pos == 0 || (pos == D3DFVF_XYZRHW && (fvf_ & D3DFVF_NORMAL)))
        return DDERR_INVALIDPARAMS;
    if (optimized_) return D3DERR_VERTEXBUFFEROPTIMIZED;
    if (locks_) return D3DERR_VERTEXBUFFERLOCKED;
    optimized_ = true;
    return D3D_OK;
}

// ---------------------------------------------------------------- state blocks (S)

struct Cmd {
    enum Kind : BYTE { kRS, kTSS, kTex, kXform, kViewport, kMaterial, kLight, kLightEnable, kClipPlane } kind;
    DWORD a = 0, b = 0, c = 0;
    Surface* tex = nullptr;  // recorded without a reference, as XP (checked when applied)
    union {
        D3DMATRIX m;
        D3DVIEWPORT7 vp;
        D3DMATERIAL7 mat;
        D3DLIGHT7 light;
        float plane[4];
    };
    Cmd() { memset(&m, 0, std::max({sizeof(D3DMATRIX), sizeof(D3DLIGHT7), sizeof(D3DMATERIAL7)})); }
};

struct StateBlock {
    std::vector<Cmd> cmds;
    bool done = false;  // finished (EndStateBlock/CreateStateBlock)
};

// ---------------------------------------------------------------- device

class Device7 final : public IDirect3DDevice7 {
public:
    Device7(Direct3D7* d3d, DevKind kind, Surface* rt, Surface* z, Palette* pal);
    ~Device7();

    STDMETHOD(QueryInterface)(REFIID r, void** out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        *out = nullptr;
        if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirect3DDevice7)) {
            AddRef();
            *out = this;
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ULONG(InterlockedIncrement(&ref_)); }
    STDMETHOD_(ULONG, Release)() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) {
            Guard l;
            delete this;
        }
        return ULONG(std::max<LONG>(r, 0));
    }
    STDMETHOD(GetCaps)(D3DDEVICEDESC7* desc) override {
        if (!desc) return DDERR_INVALIDPARAMS;
        *desc = caps_;
        return D3D_OK;
    }
    STDMETHOD(EnumTextureFormats)(LPD3DENUMPIXELFORMATSCALLBACK cb, void* ctx) override;
    STDMETHOD(BeginScene)() override;
    STDMETHOD(EndScene)() override;
    STDMETHOD(GetDirect3D)(IDirect3D7** out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        d3d_->AddRef();
        *out = d3d_;
        return D3D_OK;
    }
    STDMETHOD(SetRenderTarget)(IDirectDrawSurface7* surface, DWORD flags) override;
    STDMETHOD(GetRenderTarget)(IDirectDrawSurface7** out) override {
        if (!out) return DDERR_INVALIDPARAMS;
        Guard l;
        rt_->AddRef(Surface::kRefV7);
        *out = static_cast<IDirectDrawSurface7*>(rt_->Iface(7));
        return D3D_OK;
    }
    STDMETHOD(Clear)(DWORD count, D3DRECT* rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil) override;
    STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) override;
    STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) override;
    STDMETHOD(SetViewport)(D3DVIEWPORT7* vp) override;
    STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) override;
    STDMETHOD(GetViewport)(D3DVIEWPORT7* vp) override {
        if (!vp) return DDERR_INVALIDPARAMS;
        *vp = vp_;
        return D3D_OK;
    }
    STDMETHOD(SetMaterial)(D3DMATERIAL7* m) override;
    STDMETHOD(GetMaterial)(D3DMATERIAL7* m) override {
        if (!m) return DDERR_INVALIDPARAMS;
        *m = mat_;
        return D3D_OK;
    }
    STDMETHOD(SetLight)(DWORD i, D3DLIGHT7* l) override;
    STDMETHOD(GetLight)(DWORD i, D3DLIGHT7* l) override {
        if (!l) return DDERR_INVALIDPARAMS;
        Guard g;
        if (i >= lights_.size() || !lights_[i].set) return DDERR_INVALIDPARAMS;
        *l = lights_[i].l;
        return D3D_OK;
    }
    STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE s, DWORD v) override;
    STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE s, DWORD* v) override;
    STDMETHOD(BeginStateBlock)() override;
    STDMETHOD(EndStateBlock)(DWORD* handle) override;
    STDMETHOD(PreLoad)(IDirectDrawSurface7*) override { return D3D_OK; }
    STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE t, DWORD fvf, void* v, DWORD n, DWORD flags) override;
    STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE t, DWORD fvf, void* v, DWORD n, WORD* idx, DWORD ni,
                                    DWORD flags) override;
    STDMETHOD(SetClipStatus)(D3DCLIPSTATUS* cs) override;
    STDMETHOD(GetClipStatus)(D3DCLIPSTATUS* cs) override;
    STDMETHOD(DrawPrimitiveStrided)(D3DPRIMITIVETYPE t, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA* d, DWORD n,
                                    DWORD flags) override;
    STDMETHOD(DrawIndexedPrimitiveStrided)(D3DPRIMITIVETYPE t, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA* d, DWORD n,
                                           WORD* idx, DWORD ni, DWORD flags) override;
    STDMETHOD(DrawPrimitiveVB)(D3DPRIMITIVETYPE t, IDirect3DVertexBuffer7* vb, DWORD start, DWORD n, DWORD flags) override;
    STDMETHOD(DrawIndexedPrimitiveVB)(D3DPRIMITIVETYPE t, IDirect3DVertexBuffer7* vb, DWORD start, DWORD n, WORD* idx,
                                      DWORD ni, DWORD flags) override;
    STDMETHOD(ComputeSphereVisibility)(D3DVECTOR* centers, D3DVALUE* radii, DWORD n, DWORD flags, DWORD* ret) override;
    STDMETHOD(GetTexture)(DWORD stage, IDirectDrawSurface7** out) override;
    STDMETHOD(SetTexture)(DWORD stage, IDirectDrawSurface7* surface) override;
    STDMETHOD(GetTextureStageState)(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD* v) override;
    STDMETHOD(SetTextureStageState)(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) override;
    STDMETHOD(ValidateDevice)(DWORD* passes) override;
    STDMETHOD(ApplyStateBlock)(DWORD h) override;
    STDMETHOD(CaptureStateBlock)(DWORD h) override;
    STDMETHOD(DeleteStateBlock)(DWORD h) override;
    STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE type, DWORD* handle) override;
    STDMETHOD(Load)(IDirectDrawSurface7* dst, POINT* pt, IDirectDrawSurface7* src, RECT* rect, DWORD flags) override;
    STDMETHOD(LightEnable)(DWORD i, WINBOOL on) override;
    STDMETHOD(GetLightEnable)(DWORD i, WINBOOL* on) override {
        if (!on) return DDERR_INVALIDPARAMS;
        Guard g;
        if (i >= lights_.size() || !lights_[i].set) return DDERR_INVALIDPARAMS;
        *on = lights_[i].enabled ? 0x80 : 0;  // XP's TRUE (S §GetLightEnable)
        return D3D_OK;
    }
    STDMETHOD(SetClipPlane)(DWORD i, D3DVALUE* plane) override;
    STDMETHOD(GetClipPlane)(DWORD i, D3DVALUE* plane) override {
        if (!plane) return DDERR_INVALIDPARAMS;
        if (i < kMaxPlanes) memcpy(plane, planes_[i], sizeof(planes_[i]));
        return D3D_OK;
    }
    STDMETHOD(GetInfo)(DWORD id, void* info, DWORD size) override {
        if (!info || !size) return DDERR_INVALIDOBJECT;
        memset(info, 0, size);
        (void)id;
        return S_FALSE;  // no driver to ask (S §GetInfo)
    }

    // For VertexBuffer7::ProcessVertices.
    D3DMATRIX world_, view_, proj_, world1_, world2_, world3_;
    DWORD blendSent_ = 0;  // VERTEXBLEND as last sent to D3D9 (see DrawCommon)
    // The fog states as last sent to D3D9 (SendFog): FOGVERTEXMODE, FOGSTART, FOGEND.
    bool fogSent_ = false;
    DWORD fogSentV_[3] = {};
    void SendFog(DWORD fvf);
    int keySent_ = -1;     // whether PushAlphaTest last sent the colour key's alpha test
    bool alphaSent_ = false;  // ALPHATESTENABLE, ALPHAREF, ALPHAFUNC as PushAlphaTest last sent them
    DWORD alphaSentV_[3] = {};
    // Reused from draw to draw (g_lock): strided vertices gathered, vertices given a normal,
    // indices made safe.
    std::vector<uint8_t> gathered_, withNormal_;
    std::vector<WORD> safeIdx_;
    D3DVIEWPORT7 vp_;
    DWORD rs_[256];
    D3DMATERIAL7 mat_ = {};
    float pvPower_ = 0.0f;  // the last material's specular power above 0 (ProcessVertices with power 0 keeps it)
    struct Light {
        D3DLIGHT7 l = {};
        bool set = false, enabled = false;
    };
    std::vector<Light> lights_;
    D3DCLIPSTATUS cs_ = {};
    // The clip status drawing updates (as XP's front end): kept only once the game has asked
    // for it (GetClipStatus), as it costs a transform of every untransformed vertex drawn.
    bool csTracking_ = false;
    void TrackClipStatus(DWORD fvf, const void* v, UINT stride, DWORD n);

private:
    static constexpr DWORD kMaxPlanes = 32;
    // The Direct3D 9 device, ready for this device's calls (all state sent if it is new).
    struct Use {
        GpuGuard g;
        bool ok;
        explicit Use(Device7* d) : g(d->dd_->GetGpu()) {
            bool fresh = false;
            ok = d->ctx_.Begin(&fresh);
            if (ok && fresh) d->PushAll();
        }
    };
    void PushAll();
    void PushRS(DWORD s);
    void PushTSS(DWORD stage, DWORD t);
    void PushAlphaTest();  // ALPHATEST* combined with COLORKEYENABLE's emulation
    void PushTransform(DWORD t);
    void PushLight(DWORD i);
    D3DMATRIX* Matrix(DWORD t);
    HRESULT SetRS(DWORD s, DWORD v);                     // not recorded
    HRESULT SetTSS(DWORD stage, DWORD t, DWORD v);
    HRESULT SetTex(DWORD stage, Surface* s);
    HRESULT SetVP(const D3DVIEWPORT7& vp);
    void SetXform(DWORD t, const D3DMATRIX& m);
    HRESULT SetLightI(DWORD i, const D3DLIGHT7& l);
    HRESULT LightEnableI(DWORD i, bool on);
    void SetPlane(DWORD i, const float* p);
    void SetMat(const D3DMATERIAL7& m);
    bool Recording() const { return recording_ != 0; }
    StateBlock* Rec() { return blocks_[recording_]; }
    void Capture(StateBlock* b);
    HRESULT Replay(const StateBlock* b);
    DWORD NewHandle(StateBlock* b);
    HRESULT DrawCommon(D3DPRIMITIVETYPE t, DWORD fvf, const void* v, UINT stride, DWORD n, const WORD* idx, DWORD ni);
    bool TargetsBusy() const;
    void HoldVB(VertexBuffer7* vb);

    LONG ref_ = 1;
    Direct3D7* d3d_;
    DDraw* dd_;
    DevKind kind_;
    D3DDEVICEDESC7 caps_;
    Surface* rt_;
    Surface* z_;
    Palette* pal_;
    GpuContext ctx_;
    DWORD tss_[8][64];
    DWORD addrU_[8] = {1, 1, 1, 1, 1, 1, 1, 1}, addrV_[8] = {1, 1, 1, 1, 1, 1, 1, 1};  // effective (WRAP)
    Surface* tex_[8] = {};
    D3DMATRIX texm_[8];
    float planes_[kMaxPlanes][4] = {};
    bool inScene_ = false;
    bool lostInScene_ = false;
    DWORD recording_ = 0;  // the state block being recorded, 0 for none
    std::vector<StateBlock*> blocks_;  // by handle; [0] reserved, freed ones null
    std::vector<DWORD> freeHandles_;
    VertexBuffer7* heldVB_ = nullptr;  // the last VB drawn from directly (V §8.7)
};

// The retired render states (S §Retired states).
bool Retired(DWORD s) {
    switch (s) {
    case 1: case 3: case 5: case 6: case 11: case 12: case 13: case 17: case 18: case 21: case 31: case 32:
    case 39: case 43: case 44: case 45: case 46: case 49: case 51:
        return true;
    default:
        return s >= 64 && s <= 95;
    }
}

Device7::Device7(Direct3D7* d3d, DevKind kind, Surface* rt, Surface* z, Palette* pal)
    : d3d_(d3d), dd_(d3d->dd_), kind_(kind), rt_(rt), z_(z), pal_(pal), ctx_(d3d->dd_->GetGpu()) {
    caps_ = kind == kRGB ? RgbDesc() : HalDesc(kind == kTnLHAL);
    // References (D §Reference counting): the render target's v7 interface, the Z buffer's v1,
    // the palette, the DirectDraw interface.
    rt_->AddRef(Surface::kRefV7);
    if (z_) z_->AddRef(Surface::kRefV1);
    if (pal_) pal_->AddRef();
    d3d_->AddRef();
    world_ = view_ = proj_ = world1_ = world2_ = world3_ = kIdentity;
    for (D3DMATRIX& m : texm_) m = kIdentity;
    // S §Render-state defaults (a DDI 4+ device: unset entries read 0xFFFFFFFF).
    std::fill(std::begin(rs_), std::end(rs_), 0xFFFFFFFFu);
    struct Def {
        DWORD s, v;
    };
    const float one = 1.0f, zero = 0.0f;
    DWORD f1, f0;
    memcpy(&f1, &one, 4);
    memcpy(&f0, &zero, 4);
    const Def defs[] = {{4, TRUE}, {29, FALSE}, {7, z_ ? DWORD(D3DZB_TRUE) : DWORD(D3DZB_FALSE)}, {8, D3DFILL_SOLID},
                        {9, D3DSHADE_GOURAUD}, {10, 0}, {14, TRUE}, {15, FALSE}, {16, TRUE}, {19, D3DBLEND_ONE},
                        {20, D3DBLEND_ZERO}, {22, D3DCULL_CCW}, {23, D3DCMP_LESSEQUAL}, {24, 0}, {25, D3DCMP_ALWAYS},
                        {26, FALSE}, {28, FALSE}, {30, FALSE}, {33, FALSE}, {34, 0}, {35, D3DFOG_NONE}, {36, f0},
                        {37, f1}, {38, f1}, {41, FALSE}, {27, FALSE}, {47, 0}, {48, FALSE}, {52, FALSE},
                        {53, D3DSTENCILOP_KEEP}, {54, D3DSTENCILOP_KEEP}, {55, D3DSTENCILOP_KEEP}, {56, D3DCMP_ALWAYS},
                        {57, 0}, {58, 0xFFFFFFFF}, {59, 0xFFFFFFFF}, {60, 0xFFFFFFFF}, {128, 0}, {129, 0}, {130, 0},
                        {131, 0}, {132, 0}, {133, 0}, {134, 0}, {135, 0}, {139, 0}, {141, TRUE},
                        {140, D3DFOG_NONE}, {136, TRUE}, {137, TRUE}, {138, FALSE}, {143, FALSE}, {142, TRUE},
                        {144, FALSE}, {148, D3DMCS_MATERIAL}, {147, D3DMCS_MATERIAL}, {145, D3DMCS_COLOR1},
                        {146, D3DMCS_COLOR2}, {151, D3DVBLEND_DISABLE}, {152, 0}};
    for (const Def& d : defs) rs_[d.s] = d.v;
    for (auto& st : tss_) std::fill(std::begin(st), std::end(st), 0xFFFFFFFFu);
    for (DWORD i = 0; i < 8; i++) {
        DWORD* t = tss_[i];
        t[D3DTSS_COLOROP] = i ? D3DTOP_DISABLE : D3DTOP_MODULATE;
        t[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        t[D3DTSS_COLORARG2] = D3DTA_CURRENT;
        t[D3DTSS_ALPHAOP] = i ? D3DTOP_DISABLE : D3DTOP_SELECTARG1;
        t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        t[D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        for (DWORD k = D3DTSS_BUMPENVMAT00; k <= D3DTSS_BUMPENVMAT11; k++) t[k] = 0;
        t[D3DTSS_TEXCOORDINDEX] = 0;
        t[D3DTSS_ADDRESS] = t[D3DTSS_ADDRESSU] = t[D3DTSS_ADDRESSV] = D3DTADDRESS_WRAP;
        t[D3DTSS_BORDERCOLOR] = 0;
        t[D3DTSS_MAGFILTER] = D3DTFG_POINT;
        t[D3DTSS_MINFILTER] = D3DTFN_POINT;
        t[D3DTSS_MIPFILTER] = D3DTFP_NONE;
        t[D3DTSS_MIPMAPLODBIAS] = 0;
        t[D3DTSS_MAXMIPLEVEL] = 0;
        t[D3DTSS_MAXANISOTROPY] = 1;
        t[D3DTSS_BUMPENVLSCALE] = t[D3DTSS_BUMPENVLOFFSET] = 0;
        t[D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
    }
    vp_ = {0, 0, DWORD(rt_->W()), DWORD(rt_->H()), 0.0f, 1.0f};
    lights_.resize(8);
    cs_.dwStatus = 0x01FFF000;
    cs_.minx = cs_.miny = 2048.0f;
    blocks_.push_back(nullptr);  // handle 0 is never given out
    ctx_.SetTargets(rt_, z_);
    Use u(this);  // creates the Direct3D 9 device now, and sends the state above
}

Device7::~Device7() {
    {
        GpuGuard g(dd_->GetGpu());
        ctx_.SetTargets(nullptr, nullptr);
        for (DWORD i = 0; i < 8; i++) ctx_.SetTexture(i, nullptr);
    }
    for (StateBlock* b : blocks_) delete b;
    for (Surface* t : tex_)
        if (t) t->Release(Surface::kRefV7);
    if (heldVB_) heldVB_->Release();
    // XP: the DirectDraw reference first, then Z, palette and render target (D §Reference counting).
    d3d_->Release();
    if (z_) z_->Release(Surface::kRefV1);
    if (pal_) pal_->Release();
    rt_->Release(Surface::kRefV7);
}

// ---- sending state to Direct3D 9

void Device7::PushAll() {
    alphaSent_ = false;
    for (DWORD s = 0; s < 256; s++) PushRS(s);
    for (DWORD i = 0; i < 8; i++) {
        for (DWORD t = 1; t < 25; t++) PushTSS(i, t);
        ctx_.SetTexture(i, tex_[i]);
    }
    for (DWORD t : {1u, 2u, 3u, 4u, 5u, 6u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u}) PushTransform(t);
    ctx_.SetViewport(vp_.dwX, vp_.dwY, vp_.dwWidth, vp_.dwHeight, vp_.dvMinZ, vp_.dvMaxZ);
    ctx_.SetMaterial(&mat_);
    for (DWORD i = 0; i < lights_.size(); i++) PushLight(i);
    for (DWORD i = 0; i < std::min<DWORD>(caps_.wMaxUserClipPlanes, kMaxPlanes); i++) ctx_.SetClipPlane(i, planes_[i]);
    ctx_.SetTargets(rt_, z_);
}

// S §Implementation notes, render states: only those D3D9 has with the same meaning.
void Device7::PushRS(DWORD s) {
    const DWORD v = rs_[s];
    switch (s) {
    case 7: {  // ZENABLE: W buffering only where D3D9 has it
        const bool w = (caps_.dpcTriCaps.dwRasterCaps & D3DPRASTERCAPS_WBUFFER) != 0;
        ctx_.SetRenderState(7, v == D3DZB_USEW && !w ? DWORD(D3DZB_TRUE) : v);
        break;
    }
    case 15: case 24: case 25: case 41:  // the alpha test, and COLORKEYENABLE's emulation of it
        PushAlphaTest();
        break;
    case 47: {  // ZBIAS: D3D9's DEPTHBIAS (195), a float
        const float bias = -0.000005f * float(LONG(v));
        DWORD b;
        memcpy(&b, &bias, 4);
        ctx_.SetRenderState(195, b);
        break;
    }
    case 151:  // VERTEXBLEND: D3D9's fixed function blends at most 3 weights; per draw, at
        // most the vertices' own (DrawCommon): without, D3D9 would blend with weight 0
        blendSent_ = std::min<DWORD>(v, D3DVBLEND_3WEIGHTS);
        ctx_.SetRenderState(151, blendSent_);
        break;
    case 35: case 36: case 37: case 140:  // FOGTABLEMODE, FOGSTART, FOGEND, FOGVERTEXMODE:
        // sent per draw (DrawCommon: SendFog), what D3D9 gets depending on the vertices
        fogSent_ = false;
        if (s == 35) ctx_.SetRenderState(35, v);
        break;
    case 152:  // CLIPPLANEENABLE: the planes D3D9 has
        ctx_.SetRenderState(152, v & ((1u << std::min<DWORD>(caps_.wMaxUserClipPlanes, 31)) - 1));
        break;
    case 8: case 9: case 14: case 16: case 19: case 20: case 22: case 23: case 26: case 27: case 28: case 29:
    case 34: case 38: case 48: case 52: case 53: case 54: case 55: case 56: case 57:
    case 58: case 59: case 60: case 136: case 137: case 139: case 141: case 142: case 143: case 145:
    case 146: case 147: case 148:
        ctx_.SetRenderState(s, v);
        break;
    default:
        if (s >= 128 && s <= 135) ctx_.SetRenderState(s, v);  // WRAP0-7
        break;  // the rest: stored for Get only, never forwarded (D3D9 numbers mean other things)
    }
}

// COLORKEYENABLE: keyed texels were uploaded transparent; an alpha test drops them.
void Device7::PushAlphaTest() {
    ctx_.SetColorKeyEnable(rs_[41] == TRUE);
    const bool key = rs_[41] == TRUE && tex_[0] && tex_[0]->hasCk[kCkSrcBlt];
    keySent_ = key;
    DWORD v[3];
    if (rs_[15] == TRUE || !key) {
        v[0] = rs_[15] == TRUE;
        v[1] = rs_[24] & 0xFF;
        v[2] = rs_[25] == 0xFFFFFFFF ? DWORD(D3DCMP_ALWAYS) : rs_[25];
    } else {
        v[0] = TRUE;
        v[1] = 0;
        v[2] = D3DCMP_GREATER;
    }
    // (Every stage-0 texture change comes here: only what changed goes to D3D9.)
    static const DWORD kStates[3] = {15, 24, 25};
    for (int i = 0; i < 3; i++) {
        if (alphaSent_ && alphaSentV_[i] == v[i]) continue;
        ctx_.SetRenderState(kStates[i], v[i]);
        alphaSentV_[i] = v[i];
    }
    alphaSent_ = true;
}

// S §Texture stage states: some move to D3D9's sampler states, two with other values.
void Device7::PushTSS(DWORD stage, DWORD t) {
    const DWORD v = tss_[stage][t];
    switch (t) {
    case D3DTSS_ADDRESS: case D3DTSS_ADDRESSU: case D3DTSS_ADDRESSV:
        ctx_.SetSamplerState(stage, 1, addrU_[stage]);
        ctx_.SetSamplerState(stage, 2, addrV_[stage]);
        break;
    case D3DTSS_BORDERCOLOR: ctx_.SetSamplerState(stage, 4, v); break;
    case D3DTSS_MAGFILTER: {
        static const DWORD kMap[] = {0, 1, 2, 2, 2, 3};  // FLAT/GAUSSIANCUBIC: linear; ANISOTROPIC 5 -> 3
        ctx_.SetSamplerState(stage, 5, v < 6 ? kMap[v] : 2);
        break;
    }
    case D3DTSS_MINFILTER: ctx_.SetSamplerState(stage, 6, v <= 3 ? v : 2); break;
    case D3DTSS_MIPFILTER: ctx_.SetSamplerState(stage, 7, v >= 2 && v <= 3 ? v - 1 : 0); break;
    case D3DTSS_MIPMAPLODBIAS: ctx_.SetSamplerState(stage, 8, v); break;
    case D3DTSS_MAXMIPLEVEL: ctx_.SetSamplerState(stage, 9, v); break;
    case D3DTSS_MAXANISOTROPY: ctx_.SetSamplerState(stage, 10, v); break;
    default:
        if ((t >= 1 && t <= 11) || (t >= 22 && t <= 24)) ctx_.SetTextureStageState(stage, t, v);
        break;
    }
}

D3DMATRIX* Device7::Matrix(DWORD t) {
    switch (t) {
    case D3DTRANSFORMSTATE_WORLD: return &world_;
    case D3DTRANSFORMSTATE_VIEW: return &view_;
    case D3DTRANSFORMSTATE_PROJECTION: return &proj_;
    case D3DTRANSFORMSTATE_WORLD1: return &world1_;
    case D3DTRANSFORMSTATE_WORLD2: return &world2_;
    case D3DTRANSFORMSTATE_WORLD3: return &world3_;
    default: return t >= 16 && t <= 23 ? &texm_[t - 16] : nullptr;
    }
}

void Device7::PushTransform(DWORD t) {
    const D3DMATRIX* m = Matrix(t);
    if (!m) return;
    const DWORD t9 = t == 1 ? 256 : t >= 4 && t <= 6 ? 256 + (t - 3) : t;  // WORLD, WORLDMATRIX(1-3)
    ctx_.SetTransform(t9, &m->_11);
}

void Device7::PushLight(DWORD i) {
    const Light& l = lights_[i];
    if (!l.set) return;
    ctx_.SetLight(i, &l.l);
    ctx_.LightEnable(i, l.enabled);
}

// ---- states

HRESULT Device7::SetRS(DWORD s, DWORD v) {
    if (s >= 256) return D3D_OK;  // XP writes past its array; ignored here
    if (rs_[s] == v) return D3D_OK;
    rs_[s] = v;
    Use u(this);
    if (u.ok) PushRS(s);
    return D3D_OK;
}

HRESULT Device7::SetRenderState(D3DRENDERSTATETYPE s, DWORD v) {
    Guard l;
    if (Recording()) {
        if (Retired(s)) return DDERR_INVALIDPARAMS;
        Cmd c;
        c.kind = Cmd::kRS;
        c.a = s;
        c.b = v;
        Rec()->cmds.push_back(c);
        return D3D_OK;
    }
    return SetRS(s, v);
}

HRESULT Device7::GetRenderState(D3DRENDERSTATETYPE s, DWORD* v) {
    if (!v) return DDERR_INVALIDPARAMS;
    if (Retired(s)) return DDERR_INVALIDPARAMS;
    Guard l;
    *v = DWORD(s) < 256 ? rs_[s] : 0;
    return D3D_OK;
}

HRESULT Device7::SetTSS(DWORD stage, DWORD t, DWORD v) {
    if (stage >= 8 || t >= 64) return D3D_OK;  // XP writes outside its array; ignored here
    if (tss_[stage][t] == v) return D3D_OK;
    tss_[stage][t] = v;
    // XP hands ADDRESS to the driver as it is, which sets U and V: the last of the three
    // set wins, also when everything is sent again after a reset.
    if (t == D3DTSS_ADDRESS) addrU_[stage] = addrV_[stage] = v;
    else if (t == D3DTSS_ADDRESSU) addrU_[stage] = v;
    else if (t == D3DTSS_ADDRESSV) addrV_[stage] = v;
    Use u(this);
    if (u.ok) PushTSS(stage, t);
    return D3D_OK;
}

HRESULT Device7::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) {
    Guard l;
    if (Recording()) {
        Cmd c;
        c.kind = Cmd::kTSS;
        c.a = stage;
        c.b = t;
        c.c = v;
        Rec()->cmds.push_back(c);
        return D3D_OK;
    }
    return SetTSS(stage, t, v);
}

HRESULT Device7::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD* v) {
    if (!v) return DDERR_INVALIDPARAMS;
    Guard l;
    *v = stage < 8 && DWORD(t) < 64 ? tss_[stage][t] : 0;
    return D3D_OK;
}

HRESULT Device7::SetTex(DWORD stage, Surface* s) {
    if (stage >= 8) return D3D_OK;
    if (s && !(s->desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE)) s = nullptr;  // no texture object: binds nothing
    if (tex_[stage] == s) return D3D_OK;
    if (s) s->AddRef(Surface::kRefV7);
    Surface* old = tex_[stage];
    tex_[stage] = s;
    {
        Use u(this);
        if (u.ok) {
            ctx_.SetTexture(stage, s);
            if (stage == 0) PushAlphaTest();
        }
    }
    if (old) old->Release(Surface::kRefV7);
    return D3D_OK;
}

HRESULT Device7::SetTexture(DWORD stage, IDirectDrawSurface7* surface) {
    Guard l;
    Surface* s = Surf(surface);
    if (Recording()) {
        Cmd c;
        c.kind = Cmd::kTex;
        c.a = stage;
        c.tex = s;
        Rec()->cmds.push_back(c);
        return D3D_OK;
    }
    return SetTex(stage, s);
}

HRESULT Device7::GetTexture(DWORD stage, IDirectDrawSurface7** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    Surface* s = stage < 8 ? tex_[stage] : nullptr;
    *out = nullptr;
    if (s) {
        s->AddRef(Surface::kRefV7);
        *out = static_cast<IDirectDrawSurface7*>(s->Iface(7));
    }
    return D3D_OK;
}

void Device7::SetXform(DWORD t, const D3DMATRIX& m) {
    D3DMATRIX* dst = Matrix(t);
    if (!dst) return;  // an invalid type is ignored (S §SetTransform)
    *dst = m;
    Use u(this);
    if (u.ok) PushTransform(t);
}

HRESULT Device7::SetTransform(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) {
    if (!m) return DDERR_INVALIDPARAMS;  // XP faults
    Guard l;
    if (Recording()) {
        Cmd c;
        c.kind = Cmd::kXform;
        c.a = t;
        c.m = *m;
        Rec()->cmds.push_back(c);
        return D3D_OK;
    }
    SetXform(t, *m);
    return D3D_OK;
}

HRESULT Device7::GetTransform(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) {
    if (!m) return DDERR_INVALIDPARAMS;
    Guard l;
    const D3DMATRIX* src = Matrix(t);
    if (!src) return DDERR_INVALIDPARAMS;
    *m = *src;
    return D3D_OK;
}

// new = m x current, never recorded (S §MultiplyTransform).
HRESULT Device7::MultiplyTransform(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) {
    if (!m) return DDERR_INVALIDPARAMS;
    Guard l;
    const D3DMATRIX* cur = Matrix(t);
    if (!cur) return DDERR_INVALIDPARAMS;
    D3DMATRIX r;
    MatMul(&r, *m, *cur);
    SetXform(t, r);
    return D3D_OK;
}

HRESULT Device7::SetVP(const D3DVIEWPORT7& in) {
    const DWORD w = DWORD(rt_->W()), h = DWORD(rt_->H());
    if (in.dwX > w || in.dwY > h || in.dwWidth > w - in.dwX || in.dwHeight > h - in.dwY) return DDERR_INVALIDPARAMS;
    D3DVIEWPORT7 vp = in;
    if (vp.dvMaxZ - vp.dvMinZ < 0.001f) {  // UpdateViewportCache's nudge, kept in the stored copy
        if (vp.dvMaxZ < 0.5f) vp.dvMaxZ = vp.dvMinZ + 0.001f;
        else vp.dvMinZ = vp.dvMaxZ - 0.001f;
    }
    vp_ = vp;
    Use u(this);
    if (u.ok) ctx_.SetViewport(vp.dwX, vp.dwY, vp.dwWidth, vp.dwHeight, vp.dvMinZ, vp.dvMaxZ);
    return D3D_OK;
}

HRESULT Device7::SetViewport(D3DVIEWPORT7* vp) {
    if (!vp) return DDERR_INVALIDPARAMS;
    Guard l;
    if (Recording()) {
        Cmd c;
        c.kind = Cmd::kViewport;
        c.vp = *vp;
        Rec()->cmds.push_back(c);
        return D3D_OK;
    }
    return SetVP(*vp);
}

void Device7::SetMat(const D3DMATERIAL7& m) {
    mat_ = m;
    if (m.dvPower > 0) pvPower_ = m.dvPower;
    Use u(this);
    if (u.ok) ctx_.SetMaterial(&mat_);
}

HRESULT Device7::SetMaterial(D3DMATERIAL7* m) {
    if (!m) return DDERR_INVALIDPARAMS;
    Guard l;
    if (Recording()) {
        Cmd c;
        c.kind = Cmd::kMaterial;
        c.mat = *m;
        Rec()->cmds.push_back(c);
        return D3D_OK;
    }
    SetMat(*m);
    return D3D_OK;
}

// CheckLightParams (S §SetLight).
bool LightOk(const D3DLIGHT7* l) {
    if (!l) return false;
    if (l->dltType != D3DLIGHT_POINT && l->dltType != D3DLIGHT_SPOT && l->dltType != D3DLIGHT_DIRECTIONAL) return false;
    if (!(l->dvRange >= 0.0f) || l->dvRange > sqrtf(FLT_MAX)) return false;
    if (l->dltType != D3DLIGHT_POINT) {
        const D3DVECTOR& d = l->dvDirection;
        if (d.x * d.x + d.y * d.y + d.z * d.z < 1e-5f) return false;
    }
    if (l->dltType == D3DLIGHT_SPOT) {
        if (!(l->dvPhi >= 0.0f) || l->dvPhi > 3.14159265f || !(l->dvTheta >= 0.0f) || l->dvTheta > l->dvPhi) return false;
        if (l->dvAttenuation0 < 0.0f || l->dvAttenuation1 < 0.0f || l->dvAttenuation2 < 0.0f) return false;
    }
    return true;
}

HRESULT Device7::SetLightI(DWORD i, const D3DLIGHT7& l) {
    if (i >= 0x10000) return E_OUTOFMEMORY;  // as XP when its array can't grow (not a C++ exception)
    if (i >= lights_.size()) lights_.resize(size_t(i) + 10);
    lights_[i].l = l;
    lights_[i].set = true;
    Use u(this);
    if (u.ok) PushLight(i);
    return D3D_OK;
}

HRESULT Device7::SetLight(DWORD i, D3DLIGHT7* l) {
    if (!LightOk(l)) return DDERR_INVALIDPARAMS;
    Guard g;
    if (Recording()) {
        Cmd c;
        c.kind = Cmd::kLight;
        c.a = i;
        c.light = *l;
        Rec()->cmds.push_back(c);
        return D3D_OK;
    }
    return SetLightI(i, *l);
}

HRESULT Device7::LightEnableI(DWORD i, bool on) {
    if (i >= 0x10000) return E_OUTOFMEMORY;  // as SetLight (a vector that size would throw)
    if (i >= lights_.size()) lights_.resize(size_t(i) + 10);
    Light& l = lights_[i];
    if (!l.set) {  // a default white directional light (S §LightEnable)
        l.l = {};
        l.l.dltType = D3DLIGHT_DIRECTIONAL;
        l.l.dcvDiffuse.r = l.l.dcvDiffuse.g = l.l.dcvDiffuse.b = 1.0f;
        l.l.dvDirection.z = 1.0f;
        l.set = true;
    }
    l.enabled = on;
    Use u(this);
    if (u.ok) PushLight(i);
    return D3D_OK;
}

HRESULT Device7::LightEnable(DWORD i, WINBOOL on) {
    Guard g;
    if (Recording()) {
        Cmd c;
        c.kind = Cmd::kLightEnable;
        c.a = i;
        c.b = on != 0;
        Rec()->cmds.push_back(c);
        return D3D_OK;
    }
    return LightEnableI(i, on != 0);
}

void Device7::SetPlane(DWORD i, const float* p) {
    if (i >= kMaxPlanes) return;
    memcpy(planes_[i], p, sizeof(planes_[i]));
    Use u(this);
    if (u.ok && i < caps_.wMaxUserClipPlanes) ctx_.SetClipPlane(i, planes_[i]);
}

HRESULT Device7::SetClipPlane(DWORD i, D3DVALUE* p) {
    if (!p) return DDERR_INVALIDPARAMS;
    Guard g;
    if (Recording()) {
        Cmd c;
        c.kind = Cmd::kClipPlane;
        c.a = i;
        memcpy(c.plane, p, sizeof(c.plane));
        Rec()->cmds.push_back(c);
        return D3D_OK;
    }
    SetPlane(i, p);
    return D3D_OK;
}

// The clip status a draw of untransformed vertices leaves (D3DFE_UpdateClipStatus): each
// vertex's clip code in the front end's 0..w volume and against the user clip planes (world
// space, D3DCLIP_GEN0-5), their union ORed into dwStatus and their intersection ANDed (its
// bits 12 up), and, with D3DRENDERSTATE_EXTENTS, the extents grown by the vertices inside.
// Vertex blending as the draw does it: min(the FVF's weights, VERTEXBLEND, 3) weights after
// the position, the last world matrix weighted 1 - their sum.
void Device7::TrackClipStatus(DWORD fvf, const void* v, UINT stride, DWORD n) {
    const DWORD posType = fvf & 0x0E, betas = posType >= 0x06 ? (posType - 0x04) / 2 : 0;
    const DWORD blend = std::min<DWORD>(std::min<DWORD>(rs_[D3DRENDERSTATE_VERTEXBLEND], betas), 3);
    if (stride < 12 + 4 * blend) return;
    const D3DMATRIX* worlds[4] = {&world_, &world1_, &world2_, &world3_};
    D3DMATRIX vpm;
    MatMul(&vpm, view_, proj_);
    const DWORD planesOn = rs_[D3DRENDERSTATE_CLIPPLANEENABLE] & 0x3F;
    const bool extents = rs_[D3DRENDERSTATE_EXTENTS] == TRUE;
    const D3DVIEWPORT7& vp = vp_;
    DWORD orCodes = 0, andCodes = 0xFFF;
    for (DWORD i = 0; i < n; i++) {
        const float* p = reinterpret_cast<const float*>(static_cast<const uint8_t*>(v) + size_t(i) * stride);
        // The world-space point (homogeneous: a world matrix may have a w column).
        float wp[4] = {0, 0, 0, 0};
        float sum = 0;
        for (DWORD k = 0; k <= blend; k++) {
            const float w = k < blend ? p[3 + k] : 1.0f - sum;
            if (k < blend) sum += w;
            const D3DMATRIX& M = *worlds[k];
            wp[0] += (p[0] * M._11 + p[1] * M._21 + p[2] * M._31 + M._41) * w;
            wp[1] += (p[0] * M._12 + p[1] * M._22 + p[2] * M._32 + M._42) * w;
            wp[2] += (p[0] * M._13 + p[1] * M._23 + p[2] * M._33 + M._43) * w;
            wp[3] += (p[0] * M._14 + p[1] * M._24 + p[2] * M._34 + M._44) * w;
        }
        const float cx = wp[0] * vpm._11 + wp[1] * vpm._21 + wp[2] * vpm._31 + wp[3] * vpm._41;
        const float cy = wp[0] * vpm._12 + wp[1] * vpm._22 + wp[2] * vpm._32 + wp[3] * vpm._42;
        const float cz = wp[0] * vpm._13 + wp[1] * vpm._23 + wp[2] * vpm._33 + wp[3] * vpm._43;
        const float cw = wp[0] * vpm._14 + wp[1] * vpm._24 + wp[2] * vpm._34 + wp[3] * vpm._44;
        const float fx = (cx + cw) * 0.5f, fy = (cy + cw) * 0.5f;
        DWORD code = 0;
        if (fx < 0) code |= D3DCLIP_LEFT;
        if (fx > cw) code |= D3DCLIP_RIGHT;
        if (fy > cw) code |= D3DCLIP_TOP;
        if (fy < 0) code |= D3DCLIP_BOTTOM;
        if (cz < 0) code |= D3DCLIP_FRONT;
        if (cz > cw) code |= D3DCLIP_BACK;
        // A plane (a, b, c, d) clips where a x + b y + c z + d w < 0.
        for (DWORD k = 0; k < 6; k++)
            if ((planesOn & (1u << k)) &&
                planes_[k][0] * wp[0] + planes_[k][1] * wp[1] + planes_[k][2] * wp[2] + planes_[k][3] * wp[3] < 0)
                code |= D3DCLIP_GEN0 << k;
        orCodes |= code;
        andCodes &= code;
        if (!code && extents && cw != 0) {
            const float rhw = 1.0f / cw;
            const float sx = cx * rhw * (vp.dwWidth * 0.5f) + vp.dwX + vp.dwWidth * 0.5f;
            const float sy = -cy * rhw * (vp.dwHeight * 0.5f) + vp.dwY + vp.dwHeight * 0.5f;
            cs_.minx = std::min(cs_.minx, sx);
            cs_.maxx = std::max(cs_.maxx, sx);
            cs_.miny = std::min(cs_.miny, sy);
            cs_.maxy = std::max(cs_.maxy, sy);
        }
    }
    if (n) cs_.dwStatus = (cs_.dwStatus | (orCodes & 0xFFF)) & ((andCodes << 12) | 0xFF000FFF);
}

HRESULT Device7::SetClipStatus(D3DCLIPSTATUS* cs) {
    if (!cs) return DDERR_INVALIDPARAMS;
    if (cs->dwFlags & D3DCLIPSTATUS_EXTENTS3) return DDERR_INVALIDPARAMS;
    Guard g;
    if (cs->dwFlags & D3DCLIPSTATUS_STATUS) cs_.dwStatus = cs->dwStatus;
    if (cs->dwFlags & D3DCLIPSTATUS_EXTENTS2) {
        cs_.minx = cs->minx;
        cs_.maxx = cs->maxx;
        cs_.miny = cs->miny;
        cs_.maxy = cs->maxy;
    }
    return D3D_OK;
}

HRESULT Device7::GetClipStatus(D3DCLIPSTATUS* cs) {
    if (!cs) return DDERR_INVALIDPARAMS;
    Guard g;
    if (!csTracking_) DD2D9_LOG("Direct3D: the game reads the clip status: drawing updates it from now on");
    csTracking_ = true;
    cs->dwFlags = D3DCLIPSTATUS_EXTENTS2;
    cs->dwStatus = cs_.dwStatus;
    cs->minx = cs_.minx;
    cs->maxx = cs_.maxx;
    cs->miny = cs_.miny;
    cs->maxy = cs_.maxy;  // minz/maxz left alone
    return D3D_OK;
}

// ---- scenes, targets, clearing

bool Device7::TargetsBusy() const { return rt_->lockCount || (z_ && z_->lockCount); }

HRESULT Device7::BeginScene() {
    Guard l;
    if (inScene_) return D3DERR_SCENE_IN_SCENE;
    if (TargetsBusy()) return DDERR_SURFACEBUSY;
    if (rt_->CantDraw() || (z_ && z_->CantDraw(false))) return DDERR_SURFACELOST;
    if (Recording()) return D3DERR_SCENE_END_FAILED;  // sic (S §BeginScene)
    inScene_ = true;
    return D3D_OK;
}

HRESULT Device7::EndScene() {
    Guard l;
    if (!inScene_) return D3DERR_SCENE_NOT_IN_SCENE;
    inScene_ = false;
    if (Recording()) return D3DERR_SCENE_END_FAILED;
    {
        GpuGuard g(dd_->GetGpu());
        ctx_.EndScene();
    }
    if (lostInScene_) {
        lostInScene_ = false;
        return DDERR_SURFACELOST;
    }
    return D3D_OK;
}

HRESULT Device7::SetRenderTarget(IDirectDrawSurface7* surface, DWORD) {
    if (!surface) return DDERR_INVALIDOBJECT;
    Guard l;
    Surface* s = Surf(surface);
    if (!s) return DDERR_INVALIDOBJECT;
    if (!(s->desc.ddsCaps.dwCaps & DDSCAPS_3DDEVICE) || !(BitsToDDBD(s->buf->fmt.bpp) & caps_.dwDeviceRenderBitDepth) ||
        s->W() > 2048 || s->H() > 2048)
        return DDERR_INVALIDPARAMS;
    if (s->buf->fmt.pal8 && !s->palette) return DDERR_NOPALETTEATTACHED;
    Surface* z = AttachedZ(s);
    if (s == rt_ && z == z_) return D3D_OK;  // the Z buffer is looked up again (S §SetRenderTarget)
    s->AddRef(Surface::kRefV7);
    if (z) z->AddRef(Surface::kRefV1);
    Surface* oldRt = rt_;
    Surface* oldZ = z_;
    rt_ = s;
    z_ = z;
    {
        Use u(this);
        ctx_.SetTargets(rt_, z_);  // the viewport stays (S §SetRenderTarget)
    }
    oldRt->Release(Surface::kRefV7);
    if (oldZ) oldZ->Release(Surface::kRefV1);
    return D3D_OK;
}

HRESULT Device7::Clear(DWORD count, D3DRECT* rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil) {
    if (count > 4096) return DDERR_INVALIDPARAMS;
    Guard l;
    if (flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) {
        if (!z_) return D3DERR_ZBUFFER_NOTPRESENT;
        if ((flags & D3DCLEAR_STENCIL) && !z_->buf->fmt.a) return D3DERR_STENCILBUFFER_NOTPRESENT;
    }
    if (!(flags & 7)) return DDERR_INVALIDPARAMS;
    // Rectangles clipped to the viewport; none given: the viewport; count 0 with some: nothing.
    std::vector<RECT> clipped;
    const RECT view = {LONG(vp_.dwX), LONG(vp_.dwY), LONG(vp_.dwX + vp_.dwWidth), LONG(vp_.dwY + vp_.dwHeight)};
    if (rects) {
        for (DWORD i = 0; i < count; i++) {
            RECT r = {rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2}, c;
            if (IntersectRect(&c, &r, &view)) clipped.push_back(c);
        }
        if (clipped.empty()) return D3D_OK;
    }
    Use u(this);
    if (u.ok) ctx_.Clear(DWORD(clipped.size()), clipped.empty() ? nullptr : clipped.data(), flags & 7, color, z, stencil);
    return D3D_OK;
}

// ---- drawing

// Room for `n` elements in `v`, false (instead of std::bad_alloc out of a COM method) when the
// memory isn't there.
template <class T> static bool Reserve(std::vector<T>* v, size_t n) {
    try {
        v->reserve(n);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

HRESULT Device7::DrawCommon(D3DPRIMITIVETYPE t, DWORD fvf, const void* v, UINT stride, DWORD n, const WORD* idx,
                            DWORD ni) {
    if (!ValidPrim(t)) return DDERR_INVALIDPARAMS;  // undefined on XP (V §5.3)
    const UINT prims = PrimCount(t, idx ? ni : n);
    if (!prims || !n) return D3D_OK;
    if (!v || (idx == nullptr && ni)) return DDERR_INVALIDPARAMS;
    if (n > 0x01000000 || ni > 0x01000000) return DDERR_INVALIDPARAMS;  // sizes that wrap a 32-bit size_t
    if (rt_->CantDraw(false)) {
        if (inScene_) lostInScene_ = true;
        return D3D_OK;
    }
    fvf &= ~DWORD(0xF001);  // reserved bits (D3D9's LASTBETA flags, RESERVED0)
    if (csTracking_ && (fvf & 0x0E) && (fvf & 0x0E) != D3DFVF_XYZRHW) TrackClipStatus(fvf, v, stride, n);
    // Lit vertices without a normal: XP lights them with a zero one (ambient and emissive
    // only); D3D9 (DXVK) leaves them unlit. Given a zero normal here.
    // Indices past the vertices given: vertex 0 instead (the GPU would read past them).
    if (idx) {
        for (DWORD i = 0; i < ni; i++) {
            if (idx[i] >= n) {
                if (!Reserve(&safeIdx_, ni)) return DDERR_OUTOFMEMORY;
                safeIdx_.assign(idx, idx + ni);
                for (WORD& k : safeIdx_)
                    if (k >= n) k = 0;
                idx = safeIdx_.data();
                break;
            }
        }
    }
    if (rs_[D3DRENDERSTATE_LIGHTING] == TRUE && (fvf & 0x0E) != D3DFVF_XYZRHW && !(fvf & D3DFVF_NORMAL)) {
        const UINT posSize = FvfSize(fvf & 0x0E), newStride = stride + 12;
        std::vector<uint8_t>& withNormal = withNormal_;
        if (!Reserve(&withNormal, size_t(newStride) * n)) return DDERR_OUTOFMEMORY;
        withNormal.assign(size_t(newStride) * n, 0);
        for (DWORD i = 0; i < n; i++) {
            const uint8_t* src = static_cast<const uint8_t*>(v) + size_t(i) * stride;
            uint8_t* dst = withNormal.data() + size_t(i) * newStride;
            memcpy(dst, src, posSize);
            memcpy(dst + posSize + 12, src + posSize, stride - posSize);
        }
        v = withNormal.data();
        stride = newStride;
        fvf |= D3DFVF_NORMAL;
    }
    Use u(this);
    if (!u.ok) return D3D_OK;
    // The texture's colour key may have come or gone since (SetColorKey): sent again then.
    if (rs_[41] == TRUE && tex_[0] && int(tex_[0]->hasCk[kCkSrcBlt]) != keySent_) PushAlphaTest();
    {
        // VERTEXBLEND reads min(the FVF's weights, the state) (V §9).
        const DWORD posType = fvf & 0x0E, betas = posType >= 0x06 && posType != D3DFVF_XYZRHW ? (posType - 0x04) / 2 : 0;
        const DWORD want = std::min<DWORD>(std::min<DWORD>(rs_[151], betas), D3DVBLEND_3WEIGHTS);
        if (want != blendSent_) {
            ctx_.SetRenderState(151, want);
            blendSent_ = want;
        }
    }
    SendFog(fvf);
    ctx_.Draw(t, fvf, v, stride, n, idx, ni, prims, rs_[7] != D3DZB_FALSE);
    return D3D_OK;
}

// The fog states D3D9 gets for a draw, as Windows' hardware path behaved (Wine's tests,
// confirmed there):
// - exponential vertex fog with lighting off on vertices that bring a specular colour takes
//   the fog factor from its alpha, uncomputed (test_fog_interpolation); computed otherwise,
//   lighting off or not (test_negative_fixedfunction_fog, visual's test_blend);
// - FOGSTART equal to FOGEND: table fog a step at that depth, nearer unfogged and farther
//   fogged (D3D9 divides by their difference and fogs everything: the end made a hair
//   farther); linear vertex fog fogs everything (test_fog_special).
void Device7::SendFog(DWORD fvf) {
    DWORD mode = rs_[140];
    if ((mode == D3DFOG_EXP || mode == D3DFOG_EXP2) && rs_[137] != TRUE && (fvf & D3DFVF_SPECULAR)) mode = D3DFOG_NONE;
    float st, en;
    memcpy(&st, &rs_[36], 4);
    memcpy(&en, &rs_[37], 4);
    if (st == en && std::isfinite(st)) {
        if (rs_[35] != D3DFOG_NONE) {
            en = st + std::max(std::fabs(st) * 1e-6f, 1e-6f);
        } else if (mode == D3DFOG_LINEAR) {
            st = 2e9f;  // (end - d) / (end - start) about -1 for any depth: fogged
            en = 1e9f;
        }
    }
    DWORD v[3] = {mode, 0, 0};
    memcpy(&v[1], &st, 4);
    memcpy(&v[2], &en, 4);
    static const DWORD kStates[3] = {140, 36, 37};
    for (int i = 0; i < 3; i++) {
        if (fogSent_ && fogSentV_[i] == v[i]) continue;
        ctx_.SetRenderState(kStates[i], v[i]);
        fogSentV_[i] = v[i];
    }
    fogSent_ = true;
}

HRESULT Device7::DrawPrimitive(D3DPRIMITIVETYPE t, DWORD fvf, void* v, DWORD n, DWORD) {
    Guard l;
    HoldVB(nullptr);  // drawing from its own buffer drops the device's hold on a VB
    return DrawCommon(t, fvf, v, FvfSize(fvf), n, nullptr, 0);
}

HRESULT Device7::DrawIndexedPrimitive(D3DPRIMITIVETYPE t, DWORD fvf, void* v, DWORD n, WORD* idx, DWORD ni, DWORD) {
    Guard l;
    HoldVB(nullptr);
    return DrawCommon(t, fvf, v, FvfSize(fvf), n, idx, ni);
}

// Gathers strided data into interleaved vertices, in FVF order (V §3).
static void Gather(DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA& d, DWORD n, std::vector<uint8_t>* out) {
    const UINT stride = FvfSize(fvf);
    out->assign(size_t(stride) * n + 16, 0);  // slack: see VertexBuffer7's memory
    const UINT posSize = FvfSize(fvf & 0x0E);
    const DWORD tex = (fvf >> 8) & 0xF;
    for (DWORD i = 0; i < n; i++) {
        uint8_t* o = out->data() + size_t(i) * stride;
        auto copy = [&](const D3DDP_PTRSTRIDE& p, UINT size) {
            if (p.lpvData) memcpy(o, static_cast<const uint8_t*>(p.lpvData) + size_t(i) * p.dwStride, size);
            o += size;
        };
        copy(d.position, posSize);
        if (fvf & D3DFVF_NORMAL) copy(d.normal, 12);
        if (fvf & D3DFVF_RESERVED1) o += 4;
        if (fvf & D3DFVF_DIFFUSE) copy(d.diffuse, 4);
        if (fvf & D3DFVF_SPECULAR) copy(d.specular, 4);
        for (DWORD k = 0; k < tex && k < D3DDP_MAXTEXCOORD; k++) {
            static const UINT kSize[] = {8, 12, 16, 4};
            copy(d.textureCoords[k], kSize[(fvf >> (16 + 2 * k)) & 3]);
        }
    }
}

HRESULT Device7::DrawPrimitiveStrided(D3DPRIMITIVETYPE t, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA* d, DWORD n, DWORD) {
    if ((fvf & 0x0E) == D3DFVF_XYZRHW) return D3DERR_INVALIDVERTEXTYPE;
    if (!d || n > 0x01000000) return DDERR_INVALIDPARAMS;  // sizes that wrap a 32-bit size_t
    Guard l;
    HoldVB(nullptr);
    if (!Reserve(&gathered_, size_t(FvfSize(fvf)) * n + 16)) return DDERR_OUTOFMEMORY;
    Gather(fvf, *d, n, &gathered_);
    return DrawCommon(t, fvf, gathered_.data(), FvfSize(fvf), n, nullptr, 0);
}

HRESULT Device7::DrawIndexedPrimitiveStrided(D3DPRIMITIVETYPE t, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA* d, DWORD n,
                                             WORD* idx, DWORD ni, DWORD) {
    if ((fvf & 0x0E) == D3DFVF_XYZRHW) return D3DERR_INVALIDVERTEXTYPE;
    if (!d || n > 0x01000000) return DDERR_INVALIDPARAMS;  // sizes that wrap a 32-bit size_t
    Guard l;
    HoldVB(nullptr);
    if (!Reserve(&gathered_, size_t(FvfSize(fvf)) * n + 16)) return DDERR_OUTOFMEMORY;
    Gather(fvf, *d, n, &gathered_);
    return DrawCommon(t, fvf, gathered_.data(), FvfSize(fvf), n, idx, ni);
}

// The device holds the last VB it drew from directly (V §8.7).
void Device7::HoldVB(VertexBuffer7* vb) {
    if (vb == heldVB_) return;
    if (vb) vb->AddRef();
    if (heldVB_) heldVB_->Release();
    heldVB_ = vb;
}

// A transformed vertex buffer that ProcessVertices(D3DVOP_CLIP) left with clip codes: its
// vertices outside the frustum are in clip space (x and y as (c + w) / 2, z, w), the others
// on the screen (V §8.5). XP's draw clips them by the codes (V §4 step 3a). Here: each
// primitive in clip space, clipped against the near plane (z >= 0: whatever is behind the
// camera), then projected through the viewport; past the screen's edges the GPU clips them,
// as any transformed vertex. Every attribute is interpolated (the colours per byte). The
// result is a list of the primitive's kind, drawn instead.
namespace {
constexpr WORD kInClipSpace = 0x8000;  // a clip code's mark: the vertex was stored in clip space
struct ClipVert {
    float c[4];                // clip space: x, y, z, w
    std::vector<uint8_t> rest;  // everything after the position, as in the buffer
};
}  // namespace

static bool ExpandClipped(D3DPRIMITIVETYPE t, DWORD fvf, const uint8_t* mem, UINT stride, const WORD* codes, DWORD n,
                          const WORD* idx, DWORD ni, const D3DVIEWPORT7& vp, std::vector<uint8_t>* out,
                          D3DPRIMITIVETYPE* outType) {
    // Only vertices stored in clip space need it (codes from a transformed source mark
    // vertices left on the screen, which the GPU clips as any).
    bool any = false;
    for (DWORD i = 0; i < n && !any; i++) any = (codes[i] & kInClipSpace) != 0;
    if (!any) return false;
    const float hw = vp.dwWidth * 0.5f, hh = vp.dwHeight * 0.5f, zr = vp.dvMaxZ - vp.dvMinZ;
    // Where the colours are (lerped per byte); the rest is floats.
    UINT off = 16;
    int diffuse = -1, specular = -1;
    if (fvf & D3DFVF_NORMAL) off += 12;
    if (fvf & D3DFVF_RESERVED1) off += 4;
    if (fvf & D3DFVF_DIFFUSE) diffuse = int(off), off += 4;
    if (fvf & D3DFVF_SPECULAR) specular = int(off);
    auto load = [&](DWORD i) {
        ClipVert v;
        const uint8_t* p = mem + size_t(i) * stride;
        const float* f = reinterpret_cast<const float*>(p);
        if (codes[i] & kInClipSpace) {  // (x + w) / 2, (y + w) / 2, z, w
            v.c[3] = f[3];
            v.c[0] = 2 * f[0] - f[3];
            v.c[1] = 2 * f[1] - f[3];
            v.c[2] = f[2];
        } else {  // on the screen: back to clip space
            const float w = f[3] != 0 ? 1.0f / f[3] : 1.0f;
            v.c[3] = w;
            v.c[0] = (f[0] - vp.dwX - hw) / hw * w;
            v.c[1] = -(f[1] - vp.dwY - hh) / hh * w;
            v.c[2] = zr != 0 ? (f[2] - vp.dvMinZ) / zr * w : 0.0f;
        }
        v.rest.assign(p + 16, p + stride);
        return v;
    };
    auto lerp = [&](const ClipVert& a, const ClipVert& b, float k) {
        ClipVert v;
        for (int j = 0; j < 4; j++) v.c[j] = a.c[j] + (b.c[j] - a.c[j]) * k;
        v.rest = a.rest;
        for (UINT o = 0; o + 4 <= a.rest.size(); o += 4) {
            const int at = int(o + 16);
            if (at == diffuse || at == specular) {
                for (int byte = 0; byte < 4; byte++) {
                    const float x = a.rest[o + byte] + (float(b.rest[o + byte]) - a.rest[o + byte]) * k;
                    v.rest[o + byte] = uint8_t(std::clamp(x + 0.5f, 0.0f, 255.0f));
                }
            } else {
                float fa, fb;
                memcpy(&fa, &a.rest[o], 4);
                memcpy(&fb, &b.rest[o], 4);
                const float x = fa + (fb - fa) * k;
                memcpy(&v.rest[o], &x, 4);
            }
        }
        return v;
    };
    auto emit = [&](const ClipVert& v) {
        const size_t at = out->size();
        out->resize(at + stride);
        float* f = reinterpret_cast<float*>(out->data() + at);
        const float rhw = 1.0f / v.c[3];
        f[0] = v.c[0] * rhw * hw + vp.dwX + hw;
        f[1] = -v.c[1] * rhw * hh + vp.dwY + hh;
        f[2] = v.c[2] * rhw * zr + vp.dvMinZ;
        f[3] = rhw;
        memcpy(out->data() + at + 16, v.rest.data(), v.rest.size());
    };
    // Clipped against z >= 0 (the near plane; w > 0 beyond it). Sutherland-Hodgman.
    auto clipPoly = [&](std::vector<ClipVert> in) {
        std::vector<ClipVert> res;
        for (size_t i = 0; i < in.size(); i++) {
            const ClipVert& a = in[i];
            const ClipVert& b = in[(i + 1) % in.size()];
            const bool ain = a.c[2] >= 0, bin = b.c[2] >= 0;
            if (ain) res.push_back(a);
            if (ain != bin) res.push_back(lerp(a, b, a.c[2] / (a.c[2] - b.c[2])));
        }
        return res;
    };
    auto vertexAt = [&](DWORD k) { return idx ? DWORD(idx[k]) : k; };
    const DWORD count = idx ? ni : n;
    out->clear();
    switch (t) {
    case D3DPT_POINTLIST:
        *outType = D3DPT_POINTLIST;
        for (DWORD k = 0; k < count; k++) {
            const ClipVert v = load(vertexAt(k));
            if (v.c[2] >= 0 && v.c[3] > 0) emit(v);
        }
        return true;
    case D3DPT_LINELIST:
    case D3DPT_LINESTRIP:
        *outType = D3DPT_LINELIST;
        for (DWORD k = 0; k + 1 < count; k += t == D3DPT_LINELIST ? 2 : 1) {
            ClipVert a = load(vertexAt(k)), b = load(vertexAt(k + 1));
            if (a.c[2] < 0 && b.c[2] < 0) continue;
            if (a.c[2] < 0) a = lerp(a, b, a.c[2] / (a.c[2] - b.c[2]));
            else if (b.c[2] < 0) b = lerp(b, a, b.c[2] / (b.c[2] - a.c[2]));
            if (a.c[3] <= 0 || b.c[3] <= 0) continue;
            emit(a);
            emit(b);
        }
        return true;
    default:
        break;
    }
    *outType = D3DPT_TRIANGLELIST;
    for (DWORD k = 0; k + 2 < count; k += t == D3DPT_TRIANGLELIST ? 3 : 1) {
        DWORD i0, i1, i2;
        if (t == D3DPT_TRIANGLEFAN) {
            i0 = vertexAt(0), i1 = vertexAt(k + 1), i2 = vertexAt(k + 2);
        } else if (t == D3DPT_TRIANGLESTRIP && (k & 1)) {
            i0 = vertexAt(k + 1), i1 = vertexAt(k), i2 = vertexAt(k + 2);  // the same winding as the strip's
        } else {
            i0 = vertexAt(k), i1 = vertexAt(k + 1), i2 = vertexAt(k + 2);
        }
        std::vector<ClipVert> poly = {load(i0), load(i1), load(i2)};
        if (poly[0].c[2] < 0 || poly[1].c[2] < 0 || poly[2].c[2] < 0) poly = clipPoly(std::move(poly));
        for (size_t j = 1; j + 1 < poly.size(); j++) {
            if (poly[0].c[3] <= 0 || poly[j].c[3] <= 0 || poly[j + 1].c[3] <= 0) continue;
            emit(poly[0]);
            emit(poly[j]);
            emit(poly[j + 1]);
        }
    }
    return true;
}

HRESULT Device7::DrawPrimitiveVB(D3DPRIMITIVETYPE t, IDirect3DVertexBuffer7* p, DWORD start, DWORD n, DWORD) {
    if (!p) return DDERR_INVALIDPARAMS;
    Guard l;
    VertexBuffer7* vb = VB(p);
    // Not past the buffer's end (XP reads beyond it).
    if (start >= vb->count_) return D3D_OK;
    n = std::min(n, vb->count_ - start);
    const bool transformed = (vb->fvf_ & 0x0E) == D3DFVF_XYZRHW;
    const bool clipped = rs_[136] == TRUE && (kind_ != kTnLHAL || vb->hasClipCodes_);
    if ((transformed || kind_ == kTnLHAL) && !clipped) HoldVB(vb);
    if (transformed && clipped && vb->hasClipCodes_ && vb->clip_.size() >= size_t(start) + n && ValidPrim(t)) {
        std::vector<uint8_t> v;
        D3DPRIMITIVETYPE ot;
        if (ExpandClipped(t, vb->fvf_ & ~DWORD(0xF001), vb->mem_.data() + size_t(start) * vb->stride_, vb->stride_,
                          vb->clip_.data() + start, n, nullptr, 0, vp_, &v, &ot))
            return v.empty() ? D3D_OK : DrawCommon(ot, vb->fvf_, v.data(), vb->stride_, DWORD(v.size() / vb->stride_), nullptr, 0);
    }
    return DrawCommon(t, vb->fvf_, vb->mem_.data() + size_t(start) * vb->stride_, vb->stride_, n, nullptr, 0);
}

HRESULT Device7::DrawIndexedPrimitiveVB(D3DPRIMITIVETYPE t, IDirect3DVertexBuffer7* p, DWORD start, DWORD n,
                                        WORD* idx, DWORD ni, DWORD) {
    if (!p || (!idx && ni)) return DDERR_INVALIDPARAMS;
    Guard l;
    VertexBuffer7* vb = VB(p);
    if (start >= vb->count_) return D3D_OK;
    n = std::min(n, vb->count_ - start);
    const bool transformed = (vb->fvf_ & 0x0E) == D3DFVF_XYZRHW;
    const bool clipped = rs_[136] == TRUE && (kind_ != kTnLHAL || vb->hasClipCodes_);
    if ((transformed || kind_ == kTnLHAL) && !clipped) HoldVB(vb);
    // Indices relative to the start vertex; none may reach past the vertices given.
    std::vector<WORD> safe;
    const WORD* use = idx;
    for (DWORD i = 0; i < ni; i++) {
        if (idx[i] >= n) {
            safe.assign(idx, idx + ni);
            for (WORD& k : safe)
                if (k >= n) k = 0;
            use = safe.data();
            break;
        }
    }
    if (transformed && clipped && vb->hasClipCodes_ && vb->clip_.size() >= size_t(start) + n && ValidPrim(t)) {
        std::vector<uint8_t> v;
        D3DPRIMITIVETYPE ot;
        if (ExpandClipped(t, vb->fvf_ & ~DWORD(0xF001), vb->mem_.data() + size_t(start) * vb->stride_, vb->stride_,
                          vb->clip_.data() + start, n, use, ni, vp_, &v, &ot))
            return v.empty() ? D3D_OK : DrawCommon(ot, vb->fvf_, v.data(), vb->stride_, DWORD(v.size() / vb->stride_), nullptr, 0);
    }
    return DrawCommon(t, vb->fvf_, vb->mem_.data() + size_t(start) * vb->stride_, vb->stride_, n, use, ni);
}

// ComputeSphereVisibility: the box test of S §ComputeSphereVisibility, on world x view x
// projection with the viewport's 0..w clip volume.
HRESULT Device7::ComputeSphereVisibility(D3DVECTOR* centers, D3DVALUE* radii, DWORD n, DWORD, DWORD* ret) {
    if (!centers || !radii || !ret) return DDERR_INVALIDPARAMS;
    Guard l;
    D3DMATRIX wv, wvp;
    MatMul(&wv, world_, view_);
    MatMul(&wvp, wv, proj_);
    const DWORD planesOn = rs_[152];
    for (DWORD s = 0; s < n; s++) {
        DWORD uni = 0, inter = 0xFFF;
        for (int k = 0; k < 8; k++) {
            const float x = centers[s].x + ((k & 1) ? radii[s] : -radii[s]);
            const float y = centers[s].y + ((k & 2) ? radii[s] : -radii[s]);
            const float z = centers[s].z + ((k & 4) ? radii[s] : -radii[s]);
            const float cx = x * wvp._11 + y * wvp._21 + z * wvp._31 + wvp._41;
            const float cy = x * wvp._12 + y * wvp._22 + z * wvp._32 + wvp._42;
            const float cz = x * wvp._13 + y * wvp._23 + z * wvp._33 + wvp._43;
            const float cw = x * wvp._14 + y * wvp._24 + z * wvp._34 + wvp._44;
            const Vec3 wx = XformPoint({x, y, z}, world_);  // user clip planes are in world space
            // D3D's -w..w x/y volume as the front end's 0..w one.
            const float fx = (cx + cw) * 0.5f, fy = (cy + cw) * 0.5f;
            DWORD code = 0;
            if (fx < 0) code |= 0x01;
            if (fx > cw) code |= 0x02;
            if (fy > cw) code |= 0x04;
            if (fy < 0) code |= 0x08;
            if (cz < 0) code |= 0x10;
            if (cz > cw) code |= 0x20;
            for (DWORD p = 0; p < 6; p++)
                if ((planesOn & (1u << p)) && planes_[p][0] * wx.x + planes_[p][1] * wx.y + planes_[p][2] * wx.z + planes_[p][3] < 0)
                    code |= 0x40u << p;
            uni |= code;
            inter &= code;
        }
        ret[s] = uni | (inter << 12);
    }
    return D3D_OK;
}

// The bump map formats a hardware device lists where the GPU has them with the bump-mapping
// operations (DirectX 6 and 7: environment-mapped bump mapping).
static void AddBumpFormats(std::vector<DDPIXELFORMAT>* list) {
    const GpuCaps c = GpuGetCaps();
    auto bump = [&](bool have, DWORD bits, DWORD du, DWORD dv, DWORD lum) {
        if (!have) return;
        DDPIXELFORMAT f = {};
        f.dwSize = sizeof(f);
        f.dwFlags = DDPF_BUMPDUDV | (lum ? DDPF_BUMPLUMINANCE : 0);
        f.dwBumpBitCount = bits;
        f.dwBumpDuBitMask = du;
        f.dwBumpDvBitMask = dv;
        f.dwBumpLuminanceBitMask = lum;
        list->push_back(f);
    };
    bump(c.bumpV8U8, 16, 0xFF, 0xFF00, 0);
    bump(c.bumpL6V5U5, 16, 0x1F, 0x3E0, 0xFC00);
    bump(c.bumpX8L8V8U8, 24, 0xFF, 0xFF00, 0xFF0000);
}

HRESULT Device7::EnumTextureFormats(LPD3DENUMPIXELFORMATSCALLBACK cb, void* ctx) {
    if (!cb || IsBadCodePtr(reinterpret_cast<FARPROC>(reinterpret_cast<void*>(cb)))) return DDERR_INVALIDPARAMS;
    std::vector<DDPIXELFORMAT> list;
    auto rgb = [&](DWORD bpp, DWORD r, DWORD g, DWORD b, DWORD a) {
        DDPIXELFORMAT f = {};
        f.dwSize = sizeof(f);
        f.dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
        f.dwRGBBitCount = bpp;
        f.dwRBitMask = r;
        f.dwGBitMask = g;
        f.dwBBitMask = b;
        f.dwRGBAlphaBitMask = a;
        list.push_back(f);
    };
    rgb(16, 0x7C00, 0x03E0, 0x001F, 0);
    rgb(16, 0x7C00, 0x03E0, 0x001F, 0x8000);
    rgb(16, 0x0F00, 0x00F0, 0x000F, 0xF000);
    rgb(16, 0xF800, 0x07E0, 0x001F, 0);
    rgb(32, 0xFF0000, 0xFF00, 0xFF, 0);
    rgb(32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000);
    // Palettized: converted through the palette on the way to the GPU. Not offered with
    // palettizedtextures=off, as on a card without them (Matrox's G200 and G400, today's
    // drivers): N.I.C.E. 2 loads its 256-colour textures when it finds this format listed,
    // its 16-bit ones otherwise. (Still made if a game asks for one anyway.)
    if (g_config.palTextures) {
        DDPIXELFORMAT f = {};
        f.dwSize = sizeof(f);
        f.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        f.dwRGBBitCount = 8;
        list.push_back(f);
    }
    if (kind_ != kRGB) {
        const GpuCaps c = GpuGetCaps();
        for (int i = 0; i < 5; i++) {
            if (!c.dxt[i]) continue;
            DDPIXELFORMAT f = {};
            f.dwSize = sizeof(f);
            f.dwFlags = DDPF_FOURCC;
            f.dwFourCC = MAKEFOURCC('D', 'X', 'T', '1' + i);
            list.push_back(f);
        }
        AddBumpFormats(&list);
    }
    for (DDPIXELFORMAT& f : list)
        if (cb(&f, ctx) != D3DENUMRET_OK) break;
    return D3D_OK;
}

// No driver to ask; XP returns E_NOTIMPL then, which games take for failure: one pass
// (a deliberate difference, S §For dd2d9 17).
HRESULT Device7::ValidateDevice(DWORD* passes) {
    if (!passes) return DDERR_INVALIDPARAMS;
    *passes = 1;
    return D3D_OK;
}

// ---- state blocks

DWORD Device7::NewHandle(StateBlock* b) {
    if (!freeHandles_.empty()) {
        const DWORD h = freeHandles_.back();
        freeHandles_.pop_back();
        blocks_[h] = b;
        return h;
    }
    blocks_.push_back(b);
    return DWORD(blocks_.size() - 1);
}

HRESULT Device7::BeginStateBlock() {
    Guard l;
    if (Recording()) return D3DERR_INBEGINSTATEBLOCK;
    auto* b = new (std::nothrow) StateBlock;
    if (!b) return E_OUTOFMEMORY;
    recording_ = NewHandle(b);
    return D3D_OK;
}

HRESULT Device7::EndStateBlock(DWORD* handle) {
    if (!handle) return DDERR_INVALIDPARAMS;
    Guard l;
    if (!Recording()) return D3DERR_NOTINBEGINSTATEBLOCK;
    Rec()->done = true;
    *handle = recording_;
    recording_ = 0;
    return D3D_OK;
}

HRESULT Device7::Replay(const StateBlock* b) {
    for (const Cmd& c : b->cmds) {
        switch (c.kind) {
        case Cmd::kRS: SetRS(c.a, c.b); break;
        case Cmd::kTSS: SetTSS(c.a, c.b, c.c); break;
        case Cmd::kTex: {
            // XP keeps no reference: a texture released since would be a dangling pointer.
            Surface* s = c.tex;
            if (s && std::find(dd_->surfaces.begin(), dd_->surfaces.end(), s) == dd_->surfaces.end()) {
                bool member = false;
                for (Surface* r : dd_->surfaces)
                    if (std::find(r->children.begin(), r->children.end(), s) != r->children.end()) member = true;
                if (!member) s = nullptr;
            }
            SetTex(c.a, s);
            break;
        }
        case Cmd::kXform: SetXform(c.a, c.m); break;
        case Cmd::kViewport:
            if (HRESULT hr = SetVP(c.vp); FAILED(hr)) return hr;
            break;
        case Cmd::kMaterial: SetMat(c.mat); break;
        case Cmd::kLight: SetLightI(c.a, c.light); break;
        case Cmd::kLightEnable: LightEnableI(c.a, c.b != 0); break;
        case Cmd::kClipPlane: SetPlane(c.a, c.plane); break;
        }
    }
    return D3D_OK;
}

HRESULT Device7::ApplyStateBlock(DWORD h) {
    Guard l;
    if (Recording()) return D3DERR_INBEGINSTATEBLOCK;
    if (h >= blocks_.size() || !blocks_[h] || !blocks_[h]->done) return D3D_OK;  // XP applies nothing (or reads past)
    return Replay(blocks_[h]);
}

void Device7::Capture(StateBlock* b) {
    for (Cmd& c : b->cmds) {
        switch (c.kind) {
        case Cmd::kRS: if (c.a < 256) c.b = rs_[c.a]; break;
        case Cmd::kTSS: if (c.a < 8 && c.b < 64) c.c = tss_[c.a][c.b]; break;
        case Cmd::kTex: c.tex = c.a < 8 ? tex_[c.a] : nullptr; break;
        case Cmd::kXform: if (const D3DMATRIX* m = Matrix(c.a)) c.m = *m; break;
        case Cmd::kViewport: c.vp = vp_; break;
        case Cmd::kMaterial: c.mat = mat_; break;
        case Cmd::kLight: if (c.a < lights_.size()) c.light = lights_[c.a].l; break;
        case Cmd::kLightEnable: if (c.a < lights_.size()) c.b = lights_[c.a].enabled; break;
        case Cmd::kClipPlane: if (c.a < kMaxPlanes) memcpy(c.plane, planes_[c.a], sizeof(c.plane)); break;
        }
    }
}

HRESULT Device7::CaptureStateBlock(DWORD h) {
    Guard l;
    if (Recording()) return D3DERR_INBEGINSTATEBLOCK;
    if (h >= blocks_.size() || !blocks_[h] || !blocks_[h]->done) return D3DERR_INVALIDSTATEBLOCK;
    for (const Cmd& c : blocks_[h]->cmds)
        if ((c.kind == Cmd::kLight || c.kind == Cmd::kLightEnable) && c.a >= lights_.size()) return D3DERR_LIGHT_SET_FAILED;
    Capture(blocks_[h]);
    return D3D_OK;
}

HRESULT Device7::DeleteStateBlock(DWORD h) {
    Guard l;
    if (Recording()) return D3DERR_INBEGINSTATEBLOCK;
    if (h == 0 || h >= blocks_.size() || !blocks_[h] || !blocks_[h]->done) return D3DERR_INVALIDSTATEBLOCK;
    delete blocks_[h];
    blocks_[h] = nullptr;
    freeHandles_.push_back(h);
    return D3D_OK;
}

// The predefined blocks (S §State blocks, CreatePredefined).
HRESULT Device7::CreateStateBlock(D3DSTATEBLOCKTYPE type, DWORD* handle) {
    if (!handle) return DDERR_INVALIDPARAMS;
    Guard l;
    if (Recording()) return D3DERR_INBEGINSTATEBLOCK;
    static const DWORD kAll[] = {4, 2, 29, 7, 8, 9, 10, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 28, 33, 34, 35, 36, 37,
                                 38, 40, 41, 27, 47, 48, 52, 53, 54, 55, 56, 57, 58, 59, 60, 128, 129, 130, 131, 132,
                                 133, 134, 135, 139, 141, 140, 136, 137, 138, 143, 142, 148, 147, 145, 146, 151, 152, 144};
    static const DWORD kPixel[] = {4, 2, 7, 8, 9, 10, 14, 15, 16, 19, 20, 23, 24, 25, 26, 33, 36, 37, 38, 40, 41, 27,
                                   47, 52, 53, 54, 55, 56, 57, 58, 59, 60, 128, 129, 130, 131, 132, 133, 134, 135, 144};
    static const DWORD kVertex[] = {9, 29, 22, 28, 34, 35, 36, 37, 38, 48, 139, 141, 140, 136, 137, 138, 143, 142, 148,
                                    147, 145, 146, 151, 152};
    if (type < 0 || type > D3DSBT_VERTEXSTATE) {
        *handle = 0xFFFFFFFF;
        return DDERR_INVALIDPARAMS;
    }
    auto* b = new (std::nothrow) StateBlock;
    if (!b) return E_OUTOFMEMORY;
    auto rs = [&](const DWORD* list, size_t n) {
        for (size_t i = 0; i < n; i++) {
            Cmd c;
            c.kind = Cmd::kRS;
            c.a = list[i];
            b->cmds.push_back(c);
        }
    };
    auto tss = [&](DWORD stage, DWORD t) {
        Cmd c;
        c.kind = Cmd::kTSS;
        c.a = stage;
        c.b = t;
        b->cmds.push_back(c);
    };
    auto simple = [&](Cmd::Kind k, DWORD a) {
        Cmd c;
        c.kind = k;
        c.a = a;
        b->cmds.push_back(c);
    };
    const DWORD stages = std::min<DWORD>(caps_.wMaxTextureBlendStages, 8);
    if (type == D3DSBT_ALL) {
        rs(kAll, std::size(kAll));
        for (DWORD s = 0; s < stages; s++)
            for (DWORD t = 1; t <= 24; t++) tss(s, t);
        for (DWORD s = 0; s < stages; s++) simple(Cmd::kTex, s);
        simple(Cmd::kViewport, 0);
        for (DWORD t : {1u, 2u, 3u, 4u, 5u, 6u}) simple(Cmd::kXform, t);
        for (DWORD s = 0; s < stages; s++) simple(Cmd::kXform, 16 + s);
        for (DWORD p = 0; p < std::min<DWORD>(caps_.wMaxUserClipPlanes, kMaxPlanes); p++) simple(Cmd::kClipPlane, p);
        simple(Cmd::kMaterial, 0);
        for (DWORD i = 0; i < lights_.size(); i++)
            if (lights_[i].set) {
                simple(Cmd::kLight, i);
                simple(Cmd::kLightEnable, i);
            }
    } else if (type == D3DSBT_PIXELSTATE) {
        rs(kPixel, std::size(kPixel));
        for (DWORD s = 0; s < 8; s++)
            for (DWORD t = 1; t <= 24; t++) tss(s, t);
    } else if (type == D3DSBT_VERTEXSTATE) {
        rs(kVertex, std::size(kVertex));
        for (DWORD s = 0; s < 8; s++) {
            tss(s, D3DTSS_TEXCOORDINDEX);
            tss(s, D3DTSS_TEXTURETRANSFORMFLAGS);
        }
        for (DWORD i = 0; i < lights_.size(); i++)
            if (lights_[i].set) simple(Cmd::kLightEnable, i);
    }
    Capture(b);
    b->done = true;
    *handle = NewHandle(b);
    return D3D_OK;
}

// ---- Load (S §Load)

HRESULT Device7::Load(IDirectDrawSurface7* dstI, POINT* pt, IDirectDrawSurface7* srcI, RECT* rect, DWORD flags) {
    Guard l;
    Surface* dst = Surf(dstI);
    Surface* src = Surf(srcI);
    if (!dst || !src) return DDERR_INVALIDPARAMS;  // XP faults
    const LONG sw = src->W(), sh = src->H();
    auto valid = [&](const RECT& r) {
        return r.left >= 0 && r.left < sw && r.top >= 0 && r.top < sh && r.right >= 1 && r.right <= sw &&
               r.bottom >= 1 && r.bottom <= sh && r.right > r.left && r.bottom > r.top;
    };
    RECT sr, dr;
    if (!pt) {
        sr = rect ? *rect : RECT{0, 0, sw, sh};
        if (!valid(sr)) return DDERR_INVALIDPARAMS;
        dr = sr;
    } else {
        if (!rect || !valid(*rect)) return DDERR_INVALIDPARAMS;
        sr = *rect;
        dr = {pt->x, pt->y, pt->x + (sr.right - sr.left), pt->y + (sr.bottom - sr.top)};
        if (!valid(dr)) return DDERR_INVALIDPARAMS;  // against the source's size, as XP
    }
    if ((dst->desc.ddsCaps.dwCaps2 & 0xFC00) != (src->desc.ddsCaps.dwCaps2 & 0xFC00)) return DDERR_INVALIDPARAMS;
    if (dst->palette && src->palette) {
        if ((dst->palette->caps & 0x7) != (src->palette->caps & 0x7)) return DDERR_INVALIDPARAMS;
        memcpy(dst->palette->entries, src->palette->entries, sizeof(dst->palette->entries));
        dst->palette->version++;
    }
    for (int k : {kCkDestBlt, kCkSrcBlt}) {
        dst->hasCk[k] = src->hasCk[k];
        dst->ck[k] = src->ck[k];
    }
    // Mip levels: the source level as large as the destination's top, then level by level.
    // At each smaller level the source rectangle rounds outward (left and top down, right
    // and bottom up) and the destination point halves, the destination rectangle the
    // source's size from there (Wine's test_device_load on Windows), also over the source
    // levels skipped to reach the destination's size.
    auto half = [](RECT r) { return RECT{r.left / 2, r.top / 2, (r.right + 1) / 2, (r.bottom + 1) / 2}; };
    auto loadChain = [&](Surface* dstTop, Surface* srcTop) -> HRESULT {
        RECT r = sr;
        LONG px = dr.left, py = dr.top;
        Surface* s = srcTop;
        while (s && s->W() > dstTop->W()) {
            s = s->NextMipLevel();
            r = half(r);
            px /= 2;
            py /= 2;
        }
        if (!s || s->W() != dstTop->W() || s->H() != dstTop->H()) return DDERR_INVALIDPARAMS;
        for (Surface* d = dstTop; d; d = d->NextMipLevel(), s = s ? s->NextMipLevel() : nullptr) {
            if (!s) return DDERR_INVALIDPARAMS;
            if (d->lockCount || s->lockCount) return DDERR_SURFACEBUSY;
            // Kept on both levels (the rounding can reach past a 1-pixel edge).
            RECT a = r, b = {px, py, px + (r.right - r.left), py + (r.bottom - r.top)};
            const LONG cutR = std::max<LONG>(0, std::max(b.right - d->W(), a.right - s->W()));
            const LONG cutB = std::max<LONG>(0, std::max(b.bottom - d->H(), a.bottom - s->H()));
            a.right -= cutR, b.right -= cutR, a.bottom -= cutB, b.bottom -= cutB;
            if (a.right > a.left && a.bottom > a.top) {
                const HRESULT hr = d->BltLocked(&b, s, &a, 0, nullptr, false);
                if (hr == DDERR_SURFACELOST && inScene_) {
                    lostInScene_ = true;
                    return S_FALSE;  // (stop: D3D_OK)
                }
                if (FAILED(hr)) return hr;
                d->BumpUniqueness();
            }
            r = half(r);
            px /= 2;
            py /= 2;
        }
        return D3D_OK;
    };
    // A cube map: the faces `flags` names (DDSCAPS2_CUBEMAP_* bits; none copies none), each
    // with its mip levels.
    if (dst->desc.ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP) {
        static const DWORD kFaces[6] = {DDSCAPS2_CUBEMAP_POSITIVEX, DDSCAPS2_CUBEMAP_NEGATIVEX, DDSCAPS2_CUBEMAP_POSITIVEY,
                                        DDSCAPS2_CUBEMAP_NEGATIVEY, DDSCAPS2_CUBEMAP_POSITIVEZ, DDSCAPS2_CUBEMAP_NEGATIVEZ};
        Surface* const dRoot = dst->root ? dst->root : dst;
        Surface* const sRoot = src->root ? src->root : src;
        for (DWORD face : kFaces) {
            if (!(flags & face)) continue;
            Surface* df = CubeFace(dRoot, face);
            Surface* sf = CubeFace(sRoot, face);
            if (!df || !sf) return DDERR_INVALIDPARAMS;
            const HRESULT hr = loadChain(df, sf);
            if (hr == S_FALSE) return D3D_OK;
            if (FAILED(hr)) return hr;
        }
        return D3D_OK;
    }
    const HRESULT hr = loadChain(dst, src);
    return hr == S_FALSE ? D3D_OK : hr;
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

}  // namespace

// ---------------------------------------------------------------- ProcessVertices (V §8.5)

// Does the processing of V §8.5 on the CPU: transform (always), clip codes (D3DVOP_CLIP),
// lighting (D3DVOP_LIGHT, whatever D3DRENDERSTATE_LIGHTING says), extents (D3DVOP_EXTENTS).
// The normal matrix: the transpose of m's inverse (all of it, 4x4), its upper 3x3 used; all
// zero when m has no inverse.
static D3DMATRIX NormalMatrix(const D3DMATRIX& m) {
    const float* a = &m._11;
    float inv[16];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    D3DMATRIX r = {};
    if (det == 0.0f || !std::isfinite(det)) return r;
    const float id = 1.0f / det;
    float* o = &r._11;
    for (int i = 0; i < 4; i++)  // transposed: row i of the result = column i of the inverse
        for (int j = 0; j < 4; j++) o[i * 4 + j] = inv[j * 4 + i] * id;
    return r;
}

static HRESULT Process(VertexBuffer7* dstVb, DWORD op, DWORD dstIndex, DWORD count, const uint8_t* src, UINT srcStride,
                       DWORD srcFvf, Device7* dev, DWORD flags, const uint8_t* srcEnd) {
    const Layout in(srcFvf), out(dstVb->fvf_);
    // V §8.5 step 6: lit with specular or fog needs a destination specular; the destination
    // has at least the source's texture coordinates, with their size bits.
    if ((op & D3DVOP_LIGHT) && (dev->rs_[D3DRENDERSTATE_SPECULARENABLE] || dev->rs_[D3DRENDERSTATE_FOGENABLE]) &&
        out.specular < 0)
        return D3DERR_INVALIDVERTEXFORMAT;
    if (out.texCount < in.texCount) return D3DERR_INVALIDVERTEXFORMAT;
    {
        const DWORD used = (1u << (2 * in.texCount)) - 1;  // (at most 8 sets)
        const DWORD srcBits = (srcFvf >> 16) & used, dstBits = (dstVb->fvf_ >> 16) & used;
        if ((dstBits & srcBits) != srcBits) return D3DERR_INVALIDVERTEXFORMAT;
    }
    if (dstIndex >= dstVb->count_) return D3D_OK;
    count = std::min(count, dstVb->count_ - dstIndex);
    const bool dstRhw = (dstVb->fvf_ & 0x0E) == D3DFVF_XYZRHW;
    D3DMATRIX wv, wvp, vp4;
    MatMul(&wv, dev->world_, dev->view_);
    MatMul(&wvp, wv, dev->proj_);
    MatMul(&vp4, dev->view_, dev->proj_);
    // Weights read: VERTEXBLEND's count (at most 3), the floats after the position whatever
    // the FVF says is there (Wine's test_process_vertices on Windows: an FVF without
    // weights blends with its next values as weights); past the source's end, 0.
    const DWORD blend = std::min<DWORD>(dev->rs_[D3DRENDERSTATE_VERTEXBLEND], 3);
    // Normals through the transpose of the inverse of the whole 4x4 world x view, its upper
    // 3x3 (right under non-uniform scale): XP's (Wine's test_lighting on Windows: a
    // non-affine world turns the normal to zero, a singular one, with no inverse, too:
    // both light black). With vertex blending, one per world matrix (below).
    const D3DMATRIX* worlds[4] = {&dev->world_, &dev->world1_, &dev->world2_, &dev->world3_};
    D3DMATRIX nms[4];
    for (DWORD k = 0; k <= blend; k++) {
        D3DMATRIX m;
        if (blend) MatMul(&m, *worlds[k], dev->view_);
        else m = wv;
        nms[k] = NormalMatrix(m);
    }
    const D3DVIEWPORT7& vp = dev->vp_;
    const bool clip = (op & D3DVOP_CLIP) != 0, light = (op & D3DVOP_LIGHT) != 0;
    const bool extents = (op & D3DVOP_EXTENTS) != 0, noCopy = (flags & D3DPV_DONOTCOPYDATA) != 0;
    // Lights in camera space.
    struct L {
        const D3DLIGHT7* l;
        Vec3 pos, dir;
        float cosTheta, cosPhi;  // a spot light's cones' half angles
    };
    std::vector<L> lights;
    if (light)
        for (const Device7::Light& d : dev->lights_)
            if (d.set && d.enabled)
                lights.push_back({&d.l, XformPoint({d.l.dvPosition.x, d.l.dvPosition.y, d.l.dvPosition.z}, dev->view_),
                                  Norm(XformDir({d.l.dvDirection.x, d.l.dvDirection.y, d.l.dvDirection.z}, dev->view_)),
                                  cosf(d.l.dvTheta * 0.5f), cosf(d.l.dvPhi * 0.5f)});
    const Color ambient = FromArgb(dev->rs_[D3DRENDERSTATE_AMBIENT]);
    DWORD orCodes = 0, andCodes = 0xFFFF;
    for (DWORD i = 0; i < count; i++) {
        const uint8_t* v = src + size_t(i) * srcStride;
        uint8_t* o = dstVb->mem_.data() + size_t(dstIndex + i) * dstVb->stride_;
        const float* p = reinterpret_cast<const float*>(v + in.pos);
        Vec3 pos = {p[0], p[1], p[2]};
        float cx, cy, cz, cw;
        float weights[3] = {0, 0, 0};
        for (DWORD k = 0; k < blend; k++)
            if (reinterpret_cast<const uint8_t*>(p + 3 + k + 1) <= srcEnd) weights[k] = p[3 + k];
        if (blend) {
            // Vertex blending (V §9): the weights after the position, the last matrix
            // weighted 1 - their sum; the matrices' whole (4-component) results blended,
            // then view and projection (test_process_vertices: a world with a w column).
            float acc[4] = {0, 0, 0, 0};
            float sum = 0;
            for (DWORD k = 0; k <= blend; k++) {
                const float w = k < blend ? weights[k] : 1.0f - sum;
                sum += k < blend ? w : 0;
                const D3DMATRIX& M = *worlds[k];
                acc[0] += (pos.x * M._11 + pos.y * M._21 + pos.z * M._31 + M._41) * w;
                acc[1] += (pos.x * M._12 + pos.y * M._22 + pos.z * M._32 + M._42) * w;
                acc[2] += (pos.x * M._13 + pos.y * M._23 + pos.z * M._33 + M._43) * w;
                acc[3] += (pos.x * M._14 + pos.y * M._24 + pos.z * M._34 + M._44) * w;
            }
            const D3DMATRIX& V = vp4;  // view x projection
            cx = acc[0] * V._11 + acc[1] * V._21 + acc[2] * V._31 + acc[3] * V._41;
            cy = acc[0] * V._12 + acc[1] * V._22 + acc[2] * V._32 + acc[3] * V._42;
            cz = acc[0] * V._13 + acc[1] * V._23 + acc[2] * V._33 + acc[3] * V._43;
            cw = acc[0] * V._14 + acc[1] * V._24 + acc[2] * V._34 + acc[3] * V._44;
            // Lighting below works in world space through view only (the blended point).
            const float aw = acc[3] != 0.0f ? 1.0f / acc[3] : 1.0f;
            pos = {acc[0] * aw, acc[1] * aw, acc[2] * aw};
        } else {
            cx = pos.x * wvp._11 + pos.y * wvp._21 + pos.z * wvp._31 + wvp._41;
            cy = pos.x * wvp._12 + pos.y * wvp._22 + pos.z * wvp._32 + wvp._42;
            cz = pos.x * wvp._13 + pos.y * wvp._23 + pos.z * wvp._33 + wvp._43;
            cw = pos.x * wvp._14 + pos.y * wvp._24 + pos.z * wvp._34 + wvp._44;
        }
        // The front end's 0..w clip volume (V §7.3).
        const float fx = (cx + cw) * 0.5f, fy = (cy + cw) * 0.5f;
        DWORD code = 0;
        if (clip) {
            if (fx < 0) code |= 0x01;
            if (fx > cw) code |= 0x02;
            if (fy > cw) code |= 0x04;
            if (fy < 0) code |= 0x08;
            if (cz < 0) code |= 0x10;
            if (cz > cw) code |= 0x20;
            orCodes |= code;
            andCodes &= code;
            // 0x8000 (no D3DCLIP_ flag): the vertex is stored in clip space (below).
            if (!dstVb->clip_.empty()) dstVb->clip_[dstIndex + i] = WORD(code | (code ? kInClipSpace : 0));
        }
        float* po = reinterpret_cast<float*>(o);
        if (code) {  // outside: clip space (V §8.5)
            po[0] = fx;
            po[1] = fy;
            po[2] = cz;
            if (dstRhw) po[3] = cw;
        } else {
            const float rhw = cw != 0 ? 1.0f / cw : 0.0f;
            const float sx = cx * rhw * (vp.dwWidth * 0.5f) + vp.dwX + vp.dwWidth * 0.5f;
            const float sy = -cy * rhw * (vp.dwHeight * 0.5f) + vp.dwY + vp.dwHeight * 0.5f;
            po[0] = sx;
            po[1] = sy;
            po[2] = cz * rhw * (vp.dvMaxZ - vp.dvMinZ) + vp.dvMinZ;
            // Without an rhw slot XP writes it anyway, into the next vertex (Wine's test).
            if (dstRhw || o + 16 <= dstVb->mem_.data() + dstVb->mem_.size()) po[3] = rhw;
            if (extents) {
                dev->cs_.minx = std::min(dev->cs_.minx, sx);
                dev->cs_.maxx = std::max(dev->cs_.maxx, sx);
                dev->cs_.miny = std::min(dev->cs_.miny, sy);
                dev->cs_.maxy = std::max(dev->cs_.maxy, sy);
            }
        }
        // Colours.
        const DWORD srcDiffuse = in.diffuse >= 0 ? *reinterpret_cast<const DWORD*>(v + in.diffuse) : 0xFFFFFFFF;
        const DWORD srcSpecular = in.specular >= 0 ? *reinterpret_cast<const DWORD*>(v + in.specular) : 0;
        DWORD diffuse = srcDiffuse, specular = srcSpecular;
        if (light) {
            const D3DMATERIAL7& m = dev->mat_;
            const bool cv = dev->rs_[D3DRENDERSTATE_COLORVERTEX] == TRUE;
            auto source = [&](DWORD state, const D3DCOLORVALUE& mc) {
                const DWORD s = dev->rs_[state];
                if (cv && s == D3DMCS_COLOR1 && in.diffuse >= 0) return FromArgb(srcDiffuse);
                if (cv && s == D3DMCS_COLOR2 && in.specular >= 0) return FromArgb(srcSpecular);
                return FromCv(mc);
            };
            const Color md = source(D3DRENDERSTATE_DIFFUSEMATERIALSOURCE, m.dcvDiffuse);
            const Color ma = source(D3DRENDERSTATE_AMBIENTMATERIALSOURCE, m.dcvAmbient);
            const Color ms = source(D3DRENDERSTATE_SPECULARMATERIALSOURCE, m.dcvSpecular);
            const Color me = source(D3DRENDERSTATE_EMISSIVEMATERIALSOURCE, m.dcvEmissive);
            const Vec3 P = XformPoint(pos, blend ? dev->view_ : wv);
            // No normal: lit with a zero one (ambient and emissive only), as XP does.
            const float* n = in.normal >= 0 ? reinterpret_cast<const float*>(v + in.normal) : nullptr;
            // Normalized only with NORMALIZENORMALS: a world or view that shrinks makes the
            // normal longer and the light stronger, saturating (Wine's
            // test_fog_process_vertices on Windows: z scaled by 0.375 gives full specular
            // where a unit normal gives 57%). Blended: the matrices' normals weighted, the sum
            // as it comes (test_lighting: weights 0.2 and 0.8 of normals at right angles
            // light 0.8).
            const bool normalize = dev->rs_[D3DRENDERSTATE_NORMALIZENORMALS] == TRUE;
            auto xn = [&](const D3DMATRIX& nm) {
                const Vec3 t = XformDir({n[0], n[1], n[2]}, nm);
                return normalize ? Norm(t) : t;
            };
            Vec3 N = {0, 0, 0};
            if (n && !blend) {
                N = xn(nms[0]);
            } else if (n) {
                float sum = 0;
                for (DWORD k = 0; k <= blend; k++) {
                    const float w = k < blend ? weights[k] : 1.0f - sum;
                    sum += k < blend ? w : 0;
                    const Vec3 t = xn(nms[k]);
                    N = {N.x + t.x * w, N.y + t.y * w, N.z + t.z * w};
                }
            }
            Color d = {ambient.r * ma.r + me.r, ambient.g * ma.g + me.g, ambient.b * ma.b + me.b, md.a};
            Color s = {0, 0, 0, 0};
            const bool local = dev->rs_[D3DRENDERSTATE_LOCALVIEWER] == TRUE;
            for (const L& l : lights) {
                Vec3 Ld;
                float att = 1.0f;
                if (l.l->dltType == D3DLIGHT_DIRECTIONAL) {
                    Ld = {-l.dir.x, -l.dir.y, -l.dir.z};
                } else {
                    const Vec3 toL = Sub(l.pos, P);
                    const float dist = sqrtf(Dot(toL, toL));
                    if (dist > l.l->dvRange) continue;
                    Ld = dist > 0 ? Vec3{toL.x / dist, toL.y / dist, toL.z / dist} : Vec3{0, 0, 1};
                    const float a = l.l->dvAttenuation0 + l.l->dvAttenuation1 * dist + l.l->dvAttenuation2 * dist * dist;
                    att = a > 0 ? 1.0f / a : 1.0f;
                    if (l.l->dltType == D3DLIGHT_SPOT) {
                        const float rho = -Dot(Ld, l.dir);
                        const float ct = l.cosTheta, cp = l.cosPhi;
                        if (rho <= cp) continue;
                        if (rho < ct && ct > cp) att *= powf((rho - cp) / (ct - cp), l.l->dvFalloff);
                    }
                }
                const Color la = FromCv(l.l->dcvAmbient), ld = FromCv(l.l->dcvDiffuse), ls = FromCv(l.l->dcvSpecular);
                d.r += la.r * ma.r * att;
                d.g += la.g * ma.g * att;
                d.b += la.b * ma.b * att;
                const float nl = Dot(N, Ld);
                if (nl <= 0) continue;
                d.r += ld.r * md.r * nl * att;
                d.g += ld.g * md.g * nl * att;
                d.b += ld.b * md.b * nl * att;
                // Power 0: ProcessVertices keeps the last power above 0 (Wine's
                // test_specular_lighting on Windows; drawing takes 0 as it is).
                const float power = m.dvPower > 0 ? m.dvPower : dev->pvPower_;
                if (power > 0) {
                    const Vec3 eye = local ? Norm(Vec3{-P.x, -P.y, -P.z}) : Vec3{0, 0, -1};  // toward the viewer
                    const Vec3 H = Norm({Ld.x + eye.x, Ld.y + eye.y, Ld.z + eye.z});
                    const float nh = Dot(N, H);
                    if (nh > 0) {
                        const float k = powf(nh, power) * att;
                        s.r += ls.r * ms.r * k;
                        s.g += ls.g * ms.g * k;
                        s.b += ls.b * ms.b * k;
                    }
                }
            }
            diffuse = ToArgb(d);
            s.a = ms.a;  // the specular alpha: the material's (Wine's test on Windows), or the fog factor
            // Vertex fog (V §9): FOGENABLE and FOGVERTEXMODE, from eye z or the distance.
            const DWORD fogMode = dev->rs_[D3DRENDERSTATE_FOGVERTEXMODE];
            if (dev->rs_[D3DRENDERSTATE_FOGENABLE] == TRUE && fogMode != D3DFOG_NONE && fogMode != 0xFFFFFFFF) {
                const float dist = dev->rs_[D3DRENDERSTATE_RANGEFOGENABLE] == TRUE ? sqrtf(Dot(P, P)) : P.z;
                auto rsf = [&](DWORD st) {
                    float f;
                    memcpy(&f, &dev->rs_[st], 4);
                    return f;
                };
                float f = 1.0f;
                if (fogMode == D3DFOG_LINEAR) {
                    const float st = rsf(D3DRENDERSTATE_FOGSTART), en = rsf(D3DRENDERSTATE_FOGEND);
                    f = dist < st ? 1.0f : dist >= en ? 0.0f : (en - dist) / (en - st);
                } else if (fogMode == D3DFOG_EXP) {
                    f = expf(-dist * rsf(D3DRENDERSTATE_FOGDENSITY));
                } else if (fogMode == D3DFOG_EXP2) {
                    const float k = dist * rsf(D3DRENDERSTATE_FOGDENSITY);
                    f = expf(-k * k);
                }
                s.a = f;
            }
            specular = ToArgb(s);
        }
        // Vertex fog without lighting too (V §8.5 step 8): the factor in specular alpha.
        bool fogged = false;
        if (!light) {
            const DWORD fogMode = dev->rs_[D3DRENDERSTATE_FOGVERTEXMODE];
            if (dev->rs_[D3DRENDERSTATE_FOGENABLE] == TRUE && fogMode != D3DFOG_NONE && fogMode != 0xFFFFFFFF &&
                out.specular >= 0) {
                const Vec3 P = XformPoint(pos, blend ? dev->view_ : wv);
                const float dist = dev->rs_[D3DRENDERSTATE_RANGEFOGENABLE] == TRUE ? sqrtf(Dot(P, P)) : P.z;
                auto rsf = [&](DWORD st) {
                    float f;
                    memcpy(&f, &dev->rs_[st], 4);
                    return f;
                };
                float f = 1.0f;
                if (fogMode == D3DFOG_LINEAR) {
                    const float st = rsf(D3DRENDERSTATE_FOGSTART), en = rsf(D3DRENDERSTATE_FOGEND);
                    f = dist < st ? 1.0f : dist >= en ? 0.0f : (en - dist) / (en - st);
                } else if (fogMode == D3DFOG_EXP) {
                    f = expf(-dist * rsf(D3DRENDERSTATE_FOGDENSITY));
                } else if (fogMode == D3DFOG_EXP2) {
                    const float k = dist * rsf(D3DRENDERSTATE_FOGDENSITY);
                    f = expf(-k * k);
                }
                specular = (specular & 0x00FFFFFF) | (TruncToDword(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f) << 24);
                fogged = true;
            }
        }
        if (out.diffuse >= 0 && (light || !noCopy)) *reinterpret_cast<DWORD*>(o + out.diffuse) = diffuse;
        if (out.specular >= 0 && (light || fogged || !noCopy)) *reinterpret_cast<DWORD*>(o + out.specular) = specular;
        if (!noCopy)
            for (DWORD t = 0; t < in.texCount; t++)
                memcpy(o + out.tex[t], v + in.tex[t], std::min(in.texSize[t], out.texSize[t]));
    }
    if (clip) {
        if (count) {
            DWORD& st = dev->cs_.dwStatus;
            st = (st | (orCodes & 0xFFF)) & ((andCodes << 12) | 0xFF000FFF);
        }
        dstVb->hasClipCodes_ = true;
    } else if (dstVb->clip_.size() >= size_t(dstIndex) + count) {
        // Written in screen space now: not clipped (V §8.5 step 8), whatever an earlier
        // clipping ProcessVertices left there.
        std::fill_n(dstVb->clip_.begin() + dstIndex, count, WORD(0));
    }
    return D3D_OK;
}

HRESULT VertexBuffer7::ProcessVertices(DWORD op, DWORD dst, DWORD count, IDirect3DVertexBuffer7* src, DWORD srcIndex,
                                       IDirect3DDevice7* device, DWORD flags) {
    if (!device) return DDERR_INVALIDPARAMS;  // XP faults
    Guard l;
    if (optimized_) return D3DERR_VERTEXBUFFEROPTIMIZED;
    Device7* dev = static_cast<Device7*>(device);
    if (!src) {
        // Clip codes and extents of the destination's own transformed vertices (V §8.5 step 3).
        if ((fvf_ & 0x0E) != D3DFVF_XYZRHW || dst >= count_) return D3D_OK;
        count = std::min(count, count_ - dst);
        const D3DVIEWPORT7& vp = dev->vp_;
        DWORD orCodes = 0, andCodes = 0xFFFF;
        for (DWORD i = 0; i < count; i++) {
            const float* v = reinterpret_cast<const float*>(mem_.data() + size_t(dst + i) * stride_);
            float x = v[0], y = v[1], z = v[2];
            if (v[3] < 0) x = -x, y = -y, z = -z;
            DWORD code = 0;
            if (x < float(vp.dwX)) code |= 1;
            if (x >= float(vp.dwX + vp.dwWidth)) code |= 2;
            if (y < float(vp.dwY)) code |= 4;
            if (y >= float(vp.dwY + vp.dwHeight)) code |= 8;
            if (dev->rs_[D3DRENDERSTATE_ZENABLE]) {
                if (z < 0) code |= 0x10;
                if (z >= 1) code |= 0x20;
            }
            if ((op & D3DVOP_EXTENTS) && !code) {
                dev->cs_.minx = std::min(dev->cs_.minx, x);
                dev->cs_.maxx = std::max(dev->cs_.maxx, x);
                dev->cs_.miny = std::min(dev->cs_.miny, y);
                dev->cs_.maxy = std::max(dev->cs_.maxy, y);
            }
            orCodes |= code;
            andCodes &= code;
            if ((op & D3DVOP_CLIP) && !clip_.empty()) clip_[dst + i] = WORD(code);
        }
        if ((op & D3DVOP_CLIP) && count) {
            DWORD& st = dev->cs_.dwStatus;
            st = (st | (orCodes & 0xFFF)) & ((andCodes << 12) | 0xFF000FFF);
            hasClipCodes_ = true;
        }
        return D3D_OK;
    }
    VertexBuffer7* s = VB(src);
    if (srcIndex >= s->count_) return D3D_OK;
    count = std::min(count, s->count_ - srcIndex);
    return Process(this, op, dst, count, s->mem_.data() + size_t(srcIndex) * s->stride_, s->stride_, s->fvf_, dev, flags,
                   s->mem_.data() + s->mem_.size());
}

HRESULT VertexBuffer7::ProcessVerticesStrided(DWORD op, DWORD dst, DWORD count, D3DDRAWPRIMITIVESTRIDEDDATA* data,
                                              DWORD fvf, IDirect3DDevice7* device, DWORD flags) {
    if (!device || !data) return DDERR_INVALIDPARAMS;
    Guard l;
    if (optimized_) return D3DERR_VERTEXBUFFEROPTIMIZED;
    // The texture coordinate count is the destination's (V §8.5).
    const DWORD srcFvf = (fvf & ~DWORD(D3DFVF_TEXCOUNT_MASK)) | (fvf_ & D3DFVF_TEXCOUNT_MASK);
    if (dst >= count_) return D3D_OK;
    count = std::min(count, count_ - dst);  // what fits the destination: gathered no further
    std::vector<uint8_t> v;
    Gather(srcFvf, *data, count, &v);
    return Process(this, op, dst, count, v.data(), FvfSize(srcFvf), srcFvf, static_cast<Device7*>(device), flags,
                   v.data() + v.size());
}

// ---------------------------------------------------------------- DirectDraw's side

HRESULT D3D7Query(DDraw* dd, REFIID riid, void** out, int ddVersion) {
    if (!IsEqualGUID(riid, IID_IDirect3D7)) {
        // Direct3D 1-3 are d3dim.dll's (not yet implemented); a DirectDraw 7 object has none.
        return E_NOINTERFACE;
    }
    if (!dd->dd7) return E_NOINTERFACE;  // d3dim.dll's object: IDirect3D7 isn't among its interfaces
    if (g_config.d3d == kD3DOff) return E_NOINTERFACE;
    Guard l;
    auto* d3d = static_cast<Direct3D7*>(dd->d3d7);
    if (!d3d) {
        d3d = new (std::nothrow) Direct3D7(dd, ddVersion ? ddVersion : 7);
        if (!d3d) return E_OUTOFMEMORY;
        dd->d3d7 = d3d;
        DD2D9_LOG("Direct3D 7 object created (dd2d9's own)");
    }
    d3d->AddRef();
    *out = static_cast<IDirect3D7*>(d3d);
    return S_OK;
}

void D3D7Destroy(DDraw* dd) {
    delete static_cast<Direct3D7*>(dd->d3d7);
    dd->d3d7 = nullptr;
}

}  // namespace dd2d9
