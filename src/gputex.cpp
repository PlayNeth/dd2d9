// Direct3D's GPU copies of textures (see gpures.h, gpuint.h).
#include "gpuint.h"

#include "quadpass.h"

#include <algorithm>

namespace dd2d9 {

// The palette a P8 texture is shown with: its own, or the primary's (as DX7 drivers did).
static const Palette* TexturePalette(const Surface* s) {
    if (s->palette) return s->palette;
    if (s->dd->primary && s->dd->primary->palette) return s->dd->primary->palette;
    return nullptr;
}

// An 8-bit texture's palette made with DDPCAPS_ALPHA: each entry's peFlags is its alpha
// (D3DPTEXTURECAPS_ALPHAPALETTE, which dd2d9 reports). PopCap's SexyApp games (Bejeweled 2)
// keep their translucent images this way.
static bool PaletteHasAlpha(const Palette* pal) { return pal && (pal->caps & DDPCAPS_ALPHA); }

bool TextureHasAlpha(const Surface* s) {
    return s->buf->fmt.a || (s->buf->fmt.pal8 && PaletteHasAlpha(TexturePalette(s)));
}

// What a texture's texels are shown with: an 8-bit one's palette, and its colour key, made
// alpha only while D3DRENDERSTATE_COLORKEYENABLE is on.
struct TexLook {
    const Palette* pal;
    bool keyed;
    DWORD key;
    TexLook(const Surface* s, bool colorKey)
        : pal(s->buf->fmt.pal8 ? TexturePalette(s) : nullptr),
          keyed(colorKey && s->hasCk[kCkSrcBlt] && !s->buf->fmt.fourcc && !s->buf->fmt.bump),
          key(keyed ? s->ck[kCkSrcBlt].dwColorSpaceLowValue : 0) {}
    // Not what the copy was made with: every level uploaded again.
    bool Differs(const GpuTexCopy& t) const { return pal != t.pal || (pal && pal->version != t.palVersion) || key != t.key; }
    void Record(GpuTex* t) const {
        t->pal = pal;
        t->palVersion = pal ? pal->version : 0;
        t->keyed = keyed;
        t->key = key;
        t->shownKeyed = keyed;
    }
};

// mipmaps=on: whether the GPU can make a texture of this format's smaller levels itself.
static bool CanAutogen(IDirect3DDevice9* d, D3DFORMAT fmt) {
    static std::unordered_map<int, bool> known;  // Gpu lock
    auto it = known.find(int(fmt));
    if (it != known.end()) return it->second;
    bool ok = false;
    IDirect3D9* d3d = nullptr;
    if (SUCCEEDED(d->GetDirect3D(&d3d))) {
        D3DDEVICE_CREATION_PARAMETERS cp;
        D3DDISPLAYMODE dm;
        if (SUCCEEDED(d->GetCreationParameters(&cp)) && SUCCEEDED(d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &dm)))
            ok = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, dm.Format, D3DUSAGE_AUTOGENMIPMAP,
                                        D3DRTYPE_TEXTURE, fmt) == D3D_OK;  // D3DOK_NOAUTOGEN: no
        d3d->Release();
    }
    known[int(fmt)] = ok;
    return ok;
}

// A cube map's GPU copy: a D3D9 cube texture, each face's levels uploaded where their CPU
// copies changed (faces Direct3D drew into read back first). Gpu lock held.
static IDirect3DCubeTexture9* EnsureCube(Gpu& gpu, IDirect3DDevice9* d, Surface* s, bool colorKey) {
    GpuTex* t = s->gpuTex;
    static const DWORD kFaces[6] = {DDSCAPS2_CUBEMAP_POSITIVEX, DDSCAPS2_CUBEMAP_NEGATIVEX, DDSCAPS2_CUBEMAP_POSITIVEY,
                                    DDSCAPS2_CUBEMAP_NEGATIVEY, DDSCAPS2_CUBEMAP_POSITIVEZ, DDSCAPS2_CUBEMAP_NEGATIVEZ};
    // The faces under the cube map's root (a face bound: its own cube map). A face the game
    // didn't make (DirectX 7 allows that) stays unwritten.
    Surface* const root = s->root ? s->root : s;
    Surface* chains[6][16] = {};
    UINT levels = 0;
    for (int f = 0; f < 6; f++) {
        Surface* face = CubeFace(root, kFaces[f]);
        if (!face) continue;
        UINT n = 0;
        for (Surface* l = face; l && n < 16; l = l->NextMipLevel()) chains[f][n++] = l;
        if (!levels) levels = n;
        else if (n != levels) return nullptr;
    }
    if (!levels) return nullptr;
    const TexLook look(s, colorKey);
    const bool keyed = look.keyed;
    const DWORD key = look.key;
    bool all = false;
    if (!t->cube || t->levels != levels || keyed != t->keyed || t->w != s->buf->w || !(t->made == s->buf->fmt)) {
        t->Drop();
        t->PickFormat(s->buf->fmt, keyed);
        HRESULT hr = d->CreateCubeTexture(s->buf->w, levels, 0, t->fmt, D3DPOOL_MANAGED, &t->cube, nullptr);
        if (FAILED(hr) && t->FallBack(s->buf->fmt))
            hr = d->CreateCubeTexture(s->buf->w, levels, 0, t->fmt, D3DPOOL_MANAGED, &t->cube, nullptr);
        if (FAILED(hr)) {
            DD2D9_LOG("Direct3D: cube texture %d, %u levels (format %d) failed: 0x%08lx", s->buf->w, levels, int(t->fmt), hr);
            t->cube = nullptr;
            return nullptr;
        }
        static bool logged;  // Gpu lock
        if (!logged) {
            logged = true;
            DD2D9_LOG("Direct3D: the game uses cube maps");
        }
        t->levels = levels;
        t->w = t->h = s->buf->w;
        t->made = s->buf->fmt;
        t->autogen = false;
        all = true;
    }
    if (look.Differs(*t)) all = true;
    t->levelBufs.resize(size_t(6) * levels, nullptr);
    t->levelSerials.resize(size_t(6) * levels, 0);
    const PALETTEENTRY* entries = look.pal ? look.pal->entries : s->dd->defaultPalette;
    for (int f = 0; f < 6; f++)
        for (UINT i = 0; i < levels && chains[f][0]; i++) {
            Buffer* b = chains[f][i]->buf;
            if (b->gpu && b->cpuStale && b->gpu->surf) ReadBack(gpu, d, b->gpu);  // a face drawn into
            const size_t k = size_t(f) * levels + i;
            if (!all && t->levelBufs[k] == b && t->levelSerials[k] == b->cpuSerial) continue;
            D3DSURFACE_DESC ld;
            if (FAILED(t->cube->GetLevelDesc(i, &ld)) || UINT(b->w) > ld.Width || UINT(b->h) > ld.Height ||
                !(b->fmt == s->buf->fmt))
                continue;
            D3DLOCKED_RECT lr;
            if (FAILED(t->cube->LockRect(D3DCUBEMAP_FACES(f), i, &lr, nullptr, 0))) continue;
            WriteLevel(b, lr, entries, t->convert, keyed, key, PaletteHasAlpha(look.pal));
            t->cube->UnlockRect(D3DCUBEMAP_FACES(f), i);
            t->levelBufs[k] = b;
            t->levelSerials[k] = b->cpuSerial;
        }
    look.Record(t);
    return t->cube;
}

// ps_2_0: the texel transparent black where its colour, in the texture's own format, is the
// key (as the CPU path's RowConverter makes it). c0 = each channel's largest value (r, g, b), c3 = minus
// the key's channels. Each channel quantized as ARGBToPixel rounds: floor(c * max + 0.498).
// Assembled (psasm) from:
//   def c2, 0.498, -0.5, 0, 0 / dcl t0 / dcl_2d s0 / mov r1..r3, c2.z / texld r0, t0, s0
//   r1.xyz = r0 * c0 + c2.x / r2.xy = frc(r1) / r3.x = r1.z, r3.xy = frc(r3), r2.z = r3.x
//   r1.xyz = abs(r1 - r2 + c3) / r1.x = max(r1.x, r1.y, r1.z) + c2.y
//   r0 = r1.x >= 0 ? r0 : 0 / oC0 = r0
// Written to Windows' ps_2_0 rules as the presenter's shaders are (registers set whole
// first, no negated constants, one constant per instruction, frc on .xy).
static const DWORD kKeyPs[] = {
    0xffff0200, 0x0200001f, 0x80000000, 0xb00f0000, 0x0200001f, 0x90000000, 0xa00f0800, 0x05000051, 0xa00f0002,
    0x3efef9db, 0xbf000000, 0x00000000, 0x00000000, 0x02000001, 0x800f0001, 0xa0aa0002, 0x02000001, 0x800f0002,
    0xa0aa0002, 0x02000001, 0x800f0003, 0xa0aa0002, 0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x03000005,
    0x80070001, 0x80e40000, 0xa0e40000, 0x03000002, 0x80070001, 0x80e40001, 0xa0000002, 0x02000013, 0x80030002,
    0x80e40001, 0x02000001, 0x80010003, 0x80aa0001, 0x02000013, 0x80030003, 0x80e40003, 0x02000001, 0x80040002,
    0x80000003, 0x03000002, 0x80070001, 0x80e40001, 0x81e40002, 0x03000002, 0x80070001, 0x80e40001, 0xa0e40003,
    0x02000023, 0x80070001, 0x80e40001, 0x0300000b, 0x80010001, 0x80000001, 0x80550001, 0x0300000b, 0x80010001,
    0x80000001, 0x80aa0001, 0x03000002, 0x80010001, 0x80000001, 0xa0550002, 0x04000058, 0x800f0000, 0x80000001,
    0x80e40000, 0xa0aa0002, 0x02000001, 0x800f0800, 0x80e40000, 0x0000ffff};

// A texture with levels Direct3D drew into, made on the GPU (see GpuTex::rtTex); null when
// none of its levels is newer on the GPU, or this can't be done here (the CPU copy is used:
// read back, then uploaded). Palettized textures go through the CPU (the palette is applied
// there). Gpu lock held.
static IDirect3DTexture9* RtTexture(Gpu& gpu, IDirect3DDevice9* d, GpuTex* t, Surface* const* chain, size_t n,
                                    const Format& fmt, bool keyed, DWORD key) {
    auto newest = [](const Buffer* b) { return b->gpu && b->gpu->surf && b->cpuStale && !b->gpuStale; };
    D3DFORMAT gfmt = D3DFMT_UNKNOWN;
    for (size_t i = 0; i < n; i++) {
        const Buffer* b = chain[i]->buf;
        if (!newest(b)) continue;
        if (gfmt == D3DFMT_UNKNOWN) gfmt = b->gpu->fmt;
        else if (b->gpu->fmt != gfmt) return nullptr;  // (levels drawn in different formats)
    }
    if (gfmt == D3DFMT_UNKNOWN || fmt.pal8 || fmt.fourcc || fmt.bump) return nullptr;
    // The texture's format: the render targets', or with a key A8R8G8B8 (alpha for it). The
    // levels the CPU has newer go up as they are (the render targets' layout is the
    // texture's) or converted to 32-bit colour.
    const D3DFORMAT T = keyed ? D3DFMT_A8R8G8B8 : gfmt;
    const bool cpuConvert = keyed || T != NativeFormat(fmt);
    if (cpuConvert && T != D3DFMT_A8R8G8B8 && T != D3DFMT_X8R8G8B8) return nullptr;
    if (keyed && !t->keyPs && FAILED(d->CreatePixelShader(kKeyPs, &t->keyPs))) {
        t->keyPs = nullptr;
        return nullptr;
    }
    const Buffer* top = chain[0]->buf;
    if (!t->rtTex || t->rtW != UINT(top->w) || t->rtH != UINT(top->h) || t->rtFmt != T || t->rtLevels != n) {
        t->DropRt();
        if (FAILED(d->CreateTexture(top->w, top->h, UINT(n), D3DUSAGE_RENDERTARGET, T, D3DPOOL_DEFAULT, &t->rtTex, nullptr))) {
            t->rtTex = nullptr;
            return nullptr;
        }
        t->rtW = top->w;
        t->rtH = top->h;
        t->rtFmt = T;
        t->rtLevels = UINT(n);
    }
    if (t->rtLevelSrc.size() != n || keyed != t->rtKeyed || key != t->rtKey) {
        t->rtLevelSrc.assign(n, nullptr);
        t->rtLevelSerial.assign(n, 0);
        t->rtKeyed = keyed;
        t->rtKey = key;
    }
    for (size_t i = 0; i < n; i++) {
        Buffer* b = chain[i]->buf;
        IDirect3DSurface9* level = nullptr;
        D3DSURFACE_DESC ld;
        if (FAILED(t->rtTex->GetLevelDesc(UINT(i), &ld)) || UINT(b->w) != ld.Width || UINT(b->h) != ld.Height ||
            !(b->fmt == fmt))
            return nullptr;  // (a level given another size or format since: the CPU path copes)
        if (newest(b)) {
            GpuSurf* g = b->gpu;
            if (t->rtLevelSrc[i] == g && t->rtLevelSerial[i] == g->serial) continue;
            Resolve(gpu, d, g);
            if (FAILED(t->rtTex->GetSurfaceLevel(UINT(i), &level))) return nullptr;
            gpu.CloseScene();  // StretchRect: outside a scene
            bool ok;
            if (!keyed) {
                ok = SUCCEEDED(d->StretchRect(g->surf, nullptr, level, nullptr, D3DTEXF_NONE));
            } else {
                // The render target copied into a texture, drawn through the key shader.
                if (!t->rtTmp || t->tmpW != UINT(b->w) || t->tmpH != UINT(b->h) || t->tmpFmt != g->fmt) {
                    if (t->rtTmp) t->rtTmp->Release();
                    if (FAILED(d->CreateTexture(b->w, b->h, 1, D3DUSAGE_RENDERTARGET, g->fmt, D3DPOOL_DEFAULT, &t->rtTmp,
                                                nullptr)))
                        t->rtTmp = nullptr;
                    t->tmpW = b->w;
                    t->tmpH = b->h;
                    t->tmpFmt = g->fmt;
                }
                IDirect3DSurface9* tmp = nullptr;
                ok = t->rtTmp && SUCCEEDED(t->rtTmp->GetSurfaceLevel(0, &tmp)) &&
                     SUCCEEDED(d->StretchRect(g->surf, nullptr, tmp, nullptr, D3DTEXF_NONE));
                if (tmp) tmp->Release();
                if (ok) {
                    auto bits = [](DWORD mask) { return mask ? float(mask >> __builtin_ctz(mask)) : 0.0f; };
                    auto chan = [](DWORD v, DWORD mask) { return mask ? -float((v & mask) >> __builtin_ctz(mask)) : 0.0f; };
                    const float maxes[4] = {bits(fmt.r), bits(fmt.g), bits(fmt.b), 0};
                    const float minusKey[4] = {chan(key, fmt.r), chan(key, fmt.g), chan(key, fmt.b), 0};
                    QuadPass pass(gpu, d, t->keyPs);
                    pass.Target(level);
                    pass.Texture(0, t->rtTmp);
                    d->SetPixelShaderConstantF(0, maxes, 1);
                    d->SetPixelShaderConstantF(3, minusKey, 1);
                    const RECT all = {0, 0, b->w, b->h};
                    ok = pass.Draw(all, all, b->w, b->h);
                    gpu.CloseScene();
                }
            }
            level->Release();
            if (!ok) return nullptr;
            t->rtLevelSrc[i] = g;
            t->rtLevelSerial[i] = g->serial;
        } else {
            // A level the CPU has newer (or that nothing drew into): its CPU copy, through a
            // system-memory texture (what Direct3D drew there merged in first, if both changed).
            if (b->gpu && b->cpuStale && b->gpu->surf) ReadBack(gpu, d, b->gpu);
            if (t->rtLevelSrc[i] == b && t->rtLevelSerial[i] == b->cpuSerial) continue;
            if (!t->rtSys && FAILED(d->CreateTexture(top->w, top->h, UINT(n), 0, T, D3DPOOL_SYSTEMMEM, &t->rtSys, nullptr))) {
                t->rtSys = nullptr;
                return nullptr;
            }
            D3DLOCKED_RECT lr;
            if (FAILED(t->rtSys->LockRect(UINT(i), &lr, nullptr, 0))) return nullptr;
            WriteLevel(b, lr, chain[0]->dd->defaultPalette, cpuConvert, keyed, key);
            t->rtSys->UnlockRect(UINT(i));
            IDirect3DSurface9* sys = nullptr;
            bool ok = SUCCEEDED(t->rtSys->GetSurfaceLevel(UINT(i), &sys)) && SUCCEEDED(t->rtTex->GetSurfaceLevel(UINT(i), &level));
            gpu.CloseScene();
            ok = ok && SUCCEEDED(d->UpdateSurface(sys, nullptr, level, nullptr));
            if (sys) sys->Release();
            if (level) level->Release();
            if (!ok) return nullptr;
            t->rtLevelSrc[i] = b;
            t->rtLevelSerial[i] = b->cpuSerial;
        }
    }
    static bool loggedKeyed, loggedMip;  // Gpu lock
    if (keyed && !loggedKeyed) {
        loggedKeyed = true;
        DD2D9_LOG("Direct3D: a colour-keyed render-target texture copied on the GPU");
    }
    if (n > 1 && !loggedMip) {
        loggedMip = true;
        DD2D9_LOG("Direct3D: a mipmapped render-target texture copied on the GPU");
    }
    return t->rtTex;
}

// The texture's GPU copy, uploaded where the CPU copy changed. Gpu lock held.
IDirect3DBaseTexture9* EnsureTex(Gpu& gpu, IDirect3DDevice9* d, Surface* s, bool colorKey) {
    if (s->gpuTex && s->gpuTex->gpu != &gpu) return nullptr;
    if (!s->gpuTex) {
        s->gpuTex = new GpuTex(&gpu, s);
        gpu.AddOwner(s->gpuTex);
    }
    GpuTex* t = s->gpuTex;
    if (s->desc.ddsCaps.dwCaps2 & DDSCAPS2_CUBEMAP) return EnsureCube(gpu, d, s, colorKey);
    struct Chain {  // the mip levels, at most 16 (no allocation per bound texture per draw)
        Surface* l[16];
        size_t n = 0;
        size_t size() const { return n; }
        Surface* operator[](size_t i) const { return l[i]; }
    } chain;
    for (Surface* l = s; l && chain.n < 16; l = l->NextMipLevel()) chain.l[chain.n++] = l;
    const TexLook look(s, colorKey);
    const bool keyed = look.keyed;
    const DWORD key = look.key;
    // Levels rendered into by Direct3D and not touched by the CPU since: copied on the GPU.
    if (IDirect3DTexture9* rt = RtTexture(gpu, d, t, chain.l, chain.n, s->buf->fmt, keyed, key)) {
        t->levelBufs.assign(chain.size(), nullptr);  // the managed copies are behind now
        t->alt.levelBufs.assign(t->alt.levelBufs.size(), nullptr);
        t->shownKeyed = keyed;
        return rt;
    }
    bool all = false;
    // The colour key turned on or off since: the other copy, made now if there is none (or
    // none for this state); the one in use kept for the next change back.
    if (t->tex && keyed != t->keyed) {
        if (t->alt.tex && t->alt.keyed != keyed) t->DropAlt();
        t->SwapAlt();
    }
    // (Made with GPU mipmaps and given alpha since, by a palette with alpha: made again without.)
    if (!t->tex || t->levels != chain.size() || keyed != t->keyed || t->w != s->buf->w || t->h != s->buf->h ||
        !(t->made == s->buf->fmt) || (t->autogen && TextureHasAlpha(s))) {
        t->Drop();
        t->PickFormat(s->buf->fmt, keyed);
        // mipmaps=on: a texture the game gave one level, made with the rest by the GPU. Not
        // cut-outs, colour-keyed or with an alpha channel (the transparent texels blur into
        // the smaller levels: dark fringes and lines in the distance, NFS III's corn), not
        // block-compressed ones (D3D9 can't), not 1x1.
        HRESULT hr = E_FAIL;
        t->autogen = false;
        if (g_config.mipmaps && chain.size() == 1 && !keyed && !TextureHasAlpha(s) && !s->buf->fmt.fourcc && !s->buf->fmt.bump &&
            (s->buf->w > 1 || s->buf->h > 1) &&
            CanAutogen(d, t->fmt)) {
            hr = d->CreateTexture(s->buf->w, s->buf->h, 0, D3DUSAGE_AUTOGENMIPMAP, t->fmt, D3DPOOL_MANAGED, &t->tex, nullptr);
            t->autogen = SUCCEEDED(hr);
            static bool logged;  // Gpu lock
            if (t->autogen && !logged) {
                logged = true;
                DD2D9_LOG("mipmaps: the GPU makes the smaller levels of textures that have none");
            }
        }
        if (!t->autogen)
            hr = d->CreateTexture(s->buf->w, s->buf->h, UINT(chain.size()), 0, t->fmt, D3DPOOL_MANAGED, &t->tex, nullptr);
        if (FAILED(hr) && t->FallBack(s->buf->fmt)) {
            t->autogen = false;
            hr = d->CreateTexture(s->buf->w, s->buf->h, UINT(chain.size()), 0, t->fmt, D3DPOOL_MANAGED, &t->tex,
                                  nullptr);
        }
        if (FAILED(hr)) {
            DD2D9_LOG("Direct3D: texture %dx%d, %u levels (format %d) failed: 0x%08lx", s->buf->w, s->buf->h,
                      unsigned(chain.size()), int(t->fmt), hr);
            t->tex = nullptr;
            return nullptr;
        }
        t->levels = UINT(chain.size());
        t->w = s->buf->w;
        t->h = s->buf->h;
        t->made = s->buf->fmt;
        all = true;
    }
    if (look.Differs(*t)) all = true;
    t->levelBufs.resize(chain.size(), nullptr);
    t->levelSerials.resize(chain.size(), 0);
    const PALETTEENTRY* entries = look.pal ? look.pal->entries : s->dd->defaultPalette;
    for (UINT i = 0; i < chain.size(); i++) {
        Buffer* b = chain[i]->buf;
        // Rendered into by Direct3D (a render target used as a texture): read back first.
        if (b->gpu && b->cpuStale && b->gpu->surf) ReadBack(gpu, d, b->gpu);
        if (!all && t->levelBufs[i] == b && t->levelSerials[i] == b->cpuSerial) continue;
        D3DSURFACE_DESC ld;
        // A level given another size since (SetSurfaceDesc on a lower level): not written
        // past the GPU level's rows.
        if (FAILED(t->tex->GetLevelDesc(i, &ld)) || UINT(b->w) > ld.Width || UINT(b->h) > ld.Height ||
            !(b->fmt == s->buf->fmt))
            continue;
        D3DLOCKED_RECT lr;
        if (FAILED(t->tex->LockRect(i, &lr, nullptr, 0))) continue;
        WriteLevel(b, lr, entries, t->convert, keyed, key, PaletteHasAlpha(look.pal));
        t->tex->UnlockRect(i);
        t->levelBufs[i] = b;
        t->levelSerials[i] = b->cpuSerial;
        if (t->autogen) t->tex->GenerateMipSubLevels();
    }
    look.Record(t);
    return t->tex;
}

void GpuSurfaceGone(Surface* s) {
    GpuTex* t = s->gpuTex;
    s->gpuTex = nullptr;
    if (t->gpu) {
        GpuGuard l(*t->gpu);
        t->DropAll();
        t->gpu->RemoveOwner(t);
    }
    delete t;
}

}  // namespace dd2d9
