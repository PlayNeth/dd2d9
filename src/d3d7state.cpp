// dd2d9's Direct3D 7: the device's states (render states, texture stages, textures,
// transforms, viewport, material, lights, clip planes), as the game sets and gets them and as
// they go to Direct3D 9, and the texture formats it lists (d3d7int.h).
#include "d3d7int.h"

#include <cfloat>
#include <cmath>
#include <utility>

namespace dd2d9 {
namespace d7 {

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
    ClearDirty();
    keyAlphaSent_ = false;
    for (DWORD s = 0; s < 256; s++) PushRS(s);
    for (DWORD i = 0; i < 8; i++) {
        for (DWORD t = 1; t < 25; t++) PushTSS(i, t);
        ctx_.SetTexture(i, tex_[i]);
    }
    for (DWORD t : {1u, 2u, 3u, 4u, 5u, 6u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u}) PushTransform(t);
    ctx_.SetViewport(vp_.dwX, vp_.dwY, vp_.dwWidth, vp_.dwHeight, vp_.dvMinZ, vp_.dvMaxZ);
    ctx_.SetMaterial(&mat_);
    for (DWORD i = 0; i < lights_.size(); i++) PushLight(i);
    SendLightEnables(true);
    for (DWORD i = 0; i < std::min<DWORD>(caps_.wMaxUserClipPlanes, kMaxPlanes); i++) ctx_.SetClipPlane(i, planes_[i]);
    ctx_.SetTargets(rt_, z_);
}

void Device7::ClearDirty() {
    dirty_ = vpDirty_ = matDirty_ = false;
    std::fill(std::begin(rsDirty_), std::end(rsDirty_), 0u);
    std::fill(std::begin(tssDirty_), std::end(tssDirty_), 0u);
    texDirty_ = xformDirty_ = planesDirty_ = 0;
    if (lightsDirty_)
        for (Light& l : lights_) l.dirty = false;
    lightsDirty_ = false;
}

// What the setters stored since the last Use, to D3D9.
void Device7::Flush() {
    dirty_ = false;
    for (DWORD w = 0; w < 8; w++)
        for (DWORD m = std::exchange(rsDirty_[w], 0u); m; m &= m - 1) PushRS(w * 32 + DWORD(__builtin_ctz(m)));
    for (DWORD i = 0; i < 8; i++)
        for (DWORD m = std::exchange(tssDirty_[i], 0u); m; m &= m - 1) PushTSS(i, DWORD(__builtin_ctz(m)));
    for (DWORD m = std::exchange(texDirty_, 0u); m; m &= m - 1) {
        const DWORD i = DWORD(__builtin_ctz(m));
        ctx_.SetTexture(i, tex_[i]);
        if (i == 0) PushKeyAlpha();
    }
    const bool worldMoved = (xformDirty_ & (1u << D3DTRANSFORMSTATE_WORLD)) != 0;
    for (DWORD m = std::exchange(xformDirty_, 0u); m; m &= m - 1) PushTransform(DWORD(__builtin_ctz(m)));
    if (std::exchange(vpDirty_, false)) ctx_.SetViewport(vp_.dwX, vp_.dwY, vp_.dwWidth, vp_.dwHeight, vp_.dvMinZ, vp_.dvMaxZ);
    if (std::exchange(matDirty_, false)) ctx_.SetMaterial(&mat_);
    if (std::exchange(lightsDirty_, false)) {
        for (DWORD i = 0; i < lights_.size(); i++)
            if (std::exchange(lights_[i].dirty, false)) PushLight(i);
        SendLightEnables(false);
    } else if (lightsOver_ && worldMoved) {
        SendLightEnables(false);  // (another object: the lights that matter there)
    }
    for (DWORD m = std::exchange(planesDirty_, 0u); m; m &= m - 1) {
        const DWORD i = DWORD(__builtin_ctz(m));
        if (i < caps_.wMaxUserClipPlanes) ctx_.SetClipPlane(i, planes_[i]);
    }
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
        PushKeyAlpha();
        break;
    case 27:  // ALPHABLENDENABLE: the colour key's emulation depends on it
        ctx_.SetRenderState(27, v);
        PushKeyAlpha();
        break;
    case 47: {  // ZBIAS: D3D9's DEPTHBIAS (195), a float
        const float bias = ZBiasToDepthBias(v);
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
    case 8: case 9: case 14: case 16: case 19: case 20: case 22: case 23: case 26: case 28: case 29:
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

// COLORKEYENABLE: keyed texels were uploaded transparent; an alpha test drops them, with
// stage 0's alpha op changed where needed (d3dxlate.h: KeyedAlphaState).
void Device7::PushKeyAlpha() {
    ctx_.SetColorKeyEnable(rs_[41] == TRUE);
    keySent_ = rs_[41] == TRUE && tex_[0] && tex_[0]->hasCk[kCkSrcBlt];
    const DWORD* const later[7] = {tss_[1], tss_[2], tss_[3], tss_[4], tss_[5], tss_[6], tss_[7]};
    const KeyedAlpha k = KeyedAlphaState(rs_, tss_[0], tex_[0], Stage0AlphaUsed(later, rt_->buf->fmt.a != 0));
    const DWORD v[6] = {k.test[0], k.test[1], k.test[2], k.alphaOp, k.alphaArg1, k.alphaArg2};
    // (Every stage-0 texture change comes here: only what changed goes to D3D9.)
    for (int i = 0; i < 6; i++) {
        if (keyAlphaSent_ && keyAlphaSentV_[i] == v[i]) continue;
        if (i < 3) {
            static const DWORD kStates[3] = {15, 24, 25};
            ctx_.SetRenderState(kStates[i], v[i]);
        } else {
            ctx_.SetTextureStageState(0, D3DTSS_ALPHAOP + (i - 3), v[i]);
        }
        keyAlphaSentV_[i] = v[i];
    }
    keyAlphaSent_ = true;
}

// S §Texture stage states: some move to D3D9's sampler states, two with other values.
void Device7::PushTSS(DWORD stage, DWORD t) {
    const DWORD v = tss_[stage][t];
    DWORD sampler, v9;
    if (TssToSampler(t, v, &sampler, &v9)) {
        ctx_.SetSamplerState(stage, sampler, v9);
        return;
    }
    switch (t) {
    case D3DTSS_ADDRESS: case D3DTSS_ADDRESSU: case D3DTSS_ADDRESSV:
        ctx_.SetSamplerState(stage, 1, addrU_[stage]);
        ctx_.SetSamplerState(stage, 2, addrV_[stage]);
        break;
    case D3DTSS_COLOROP: case D3DTSS_ALPHAOP: case D3DTSS_ALPHAARG1: case D3DTSS_ALPHAARG2:
        if (stage == 0) {  // the colour key's emulation depends on these (PushKeyAlpha)
            if (t == D3DTSS_COLOROP) ctx_.SetTextureStageState(0, t, v);
            PushKeyAlpha();
            break;
        }
        ctx_.SetTextureStageState(stage, t, v);
        PushKeyAlpha();  // (whether a later stage reads stage 0's alpha)
        break;
    case D3DTSS_COLORARG1: case D3DTSS_COLORARG2:
        ctx_.SetTextureStageState(stage, t, v);
        if (stage > 0) PushKeyAlpha();
        break;
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
    ctx_.SetLight(i, &l.l);  // (which of them are enabled: SendLightEnables)
}

// The lights D3D9 has enabled. XP lit a HAL or software device's vertices itself, with any
// number of lights (dwMaxActiveLights unlimited); D3D9's hardware lights only so many at once
// (D3DCAPS9::MaxActiveLights, 8 as a rule) and refuses to enable more. With more enabled than
// that, those that light the object most are chosen: by their colour, and for point and spot
// lights their attenuation at the object's place (the world matrix's translation), none past
// their range. Chosen again when a light or the world matrix changes. An approximation: an
// object large against the lights' distances could be lit by others.
// all: D3D9's own state is unknown (a new device, another context drew): every light sent.
void Device7::SendLightEnables(bool all) {
    DWORD limit = GpuGetCaps().maxActiveLights;
    if (!limit) limit = 8;
    DWORD enabled = 0;
    for (const Light& l : lights_) enabled += l.set && l.enabled;
    lightsOver_ = enabled > limit;
    static std::vector<std::pair<float, DWORD>> ranked;  // (g_lock held)
    if (lightsOver_) {
        if (!std::exchange(lightsOverLogged_, true))
            DD2D9_LOG("Direct3D 7: %lu lights enabled, the GPU lights %lu at once: the %lu that light each object most are used",
                      enabled, limit, limit);
        ranked.clear();
        const float ox = world_._41, oy = world_._42, oz = world_._43;
        for (DWORD i = 0; i < lights_.size(); i++) {
            const Light& l = lights_[i];
            if (!l.set || !l.enabled) continue;
            const D3DLIGHT7& d = l.l;
            float score = std::max({d.dcvDiffuse.r, d.dcvDiffuse.g, d.dcvDiffuse.b, d.dcvSpecular.r, d.dcvSpecular.g,
                                    d.dcvSpecular.b, d.dcvAmbient.r, d.dcvAmbient.g, d.dcvAmbient.b});
            if (d.dltType == D3DLIGHT_POINT || d.dltType == D3DLIGHT_SPOT) {
                const float dx = d.dvPosition.x - ox, dy = d.dvPosition.y - oy, dz = d.dvPosition.z - oz;
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float att = d.dvAttenuation0 + d.dvAttenuation1 * dist + d.dvAttenuation2 * dist * dist;
                if (dist > d.dvRange) score = 0.0f;
                else if (att > 1e-6f) score /= att;
                else score *= 1e6f;
            }
            if (!(score >= 0.0f)) score = 0.0f;  // (NaN, negative colours: last)
            ranked.emplace_back(-score, i);       // strongest first; equal ones by index
        }
        std::sort(ranked.begin(), ranked.end());
    }
    // Off before on: never more enabled in D3D9 than it takes.
    for (int pass = 0; pass < 2; pass++) {
        for (DWORD i = 0; i < lights_.size(); i++) {
            Light& l = lights_[i];
            if (!l.set) continue;
            bool want = l.enabled;
            if (want && lightsOver_) {
                want = false;
                for (DWORD k = 0; k < limit && k < ranked.size(); k++) want = want || ranked[k].second == i;
            }
            if (want != (pass == 1)) continue;
            if (all || want != l.onGpu) ctx_.LightEnable(i, want);
            l.onGpu = want;
        }
    }
}

// ---- states

HRESULT Device7::SetRS(DWORD s, DWORD v) {
    if (s >= 256) return D3D_OK;  // XP writes past its array; ignored here
    if (rs_[s] == v) return D3D_OK;
    rs_[s] = v;
    rsDirty_[s / 32] |= 1u << (s % 32);
    dirty_ = true;
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
        return Record(c);
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
    if (t < 32) {  // (the rest isn't sent: PushTSS)
        tssDirty_[stage] |= 1u << t;
        dirty_ = true;
    }
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
        return Record(c);
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
    // (D3D9's context keeps the old one's pointer until then, unused: it draws only after a Use.)
    texDirty_ |= 1u << stage;
    dirty_ = true;
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
        return Record(c);
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
    xformDirty_ |= 1u << t;  // (types up to 23: Matrix)
    dirty_ = true;
}

HRESULT Device7::SetTransform(D3DTRANSFORMSTATETYPE t, D3DMATRIX* m) {
    if (!m) return DDERR_INVALIDPARAMS;  // XP faults
    Guard l;
    if (Recording()) {
        Cmd c;
        c.kind = Cmd::kXform;
        c.a = t;
        c.m = *m;
        return Record(c);
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
    vpDirty_ = dirty_ = true;
    return D3D_OK;
}

HRESULT Device7::SetViewport(D3DVIEWPORT7* vp) {
    if (!vp) return DDERR_INVALIDPARAMS;
    Guard l;
    if (Recording()) {
        Cmd c;
        c.kind = Cmd::kViewport;
        c.vp = *vp;
        return Record(c);
    }
    return SetVP(*vp);
}

void Device7::SetMat(const D3DMATERIAL7& m) {
    mat_ = m;
    if (m.dvPower > 0) pvPower_ = m.dvPower;
    matDirty_ = dirty_ = true;
}

HRESULT Device7::SetMaterial(D3DMATERIAL7* m) {
    if (!m) return DDERR_INVALIDPARAMS;
    Guard l;
    if (Recording()) {
        Cmd c;
        c.kind = Cmd::kMaterial;
        c.mat = *m;
        return Record(c);
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

// Room for light i (as XP's array, ten more than needed); false without the memory.
bool Device7::GrowLights(DWORD i) {
    if (i < lights_.size()) return true;
    try {
        lights_.resize(size_t(i) + 10);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

HRESULT Device7::SetLightI(DWORD i, const D3DLIGHT7& l) {
    if (i >= 0x10000) return E_OUTOFMEMORY;  // as XP when its array can't grow (not a C++ exception)
    if (!GrowLights(i)) return E_OUTOFMEMORY;
    lights_[i].l = l;
    lights_[i].set = true;
    lights_[i].dirty = lightsDirty_ = dirty_ = true;
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
        return Record(c);
    }
    return SetLightI(i, *l);
}

HRESULT Device7::LightEnableI(DWORD i, bool on) {
    if (i >= 0x10000) return E_OUTOFMEMORY;  // as SetLight
    if (!GrowLights(i)) return E_OUTOFMEMORY;
    Light& l = lights_[i];
    if (!l.set) {  // a default white directional light (S §LightEnable)
        l.l = {};
        l.l.dltType = D3DLIGHT_DIRECTIONAL;
        l.l.dcvDiffuse.r = l.l.dcvDiffuse.g = l.l.dcvDiffuse.b = 1.0f;
        l.l.dvDirection.z = 1.0f;
        l.set = true;
    }
    l.enabled = on;
    l.dirty = lightsDirty_ = dirty_ = true;
    return D3D_OK;
}

HRESULT Device7::LightEnable(DWORD i, WINBOOL on) {
    Guard g;
    if (Recording()) {
        Cmd c;
        c.kind = Cmd::kLightEnable;
        c.a = i;
        c.b = on != 0;
        return Record(c);
    }
    return LightEnableI(i, on != 0);
}

void Device7::SetPlane(DWORD i, const float* p) {
    if (i >= kMaxPlanes) return;
    memcpy(planes_[i], p, sizeof(planes_[i]));
    planesDirty_ |= 1u << i;
    dirty_ = true;
}

HRESULT Device7::SetClipPlane(DWORD i, D3DVALUE* p) {
    if (!p) return DDERR_INVALIDPARAMS;
    Guard g;
    if (Recording()) {
        Cmd c;
        c.kind = Cmd::kClipPlane;
        c.a = i;
        memcpy(c.plane, p, sizeof(c.plane));
        return Record(c);
    }
    SetPlane(i, p);
    return D3D_OK;
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

// No driver to ask (a deliberate difference, S §For dd2d9 17): ValidateStages.
HRESULT Device7::ValidateDevice(DWORD* passes) {
    if (!passes) return DDERR_INVALIDPARAMS;
    Guard l;
    DWORD textures = 0;
    for (DWORD s = 0; s < 8; s++)
        if (tex_[s]) textures |= 1u << s;
    return ValidateStages(tss_, textures, caps_.dwTextureOpCaps, caps_.wMaxTextureBlendStages, caps_.wMaxSimultaneousTextures, passes);
}

}  // namespace d7
}  // namespace dd2d9
