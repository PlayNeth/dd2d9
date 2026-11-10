// dd2d9's Direct3D 7: scenes, render targets, clearing, the draws (from the game's memory,
// strided, from vertex buffers, these clipped where ProcessVertices left them in clip space)
// and Load (d3d7int.h).
#include "d3d7int.h"

#include <cmath>

namespace dd2d9 {
namespace d7 {

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
        GpuGuard g(dd_->GetGpu());
        ctx_.SetTargets(rt_, z_);  // the viewport stays (S §SetRenderTarget)
    }
    // The colour key's emulation depends on whether the target has alpha (PushKeyAlpha, by
    // COLORKEYENABLE's state at the next draw).
    rsDirty_[41 / 32] |= 1u << (41 % 32);
    dirty_ = true;
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
    if (rects && !Reserve(&clipped, count)) return DDERR_OUTOFMEMORY;
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

// idx with the indices past the vertices given (n) made vertex 0 (the GPU would read past
// them); null without the memory for the copy.
const WORD* Device7::SafeIndices(const WORD* idx, DWORD ni, DWORD n) {
    for (DWORD i = 0; i < ni; i++) {
        if (idx[i] >= n) {
            if (!Reserve(&safeIdx_, ni)) return nullptr;
            safeIdx_.assign(idx, idx + ni);
            for (WORD& k : safeIdx_)
                if (k >= n) k = 0;
            return safeIdx_.data();
        }
    }
    return idx;
}

// fromVB: the vertices are a vertex buffer's (a transformed one's leave the clip status alone).
HRESULT Device7::DrawCommon(D3DPRIMITIVETYPE t, DWORD fvf, const void* v, UINT stride, DWORD n, const WORD* idx,
                            DWORD ni, bool fromVB) {
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
    // (On a T&L HAL device the driver clips, and the runtime keeps no clip status: §6.)
    const bool rhw = (fvf & 0x0E) == D3DFVF_XYZRHW;
    if (csTracking_ && kind_ != kTnLHAL && (fvf & 0x0E) && !(rhw && fromVB)) TrackClipStatus(fvf, v, stride, n);
    if (idx && !(idx = SafeIndices(idx, ni, n))) return DDERR_OUTOFMEMORY;
    // Lit vertices without a normal: XP lights them with a zero one (ambient and emissive
    // only); D3D9 (DXVK) leaves them unlit. Given a zero normal here.
    if (rs_[D3DRENDERSTATE_LIGHTING] == TRUE && !rhw && !(fvf & D3DFVF_NORMAL)) {
        const UINT posSize = FvfSize(fvf & 0x0E), newStride = stride + 12;
        // Indexed: only the vertices the indices reach (a VB draw often names the whole buffer
        // for a few triangles); those below the lowest are left zero, never read.
        DWORD lo = 0;
        if (idx) {
            WORD mn = 0xFFFF, mx = 0;
            for (DWORD i = 0; i < ni; i++) {
                mn = std::min(mn, idx[i]);
                mx = std::max(mx, idx[i]);
            }
            if (mn <= mx) lo = mn, n = DWORD(mx) + 1;  // (all below n: SafeIndices)
        }
        std::vector<uint8_t>& withNormal = withNormal_;
        if (!Reserve(&withNormal, size_t(newStride) * n)) return DDERR_OUTOFMEMORY;
        withNormal.assign(size_t(newStride) * n, 0);
        for (DWORD i = lo; i < n; i++) {
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
    if (rs_[41] == TRUE && tex_[0] && int(tex_[0]->hasCk[kCkSrcBlt]) != keySent_) PushKeyAlpha();
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
    return DrawCommon(t, fvf, v, FvfSize(fvf), n, nullptr, 0, false);
}

HRESULT Device7::DrawIndexedPrimitive(D3DPRIMITIVETYPE t, DWORD fvf, void* v, DWORD n, WORD* idx, DWORD ni, DWORD) {
    Guard l;
    HoldVB(nullptr);
    return DrawCommon(t, fvf, v, FvfSize(fvf), n, idx, ni, false);
}

// Gathers strided data into interleaved vertices, in FVF order (V §3); false without the
// memory for them.
bool Gather(DWORD fvf, const D3DDRAWPRIMITIVESTRIDEDDATA& d, DWORD n, std::vector<uint8_t>* out) {
    const UINT stride = FvfSize(fvf);
    try {
        out->assign(size_t(stride) * n + 16, 0);  // slack: see VertexBuffer7's memory
    } catch (const std::exception&) {
        return false;
    }
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
    return true;
}

HRESULT Device7::DrawPrimitiveStrided(D3DPRIMITIVETYPE t, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA* d, DWORD n, DWORD) {
    if ((fvf & 0x0E) == D3DFVF_XYZRHW) return D3DERR_INVALIDVERTEXTYPE;
    if (!d || n > 0x01000000) return DDERR_INVALIDPARAMS;  // sizes that wrap a 32-bit size_t
    Guard l;
    HoldVB(nullptr);
    if (!Gather(fvf, *d, n, &gathered_)) return DDERR_OUTOFMEMORY;
    return DrawCommon(t, fvf, gathered_.data(), FvfSize(fvf), n, nullptr, 0, false);
}

HRESULT Device7::DrawIndexedPrimitiveStrided(D3DPRIMITIVETYPE t, DWORD fvf, D3DDRAWPRIMITIVESTRIDEDDATA* d, DWORD n,
                                             WORD* idx, DWORD ni, DWORD) {
    if ((fvf & 0x0E) == D3DFVF_XYZRHW) return D3DERR_INVALIDVERTEXTYPE;
    if (!d || n > 0x01000000) return DDERR_INVALIDPARAMS;  // sizes that wrap a 32-bit size_t
    Guard l;
    HoldVB(nullptr);
    if (!Gather(fvf, *d, n, &gathered_)) return DDERR_OUTOFMEMORY;
    return DrawCommon(t, fvf, gathered_.data(), FvfSize(fvf), n, idx, ni, false);
}

// The device holds the last VB it drew from directly (V §8.7).
void Device7::HoldVB(VertexBuffer7* vb) {
    if (vb == heldVB_) return;
    if (vb) vb->AddRef();
    if (heldVB_) heldVB_->Release();
    heldVB_ = vb;
}

// A transformed vertex buffer that ProcessVertices(D3DVOP_CLIP) left with clip codes: its
// vertices outside the frustum or behind a user clip plane are in clip space (x and y as
// (c + w) / 2, z, w), the others on the screen (V §8.5). XP's draw clips them by the codes (V §4
// step 3a). Here: each primitive with a vertex in clip space, in clip space, clipped against the
// near plane (z >= 0: whatever is behind the camera) and the user clip planes its vertices'
// codes name (`planes`: in Direct3D's clip space, those in `planesOk` usable), then projected
// through the viewport; past the screen's edges the GPU clips them, as any transformed vertex.
// Every attribute is interpolated (the colours per byte). The others are copied as they are.
// The result is a list of the primitive's kind, drawn instead, in *out. Returns 1 then, 0 when
// no vertex is in clip space (drawn as it is), -1 without the memory.
namespace {
// After the position: the largest Direct3D 7 vertex is 168 bytes (16 + 12 + 4 + 4 + 4 + 8 x 16).
constexpr UINT kMaxRest = 152;
struct ClipVert {
    float c[4];  // clip space: x, y, z, w
    uint8_t rest[kMaxRest];  // everything after the position, as in the buffer
};
constexpr int kMaxPoly = 10;  // a triangle gains at most one vertex per plane (the near one and six)
}  // namespace

static int ExpandClipped(D3DPRIMITIVETYPE t, DWORD fvf, const uint8_t* mem, UINT stride, const WORD* codes, DWORD n,
                         const WORD* idx, DWORD ni, const D3DVIEWPORT7& vp, const float (*planes)[4], DWORD planesOk,
                         std::vector<uint8_t>* out, D3DPRIMITIVETYPE* outType) {
    // Only vertices stored in clip space need it (codes from a transformed source mark
    // vertices left on the screen, which the GPU clips as any).
    bool any = false;
    for (DWORD i = 0; i < n && !any; i++) any = (codes[i] & kInClipSpace) != 0;
    if (!any || stride < 16 || stride - 16 > kMaxRest) return 0;
    const UINT restSize = stride - 16;
    const float hw = vp.dwWidth * 0.5f, hh = vp.dwHeight * 0.5f, zr = vp.dvMaxZ - vp.dvMinZ;
    // Where the colours are (lerped per byte); the rest is floats.
    UINT off = 16;
    int diffuse = -1, specular = -1;
    if (fvf & D3DFVF_NORMAL) off += 12;
    if (fvf & D3DFVF_RESERVED1) off += 4;
    if (fvf & D3DFVF_DIFFUSE) diffuse = int(off), off += 4;
    if (fvf & D3DFVF_SPECULAR) specular = int(off);
    auto load = [&](DWORD i, ClipVert* v) {
        const uint8_t* p = mem + size_t(i) * stride;
        const float* f = reinterpret_cast<const float*>(p);
        if (codes[i] & kInClipSpace) {  // (x + w) / 2, (y + w) / 2, z, w
            v->c[3] = f[3];
            v->c[0] = 2 * f[0] - f[3];
            v->c[1] = 2 * f[1] - f[3];
            v->c[2] = f[2];
        } else {  // on the screen: back to clip space
            const float w = f[3] != 0 ? 1.0f / f[3] : 1.0f;
            v->c[3] = w;
            v->c[0] = (f[0] - vp.dwX - hw) / hw * w;
            v->c[1] = -(f[1] - vp.dwY - hh) / hh * w;
            v->c[2] = zr != 0 ? (f[2] - vp.dvMinZ) / zr * w : 0.0f;
        }
        memcpy(v->rest, p + 16, restSize);
    };
    auto lerp = [&](const ClipVert& a, const ClipVert& b, float k, ClipVert* v) {
        for (int j = 0; j < 4; j++) v->c[j] = a.c[j] + (b.c[j] - a.c[j]) * k;
        for (UINT o = 0; o + 4 <= restSize; o += 4) {
            const int at = int(o + 16);
            if (at == diffuse || at == specular) {
                for (int byte = 0; byte < 4; byte++) {
                    const float x = a.rest[o + byte] + (float(b.rest[o + byte]) - a.rest[o + byte]) * k;
                    v->rest[o + byte] = uint8_t(std::clamp(x + 0.5f, 0.0f, 255.0f));
                }
            } else {
                float fa, fb;
                memcpy(&fa, &a.rest[o], 4);
                memcpy(&fb, &b.rest[o], 4);
                const float x = fa + (fb - fa) * k;
                memcpy(&v->rest[o], &x, 4);
            }
        }
    };
    // Plane -1: the near plane; 0-5: the user clip planes.
    auto dist = [&](const ClipVert& v, int plane) {
        if (plane < 0) return v.c[2];
        const float* q = planes[plane];
        return q[0] * v.c[0] + q[1] * v.c[1] + q[2] * v.c[2] + q[3] * v.c[3];
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
        memcpy(out->data() + at + 16, v.rest, restSize);
    };
    auto copy = [&](DWORD i) {  // a vertex on the screen, as it is
        const size_t at = out->size();
        out->resize(at + stride);
        memcpy(out->data() + at, mem + size_t(i) * stride, stride);
    };
    // The planes a primitive is clipped against, from its vertices' codes.
    auto planeMask = [&](DWORD codeUnion) { return (codeUnion >> 6) & 0x3F & planesOk; };
    // Sutherland-Hodgman, against the near plane and the user planes in `mask`.
    auto clipPoly = [&](ClipVert* poly, int m, DWORD mask) {
        ClipVert next[kMaxPoly];
        for (int plane = -1; plane < 6 && m >= 2; plane++) {
            if (plane >= 0 && !(mask & (1u << plane))) continue;
            int k = 0;
            for (int i = 0; i < m; i++) {
                const ClipVert& a = poly[i];
                const ClipVert& b = poly[i + 1 < m ? i + 1 : 0];
                const float da = dist(a, plane), db = dist(b, plane);
                if (da >= 0 && k < kMaxPoly) next[k++] = a;
                if ((da >= 0) != (db >= 0) && k < kMaxPoly) lerp(a, b, da / (da - db), &next[k++]);
            }
            memcpy(poly, next, sizeof(ClipVert) * size_t(k));
            m = k;
        }
        return m;
    };
    auto vertexAt = [&](DWORD k) { return idx ? DWORD(idx[k]) : k; };
    const DWORD count = idx ? ni : n;
    out->clear();
    try {
        switch (t) {
        case D3DPT_POINTLIST:
            *outType = D3DPT_POINTLIST;
            for (DWORD k = 0; k < count; k++) {
                const DWORD i = vertexAt(k);
                if (!(codes[i] & kInClipSpace)) {
                    copy(i);
                    continue;
                }
                ClipVert v;
                load(i, &v);
                const DWORD mask = planeMask(codes[i]);
                bool in = v.c[2] >= 0 && v.c[3] > 0;
                for (int p = 0; p < 6 && in; p++) in = !(mask & (1u << p)) || dist(v, p) >= 0;
                if (in) emit(v);
            }
            return 1;
        case D3DPT_LINELIST:
        case D3DPT_LINESTRIP:
            *outType = D3DPT_LINELIST;
            for (DWORD k = 0; k + 1 < count; k += t == D3DPT_LINELIST ? 2 : 1) {
                const DWORD i0 = vertexAt(k), i1 = vertexAt(k + 1);
                if (!((codes[i0] | codes[i1]) & kInClipSpace)) {
                    copy(i0);
                    copy(i1);
                    continue;
                }
                ClipVert a, b, c;
                load(i0, &a);
                load(i1, &b);
                const DWORD mask = planeMask(codes[i0] | codes[i1]);
                bool keep = true;
                for (int p = -1; p < 6 && keep; p++) {
                    if (p >= 0 && !(mask & (1u << p))) continue;
                    const float da = dist(a, p), db = dist(b, p);
                    if (da < 0 && db < 0) keep = false;
                    else if (da < 0) lerp(a, b, da / (da - db), &c), a = c;
                    else if (db < 0) lerp(b, a, db / (db - da), &c), b = c;
                }
                if (!keep || a.c[3] <= 0 || b.c[3] <= 0) continue;
                emit(a);
                emit(b);
            }
            return 1;
        default:
            break;
        }
        *outType = D3DPT_TRIANGLELIST;
        for (DWORD k = 0; k + 2 < count; k += t == D3DPT_TRIANGLELIST ? 3 : 1) {
            DWORD i[3];
            if (t == D3DPT_TRIANGLEFAN) {
                i[0] = vertexAt(0), i[1] = vertexAt(k + 1), i[2] = vertexAt(k + 2);
            } else if (t == D3DPT_TRIANGLESTRIP && (k & 1)) {
                i[0] = vertexAt(k + 1), i[1] = vertexAt(k), i[2] = vertexAt(k + 2);  // the same winding as the strip's
            } else {
                i[0] = vertexAt(k), i[1] = vertexAt(k + 1), i[2] = vertexAt(k + 2);
            }
            const DWORD codeUnion = codes[i[0]] | codes[i[1]] | codes[i[2]];
            if (!(codeUnion & kInClipSpace)) {
                for (DWORD j : i) copy(j);
                continue;
            }
            ClipVert poly[kMaxPoly];
            for (int j = 0; j < 3; j++) load(i[j], &poly[j]);
            const DWORD mask = planeMask(codeUnion);
            int m = 3;
            if (mask || poly[0].c[2] < 0 || poly[1].c[2] < 0 || poly[2].c[2] < 0) m = clipPoly(poly, m, mask);
            for (int j = 1; j + 1 < m; j++) {
                if (poly[0].c[3] <= 0 || poly[j].c[3] <= 0 || poly[j + 1].c[3] <= 0) continue;
                emit(poly[0]);
                emit(poly[j]);
                emit(poly[j + 1]);
            }
        }
    } catch (const std::exception&) {
        return -1;
    }
    return 1;
}

// The user clip planes CLIPPLANEENABLE turns on, in Direct3D's clip space (from world space
// through the inverse of view x projection): the bits of those set in out[] (none when that
// has no inverse).
DWORD Device7::ClipSpacePlanes(float (*out)[4]) const {
    const DWORD on = rs_[D3DRENDERSTATE_CLIPPLANEENABLE] & 0x3F;
    if (!on) return 0;
    D3DMATRIX vpm;
    MatMul(&vpm, view_, proj_);
    float inv[16];
    if (!Invert(vpm, inv)) return 0;
    // A clip-space point c is the world point c x inverse: the plane's dot with that is c's
    // with inverse x plane.
    for (DWORD k = 0; k < 6; k++) {
        if (!(on & (1u << k))) continue;
        const float* p = planes_[k];
        for (int r = 0; r < 4; r++)
            out[k][r] = inv[r * 4] * p[0] + inv[r * 4 + 1] * p[1] + inv[r * 4 + 2] * p[2] + inv[r * 4 + 3] * p[3];
    }
    return on;
}

HRESULT Device7::DrawVB(D3DPRIMITIVETYPE t, VertexBuffer7* vb, DWORD start, DWORD n, const WORD* idx, DWORD ni) {
    // Not past the buffer's end (XP reads beyond it).
    if (start >= vb->count_) return D3D_OK;
    n = std::min(n, vb->count_ - start);
    const bool transformed = (vb->fvf_ & 0x0E) == D3DFVF_XYZRHW;
    const bool clipped = rs_[136] == TRUE && (kind_ != kTnLHAL || vb->hasClipCodes_);
    if ((transformed || kind_ == kTnLHAL) && !clipped) HoldVB(vb);
    const uint8_t* mem = vb->mem_.data() + size_t(start) * vb->stride_;
    if (transformed && clipped && vb->hasClipCodes_ && vb->clip_.size() >= size_t(start) + n && ValidPrim(t)) {
        // (ExpandClipped reads the vertices the indices name.)
        if (idx && !(idx = SafeIndices(idx, ni, n))) return DDERR_OUTOFMEMORY;
        float planes[6][4];
        const DWORD planesOk = ClipSpacePlanes(planes);
        D3DPRIMITIVETYPE ot;
        const int r = ExpandClipped(t, vb->fvf_ & ~DWORD(0xF001), mem, vb->stride_, vb->clip_.data() + start, n, idx, ni,
                                    vp_, planes, planesOk, &clipped_, &ot);
        if (r < 0) return DDERR_OUTOFMEMORY;
        if (r > 0)
            return clipped_.empty() ? D3D_OK
                                    : DrawCommon(ot, vb->fvf_, clipped_.data(), vb->stride_,
                                                 DWORD(clipped_.size() / vb->stride_), nullptr, 0, true);
    }
    return DrawCommon(t, vb->fvf_, mem, vb->stride_, n, idx, ni, true);
}

HRESULT Device7::DrawPrimitiveVB(D3DPRIMITIVETYPE t, IDirect3DVertexBuffer7* p, DWORD start, DWORD n, DWORD) {
    if (!p) return DDERR_INVALIDPARAMS;
    Guard l;
    return DrawVB(t, VB(p), start, n, nullptr, 0);
}

// Indices relative to the start vertex.
HRESULT Device7::DrawIndexedPrimitiveVB(D3DPRIMITIVETYPE t, IDirect3DVertexBuffer7* p, DWORD start, DWORD n,
                                        WORD* idx, DWORD ni, DWORD) {
    if (!p || (!idx && ni)) return DDERR_INVALIDPARAMS;
    Guard l;
    return DrawVB(t, VB(p), start, n, idx, ni);
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

}  // namespace d7
}  // namespace dd2d9
