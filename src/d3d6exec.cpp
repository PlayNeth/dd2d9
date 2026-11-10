// dd2d9's Direct3D 3-6: execute buffers (C §5), Execute and Pick (B §9) and the instruction
// stream's interpreter (C §6).
#include "d3d6int.h"

#include <new>
#include <string>

namespace dd2d9 {
namespace d6 {

// ---------------------------------------------------------------- execute buffer (C §5)

ExecBuf::ExecBuf(Device* dev, DWORD size) : dev(dev), mem(size, 0) {}

ExecBuf::~ExecBuf() {
    if (dev) Unlink(dev->execBufs, this);
}

HRESULT ExecBuf::QueryInterface(REFIID r, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirect3DExecuteBuffer)) {
        AddRef();
        *out = this;
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG ExecBuf::Release() {
    const LONG r = InterlockedDecrement(&ref_);
    if (r == 0) {
        Guard l;
        delete this;
    }
    return ULONG(std::max<LONG>(r, 0));
}

HRESULT ExecBuf::Lock(D3DEXECUTEBUFFERDESC* d) {
    if (!d || d->dwSize != sizeof(D3DEXECUTEBUFFERDESC)) return DDERR_INVALIDPARAMS;
    Guard l;
    if (locked) return D3DERR_EXECUTE_LOCKED;
    locked = true;
    d->dwSize = sizeof(D3DEXECUTEBUFFERDESC);
    d->dwFlags = D3DDEB_LPDATA;
    d->dwCaps = 0;
    d->dwBufferSize = DWORD(mem.size());
    d->lpData = mem.data();
    return D3D_OK;
}

HRESULT ExecBuf::Unlock() {
    Guard l;
    if (!locked) return D3DERR_EXECUTE_NOT_LOCKED;
    locked = false;
    return D3D_OK;
}

HRESULT ExecBuf::SetExecuteData(D3DEXECUTEDATA* d) {
    if (!d || d->dwSize != sizeof(D3DEXECUTEDATA)) return DDERR_INVALIDPARAMS;
    Guard l;
    data = *d;
    return D3D_OK;
}

HRESULT ExecBuf::GetExecuteData(D3DEXECUTEDATA* d) {
    if (!d) return DDERR_INVALIDPARAMS;
    Guard l;
    *d = data;
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
    if (d->dwBufferSize > 0x10000000) return E_OUTOFMEMORY;  // (a vector that size would throw)
    ExecBuf* b = nullptr;
    try {
        b = new ExecBuf(this, d->dwBufferSize);
        execBufs.push_back(b);
    } catch (const std::bad_alloc&) {  // (its memory, in a 32-bit address space)
        delete b;
        return E_OUTOFMEMORY;
    }
    *out = b;
    return D3D_OK;
}

// Execute (C §6.1): the viewport is current for the call.
// log=2: one line per Execute, what its instruction stream holds (walked in order, branches
// not followed): the opcodes with their record counts, triangles and vertices processed.
static void TraceExecute(const ExecBuf* eb, DWORD flags) {
    static const char* const kOps[] = {"?", "POINT", "LINE", "TRIANGLE", "MATRIXLOAD", "MATRIXMULTIPLY",
                                       "STATETRANSFORM", "STATELIGHT", "STATERENDER", "PROCESSVERTICES",
                                       "TEXTURELOAD", "EXIT", "BRANCHFORWARD", "SPAN", "SETSTATUS"};
    DWORD counts[15] = {};
    const D3DEXECUTEDATA& ed = eb->data;
    const uint8_t* base = eb->mem.data();
    const size_t size = eb->mem.size();
    size_t pos = ed.dwInstructionOffset;
    while (pos + sizeof(D3DINSTRUCTION) <= size) {
        const auto* ins = reinterpret_cast<const D3DINSTRUCTION*>(base + pos);
        const DWORD op = ins->bOpcode < 15 ? ins->bOpcode : 0;
        counts[op] += ins->wCount;
        if (ins->bOpcode == D3DOP_EXIT) break;
        pos += sizeof(D3DINSTRUCTION) + size_t(ins->bSize) * ins->wCount;
    }
    std::string ops;
    char part[48];
    for (DWORD i = 1; i < 15; i++) {
        if (!counts[i]) continue;
        snprintf(part, sizeof(part), "%s%s x%lu", ops.empty() ? " " : ", ", kOps[i], static_cast<unsigned long>(counts[i]));
        ops += part;
    }
    DD2D9_TRACE("Execute %p (%s): %lu vertices,%s", static_cast<const void*>(eb),
                flags == D3DEXECUTE_CLIPPED ? "clipped" : "unclipped", static_cast<unsigned long>(ed.dwVertexCount),
                ops.c_str());
}

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
    if (LogLevel() >= 2) TraceExecute(eb, flags);
    HRESULT hr;
    try {
        hr = eb->dev != this ? DDERR_INVALIDPARAMS : eb->locked ? D3DERR_EXECUTE_LOCKED : ExecuteI(eb, flags);
    } catch (const std::bad_alloc&) {  // (the TL buffer and scratch: as DrawCommon)
        hr = E_OUTOFMEMORY;
    }
    cur = prev;
    return hr;
}

HRESULT Device::F1::Execute(IDirect3DExecuteBuffer* b, IDirect3DViewport* v, DWORD flags) {
    return o->ExecuteE(static_cast<ExecBuf*>(b), VpFrom(v), flags);
}

// The execute buffer walked as XP's D3DHAL_ExecutePick walks it (ExecuteI with `picking`:
// transforms and state, no drawing), the hits kept for GetPickRecords. Its result is ignored,
// as XP's: D3D_OK.
HRESULT Device::F1::Pick(IDirect3DExecuteBuffer* b, IDirect3DViewport* v, DWORD, D3DRECT* r) {
    if (!b || !v || !r) return DDERR_INVALIDOBJECT;
    Guard l;
    ExecBuf* eb = static_cast<ExecBuf*>(b);
    if (eb->dev != o || VpFrom(v)->dev != o) return DDERR_INVALIDPARAMS;
    if (eb->locked) return D3DERR_EXECUTE_LOCKED;
    o->pickRecs.clear();
    // What the walk's vertices would leave in the clip status, extents and statistics is
    // put back: a pick isn't a draw.
    const DWORD status = o->status;
    float ext[4];
    memcpy(ext, o->ext, sizeof(ext));
    const D3DSTATS stats = o->stats;
    o->picking = r;
    o->ExecuteE(eb, VpFrom(v), D3DEXECUTE_CLIPPED);
    o->picking = nullptr;
    o->status = status;
    memcpy(o->ext, ext, sizeof(ext));
    o->stats = stats;
    return D3D_OK;
}

// As XP: the records copied when there is room for all of them, the count written always.
HRESULT Device::F1::GetPickRecords(DWORD* n, D3DPICKRECORD* recs) {
    if (!n) return DDERR_INVALIDPARAMS;  // XP writes it unchecked
    Guard l;
    const DWORD count = DWORD(o->pickRecs.size());
    if (recs && *n >= count) std::copy(o->pickRecs.begin(), o->pickRecs.end(), recs);
    *n = count;
    return D3D_OK;
}

// ---------------------------------------------------------------- Execute (C §6.2)

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
    // EXIT: the status and extents into the buffer (a pick leaves them alone).
    auto exit = [&] {
        if (!picking) {
            eb->data.dsStatus.dwFlags = 0;
            eb->data.dsStatus.dwStatus = status;
            ExtentRect(map, ext, &eb->data.dsStatus.drExtent);
        }
        return D3D_OK;
    };
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
            return exit();
        case D3DOP_BRANCHFORWARD: {
            if (!fits(sizeof(D3DBRANCH)) || !count) return D3D_OK;
            const D3DBRANCH& b = *reinterpret_cast<const D3DBRANCH*>(rec);
            const bool c = (status & b.dwMask) == b.dwValue;
            if (c != bool(b.bNegate)) {
                if (!b.dwOffset) return exit();  // as EXIT
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

}  // namespace d6
}  // namespace dd2d9
