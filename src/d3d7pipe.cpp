// dd2d9's Direct3D 7: the CPU vertex pipeline: the clip status draws leave,
// ComputeSphereVisibility, and the vertex buffers' Optimize and ProcessVertices (V §8.5;
// d3d7int.h).
#include "d3d7int.h"

#include <cmath>

namespace dd2d9 {
namespace d7 {

// Vertex blending (V §9): the point p's (homogeneous) results through the world matrices,
// weighted: weights[k] for the first `blend` of them, the last 1 - their sum.
void BlendPoint(const float* p, const float* weights, DWORD blend, const D3DMATRIX* const* worlds, float out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0;
    float sum = 0;
    for (DWORD k = 0; k <= blend; k++) {
        const float w = k < blend ? weights[k] : 1.0f - sum;
        if (k < blend) sum += w;
        const D3DMATRIX& M = *worlds[k];
        out[0] += (p[0] * M._11 + p[1] * M._21 + p[2] * M._31 + M._41) * w;
        out[1] += (p[0] * M._12 + p[1] * M._22 + p[2] * M._32 + M._42) * w;
        out[2] += (p[0] * M._13 + p[1] * M._23 + p[2] * M._33 + M._43) * w;
        out[3] += (p[0] * M._14 + p[1] * M._24 + p[2] * M._34 + M._44) * w;
    }
}

// m's inverse, row by row as a D3DMATRIX; false when it has none.
bool Invert(const D3DMATRIX& m, float out[16]) {
    const float* a = &m._11;
    float* inv = out;
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
    if (det == 0.0f || !std::isfinite(det)) return false;
    const float id = 1.0f / det;
    for (int i = 0; i < 16; i++) inv[i] *= id;
    return true;
}

// The normal matrix: the transpose of m's inverse (all of it, 4x4), its upper 3x3 used; all
// zero when m has no inverse.
D3DMATRIX NormalMatrix(const D3DMATRIX& m) {
    D3DMATRIX r = {};
    float inv[16];
    if (!Invert(m, inv)) return r;
    float* o = &r._11;
    for (int i = 0; i < 4; i++)  // transposed: row i of the result = column i of the inverse
        for (int j = 0; j < 4; j++) o[i * 4 + j] = inv[j * 4 + i];
    return r;
}

// The clip status a draw leaves (§5.1, §5.2, D3DFE_UpdateClipStatus): with CLIPPING on, the
// vertices' clip codes merged into it (MergeClipStatus) and, with D3DRENDERSTATE_EXTENTS, the
// extents grown by the vertices inside; with it off, transformed vertices still grow the
// extents, all of them, and untransformed ones leave both alone. Not for a T&L HAL device's
// draws, nor for a transformed vertex buffer's (§4 step 3a; the caller sees to those).
void Device7::TrackClipStatus(DWORD fvf, const void* v, UINT stride, DWORD n) {
    const bool clip = rs_[D3DRENDERSTATE_CLIPPING] == TRUE, extents = rs_[D3DRENDERSTATE_EXTENTS] == TRUE;
    DWORD orCodes, andCodes;
    if ((fvf & 0x0E) == D3DFVF_XYZRHW) {
        if (!clip) {
            for (DWORD i = 0; i < n && extents; i++) {
                const float* f = reinterpret_cast<const float*>(static_cast<const uint8_t*>(v) + size_t(i) * stride);
                GrowExtents(f[0], f[1]);
            }
            return;
        }
        orCodes = TlClipCodes(static_cast<const uint8_t*>(v), stride, n, extents, nullptr, &andCodes);
    } else {
        if (!clip) return;
        orCodes = ClipCodes(fvf, v, stride, n, extents, &andCodes);
    }
    if (n) cs_.dwStatus = MergeClipStatus(cs_.dwStatus, orCodes, andCodes);
}

void Device7::GrowExtents(float x, float y) {
    cs_.minx = std::min(cs_.minx, x);
    cs_.maxx = std::max(cs_.maxx, x);
    cs_.miny = std::min(cs_.miny, y);
    cs_.maxy = std::max(cs_.maxy, y);
}

// The user clip planes in `on` (CLIPPLANEENABLE's bits) a world-space point (homogeneous: a
// world matrix may have a w column) is behind: D3DCLIP_GEN0-5. A plane (a, b, c, d) clips
// where a x + b y + c z + d w < 0.
DWORD Device7::PlaneCodes(const float* wp, DWORD on) const {
    DWORD code = 0;
    for (DWORD k = 0; k < 6; k++)
        if ((on & (1u << k)) && planes_[k][0] * wp[0] + planes_[k][1] * wp[1] + planes_[k][2] * wp[2] + planes_[k][3] * wp[3] < 0)
            code |= D3DCLIP_GEN0 << k;
    return code;
}

// The clip codes of untransformed vertices (§7.3): each one's in the front end's 0..w volume
// and against the user clip planes (world space); with `extents`, the extents grown by the
// vertices inside. Vertex blending as the draw does it: min(the FVF's weights, VERTEXBLEND, 3)
// weights after the position, the last world matrix weighted 1 - their sum. Returns the
// codes' union, *andCodes their intersection.
DWORD Device7::ClipCodes(DWORD fvf, const void* v, UINT stride, DWORD n, bool extents, DWORD* andCodes) {
    const DWORD posType = fvf & 0x0E, betas = posType >= 0x06 ? (posType - 0x04) / 2 : 0;
    const DWORD blend = std::min<DWORD>(std::min<DWORD>(rs_[D3DRENDERSTATE_VERTEXBLEND], betas), 3);
    *andCodes = 0xFFF;
    if (stride < 12 + 4 * blend) return 0;
    const D3DMATRIX* worlds[4] = {&world_, &world1_, &world2_, &world3_};
    D3DMATRIX vpm;
    MatMul(&vpm, view_, proj_);
    const DWORD planesOn = rs_[D3DRENDERSTATE_CLIPPLANEENABLE] & 0x3F;
    const D3DVIEWPORT7& vp = vp_;
    DWORD orCodes = 0, andC = 0xFFF;
    for (DWORD i = 0; i < n; i++) {
        const float* p = reinterpret_cast<const float*>(static_cast<const uint8_t*>(v) + size_t(i) * stride);
        // The world-space point (homogeneous: a world matrix may have a w column).
        float wp[4];
        BlendPoint(p, p + 3, blend, worlds, wp);
        const float cx = wp[0] * vpm._11 + wp[1] * vpm._21 + wp[2] * vpm._31 + wp[3] * vpm._41;
        const float cy = wp[0] * vpm._12 + wp[1] * vpm._22 + wp[2] * vpm._32 + wp[3] * vpm._42;
        const float cz = wp[0] * vpm._13 + wp[1] * vpm._23 + wp[2] * vpm._33 + wp[3] * vpm._43;
        const float cw = wp[0] * vpm._14 + wp[1] * vpm._24 + wp[2] * vpm._34 + wp[3] * vpm._44;
        const DWORD code = ClipCode01((cx + cw) * 0.5f, (cy + cw) * 0.5f, cz, cw) | PlaneCodes(wp, planesOn);
        orCodes |= code;
        andC &= code;
        if (!code && extents && cw != 0) {
            const float rhw = 1.0f / cw;
            GrowExtents(cx * rhw * (vp.dwWidth * 0.5f) + vp.dwX + vp.dwWidth * 0.5f,
                        -cy * rhw * (vp.dwHeight * 0.5f) + vp.dwY + vp.dwHeight * 0.5f);
        }
    }
    *andCodes = andC;
    return orCodes;
}

// The clip codes of transformed vertices (§7.1); with `extents`, the extents grown by those
// inside; each one's code into codes[] when given. Returns the codes' union, *andCodes their
// intersection.
DWORD Device7::TlClipCodes(const uint8_t* v, UINT stride, DWORD n, bool extents, WORD* codes, DWORD* andCodes) {
    const bool zenable = rs_[D3DRENDERSTATE_ZENABLE] != 0;
    DWORD orCodes = 0, andC = 0xFFFF;
    for (DWORD i = 0; i < n; i++) {
        const float* f = reinterpret_cast<const float*>(v + size_t(i) * stride);
        float x = f[0], y = f[1];
        const DWORD code = TlClipCode(&x, &y, f[2], f[3], vp_, zenable);
        if (extents && !code) GrowExtents(x, y);
        orCodes |= code;
        andC &= code;
        if (codes) codes[i] = WORD(code);
    }
    *andCodes = andC;
    return orCodes;
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

// ComputeSphereVisibility: the box test of S §ComputeSphereVisibility, on world x view x
// projection with the viewport's 0..w clip volume.
HRESULT Device7::ComputeSphereVisibility(D3DVECTOR* centers, D3DVALUE* radii, DWORD n, DWORD, DWORD* ret) {
    if (!centers || !radii || !ret) return DDERR_INVALIDPARAMS;
    Guard l;
    D3DMATRIX wv, wvp;
    MatMul(&wv, world_, view_);
    MatMul(&wvp, wv, proj_);
    const DWORD planesOn = rs_[D3DRENDERSTATE_CLIPPLANEENABLE];
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
            const float wp[4] = {wx.x, wx.y, wx.z, 1.0f};
            // D3D's -w..w x/y volume as the front end's 0..w one.
            const DWORD code = ClipCode01((cx + cw) * 0.5f, (cy + cw) * 0.5f, cz, cw) | PlaneCodes(wp, planesOn);
            uni |= code;
            inter &= code;
        }
        ret[s] = uni | (inter << 12);
    }
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

// ---------------------------------------------------------------- ProcessVertices (V §8.5)

// Vertex fog (V §9): on with FOGENABLE and a FOGVERTEXMODE; its factor at the eye-space point
// P, from its z or its distance.
static bool VertexFogOn(const DWORD* rs) {
    const DWORD fogMode = rs[D3DRENDERSTATE_FOGVERTEXMODE];
    return rs[D3DRENDERSTATE_FOGENABLE] == TRUE && fogMode != D3DFOG_NONE && fogMode != 0xFFFFFFFF;
}

static float VertexFog(const DWORD* rs, Vec3 P) {
    const DWORD fogMode = rs[D3DRENDERSTATE_FOGVERTEXMODE];
    const float dist = rs[D3DRENDERSTATE_RANGEFOGENABLE] == TRUE ? sqrtf(Dot(P, P)) : P.z;
    auto rsf = [&](DWORD st) {
        float f;
        memcpy(&f, &rs[st], 4);
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
    return f;
}

// Does the processing of V §8.5 on the CPU: transform (always), clip codes (D3DVOP_CLIP),
// lighting (D3DVOP_LIGHT, whatever D3DRENDERSTATE_LIGHTING says), extents (D3DVOP_EXTENTS).
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
    try {
        if (light)
            for (const Device7::Light& d : dev->lights_)
                if (d.set && d.enabled)
                    lights.push_back({&d.l, XformPoint({d.l.dvPosition.x, d.l.dvPosition.y, d.l.dvPosition.z}, dev->view_),
                                      Norm(XformDir({d.l.dvDirection.x, d.l.dvDirection.y, d.l.dvDirection.z}, dev->view_)),
                                      cosf(d.l.dvTheta * 0.5f), cosf(d.l.dvPhi * 0.5f)});
    } catch (const std::exception&) {
        return DDERR_OUTOFMEMORY;
    }
    // The user clip planes give clip codes too (GEN0-5, from the world-space point).
    const DWORD planesOn = clip ? dev->rs_[D3DRENDERSTATE_CLIPPLANEENABLE] & 0x3F : 0;
    const Color ambient = FromArgb(dev->rs_[D3DRENDERSTATE_AMBIENT]);
    const bool fog = VertexFogOn(dev->rs_);
    DWORD orCodes = 0, andCodes = 0xFFFF;
    for (DWORD i = 0; i < count; i++) {
        const uint8_t* v = src + size_t(i) * srcStride;
        uint8_t* o = dstVb->mem_.data() + size_t(dstIndex + i) * dstVb->stride_;
        const float* p = reinterpret_cast<const float*>(v + in.pos);
        Vec3 pos = {p[0], p[1], p[2]};
        float cx, cy, cz, cw;
        float wp[4];  // the world-space point, for the user clip planes
        float weights[3] = {0, 0, 0};
        for (DWORD k = 0; k < blend; k++)
            if (reinterpret_cast<const uint8_t*>(p + 3 + k + 1) <= srcEnd) weights[k] = p[3 + k];
        if (blend) {
            // Vertex blending: the matrices' whole (4-component) results blended, then view
            // and projection (test_process_vertices: a world with a w column).
            float acc[4];
            BlendPoint(p, weights, blend, worlds, acc);
            memcpy(wp, acc, sizeof(wp));
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
            if (planesOn) {
                const D3DMATRIX& W = dev->world_;
                wp[0] = pos.x * W._11 + pos.y * W._21 + pos.z * W._31 + W._41;
                wp[1] = pos.x * W._12 + pos.y * W._22 + pos.z * W._32 + W._42;
                wp[2] = pos.x * W._13 + pos.y * W._23 + pos.z * W._33 + W._43;
                wp[3] = pos.x * W._14 + pos.y * W._24 + pos.z * W._34 + W._44;
            }
        }
        // The front end's 0..w clip volume (V §7.3).
        const float fx = (cx + cw) * 0.5f, fy = (cy + cw) * 0.5f;
        DWORD code = 0;
        if (clip) {
            code = ClipCode01(fx, fy, cz, cw);
            if (planesOn) code |= dev->PlaneCodes(wp, planesOn);
            orCodes |= code;
            andCodes &= code;
            // kInClipSpace (no D3DCLIP_ flag): the vertex is stored in clip space (below).
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
            if (extents) dev->GrowExtents(sx, sy);
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
            if (fog) s.a = VertexFog(dev->rs_, P);
            specular = ToArgb(s);
        }
        // Vertex fog without lighting too (V §8.5 step 8): the factor in specular alpha.
        const bool fogged = !light && fog && out.specular >= 0;
        if (fogged) {
            const float f = VertexFog(dev->rs_, XformPoint(pos, blend ? dev->view_ : wv));
            specular = (specular & 0x00FFFFFF) | (TruncToDword(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f) << 24);
        }
        if (out.diffuse >= 0 && (light || !noCopy)) *reinterpret_cast<DWORD*>(o + out.diffuse) = diffuse;
        if (out.specular >= 0 && (light || fogged || !noCopy)) *reinterpret_cast<DWORD*>(o + out.specular) = specular;
        if (!noCopy)
            for (DWORD t = 0; t < in.texCount; t++)
                memcpy(o + out.tex[t], v + in.tex[t], std::min(in.texSize[t], out.texSize[t]));
    }
    if (clip) {
        if (count) dev->cs_.dwStatus = MergeClipStatus(dev->cs_.dwStatus, orCodes, andCodes);
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
        // The destination's own vertices (V §8.5 step 3): transformed ones' clip codes and
        // extents, untransformed ones' clip status.
        if (dst >= count_) return D3D_OK;
        count = std::min(count, count_ - dst);
        const uint8_t* v = mem_.data() + size_t(dst) * stride_;
        DWORD andCodes;
        if ((fvf_ & 0x0E) != D3DFVF_XYZRHW) {
            if ((op & D3DVOP_CLIP) && (fvf_ & 0x0E) && count) {
                const DWORD orCodes = dev->ClipCodes(fvf_, v, stride_, count, false, &andCodes);
                dev->cs_.dwStatus = MergeClipStatus(dev->cs_.dwStatus, orCodes, andCodes);
            }
            return D3D_OK;
        }
        WORD* codes = (op & D3DVOP_CLIP) && !clip_.empty() ? clip_.data() + dst : nullptr;
        const DWORD orCodes = dev->TlClipCodes(v, stride_, count, (op & D3DVOP_EXTENTS) != 0, codes, &andCodes);
        if ((op & D3DVOP_CLIP) && count) {
            dev->cs_.dwStatus = MergeClipStatus(dev->cs_.dwStatus, orCodes, andCodes);
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
    if (!Gather(srcFvf, *data, count, &v)) return DDERR_OUTOFMEMORY;
    return Process(this, op, dst, count, v.data(), FvfSize(srcFvf), srcFvf, static_cast<Device7*>(device), flags,
                   v.data() + v.size());
}

}  // namespace d7
}  // namespace dd2d9
