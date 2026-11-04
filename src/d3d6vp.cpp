// dd2d9's Direct3D 3-6: viewports (C §1).
#include "d3d6int.h"

#include <new>

namespace dd2d9 {
namespace d6 {

// The reference slot of the interface pointer a game gave (XP releases through it).
static int SlotOfIface(Surface* s, void* iface) {
    for (int v : {1, 2, 3, 4, 7})
        if (s->Iface(v) == iface) return Surface::SlotFor(v);
    return Surface::kRefV1;
}

Viewport* VpFrom(IUnknown* p) { return p ? static_cast<Viewport*>(reinterpret_cast<IDirect3DViewport3*>(p)) : nullptr; }

// UpdateViewportCache (C §1.6): nothing for degenerate data (the previous mapping stays).
bool MakeMap(const D3DVIEWPORT2& v, VpMap* m) {
    if (!v.dwWidth || !v.dwHeight || v.dvClipWidth == 0.0f || v.dvClipHeight == 0.0f || v.dvMaxZ - v.dvMinZ == 0.0f)
        return false;
    m->valid = true;
    m->X = float(v.dwX);
    m->Y = float(v.dwY);
    m->W = float(v.dwWidth);
    m->H = float(v.dwHeight);
    m->cx = v.dvClipX;
    m->cy = v.dvClipY;
    m->cw = v.dvClipWidth;
    m->ch = v.dvClipHeight;
    m->zmin = v.dvMinZ;
    m->zmax = v.dvMaxZ;
    // x' = (x - cx w)/cw, y' = y/ch + w (1 - cy/ch), z' = (z - zmin w)/(zmax - zmin), w' = w.
    const float dz = v.dvMaxZ - v.dvMinZ;
    D3DMATRIX& c = m->mclip;
    c = kIdentity;
    c._11 = 1.0f / v.dvClipWidth;
    c._41 = -v.dvClipX / v.dvClipWidth;
    c._22 = 1.0f / v.dvClipHeight;
    c._42 = 1.0f - v.dvClipY / v.dvClipHeight;
    c._33 = 1.0f / dz;
    c._43 = -v.dvMinZ / dz;
    return true;
}

Viewport::Viewport(D3D* d3d) : d3d_(d3d), id(d3d->nextViewportId++) { data.dwSize = sizeof(D3DVIEWPORT2); }

Viewport::~Viewport() {
    for (Light* l : lights) {
        l->vp = nullptr;
        l->Release();
    }
    lights.clear();
    g_lightGen++;
    if (dev) dev->RemoveViewport(this);
    if (d3d_) {
        auto it = std::find(d3d_->viewports.begin(), d3d_->viewports.end(), this);
        if (it != d3d_->viewports.end()) d3d_->viewports.erase(it);
    }
    // XP never releases the background depth surface; one set through SetBackgroundDepth2
    // (which took a reference) is released here (C §For dd2d9 6).
    if (bgDepth && bgDepthSlot >= 0) bgDepth->Release(bgDepthSlot);
}

void Viewport::Orphan() {
    if (d3d_) {
        auto it = std::find(d3d_->viewports.begin(), d3d_->viewports.end(), this);
        if (it != d3d_->viewports.end()) d3d_->viewports.erase(it);
    }
    d3d_ = nullptr;
}

HRESULT Viewport::QueryInterface(REFIID r, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirect3DViewport) || IsEqualGUID(r, IID_IDirect3DViewport2) ||
        IsEqualGUID(r, IID_IDirect3DViewport3)) {
        AddRef();
        *out = static_cast<IDirect3DViewport3*>(this);
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG Viewport::Release() {
    const LONG r = InterlockedDecrement(&ref_);
    if (r == 0) {
        Guard l;
        delete this;
    }
    return ULONG(std::max<LONG>(r, 0));
}

// ---- data (C §1.3-1.5)

HRESULT Viewport::SetViewport2(D3DVIEWPORT2* d) {
    if (!d || d->dwSize != sizeof(D3DVIEWPORT2)) return DDERR_INVALIDPARAMS;
    Guard l;
    if (!dev) return D3DERR_VIEWPORTHASNODEVICE;
    if (dev->version >= 2 && dev->rt) {  // version-1 devices take anything
        const DWORD w = DWORD(dev->rt->W()), h = DWORD(dev->rt->H());
        if (d->dvClipWidth == 0.0f || d->dvClipHeight == 0.0f || d->dvMinZ == d->dvMaxZ || d->dwX > w || d->dwY > h ||
            d->dwWidth > w - d->dwX || d->dwHeight > h - d->dwY)  // (not dwX + dwWidth: it can wrap)
            return DDERR_INVALIDPARAMS;
    }
    data = *d;
    dataSet = true;
    if (dev->downloaded == id) return dev->Download(this);
    return D3D_OK;
}

HRESULT Viewport::SetViewport(D3DVIEWPORT* d) {
    if (!d || d->dwSize != sizeof(D3DVIEWPORT)) return DDERR_INVALIDPARAMS;
    D3DVIEWPORT2 v = {};
    v.dwSize = sizeof(v);
    v.dwX = d->dwX;
    v.dwY = d->dwY;
    v.dwWidth = d->dwWidth;
    v.dwHeight = d->dwHeight;
    if (d->dvScaleX != 0.0f && d->dvScaleY != 0.0f) {
        v.dvClipWidth = float(d->dwWidth) / d->dvScaleX;
        v.dvClipHeight = float(d->dwHeight) / d->dvScaleY;
    }
    v.dvClipX = -v.dvClipWidth / 2;
    v.dvClipY = v.dvClipHeight / 2;
    v.dvMinZ = 0.0f;
    v.dvMaxZ = 1.0f;
    return SetViewport2(&v);
}

HRESULT Viewport::GetViewport2(D3DVIEWPORT2* d) {
    if (!d || d->dwSize != sizeof(D3DVIEWPORT2)) return DDERR_INVALIDPARAMS;
    Guard l;
    if (!dataSet) return D3DERR_VIEWPORTDATANOTSET;
    *d = data;
    return D3D_OK;
}

// Written even when GetViewport2 fails (XP: stack garbage; zeros here).
HRESULT Viewport::GetViewport(D3DVIEWPORT* d) {
    if (!d || d->dwSize != sizeof(D3DVIEWPORT)) return DDERR_INVALIDPARAMS;
    Guard l;
    D3DVIEWPORT out = {};
    out.dwSize = sizeof(out);
    const HRESULT hr = dataSet ? D3D_OK : D3DERR_VIEWPORTDATANOTSET;
    if (dataSet) {
        out.dwX = data.dwX;
        out.dwY = data.dwY;
        out.dwWidth = data.dwWidth;
        out.dwHeight = data.dwHeight;
        out.dvScaleX = float(data.dwWidth) / data.dvClipWidth;
        out.dvScaleY = float(data.dwHeight) / data.dvClipHeight;
        out.dvMaxX = data.dvClipX + data.dvClipWidth;
        out.dvMaxY = data.dvClipY;
        out.dvMinZ = 0.0f;
        out.dvMaxZ = 1.0f;
    }
    *d = out;
    return hr;
}

// ---- TransformVertices (C §1.7)

HRESULT Viewport::TransformVertices(DWORD n, D3DTRANSFORMDATA* d, DWORD flags, DWORD* offscreen) {
    if (!d || d->dwSize != sizeof(D3DTRANSFORMDATA)) return DDERR_INVALIDPARAMS;
    if (!(flags & 3)) return DDERR_INVALIDPARAMS;
    Guard l;
    if (!dev) return DDERR_INVALIDPARAMS;  // XP faults inside its SEH frame
    if (!dataSet) return D3DERR_VIEWPORTDATANOTSET;
    const bool clipped = (flags & 3) == 1;
    if (n && (!d->lpIn || !d->lpOut || (clipped && !d->lpHOut))) return DDERR_INVALIDPARAMS;
    VpMap m = dev->map;
    MakeMap(data, &m);
    const D3DMATRIX wvp = Mul(Mul(dev->world, dev->view), dev->proj);
    const D3DMATRIX M = Mul(wvp, m.mclip);
    const float adj = dev->kind == kHAL && dev->hal.dvExtentsAdjust > 0 ? dev->hal.dvExtentsAdjust : 1.0f;
    float ex[4] = {float(d->drExtent.x1), float(d->drExtent.y1), float(d->drExtent.x2), float(d->drExtent.y2)};
    DWORD andCodes = 0xFFFFFFFF, orCodes = 0;
    bool outsideVp = false;
    for (DWORD i = 0; i < n; i++) {
        const auto* in = reinterpret_cast<const DWORD*>(static_cast<const uint8_t*>(d->lpIn) + size_t(i) * d->dwInSize);
        auto* o = reinterpret_cast<D3DTLVERTEX*>(static_cast<uint8_t*>(d->lpOut) + size_t(i) * d->dwOutSize);
        const float x = F(in[0]), y = F(in[1]), z = F(in[2]);
        const float px = x * M._11 + y * M._21 + z * M._31 + M._41;
        const float py = x * M._12 + y * M._22 + z * M._32 + M._42;
        const float pz = x * M._13 + y * M._23 + z * M._33 + M._43;
        const float pw = x * M._14 + y * M._24 + z * M._34 + M._44;
        DWORD* od = reinterpret_cast<DWORD*>(&o->color);
        od[0] = in[4];  // DWORDs 4-7 copied as they are: colour, specular, tu, tv
        od[1] = in[5];
        od[2] = in[6];
        od[3] = in[7];
        const float rhw = pw != 0.0f ? 1.0f / pw : 2097152.0f;
        WORD code = 0;
        if (clipped) {
            if (px < 0) code |= D3DCLIP_LEFT;
            if (px > pw) code |= D3DCLIP_RIGHT;
            if (py > pw) code |= D3DCLIP_TOP;
            if (py < 0) code |= D3DCLIP_BOTTOM;
            if (pz < 0) code |= D3DCLIP_FRONT;
            if (pz > pw) code |= D3DCLIP_BACK;
            D3DHVERTEX& h = d->lpHOut[i];
            h.dwFlags = code;
            // Back in the viewport's own clip coordinates (the inverse Mclip).
            h.hx = pw != 0.0f ? px * m.cw + m.cx * pw : 0.0f;
            h.hy = (py - pw * (1.0f - m.cy / m.ch)) * m.ch;
            h.hz = pz * (m.zmax - m.zmin) + m.zmin * pw;
            andCodes &= code;
            orCodes |= code;
        }
        o->rhw = rhw;
        if (!code) {
            o->sx = m.X + m.W * px * rhw;
            o->sy = m.Y + m.H - m.H * py * rhw;
            o->sz = pz * rhw;
            ex[0] = std::min(ex[0], o->sx);
            ex[1] = std::min(ex[1], o->sy);
            ex[2] = std::max(ex[2], o->sx);
            ex[3] = std::max(ex[3], o->sy);
            if (o->sx < m.X || o->sx > m.X + m.W || o->sy < m.Y || o->sy > m.Y + m.H) outsideVp = true;
        }
    }
    // Version 1, unclipped: outputs clamped to the viewport when the extent left it.
    if (!clipped && dev->version == 1 && outsideVp) {
        for (DWORD i = 0; i < n; i++) {
            auto* o = reinterpret_cast<D3DTLVERTEX*>(static_cast<uint8_t*>(d->lpOut) + size_t(i) * d->dwOutSize);
            o->sx = std::clamp(o->sx, m.X, m.X + m.W);
            o->sy = std::clamp(o->sy, m.Y, m.Y + m.H);
        }
    }
    if (clipped) {
        d->dwClipIntersection = n ? andCodes : 0;
        d->dwClipUnion = orCodes;
    }
    // The extent: widened, clamped to the viewport, rounded, one more pixel each side.
    ex[0] = std::max(ex[0] - adj, m.X);
    ex[1] = std::max(ex[1] - adj, m.Y);
    ex[2] = std::min(ex[2] + adj, m.X + m.W);
    ex[3] = std::min(ex[3] + adj, m.Y + m.H);
    const LONG vx1 = LONG(m.X), vy1 = LONG(m.Y), vx2 = LONG(m.X + m.W), vy2 = LONG(m.Y + m.H);
    d->drExtent.x1 = std::max(LONG(floorf(ex[0] + 0.5f)) - 1, vx1);
    d->drExtent.y1 = std::max(LONG(floorf(ex[1] + 0.5f)) - 1, vy1);
    d->drExtent.x2 = std::min(LONG(floorf(ex[2] + 0.5f)) + 1, vx2);
    d->drExtent.y2 = std::min(LONG(floorf(ex[3] + 0.5f)) + 1, vy2);
    if (!offscreen) return DDERR_INVALIDPARAMS;  // XP faults after the work
    *offscreen = clipped ? (n ? andCodes : 0xFFFFFFFF) : 0;
    return D3D_OK;
}

// ---- background (C §1.9, §1.10)

HRESULT Viewport::SetBackground(D3DMATERIALHANDLE h) {
    if (!h) return DDERR_INVALIDPARAMS;
    Guard l;
    bg = h;
    bgSet = true;
    return D3D_OK;
}

HRESULT Viewport::GetBackground(D3DMATERIALHANDLE* h, WINBOOL* valid) {
    if (!h) return DDERR_INVALIDPARAMS;
    Guard l;
    *h = bgSet ? bg : 0;
    if (valid) *valid = bgSet;
    return D3D_OK;
}

// The previous surface is released and the new one kept without a reference (sic).
HRESULT Viewport::SetBackgroundDepth(IDirectDrawSurface* s) {
    Guard l;
    if (bgDepth) bgDepth->Release(bgDepthSlot >= 0 ? bgDepthSlot : SlotOfIface(bgDepth, bgDepthIface));
    bgDepth = s ? Surface::From(s) : nullptr;
    bgDepthIface = s;
    bgDepthSlot = -1;
    return D3D_OK;
}

HRESULT Viewport::SetBackgroundDepth2(IDirectDrawSurface4* s) {
    Guard l;
    Surface* n = nullptr;
    if (s) {
        n = Surface::From(s);
        if (!n) return DDERR_INVALIDPARAMS;
        n->AddRef(Surface::kRefV1);  // QueryInterface(IID_IDirectDrawSurface)
    }
    if (bgDepth) bgDepth->Release(bgDepthSlot >= 0 ? bgDepthSlot : SlotOfIface(bgDepth, bgDepthIface));
    bgDepth = n;
    bgDepthIface = n ? n->Iface(1) : nullptr;
    bgDepthSlot = n ? Surface::kRefV1 : -1;
    return D3D_OK;
}

HRESULT Viewport::GetBackgroundDepth(IDirectDrawSurface** s, WINBOOL* valid) {
    if (!s) return DDERR_INVALIDPARAMS;
    Guard l;
    *s = static_cast<IDirectDrawSurface*>(bgDepth ? bgDepthIface : nullptr);  // no reference
    if (valid) *valid = bgDepth != nullptr;
    return D3D_OK;
}

HRESULT Viewport::GetBackgroundDepth2(IDirectDrawSurface4** s, WINBOOL* valid) {
    if (!s) return DDERR_INVALIDPARAMS;
    Guard l;
    *s = nullptr;
    if (bgDepth) {
        bgDepth->AddRef(Surface::kRefV4);
        *s = static_cast<IDirectDrawSurface4*>(bgDepth->Iface(4));
    }
    if (valid) *valid = *s != nullptr;
    return D3D_OK;
}

// ---- clearing (C §1.11-1.13)

HRESULT Viewport::ProcessRects(DWORD n, const D3DRECT* rects, std::vector<RECT>* out) const {
    if (!rects) return D3D_OK;  // nothing cleared (Wine's test_clear_rect_count on Windows)
    // To the viewport. d3dim clips a version 1 device's rectangles to the render target
    // only, but the drivers then held the clear to the viewport (Wine's test_clear; AMD's
    // didn't).
    // (A version 1 device's viewport is taken unchecked: kept to 32 bits signed.)
    auto edge = [](DWORD a, DWORD b = 0) { return LONG(std::min<uint64_t>(uint64_t(a) + b, 0x7FFFFFFF)); };
    const RECT clip = {edge(data.dwX), edge(data.dwY), edge(data.dwX, data.dwWidth), edge(data.dwY, data.dwHeight)};
    for (DWORD i = 0; i < n; i++) {
        RECT r = {rects[i].x1, rects[i].y1, rects[i].x2, rects[i].y2}, c;
        if (IntersectRect(&c, &r, &clip)) out->push_back(c);
    }
    return D3D_OK;
}

HRESULT Viewport::Clear(DWORD n, D3DRECT* rects, DWORD flags) {
    Guard l;
    if (!dev) return D3DERR_VIEWPORTHASNODEVICE;
    if ((flags & D3DCLEAR_ZBUFFER) && !dev->z) {
        flags &= ~DWORD(D3DCLEAR_ZBUFFER);
        if (!flags) return D3D_OK;
    }
    if (!(flags & 3)) return DDERR_INVALIDPARAMS;
    if (dev->downloaded != id) {
        const HRESULT hr = dev->Download(this);
        if (FAILED(hr)) return hr;
    }
    std::vector<RECT> list;
    HRESULT hr = ProcessRects(n, rects, &list);
    if (FAILED(hr)) return hr;
    if (list.empty()) return D3D_OK;  // nothing is cleared
    DWORD color = 0;
    Surface* tex = nullptr;
    if (bgSet) {
        if (MatHandle* m = FindMatHandle(bg)) {
            const D3DCOLORVALUE& c = m->data.diffuse;
            // Truncated and not clamped: a value over 1.0 spills into the next byte.
            auto b = [](float v) { return DWORD(LONG(v * 255.0f)); };
            color = (b(c.a) << 24) | (b(c.r) << 16) | (b(c.g) << 8) | b(c.b);
            tex = SurfaceForHandle(m->data.hTexture);
        }
    }
    if (tex && (flags & D3DCLEAR_TARGET)) {
        FillRectsTex(dev, list, tex, this);
        flags &= ~DWORD(D3DCLEAR_TARGET);
        if (!(flags & D3DCLEAR_ZBUFFER)) return D3D_OK;
    }
    ClearRects(dev, list, flags & 7, color, 1.0f, 0);
    return D3D_OK;
}

HRESULT Viewport::Clear2(DWORD n, D3DRECT* rects, DWORD flags, D3DCOLOR color, D3DVALUE z, DWORD stencil) {
    Guard l;
    if (!dev) return D3DERR_VIEWPORTHASNODEVICE;
    if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && !dev->z) return D3DERR_ZBUFFER_NOTPRESENT;
    if ((flags & D3DCLEAR_STENCIL) && !dev->z->buf->fmt.a) return D3DERR_STENCILBUFFER_NOTPRESENT;
    if (!(flags & 7)) return DDERR_INVALIDPARAMS;
    if ((flags & D3DCLEAR_ZBUFFER) && bgDepth) return DDERR_INVALIDPARAMS;
    if (dev->downloaded != id) {
        const HRESULT hr = dev->Download(this);
        if (FAILED(hr)) return hr;
    }
    std::vector<RECT> list;
    const HRESULT hr = ProcessRects(n, rects, &list);
    if (FAILED(hr)) return hr;
    if (!list.empty()) ClearRects(dev, list, flags & 7, color, z, stencil);
    return D3D_OK;
}

// ---- lights (C §1.14)

static Light* LightFrom(IDirect3DLight* p) { return static_cast<Light*>(p); }

HRESULT Viewport::AddLight(IDirect3DLight* p) {
    if (!p) return DDERR_INVALIDPARAMS;
    Guard l;
    Light* light = LightFrom(p);
    if (light->vp) return D3DERR_LIGHTHASVIEWPORT;
    lights.push_back(light);
    light->vp = this;
    lightsDirty = true;
    g_lightGen++;
    light->AddRef();
    return D3D_OK;
}

HRESULT Viewport::DeleteLight(IDirect3DLight* p) {
    if (!p) return DDERR_INVALIDPARAMS;
    Guard l;
    Light* light = LightFrom(p);
    if (light->vp != this) return D3DERR_LIGHTNOTINTHISVIEWPORT;
    lights.erase(std::find(lights.begin(), lights.end(), light));
    light->vp = nullptr;
    lightsDirty = true;
    g_lightGen++;
    light->Release();
    return D3D_OK;
}

HRESULT Viewport::NextLight(IDirect3DLight* ref, IDirect3DLight** out, DWORD flags) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    Guard l;
    Light* r = LightFrom(ref);
    if (flags & D3DNEXT_NEXT) {
        if (!r) return DDERR_INVALIDPARAMS;
        if (r->vp != this) return D3DERR_LIGHTNOTINTHISVIEWPORT;
    }
    Light* found = nullptr;
    if (flags == D3DNEXT_NEXT) {
        auto it = std::find(lights.begin(), lights.end(), r);
        if (it != lights.end() && ++it != lights.end()) found = *it;
    } else if (flags == D3DNEXT_HEAD) {
        if (!lights.empty()) found = lights.front();
    } else if (flags == D3DNEXT_TAIL) {
        if (!lights.empty()) found = lights.back();
    } else {
        return DDERR_INVALIDPARAMS;
    }
    if (found) {
        found->AddRef();
        *out = found;
    }
    return D3D_OK;
}

}  // namespace d6
}  // namespace dd2d9
