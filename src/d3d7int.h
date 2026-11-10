// dd2d9's Direct3D 7 (see d3d7.h), internal declarations shared by d3d7obj.cpp (the Direct3D
// object, its device and vertex buffer creation, the hand-off from DirectDraw), d3d7state.cpp
// (the device's states and their translation to Direct3D 9), d3d7sb.cpp (state blocks),
// d3d7draw.cpp (scenes, clearing, drawing, Load) and d3d7pipe.cpp (the CPU vertex pipeline:
// clip status, ProcessVertices). The Direct3D 7 state lives in Device7, in Direct3D 7's own
// terms, and answers every Get; the Direct3D 9 device (gpures.h) gets it translated. The
// section numbers cited are those of the specs in re/spec/: D = d3d7-direct3d.md,
// S = d3d7-device-state.md, V = d3d7-draw-vb.md.
#pragma once
#include "d3d7.h"

#include "d3dcommon.h"
#include "d3dxlate.h"
#include "gpu.h"
#include "gpures.h"
#include "objects.h"

#include <d3d.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace dd2d9 {
namespace d7 {

enum DevKind { kRGB, kHAL, kTnLHAL };

class Direct3D7;
class Device7;
class VertexBuffer7;

// ---------------------------------------------------------------- small helpers

inline Surface* Surf(IDirectDrawSurface7* p) { return p ? Surface::From(p) : nullptr; }

// Room for `n` elements in `v`, false (instead of std::bad_alloc out of a COM method) when the
// memory isn't there.
template <class T> inline bool Reserve(std::vector<T>* v, size_t n) {
    try {
        v->reserve(n);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// (Each described where it is defined.)
D3DDEVICEDESC7 RgbDesc();  // d3d7obj.cpp
bool Retired(DWORD s);  // d3d7state.cpp
bool LightOk(const D3DLIGHT7* l);
bool Gather(DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA& d, DWORD n, std::vector<uint8_t>* out);  // d3d7draw.cpp
void BlendPoint(const float* p, const float* weights, DWORD blend, const D3DMATRIX* const* worlds, float out[4]);  // d3d7pipe.cpp
bool Invert(const D3DMATRIX& m, float out[16]);
D3DMATRIX NormalMatrix(const D3DMATRIX& m);

constexpr WORD kInClipSpace = 0x8000;  // a clip code's mark: the vertex was stored in clip space

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
    ~VertexBuffer7() {
        d3d_->Release();
    }

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

inline VertexBuffer7* VB(IDirect3DVertexBuffer7* p) { return static_cast<VertexBuffer7*>(p); }


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
    int keySent_ = -1;  // whether PushKeyAlpha last found stage 0's texture keyed (with COLORKEYENABLE)
    // ALPHATESTENABLE, ALPHAREF, ALPHAFUNC and stage 0's ALPHAOP, ALPHAARG1, ALPHAARG2 as
    // PushKeyAlpha last sent them.
    bool keyAlphaSent_ = false;
    DWORD keyAlphaSentV_[6] = {};
    // Reused from draw to draw (g_lock): strided vertices gathered, vertices given a normal,
    // a clipped vertex buffer's primitives, indices made safe.
    std::vector<uint8_t> gathered_, withNormal_, clipped_;
    std::vector<WORD> safeIdx_;
    D3DVIEWPORT7 vp_;
    DWORD rs_[256];
    D3DMATERIAL7 mat_ = {};
    float pvPower_ = 0.0f;  // the last material's specular power above 0 (ProcessVertices with power 0 keeps it)
    struct Light {
        D3DLIGHT7 l = {};
        bool set = false, enabled = false;
        bool dirty = false;  // changed since last sent to D3D9
        bool onGpu = false;  // enabled in D3D9 (SendLightEnables)
    };
    std::vector<Light> lights_;
    // More lights enabled than the GPU lights at once (SendLightEnables chooses among them
    // for each object: again when the world matrix changes).
    bool lightsOver_ = false;
    bool lightsOverLogged_ = false;
    D3DCLIPSTATUS cs_ = {};
    // The clip status drawing updates (as XP's front end): kept only once the game has asked
    // for it (GetClipStatus), as it costs a transform of every untransformed vertex drawn.
    bool csTracking_ = false;
    void TrackClipStatus(DWORD fvf, const void* v, UINT stride, DWORD n);
    DWORD ClipCodes(DWORD fvf, const void* v, UINT stride, DWORD n, bool extents, DWORD* andCodes);
    DWORD TlClipCodes(const uint8_t* v, UINT stride, DWORD n, bool extents, WORD* codes, DWORD* andCodes);
    DWORD PlaneCodes(const float* wp, DWORD on) const;
    void GrowExtents(float x, float y);

private:
    static constexpr DWORD kMaxPlanes = 32;
    // The Direct3D 9 device, ready for this device's calls: all state sent if it is new, else
    // what changed since the last Use. (The setters only store the state and mark it: a game
    // sets many states per draw, and getting the device ready costs a lock and a COM call.)
    struct Use {
        GpuGuard g;
        bool ok;
        explicit Use(Device7* d) : g(d->dd_->GetGpu()) {
            bool fresh = false;
            ok = d->ctx_.Begin(&fresh);
            if (ok && fresh) d->PushAll();
            else if (ok && d->dirty_) d->Flush();
        }
    };
    // The state changed since the last Use, not yet sent to D3D9: render states and texture
    // stage states a bit each, stages' textures, transforms by type, user clip planes by
    // index; the lights each carry their own mark.
    bool dirty_ = false;
    DWORD rsDirty_[8] = {}, tssDirty_[8] = {}, texDirty_ = 0, xformDirty_ = 0, planesDirty_ = 0;
    bool vpDirty_ = false, matDirty_ = false, lightsDirty_ = false;
    void ClearDirty();
    void Flush();
    void PushAll();
    void PushRS(DWORD s);
    void PushTSS(DWORD stage, DWORD t);
    void PushKeyAlpha();  // the alpha test and stage 0's alpha op, with COLORKEYENABLE's emulation
    void PushTransform(DWORD t);
    void PushLight(DWORD i);
    void SendLightEnables(bool all);
    D3DMATRIX* Matrix(DWORD t);
    HRESULT SetRS(DWORD s, DWORD v);                     // not recorded
    HRESULT SetTSS(DWORD stage, DWORD t, DWORD v);
    HRESULT SetTex(DWORD stage, Surface* s);
    HRESULT SetVP(const D3DVIEWPORT7& vp);
    void SetXform(DWORD t, const D3DMATRIX& m);
    HRESULT SetLightI(DWORD i, const D3DLIGHT7& l);
    HRESULT LightEnableI(DWORD i, bool on);
    bool GrowLights(DWORD i);
    void SetPlane(DWORD i, const float* p);
    void SetMat(const D3DMATERIAL7& m);
    bool Recording() const { return recording_ != 0; }
    HRESULT Record(const Cmd& c);
    void Capture(StateBlock* b);
    HRESULT Replay(const StateBlock* b);
    DWORD NewHandle(StateBlock* b);
    HRESULT DrawCommon(D3DPRIMITIVETYPE t, DWORD fvf, const void* v, UINT stride, DWORD n, const WORD* idx, DWORD ni,
                       bool fromVB);
    HRESULT DrawVB(D3DPRIMITIVETYPE t, VertexBuffer7* vb, DWORD start, DWORD n, const WORD* idx, DWORD ni);
    const WORD* SafeIndices(const WORD* idx, DWORD ni, DWORD n);
    DWORD ClipSpacePlanes(float (*out)[4]) const;
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

}  // namespace d7
}  // namespace dd2d9
