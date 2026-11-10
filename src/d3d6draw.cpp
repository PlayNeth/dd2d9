// dd2d9's Direct3D 3-6: clipping and drawing PVerts, the draws of Device2/3 (DrawCommon),
// Viewport::Clear's drawing and DX6 ProcessVertices. Direct3D 9 gets transformed vertices
// only: D3DVIEWPORT2's clip volume and the guard band have no Direct3D 9 equivalent.
#include "d3d6int.h"

#include <new>

namespace dd2d9 {
namespace d6 {

namespace {

// Guard band for clipping (in viewports, each side): primitives are clipped only where they
// leave it or cross the near or far plane; Direct3D 9's viewport cuts the rest, as XP's
// guard-band clipping left it to the driver.
constexpr float kGuard = 2.0f;

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
std::vector<std::pair<DWORD, DWORD>> t_ranges;
std::vector<uint8_t> t_derefV;  // (Dereference's)
std::vector<WORD> t_derefI;
std::vector<DWORD> t_derefOrder;
std::vector<DWORD> t_derefMap;  // vertex -> its place in t_derefV; ~0 between draws

}  // namespace

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

}  // namespace

int ClipPoly(PVert (&poly)[kMaxPoly], int n, WORD mask, DWORD ntex, bool volume) {
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

namespace {

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

// The lowest and highest of ni indices (none: 0xFFFF and 0).
inline void IndexRange(const WORD* idx, DWORD ni, DWORD* lo, DWORD* hi) {
    WORD l, h;
    IndexRange16(idx, ni, &l, &h);
    *lo = l, *hi = h;
}

// Vertices lo..hi drawn as they are: in screen coordinates, those with a clip code (inside
// the guard band, drawn unclipped) projected first. (e.Expect(hi - lo + 1) came before.)
inline void AddAsTheyAre(Emitter& e, const VpMap& m, const PVert* v, DWORD lo, DWORD hi, bool clip) {
    if (!clip) {
        // None has a clip code: screen position and colours (24 bytes in a row in a PVert
        // as in the output) and the texture coordinates, with sizes the compiler knows.
        static_assert(offsetof(PVert, sx) + 16 == offsetof(PVert, diffuse) && offsetof(PVert, diffuse) + 8 == offsetof(PVert, tex),
                      "PVert: sx..specular, then tex");
        uint8_t* o = e.buf.data() + size_t(e.count) * e.stride;
        const UINT stride = e.stride;
        const DWORD ntex = std::min<DWORD>(e.ntex, 8);
        for (DWORD i = lo; i <= hi; i++, o += stride) {
            memcpy(o, &v[i].sx, 24);
            switch (ntex) {
            case 0: break;
            case 1: memcpy(o + 24, v[i].tex, 8); break;
            case 2: memcpy(o + 24, v[i].tex, 16); break;
            default: memcpy(o + 24, v[i].tex, 8 * ntex); break;
            }
        }
        e.count += hi - lo + 1;
        return;
    }
    for (DWORD i = lo; i <= hi; i++) {
        if (clip && v[i].code) {
            PVert p = v[i];
            Project(m, &p);
            e.Add(p);
        } else {
            e.Add(v[i]);
        }
    }
}

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
// inside: every vertex has clip code 0 (TransformVerts found so). The guard band holds the
// clip volume, so none has a guard-band code either, and none needs projecting.
bool EmitPVerts(Device* d, D3DPRIMITIVETYPE t, const PVert* v, DWORD n, const WORD* idx, DWORD ni, bool clip,
                DWORD ntex, Emitter& out, std::vector<WORD>& outIdx, bool seqIdx, PDraw* r, const WORD* gb = nullptr,
                bool inside = false) {
    if (!ValidPrim(t) || !n) return false;
    // Worked on as a local (its count kept in a register, not reloaded after every byte
    // written), put back at the end.
    Emitter e = out;
    // The vertices referenced.
    DWORD lo = 0, hi = n - 1;
    if (idx) {
        if (!ni) return false;
        IndexRange(idx, ni, &lo, &hi);
        if (hi >= n) return false;  // past the vertices (XP reads whatever is there)
    }
    WORD orGb = gb ? *gb : 0, andGb = 0xFFFF;
    if (clip && !gb && !inside) {
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
        AddAsTheyAre(e, d->map, v, lo, hi, clip && !inside);
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
                DWORD ntex, bool inside) {
    ntex = std::min<DWORD>(ntex, 8);
    Emitter e(ntex, t_out, 0);
    std::vector<WORD>& outIdx = t_idx;
    outIdx.clear();
    PDraw r;
    if (!EmitPVerts(d, t, v, n, idx, ni, clip, ntex, e, outIdx, false, &r, nullptr, inside)) return;
    SubmitPVerts(d, r.type, e, r.indexed ? outIdx.data() : nullptr, r.indexed ? DWORD(outIdx.size()) : 0, r.prims,
                 nullptr, 0);
}

void DrawPVertsBatch(Device* d, D3DPRIMITIVETYPE t, const PVert* v, DWORD n, const WORD* idx,
                     const std::vector<std::pair<DWORD, DWORD>>& parts, bool clip) {
    // Lists only: a clipped strip or fan comes out of EmitPVerts as another type than the
    // batch's, which a shared draw can't take. Any other type is drawn part by part.
    if (parts.size() == 1 || (t != D3DPT_POINTLIST && t != D3DPT_LINELIST && t != D3DPT_TRIANGLELIST)) {
        for (const auto& [off, count] : parts) DrawPVerts(d, t, v, n, idx + off, count, clip, 1);
        return;
    }
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
                AddAsTheyAre(e, d->map, v, l, h, clip);
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
    std::vector<std::pair<DWORD, DWORD>>& ranges = t_ranges;  // each list's
    ranges.resize(parts.size());
    DWORD cLo = 0xFFFF, cHi = 0;
    for (size_t j = 0; j < parts.size(); j++) {
        IndexRange(idx + parts[j].first, parts[j].second, &ranges[j].first, &ranges[j].second);
        cLo = std::min(cLo, ranges[j].first), cHi = std::max(cHi, ranges[j].second);
    }
    std::vector<WORD>& codes = t_codes;
    bool anyCode = false;
    if (clip && cLo <= cHi && cLo < n) {
        cHi = std::min<DWORD>(cHi, n - 1);
        codes.resize(cHi - cLo + 1);
        for (DWORD i = cLo; i <= cHi; i++) anyCode |= (codes[i - cLo] = GbCode(v[i])) != 0;
    }
    for (size_t j = 0; j < parts.size(); j++) {
        const auto [off, count] = parts[j];
        const auto [lo, hi] = ranges[j];
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

// XP's DereferenceIndexedPrim (B §8.1): the vertices the indices use, copied together in
// the order first used (into t_derefV, `stride` bytes each), and the indices renumbered to
// them (t_derefI). Their count; 0 (nothing made) when an index is past the vertices.
static DWORD Dereference(const uint8_t* v, UINT stride, DWORD n, const WORD* idx, DWORD ni) {
    std::vector<DWORD>& map = t_derefMap;
    std::vector<DWORD>& order = t_derefOrder;
    // Everything allocated before `map` is marked (out of memory then leaves it clean): at
    // most as many vertices as indices, and as 16-bit indices name.
    const DWORD most = std::min<DWORD>(ni, 0x10000);
    if (map.empty()) map.assign(0x10000, ~0u);
    t_derefI.resize(ni);
    order.clear();
    order.reserve(most);
    t_derefV.resize(size_t(most) * stride);
    bool ok = true;
    for (DWORD i = 0; i < ni; i++) {
        const WORD k = idx[i];
        if (k >= n) {
            ok = false;
            break;
        }
        if (map[k] == ~0u) {
            map[k] = DWORD(order.size());
            order.push_back(k);
        }
        t_derefI[i] = WORD(map[k]);
    }
    for (size_t j = 0; j < order.size(); j++) {
        if (ok) memcpy(t_derefV.data() + j * stride, v + size_t(order[j]) * stride, stride);
        map[order[j]] = ~0u;
    }
    return ok ? DWORD(order.size()) : 0;
}

void CountPrims(Device* d, D3DPRIMITIVETYPE t, UINT prims, DWORD verts) {
    switch (t) {
    case D3DPT_POINTLIST: d->stats.dwPointsDrawn += prims; break;
    case D3DPT_LINELIST: case D3DPT_LINESTRIP: d->stats.dwLinesDrawn += prims; break;
    default: d->stats.dwTrianglesDrawn += prims; break;
    }
    d->stats.dwVerticesProcessed += verts;
}

// The draws of Device2 and Device3 (B §8). Scratch that can't grow gives E_OUTOFMEMORY, as
// XP's TL buffer (V §1), not an exception through the game's call.
HRESULT Device::DrawCommon(D3DPRIMITIVETYPE t, DWORD fvf, const uint8_t* v, UINT stride, DWORD n, const WORD* idx,
                           DWORD ni, DWORD flags, const WORD* vbClip, bool deref) try {
    if (!ValidPrim(t)) return D3D_OK;  // 0 primitives: nothing drawn
    const UINT prims = PrimCount(t, idx ? ni : n);
    if (!prims || !n) return D3D_OK;
    if (!v || (!idx && ni)) return DDERR_INVALIDPARAMS;
    if (n > 0x01000000 || ni > 0x01000000) return DDERR_INVALIDPARAMS;  // as Direct3D 7 (no huge allocations)
    // Fewer indices than half the vertices: only the vertices used go through the pipeline,
    // so that the clip status and extents are made of those alone (and the work is theirs).
    if (deref && idx && ni < n / 2 && (fvf & 0x0E) != D3DFVF_XYZRHW) {
        if (const DWORD m = Dereference(v, stride, n, idx, ni)) {
            v = t_derefV.data();
            n = m;
            idx = t_derefI.data();
        }
    }
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
        if (idx) IndexRange(idx, ni, &lo, &hi);
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
        IndexRange(idx, ni, &lo, &hi);
        if (hi >= n) lo = 1, hi = 0;  // past the vertices: none lit (DrawPVerts draws nothing)
    }
    if (t_pverts.size() < n) t_pverts.resize(n);
    const DWORD codes =
        TransformVerts(this, v, stride, fvf, n, clip, light, !(flags & D3DDP_DONOTUPDATEEXTENTS), t_pverts.data(), lo, hi);
    DrawPVerts(this, t, t_pverts.data(), n, idx, ni, clip, std::min<DWORD>((fvf >> 8) & 0xF, 8), clip && !codes);
    return D3D_OK;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
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
}

// ---------------------------------------------------------------- DX6 ProcessVertices (C §7.3)

HRESULT VBuf::ProcessVertices(DWORD op, DWORD dst, DWORD n, IDirect3DVertexBuffer* srcI, DWORD srcIndex,
                              IDirect3DDevice3* devI, DWORD) try {
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
        // Without D3DVOP_CLIP every vertex is in screen space: its code is 0, so that a later
        // clipped draw doesn't take it for clip coordinates by a code an earlier
        // ProcessVertices left (as DX7's, V §8.5 step 8).
        if (!clip.empty()) clip[dst + i] = p.code;
    }
    return D3D_OK;
} catch (const std::bad_alloc&) {  // (the scratch vertices: as DrawCommon)
    return E_OUTOFMEMORY;
}

}  // namespace d6
}  // namespace dd2d9
