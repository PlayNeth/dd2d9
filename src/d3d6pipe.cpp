// dd2d9's Direct3D 3-6: the CPU vertex pipeline (XP had no hardware T&L for DX3-6: d3dim's
// front end transformed, lit and clipped every vertex), Execute's instruction stream, the
// draws, Viewport::Clear's drawing and DX6 ProcessVertices. Direct3D 9 gets transformed
// vertices only: D3DVIEWPORT2's clip volume, the two DX5/6 lighting models and execute
// buffers' semantics have no Direct3D 9 equivalent.
#include "d3d6int.h"

#include <emmintrin.h>
#include <xmmintrin.h>

namespace dd2d9 {
namespace d6 {

namespace {

// Guard band for clipping (in viewports, each side): primitives are clipped only where they
// leave it or cross the near or far plane; Direct3D 9's viewport cuts the rest, as XP's
// guard-band clipping left it to the driver.
constexpr float kGuard = 2.0f;

struct LightC {
    DWORD type;
    bool v2, noSpec;
    Color color;
    Vec3 pos, dir;  // camera space; dir normalized
    float range, range2, a0, a1, a2, falloff, cosTheta, cosPhi, k;
};

// The lights of the viewport in use that were set and are active, in the order added (C §1.14).
void ActiveLights(Device* d, std::vector<LightC>* out) {
    out->clear();
    if (!d->cur) return;
    for (const Light* l : d->cur->lights) {
        if (!l->data.dwSize || !l->Active()) continue;
        const D3DLIGHT2& s = l->data;
        LightC c = {};
        c.type = s.dltType;
        c.v2 = s.dwSize == sizeof(D3DLIGHT2);
        c.noSpec = c.v2 && (s.dwFlags & D3DLIGHT_NO_SPECULAR);
        c.color = FromCv(s.dcvColor);
        if (d->kind == kRamp) {  // the mono model: the light's brightness only (C §2.2)
            const float y = 0.3f * c.color.r + 0.59f * c.color.g + 0.11f * c.color.b;
            c.color.r = c.color.g = c.color.b = y;
        }
        c.pos = XformPoint({s.dvPosition.x, s.dvPosition.y, s.dvPosition.z}, d->view);
        c.dir = Norm(XformDir({s.dvDirection.x, s.dvDirection.y, s.dvDirection.z}, d->view));
        c.range = s.dvRange;
        c.range2 = s.dvRange * s.dvRange;
        c.a0 = s.dvAttenuation0;
        c.a1 = s.dvAttenuation1;
        c.a2 = s.dvAttenuation2;
        c.falloff = s.dvFalloff;
        c.cosTheta = cosf(s.dvTheta * 0.5f);
        c.cosPhi = cosf(s.dvPhi * 0.5f);
        const float den = c.cosTheta - c.cosPhi;
        c.k = den != 0.0f ? 1.0f / den : (c.v2 ? 1.0f : 0.0f);
        out->push_back(c);
    }
}

inline DWORD ColorToArgb(float r, float g, float b, float a) {
    auto q = [](float v) { return TruncToDword(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (q(a) << 24) | (q(r) << 16) | (q(g) << 8) | q(b);
}

void Project(const VpMap& m, PVert* p) {
    const float rhw = p->cw != 0.0f ? 1.0f / p->cw : 2097152.0f;
    p->sx = m.X + m.W * p->cx * rhw;
    p->sy = m.Y + m.H - m.H * p->cy * rhw;
    p->sz = p->cz * rhw;
    p->rhw = rhw;
}

// Scratch reused from draw to draw (every caller holds g_lock).
std::vector<PVert> t_pverts;
std::vector<WORD> t_safeIdx;  // indices past the vertices, as vertex 0
std::vector<uint8_t> t_out;
std::vector<WORD> t_idx;
std::vector<uint8_t> t_bout;  // (DrawPVertsBatch's)
std::vector<WORD> t_bidx;
std::vector<GpuContext::DrawSeg> t_segs;
struct BatchPending {  // (DrawPVertsBatch's lists waiting)
    DWORD off, count, lo, hi;
    UINT prims;    // (drawn as they are)
    bool clipped;  // cut by the guard band
    WORD orGb;     // (clipped: the range's guard-band codes ORed)
};
std::vector<BatchPending> t_pend;
std::vector<WORD> t_codes;
std::vector<LightC> t_lights;

}  // namespace

// ---------------------------------------------------------------- transform and light
//
// Four vertices at a time with SSE2, each lane doing the scalar code's operations in the
// scalar code's order (no fused or reordered arithmetic, IEEE division and square roots), so
// the output is bit for bit the scalar code's; expf is called per lane, and both use Pow4
// for the specular and spot powers. The scalar code does the vertices left over (fewer than
// four) and is the reference.

namespace {

inline __m128 Splat(float v) { return _mm_set1_ps(v); }
inline __m128 Sel(__m128 m, __m128 a, __m128 b) { return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b)); }
inline __m128 Neg(__m128 a) { return _mm_xor_ps(a, _mm_set1_ps(-0.0f)); }
// a.x * b.x + a.y * b.y + a.z * b.z, as Dot.
inline __m128 Dot3(__m128 ax, __m128 ay, __m128 az, __m128 bx, __m128 by, __m128 bz) {
    return _mm_add_ps(_mm_add_ps(_mm_mul_ps(ax, bx), _mm_mul_ps(ay, by)), _mm_mul_ps(az, bz));
}
// Norm: a / |a| where |a| > 0, else a.
inline void Norm3(__m128* x, __m128* y, __m128* z) {
    const __m128 l = _mm_sqrt_ps(Dot3(*x, *y, *z, *x, *y, *z));
    const __m128 ok = _mm_cmpgt_ps(l, _mm_setzero_ps());
    *x = Sel(ok, _mm_div_ps(*x, l), *x);
    *y = Sel(ok, _mm_div_ps(*y, l), *y);
    *z = Sel(ok, _mm_div_ps(*z, l), *z);
}
// p * row-major m: XformPoint (with the translation) or XformDir.
inline void Xform3(__m128 x, __m128 y, __m128 z, const D3DMATRIX& m, bool point, __m128* ox, __m128* oy, __m128* oz) {
    *ox = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, Splat(m._11)), _mm_mul_ps(y, Splat(m._21))), _mm_mul_ps(z, Splat(m._31)));
    *oy = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, Splat(m._12)), _mm_mul_ps(y, Splat(m._22))), _mm_mul_ps(z, Splat(m._32)));
    *oz = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, Splat(m._13)), _mm_mul_ps(y, Splat(m._23))), _mm_mul_ps(z, Splat(m._33)));
    if (point) {
        *ox = _mm_add_ps(*ox, Splat(m._41));
        *oy = _mm_add_ps(*oy, Splat(m._42));
        *oz = _mm_add_ps(*oz, Splat(m._43));
    }
}
// Three floats from each of four vertices, as lanes.
inline void Load3(const uint8_t* const v[4], int off, __m128* x, __m128* y, __m128* z) {
    const float *a = reinterpret_cast<const float*>(v[0] + off), *b = reinterpret_cast<const float*>(v[1] + off),
                *c = reinterpret_cast<const float*>(v[2] + off), *e = reinterpret_cast<const float*>(v[3] + off);
    *x = _mm_setr_ps(a[0], b[0], c[0], e[0]);
    *y = _mm_setr_ps(a[1], b[1], c[1], e[1]);
    *z = _mm_setr_ps(a[2], b[2], c[2], e[2]);
}
// f(lane) for the lanes in mask (the others keep a), as the scalar code calls it.
template <class Fn>
inline __m128 PerLane(__m128 a, __m128 mask, Fn fn) {
    const int m = _mm_movemask_ps(mask);
    if (!m) return a;
    alignas(16) float t[4];
    _mm_store_ps(t, a);
    for (int k = 0; k < 4; k++)
        if (m & (1 << k)) t[k] = fn(t[k], k);
    return _mm_load_ps(t);
}
// x to the power y for the lanes in mask (the others keep x), as exp2(y log2 x) with Cephes'
// single-precision polynomials: within 1e-5 of powf (115 ulps at most, at a power of 128),
// which moved an 8-bit colour by one step in 6 of 4.2 million lighting terms tried. powf was
// most of the time of lighting with specular or spot lights. Lanes where x isn't a positive
// normal number or y log2 x leaves [-125, 125] go through powf; a power of 1 gives x.
inline __m128 Pow4(__m128 x, float y, __m128 mask) {
    if (y == 1.0f) return x;  // (powf's answer: x itself)
    const __m128 normMin = Splat(FLT_MIN), normMax = Splat(FLT_MAX);
    const __m128 ok = _mm_and_ps(mask, _mm_and_ps(_mm_cmpge_ps(x, normMin), _mm_cmple_ps(x, normMax)));
    // log2 x: x = m 2^e, m in [sqrt(1/2), sqrt(2)); log(m) = f + f^3 P(f) - f^2 / 2, f = m - 1.
    const __m128i xi = _mm_castps_si128(x);
    __m128i e = _mm_sub_epi32(_mm_srli_epi32(xi, 23), _mm_set1_epi32(127));
    __m128 m = _mm_castsi128_ps(_mm_or_si128(_mm_and_si128(xi, _mm_set1_epi32(0x007FFFFF)), _mm_set1_epi32(0x3F800000)));
    const __m128 big = _mm_cmpgt_ps(m, Splat(1.41421356f));
    m = Sel(big, _mm_mul_ps(m, Splat(0.5f)), m);
    e = _mm_sub_epi32(e, _mm_castps_si128(big));  // (big: -1, e + 1)
    const __m128 f = _mm_sub_ps(m, Splat(1.0f)), z = _mm_mul_ps(f, f);
    __m128 p = Splat(7.0376836292e-2f);
    for (float c : {-1.1514610310e-1f, 1.1676998740e-1f, -1.2420140846e-1f, 1.4249322787e-1f, -1.6668057665e-1f,
                    2.0000714765e-1f, -2.4999993993e-1f, 3.3333331174e-1f})
        p = _mm_add_ps(_mm_mul_ps(p, f), Splat(c));
    const __m128 ln = _mm_add_ps(f, _mm_sub_ps(_mm_mul_ps(_mm_mul_ps(p, f), z), _mm_mul_ps(Splat(0.5f), z)));
    const __m128 l2 = _mm_add_ps(_mm_mul_ps(ln, Splat(1.44269504089f)), _mm_cvtepi32_ps(e));
    // exp2 t: t = i + g, g in [-0.5, 0.5]; 2^g = 1 + g Q(g).
    const __m128 t = _mm_mul_ps(l2, Splat(y));
    const __m128 inRange = _mm_and_ps(_mm_cmpge_ps(t, Splat(-125.0f)), _mm_cmple_ps(t, Splat(125.0f)));
    const __m128 tc = _mm_and_ps(inRange, t);  // (out of range: computed from 0, then replaced)
    // i = floor(tc + 1/2), whatever the rounding mode (cvtps rounds by it).
    const __m128 half = _mm_add_ps(tc, Splat(0.5f));
    __m128i i = _mm_cvttps_epi32(half);
    i = _mm_add_epi32(i, _mm_castps_si128(_mm_cmpgt_ps(_mm_cvtepi32_ps(i), half)));  // (above: -1)
    const __m128 g = _mm_sub_ps(tc, _mm_cvtepi32_ps(i));
    __m128 q = Splat(1.535336188319500e-4f);
    for (float c : {1.339887440266574e-3f, 9.618437357674640e-3f, 5.550332471162809e-2f, 2.402264791363012e-1f,
                    6.931472028550421e-1f})
        q = _mm_add_ps(_mm_mul_ps(q, g), Splat(c));
    const __m128 two = _mm_castsi128_ps(_mm_slli_epi32(_mm_add_epi32(i, _mm_set1_epi32(127)), 23));
    const __m128 r = _mm_mul_ps(_mm_add_ps(_mm_mul_ps(q, g), Splat(1.0f)), two);
    const __m128 fast = _mm_and_ps(ok, inRange);
    __m128 out = Sel(fast, r, x);
    const __m128 slow = _mm_andnot_ps(fast, mask);
    if (_mm_movemask_ps(slow)) out = PerLane(out, slow, [&](float, int k) {
        alignas(16) float xs[4];
        _mm_store_ps(xs, x);
        return powf(xs[k], y);
    });
    return out;
}
// One value through Pow4 (the vertices left over, so that every vertex gets the same answer).
inline float Pow1(float x, float y) { return _mm_cvtss_f32(Pow4(_mm_set_ss(x), y, _mm_castsi128_ps(_mm_setr_epi32(-1, 0, 0, 0)))); }

// ColorToArgb for four lanes; a lane with a NaN goes through ColorToArgb itself (its float
// to DWORD conversion of NaN isn't the vector one's).
inline void ToArgb4(__m128 r, __m128 g, __m128 b, __m128 a, DWORD out[4]) {
    const __m128 nan = _mm_or_ps(_mm_or_ps(_mm_cmpunord_ps(r, r), _mm_cmpunord_ps(g, g)),
                                 _mm_or_ps(_mm_cmpunord_ps(b, b), _mm_cmpunord_ps(a, a)));
    auto q = [](__m128 v) {  // std::clamp(v, 0, 1) * 255 + 0.5, truncated (v isn't NaN)
        const __m128 zero = _mm_setzero_ps(), one = Splat(1.0f);
        v = Sel(_mm_cmplt_ps(v, zero), zero, Sel(_mm_cmplt_ps(one, v), one, v));
        return _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(v, Splat(255.0f)), Splat(0.5f)));
    };
    const __m128i c = _mm_or_si128(_mm_or_si128(_mm_slli_epi32(q(a), 24), _mm_slli_epi32(q(r), 16)),
                                   _mm_or_si128(_mm_slli_epi32(q(g), 8), q(b)));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out), c);
    if (const int m = _mm_movemask_ps(nan)) {
        alignas(16) float rr[4], gg[4], bb[4], aa[4];
        _mm_store_ps(rr, r), _mm_store_ps(gg, g), _mm_store_ps(bb, b), _mm_store_ps(aa, a);
        for (int k = 0; k < 4; k++)
            if (m & (1 << k)) out[k] = ColorToArgb(rr[k], gg[k], bb[k], aa[k]);
    }
}
// FromArgb's channel (c >> shift) & 0xFF, / 255.
inline __m128 Channel(__m128i c, int shift) {
    return _mm_div_ps(_mm_cvtepi32_ps(_mm_and_si128(_mm_srli_epi32(c, shift), _mm_set1_epi32(0xFF))), Splat(255.0f));
}

}  // namespace

void TransformVerts(Device* d, const uint8_t* src, UINT stride, DWORD fvf, DWORD n, bool clip, bool light,
                    bool extents, PVert* out, DWORD lo, DWORD hi) {
    // The four-lane code divides and takes roots on every lane and picks the answers after,
    // where the scalar code skips (a vertex outside, a light on the vertex): with the game's
    // floating-point exceptions unmasked (some runtimes' default) that would trap. Masked
    // here, the game's state put back after (its flags as they were). The rounding stays.
    struct Csr {
        unsigned saved = _mm_getcsr();
        Csr() { _mm_setcsr(saved | 0x1F80); }
        ~Csr() { _mm_setcsr(saved); }
    } csr;
    const Layout in(fvf & 0xFFFF);
    const D3DMATRIX wv = Mul(d->world, d->view);
    const D3DMATRIX M = Mul(Mul(wv, d->proj), d->map.mclip);
    // The active lights in camera space: kept while nothing they depend on changed (the
    // device, its viewport, any light or viewport light list, the view matrix).
    std::vector<LightC>& lights = t_lights;
    if (light) {
        static const Device* cDev = nullptr;
        static const Viewport* cVp = nullptr;
        static unsigned cGen = ~0u;
        static D3DMATRIX cView;
        if (cDev != d || cVp != d->cur || cGen != g_lightGen || memcmp(&cView, &d->view, sizeof(cView)) != 0) {
            ActiveLights(d, &lights);
            cDev = d, cVp = d->cur, cGen = g_lightGen, cView = d->view;
        }
    }
    const LightC* const lightsEnd = lights.data() + lights.size();
    const D3DMATERIAL& mat = d->curMat;
    // Ramp devices light in the mono model: the lights' and the ambient light's brightness
    // shade the material's colours, uncoloured (XP's Ramp rasterizer drew through a ramp of
    // each material's shades).
    Color amb = FromArgb(d->ls.ambient);
    if (d->kind == kRamp) amb.r = amb.g = amb.b = 0.3f * amb.r + 0.59f * amb.g + 0.11f * amb.b;
    const bool spec = d->rs[29] == TRUE && mat.dvPower > 0.001f;
    const bool fog = d->rs[28] == TRUE && d->ls.fogMode != D3DFOG_NONE;
    const bool rangeFog = d->rs[48] == TRUE;
    const float fs = F(d->ls.fogStart), fe = F(d->ls.fogEnd), fd = F(d->ls.fogDensity);
    const Vec3 origin = {wv._41, wv._42, wv._43};
    const VpMap map = d->map;
    float ext[4] = {d->ext[0], d->ext[1], d->ext[2], d->ext[3]};
    DWORD orCodes = 0, andCodes = 0xFFFF;

    // Positions, clip codes, screen coordinates and extents: every vertex where the clip
    // status or the extents keep something of it, else only those drawn (lo..hi).
    const DWORD aFrom = lo, aTo = hi >= n ? n : hi + 1;  // the vertices drawn
    const bool all = clip || extents;
    const DWORD pFrom = all ? 0 : aFrom, pTo = all ? n : aTo;
    auto extend = [&](const PVert& o) {
        ext[0] = std::min(ext[0], o.sx);
        ext[1] = std::min(ext[1], o.sy);
        ext[2] = std::max(ext[2], o.sx);
        ext[3] = std::max(ext[3], o.sy);
    };
    auto position = [&](DWORD i) {  // (the reference)
        const float* p = reinterpret_cast<const float*>(src + size_t(i) * stride);
        PVert& o = out[i];
        const float x = p[0], y = p[1], zz = p[2];
        o.cx = x * M._11 + y * M._21 + zz * M._31 + M._41;
        o.cy = x * M._12 + y * M._22 + zz * M._32 + M._42;
        o.cz = x * M._13 + y * M._23 + zz * M._33 + M._43;
        o.cw = x * M._14 + y * M._24 + zz * M._34 + M._44;
        WORD code = 0;
        if (clip) {
            if (o.cx < 0) code |= D3DCLIP_LEFT;
            if (o.cx > o.cw) code |= D3DCLIP_RIGHT;
            if (o.cy > o.cw) code |= D3DCLIP_TOP;
            if (o.cy < 0) code |= D3DCLIP_BOTTOM;
            if (o.cz < 0) code |= D3DCLIP_FRONT;
            if (o.cz > o.cw) code |= D3DCLIP_BACK;
            orCodes |= code;
            andCodes &= code;
        }
        o.code = code;
        if (!code) {
            Project(map, &o);
            if (extents) extend(o);
        } else {  // XP's TL buffer: an outside vertex keeps its clip coordinates there
            o.sx = o.cx;
            o.sy = o.cy;
            o.sz = o.cz;
            o.rhw = o.cw;
        }
    };
    DWORD i = pFrom;
    if (pTo - std::min(pFrom, pTo) >= 4) {
        const __m128 zero = _mm_setzero_ps();
        const __m128 mX = Splat(map.X), mW = Splat(map.W), mYH = Splat(map.Y + map.H), mH = Splat(map.H);
        for (; i + 4 <= pTo; i += 4) {
            const uint8_t* v[4] = {src + size_t(i) * stride, src + size_t(i + 1) * stride, src + size_t(i + 2) * stride,
                                   src + size_t(i + 3) * stride};
            __m128 x, y, z;
            Load3(v, 0, &x, &y, &z);
            auto row = [&](float a, float b, float c, float e) {
                return _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(x, Splat(a)), _mm_mul_ps(y, Splat(b))), _mm_mul_ps(z, Splat(c))),
                                  Splat(e));
            };
            __m128 cx = row(M._11, M._21, M._31, M._41), cy = row(M._12, M._22, M._32, M._42),
                   cz = row(M._13, M._23, M._33, M._43), cw = row(M._14, M._24, M._34, M._44);
            alignas(16) int codes[4] = {0, 0, 0, 0};
            __m128i code4 = _mm_setzero_si128();
            if (clip) {
                auto bit = [](__m128 m, int b) { return _mm_and_si128(_mm_castps_si128(m), _mm_set1_epi32(b)); };
                code4 = _mm_or_si128(_mm_or_si128(_mm_or_si128(bit(_mm_cmplt_ps(cx, zero), D3DCLIP_LEFT),
                                                               bit(_mm_cmpgt_ps(cx, cw), D3DCLIP_RIGHT)),
                                                  _mm_or_si128(bit(_mm_cmpgt_ps(cy, cw), D3DCLIP_TOP),
                                                               bit(_mm_cmplt_ps(cy, zero), D3DCLIP_BOTTOM))),
                                     _mm_or_si128(bit(_mm_cmplt_ps(cz, zero), D3DCLIP_FRONT), bit(_mm_cmpgt_ps(cz, cw), D3DCLIP_BACK)));
                _mm_store_si128(reinterpret_cast<__m128i*>(codes), code4);
                orCodes |= DWORD(codes[0] | codes[1] | codes[2] | codes[3]);
                andCodes &= DWORD(codes[0] & codes[1] & codes[2] & codes[3]);
            }
            // Project: rhw 1 / w (w 0: 2097152), then the viewport mapping.
            const __m128 rhw = Sel(_mm_cmpneq_ps(cw, zero), _mm_div_ps(Splat(1.0f), cw), Splat(2097152.0f));
            __m128 sx = _mm_add_ps(mX, _mm_mul_ps(_mm_mul_ps(mW, cx), rhw));
            __m128 sy = _mm_sub_ps(mYH, _mm_mul_ps(_mm_mul_ps(mH, cy), rhw));
            __m128 sz = _mm_mul_ps(cz, rhw), sw = rhw;
            const __m128 outside = _mm_castsi128_ps(_mm_cmpgt_epi32(code4, _mm_setzero_si128()));
            sx = Sel(outside, cx, sx);
            sy = Sel(outside, cy, sy);
            sz = Sel(outside, cz, sz);
            sw = Sel(outside, cw, sw);
            _MM_TRANSPOSE4_PS(cx, cy, cz, cw);
            _MM_TRANSPOSE4_PS(sx, sy, sz, sw);
            const __m128 c4[4] = {cx, cy, cz, cw}, s4[4] = {sx, sy, sz, sw};
            for (int k = 0; k < 4; k++) {
                PVert& o = out[i + k];
                _mm_storeu_ps(&o.cx, c4[k]);
                _mm_storeu_ps(&o.sx, s4[k]);
                o.code = WORD(codes[k]);
                if (extents && !codes[k]) extend(o);  // in vertex order, as the scalar code
            }
        }
    }
    for (; i < pTo; i++) position(i);

    // Colours (lit, fogged) and texture coordinates of the vertices drawn.
    auto attributes = [&](DWORD i) {  // (the reference)
        const uint8_t* v = src + size_t(i) * stride;
        PVert& o = out[i];
        const float* p = reinterpret_cast<const float*>(v);
        const float x = p[0], y = p[1], zz = p[2];
        const DWORD vd = in.diffuse >= 0 ? *reinterpret_cast<const DWORD*>(v + in.diffuse) : 0xFFFFFFFF;
        const DWORD vs = in.specular >= 0 ? *reinterpret_cast<const DWORD*>(v + in.specular) : 0;
        DWORD diffuse = vd, specular = vs;
        Vec3 P = {0, 0, 0};
        if (light || fog) P = XformPoint({x, y, zz}, wv);
        if (light) {
            // C §2.2: the D3DLIGHT (DX3/5) and D3DLIGHT2 (DX6) models; COLORVERTEX takes the
            // vertex colours for the material's diffuse and specular (B §2.3).
            Color md = FromCv(mat.diffuse), ms = FromCv(mat.specular);
            if (d->ls.colorVertex && in.diffuse >= 0) md = FromArgb(vd);
            if (d->ls.colorVertex && in.specular >= 0) ms = FromArgb(vs);
            const float* nn = reinterpret_cast<const float*>(v + in.normal);
            const Vec3 n0 = {nn[0], nn[1], nn[2]};
            // Lit in model space on XP: the normal keeps its length through world and view.
            const Vec3 N = [&] {
                const Vec3 t = XformDir(n0, wv);
                const float tt = Dot(t, t);
                if (!(tt > 0)) return t;
                const float k = sqrtf(Dot(n0, n0) / tt);  // |n0| / |t|: one root
                return Vec3{t.x * k, t.y * k, t.z * k};
            }();
            float dr = mat.emissive.r + amb.r * mat.ambient.r, dg = mat.emissive.g + amb.g * mat.ambient.g,
                  db = mat.emissive.b + amb.b * mat.ambient.b;
            float sr = 0, sg = 0, sb = 0;
            Vec3 V = {0, 0, 0};  // toward the eye: for specular only
            if (spec) V = Norm({-P.x, -P.y, -P.z});
            for (const LightC* lp = lights.data(); lp != lightsEnd; lp++) {
                const LightC& l = *lp;
                Vec3 L;
                float f = 1.0f;
                if (l.type == D3DLIGHT_DIRECTIONAL) {
                    L = {-l.dir.x, -l.dir.y, -l.dir.z};
                } else if (l.type == D3DLIGHT_PARALLELPOINT) {
                    L = Norm(Sub(l.pos, origin));  // from the position toward the model's origin
                } else if (l.type == D3DLIGHT_POINT || l.type == D3DLIGHT_SPOT) {
                    const Vec3 toL = Sub(l.pos, P);
                    const float d2 = Dot(toL, toL), dist = sqrtf(d2);
                    if (l.v2) {
                        if (d2 >= l.range2) continue;
                        const float t = l.range > 0 ? (l.range - dist) / l.range : 0.0f;
                        f = (l.a0 != 0 ? l.a0 : 0.0f) + (l.a1 != 0 ? l.a1 * t : 0.0f) + (l.a2 != 0 ? l.a2 * t * t : 0.0f);
                    } else {
                        float att = l.a0 + l.a1 * dist + l.a2 * d2;
                        if (att > 1000.0f) continue;
                        if (att <= 1.0f) att = 1.0f;
                        f = 1.0f / att;
                    }
                    L = dist > 0 ? Vec3{toL.x / dist, toL.y / dist, toL.z / dist} : Vec3{0, 0, 1};
                    if (l.type == D3DLIGHT_SPOT) {
                        const float ca = -Dot(L, l.dir);
                        if (ca <= l.cosPhi) continue;
                        if (ca < l.cosTheta) {
                            if (l.v2) {
                                const float s = (ca - l.cosPhi) * l.k;
                                f *= l.falloff == 1.0f ? s : Pow1(s, l.falloff);
                            } else {
                                f *= 1.0f - (l.cosTheta - ca) * l.k;
                            }
                        }
                    }
                } else {
                    continue;  // no lighting function (XP likely faults)
                }
                const float nl = Dot(N, L);
                if (nl <= 0) continue;
                dr += l.color.r * md.r * nl * f;
                dg += l.color.g * md.g * nl * f;
                db += l.color.b * md.b * nl * f;
                if (spec && !l.noSpec) {
                    const Vec3 H = Norm({L.x + V.x, L.y + V.y, L.z + V.z});
                    const float nh = Dot(N, H);
                    if (nh > 0) {
                        const float k = Pow1(nh, mat.dvPower) * f;
                        sr += l.color.r * ms.r * k;
                        sg += l.color.g * ms.g * k;
                        sb += l.color.b * ms.b * k;
                    }
                }
            }
            diffuse = ColorToArgb(dr, dg, db, md.a);
            specular = ColorToArgb(sr, sg, sb, 0.0f);  // alpha 0 (Wine's test_lighting on Windows)
        }
        if (fog) {  // vertex fog from the light states, into specular alpha
            const float dist = rangeFog ? sqrtf(Dot(P, P)) : P.z;
            float f = 1.0f;
            if (d->ls.fogMode == D3DFOG_LINEAR) f = fe != fs ? (fe - dist) / (fe - fs) : (dist < fs ? 1.0f : 0.0f);
            else if (d->ls.fogMode == D3DFOG_EXP) f = expf(-dist * fd);
            else if (d->ls.fogMode == D3DFOG_EXP2) f = expf(-(dist * fd) * (dist * fd));
            f = std::clamp(f, 0.0f, 1.0f);
            specular = (specular & 0x00FFFFFF) | (TruncToDword(f * 255.0f + 0.5f) << 24);
        }
        o.diffuse = diffuse;
        o.specular = specular;
        for (DWORD t = 0; t < in.texCount; t++) memcpy(o.tex[t], v + in.tex[t], 8);
    };
    i = aFrom;
    if ((light || fog) && aTo > aFrom && aTo - aFrom >= 4) {
        const __m128 zero = _mm_setzero_ps(), one = Splat(1.0f);
        const bool cvD = light && d->ls.colorVertex && in.diffuse >= 0, cvS = light && d->ls.colorVertex && in.specular >= 0;
        const __m128 dr0 = Splat(mat.emissive.r + amb.r * mat.ambient.r), dg0 = Splat(mat.emissive.g + amb.g * mat.ambient.g),
                     db0 = Splat(mat.emissive.b + amb.b * mat.ambient.b);
        for (; i + 4 <= aTo; i += 4) {
            const uint8_t* v[4] = {src + size_t(i) * stride, src + size_t(i + 1) * stride, src + size_t(i + 2) * stride,
                                   src + size_t(i + 3) * stride};
            DWORD vd[4], vs[4];
            for (int k = 0; k < 4; k++) {
                vd[k] = in.diffuse >= 0 ? *reinterpret_cast<const DWORD*>(v[k] + in.diffuse) : 0xFFFFFFFF;
                vs[k] = in.specular >= 0 ? *reinterpret_cast<const DWORD*>(v[k] + in.specular) : 0;
            }
            DWORD diffuse[4] = {vd[0], vd[1], vd[2], vd[3]}, specular[4] = {vs[0], vs[1], vs[2], vs[3]};
            __m128 x, y, z, Px, Py, Pz;
            Load3(v, 0, &x, &y, &z);
            Xform3(x, y, z, wv, true, &Px, &Py, &Pz);
            if (light) {
                __m128 mdr = Splat(mat.diffuse.r), mdg = Splat(mat.diffuse.g), mdb = Splat(mat.diffuse.b), mda = Splat(mat.diffuse.a);
                __m128 msr = Splat(mat.specular.r), msg = Splat(mat.specular.g), msb = Splat(mat.specular.b);
                if (cvD) {
                    const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(vd));
                    mdr = Channel(c, 16), mdg = Channel(c, 8), mdb = Channel(c, 0), mda = Channel(c, 24);
                }
                if (cvS) {
                    const __m128i c = _mm_loadu_si128(reinterpret_cast<const __m128i*>(vs));
                    msr = Channel(c, 16), msg = Channel(c, 8), msb = Channel(c, 0);
                }
                __m128 nx, ny, nz, tx, ty, tz;
                Load3(v, in.normal, &nx, &ny, &nz);
                Xform3(nx, ny, nz, wv, false, &tx, &ty, &tz);
                const __m128 tt = Dot3(tx, ty, tz, tx, ty, tz);
                const __m128 kN = _mm_sqrt_ps(_mm_div_ps(Dot3(nx, ny, nz, nx, ny, nz), tt));
                const __m128 okN = _mm_cmpgt_ps(tt, zero);
                const __m128 Nx = Sel(okN, _mm_mul_ps(tx, kN), tx), Ny = Sel(okN, _mm_mul_ps(ty, kN), ty),
                             Nz = Sel(okN, _mm_mul_ps(tz, kN), tz);
                __m128 dr = dr0, dg = dg0, db = db0, sr = zero, sg = zero, sb = zero;
                __m128 Vx = zero, Vy = zero, Vz = zero;
                if (spec) {
                    Vx = Neg(Px), Vy = Neg(Py), Vz = Neg(Pz);
                    Norm3(&Vx, &Vy, &Vz);
                }
                for (const LightC* lp = lights.data(); lp != lightsEnd; lp++) {
                    const LightC& l = *lp;
                    __m128 Lx, Ly, Lz, f = one, inc = _mm_cmpeq_ps(zero, zero);
                    if (l.type == D3DLIGHT_DIRECTIONAL) {
                        Lx = Splat(-l.dir.x), Ly = Splat(-l.dir.y), Lz = Splat(-l.dir.z);
                    } else if (l.type == D3DLIGHT_PARALLELPOINT) {
                        const Vec3 L = Norm(Sub(l.pos, origin));
                        Lx = Splat(L.x), Ly = Splat(L.y), Lz = Splat(L.z);
                    } else if (l.type == D3DLIGHT_POINT || l.type == D3DLIGHT_SPOT) {
                        const __m128 tLx = _mm_sub_ps(Splat(l.pos.x), Px), tLy = _mm_sub_ps(Splat(l.pos.y), Py),
                                     tLz = _mm_sub_ps(Splat(l.pos.z), Pz);
                        const __m128 d2 = Dot3(tLx, tLy, tLz, tLx, tLy, tLz), dist = _mm_sqrt_ps(d2);
                        if (l.v2) {
                            inc = _mm_cmpnge_ps(d2, Splat(l.range2));
                            const __m128 t = l.range > 0 ? _mm_div_ps(_mm_sub_ps(Splat(l.range), dist), Splat(l.range)) : zero;
                            const __m128 f1 = l.a1 != 0 ? _mm_mul_ps(Splat(l.a1), t) : zero;
                            const __m128 f2 = l.a2 != 0 ? _mm_mul_ps(_mm_mul_ps(Splat(l.a2), t), t) : zero;
                            f = _mm_add_ps(_mm_add_ps(Splat(l.a0 != 0 ? l.a0 : 0.0f), f1), f2);
                        } else {
                            __m128 att = _mm_add_ps(_mm_add_ps(Splat(l.a0), _mm_mul_ps(Splat(l.a1), dist)), _mm_mul_ps(Splat(l.a2), d2));
                            inc = _mm_cmpngt_ps(att, Splat(1000.0f));
                            att = Sel(_mm_cmple_ps(att, one), one, att);
                            f = _mm_div_ps(one, att);
                        }
                        const __m128 pos = _mm_cmpgt_ps(dist, zero);
                        Lx = Sel(pos, _mm_div_ps(tLx, dist), zero);
                        Ly = Sel(pos, _mm_div_ps(tLy, dist), zero);
                        Lz = Sel(pos, _mm_div_ps(tLz, dist), one);
                        if (l.type == D3DLIGHT_SPOT) {
                            const __m128 ca = Neg(Dot3(Lx, Ly, Lz, Splat(l.dir.x), Splat(l.dir.y), Splat(l.dir.z)));
                            inc = _mm_and_ps(inc, _mm_cmpnle_ps(ca, Splat(l.cosPhi)));
                            const __m128 edge = _mm_and_ps(inc, _mm_cmplt_ps(ca, Splat(l.cosTheta)));
                            if (_mm_movemask_ps(edge)) {
                                __m128 g;
                                if (l.v2) {
                                    g = _mm_mul_ps(_mm_sub_ps(ca, Splat(l.cosPhi)), Splat(l.k));
                                    if (l.falloff != 1.0f) g = Pow4(g, l.falloff, edge);
                                } else {
                                    g = _mm_sub_ps(one, _mm_mul_ps(_mm_sub_ps(Splat(l.cosTheta), ca), Splat(l.k)));
                                }
                                f = Sel(edge, _mm_mul_ps(f, g), f);
                            }
                        }
                    } else {
                        continue;  // no lighting function
                    }
                    const __m128 nl = Dot3(Nx, Ny, Nz, Lx, Ly, Lz);
                    inc = _mm_and_ps(inc, _mm_cmpnle_ps(nl, zero));
                    if (!_mm_movemask_ps(inc)) continue;
                    dr = Sel(inc, _mm_add_ps(dr, _mm_mul_ps(_mm_mul_ps(_mm_mul_ps(Splat(l.color.r), mdr), nl), f)), dr);
                    dg = Sel(inc, _mm_add_ps(dg, _mm_mul_ps(_mm_mul_ps(_mm_mul_ps(Splat(l.color.g), mdg), nl), f)), dg);
                    db = Sel(inc, _mm_add_ps(db, _mm_mul_ps(_mm_mul_ps(_mm_mul_ps(Splat(l.color.b), mdb), nl), f)), db);
                    if (spec && !l.noSpec) {
                        __m128 Hx = _mm_add_ps(Lx, Vx), Hy = _mm_add_ps(Ly, Vy), Hz = _mm_add_ps(Lz, Vz);
                        Norm3(&Hx, &Hy, &Hz);
                        const __m128 nh = Dot3(Nx, Ny, Nz, Hx, Hy, Hz);
                        const __m128 lit = _mm_and_ps(inc, _mm_cmpgt_ps(nh, zero));
                        if (_mm_movemask_ps(lit)) {
                            const __m128 k = _mm_mul_ps(Pow4(nh, mat.dvPower, lit), f);
                            sr = Sel(lit, _mm_add_ps(sr, _mm_mul_ps(_mm_mul_ps(Splat(l.color.r), msr), k)), sr);
                            sg = Sel(lit, _mm_add_ps(sg, _mm_mul_ps(_mm_mul_ps(Splat(l.color.g), msg), k)), sg);
                            sb = Sel(lit, _mm_add_ps(sb, _mm_mul_ps(_mm_mul_ps(Splat(l.color.b), msb), k)), sb);
                        }
                    }
                }
                ToArgb4(dr, dg, db, mda, diffuse);
                ToArgb4(sr, sg, sb, zero, specular);
            }
            if (fog) {
                const __m128 dist = rangeFog ? _mm_sqrt_ps(Dot3(Px, Py, Pz, Px, Py, Pz)) : Pz;
                __m128 f = one;
                const DWORD mode = d->ls.fogMode;
                if (mode == D3DFOG_LINEAR) {
                    f = fe != fs ? _mm_div_ps(_mm_sub_ps(Splat(fe), dist), Splat(fe - fs))
                                 : Sel(_mm_cmplt_ps(dist, Splat(fs)), one, zero);
                } else if (mode == D3DFOG_EXP || mode == D3DFOG_EXP2) {
                    f = PerLane(dist, _mm_cmpeq_ps(zero, zero), [&](float dd, int) {
                        return mode == D3DFOG_EXP ? expf(-dd * fd) : expf(-(dd * fd) * (dd * fd));
                    });
                }
                // std::clamp(f, 0, 1) * 255 + 0.5, truncated, into the alpha (a NaN stays NaN
                // through the clamp and truncates to 0x80000000, as TruncToDword: alpha 0).
                const __m128 c = Sel(_mm_cmplt_ps(f, zero), zero, Sel(_mm_cmplt_ps(one, f), one, f));
                const __m128i fa = _mm_slli_epi32(_mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(c, Splat(255.0f)), Splat(0.5f))), 24);
                __m128i sp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(specular));
                sp = _mm_or_si128(_mm_and_si128(sp, _mm_set1_epi32(0x00FFFFFF)), fa);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(specular), sp);
            }
            for (int k = 0; k < 4; k++) {
                PVert& o = out[i + k];
                o.diffuse = diffuse[k];
                o.specular = specular[k];
                for (DWORD t = 0; t < in.texCount; t++) memcpy(o.tex[t], v[k] + in.tex[t], 8);
            }
        }
    }
    for (; i < aTo; i++) attributes(i);
    if (extents) memcpy(d->ext, ext, sizeof(ext));
    if (clip && n) d->status = (d->status | (orCodes & 0xFFF)) & ((DWORD(andCodes) << 12) | 0xFF000FFF);
}

// ---------------------------------------------------------------- clipping and drawing

namespace {

// Guard-band clip codes: which of the six planes a vertex is outside.
WORD GbCode(const PVert& p) {
    WORD c = 0;
    if (p.cx < -kGuard * p.cw) c |= 1;
    if (p.cx > (1 + kGuard) * p.cw) c |= 2;
    if (p.cy < -kGuard * p.cw) c |= 4;
    if (p.cy > (1 + kGuard) * p.cw) c |= 8;
    if (p.cz < 0) c |= 0x10;
    if (p.cz > p.cw) c |= 0x20;
    return c;
}

float PlaneDist(const PVert& p, int plane) {
    switch (plane) {
    case 0: return p.cx + kGuard * p.cw;
    case 1: return (1 + kGuard) * p.cw - p.cx;
    case 2: return p.cy + kGuard * p.cw;
    case 3: return (1 + kGuard) * p.cw - p.cy;
    case 4: return p.cz;
    default: return p.cw - p.cz;
    }
}

PVert Lerp(const PVert& a, const PVert& b, float t, DWORD ntex) {
    PVert r;
    r.cx = a.cx + (b.cx - a.cx) * t;
    r.cy = a.cy + (b.cy - a.cy) * t;
    r.cz = a.cz + (b.cz - a.cz) * t;
    r.cw = a.cw + (b.cw - a.cw) * t;
    auto mix = [&](DWORD x, DWORD y) {
        DWORD o = 0;
        for (int s = 0; s < 32; s += 8) {
            const float u = float((x >> s) & 0xFF), v = float((y >> s) & 0xFF);
            o |= TruncToDword(std::clamp(u + (v - u) * t + 0.5f, 0.0f, 255.0f)) << s;
        }
        return o;
    };
    r.diffuse = mix(a.diffuse, b.diffuse);
    r.specular = mix(a.specular, b.specular);
    for (DWORD k = 0; k < ntex; k++) {
        r.tex[k][0] = a.tex[k][0] + (b.tex[k][0] - a.tex[k][0]) * t;
        r.tex[k][1] = a.tex[k][1] + (b.tex[k][1] - a.tex[k][1]) * t;
    }
    r.code = 0;
    return r;
}

// The clip volume itself (no guard band), its planes in D3DCLIP_* order (LEFT, RIGHT, TOP,
// BOTTOM, FRONT, BACK), as PVert::code has them: Pick clips with these.
float VolumeDist(const PVert& p, int plane) {
    switch (plane) {
    case 0: return p.cx;
    case 1: return p.cw - p.cx;
    case 2: return p.cw - p.cy;
    case 3: return p.cy;
    case 4: return p.cz;
    default: return p.cw - p.cz;
    }
}

// Sutherland-Hodgman against the planes in `mask` (GbCode's guard-band planes, or with
// `volume` VolumeDist's); the first vertex stays first when inside. A triangle gains at most
// one vertex per plane: 9 at most (kMaxPoly).
constexpr int kMaxPoly = 12;
int ClipPoly(PVert (&poly)[kMaxPoly], int n, WORD mask, DWORD ntex, bool volume = false) {
    PVert next[kMaxPoly];
    for (int plane = 0; plane < 6 && n >= 2; plane++) {
        if (!(mask & (1 << plane))) continue;
        int m = 0;
        for (int i = 0; i < n; i++) {
            const PVert& a = poly[i];
            const PVert& b = poly[i + 1 < n ? i + 1 : 0];
            const float da = volume ? VolumeDist(a, plane) : PlaneDist(a, plane);
            const float db = volume ? VolumeDist(b, plane) : PlaneDist(b, plane);
            if (da >= 0 && m < kMaxPoly) next[m++] = a;
            if ((da >= 0) != (db >= 0) && m < kMaxPoly) next[m++] = Lerp(a, b, da / (da - db), ntex);
        }
        for (int k = 0; k < m; k++) poly[k] = next[k];
        n = m;
    }
    return n;
}

// Output vertices: XYZRHW, diffuse, specular, ntex pairs of texture coordinates.
struct Emitter {
    DWORD ntex;
    UINT stride;
    std::vector<uint8_t>& buf;
    DWORD count = 0;
    // expect: the vertices likely added (more grows the buffer).
    Emitter(DWORD nt, std::vector<uint8_t>& b, size_t expect) : ntex(nt), stride(24 + 8 * nt), buf(b) {
        Expect(expect);
    }
    void Expect(size_t more) {
        if (buf.size() < (count + more) * stride) buf.resize((count + more) * stride);
    }
    void Add(const PVert& p) {
        const size_t o = size_t(count) * stride;
        if (o + stride > buf.size()) buf.resize(std::max(buf.size() * 2, o + stride));
        float* f = reinterpret_cast<float*>(buf.data() + o);
        f[0] = p.sx;
        f[1] = p.sy;
        f[2] = p.sz;
        f[3] = p.rhw;
        DWORD* dw = reinterpret_cast<DWORD*>(f + 4);
        dw[0] = p.diffuse;
        dw[1] = p.specular;
        // (The bound in sight: a copy of unknown size is a library call per vertex.)
        memcpy(buf.data() + o + 24, p.tex, 8 * std::min<DWORD>(ntex, 8));
        count++;
    }
};

}  // namespace

namespace {

// What DrawPVerts draws of one primitive list (EmitPVerts).
struct PDraw {
    D3DPRIMITIVETYPE type;
    UINT prims;
    bool indexed;  // with indices (else its vertices in order)
};

// The vertices DrawPVerts draws for one primitive list, added after those in `e`, and its
// indices after those in `outIdx` (counting from the first vertex in `e`). seqIdx: indices
// for vertices drawn in order too (0, 1, 2... from where they start), as part of several.
// False: nothing to draw.
// gb: the vertex range's guard-band codes ORed, when known (outside none of them all).
bool EmitPVerts(Device* d, D3DPRIMITIVETYPE t, const PVert* v, DWORD n, const WORD* idx, DWORD ni, bool clip,
                DWORD ntex, Emitter& out, std::vector<WORD>& outIdx, bool seqIdx, PDraw* r, const WORD* gb = nullptr) {
    if (!ValidPrim(t) || !n) return false;
    // Worked on as a local (its count kept in a register, not reloaded after every byte
    // written), put back at the end.
    Emitter e = out;
    // The vertices referenced.
    DWORD lo = 0, hi = n - 1;
    if (idx) {
        if (!ni) return false;
        lo = 0xFFFF;
        hi = 0;
        for (DWORD i = 0; i < ni; i++) {
            lo = std::min<DWORD>(lo, idx[i]);
            hi = std::max<DWORD>(hi, idx[i]);
        }
        if (hi >= n) return false;  // past the vertices (XP reads whatever is there)
    }
    WORD orGb = gb ? *gb : 0, andGb = 0xFFFF;
    if (clip && !gb) {
        for (DWORD i = lo; i <= hi; i++) {
            const WORD c = GbCode(v[i]);
            orGb |= c;
            andGb &= c;
        }
        if (andGb) return false;  // all outside one plane
    }
    const DWORD first = e.count;
    e.Expect(hi - lo + 1);
    D3DPRIMITIVETYPE outType = t;
    bool indexed = false;
    UINT prims;
    if (!clip || !orGb) {
        // Everything inside the guard band: as it is, in screen coordinates.
        for (DWORD i = lo; i <= hi; i++) {
            if (clip && v[i].code) {
                PVert p = v[i];
                Project(d->map, &p);
                e.Add(p);
            } else {
                e.Add(v[i]);
            }
        }
        if (idx) {
            const size_t at = outIdx.size();
            outIdx.resize(at + ni);
            WORD* o = outIdx.data() + at;
            for (DWORD i = 0; i < ni; i++) o[i] = WORD(first + idx[i] - lo);
            indexed = true;
        }
        prims = PrimCount(t, idx ? ni : n);
    } else {
        // Primitive by primitive, clipped where they leave the guard band.
        auto at = [&](DWORD k) -> const PVert& { return v[idx ? idx[k] : k]; };
        const DWORD count = idx ? ni : n;
        auto addProjected = [&](PVert p) {
            Project(d->map, &p);
            e.Add(p);
        };
        if (t == D3DPT_POINTLIST) {
            outType = D3DPT_POINTLIST;
            for (DWORD k = 0; k < count; k++)
                if (!GbCode(at(k))) addProjected(at(k));
            prims = e.count - first;
        } else if (t == D3DPT_LINELIST || t == D3DPT_LINESTRIP) {
            outType = D3DPT_LINELIST;
            const DWORD step = t == D3DPT_LINELIST ? 2 : 1;
            for (DWORD k = 0; k + 1 < count; k += step) {
                PVert a = at(k), b = at(k + 1);
                const WORD ca = GbCode(a), cb = GbCode(b);
                if (ca & cb) continue;
                bool keep = true;
                for (int plane = 0; plane < 6 && keep; plane++) {
                    if (!((ca | cb) & (1 << plane))) continue;
                    const float da = PlaneDist(a, plane), db = PlaneDist(b, plane);
                    if (da < 0 && db < 0) keep = false;
                    else if (da < 0) a = Lerp(a, b, da / (da - db), ntex);
                    else if (db < 0) b = Lerp(a, b, da / (da - db), ntex);
                }
                if (!keep) continue;
                addProjected(a);
                addProjected(b);
            }
            prims = (e.count - first) / 2;
        } else {
            outType = D3DPT_TRIANGLELIST;
            PVert poly[kMaxPoly];
            const DWORD tris = PrimCount(t, count);
            for (DWORD k = 0; k < tris; k++) {
                DWORD i0, i1, i2;
                if (t == D3DPT_TRIANGLELIST) {
                    i0 = 3 * k, i1 = 3 * k + 1, i2 = 3 * k + 2;
                } else if (t == D3DPT_TRIANGLESTRIP) {
                    // Odd triangles reversed (as Direct3D draws strips), first vertex kept.
                    i0 = k;
                    i1 = (k & 1) ? k + 2 : k + 1;
                    i2 = (k & 1) ? k + 1 : k + 2;
                } else {  // fan
                    i0 = k + 1, i1 = k + 2, i2 = 0;
                }
                const PVert &a = at(i0), &b = at(i1), &c = at(i2);
                const WORD ca = GbCode(a), cb = GbCode(b), cc = GbCode(c);
                if (ca & cb & cc) continue;
                if (!(ca | cb | cc)) {
                    addProjected(a);
                    addProjected(b);
                    addProjected(c);
                    continue;
                }
                poly[0] = a, poly[1] = b, poly[2] = c;
                const int pn = ClipPoly(poly, 3, ca | cb | cc, ntex);
                for (int j = 1; j + 1 < pn; j++) {
                    addProjected(poly[0]);
                    addProjected(poly[j]);
                    addProjected(poly[j + 1]);
                }
            }
            prims = (e.count - first) / 3;
        }
    }
    out.count = e.count;
    if (!prims || e.count == first) return false;
    if (seqIdx && !indexed) {
        const size_t at = outIdx.size();
        outIdx.resize(at + (e.count - first));
        for (DWORD i = first; i < e.count; i++) outIdx[at + (i - first)] = WORD(i);
    }
    r->type = outType, r->prims = prims, r->indexed = indexed;
    return true;
}

// first, count: the vertices of e's drawn (count 0: all).
void SubmitPVerts(Device* d, D3DPRIMITIVETYPE t, const Emitter& e, const WORD* idx, DWORD ni, UINT prims,
                  const GpuContext::DrawSeg* segs, UINT nseg, DWORD first = 0, DWORD count = 0) {
    Device::Use u(d);
    if (!u.ok) return;
    d->BindTextures9();
    const DWORD fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | (e.ntex << D3DFVF_TEXCOUNT_SHIFT);
    d->ctx.Draw(t, fvf, e.buf.data() + size_t(first) * e.stride, e.stride, count ? count : e.count, idx, ni, prims,
                d->rs[7] != D3DZB_FALSE, segs, nseg);
}

}  // namespace

void DrawPVerts(Device* d, D3DPRIMITIVETYPE t, const PVert* v, DWORD n, const WORD* idx, DWORD ni, bool clip,
                DWORD ntex) {
    ntex = std::min<DWORD>(ntex, 8);
    Emitter e(ntex, t_out, 0);
    std::vector<WORD>& outIdx = t_idx;
    outIdx.clear();
    PDraw r;
    if (!EmitPVerts(d, t, v, n, idx, ni, clip, ntex, e, outIdx, false, &r)) return;
    SubmitPVerts(d, r.type, e, r.indexed ? outIdx.data() : nullptr, r.indexed ? DWORD(outIdx.size()) : 0, r.prims,
                 nullptr, 0);
}

void DrawPVertsBatch(Device* d, D3DPRIMITIVETYPE t, const PVert* v, DWORD n, const WORD* idx,
                     const std::vector<std::pair<DWORD, DWORD>>& parts, bool clip) {
    if (parts.size() == 1) return DrawPVerts(d, t, v, n, idx + parts[0].first, parts[0].second, clip, 1);
    Emitter e(1, t_bout, 0);
    std::vector<WORD>& outIdx = t_bidx;
    std::vector<GpuContext::DrawSeg>& segs = t_segs;
    std::vector<BatchPending>& pend = t_pend;
    pend.clear();
    // The lists drawn as they are (inside the guard band, or unclipped) share the vertices
    // from the lowest index of theirs to the highest, each its own part of them as drawn
    // alone; clipped ones are cut primitive by primitive, their vertices after those. The
    // indices keep the lists' order.
    DWORD lo0 = 0, hi0 = 0, span = 0;
    bool anyRange = false;
    // Draws what is waiting: as few draws as 16-bit indices (and 65535 primitives, old
    // drivers' most at once) allow.
    auto submit = [&] {
        size_t start = 0;
        while (start < pend.size()) {
            e.count = 0, outIdx.clear(), segs.clear();
            UINT prims = 0;
            DWORD l = 0xFFFF, h = 0;
            for (size_t j = start; j < pend.size(); j++)
                if (!pend[j].clipped) l = std::min(l, pend[j].lo), h = std::max(h, pend[j].hi);
            if (l <= h) {
                e.Expect(h - l + 1);
                for (DWORD i = l; i <= h; i++) {
                    if (clip && v[i].code) {
                        PVert p = v[i];
                        Project(d->map, &p);
                        e.Add(p);
                    } else {
                        e.Add(v[i]);
                    }
                }
            }
            size_t j = start;
            for (; j < pend.size(); j++) {
                const BatchPending& q = pend[j];
                if (!q.clipped) {
                    if (!segs.empty() && prims + q.prims > 0xFFFF) break;
                    const size_t at = outIdx.size();
                    segs.push_back({q.lo - l, q.hi - q.lo + 1, UINT(at), q.count, q.prims, true});
                    outIdx.resize(at + q.count);
                    for (DWORD k = 0; k < q.count; k++) outIdx[at + k] = WORD(idx[q.off + k] - l);
                    prims += q.prims;
                    continue;
                }
                const DWORD first = e.count;
                const size_t firstIdx = outIdx.size();
                PDraw r;
                const bool any = EmitPVerts(d, t, v, n, idx + q.off, q.count, clip, 1, e, outIdx, true, &r, &q.orGb);
                if (any && e.count <= 0x10000 && (segs.empty() || prims + r.prims <= 0xFFFF)) {
                    segs.push_back({first, e.count - first, UINT(firstIdx), UINT(outIdx.size() - firstIdx), r.prims, r.indexed});
                    prims += r.prims;
                    continue;
                }
                e.count = first;
                outIdx.resize(firstIdx);
                if (!any) continue;
                if (segs.empty()) {  // too many for one draw's indices even alone: as DrawPVerts draws it
                    DrawPVerts(d, t, v, n, idx + q.off, q.count, clip, 1);
                    j++;
                }
                break;
            }
            if (segs.size() == 1) {  // as DrawPVerts draws it alone
                const GpuContext::DrawSeg& g = segs[0];
                if (g.indexed)
                    for (WORD& i : outIdx) i = WORD(i - g.firstVertex);
                SubmitPVerts(d, t, e, g.indexed ? outIdx.data() : nullptr, g.indexed ? DWORD(outIdx.size()) : 0, prims,
                             nullptr, 0, g.firstVertex, g.vertexCount);
            } else if (!segs.empty()) {
                SubmitPVerts(d, t, e, outIdx.data(), DWORD(outIdx.size()), prims, segs.data(), UINT(segs.size()));
            }
            start = j;
        }
        pend.clear();
        anyRange = false;
    };
    // The guard-band codes of the vertices the lists use, once (each list's own range ORed
    // and ANDed below, as DrawPVerts would; all 0: nothing to look at per list).
    DWORD cLo = 0xFFFF, cHi = 0;
    for (const auto& [off, count] : parts)
        for (DWORD k = 0; k < count; k++) cLo = std::min<DWORD>(cLo, idx[off + k]), cHi = std::max<DWORD>(cHi, idx[off + k]);
    std::vector<WORD>& codes = t_codes;
    bool anyCode = false;
    if (clip && cLo <= cHi && cLo < n) {
        cHi = std::min<DWORD>(cHi, n - 1);
        codes.resize(cHi - cLo + 1);
        for (DWORD i = cLo; i <= cHi; i++) anyCode |= (codes[i - cLo] = GbCode(v[i])) != 0;
    }
    for (const auto& [off, count] : parts) {
        DWORD lo = 0xFFFF, hi = 0;
        for (DWORD k = 0; k < count; k++) lo = std::min<DWORD>(lo, idx[off + k]), hi = std::max<DWORD>(hi, idx[off + k]);
        if (!count || hi >= n || !ValidPrim(t)) continue;  // (not drawn: DrawPVerts' rules)
        WORD orGb = 0, andGb = 0;
        if (anyCode) {
            andGb = 0xFFFF;
            for (DWORD i = lo; i <= hi; i++) {
                const WORD c = codes[i - cLo];
                orGb |= c;
                andGb &= c;
            }
        }
        if (andGb) continue;
        if (orGb) {
            pend.push_back({off, count, lo, hi, 0, true, orGb});
            continue;
        }
        const UINT pr = PrimCount(t, count);
        if (!pr) continue;
        // Joined while the vertices sent stay near those the lists use themselves.
        if (anyRange && std::max(hi0, hi) - std::min(lo0, lo) + 1 > span + (hi - lo + 1) + 1024) submit();
        if (!anyRange) anyRange = true, lo0 = lo, hi0 = hi, span = 0;
        lo0 = std::min(lo0, lo), hi0 = std::max(hi0, hi), span += hi - lo + 1;
        pend.push_back({off, count, lo, hi, pr, false, 0});
    }
    submit();
}

static void CountPrims(Device* d, D3DPRIMITIVETYPE t, UINT prims, DWORD verts) {
    switch (t) {
    case D3DPT_POINTLIST: d->stats.dwPointsDrawn += prims; break;
    case D3DPT_LINELIST: case D3DPT_LINESTRIP: d->stats.dwLinesDrawn += prims; break;
    default: d->stats.dwTrianglesDrawn += prims; break;
    }
    d->stats.dwVerticesProcessed += verts;
}

// The draws of Device2 and Device3 (B §8).
HRESULT Device::DrawCommon(D3DPRIMITIVETYPE t, DWORD fvf, const uint8_t* v, UINT stride, DWORD n, const WORD* idx,
                           DWORD ni, DWORD flags, const WORD* vbClip) {
    if (!ValidPrim(t)) return D3D_OK;  // 0 primitives: nothing drawn
    const UINT prims = PrimCount(t, idx ? ni : n);
    if (!prims || !n) return D3D_OK;
    if (!v || (!idx && ni)) return DDERR_INVALIDPARAMS;
    if (n > 0x01000000 || ni > 0x01000000) return DDERR_INVALIDPARAMS;  // as Direct3D 7 (no huge allocations)
    // Transformed vertices draw without a current viewport too (Wine's test_vb_discard).
    if (cur || (fvf & 0x0E) != D3DFVF_XYZRHW) {
        const HRESULT hr = CheckViewport();
        if (FAILED(hr)) return hr;
    }
    CountPrims(this, t, prims, n);
    if (!rt || rt->CantDraw(false)) {
        if (inScene) lostInScene = true;
        return D3D_OK;
    }
    fvf &= 0xFFF;  // no texture coordinate sizes in DX6
    if (!(fvf & 0x0E)) return DDERR_INVALIDPARAMS;  // no position (XP reads past the vertices)
    if ((fvf & 0x0E) == D3DFVF_XYZRHW && vbClip) {
        // A processed vertex buffer drawn with clipping (B §8.4): outside vertices hold clip
        // coordinates, so those primitives go through the clipper.
        DWORD lo = 0, hi = n - 1;
        if (idx) {
            lo = 0xFFFF, hi = 0;
            for (DWORD i = 0; i < ni; i++) lo = std::min<DWORD>(lo, idx[i]), hi = std::max<DWORD>(hi, idx[i]);
        }
        bool any = false;
        for (DWORD i = lo; i <= hi && i < n; i++) any = any || vbClip[i];
        if (any) {
            const Layout in(fvf);
            t_pverts.resize(n);
            for (DWORD i = 0; i < n; i++) {
                const uint8_t* s = v + size_t(i) * stride;
                const float* f = reinterpret_cast<const float*>(s);
                PVert& p = t_pverts[i];
                p.code = vbClip[i];
                if (p.code) {
                    p.cx = f[0], p.cy = f[1], p.cz = f[2], p.cw = f[3];
                } else {
                    p.sx = f[0], p.sy = f[1], p.sz = f[2], p.rhw = f[3];
                    const float w = f[3] != 0.0f ? 1.0f / f[3] : 0.0f;
                    p.cw = w;
                    p.cx = map.W != 0 ? (f[0] - map.X) / map.W * w : 0.0f;
                    p.cy = map.H != 0 ? (map.Y + map.H - f[1]) / map.H * w : 0.0f;
                    p.cz = f[2] * w;
                }
                p.diffuse = in.diffuse >= 0 ? *reinterpret_cast<const DWORD*>(s + in.diffuse) : 0xFFFFFFFF;
                p.specular = in.specular >= 0 ? *reinterpret_cast<const DWORD*>(s + in.specular) : 0;
                for (DWORD k = 0; k < in.texCount; k++) memcpy(p.tex[k], s + in.tex[k], 8);
            }
            DrawPVerts(this, t, t_pverts.data(), n, idx, ni, true, std::min<DWORD>((fvf >> 8) & 0xF, 8));
            return D3D_OK;
        }
    }
    if ((fvf & 0x0E) == D3DFVF_XYZRHW) {
        // Transformed: drawn as they are; Direct3D 9's viewport clips them.
        if (!(flags & D3DDP_DONOTCLIP)) status &= 0xFF000FFF;
        Use u(this);
        if (!u.ok) return D3D_OK;
        BindTextures9();
        // Indices past the vertices given: vertex 0 instead (the GPU would read past them).
        for (DWORD i = 0; idx && i < ni; i++)
            if (idx[i] >= n) {
                t_safeIdx.assign(idx, idx + ni);
                for (WORD& k : t_safeIdx)
                    if (k >= n) k = 0;
                idx = t_safeIdx.data();
                break;
            }
        ctx.Draw(t, fvf & ~DWORD(0x001), v, stride, n, idx, ni, prims, rs[7] != D3DZB_FALSE);
        return D3D_OK;
    }
    const bool light = (fvf & D3DFVF_NORMAL) && !(flags & D3DDP_DONOTLIGHT) && FindMatHandle(ls.material);
    const bool clip = !(flags & D3DDP_DONOTCLIP);
    DWORD lo = 0, hi = n - 1;  // the vertices the indices use: the only ones lit
    if (idx) {
        lo = 0xFFFF, hi = 0;
        for (DWORD i = 0; i < ni; i++) lo = std::min<DWORD>(lo, idx[i]), hi = std::max<DWORD>(hi, idx[i]);
        if (hi >= n) lo = 1, hi = 0;  // past the vertices: none lit (DrawPVerts draws nothing)
    }
    if (t_pverts.size() < n) t_pverts.resize(n);
    TransformVerts(this, v, stride, fvf, n, clip, light, !(flags & D3DDP_DONOTUPDATEEXTENTS), t_pverts.data(), lo, hi);
    DrawPVerts(this, t, t_pverts.data(), n, idx, ni, clip, std::min<DWORD>((fvf >> 8) & 0xF, 8));
    return D3D_OK;
}

// ---------------------------------------------------------------- Execute (C §6.2)

namespace {
void ConvertExtent(const Device* d, D3DRECT* r) {
    const VpMap& m = d->map;
    const LONG vx1 = LONG(m.X), vy1 = LONG(m.Y), vx2 = LONG(m.X + m.W), vy2 = LONG(m.Y + m.H);
    r->x1 = std::max(LONG(floorf(d->ext[0] + 0.5f)) - 1, vx1);
    r->y1 = std::max(LONG(floorf(d->ext[1] + 0.5f)) - 1, vy1);
    r->x2 = std::min(LONG(floorf(d->ext[2] + 0.5f)) + 1, vx2);
    r->y2 = std::min(LONG(floorf(d->ext[3] + 0.5f)) + 1, vy2);
}
}  // namespace

HRESULT Device::ExecuteI(ExecBuf* eb, DWORD flags) {
    if (!rt || rt->CantDraw(false) || (z && z->CantDraw(false))) {
        if (inScene) lostInScene = true;
        return DDERR_SURFACELOST;
    }
    if (flags != D3DEXECUTE_CLIPPED && flags != D3DEXECUTE_UNCLIPPED) return DDERR_INVALIDPARAMS;
    const D3DEXECUTEDATA& ed = eb->data;
    uint8_t* base = eb->mem.data();
    const size_t size = eb->mem.size();
    // (At most 65536: nothing addresses more, wDest and the indices being WORDs.)
    const DWORD tlSize = std::min<DWORD>(std::max<DWORD>(ed.dwVertexCount, 4096), 65536);
    if (tl.size() < tlSize) tl.resize(tlSize);
    bool copied = false;   // a PROCESSVERTICES COPY ran: the rest is drawn unclipped
    bool lightFlag = false;  // sticky for the Execute (C §6.4)
    size_t pos = ed.dwInstructionOffset;
    // Consecutive points, lines or triangles are drawn together, as one Direct3D 9 draw: a
    // game's execute buffer may hold a TRIANGLE instruction per triangle or per strip, each a
    // draw of its own otherwise (driver work every time). Any other instruction (a state, a
    // matrix, PROCESSVERTICES, a branch, the end) draws what is waiting first, so every
    // primitive is drawn in the same order and with the same states as one by one. Each
    // instruction keeps the vertices and clipping it had drawn alone, and the draw is split
    // again where one draw would decide anything otherwise for it (GpuContext::Draw).
    std::vector<WORD>& batch = execBatch;
    std::vector<std::pair<DWORD, DWORD>>& parts = execParts;  // each instruction's indices in `batch`
    batch.clear();
    parts.clear();
    D3DPRIMITIVETYPE batchType = D3DPT_TRIANGLELIST;
    auto flush = [&] {
        if (parts.empty()) return;
        DrawPVertsBatch(this, batchType, tl.data(), DWORD(tl.size()), batch.data(), parts,
                        flags == D3DEXECUTE_CLIPPED && !copied);
        batch.clear();
        parts.clear();
    };
    struct AtExit {  // (every way out draws what is waiting, as drawn one by one it would be)
        decltype(flush)& f;
        ~AtExit() {
            try {
                f();
            } catch (...) {  // (out of memory: a destructor can't throw)
            }
        }
    } atExit{flush};
    // Draws an index list of TL-buffer vertices (with the ones waiting, when it can).
    auto drawTL = [&](D3DPRIMITIVETYPE type, const WORD* idx, DWORD ni) {
        if (picking) return;  // points, lines and spans aren't picked
        CountPrims(this, type, PrimCount(type, ni), 0);
        if (rs[30]) {  // ZVISIBLE: nothing drawn, "visible"
            status &= ~DWORD(D3DSTATUS_ZNOTVISIBLE);
            return;
        }
        if (!parts.empty() && type != batchType) flush();
        batchType = type;
        parts.emplace_back(DWORD(batch.size()), ni);
        batch.insert(batch.end(), idx, idx + ni);
    };
    std::vector<WORD>& idx = execIdx;  // (kept from Execute to Execute)
    while (pos <= size && size - pos >= sizeof(D3DINSTRUCTION)) {
        const auto* ins = reinterpret_cast<const D3DINSTRUCTION*>(base + pos);
        if (ins->bOpcode != D3DOP_POINT && ins->bOpcode != D3DOP_LINE && ins->bOpcode != D3DOP_TRIANGLE &&
            ins->bOpcode != D3DOP_SPAN)
            flush();
        const uint8_t* rec = base + pos + sizeof(D3DINSTRUCTION);
        const DWORD count = ins->wCount;
        auto fits = [&](size_t recSize) { return size_t(rec - base) + recSize * count <= size; };
        size_t recSize;
        switch (ins->bOpcode) {
        case D3DOP_POINT: {
            recSize = sizeof(D3DPOINT);
            if (!fits(recSize)) return D3D_OK;
            for (DWORD i = 0; i < count; i++) {
                const D3DPOINT& p = reinterpret_cast<const D3DPOINT*>(rec)[i];
                idx.resize(p.wCount);
                for (WORD k = 0; k < p.wCount; k++) idx[k] = WORD(p.wFirst + k);
                if (p.wCount) drawTL(D3DPT_POINTLIST, idx.data(), p.wCount);
            }
            break;
        }
        case D3DOP_LINE: {
            recSize = sizeof(D3DLINE);
            if (!fits(recSize)) return D3D_OK;
            idx.resize(2 * count);
            for (DWORD i = 0; i < count; i++) {
                const D3DLINE& l = reinterpret_cast<const D3DLINE*>(rec)[i];
                idx[2 * i] = l.v1;
                idx[2 * i + 1] = l.v2;
            }
            if (count) drawTL(D3DPT_LINELIST, idx.data(), 2 * count);
            break;
        }
        case D3DOP_TRIANGLE: {
            recSize = sizeof(D3DTRIANGLE);
            if (!fits(recSize)) return D3D_OK;
            if (picking) {
                // XP's GenPickTriangle: the triangle (its part inside the clip volume, for one
                // the volume cuts) covers the rectangle's top-left pixel; the record names the
                // triangle's offset from the start of the instructions and its depth there.
                const float px = float(picking->x1), py = float(picking->y1);
                // Whether (px, py) is in the screen triangle a-b-c, and its depth there.
                auto hit = [px, py](const PVert& a0, const PVert& a1, const PVert& a2, float* z) {
                    const float area = (a1.sx - a0.sx) * (a2.sy - a0.sy) - (a2.sx - a0.sx) * (a1.sy - a0.sy);
                    if (area == 0.0f) return false;
                    const float w0 = ((a1.sx - px) * (a2.sy - py) - (a2.sx - px) * (a1.sy - py)) / area;
                    const float w1 = ((a2.sx - px) * (a0.sy - py) - (a0.sx - px) * (a2.sy - py)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) return false;
                    *z = w0 * a0.sz + w1 * a1.sz + w2 * a2.sz;
                    return true;
                };
                for (DWORD i = 0; i < count; i++) {
                    const D3DTRIANGLE& t = reinterpret_cast<const D3DTRIANGLE*>(rec)[i];
                    if (t.v1 >= tl.size() || t.v2 >= tl.size() || t.v3 >= tl.size()) continue;
                    const PVert &a0 = tl[t.v1], &a1 = tl[t.v2], &a2 = tl[t.v3];
                    const WORD ca = a0.code & 0x3F, cb = a1.code & 0x3F, cc = a2.code & 0x3F;
                    if (ca & cb & cc) continue;  // wholly outside one plane
                    float z = 0;
                    bool found = false;
                    if (!(ca | cb | cc)) {
                        found = hit(a0, a1, a2, &z);
                    } else {
                        // Clipped in clip space, the pieces projected as drawing does.
                        PVert poly[kMaxPoly] = {a0, a1, a2};
                        const int pn = ClipPoly(poly, 3, ca | cb | cc, 0, true);
                        for (int j = 0; j < pn; j++) Project(map, &poly[j]);
                        for (int j = 1; j + 1 < pn && !found; j++) found = hit(poly[0], poly[j], poly[j + 1], &z);
                    }
                    if (!found) continue;
                    D3DPICKRECORD r = {};
                    r.bOpcode = D3DOP_TRIANGLE;
                    r.dwOffset = DWORD(reinterpret_cast<const uint8_t*>(&t) - (base + ed.dwInstructionOffset));
                    r.dvZ = z;
                    pickRecs.push_back(r);
                }
                break;
            }
            idx.resize(3 * count);
            for (DWORD i = 0; i < count; i++) {
                const D3DTRIANGLE& t = reinterpret_cast<const D3DTRIANGLE*>(rec)[i];
                idx[3 * i] = t.v1;
                idx[3 * i + 1] = t.v2;
                idx[3 * i + 2] = t.v3;
            }
            if (count) drawTL(D3DPT_TRIANGLELIST, idx.data(), 3 * count);
            break;
        }
        case D3DOP_MATRIXLOAD: {
            recSize = sizeof(D3DMATRIXLOAD);
            if (!fits(recSize)) return D3D_OK;
            for (DWORD i = 0; i < count; i++) {
                const D3DMATRIXLOAD& m = reinterpret_cast<const D3DMATRIXLOAD*>(rec)[i];
                if (!m.hDestMatrix || !m.hSrcMatrix) return DDERR_INVALIDPARAMS;
                auto s = matrices.find(m.hSrcMatrix), dst = matrices.find(m.hDestMatrix);
                if (s == matrices.end() || dst == matrices.end()) return E_FAIL;
                dst->second = s->second;
                MatrixChanged(m.hDestMatrix);
            }
            break;
        }
        case D3DOP_MATRIXMULTIPLY: {
            recSize = sizeof(D3DMATRIXMULTIPLY);
            if (!fits(recSize)) return D3D_OK;
            for (DWORD i = 0; i < count; i++) {
                const D3DMATRIXMULTIPLY& m = reinterpret_cast<const D3DMATRIXMULTIPLY*>(rec)[i];
                auto a = matrices.find(m.hSrcMatrix1), b = matrices.find(m.hSrcMatrix2), dst = matrices.find(m.hDestMatrix);
                if (!m.hSrcMatrix1 || !m.hSrcMatrix2 || a == matrices.end() || b == matrices.end() || dst == matrices.end())
                    return E_FAIL;
                dst->second = Mul(a->second, b->second);
                MatrixChanged(m.hDestMatrix);
            }
            break;
        }
        case D3DOP_STATETRANSFORM: {
            recSize = sizeof(D3DSTATE);
            if (!fits(recSize)) return D3D_OK;
            for (DWORD i = 0; i < count; i++) {
                const D3DSTATE& s = reinterpret_cast<const D3DSTATE*>(rec)[i];
                const DWORD st = s.dtstTransformStateType, h = s.dwArg[0];
                if (st > 256) {
                    if (st - 256 < overXS.size()) overXS[st - 256] = h != 0;
                    continue;
                }
                if (st < overXS.size() && overXS[st]) continue;
                if (!h) {
                    if (version >= 2) return DDERR_INVALIDPARAMS;
                    break;  // version 1: the rest of the instruction is skipped
                }
                if (st < 1 || st > 3) return E_FAIL;
                auto m = matrices.find(h);
                if (m == matrices.end()) return E_FAIL;
                *Xform(st) = m->second;
                bound[st - 1] = h;
            }
            break;
        }
        case D3DOP_STATELIGHT: {
            recSize = sizeof(D3DSTATE);
            if (picking) break;  // (XP's pick walk skips it)
            if (!fits(recSize)) return D3D_OK;
            for (DWORD i = 0; i < count; i++) {
                const D3DSTATE& s = reinterpret_cast<const D3DSTATE*>(rec)[i];
                const HRESULT hr = SetLS(s.dlstLightStateType, s.dwArg[0]);
                if (FAILED(hr)) return hr;
            }
            break;
        }
        case D3DOP_STATERENDER: {
            recSize = sizeof(D3DSTATE);
            if (!fits(recSize)) return D3D_OK;
            Use u(this);
            for (DWORD i = 0; i < count; i++) {
                const D3DSTATE& s = reinterpret_cast<const D3DSTATE*>(rec)[i];
                const DWORD st = s.drstRenderStateType;
                if (st > 256) {
                    if (st - 256 < overRS.size()) overRS[st - 256] = s.dwArg[0] != 0;
                    continue;
                }
                if (st < overRS.size() && overRS[st]) continue;
                SetRS(st, s.dwArg[0], true);
            }
            break;
        }
        case D3DOP_PROCESSVERTICES: {
            recSize = sizeof(D3DPROCESSVERTICES);
            if (!fits(recSize)) return D3D_OK;
            for (DWORD i = 0; i < count; i++) {
                const D3DPROCESSVERTICES& p = reinterpret_cast<const D3DPROCESSVERTICES*>(rec)[i];
                // Source vertices at the buffer's start + wStart (dwVertexOffset isn't read).
                const size_t srcOff = size_t(p.wStart) * 32;
                DWORD n = p.dwCount;
                if (srcOff >= size) continue;
                n = std::min<DWORD>(n, DWORD((size - srcOff) / 32));
                if (p.wDest >= tl.size()) continue;
                n = std::min<DWORD>(n, DWORD(tl.size() - p.wDest));
                const uint8_t* src = base + srcOff;
                const DWORD op = p.dwFlags & D3DPROCESSVERTICES_OPMASK;
                if (op == D3DPROCESSVERTICES_COPY) {
                    for (DWORD k = 0; k < n; k++) {
                        const D3DTLVERTEX& s = reinterpret_cast<const D3DTLVERTEX*>(src)[k];
                        PVert& o = tl[p.wDest + k];
                        o.sx = s.sx;
                        o.sy = s.sy;
                        o.sz = s.sz;
                        o.rhw = s.rhw;
                        o.cx = s.sx, o.cy = s.sy, o.cz = s.sz, o.cw = 1.0f;
                        o.diffuse = s.color;
                        o.specular = s.specular;
                        o.tex[0][0] = s.tu;
                        o.tex[0][1] = s.tv;
                        o.code = 0;
                        ext[0] = std::min(ext[0], s.sx);
                        ext[1] = std::min(ext[1], s.sy);
                        ext[2] = std::max(ext[2], s.sx);
                        ext[3] = std::max(ext[3], s.sy);
                    }
                    copied = true;
                    status &= 0xFF000FFF;
                    continue;
                }
                if (op == D3DPROCESSVERTICES_TRANSFORMLIGHT) lightFlag = true;
                const DWORD fvf = op == D3DPROCESSVERTICES_TRANSFORMLIGHT ? D3DFVF_VERTEX : D3DFVF_LVERTEX;
                const bool lit = lightFlag && fvf == D3DFVF_VERTEX && FindMatHandle(ls.material);
                TransformVerts(this, src, 32, fvf, n, flags == D3DEXECUTE_CLIPPED, lit, true, &tl[p.wDest]);
                if (flags != D3DEXECUTE_CLIPPED) status &= 0xFF000FFF;  // unclipped: no intersection (C §6.2)
                stats.dwVerticesProcessed += n;
            }
            break;
        }
        case D3DOP_TEXTURELOAD: {
            recSize = sizeof(D3DTEXTURELOAD);
            if (picking) break;  // (XP's pick walk skips it)
            if (!fits(recSize)) return D3D_OK;
            for (DWORD i = 0; i < count; i++) {
                const D3DTEXTURELOAD& t = reinterpret_cast<const D3DTEXTURELOAD*>(rec)[i];
                auto known = [&](DWORD h) { return std::find(texHandles.begin(), texHandles.end(), h) != texHandles.end(); };
                Surface* dst = known(t.hDestTexture) ? SurfaceForHandle(t.hDestTexture) : nullptr;
                Surface* src = known(t.hSrcTexture) ? SurfaceForHandle(t.hSrcTexture) : nullptr;
                if (!dst || !src) return D3DERR_TEXTURE_CREATE_FAILED;
                if (FAILED(CopyTexture(dst, src))) return D3DERR_TEXTURE_LOAD_FAILED;
            }
            break;
        }
        case D3DOP_EXIT:
            if (picking) return D3D_OK;  // (the buffer's status is left alone)
            eb->data.dsStatus.dwFlags = 0;
            eb->data.dsStatus.dwStatus = status;
            ConvertExtent(this, &eb->data.dsStatus.drExtent);
            return D3D_OK;
        case D3DOP_BRANCHFORWARD: {
            if (!fits(sizeof(D3DBRANCH)) || !count) return D3D_OK;
            const D3DBRANCH& b = *reinterpret_cast<const D3DBRANCH*>(rec);
            const bool c = (status & b.dwMask) == b.dwValue;
            if (c != bool(b.bNegate)) {
                if (!b.dwOffset) {  // as EXIT
                    if (picking) return D3D_OK;
                    eb->data.dsStatus.dwFlags = 0;
                    eb->data.dsStatus.dwStatus = status;
                    ConvertExtent(this, &eb->data.dsStatus.drExtent);
                    return D3D_OK;
                }
                if (b.dwOffset >= size - pos) return D3D_OK;  // past the end (or wrapping round): stop
                pos += b.dwOffset;
                continue;
            }
            recSize = sizeof(D3DBRANCH);
            break;
        }
        case D3DOP_SPAN: {
            // Drawn only on XP's driver path (unclipped execution, or after a PROCESSVERTICES
            // COPY), skipped when clipped (C §6). Each record (bSize bytes, a D3DSPAN first):
            // wCount TL vertices from wFirst, one pixel each, drawn here as points.
            recSize = ins->bSize;
            if (recSize < sizeof(D3DSPAN) || !fits(recSize) || (flags == D3DEXECUTE_CLIPPED && !copied)) break;
            for (DWORD i = 0; i < count; i++) {
                const D3DSPAN& sp = *reinterpret_cast<const D3DSPAN*>(rec + i * recSize);
                idx.resize(sp.wCount);
                for (WORD k = 0; k < sp.wCount; k++) idx[k] = WORD(sp.wFirst + k);
                if (sp.wCount) drawTL(D3DPT_POINTLIST, idx.data(), sp.wCount);
            }
            break;
        }
        case D3DOP_SETSTATUS: {
            recSize = sizeof(D3DSTATUS);
            if (!fits(recSize)) return D3D_OK;
            if (count) {
                const D3DSTATUS& s = *reinterpret_cast<const D3DSTATUS*>(rec);
                if (s.dwFlags & D3DSETSTATUS_STATUS) status = s.dwStatus;
                if (s.dwFlags & D3DSETSTATUS_EXTENTS) {
                    ext[0] = float(s.drExtent.x1);
                    ext[1] = float(s.drExtent.y1);
                    ext[2] = float(s.drExtent.x2);
                    ext[3] = float(s.drExtent.y2);
                }
            }
            break;
        }
        default:  // 0 or unknown: a silent stop, the status not written
            if (!picking) memset(&eb->data.dsStatus, 0, sizeof(eb->data.dsStatus));
            return D3D_OK;
        }
        pos = size_t(rec - base) + recSize * count;
    }
    return D3D_OK;
}

// ---------------------------------------------------------------- Viewport::Clear's drawing

void ClearRects(Device* d, const std::vector<RECT>& rects, DWORD flags, DWORD color, float zv, DWORD stencil) {
    if (!d->rt) return;
    Device::Use u(d);
    if (!u.ok) return;
    if (!d->z) flags &= ~DWORD(D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
    if (!flags) return;
    d->ctx.SetViewport(0, 0, DWORD(d->rt->W()), DWORD(d->rt->H()), 0.0f, 1.0f);
    d->ctx.Clear(DWORD(rects.size()), rects.data(), flags, color, zv, stencil);
    d->SetViewport9();
}

// TriFillRectsTex (C §1.11): the background texture stretched over the viewport.
void FillRectsTex(Device* d, const std::vector<RECT>& rects, Surface* tex, const Viewport* vp) {
    if (!d->rt) return;
    Device::Use u(d);
    const DWORD saved[][2] = {{7, 0}, {14, 0}, {23, D3DCMP_ALWAYS}, {5, 0}, {6, 0}, {8, D3DFILL_SOLID}, {35, 0},
                              {28, 0}, {27, 0}, {41, 0}, {52, 0}, {4, 0}, {26, 0}, {128, 0}, {129, 0}, {130, 0},
                              {131, 0}, {132, 0}, {133, 0}, {134, 0}, {135, 0}, {1, TexHandleFor(tex)}};
    DWORD old[std::size(saved)];
    const DWORD oldHandle = d->dTexHandle;  // the driver's, which SetTexture may have ended
    Surface* const oldTex0 = d->dtex[0];
    const DWORD oldWrap0 = d->dWrap0;  // (WRAPU/V put back after WRAP0 would take its bits off)
    for (size_t i = 0; i < std::size(saved); i++) {
        old[i] = d->rs[saved[i][0]];
        d->SetRS(saved[i][0], saved[i][1], true);
    }
    const float vx = float(vp->data.dwX), vy = float(vp->data.dwY);
    const float vw = float(std::max<DWORD>(vp->data.dwWidth, 1)), vh = float(std::max<DWORD>(vp->data.dwHeight, 1));
    for (const RECT& r : rects) {
        PVert q[4] = {};
        const float xs[4] = {float(r.left), float(r.right), float(r.right), float(r.left)};
        const float ys[4] = {float(r.top), float(r.top), float(r.bottom), float(r.bottom)};
        for (int k = 0; k < 4; k++) {
            q[k].sx = xs[k];
            q[k].sy = ys[k];
            q[k].sz = 0.0f;
            q[k].rhw = 1.0f;
            q[k].diffuse = 0xFFFFFFFF;
            q[k].specular = 0;
            q[k].tex[0][0] = (xs[k] - (vx - 0.41f)) / vw;
            q[k].tex[0][1] = (ys[k] - (vy - 0.41f)) / vh;
        }
        DrawPVerts(d, D3DPT_TRIANGLEFAN, q, 4, nullptr, 0, false, 1);
    }
    for (size_t i = std::size(saved); i-- > 0;) d->SetRS(saved[i][0], old[i], true);
    d->dTexHandle = oldHandle;
    d->dtex[0] = oldTex0;
    d->dWrap0 = oldWrap0;
    d->ctx.SetRenderState(128, oldWrap0);
    d->PushStageOps9();
    d->PushAlpha9();
}

// ---------------------------------------------------------------- DX6 ProcessVertices (C §7.3)

HRESULT VBuf::ProcessVertices(DWORD op, DWORD dst, DWORD n, IDirect3DVertexBuffer* srcI, DWORD srcIndex,
                              IDirect3DDevice3* devI, DWORD) {
    if (!devI || !srcI) return DDERR_INVALIDPARAMS;  // XP faults
    Guard l;
    if (!(fvf & 0x0E) || !(static_cast<VBuf*>(srcI)->fvf & 0x0E)) return DDERR_INVALIDPARAMS;  // no position: XP overruns
    Device* d = DevFrom(devI);
    HRESULT hr = d->CheckViewport();
    if (FAILED(hr)) return hr;
    VBuf* src = static_cast<VBuf*>(srcI);
    if (srcIndex >= src->count || dst >= count) return D3D_OK;
    n = std::min({n, src->count - srcIndex, count - dst});
    const bool clipOn = (op & D3DVOP_CLIP) != 0;
    const bool light = (op & D3DVOP_LIGHT) && (src->fvf & D3DFVF_NORMAL) && FindMatHandle(d->ls.material);
    t_pverts.resize(n);
    TransformVerts(d, src->mem.data() + size_t(srcIndex) * src->stride, src->stride, src->fvf, n, clipOn, light,
                   (op & D3DVOP_EXTENTS) != 0, t_pverts.data());
    const Layout out(fvf), in(src->fvf);
    const bool rhw = (fvf & 0x0E) == D3DFVF_XYZRHW;
    const DWORD ntex = std::min(in.texCount, out.texCount);
    for (DWORD i = 0; i < n; i++) {
        const PVert& p = t_pverts[i];
        uint8_t* o = mem.data() + size_t(dst + i) * stride;
        float* f = reinterpret_cast<float*>(o);
        f[0] = p.sx;  // screen inside, clip coordinates outside (as XP)
        f[1] = p.sy;
        f[2] = p.sz;
        if (rhw) f[3] = p.rhw;
        if (out.diffuse >= 0) *reinterpret_cast<DWORD*>(o + out.diffuse) = p.diffuse;
        if (out.specular >= 0) *reinterpret_cast<DWORD*>(o + out.specular) = p.specular;
        for (DWORD t = 0; t < ntex; t++) memcpy(o + out.tex[t], p.tex[t], 8);
        if (clipOn && !clip.empty()) clip[dst + i] = p.code;
    }
    return D3D_OK;
}

}  // namespace d6
}  // namespace dd2d9
