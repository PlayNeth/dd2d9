// dd2d9's Direct3D 7: state blocks, recorded and predefined (S §State blocks; d3d7int.h).
#include "d3d7int.h"

#include <new>

namespace dd2d9 {
namespace d7 {

// ---- state blocks

// A handle for `b`, 0 without the memory for one.
DWORD Device7::NewHandle(StateBlock* b) {
    if (!freeHandles_.empty()) {
        const DWORD h = freeHandles_.back();
        freeHandles_.pop_back();
        blocks_[h] = b;
        return h;
    }
    try {
        blocks_.push_back(b);
    } catch (const std::exception&) {
        return 0;
    }
    return DWORD(blocks_.size() - 1);
}

// A call appended to the state block being recorded.
HRESULT Device7::Record(const Cmd& c) {
    try {
        blocks_[recording_]->cmds.push_back(c);
    } catch (const std::exception&) {
        return E_OUTOFMEMORY;
    }
    return D3D_OK;
}

HRESULT Device7::BeginStateBlock() {
    Guard l;
    if (Recording()) return D3DERR_INBEGINSTATEBLOCK;
    auto* b = new (std::nothrow) StateBlock;
    if (!b) return E_OUTOFMEMORY;
    recording_ = NewHandle(b);
    if (!recording_) {
        delete b;
        return E_OUTOFMEMORY;
    }
    return D3D_OK;
}

HRESULT Device7::EndStateBlock(DWORD* handle) {
    if (!handle) return DDERR_INVALIDPARAMS;
    Guard l;
    if (!Recording()) return D3DERR_NOTINBEGINSTATEBLOCK;
    blocks_[recording_]->done = true;
    *handle = recording_;
    recording_ = 0;
    return D3D_OK;
}

HRESULT Device7::Replay(const StateBlock* b) {
    for (const Cmd& c : b->cmds) {
        switch (c.kind) {
        case Cmd::kRS: SetRS(c.a, c.b); break;
        case Cmd::kTSS: SetTSS(c.a, c.b, c.c); break;
        case Cmd::kTex: {
            // XP keeps no reference: a texture released since would be a dangling pointer.
            Surface* s = c.tex;
            if (s && std::find(dd_->surfaces.begin(), dd_->surfaces.end(), s) == dd_->surfaces.end()) {
                bool member = false;
                for (Surface* r : dd_->surfaces)
                    if (std::find(r->children.begin(), r->children.end(), s) != r->children.end()) member = true;
                if (!member) s = nullptr;
            }
            SetTex(c.a, s);
            break;
        }
        case Cmd::kXform: SetXform(c.a, c.m); break;
        case Cmd::kViewport:
            if (HRESULT hr = SetVP(c.vp); FAILED(hr)) return hr;
            break;
        case Cmd::kMaterial: SetMat(c.mat); break;
        case Cmd::kLight: SetLightI(c.a, c.light); break;
        case Cmd::kLightEnable: LightEnableI(c.a, c.b != 0); break;
        case Cmd::kClipPlane: SetPlane(c.a, c.plane); break;
        }
    }
    return D3D_OK;
}

HRESULT Device7::ApplyStateBlock(DWORD h) {
    Guard l;
    if (Recording()) return D3DERR_INBEGINSTATEBLOCK;
    if (h >= blocks_.size() || !blocks_[h] || !blocks_[h]->done) return D3D_OK;  // XP applies nothing (or reads past)
    return Replay(blocks_[h]);
}

void Device7::Capture(StateBlock* b) {
    for (Cmd& c : b->cmds) {
        switch (c.kind) {
        case Cmd::kRS: if (c.a < 256) c.b = rs_[c.a]; break;
        case Cmd::kTSS: if (c.a < 8 && c.b < 64) c.c = tss_[c.a][c.b]; break;
        case Cmd::kTex: c.tex = c.a < 8 ? tex_[c.a] : nullptr; break;
        case Cmd::kXform: if (const D3DMATRIX* m = Matrix(c.a)) c.m = *m; break;
        case Cmd::kViewport: c.vp = vp_; break;
        case Cmd::kMaterial: c.mat = mat_; break;
        case Cmd::kLight: if (c.a < lights_.size()) c.light = lights_[c.a].l; break;
        case Cmd::kLightEnable: if (c.a < lights_.size()) c.b = lights_[c.a].enabled; break;
        case Cmd::kClipPlane: if (c.a < kMaxPlanes) memcpy(c.plane, planes_[c.a], sizeof(c.plane)); break;
        }
    }
}

HRESULT Device7::CaptureStateBlock(DWORD h) {
    Guard l;
    if (Recording()) return D3DERR_INBEGINSTATEBLOCK;
    if (h >= blocks_.size() || !blocks_[h] || !blocks_[h]->done) return D3DERR_INVALIDSTATEBLOCK;
    for (const Cmd& c : blocks_[h]->cmds)
        if ((c.kind == Cmd::kLight || c.kind == Cmd::kLightEnable) && c.a >= lights_.size()) return D3DERR_LIGHT_SET_FAILED;
    Capture(blocks_[h]);
    return D3D_OK;
}

HRESULT Device7::DeleteStateBlock(DWORD h) {
    Guard l;
    if (Recording()) return D3DERR_INBEGINSTATEBLOCK;
    if (h == 0 || h >= blocks_.size() || !blocks_[h] || !blocks_[h]->done) return D3DERR_INVALIDSTATEBLOCK;
    delete blocks_[h];
    blocks_[h] = nullptr;
    try {
        freeHandles_.push_back(h);
    } catch (const std::exception&) {
        // (Without the memory to list it, the handle is just never given out again.)
    }
    return D3D_OK;
}

// The predefined blocks (S §State blocks, CreatePredefined).
HRESULT Device7::CreateStateBlock(D3DSTATEBLOCKTYPE type, DWORD* handle) {
    if (!handle) return DDERR_INVALIDPARAMS;
    Guard l;
    if (Recording()) return D3DERR_INBEGINSTATEBLOCK;
    static const DWORD kAll[] = {4, 2, 29, 7, 8, 9, 10, 14, 15, 16, 19, 20, 22, 23, 24, 25, 26, 28, 33, 34, 35, 36, 37,
                                 38, 40, 41, 27, 47, 48, 52, 53, 54, 55, 56, 57, 58, 59, 60, 128, 129, 130, 131, 132,
                                 133, 134, 135, 139, 141, 140, 136, 137, 138, 143, 142, 148, 147, 145, 146, 151, 152, 144};
    static const DWORD kPixel[] = {4, 2, 7, 8, 9, 10, 14, 15, 16, 19, 20, 23, 24, 25, 26, 33, 36, 37, 38, 40, 41, 27,
                                   47, 52, 53, 54, 55, 56, 57, 58, 59, 60, 128, 129, 130, 131, 132, 133, 134, 135, 144};
    static const DWORD kVertex[] = {9, 29, 22, 28, 34, 35, 36, 37, 38, 48, 139, 141, 140, 136, 137, 138, 143, 142, 148,
                                    147, 145, 146, 151, 152};
    if (type < 0 || type > D3DSBT_VERTEXSTATE) {
        *handle = 0xFFFFFFFF;
        return DDERR_INVALIDPARAMS;
    }
    auto* b = new (std::nothrow) StateBlock;
    if (!b) return E_OUTOFMEMORY;
    try {
        b->cmds.reserve(std::size(kAll) + 8 * 24 + 8 + 1 + 6 + 8 + kMaxPlanes + 1 + 2 * lights_.size());
    } catch (const std::exception&) {
        delete b;
        return E_OUTOFMEMORY;
    }
    auto rs = [&](const DWORD* list, size_t n) {
        for (size_t i = 0; i < n; i++) {
            Cmd c;
            c.kind = Cmd::kRS;
            c.a = list[i];
            b->cmds.push_back(c);
        }
    };
    auto tss = [&](DWORD stage, DWORD t) {
        Cmd c;
        c.kind = Cmd::kTSS;
        c.a = stage;
        c.b = t;
        b->cmds.push_back(c);
    };
    auto simple = [&](Cmd::Kind k, DWORD a) {
        Cmd c;
        c.kind = k;
        c.a = a;
        b->cmds.push_back(c);
    };
    const DWORD stages = std::min<DWORD>(caps_.wMaxTextureBlendStages, 8);
    if (type == D3DSBT_ALL) {
        rs(kAll, std::size(kAll));
        for (DWORD s = 0; s < stages; s++)
            for (DWORD t = 1; t <= 24; t++) tss(s, t);
        for (DWORD s = 0; s < stages; s++) simple(Cmd::kTex, s);
        simple(Cmd::kViewport, 0);
        for (DWORD t : {1u, 2u, 3u, 4u, 5u, 6u}) simple(Cmd::kXform, t);
        for (DWORD s = 0; s < stages; s++) simple(Cmd::kXform, 16 + s);
        for (DWORD p = 0; p < std::min<DWORD>(caps_.wMaxUserClipPlanes, kMaxPlanes); p++) simple(Cmd::kClipPlane, p);
        simple(Cmd::kMaterial, 0);
        for (DWORD i = 0; i < lights_.size(); i++)
            if (lights_[i].set) {
                simple(Cmd::kLight, i);
                simple(Cmd::kLightEnable, i);
            }
    } else if (type == D3DSBT_PIXELSTATE) {
        rs(kPixel, std::size(kPixel));
        for (DWORD s = 0; s < 8; s++)
            for (DWORD t = 1; t <= 24; t++) tss(s, t);
    } else if (type == D3DSBT_VERTEXSTATE) {
        rs(kVertex, std::size(kVertex));
        for (DWORD s = 0; s < 8; s++) {
            tss(s, D3DTSS_TEXCOORDINDEX);
            tss(s, D3DTSS_TEXTURETRANSFORMFLAGS);
        }
        for (DWORD i = 0; i < lights_.size(); i++)
            if (lights_[i].set) simple(Cmd::kLightEnable, i);
    }
    Capture(b);
    b->done = true;
    const DWORD h = NewHandle(b);
    if (!h) {
        delete b;
        return E_OUTOFMEMORY;
    }
    *handle = h;
    return D3D_OK;
}

}  // namespace d7
}  // namespace dd2d9
