// fmtprobe: what a ddraw.dll and its display driver say about texture formats, written to
// fmtprobe.txt beside the program. Run it with Windows' own ddraw.dll (on XP or 98 with a card
// of the time, or on a newer Windows) to see what games are shown there, and with dd2d9's to
// compare:
//   - the texture formats each Direct3D 7 device and Direct3D 3's and 2's hardware and
//     software devices list;
//   - for the formats that aren't colours or are palettized below 8 bits: whether a texture
//     of it can be made, and whether a surface of it can be a render target (made in video
//     memory for a hardware device, in system memory for the RGB one);
//   - which other layouts of those kinds CreateSurface takes for a plain system-memory
//     surface, with the pitch and the format the surface then reports, its DC (without a
//     palette and with one) and a colour fill;
//   - a 4-bit surface's palettes and copies;
//   - SetRenderTarget on the software device to textures of those formats and to a Z buffer,
//     with the target the device has afterwards;
//   - what the software and the hardware device leave in targets they are asked to clear;
//   - with the display in a 256-colour mode (640x480, set for a moment): which 8-bit layouts
//     CreateSurface takes, and their DC without a palette.
// With DirectX 7 or later the surfaces and devices are IDirectDraw7's and IDirect3D7's; with
// DirectX 6 (no DirectDrawCreateEx in ddraw.dll) IDirectDraw4's and IDirect3D3's.
// "fmtprobe dx6", or the program under a name with "dx6" in it (fmtprobe-dx6.exe), uses
// DirectX 6's interfaces where 7's are there too, and writes fmtprobe-dx6.txt.
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <cstdio>
#include <cstring>

#include "fmtprobe.h"

static FILE* g_out;

static void Describe(const DDPIXELFORMAT& f, char* out, size_t n) {
    if (f.dwFlags & DDPF_FOURCC) {
        snprintf(out, n, "FourCC %.4s", reinterpret_cast<const char*>(&f.dwFourCC));
    } else if (f.dwFlags & DDPF_BUMPDUDV) {
        snprintf(out, n, "bump %lu bits, du 0x%lx dv 0x%lx luminance 0x%lx", f.dwBumpBitCount, f.dwBumpDuBitMask, f.dwBumpDvBitMask,
                 f.dwBumpLuminanceBitMask);
    } else if (f.dwFlags & DDPF_LUMINANCE) {
        snprintf(out, n, "luminance %lu bits, 0x%lx%s alpha 0x%lx", f.dwLuminanceBitCount, f.dwLuminanceBitMask,
                 (f.dwFlags & DDPF_ALPHAPIXELS) ? "," : ", no alpha flag,", f.dwLuminanceAlphaBitMask);
    } else if (f.dwFlags & DDPF_ALPHA) {
        snprintf(out, n, "alpha only, %lu bits", f.dwAlphaBitDepth);
    } else if (f.dwFlags & (DDPF_PALETTEINDEXED1 | DDPF_PALETTEINDEXED2 | DDPF_PALETTEINDEXED4 | DDPF_PALETTEINDEXED8)) {
        snprintf(out, n, "palettized %lu bits", f.dwRGBBitCount);
    } else if (f.dwFlags & DDPF_ZBUFFER) {
        snprintf(out, n, "Z %lu bits", f.dwZBufferBitDepth);
    } else {
        snprintf(out, n, "RGB %lu bits, r 0x%lx g 0x%lx b 0x%lx a 0x%lx", f.dwRGBBitCount, f.dwRBitMask, f.dwGBitMask, f.dwBBitMask,
                 (f.dwFlags & DDPF_ALPHAPIXELS) ? f.dwRGBAlphaBitMask : 0);
    }
}

static void Line(const DDPIXELFORMAT& f) {
    char text[160];
    Describe(f, text, sizeof(text));
    fprintf(g_out, "    %s (flags 0x%lx)\n", text, f.dwFlags);
}

// (Direct3D 3's and 7's callback.)
static HRESULT WINAPI FormatLine(DDPIXELFORMAT* f, void*) {
    Line(*f);
    return D3DENUMRET_OK;
}

static HRESULT WINAPI FormatLine2(DDSURFACEDESC* d, void*) {
    Line(d->ddpfPixelFormat);
    return D3DENUMRET_OK;
}

// A 64x64 surface to make a device on (a plain one, or a texture): the desktop's format, or *pf.
template <class DD, class Surf>
static Surf* Target(DD* dd, DWORD memory, const DDPIXELFORMAT* pf, HRESULT* hr, bool texture = false) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | (pf ? DDSD_PIXELFORMAT : 0);
    d.ddsCaps.dwCaps = (texture ? DDSCAPS_TEXTURE : DDSCAPS_OFFSCREENPLAIN) | DDSCAPS_3DDEVICE | memory;
    d.dwWidth = d.dwHeight = 64;
    if (pf) d.ddpfPixelFormat = *pf;
    Surf* s = nullptr;
    *hr = dd->CreateSurface(&d, &s, nullptr);
    return s;
}

// A device of `guid` on `rt`, let go at once: whether it can be made.
static HRESULT TryDevice(IDirect3D7* d3d, REFCLSID guid, IDirectDrawSurface7* rt) {
    IDirect3DDevice7* dev = nullptr;
    const HRESULT hr = d3d->CreateDevice(guid, rt, &dev);
    if (dev) dev->Release();
    return hr;
}
static HRESULT TryDevice(IDirect3D3* d3d, REFCLSID guid, IDirectDrawSurface4* rt) {
    IDirect3DDevice3* dev = nullptr;
    const HRESULT hr = d3d->CreateDevice(guid, rt, &dev, nullptr);
    if (dev) dev->Release();
    return hr;
}

// A device of `guid` on `rt`, the whole of it cleared to `color`: the device's result, or the
// clear's.
static HRESULT ClearWith(IDirect3D7* d3d, REFCLSID guid, IDirectDrawSurface7* rt, DWORD color) {
    IDirect3DDevice7* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(guid, rt, &dev);
    if (!dev) return hr;
    hr = dev->Clear(0, nullptr, D3DCLEAR_TARGET, color, 1.0f, 0);
    dev->Release();
    return hr;
}
static HRESULT ClearWith(IDirect3D3* d3d, REFCLSID guid, IDirectDrawSurface4* rt, DWORD color) {
    IDirect3DDevice3* dev = nullptr;
    IDirect3DViewport3* vp = nullptr;
    HRESULT hr = d3d->CreateDevice(guid, rt, &dev, nullptr);
    if (!dev) return hr;
    hr = d3d->CreateViewport(&vp, nullptr);
    if (SUCCEEDED(hr)) hr = dev->AddViewport(vp);
    D3DVIEWPORT2 vd = {};
    vd.dwSize = sizeof(vd);
    vd.dwWidth = vd.dwHeight = 64;
    vd.dvClipX = -1.0f, vd.dvClipY = 1.0f, vd.dvClipWidth = vd.dvClipHeight = 2.0f;
    vd.dvMaxZ = 1.0f;
    if (SUCCEEDED(hr)) hr = vp->SetViewport2(&vd);
    if (SUCCEEDED(hr)) hr = dev->SetCurrentViewport(vp);
    // (In a scene: a hardware device's clear outside one isn't in the surface when it is
    // locked next, on a Voodoo3 and on VMware's adapter alike.)
    D3DRECT all = {{0}, {0}, {64}, {64}};
    if (SUCCEEDED(hr)) {
        dev->BeginScene();
        hr = vp->Clear2(1, &all, D3DCLEAR_TARGET, color, 1.0f, 0);
        dev->EndScene();
    }
    if (vp) vp->Release();
    dev->Release();
    return hr;
}

// An RGB device on `first`, then told to draw into `next` instead: SetRenderTarget's result
// (the device's own, if it can't be made), and in *after the target the device then has: "the
// new one", "the old one", or what GetRenderTarget said.
template <class Surf> static void NameTarget(HRESULT got, Surf* now, Surf* first, Surf* next, char* after, size_t n) {
    if (FAILED(got)) snprintf(after, n, "GetRenderTarget 0x%08lx", got);
    else snprintf(after, n, "%s", now == next ? "the new one" : now == first ? "the old one" : "another surface");
    if (now) now->Release();
}
static HRESULT Retarget(IDirect3D7* d3d, IDirectDrawSurface7* first, IDirectDrawSurface7* next, char* after, size_t n) {
    IDirect3DDevice7* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(IID_IDirect3DRGBDevice, first, &dev);
    snprintf(after, n, "no device");
    if (!dev) return hr;
    hr = dev->SetRenderTarget(next, 0);
    IDirectDrawSurface7* now = nullptr;
    const HRESULT got = dev->GetRenderTarget(&now);
    NameTarget(got, now, first, next, after, n);
    dev->Release();
    return hr;
}
static HRESULT Retarget(IDirect3D3* d3d, IDirectDrawSurface4* first, IDirectDrawSurface4* next, char* after, size_t n) {
    IDirect3DDevice3* dev = nullptr;
    HRESULT hr = d3d->CreateDevice(IID_IDirect3DRGBDevice, first, &dev, nullptr);
    snprintf(after, n, "no device");
    if (!dev) return hr;
    hr = dev->SetRenderTarget(next, 0);
    IDirectDrawSurface4* now = nullptr;
    const HRESULT got = dev->GetRenderTarget(&now);
    NameTarget(got, now, first, next, after, n);
    dev->Release();
    return hr;
}

struct Lister {
    IDirectDraw7* dd;
    IDirect3D7* d3d;
};

static HRESULT WINAPI ListDevice7(char* description, char* name, D3DDEVICEDESC7* desc, void* ctx) {
    auto* l = static_cast<Lister*>(ctx);
    const bool hardware = (desc->dwDevCaps & D3DDEVCAPS_HWRASTERIZATION) != 0;
    fprintf(g_out, "\nDirect3D 7 device \"%s\" (%s), %s:\n", name, description, hardware ? "hardware" : "software");
    HRESULT hr;
    IDirectDrawSurface7* rt =
        Target<IDirectDraw7, IDirectDrawSurface7>(l->dd, hardware ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY, nullptr, &hr);
    IDirect3DDevice7* dev = nullptr;
    if (rt) hr = l->d3d->CreateDevice(desc->deviceGUID, rt, &dev);
    if (dev) {
        dev->EnumTextureFormats(FormatLine, nullptr);
        dev->Release();
    } else {
        fprintf(g_out, "    no device (0x%08lx)\n", hr);
    }
    if (rt) rt->Release();
    return D3DENUMRET_OK;
}

// Formats that aren't colours, or palettized below 8 bits: as a texture, as a render target.
// Then layouts as plain system-memory surfaces.
template <class DD, class Surf, class D3D> static void Formats(DD* dd, D3D* d3d) {
    struct Kind {
        const char* name;
        DWORD flags, bits, m0, m1, alpha;
    };
    static const Kind kKinds[] = {
        {"8-bit luminance", DDPF_LUMINANCE, 8, 0xFF, 0, 0},
        {"16-bit luminance with alpha", DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 16, 0xFF, 0, 0xFF00},
        {"8-bit luminance with alpha", DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 8, 0x0F, 0, 0xF0},
        {"8-bit alpha only", DDPF_ALPHA, 8, 0, 0, 0},
        {"16-bit bump map (du, dv)", DDPF_BUMPDUDV, 16, 0xFF, 0xFF00, 0},
        {"4-bit palettized", DDPF_RGB | DDPF_PALETTEINDEXED4, 4, 0, 0, 0},
        {"1-bit palettized", DDPF_RGB | DDPF_PALETTEINDEXED1, 1, 0, 0, 0},
    };
    fprintf(g_out, "\nOther formats (0x00000000 is success):\n");
    for (const Kind& k : kKinds) {
        DDPIXELFORMAT pf = {};
        pf.dwSize = sizeof(pf);
        pf.dwFlags = k.flags;
        pf.dwRGBBitCount = k.bits;
        pf.dwRBitMask = k.m0;
        pf.dwGBitMask = k.m1;
        pf.dwRGBAlphaBitMask = k.alpha;
        DDSURFACEDESC2 t = {};
        t.dwSize = sizeof(t);
        t.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        t.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
        t.dwWidth = t.dwHeight = 64;
        t.ddpfPixelFormat = pf;
        fprintf(g_out, "  %s: texture", k.name);
        static const DWORD kMemory[3] = {0, DDSCAPS_SYSTEMMEMORY, DDSCAPS_VIDEOMEMORY};
        static const char* const kMemoryName[3] = {"", " in system memory", " in video memory"};
        for (int m = 0; m < 3; m++) {
            t.ddsCaps.dwCaps = DDSCAPS_TEXTURE | kMemory[m];
            Surf* tex = nullptr;
            const HRESULT texHr = dd->CreateSurface(&t, &tex, nullptr);
            if (tex) tex->Release();
            fprintf(g_out, "%s%s 0x%08lx", m ? "," : "", kMemoryName[m], texHr);
        }
        static const GUID* const kDevice[2] = {&IID_IDirect3DHALDevice, &IID_IDirect3DRGBDevice};
        for (int sw = 0; sw < 2; sw++) {
            HRESULT surfHr;
            Surf* rt = Target<DD, Surf>(dd, sw ? DDSCAPS_SYSTEMMEMORY : DDSCAPS_VIDEOMEMORY, &pf, &surfHr);
            fprintf(g_out, "; %s target 0x%08lx", sw ? "system-memory" : "video-memory", surfHr);
            if (rt && d3d) fprintf(g_out, ", %s device on it 0x%08lx", sw ? "RGB" : "hardware", TryDevice(d3d, *kDevice[sw], rt));
            if (rt) rt->Release();
        }
        {  // (a texture to draw into: the software device's render target as games made them)
            HRESULT surfHr;
            Surf* rt = Target<DD, Surf>(dd, DDSCAPS_SYSTEMMEMORY, &pf, &surfHr, true);
            fprintf(g_out, "; system-memory texture as a target 0x%08lx", surfHr);
            if (rt && d3d) fprintf(g_out, ", RGB device on it 0x%08lx", TryDevice(d3d, IID_IDirect3DRGBDevice, rt));
            if (rt) rt->Release();
        }
        fprintf(g_out, "\n");
    }

    struct Layout {
        const char* name;
        DWORD flags, bits, m0, alpha;
    };
    static const Layout kLayouts[] = {
        {"1-bit palettized", DDPF_RGB | DDPF_PALETTEINDEXED1, 1, 0, 0},
        {"2-bit palettized", DDPF_RGB | DDPF_PALETTEINDEXED2, 2, 0, 0},
        {"4-bit palettized", DDPF_RGB | DDPF_PALETTEINDEXED4, 4, 0, 0},
        {"4-bit palettized without DDPF_RGB", DDPF_PALETTEINDEXED4, 4, 0, 0},
        {"4-bit palettized with a bit count of 8", DDPF_RGB | DDPF_PALETTEINDEXED4, 8, 0, 0},
        {"8-bit palettized", DDPF_RGB | DDPF_PALETTEINDEXED8, 8, 0, 0},
        {"8-bit luminance", DDPF_LUMINANCE, 8, 0xFF, 0},
        {"8-bit luminance, 4 bits used", DDPF_LUMINANCE, 8, 0x0F, 0},
        {"16-bit luminance", DDPF_LUMINANCE, 16, 0xFFFF, 0},
        {"24-bit luminance", DDPF_LUMINANCE, 24, 0xFFFFFF, 0},
        {"16-bit luminance with alpha", DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 16, 0xFF, 0xFF00},
        {"16-bit luminance, an alpha mask without the alpha flag", DDPF_LUMINANCE, 16, 0xFF, 0xFF00},
        {"8-bit luminance with alpha", DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 8, 0x0F, 0xF0},
        {"32-bit luminance with alpha", DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 32, 0xFFFFFF, 0xFF000000},
        {"8-bit luminance flagged RGB too", DDPF_LUMINANCE | DDPF_RGB, 8, 0xFF, 0},
        {"8-bit alpha only", DDPF_ALPHA, 8, 0, 0},
        {"4-bit alpha only", DDPF_ALPHA, 4, 0, 0},
        {"1-bit alpha only", DDPF_ALPHA, 1, 0, 0},
        {"16-bit alpha only", DDPF_ALPHA, 16, 0, 0},
        {"8-bit alpha only flagged RGB too", DDPF_ALPHA | DDPF_RGB, 8, 0, 0},
        {"16-bit bump map (du, dv)", DDPF_BUMPDUDV, 16, 0xFF, 0},
        {"8-bit RGB with no masks", DDPF_RGB, 8, 0, 0},
        {"8-bit RGB 3-3-2", DDPF_RGB, 8, 0xE0, 0},
        {"8-bit palettized without DDPF_RGB", DDPF_PALETTEINDEXED8, 8, 0, 0},
        // Flags that don't go together: which error each gets.
        {"16-bit RGB 5-6-5 flagged alpha-only too", DDPF_ALPHA | DDPF_RGB, 16, 0xF800, 0},
        {"8-bit alpha only flagged ALPHAPIXELS too", DDPF_ALPHA | DDPF_ALPHAPIXELS, 8, 0, 0xFF},
        {"8-bit alpha only flagged luminance too", DDPF_ALPHA | DDPF_LUMINANCE, 8, 0xFF, 0},
        {"8-bit palettized flagged alpha-only too", DDPF_ALPHA | DDPF_RGB | DDPF_PALETTEINDEXED8, 8, 0, 0},
        {"8-bit alpha only flagged Z buffer too", DDPF_ALPHA | DDPF_ZBUFFER, 8, 0, 0},
        {"8-bit alpha only flagged FourCC too (no code)", DDPF_ALPHA | DDPF_FOURCC, 8, 0, 0},
        {"16-bit luminance flagged bump map too", DDPF_LUMINANCE | DDPF_BUMPDUDV, 16, 0xFF, 0},
        {"16-bit RGB 5-6-5 flagged luminance too", DDPF_LUMINANCE | DDPF_RGB, 16, 0xF800, 0},
        {"16-bit ALPHAPIXELS alone", DDPF_ALPHAPIXELS, 16, 0, 0xFF00},
        {"no flags at all, 16 bits", 0, 16, 0, 0},
    };
    fprintf(g_out, "\nPlain system-memory surfaces, 20 pixels wide (and the same layout as a 16x16 system-memory texture):\n");
    for (const Layout& k : kLayouts) {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        d.dwWidth = 20;
        d.dwHeight = 4;
        d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        d.ddpfPixelFormat.dwFlags = k.flags;
        d.ddpfPixelFormat.dwRGBBitCount = k.bits;
        d.ddpfPixelFormat.dwRBitMask = k.m0;
        // (a bump map's dv; 5-6-5's and 3-3-2's green and blue)
        d.ddpfPixelFormat.dwGBitMask = (k.flags & DDPF_BUMPDUDV) ? 0xFF00 : k.m0 == 0xF800 ? 0x07E0 : k.m0 == 0xE0 ? 0x1C : 0;
        d.ddpfPixelFormat.dwBBitMask = k.m0 == 0xF800 ? 0x001F : k.m0 == 0xE0 ? 0x03 : 0;
        d.ddpfPixelFormat.dwRGBAlphaBitMask = k.alpha;
        Surf* s = nullptr;
        const HRESULT hr = dd->CreateSurface(&d, &s, nullptr);
        {
            DDSURFACEDESC2 t = d;
            t.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY;
            t.dwWidth = t.dwHeight = 16;
            Surf* tex = nullptr;
            const HRESULT texHr = dd->CreateSurface(&t, &tex, nullptr);
            if (tex) tex->Release();
            fprintf(g_out, "  %s: 0x%08lx (texture 0x%08lx)", k.name, hr, texHr);
        }
        if (s) {
            DDSURFACEDESC2 l = {};
            l.dwSize = sizeof(l);
            const HRESULT lock = s->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
            if (SUCCEEDED(lock)) {
                s->Unlock(nullptr);
                fprintf(g_out, ", pitch %ld", l.lPitch);
            } else {
                fprintf(g_out, ", Lock 0x%08lx", lock);
            }
            DDPIXELFORMAT got = {};
            got.dwSize = sizeof(got);
            if (SUCCEEDED(s->GetPixelFormat(&got))) {
                char text[160];
                Describe(got, text, sizeof(text));
                fprintf(g_out, ", reads back as %s (flags 0x%lx)", text, got.dwFlags);
            }
            HDC dc = nullptr;
            const HRESULT dcHr = s->GetDC(&dc);
            if (SUCCEEDED(dcHr)) s->ReleaseDC(dc);
            DDBLTFX fx = {};
            fx.dwSize = sizeof(fx);
            fprintf(g_out, ", GetDC 0x%08lx, colour fill 0x%08lx", dcHr, s->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx));
            // A palettized one again with a palette of its size (index 1: 10, 200, 30).
            const DWORD size = got.dwFlags & DDPF_PALETTEINDEXED1 ? DDPCAPS_1BIT : got.dwFlags & DDPF_PALETTEINDEXED2 ? DDPCAPS_2BIT
                               : got.dwFlags & DDPF_PALETTEINDEXED4 ? DDPCAPS_4BIT
                               : got.dwFlags & DDPF_PALETTEINDEXED8 ? DWORD(DDPCAPS_8BIT | DDPCAPS_ALLOW256) : 0;
            if (size) {
                PALETTEENTRY pe[256] = {};
                pe[1] = {10, 200, 30, 0};
                IDirectDrawPalette* pal = nullptr;
                dd->CreatePalette(size, pe, &pal, nullptr);
                const HRESULT sp = pal ? s->SetPalette(pal) : E_FAIL;
                if (SUCCEEDED(s->Lock(nullptr, &l, DDLOCK_WAIT, nullptr))) {  // index 1 everywhere
                    memset(l.lpSurface, k.bits == 1 ? 0xFF : k.bits == 2 ? 0x55 : k.bits == 4 ? 0x11 : 0x01, size_t(l.lPitch) * 4);
                    s->Unlock(nullptr);
                }
                const HRESULT dc2 = s->GetDC(&dc);
                fprintf(g_out, "; with a palette (SetPalette 0x%08lx): GetDC 0x%08lx", sp, dc2);
                if (SUCCEEDED(dc2)) {
                    fprintf(g_out, ", a pixel of index 1 reads 0x%06lx through it (0x1ec80a: the palette's)", GetPixel(dc, 5, 1));
                    s->ReleaseDC(dc);
                }
                if (pal) pal->Release();
            }
            s->Release();
        }
        fprintf(g_out, "\n");
    }

    // A 4-bit surface: a palette of another size and of its own, copies to its like.
    {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        d.dwWidth = 20;
        d.dwHeight = 4;
        d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        d.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED4;
        d.ddpfPixelFormat.dwRGBBitCount = 4;
        Surf *a = nullptr, *b = nullptr;
        dd->CreateSurface(&d, &a, nullptr);
        dd->CreateSurface(&d, &b, nullptr);
        PALETTEENTRY pe[256] = {};
        IDirectDrawPalette *p8 = nullptr, *p4 = nullptr, *p1 = nullptr;
        dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &p8, nullptr);
        dd->CreatePalette(DDPCAPS_4BIT, pe, &p4, nullptr);
        dd->CreatePalette(DDPCAPS_1BIT, pe, &p1, nullptr);
        fprintf(g_out, "\nA 4-bit palettized surface:\n");
        if (a && b) {
            // (Each call on its own line: in this order.)
            const HRESULT big = p8 ? a->SetPalette(p8) : E_FAIL;
            const HRESULT small = p1 ? a->SetPalette(p1) : E_FAIL;
            const HRESULT own = p4 ? a->SetPalette(p4) : E_FAIL;
            fprintf(g_out, "  SetPalette: a 256-entry palette 0x%08lx, a 2-entry one 0x%08lx, a 16-entry one 0x%08lx\n", big, small, own);
            RECT part = {0, 0, 8, 2};
            const HRESULT whole = b->Blt(nullptr, a, nullptr, DDBLT_WAIT, nullptr);
            const HRESULT fast = b->BltFast(0, 0, a, nullptr, DDBLTFAST_WAIT);
            const HRESULT some = b->Blt(&part, a, &part, DDBLT_WAIT, nullptr);
            fprintf(g_out, "  to another like it: Blt 0x%08lx, BltFast 0x%08lx, Blt of a part 0x%08lx\n", whole, fast, some);
        } else {
            fprintf(g_out, "  not made\n");
        }
        if (p1) p1->Release();
        if (p4) p4->Release();
        if (p8) p8->Release();
        if (b) b->Release();
        if (a) a->Release();
    }

    if (d3d) {
        // SetRenderTarget on an RGB device made on a 16-bit colour surface, to textures of the
        // other formats (system memory, DDSCAPS_3DDEVICE) and to a colour one for comparison.
        struct Next {
            const char* name;
            DWORD flags, bits, m0, m1, alpha;
        };
        static const Next kNext[] = {{"8-bit luminance", DDPF_LUMINANCE, 8, 0xFF, 0, 0},
                                     {"16-bit luminance with alpha", DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 16, 0xFF, 0, 0xFF00},
                                     {"8-bit alpha only", DDPF_ALPHA, 8, 0, 0, 0},
                                     {"16-bit bump map (du, dv)", DDPF_BUMPDUDV, 16, 0xFF, 0xFF00, 0},
                                     {"4-bit palettized", DDPF_RGB | DDPF_PALETTEINDEXED4, 4, 0, 0, 0},
                                     {"8-bit palettized", DDPF_RGB | DDPF_PALETTEINDEXED8, 8, 0, 0, 0},
                                     {"8-bit RGB 3-3-2", DDPF_RGB, 8, 0xE0, 0x1C, 0},
                                     {"16-bit RGB 5-5-5", DDPF_RGB, 16, 0x7C00, 0x03E0, 0}};
        DDPIXELFORMAT first = {};
        first.dwSize = sizeof(first);
        first.dwFlags = DDPF_RGB;
        first.dwRGBBitCount = 16;
        first.dwRBitMask = 0xF800, first.dwGBitMask = 0x07E0, first.dwBBitMask = 0x001F;
        HRESULT firstHr;
        Surf* base = Target<DD, Surf>(dd, DDSCAPS_SYSTEMMEMORY, &first, &firstHr);
        fprintf(g_out, "\nSetRenderTarget on an RGB device (made on a 16-bit 5-6-5 surface, 0x%08lx) to a system-memory texture:\n", firstHr);
        for (const Next& k : kNext) {
            if (!base) break;
            DDPIXELFORMAT pf = {};
            pf.dwSize = sizeof(pf);
            pf.dwFlags = k.flags;
            pf.dwRGBBitCount = k.bits;
            pf.dwRBitMask = k.m0;
            pf.dwGBitMask = k.m1;
            pf.dwBBitMask = k.flags == DDPF_RGB ? (k.bits == 8 ? 0x03 : 0x001F) : 0;
            pf.dwRGBAlphaBitMask = k.alpha;
            HRESULT surfHr;
            Surf* next = Target<DD, Surf>(dd, DDSCAPS_SYSTEMMEMORY, &pf, &surfHr, true);
            fprintf(g_out, "  %s: texture 0x%08lx", k.name, surfHr);
            if (next) {
                char after[64];
                const HRESULT set = Retarget(d3d, base, next, after, sizeof(after));
                fprintf(g_out, ", SetRenderTarget 0x%08lx (the device's target then: %s), a new RGB device on it 0x%08lx", set, after,
                        TryDevice(d3d, IID_IDirect3DRGBDevice, next));
            }
            if (next) next->Release();
            fprintf(g_out, "\n");
        }
        if (base) {  // (a Z buffer with DDSCAPS_3DDEVICE: Windows refuses it after switching to it)
            DDSURFACEDESC2 zd = {};
            zd.dwSize = sizeof(zd);
            zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY;
            zd.dwWidth = zd.dwHeight = 64;
            zd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            zd.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
            zd.ddpfPixelFormat.dwZBufferBitDepth = 16;
            zd.ddpfPixelFormat.dwZBitMask = 0xFFFF;
            Surf* zs = nullptr;
            const HRESULT zHr = dd->CreateSurface(&zd, &zs, nullptr);
            fprintf(g_out, "  a 16-bit Z buffer with DDSCAPS_3DDEVICE: surface 0x%08lx", zHr);
            if (zs) {
                char after[64];
                const HRESULT set = Retarget(d3d, base, zs, after, sizeof(after));
                fprintf(g_out, ", SetRenderTarget 0x%08lx (the device's target then: %s)", set, after);
                zs->Release();
            }
            fprintf(g_out, "\n");
        }
        if (base) base->Release();

        // The software and the hardware device asked to clear a target, to 0x40c02010 (alpha
        // 0x40, red 0xc0, green 0x20, blue 0x10), to 0xc0c0c0c0 (0xc0 cut to 4, 5 or 6 bits is
        // one more than 0xc0 rounded to them) and to 0xffffffff: what the target then holds (the
        // bits no channel has too). The targets that aren't colour formats as well, where a
        // device can be made on one.
        struct Cleared {
            const char* name;
            DWORD flags, bits, m0, m1, m2, alpha;
        };
        static const Cleared kCleared[] = {
            {"RGB 5-6-5", DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F, 0},
            {"RGB 5-5-5", DDPF_RGB, 16, 0x7C00, 0x03E0, 0x001F, 0},
            {"RGB 1-5-5-5 with alpha", DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x7C00, 0x03E0, 0x001F, 0x8000},
            {"RGB 4-4-4-4 with alpha", DDPF_RGB | DDPF_ALPHAPIXELS, 16, 0x0F00, 0x00F0, 0x000F, 0xF000},
            {"RGB 8-8-8, 32 bits", DDPF_RGB, 32, 0xFF0000, 0xFF00, 0xFF, 0},
            {"RGB 8-8-8-8 with alpha", DDPF_RGB | DDPF_ALPHAPIXELS, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000},
            {"16-bit luminance with alpha", DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 16, 0xFF, 0, 0, 0xFF00},
            {"16-bit bump map (du, dv)", DDPF_BUMPDUDV, 16, 0xFF, 0xFF00, 0, 0},
        };
        static const DWORD kColors[3] = {0x40C02010, 0xC0C0C0C0, 0xFFFFFFFF};
        fprintf(g_out, "\nA device clearing a target to 0x40c02010, to 0xc0c0c0c0, then to 0xffffffff (0x1234 or 0x12345678 in it before each):\n");
        for (const Cleared& k : kCleared) {
            DDPIXELFORMAT pf = {};
            pf.dwSize = sizeof(pf);
            pf.dwFlags = k.flags;
            pf.dwRGBBitCount = k.bits;
            pf.dwRBitMask = k.m0;
            pf.dwGBitMask = k.m1;
            pf.dwBBitMask = k.m2;
            pf.dwRGBAlphaBitMask = k.alpha;
            for (int hw = 0; hw < 2; hw++) {
                // A plain surface, or a texture where the format is a texture's alone.
                HRESULT surfHr;
                const DWORD memory = hw ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY;
                Surf* rt = Target<DD, Surf>(dd, memory, &pf, &surfHr);
                fprintf(g_out, "  %s, %s device: %s target 0x%08lx", k.name, hw ? "hardware" : "RGB", hw ? "video-memory" : "system-memory",
                        surfHr);
                if (!rt) {
                    rt = Target<DD, Surf>(dd, memory, &pf, &surfHr, true);
                    fprintf(g_out, ", as a texture 0x%08lx", surfHr);
                }
                if (!rt) {
                    fprintf(g_out, "\n");
                    continue;
                }
                for (DWORD color : kColors) {
                    DDSURFACEDESC2 l = {};
                    l.dwSize = sizeof(l);
                    if (SUCCEEDED(rt->Lock(nullptr, &l, DDLOCK_WAIT, nullptr))) {
                        for (int y = 0; y < 64; y++) {
                            BYTE* row = static_cast<BYTE*>(l.lpSurface) + y * l.lPitch;
                            for (int x = 0; x < 64; x++) {
                                if (k.bits == 16) reinterpret_cast<WORD*>(row)[x] = 0x1234;
                                else reinterpret_cast<DWORD*>(row)[x] = 0x12345678;
                            }
                        }
                        rt->Unlock(nullptr);
                    }
                    const HRESULT cleared = ClearWith(d3d, hw ? IID_IDirect3DHALDevice : IID_IDirect3DRGBDevice, rt, color);
                    fprintf(g_out, ", clear 0x%08lx", cleared);
                    if (SUCCEEDED(rt->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
                        const BYTE* row = static_cast<const BYTE*>(l.lpSurface) + 32 * l.lPitch;
                        if (k.bits == 16) fprintf(g_out, " holds 0x%04x", reinterpret_cast<const WORD*>(row)[32]);
                        else fprintf(g_out, " holds 0x%08lx", reinterpret_cast<const DWORD*>(row)[32]);
                        rt->Unlock(nullptr);
                    }
                }
                rt->Release();
                fprintf(g_out, "\n");
            }
        }
    }
}

// The display in a 256-colour mode: 8-bit layouts as plain system-memory surfaces, and their
// DC before they have a palette.
static void EightBitMode(IDirectDraw4* dd) {
    fprintf(g_out, "\nWith the display at 640x480 in 256 colours:\n");
    HWND hwnd = CreateWindowExA(0, "STATIC", "fmtprobe", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, nullptr, nullptr, nullptr, nullptr);
    HRESULT hr = dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    if (SUCCEEDED(hr)) hr = dd->SetDisplayMode(640, 480, 8, 0, 0);
    if (FAILED(hr)) {
        fprintf(g_out, "  the mode could not be set (0x%08lx)\n", hr);
    } else {
        struct Layout {
            const char* name;
            DWORD flags, bits, m0, m1, m2;  // flags 0: no DDSD_PIXELFORMAT
        };
        static const Layout kLayouts[] = {
            {"no pixel format given", 0, 0, 0, 0, 0},
            {"8-bit palettized", DDPF_RGB | DDPF_PALETTEINDEXED8, 8, 0, 0, 0},
            {"8-bit palettized without DDPF_RGB", DDPF_PALETTEINDEXED8, 8, 0, 0, 0},
            {"8-bit RGB with no masks", DDPF_RGB, 8, 0, 0, 0},
            {"8-bit RGB 3-3-2", DDPF_RGB, 8, 0xE0, 0x1C, 0x03},
            {"4-bit palettized", DDPF_RGB | DDPF_PALETTEINDEXED4, 4, 0, 0, 0},
            {"4-bit palettized without DDPF_RGB", DDPF_PALETTEINDEXED4, 4, 0, 0, 0},
            {"16-bit RGB 5-6-5", DDPF_RGB, 16, 0xF800, 0x07E0, 0x001F},
        };
        for (const Layout& k : kLayouts) {
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | (k.flags ? DDSD_PIXELFORMAT : 0);
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
            d.dwWidth = 20;
            d.dwHeight = 4;
            if (k.flags) {
                d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
                d.ddpfPixelFormat.dwFlags = k.flags;
                d.ddpfPixelFormat.dwRGBBitCount = k.bits;
                d.ddpfPixelFormat.dwRBitMask = k.m0;
                d.ddpfPixelFormat.dwGBitMask = k.m1;
                d.ddpfPixelFormat.dwBBitMask = k.m2;
            }
            IDirectDrawSurface4* s = nullptr;
            hr = dd->CreateSurface(&d, &s, nullptr);
            fprintf(g_out, "  %s: 0x%08lx", k.name, hr);
            if (s) {
                DDPIXELFORMAT got = {};
                got.dwSize = sizeof(got);
                if (SUCCEEDED(s->GetPixelFormat(&got))) {
                    char text[160];
                    Describe(got, text, sizeof(text));
                    fprintf(g_out, ", reads back as %s (flags 0x%lx)", text, got.dwFlags);
                }
                HDC dc = nullptr;
                const HRESULT dcHr = s->GetDC(&dc);
                if (SUCCEEDED(dcHr)) s->ReleaseDC(dc);
                fprintf(g_out, ", GetDC without a palette 0x%08lx", dcHr);
                s->Release();
            }
            fprintf(g_out, "\n");
        }
        dd->RestoreDisplayMode();
    }
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    DestroyWindow(hwnd);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmd, int) {
    // (DirectDrawCreateEx by name: DirectX 6's ddraw.dll doesn't have it.)
    const auto found =
        reinterpret_cast<CreateExFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("ddraw.dll"), "DirectDrawCreateEx")));
    if (!strncmp(cmd, "try ", 4)) return FmtprobeTry(cmd + 4, found);
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* name = strrchr(path, '\\') + 1;
    CharLowerA(name);
    const bool asked6 = strstr(cmd, "dx6") || strstr(name, "dx6");
    strcpy(name, asked6 ? "fmtprobe-dx6.txt" : "fmtprobe.txt");
    if (!strncmp(cmd, "section ", 8)) return FmtprobeSection(cmd + 8, path, found);
    char file[MAX_PATH];
    strcpy(file, path);
    g_out = fopen(path, "w");
    if (!g_out) return 1;
    setvbuf(g_out, nullptr, _IONBF, 0);  // (each line in the file at once: what was asked last, if it ends early)
    const HMODULE ddraw = GetModuleHandleA("ddraw.dll");
    GetModuleFileNameA(ddraw, path, MAX_PATH);
    fprintf(g_out, "ddraw.dll: %s\n", path);
    const CreateExFn createEx = asked6 ? nullptr : found;
    IDirectDraw* dd1 = nullptr;
    HRESULT hr = DirectDrawCreate(nullptr, &dd1, nullptr);
    if (!dd1) {
        fprintf(g_out, "DirectDrawCreate: 0x%08lx\n", hr);
        fclose(g_out);
        return 1;
    }
    dd1->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
    IDirectDraw4* dd4 = nullptr;
    dd1->QueryInterface(IID_IDirectDraw4, reinterpret_cast<void**>(&dd4));
    IDirectDraw7* dd7 = nullptr;
    if (createEx) hr = createEx(nullptr, reinterpret_cast<void**>(&dd7), IID_IDirectDraw7, nullptr);
    if (dd7) dd7->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
    fprintf(g_out, "DirectX: %s\n",
            dd7 ? "7 or later" : createEx ? "7 or later, but no IDirectDraw7 object" : !dd4 ? "5 or earlier (no IDirectDraw4)"
                                        : asked6 ? "6's interfaces (asked for: dx6)" : "6 (no DirectDrawCreateEx)");
    if (createEx && !dd7) fprintf(g_out, "DirectDrawCreateEx: 0x%08lx\n", hr);
    if (dd7) {
        DDDEVICEIDENTIFIER2 id = {};
        if (SUCCEEDED(dd7->GetDeviceIdentifier(&id, 0)))
            fprintf(g_out, "display: %s (%s %u.%u.%u.%u)\n", id.szDescription, id.szDriver, HIWORD(id.liDriverVersion.HighPart),
                    LOWORD(id.liDriverVersion.HighPart), HIWORD(id.liDriverVersion.LowPart), LOWORD(id.liDriverVersion.LowPart));
    } else if (dd4) {
        DDDEVICEIDENTIFIER id = {};
        if (SUCCEEDED(dd4->GetDeviceIdentifier(&id, 0)))
            fprintf(g_out, "display: %s (%s %u.%u.%u.%u)\n", id.szDescription, id.szDriver, HIWORD(id.liDriverVersion.HighPart),
                    LOWORD(id.liDriverVersion.HighPart), HIWORD(id.liDriverVersion.LowPart), LOWORD(id.liDriverVersion.LowPart));
    }
    DDSURFACEDESC mode = {};
    mode.dwSize = sizeof(mode);
    dd1->GetDisplayMode(&mode);
    fprintf(g_out, "desktop: %lux%lu, %lu bits\n", mode.dwWidth, mode.dwHeight, mode.ddpfPixelFormat.dwRGBBitCount);

    IDirect3D7* d3d7 = nullptr;
    if (dd7) {
        hr = dd7->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d7));
        if (d3d7) {
            Lister l = {dd7, d3d7};
            d3d7->EnumDevices(ListDevice7, &l);
        } else {
            fprintf(g_out, "no IDirect3D7 (0x%08lx)\n", hr);
        }
    }

    // Direct3D 3's (DirectX 6) and Direct3D 2's devices: the hardware one on a video-memory
    // target, the software ones on a system-memory one.
    struct Kind {
        const char* name;
        const GUID* guid;
        bool hardware;
    };
    static const Kind kDevices[] = {{"hardware", &IID_IDirect3DHALDevice, true},
                                    {"RGB software", &IID_IDirect3DRGBDevice, false},
                                    {"MMX software", &IID_IDirect3DMMXDevice, false},
                                    {"Ramp software", &IID_IDirect3DRampDevice, false}};
    IDirect3D3* d3d3 = nullptr;
    if (dd4) dd4->QueryInterface(IID_IDirect3D3, reinterpret_cast<void**>(&d3d3));
    for (const Kind& k : kDevices) {
        if (!d3d3) break;
        IDirect3DDevice3* dev = nullptr;
        IDirectDrawSurface4* rt =
            Target<IDirectDraw4, IDirectDrawSurface4>(dd4, k.hardware ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY, nullptr, &hr);
        if (rt) hr = d3d3->CreateDevice(*k.guid, rt, &dev, nullptr);
        fprintf(g_out, "\nDirect3D 3 %s device:\n", k.name);
        if (dev) dev->EnumTextureFormats(FormatLine, nullptr);
        else fprintf(g_out, "    no device (0x%08lx)\n", hr);
        if (dev) dev->Release();
        if (rt) rt->Release();
    }
    IDirect3D2* d3d2 = nullptr;
    dd1->QueryInterface(IID_IDirect3D2, reinterpret_cast<void**>(&d3d2));
    for (const Kind& k : kDevices) {
        if (!d3d2) break;
        IDirectDrawSurface* rt = nullptr;
        IDirect3DDevice2* dev = nullptr;
        DDSURFACEDESC d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | (k.hardware ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
        d.dwWidth = d.dwHeight = 64;
        hr = dd1->CreateSurface(&d, &rt, nullptr);
        if (rt) hr = d3d2->CreateDevice(*k.guid, rt, &dev);
        fprintf(g_out, "\nDirect3D 2 %s device:\n", k.name);
        if (dev) dev->EnumTextureFormats(FormatLine2, nullptr);
        else fprintf(g_out, "    no device (0x%08lx)\n", hr);
        if (dev) dev->Release();
        if (rt) rt->Release();
    }
    if (d3d2) d3d2->Release();

    if (dd7) Formats<IDirectDraw7, IDirectDrawSurface7, IDirect3D7>(dd7, d3d7);
    else if (dd4) Formats<IDirectDraw4, IDirectDrawSurface4, IDirect3D3>(dd4, d3d3);
    else fprintf(g_out, "\n(The other formats need DirectX 6 or later.)\n");
    if (dd4) EightBitMode(dd4);

    if (d3d3) d3d3->Release();
    if (d3d7) d3d7->Release();
    if (dd7) dd7->Release();
    if (dd4) dd4->Release();
    dd1->Release();
    fclose(g_out);
    if (createEx) FmtprobeMore(file);
    return 0;
}
