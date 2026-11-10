// dd2d9's Direct3D 3-6: the CPU front end's transform and lighting (XP had no hardware T&L
// for DX3-6: d3dim's front end transformed, lit and clipped every vertex), into PVerts for
// the clipper and the draws (d3d6draw.cpp).
#include "d3d6int.h"

#include <emmintrin.h>
#include <xmmintrin.h>

namespace dd2d9 {
namespace d6 {

namespace {

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
            code = WORD(ClipCode01(o.cx, o.cy, o.cz, o.cw));
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
    if (clip && n) d->status = MergeClipStatus(d->status, orCodes, andCodes);
}

}  // namespace d6
}  // namespace dd2d9
