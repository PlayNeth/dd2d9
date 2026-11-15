// fmtprobe: what a ddraw.dll and its display driver say about texture formats, written to
// fmtprobe.txt beside the program. Run it with Windows' own ddraw.dll (on XP with a card of
// the time, or on a newer Windows) to see what games are shown there, and with dd2d9's to
// compare:
//   - the texture formats each Direct3D 7 device and a Direct3D 2 hardware device list;
//   - for the formats that aren't colours or are palettized below 8 bits: whether a texture
//     of it can be made, and whether a surface of it can be a render target (made in video
//     memory for a hardware device, in system memory for the RGB one).
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <cstdio>
#include <cstring>

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

static HRESULT WINAPI Format7(DDPIXELFORMAT* f, void*) {
    Line(*f);
    return D3DENUMRET_OK;
}

static HRESULT WINAPI Format2(DDSURFACEDESC* d, void*) {
    Line(d->ddpfPixelFormat);
    return D3DENUMRET_OK;
}

// A 64x64 surface with the desktop's format, to make a device on.
static IDirectDrawSurface7* Target(IDirectDraw7* dd, DWORD memory, const DDPIXELFORMAT* pf, HRESULT* hr) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | (pf ? DDSD_PIXELFORMAT : 0);
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | memory;
    d.dwWidth = d.dwHeight = 64;
    if (pf) d.ddpfPixelFormat = *pf;
    IDirectDrawSurface7* s = nullptr;
    *hr = dd->CreateSurface(&d, &s, nullptr);
    return s;
}

struct Lister {
    IDirectDraw7* dd;
    IDirect3D7* d3d;
};

static HRESULT WINAPI Device7(char* description, char* name, D3DDEVICEDESC7* desc, void* ctx) {
    auto* l = static_cast<Lister*>(ctx);
    const bool hardware = (desc->dwDevCaps & D3DDEVCAPS_HWRASTERIZATION) != 0;
    fprintf(g_out, "\nDirect3D 7 device \"%s\" (%s), %s:\n", name, description, hardware ? "hardware" : "software");
    HRESULT hr;
    IDirectDrawSurface7* rt = Target(l->dd, hardware ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY, nullptr, &hr);
    IDirect3DDevice7* dev = nullptr;
    if (rt) hr = l->d3d->CreateDevice(desc->deviceGUID, rt, &dev);
    if (dev) {
        dev->EnumTextureFormats(Format7, nullptr);
        dev->Release();
    } else {
        fprintf(g_out, "    no device (0x%08lx)\n", hr);
    }
    if (rt) rt->Release();
    return D3DENUMRET_OK;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "fmtprobe.txt");
    g_out = fopen(path, "w");
    if (!g_out) return 1;
    GetModuleFileNameA(GetModuleHandleA("ddraw.dll"), path, MAX_PATH);
    fprintf(g_out, "ddraw.dll: %s\n", path);
    IDirectDraw7* dd = nullptr;
    HRESULT hr = DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    if (!dd) {
        fprintf(g_out, "DirectDrawCreateEx: 0x%08lx\n", hr);
        fclose(g_out);
        return 1;
    }
    dd->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
    DDDEVICEIDENTIFIER2 id = {};
    if (SUCCEEDED(dd->GetDeviceIdentifier(&id, 0)))
        fprintf(g_out, "display: %s (%s %u.%u.%u.%u)\n", id.szDescription, id.szDriver, HIWORD(id.liDriverVersion.HighPart),
                LOWORD(id.liDriverVersion.HighPart), HIWORD(id.liDriverVersion.LowPart), LOWORD(id.liDriverVersion.LowPart));
    DDSURFACEDESC2 mode = {};
    mode.dwSize = sizeof(mode);
    dd->GetDisplayMode(&mode);
    fprintf(g_out, "desktop: %lux%lu, %lu bits\n", mode.dwWidth, mode.dwHeight, mode.ddpfPixelFormat.dwRGBBitCount);

    IDirect3D7* d3d = nullptr;
    hr = dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d));
    if (d3d) {
        Lister l = {dd, d3d};
        d3d->EnumDevices(Device7, &l);
    } else {
        fprintf(g_out, "no IDirect3D7 (0x%08lx)\n", hr);
    }

    // Direct3D 2's hardware device, on a DirectDraw object of its time.
    {
        IDirectDraw* dd1 = nullptr;
        IDirect3D2* d3d2 = nullptr;
        IDirectDrawSurface* rt = nullptr;
        IDirect3DDevice2* dev = nullptr;
        hr = DirectDrawCreate(nullptr, &dd1, nullptr);
        if (dd1) {
            dd1->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
            DDSURFACEDESC d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
            d.dwWidth = d.dwHeight = 64;
            hr = dd1->CreateSurface(&d, &rt, nullptr);
            if (SUCCEEDED(hr)) hr = dd1->QueryInterface(IID_IDirect3D2, reinterpret_cast<void**>(&d3d2));
            if (SUCCEEDED(hr)) hr = d3d2->CreateDevice(IID_IDirect3DHALDevice, rt, &dev);
        }
        fprintf(g_out, "\nDirect3D 2 hardware device:\n");
        if (dev) dev->EnumTextureFormats(Format2, nullptr);
        else fprintf(g_out, "    no device (0x%08lx)\n", hr);
        if (dev) dev->Release();
        if (d3d2) d3d2->Release();
        if (rt) rt->Release();
        if (dd1) dd1->Release();
    }

    // Formats that aren't colours, or palettized below 8 bits: as a texture, as a render target.
    struct Kind {
        const char* name;
        DWORD flags, bits, m0, m1, m2, alpha;
    };
    static const Kind kKinds[] = {
        {"8-bit luminance", DDPF_LUMINANCE, 8, 0xFF, 0, 0, 0},
        {"16-bit luminance with alpha", DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 16, 0xFF, 0, 0, 0xFF00},
        {"8-bit luminance with alpha", DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 8, 0x0F, 0, 0, 0xF0},
        {"8-bit alpha only", DDPF_ALPHA, 8, 0, 0, 0, 0},
        {"16-bit bump map (du, dv)", DDPF_BUMPDUDV, 16, 0xFF, 0xFF00, 0, 0},
        {"4-bit palettized", DDPF_RGB | DDPF_PALETTEINDEXED4, 4, 0, 0, 0, 0},
        {"1-bit palettized", DDPF_RGB | DDPF_PALETTEINDEXED1, 1, 0, 0, 0, 0},
    };
    fprintf(g_out, "\nOther formats (0x00000000 is success):\n");
    for (const Kind& k : kKinds) {
        DDPIXELFORMAT pf = {};
        pf.dwSize = sizeof(pf);
        pf.dwFlags = k.flags;
        pf.dwRGBBitCount = k.bits;
        pf.dwRBitMask = k.m0;
        pf.dwGBitMask = k.m1;
        pf.dwBBitMask = k.m2;
        pf.dwRGBAlphaBitMask = k.alpha;
        DDSURFACEDESC2 t = {};
        t.dwSize = sizeof(t);
        t.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        t.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
        t.dwWidth = t.dwHeight = 64;
        t.ddpfPixelFormat = pf;
        IDirectDrawSurface7* tex = nullptr;
        const HRESULT texHr = dd->CreateSurface(&t, &tex, nullptr);
        if (tex) tex->Release();
        fprintf(g_out, "  %s: texture 0x%08lx", k.name, texHr);
        static const GUID* const kDevice[2] = {&IID_IDirect3DHALDevice, &IID_IDirect3DRGBDevice};
        for (int sw = 0; sw < 2; sw++) {
            HRESULT surfHr, devHr = E_FAIL;
            IDirectDrawSurface7* rt = Target(dd, sw ? DDSCAPS_SYSTEMMEMORY : DDSCAPS_VIDEOMEMORY, &pf, &surfHr);
            IDirect3DDevice7* dev = nullptr;
            if (rt && d3d) devHr = d3d->CreateDevice(*kDevice[sw], rt, &dev);
            fprintf(g_out, "; %s target 0x%08lx", sw ? "system-memory" : "video-memory", surfHr);
            if (rt) fprintf(g_out, ", %s device on it 0x%08lx", sw ? "RGB" : "hardware", devHr);
            if (dev) dev->Release();
            if (rt) rt->Release();
        }
        fprintf(g_out, "\n");
    }
    if (d3d) d3d->Release();
    dd->Release();
    fclose(g_out);
    return 0;
}
