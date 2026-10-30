// dd2d9's Direct3D 3-6: devices (B), and the emulated DX6 driver behind them that turns
// their state into Direct3D 9's (B §1.4).
#include "d3d6int.h"

#include <new>

namespace dd2d9 {
namespace d6 {

Device* DevFrom(IDirect3DDevice* p) { return static_cast<Device::F1*>(p)->o; }
Device* DevFrom(IDirect3DDevice2* p) { return static_cast<Device::F2*>(p)->o; }
Device* DevFrom(IDirect3DDevice3* p) { return static_cast<Device::F3*>(p)->o; }

template <class T> static void Unlink(std::vector<T>& v, T x) {
    auto it = std::find(v.begin(), v.end(), x);
    if (it != v.end()) v.erase(it);
}

// ---------------------------------------------------------------- creation (B §1.3)

Device::Device(D3D* d3d, DevKind kind, const GUID& guid, int version, Surface* rt, int rtVersion, Surface* z,
               Palette* pal)
    : d3d(d3d), dd(d3d->dd), kind(kind), guid(guid), version(version), rt(rt), rtVersion(rtVersion), z(z), pal(pal),
      ctx(d3d->dd->GetGpu()) {
    f1.o = f2.o = f3.o = this;
    if (kind == kHAL) {
        HalHalDesc(&hal);
        HalHelDesc(&hel);
    } else {
        NullDesc(&hal);
        SoftHelDesc(kind, version, &hel);
    }
    // References (A §What a device holds): none for version 1 (the surface's aggregate).
    if (version == 2) {
        rt->AddRef(Surface::SlotFor(rtVersion));
        if (z) z->AddRef(Surface::SlotFor(rtVersion));
    } else if (version == 3) {
        rt->AddRef(Surface::kRefV4);
        if (z) z->AddRef(Surface::kRefV1);
    }
    if (version >= 2 && pal) pal->AddRef();
    std::fill(std::begin(rs), std::end(rs), 0xFFFFFFFFu);
    rs[13] = rs[58] = rs[59] = 0;  // so that their ~0 defaults count as changes
    for (auto& t : tss) std::fill(std::begin(t), std::end(t), 0xFFFFFFFFu);
    for (auto& t : dtss) std::fill(std::begin(t), std::end(t), 0xFFFFFFFFu);
    ctx.SetTargets(rt, z);
    // The defaults, in XP's order, through the public paths (the driver sees every one).
    const bool v3 = version >= 3;
    const DWORD colorKey = version < 2 && (kind == kHAL || kind == kRef);
    const DWORD mono = (kind == kRamp) ? TRUE : FALSE;
    const DWORD defs[][2] = {
        {1, 0}, {2, 0}, {3, D3DTADDRESS_WRAP}, {4, v3 ? 1u : 0u}, {29, v3 ? 0u : 1u}, {5, 0}, {6, 0},
        {7, z ? 1u : 0u}, {8, D3DFILL_SOLID}, {9, D3DSHADE_GOURAUD}, {10, 0}, {12, 13}, {13, 0xFFFFFFFF},
        {14, TRUE}, {15, FALSE}, {16, TRUE}, {19, D3DBLEND_ONE}, {20, D3DBLEND_ZERO}, {22, D3DCULL_CCW},
        {23, D3DCMP_LESSEQUAL}, {24, 0}, {25, D3DCMP_ALWAYS}, {26, FALSE}, {27, FALSE}, {28, FALSE}, {30, 0},
        {31, 0}, {32, 0}, {33, 0}, {34, 0}, {35, 0}, {36, D(0.0f)}, {37, D(1.0f)}, {38, D(1.0f)}, {39, 0},
        {40, 0}, {41, colorKey}, {28, FALSE}, {43, 0}, {44, D3DTADDRESS_WRAP}, {45, D3DTADDRESS_WRAP}, {46, 0},
        {47, 0}, {48, 0}, {49, 1}};
    for (const auto& d : defs) SetRS(d[0], d[1], false);
    for (DWORD s = 64; s <= 95; s++) SetRS(s, 0, false);
    const DWORD stencil[][2] = {{52, FALSE}, {53, D3DSTENCILOP_KEEP}, {54, D3DSTENCILOP_KEEP}, {55, D3DSTENCILOP_KEEP},
                                {56, D3DCMP_ALWAYS}, {57, 0}, {58, 0xFFFFFFFF}, {59, 0xFFFFFFFF}, {60, 0xFFFFFFFF},
                                {11, mono}};
    for (const auto& d : stencil) SetRS(d[0], d[1], false);
    for (DWORD s = 128; s <= 135; s++) SetRS(s, 0, false);
    ls.colorVertex = true;
    for (DWORD s = 0; s < 8; s++) DrvTexture(s, nullptr);
    for (DWORD s = 0; s < 8; s++) {
        const DWORD t[][2] = {{D3DTSS_COLOROP, s ? DWORD(D3DTOP_DISABLE) : DWORD(D3DTOP_MODULATE)},
                              {D3DTSS_COLORARG1, D3DTA_TEXTURE},
                              {D3DTSS_COLORARG2, D3DTA_CURRENT},
                              {D3DTSS_ALPHAOP, s ? DWORD(D3DTOP_DISABLE) : DWORD(D3DTOP_SELECTARG1)},
                              {D3DTSS_ALPHAARG1, D3DTA_TEXTURE},
                              {D3DTSS_ALPHAARG2, D3DTA_CURRENT},
                              {D3DTSS_BUMPENVMAT00, 0},
                              {D3DTSS_BUMPENVMAT01, 0},
                              {D3DTSS_BUMPENVMAT10, 0},
                              {D3DTSS_BUMPENVMAT11, 0},
                              {D3DTSS_TEXCOORDINDEX, 0},
                              {D3DTSS_ADDRESS, D3DTADDRESS_WRAP},
                              {D3DTSS_ADDRESSU, D3DTADDRESS_WRAP},
                              {D3DTSS_ADDRESSV, D3DTADDRESS_WRAP},
                              {D3DTSS_BORDERCOLOR, 0},
                              {D3DTSS_MAGFILTER, D3DTFG_POINT},
                              {D3DTSS_MINFILTER, D3DTFN_POINT},
                              {D3DTSS_MIPFILTER, D3DTFP_NONE},
                              {D3DTSS_MIPMAPLODBIAS, 0},
                              {D3DTSS_MAXMIPLEVEL, 0},
                              {D3DTSS_MAXANISOTROPY, 1},
                              {D3DTSS_BUMPENVLSCALE, 0},
                              {D3DTSS_BUMPENVLOFFSET, 0}};
        for (const auto& e : t) SetTSS(s, e[0], e[1]);
    }
    SetRS(17, D3DFILTER_NEAREST, false);
    SetRS(18, D3DFILTER_NEAREST, false);
    SetRS(21, D3DTBLEND_MODULATE, false);  // last
    Use u(this);  // creates the Direct3D 9 device now and sends all of it
}

Device::~Device() {
    // DestroyDevice (A): viewports deleted, stage textures released, handles and buffers go.
    inBegin = false;  // DeleteViewport would refuse (and leave them pointing here)
    for (Viewport* v : std::vector<Viewport*>(viewports)) DeleteViewportI(v);
    for (DWORD s = 0; s < 8; s++)
        if (stageTex[s]) stageTex[s]->s->Release(Surface::SlotFor(stageTex[s]->version));
    for (MatHandle* h : std::vector<MatHandle*>(matHandles)) h->mat->DestroyHandle(h);
    for (ExecBuf* b : execBufs) b->dev = nullptr;  // XP destroys them under the game
    execBufs.clear();
    if (d3d) Unlink(d3d->devices, this);
    {
        GpuGuard g(dd->GetGpu());
        ctx.SetTargets(nullptr, nullptr);
        for (DWORD i = 0; i < 8; i++) ctx.SetTexture(i, nullptr);
    }
    if (rtUnheld) rt = nullptr;
    if (version == 2) {
        if (rt) rt->Release(Surface::SlotFor(rtVersion));
        if (z) z->Release(zSlot >= 0 ? zSlot : Surface::SlotFor(rtVersion));
    } else if (version == 3) {
        if (rt) rt->Release(Surface::kRefV4);
        if (z) z->Release(Surface::kRefV1);
    }
    if (version >= 2 && pal) pal->Release();
}

void Device::Orphan() {
    if (d3d) Unlink(d3d->devices, this);
    d3d = nullptr;
}

// ---------------------------------------------------------------- IUnknown (B §10)

HRESULT Device::InnerQuery(REFIID r, void** out) {
    void* p = nullptr;
    if (version == 1) {
        if (IsEqualGUID(r, guid)) p = static_cast<IDirect3DDevice*>(&f1);
    } else if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirect3DDevice)) {
        p = static_cast<IDirect3DDevice*>(&f1);
    } else if (IsEqualGUID(r, IID_IDirect3DDevice2)) {
        p = static_cast<IDirect3DDevice2*>(&f2);
    } else if (IsEqualGUID(r, IID_IDirect3DDevice3) && version >= 3) {
        p = static_cast<IDirect3DDevice3*>(&f3);
    }
    if (!p) return E_NOINTERFACE;
    AddRef();
    *out = p;
    return S_OK;
}

HRESULT Device::QI(REFIID r, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (version == 1) return rt->QueryInterface(r, out, outerSlot == Surface::kRefV7 ? 7 : outerSlot + 1);
    Guard l;
    return InnerQuery(r, out);
}

ULONG Device::AddRef() {
    if (version == 1) return rt->AddRef(outerSlot);
    return ULONG(InterlockedIncrement(&ref));
}

ULONG Device::Release() {
    if (version == 1) return rt->Release(outerSlot);
    const LONG r = InterlockedDecrement(&ref);
    if (r == 0) {
        Guard l;
        delete this;
    }
    return ULONG(std::max<LONG>(r, 0));
}

void Device::SurfaceGone() {
    rt = nullptr;  // being destroyed: not to be touched
    delete this;
}

void Device::ForgetSurface(Surface* s) {
    bool targets = false;
    if (z == s) {
        z = nullptr;
        targets = true;
    }
    if (rt == s) {
        rt = nullptr;
        targets = true;
    }
    for (Surface*& t : dtex)
        if (t == s) t = nullptr;
    const DWORD h = static_cast<SurfaceInfo*>(s->d3dLegacy) ? static_cast<SurfaceInfo*>(s->d3dLegacy)->texHandle : 0;
    if (h) {
        Unlink(texHandles, h);
        if (rs[1] == h) rs[1] = 0;  // TEXTUREHANDLE (C §4.1)
        if (dTexHandle == h) dTexHandle = 0;
    }
    GpuGuard g(dd->GetGpu());
    if (targets) ctx.SetTargets(rt, z);
    for (DWORD i = 0; i < 8; i++) ctx.SetTexture(i, nullptr);
}

// ---------------------------------------------------------------- the emulated driver (B §1.4)

// Pushes everything to a new (or reset, or shared) Direct3D 9 device. Use held.
void Device::PushAll() {
    // What the vertices bring: Direct3D 9 gets transformed vertices only (d3d6pipe.cpp),
    // with the fog factor in specular alpha.
    ctx.SetRenderState(137, FALSE);  // LIGHTING
    ctx.SetRenderState(140, 0);      // FOGVERTEXMODE NONE
    ctx.SetRenderState(136, TRUE);   // CLIPPING
    ctx.SetRenderState(151, 0);      // VERTEXBLEND
    ctx.SetRenderState(152, 0);      // CLIPPLANEENABLE
    for (DWORD s = 1; s < 256; s++) PushRS9(s);
    for (DWORD st = 0; st < 8; st++)
        for (DWORD t = 1; t < 24; t++) PushStage9(st, t);
    PushStageOps9();
    PushAlpha9();
    ctx.SetRenderState(128, dWrap0);
    ctx.SetTargets(rt, z);
    SetViewport9();
}

// Render states Direct3D 9 has with the same meaning (B §Implementation notes).
void Device::PushRS9(DWORD s) {
    const DWORD v = rs[s];
    switch (s) {
    case 7: {  // ZENABLE: W buffering only where D3D9 has it
        const bool w = (hal.dpcTriCaps.dwRasterCaps & D3DPRASTERCAPS_WBUFFER) != 0 && kind == kHAL;
        ctx.SetRenderState(7, v == D3DZB_USEW && !w ? DWORD(D3DZB_TRUE) : v);
        break;
    }
    case 15: case 24: case 25: case 41:
        PushAlpha9();
        PushStageOps9();
        break;
    case 27:
        ctx.SetRenderState(27, v);
        PushStageOps9();
        break;
    case 47: {  // ZBIAS: D3D9's DEPTHBIAS (195), a float
        const float bias = -0.000005f * float(LONG(v));
        ctx.SetRenderState(195, D(bias));
        break;
    }
    case 8: case 9: case 14: case 16: case 19: case 20: case 22: case 23: case 26: case 28: case 29:
    case 34: case 35: case 36: case 37: case 38: case 48: case 52: case 53: case 54: case 55: case 56: case 57:
    case 58: case 59: case 60:
        ctx.SetRenderState(s, v);
        break;
    default:
        if (s >= 129 && s <= 135) ctx.SetRenderState(s, v);  // WRAP1-7 (WRAP0: the driver's copy)
        break;
    }
}

void Device::PushStage9(DWORD stage, DWORD t) {
    const DWORD v = dtss[stage][t];
    if (v == 0xFFFFFFFF && t != D3DTSS_BORDERCOLOR) return;  // never set
    switch (t) {
    case D3DTSS_COLOROP: case D3DTSS_COLORARG1: case D3DTSS_COLORARG2:
    case D3DTSS_ALPHAOP: case D3DTSS_ALPHAARG1: case D3DTSS_ALPHAARG2:
        if (stage <= 1) PushStageOps9();
        else ctx.SetTextureStageState(stage, t, v);
        break;
    case D3DTSS_ADDRESS:
        ctx.SetSamplerState(stage, 1, v);
        ctx.SetSamplerState(stage, 2, v);
        break;
    case D3DTSS_ADDRESSU: ctx.SetSamplerState(stage, 1, v); break;
    case D3DTSS_ADDRESSV: ctx.SetSamplerState(stage, 2, v); break;
    case D3DTSS_BORDERCOLOR: ctx.SetSamplerState(stage, 4, v); break;
    case D3DTSS_MAGFILTER: {
        static const DWORD kMap[] = {0, 1, 2, 2, 2, 3};
        ctx.SetSamplerState(stage, 5, v < 6 ? kMap[v] : 2);
        break;
    }
    case D3DTSS_MINFILTER: ctx.SetSamplerState(stage, 6, v <= 3 ? v : 2); break;
    case D3DTSS_MIPFILTER: ctx.SetSamplerState(stage, 7, v >= 2 && v <= 3 ? v - 1 : 0); break;
    case D3DTSS_MIPMAPLODBIAS: ctx.SetSamplerState(stage, 8, v); break;
    case D3DTSS_MAXMIPLEVEL: ctx.SetSamplerState(stage, 9, v); break;
    case D3DTSS_MAXANISOTROPY: ctx.SetSamplerState(stage, 10, v); break;
    default:
        if ((t >= 7 && t <= 11) || t == 22 || t == 23) ctx.SetTextureStageState(stage, t, v);
        break;
    }
}

// A hardware device textures only from texture surfaces (Wine's test_texture_wrong_caps).
Surface* Device::StageSurface(DWORD stage) const {
    Surface* s = dTexHandle ? (stage == 0 ? SurfaceForHandle(dTexHandle) : nullptr) : dtex[stage];
    if (s && kind == kHAL && !(s->desc.ddsCaps.dwCaps & DDSCAPS_TEXTURE)) return nullptr;
    return s;
}

// Stages 0 and 1: while TEXTUREHANDLE is set only stage 0 runs; legacy MODULATE takes alpha
// from the texture when it has alpha, else from the vertex (B §1.4: the reference
// rasterizer's span choice for MODULATE(TEXTURE, DIFFUSE) / SELECTARG1(TEXTURE)).
void Device::PushStageOps9() {
    const DWORD* s0 = dtss[0];
    for (DWORD t : {D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2})
        if (s0[t] != 0xFFFFFFFF) ctx.SetTextureStageState(0, t, s0[t]);
    const DWORD op1 = dTexHandle ? DWORD(D3DTOP_DISABLE) : dtss[1][D3DTSS_COLOROP];
    DWORD alphaOp = s0[D3DTSS_ALPHAOP];
    Surface* tex = StageSurface(0);
    if (alphaOp == D3DTOP_SELECTARG1 && s0[D3DTSS_ALPHAARG1] == D3DTA_TEXTURE && s0[D3DTSS_COLOROP] == D3DTOP_MODULATE &&
        s0[D3DTSS_COLORARG1] == D3DTA_TEXTURE && (s0[D3DTSS_COLORARG2] == D3DTA_DIFFUSE || s0[D3DTSS_COLORARG2] == D3DTA_CURRENT) &&
        op1 == D3DTOP_DISABLE && tex && !tex->buf->fmt.a && !tex->buf->fmt.fourcc) {
        // Texture alpha is 1 (or 0 where colour-keyed): MODULATE gives the vertex alpha.
        alphaOp = D3DTOP_MODULATE;
        ctx.SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    }
    // Colour keying works on the texel alone on XP: with the game's alpha unused (no blending,
    // no alpha test), the alpha test of the key emulation gets the texture's alpha
    // (Wine's test_colorkey_precision: MODULATEALPHA with a vertex alpha of 0).
    if (rs[41] == TRUE && tex && tex->hasCk[kCkSrcBlt] && rs[27] != TRUE && rs[15] != TRUE &&
        s0[D3DTSS_COLOROP] != D3DTOP_DISABLE) {
        alphaOp = D3DTOP_SELECTARG1;
        ctx.SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    }
    if (alphaOp != 0xFFFFFFFF) ctx.SetTextureStageState(0, D3DTSS_ALPHAOP, alphaOp);
    for (DWORD t : {D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2})
        if (dtss[1][t] != 0xFFFFFFFF) ctx.SetTextureStageState(1, t, dtss[1][t]);
    if (op1 != 0xFFFFFFFF) ctx.SetTextureStageState(1, D3DTSS_COLOROP, op1);
}

// COLORKEYENABLE: keyed texels were uploaded transparent; an alpha test drops them.
void Device::PushAlpha9() {
    ctx.SetColorKeyEnable(rs[41] == TRUE);
    Surface* t = StageSurface(0);
    const bool key = rs[41] == TRUE && t && t->hasCk[kCkSrcBlt];
    if (rs[15] == TRUE || !key) {
        ctx.SetRenderState(15, rs[15] == TRUE);
        ctx.SetRenderState(24, rs[24] & 0xFF);
        ctx.SetRenderState(25, rs[25] == 0xFFFFFFFF ? DWORD(D3DCMP_ALWAYS) : rs[25]);
    } else {
        ctx.SetRenderState(15, TRUE);
        ctx.SetRenderState(24, 0);
        ctx.SetRenderState(25, D3DCMP_GREATER);
    }
}

// At each draw: the stage textures (a handle resolves now), and what depends on them.
void Device::BindTextures9() {
    for (DWORD i = 0; i < 8; i++) ctx.SetTexture(i, StageSurface(i));
    PushStageOps9();
    PushAlpha9();
}

// A render state as the driver gets it (B §1.4).
void Device::DrvRS(DWORD s, DWORD v) {
    switch (s) {
    case 1:  // TEXTUREHANDLE
        dTexHandle = v;
        PushStageOps9();
        PushAlpha9();
        return;
    case 3:  // TEXTUREADDRESS
        dtss[0][D3DTSS_ADDRESS] = dtss[0][D3DTSS_ADDRESSU] = dtss[0][D3DTSS_ADDRESSV] = v;
        PushStage9(0, D3DTSS_ADDRESS);
        return;
    case 5: case 6: {  // WRAPU / WRAPV
        const DWORD bit = s == 5 ? D3DWRAP_U : D3DWRAP_V;
        dWrap0 = v ? (dWrap0 | bit) : (dWrap0 & ~bit);
        ctx.SetRenderState(128, dWrap0);
        return;
    }
    case 128:
        dWrap0 = v;
        ctx.SetRenderState(128, v);
        return;
    case 17:  // TEXTUREMAG
        dtss[0][D3DTSS_MAGFILTER] = v == D3DFILTER_LINEAR ? D3DTFG_LINEAR : D3DTFG_POINT;
        PushStage9(0, D3DTSS_MAGFILTER);
        return;
    case 18: {  // TEXTUREMIN
        static const DWORD kMin[7][2] = {{0, 0},
                                         {D3DTFN_POINT, D3DTFP_NONE},
                                         {D3DTFN_LINEAR, D3DTFP_NONE},
                                         {D3DTFN_POINT, D3DTFP_POINT},
                                         {D3DTFN_LINEAR, D3DTFP_POINT},
                                         {D3DTFN_POINT, D3DTFP_LINEAR},
                                         {D3DTFN_LINEAR, D3DTFP_LINEAR}};
        if (v < 1 || v > 6) return;  // other values: no change
        dtss[0][D3DTSS_MINFILTER] = kMin[v][0];
        dtss[0][D3DTSS_MIPFILTER] = kMin[v][1];
        PushStage9(0, D3DTSS_MINFILTER);
        PushStage9(0, D3DTSS_MIPFILTER);
        return;
    }
    case 21: {  // TEXTUREMAPBLEND
        DWORD* t = dtss[0];
        switch (v) {
        case D3DTBLEND_MODULATE: case D3DTBLEND_MODULATEMASK:
            t[D3DTSS_COLOROP] = D3DTOP_MODULATE;
            t[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
            t[D3DTSS_COLORARG2] = D3DTA_DIFFUSE;
            t[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
            t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            t[D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
            break;
        case D3DTBLEND_DECALALPHA:
            t[D3DTSS_COLOROP] = D3DTOP_BLENDTEXTUREALPHA;
            t[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
            t[D3DTSS_COLORARG2] = D3DTA_DIFFUSE;
            t[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG2;
            t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            t[D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
            break;
        case D3DTBLEND_MODULATEALPHA:
            t[D3DTSS_COLOROP] = t[D3DTSS_ALPHAOP] = D3DTOP_MODULATE;
            t[D3DTSS_COLORARG1] = t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            t[D3DTSS_COLORARG2] = t[D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
            break;
        case D3DTBLEND_ADD:
            t[D3DTSS_COLOROP] = D3DTOP_ADD;
            t[D3DTSS_COLORARG1] = D3DTA_TEXTURE;
            t[D3DTSS_COLORARG2] = D3DTA_DIFFUSE;
            t[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG2;
            t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            t[D3DTSS_ALPHAARG2] = D3DTA_DIFFUSE;
            break;
        default:  // DECAL, DECALMASK, COPY and anything else
            t[D3DTSS_COLOROP] = t[D3DTSS_ALPHAOP] = D3DTOP_SELECTARG1;
            t[D3DTSS_COLORARG1] = t[D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
            break;
        }
        dtss[1][D3DTSS_COLOROP] = D3DTOP_DISABLE;
        PushStageOps9();
        return;
    }
    case 43:
        dtss[0][D3DTSS_BORDERCOLOR] = v;
        PushStage9(0, D3DTSS_BORDERCOLOR);
        return;
    case 44:
        dtss[0][D3DTSS_ADDRESSU] = v;
        PushStage9(0, D3DTSS_ADDRESSU);
        return;
    case 45:
        dtss[0][D3DTSS_ADDRESSV] = v;
        PushStage9(0, D3DTSS_ADDRESSV);
        return;
    case 46:
        dtss[0][D3DTSS_MIPMAPLODBIAS] = v;
        PushStage9(0, D3DTSS_MIPMAPLODBIAS);
        return;
    default:
        PushRS9(s);  // ANISOTROPY and the other legacy states: nothing (only stored)
        return;
    }
}

void Device::DrvTSS(DWORD stage, DWORD t, DWORD v) {
    if (t >= 32) return;
    dtss[stage][t] = v;
    if (t == D3DTSS_ADDRESS) dtss[stage][D3DTSS_ADDRESSU] = dtss[stage][D3DTSS_ADDRESSV] = v;
    PushStage9(stage, t);
}

void Device::DrvTexture(DWORD stage, Surface* s) {
    dtex[stage] = s;
    if (stage <= 1 && s) dTexHandle = 0;  // binding a texture on stage 0 or 1 ends TEXTUREHANDLE
}

// ---------------------------------------------------------------- render states (B §1.1)

HRESULT Device::SetRS(DWORD s, DWORD v, bool fromExecute) {
    if (s == 0 || s >= 256) return D3D_OK;
    if (s == D3DRENDERSTATE_FLUSHBATCH) return D3D_OK;  // flushes; not stored
    if (!fromExecute && (s == 5 || s == 6)) {  // WRAPU / WRAPV: WRAP0's bits
        const DWORD bit = s == 5 ? D3DWRAP_U : D3DWRAP_V;
        const DWORD w = v ? (rs[128] | bit) : (rs[128] & ~bit);
        if (w != rs[128]) {
            rs[128] = w;
            DrvRS(128, w);
        }
    }
    if (!fromExecute && rs[s] == v) return D3D_OK;
    rs[s] = v;
    DrvRS(s, v);
    return D3D_OK;
}

HRESULT Device::F2::SetRenderState(D3DRENDERSTATETYPE s, DWORD v) {
    if (DWORD(s) == 0 || DWORD(s) > 255) return DDERR_INVALIDPARAMS;
    Guard l;
    Use u(o);
    return o->SetRS(s, v, false);
}

HRESULT Device::F2::GetRenderState(D3DRENDERSTATETYPE s, LPDWORD v) {
    if (DWORD(s) == 0 || DWORD(s) > 255) return DDERR_INVALIDPARAMS;
    if (!v) return DDERR_INVALIDPARAMS;  // XP faults inside its SEH frame
    Guard l;
    *v = o->rs[s];
    return D3D_OK;
}

// ---------------------------------------------------------------- light states (B §2)

void Device::SetMaterialHandle(DWORD h) {
    ls.material = h;
    const MatHandle* m = FindMatHandle(h);
    if (m) curMat = m->data;
    else memset(&curMat, 0, sizeof(curMat));
}

HRESULT Device::SetLS(DWORD s, DWORD v) {
    if (s > 256) {  // an override (C §6.5)
        if (s - 256 < overLS.size()) overLS[s - 256] = v != 0;
        return D3D_OK;
    }
    if (s < overLS.size() && overLS[s]) return D3D_OK;
    switch (s) {
    case D3DLIGHTSTATE_MATERIAL:
        if (v != ls.material) SetMaterialHandle(v);
        break;
    case D3DLIGHTSTATE_AMBIENT: ls.ambient = v; break;
    case D3DLIGHTSTATE_COLORMODEL: ls.colorModel = v; break;
    case D3DLIGHTSTATE_FOGMODE: ls.fogMode = v; break;
    case D3DLIGHTSTATE_FOGSTART: ls.fogStart = v; break;
    case D3DLIGHTSTATE_FOGEND: ls.fogEnd = v; break;
    case D3DLIGHTSTATE_FOGDENSITY: ls.fogDensity = v; break;
    case D3DLIGHTSTATE_COLORVERTEX: ls.colorVertex = v != 0; break;
    default: return DDERR_INVALIDPARAMS;
    }
    return D3D_OK;
}

HRESULT Device::F2::SetLightState(D3DLIGHTSTATETYPE s, DWORD v) {
    Guard l;
    if (o->inBegin) return D3DERR_INBEGIN;
    return o->SetLS(s, v);
}

HRESULT Device::GetLightStateI(DWORD s, DWORD* v) {
    if (!v) return DDERR_INVALIDPARAMS;  // XP faults
    Guard l;
    switch (s) {
    case D3DLIGHTSTATE_MATERIAL: *v = ls.material; break;
    case D3DLIGHTSTATE_AMBIENT: *v = ls.ambient; break;
    case D3DLIGHTSTATE_COLORMODEL: *v = ls.colorModel; break;
    case D3DLIGHTSTATE_FOGMODE: *v = ls.fogMode; break;
    case D3DLIGHTSTATE_FOGSTART: *v = ls.fogStart; break;
    case D3DLIGHTSTATE_FOGEND: *v = ls.fogEnd; break;
    case D3DLIGHTSTATE_FOGDENSITY: *v = ls.fogDensity; break;
    case D3DLIGHTSTATE_COLORVERTEX:
        if (version < 3) return DDERR_INVALIDPARAMS;
        *v = ls.colorVertex;
        break;
    default: return DDERR_INVALIDPARAMS;
    }
    return D3D_OK;
}

// ---------------------------------------------------------------- stages and textures (B §3)

HRESULT Device::SetTSS(DWORD stage, DWORD t, DWORD v) {
    if (stage >= 8 || t >= 64) return D3D_OK;  // XP writes past its array; ignored here
    if (tss[stage][t] == v) return D3D_OK;
    tss[stage][t] = v;
    DrvTSS(stage, t, v);
    return D3D_OK;
}

HRESULT Device::F3::SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v) {
    Guard l;
    Use u(o);
    return o->SetTSS(stage, t, v);
}

HRESULT Device::F3::GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE t, LPDWORD v) {
    if (!v) return DDERR_INVALIDPARAMS;
    Guard l;
    *v = stage < 8 && DWORD(t) < 64 ? o->tss[stage][t] : 0;
    return D3D_OK;
}

HRESULT Device::SetTex(DWORD stage, Texture* t) {
    if (stage >= 8) return D3D_OK;
    if (stageTex[stage] == t) return D3D_OK;
    if (t) t->s->AddRef(Surface::SlotFor(t->version));
    Texture* old = stageTex[stage];
    stageTex[stage] = t;
    DrvTexture(stage, t ? t->s : nullptr);
    if (old) old->s->Release(Surface::SlotFor(old->version));
    return D3D_OK;
}

HRESULT Device::F3::SetTexture(DWORD stage, IDirect3DTexture2* t) {
    Guard l;
    Use u(o);
    return o->SetTex(stage, TexFrom(t));
}

HRESULT Device::F3::GetTexture(DWORD stage, IDirect3DTexture2** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    Texture* t = stage < 8 ? o->stageTex[stage] : nullptr;
    *out = nullptr;
    if (t) {
        t->s->AddRef(Surface::SlotFor(t->version));
        *out = &t->f2;
    }
    return D3D_OK;
}

HRESULT Device::SwapTextureHandlesI(Texture* a, Texture* b) {
    if (!a || !b) return DDERR_INVALIDOBJECT;
    Guard l;
    const DWORD managed = DDSCAPS2_TEXTUREMANAGE;
    if ((a->s->desc.ddsCaps.dwCaps2 & managed) || (b->s->desc.ddsCaps.dwCaps2 & managed)) return DDERR_INVALIDPARAMS;
    auto has = [&](Surface* s) {
        const auto* i = static_cast<SurfaceInfo*>(s->d3dLegacy);
        return i && i->texHandle && std::find(texHandles.begin(), texHandles.end(), i->texHandle) != texHandles.end();
    };
    if (!has(a->s) || !has(b->s)) return DDERR_INVALIDPARAMS;
    SwapHandles(a->s, b->s);
    return D3D_OK;
}

HRESULT Device::F1::SwapTextureHandles(IDirect3DTexture* a, IDirect3DTexture* b) {
    return o->SwapTextureHandlesI(TexFrom(a), TexFrom(b));
}
HRESULT Device::F2::SwapTextureHandles(IDirect3DTexture2* a, IDirect3DTexture2* b) {
    return o->SwapTextureHandlesI(TexFrom(a), TexFrom(b));
}

// SetRenderTarget forgets the device's texture handles (B §3.5).
void Device::InvalidateTexHandles() {
    for (MatHandle* m : matHandles)
        if (std::find(texHandles.begin(), texHandles.end(), m->data.hTexture) != texHandles.end()) m->data.hTexture = 0;
    texHandles.clear();
}

// ---------------------------------------------------------------- transforms (B §4)

std::unordered_map<DWORD, D3DMATRIX>& Matrices() {
    static std::unordered_map<DWORD, D3DMATRIX> m;
    return m;
}

DWORD NewMatrixHandle() {
    static DWORD next = 1;
    return next++;
}

D3DMATRIX* Device::Xform(DWORD t) {
    switch (t) {
    case D3DTRANSFORMSTATE_WORLD: return &world;
    case D3DTRANSFORMSTATE_VIEW: return &view;
    case D3DTRANSFORMSTATE_PROJECTION: return &proj;
    default: return nullptr;
    }
}

HRESULT Device::SetTransformI(DWORD t, const D3DMATRIX* m) {
    Guard l;
    D3DMATRIX* x = Xform(t);
    if (!x || !m) return DDERR_INVALIDPARAMS;
    *x = *m;
    return D3D_OK;
}

HRESULT Device::GetTransformI(DWORD t, D3DMATRIX* m) {
    Guard l;
    D3DMATRIX* x = Xform(t);
    if (!x || !m) return DDERR_INVALIDPARAMS;
    *m = *x;
    return D3D_OK;
}

HRESULT Device::MultiplyTransformI(DWORD t, const D3DMATRIX* m) {
    Guard l;
    D3DMATRIX* x = Xform(t);
    if (!x || !m) return DDERR_INVALIDPARAMS;
    *x = Mul(*m, *x);
    return D3D_OK;
}

void Device::MatrixChanged(DWORD h) {
    auto it = matrices.find(h);
    if (it == matrices.end()) return;
    for (DWORD k = 0; k < 3; k++)
        if (bound[k] == h) *Xform(k + 1) = it->second;
}

HRESULT Device::F1::CreateMatrix(D3DMATRIXHANDLE* h) {
    if (!h) return DDERR_INVALIDPARAMS;
    Guard l;
    const DWORD id = NewMatrixHandle();
    o->matrices[id] = kIdentity;
    *h = id;
    return D3D_OK;
}

// Handles are checked against the device's own (XP dereferences any non-zero value).
HRESULT Device::F1::SetMatrix(D3DMATRIXHANDLE h, D3DMATRIX* m) {
    if (!m || !h) return DDERR_INVALIDPARAMS;
    Guard l;
    auto it = o->matrices.find(h);
    if (it == o->matrices.end()) return DDERR_INVALIDPARAMS;
    it->second = *m;
    o->MatrixChanged(h);
    return D3D_OK;
}

HRESULT Device::F1::GetMatrix(D3DMATRIXHANDLE h, D3DMATRIX* m) {
    if (!m) return DDERR_INVALIDPARAMS;
    memset(m, 0, sizeof(*m));
    if (!h) return DDERR_INVALIDPARAMS;
    Guard l;
    auto it = o->matrices.find(h);
    if (it == o->matrices.end()) return DDERR_INVALIDPARAMS;
    *m = it->second;
    return D3D_OK;
}

HRESULT Device::F1::DeleteMatrix(D3DMATRIXHANDLE h) {
    if (!h) return DDERR_INVALIDPARAMS;
    Guard l;
    if (!o->matrices.erase(h)) return DDERR_INVALIDPARAMS;
    return D3D_OK;
}

// ---------------------------------------------------------------- viewports (B §5)

HRESULT Device::Download(Viewport* vp) {
    if (!vp->dataSet) return D3DERR_VIEWPORTDATANOTSET;
    MakeMap(vp->data, &map);  // degenerate data keeps the previous mapping
    downloaded = vp->id;
    vp->lightsDirty = true;
    ApplyViewport9();
    return D3D_OK;
}

// Direct3D 9's viewport: the DX6 viewport rectangle (it clips transformed vertices).
void Device::ApplyViewport9() {
    Use u(this);
    if (u.ok) SetViewport9();
}

void Device::SetViewport9() {
    if (!rt) return;
    const LONG w = rt->W(), h = rt->H();
    LONG x = 0, y = 0, vw = w, vh = h;
    if (map.valid) {
        x = std::clamp(LONG(map.X), 0L, w);
        y = std::clamp(LONG(map.Y), 0L, h);
        vw = std::clamp(LONG(map.W), 0L, w - x);
        vh = std::clamp(LONG(map.H), 0L, h - y);
    }
    ctx.SetViewport(DWORD(x), DWORD(y), DWORD(std::max(vw, 1L)), DWORD(std::max(vh, 1L)), 0.0f, 1.0f);
}

HRESULT Device::CheckViewport() {
    if (!cur) return D3DERR_NOCURRENTVIEWPORT;  // XP dereferences null (deliberate)
    if (downloaded != cur->id) return Download(cur);
    return D3D_OK;
}

void Device::RemoveViewport(Viewport* vp) {
    Unlink(viewports, vp);
    vp->dev = nullptr;
    if (cur == vp) {
        cur = nullptr;
        downloaded = 0;
    }
}

HRESULT Device::AddViewportI(Viewport* vp) {
    if (!vp) return DDERR_INVALIDPARAMS;  // XP faults
    Guard l;
    if (vp->dev) return DDERR_INVALIDPARAMS;
    viewports.push_back(vp);
    vp->dev = this;
    vp->AddRef();
    return D3D_OK;
}

HRESULT Device::DeleteViewportI(Viewport* vp) {
    if (!vp) return DDERR_INVALIDPARAMS;
    Guard l;
    if (inBegin) return D3DERR_INBEGIN;
    if (vp->dev != this) return DDERR_INVALIDPARAMS;
    const bool wasCurrent = cur == vp;
    RemoveViewport(vp);
    if (wasCurrent && version >= 3) vp->Release();  // the current-viewport reference: version 3 only
    vp->Release();
    return D3D_OK;
}

HRESULT Device::NextViewportI(Viewport* ref, Viewport** out, DWORD flags) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    Guard l;
    if (flags == D3DNEXT_NEXT && (!ref || ref->dev != this)) return DDERR_INVALIDPARAMS;
    if (viewports.empty()) return D3DERR_NOVIEWPORTS;
    if (inBegin) return D3DERR_INBEGIN;
    Viewport* found = nullptr;
    if (flags == D3DNEXT_NEXT) {
        auto it = std::find(viewports.begin(), viewports.end(), ref);
        if (it != viewports.end() && ++it != viewports.end()) found = *it;
    } else if (flags == D3DNEXT_HEAD) {
        found = viewports.front();
    } else if (flags == D3DNEXT_TAIL) {
        found = viewports.back();
    } else {
        return DDERR_INVALIDPARAMS;
    }
    if (found) {
        found->AddRef();
        *out = found;
    }
    return D3D_OK;
}

HRESULT Device::SetCurrentViewportI(Viewport* vp) {
    if (!vp) return DDERR_INVALIDPARAMS;
    Guard l;
    if (inBegin) return D3DERR_INBEGIN;
    if (vp->dev != this) return DDERR_INVALIDPARAMS;
    Viewport* prev = cur;
    cur = vp;
    if (vp->dataSet && downloaded != vp->id) {
        const HRESULT hr = Download(vp);
        if (FAILED(hr)) {
            cur = prev;
            return hr;
        }
    }
    vp->AddRef();  // on every version (a version 1/2 device leaks one on DeleteViewport)
    if (prev) prev->Release();
    return D3D_OK;
}

HRESULT Device::GetCurrentViewportI(Viewport** out) {
    Guard l;
    if (inBegin) return D3DERR_INBEGIN;
    if (!cur) return D3DERR_NOCURRENTVIEWPORT;
    if (!out) return DDERR_INVALIDPARAMS;  // XP faults after the AddRef
    cur->AddRef();
    *out = cur;
    return D3D_OK;
}

HRESULT Device::F1::AddViewport(IDirect3DViewport* v) { return o->AddViewportI(VpFrom(v)); }
HRESULT Device::F2::AddViewport(IDirect3DViewport2* v) { return o->AddViewportI(VpFrom(v)); }
HRESULT Device::F3::AddViewport(IDirect3DViewport3* v) { return o->AddViewportI(VpFrom(v)); }
HRESULT Device::F1::DeleteViewport(IDirect3DViewport* v) { return o->DeleteViewportI(VpFrom(v)); }
HRESULT Device::F2::DeleteViewport(IDirect3DViewport2* v) { return o->DeleteViewportI(VpFrom(v)); }
HRESULT Device::F3::DeleteViewport(IDirect3DViewport3* v) { return o->DeleteViewportI(VpFrom(v)); }
HRESULT Device::F1::NextViewport(IDirect3DViewport* r, IDirect3DViewport** out, DWORD flags) {
    return o->NextViewportI(VpFrom(r), reinterpret_cast<Viewport**>(out), flags);
}
HRESULT Device::F2::NextViewport(IDirect3DViewport2* r, IDirect3DViewport2** out, DWORD flags) {
    return o->NextViewportI(VpFrom(r), reinterpret_cast<Viewport**>(out), flags);
}
HRESULT Device::F3::NextViewport(IDirect3DViewport3* r, IDirect3DViewport3** out, DWORD flags) {
    return o->NextViewportI(VpFrom(r), reinterpret_cast<Viewport**>(out), flags);
}
HRESULT Device::F2::SetCurrentViewport(IDirect3DViewport2* v) { return o->SetCurrentViewportI(VpFrom(v)); }
HRESULT Device::F3::SetCurrentViewport(IDirect3DViewport3* v) { return o->SetCurrentViewportI(VpFrom(v)); }
HRESULT Device::F2::GetCurrentViewport(IDirect3DViewport2** v) {
    return o->GetCurrentViewportI(reinterpret_cast<Viewport**>(v));
}
HRESULT Device::F3::GetCurrentViewport(IDirect3DViewport3** v) {
    return o->GetCurrentViewportI(reinterpret_cast<Viewport**>(v));
}

// ---------------------------------------------------------------- scenes (B §6)

bool Device::TargetsBusy() const { return (rt && rt->lockCount) || (z && z->lockCount); }

HRESULT Device::BeginSceneI() {
    Guard l;
    if (inScene) return D3DERR_SCENE_IN_SCENE;
    if (!rt) return DDERR_SURFACELOST;
    if (rt->CantDraw() || (z && z->CantDraw(false))) {
        // d3dim restores every surface of the DirectDraw object for the game (B §6.1).
        if (FAILED(dd->TestCooperativeLevel())) return DDERR_SURFACELOST;
        if (FAILED(dd->RestoreAllSurfaces())) return D3DERR_SCENE_BEGIN_FAILED;
    }
    if (TargetsBusy()) return DDERR_SURFACEBUSY;
    inScene = true;
    return D3D_OK;
}

HRESULT Device::EndSceneI() {
    Guard l;
    if (!inScene) return D3DERR_SCENE_NOT_IN_SCENE;
    inScene = false;
    {
        GpuGuard g(dd->GetGpu());
        ctx.EndScene();
    }
    if (lostInScene) {
        lostInScene = false;
        return DDERR_SURFACELOST;
    }
    return D3D_OK;
}

// ---------------------------------------------------------------- render target (B §7.1)

HRESULT Device::SetRenderTargetI(Surface* s, int sVersion) {
    if (!s) return DDERR_INVALIDOBJECT;
    if (version == 1) return D3DERR_DEVICEAGGREGATED;
    const DWORD caps = s->desc.ddsCaps.dwCaps;
    const DWORD depths = hal.dwDeviceRenderBitDepth | hel.dwDeviceRenderBitDepth;
    if (!(caps & DDSCAPS_3DDEVICE) || !(BitsToDDBD(s->buf->fmt.bpp) & depths)) return DDERR_INVALIDCAPS;
    if (s->W() > 2048 || s->H() > 2048) return DDERR_INVALIDPARAMS;
    if (caps & DDSCAPS_ZBUFFER) {
        // A Z buffer: the driver refuses it, but the device has switched already, without
        // a reference on it (Wine's test_rt_caps: GetRenderTarget then returns it).
        if (rt && !rtUnheld) rt->Release(version == 2 ? Surface::SlotFor(rtVersion) : Surface::kRefV4);
        rtUnheld = true;
        rt = s;
        rtVersion = version == 2 ? 1 : 4;
        return DDERR_INVALIDPIXELFORMAT;
    }
    if (!s->palette && s->buf->fmt.bpp < 16) return DDERR_NOPALETTEATTACHED;
    Surface* nz = AttachedZ(s);
    if (!nz && z) return DDERR_INVALIDPARAMS;  // a Z buffer can be gained, not lost
    // A version 2 device holds the new target's IDirectDrawSurface (v1) interface (B §7.1).
    (void)sVersion;
    const int rtSlot = version == 2 ? Surface::kRefV1 : Surface::kRefV4;
    s->AddRef(rtSlot);
    if (nz) nz->AddRef(Surface::kRefV1);
    if (s->palette) s->palette->AddRef();
    if (rt && !rtUnheld) rt->Release(version == 2 ? Surface::SlotFor(rtVersion) : Surface::kRefV4);
    rtUnheld = false;
    if (z) z->Release(version == 2 ? (zSlot >= 0 ? zSlot : Surface::SlotFor(rtVersion)) : Surface::kRefV1);
    if (pal) pal->Release();
    rt = s;
    rtVersion = version == 2 ? 1 : 4;
    z = nz;
    zSlot = Surface::kRefV1;
    pal = s->palette;
    InvalidateTexHandles();
    {
        Use u(this);
        ctx.SetTargets(rt, z);
    }
    ApplyViewport9();  // the viewport stays (B §7.1)
    return D3D_OK;
}

HRESULT Device::F2::SetRenderTarget(IDirectDrawSurface* s, DWORD) {
    if (!s) return DDERR_INVALIDOBJECT;
    Guard l;
    Surface* x = Surface::From(s);
    int v = 1;
    for (int k : {1, 2, 3, 4, 7})
        if (x->Iface(k) == s) v = k;
    return o->SetRenderTargetI(x, v);
}

HRESULT Device::F3::SetRenderTarget(IDirectDrawSurface4* s, DWORD) {
    if (!s) return DDERR_INVALIDOBJECT;
    Guard l;
    return o->SetRenderTargetI(Surface::From(s), 4);
}

HRESULT Device::F2::GetRenderTarget(IDirectDrawSurface** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    if (!o->rt) return DDERR_INVALIDOBJECT;
    const int v = o->version == 2 ? o->rtVersion : 1;
    o->rt->AddRef(Surface::SlotFor(v));
    *out = static_cast<IDirectDrawSurface*>(o->rt->Iface(v));
    return D3D_OK;
}

HRESULT Device::F3::GetRenderTarget(IDirectDrawSurface4** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    if (!o->rt) return DDERR_INVALIDOBJECT;
    o->rt->AddRef(Surface::kRefV4);
    *out = static_cast<IDirectDrawSurface4*>(o->rt->Iface(4));
    return D3D_OK;
}

// ---------------------------------------------------------------- caps, formats, stats (B §7)

HRESULT Device::GetCapsI(D3DDEVICEDESC* h, D3DDEVICEDESC* s) {
    auto sizeOk = [](const D3DDEVICEDESC* d) { return d && (d->dwSize == 0xAC || d->dwSize == 0xCC || d->dwSize == 0xFC); };
    if (!sizeOk(h) || !sizeOk(s)) return DDERR_INVALIDPARAMS;
    Guard l;
    memcpy(h, &hal, h->dwSize);  // dwSize too: the stored descriptor's (0xFC)
    memcpy(s, &hel, s->dwSize);
    return D3D_OK;
}

// B §7.4: XP's inclusion list (and FOURCC formats) among what dd2d9 textures can be.
HRESULT Device::EnumTexFormats(void* cb, void* ctxp, bool v3) {
    if (!cb || IsBadCodePtr(reinterpret_cast<FARPROC>(cb))) return DDERR_INVALIDPARAMS;
    Guard l;
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
    rgb(16, 0xF800, 0x07E0, 0x001F, 0);
    rgb(16, 0x0F00, 0x00F0, 0x000F, 0xF000);
    rgb(32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000);
    rgb(32, 0xFF0000, 0xFF00, 0xFF, 0);
    {  // palettized: converted through the palette on the way to the GPU
        DDPIXELFORMAT f = {};
        f.dwSize = sizeof(f);
        f.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        f.dwRGBBitCount = 8;
        list.push_back(f);
    }
    if (version >= 3 && kind == kHAL) {  // DXTn: never for version 1/2 devices
        const GpuCaps c = GpuGetCaps();
        for (int i = 0; i < 5; i++) {
            if (!c.dxt[i]) continue;
            DDPIXELFORMAT f = {};
            f.dwSize = sizeof(f);
            f.dwFlags = DDPF_FOURCC;
            f.dwFourCC = MAKEFOURCC('D', 'X', 'T', '1' + i);
            list.push_back(f);
        }
    }
    if (version < 3 && list.size() > 10) list.resize(10);
    for (DDPIXELFORMAT& f : list) {
        HRESULT r;
        if (v3) {
            r = reinterpret_cast<LPD3DENUMPIXELFORMATSCALLBACK>(cb)(&f, ctxp);
        } else {
            DDSURFACEDESC d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_PIXELFORMAT;
            d.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
            d.ddpfPixelFormat = f;
            r = reinterpret_cast<LPD3DENUMTEXTUREFORMATSCALLBACK>(cb)(&d, ctxp);
        }
        if (r != D3DENUMRET_OK) break;
    }
    return D3D_OK;
}

HRESULT Device::GetStatsI(D3DSTATS* s) {
    if (version == 3) return E_NOTIMPL;
    if (!s || s->dwSize != sizeof(D3DSTATS)) return DDERR_INVALIDPARAMS;
    Guard l;
    *s = stats;
    return D3D_OK;
}

HRESULT Device::F1::GetDirect3D(IDirect3D** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    if (!o->d3d) return DDERR_INVALIDOBJECT;
    return o->d3d->InnerQuery(IID_IDirect3D, reinterpret_cast<void**>(out));
}
HRESULT Device::F2::GetDirect3D(IDirect3D2** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    if (!o->d3d) return DDERR_INVALIDOBJECT;
    return o->d3d->InnerQuery(IID_IDirect3D2, reinterpret_cast<void**>(out));
}
HRESULT Device::F3::GetDirect3D(IDirect3D3** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    if (!o->d3d) return DDERR_INVALIDOBJECT;
    return o->d3d->InnerQuery(IID_IDirect3D3, reinterpret_cast<void**>(out));
}

HRESULT Device::F3::ValidateDevice(LPDWORD passes) {
    if (!passes) return DDERR_INVALIDPARAMS;
    *passes = 1;
    return D3D_OK;
}

// ---------------------------------------------------------------- clip status (B §8.7)

HRESULT Device::SetClipStatusI(const D3DCLIPSTATUS* cs) {
    if (!cs) return DDERR_INVALIDPARAMS;  // XP faults
    Guard l;
    if (version >= 3 && (cs->dwFlags & D3DCLIPSTATUS_EXTENTS3)) return DDERR_INVALIDPARAMS;
    if (cs->dwFlags & D3DCLIPSTATUS_STATUS) status = cs->dwStatus;
    if (cs->dwFlags & (D3DCLIPSTATUS_EXTENTS2 | D3DCLIPSTATUS_EXTENTS3)) {
        ext[0] = cs->minx;
        ext[2] = cs->maxx;
        ext[1] = cs->miny;
        ext[3] = cs->maxy;
    }
    return D3D_OK;
}

HRESULT Device::GetClipStatusI(D3DCLIPSTATUS* cs) {
    if (!cs) return DDERR_INVALIDPARAMS;
    Guard l;
    cs->dwFlags = D3DCLIPSTATUS_EXTENTS2;
    cs->dwStatus = status;
    cs->minx = ext[0];
    cs->maxx = ext[2];
    cs->miny = ext[1];
    cs->maxy = ext[3];
    return D3D_OK;
}

// ---------------------------------------------------------------- execute buffers (B §9)

HRESULT Device::CreateExecuteBufferI(D3DEXECUTEBUFFERDESC* d, IDirect3DExecuteBuffer** out, IUnknown* outer) {
    if (!d || d->dwSize != sizeof(D3DEXECUTEBUFFERDESC) || !out) return DDERR_INVALIDPARAMS;
    if (outer) return CLASS_E_NOAGGREGATION;
    Guard l;
    *out = nullptr;
    const DWORD max = (kind == kHAL ? hal : hel).dwMaxBufferSize;
    if (!d->dwFlags || !d->dwBufferSize || (max && d->dwBufferSize > max)) return DDERR_INVALIDPARAMS;
    ExecBuf* b = new (std::nothrow) ExecBuf(this, d->dwBufferSize);
    if (!b) return E_OUTOFMEMORY;
    execBufs.push_back(b);
    *out = b;
    return D3D_OK;
}

// Execute (C §6.1): the viewport is current for the call.
HRESULT Device::ExecuteE(ExecBuf* eb, Viewport* vp, DWORD flags) {
    if (!eb) return DDERR_INVALIDOBJECT;
    Guard l;
    if (!vp) vp = cur;
    if (!vp) return DDERR_INVALIDPARAMS;  // XP faults inside its SEH frame
    if (vp->dev != this) return DDERR_INVALIDPARAMS;
    Viewport* prev = cur;
    cur = vp;  // no reference
    if (downloaded != vp->id) {
        const HRESULT hr = Download(vp);
        if (FAILED(hr)) {
            cur = prev;
            return hr;
        }
    }
    // XP leaves the viewport current, unreferenced, on the errors below; restored here (a
    // later SetCurrentViewport would release a reference never taken).
    HRESULT hr = eb->dev != this ? DDERR_INVALIDPARAMS : eb->locked ? D3DERR_EXECUTE_LOCKED : ExecuteI(eb, flags);
    cur = prev;
    return hr;
}

HRESULT Device::F1::Execute(IDirect3DExecuteBuffer* b, IDirect3DViewport* v, DWORD flags) {
    return o->ExecuteE(static_cast<ExecBuf*>(b), VpFrom(v), flags);
}

// Picking isn't emulated: no records (XP ignores ExecutePick's result too).
HRESULT Device::F1::Pick(IDirect3DExecuteBuffer* b, IDirect3DViewport* v, DWORD, D3DRECT* r) {
    if (!b || !v || !r) return DDERR_INVALIDOBJECT;
    Guard l;
    ExecBuf* eb = static_cast<ExecBuf*>(b);
    if (eb->dev != o || VpFrom(v)->dev != o) return DDERR_INVALIDPARAMS;
    if (eb->locked) return D3DERR_EXECUTE_LOCKED;
    o->pickCount = 0;
    return D3D_OK;
}

HRESULT Device::F1::GetPickRecords(DWORD* n, D3DPICKRECORD*) {
    if (!n) return DDERR_INVALIDPARAMS;  // XP writes it unchecked
    Guard l;
    *n = o->pickCount;
    return D3D_OK;
}

// ---------------------------------------------------------------- draws (B §8)

static const DWORD kVertexTypeFvf[4] = {0, D3DFVF_VERTEX, D3DFVF_LVERTEX, D3DFVF_TLVERTEX};

HRESULT Device::F2::DrawPrimitive(D3DPRIMITIVETYPE t, D3DVERTEXTYPE vt, void* v, DWORD n, DWORD flags) {
    if (DWORD(vt) < 1 || DWORD(vt) > 3) return DDERR_INVALIDPARAMS;  // XP reads past its table
    Guard l;
    return o->DrawCommon(t, kVertexTypeFvf[vt], static_cast<const uint8_t*>(v), 32, n, nullptr, 0, flags, nullptr);
}

HRESULT Device::F2::DrawIndexedPrimitive(D3DPRIMITIVETYPE t, D3DVERTEXTYPE vt, void* v, DWORD n, WORD* idx, DWORD ni,
                                         DWORD flags) {
    if (DWORD(vt) < 1 || DWORD(vt) > 3) return DDERR_INVALIDPARAMS;
    Guard l;
    return o->DrawCommon(t, kVertexTypeFvf[vt], static_cast<const uint8_t*>(v), 32, n, idx, ni, flags, nullptr);
}

HRESULT Device::F3::DrawPrimitive(D3DPRIMITIVETYPE t, DWORD fvf, void* v, DWORD n, DWORD flags) {
    Guard l;
    return o->DrawCommon(t, fvf, static_cast<const uint8_t*>(v), FvfSize6(fvf), n, nullptr, 0, flags, nullptr);
}

HRESULT Device::F3::DrawIndexedPrimitive(D3DPRIMITIVETYPE t, DWORD fvf, void* v, DWORD n, WORD* idx, DWORD ni,
                                         DWORD flags) {
    Guard l;
    return o->DrawCommon(t, fvf, static_cast<const uint8_t*>(v), FvfSize6(fvf), n, idx, ni, flags, nullptr);
}

// Strided data gathered into interleaved vertices (texture coordinates: two floats each).
static void Gather6(DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA& d, DWORD n, std::vector<uint8_t>* out) {
    const UINT stride = FvfSize6(fvf);
    out->assign(size_t(stride) * n, 0);
    const UINT posSize = (fvf & 0x0E) == D3DFVF_XYZ ? 12 : FvfSize6(fvf & 0x0E);
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
        for (DWORD t = 0; t < tex && t < D3DDP_MAXTEXCOORD; t++) copy(d.textureCoords[t], 8);
    }
}

HRESULT Device::F3::DrawPrimitiveStrided(D3DPRIMITIVETYPE t, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA* d, DWORD n,
                                         DWORD flags) {
    if ((fvf & 0x0E) == D3DFVF_XYZRHW) return D3DERR_INVALIDVERTEXTYPE;
    if (!d) return DDERR_INVALIDPARAMS;
    Guard l;
    std::vector<uint8_t> v;
    Gather6(fvf, *d, n, &v);
    return o->DrawCommon(t, fvf, v.data(), FvfSize6(fvf), n, nullptr, 0, flags, nullptr);
}

HRESULT Device::F3::DrawIndexedPrimitiveStrided(D3DPRIMITIVETYPE t, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA* d, DWORD n,
                                                WORD* idx, DWORD ni, DWORD flags) {
    if ((fvf & 0x0E) == D3DFVF_XYZRHW) return D3DERR_INVALIDVERTEXTYPE;
    if (!d) return DDERR_INVALIDPARAMS;
    Guard l;
    std::vector<uint8_t> v;
    Gather6(fvf, *d, n, &v);
    return o->DrawCommon(t, fvf, v.data(), FvfSize6(fvf), n, idx, ni, flags, nullptr);
}

HRESULT Device::DrawPrimitiveVBI(D3DPRIMITIVETYPE t, VBuf* vb, DWORD start, DWORD n, const WORD* idx, DWORD ni,
                                 DWORD flags) {
    if (!vb) return DDERR_INVALIDPARAMS;  // XP faults
    Guard l;
    if (idx) {  // no start vertex: the indices address the whole buffer
        start = 0;
        n = vb->count;
    }
    if (start > vb->count) return DDERR_INVALIDPARAMS;
    n = std::min(n, vb->count - start);
    const WORD* clipCodes = vb->clip.empty() || (flags & D3DDP_DONOTCLIP) ? nullptr : vb->clip.data() + start;
    return DrawCommon(t, vb->fvf, vb->mem.data() + size_t(start) * vb->stride, vb->stride, n, idx, ni, flags, clipCodes);
}

HRESULT Device::F3::DrawPrimitiveVB(D3DPRIMITIVETYPE t, IDirect3DVertexBuffer* vb, DWORD start, DWORD n, DWORD flags) {
    return o->DrawPrimitiveVBI(t, static_cast<VBuf*>(vb), start, n, nullptr, 0, flags);
}

HRESULT Device::F3::DrawIndexedPrimitiveVB(D3DPRIMITIVETYPE t, IDirect3DVertexBuffer* vb, WORD* idx, DWORD ni,
                                           DWORD flags) {
    if (!idx && ni) return DDERR_INVALIDPARAMS;
    static const WORD kNone = 0;
    return o->DrawPrimitiveVBI(t, static_cast<VBuf*>(vb), 0, 0, idx ? idx : &kNone, ni, flags);
}

// ---- Begin / Vertex / Index / End (B §8.5)

HRESULT Device::BeginI(D3DPRIMITIVETYPE t, DWORD fvf, const void* verts, DWORD n, DWORD flags, bool indexed) {
    Guard l;
    const HRESULT hr = CheckViewport();
    if (FAILED(hr)) return hr;
    inBegin = true;  // no check that one is open already
    beginType = t;
    beginFvf = fvf;
    beginFlags = flags;
    beginIndexed = indexed;
    beginVerts = static_cast<const uint8_t*>(verts);
    beginCount = n;
    batchV.clear();
    batchI.clear();
    return D3D_OK;
}

HRESULT Device::VertexI(const void* v) {
    Guard l;
    if (!inBegin || beginIndexed || !v) return D3D_OK;  // XP writes through a stale pointer
    const UINT size = FvfSize6(beginFvf);
    const auto* p = static_cast<const uint8_t*>(v);
    batchV.insert(batchV.end(), p, p + size);
    return D3D_OK;
}

HRESULT Device::IndexI(WORD i) {
    Guard l;
    if (!inBegin || !beginIndexed) return D3D_OK;
    batchI.push_back(i);
    return D3D_OK;
}

HRESULT Device::EndI(DWORD) {
    Guard l;
    const bool was = inBegin;
    inBegin = false;
    if (!was) return DDERR_INVALIDPARAMS;
    const UINT stride = FvfSize6(beginFvf);
    if (beginIndexed) {
        if (!PrimCount(beginType, DWORD(batchI.size()))) return DDERR_INVALIDPARAMS;
        return DrawCommon(beginType, beginFvf, beginVerts, stride, beginCount, batchI.data(), DWORD(batchI.size()),
                          beginFlags, nullptr);
    }
    const DWORD n = stride ? DWORD(batchV.size() / stride) : 0;
    if (!PrimCount(beginType, n)) return DDERR_INVALIDPARAMS;
    return DrawCommon(beginType, beginFvf, batchV.data(), stride, n, nullptr, 0, beginFlags, nullptr);
}

HRESULT Device::F2::Begin(D3DPRIMITIVETYPE t, D3DVERTEXTYPE vt, DWORD flags) {
    if (DWORD(vt) < 1 || DWORD(vt) > 3) return DDERR_INVALIDPARAMS;
    return o->BeginI(t, kVertexTypeFvf[vt], nullptr, 0, flags, false);
}
HRESULT Device::F2::BeginIndexed(D3DPRIMITIVETYPE t, D3DVERTEXTYPE vt, void* v, DWORD n, DWORD flags) {
    if (DWORD(vt) < 1 || DWORD(vt) > 3) return DDERR_INVALIDPARAMS;
    return o->BeginI(t, kVertexTypeFvf[vt], v, n, flags, true);
}
HRESULT Device::F3::Begin(D3DPRIMITIVETYPE t, DWORD fvf, DWORD flags) {
    return o->BeginI(t, fvf, nullptr, 0, flags, false);
}
HRESULT Device::F3::BeginIndexed(D3DPRIMITIVETYPE t, DWORD fvf, void* v, DWORD n, DWORD flags) {
    return o->BeginI(t, fvf, v, n, flags, true);
}

// ComputeSphereVisibility: DX6 answers in D3DVIS_* flags (Wine's test_compute_sphere_visibility),
// per frustum plane inside, intersecting or outside, from the sphere's distance to the planes
// of world x view x projection.
HRESULT Device::ComputeSphereVisibilityI(const D3DVECTOR* c, const D3DVALUE* r, DWORD n, DWORD* ret) {
    if (!c || !r || !ret) return DDERR_INVALIDPARAMS;
    Guard l;
    const D3DMATRIX m = Mul(Mul(world, view), proj);
    const float col[4][4] = {{m._11, m._21, m._31, m._41}, {m._12, m._22, m._32, m._42},
                             {m._13, m._23, m._33, m._43}, {m._14, m._24, m._34, m._44}};
    float planes[6][4];
    for (int k = 0; k < 4; k++) {
        planes[0][k] = col[3][k] + col[0][k];  // left
        planes[1][k] = col[3][k] - col[0][k];  // right
        planes[2][k] = col[3][k] - col[1][k];  // top
        planes[3][k] = col[3][k] + col[1][k];  // bottom
        planes[4][k] = col[2][k];              // near
        planes[5][k] = col[3][k] - col[2][k];  // far
    }
    for (auto& p : planes) {
        const float len = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (len > 0)
            for (float& v : p) v /= len;
    }
    for (DWORD s = 0; s < n; s++) {
        DWORD res = 0;
        bool out = false, cross = false;
        for (int k = 0; k < 6; k++) {
            const float* p = planes[k];
            const float dist = p[0] * c[s].x + p[1] * c[s].y + p[2] * c[s].z + p[3];
            if (dist < -r[s]) {
                res |= 8u << (2 * k);
                out = true;
            } else if (dist <= r[s]) {
                res |= 4u << (2 * k);
                cross = true;
            }
        }
        ret[s] = res | (out ? D3DVIS_OUTSIDE_FRUSTUM : cross ? D3DVIS_INTERSECT_FRUSTUM : 0);
    }
    return D3D_OK;
}

}  // namespace d6
}  // namespace dd2d9
