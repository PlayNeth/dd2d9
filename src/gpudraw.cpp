// The Direct3D 9 side of a Direct3D 7 device: its draws (see gpures.h, gpuint.h).
#include "gpuint.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <emmintrin.h>

namespace dd2d9 {

// One pass over transformed vertices (x, y, z, rhw first; stride 16 or more), each vertex
// one vector, in vertex order: minps and maxps with the vertex first give std::min's and
// std::max's answers (NaN, and the zeros' signs, included), so the loops this replaces decide
// the same.
struct TlScan {
    float x0, y0, x1, y1;  // x and y from lowest to highest
    bool flat;             // every vertex at vertex 0's z and rhw
    bool zOut;             // a z below 0 or above 1
    bool badRhw;           // an rhw that isn't a positive normal number (0, negative, a denormal, NaN, infinite)
    float minRhw, maxRhw;  // the others' (0: none)
};
static TlScan ScanTl(const uint8_t* src, UINT stride, UINT n) {
    if (!n) return TlScan{};  // (a draw of no vertices among several: vertex 0 may lie past the buffer)
    const __m128 v0 = _mm_loadu_ps(reinterpret_cast<const float*>(src));
    const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f), inf = _mm_set1_ps(INFINITY);
    const __m128 normMin = _mm_set1_ps(FLT_MIN), normMax = _mm_set1_ps(FLT_MAX);
    __m128 lo = v0, hi = v0, eq = _mm_cmpeq_ps(zero, zero), out = zero, bad = zero, gMin = inf, gMax = zero;
    for (UINT i = 0; i < n; i++) {
        const __m128 v = _mm_loadu_ps(reinterpret_cast<const float*>(src + size_t(i) * stride));
        if (i) {
            lo = _mm_min_ps(v, lo);  // v < lo ? v : lo, as std::min(lo, v)
            hi = _mm_max_ps(v, hi);  // v > hi ? v : hi, as std::max(hi, v)
            eq = _mm_and_ps(eq, _mm_cmpeq_ps(v, v0));
        }
        out = _mm_or_ps(out, _mm_or_ps(_mm_cmplt_ps(v, zero), _mm_cmpgt_ps(v, one)));
        const __m128 good = _mm_and_ps(_mm_cmpge_ps(v, normMin), _mm_cmple_ps(v, normMax));
        bad = _mm_or_ps(bad, _mm_andnot_ps(good, _mm_cmpeq_ps(zero, zero)));
        gMin = _mm_min_ps(_mm_or_ps(_mm_and_ps(good, v), _mm_andnot_ps(good, inf)), gMin);
        gMax = _mm_max_ps(_mm_and_ps(good, v), gMax);
    }
    alignas(16) float l[4], h[4], mn[4], mx[4];
    _mm_store_ps(l, lo), _mm_store_ps(h, hi), _mm_store_ps(mn, gMin), _mm_store_ps(mx, gMax);
    TlScan r;
    r.x0 = l[0], r.y0 = l[1], r.x1 = h[0], r.y1 = h[1];
    r.flat = (_mm_movemask_ps(eq) & 0xC) == 0xC;
    r.zOut = (_mm_movemask_ps(out) & 4) != 0;
    r.badRhw = (_mm_movemask_ps(bad) & 8) != 0;
    r.minRhw = mn[3] == INFINITY ? 0.0f : mn[3];
    r.maxRhw = mx[3];
    return r;
}

// Emulated W buffering: an rhw above this (a w below 1/65536) is nearer than a W buffer's
// range reaches. Such a vertex is the nearest there can be (depth 0) and doesn't set the
// scale the others' depths are taken with (g_wScale). KOF 99 Evolution draws its low-life
// warning lines first in the frame with an rhw of 13131952 (an integer made a float, by the
// look of it): the scale it set left every depth after it in that frame the same, and the
// picture flickered with the warning's blinking.
constexpr float kWNearest = 65536.0f;

// The largest rhw of `n` transformed vertices that emulated W buffering's scale has to hold
// (0: none), given the largest valid one of all (TlScan::maxRhw).
static float WScaleRhw(const uint8_t* src, UINT stride, UINT n, float maxRhw) {
    if (maxRhw <= kWNearest) return maxRhw;  // (nearly always)
    float held = 0.0f;
    for (UINT i = 0; i < n; i++) {
        float rhw;
        memcpy(&rhw, src + size_t(i) * stride + 12, sizeof(rhw));
        if (rhw >= FLT_MIN && rhw <= kWNearest) held = std::max(held, rhw);
    }
    return held;
}

void GpuContext::Draw(DWORD primType, DWORD fvf, const void* verts, UINT stride, UINT vertexCount,
                      const WORD* indices, UINT indexCount, UINT primCount, bool zEnabled, const DrawSeg* segs,
                      UINT segCount) {
    Impl& p = *p_;
    if (!p.dev || !primCount || !verts || !vertexCount) return;
    if (segCount < 2) segs = nullptr, segCount = 0;
    // Several draws (segs), one by one: as each would have been drawn on its own.
    auto drawEach = [&] {
        std::vector<WORD> own;
        for (UINT k = 0; k < segCount; k++) {
            const DrawSeg& g = segs[k];
            if (g.indexed) {
                own.assign(indices + g.firstIndex, indices + g.firstIndex + g.indexCount);
                for (WORD& i : own) i = WORD(i - g.firstVertex);
            }
            Draw(primType, fvf, static_cast<const uint8_t*>(verts) + size_t(g.firstVertex) * stride, stride,
                 g.vertexCount, g.indexed ? own.data() : nullptr, g.indexed ? g.indexCount : 0, g.primCount, zEnabled);
        }
    };
    // antialias2d=off: a 2D draw (a HUD, a menu, text) skips antialiasing: drawn into the
    // resolved image after the 3D so far, pixel for pixel, its edges as the game drew them
    // (multisampled, a HUD's edges came out soft). 2D: transformed vertices at one depth
    // and one rhw (screen-aligned, no perspective: a sky dome has it), the depth test off,
    // or passing always (ALWAYS) without writing depth: `surf` has no Z buffer, and a 2D
    // draw that writes depth hides what is drawn behind it later (NFS Porsche Unleashed's
    // menu labels, written so, were covered by the translucent buttons drawn after: dim).
    // The next 3D draw copies the part drawn (msDirty) into the multisampled image first,
    // as for the CPU's drawing.
    RECT flatRect = {};
    p.flat2d = false;
    const bool tl = (fvf & 0x0E) == 0x04 && stride >= 16;
    // Emulated W buffering: untransformed vertices take their depth from the projection
    // matrix, and once the game has drawn some, so do its transformed ones (their w put
    // through the same projection: one order for both, as in a W buffer). A game that only
    // draws transformed vertices keeps the scale found from them (g_wScale), which spends
    // the depth buffer's bits better than a projection's near plane does.
    if (p.wEmulate && !tl) p.wMixed = true;
    const bool wRange = p.wEmulate && p.wMixed && p.wProj;
    // Several draws (segs): their vertices only, each scanned once (between their ranges may
    // lie vertices none of them uses).
    static std::vector<std::pair<UINT, UINT>> spans;  // (every caller holds g_lock)
    TlScan scan{};
    if (tl && !segs) {
        scan = ScanTl(static_cast<const uint8_t*>(verts), stride, vertexCount);
    } else if (tl) {
        spans.clear();
        for (UINT k = 0; k < segCount; k++) spans.emplace_back(segs[k].firstVertex, segs[k].firstVertex + segs[k].vertexCount);
        std::sort(spans.begin(), spans.end());
        bool firstSpan = true;
        for (size_t k = 0; k < spans.size();) {
            UINT a = spans[k].first, b = spans[k].second;
            for (k++; k < spans.size() && spans[k].first <= b; k++) b = std::max(b, spans[k].second);
            const TlScan t = ScanTl(static_cast<const uint8_t*>(verts) + size_t(a) * stride, stride, b - a);
            if (firstSpan) {
                scan = t;
            } else {
                scan.zOut |= t.zOut;
                scan.badRhw |= t.badRhw;
                scan.minRhw = scan.minRhw == 0.0f ? t.minRhw : t.minRhw == 0.0f ? scan.minRhw : std::min(scan.minRhw, t.minRhw);
                scan.maxRhw = std::max(scan.maxRhw, t.maxRhw);
            }
            firstSpan = false;
        }
        scan.flat = false;  // (2D: decided draw by draw below)
    }
    const bool depthNeither = !zEnabled || !p.z || (p.rsMirror[D3DRS_ZFUNC] == D3DCMP_ALWAYS && !p.zWrite);
    const bool flatTest = !g_config.aa2d && tl && depthNeither && p.rsMirror[D3DRS_STENCILENABLE] != TRUE && p.rt &&
                          p.rt->buf->gpu && p.rt->buf->gpu->ms;  // (no depth-stencil with `surf`)
    // Several draws: each one's own scan, for what's decided from it below.
    static std::vector<TlScan> segScan;  // (every caller holds g_lock)
    if (segs) {
        if (!tl || scan.badRhw) return drawEach();  // (DrawPVerts' are transformed; bad rhw: the draw's smallest valid one)
        if (flatTest || p.wEmulate) {  // (only what's decided from them below)
            segScan.resize(segCount);
            for (UINT k = 0; k < segCount; k++) {
                segScan[k] = ScanTl(static_cast<const uint8_t*>(verts) + size_t(segs[k].firstVertex) * stride, stride,
                                    segs[k].vertexCount);
                if (flatTest && segScan[k].flat) return drawEach();  // a 2D draw: drawn into `surf`
            }
        }
    }
    if (flatTest) {
        const float x0 = scan.x0, y0 = scan.y0, x1 = scan.x1, y1 = scan.y1;
        if (scan.flat && std::isfinite(x0) && std::isfinite(x1) && std::isfinite(y0) && std::isfinite(y1)) {
            const float w = float(p.rt->buf->w), h = float(p.rt->buf->h);
            flatRect = {LONG(std::clamp(std::floor(x0) - 1.0f, 0.0f, w)), LONG(std::clamp(std::floor(y0) - 1.0f, 0.0f, h)),
                        LONG(std::clamp(std::ceil(x1) + 1.0f, 0.0f, w)), LONG(std::clamp(std::ceil(y1) + 1.0f, 0.0f, h))};
            p.flat2d = true;
        }
    }
    const bool bound = p.BindTargets(false, false);
    const bool flat2d = p.flat2d;
    p.flat2d = false;
    if (!bound || p.vpEmpty) return;
    p.BindTextures();
    if (segs) {
        // Untextured rhw flattened (below): the whole draw's or none.
        bool apart = (scan.minRhw > 0.0f && scan.maxRhw > scan.minRhw * 10000.0f) && (!p.tex[0] || scan.minRhw == 0.0f);
        // (Vertices nearer than a W buffer reaches among them: each draw's scale on its own.)
        if (p.wEmulate && !wRange && scan.maxRhw > kWNearest) return drawEach();
        if (p.wEmulate && !wRange) {  // the W scale each would have been drawn with, the scale as one
            const float all = scan.maxRhw * g_wScale > 1.0f ? 0.5f / scan.maxRhw : g_wScale;
            float s = g_wScale;
            for (UINT k = 0; k < segCount && !apart; k++) {
                if (segScan[k].maxRhw * s > 1.0f) s = 0.5f / segScan[k].maxRhw;
                apart = s != all;
            }
        }
        if (apart) return drawEach();
    }
    if (MsaaType(p.dev) != D3DMULTISAMPLE_NONE) {
        if (!segs) {
            p.ClampCutouts(fvf, verts, stride, vertexCount);
        } else {  // each draw's address modes for the cut-outs: the same, or one by one
            DWORD all[8][2], own[8][2];
            for (UINT k = 0; k < segCount; k++) {
                p.CutoutAddr(fvf, static_cast<const uint8_t*>(verts) + size_t(segs[k].firstVertex) * stride, stride,
                              segs[k].vertexCount, k ? own : all);
                if (k && memcmp(all, own, sizeof all) != 0) return drawEach();
            }
            p.SetAddr(all);
        }
    }
    IDirect3DVertexDeclaration9* decl = p.Decl(fvf);
    if (!decl) return;
    // The declaration this function set last (Gpu::lastDecl).
    if (decl != p.gpu.lastDecl || p.gpu.lastDeclEpoch != g_deviceStateEpoch) {
        p.dev->SetVertexDeclaration(decl);
        p.gpu.lastDecl = decl;
        p.gpu.lastDeclEpoch = g_deviceStateEpoch;
    }
    // Transformed vertices: Direct3D 7 clips their z to [0, 1] only with ZENABLE on, so it
    // can't matter with it off, but D3D9 always clips: clamp it then. rhw 0 on every vertex
    // (2D drawn through Direct3D) is 1: the same picture, without DXVK's infinities.
    // alternatepixelcenter: everything half a pixel up and left, so that pixel centres fall
    // where the drivers that offered it put them (texel centres for 2D drawn 1:1, no lines at
    // texture edges): transformed vertices moved, the projection of the others too.
    // overlaybias: a pass laid on a surface already drawn is pulled forward by N 16-bit
    // depth steps (on: one) and a slope-scaled one pixel's depth change. Its depth equals
    // the surface's only to within rounding (the polygons are cut differently), which the
    // 16-bit depth buffers of the era rounded the same way, and the overlay won; with
    // today's 24 bits less-or-equal fails on half its pixels, in a fine hatch (NFS III's
    // headlights and light maps on the road). The games' depth maths was made for 16 bits,
    // whatever Z buffer they ask for now (NFS3+: 32 bits). An overlay is a pass that
    // doesn't write depth, or one that adds or multiplies (D3DRS_DESTBLEND ONE or a colour
    // factor, or SRCBLEND ZERO or DESTCOLOR): ordinary alpha blending doesn't count (NFS III
    // blends everything). Overlays that would write depth don't, meanwhile: the next light
    // laid on the same spot would fight the first's depth as the first fought the road's.
    {
        const DWORD src = p.rsMirror[D3DRS_SRCBLEND], dst = p.rsMirror[D3DRS_DESTBLEND];
        const bool combines = p.alphaBlend && (dst == D3DBLEND_ONE || dst == D3DBLEND_SRCCOLOR ||
                                               dst == D3DBLEND_INVSRCCOLOR || dst == D3DBLEND_DESTCOLOR ||
                                               src == D3DBLEND_ZERO || src == D3DBLEND_DESTCOLOR);
        const bool overlay = g_config.overlayBias > 0 && zEnabled && (!p.zWrite || combines);
        const float bias = p.gameBias - (overlay ? float(g_config.overlayBias) / 65535.0f : 0.0f);
        const float slope = overlay ? -1.0f : 0.0f;  // (slope-scaled: a pixel's depth change)
        const bool write = p.zWrite && !overlay;
        if (bias != p.biasSet) {
            DWORD b;
            memcpy(&b, &bias, 4);
            p.dev->SetRenderState(D3DRS_DEPTHBIAS, b);
            p.biasSet = bias;
        }
        if (slope != p.slopeSet) {
            DWORD b;
            memcpy(&b, &slope, 4);
            p.dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, b);
            p.slopeSet = slope;
        }
        if (write != p.zWriteSet) {
            p.dev->SetRenderState(D3DRS_ZWRITEENABLE, write);
            p.zWriteSet = write;
        }
    }
    // Transformed vertices aren't clipped: the hardware of the era clamped their depth to
    // the depth range per pixel, and so does D3D9 without clipping. With it, D3D9 cuts
    // triangles where their z leaves [0, 1] (NFS Porsche Unleashed's sky haze, z up to 11,
    // was cut in wedges on Intel's driver; Wine's own Direct3D clamps). Their x and y stay
    // within the guard band: the game's or dd2d9's own clipper (Direct3D 3-6) sees to it.
    const DWORD clipping = (fvf & 0x0E) == 0x04 ? DWORD(FALSE) : p.gameClipping;
    if (clipping != p.clippingSet) {
        p.dev->SetRenderState(D3DRS_CLIPPING, clipping);
        p.clippingSet = clipping;
    }
    const bool shift = g_config.pixelCenter;
    if ((fvf & 0x0E) != 0x04 && p.projDirty) {
        D3DMATRIX m = p.proj;
        if (shift) {
            const float kx = -1.0f / float(std::max<DWORD>(p.vp.Width, 1)), ky = 1.0f / float(std::max<DWORD>(p.vp.Height, 1));
            for (int r = 0; r < 4; r++) {  // clip x += kx * w, clip y += ky * w
                m.m[r][0] += m.m[r][3] * kx;
                m.m[r][1] += m.m[r][3] * ky;
            }
        }
        const float* f = p.fold;
        if (f[0] != 1.0f || f[1] != 0.0f || f[2] != 1.0f || f[3] != 0.0f) {
            for (int r = 0; r < 4; r++) {  // the viewport D3D9 has (SendViewport)
                m.m[r][0] = m.m[r][0] * f[0] + m.m[r][3] * f[1];
                m.m[r][1] = m.m[r][1] * f[2] + m.m[r][3] * f[3];
            }
        }
        p.dev->SetTransform(D3DTS_PROJECTION, &m);
        p.projDirty = false;
    }
    if (tl) {
        const auto* src = static_cast<const uint8_t*>(verts);
        // Bad rhw: hardware of the era interpolated colour across the screen without rhw and
        // used it for texture perspective only; Direct3D 9 weighs every attribute by it, and
        // a triangle whose rhw are all 0 comes out black (Moto Racer's menu letters:
        // untextured, rhw 0 or near it on part of the vertices). Bad: 0, negative, not a
        // number, or a denormal (the GPU takes it as 0). A draw with any bad rhw gets rhw 1 on
        // every vertex when untextured (colour linear on the screen, as then), else the bad
        // ones the draw's smallest valid rhw. An untextured draw whose rhw differ over 10000
        // times is flattened too: the smallest weigh nothing and flush to 0 as easily.
        const bool clampZ = !zEnabled && scan.zOut;  // (ScanTl)
        bool badRhw = scan.badRhw;
        const float minRhw = scan.minRhw, maxRhw = scan.maxRhw;
        const bool flatRhw = (badRhw || (minRhw > 0.0f && maxRhw > minRhw * 10000.0f)) && (!p.tex[0] || minRhw == 0.0f);
        badRhw = badRhw || flatRhw;
        const bool wz = p.wEmulate;
        // The scale for the whole draw at once, from its nearest valid w: shrunk vertex by
        // vertex, a triangle's corners before the nearest one got another scale than those
        // after (its depth warped); an infinite rhw (bad, above) made it 0 and every depth
        // after it 1 until the next whole clear. (Not from an rhw above kWNearest.)
        // (Not with the projection's range, wRange: that has no scale.)
        const float scaleRhw = wz && !wRange ? WScaleRhw(src, stride, vertexCount, maxRhw) : 0.0f;
        if (scaleRhw * g_wScale > 1.0f) g_wScale = 0.5f / scaleRhw;
        if (clampZ || badRhw || shift || wz) {
            p.scratch.assign(src, src + size_t(vertexCount) * stride);
            for (UINT i = 0; i < vertexCount; i++) {
                float* v = reinterpret_cast<float*>(p.scratch.data() + size_t(i) * stride);
                if (clampZ) v[2] = std::clamp(v[2], 0.0f, 1.0f);
                // Emulated W buffering: depth from 1/w (rhw), which varies linearly across
                // the screen over any flat surface, as the Z buffer interpolates it: front to
                // back as a W buffer orders, and coplanar polygons agree however they are cut
                // up (the game's own z may not: NFS III's light maps fought with the road).
                // The scale keeps the nearest w seen within [0, 1], 24 bits for the rest.
                if (wz) {
                    const float rhw = v[3] >= FLT_MIN && std::isfinite(v[3]) ? v[3] : 0.0f;  // (bad: as far as can be)
                    // (wRange: nearer than the near plane is the nearest there can be, past
                    // the far plane the farthest, as a W buffer's range ends.)
                    if (wRange) v[2] = std::clamp(p.wDepthFar + p.wDepthPerRhw * rhw, 0.0f, 1.0f);
                    else v[2] = rhw > kWNearest ? 0.0f : 1.0f - rhw * g_wScale;
                }
                if (flatRhw) v[3] = 1.0f;
                else if (badRhw && (!(v[3] >= FLT_MIN) || !std::isfinite(v[3]))) v[3] = minRhw;
                if (shift) {
                    v[0] -= 0.5f;
                    v[1] -= 0.5f;
                }
            }
            verts = p.scratch.data();
        }
    }
    if (TracingFrame()) {
        float zlo = 1e30f, zhi = -1e30f, wlo = 1e30f, whi = -1e30f;
        for (UINT i = 0; tl && i < vertexCount; i++) {
            const float* v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(verts) + size_t(i) * stride);
            zlo = std::min(zlo, v[2]);
            zhi = std::max(zhi, v[2]);
            wlo = std::min(wlo, v[3]);
            whi = std::max(whi, v[3]);
        }
        const DWORD* rs = p.rsMirror;
        Surface* t0 = p.tex[0];
        float bias, slope;
        memcpy(&bias, &p.biasSet, 4);
        slope = p.slopeSet;
        DD2D9_LOG("drawtrace: prim %lu x%u v%u fvf 0x%lx z(en %lu wr %lu fn %lu) blend(%lu %lu>%lu) atest(%lu %lu ref %lu) "
                  "tex0 %p %dbpp%s%s bias %g slope %g fog(en %lu v %lu t %lu) | z %.6f..%.6f rhw %g..%g%s",
                  primType, primCount, vertexCount, fvf, rs[7], rs[14], rs[23], rs[27], rs[19], rs[20], rs[15], rs[25], rs[24],
                  static_cast<void*>(t0), t0 ? int(t0->buf->fmt.bpp) : 0, t0 && TextureHasAlpha(t0) ? " alpha" : "",
                  t0 && t0->gpuTex && t0->gpuTex->shownKeyed ? " keyed" : "", bias, slope, rs[28], rs[140], rs[35],
                  tl ? zlo : 0.0f, tl ? zhi : 0.0f, tl ? wlo : 0.0f, tl ? whi : 0.0f, flat2d ? " FLAT2D" : "");
    }
    p.gpu.OpenScene();
    if (indices) {
        // Only the vertices the indices use are copied to the GPU (D3D9 takes them from the
        // lowest one): a few triangles from a big vertex buffer don't send all of it.
        WORD lo, hi;
        IndexRange16(indices, indexCount, &lo, &hi);
        // No indices, or one past the vertices: D3D9 would read past what it copied. (The
        // callers replace such indices with vertex 0 already, as XP's runtime tolerated them.)
        if (lo > hi || hi >= vertexCount) return;
        p.dev->DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE(primType), lo, UINT(hi - lo) + 1, primCount, indices, D3DFMT_INDEX16,
                                      verts, stride);
    } else {
        p.dev->DrawPrimitiveUP(D3DPRIMITIVETYPE(primType), primCount, verts, stride);
    }
    p.rt->buf->cpuStale = true;
    p.rt->buf->gpuStale = false;
    if (p.z && p.boundZ && zEnabled && p.zWriteSet && p.z->buf->gpu && p.z->buf->gpu->ztex)
        p.z->buf->cpuStale = true;  // a writable Z buffer: read back when DirectDraw reads it
    GpuSurf* g = p.rt->buf->gpu;
    if (flat2d && g->ms) {  // drawn into `surf`: the multisampled image lacks it there
        g->MsStale(flatRect);
        g->msNewer = false;
    } else {
        g->msNewer = g->ms != nullptr;
    }
    g->serial = ++g_drawSerial;
    if (p.rt->isPrimary) p.rt->MarkDirtyIfVisible();  // drawn into the front surface itself
}

}  // namespace dd2d9
