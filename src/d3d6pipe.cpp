// dd2d9's Direct3D 3-6: the CPU vertex pipeline (XP had no hardware T&L for DX3-6: d3dim's
// front end transformed, lit and clipped every vertex), Execute's instruction stream, the
// draws, Viewport::Clear's drawing and DX6 ProcessVertices. Direct3D 9 gets transformed
// vertices only: D3DVIEWPORT2's clip volume, the two DX5/6 lighting models and execute
// buffers' semantics have no Direct3D 9 equivalent.
#include "d3d6int.h"

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
    auto q = [](float v) { return DWORD(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
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
std::vector<LightC> t_lights;

}  // namespace

// ---------------------------------------------------------------- transform and light

void TransformVerts(Device* d, const uint8_t* src, UINT stride, DWORD fvf, DWORD n, bool clip, bool light,
                    bool extents, PVert* out, DWORD lo, DWORD hi) {
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
    const float fs = F(d->ls.fogStart), fe = F(d->ls.fogEnd), fd = F(d->ls.fogDensity);
    const Vec3 origin = {wv._41, wv._42, wv._43};
    const VpMap map = d->map;
    float ext[4] = {d->ext[0], d->ext[1], d->ext[2], d->ext[3]};
    DWORD orCodes = 0, andCodes = 0xFFFF;
    for (DWORD i = 0; i < n; i++) {
        const bool used = i >= lo && i <= hi;
        if (!used && !clip && !extents) {  // nothing kept from it
            if (i < lo) i = lo - 1;  // on to lo
            else break;               // (past hi: none left)
            continue;
        }
        const uint8_t* v = src + size_t(i) * stride;
        PVert& o = out[i];
        const float* p = reinterpret_cast<const float*>(v);
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
            if (extents) {
                ext[0] = std::min(ext[0], o.sx);
                ext[1] = std::min(ext[1], o.sy);
                ext[2] = std::max(ext[2], o.sx);
                ext[3] = std::max(ext[3], o.sy);
            }
        } else {  // XP's TL buffer: an outside vertex keeps its clip coordinates there
            o.sx = o.cx;
            o.sy = o.cy;
            o.sz = o.cz;
            o.rhw = o.cw;
        }
        if (!used) continue;  // drawn by no index: its position (and code) only
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
                                f *= l.falloff == 1.0f ? s : powf(s, l.falloff);
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
                        const float k = powf(nh, mat.dvPower) * f;
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
            const float dist = d->rs[48] == TRUE ? sqrtf(Dot(P, P)) : P.z;
            float f = 1.0f;
            if (d->ls.fogMode == D3DFOG_LINEAR) f = fe != fs ? (fe - dist) / (fe - fs) : (dist < fs ? 1.0f : 0.0f);
            else if (d->ls.fogMode == D3DFOG_EXP) f = expf(-dist * fd);
            else if (d->ls.fogMode == D3DFOG_EXP2) f = expf(-(dist * fd) * (dist * fd));
            f = std::clamp(f, 0.0f, 1.0f);
            specular = (specular & 0x00FFFFFF) | (DWORD(f * 255.0f + 0.5f) << 24);
        }
        o.diffuse = diffuse;
        o.specular = specular;
        for (DWORD t = 0; t < in.texCount; t++) memcpy(o.tex[t], v + in.tex[t], 8);
    }
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
            o |= DWORD(std::clamp(u + (v - u) * t + 0.5f, 0.0f, 255.0f)) << s;
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
        if (buf.size() < expect * stride) buf.resize(expect * stride);
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
        memcpy(buf.data() + o + 24, p.tex, 8 * ntex);
        count++;
    }
};

}  // namespace

void DrawPVerts(Device* d, D3DPRIMITIVETYPE t, const PVert* v, DWORD n, const WORD* idx, DWORD ni, bool clip,
                DWORD ntex) {
    if (!ValidPrim(t) || !n) return;
    ntex = std::min<DWORD>(ntex, 8);
    // The vertices referenced.
    DWORD lo = 0, hi = n - 1;
    if (idx) {
        if (!ni) return;
        lo = 0xFFFF;
        hi = 0;
        for (DWORD i = 0; i < ni; i++) {
            lo = std::min<DWORD>(lo, idx[i]);
            hi = std::max<DWORD>(hi, idx[i]);
        }
        if (hi >= n) return;  // past the vertices (XP reads whatever is there)
    }
    WORD orGb = 0, andGb = 0xFFFF;
    if (clip) {
        for (DWORD i = lo; i <= hi; i++) {
            const WORD c = GbCode(v[i]);
            orGb |= c;
            andGb &= c;
        }
        if (andGb) return;  // all outside one plane
    }
    Emitter e(ntex, t_out, hi - lo + 1);
    std::vector<WORD>& outIdx = t_idx;
    outIdx.clear();
    D3DPRIMITIVETYPE outType = t;
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
            outIdx.resize(ni);
            for (DWORD i = 0; i < ni; i++) outIdx[i] = WORD(idx[i] - lo);
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
            prims = e.count;
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
            prims = e.count / 2;
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
            prims = e.count / 3;
        }
    }
    if (!prims || !e.count) return;
    Device::Use u(d);
    if (!u.ok) return;
    d->BindTextures9();
    const DWORD fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | (ntex << D3DFVF_TEXCOUNT_SHIFT);
    d->ctx.Draw(outType, fvf, e.buf.data(), e.stride, e.count, outIdx.empty() ? nullptr : outIdx.data(),
                DWORD(outIdx.size()), prims, d->rs[7] != D3DZB_FALSE);
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
    // Draws an index list of TL-buffer vertices.
    auto drawTL = [&](D3DPRIMITIVETYPE type, const WORD* idx, DWORD ni) {
        if (picking) return;  // points, lines and spans aren't picked
        CountPrims(this, type, PrimCount(type, ni), 0);
        if (rs[30]) {  // ZVISIBLE: nothing drawn, "visible"
            status &= ~DWORD(D3DSTATUS_ZNOTVISIBLE);
            return;
        }
        DrawPVerts(this, type, tl.data(), DWORD(tl.size()), idx, ni, flags == D3DEXECUTE_CLIPPED && !copied, 1);
    };
    std::vector<WORD>& idx = execIdx;  // (kept from Execute to Execute)
    while (pos <= size && size - pos >= sizeof(D3DINSTRUCTION)) {
        const auto* ins = reinterpret_cast<const D3DINSTRUCTION*>(base + pos);
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
