// fmtprobe, second part: what else a ddraw.dll answers that dd2d9's specs (re/spec, their
// "Not determined" lists) could not read from Windows XP's code. Only with DirectX 7's
// interfaces present; the older ones are asked through them where the question is theirs.
//   - the registry keys and files Direct3D's device lists depend on;
//   - DirectDrawEnumerate's entries, and each Direct3D version's device list;
//   - Z-buffer formats: listed by the devices, and made by CreateSurface;
//   - DirectDraw's and the devices' caps, as hex words;
//   - reference counts around Direct3D objects;
//   - EnumSurfaces after a surface was asked for other interface versions;
//   - blits: stretch sampling, overlapping copies, clipped stretches, odd flags;
//   - texture-management calls on surfaces that aren't managed textures;
//   - palettes: the entries and flags as stored;
//   - clears through Direct3D 1, 2 and 3's viewports (the background material's colour);
//   - exclusive mode: TestCooperativeLevel, lost surfaces and the window's messages after each
//     step; attachment order; a palette on an 8-bit primary; gamma ramps;
//   - the devices' default render, light and texture-stage states;
//   - what the first runs on Windows XP left open: emulation-only objects, more stretch
//     ratios, the display mode TestCooperativeLevel expects, EnumSurfaces' order, SetLOD;
//   - calls with NULL or invalid arguments, each in a process of its own ("fmtprobe try N":
//     its exit code is the result, or the exception's code);
//   - what 1.9.0 asked: colour-key ranges in blits and overlays, overlay bob, interleaving,
//     field flips, alpha and effects, colour controls, the video-port container, Z buffers
//     whose depth and stencil masks share bits, the device window's mouse messages,
//     DDSCAPS_OWNDC, activation and focus loss (the system settings, the flip chain, Alt+Tab's
//     key-up), YUV fills and planar surfaces, the gamma calibrator (fmtcal.dll), and the
//     primary copied to the clipboard on PrintScreen (EnablePrintScreen), and GDI's palette
//     of an 8-bit screen beside DirectDraw's palettes, and a palette's entries read back
//     (their peFlags).
// Each section runs in a process of its own too ("fmtprobe section N", appending to the file),
// so that one a ddraw.dll crashes or hangs in costs only itself.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "fmtprobe.h"

static FILE* g_out;
static const char* g_path;  // the file g_out writes to
static CreateExFn g_createEx;
#define P(...) fprintf(g_out, __VA_ARGS__)

// ---------------------------------------------------------------- helpers

static void Hex(const char* what, const void* p, size_t bytes) {
    P("  %s, %u bytes:", what, unsigned(bytes));
    const DWORD* w = static_cast<const DWORD*>(p);
    for (size_t i = 0; i < bytes / 4; i++) P("%s%08lx", i % 8 ? " " : "\n    ", w[i]);
    P("\n");
}

static void GuidText(const GUID* g, char* out, size_t n) {
    if (!g) snprintf(out, n, "(null)");
    else
        snprintf(out, n, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1],
                 g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

template <class T> static ULONG Refs(T* p) {
    if (!p) return 0;
    p->AddRef();
    return p->Release();
}

static IDirectDraw7* NewDD7(GUID* which = nullptr) {
    IDirectDraw7* dd = nullptr;
    if (g_createEx) g_createEx(which, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    return dd;
}

struct Rgb {
    const char* name;
    DWORD bits, r, g, b, a;
};
static const Rgb kX888 = {"8-8-8 in 32 bits", 32, 0xFF0000, 0xFF00, 0xFF, 0};
static const Rgb k565 = {"5-6-5", 16, 0xF800, 0x07E0, 0x001F, 0};

static void SetFormat(DDPIXELFORMAT* pf, const Rgb& f) {
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(*pf);
    pf->dwFlags = DDPF_RGB | (f.a ? DDPF_ALPHAPIXELS : 0);
    pf->dwRGBBitCount = f.bits;
    pf->dwRBitMask = f.r;
    pf->dwGBitMask = f.g;
    pf->dwBBitMask = f.b;
    pf->dwRGBAlphaBitMask = f.a;
}

// A surface of `caps` (the display's format without `f`).
static IDirectDrawSurface7* Make(IDirectDraw7* dd, DWORD caps, int w, int h, const Rgb* f = nullptr, HRESULT* hrOut = nullptr,
                                 DWORD caps2 = 0) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | (f ? DDSD_PIXELFORMAT : 0);
    d.ddsCaps.dwCaps = caps;
    d.ddsCaps.dwCaps2 = caps2;
    d.dwWidth = DWORD(w);
    d.dwHeight = DWORD(h);
    if (f) SetFormat(&d.ddpfPixelFormat, *f);
    IDirectDrawSurface7* s = nullptr;
    const HRESULT hr = dd->CreateSurface(&d, &s, nullptr);
    if (hrOut) *hrOut = hr;
    return s;
}

// Up to 64 pixel values; Count(n): 1 to n; Zero(n): n zeros.
struct Pixels {
    DWORD v[64];
    int n;
};
static Pixels Count(int n) {
    Pixels p = {};
    p.n = n;
    for (int i = 0; i < n; i++) p.v[i] = DWORD(i + 1);
    return p;
}
static Pixels Zero(int n) {
    Pixels p = {};
    p.n = n;
    return p;
}
// A w-wide 32-bit surface's pixels set to `p`, and read back as text (into one buffer: one
// Row to a line).
static void Put(IDirectDrawSurface7* s, const Pixels& p, int w) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    if (FAILED(s->Lock(nullptr, &d, DDLOCK_WAIT, nullptr))) return;
    for (int i = 0; i < p.n; i++) reinterpret_cast<DWORD*>(static_cast<BYTE*>(d.lpSurface) + (i / w) * d.lPitch)[i % w] = p.v[i];
    s->Unlock(nullptr);
}
static const char* Row(IDirectDrawSurface7* s, int w, int h) {
    static char out[512];
    size_t used = 0;
    out[0] = 0;
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    if (FAILED(s->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) return "(no lock)";
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w && used < sizeof(out) - 16; x++) {
            const DWORD v = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(d.lpSurface) + y * d.lPitch)[x] & 0xFFFFFF;
            if (v == 0) used += size_t(snprintf(out + used, sizeof(out) - used, " ."));
            else if (v < 100) used += size_t(snprintf(out + used, sizeof(out) - used, " %lu", v));
            else used += size_t(snprintf(out + used, sizeof(out) - used, " %lx", v));
        }
    s->Unlock(nullptr);
    return out;
}

// ---------------------------------------------------------------- registry and files

static void RegistryAndFiles() {
    P("\nRegistry and files:\n");
    static const char* const kKeys[] = {"Software\\Microsoft\\Direct3D", "Software\\Microsoft\\Direct3D\\Drivers",
                                        "Software\\Microsoft\\Direct3D\\Drivers\\Ramp Emulation",
                                        "Software\\Microsoft\\Direct3D\\Drivers\\Direct3D HAL",
                                        "Software\\Microsoft\\Direct3D\\DX6TextureEnumInclusionList",
                                        "Software\\Microsoft\\Direct3D\\DX6TextureEnumExclusionList", "Software\\Microsoft\\DirectDraw",
                                        "Software\\Microsoft\\DirectDraw\\Compatibility"};
    for (const char* key : kKeys) {
        HKEY k = nullptr;
        const LONG r = RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k);
        P("  HKLM\\%s: %s\n", key, r == ERROR_SUCCESS ? "there" : "not there");
        if (r != ERROR_SUCCESS) continue;
        for (DWORD i = 0; i < 64; i++) {
            char name[128];
            DWORD n = sizeof(name);
            if (RegEnumKeyExA(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            P("    key %s\n", name);
        }
        for (DWORD i = 0; i < 64; i++) {
            char name[128];
            BYTE data[256];
            DWORD n = sizeof(name), size = sizeof(data), type = 0;
            if (RegEnumValueA(k, i, name, &n, nullptr, &type, data, &size) != ERROR_SUCCESS) break;
            P("    value %s (type %lu, %lu bytes):", name, type, size);
            if (type == REG_SZ) P(" \"%.*s\"", int(size), reinterpret_cast<const char*>(data));
            else
                for (DWORD b = 0; b < size && b < 48; b++) P(" %02x", data[b]);
            P("\n");
        }
        RegCloseKey(k);
    }
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    static const char* const kFiles[] = {"ddraw.dll", "d3dim.dll", "d3dim700.dll", "d3dref.dll", "d3drm.dll", "d3dramp.dll",
                                         "d3dpmesh.dll", "ddrawex.dll", "d3d8.dll", "d3d9.dll"};
    for (const char* f : kFiles) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", sys, f);
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &a)) P("  %s: %lu bytes\n", f, a.nFileSizeLow);
        else P("  %s: not there\n", f);
    }
}

// ---------------------------------------------------------------- enumeration

static int g_enumCalls;
static BOOL WINAPI DriverCb(GUID* guid, char* description, char* name, void* stop) {
    char g[64];
    GuidText(guid, g, sizeof(g));
    g_enumCalls++;
    if (!stop) P("    %s, \"%s\", \"%s\"\n", g, description, name);
    return stop ? FALSE : TRUE;
}
static BOOL WINAPI DriverCbEx(GUID* guid, char* description, char* name, void* stop, HMONITOR monitor) {
    char g[64];
    GuidText(guid, g, sizeof(g));
    g_enumCalls++;
    if (!stop) P("    %s, \"%s\", \"%s\", monitor %s\n", g, description, name, monitor ? "given" : "null");
    return stop ? FALSE : TRUE;
}

static HRESULT CALLBACK DeviceCb(GUID* guid, char* description, char* name, D3DDEVICEDESC* hw, D3DDEVICEDESC* hel, void*) {
    char g[64];
    GuidText(guid, g, sizeof(g));
    P("    \"%s\" (%s) %s\n      hardware desc: size %lu, flags 0x%lx, colour model %lu, render depths 0x%lx, Z depths 0x%lx\n"
      "      emulation desc: size %lu, flags 0x%lx, colour model %lu, render depths 0x%lx, Z depths 0x%lx\n",
      name, description, g, hw->dwSize, hw->dwFlags, DWORD(hw->dcmColorModel), hw->dwDeviceRenderBitDepth, hw->dwDeviceZBufferBitDepth,
      hel->dwSize, hel->dwFlags, DWORD(hel->dcmColorModel), hel->dwDeviceRenderBitDepth, hel->dwDeviceZBufferBitDepth);
    return D3DENUMRET_OK;
}
static HRESULT CALLBACK DeviceCb7(char* description, char* name, D3DDEVICEDESC7* d, void*) {
    char g[64];
    GuidText(&d->deviceGUID, g, sizeof(g));
    P("    \"%s\" (%s) %s\n      dev caps 0x%lx, render depths 0x%lx, Z depths 0x%lx\n", name, description, g, d->dwDevCaps,
      d->dwDeviceRenderBitDepth, d->dwDeviceZBufferBitDepth);
    return D3DENUMRET_OK;
}
static HRESULT CALLBACK ZCb(DDPIXELFORMAT* f, void*) {
    P("    flags 0x%lx, %lu bits, Z mask 0x%lx, stencil %lu bits (mask 0x%lx)\n", f->dwFlags, f->dwZBufferBitDepth, f->dwZBitMask,
      (f->dwFlags & DDPF_STENCILBUFFER) ? f->dwStencilBitDepth : 0, f->dwStencilBitMask);
    return D3DENUMRET_OK;
}

static void Enumeration(IDirectDraw7* dd7, IDirectDraw* dd1) {
    P("\nDirectDrawEnumerateA:\n");
    HRESULT hr = DirectDrawEnumerateA(DriverCb, nullptr);
    P("  result 0x%08lx\n", hr);
    g_enumCalls = 0;
    hr = DirectDrawEnumerateA(DriverCb, &g_enumCalls);
    P("  with a callback that returns 0: 0x%08lx, called %d time(s)\n", hr, g_enumCalls);
    using EnumEx = HRESULT(WINAPI*)(LPDDENUMCALLBACKEXA, void*, DWORD);
    const auto enumEx = reinterpret_cast<EnumEx>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("ddraw.dll"), "DirectDrawEnumerateExA")));
    if (enumEx) {
        P("DirectDrawEnumerateExA, flags 0:\n");
        P("  result 0x%08lx\n", enumEx(DriverCbEx, nullptr, 0));
        P("DirectDrawEnumerateExA, attached secondary and detached and non-display devices:\n");
        P("  result 0x%08lx\n",
          enumEx(DriverCbEx, nullptr, DDENUM_ATTACHEDSECONDARYDEVICES | DDENUM_DETACHEDSECONDARYDEVICES | DDENUM_NONDISPLAYDEVICES));
    }

    IDirect3D* d3d1 = nullptr;
    IDirect3D2* d3d2 = nullptr;
    IDirect3D3* d3d3 = nullptr;
    IDirect3D7* d3d7 = nullptr;
    dd1->QueryInterface(IID_IDirect3D, reinterpret_cast<void**>(&d3d1));
    dd1->QueryInterface(IID_IDirect3D2, reinterpret_cast<void**>(&d3d2));
    dd1->QueryInterface(IID_IDirect3D3, reinterpret_cast<void**>(&d3d3));
    dd7->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d7));
    P("\nIDirect3D::EnumDevices:\n");
    if (d3d1) P("  result 0x%08lx\n", d3d1->EnumDevices(DeviceCb, nullptr));
    P("IDirect3D2::EnumDevices:\n");
    if (d3d2) P("  result 0x%08lx\n", d3d2->EnumDevices(DeviceCb, nullptr));
    P("IDirect3D3::EnumDevices:\n");
    if (d3d3) P("  result 0x%08lx\n", d3d3->EnumDevices(DeviceCb, nullptr));
    P("IDirect3D7::EnumDevices:\n");
    if (d3d7) P("  result 0x%08lx\n", d3d7->EnumDevices(DeviceCb7, nullptr));
    // Other interface versions from each object: which a DirectDraw object of each kind gives.
    {
        void* p = nullptr;
        const HRESULT a = dd7->QueryInterface(IID_IDirect3D3, &p);
        if (SUCCEEDED(a)) static_cast<IUnknown*>(p)->Release();
        p = nullptr;
        const HRESULT b = dd1->QueryInterface(IID_IDirect3D7, &p);
        if (SUCCEEDED(b)) static_cast<IUnknown*>(p)->Release();
        P("IDirect3D3 from a DirectDrawCreateEx object: 0x%08lx; IDirect3D7 from a DirectDrawCreate object: 0x%08lx\n", a, b);
    }

    struct Dev {
        const char* name;
        const GUID* guid;
    };
    static const Dev kDevs[] = {{"RGB", &IID_IDirect3DRGBDevice},     {"HAL", &IID_IDirect3DHALDevice},
                                {"T&L HAL", &IID_IDirect3DTnLHalDevice}, {"MMX", &IID_IDirect3DMMXDevice},
                                {"Ramp", &IID_IDirect3DRampDevice},   {"Reference", &IID_IDirect3DRefDevice},
                                {"Null", &IID_IDirect3DNullDevice}};
    P("\nZ-buffer formats (EnumZBufferFormats):\n");
    for (const Dev& k : kDevs) {
        if (d3d7) {
            P("  Direct3D 7, %s:\n", k.name);
            P("    result 0x%08lx\n", d3d7->EnumZBufferFormats(*k.guid, ZCb, nullptr));
        }
        if (d3d3) {
            P("  Direct3D 3, %s:\n", k.name);
            P("    result 0x%08lx\n", d3d3->EnumZBufferFormats(*k.guid, ZCb, nullptr));
        }
    }
    P("\nZ buffers through CreateSurface (64x64), in system memory and in video memory:\n");
    struct Z {
        const char* name;
        DWORD flags, bits, zmask, stencilBits, stencilMask;
    };
    static const Z kZ[] = {{"8 bits", DDPF_ZBUFFER, 8, 0xFF, 0, 0},
                           {"15 bits and 1 of stencil", DDPF_ZBUFFER | DDPF_STENCILBUFFER, 16, 0xFFFE, 1, 0x0001},
                           {"16 bits", DDPF_ZBUFFER, 16, 0xFFFF, 0, 0},
                           {"24 bits", DDPF_ZBUFFER, 24, 0xFFFFFF, 0, 0},
                           {"24 bits in 32", DDPF_ZBUFFER, 32, 0xFFFFFF00, 0, 0},
                           {"24 bits in 32, low", DDPF_ZBUFFER, 32, 0x00FFFFFF, 0, 0},
                           {"24 bits and 8 of stencil", DDPF_ZBUFFER | DDPF_STENCILBUFFER, 32, 0xFFFFFF00, 8, 0xFF},
                           {"24 bits and 8 of stencil, stencil high", DDPF_ZBUFFER | DDPF_STENCILBUFFER, 32, 0x00FFFFFF, 8, 0xFF000000},
                           {"32 bits", DDPF_ZBUFFER, 32, 0xFFFFFFFF, 0, 0},
                           {"16 bits, no mask given", DDPF_ZBUFFER, 16, 0, 0, 0},
                           {"12 bits", DDPF_ZBUFFER, 12, 0xFFF, 0, 0}};
    for (const Z& k : kZ) {
        P("  %s:", k.name);
        for (DWORD memory : {DWORD(DDSCAPS_SYSTEMMEMORY), DWORD(DDSCAPS_VIDEOMEMORY)}) {
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            d.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | memory;
            d.dwWidth = d.dwHeight = 64;
            d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            d.ddpfPixelFormat.dwFlags = k.flags;
            d.ddpfPixelFormat.dwZBufferBitDepth = k.bits;
            d.ddpfPixelFormat.dwZBitMask = k.zmask;
            d.ddpfPixelFormat.dwStencilBitDepth = k.stencilBits;
            d.ddpfPixelFormat.dwStencilBitMask = k.stencilMask;
            IDirectDrawSurface7* s = nullptr;
            const HRESULT hr2 = dd7->CreateSurface(&d, &s, nullptr);
            P(" %s 0x%08lx", memory == DDSCAPS_SYSTEMMEMORY ? "system" : "; video", hr2);
            if (s) {
                DDSURFACEDESC2 got = {};
                got.dwSize = sizeof(got);
                s->GetSurfaceDesc(&got);
                P(" (pitch %ld, reads back flags 0x%lx, %lu bits, Z mask 0x%lx, stencil mask 0x%lx)", got.lPitch,
                  got.ddpfPixelFormat.dwFlags, got.ddpfPixelFormat.dwZBufferBitDepth, got.ddpfPixelFormat.dwZBitMask,
                  got.ddpfPixelFormat.dwStencilBitMask);
                s->Release();
            }
        }
        P("\n");
    }
    if (d3d7) d3d7->Release();
    if (d3d3) d3d3->Release();
    if (d3d2) d3d2->Release();
    if (d3d1) d3d1->Release();
}

// ---------------------------------------------------------------- caps

static void Caps(IDirectDraw7* dd7) {
    P("\nDirectDraw caps (GetCaps: the driver's, then the emulation's):\n");
    DDCAPS drv = {}, hel = {};
    drv.dwSize = hel.dwSize = sizeof(DDCAPS);
    HRESULT hr = dd7->GetCaps(&drv, &hel);
    P("  result 0x%08lx\n", hr);
    Hex("driver", &drv, sizeof(drv));
    Hex("emulation", &hel, sizeof(hel));
    IDirectDraw7* emu = NewDD7(reinterpret_cast<GUID*>(DDCREATE_EMULATIONONLY));
    P("An emulation-only object (DDCREATE_EMULATIONONLY): %s\n", emu ? "made" : "not made");
    if (emu) {
        emu->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
        DDCAPS d2 = {}, h2 = {};
        d2.dwSize = h2.dwSize = sizeof(DDCAPS);
        hr = emu->GetCaps(&d2, &h2);
        P("  result 0x%08lx\n", hr);
        Hex("driver", &d2, sizeof(d2));
        Hex("emulation", &h2, sizeof(h2));
        IDirect3D7* d3d = nullptr;
        hr = emu->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d));
        P("  IDirect3D7 from it: 0x%08lx\n", hr);
        if (d3d) {
            P("  its devices:\n");
            d3d->EnumDevices(DeviceCb7, nullptr);
            d3d->Release();
        }
        HRESULT vh = E_FAIL;
        IDirectDrawSurface7* v = Make(emu, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, 16, 16, nullptr, &vh);
        P("  a video-memory surface on it: 0x%08lx\n", vh);
        if (v) v->Release();
        emu->Release();
    }

    IDirect3D7* d3d7 = nullptr;
    dd7->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d7));
    P("\nDevice caps (GetCaps):\n");
    for (int hard = 0; hard < 2 && d3d7; hard++) {
        IDirectDrawSurface7* rt =
            Make(dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | (hard ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY), 64, 64);
        IDirect3DDevice7* dev = nullptr;
        if (rt) d3d7->CreateDevice(hard ? IID_IDirect3DHALDevice : IID_IDirect3DRGBDevice, rt, &dev);
        if (dev) {
            D3DDEVICEDESC7 d = {};
            hr = dev->GetCaps(&d);
            P("  Direct3D 7 %s device: 0x%08lx\n", hard ? "hardware" : "RGB", hr);
            Hex("desc", &d, sizeof(d));
            dev->Release();
        }
        if (rt) rt->Release();
    }
    if (d3d7) d3d7->Release();
    IDirectDraw* dd1 = nullptr;
    IDirectDraw4* dd4 = nullptr;
    IDirect3D3* d3d3 = nullptr;
    DirectDrawCreate(nullptr, &dd1, nullptr);
    if (dd1) {
        dd1->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
        dd1->QueryInterface(IID_IDirectDraw4, reinterpret_cast<void**>(&dd4));
        dd1->QueryInterface(IID_IDirect3D3, reinterpret_cast<void**>(&d3d3));
    }
    for (int hard = 0; hard < 2 && dd4 && d3d3; hard++) {
        DDSURFACEDESC2 sd = {};
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | (hard ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
        sd.dwWidth = sd.dwHeight = 64;
        IDirectDrawSurface4* rt = nullptr;
        IDirect3DDevice3* dev = nullptr;
        if (SUCCEEDED(dd4->CreateSurface(&sd, &rt, nullptr)))
            d3d3->CreateDevice(hard ? IID_IDirect3DHALDevice : IID_IDirect3DRGBDevice, rt, &dev, nullptr);
        if (dev) {
            D3DDEVICEDESC hw = {}, sw = {};
            hw.dwSize = sw.dwSize = sizeof(D3DDEVICEDESC);
            hr = dev->GetCaps(&hw, &sw);
            P("  Direct3D 3 %s device: 0x%08lx (asked with size %u; sizes then %lu and %lu)\n", hard ? "hardware" : "RGB", hr,
              unsigned(sizeof(D3DDEVICEDESC)), hw.dwSize, sw.dwSize);
            Hex("hardware desc", &hw, sizeof(hw));
            Hex("emulation desc", &sw, sizeof(sw));
            dev->Release();
        }
        if (rt) rt->Release();
    }
    if (d3d3) d3d3->Release();
    if (dd4) dd4->Release();
    if (dd1) dd1->Release();
}

// ---------------------------------------------------------------- reference counts

static void References() {
    P("\nReference counts (read with AddRef and Release):\n");
    IDirectDraw7* dd = NewDD7();
    if (!dd) return;
    dd->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
    P("  IDirectDraw7 new: %lu\n", Refs(dd));
    IDirect3D7* d3d = nullptr;
    dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d));
    P("  after QueryInterface for IDirect3D7: DirectDraw %lu, Direct3D %lu\n", Refs(dd), Refs(d3d));
    IDirectDrawSurface7* rt = Make(dd, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY, 64, 64);
    HRESULT zh = E_FAIL;
    IDirectDrawSurface7* z = nullptr;
    {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        d.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_SYSTEMMEMORY;
        d.dwWidth = d.dwHeight = 64;
        d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        d.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
        d.ddpfPixelFormat.dwZBufferBitDepth = 16;
        d.ddpfPixelFormat.dwZBitMask = 0xFFFF;
        zh = dd->CreateSurface(&d, &z, nullptr);
    }
    const HRESULT ah = rt && z ? rt->AddAttachedSurface(z) : E_FAIL;
    P("  with a target and a Z buffer (0x%08lx) attached to it (0x%08lx): DirectDraw %lu, target %lu, Z buffer %lu\n", zh, ah, Refs(dd),
      Refs(rt), Refs(z));
    IDirect3DDevice7* dev = nullptr;
    const HRESULT dh = d3d && rt ? d3d->CreateDevice(IID_IDirect3DRGBDevice, rt, &dev) : E_FAIL;
    P("  after CreateDevice (RGB, 0x%08lx): DirectDraw %lu, Direct3D %lu, target %lu, Z buffer %lu, device %lu\n", dh, Refs(dd), Refs(d3d),
      Refs(rt), Refs(z), Refs(dev));
    IDirect3DVertexBuffer7* vb = nullptr;
    D3DVERTEXBUFFERDESC vd = {};
    vd.dwSize = sizeof(vd);
    vd.dwCaps = D3DVBCAPS_SYSTEMMEMORY;
    vd.dwFVF = D3DFVF_XYZ | D3DFVF_DIFFUSE;
    vd.dwNumVertices = 16;
    const HRESULT vh = d3d ? d3d->CreateVertexBuffer(&vd, &vb, 0) : E_FAIL;
    P("  after CreateVertexBuffer (0x%08lx): DirectDraw %lu, Direct3D %lu, vertex buffer %lu\n", vh, Refs(dd), Refs(d3d), Refs(vb));
    IDirectDrawSurface7* tex = Make(dd, DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY, 16, 16);
    if (dev && tex) {
        dev->SetTexture(0, tex);
        P("  a texture set in stage 0: texture %lu", Refs(tex));
        dev->SetTexture(0, nullptr);
        P(", unset: %lu\n", Refs(tex));
    }
    if (dev && rt) {
        IDirectDrawSurface7* other = Make(dd, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY, 64, 64);
        if (other) {
            const HRESULT sh = dev->SetRenderTarget(other, 0);
            P("  after SetRenderTarget to another surface (0x%08lx): old target %lu, new %lu, Z buffer %lu\n", sh, Refs(rt), Refs(other),
              Refs(z));
            dev->SetRenderTarget(rt, 0);
            P("  and back: old target %lu, the other %lu\n", Refs(rt), Refs(other));
            other->Release();
        }
    }
    // (Each count read after the release it follows: the arguments of one call aren't evaluated in order.)
    if (vb) {
        const ULONG left = vb->Release();
        P("  vertex buffer released: %lu left; DirectDraw then %lu\n", left, Refs(dd));
    }
    if (dev) {
        const ULONG left = dev->Release();
        P("  device released: %lu left; DirectDraw then %lu, Direct3D %lu, target %lu, Z buffer %lu\n", left, Refs(dd), Refs(d3d), Refs(rt),
          Refs(z));
    }
    if (tex) tex->Release();
    if (z) z->Release();
    if (rt) rt->Release();
    P("  texture, Z buffer and target released: DirectDraw then %lu\n", Refs(dd));
    if (d3d) {
        const ULONG left = d3d->Release();
        P("  Direct3D released: %lu left; DirectDraw then %lu\n", left, Refs(dd));
    }
    P("  DirectDraw released: %lu left\n", dd->Release());

    // Direct3D 3 on a DirectDrawCreate object.
    IDirectDraw* dd1 = nullptr;
    DirectDrawCreate(nullptr, &dd1, nullptr);
    if (!dd1) return;
    dd1->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
    IDirectDraw4* dd4 = nullptr;
    IDirect3D3* d3d3 = nullptr;
    const ULONG fresh = Refs(dd1);
    dd1->QueryInterface(IID_IDirectDraw4, reinterpret_cast<void**>(&dd4));
    P("  IDirectDraw new: %lu; with IDirectDraw4: v1 %lu, v4 %lu\n", fresh, Refs(dd1), Refs(dd4));
    dd1->QueryInterface(IID_IDirect3D3, reinterpret_cast<void**>(&d3d3));
    P("  after QueryInterface for IDirect3D3: v1 %lu, v4 %lu, Direct3D %lu\n", Refs(dd1), Refs(dd4), Refs(d3d3));
    IDirectDrawSurface4* rt4 = nullptr;
    IDirect3DDevice3* dev3 = nullptr;
    if (dd4 && d3d3) {
        DDSURFACEDESC2 sd = {};
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY;
        sd.dwWidth = sd.dwHeight = 64;
        dd4->CreateSurface(&sd, &rt4, nullptr);
        P("  with a target: v1 %lu, v4 %lu, target %lu\n", Refs(dd1), Refs(dd4), Refs(rt4));
        if (rt4) d3d3->CreateDevice(IID_IDirect3DRGBDevice, rt4, &dev3, nullptr);
        P("  after CreateDevice (RGB): v1 %lu, v4 %lu, Direct3D %lu, target %lu, device %lu\n", Refs(dd1), Refs(dd4), Refs(d3d3),
          Refs(rt4), Refs(dev3));
        IDirect3DViewport3* vp = nullptr;
        d3d3->CreateViewport(&vp, nullptr);
        P("  with a viewport: Direct3D %lu, viewport %lu", Refs(d3d3), Refs(vp));
        if (vp && dev3) {
            dev3->AddViewport(vp);
            P("; added to the device: viewport %lu, device %lu", Refs(vp), Refs(dev3));
            dev3->SetCurrentViewport(vp);
            P("; made current: viewport %lu", Refs(vp));
        }
        P("\n");
        if (dev3) {
            const ULONG left = dev3->Release();
            P("  device released: %lu left; then v1 %lu, v4 %lu, Direct3D %lu, target %lu, viewport %lu\n", left, Refs(dd1), Refs(dd4),
              Refs(d3d3), Refs(rt4), Refs(vp));
        }
        if (vp) vp->Release();
    }
    if (rt4) rt4->Release();
    if (d3d3) d3d3->Release();
    if (dd4) dd4->Release();
    P("  DirectDraw released: %lu left\n", dd1->Release());
}

// ---------------------------------------------------------------- EnumSurfaces after QueryInterface

static HRESULT WINAPI CountCb(IDirectDrawSurface* s, DDSURFACEDESC*, void* n) {
    ++*static_cast<int*>(n);
    if (s) s->Release();
    return DDENUMRET_OK;
}

static void EnumAfterQuery() {
    P("\nEnumSurfaces (DOESEXIST | ALL) on a DirectDrawCreate object:\n");
    IDirectDraw* dd1 = nullptr;
    DirectDrawCreate(nullptr, &dd1, nullptr);
    if (!dd1) return;
    dd1->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
    IDirectDraw2* dd2 = nullptr;
    dd1->QueryInterface(IID_IDirectDraw2, reinterpret_cast<void**>(&dd2));
    auto count = [&](const char* when) {
        int a = 0, b = 0;
        const HRESULT h1 = dd1->EnumSurfaces(DDENUMSURFACES_DOESEXIST | DDENUMSURFACES_ALL, nullptr, &a, CountCb);
        const HRESULT h2 = dd2 ? dd2->EnumSurfaces(DDENUMSURFACES_DOESEXIST | DDENUMSURFACES_ALL, nullptr, &b, CountCb) : E_FAIL;
        P("  %s: through IDirectDraw %d (0x%08lx), through IDirectDraw2 %d (0x%08lx)\n", when, a, h1, b, h2);
    };
    count("no surfaces");
    DDSURFACEDESC d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    d.dwWidth = d.dwHeight = 16;
    IDirectDrawSurface* s = nullptr;
    dd1->CreateSurface(&d, &s, nullptr);
    count("one surface");
    IDirectDrawSurface2* s2 = nullptr;
    IDirectDrawSurface3* s3 = nullptr;
    if (s) {
        s->QueryInterface(IID_IDirectDrawSurface2, reinterpret_cast<void**>(&s2));
        s->QueryInterface(IID_IDirectDrawSurface3, reinterpret_cast<void**>(&s3));
    }
    count("asked for IDirectDrawSurface2 and 3");
    if (s3) s3->Release();
    if (s2) s2->Release();
    count("those two released");
    if (s) s->Release();
    count("the surface released");
    if (dd2) dd2->Release();
    dd1->Release();
}

// ---------------------------------------------------------------- blits

static void Blits(IDirectDraw7* dd) {
    P("\nBlits between 32-bit system-memory surfaces (source pixels numbered from 1; '.' is a pixel left 0):\n");
    const DWORD kMem = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    static const int kStretch[][2] = {{4, 7}, {4, 8}, {5, 3}, {3, 8}, {8, 3}, {7, 4}, {2, 5}, {16, 5}, {5, 16}, {3, 2}, {9, 4}, {1, 5}};
    for (const auto& c : kStretch) {
        IDirectDrawSurface7* src = Make(dd, kMem, c[0], 1, &kX888);
        IDirectDrawSurface7* dst = Make(dd, kMem, c[1], 1, &kX888);
        IDirectDrawSurface7* srcV = Make(dd, kMem, 1, c[0], &kX888);
        IDirectDrawSurface7* dstV = Make(dd, kMem, 1, c[1], &kX888);
        if (src && dst && srcV && dstV) {
            Put(src, Count(c[0]), c[0]);
            Put(srcV, Count(c[0]), 1);
            const HRESULT h = dst->Blt(nullptr, src, nullptr, DDBLT_WAIT, nullptr);
            const HRESULT v = dstV->Blt(nullptr, srcV, nullptr, DDBLT_WAIT, nullptr);
            P("  %d to %d across (0x%08lx):%s\n", c[0], c[1], h, Row(dst, c[1], 1));
            P("  %d to %d down (0x%08lx):%s\n", c[0], c[1], v, Row(dstV, 1, c[1]));
            DDBLTFX fx = {};
            fx.dwSize = sizeof(fx);
            fx.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
            Put(dst, Zero(c[1]), c[1]);
            const HRESULT m = dst->Blt(nullptr, src, nullptr, DDBLT_WAIT | DDBLT_DDFX, &fx);
            P("  %d to %d across, mirrored left-right (0x%08lx):%s\n", c[0], c[1], m, Row(dst, c[1], 1));
        }
        if (src) src->Release();
        if (dst) dst->Release();
        if (srcV) srcV->Release();
        if (dstV) dstV->Release();
    }
    {  // Part of a source, part of a destination.
        IDirectDrawSurface7* src = Make(dd, kMem, 8, 1, &kX888);
        IDirectDrawSurface7* dst = Make(dd, kMem, 12, 1, &kX888);
        if (src && dst) {
            Put(src, Count(8), 8);
            RECT sr = {2, 0, 5, 1}, dr = {1, 0, 8, 1};
            HRESULT hr = dst->Blt(&dr, src, &sr, DDBLT_WAIT, nullptr);
            P("  source pixels 3-5 to destination pixels 2-8 of 12 (0x%08lx):%s\n", hr, Row(dst, 12, 1));
            Put(dst, Zero(12), 12);
            RECT sr2 = {1, 0, 8, 1}, dr2 = {4, 0, 7, 1};
            hr = dst->Blt(&dr2, src, &sr2, DDBLT_WAIT, nullptr);
            P("  source pixels 2-8 to destination pixels 5-7 of 12 (0x%08lx):%s\n", hr, Row(dst, 12, 1));
            // Clipped: the destination's clip list is pixels 4-9 and 12-14 of 16.
            IDirectDrawSurface7* wide = Make(dd, kMem, 16, 1, &kX888);
            IDirectDrawClipper* clip = nullptr;
            dd->CreateClipper(0, &clip, nullptr);
            if (wide && clip) {
                struct {
                    RGNDATAHEADER h;
                    RECT r[2];
                } rgn = {};
                rgn.h.dwSize = sizeof(RGNDATAHEADER);
                rgn.h.iType = RDH_RECTANGLES;
                rgn.h.nCount = 2;
                rgn.h.nRgnSize = sizeof(rgn.r);
                rgn.h.rcBound = {3, 0, 14, 1};
                rgn.r[0] = {3, 0, 9, 1};
                rgn.r[1] = {11, 0, 14, 1};
                const HRESULT ch = clip->SetClipList(reinterpret_cast<RGNDATA*>(&rgn), 0);
                const HRESULT sh = wide->SetClipper(clip);
                RECT five = {0, 0, 5, 1};
                hr = wide->Blt(nullptr, src, &five, DDBLT_WAIT, nullptr);
                P("  5 to 16 across, clipped to pixels 4-9 and 12-14 (SetClipList 0x%08lx, SetClipper 0x%08lx, Blt 0x%08lx):%s\n", ch, sh,
                  hr, Row(wide, 16, 1));
                Put(wide, Zero(16), 16);
                hr = wide->Blt(nullptr, src, nullptr, DDBLT_WAIT, nullptr);
                P("  8 to 16 across, clipped the same (0x%08lx):%s\n", hr, Row(wide, 16, 1));
                Put(wide, Zero(16), 16);
                hr = wide->BltFast(0, 0, src, nullptr, DDBLTFAST_WAIT);
                P("  BltFast to a surface with a clipper: 0x%08lx:%s\n", hr, Row(wide, 16, 1));
                wide->SetClipper(nullptr);
            }
            if (clip) clip->Release();
            if (wide) wide->Release();
        }
        if (src) src->Release();
        if (dst) dst->Release();
    }
    {  // A surface onto itself.
        struct Self {
            const char* name;
            RECT src, dst;
        };
        static const Self kSelf[] = {{"pixels 1-6 to 3-8", {0, 0, 6, 1}, {2, 0, 8, 1}},
                                     {"pixels 3-8 to 1-6", {2, 0, 8, 1}, {0, 0, 6, 1}},
                                     {"pixels 3-6 stretched to 1-8", {2, 0, 6, 1}, {0, 0, 8, 1}},
                                     {"pixels 1-8 shrunk to 3-6", {0, 0, 8, 1}, {2, 0, 6, 1}}};
        for (const Self& k : kSelf) {
            IDirectDrawSurface7* s = Make(dd, kMem, 8, 1, &kX888);
            if (!s) continue;
            Put(s, Count(8), 8);
            RECT sr = k.src, dr = k.dst;
            HRESULT hr = s->Blt(&dr, s, &sr, DDBLT_WAIT, nullptr);
            P("  onto itself, %s: Blt 0x%08lx:%s", k.name, hr, Row(s, 8, 1));
            if (sr.right - sr.left == dr.right - dr.left) {
                Put(s, Count(8), 8);
                hr = s->BltFast(DWORD(dr.left), 0, s, &sr, DDBLTFAST_WAIT);
                P("; BltFast 0x%08lx:%s", hr, Row(s, 8, 1));
            }
            P("\n");
            s->Release();
        }
        // Down: rows 1-3 of 4 to rows 2-4, and back.
        IDirectDrawSurface7* s = Make(dd, kMem, 1, 4, &kX888);
        if (s) {
            Put(s, Count(4), 1);
            RECT sr = {0, 0, 1, 3}, dr = {0, 1, 1, 4};
            HRESULT hr = s->Blt(&dr, s, &sr, DDBLT_WAIT, nullptr);
            P("  onto itself, rows 1-3 to 2-4: Blt 0x%08lx:%s", hr, Row(s, 1, 4));
            Put(s, Count(4), 1);
            hr = s->Blt(&sr, s, &dr, DDBLT_WAIT, nullptr);
            P("; rows 2-4 to 1-3: 0x%08lx:%s\n", hr, Row(s, 1, 4));
            s->Release();
        }
    }
    {  // Odd flags.
        IDirectDrawSurface7* src = Make(dd, kMem, 4, 1, &kX888);
        IDirectDrawSurface7* dst = Make(dd, kMem, 4, 1, &kX888);
        if (src && dst) {
            Put(src, Count(4), 4);
            for (DWORD trans : {DWORD(3), DWORD(4), DWORD(8), DWORD(0x20)}) {
                Put(dst, Zero(4), 4);
                const HRESULT hr = dst->BltFast(0, 0, src, nullptr, trans);
                P("  BltFast with dwTrans %lu: 0x%08lx:%s\n", trans, hr, Row(dst, 4, 1));
            }
            DDBLTFX fx = {};
            fx.dwSize = sizeof(fx);
            fx.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
            Put(dst, Count(4), 4);
            HRESULT hr = dst->Blt(nullptr, nullptr, nullptr, DDBLT_WAIT | DDBLT_DDFX, &fx);
            P("  Blt with DDBLT_DDFX (mirror) and no source: 0x%08lx:%s\n", hr, Row(dst, 4, 1));
            hr = dst->Blt(nullptr, src, nullptr, DDBLT_WAIT | DDBLT_DDFX, nullptr);
            P("  Blt with DDBLT_DDFX and no DDBLTFX: 0x%08lx\n", hr);
            fx.dwDDFX = DDBLTFX_MIRRORUPDOWN | DDBLTFX_MIRRORLEFTRIGHT;
            hr = dst->Blt(nullptr, src, nullptr, DDBLT_WAIT | DDBLT_DDFX, &fx);
            P("  Blt mirrored both ways: 0x%08lx:%s\n", hr, Row(dst, 4, 1));
            for (DWORD ddfx : {DWORD(DDBLTFX_ROTATE90), DWORD(DDBLTFX_ROTATE180), DWORD(DDBLTFX_ARITHSTRETCHY), DWORD(DDBLTFX_NOTEARING),
                               DWORD(DDBLTFX_ZBUFFERBASEDEST)}) {
                fx.dwDDFX = ddfx;
                Put(dst, Zero(4), 4);
                hr = dst->Blt(nullptr, src, nullptr, DDBLT_WAIT | DDBLT_DDFX, &fx);
                P("  Blt with dwDDFX 0x%lx: 0x%08lx:%s\n", ddfx, hr, Row(dst, 4, 1));
            }
            RECT empty = {2, 0, 2, 1}, backwards = {3, 0, 1, 1}, outside = {2, 0, 6, 1};
            P("  Blt with an empty destination rectangle: 0x%08lx; a backwards one: 0x%08lx; one past the surface: 0x%08lx\n",
              dst->Blt(&empty, src, nullptr, DDBLT_WAIT, nullptr), dst->Blt(&backwards, src, nullptr, DDBLT_WAIT, nullptr),
              dst->Blt(&outside, src, nullptr, DDBLT_WAIT, nullptr));
            P("  Blt with an empty source rectangle: 0x%08lx; one past the surface: 0x%08lx\n",
              dst->Blt(nullptr, src, &empty, DDBLT_WAIT, nullptr), dst->Blt(nullptr, src, &outside, DDBLT_WAIT, nullptr));
        }
        if (src) src->Release();
        if (dst) dst->Release();
    }
    {  // A colour fill and a keyed copy on 16 bits: which bits of the value count.
        IDirectDrawSurface7* a = Make(dd, kMem, 4, 1, &k565);
        IDirectDrawSurface7* b = Make(dd, kMem, 4, 1, &k565);
        if (a && b) {
            DDBLTFX fx = {};
            fx.dwSize = sizeof(fx);
            fx.dwFillColor = 0xABCD1234;
            const HRESULT fh = a->Blt(nullptr, nullptr, nullptr, DDBLT_WAIT | DDBLT_COLORFILL, &fx);
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            WORD got = 0;
            if (SUCCEEDED(a->Lock(nullptr, &d, DDLOCK_WAIT, nullptr))) {
                got = *static_cast<WORD*>(d.lpSurface);
                static_cast<WORD*>(d.lpSurface)[1] = 0x0001;
                a->Unlock(nullptr);
            }
            P("  a 5-6-5 surface filled with 0xabcd1234 (0x%08lx) holds 0x%04x\n", fh, got);
            DDCOLORKEY key = {0xFFFF1234, 0xFFFF1234};
            const HRESULT kh = a->SetColorKey(DDCKEY_SRCBLT, &key);
            DDCOLORKEY back = {};
            a->GetColorKey(DDCKEY_SRCBLT, &back);
            fx.dwFillColor = 0x7777;
            b->Blt(nullptr, nullptr, nullptr, DDBLT_WAIT | DDBLT_COLORFILL, &fx);
            const HRESULT bh = b->BltFast(0, 0, a, nullptr, DDBLTFAST_WAIT | DDBLTFAST_SRCCOLORKEY);
            WORD px[4] = {};
            if (SUCCEEDED(b->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
                memcpy(px, d.lpSurface, sizeof(px));
                b->Unlock(nullptr);
            }
            P("  its source key set to 0xffff1234 (0x%08lx) reads back 0x%lx; copied with the key over 0x7777 (0x%08lx): %04x %04x %04x %04x\n",
              kh, back.dwColorSpaceLowValue, bh, px[0], px[1], px[2], px[3]);
        }
        if (a) a->Release();
        if (b) b->Release();
    }
}

// ---------------------------------------------------------------- texture management and overlays

static void TextureCalls(IDirectDraw7* dd) {
    P("\nTexture-management and overlay calls:\n");
    struct Kind {
        const char* name;
        DWORD caps, caps2;
    };
    static const Kind kKinds[] = {{"a managed texture", DDSCAPS_TEXTURE, DDSCAPS2_TEXTUREMANAGE},
                                  {"a system-memory texture", DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY, 0},
                                  {"a video-memory texture", DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY, 0},
                                  {"a plain system-memory surface", DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, 0}};
    for (const Kind& k : kKinds) {
        HRESULT hr = E_FAIL;
        IDirectDrawSurface7* s = Make(dd, k.caps, 16, 16, nullptr, &hr, k.caps2);
        P("  %s (0x%08lx):", k.name, hr);
        if (s) {
            DWORD lod = 0xBAD, pri = 0xBAD;
            const HRESULT a = s->SetLOD(1), b = s->GetLOD(&lod), c = s->SetPriority(5), e = s->GetPriority(&pri);
            P(" SetLOD 0x%08lx, GetLOD 0x%08lx (%lu), SetPriority 0x%08lx, GetPriority 0x%08lx (%lu)", a, b, lod, c, e, pri);
            RECT r = {0, 0, 8, 8};
            const HRESULT f = s->AddOverlayDirtyRect(&r), g = s->UpdateOverlayDisplay(0);
            LONG x = 0, y = 0;
            const HRESULT h = s->GetOverlayPosition(&x, &y), i = s->SetOverlayPosition(1, 1), j = s->UpdateOverlayZOrder(0 /* DDOVERZ_SENDTOFRONT */, nullptr);
            P("; AddOverlayDirtyRect 0x%08lx, UpdateOverlayDisplay 0x%08lx, GetOverlayPosition 0x%08lx, SetOverlayPosition 0x%08lx, "
              "UpdateOverlayZOrder 0x%08lx",
              f, g, h, i, j);
            s->Release();
        }
        P("\n");
    }
}

// ---------------------------------------------------------------- palettes

static void Entries(const char* what, IDirectDrawPalette* pal) {
    PALETTEENTRY e[256] = {};
    DWORD caps = 0;
    const HRESULT gh = pal->GetEntries(0, 0, 256, e);
    const HRESULT ch = pal->GetCaps(&caps);
    P("  %s: GetEntries 0x%08lx, caps 0x%lx (0x%08lx);", what, gh, caps, ch);
    for (int i : {0, 1, 2, 3, 4, 10, 245, 246, 254, 255}) P(" [%d] %02x %02x %02x flags %02x", i, e[i].peRed, e[i].peGreen, e[i].peBlue, e[i].peFlags);
    P("\n");
}

static void FillEntries(PALETTEENTRY* e) {
    static const BYTE kFlags[4] = {0, PC_RESERVED, PC_EXPLICIT, PC_NOCOLLAPSE};
    for (int i = 0; i < 256; i++) e[i] = {BYTE(i), BYTE(255 - i), BYTE(i / 2), kFlags[i % 4]};
    e[0] = {10, 20, 30, PC_NOCOLLAPSE};
    e[255] = {40, 50, 60, 0};
}

static void Palettes(IDirectDraw7* dd) {
    P("\nPalettes (entry i given as red i, green 255-i, blue i/2, flags 0, 1, 2, 4 in turn; [0] as 0a 14 1e flags 04, [255] as 28 32 3c flags 00):\n");
    PALETTEENTRY e[256];
    FillEntries(e);
    struct Kind {
        const char* name;
        DWORD caps;
    };
    static const Kind kKinds[] = {{"8 bits", DDPCAPS_8BIT},
                                  {"8 bits, ALLOW256", DDPCAPS_8BIT | DDPCAPS_ALLOW256},
                                  {"8 bits, INITIALIZE", DDPCAPS_8BIT | DDPCAPS_INITIALIZE},
                                  {"4 bits", DDPCAPS_4BIT},
                                  {"2 bits", DDPCAPS_2BIT},
                                  {"1 bit", DDPCAPS_1BIT},
                                  {"8 bits, 8BITENTRIES", DDPCAPS_8BIT | DDPCAPS_8BITENTRIES},
                                  {"4 bits, 8BITENTRIES", DDPCAPS_4BIT | DDPCAPS_8BITENTRIES},
                                  {"8 bits, ALPHA", DDPCAPS_8BIT | DDPCAPS_ALPHA},
                                  {"8 bits, PRIMARYSURFACE", DDPCAPS_8BIT | DDPCAPS_PRIMARYSURFACE},
                                  {"8 bits, VSYNC", DDPCAPS_8BIT | DDPCAPS_VSYNC},
                                  {"no size", 0},
                                  {"8 and 4 bits", DDPCAPS_8BIT | DDPCAPS_4BIT}};
    for (const Kind& k : kKinds) {
        IDirectDrawPalette* pal = nullptr;
        const HRESULT hr = dd->CreatePalette(k.caps, e, &pal, nullptr);
        if (!pal) {
            P("  %s: CreatePalette 0x%08lx\n", k.name, hr);
            continue;
        }
        Entries(k.name, pal);
        PALETTEENTRY one[2] = {{1, 2, 3, 0xFF}, {4, 5, 6, 0x80}};
        const HRESULT a = pal->SetEntries(0, 0, 1, one), b = pal->SetEntries(0, 255, 1, one), c = pal->SetEntries(0, 255, 2, one),
                      d = pal->SetEntries(0, 5, 2, one), f = pal->SetEntries(1, 5, 1, one);
        P("    SetEntries of [0] 0x%08lx, of [255] 0x%08lx, of two from 255 0x%08lx, of [5] and [6] 0x%08lx, with flags 1 0x%08lx\n", a, b,
          c, d, f);
        Entries("    then", pal);
        pal->Release();
    }
}

// ---------------------------------------------------------------- legacy clears

// A 64x64 target of `f` cleared through a viewport of Direct3D `version` (1, 2 or 3) whose
// background material is `color`, inside a scene: the result and what the target holds.
static void LegacyClear(IDirectDraw* dd1, int version, const Rgb& f, bool hard) {
    DDSURFACEDESC sd = {};
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | (hard ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
    sd.dwWidth = sd.dwHeight = 64;
    SetFormat(&sd.ddpfPixelFormat, f);
    IDirectDrawSurface* rt = nullptr;
    HRESULT hr = dd1->CreateSurface(&sd, &rt, nullptr);
    P("  Direct3D %d, %s, %s device: target 0x%08lx", version, f.name, hard ? "hardware" : "RGB", hr);
    if (!rt) {
        sd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE | (hard ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
        hr = dd1->CreateSurface(&sd, &rt, nullptr);
        P(", as a texture 0x%08lx", hr);
    }
    if (!rt) {
        P("\n");
        return;
    }
    const GUID& guid = hard ? IID_IDirect3DHALDevice : IID_IDirect3DRGBDevice;
    static const DWORD kColors[3] = {0x40C02010, 0xC0C0C0C0, 0xFFFFFFFF};
    for (DWORD color : kColors) {
        DDSURFACEDESC l = {};
        l.dwSize = sizeof(l);
        if (SUCCEEDED(rt->Lock(nullptr, &l, DDLOCK_WAIT, nullptr))) {
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) {
                    BYTE* row = static_cast<BYTE*>(l.lpSurface) + y * l.lPitch;
                    if (f.bits == 16) reinterpret_cast<WORD*>(row)[x] = 0x1234;
                    else reinterpret_cast<DWORD*>(row)[x] = 0x12345678;
                }
            rt->Unlock(nullptr);
        }
        D3DMATERIAL m = {};
        m.dwSize = sizeof(m);
        m.diffuse.r = float((color >> 16) & 0xFF) / 255.0f;
        m.diffuse.g = float((color >> 8) & 0xFF) / 255.0f;
        m.diffuse.b = float(color & 0xFF) / 255.0f;
        m.diffuse.a = float(color >> 24) / 255.0f;
        m.dwRampSize = 1;
        D3DRECT all = {{0}, {0}, {64}, {64}};
        D3DMATERIALHANDLE h = 0;
        hr = E_FAIL;
        if (version == 1) {
            IDirect3D* d3d = nullptr;
            IDirect3DDevice* dev = nullptr;
            IDirect3DViewport* vp = nullptr;
            IDirect3DMaterial* mat = nullptr;
            dd1->QueryInterface(IID_IDirect3D, reinterpret_cast<void**>(&d3d));
            hr = rt->QueryInterface(guid, reinterpret_cast<void**>(&dev));
            if (d3d && dev && SUCCEEDED(hr = d3d->CreateViewport(&vp, nullptr)) && SUCCEEDED(hr = dev->AddViewport(vp)) &&
                SUCCEEDED(hr = d3d->CreateMaterial(&mat, nullptr))) {
                D3DVIEWPORT vd = {};
                vd.dwSize = sizeof(vd);
                vd.dwWidth = vd.dwHeight = 64;
                vd.dvScaleX = vd.dvScaleY = 32.0f;
                vd.dvMaxX = vd.dvMaxY = 1.0f;
                vd.dvMaxZ = 1.0f;
                vp->SetViewport(&vd);
                mat->SetMaterial(&m);
                mat->GetHandle(dev, &h);
                vp->SetBackground(h);
                dev->BeginScene();
                hr = vp->Clear(1, &all, D3DCLEAR_TARGET);
                dev->EndScene();
            }
            if (mat) mat->Release();
            if (vp) vp->Release();
            if (dev) dev->Release();
            if (d3d) d3d->Release();
        } else if (version == 2) {
            IDirect3D2* d3d = nullptr;
            IDirect3DDevice2* dev = nullptr;
            IDirect3DViewport2* vp = nullptr;
            IDirect3DMaterial2* mat = nullptr;
            dd1->QueryInterface(IID_IDirect3D2, reinterpret_cast<void**>(&d3d));
            if (d3d) hr = d3d->CreateDevice(guid, rt, &dev);
            if (dev && SUCCEEDED(hr = d3d->CreateViewport(&vp, nullptr)) && SUCCEEDED(hr = dev->AddViewport(vp)) &&
                SUCCEEDED(hr = d3d->CreateMaterial(&mat, nullptr))) {
                D3DVIEWPORT2 vd = {};
                vd.dwSize = sizeof(vd);
                vd.dwWidth = vd.dwHeight = 64;
                vd.dvClipX = -1.0f, vd.dvClipY = 1.0f, vd.dvClipWidth = vd.dvClipHeight = 2.0f;
                vd.dvMaxZ = 1.0f;
                vp->SetViewport2(&vd);
                dev->SetCurrentViewport(vp);
                mat->SetMaterial(&m);
                mat->GetHandle(dev, &h);
                vp->SetBackground(h);
                dev->BeginScene();
                hr = vp->Clear(1, &all, D3DCLEAR_TARGET);
                dev->EndScene();
            }
            if (mat) mat->Release();
            if (vp) vp->Release();
            if (dev) dev->Release();
            if (d3d) d3d->Release();
        } else {
            IDirect3D3* d3d = nullptr;
            IDirect3DDevice3* dev = nullptr;
            IDirect3DViewport3* vp = nullptr;
            IDirect3DMaterial3* mat = nullptr;
            IDirectDrawSurface4* rt4 = nullptr;
            dd1->QueryInterface(IID_IDirect3D3, reinterpret_cast<void**>(&d3d));
            rt->QueryInterface(IID_IDirectDrawSurface4, reinterpret_cast<void**>(&rt4));
            if (d3d && rt4) hr = d3d->CreateDevice(guid, rt4, &dev, nullptr);
            if (dev && SUCCEEDED(hr = d3d->CreateViewport(&vp, nullptr)) && SUCCEEDED(hr = dev->AddViewport(vp)) &&
                SUCCEEDED(hr = d3d->CreateMaterial(&mat, nullptr))) {
                D3DVIEWPORT2 vd = {};
                vd.dwSize = sizeof(vd);
                vd.dwWidth = vd.dwHeight = 64;
                vd.dvClipX = -1.0f, vd.dvClipY = 1.0f, vd.dvClipWidth = vd.dvClipHeight = 2.0f;
                vd.dvMaxZ = 1.0f;
                vp->SetViewport2(&vd);
                dev->SetCurrentViewport(vp);
                mat->SetMaterial(&m);
                mat->GetHandle(dev, &h);
                vp->SetBackground(h);
                dev->BeginScene();
                hr = vp->Clear(1, &all, D3DCLEAR_TARGET);
                dev->EndScene();
            }
            if (mat) mat->Release();
            if (vp) vp->Release();
            if (dev) dev->Release();
            if (rt4) rt4->Release();
            if (d3d) d3d->Release();
        }
        P(", clear 0x%08lx", hr);
        if (SUCCEEDED(rt->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
            const BYTE* row = static_cast<const BYTE*>(l.lpSurface) + 32 * l.lPitch;
            if (f.bits == 16) P(" holds 0x%04x", reinterpret_cast<const WORD*>(row)[32]);
            else P(" holds 0x%08lx", reinterpret_cast<const DWORD*>(row)[32]);
            rt->Unlock(nullptr);
        }
    }
    P("\n");
    rt->Release();
}

static void LegacyClears() {
    P("\nA viewport's Clear with a background material of 0x40c02010, 0xc0c0c0c0, then 0xffffffff, in a scene "
      "(0x1234 or 0x12345678 in the target before each):\n");
    IDirectDraw* dd1 = nullptr;
    DirectDrawCreate(nullptr, &dd1, nullptr);
    if (!dd1) return;
    dd1->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
    static const Rgb kTargets[] = {k565,
                                   {"5-5-5", 16, 0x7C00, 0x03E0, 0x001F, 0},
                                   {"1-5-5-5", 16, 0x7C00, 0x03E0, 0x001F, 0x8000},
                                   {"4-4-4-4", 16, 0x0F00, 0x00F0, 0x000F, 0xF000},
                                   kX888,
                                   {"8-8-8-8", 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000}};
    for (int version = 1; version <= 3; version++)
        for (const Rgb& f : kTargets)
            for (int hard = 0; hard < 2; hard++) LegacyClear(dd1, version, f, hard != 0);
    dd1->Release();
}

// ---------------------------------------------------------------- exclusive mode

static char g_messages[256][96];
static int g_messageCount;
static bool g_logMessages;

static LRESULT CALLBACK ProbeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_logMessages) {
        struct Name {
            UINT msg;
            const char* name;
        };
        static const Name kNames[] = {{WM_ACTIVATEAPP, "WM_ACTIVATEAPP"}, {WM_ACTIVATE, "WM_ACTIVATE"},
                                      {WM_NCACTIVATE, "WM_NCACTIVATE"},   {WM_SIZE, "WM_SIZE"},
                                      {WM_MOVE, "WM_MOVE"},               {WM_DISPLAYCHANGE, "WM_DISPLAYCHANGE"},
                                      {WM_SETFOCUS, "WM_SETFOCUS"},       {WM_KILLFOCUS, "WM_KILLFOCUS"},
                                      {WM_SHOWWINDOW, "WM_SHOWWINDOW"},   {WM_SYSCOMMAND, "WM_SYSCOMMAND"},
                                      {WM_WINDOWPOSCHANGING, "WM_WINDOWPOSCHANGING"},
                                      {WM_WINDOWPOSCHANGED, "WM_WINDOWPOSCHANGED"},
                                      {WM_STYLECHANGED, "WM_STYLECHANGED"}, {WM_QUERYNEWPALETTE, "WM_QUERYNEWPALETTE"},
                                      {WM_PALETTECHANGED, "WM_PALETTECHANGED"}, {WM_PALETTEISCHANGING, "WM_PALETTEISCHANGING"},
                                      {WM_SYSCOLORCHANGE, "WM_SYSCOLORCHANGE"}, {WM_SETTINGCHANGE, "WM_SETTINGCHANGE"},
                                      {WM_CAPTURECHANGED, "WM_CAPTURECHANGED"}, {WM_ENTERSIZEMOVE, "WM_ENTERSIZEMOVE"},
                                      {WM_EXITSIZEMOVE, "WM_EXITSIZEMOVE"}, {WM_QUERYOPEN, "WM_QUERYOPEN"},
                                      {WM_POWER, "WM_POWER"}, {WM_POWERBROADCAST, "WM_POWERBROADCAST"}};
        for (const Name& n : kNames)
            if (n.msg == msg) {
                if (g_messageCount >= 256) break;
                char* t = g_messages[g_messageCount++];
                if (msg == WM_WINDOWPOSCHANGING || msg == WM_WINDOWPOSCHANGED) {
                    const WINDOWPOS* p = reinterpret_cast<const WINDOWPOS*>(lp);
                    snprintf(t, 96, "%s(%d,%d %dx%d flags 0x%x)", n.name, p->x, p->y, p->cx, p->cy, p->flags);
                } else {
                    snprintf(t, 96, "%s(0x%lx, 0x%lx)", n.name, DWORD(wp), DWORD(lp));
                }
            }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// The window's rectangle, through the program's import each time it is asked: a wrapper
// that points the import at its own function while the program runs (dd2d9 did, up to
// 1.8.3, at the first SetDisplayMode) is then asked every time. Called inline, the compiler
// loads the import's address once for several calls, and those after the first went on to
// Windows' GetWindowRect: which ones depended on the build.
[[gnu::noinline]] static RECT WindowRect(HWND hwnd) {
    RECT r = {};
    GetWindowRect(hwnd, &r);
    return r;
}

static void Pump(int ms) {
    const DWORD until = GetTickCount() + DWORD(ms);
    do {
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        Sleep(10);
    } while (GetTickCount() < until);
}

static void AttachedOf(IDirectDrawSurface7* s, const char* name);
static HRESULT WINAPI AttachedCb(IDirectDrawSurface7* s, DDSURFACEDESC2* d, void*) {
    P(" [caps 0x%lx caps2 0x%lx, %lux%lu]", d->ddsCaps.dwCaps, d->ddsCaps.dwCaps2, d->dwWidth, d->dwHeight);
    if (s) s->Release();
    return DDENUMRET_OK;
}
static void AttachedOf(IDirectDrawSurface7* s, const char* name) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    s->GetSurfaceDesc(&d);
    P("    %s (caps 0x%lx caps2 0x%lx, %lux%lu) has attached:", name, d.ddsCaps.dwCaps, d.ddsCaps.dwCaps2, d.dwWidth, d.dwHeight);
    const HRESULT hr = s->EnumAttachedSurfaces(nullptr, AttachedCb);
    P(" (0x%08lx)\n", hr);
}

static void Exclusive(IDirectDraw7* dd) {
    P("\nExclusive mode, step by step (TestCooperativeLevel; IsLost of a primary made before, of a system-memory surface, a "
      "video-memory one and a managed texture; the window's messages):\n");
    WNDCLASSA wc = {};
    wc.lpfnWndProc = ProbeProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "fmtprobe window";
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "fmtprobe", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    Pump(200);
    HRESULT hr = dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    P("  SetCooperativeLevel(window, NORMAL): 0x%08lx\n", hr);
    DDSURFACEDESC2 pd = {};
    pd.dwSize = sizeof(pd);
    pd.dwFlags = DDSD_CAPS;
    pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* prim = nullptr;
    dd->CreateSurface(&pd, &prim, nullptr);
    IDirectDrawSurface7* sys = Make(dd, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, 32, 32);
    IDirectDrawSurface7* vid = Make(dd, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, 32, 32);
    IDirectDrawSurface7* man = Make(dd, DDSCAPS_TEXTURE, 32, 32, nullptr, nullptr, DDSCAPS2_TEXTUREMANAGE);
    auto state = [&](const char* step, HRESULT result) {
        Pump(400);
        P("  %s: 0x%08lx; TestCooperativeLevel 0x%08lx; lost: primary 0x%08lx, system 0x%08lx, video 0x%08lx, managed 0x%08lx\n", step,
          result, dd->TestCooperativeLevel(), prim ? prim->IsLost() : E_FAIL, sys ? sys->IsLost() : E_FAIL, vid ? vid->IsLost() : E_FAIL,
          man ? man->IsLost() : E_FAIL);
        DDSURFACEDESC2 m = {};
        m.dwSize = sizeof(m);
        dd->GetDisplayMode(&m);
        const RECT wr = WindowRect(hwnd);
        P("    mode %lux%lu, %lu bits; window at %ld,%ld %ldx%ld, style 0x%lx, ex style 0x%lx, %s%s\n", m.dwWidth, m.dwHeight,
          m.ddpfPixelFormat.dwRGBBitCount, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, DWORD(GetWindowLongA(hwnd, GWL_STYLE)),
          DWORD(GetWindowLongA(hwnd, GWL_EXSTYLE)), IsIconic(hwnd) ? "minimized" : "not minimized",
          GetForegroundWindow() == hwnd ? ", foreground" : ", not foreground");
        P("    messages:");
        for (int i = 0; i < g_messageCount; i++) P(" %s", g_messages[i]);
        P("\n");
        g_messageCount = 0;
    };
    g_logMessages = true;
    state("at the start", S_OK);
    state("SetCooperativeLevel(EXCLUSIVE | FULLSCREEN)", dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN));
    state("SetDisplayMode(640, 480, 16)", dd->SetDisplayMode(640, 480, 16, 0, 0));
    if (prim) state("Restore of the primary", prim->Restore());
    if (vid) state("Restore of the video-memory surface", vid->Restore());
    state("SetDisplayMode(640, 480, 16) again", dd->SetDisplayMode(640, 480, 16, 0, 0));
    state("SetDisplayMode(800, 600, 16)", dd->SetDisplayMode(800, 600, 16, 0, 0));
    state("RestoreAllSurfaces", dd->RestoreAllSurfaces());
    state("SetDisplayMode(640, 480, 32)", dd->SetDisplayMode(640, 480, 32, 0, 0));
    state("SetDisplayMode(640, 480, 16)", dd->SetDisplayMode(640, 480, 16, 0, 0));
    dd->RestoreAllSurfaces();

    {  // Attachment order.
        P("  Attachments, in the order EnumAttachedSurfaces gives them:\n");
        DDSURFACEDESC2 fd = {};
        fd.dwSize = sizeof(fd);
        fd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        fd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
        fd.dwBackBufferCount = 2;
        if (prim) {
            prim->Release();
            prim = nullptr;
        }
        IDirectDrawSurface7* front = nullptr;
        hr = dd->CreateSurface(&fd, &front, nullptr);
        P("    a flipping primary with two back buffers: 0x%08lx\n", hr);
        if (front) {
            AttachedOf(front, "the front buffer");
            DDSCAPS2 back = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
            IDirectDrawSurface7 *b1 = nullptr, *b2 = nullptr, *b3 = nullptr;
            front->GetAttachedSurface(&back, &b1);
            if (b1) {
                AttachedOf(b1, "its back buffer");
                DDSCAPS2 any = {DDSCAPS_FLIP, 0, 0, {0}};
                const HRESULT gh = b1->GetAttachedSurface(&any, &b2);
                P("    GetAttachedSurface(FLIP) from the back buffer: 0x%08lx\n", gh);
                if (b2) {
                    AttachedOf(b2, "that one");
                    const HRESULT g3 = b2->GetAttachedSurface(&any, &b3);
                    P("    GetAttachedSurface(FLIP) from that one: 0x%08lx, %s\n", g3, b3 == front ? "the front buffer" : "another surface");
                }
                // A Z buffer attached to the back buffer.
                DDSURFACEDESC2 zd = {};
                zd.dwSize = sizeof(zd);
                zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
                zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
                zd.dwWidth = 640;
                zd.dwHeight = 480;
                zd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
                zd.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
                zd.ddpfPixelFormat.dwZBufferBitDepth = 16;
                zd.ddpfPixelFormat.dwZBitMask = 0xFFFF;
                IDirectDrawSurface7* z = nullptr;
                HRESULT zh = dd->CreateSurface(&zd, &z, nullptr);
                if (!z) {
                    zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_SYSTEMMEMORY;
                    zh = dd->CreateSurface(&zd, &z, nullptr);
                }
                if (z) {
                    const HRESULT ah = b1->AddAttachedSurface(z);
                    P("    a Z buffer (0x%08lx) attached to the back buffer: 0x%08lx\n", zh, ah);
                    AttachedOf(b1, "the back buffer");
                    AttachedOf(z, "the Z buffer");
                    AttachedOf(front, "the front buffer");
                    const HRESULT fh = front->Flip(nullptr, DDFLIP_WAIT);
                    P("    after a Flip (0x%08lx):\n", fh);
                    AttachedOf(front, "the front buffer");
                    AttachedOf(b1, "the back buffer");
                    const HRESULT dh = b1->DeleteAttachedSurface(0, z);
                    P("    DeleteAttachedSurface of the Z buffer: 0x%08lx\n", dh);
                    z->Release();
                }
            }
            if (b3) b3->Release();
            if (b2) b2->Release();
            if (b1) b1->Release();
            front->Release();
        }
        DDSURFACEDESC2 md = {};
        md.dwSize = sizeof(md);
        md.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        md.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX | DDSCAPS_SYSTEMMEMORY;
        md.dwWidth = 16;
        md.dwHeight = 8;
        IDirectDrawSurface7* mip = nullptr;
        hr = dd->CreateSurface(&md, &mip, nullptr);
        P("    a 16x8 mipmapped texture, no count given: 0x%08lx\n", hr);
        IDirectDrawSurface7* level = mip;
        if (level) level->AddRef();
        for (int i = 0; level && i < 8; i++) {
            char name[32];
            snprintf(name, sizeof(name), "level %d", i);
            AttachedOf(level, name);
            DDSCAPS2 next = {DDSCAPS_TEXTURE | DDSCAPS_MIPMAP, 0, 0, {0}};
            IDirectDrawSurface7* n = nullptr;
            level->GetAttachedSurface(&next, &n);
            level->Release();
            level = n;
        }
        if (mip) mip->Release();
        prim = nullptr;
        pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        dd->CreateSurface(&pd, &prim, nullptr);
    }

    {  // Gamma.
        P("  Gamma ramps:\n");
        DDCAPS drv = {}, hel = {};
        drv.dwSize = hel.dwSize = sizeof(DDCAPS);
        dd->GetCaps(&drv, &hel);
        P("    DDCAPS2_PRIMARYGAMMA: driver %s, emulation %s; DDCAPS2_CANCALIBRATEGAMMA: driver %s\n",
          (drv.dwCaps2 & DDCAPS2_PRIMARYGAMMA) ? "yes" : "no", (hel.dwCaps2 & DDCAPS2_PRIMARYGAMMA) ? "yes" : "no",
          (drv.dwCaps2 & DDCAPS2_CANCALIBRATEGAMMA) ? "yes" : "no");
        IDirectDrawGammaControl* gamma = nullptr;
        hr = prim ? prim->QueryInterface(IID_IDirectDrawGammaControl, reinterpret_cast<void**>(&gamma)) : E_FAIL;
        P("    IDirectDrawGammaControl from the primary: 0x%08lx\n", hr);
        if (gamma) {
            static DDGAMMARAMP saved, ramp, got;
            const HRESULT gh = gamma->GetGammaRamp(0, &saved);
            P("    GetGammaRamp: 0x%08lx, red[0] 0x%04x, red[128] 0x%04x, red[255] 0x%04x\n", gh, saved.red[0], saved.red[128], saved.red[255]);
            struct Ramp {
                const char* name;
                int kind;
            };
            static const Ramp kRamps[] = {{"all zero", 0}, {"half brightness", 1}, {"inverted", 2}, {"all 0xffff", 3}, {"a step at 128", 4}};
            for (const Ramp& k : kRamps) {
                for (int i = 0; i < 256; i++) {
                    const WORD v = k.kind == 0 ? 0 : k.kind == 1 ? WORD(i * 128) : k.kind == 2 ? WORD((255 - i) * 257) : k.kind == 3 ? 0xFFFF
                                                                                                                  : i < 128 ? 0 : 0xFFFF;
                    ramp.red[i] = ramp.green[i] = ramp.blue[i] = v;
                }
                const HRESULT sh = gamma->SetGammaRamp(0, &ramp);
                memset(&got, 0xEE, sizeof(got));
                const HRESULT rh = gamma->GetGammaRamp(0, &got);
                P("    SetGammaRamp, %s: 0x%08lx; GetGammaRamp then 0x%08lx, red[0] 0x%04x, red[128] 0x%04x, red[255] 0x%04x\n", k.name, sh,
                  rh, got.red[0], got.red[128], got.red[255]);
                if (SUCCEEDED(gh)) gamma->SetGammaRamp(0, &saved);
            }
            const HRESULT nh = gamma->SetGammaRamp(0, nullptr);
            const HRESULT ch = SUCCEEDED(gh) ? gamma->SetGammaRamp(1 /* DDSGR_CALIBRATE */, &saved) : E_FAIL;
            P("    SetGammaRamp with a null ramp: 0x%08lx; with DDSGR_CALIBRATE: 0x%08lx\n", nh, ch);
            if (SUCCEEDED(gh)) gamma->SetGammaRamp(0, &saved);
            gamma->Release();
        }
        IDirectDrawSurface7* off = Make(dd, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, 16, 16);
        if (off) {
            gamma = nullptr;
            hr = off->QueryInterface(IID_IDirectDrawGammaControl, reinterpret_cast<void**>(&gamma));
            static DDGAMMARAMP r2;
            P("    IDirectDrawGammaControl from a plain surface: 0x%08lx; its GetGammaRamp: 0x%08lx\n", hr,
              gamma ? gamma->GetGammaRamp(0, &r2) : E_FAIL);
            if (gamma) gamma->Release();
            off->Release();
        }
    }

    // The window left and come back to.
    g_messageCount = 0;
    ShowWindow(hwnd, SW_MINIMIZE);
    state("ShowWindow(SW_MINIMIZE)", S_OK);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    state("ShowWindow(SW_RESTORE) and SetForegroundWindow", S_OK);
    if (prim) state("Restore of the primary", prim->Restore());
    state("RestoreAllSurfaces", dd->RestoreAllSurfaces());

    {  // 256 colours: a palette on the primary.
        state("SetDisplayMode(640, 480, 8)", dd->SetDisplayMode(640, 480, 8, 0, 0));
        dd->RestoreAllSurfaces();
        PALETTEENTRY e[256];
        FillEntries(e);
        for (DWORD extra : {DWORD(0), DWORD(DDPCAPS_ALLOW256)}) {
            IDirectDrawPalette* pal = nullptr;
            hr = dd->CreatePalette(DDPCAPS_8BIT | extra, e, &pal, nullptr);
            if (!pal) continue;
            const HRESULT sh = prim ? prim->SetPalette(pal) : E_FAIL;
            Pump(200);
            P("  a palette (%s) set on the 8-bit primary: 0x%08lx\n", extra ? "ALLOW256" : "plain", sh);
            Entries("  it then", pal);
            PALETTEENTRY sysPal[256] = {};
            HDC dc = GetDC(nullptr);
            const UINT n = GetSystemPaletteEntries(dc, 0, 256, sysPal);
            P("    the system palette (%u entries, SYSPAL use %u):", n, GetSystemPaletteUse(dc));
            for (int i : {0, 1, 2, 9, 10, 245, 246, 254, 255})
                P(" [%d] %02x %02x %02x flags %02x", i, sysPal[i].peRed, sysPal[i].peGreen, sysPal[i].peBlue, sysPal[i].peFlags);
            P("\n");
            ReleaseDC(nullptr, dc);
            PALETTEENTRY one = {1, 2, 3, 0};
            const HRESULT a = pal->SetEntries(0, 0, 1, &one), b = pal->SetEntries(0, 255, 1, &one), c = pal->SetEntries(0, 100, 1, &one);
            P("    SetEntries of [0] 0x%08lx, of [255] 0x%08lx, of [100] 0x%08lx\n", a, b, c);
            Entries("    then", pal);
            IDirectDrawPalette* back = nullptr;
            const HRESULT gh = prim ? prim->GetPalette(&back) : E_FAIL;
            P("    GetPalette from the primary: 0x%08lx, %s; palette references %lu\n", gh, back == pal ? "the same" : "another", Refs(pal));
            if (back) back->Release();
            if (prim) prim->SetPalette(nullptr);
            P("    after SetPalette(null): references %lu\n", Refs(pal));
            pal->Release();
        }
        g_messageCount = 0;
    }

    state("RestoreDisplayMode", dd->RestoreDisplayMode());
    state("SetCooperativeLevel(NORMAL)", dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL));
    if (prim) state("Restore of the primary", prim->Restore());
    state("RestoreAllSurfaces", dd->RestoreAllSurfaces());
    g_logMessages = false;
    if (man) man->Release();
    if (vid) vid->Release();
    if (sys) sys->Release();
    if (prim) prim->Release();
    dd->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
    DestroyWindow(hwnd);
    Pump(100);
}

// ---------------------------------------------------------------- default states

static void States(IDirectDraw7* dd7) {
    P("\nDefault states (number=value; a failing Get as number:result):\n");
    IDirect3D7* d3d7 = nullptr;
    dd7->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d7));
    static char all[400][48], rgb[400][48];  // (a device's states as text; the RGB device's kept to compare)
    int count = 0, rgbCount = 0;
    for (int hard = 0; hard < 2 && d3d7; hard++) {
        IDirectDrawSurface7* rt =
            Make(dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | (hard ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY), 64, 64);
        IDirect3DDevice7* dev = nullptr;
        if (rt) d3d7->CreateDevice(hard ? IID_IDirect3DHALDevice : IID_IDirect3DRGBDevice, rt, &dev);
        if (dev) {
            count = 0;
            for (int i = 1; i <= 152; i++) {
                DWORD v = 0xBADBAD;
                const HRESULT hr = dev->GetRenderState(D3DRENDERSTATETYPE(i), &v);
                char* t = all[count++];
                if (SUCCEEDED(hr)) snprintf(t, 48, "%d=0x%lx", i, v);
                else snprintf(t, 48, "%d:0x%08lx", i, hr);
            }
            for (int stage = 0; stage < 8; stage++)
                for (int i = 1; i <= 24; i++) {
                    DWORD v = 0xBADBAD;
                    const HRESULT hr = dev->GetTextureStageState(DWORD(stage), D3DTEXTURESTAGESTATETYPE(i), &v);
                    char* t = all[count++];
                    if (SUCCEEDED(hr)) snprintf(t, 48, "s%d.%d=0x%lx", stage, i, v);
                    else snprintf(t, 48, "s%d.%d:0x%08lx", stage, i, hr);
                }
            if (!hard) {
                memcpy(rgb, all, sizeof(all));
                rgbCount = count;
                P("  Direct3D 7 RGB device, render states 1-152, then texture-stage states (stage.number):\n   ");
                for (int i = 0; i < count; i++) P(" %s%s", all[i], i % 10 == 9 ? "\n   " : "");
                P("\n");
            } else {
                P("  Direct3D 7 hardware device, where it differs from the RGB device:");
                for (int i = 0; i < count && i < rgbCount; i++)
                    if (strcmp(all[i], rgb[i])) P(" %s", all[i]);
                P("\n");
            }
            D3DVIEWPORT7 vp = {};
            D3DMATERIAL7 m = {};
            D3DCLIPSTATUS cs = {};
            BOOL on = 7;
            D3DLIGHT7 light = {};
            const HRESULT a = dev->GetViewport(&vp), b = dev->GetMaterial(&m), c = dev->GetClipStatus(&cs), e = dev->GetLightEnable(0, &on),
                          f = dev->GetLight(0, &light);
            P("    viewport 0x%08lx: %lu,%lu %lux%lu z %g-%g; material 0x%08lx: diffuse %g %g %g %g, power %g; clip status 0x%08lx: "
              "flags 0x%lx status 0x%lx; light 0 enabled 0x%08lx (%d), GetLight 0x%08lx\n",
              a, vp.dwX, vp.dwY, vp.dwWidth, vp.dwHeight, double(vp.dvMinZ), double(vp.dvMaxZ), b, double(m.diffuse.r), double(m.diffuse.g),
              double(m.diffuse.b), double(m.diffuse.a), double(m.power), c, cs.dwFlags, cs.dwStatus, e, int(on), f);
            for (D3DTRANSFORMSTATETYPE t : {D3DTRANSFORMSTATE_WORLD, D3DTRANSFORMSTATE_VIEW, D3DTRANSFORMSTATE_PROJECTION,
                                            D3DTRANSFORMSTATE_TEXTURE0, D3DTRANSFORMSTATE_WORLD1}) {
                D3DMATRIX mx = {};
                const HRESULT th = dev->GetTransform(t, &mx);
                P("    transform %d: 0x%08lx, diagonal %g %g %g %g, sum of the rest %g\n", int(t), th, double(mx._11), double(mx._22),
                  double(mx._33), double(mx._44),
                  double(mx._12 + mx._13 + mx._14 + mx._21 + mx._23 + mx._24 + mx._31 + mx._32 + mx._34 + mx._41 + mx._42 + mx._43));
            }
            dev->Release();
        }
        if (rt) rt->Release();
    }
    if (d3d7) d3d7->Release();

    IDirectDraw* dd1 = nullptr;
    DirectDrawCreate(nullptr, &dd1, nullptr);
    if (!dd1) return;
    dd1->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
    IDirect3D3* d3d3 = nullptr;
    IDirect3D2* d3d2 = nullptr;
    dd1->QueryInterface(IID_IDirect3D3, reinterpret_cast<void**>(&d3d3));
    dd1->QueryInterface(IID_IDirect3D2, reinterpret_cast<void**>(&d3d2));
    rgbCount = 0;
    for (int hard = 0; hard < 2; hard++) {
        DDSURFACEDESC sd = {};
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | (hard ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
        sd.dwWidth = sd.dwHeight = 64;
        IDirectDrawSurface* rt = nullptr;
        IDirectDrawSurface4* rt4 = nullptr;
        if (FAILED(dd1->CreateSurface(&sd, &rt, nullptr))) continue;
        rt->QueryInterface(IID_IDirectDrawSurface4, reinterpret_cast<void**>(&rt4));
        const GUID& guid = hard ? IID_IDirect3DHALDevice : IID_IDirect3DRGBDevice;
        IDirect3DDevice3* dev3 = nullptr;
        if (d3d3 && rt4) d3d3->CreateDevice(guid, rt4, &dev3, nullptr);
        if (dev3) {
            count = 0;
            for (int i = 1; i <= 135; i++) {
                DWORD v = 0xBADBAD;
                const HRESULT hr = dev3->GetRenderState(D3DRENDERSTATETYPE(i), &v);
                char* t = all[count++];
                if (SUCCEEDED(hr)) snprintf(t, 48, "%d=0x%lx", i, v);
                else snprintf(t, 48, "%d:0x%08lx", i, hr);
            }
            for (int i = 1; i <= 8; i++) {
                DWORD v = 0xBADBAD;
                const HRESULT hr = dev3->GetLightState(D3DLIGHTSTATETYPE(i), &v);
                char* t = all[count++];
                if (SUCCEEDED(hr)) snprintf(t, 48, "L%d=0x%lx", i, v);
                else snprintf(t, 48, "L%d:0x%08lx", i, hr);
            }
            for (int stage = 0; stage < 8; stage++)
                for (int i = 1; i <= 23; i++) {
                    DWORD v = 0xBADBAD;
                    const HRESULT hr = dev3->GetTextureStageState(DWORD(stage), D3DTEXTURESTAGESTATETYPE(i), &v);
                    char* t = all[count++];
                    if (SUCCEEDED(hr)) snprintf(t, 48, "s%d.%d=0x%lx", stage, i, v);
                    else snprintf(t, 48, "s%d.%d:0x%08lx", stage, i, hr);
                }
            if (!hard) {
                memcpy(rgb, all, sizeof(all));
                rgbCount = count;
                P("  Direct3D 3 RGB device, render states 1-135, light states L1-L8, texture-stage states:\n   ");
                for (int i = 0; i < count; i++) P(" %s%s", all[i], i % 10 == 9 ? "\n   " : "");
                P("\n");
            } else {
                P("  Direct3D 3 hardware device, where it differs from the RGB device:");
                for (int i = 0; i < count && i < rgbCount; i++)
                    if (strcmp(all[i], rgb[i])) P(" %s", all[i]);
                P("\n");
            }
            IDirect3DViewport3* cur = nullptr;
            D3DCLIPSTATUS cs = {};
            const HRESULT a = dev3->GetCurrentViewport(&cur), b = dev3->GetClipStatus(&cs);
            P("    GetCurrentViewport without one 0x%08lx; clip status 0x%08lx: flags 0x%lx status 0x%lx\n", a, b, cs.dwFlags, cs.dwStatus);
            if (cur) cur->Release();
            dev3->Release();
        }
        IDirect3DDevice2* dev2 = nullptr;
        if (d3d2) d3d2->CreateDevice(guid, rt, &dev2);
        if (dev2) {
            P("  Direct3D 2 %s device, render states 1-95 and light states L1-L8:\n   ", hard ? "hardware" : "RGB");
            for (int i = 1; i <= 95; i++) {
                DWORD v = 0xBADBAD;
                const HRESULT hr = dev2->GetRenderState(D3DRENDERSTATETYPE(i), &v);
                if (SUCCEEDED(hr)) P(" %d=0x%lx", i, v);
                else P(" %d:0x%08lx", i, hr);
                if (i % 10 == 0) P("\n   ");
            }
            for (int i = 1; i <= 8; i++) {
                DWORD v = 0xBADBAD;
                const HRESULT hr = dev2->GetLightState(D3DLIGHTSTATETYPE(i), &v);
                if (SUCCEEDED(hr)) P(" L%d=0x%lx", i, v);
                else P(" L%d:0x%08lx", i, hr);
            }
            P("\n");
            dev2->Release();
        }
        if (rt4) rt4->Release();
        rt->Release();
    }
    if (d3d2) d3d2->Release();
    if (d3d3) d3d3->Release();
    dd1->Release();
}

// ---------------------------------------------------------------- calls with bad arguments

struct Devices {
    IDirectDraw7* dd7 = nullptr;
    IDirect3D7* d3d7 = nullptr;
    IDirectDrawSurface7* rt7 = nullptr;
    IDirect3DDevice7* dev7 = nullptr;
    IDirectDraw* dd1 = nullptr;
    IDirectDraw4* dd4 = nullptr;
    IDirect3D3* d3d3 = nullptr;
    IDirectDrawSurface4* rt4 = nullptr;
    IDirect3DDevice3* dev3 = nullptr;
    IDirect3DViewport3* vp3 = nullptr;
};

static bool MakeDevices(Devices* d) {
    d->dd7 = NewDD7();
    if (!d->dd7) return false;
    d->dd7->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
    d->dd7->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d->d3d7));
    d->rt7 = Make(d->dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY, 64, 64);
    if (d->d3d7 && d->rt7) d->d3d7->CreateDevice(IID_IDirect3DRGBDevice, d->rt7, &d->dev7);
    DirectDrawCreate(nullptr, &d->dd1, nullptr);
    if (!d->dd1) return false;
    d->dd1->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
    d->dd1->QueryInterface(IID_IDirectDraw4, reinterpret_cast<void**>(&d->dd4));
    d->dd1->QueryInterface(IID_IDirect3D3, reinterpret_cast<void**>(&d->d3d3));
    if (d->dd4 && d->d3d3) {
        DDSURFACEDESC2 sd = {};
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY;
        sd.dwWidth = sd.dwHeight = 64;
        d->dd4->CreateSurface(&sd, &d->rt4, nullptr);
        if (d->rt4) d->d3d3->CreateDevice(IID_IDirect3DRGBDevice, d->rt4, &d->dev3, nullptr);
        if (d->dev3 && SUCCEEDED(d->d3d3->CreateViewport(&d->vp3, nullptr))) {
            d->dev3->AddViewport(d->vp3);
            D3DVIEWPORT2 vd = {};
            vd.dwSize = sizeof(vd);
            vd.dwWidth = vd.dwHeight = 64;
            vd.dvClipX = -1.0f, vd.dvClipY = 1.0f, vd.dvClipWidth = vd.dvClipHeight = 2.0f;
            vd.dvMaxZ = 1.0f;
            d->vp3->SetViewport2(&vd);
            d->dev3->SetCurrentViewport(d->vp3);
        }
    }
    return d->dev7 && d->dev3;
}

struct Try {
    const char* name;
    HRESULT (*run)(Devices&);
};

static IDirect3DVertexBuffer7* Vb(Devices& d, DWORD fvf, DWORD caps = D3DVBCAPS_SYSTEMMEMORY) {
    D3DVERTEXBUFFERDESC vd = {};
    vd.dwSize = sizeof(vd);
    vd.dwCaps = caps;
    vd.dwFVF = fvf;
    vd.dwNumVertices = 8;
    IDirect3DVertexBuffer7* vb = nullptr;
    d.d3d7->CreateVertexBuffer(&vd, &vb, 0);
    return vb;
}

// A 64x64 system-memory 3D surface of the DirectDrawCreate object with no device on it, asked
// for a software device through one of its interfaces (`iid`); its IDirectDrawSurface4 is kept.
static IDirectDrawSurface4* g_deviceSurface4;
static HRESULT DeviceThroughSurface(Devices& d, REFIID iid) {
    DDSURFACEDESC sd = {};
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY;
    sd.dwWidth = sd.dwHeight = 64;
    IDirectDrawSurface* s = nullptr;
    if (FAILED(d.dd1->CreateSurface(&sd, &s, nullptr))) return HRESULT(0xDEAD0002);
    s->QueryInterface(IID_IDirectDrawSurface4, reinterpret_cast<void**>(&g_deviceSurface4));
    IUnknown* through = nullptr;
    if (FAILED(s->QueryInterface(iid, reinterpret_cast<void**>(&through)))) return HRESULT(0xDEAD0003);
    void* dev = nullptr;
    return through->QueryInterface(IID_IDirect3DRGBDevice, &dev);
}

static const Try kTries[] = {
    // Direct3D 7, the RGB device.
    {"Device7::GetRenderState(ZENABLE, null)", [](Devices& d) { return d.dev7->GetRenderState(D3DRENDERSTATE_ZENABLE, nullptr); }},
    {"Device7::GetTransform(WORLD, null)", [](Devices& d) { return d.dev7->GetTransform(D3DTRANSFORMSTATE_WORLD, nullptr); }},
    {"Device7::SetTransform(WORLD, null)", [](Devices& d) { return d.dev7->SetTransform(D3DTRANSFORMSTATE_WORLD, nullptr); }},
    {"Device7::GetViewport(null)", [](Devices& d) { return d.dev7->GetViewport(nullptr); }},
    {"Device7::SetViewport(null)", [](Devices& d) { return d.dev7->SetViewport(nullptr); }},
    {"Device7::GetMaterial(null)", [](Devices& d) { return d.dev7->GetMaterial(nullptr); }},
    {"Device7::SetMaterial(null)", [](Devices& d) { return d.dev7->SetMaterial(nullptr); }},
    {"Device7::GetLight(0, null)", [](Devices& d) { return d.dev7->GetLight(0, nullptr); }},
    {"Device7::SetLight(0, null)", [](Devices& d) { return d.dev7->SetLight(0, nullptr); }},
    {"Device7::GetLightEnable(0, null)", [](Devices& d) { return d.dev7->GetLightEnable(0, nullptr); }},
    {"Device7::GetTexture(0, null)", [](Devices& d) { return d.dev7->GetTexture(0, nullptr); }},
    {"Device7::GetTextureStageState(0, COLOROP, null)",
     [](Devices& d) { return d.dev7->GetTextureStageState(0, D3DTSS_COLOROP, nullptr); }},
    {"Device7::GetCaps(null)", [](Devices& d) { return d.dev7->GetCaps(nullptr); }},
    {"Device7::GetClipStatus(null)", [](Devices& d) { return d.dev7->GetClipStatus(nullptr); }},
    {"Device7::GetClipPlane(0, null)", [](Devices& d) { return d.dev7->GetClipPlane(0, nullptr); }},
    {"Device7::GetRenderTarget(null)", [](Devices& d) { return d.dev7->GetRenderTarget(nullptr); }},
    {"Device7::GetDirect3D(null)", [](Devices& d) { return d.dev7->GetDirect3D(nullptr); }},
    {"Device7::EnumTextureFormats(null, null)", [](Devices& d) { return d.dev7->EnumTextureFormats(nullptr, nullptr); }},
    {"Device7::Clear(1, null, TARGET)", [](Devices& d) { return d.dev7->Clear(1, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0); }},
    {"Device7::Clear(0, null, 0)", [](Devices& d) { return d.dev7->Clear(0, nullptr, 0, 0, 1.0f, 0); }},
    {"Device7::Clear(0, null, ZBUFFER) with no Z buffer", [](Devices& d) { return d.dev7->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 1.0f, 0); }},
    {"Device7::Clear(0, null, STENCIL) with no Z buffer", [](Devices& d) { return d.dev7->Clear(0, nullptr, D3DCLEAR_STENCIL, 0, 1.0f, 0); }},
    {"Device7::SetRenderState(0, 1)", [](Devices& d) { return d.dev7->SetRenderState(D3DRENDERSTATETYPE(0), 1); }},
    {"Device7::GetRenderState(0)",
     [](Devices& d) {
         DWORD v = 0;
         return d.dev7->GetRenderState(D3DRENDERSTATETYPE(0), &v);
     }},
    {"Device7::SetRenderState(153, 1)", [](Devices& d) { return d.dev7->SetRenderState(D3DRENDERSTATETYPE(153), 1); }},
    {"Device7::GetRenderState(153)",
     [](Devices& d) {
         DWORD v = 0;
         return d.dev7->GetRenderState(D3DRENDERSTATETYPE(153), &v);
     }},
    {"Device7::SetRenderState(300, 1)", [](Devices& d) { return d.dev7->SetRenderState(D3DRENDERSTATETYPE(300), 1); }},
    {"Device7::GetRenderState(300)",
     [](Devices& d) {
         DWORD v = 0;
         return d.dev7->GetRenderState(D3DRENDERSTATETYPE(300), &v);
     }},
    {"Device7::SetTextureStageState(8, COLOROP, 1)", [](Devices& d) { return d.dev7->SetTextureStageState(8, D3DTSS_COLOROP, 1); }},
    {"Device7::SetTextureStageState(0, 0, 1)", [](Devices& d) { return d.dev7->SetTextureStageState(0, D3DTEXTURESTAGESTATETYPE(0), 1); }},
    {"Device7::SetTextureStageState(0, 30, 1)", [](Devices& d) { return d.dev7->SetTextureStageState(0, D3DTEXTURESTAGESTATETYPE(30), 1); }},
    {"Device7::SetTexture(8, null)", [](Devices& d) { return d.dev7->SetTexture(8, nullptr); }},
    {"Device7::SetTexture(0, a plain surface)",
     [](Devices& d) {
         IDirectDrawSurface7* s = Make(d.dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, 16, 16);
         return s ? d.dev7->SetTexture(0, s) : HRESULT(0xDEAD0002);
     }},
    {"Device7::SetTexture(0, the render target)", [](Devices& d) { return d.dev7->SetTexture(0, d.rt7); }},
    {"Device7::SetLight(0, type 0)",
     [](Devices& d) {
         D3DLIGHT7 l = {};
         l.dltType = D3DLIGHTTYPE(0);
         return d.dev7->SetLight(0, &l);
     }},
    {"Device7::SetLight(0, type 4)",
     [](Devices& d) {
         D3DLIGHT7 l = {};
         l.dltType = D3DLIGHTTYPE(4);
         return d.dev7->SetLight(0, &l);
     }},
    {"Device7::SetLight(0, type 5)",
     [](Devices& d) {
         D3DLIGHT7 l = {};
         l.dltType = D3DLIGHTTYPE(5);
         return d.dev7->SetLight(0, &l);
     }},
    {"Device7::SetLight(0, a point light with range 0 and no attenuation)",
     [](Devices& d) {
         D3DLIGHT7 l = {};
         l.dltType = D3DLIGHT_POINT;
         return d.dev7->SetLight(0, &l);
     }},
    {"Device7::LightEnable(5, TRUE) with no light 5 set", [](Devices& d) { return d.dev7->LightEnable(5, TRUE); }},
    {"Device7::GetLight(5) with no light 5 set",
     [](Devices& d) {
         D3DLIGHT7 l = {};
         return d.dev7->GetLight(5, &l);
     }},
    {"Device7::GetInfo(1, 40 bytes)",
     [](Devices& d) {
         BYTE b[64] = {};
         return d.dev7->GetInfo(1, b, 40);
     }},
    {"Device7::GetInfo(2, 40 bytes)",
     [](Devices& d) {
         BYTE b[64] = {};
         return d.dev7->GetInfo(2, b, 40);
     }},
    {"Device7::GetInfo(3, 20 bytes)",
     [](Devices& d) {
         BYTE b[64] = {};
         return d.dev7->GetInfo(3, b, 20);
     }},
    {"Device7::GetInfo(9, 16 bytes)",
     [](Devices& d) {
         BYTE b[64] = {};
         return d.dev7->GetInfo(9, b, 16);
     }},
    {"Device7::GetInfo(1, null, 0)", [](Devices& d) { return d.dev7->GetInfo(1, nullptr, 0); }},
    {"Device7::ApplyStateBlock(12345)", [](Devices& d) { return d.dev7->ApplyStateBlock(12345); }},
    {"Device7::CaptureStateBlock(12345)", [](Devices& d) { return d.dev7->CaptureStateBlock(12345); }},
    {"Device7::DeleteStateBlock(12345)", [](Devices& d) { return d.dev7->DeleteStateBlock(12345); }},
    {"Device7::ApplyStateBlock(0)", [](Devices& d) { return d.dev7->ApplyStateBlock(0); }},
    {"Device7::EndStateBlock without a Begin",
     [](Devices& d) {
         DWORD h = 0;
         return d.dev7->EndStateBlock(&h);
     }},
    {"Device7::CreateStateBlock(0)",
     [](Devices& d) {
         DWORD h = 0;
         return d.dev7->CreateStateBlock(D3DSTATEBLOCKTYPE(0), &h);
     }},
    {"Device7::EndScene without a BeginScene", [](Devices& d) { return d.dev7->EndScene(); }},
    {"Device7::BeginScene twice",
     [](Devices& d) {
         d.dev7->BeginScene();
         return d.dev7->BeginScene();
     }},
    {"Device7::DrawPrimitive outside a scene",
     [](Devices& d) {
         static D3DTLVERTEX v[3] = {};
         return d.dev7->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, v, 3, 0);
     }},
    {"Device7::DrawPrimitive with 0 vertices",
     [](Devices& d) {
         static D3DTLVERTEX v[3] = {};
         d.dev7->BeginScene();
         return d.dev7->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, v, 0, 0);
     }},
    {"Device7::DrawPrimitive with null vertices",
     [](Devices& d) {
         d.dev7->BeginScene();
         return d.dev7->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, nullptr, 3, 0);
     }},
    {"Device7::DrawPrimitive with primitive type 0",
     [](Devices& d) {
         static D3DTLVERTEX v[3] = {};
         d.dev7->BeginScene();
         return d.dev7->DrawPrimitive(D3DPRIMITIVETYPE(0), D3DFVF_TLVERTEX, v, 3, 0);
     }},
    {"Device7::DrawPrimitive with primitive type 7",
     [](Devices& d) {
         static D3DTLVERTEX v[3] = {};
         d.dev7->BeginScene();
         return d.dev7->DrawPrimitive(D3DPRIMITIVETYPE(7), D3DFVF_TLVERTEX, v, 3, 0);
     }},
    {"Device7::DrawPrimitive, a triangle list of 4 vertices",
     [](Devices& d) {
         static D3DTLVERTEX v[4] = {};
         d.dev7->BeginScene();
         return d.dev7->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, v, 4, 0);
     }},
    {"Device7::DrawPrimitive with FVF 0",
     [](Devices& d) {
         static D3DTLVERTEX v[3] = {};
         d.dev7->BeginScene();
         return d.dev7->DrawPrimitive(D3DPT_TRIANGLELIST, 0, v, 3, 0);
     }},
    {"Device7::DrawPrimitive with FVF XYZ | XYZRHW",
     [](Devices& d) {
         static float v[64] = {};
         d.dev7->BeginScene();
         return d.dev7->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZ | D3DFVF_XYZRHW, v, 3, 0);
     }},
    {"Device7::DrawPrimitive with 9 texture coordinate sets",
     [](Devices& d) {
         static float v[3 * 32] = {};
         d.dev7->BeginScene();
         return d.dev7->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | (9 << D3DFVF_TEXCOUNT_SHIFT), v, 3, 0);
     }},
    {"Device7::DrawPrimitive with FVF reserved bit 0x2000",
     [](Devices& d) {
         static float v[64] = {};
         d.dev7->BeginScene();
         return d.dev7->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | 0x2000, v, 3, 0);
     }},
    {"Device7::DrawIndexedPrimitive with an index past the vertices",
     [](Devices& d) {
         static D3DTLVERTEX v[3] = {};
         static WORD idx[3] = {0, 1, 9};
         d.dev7->BeginScene();
         return d.dev7->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, v, 3, idx, 3, 0);
     }},
    {"Device7::DrawPrimitiveVB with a null buffer",
     [](Devices& d) {
         d.dev7->BeginScene();
         return d.dev7->DrawPrimitiveVB(D3DPT_TRIANGLELIST, nullptr, 0, 3, 0);
     }},
    {"Device7::DrawPrimitiveVB past the buffer's end",
     [](Devices& d) {
         IDirect3DVertexBuffer7* vb = Vb(d, D3DFVF_TLVERTEX);
         if (!vb) return HRESULT(0xDEAD0002);
         d.dev7->BeginScene();
         return d.dev7->DrawPrimitiveVB(D3DPT_TRIANGLELIST, vb, 6, 6, 0);
     }},
    {"Device7::DrawPrimitiveVB with the buffer locked",
     [](Devices& d) {
         IDirect3DVertexBuffer7* vb = Vb(d, D3DFVF_TLVERTEX);
         if (!vb) return HRESULT(0xDEAD0002);
         void* p = nullptr;
         vb->Lock(0, &p, nullptr);
         d.dev7->BeginScene();
         return d.dev7->DrawPrimitiveVB(D3DPT_TRIANGLELIST, vb, 0, 3, 0);
     }},
    {"Device7::Load(null, null, null, null, 0)", [](Devices& d) { return d.dev7->Load(nullptr, nullptr, nullptr, nullptr, 0); }},
    {"Device7::SetRenderTarget(null)", [](Devices& d) { return d.dev7->SetRenderTarget(nullptr, 0); }},
    {"Device7::SetClipPlane(40, plane)",
     [](Devices& d) {
         float p[4] = {0, 0, 1, 0};
         return d.dev7->SetClipPlane(40, p);
     }},
    {"Device7::ComputeSphereVisibility with null arrays", [](Devices& d) { return d.dev7->ComputeSphereVisibility(nullptr, nullptr, 1, 0, nullptr); }},
    {"Device7::ValidateDevice(null)", [](Devices& d) { return d.dev7->ValidateDevice(nullptr); }},
    {"Device7::PreLoad(null)", [](Devices& d) { return d.dev7->PreLoad(nullptr); }},
    {"Device7::PreLoad(a plain surface)",
     [](Devices& d) {
         IDirectDrawSurface7* s = Make(d.dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, 16, 16);
         return s ? d.dev7->PreLoad(s) : HRESULT(0xDEAD0002);
     }},
    {"Direct3D7::CreateVertexBuffer(null desc)",
     [](Devices& d) {
         IDirect3DVertexBuffer7* vb = nullptr;
         return d.d3d7->CreateVertexBuffer(nullptr, &vb, 0);
     }},
    {"Direct3D7::CreateVertexBuffer(null out)",
     [](Devices& d) {
         D3DVERTEXBUFFERDESC vd = {};
         vd.dwSize = sizeof(vd);
         vd.dwFVF = D3DFVF_XYZ;
         vd.dwNumVertices = 4;
         return d.d3d7->CreateVertexBuffer(&vd, nullptr, 0);
     }},
    {"Direct3D7::CreateVertexBuffer with size 0 in the desc",
     [](Devices& d) {
         D3DVERTEXBUFFERDESC vd = {};
         vd.dwFVF = D3DFVF_XYZ;
         vd.dwNumVertices = 4;
         IDirect3DVertexBuffer7* vb = nullptr;
         return d.d3d7->CreateVertexBuffer(&vd, &vb, 0);
     }},
    {"Direct3D7::CreateVertexBuffer with 0 vertices",
     [](Devices& d) {
         D3DVERTEXBUFFERDESC vd = {};
         vd.dwSize = sizeof(vd);
         vd.dwFVF = D3DFVF_XYZ;
         IDirect3DVertexBuffer7* vb = nullptr;
         return d.d3d7->CreateVertexBuffer(&vd, &vb, 0);
     }},
    {"Direct3D7::CreateVertexBuffer with 70000 vertices",
     [](Devices& d) {
         D3DVERTEXBUFFERDESC vd = {};
         vd.dwSize = sizeof(vd);
         vd.dwFVF = D3DFVF_XYZ;
         vd.dwNumVertices = 70000;
         IDirect3DVertexBuffer7* vb = nullptr;
         return d.d3d7->CreateVertexBuffer(&vd, &vb, 0);
     }},
    {"Direct3D7::CreateVertexBuffer with FVF 0",
     [](Devices& d) {
         D3DVERTEXBUFFERDESC vd = {};
         vd.dwSize = sizeof(vd);
         vd.dwNumVertices = 4;
         IDirect3DVertexBuffer7* vb = nullptr;
         return d.d3d7->CreateVertexBuffer(&vd, &vb, 0);
     }},
    {"Direct3D7::CreateVertexBuffer with flags 1",
     [](Devices& d) {
         D3DVERTEXBUFFERDESC vd = {};
         vd.dwSize = sizeof(vd);
         vd.dwFVF = D3DFVF_XYZ;
         vd.dwNumVertices = 4;
         IDirect3DVertexBuffer7* vb = nullptr;
         return d.d3d7->CreateVertexBuffer(&vd, &vb, 1);
     }},
    {"Direct3D7::EnumDevices(null)", [](Devices& d) { return d.d3d7->EnumDevices(nullptr, nullptr); }},
    {"Direct3D7::EnumZBufferFormats(RGB, null)", [](Devices& d) { return d.d3d7->EnumZBufferFormats(IID_IDirect3DRGBDevice, nullptr, nullptr); }},
    {"Direct3D7::EnumZBufferFormats(an unknown GUID)", [](Devices& d) { return d.d3d7->EnumZBufferFormats(IID_IDirectDraw, ZCb, nullptr); }},
    {"Direct3D7::CreateDevice(RGB, target, null)", [](Devices& d) { return d.d3d7->CreateDevice(IID_IDirect3DRGBDevice, d.rt7, nullptr); }},
    {"Direct3D7::CreateDevice(RGB, null surface)",
     [](Devices& d) {
         IDirect3DDevice7* dev = nullptr;
         return d.d3d7->CreateDevice(IID_IDirect3DRGBDevice, nullptr, &dev);
     }},
    {"Direct3D7::CreateDevice(an unknown GUID)",
     [](Devices& d) {
         IDirect3DDevice7* dev = nullptr;
         return d.d3d7->CreateDevice(IID_IDirectDraw, d.rt7, &dev);
     }},
    {"Direct3D7::CreateDevice(MMX)",
     [](Devices& d) {
         IDirect3DDevice7* dev = nullptr;
         return d.d3d7->CreateDevice(IID_IDirect3DMMXDevice, d.rt7, &dev);
     }},
    {"Direct3D7::CreateDevice(Ramp)",
     [](Devices& d) {
         IDirect3DDevice7* dev = nullptr;
         return d.d3d7->CreateDevice(IID_IDirect3DRampDevice, d.rt7, &dev);
     }},
    {"Direct3D7::CreateDevice(Reference)",
     [](Devices& d) {
         IDirect3DDevice7* dev = nullptr;
         return d.d3d7->CreateDevice(IID_IDirect3DRefDevice, d.rt7, &dev);
     }},
    {"Direct3D7::CreateDevice(HAL) on a system-memory target",
     [](Devices& d) {
         IDirect3DDevice7* dev = nullptr;
         return d.d3d7->CreateDevice(IID_IDirect3DHALDevice, d.rt7, &dev);
     }},
    {"Direct3D7::CreateDevice(RGB) on a video-memory target",
     [](Devices& d) {
         IDirectDrawSurface7* s = Make(d.dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY, 64, 64);
         IDirect3DDevice7* dev = nullptr;
         return s ? d.d3d7->CreateDevice(IID_IDirect3DRGBDevice, s, &dev) : HRESULT(0xDEAD0002);
     }},
    {"Direct3D7::CreateDevice(RGB) on a target without DDSCAPS_3DDEVICE",
     [](Devices& d) {
         IDirectDrawSurface7* s = Make(d.dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, 64, 64);
         IDirect3DDevice7* dev = nullptr;
         return s ? d.d3d7->CreateDevice(IID_IDirect3DRGBDevice, s, &dev) : HRESULT(0xDEAD0002);
     }},
    {"Direct3D7::CreateDevice(RGB) a second time on the same target",
     [](Devices& d) {
         IDirect3DDevice7* dev = nullptr;
         return d.d3d7->CreateDevice(IID_IDirect3DRGBDevice, d.rt7, &dev);
     }},
    {"Direct3D7::EvictManagedTextures", [](Devices& d) { return d.d3d7->EvictManagedTextures(); }},
    {"VertexBuffer7::ProcessVertices from a transformed (XYZRHW) source",
     [](Devices& d) {
         IDirect3DVertexBuffer7 *dst = Vb(d, D3DFVF_TLVERTEX), *src = Vb(d, D3DFVF_TLVERTEX);
         return dst && src ? dst->ProcessVertices(D3DVOP_TRANSFORM, 0, 3, src, 0, d.dev7, 0) : HRESULT(0xDEAD0002);
     }},
    {"VertexBuffer7::ProcessVertices with CLIP into a DONOTCLIP buffer",
     [](Devices& d) {
         IDirect3DVertexBuffer7 *dst = Vb(d, D3DFVF_TLVERTEX, D3DVBCAPS_SYSTEMMEMORY | D3DVBCAPS_DONOTCLIP), *src = Vb(d, D3DFVF_XYZ);
         return dst && src ? dst->ProcessVertices(D3DVOP_TRANSFORM | D3DVOP_CLIP, 0, 3, src, 0, d.dev7, 0) : HRESULT(0xDEAD0002);
     }},
    {"VertexBuffer7::ProcessVertices into an untransformed buffer",
     [](Devices& d) {
         IDirect3DVertexBuffer7 *dst = Vb(d, D3DFVF_XYZ), *src = Vb(d, D3DFVF_XYZ);
         return dst && src ? dst->ProcessVertices(D3DVOP_TRANSFORM, 0, 3, src, 0, d.dev7, 0) : HRESULT(0xDEAD0002);
     }},
    {"VertexBuffer7::ProcessVertices with no operation flags",
     [](Devices& d) {
         IDirect3DVertexBuffer7 *dst = Vb(d, D3DFVF_TLVERTEX), *src = Vb(d, D3DFVF_XYZ);
         return dst && src ? dst->ProcessVertices(0, 0, 3, src, 0, d.dev7, 0) : HRESULT(0xDEAD0002);
     }},
    {"VertexBuffer7::ProcessVertices past the source's end",
     [](Devices& d) {
         IDirect3DVertexBuffer7 *dst = Vb(d, D3DFVF_TLVERTEX), *src = Vb(d, D3DFVF_XYZ);
         return dst && src ? dst->ProcessVertices(D3DVOP_TRANSFORM, 0, 6, src, 6, d.dev7, 0) : HRESULT(0xDEAD0002);
     }},
    {"VertexBuffer7::ProcessVertices with a null device",
     [](Devices& d) {
         IDirect3DVertexBuffer7 *dst = Vb(d, D3DFVF_TLVERTEX), *src = Vb(d, D3DFVF_XYZ);
         return dst && src ? dst->ProcessVertices(D3DVOP_TRANSFORM, 0, 3, src, 0, nullptr, 0) : HRESULT(0xDEAD0002);
     }},
    {"VertexBuffer7::ProcessVerticesStrided from transformed vertices",
     [](Devices& d) {
         IDirect3DVertexBuffer7* dst = Vb(d, D3DFVF_TLVERTEX);
         static D3DTLVERTEX v[3] = {};
         D3DDRAWPRIMITIVESTRIDEDDATA s = {};
         s.position.lpvData = v;
         s.position.dwStride = sizeof(D3DTLVERTEX);
         return dst ? dst->ProcessVerticesStrided(D3DVOP_TRANSFORM, 0, 3, &s, D3DFVF_XYZRHW, d.dev7, 0) : HRESULT(0xDEAD0002);
     }},
    {"VertexBuffer7::Lock twice",
     [](Devices& d) {
         IDirect3DVertexBuffer7* vb = Vb(d, D3DFVF_XYZ);
         void* p = nullptr;
         if (!vb) return HRESULT(0xDEAD0002);
         vb->Lock(0, &p, nullptr);
         return vb->Lock(0, &p, nullptr);
     }},
    {"VertexBuffer7::Unlock without a Lock",
     [](Devices& d) {
         IDirect3DVertexBuffer7* vb = Vb(d, D3DFVF_XYZ);
         return vb ? vb->Unlock() : HRESULT(0xDEAD0002);
     }},
    {"VertexBuffer7::Lock(null out)",
     [](Devices& d) {
         IDirect3DVertexBuffer7* vb = Vb(d, D3DFVF_XYZ);
         return vb ? vb->Lock(0, nullptr, nullptr) : HRESULT(0xDEAD0002);
     }},
    {"VertexBuffer7::Optimize, then Lock",
     [](Devices& d) {
         IDirect3DVertexBuffer7* vb = Vb(d, D3DFVF_XYZ);
         void* p = nullptr;
         if (!vb) return HRESULT(0xDEAD0002);
         const HRESULT o = vb->Optimize(d.dev7, 0);
         return FAILED(o) ? o : vb->Lock(0, &p, nullptr);
     }},
    {"VertexBuffer7::GetVertexBufferDesc(null)",
     [](Devices& d) {
         IDirect3DVertexBuffer7* vb = Vb(d, D3DFVF_XYZ);
         return vb ? vb->GetVertexBufferDesc(nullptr) : HRESULT(0xDEAD0002);
     }},
    // Direct3D 3, the RGB device.
    {"Device3::GetRenderState(ZENABLE, null)", [](Devices& d) { return d.dev3->GetRenderState(D3DRENDERSTATE_ZENABLE, nullptr); }},
    {"Device3::GetLightState(AMBIENT, null)", [](Devices& d) { return d.dev3->GetLightState(D3DLIGHTSTATE_AMBIENT, nullptr); }},
    {"Device3::SetLightState(0, 1)", [](Devices& d) { return d.dev3->SetLightState(D3DLIGHTSTATETYPE(0), 1); }},
    {"Device3::GetLightState(0)",
     [](Devices& d) {
         DWORD v = 0;
         return d.dev3->GetLightState(D3DLIGHTSTATETYPE(0), &v);
     }},
    {"Device3::SetLightState(9, 1)", [](Devices& d) { return d.dev3->SetLightState(D3DLIGHTSTATETYPE(9), 1); }},
    {"Device3::SetLightState(MATERIAL, 12345)", [](Devices& d) { return d.dev3->SetLightState(D3DLIGHTSTATE_MATERIAL, 12345); }},
    {"Device3::SetRenderState(0, 1)", [](Devices& d) { return d.dev3->SetRenderState(D3DRENDERSTATETYPE(0), 1); }},
    {"Device3::SetRenderState(136, 1)", [](Devices& d) { return d.dev3->SetRenderState(D3DRENDERSTATETYPE(136), 1); }},
    {"Device3::GetRenderState(136)",
     [](Devices& d) {
         DWORD v = 0;
         return d.dev3->GetRenderState(D3DRENDERSTATETYPE(136), &v);
     }},
    {"Device3::SetRenderState(300, 1)", [](Devices& d) { return d.dev3->SetRenderState(D3DRENDERSTATETYPE(300), 1); }},
    {"Device3::GetRenderState(300)",
     [](Devices& d) {
         DWORD v = 0;
         return d.dev3->GetRenderState(D3DRENDERSTATETYPE(300), &v);
     }},
    {"Device3::SetRenderState(TEXTUREHANDLE, 12345)", [](Devices& d) { return d.dev3->SetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, 12345); }},
    {"Device3::GetTexture(0, null)", [](Devices& d) { return d.dev3->GetTexture(0, nullptr); }},
    {"Device3::SetTexture(8, null)", [](Devices& d) { return d.dev3->SetTexture(8, nullptr); }},
    {"Device3::GetCaps(null, null)", [](Devices& d) { return d.dev3->GetCaps(nullptr, nullptr); }},
    {"Device3::GetCaps with size 0 in both",
     [](Devices& d) {
         static D3DDEVICEDESC a, b;
         return d.dev3->GetCaps(&a, &b);
     }},
    {"Device3::GetCurrentViewport(null)", [](Devices& d) { return d.dev3->GetCurrentViewport(nullptr); }},
    {"Device3::SetCurrentViewport(null)", [](Devices& d) { return d.dev3->SetCurrentViewport(nullptr); }},
    {"Device3::AddViewport(null)", [](Devices& d) { return d.dev3->AddViewport(nullptr); }},
    {"Device3::AddViewport of the viewport it has", [](Devices& d) { return d.dev3->AddViewport(d.vp3); }},
    {"Device3::DeleteViewport(null)", [](Devices& d) { return d.dev3->DeleteViewport(nullptr); }},
    {"Device3::DeleteViewport of the current one", [](Devices& d) { return d.dev3->DeleteViewport(d.vp3); }},
    {"Device3::NextViewport(null, out, FIRST)",
     [](Devices& d) {
         IDirect3DViewport3* v = nullptr;
         return d.dev3->NextViewport(nullptr, &v, D3DNEXT_HEAD);
     }},
    {"Device3::NextViewport(null, out, NEXT)",
     [](Devices& d) {
         IDirect3DViewport3* v = nullptr;
         return d.dev3->NextViewport(nullptr, &v, D3DNEXT_NEXT);
     }},
    {"Device3::GetTransform(WORLD, null)", [](Devices& d) { return d.dev3->GetTransform(D3DTRANSFORMSTATE_WORLD, nullptr); }},
    {"Device3::MultiplyTransform(WORLD, null)", [](Devices& d) { return d.dev3->MultiplyTransform(D3DTRANSFORMSTATE_WORLD, nullptr); }},
    {"Device3::GetRenderTarget(null)", [](Devices& d) { return d.dev3->GetRenderTarget(nullptr); }},
    {"Device3::SetRenderTarget(null)", [](Devices& d) { return d.dev3->SetRenderTarget(nullptr, 0); }},
    {"Device3::GetStats(null)", [](Devices& d) { return d.dev3->GetStats(nullptr); }},
    {"Device3::GetStats",
     [](Devices& d) {
         D3DSTATS s = {};
         s.dwSize = sizeof(s);
         return d.dev3->GetStats(&s);
     }},
    {"Device3::EndScene without a BeginScene", [](Devices& d) { return d.dev3->EndScene(); }},
    {"Device3::BeginScene twice",
     [](Devices& d) {
         d.dev3->BeginScene();
         return d.dev3->BeginScene();
     }},
    {"Device3::Begin, then End with no vertices",
     [](Devices& d) {
         d.dev3->BeginScene();
         const HRESULT b = d.dev3->Begin(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, 0);
         return FAILED(b) ? b : d.dev3->End(0);
     }},
    {"Device3::End without a Begin", [](Devices& d) { return d.dev3->End(0); }},
    {"Device3::Vertex without a Begin",
     [](Devices& d) {
         static D3DTLVERTEX v = {};
         return d.dev3->Vertex(&v);
     }},
    {"Device3::DrawPrimitive outside a scene",
     [](Devices& d) {
         static D3DTLVERTEX v[3] = {};
         return d.dev3->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, v, 3, 0);
     }},
    {"Device3::DrawPrimitive with no current viewport",
     [](Devices& d) {
         static D3DTLVERTEX v[3] = {};
         d.dev3->DeleteViewport(d.vp3);
         d.dev3->BeginScene();
         return d.dev3->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, v, 3, 0);
     }},
    {"Device3::DrawPrimitive with FVF 0",
     [](Devices& d) {
         static D3DTLVERTEX v[3] = {};
         d.dev3->BeginScene();
         return d.dev3->DrawPrimitive(D3DPT_TRIANGLELIST, 0, v, 3, 0);
     }},
    {"Device3::DrawPrimitive with 9 texture coordinate sets",
     [](Devices& d) {
         static float v[3 * 32] = {};
         d.dev3->BeginScene();
         return d.dev3->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | (9 << D3DFVF_TEXCOUNT_SHIFT), v, 3, 0);
     }},
    {"Device3::DrawPrimitive with null vertices",
     [](Devices& d) {
         d.dev3->BeginScene();
         return d.dev3->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, nullptr, 3, 0);
     }},
    {"Device3::ComputeSphereVisibility with null arrays", [](Devices& d) { return d.dev3->ComputeSphereVisibility(nullptr, nullptr, 1, 0, nullptr); }},
    {"Device3::ValidateDevice(null)", [](Devices& d) { return d.dev3->ValidateDevice(nullptr); }},
    {"Viewport3::Clear2(1, null, TARGET)", [](Devices& d) { return d.vp3->Clear2(1, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0); }},
    {"Viewport3::Clear2(0, null, TARGET)", [](Devices& d) { return d.vp3->Clear2(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0); }},
    {"Viewport3::Clear2 with ZBUFFER and no Z buffer",
     [](Devices& d) {
         D3DRECT r = {{0}, {0}, {64}, {64}};
         return d.vp3->Clear2(1, &r, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
     }},
    {"Viewport3::Clear with no background material",
     [](Devices& d) {
         D3DRECT r = {{0}, {0}, {64}, {64}};
         return d.vp3->Clear(1, &r, D3DCLEAR_TARGET);
     }},
    {"Viewport3::Clear2 of a viewport on no device",
     [](Devices& d) {
         IDirect3DViewport3* vp = nullptr;
         d.d3d3->CreateViewport(&vp, nullptr);
         D3DRECT r = {{0}, {0}, {64}, {64}};
         return vp ? vp->Clear2(1, &r, D3DCLEAR_TARGET, 0, 1.0f, 0) : HRESULT(0xDEAD0002);
     }},
    {"Viewport3::GetViewport2 of a new viewport",
     [](Devices& d) {
         IDirect3DViewport3* vp = nullptr;
         d.d3d3->CreateViewport(&vp, nullptr);
         D3DVIEWPORT2 v = {};
         v.dwSize = sizeof(v);
         return vp ? vp->GetViewport2(&v) : HRESULT(0xDEAD0002);
     }},
    {"Viewport3::SetViewport2 of a viewport on no device",
     [](Devices& d) {
         IDirect3DViewport3* vp = nullptr;
         d.d3d3->CreateViewport(&vp, nullptr);
         D3DVIEWPORT2 v = {};
         v.dwSize = sizeof(v);
         v.dwWidth = v.dwHeight = 64;
         return vp ? vp->SetViewport2(&v) : HRESULT(0xDEAD0002);
     }},
    {"Viewport3::SetViewport2(null)", [](Devices& d) { return d.vp3->SetViewport2(nullptr); }},
    {"Viewport3::SetViewport2 with size 0",
     [](Devices& d) {
         D3DVIEWPORT2 v = {};
         return d.vp3->SetViewport2(&v);
     }},
    {"Viewport3::SetViewport2 larger than the target",
     [](Devices& d) {
         D3DVIEWPORT2 v = {};
         v.dwSize = sizeof(v);
         v.dwWidth = v.dwHeight = 200;
         v.dvClipWidth = v.dvClipHeight = 2.0f;
         v.dvMaxZ = 1.0f;
         return d.vp3->SetViewport2(&v);
     }},
    {"Viewport3::SetBackground(12345)", [](Devices& d) { return d.vp3->SetBackground(12345); }},
    {"Viewport3::GetBackground with none set",
     [](Devices& d) {
         D3DMATERIALHANDLE h = 0;
         BOOL valid = 7;
         return d.vp3->GetBackground(&h, &valid);
     }},
    {"Viewport3::AddLight(null)", [](Devices& d) { return d.vp3->AddLight(nullptr); }},
    {"Viewport3::DeleteLight(null)", [](Devices& d) { return d.vp3->DeleteLight(nullptr); }},
    {"Viewport3::TransformVertices with null data",
     [](Devices& d) {
         DWORD off = 0;
         return d.vp3->TransformVertices(1, nullptr, D3DTRANSFORM_UNCLIPPED, &off);
     }},
    {"Direct3D3::CreateMaterial(null)", [](Devices& d) { return d.d3d3->CreateMaterial(nullptr, nullptr); }},
    {"Direct3D3::CreateLight(null)", [](Devices& d) { return d.d3d3->CreateLight(nullptr, nullptr); }},
    {"Direct3D3::CreateViewport(null)", [](Devices& d) { return d.d3d3->CreateViewport(nullptr, nullptr); }},
    {"Direct3D3::FindDevice(null, null)", [](Devices& d) { return d.d3d3->FindDevice(nullptr, nullptr); }},
    {"Direct3D3::FindDevice with nothing asked for",
     [](Devices& d) {
         D3DFINDDEVICESEARCH s = {};
         s.dwSize = sizeof(s);
         D3DFINDDEVICERESULT r = {};
         r.dwSize = sizeof(r);
         return d.d3d3->FindDevice(&s, &r);
     }},
    {"Direct3D3::FindDevice for the Ramp GUID",
     [](Devices& d) {
         D3DFINDDEVICESEARCH s = {};
         s.dwSize = sizeof(s);
         s.dwFlags = D3DFDS_GUID;
         s.guid = IID_IDirect3DRampDevice;
         D3DFINDDEVICERESULT r = {};
         r.dwSize = sizeof(r);
         return d.d3d3->FindDevice(&s, &r);
     }},
    {"Direct3D3::FindDevice for the MMX GUID",
     [](Devices& d) {
         D3DFINDDEVICESEARCH s = {};
         s.dwSize = sizeof(s);
         s.dwFlags = D3DFDS_GUID;
         s.guid = IID_IDirect3DMMXDevice;
         D3DFINDDEVICERESULT r = {};
         r.dwSize = sizeof(r);
         return d.d3d3->FindDevice(&s, &r);
     }},
    {"Direct3D3::EnumDevices(null)", [](Devices& d) { return d.d3d3->EnumDevices(nullptr, nullptr); }},
    {"Direct3D3::CreateVertexBuffer(null desc)",
     [](Devices& d) {
         IDirect3DVertexBuffer* vb = nullptr;
         return d.d3d3->CreateVertexBuffer(nullptr, &vb, 0, nullptr);
     }},
    {"Direct3D3::CreateDevice(RGB, target, null)", [](Devices& d) { return d.d3d3->CreateDevice(IID_IDirect3DRGBDevice, d.rt4, nullptr, nullptr); }},
    {"Direct3D3::CreateDevice(Ramp)",
     [](Devices& d) {
         IDirect3DDevice3* dev = nullptr;
         return d.d3d3->CreateDevice(IID_IDirect3DRampDevice, d.rt4, &dev, nullptr);
     }},
    {"Direct3D3::CreateDevice(T&L HAL)",
     [](Devices& d) {
         IDirect3DDevice3* dev = nullptr;
         return d.d3d3->CreateDevice(IID_IDirect3DTnLHalDevice, d.rt4, &dev, nullptr);
     }},
    {"Direct3D3::CreateDevice(Reference)",
     [](Devices& d) {
         IDirect3DDevice3* dev = nullptr;
         return d.d3d3->CreateDevice(IID_IDirect3DRefDevice, d.rt4, &dev, nullptr);
     }},
    {"Direct3D3::CreateDevice(RGB) a second time on the same target",
     [](Devices& d) {
         IDirect3DDevice3* dev = nullptr;
         return d.d3d3->CreateDevice(IID_IDirect3DRGBDevice, d.rt4, &dev, nullptr);
     }},
    {"Direct3D3::EvictManagedTextures", [](Devices& d) { return d.d3d3->EvictManagedTextures(); }},
    {"Direct3D::Initialize (IDirect3D)",
     [](Devices& d) {
         IDirect3D* d1 = nullptr;
         d.dd1->QueryInterface(IID_IDirect3D, reinterpret_cast<void**>(&d1));
         return d1 ? d1->Initialize(IID_IDirect3D) : HRESULT(0xDEAD0002);
     }},
    // DirectDraw.
    {"DirectDraw7::CreateSurface(null desc)",
     [](Devices& d) {
         IDirectDrawSurface7* s = nullptr;
         return d.dd7->CreateSurface(nullptr, &s, nullptr);
     }},
    {"DirectDraw7::CreateSurface(null out)",
     [](Devices& d) {
         DDSURFACEDESC2 sd = {};
         sd.dwSize = sizeof(sd);
         sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
         sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
         sd.dwWidth = sd.dwHeight = 16;
         return d.dd7->CreateSurface(&sd, nullptr, nullptr);
     }},
    {"DirectDraw7::CreateSurface with a DDSURFACEDESC-sized desc",
     [](Devices& d) {
         DDSURFACEDESC2 sd = {};
         sd.dwSize = sizeof(DDSURFACEDESC);
         sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
         sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
         sd.dwWidth = sd.dwHeight = 16;
         IDirectDrawSurface7* s = nullptr;
         return d.dd7->CreateSurface(&sd, &s, nullptr);
     }},
    {"DirectDraw7::CreateSurface 0x0",
     [](Devices& d) {
         HRESULT hr = E_FAIL;
         Make(d.dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, 0, 0, nullptr, &hr);
         return hr;
     }},
    {"DirectDraw7::CreateSurface 70000x1 in system memory",
     [](Devices& d) {
         HRESULT hr = E_FAIL;
         Make(d.dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, 70000, 1, nullptr, &hr);
         return hr;
     }},
    {"DirectDraw7::CreateSurface with no caps",
     [](Devices& d) {
         HRESULT hr = E_FAIL;
         Make(d.dd7, 0, 16, 16, nullptr, &hr);
         return hr;
     }},
    {"DirectDraw7::CreateSurface, a texture of 17x9",
     [](Devices& d) {
         HRESULT hr = E_FAIL;
         Make(d.dd7, DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY, 17, 9, nullptr, &hr);
         return hr;
     }},
    {"DirectDraw7::CreateSurface, a managed texture with SYSTEMMEMORY",
     [](Devices& d) {
         HRESULT hr = E_FAIL;
         Make(d.dd7, DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY, 16, 16, nullptr, &hr, DDSCAPS2_TEXTUREMANAGE);
         return hr;
     }},
    {"DirectDraw7::CreateSurface, TEXTUREMANAGE without TEXTURE",
     [](Devices& d) {
         HRESULT hr = E_FAIL;
         Make(d.dd7, DDSCAPS_OFFSCREENPLAIN, 16, 16, nullptr, &hr, DDSCAPS2_TEXTUREMANAGE);
         return hr;
     }},
    {"DirectDraw7::CreateSurface, a primary in system memory",
     [](Devices& d) {
         DDSURFACEDESC2 sd = {};
         sd.dwSize = sizeof(sd);
         sd.dwFlags = DDSD_CAPS;
         sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY;
         IDirectDrawSurface7* s = nullptr;
         return d.dd7->CreateSurface(&sd, &s, nullptr);
     }},
    {"DirectDraw7::CreateSurface, a flipping primary in normal mode",
     [](Devices& d) {
         DDSURFACEDESC2 sd = {};
         sd.dwSize = sizeof(sd);
         sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
         sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
         sd.dwBackBufferCount = 1;
         IDirectDrawSurface7* s = nullptr;
         return d.dd7->CreateSurface(&sd, &s, nullptr);
     }},
    {"DirectDraw7::CreateSurface, a second primary",
     [](Devices& d) {
         DDSURFACEDESC2 sd = {};
         sd.dwSize = sizeof(sd);
         sd.dwFlags = DDSD_CAPS;
         sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
         IDirectDrawSurface7 *a = nullptr, *b = nullptr;
         d.dd7->CreateSurface(&sd, &a, nullptr);
         return d.dd7->CreateSurface(&sd, &b, nullptr);
     }},
    {"DirectDraw7::SetDisplayMode in normal mode", [](Devices& d) { return d.dd7->SetDisplayMode(640, 480, 16, 0, 0); }},
    {"DirectDraw7::RestoreDisplayMode in normal mode", [](Devices& d) { return d.dd7->RestoreDisplayMode(); }},
    {"DirectDraw7::SetCooperativeLevel(null, EXCLUSIVE | FULLSCREEN)",
     [](Devices& d) { return d.dd7->SetCooperativeLevel(nullptr, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN); }},
    {"DirectDraw7::SetCooperativeLevel(null, 0)", [](Devices& d) { return d.dd7->SetCooperativeLevel(nullptr, 0); }},
    {"DirectDraw7::SetCooperativeLevel(null, NORMAL | EXCLUSIVE)",
     [](Devices& d) { return d.dd7->SetCooperativeLevel(nullptr, DDSCL_NORMAL | DDSCL_EXCLUSIVE); }},
    {"DirectDraw7::SetCooperativeLevel(null, FULLSCREEN)", [](Devices& d) { return d.dd7->SetCooperativeLevel(nullptr, DDSCL_FULLSCREEN); }},
    {"DirectDraw7::SetCooperativeLevel(null, NORMAL | FPUSETUP | MULTITHREADED)",
     [](Devices& d) { return d.dd7->SetCooperativeLevel(nullptr, DDSCL_NORMAL | DDSCL_FPUSETUP | DDSCL_MULTITHREADED); }},
    {"DirectDraw7::SetCooperativeLevel(null, 0x20)", [](Devices& d) { return d.dd7->SetCooperativeLevel(nullptr, 0x20); }},
    {"DirectDraw7::SetCooperativeLevel(null, NORMAL | 0x20)", [](Devices& d) { return d.dd7->SetCooperativeLevel(nullptr, DDSCL_NORMAL | 0x20); }},
    {"DirectDraw7::TestCooperativeLevel before any SetCooperativeLevel",
     [](Devices&) {
         IDirectDraw7* dd = NewDD7();
         return dd ? dd->TestCooperativeLevel() : HRESULT(0xDEAD0002);
     }},
    {"DirectDraw7::CreateSurface before any SetCooperativeLevel",
     [](Devices&) {
         IDirectDraw7* dd = NewDD7();
         HRESULT hr = E_FAIL;
         if (dd) Make(dd, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, 16, 16, nullptr, &hr);
         return hr;
     }},
    {"DirectDraw7::GetCaps(null, null)", [](Devices& d) { return d.dd7->GetCaps(nullptr, nullptr); }},
    {"DirectDraw7::GetDisplayMode(null)", [](Devices& d) { return d.dd7->GetDisplayMode(nullptr); }},
    {"DirectDraw7::GetFourCCCodes(null, null)", [](Devices& d) { return d.dd7->GetFourCCCodes(nullptr, nullptr); }},
    {"DirectDraw7::GetAvailableVidMem(null caps)",
     [](Devices& d) {
         DWORD a = 0, b = 0;
         return d.dd7->GetAvailableVidMem(nullptr, &a, &b);
     }},
    {"DirectDraw7::GetAvailableVidMem for SYSTEMMEMORY",
     [](Devices& d) {
         DDSCAPS2 c = {DDSCAPS_SYSTEMMEMORY, 0, 0, {0}};
         DWORD a = 0, b = 0;
         return d.dd7->GetAvailableVidMem(&c, &a, &b);
     }},
    {"DirectDraw7::GetScanLine(null)", [](Devices& d) { return d.dd7->GetScanLine(nullptr); }},
    {"DirectDraw7::GetVerticalBlankStatus(null)", [](Devices& d) { return d.dd7->GetVerticalBlankStatus(nullptr); }},
    {"DirectDraw7::WaitForVerticalBlank(0)", [](Devices& d) { return d.dd7->WaitForVerticalBlank(0, nullptr); }},
    {"DirectDraw7::GetGDISurface with no primary",
     [](Devices& d) {
         IDirectDrawSurface7* s = nullptr;
         return d.dd7->GetGDISurface(&s);
     }},
    {"DirectDraw7::FlipToGDISurface in normal mode", [](Devices& d) { return d.dd7->FlipToGDISurface(); }},
    {"DirectDraw7::Compact", [](Devices& d) { return d.dd7->Compact(); }},
    {"DirectDraw7::Initialize", [](Devices& d) { return d.dd7->Initialize(nullptr); }},
    {"DirectDraw7::DuplicateSurface of a system-memory surface",
     [](Devices& d) {
         IDirectDrawSurface7* s = Make(d.dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, 16, 16);
         IDirectDrawSurface7* dup = nullptr;
         return s ? d.dd7->DuplicateSurface(s, &dup) : HRESULT(0xDEAD0002);
     }},
    {"DirectDraw7::GetSurfaceFromDC(null)",
     [](Devices& d) {
         IDirectDrawSurface7* s = nullptr;
         return d.dd7->GetSurfaceFromDC(nullptr, &s);
     }},
    {"DirectDraw7::EvaluateMode(0)",
     [](Devices& d) {
         DWORD secs = 0;
         return d.dd7->EvaluateMode(0, &secs);
     }},
    {"DirectDraw7::StartModeTest(null, 0, 0)", [](Devices& d) { return d.dd7->StartModeTest(nullptr, 0, 0); }},
    {"DirectDraw7::CreateClipper(1)",
     [](Devices& d) {
         IDirectDrawClipper* c = nullptr;
         return d.dd7->CreateClipper(1, &c, nullptr);
     }},
    {"DirectDraw7::CreatePalette(8BIT, null entries)",
     [](Devices& d) {
         IDirectDrawPalette* p = nullptr;
         return d.dd7->CreatePalette(DDPCAPS_8BIT, nullptr, &p, nullptr);
     }},
    // Surfaces.
    {"Surface7::Lock(null desc)", [](Devices& d) { return d.rt7->Lock(nullptr, nullptr, DDLOCK_WAIT, nullptr); }},
    {"Surface7::Lock with size 0 in the desc",
     [](Devices& d) {
         DDSURFACEDESC2 sd = {};
         return d.rt7->Lock(nullptr, &sd, DDLOCK_WAIT, nullptr);
     }},
    {"Surface7::Lock twice",
     [](Devices& d) {
         DDSURFACEDESC2 sd = {};
         sd.dwSize = sizeof(sd);
         d.rt7->Lock(nullptr, &sd, DDLOCK_WAIT, nullptr);
         return d.rt7->Lock(nullptr, &sd, DDLOCK_WAIT, nullptr);
     }},
    {"Surface7::Lock of two different rectangles",
     [](Devices& d) {
         DDSURFACEDESC2 sd = {};
         sd.dwSize = sizeof(sd);
         RECT a = {0, 0, 8, 8}, b = {16, 16, 24, 24};
         d.rt7->Lock(&a, &sd, DDLOCK_WAIT, nullptr);
         return d.rt7->Lock(&b, &sd, DDLOCK_WAIT, nullptr);
     }},
    {"Surface7::Lock of a rectangle past the surface",
     [](Devices& d) {
         DDSURFACEDESC2 sd = {};
         sd.dwSize = sizeof(sd);
         RECT a = {32, 32, 100, 100};
         return d.rt7->Lock(&a, &sd, DDLOCK_WAIT, nullptr);
     }},
    {"Surface7::Lock of an empty rectangle",
     [](Devices& d) {
         DDSURFACEDESC2 sd = {};
         sd.dwSize = sizeof(sd);
         RECT a = {8, 8, 8, 8};
         return d.rt7->Lock(&a, &sd, DDLOCK_WAIT, nullptr);
     }},
    {"Surface7::Unlock without a Lock", [](Devices& d) { return d.rt7->Unlock(nullptr); }},
    {"Surface7::Unlock with another rectangle than was locked",
     [](Devices& d) {
         DDSURFACEDESC2 sd = {};
         sd.dwSize = sizeof(sd);
         RECT a = {0, 0, 8, 8}, b = {16, 16, 24, 24};
         d.rt7->Lock(&a, &sd, DDLOCK_WAIT, nullptr);
         return d.rt7->Unlock(&b);
     }},
    {"Surface7::GetDC twice",
     [](Devices& d) {
         HDC a = nullptr, b = nullptr;
         d.rt7->GetDC(&a);
         return d.rt7->GetDC(&b);
     }},
    {"Surface7::GetDC while locked",
     [](Devices& d) {
         DDSURFACEDESC2 sd = {};
         sd.dwSize = sizeof(sd);
         d.rt7->Lock(nullptr, &sd, DDLOCK_WAIT, nullptr);
         HDC a = nullptr;
         return d.rt7->GetDC(&a);
     }},
    {"Surface7::Lock while a DC is out",
     [](Devices& d) {
         HDC a = nullptr;
         d.rt7->GetDC(&a);
         DDSURFACEDESC2 sd = {};
         sd.dwSize = sizeof(sd);
         return d.rt7->Lock(nullptr, &sd, DDLOCK_WAIT, nullptr);
     }},
    {"Surface7::Blt while the destination is locked",
     [](Devices& d) {
         DDSURFACEDESC2 sd = {};
         sd.dwSize = sizeof(sd);
         d.rt7->Lock(nullptr, &sd, DDLOCK_WAIT, nullptr);
         DDBLTFX fx = {};
         fx.dwSize = sizeof(fx);
         return d.rt7->Blt(nullptr, nullptr, nullptr, DDBLT_WAIT | DDBLT_COLORFILL, &fx);
     }},
    {"Surface7::ReleaseDC(null)", [](Devices& d) { return d.rt7->ReleaseDC(nullptr); }},
    {"Surface7::ReleaseDC of a DC that isn't its own",
     [](Devices& d) {
         HDC dc = GetDC(nullptr);
         return d.rt7->ReleaseDC(dc);
     }},
    {"Surface7::Flip on a surface that doesn't flip", [](Devices& d) { return d.rt7->Flip(nullptr, DDFLIP_WAIT); }},
    {"Surface7::Restore of a surface that isn't lost", [](Devices& d) { return d.rt7->Restore(); }},
    {"Surface7::SetColorKey(SRCBLT, null)", [](Devices& d) { return d.rt7->SetColorKey(DDCKEY_SRCBLT, nullptr); }},
    {"Surface7::SetColorKey(0, key)",
     [](Devices& d) {
         DDCOLORKEY k = {};
         return d.rt7->SetColorKey(0, &k);
     }},
    {"Surface7::SetColorKey(SRCBLT | DESTBLT, key)",
     [](Devices& d) {
         DDCOLORKEY k = {};
         return d.rt7->SetColorKey(DDCKEY_SRCBLT | DDCKEY_DESTBLT, &k);
     }},
    {"Surface7::SetColorKey(SRCBLT | COLORSPACE, a range)",
     [](Devices& d) {
         DDCOLORKEY k = {1, 5};
         return d.rt7->SetColorKey(DDCKEY_SRCBLT | DDCKEY_COLORSPACE, &k);
     }},
    {"Surface7::SetColorKey(SRCOVERLAY, key) on a plain surface",
     [](Devices& d) {
         DDCOLORKEY k = {};
         return d.rt7->SetColorKey(DDCKEY_SRCOVERLAY, &k);
     }},
    {"Surface7::GetColorKey(SRCBLT) with none set",
     [](Devices& d) {
         DDCOLORKEY k = {};
         return d.rt7->GetColorKey(DDCKEY_SRCBLT, &k);
     }},
    {"Surface7::GetPalette with none set",
     [](Devices& d) {
         IDirectDrawPalette* p = nullptr;
         return d.rt7->GetPalette(&p);
     }},
    {"Surface7::GetClipper with none set",
     [](Devices& d) {
         IDirectDrawClipper* c = nullptr;
         return d.rt7->GetClipper(&c);
     }},
    {"Surface7::SetPalette of an 8-bit palette on a surface that isn't palettized",
     [](Devices& d) {
         PALETTEENTRY e[256] = {};
         IDirectDrawPalette* p = nullptr;
         d.dd7->CreatePalette(DDPCAPS_8BIT, e, &p, nullptr);
         return p ? d.rt7->SetPalette(p) : HRESULT(0xDEAD0002);
     }},
    {"Surface7::AddAttachedSurface of itself", [](Devices& d) { return d.rt7->AddAttachedSurface(d.rt7); }},
    {"Surface7::AddAttachedSurface of a plain surface",
     [](Devices& d) {
         IDirectDrawSurface7* s = Make(d.dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, 64, 64);
         return s ? d.rt7->AddAttachedSurface(s) : HRESULT(0xDEAD0002);
     }},
    {"Surface7::AddAttachedSurface of a Z buffer of another size",
     [](Devices& d) {
         DDSURFACEDESC2 zd = {};
         zd.dwSize = sizeof(zd);
         zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
         zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_SYSTEMMEMORY;
         zd.dwWidth = zd.dwHeight = 32;
         zd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
         zd.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
         zd.ddpfPixelFormat.dwZBufferBitDepth = 16;
         zd.ddpfPixelFormat.dwZBitMask = 0xFFFF;
         IDirectDrawSurface7* z = nullptr;
         d.dd7->CreateSurface(&zd, &z, nullptr);
         return z ? d.rt7->AddAttachedSurface(z) : HRESULT(0xDEAD0002);
     }},
    {"Surface7::AddAttachedSurface of a second Z buffer",
     [](Devices& d) {
         DDSURFACEDESC2 zd = {};
         zd.dwSize = sizeof(zd);
         zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
         zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_SYSTEMMEMORY;
         zd.dwWidth = zd.dwHeight = 64;
         zd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
         zd.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
         zd.ddpfPixelFormat.dwZBufferBitDepth = 16;
         zd.ddpfPixelFormat.dwZBitMask = 0xFFFF;
         IDirectDrawSurface7 *a = nullptr, *b = nullptr;
         d.dd7->CreateSurface(&zd, &a, nullptr);
         d.dd7->CreateSurface(&zd, &b, nullptr);
         if (!a || !b) return HRESULT(0xDEAD0002);
         d.rt7->AddAttachedSurface(a);
         return d.rt7->AddAttachedSurface(b);
     }},
    {"Surface7::DeleteAttachedSurface of a surface that isn't attached",
     [](Devices& d) {
         IDirectDrawSurface7* s = Make(d.dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, 64, 64);
         return s ? d.rt7->DeleteAttachedSurface(0, s) : HRESULT(0xDEAD0002);
     }},
    {"Surface7::DeleteAttachedSurface(null) with nothing attached", [](Devices& d) { return d.rt7->DeleteAttachedSurface(0, nullptr); }},
    {"Surface7::GetAttachedSurface with nothing attached",
     [](Devices& d) {
         DDSCAPS2 c = {DDSCAPS_ZBUFFER, 0, 0, {0}};
         IDirectDrawSurface7* s = nullptr;
         return d.rt7->GetAttachedSurface(&c, &s);
     }},
    {"Surface7::SetSurfaceDesc on a surface with its own memory",
     [](Devices& d) {
         static DWORD mem[64 * 64];
         DDSURFACEDESC2 sd = {};
         sd.dwSize = sizeof(sd);
         sd.dwFlags = DDSD_LPSURFACE;
         sd.lpSurface = mem;
         return d.rt7->SetSurfaceDesc(&sd, 0);
     }},
    {"Surface7::SetPrivateData, then GetPrivateData with a buffer too small",
     [](Devices& d) {
         DWORD v = 5, out = 0, size = 2;
         d.rt7->SetPrivateData(IID_IDirectDraw, &v, sizeof(v), 0);
         return d.rt7->GetPrivateData(IID_IDirectDraw, &out, &size);
     }},
    {"Surface7::GetPrivateData of a tag never set",
     [](Devices& d) {
         DWORD out = 0, size = sizeof(out);
         return d.rt7->GetPrivateData(IID_IDirectDraw, &out, &size);
     }},
    {"Surface7::FreePrivateData of a tag never set", [](Devices& d) { return d.rt7->FreePrivateData(IID_IDirectDraw); }},
    {"Surface7::ChangeUniquenessValue", [](Devices& d) { return d.rt7->ChangeUniquenessValue(); }},
    {"Surface7::PageLock, then PageUnlock twice",
     [](Devices& d) {
         d.rt7->PageLock(0);
         d.rt7->PageUnlock(0);
         return d.rt7->PageUnlock(0);
     }},
    {"Surface7::GetBltStatus(0)", [](Devices& d) { return d.rt7->GetBltStatus(0); }},
    {"Surface7::GetFlipStatus(DDGFS_CANFLIP) on a surface that doesn't flip", [](Devices& d) { return d.rt7->GetFlipStatus(DDGFS_CANFLIP); }},
    {"Surface7::IsLost", [](Devices& d) { return d.rt7->IsLost(); }},
    {"Surface7::Initialize", [](Devices& d) { return d.rt7->Initialize(nullptr, nullptr); }},
    {"Surface7::UpdateOverlay on a plain surface", [](Devices& d) { return d.rt7->UpdateOverlay(nullptr, d.rt7, nullptr, DDOVER_SHOW, nullptr); }},
    {"Surface7::BltBatch", [](Devices& d) { return d.rt7->BltBatch(nullptr, 0, 0); }},
    {"Surface7::QueryInterface for IDirect3DRGBDevice",
     [](Devices& d) {
         void* p = nullptr;
         return d.rt7->QueryInterface(IID_IDirect3DRGBDevice, &p);
     }},
    {"Surface7::QueryInterface for IDirect3DTexture2",
     [](Devices& d) {
         void* p = nullptr;
         return d.rt7->QueryInterface(IID_IDirect3DTexture2, &p);
     }},
    {"Surface7::QueryInterface for IDirectDrawSurface",
     [](Devices& d) {
         void* p = nullptr;
         return d.rt7->QueryInterface(IID_IDirectDrawSurface, &p);
     }},
    {"Surface4::QueryInterface for IDirect3DRGBDevice",
     [](Devices& d) {
         void* p = nullptr;
         return d.rt4->QueryInterface(IID_IDirect3DRGBDevice, &p);
     }},
    {"Surface4::QueryInterface for IDirect3DHALDevice (system memory)",
     [](Devices& d) {
         void* p = nullptr;
         return d.rt4->QueryInterface(IID_IDirect3DHALDevice, &p);
     }},
    {"Surface4::QueryInterface for IDirect3DTexture2",
     [](Devices& d) {
         void* p = nullptr;
         return d.rt4->QueryInterface(IID_IDirect3DTexture2, &p);
     }},
    {"Surface4::QueryInterface for IDirectDrawSurface7",
     [](Devices& d) {
         void* p = nullptr;
         return d.rt4->QueryInterface(IID_IDirectDrawSurface7, &p);
     }},
    {"Surface4::QueryInterface for IDirect3DDevice2 (the interface, not a device GUID)",
     [](Devices& d) {
         void* p = nullptr;
         return d.rt4->QueryInterface(IID_IDirect3DDevice2, &p);
     }},
    {"DirectDraw (v1)::QueryInterface for IDirectDraw7",
     [](Devices& d) {
         void* p = nullptr;
         return d.dd1->QueryInterface(IID_IDirectDraw7, &p);
     }},
    {"DirectDraw7::QueryInterface for IDirectDraw",
     [](Devices& d) {
         void* p = nullptr;
         return d.dd7->QueryInterface(IID_IDirectDraw, &p);
     }},
    {"DirectDraw7::QueryInterface for IDirect3D",
     [](Devices& d) {
         void* p = nullptr;
         return d.dd7->QueryInterface(IID_IDirect3D, &p);
     }},
    {"DirectDraw7::QueryInterface for IDirect3D2",
     [](Devices& d) {
         void* p = nullptr;
         return d.dd7->QueryInterface(IID_IDirect3D2, &p);
     }},
    {"DirectDraw7::QueryInterface for IDirectDrawGammaControl",
     [](Devices& d) {
         void* p = nullptr;
         return d.dd7->QueryInterface(IID_IDirectDrawGammaControl, &p);
     }},
    {"DirectDraw7::QueryInterface for IDirectDrawColorControl",
     [](Devices& d) {
         void* p = nullptr;
         return d.dd7->QueryInterface(IID_IDirectDrawColorControl, &p);
     }},
    {"Surface7::QueryInterface for IDirectDrawColorControl",
     [](Devices& d) {
         void* p = nullptr;
         return d.rt7->QueryInterface(IID_IDirectDrawColorControl, &p);
     }},
    // Follow-ups to the first runs on Windows XP.
    {"Device3::Index without a Begin", [](Devices& d) { return d.dev3->Index(0); }},
    {"Device3::Vertex after a Begin and its End",
     [](Devices& d) {
         static D3DTLVERTEX v[3] = {};
         d.dev3->BeginScene();
         d.dev3->Begin(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, 0);
         for (D3DTLVERTEX& x : v) d.dev3->Vertex(&x);
         d.dev3->End(0);
         return d.dev3->Vertex(&v[0]);
     }},
    {"Device3::Index after a BeginIndexed and its End",
     [](Devices& d) {
         static D3DTLVERTEX v[3] = {};
         d.dev3->BeginScene();
         d.dev3->BeginIndexed(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, v, 3, 0);
         for (WORD i = 0; i < 3; i++) d.dev3->Index(i);
         d.dev3->End(0);
         return d.dev3->Index(0);
     }},
    {"Device3::Index after a Begin (not indexed) and its End",
     [](Devices& d) {
         static D3DTLVERTEX v[3] = {};
         d.dev3->BeginScene();
         d.dev3->Begin(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, 0);
         for (D3DTLVERTEX& x : v) d.dev3->Vertex(&x);
         d.dev3->End(0);
         return d.dev3->Index(0);
     }},
    {"Device3::Index inside a Begin (not indexed)",
     [](Devices& d) {
         d.dev3->BeginScene();
         d.dev3->Begin(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, 0);
         return d.dev3->Index(0);
     }},
    {"Device3::End after a Begin with 2 vertices of a triangle list",
     [](Devices& d) {
         static D3DTLVERTEX v[2] = {};
         d.dev3->BeginScene();
         d.dev3->Begin(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, 0);
         for (D3DTLVERTEX& x : v) d.dev3->Vertex(&x);
         return d.dev3->End(0);
     }},
    {"Device7::GetRenderState(255)",
     [](Devices& d) {
         DWORD v = 0;
         return d.dev7->GetRenderState(D3DRENDERSTATETYPE(255), &v);
     }},
    {"Device7::GetRenderState(256)",
     [](Devices& d) {
         DWORD v = 0;
         return d.dev7->GetRenderState(D3DRENDERSTATETYPE(256), &v);
     }},
    {"Device7::GetRenderState(287)",
     [](Devices& d) {
         DWORD v = 0;
         return d.dev7->GetRenderState(D3DRENDERSTATETYPE(287), &v);
     }},
    {"Device7::GetRenderState(288)",
     [](Devices& d) {
         DWORD v = 0;
         return d.dev7->GetRenderState(D3DRENDERSTATETYPE(288), &v);
     }},
    {"Device7::GetRenderState(1000)",
     [](Devices& d) {
         DWORD v = 0;
         return d.dev7->GetRenderState(D3DRENDERSTATETYPE(1000), &v);
     }},
    {"Device7::SetRenderState(256, 1)", [](Devices& d) { return d.dev7->SetRenderState(D3DRENDERSTATETYPE(256), 1); }},
    {"Device7::SetRenderState(1000, 1)", [](Devices& d) { return d.dev7->SetRenderState(D3DRENDERSTATETYPE(1000), 1); }},
    {"Device7::GetInfo(4, 16 bytes)",
     [](Devices& d) {
         BYTE b[64] = {};
         return d.dev7->GetInfo(4, b, 16);
     }},
    {"Device7::GetInfo(0, 16 bytes)",
     [](Devices& d) {
         BYTE b[64] = {};
         return d.dev7->GetInfo(0, b, 16);
     }},
    {"Device7::DrawIndexedPrimitive with primitive type 0",
     [](Devices& d) {
         static D3DTLVERTEX v[3] = {};
         static WORD idx[3] = {0, 1, 2};
         d.dev7->BeginScene();
         return d.dev7->DrawIndexedPrimitive(D3DPRIMITIVETYPE(0), D3DFVF_TLVERTEX, v, 3, idx, 3, 0);
     }},
    {"Device3::DrawPrimitive with primitive type 0",
     [](Devices& d) {
         static D3DTLVERTEX v[3] = {};
         d.dev3->BeginScene();
         return d.dev3->DrawPrimitive(D3DPRIMITIVETYPE(0), D3DFVF_TLVERTEX, v, 3, 0);
     }},
    {"Direct3D3::CreateVertexBuffer with 0 vertices",
     [](Devices& d) {
         D3DVERTEXBUFFERDESC vd = {};
         vd.dwSize = sizeof(vd);
         vd.dwCaps = D3DVBCAPS_SYSTEMMEMORY;
         vd.dwFVF = D3DFVF_XYZ;
         IDirect3DVertexBuffer* vb = nullptr;
         return d.d3d3->CreateVertexBuffer(&vd, &vb, 0, nullptr);
     }},
    {"IDirectDrawSurface (a 3D surface without a device)::QueryInterface for IDirect3DRGBDevice",
     [](Devices& d) { return DeviceThroughSurface(d, IID_IDirectDrawSurface); }},
    {"IDirectDrawSurface2 of such a surface::QueryInterface for IDirect3DRGBDevice",
     [](Devices& d) { return DeviceThroughSurface(d, IID_IDirectDrawSurface2); }},
    {"IDirectDrawSurface3 of such a surface::QueryInterface for IDirect3DRGBDevice",
     [](Devices& d) { return DeviceThroughSurface(d, IID_IDirectDrawSurface3); }},
    {"IDirectDrawSurface4 of such a surface::QueryInterface for IDirect3DRGBDevice",
     [](Devices& d) { return DeviceThroughSurface(d, IID_IDirectDrawSurface4); }},
    {"IDirectDrawSurface7 of such a surface::QueryInterface for IDirect3DRGBDevice",
     [](Devices& d) { return DeviceThroughSurface(d, IID_IDirectDrawSurface7); }},
    {"IDirectDrawSurface4 of such a surface::QueryInterface for IDirect3DRGBDevice, after IDirectDrawSurface gave one",
     [](Devices& d) {
         DeviceThroughSurface(d, IID_IDirectDrawSurface);
         void* dev = nullptr;
         return g_deviceSurface4 ? g_deviceSurface4->QueryInterface(IID_IDirect3DRGBDevice, &dev) : E_FAIL;
     }},
};

int FmtprobeTry(const char* cmd, CreateExFn createEx) {
    g_createEx = createEx;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const int n = atoi(cmd);
    if (n < 0 || n >= int(sizeof(kTries) / sizeof(kTries[0]))) return int(0xDEAD0000);
    Devices d;
    if (!MakeDevices(&d)) return int(0xDEAD0001);
    return int(kTries[n].run(d));
}

static void BadArguments() {
    P("\nCalls with missing or invalid arguments, each in a process of its own (the call's result; or how the process ended):\n");
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    for (int i = 0; i < int(sizeof(kTries) / sizeof(kTries[0])); i++) {
        char cmd[MAX_PATH + 32];
        snprintf(cmd, sizeof(cmd), "\"%s\" try %d", exe, i);
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            P("  %s: the process could not be started (%lu)\n", kTries[i].name, GetLastError());
            continue;
        }
        DWORD code = 0;
        if (WaitForSingleObject(pi.hProcess, 20000) != WAIT_OBJECT_0) {
            TerminateProcess(pi.hProcess, 1);
            P("  %s: no answer in 20 seconds\n", kTries[i].name);
        } else {
            GetExitCodeProcess(pi.hProcess, &code);
            if ((code & 0xFFFF0000) == 0xDEAD0000) P("  %s: could not be set up (%lu)\n", kTries[i].name, code & 0xFFFF);
            else if ((code >> 28) == 0xC) P("  %s: crashed (exception 0x%08lx)\n", kTries[i].name, code);
            else P("  %s: 0x%08lx\n", kTries[i].name, code);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// ---------------------------------------------------------------- follow-ups
// What the first runs on Windows XP left open.

// An emulation-only object: made first in its process, then beside an ordinary one.
static void EmulationOnly() {
    P("\nDDCREATE_EMULATIONONLY and DDCREATE_HARDWAREONLY objects:\n");
    if (!g_createEx) return;
    GUID* const emuGuid = reinterpret_cast<GUID*>(DDCREATE_EMULATIONONLY);
    IDirectDraw7* emu = nullptr;
    HRESULT hr = g_createEx(emuGuid, reinterpret_cast<void**>(&emu), IID_IDirectDraw7, nullptr);
    P("  emulation-only, the first object of the process: 0x%08lx\n", hr);
    if (emu) {
        P("    SetCooperativeLevel(NORMAL): 0x%08lx\n", emu->SetCooperativeLevel(nullptr, DDSCL_NORMAL));
        DDCAPS drv = {}, hel = {};
        drv.dwSize = hel.dwSize = sizeof(DDCAPS);
        hr = emu->GetCaps(&drv, &hel);
        P("    GetCaps 0x%08lx: driver caps 0x%lx caps2 0x%lx Z depths 0x%lx; emulation caps 0x%lx caps2 0x%lx Z depths 0x%lx\n", hr, drv.dwCaps,
          drv.dwCaps2, drv.dwZBufferBitDepths, hel.dwCaps, hel.dwCaps2, hel.dwZBufferBitDepths);
        IDirect3D7* d3d = nullptr;
        hr = emu->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d));
        P("    IDirect3D7 from it: 0x%08lx\n", hr);
        if (d3d) {
            P("    its devices:\n");
            d3d->EnumDevices(DeviceCb7, nullptr);
            d3d->Release();
        }
        HRESULT vh = E_FAIL, sh = E_FAIL;
        IDirectDrawSurface7* v = Make(emu, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, 16, 16, nullptr, &vh);
        IDirectDrawSurface7* s = Make(emu, DDSCAPS_OFFSCREENPLAIN, 16, 16, nullptr, &sh);
        DDSURFACEDESC2 sd = {};
        sd.dwSize = sizeof(sd);
        if (s) s->GetSurfaceDesc(&sd);
        P("    a video-memory surface: 0x%08lx; one with no memory type: 0x%08lx (caps 0x%lx)\n", vh, sh, sd.ddsCaps.dwCaps);
        if (v) v->Release();
        if (s) s->Release();
    }
    IDirectDraw7* plain = nullptr;
    hr = g_createEx(nullptr, reinterpret_cast<void**>(&plain), IID_IDirectDraw7, nullptr);
    P("  an ordinary object %s: 0x%08lx\n", emu ? "beside it" : "after that", hr);
    if (emu) emu->Release();
    emu = nullptr;
    hr = g_createEx(emuGuid, reinterpret_cast<void**>(&emu), IID_IDirectDraw7, nullptr);
    P("  emulation-only while an ordinary object exists: 0x%08lx\n", hr);
    if (emu) emu->Release();
    IDirectDraw* emu1 = nullptr;
    hr = DirectDrawCreate(emuGuid, &emu1, nullptr);
    P("  the same through DirectDrawCreate: 0x%08lx\n", hr);
    if (emu1) emu1->Release();
    if (plain) plain->Release();
    emu1 = nullptr;
    hr = DirectDrawCreate(emuGuid, &emu1, nullptr);
    P("  through DirectDrawCreate with no other object left: 0x%08lx\n", hr);
    if (emu1) emu1->Release();
    IDirectDraw7* hard = nullptr;
    hr = g_createEx(reinterpret_cast<GUID*>(DDCREATE_HARDWAREONLY), reinterpret_cast<void**>(&hard), IID_IDirectDraw7, nullptr);
    P("  hardware-only: 0x%08lx\n", hr);
    if (hard) hard->Release();
}

// Shrinks by whole and other factors, and BltFast with both colour-key flags and keys set.
static void MoreBlits(IDirectDraw7* dd) {
    P("\nMore blits between 32-bit system-memory surfaces:\n");
    const DWORD kMem = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    static const int kShrink[][2] = {{8, 4}, {6, 3}, {9, 3}, {12, 4}, {8, 2}, {10, 5}, {6, 2}, {16, 4}, {16, 8}, {12, 3}, {15, 5},
                                     {7, 3}, {5, 2}, {16, 6}, {11, 4}, {13, 5}, {16, 7}, {10, 3}, {10, 4}, {16, 3}, {2, 1}, {3, 1},
                                     {4, 1}, {5, 1}, {7, 5}, {9, 7}, {16, 15}, {16, 9}, {6, 4}, {6, 5}, {32, 5}, {32, 12}};
    for (const auto& c : kShrink) {
        IDirectDrawSurface7* src = Make(dd, kMem, c[0], 1, &kX888);
        IDirectDrawSurface7* dst = Make(dd, kMem, c[1], 1, &kX888);
        IDirectDrawSurface7* srcV = Make(dd, kMem, 1, c[0], &kX888);
        IDirectDrawSurface7* dstV = Make(dd, kMem, 1, c[1], &kX888);
        if (src && dst && srcV && dstV) {
            Put(src, Count(c[0]), c[0]);
            Put(srcV, Count(c[0]), 1);
            const HRESULT h = dst->Blt(nullptr, src, nullptr, DDBLT_WAIT, nullptr);
            P("  %d to %d across (0x%08lx):%s\n", c[0], c[1], h, Row(dst, c[1], 1));
            const HRESULT v = dstV->Blt(nullptr, srcV, nullptr, DDBLT_WAIT, nullptr);
            P("  %d to %d down (0x%08lx):%s\n", c[0], c[1], v, Row(dstV, 1, c[1]));
        }
        if (src) src->Release();
        if (dst) dst->Release();
        if (srcV) srcV->Release();
        if (dstV) dstV->Release();
    }
    {  // Shrinks through a clip list (pixels 2-3 and 6-8 of the destination's 8).
        IDirectDrawSurface7* src = Make(dd, kMem, 32, 1, &kX888);
        IDirectDrawSurface7* dst = Make(dd, kMem, 8, 1, &kX888);
        IDirectDrawClipper* clip = nullptr;
        dd->CreateClipper(0, &clip, nullptr);
        if (src && dst && clip) {
            struct {
                RGNDATAHEADER h;
                RECT r[2];
            } rgn = {};
            rgn.h.dwSize = sizeof(RGNDATAHEADER);
            rgn.h.iType = RDH_RECTANGLES;
            rgn.h.nCount = 2;
            rgn.h.nRgnSize = sizeof(rgn.r);
            rgn.h.rcBound = {1, 0, 8, 1};
            rgn.r[0] = {1, 0, 3, 1};
            rgn.r[1] = {5, 0, 8, 1};
            clip->SetClipList(reinterpret_cast<RGNDATA*>(&rgn), 0);
            dst->SetClipper(clip);
            Put(src, Count(32), 32);
            for (int w : {16, 24, 32, 12, 9}) {
                RECT sr = {0, 0, w, 1};
                Put(dst, Zero(8), 8);
                const HRESULT hr = dst->Blt(nullptr, src, &sr, DDBLT_WAIT, nullptr);
                P("  %d to 8 across, clipped to pixels 2-3 and 6-8 (0x%08lx):%s\n", w, hr, Row(dst, 8, 1));
            }
            dst->SetClipper(nullptr);
        }
        if (clip) clip->Release();
        if (src) src->Release();
        if (dst) dst->Release();
    }
    {  // BltFast's colour-key flags, with the source's key on pixel value 2 and the destination's on 7.
        IDirectDrawSurface7* src = Make(dd, kMem, 4, 1, &kX888);
        IDirectDrawSurface7* dst = Make(dd, kMem, 4, 1, &kX888);
        if (src && dst) {
            DDCOLORKEY two = {2, 2}, seven = {7, 7};
            const HRESULT ks = src->SetColorKey(DDCKEY_SRCBLT, &two), kd = dst->SetColorKey(DDCKEY_DESTBLT, &seven);
            P("  with a source key on 2 (0x%08lx) and a destination key on 7 (0x%08lx), onto 7 7 9 9:\n", ks, kd);
            Put(src, Count(4), 4);
            Pixels base = Zero(4);
            base.v[0] = base.v[1] = 7;
            base.v[2] = base.v[3] = 9;
            for (DWORD trans : {DWORD(0), DWORD(1), DWORD(2), DWORD(3)}) {
                Put(dst, base, 4);
                const HRESULT hr = dst->BltFast(0, 0, src, nullptr, trans | DDBLTFAST_WAIT);
                P("    BltFast with dwTrans %lu: 0x%08lx:%s\n", trans, hr, Row(dst, 4, 1));
            }
            for (DWORD flags : {DWORD(DDBLT_KEYSRC), DWORD(DDBLT_KEYDEST), DWORD(DDBLT_KEYSRC | DDBLT_KEYDEST)}) {
                Put(dst, base, 4);
                const HRESULT hr = dst->Blt(nullptr, src, nullptr, flags | DDBLT_WAIT, nullptr);
                P("    Blt with flags 0x%lx: 0x%08lx:%s\n", flags, hr, Row(dst, 4, 1));
            }
        }
        if (src) src->Release();
        if (dst) dst->Release();
    }
}

// TestCooperativeLevel out of exclusive mode: which display mode an object takes as its own.
static void ExpectedMode() {
    P("\nTestCooperativeLevel and the display mode (a new object for each sequence; the desktop's mode is put back after each):\n");
    if (!g_createEx) return;
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "fmtprobe mode window";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "fmtprobe", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    Pump(200);
    DEVMODEA desk = {};
    desk.dmSize = sizeof(desk);
    EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &desk);
    const DWORD dw = desk.dmPelsWidth, dh = desk.dmPelsHeight, dbpp = desk.dmBitsPerPel;
    P("  the desktop: %lux%lu, %lu bits\n", dw, dh, dbpp);
    enum Step { kEnd, kExclusive, kNormal, kSet640, kSet800, kSetDesktop, kRestore, kOther640, kOtherRestore, kOtherRelease };
    static const char* const kStepNames[] = {"", "SetCooperativeLevel(EXCLUSIVE | FULLSCREEN)", "SetCooperativeLevel(NORMAL)",
                                             "SetDisplayMode(640, 480, 16)", "SetDisplayMode(800, 600, 16)",
                                             "SetDisplayMode(the desktop's mode)", "RestoreDisplayMode",
                                             "a second object's SetDisplayMode(640, 480, 16)", "the second object's RestoreDisplayMode",
                                             "the second object released"};
    static const Step kRuns[][8] = {
        {kSet640, kRestore, kSet640, kSetDesktop, kRestore, kEnd},
        {kSetDesktop, kRestore, kEnd},
        {kExclusive, kNormal, kEnd},
        {kExclusive, kSetDesktop, kNormal, kEnd},
        {kExclusive, kSet640, kNormal, kSet640, kRestore, kEnd},
        {kExclusive, kSet640, kRestore, kNormal, kSetDesktop, kEnd},
        {kExclusive, kSet640, kSet800, kRestore, kSet640, kNormal, kEnd},
        {kOther640, kOtherRestore, kOtherRelease, kEnd},
        {kOther640, kSet640, kOtherRestore, kOtherRelease, kEnd},
        {kSet640, kOther640, kOtherRestore, kOtherRelease, kEnd},
    };
    for (const auto& run : kRuns) {
        IDirectDraw7* dd = NewDD7();
        IDirectDraw7* other = nullptr;
        if (!dd) break;
        HRESULT hr = dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
        P("  new, SetCooperativeLevel(NORMAL) 0x%08lx: TestCooperativeLevel 0x%08lx\n", hr, dd->TestCooperativeLevel());
        for (int i = 0; run[i] != kEnd; i++) {
            if (!other && run[i] >= kOther640) {
                other = NewDD7();
                if (other) other->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
            }
            switch (run[i]) {
            case kExclusive: hr = dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN); break;
            case kNormal: hr = dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL); break;
            case kSet640: hr = dd->SetDisplayMode(640, 480, 16, 0, 0); break;
            case kSet800: hr = dd->SetDisplayMode(800, 600, 16, 0, 0); break;
            case kSetDesktop: hr = dd->SetDisplayMode(dw, dh, dbpp, 0, 0); break;
            case kRestore: hr = dd->RestoreDisplayMode(); break;
            case kOther640: hr = other ? other->SetDisplayMode(640, 480, 16, 0, 0) : E_FAIL; break;
            case kOtherRestore: hr = other ? other->RestoreDisplayMode() : E_FAIL; break;
            case kOtherRelease:
                hr = other ? HRESULT(other->Release()) : E_FAIL;
                other = nullptr;
                break;
            default: break;
            }
            Pump(300);
            DDSURFACEDESC2 m = {};
            m.dwSize = sizeof(m);
            dd->GetDisplayMode(&m);
            P("    %s: 0x%08lx; TestCooperativeLevel 0x%08lx; mode %lux%lu, %lu bits\n", kStepNames[run[i]], hr, dd->TestCooperativeLevel(),
              m.dwWidth, m.dwHeight, m.ddpfPixelFormat.dwRGBBitCount);
        }
        if (other) other->Release();
        dd->Release();
        ChangeDisplaySettingsA(nullptr, 0);
        Pump(300);
    }
    DestroyWindow(hwnd);
}

// EnumSurfaces: which interface comes back, in which order, for which surfaces.
struct Listed {
    void* iface[16];
    DWORD w[16];
    int n;
};
static HRESULT WINAPI ListCb(IDirectDrawSurface* s, DDSURFACEDESC* d, void* ctx) {
    Listed* l = static_cast<Listed*>(ctx);
    if (l->n < 16) {
        l->iface[l->n] = s;
        l->w[l->n++] = d ? d->dwWidth : 0;
    }
    if (s) s->Release();
    return DDENUMRET_OK;
}
static HRESULT WINAPI ListCb7(IDirectDrawSurface7* s, DDSURFACEDESC2* d, void* ctx) {
    Listed* l = static_cast<Listed*>(ctx);
    if (l->n < 16) {
        l->iface[l->n] = s;
        l->w[l->n++] = d ? d->dwWidth : 0;
    }
    if (s) s->Release();
    return DDENUMRET_OK;
}

static void MoreSurfaces(IDirectDraw7* dd7, IDirectDraw* dd1) {
    P("\nEnumSurfaces (DOESEXIST | ALL), each entry as the surface's width and which of its interfaces came:\n");
    const DWORD kAll = DDENUMSURFACES_DOESEXIST | DDENUMSURFACES_ALL;
    {
        auto make = [&](DWORD w) {
            DDSURFACEDESC d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
            d.dwWidth = w;
            d.dwHeight = 8;
            IDirectDrawSurface* s = nullptr;
            dd1->CreateSurface(&d, &s, nullptr);
            return s;
        };
        IDirectDrawSurface* a = make(11);
        IDirectDrawSurface* b = make(22);
        IDirectDrawSurface2* a2 = nullptr;
        IDirectDrawSurface3* a3 = nullptr;
        IDirectDrawSurface4* b4 = nullptr;
        auto list = [&](const char* when) {
            Listed l = {};
            const HRESULT hr = dd1->EnumSurfaces(kAll, nullptr, &l, ListCb);
            P("  IDirectDraw, %s (0x%08lx):", when, hr);
            for (int i = 0; i < l.n; i++) {
                const void* p = l.iface[i];
                P(" %lu:%s", l.w[i], p == a || p == b ? "v1" : p == a2 ? "v2" : p == a3 ? "v3" : p == b4 ? "v4" : "another pointer");
            }
            P("\n");
        };
        if (a && b) {
            list("surfaces 11 and 22 made in that order");
            a->QueryInterface(IID_IDirectDrawSurface2, reinterpret_cast<void**>(&a2));
            list("11 asked for IDirectDrawSurface2");
            b->QueryInterface(IID_IDirectDrawSurface4, reinterpret_cast<void**>(&b4));
            list("22 asked for IDirectDrawSurface4");
            a->QueryInterface(IID_IDirectDrawSurface3, reinterpret_cast<void**>(&a3));
            list("11 asked for IDirectDrawSurface3");
            if (a2) {
                a2->AddRef();
                list("11's IDirectDrawSurface2 referenced once more");
                a2->Release();
            }
            const ULONG left = a->Release();
            a = nullptr;
            P("  11's IDirectDrawSurface released: %lu left\n", left);
            list("after that");
            if (a2) a2->Release();
            a2 = nullptr;
            list("11's IDirectDrawSurface2 released");
        }
        if (a3) a3->Release();
        if (a2) a2->Release();
        if (a) a->Release();
        if (b4) b4->Release();
        if (b) b->Release();
        // A flip chain's and a mipmap's members.
        DDSURFACEDESC d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_MIPMAPCOUNT;
        d.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX | DDSCAPS_SYSTEMMEMORY;
        d.dwWidth = d.dwHeight = 32;
        d.dwMipMapCount = 3;
        IDirectDrawSurface* mip = nullptr;
        const HRESULT mh = dd1->CreateSurface(&d, &mip, nullptr);
        Listed l = {};
        const HRESULT eh = dd1->EnumSurfaces(kAll, nullptr, &l, ListCb);
        P("  IDirectDraw, a mipmap of 3 levels (0x%08lx) alone (0x%08lx):", mh, eh);
        for (int i = 0; i < l.n; i++) P(" %lu%s", l.w[i], l.iface[i] == mip ? ":the top level" : "");
        P("\n");
        if (mip) {
            DDSCAPS caps = {DDSCAPS_MIPMAP};
            IDirectDrawSurface* level = nullptr;
            mip->GetAttachedSurface(&caps, &level);
            l = {};
            dd1->EnumSurfaces(kAll, nullptr, &l, ListCb);
            P("  its second level fetched:");
            for (int i = 0; i < l.n; i++) P(" %lu%s", l.w[i], l.iface[i] == mip ? ":the top level" : l.iface[i] == level ? ":the one fetched" : "");
            P("\n");
            if (level) level->Release();
            mip->Release();
        }
    }
    {
        IDirectDrawSurface7* a = Make(dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, 33, 8);
        IDirectDrawSurface7* b = Make(dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, 44, 8);
        IDirectDrawSurface4* a4 = nullptr;
        IDirectDrawSurface* a1 = nullptr;
        auto list = [&](const char* when) {
            Listed l = {};
            const HRESULT hr = dd7->EnumSurfaces(kAll, nullptr, &l, ListCb7);
            P("  IDirectDraw7, %s (0x%08lx):", when, hr);
            for (int i = 0; i < l.n; i++) {
                const void* p = l.iface[i];
                P(" %lu:%s", l.w[i], p == a || p == b ? "v7" : p == a4 ? "v4" : p == a1 ? "v1" : "another pointer");
            }
            P("\n");
        };
        if (a && b) {
            list("surfaces 33 and 44 made in that order");
            a->QueryInterface(IID_IDirectDrawSurface4, reinterpret_cast<void**>(&a4));
            a->QueryInterface(IID_IDirectDrawSurface, reinterpret_cast<void**>(&a1));
            list("33 asked for IDirectDrawSurface4 and IDirectDrawSurface");
        }
        if (a1) a1->Release();
        if (a4) a4->Release();
        if (a) a->Release();
        if (b) b->Release();
    }

    P("\nSetLOD and GetLOD on managed textures:\n");
    for (DWORD levels : {DWORD(1), DWORD(3), DWORD(6)}) {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | (levels > 1 ? DDSD_MIPMAPCOUNT : 0);
        d.ddsCaps.dwCaps = DDSCAPS_TEXTURE | (levels > 1 ? DDSCAPS_MIPMAP | DDSCAPS_COMPLEX : 0);
        d.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
        d.dwWidth = d.dwHeight = 32;
        d.dwMipMapCount = levels > 1 ? levels : 0;
        IDirectDrawSurface7* t = nullptr;
        const HRESULT hr = dd7->CreateSurface(&d, &t, nullptr);
        P("  %lu level%s (0x%08lx):", levels, levels > 1 ? "s" : "", hr);
        if (t) {
            for (DWORD lod : {DWORD(0), DWORD(1), DWORD(2), DWORD(3), DWORD(5), DWORD(6), DWORD(100)}) {
                DWORD got = 0xBAD;
                const HRESULT sh = t->SetLOD(lod), gh = t->GetLOD(&got);
                P(" SetLOD(%lu) 0x%08lx, GetLOD 0x%08lx (%lu);", lod, sh, gh, got);
            }
            t->Release();
        }
        P("\n");
    }

    P("\nSurface sizes, Z buffers through DDSD_ZBUFFERBITDEPTH, palettes:\n");
    static const int kSizes[][2] = {{65535, 1}, {65536, 1}, {1, 65535}, {1, 65536}, {32768, 1}, {0, 1}};
    for (const auto& s : kSizes) {
        HRESULT sh = E_FAIL, vh = E_FAIL;
        IDirectDrawSurface7* sys = Make(dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY, s[0], s[1], nullptr, &sh);
        IDirectDrawSurface7* vid = Make(dd7, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, s[0], s[1], nullptr, &vh);
        P("  %dx%d: system memory 0x%08lx, video memory 0x%08lx\n", s[0], s[1], sh, vh);
        if (sys) sys->Release();
        if (vid) vid->Release();
    }
    for (DWORD depth : {DWORD(8), DWORD(15), DWORD(16), DWORD(24), DWORD(32)}) {
        P("  IDirectDraw, a Z buffer of %lu bits by DDSD_ZBUFFERBITDEPTH:", depth);
        for (DWORD mem : {DWORD(DDSCAPS_SYSTEMMEMORY), DWORD(DDSCAPS_VIDEOMEMORY), DWORD(0)}) {
            DDSURFACEDESC d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_ZBUFFERBITDEPTH;
            d.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | mem;
            d.dwWidth = d.dwHeight = 64;
            d.dwZBufferBitDepth = depth;
            IDirectDrawSurface* z = nullptr;
            const HRESULT hr = dd1->CreateSurface(&d, &z, nullptr);
            DDSURFACEDESC got = {};
            got.dwSize = sizeof(got);
            if (z) z->GetSurfaceDesc(&got);
            P(" %s 0x%08lx (caps 0x%lx, pitch %ld, %lu bits);", mem == DDSCAPS_SYSTEMMEMORY ? "system" : mem ? "video" : "no memory type", hr,
              got.ddsCaps.dwCaps, got.lPitch, got.dwZBufferBitDepth);
            if (z) z->Release();
        }
        P("\n");
    }
    static const DWORD kPalFlags[] = {DDPCAPS_8BIT | DDPCAPS_PRIMARYSURFACE, DDPCAPS_8BIT | DDPCAPS_ALLOW256 | DDPCAPS_PRIMARYSURFACE,
                                      DDPCAPS_8BIT | DDPCAPS_PRIMARYSURFACELEFT, DDPCAPS_8BIT | DDPCAPS_VSYNC, DDPCAPS_8BIT | DDPCAPS_ALPHA,
                                      DDPCAPS_8BIT | DDPCAPS_INITIALIZE, DDPCAPS_8BIT | 0x800, DDPCAPS_8BIT | DDPCAPS_4BIT, 0};
    PALETTEENTRY entries[256] = {};
    for (DWORD flags : kPalFlags) {
        IDirectDrawPalette* p7 = nullptr;
        IDirectDrawPalette* p1 = nullptr;
        const HRESULT h7 = dd7->CreatePalette(flags, entries, &p7, nullptr), h1 = dd1->CreatePalette(flags, entries, &p1, nullptr);
        DWORD c7 = 0, c1 = 0;
        if (p7) p7->GetCaps(&c7);
        if (p1) p1->GetCaps(&c1);
        P("  CreatePalette with flags 0x%lx: IDirectDraw7 0x%08lx (caps 0x%lx), IDirectDraw 0x%08lx (caps 0x%lx)\n", flags, h7, c7, h1, c1);
        if (p7) p7->Release();
        if (p1) p1->Release();
    }
}

// ----------------------------------------------------------------

// The sections, each run in a process of its own ("fmtprobe section N") that appends to the
// file: one that crashes or hangs on some ddraw.dll costs that section only.
// ---------------------------------------------------------------- a 15+1 Z buffer, large surfaces
// What 1.8.2 left unmeasured. A Z buffer of 15 bits with 1 of stencil: where it is made when
// no memory is named, and what each device does with it (its clears, its depth test, each
// stencil operation and the stencil test, read back from the buffer's words and the
// target's pixels). Surfaces of very many pixels in system memory.
static IDirectDrawSurface7* MakeZ(IDirectDraw7* dd, DWORD memory, DWORD bits, DWORD zmask, DWORD stencilBits, DWORD stencilMask,
                                  HRESULT* hr) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | memory;
    d.dwWidth = d.dwHeight = 64;
    d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    d.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER | (stencilBits ? DDPF_STENCILBUFFER : 0);
    d.ddpfPixelFormat.dwZBufferBitDepth = bits;
    d.ddpfPixelFormat.dwZBitMask = zmask;
    d.ddpfPixelFormat.dwStencilBitDepth = stencilBits;
    d.ddpfPixelFormat.dwStencilBitMask = stencilMask;
    IDirectDrawSurface7* s = nullptr;
    *hr = dd->CreateSurface(&d, &s, nullptr);
    return s;
}

static void StencilZCreation(IDirectDraw7* dd) {
    P("\nZ buffers through CreateSurface (64x64) with no memory named, beside each memory named:\n");
    struct Z {
        const char* name;
        DWORD bits, zmask, stencilBits, stencilMask;
    };
    static const Z kZ[] = {{"15 bits and 1 of stencil", 16, 0xFFFE, 1, 0x0001},
                           {"15 bits and 1 of stencil, stencil high", 16, 0x7FFF, 1, 0x8000},
                           {"16 bits", 16, 0xFFFF, 0, 0},
                           {"24 bits and 8 of stencil", 32, 0xFFFFFF00, 8, 0xFF},
                           {"8 bits and 8 of stencil", 16, 0xFF00, 8, 0xFF},
                           {"12 bits and 4 of stencil", 16, 0xFFF0, 4, 0xF},
                           {"16 bits and 16 of stencil", 32, 0xFFFF0000, 16, 0xFFFF}};
    for (const Z& k : kZ) {
        P("  %s:\n", k.name);
        for (DWORD memory : {DWORD(0), DWORD(DDSCAPS_SYSTEMMEMORY), DWORD(DDSCAPS_VIDEOMEMORY)}) {
            HRESULT hr;
            IDirectDrawSurface7* s = MakeZ(dd, memory, k.bits, k.zmask, k.stencilBits, k.stencilMask, &hr);
            P("    %s: 0x%08lx", !memory ? "no memory named" : memory == DDSCAPS_SYSTEMMEMORY ? "system memory" : "video memory",
              hr);
            if (s) {
                DDSURFACEDESC2 got = {};
                got.dwSize = sizeof(got);
                s->GetSurfaceDesc(&got);
                P(", caps 0x%08lx, pitch %ld, flags 0x%lx, %lu bits, Z mask 0x%lx, %lu stencil bits, stencil mask 0x%lx",
                  got.ddsCaps.dwCaps, got.lPitch, got.ddpfPixelFormat.dwFlags, got.ddpfPixelFormat.dwZBufferBitDepth,
                  got.ddpfPixelFormat.dwZBitMask, got.ddpfPixelFormat.dwStencilBitDepth, got.ddpfPixelFormat.dwStencilBitMask);
                s->Release();
            }
            P("\n");
        }
    }
}

// The Z buffer's word and the target's pixel at 32,32 (0xdead, 0xdeadbeef: not locked).
static DWORD ZWord(IDirectDrawSurface7* z) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    if (FAILED(z->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) return 0xDEAD;
    const DWORD v = *reinterpret_cast<const WORD*>(static_cast<const BYTE*>(d.lpSurface) + 32 * d.lPitch + 32 * 2);
    z->Unlock(nullptr);
    return v;
}
static DWORD TargetPixel(IDirectDrawSurface7* s) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    if (FAILED(s->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) return 0xDEADBEEF;
    const DWORD bytes = d.ddpfPixelFormat.dwRGBBitCount / 8;
    DWORD v = 0;
    memcpy(&v, static_cast<const BYTE*>(d.lpSurface) + 32 * d.lPitch + 32 * bytes, bytes > 4 ? 4 : bytes);
    s->Unlock(nullptr);
    return v;
}
// A quad over the whole target at depth z.
static HRESULT Quad(IDirect3DDevice7* dev, float z, D3DCOLOR color) {
    D3DTLVERTEX v[4] = {};
    const float xy[4][2] = {{0, 0}, {64, 0}, {0, 64}, {64, 64}};
    for (int i = 0; i < 4; i++) {
        v[i].sx = xy[i][0];
        v[i].sy = xy[i][1];
        v[i].sz = z;
        v[i].rhw = 1.0f;
        v[i].color = color;
    }
    HRESULT hr = dev->BeginScene();
    if (FAILED(hr)) return hr;
    hr = dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_TLVERTEX, v, 4, 0);
    dev->EndScene();
    return hr;
}

static void StencilZDevice(IDirectDraw7* dd, const char* name, const GUID& guid, bool hardware) {
    P("\nA Z buffer of 15 bits and 1 of stencil (Z mask 0xfffe, stencil mask 0x1) with the %s device:\n", name);
    IDirect3D7* d3d = nullptr;
    dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d));
    if (!d3d) {
        P("  (no IDirect3D7)\n");
        return;
    }
    const DWORD memory = hardware ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY;
    HRESULT hr;
    IDirectDrawSurface7* rt = Make(dd, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | memory, 64, 64, nullptr, &hr);
    P("  target in %s memory: 0x%08lx\n", hardware ? "video" : "system", hr);
    IDirectDrawSurface7* z = MakeZ(dd, memory, 16, 0xFFFE, 1, 1, &hr);
    P("  Z buffer in %s memory: 0x%08lx\n", hardware ? "video" : "system", hr);
    if (!z && hardware) {
        z = MakeZ(dd, DDSCAPS_SYSTEMMEMORY, 16, 0xFFFE, 1, 1, &hr);
        P("  Z buffer in system memory instead: 0x%08lx\n", hr);
    }
    IDirect3DDevice7* dev = nullptr;
    if (rt && z) {
        P("  AddAttachedSurface: 0x%08lx\n", rt->AddAttachedSurface(z));
        P("  CreateDevice: 0x%08lx\n", d3d->CreateDevice(guid, rt, &dev));
    }
    if (dev) {
        D3DVIEWPORT7 vp = {0, 0, 64, 64, 0.0f, 1.0f};
        dev->SetViewport(&vp);
        dev->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
        dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
        DWORD a = 0, b = 0, c = 0;
        dev->GetRenderState(D3DRENDERSTATE_STENCILMASK, &a);
        dev->GetRenderState(D3DRENDERSTATE_STENCILWRITEMASK, &b);
        dev->GetRenderState(D3DRENDERSTATE_STENCILREF, &c);
        P("  at first: STENCILMASK 0x%lx, STENCILWRITEMASK 0x%lx, STENCILREF 0x%lx, the buffer's word 0x%04lx\n", a, b, c, ZWord(z));

        P("  Clear(ZBUFFER | STENCIL), then the buffer's word:\n");
        struct ClearCase {
            float depth;
            DWORD stencil;
        };
        static const ClearCase kClears[] = {{1.0f, 0}, {1.0f, 1}, {0.0f, 0}, {0.0f, 1}, {0.5f, 0}, {0.5f, 1},
                                            {1.0f, 2}, {1.0f, 3}, {1.0f, 0xFF}, {1.0f, 0xFFFFFFFF}};
        for (const ClearCase& k : kClears) {
            hr = dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, k.depth, k.stencil);
            P("    depth %.1f, stencil 0x%lx: 0x%08lx, word 0x%04lx\n", double(k.depth), k.stencil, hr, ZWord(z));
        }
        dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 1);
        hr = dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0, 0.5f, 0);
        P("    after depth 1.0 and stencil 1, Clear(ZBUFFER) alone to 0.5: 0x%08lx, word 0x%04lx\n", hr, ZWord(z));
        hr = dev->Clear(0, nullptr, D3DCLEAR_STENCIL, 0, 1.0f, 0);
        P("    then Clear(STENCIL) alone to 0: 0x%08lx, word 0x%04lx\n", hr, ZWord(z));
        hr = dev->Clear(0, nullptr, D3DCLEAR_STENCIL, 0, 1.0f, 1);
        P("    then Clear(STENCIL) alone to 1: 0x%08lx, word 0x%04lx\n", hr, ZWord(z));

        P("  the depth test (ZFUNC LESSEQUAL), the buffer cleared to depth 0.5 and stencil 0, the target to 0:\n");
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 0.5f, 0);
        dev->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
        dev->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, TRUE);
        dev->SetRenderState(D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
        hr = Quad(dev, 0.75f, 0xFFFF0000);
        P("    a red quad at 0.75: 0x%08lx, pixel 0x%lx, word 0x%04lx\n", hr, TargetPixel(rt), ZWord(z));
        hr = Quad(dev, 0.25f, 0xFFFFFFFF);
        P("    a white quad at 0.25: 0x%08lx, pixel 0x%lx, word 0x%04lx\n", hr, TargetPixel(rt), ZWord(z));
        dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 0.5f, 1);
        hr = Quad(dev, 0.25f, 0xFF00FF00);
        P("    stencil 1 and a green quad at 0.25, the stencil disabled: 0x%08lx, pixel 0x%lx, word 0x%04lx\n", hr, TargetPixel(rt),
          ZWord(z));

        P("  each stencil operation as STENCILPASS (STENCILFUNC ALWAYS, ZFUNC ALWAYS, a quad at 0.5 over depth 1.0):\n");
        dev->SetRenderState(D3DRENDERSTATE_ZFUNC, D3DCMP_ALWAYS);
        P("    STENCILENABLE: 0x%08lx\n", dev->SetRenderState(D3DRENDERSTATE_STENCILENABLE, TRUE));
        dev->SetRenderState(D3DRENDERSTATE_STENCILFUNC, D3DCMP_ALWAYS);
        struct Op {
            const char* name;
            DWORD op, start, ref, writeMask;
        };
        static const Op kOps[] = {{"KEEP from 1", D3DSTENCILOP_KEEP, 1, 0, 0xFFFFFFFF},
                                  {"ZERO from 1", D3DSTENCILOP_ZERO, 1, 1, 0xFFFFFFFF},
                                  {"REPLACE with 1 from 0", D3DSTENCILOP_REPLACE, 0, 1, 0xFFFFFFFF},
                                  {"REPLACE with 2 from 1", D3DSTENCILOP_REPLACE, 1, 2, 0xFFFFFFFF},
                                  {"REPLACE with 3 from 0", D3DSTENCILOP_REPLACE, 0, 3, 0xFFFFFFFF},
                                  {"REPLACE with 0xff from 0", D3DSTENCILOP_REPLACE, 0, 0xFF, 0xFFFFFFFF},
                                  {"REPLACE with 1 from 0, STENCILWRITEMASK 0", D3DSTENCILOP_REPLACE, 0, 1, 0},
                                  {"REPLACE with 1 from 0, STENCILWRITEMASK 2", D3DSTENCILOP_REPLACE, 0, 1, 2},
                                  {"INCRSAT from 0", D3DSTENCILOP_INCRSAT, 0, 0, 0xFFFFFFFF},
                                  {"INCRSAT from 1", D3DSTENCILOP_INCRSAT, 1, 0, 0xFFFFFFFF},
                                  {"DECRSAT from 1", D3DSTENCILOP_DECRSAT, 1, 0, 0xFFFFFFFF},
                                  {"DECRSAT from 0", D3DSTENCILOP_DECRSAT, 0, 0, 0xFFFFFFFF},
                                  {"INVERT from 0", D3DSTENCILOP_INVERT, 0, 0, 0xFFFFFFFF},
                                  {"INVERT from 1", D3DSTENCILOP_INVERT, 1, 0, 0xFFFFFFFF},
                                  {"INCR from 0", D3DSTENCILOP_INCR, 0, 0, 0xFFFFFFFF},
                                  {"INCR from 1", D3DSTENCILOP_INCR, 1, 0, 0xFFFFFFFF},
                                  {"DECR from 1", D3DSTENCILOP_DECR, 1, 0, 0xFFFFFFFF},
                                  {"DECR from 0", D3DSTENCILOP_DECR, 0, 0, 0xFFFFFFFF}};
        for (const Op& k : kOps) {
            dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, k.start);
            const HRESULT h1 = dev->SetRenderState(D3DRENDERSTATE_STENCILPASS, k.op);
            dev->SetRenderState(D3DRENDERSTATE_STENCILREF, k.ref);
            dev->SetRenderState(D3DRENDERSTATE_STENCILWRITEMASK, k.writeMask);
            hr = Quad(dev, 0.5f, 0xFFFFFFFF);
            P("    %s: set 0x%08lx, drawn 0x%08lx, word 0x%04lx\n", k.name, h1, hr, ZWord(z));
        }
        dev->SetRenderState(D3DRENDERSTATE_STENCILWRITEMASK, 0xFFFFFFFF);
        dev->SetRenderState(D3DRENDERSTATE_STENCILPASS, D3DSTENCILOP_KEEP);

        P("  the stencil test (a white quad over a target cleared to 0; STENCILPASS KEEP):\n");
        struct Test {
            const char* name;
            DWORD func, start, ref, mask;
        };
        static const Test kTests[] = {{"EQUAL, stencil 1, ref 1, mask 0xffffffff", D3DCMP_EQUAL, 1, 1, 0xFFFFFFFF},
                                      {"EQUAL, stencil 0, ref 1, mask 0xffffffff", D3DCMP_EQUAL, 0, 1, 0xFFFFFFFF},
                                      {"EQUAL, stencil 1, ref 0xff, mask 0xffffffff", D3DCMP_EQUAL, 1, 0xFF, 0xFFFFFFFF},
                                      {"EQUAL, stencil 1, ref 3, mask 0xffffffff", D3DCMP_EQUAL, 1, 3, 0xFFFFFFFF},
                                      {"EQUAL, stencil 1, ref 3, mask 1", D3DCMP_EQUAL, 1, 3, 1},
                                      {"EQUAL, stencil 0, ref 2, mask 0xffffffff", D3DCMP_EQUAL, 0, 2, 0xFFFFFFFF},
                                      {"EQUAL, stencil 1, ref 1, mask 2", D3DCMP_EQUAL, 1, 1, 2},
                                      {"EQUAL, stencil 1, ref 0, mask 2", D3DCMP_EQUAL, 1, 0, 2},
                                      {"LESS, stencil 1, ref 0, mask 0xffffffff", D3DCMP_LESS, 1, 0, 0xFFFFFFFF},
                                      {"LESS, stencil 1, ref 2, mask 0xffffffff", D3DCMP_LESS, 1, 2, 0xFFFFFFFF},
                                      {"GREATER, stencil 1, ref 2, mask 0xffffffff", D3DCMP_GREATER, 1, 2, 0xFFFFFFFF},
                                      {"NEVER, stencil 1", D3DCMP_NEVER, 1, 1, 0xFFFFFFFF}};
        for (const Test& k : kTests) {
            dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, k.start);
            dev->SetRenderState(D3DRENDERSTATE_STENCILFUNC, k.func);
            dev->SetRenderState(D3DRENDERSTATE_STENCILREF, k.ref);
            dev->SetRenderState(D3DRENDERSTATE_STENCILMASK, k.mask);
            hr = Quad(dev, 0.5f, 0xFFFFFFFF);
            P("    %s: drawn 0x%08lx, pixel 0x%lx, word 0x%04lx\n", k.name, hr, TargetPixel(rt), ZWord(z));
        }
        // A failed stencil test's operation, and a failed depth test's.
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 0.25f, 0);
        dev->SetRenderState(D3DRENDERSTATE_STENCILMASK, 0xFFFFFFFF);
        dev->SetRenderState(D3DRENDERSTATE_STENCILFUNC, D3DCMP_NEVER);
        dev->SetRenderState(D3DRENDERSTATE_STENCILFAIL, D3DSTENCILOP_INVERT);
        hr = Quad(dev, 0.5f, 0xFFFFFFFF);
        P("    NEVER with STENCILFAIL INVERT from 0: drawn 0x%08lx, pixel 0x%lx, word 0x%04lx\n", hr, TargetPixel(rt), ZWord(z));
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 0.25f, 0);
        dev->SetRenderState(D3DRENDERSTATE_STENCILFUNC, D3DCMP_ALWAYS);
        dev->SetRenderState(D3DRENDERSTATE_STENCILFAIL, D3DSTENCILOP_KEEP);
        dev->SetRenderState(D3DRENDERSTATE_STENCILZFAIL, D3DSTENCILOP_INVERT);
        dev->SetRenderState(D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
        hr = Quad(dev, 0.5f, 0xFFFFFFFF);
        P("    a quad at 0.5 behind depth 0.25 with STENCILZFAIL INVERT from 0: drawn 0x%08lx, pixel 0x%lx, word 0x%04lx\n", hr,
          TargetPixel(rt), ZWord(z));
        P("  the device released: %lu left\n", dev->Release());
    }
    if (rt && z) rt->DeleteAttachedSurface(0, z);
    if (z) z->Release();
    if (rt) rt->Release();
    d3d->Release();
}

// What a device leaves in a Z buffer: the word after Clear at several depths, and
// after a quad drawn at them (ZFUNC ALWAYS), in each layout the emulation layer makes; and
// along a quad whose depth runs from 0 at the left edge to 1 at the right.
static void DepthWords(IDirectDraw7* dd, const char* name, const GUID& guid, bool hardware) {
    P("\nZ buffer words with the %s device (the value at 32,32 as the buffer's bits; 64x64):\n", name);
    IDirect3D7* d3d = nullptr;
    dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d));
    if (!d3d) {
        P("  (no IDirect3D7)\n");
        return;
    }
    struct Z {
        const char* name;
        DWORD bits, zmask, stencilBits, stencilMask;
    };
    static const Z kZ[] = {{"16 bits", 16, 0xFFFF, 0, 0},
                           {"24 bits in 32, high", 32, 0xFFFFFF00, 0, 0},
                           {"24 bits in 32, low", 32, 0x00FFFFFF, 0, 0},
                           {"32 bits", 32, 0xFFFFFFFF, 0, 0},
                           {"24 bits and 8 of stencil", 32, 0xFFFFFF00, 8, 0xFF},
                           {"24 bits and 8 of stencil, stencil high", 32, 0x00FFFFFF, 8, 0xFF000000}};
    static const float kDepths[] = {0.0f, 0.1f, 0.25f, 1.0f / 3, 0.5f, 0.6f, 0.75f, 0.9f, 0.999f, 1.0f};
    const DWORD memory = hardware ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY;
    for (const Z& k : kZ) {
        HRESULT hr, zh;
        IDirectDrawSurface7* rt = Make(dd, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | memory, 64, 64, nullptr, &hr);
        IDirectDrawSurface7* z = MakeZ(dd, memory, k.bits, k.zmask, k.stencilBits, k.stencilMask, &zh);
        IDirect3DDevice7* dev = nullptr;
        HRESULT ah = E_FAIL, dh = E_FAIL;
        if (rt && z) {
            ah = rt->AddAttachedSurface(z);
            dh = d3d->CreateDevice(guid, rt, &dev);
        }
        P("  %s: target 0x%08lx, Z buffer 0x%08lx, attached 0x%08lx, device 0x%08lx\n", k.name, hr, zh, ah, dh);
        if (dev) {
            auto word = [&](int x) -> DWORD {
                DDSURFACEDESC2 d = {};
                d.dwSize = sizeof(d);
                if (FAILED(z->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) return 0xDEADDEAD;
                DWORD v = 0;
                memcpy(&v, static_cast<const BYTE*>(d.lpSurface) + 32 * d.lPitch + x * (k.bits / 8), k.bits / 8);
                z->Unlock(nullptr);
                return v;
            };
            D3DVIEWPORT7 vp = {0, 0, 64, 64, 0.0f, 1.0f};
            dev->SetViewport(&vp);
            dev->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
            dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
            dev->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, TRUE);
            dev->SetRenderState(D3DRENDERSTATE_ZFUNC, D3DCMP_ALWAYS);
            const DWORD clearFlags = D3DCLEAR_ZBUFFER | (k.stencilBits ? D3DCLEAR_STENCIL : 0);
            P("    cleared to:");
            for (float depth : kDepths) {
                hr = dev->Clear(0, nullptr, clearFlags, 0, depth, 0);
                P(" %.3f 0x%lx%s", double(depth), word(32), FAILED(hr) ? " (failed)" : "");
            }
            P("\n    a quad at (over a buffer cleared to 0.0):");
            for (float depth : kDepths) {
                dev->Clear(0, nullptr, clearFlags, 0, 0.0f, 0);
                hr = Quad(dev, depth, 0xFFFFFFFF);
                P(" %.3f 0x%lx%s", double(depth), word(32), FAILED(hr) ? " (failed)" : "");
            }
            // Depth 0 at x = 0 to 1 at x = 64.
            dev->Clear(0, nullptr, clearFlags, 0, 0.0f, 0);
            D3DTLVERTEX v[4] = {};
            const float xy[4][2] = {{0, 0}, {64, 0}, {0, 64}, {64, 64}};
            for (int i = 0; i < 4; i++) {
                v[i].sx = xy[i][0];
                v[i].sy = xy[i][1];
                v[i].sz = xy[i][0] / 64.0f;
                v[i].rhw = 1.0f;
                v[i].color = 0xFFFFFFFF;
            }
            hr = dev->BeginScene();
            if (SUCCEEDED(hr)) {
                hr = dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_TLVERTEX, v, 4, 0);
                dev->EndScene();
            }
            P("\n    a quad from 0.0 at the left edge to 1.0 at the right (0x%08lx), at x =", hr);
            for (int x : {0, 1, 15, 16, 31, 32, 48, 62, 63}) P(" %d 0x%lx", x, word(x));
            P("\n");
            dev->Release();
        }
        if (rt && z) rt->DeleteAttachedSurface(0, z);
        if (z) z->Release();
        if (rt) rt->Release();
    }
    d3d->Release();
}

static void LargeSurfaces(IDirectDraw7* dd) {
    P("\nSurfaces of very many pixels in system memory (then Lock and the last pixel written and read back):\n");
    static const Rgb kPal8 = {"8 bits", 8, 0, 0, 0, 0};
    static const Rgb* const kFormats[] = {&kPal8, &k565, &kX888};
    // (By size in bytes, so that a ddraw.dll that stops at one has answered the smaller.)
    static const int kSizes[][2] = {{16384, 16384}, {16385, 16384}, {65535, 4096}, {65535, 4097}, {20000, 20000},
                                    {32768, 32768}, {46341, 46341}, {65535, 65535}};
    for (const Rgb* f : kFormats) {
        for (const auto& size : kSizes) {
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
            d.dwWidth = DWORD(size[0]);
            d.dwHeight = DWORD(size[1]);
            SetFormat(&d.ddpfPixelFormat, *f);
            if (f->bits == 8) d.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
            IDirectDrawSurface7* s = nullptr;
            const HRESULT hr = dd->CreateSurface(&d, &s, nullptr);
            P("  %dx%d, %s: 0x%08lx", size[0], size[1], f->name, hr);
            if (s) {
                DDSURFACEDESC2 l = {};
                l.dwSize = sizeof(l);
                const HRESULT lh = s->Lock(nullptr, &l, DDLOCK_WAIT, nullptr);
                P(", Lock 0x%08lx", lh);
                if (SUCCEEDED(lh)) {
                    BYTE* last = static_cast<BYTE*>(l.lpSurface) + size_t(size[1] - 1) * size_t(l.lPitch) +
                                 size_t(size[0] - 1) * (f->bits / 8);
                    *last = 0x5A;
                    P(", pitch %ld, last pixel's byte 0x%02x", l.lPitch, *last);
                    s->Unlock(nullptr);
                }
                P(", released: %lu left", s->Release());
            }
            P("\n");
        }
    }
}

// ---------------------------------------------------------------- what 1.9.0 asked

// A surface of `w` pixels in one row (16 or 32 bits), as text: each pixel in hex.
static const char* Pixels16or32(IDirectDrawSurface7* s, int w) {
    static char out[512];
    size_t used = 0;
    out[0] = 0;
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    if (FAILED(s->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) return " (no lock)";
    const DWORD bits = d.ddpfPixelFormat.dwRGBBitCount;
    for (int x = 0; x < w && used < sizeof(out) - 16; x++) {
        const BYTE* p = static_cast<const BYTE*>(d.lpSurface);
        const DWORD v = bits == 16 ? reinterpret_cast<const WORD*>(p)[x] : reinterpret_cast<const DWORD*>(p)[x];
        used += size_t(snprintf(out + used, sizeof(out) - used, bits == 16 ? " %04lx" : " %08lx", v));
    }
    s->Unlock(nullptr);
    return out;
}
static void PutRow(IDirectDrawSurface7* s, const DWORD* v, int w) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    if (FAILED(s->Lock(nullptr, &d, DDLOCK_WAIT, nullptr))) return;
    BYTE* p = static_cast<BYTE*>(d.lpSurface);
    for (int x = 0; x < w; x++)
        if (d.ddpfPixelFormat.dwRGBBitCount == 16) reinterpret_cast<WORD*>(p)[x] = WORD(v[x]);
        else reinterpret_cast<DWORD*>(p)[x] = v[x];
    s->Unlock(nullptr);
}
static void FillRow(IDirectDrawSurface7* s, DWORD v, int w) {
    DWORD row[16];
    for (int x = 0; x < w; x++) row[x] = v;
    PutRow(s, row, w);
}

// Colour-key ranges (DDCKEY_COLORSPACE): set, created with, and blitted with, in video and
// system memory, at 16 and 32 bits. The pixels are chosen so that comparing the whole value
// and comparing each colour on its own give different answers.
static void KeyRanges(IDirectDraw7* dd) {
    DDCAPS drv = {}, hel = {};
    drv.dwSize = hel.dwSize = sizeof(DDCAPS);
    dd->GetCaps(&drv, &hel);
    P("\nColour-key ranges: the driver's key caps 0x%08lx, the emulation's 0x%08lx\n", drv.dwCKeyCaps, hel.dwCKeyCaps);
    struct Case {
        const char* name;
        Rgb f;
        DWORD low, high, marker, solid;
        DWORD px[8];
    };
    static const Case kCases[] = {
        {"5-6-5", k565, 0x0841, 0x2104, 0xF81F, 0x07E0, {0x0840, 0x0841, 0x1000, 0x1082, 0x2104, 0x2105, 0x1FFF, 0x0001}},
        {"8-8-8 in 32 bits", kX888, 0x00102030, 0x00405060, 0x00FF00FF, 0x0000FF00,
         {0x0010202F, 0x00102030, 0x00300000, 0x00203040, 0x00405060, 0x00405061, 0x003FFFFF, 0x00000001}}};
    for (const Case& c : kCases)
        for (DWORD memory : {DWORD(DDSCAPS_VIDEOMEMORY), DWORD(DDSCAPS_SYSTEMMEMORY)}) {
            const char* where = memory == DDSCAPS_VIDEOMEMORY ? "video memory" : "system memory";
            HRESULT hs, hd;
            IDirectDrawSurface7* src = Make(dd, DDSCAPS_OFFSCREENPLAIN | memory, 8, 1, &c.f, &hs);
            IDirectDrawSurface7* dst = Make(dd, DDSCAPS_OFFSCREENPLAIN | memory, 8, 1, &c.f, &hd);
            P("  %s, %s (key %lx-%lx; source pixels", c.name, where, c.low, c.high);
            for (DWORD v : c.px) P(" %lx", v);
            P("):\n");
            if (!src || !dst) {
                P("    surfaces not made: 0x%08lx, 0x%08lx\n", hs, hd);
                if (src) src->Release();
                if (dst) dst->Release();
                continue;
            }
            DDCOLORKEY range = {c.low, c.high}, got = {};
            HRESULT hr = src->SetColorKey(DDCKEY_SRCBLT | DDCKEY_COLORSPACE, &range);
            const HRESULT gh = src->GetColorKey(DDCKEY_SRCBLT, &got);
            P("    SetColorKey(SRCBLT | COLORSPACE): 0x%08lx; GetColorKey 0x%08lx: %lx-%lx\n", hr, gh, got.dwColorSpaceLowValue,
              got.dwColorSpaceHighValue);
            PutRow(src, c.px, 8);
            FillRow(dst, c.marker, 8);
            hr = dst->Blt(nullptr, src, nullptr, DDBLT_KEYSRC | DDBLT_WAIT, nullptr);
            P("    Blt KEYSRC: 0x%08lx, the destination:%s\n", hr, Pixels16or32(dst, 8));
            FillRow(dst, c.marker, 8);
            DDBLTFX fx = {};
            fx.dwSize = sizeof(fx);
            fx.ddckSrcColorkey = range;
            hr = dst->Blt(nullptr, src, nullptr, DDBLT_KEYSRCOVERRIDE | DDBLT_WAIT, &fx);
            P("    Blt KEYSRCOVERRIDE with the range: 0x%08lx, the destination:%s\n", hr, Pixels16or32(dst, 8));
            FillRow(dst, c.marker, 8);
            RECT all = {0, 0, 8, 1};
            hr = dst->BltFast(0, 0, src, &all, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT);
            P("    BltFast SRCCOLORKEY: 0x%08lx, the destination:%s\n", hr, Pixels16or32(dst, 8));
            // Destination keys: the pixels in the destination, a solid source over them.
            FillRow(src, c.solid, 8);
            hr = dst->SetColorKey(DDCKEY_DESTBLT | DDCKEY_COLORSPACE, &range);
            const HRESULT gd = dst->GetColorKey(DDCKEY_DESTBLT, &got);
            P("    SetColorKey(DESTBLT | COLORSPACE): 0x%08lx; GetColorKey 0x%08lx: %lx-%lx\n", hr, gd, got.dwColorSpaceLowValue,
              got.dwColorSpaceHighValue);
            PutRow(dst, c.px, 8);
            hr = dst->Blt(nullptr, src, nullptr, DDBLT_KEYDEST | DDBLT_WAIT, nullptr);
            P("    Blt KEYDEST of %lx: 0x%08lx, the destination:%s\n", c.solid, hr, Pixels16or32(dst, 8));
            PutRow(dst, c.px, 8);
            fx.ddckDestColorkey = range;
            hr = dst->Blt(nullptr, src, nullptr, DDBLT_KEYDESTOVERRIDE | DDBLT_WAIT, &fx);
            P("    Blt KEYDESTOVERRIDE with the range: 0x%08lx, the destination:%s\n", hr, Pixels16or32(dst, 8));
            src->Release();
            dst->Release();
            // A surface made with a range.
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_CKSRCBLT;
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | memory;
            d.dwWidth = 8;
            d.dwHeight = 1;
            SetFormat(&d.ddpfPixelFormat, c.f);
            d.ddckCKSrcBlt = range;
            IDirectDrawSurface7* made = nullptr;
            hr = dd->CreateSurface(&d, &made, nullptr);
            got = {};
            const HRESULT gm = made ? made->GetColorKey(DDCKEY_SRCBLT, &got) : E_FAIL;
            P("    CreateSurface with a CKSRCBLT range: 0x%08lx; GetColorKey 0x%08lx: %lx-%lx\n", hr, gm, got.dwColorSpaceLowValue,
              got.dwColorSpaceHighValue);
            if (made) made->Release();
        }
}

// Overlays: keys with ranges, bob and interleaving, flips of a field, alpha and effects; and
// the colour controls of the primary and an overlay.
static void OverlayMore(IDirectDraw7* dd) {
    DWORD codes[32] = {}, n = 32;
    HRESULT hr = dd->GetFourCCCodes(&n, codes);
    P("\nOverlays (keys, bob, interleaving, field flips, alpha, effects); GetFourCCCodes 0x%08lx:", hr);
    for (DWORD i = 0; i < n && i < 32; i++) P(" %.4s", reinterpret_cast<const char*>(&codes[i]));
    P("\n");
    DDSURFACEDESC2 pd = {};
    pd.dwSize = sizeof(pd);
    pd.dwFlags = DDSD_CAPS;
    pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* prim = nullptr;
    hr = dd->CreateSurface(&pd, &prim, nullptr);
    if (!prim) {
        P("  no primary: 0x%08lx\n", hr);
        return;
    }
    // A video-memory YUV overlay with a back buffer (the first YUY2 or UYVY listed), else one alone.
    DWORD fourcc = 0;
    for (DWORD i = 0; i < n && i < 32; i++)
        if (codes[i] == MAKEFOURCC('Y', 'U', 'Y', '2') || codes[i] == MAKEFOURCC('U', 'Y', 'V', 'Y')) {
            fourcc = codes[i];
            break;
        }
    IDirectDrawSurface7* ov = nullptr;
    bool chain = false;
    for (int attempt = 0; attempt < 4 && !ov; attempt++) {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        d.ddsCaps.dwCaps = DDSCAPS_OVERLAY | DDSCAPS_VIDEOMEMORY;
        d.dwWidth = 64;
        d.dwHeight = 64;
        chain = attempt % 2 == 0;
        if (chain) {
            d.dwFlags |= DDSD_BACKBUFFERCOUNT;
            d.ddsCaps.dwCaps |= DDSCAPS_FLIP | DDSCAPS_COMPLEX;
            d.dwBackBufferCount = 1;
        }
        if (attempt < 2 && fourcc) {
            d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            d.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
            d.ddpfPixelFormat.dwFourCC = fourcc;
        } else {
            SetFormat(&d.ddpfPixelFormat, k565);
        }
        hr = dd->CreateSurface(&d, &ov, nullptr);
        P("  an overlay, %s%s: 0x%08lx\n", attempt < 2 && fourcc ? "YUV" : "5-6-5", chain ? " with a back buffer" : "", hr);
    }
    if (ov) {
        const RECT dst = {0, 0, 64, 64};
        auto show = [&](const char* what, DWORD flags, DDOVERLAYFX* fx) {
            const HRESULT r = ov->UpdateOverlay(nullptr, prim, const_cast<RECT*>(&dst), DDOVER_SHOW | flags, fx);
            P("    UpdateOverlay(SHOW | %s): 0x%08lx\n", what, r);
            ov->UpdateOverlay(nullptr, prim, nullptr, DDOVER_HIDE, nullptr);
        };
        show("nothing else", 0, nullptr);
        DDCOLORKEY range = {0x0841, 0x2104}, got = {};
        hr = prim->SetColorKey(DDCKEY_DESTOVERLAY | DDCKEY_COLORSPACE, &range);
        const HRESULT gp = prim->GetColorKey(DDCKEY_DESTOVERLAY, &got);
        P("    the primary's SetColorKey(DESTOVERLAY | COLORSPACE, 841-2104): 0x%08lx; GetColorKey 0x%08lx: %lx-%lx\n", hr, gp,
          got.dwColorSpaceLowValue, got.dwColorSpaceHighValue);
        show("KEYDEST", DDOVER_KEYDEST, nullptr);
        DDOVERLAYFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dckDestColorkey = range;
        show("KEYDESTOVERRIDE with a range", DDOVER_KEYDESTOVERRIDE, &fx);
        hr = ov->SetColorKey(DDCKEY_SRCOVERLAY | DDCKEY_COLORSPACE, &range);
        got = {};
        const HRESULT go = ov->GetColorKey(DDCKEY_SRCOVERLAY, &got);
        P("    the overlay's SetColorKey(SRCOVERLAY | COLORSPACE): 0x%08lx; GetColorKey 0x%08lx: %lx-%lx\n", hr, go,
          got.dwColorSpaceLowValue, got.dwColorSpaceHighValue);
        fx = {};
        fx.dwSize = sizeof(fx);
        fx.dckSrcColorkey = range;
        show("KEYSRCOVERRIDE with a range", DDOVER_KEYSRCOVERRIDE, &fx);
        struct Flag {
            const char* name;
            DWORD flags;
        };
        static const Flag kBob[] = {{"BOB", DDOVER_BOB},
                                    {"BOB | INTERLEAVED", DDOVER_BOB | DDOVER_INTERLEAVED},
                                    {"INTERLEAVED", DDOVER_INTERLEAVED},
                                    {"BOBHARDWARE", DDOVER_BOBHARDWARE},
                                    {"BOB | BOBHARDWARE", DDOVER_BOB | DDOVER_BOBHARDWARE},
                                    {"OVERRIDEBOBWEAVE", DDOVER_OVERRIDEBOBWEAVE},
                                    {"BOB | OVERRIDEBOBWEAVE", DDOVER_BOB | DDOVER_OVERRIDEBOBWEAVE},
                                    {"AUTOFLIP", DDOVER_AUTOFLIP}};
        for (const Flag& f : kBob) show(f.name, f.flags, nullptr);
        // Field flips: shown with bob, then without.
        for (int bob = 1; bob >= 0; bob--) {
            hr = ov->UpdateOverlay(nullptr, prim, const_cast<RECT*>(&dst), DDOVER_SHOW | (bob ? DDOVER_BOB | DDOVER_INTERLEAVED : 0),
                                   nullptr);
            const HRESULT odd = ov->Flip(nullptr, DDFLIP_ODD | DDFLIP_WAIT);
            const HRESULT even = ov->Flip(nullptr, DDFLIP_EVEN | DDFLIP_WAIT);
            const HRESULT both = ov->Flip(nullptr, DDFLIP_ODD | DDFLIP_EVEN | DDFLIP_WAIT);
            const HRESULT plain = ov->Flip(nullptr, DDFLIP_WAIT);
            P("    shown%s (0x%08lx): Flip(ODD) 0x%08lx, Flip(EVEN) 0x%08lx, Flip(ODD | EVEN) 0x%08lx, Flip(0) 0x%08lx\n",
              bob ? " with BOB | INTERLEAVED" : "", hr, odd, even, both, plain);
            ov->UpdateOverlay(nullptr, prim, nullptr, DDOVER_HIDE, nullptr);
        }
        static const Flag kAlpha[] = {{"ALPHADEST", DDOVER_ALPHADEST},
                                      {"ALPHADESTCONSTOVERRIDE", DDOVER_ALPHADESTCONSTOVERRIDE},
                                      {"ALPHADESTNEG", DDOVER_ALPHADESTNEG},
                                      {"ALPHADESTSURFACEOVERRIDE", DDOVER_ALPHADESTSURFACEOVERRIDE},
                                      {"ALPHAEDGEBLEND", DDOVER_ALPHAEDGEBLEND},
                                      {"ALPHASRC", DDOVER_ALPHASRC},
                                      {"ALPHASRCCONSTOVERRIDE", DDOVER_ALPHASRCCONSTOVERRIDE},
                                      {"ALPHASRCNEG", DDOVER_ALPHASRCNEG},
                                      {"ALPHASRCSURFACEOVERRIDE", DDOVER_ALPHASRCSURFACEOVERRIDE}};
        for (const Flag& f : kAlpha) {
            fx = {};
            fx.dwSize = sizeof(fx);
            fx.dwAlphaDestConstBitDepth = fx.dwAlphaSrcConstBitDepth = 8;
            fx.dwAlphaDestConst = fx.dwAlphaSrcConst = 0x80;
            fx.dwAlphaEdgeBlendBitDepth = 8;
            fx.dwAlphaEdgeBlend = 0x80;
            char name[64];
            snprintf(name, sizeof(name), "DDFX | %s", f.name);
            show(name, DDOVER_DDFX | f.flags, &fx);
        }
        static const Flag kFx[] = {{"ARITHSTRETCHY", DDOVERFX_ARITHSTRETCHY},
                                   {"MIRRORLEFTRIGHT", DDOVERFX_MIRRORLEFTRIGHT},
                                   {"MIRRORUPDOWN", DDOVERFX_MIRRORUPDOWN},
                                   {"DEINTERLACE", 0x00000008},
                                   {"0x10", 0x10}};
        for (const Flag& f : kFx) {
            fx = {};
            fx.dwSize = sizeof(fx);
            fx.dwDDFX = f.flags;
            char name[64];
            snprintf(name, sizeof(name), "DDFX, dwDDFX %s", f.name);
            show(name, DDOVER_DDFX, &fx);
        }
        // Colour controls on the overlay.
        IDirectDrawColorControl* cc = nullptr;
        hr = ov->QueryInterface(IID_IDirectDrawColorControl, reinterpret_cast<void**>(&cc));
        P("    the overlay's IDirectDrawColorControl: 0x%08lx", hr);
        if (cc) {
            DDCOLORCONTROL c = {};
            c.dwSize = sizeof(c);
            hr = cc->GetColorControls(&c);
            P(", GetColorControls 0x%08lx: flags 0x%lx, brightness %ld, contrast %ld, hue %ld, saturation %ld, sharpness %ld, "
              "gamma %ld, colour %ld",
              hr, c.dwFlags, c.lBrightness, c.lContrast, c.lHue, c.lSaturation, c.lSharpness, c.lGamma, c.lColorEnable);
            cc->Release();
        }
        P("\n");
        ov->Release();
    }
    IDirectDrawColorControl* cc = nullptr;
    hr = prim->QueryInterface(IID_IDirectDrawColorControl, reinterpret_cast<void**>(&cc));
    P("  the primary's IDirectDrawColorControl: 0x%08lx", hr);
    if (cc) {
        DDCOLORCONTROL c = {};
        c.dwSize = sizeof(c);
        hr = cc->GetColorControls(&c);
        P(", GetColorControls 0x%08lx: flags 0x%lx, brightness %ld, contrast %ld, hue %ld, saturation %ld, sharpness %ld, gamma %ld, "
          "colour %ld",
          hr, c.dwFlags, c.lBrightness, c.lContrast, c.lHue, c.lSaturation, c.lSharpness, c.lGamma, c.lColorEnable);
        cc->Release();
    }
    P("\n");
    IDirectDrawSurface7* plain = Make(dd, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY, 16, 16);
    if (plain) {
        cc = nullptr;
        P("  an offscreen surface's IDirectDrawColorControl: 0x%08lx\n",
          plain->QueryInterface(IID_IDirectDrawColorControl, reinterpret_cast<void**>(&cc)));
        if (cc) cc->Release();
        plain->Release();
    }
    prim->Release();
}

// The video-port interfaces (dvp.h; not in the headers used here).
struct VpConnect {
    DWORD dwSize, dwPortWidth;
    GUID guidTypeID;
    DWORD dwFlags;
    ULONG_PTR dwReserved1;
};
struct VpCaps {
    DWORD dwSize, dwFlags, dwMaxWidth, dwMaxVBIWidth, dwMaxHeight, dwVideoPortID, dwCaps, dwFX, dwNumAutoFlipSurfaces,
        dwAlignVideoPortBoundary, dwAlignVideoPortPrescaleWidth, dwAlignVideoPortCropBoundary, dwAlignVideoPortCropWidth,
        dwPreshrinkXStep, dwPreshrinkYStep, dwNumVBIAutoFlipSurfaces, dwNumPreferredAutoflip;
    WORD wNumFilterTapsX, wNumFilterTapsY;
};
struct VpDesc {
    DWORD dwSize, dwFieldWidth, dwVBIWidth, dwFieldHeight, dwMicrosecondsPerField, dwMaxPixelsPerSecond, dwVideoPortID,
        dwReserved1;
    VpConnect VideoPortType;
    ULONG_PTR dwReserved2, dwReserved3;
};
struct VpStatus {
    DWORD dwSize;
    BOOL bInUse;
    DWORD dwFlags, dwReserved1;
    VpConnect VideoPortType;
    ULONG_PTR dwReserved2, dwReserved3;
};
using VpEnumCb = HRESULT(WINAPI*)(VpCaps*, void*);
struct IVpContainer : IUnknown {
    virtual HRESULT WINAPI CreateVideoPort(DWORD, VpDesc*, IUnknown**, IUnknown*) = 0;
    virtual HRESULT WINAPI EnumVideoPorts(DWORD, VpCaps*, void*, VpEnumCb) = 0;
    virtual HRESULT WINAPI GetVideoPortConnectInfo(DWORD, DWORD*, VpConnect*) = 0;
    virtual HRESULT WINAPI QueryVideoPortStatus(DWORD, VpStatus*) = 0;
};
static const GUID kIidVpContainer = {0x6C142760, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
static const GUID kIidVideoPort = {0xB36D93E0, 0x2B43, 0x11CF, {0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56}};
static const GUID kIidVpNotify = {0xA655FB94, 0x0589, 0x4E57, {0xB3, 0x33, 0x56, 0x7A, 0x89, 0x46, 0x8C, 0x88}};
static const GUID kIidVaContainer = {0xACA12120, 0x3356, 0x11D1, {0x8F, 0xCF, 0x00, 0xC0, 0x4F, 0xC2, 0x9B, 0x4E}};

static int g_ports;
static HRESULT WINAPI PortCb(VpCaps* c, void*) {
    g_ports++;
    Hex("a port's caps", c, sizeof(VpCaps));
    return DDENUMRET_OK;
}

static void VideoPorts(IDirectDraw7* dd7, IDirectDraw* dd1) {
    DDCAPS drv = {}, hel = {};
    drv.dwSize = hel.dwSize = sizeof(DDCAPS);
    dd7->GetCaps(&drv, &hel);
    P("\nVideo ports (the driver's caps2 0x%08lx, at most %lu ports, %lu in use):\n", drv.dwCaps2, drv.dwMaxVideoPorts,
      drv.dwCurrVideoPorts);
    IUnknown* va = nullptr;
    const HRESULT hva = dd7->QueryInterface(kIidVaContainer, reinterpret_cast<void**>(&va));
    P("  IDDVideoAcceleratorContainer from IDirectDraw7: 0x%08lx\n", hva);
    if (va) va->Release();
    IVpContainer* c1 = nullptr;
    HRESULT hr = dd1->QueryInterface(kIidVpContainer, reinterpret_cast<void**>(&c1));
    P("  IDDVideoPortContainer from IDirectDraw: 0x%08lx\n", hr);
    if (c1) c1->Release();
    IVpContainer* c = nullptr;
    const ULONG before = Refs(dd7);
    hr = dd7->QueryInterface(kIidVpContainer, reinterpret_cast<void**>(&c));
    P("  IDDVideoPortContainer from IDirectDraw7: 0x%08lx (the object's count %lu before, %lu after)\n", hr, before, Refs(dd7));
    if (!c) return;
    IVpContainer* again = nullptr;
    dd7->QueryInterface(kIidVpContainer, reinterpret_cast<void**>(&again));
    IUnknown* back = nullptr;
    c->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&back));
    P("    the same pointer when asked again: %s; IDirectDraw7 from it: %s\n", again == c ? "yes" : "no",
      back == reinterpret_cast<IUnknown*>(dd7) ? "the object" : back ? "another pointer" : "none");
    if (again) again->Release();
    if (back) back->Release();
    g_ports = 0;
    hr = c->EnumVideoPorts(0, nullptr, nullptr, PortCb);
    P("    EnumVideoPorts: 0x%08lx, %d ports\n", hr, g_ports);
    g_ports = 0;
    hr = c->EnumVideoPorts(1, nullptr, nullptr, PortCb);
    P("    EnumVideoPorts with flags 1: 0x%08lx, %d ports\n", hr, g_ports);
    VpCaps filter = {};
    filter.dwSize = sizeof(filter);
    g_ports = 0;
    hr = c->EnumVideoPorts(0, &filter, nullptr, PortCb);
    P("    EnumVideoPorts with an empty filter: 0x%08lx, %d ports\n", hr, g_ports);
    filter.dwSize = 0;
    g_ports = 0;
    hr = c->EnumVideoPorts(0, &filter, nullptr, PortCb);
    P("    with a filter of size 0: 0x%08lx, %d ports\n", hr, g_ports);
    DWORD count = 0;
    hr = c->GetVideoPortConnectInfo(0, &count, nullptr);
    P("    GetVideoPortConnectInfo(0): 0x%08lx, %lu entries\n", hr, count);
    VpConnect conn[8] = {};
    if (SUCCEEDED(hr) && count) {
        DWORD k = count < 8 ? count : 8;
        for (DWORD i = 0; i < k; i++) conn[i].dwSize = sizeof(VpConnect);
        hr = c->GetVideoPortConnectInfo(0, &k, conn);
        P("    with an array: 0x%08lx, %lu entries\n", hr, k);
        for (DWORD i = 0; i < k && i < 8; i++) Hex("an entry", &conn[i], sizeof(VpConnect));
    }
    count = 0;
    hr = c->GetVideoPortConnectInfo(99, &count, nullptr);
    P("    GetVideoPortConnectInfo(99): 0x%08lx\n", hr);
    VpStatus st = {};
    st.dwSize = sizeof(st);
    hr = c->QueryVideoPortStatus(0, &st);
    P("    QueryVideoPortStatus(0): 0x%08lx\n", hr);
    if (SUCCEEDED(hr)) Hex("the status", &st, sizeof(st));
    st = {};
    st.dwSize = sizeof(st);
    const HRESULT st99 = c->QueryVideoPortStatus(99, &st);
    st = {};
    const HRESULT st0 = c->QueryVideoPortStatus(0, &st);
    P("    QueryVideoPortStatus(99): 0x%08lx; with dwSize 0: 0x%08lx\n", st99, st0);
    VpDesc d = {};
    d.dwSize = sizeof(d);
    d.dwFieldWidth = 640;
    d.dwFieldHeight = 240;
    d.dwMicrosecondsPerField = 16683;
    d.dwMaxPixelsPerSecond = 13500000;
    d.dwVideoPortID = 0;
    d.VideoPortType = conn[0];
    d.VideoPortType.dwSize = sizeof(VpConnect);
    IUnknown* vp = nullptr;
    hr = c->CreateVideoPort(0, &d, &vp, nullptr);
    P("    CreateVideoPort(0, port 0 at 640x240, its first connection): 0x%08lx\n", hr);
    if (vp) {
        IUnknown* x = nullptr;
        P("      IDirectDrawVideoPort from it: 0x%08lx; ", vp->QueryInterface(kIidVideoPort, reinterpret_cast<void**>(&x)));
        if (x) x->Release();
        x = nullptr;
        P("IDirectDrawVideoPortNotify: 0x%08lx\n", vp->QueryInterface(kIidVpNotify, reinterpret_cast<void**>(&x)));
        if (x) x->Release();
        vp->Release();
    }
    vp = nullptr;
    P("    CreateVideoPort with no description: 0x%08lx; ", c->CreateVideoPort(0, nullptr, &vp, nullptr));
    if (vp) vp->Release();
    vp = nullptr;
    d.dwVideoPortID = 99;
    P("port 99: 0x%08lx; ", c->CreateVideoPort(0, &d, &vp, nullptr));
    if (vp) vp->Release();
    vp = nullptr;
    d.dwVideoPortID = 0;
    d.dwSize = 0;
    P("dwSize 0: 0x%08lx; ", c->CreateVideoPort(0, &d, &vp, nullptr));
    if (vp) vp->Release();
    P("no out pointer: 0x%08lx\n", c->CreateVideoPort(0, &d, nullptr, nullptr));
    // (Last: what a ddraw.dll may fault on.)
    hr = c->EnumVideoPorts(0, nullptr, nullptr, nullptr);
    P("    EnumVideoPorts with no callback: 0x%08lx; ", hr);
    hr = c->GetVideoPortConnectInfo(0, nullptr, nullptr);
    P("GetVideoPortConnectInfo with no count: 0x%08lx; ", hr);
    hr = c->QueryVideoPortStatus(0, nullptr);
    P("QueryVideoPortStatus with none: 0x%08lx\n", hr);
    const ULONG left = c->Release();
    P("    Release of the container: %lu (the object's count %lu)\n", left, Refs(dd7));
}

// Z buffers whose depth and stencil masks share bits.
static void SharedZMasks(IDirectDraw7* dd) {
    P("\nZ buffers through CreateSurface (64x64) whose depth and stencil masks share bits:\n");
    struct Z {
        const char* name;
        DWORD bits, zmask, stencilBits, stencilMask;
    };
    static const Z kZ[] = {{"16 bits of depth, 1 of stencil in its lowest bit", 16, 0xFFFF, 1, 0x0001},
                           {"15 bits of depth, 2 of stencil, sharing one", 16, 0xFFFE, 2, 0x0003},
                           {"8 bits of depth and 8 of stencil, the same 8", 16, 0xFF00, 8, 0xFF00},
                           {"32 bits of depth, 8 of stencil in its lowest byte", 32, 0xFFFFFFFF, 8, 0xFF},
                           {"24 bits of depth, 9 of stencil, sharing one", 32, 0xFFFFFF00, 9, 0x1FF},
                           {"24 bits of depth low, 8 of stencil inside it", 32, 0x00FFFFFF, 8, 0x00FF0000},
                           {"15 bits of depth, 1 of stencil outside the word", 16, 0xFFFE, 1, 0x10000}};
    for (const Z& k : kZ) {
        P("  %s (Z mask 0x%lx, stencil mask 0x%lx):\n", k.name, k.zmask, k.stencilMask);
        for (DWORD memory : {DWORD(0), DWORD(DDSCAPS_SYSTEMMEMORY), DWORD(DDSCAPS_VIDEOMEMORY)}) {
            HRESULT hr;
            IDirectDrawSurface7* s = MakeZ(dd, memory, k.bits, k.zmask, k.stencilBits, k.stencilMask, &hr);
            P("    %s: 0x%08lx", !memory ? "no memory named" : memory == DDSCAPS_SYSTEMMEMORY ? "system memory" : "video memory",
              hr);
            if (s) {
                DDSURFACEDESC2 got = {};
                got.dwSize = sizeof(got);
                s->GetSurfaceDesc(&got);
                P(", caps 0x%08lx, pitch %ld, flags 0x%lx, %lu bits, Z mask 0x%lx, %lu stencil bits, stencil mask 0x%lx",
                  got.ddsCaps.dwCaps, got.lPitch, got.ddpfPixelFormat.dwFlags, got.ddpfPixelFormat.dwZBufferBitDepth,
                  got.ddpfPixelFormat.dwZBitMask, got.ddpfPixelFormat.dwStencilBitDepth, got.ddpfPixelFormat.dwStencilBitMask);
                DDBLTFX fx = {};
                fx.dwSize = sizeof(fx);
                fx.dwFillDepth = 0x12345678;
                P("; DEPTHFILL 0x12345678: 0x%08lx", s->Blt(nullptr, nullptr, nullptr, DDBLT_DEPTHFILL | DDBLT_WAIT, &fx));
                DDSURFACEDESC2 l = {};
                l.dwSize = sizeof(l);
                if (SUCCEEDED(s->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
                    if (k.bits == 16) P(", the word 0x%04x", *static_cast<const WORD*>(l.lpSurface));
                    else P(", the word 0x%08lx", *static_cast<const DWORD*>(l.lpSurface));
                    s->Unlock(nullptr);
                }
                s->Release();
            }
            P("\n");
        }
    }
}

// The device window (DDSCL_CREATEDEVICEWINDOW) and mouse messages: posted to it, and real
// clicks over it. Where they go: the focus window's procedure, or the device window's only.
static int g_focusMouse;
static char g_focusLog[512];
static LRESULT CALLBACK FocusProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
        g_focusMouse++;
        const size_t used = strlen(g_focusLog);
        if (used < sizeof(g_focusLog) - 24) snprintf(g_focusLog + used, sizeof(g_focusLog) - used, " 0x%x(%ld,%ld)", msg,
                                                     long(short(LOWORD(lp))), long(short(HIWORD(lp))));
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}
static HWND g_deviceWnd;
static int g_deviceMouse;
static LRESULT CALLBACK SeeDeviceMessages(int code, WPARAM wp, LPARAM lp) {
    const MSG* m = reinterpret_cast<const MSG*>(lp);
    if (code >= 0 && m && m->hwnd == g_deviceWnd && m->message >= WM_MOUSEFIRST && m->message <= WM_MOUSELAST && wp == PM_REMOVE)
        g_deviceMouse++;
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void DeviceWindowMouse(IDirectDraw7* dd) {
    P("\nThe device window (DDSCL_CREATEDEVICEWINDOW) and the mouse:\n");
    WNDCLASSA wc = {};
    wc.lpfnWndProc = FocusProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "fmtprobe focus window";
    wc.style = CS_DBLCLKS;
    RegisterClassA(&wc);
    HWND focus = CreateWindowExA(0, wc.lpszClassName, "fmtprobe", WS_POPUP | WS_VISIBLE, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance,
                                 nullptr);
    Pump(200);
    HRESULT hr = dd->SetCooperativeLevel(focus, DDSCL_SETFOCUSWINDOW);
    const HRESULT hr2 = dd->SetCooperativeLevel(nullptr, DDSCL_CREATEDEVICEWINDOW | DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    const HRESULT hm = dd->SetDisplayMode(640, 480, 16, 0, 0);
    Pump(400);
    g_deviceWnd = FindWindowA("DirectDrawDeviceWnd", nullptr);
    P("  SETFOCUSWINDOW 0x%08lx, CREATEDEVICEWINDOW | EXCLUSIVE | FULLSCREEN 0x%08lx, SetDisplayMode(640, 480, 16) 0x%08lx; "
      "the device window %s\n",
      hr, hr2, hm, g_deviceWnd ? "found" : "not found");
    if (g_deviceWnd) {
        const RECT r = WindowRect(g_deviceWnd);
        P("    at %ld,%ld %ldx%ld, style 0x%lx, ex style 0x%lx, owner %s, %s, class style 0x%lx\n", r.left, r.top, r.right - r.left,
          r.bottom - r.top, DWORD(GetWindowLongA(g_deviceWnd, GWL_STYLE)), DWORD(GetWindowLongA(g_deviceWnd, GWL_EXSTYLE)),
          GetWindow(g_deviceWnd, GW_OWNER) == focus ? "the focus window" : "another",
          IsWindowVisible(g_deviceWnd) ? "visible" : "hidden", DWORD(GetClassLongA(g_deviceWnd, GCL_STYLE)));
        HHOOK hook = SetWindowsHookExA(WH_GETMESSAGE, SeeDeviceMessages, nullptr, GetCurrentThreadId());
        struct Post {
            UINT msg;
            WPARAM wp;
        };
        static const Post kPosts[] = {{WM_MOUSEMOVE, 0},          {WM_LBUTTONDOWN, MK_LBUTTON}, {WM_LBUTTONUP, 0},
                                      {WM_LBUTTONDBLCLK, MK_LBUTTON}, {WM_RBUTTONDOWN, MK_RBUTTON}, {WM_RBUTTONUP, 0},
                                      {WM_MOUSEWHEEL, WPARAM(120 << 16)}};
        g_focusMouse = g_deviceMouse = 0;
        g_focusLog[0] = 0;
        for (const Post& p : kPosts) PostMessageA(g_deviceWnd, p.msg, p.wp, MAKELPARAM(20, 30));
        Pump(300);
        P("    7 mouse messages posted to it: the device window took %d, the focus window got %d:%s\n", g_deviceMouse, g_focusMouse,
          g_focusLog);
        g_focusMouse = 0;
        g_focusLog[0] = 0;
        const LRESULT sent = SendMessageA(g_deviceWnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 30));
        Pump(200);
        P("    WM_LBUTTONDOWN sent to it: returned %ld, the focus window got %d:%s\n", long(sent), g_focusMouse, g_focusLog);
        // A real click over it (the screen's middle): input goes to the window under the pointer.
        g_focusMouse = g_deviceMouse = 0;
        g_focusLog[0] = 0;
        const int cx = GetSystemMetrics(SM_CXSCREEN) / 2, cy = GetSystemMetrics(SM_CYSCREEN) / 2;
        SetCursorPos(cx, cy);
        Pump(100);
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(2, in, sizeof(INPUT));
        Pump(300);
        POINT pt = {cx, cy};
        P("    a real click at %d,%d (the window there: %s): the device window took %d, the focus window got %d:%s\n", cx, cy,
          WindowFromPoint(pt) == g_deviceWnd ? "the device window" : WindowFromPoint(pt) == focus ? "the focus window" : "another",
          g_deviceMouse, g_focusMouse, g_focusLog);
        if (hook) UnhookWindowsHookEx(hook);
    }
    dd->RestoreDisplayMode();
    dd->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
    DestroyWindow(focus);
    Pump(200);
}

// DDSCAPS_OWNDC: the DC kept, a second GetDC, Lock and Blt while it is out, ReleaseDC's checks.
static void OwnDc(IDirectDraw7* dd) {
    P("\nDDSCAPS_OWNDC (a 64x64 5-6-5 surface):\n");
    for (DWORD memory : {DWORD(DDSCAPS_SYSTEMMEMORY), DWORD(DDSCAPS_VIDEOMEMORY), DWORD(0)}) {
        HRESULT hr;
        IDirectDrawSurface7* s = Make(dd, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_OWNDC | memory, 64, 64, &k565, &hr);
        P("  %s: 0x%08lx\n", memory == DDSCAPS_SYSTEMMEMORY ? "system memory" : memory ? "video memory" : "no memory named", hr);
        if (!s) continue;
        DWORD u = 0;
        auto uniq = [&]() {
            u = 0;
            s->GetUniquenessValue(&u);
            return u;
        };
        P("    uniqueness at first %lu\n", uniq());
        HDC dc1 = nullptr, dc2 = nullptr;
        hr = s->GetDC(&dc1);
        P("    GetDC 0x%08lx (uniqueness %lu); ", hr, uniq());
        HRESULT hr2 = s->GetDC(&dc2);
        P("again 0x%08lx, %s\n", hr2, !dc2 ? "no DC" : dc2 == dc1 ? "the same DC" : "another DC");
        DDSURFACEDESC2 l = {};
        l.dwSize = sizeof(l);
        hr = s->Lock(nullptr, &l, DDLOCK_WAIT, nullptr);
        P("    Lock with the DC out 0x%08lx", hr);
        if (SUCCEEDED(hr)) P(", Unlock 0x%08lx", s->Unlock(nullptr));
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = 0x1234;
        hr = s->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        P("; a colour fill 0x%08lx (uniqueness %lu)\n", hr, uniq());
        if (dc1) {
            // The DC and the memory, both ways, with the DC still out (no GdiFlush).
            P("    with the DC out: the fill on the DC 0x%06lx", DWORD(GetPixel(dc1, 0, 0)));
            SetPixelV(dc1, 3, 2, RGB(255, 0, 0));
            l = {};
            l.dwSize = sizeof(l);
            if (SUCCEEDED(s->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
                const WORD* row = reinterpret_cast<const WORD*>(static_cast<const BYTE*>(l.lpSurface) + 2 * l.lPitch);
                P("; GDI's red pixel at the next lock 0x%04x", row[3]);
                s->Unlock(nullptr);
            }
            l = {};
            l.dwSize = sizeof(l);
            if (SUCCEEDED(s->Lock(nullptr, &l, DDLOCK_WAIT, nullptr))) {
                static_cast<WORD*>(l.lpSurface)[5] = 0x001F;
                s->Unlock(nullptr);
                P("; the lock's blue (0x001f) on the DC 0x%06lx", DWORD(GetPixel(dc1, 5, 0)));
            }
            P("\n");
        }
        HDC other = CreateCompatibleDC(nullptr);
        P("    ReleaseDC of another DC 0x%08lx; ", s->ReleaseDC(other));
        DeleteDC(other);
        hr = s->ReleaseDC(dc1);
        P("of its own 0x%08lx (uniqueness %lu); ", hr, uniq());
        P("again 0x%08lx\n", s->ReleaseDC(dc1));
        HDC dc3 = nullptr;
        hr = s->GetDC(&dc3);
        P("    GetDC after 0x%08lx, %s; ReleaseDC 0x%08lx\n", hr, !dc3 ? "no DC" : dc3 == dc1 ? "the same DC as before" : "another DC",
          s->ReleaseDC(dc3));
        IDirectDrawSurface* s1 = nullptr;
        s->QueryInterface(IID_IDirectDrawSurface, reinterpret_cast<void**>(&s1));
        if (s1) {
            HDC dc4 = nullptr;
            hr = s1->GetDC(&dc4);
            P("    through IDirectDrawSurface: GetDC 0x%08lx, ReleaseDC(NULL) 0x%08lx\n", hr, s1->ReleaseDC(nullptr));
            s1->Release();
        }
        s->Release();
    }
    HRESULT hr;
    IDirectDrawSurface7* t = Make(dd, DDSCAPS_TEXTURE | DDSCAPS_OWNDC | DDSCAPS_SYSTEMMEMORY, 64, 64, &k565, &hr);
    P("  a system-memory texture with OWNDC: 0x%08lx\n", hr);
    if (t) t->Release();
    // 8 bits with a palette (entry i = i, 255 - i, i * 7), in system memory: with OWNDC and
    // without, the DC and the memory both ways, and a palette change while the DC is out.
    PALETTEENTRY e[256] = {};
    for (int i = 0; i < 256; i++) e[i] = {BYTE(i), BYTE(255 - i), BYTE(i * 7), 0};
    for (const bool own : {true, false}) {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY | (own ? DWORD(DDSCAPS_OWNDC) : 0);
        d.dwWidth = d.dwHeight = 16;
        d.ddpfPixelFormat.dwSize = sizeof(d.ddpfPixelFormat);
        d.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        d.ddpfPixelFormat.dwRGBBitCount = 8;
        IDirectDrawSurface7* p8 = nullptr;
        hr = dd->CreateSurface(&d, &p8, nullptr);
        IDirectDrawPalette* pal = nullptr;
        const HRESULT hp = dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, e, &pal, nullptr);
        const HRESULT hs = p8 && pal ? p8->SetPalette(pal) : E_FAIL;
        P("  8 bits%s: CreateSurface 0x%08lx, CreatePalette 0x%08lx, SetPalette 0x%08lx\n", own ? " with OWNDC" : "", hr, hp, hs);
        auto at = [&](int x, int y) {  // an index through a lock, -1 where it fails
            DDSURFACEDESC2 pl = {};
            pl.dwSize = sizeof(pl);
            if (FAILED(p8->Lock(nullptr, &pl, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) return -1;
            const int v = static_cast<const BYTE*>(pl.lpSurface)[y * pl.lPitch + x];
            p8->Unlock(nullptr);
            return v;
        };
        auto put = [&](int x, BYTE v) {  // an index written through a lock, on row 0
            DDSURFACEDESC2 pl = {};
            pl.dwSize = sizeof(pl);
            const HRESULT h = p8->Lock(nullptr, &pl, DDLOCK_WAIT, nullptr);
            if (SUCCEEDED(h)) {
                static_cast<BYTE*>(pl.lpSurface)[x] = v;
                p8->Unlock(nullptr);
            }
            return h;
        };
        auto colour = [&](HDC dc, int x) {
            const COLORREF c = GetPixel(dc, x, 0);
            static char t[48];
            snprintf(t, sizeof(t), "0x%06lx%s", DWORD(c), c == RGB(e[9].peRed, e[9].peGreen, e[9].peBlue) ? " (entry 9's colour)" : "");
            return t;
        };
        if (p8 && SUCCEEDED(hs)) {
            put(2, 9);
            HDC pdc = nullptr;
            hr = p8->GetDC(&pdc);
            P("    index 9 written, then GetDC 0x%08lx", hr);
            if (SUCCEEDED(hr)) {
                P(": on the DC %s", colour(pdc, 2));
                SetPixelV(pdc, 1, 1, RGB(e[5].peRed, e[5].peGreen, e[5].peBlue));
                hr = put(3, 9);
                P("; with the DC out: Lock 0x%08lx", hr);
                if (SUCCEEDED(hr))
                    P(", GDI's pixel of entry 5's colour index %d, index 9 written through it on the DC %s", at(1, 1),
                      colour(pdc, 3));
                PALETTEENTRY n = {1, 2, 3, 0};
                hr = pal->SetEntries(0, 9, 1, &n);
                P("\n    entry 9 set to 1, 2, 3 (0x%08lx) with the DC out: on the DC 0x%06lx", hr, DWORD(GetPixel(pdc, 2, 0)));
                hr = p8->ReleaseDC(pdc);
                P("; ReleaseDC 0x%08lx, GDI's pixel index %d", hr, at(1, 1));
                HDC again = nullptr;
                const HRESULT hg = p8->GetDC(&again);
                P("; GetDC 0x%08lx: on the DC 0x%06lx", hg, again ? DWORD(GetPixel(again, 2, 0)) : 0);
                if (again) p8->ReleaseDC(again);
                pal->SetEntries(0, 9, 1, &e[9]);
            }
            P("\n");
        }
        if (pal) pal->Release();
        if (p8) p8->Release();
    }
}

// Activation: the system settings while exclusive, the flip chain at a focus loss, Alt+Tab's
// key-up, WM_ACTIVATEAPP(TRUE) to a minimized window and while another of the program's
// windows is in front. Messages are sent here (the system's own switching isn't used).
static void Settings(const char* when) {
    BOOL shadow = FALSE, hot = FALSE, saver = FALSE, ime = FALSE;
    SystemParametersInfoA(SPI_GETCURSORSHADOW, 0, &shadow, 0);
    SystemParametersInfoA(SPI_GETHOTTRACKING, 0, &hot, 0);
    SystemParametersInfoA(SPI_GETSCREENSAVERRUNNING, 0, &saver, 0);
    SystemParametersInfoA(SPI_GETSHOWIMEUI, 0, &ime, 0);
    const UINT mode = SetErrorMode(0);
    SetErrorMode(mode);
    // The semaphore: there or not, and held (count 0) or free (taken here and given back).
    const char* semaphore = "not there";
    if (HANDLE sem = OpenSemaphoreA(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, "DDrawCheckFullscreenSemaphore")) {
        semaphore = "there, held";
        if (WaitForSingleObject(sem, 0) == WAIT_OBJECT_0) {
            semaphore = "there, free";
            ReleaseSemaphore(sem, 1, nullptr);
        }
        CloseHandle(sem);
    }
    P("    %s: cursor shadow %d, hot tracking %d, screen saver running %d, IME shown %d, error mode 0x%x, the fullscreen semaphore "
      "%s\n",
      when, shadow, hot, saver, ime, mode, semaphore);
}
static void Messages() {
    P("    messages:");
    for (int i = 0; i < g_messageCount; i++) P(" %s", g_messages[i]);
    P("\n");
    g_messageCount = 0;
}

static void Activation(IDirectDraw7* dd) {
    P("\nActivation and focus loss (messages sent to the window; the system's switching isn't used):\n");
    WNDCLASSA wc = {};
    wc.lpfnWndProc = ProbeProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "fmtprobe activation";
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassA(&wc);
    const UINT keep = SetErrorMode(SEM_NOGPFAULTERRORBOX);  // (a mode of the section's own, so that ddraw's shows)
    Settings("before");
    // ddraw's CheckFullscreen export (what Windows asks to find a program holding the screen).
    typedef BOOL(WINAPI * CheckFn)();
    const FARPROC checkFn = GetProcAddress(GetModuleHandleA("ddraw.dll"), "CheckFullscreen");
    const auto check = reinterpret_cast<CheckFn>(reinterpret_cast<void*>(checkFn));
    if (check) P("  CheckFullscreen before: %d\n", check());
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "fmtprobe", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, nullptr, nullptr, wc.hInstance,
                                nullptr);
    Pump(200);
    g_logMessages = true;
    g_messageCount = 0;
    HRESULT hr = dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    const HRESULT hm = dd->SetDisplayMode(640, 480, 16, 0, 0);
    Pump(400);
    P("  SetCooperativeLevel(EXCLUSIVE | FULLSCREEN) 0x%08lx, SetDisplayMode(640, 480, 16) 0x%08lx\n", hr, hm);
    Settings("exclusive, in front");
    Messages();
    DDSURFACEDESC2 fd = {};
    fd.dwSize = sizeof(fd);
    fd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    fd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    fd.dwBackBufferCount = 1;
    IDirectDrawSurface7* front = nullptr;
    const HRESULT made = dd->CreateSurface(&fd, &front, nullptr);
    IDirectDrawSurface7* backBuf = nullptr;
    DDSCAPS2 bc = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
    if (front) front->GetAttachedSurface(&bc, &backBuf);
    auto gdiIs = [&]() {
        IDirectDrawSurface7* g = nullptr;
        dd->GetGDISurface(&g);
        const char* r = !g            ? "none"
                        : g == front   ? "the front buffer's interface"
                        : g == backBuf ? "the back buffer's interface"
                                       : "another";
        if (g) g->Release();
        return r;
    };
    // (Lock reads the first word: the buffers are filled with 0x1111 and 0x2222 to tell them apart.)
    auto word = [](IDirectDrawSurface7* s) {
        DDSURFACEDESC2 l = {};
        l.dwSize = sizeof(l);
        if (!s || FAILED(s->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) return DWORD(0xDEAD);
        const DWORD v = *static_cast<const WORD*>(l.lpSurface);
        s->Unlock(nullptr);
        return v;
    };
    if (front && backBuf) {
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = 0x1111;
        front->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        fx.dwFillColor = 0x2222;
        backBuf->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        hr = front->Flip(nullptr, DDFLIP_WAIT);
        const DWORD f = word(front), b = word(backBuf);
        P("  a flipping primary 0x%08lx; after one Flip (0x%08lx): the front holds 0x%lx, the back 0x%lx, the GDI surface is %s\n", made,
          hr, f, b, gdiIs());
    } else {
        P("  no flipping primary: 0x%08lx\n", made);
    }
    g_messageCount = 0;
    SendMessageA(hwnd, WM_ACTIVATEAPP, FALSE, 0);
    Pump(400);
    P("  WM_ACTIVATEAPP(FALSE) sent: %s, %s; TestCooperativeLevel 0x%08lx\n", IsIconic(hwnd) ? "minimized" : "not minimized",
      GetForegroundWindow() == hwnd ? "foreground" : "not foreground", dd->TestCooperativeLevel());
    Settings("after the focus loss");
    Messages();
    if (front) {
        const HRESULT rf = front->Restore(), rb = backBuf ? backBuf->Restore() : E_FAIL;
        const DWORD f = word(front), b = word(backBuf);
        P("    after Restore (0x%08lx, 0x%08lx): the front holds 0x%lx, the back 0x%lx, the GDI surface is %s\n", rf, rb, f, b, gdiIs());
    }
    // A minimized window told it's active, then shown again.
    P("  the window minimized (ShowWindow SW_MINIMIZE):\n");
    ShowWindow(hwnd, SW_MINIMIZE);
    Pump(300);
    Messages();
    g_messageCount = 0;
    SendMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
    Pump(300);
    P("  WM_ACTIVATEAPP(TRUE) sent to the minimized window: %s, mode %s\n", IsIconic(hwnd) ? "still minimized" : "restored",
      dd->TestCooperativeLevel() == DD_OK ? "the game's" : "not the game's");
    Messages();
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    Pump(600);
    P("  restored (SW_RESTORE, SetForegroundWindow): %s; TestCooperativeLevel 0x%08lx\n",
      GetForegroundWindow() == hwnd ? "foreground" : "not foreground", dd->TestCooperativeLevel());
    Settings("restored");
    Messages();
    // Another of the program's windows in front.
    HWND other = CreateWindowExA(0, wc.lpszClassName, "fmtprobe other", WS_POPUP | WS_VISIBLE, 100, 100, 100, 100, nullptr, nullptr,
                                 wc.hInstance, nullptr);
    SetForegroundWindow(other);
    Pump(300);
    g_messageCount = 0;
    SendMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
    Pump(300);
    P("  another window of the program in front (%s), WM_ACTIVATEAPP(TRUE) sent to the game's: TestCooperativeLevel 0x%08lx\n",
      GetForegroundWindow() == other ? "foreground" : "not foreground", dd->TestCooperativeLevel());
    Messages();
    DestroyWindow(other);
    SetForegroundWindow(hwnd);
    SendMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
    Pump(400);
    // Alt+Tab's and Alt+Esc's key-up.
    struct Key {
        const char* name;
        WPARAM vk;
        LPARAM lp;
    };
    static const Key kKeys[] = {{"Tab with Alt (bit 29)", VK_TAB, LPARAM(0xE00F0001)},
                                {"Tab without Alt", VK_TAB, LPARAM(0xC00F0001)},
                                {"Esc with Alt", VK_ESCAPE, LPARAM(0xE0010001)},
                                {"Esc without Alt", VK_ESCAPE, LPARAM(0xC0010001)},
                                {"F4 with Alt", VK_F4, LPARAM(0xE03E0001)}};
    for (const Key& k : kKeys) {
        SetForegroundWindow(hwnd);
        SendMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
        Pump(300);
        g_messageCount = 0;
        SendMessageA(hwnd, WM_SYSKEYUP, k.vk, k.lp);
        Pump(300);
        P("  WM_SYSKEYUP, %s: %s; TestCooperativeLevel 0x%08lx\n", k.name, IsIconic(hwnd) ? "minimized" : "not minimized",
          dd->TestCooperativeLevel());
        Messages();
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    }
    auto inFront = [&]() {
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        SendMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
        Pump(400);
    };
    // The Tab key-up of the Alt+Tab that switched to the game (Alt down as it activates): the
    // window loses the focus, Alt is pressed (SendInput), the window is restored and brought
    // in front, then two Tab key-ups.
    inFront();
    SendMessageA(hwnd, WM_ACTIVATEAPP, FALSE, 0);
    Pump(400);
    INPUT alt = {};
    alt.type = INPUT_KEYBOARD;
    alt.ki.wVk = VK_MENU;
    SendInput(1, &alt, sizeof(alt));
    Pump(100);
    const bool altDown = GetAsyncKeyState(VK_MENU) < 0;
    g_messageCount = 0;
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
    Pump(600);
    P("  Alt held (%s), the window restored and in front: %s; TestCooperativeLevel 0x%08lx\n", altDown ? "down" : "not down",
      GetForegroundWindow() == hwnd ? "foreground" : "not foreground", dd->TestCooperativeLevel());
    Messages();
    for (int i = 0; i < 2 && !IsIconic(hwnd); i++) {
        g_messageCount = 0;
        SendMessageA(hwnd, WM_SYSKEYUP, VK_TAB, LPARAM(0xE00F0001));
        Pump(300);
        P("    WM_SYSKEYUP, Tab with Alt, %s: %s; TestCooperativeLevel 0x%08lx\n", i ? "the second" : "the first",
          IsIconic(hwnd) ? "minimized" : "not minimized", dd->TestCooperativeLevel());
        Messages();
    }
    alt.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &alt, sizeof(alt));
    Pump(300);
    // WM_POWER(PWR_SUSPENDREQUEST), in front.
    inFront();
    g_messageCount = 0;
    const LRESULT pr = SendMessageA(hwnd, WM_POWER, PWR_SUSPENDREQUEST, 0);
    Pump(400);
    P("  WM_POWER(PWR_SUSPENDREQUEST) sent (returned %ld): %s; TestCooperativeLevel 0x%08lx\n", long(pr),
      IsIconic(hwnd) ? "minimized" : "not minimized", dd->TestCooperativeLevel());
    Settings("after WM_POWER");
    Messages();
    // ddraw's CheckFullscreen again, last (where the screen is held, it resets the mode).
    if (check) {
        inFront();
        Settings("in front again");
        g_messageCount = 0;
        const BOOL c1 = check();
        Pump(600);
        DEVMODEA m = {};
        m.dmSize = sizeof(m);
        EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &m);
        P("  CheckFullscreen, exclusive and in front: %d; the display mode after it %lux%lux%lu; TestCooperativeLevel 0x%08lx\n", c1,
          m.dmPelsWidth, m.dmPelsHeight, m.dmBitsPerPel, dd->TestCooperativeLevel());
        Settings("after CheckFullscreen");
        Messages();
        P("    again: %d\n", check());
    } else {
        P("  CheckFullscreen: not exported\n");
    }
    g_logMessages = false;
    if (backBuf) backBuf->Release();
    if (front) front->Release();
    dd->RestoreDisplayMode();
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    Pump(200);
    Settings("normal again");
    if (check) P("  CheckFullscreen, normal again: %d\n", check());
    DestroyWindow(hwnd);
    SetErrorMode(keep);
}

// YUV colour fills (what dwFillColor becomes in the memory) and planar surfaces (YV12, I420):
// where they are made, and filled.
static void YuvFills(IDirectDraw7* dd) {
    P("\nYUV surfaces: colour fills, and planar ones:\n");
    static const DWORD kCodes[] = {MAKEFOURCC('Y', 'U', 'Y', '2'), MAKEFOURCC('U', 'Y', 'V', 'Y'), MAKEFOURCC('Y', 'V', '1', '2'),
                                   MAKEFOURCC('I', '4', '2', '0'), MAKEFOURCC('I', 'Y', 'U', 'V')};
    struct Kind {
        const char* name;
        DWORD caps;
    };
    static const Kind kKinds[] = {{"offscreen, video memory", DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY},
                                  {"offscreen, system memory", DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY},
                                  {"overlay, video memory", DDSCAPS_OVERLAY | DDSCAPS_VIDEOMEMORY},
                                  {"texture, video memory", DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY},
                                  {"texture, system memory", DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY}};
    for (DWORD code : kCodes)
        for (const Kind& k : kKinds) {
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            d.ddsCaps.dwCaps = k.caps;
            d.dwWidth = 8;
            d.dwHeight = 4;
            d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            d.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
            d.ddpfPixelFormat.dwFourCC = code;
            IDirectDrawSurface7* s = nullptr;
            HRESULT hr = dd->CreateSurface(&d, &s, nullptr);
            P("  %.4s, %s: 0x%08lx", reinterpret_cast<const char*>(&code), k.name, hr);
            if (s) {
                for (DWORD fill : {DWORD(0x12345678), DWORD(0x00000000), DWORD(0xFFFFFFFF)}) {
                    DDBLTFX fx = {};
                    fx.dwSize = sizeof(fx);
                    fx.dwFillColor = fill;
                    hr = s->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
                    P("; fill %08lx: 0x%08lx", fill, hr);
                    DDSURFACEDESC2 l = {};
                    l.dwSize = sizeof(l);
                    if (SUCCEEDED(hr) && SUCCEEDED(s->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
                        const BYTE* p = static_cast<const BYTE*>(l.lpSurface);
                        P(", pitch %ld, row 0:", l.lPitch);
                        for (int i = 0; i < 16; i++) P(" %02x", p[i]);
                        // Planar: the bytes after the 4 rows of Y (the next plane, as laid out).
                        if (code != MAKEFOURCC('Y', 'U', 'Y', '2') && code != MAKEFOURCC('U', 'Y', 'V', 'Y')) {
                            P(", after Y:");
                            for (int i = 0; i < 8; i++) P(" %02x", p[4 * l.lPitch + i]);
                        }
                        s->Unlock(nullptr);
                    }
                }
                s->Release();
            }
            P("\n");
        }
}

// The gamma calibrator: fmtcal.dll beside the program, registered under
// HKLM\Software\Microsoft\DirectDraw\GammaCalibrator ("Path") while the next section runs in
// a process of its own (ddraw.dll reads the key as it loads), then the key as it was.
static void CalibratorRegistered();
static int SectionIndex(void (*run)());
static void Calibrator() {
    P("\nThe gamma calibrator (fmtcal.dll beside the program, registered while the next section runs):\n");
    char dll[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    strcpy(dll, exe);
    strcpy(strrchr(dll, '\\') + 1, "fmtcal.dll");
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        P("  fmtcal.dll isn't there\n");
        return;
    }
    HKEY key = nullptr;
    DWORD made = 0;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\DirectDraw\\GammaCalibrator", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                        &key, &made) != ERROR_SUCCESS) {
        P("  the key couldn't be opened (not an administrator?)\n");
        return;
    }
    char old[MAX_PATH] = "";
    DWORD type = 0, size = sizeof(old);
    const bool hadPath = RegQueryValueExA(key, "Path", nullptr, &type, reinterpret_cast<BYTE*>(old), &size) == ERROR_SUCCESS;
    RegSetValueExA(key, "Path", 0, REG_SZ, reinterpret_cast<const BYTE*>(dll), DWORD(strlen(dll) + 1));
    fflush(g_out);
    fclose(g_out);
    g_out = nullptr;
    char cmd[MAX_PATH + 64];
    snprintf(cmd, sizeof(cmd), "\"%s\" section %d", exe, SectionIndex(CalibratorRegistered));
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    const bool started = CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) != FALSE;
    if (started) {
        if (WaitForSingleObject(pi.hProcess, 120000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (hadPath) RegSetValueExA(key, "Path", 0, type, reinterpret_cast<const BYTE*>(old), size);
    else RegDeleteValueA(key, "Path");
    RegCloseKey(key);
    if (made == REG_CREATED_NEW_KEY) RegDeleteKeyA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\DirectDraw\\GammaCalibrator");
    ChangeDisplaySettingsA(nullptr, 0);
    g_out = fopen(g_path, "a");  // (the section's file again; FmtprobeSection closes it)
    if (g_out) setvbuf(g_out, nullptr, _IONBF, 0);
    if (!started && g_out) P("  the next section couldn't be started (%lu)\n", GetLastError());
}

static void CalibratorRegistered() {
    char log[MAX_PATH], fail[MAX_PATH];
    GetModuleFileNameA(nullptr, log, MAX_PATH);
    strcpy(fail, log);
    strcpy(strrchr(log, '\\') + 1, "fmtcal.txt");
    strcpy(strrchr(fail, '\\') + 1, "fmtcal-fail.txt");
    DeleteFileA(log);
    DeleteFileA(fail);
    IDirectDraw7* dd = NewDD7();
    if (!dd) return;
    DDCAPS drv = {}, hel = {};
    drv.dwSize = hel.dwSize = sizeof(DDCAPS);
    dd->GetCaps(&drv, &hel);
    P("  (a process of its own) CANCALIBRATEGAMMA: driver %s, emulation %s\n", drv.dwCaps2 & DDCAPS2_CANCALIBRATEGAMMA ? "yes" : "no",
      hel.dwCaps2 & DDCAPS2_CANCALIBRATEGAMMA ? "yes" : "no");
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "fmtprobe calibrator";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "fmtprobe", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, nullptr, nullptr, wc.hInstance,
                                nullptr);
    dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    dd->SetDisplayMode(640, 480, 16, 0, 0);
    Pump(300);
    DDSURFACEDESC2 pd = {};
    pd.dwSize = sizeof(pd);
    pd.dwFlags = DDSD_CAPS;
    pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* prim = nullptr;
    dd->CreateSurface(&pd, &prim, nullptr);
    IDirectDrawGammaControl* g = nullptr;
    if (prim) prim->QueryInterface(IID_IDirectDrawGammaControl, reinterpret_cast<void**>(&g));
    if (g) {
        DDGAMMARAMP ramp = {}, got = {};
        for (int i = 0; i < 256; i++) ramp.red[i] = ramp.green[i] = ramp.blue[i] = WORD(i * 0x80);  // (half of the identity)
        auto show = [&](const char* what, HRESULT hr) {
            got = {};
            const HRESULT gh = g->GetGammaRamp(0, &got);
            P("  %s: 0x%08lx; GetGammaRamp 0x%08lx: red %04x %04x %04x .. %04x, green %04x, blue %04x\n", what, hr, gh, got.red[0],
              got.red[1], got.red[2], got.red[255], got.green[255], got.blue[255]);
        };
        show("SetGammaRamp(0, half the identity)", g->SetGammaRamp(0, &ramp));
        show("SetGammaRamp(CALIBRATE, half the identity)", g->SetGammaRamp(1, &ramp));
        if (FILE* f = fopen(fail, "w")) fclose(f);
        show("SetGammaRamp(CALIBRATE, half the identity), the calibrator failing", g->SetGammaRamp(1, &ramp));
        DeleteFileA(fail);
        show("SetGammaRamp(CALIBRATE, NULL)", g->SetGammaRamp(1, nullptr));
        show("SetGammaRamp(0, NULL)", g->SetGammaRamp(0, nullptr));
        g->Release();
    } else {
        P("  no gamma control on the primary\n");
    }
    if (FILE* f = fopen(log, "r")) {
        char line[256];
        P("  what the calibrator was called with:\n");
        while (fgets(line, sizeof(line), f)) P("    %s", line);
        fclose(f);
    } else {
        P("  the calibrator wasn't called\n");
    }
    if (prim) prim->Release();
    dd->RestoreDisplayMode();
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    DestroyWindow(hwnd);
    dd->Release();
}

// ---------------------------------------------------------------- PrintScreen

// What the clipboard holds: its formats, and a DIB's header, its masks or colours, and the
// surface's four corners in it (a bottom-up DIB's last row is the surface's first).
static void ClipboardDib(HWND hwnd, int w, int h) {
    if (!OpenClipboard(hwnd)) {
        P("    the clipboard couldn't be opened\n");
        return;
    }
    P("    the clipboard's formats:");
    for (UINT f = EnumClipboardFormats(0); f; f = EnumClipboardFormats(f)) P(" %u", f);
    if (HANDLE ht = GetClipboardData(CF_TEXT)) {
        const char* t = static_cast<const char*>(GlobalLock(ht));
        P("; CF_TEXT \"%.16s\"", t ? t : "");
        GlobalUnlock(ht);
    }
    P("\n");
    HANDLE hd = GetClipboardData(CF_DIB);
    const auto* bi = hd ? static_cast<const BITMAPINFOHEADER*>(GlobalLock(hd)) : nullptr;
    if (bi) {
        P("    CF_DIB, %lu bytes: biSize %lu, %ldx%ld, %u plane, %u bits, compression %lu, image size %lu, %ldx%ld a metre, "
          "%lu colours used, %lu important\n",
          static_cast<unsigned long>(GlobalSize(hd)), bi->biSize, bi->biWidth, bi->biHeight, bi->biPlanes, bi->biBitCount,
          bi->biCompression, bi->biSizeImage, bi->biXPelsPerMeter, bi->biYPelsPerMeter, bi->biClrUsed, bi->biClrImportant);
        const auto* extra = reinterpret_cast<const DWORD*>(reinterpret_cast<const BYTE*>(bi) + bi->biSize);
        const DWORD colours = bi->biCompression == BI_BITFIELDS ? 3 : bi->biClrUsed ? bi->biClrUsed
                                                                 : bi->biBitCount <= 8 ? 1u << bi->biBitCount
                                                                                       : 0;
        if (bi->biCompression == BI_BITFIELDS) {
            P("      masks %08lx %08lx %08lx\n", extra[0], extra[1], extra[2]);
        } else if (colours) {
            P("      colours [0] %08lx [1] %08lx [2] %08lx [255] %08lx; all of them:", extra[0], extra[1], extra[2], extra[colours - 1]);
            for (DWORD i = 0; i < colours; i++) P("%s%06lx", i % 16 ? " " : "\n       ", extra[i] & 0xFFFFFF);
            P("\n");
        }
        const BYTE* bits = reinterpret_cast<const BYTE*>(extra + colours);
        const LONG rows = bi->biHeight < 0 ? -bi->biHeight : bi->biHeight;
        const size_t stride = (size_t(bi->biWidth) * bi->biBitCount + 31) / 32 * 4;
        const size_t need = bi->biSize + colours * 4 + stride * size_t(rows);
        if (bi->biWidth >= w && rows >= h && bi->biBitCount >= 8 && GlobalSize(hd) >= need) {
            auto px = [&](int x, int y) {
                const BYTE* row = bits + stride * size_t(bi->biHeight < 0 ? y : rows - 1 - y);
                DWORD v = 0;
                memcpy(&v, row + size_t(x) * (bi->biBitCount / 8), bi->biBitCount / 8);
                return v;
            };
            P("      the corners: top left %lx, top right %lx, bottom left %lx, bottom right %lx; (1, 1) %lx\n", px(0, 0), px(w - 1, 0),
              px(0, h - 1), px(w - 1, h - 1), px(1, 1));
        }
        GlobalUnlock(hd);
    }
    CloseClipboard();
}

// The clipboard emptied and given a text of its own, to tell whether ddraw.dll replaced it.
static void ClipboardSentinel(HWND hwnd) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, 9)) {
        if (char* t = static_cast<char*>(GlobalLock(g))) {
            memcpy(t, "fmtprobe", 9);
            GlobalUnlock(g);
        }
        if (!SetClipboardData(CF_TEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

// The primary filled with `other` but for its four corners (tl, tr, bl, br), through a lock.
static void FillCorners(IDirectDrawSurface7* s, DWORD bytes, DWORD other, DWORD tl, DWORD tr, DWORD bl, DWORD br) {
    DDSURFACEDESC2 l = {};
    l.dwSize = sizeof(l);
    if (FAILED(s->Lock(nullptr, &l, DDLOCK_WAIT, nullptr))) return;
    for (DWORD y = 0; y < l.dwHeight; y++) {
        BYTE* row = static_cast<BYTE*>(l.lpSurface) + y * l.lPitch;
        for (DWORD x = 0; x < l.dwWidth; x++) {
            const bool top = y == 0, bottom = y == l.dwHeight - 1, left = x == 0, right = x == l.dwWidth - 1;
            const DWORD v = top && left ? tl : top && right ? tr : bottom && left ? bl : bottom && right ? br : other;
            memcpy(row + x * bytes, &v, bytes);
        }
    }
    s->Unlock(nullptr);
}

// PrintScreen with EnablePrintScreen under HKLM\Software\Microsoft\DirectDraw, which ddraw.dll
// reads as a DirectDraw object is made: its window procedure copies the primary to the
// clipboard at the key-up (WM_KEYUP VK_SNAPSHOT, sent here; the system's own PrintScreen at the
// end). Without the value, then with it.
static void PrintScreenCopy() {
    P("\nPrintScreen (EnablePrintScreen under HKLM\\Software\\Microsoft\\DirectDraw; WM_KEYUP VK_SNAPSHOT sent):\n");
    HKEY key = nullptr;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\DirectDraw", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        P("  the key couldn't be opened (not an administrator?)\n");
        return;
    }
    BYTE old[64];
    DWORD oldType = 0, oldSize = sizeof(old);
    const bool had = RegQueryValueExA(key, "EnablePrintScreen", nullptr, &oldType, old, &oldSize) == ERROR_SUCCESS;
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "fmtprobe printscreen";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "fmtprobe", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, nullptr, nullptr, wc.hInstance,
                                nullptr);
    Pump(200);
    auto snapshot = [&](const char* what) {
        ClipboardSentinel(hwnd);
        SendMessageA(hwnd, WM_KEYUP, VK_SNAPSHOT, 0xC1370001);
        Pump(200);
        P("  %s:\n", what);
        ClipboardDib(hwnd, 640, 480);
    };
    for (int on = 0; on < 2; on++) {
        if (on) {
            const DWORD one = 1;
            RegSetValueExA(key, "EnablePrintScreen", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
        } else {
            RegDeleteValueA(key, "EnablePrintScreen");
        }
        P(" %s:\n", on ? "EnablePrintScreen 1 (a DWORD)" : "no EnablePrintScreen");
        IDirectDraw7* dd = NewDD7();  // (the value read now)
        if (!dd) continue;
        HRESULT hr = dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        P("  SetCooperativeLevel(EXCLUSIVE | FULLSCREEN): 0x%08lx\n", hr);
        // 16 bits; 8 without a palette, with one, without one again; 32.
        static const DWORD kDepths[] = {16, 8, 8, 8, 32};
        for (int m = 0; m < 5 && (on || m == 0); m++) {
            const DWORD bpp = kDepths[m];
            hr = dd->SetDisplayMode(640, 480, bpp, 0, 0);
            Pump(300);
            DDSURFACEDESC2 pd = {};
            pd.dwSize = sizeof(pd);
            pd.dwFlags = DDSD_CAPS;
            pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
            IDirectDrawSurface7* prim = nullptr;
            const HRESULT ch = dd->CreateSurface(&pd, &prim, nullptr);
            char what[160];
            DDPIXELFORMAT pf = {};
            pf.dwSize = sizeof(pf);
            if (prim) prim->GetPixelFormat(&pf);
            IDirectDrawPalette* pal = nullptr;
            if (prim && bpp == 8 && m == 2) {
                PALETTEENTRY e[256];
                for (int i = 0; i < 256; i++) e[i] = {BYTE(i), BYTE(255 - i), BYTE(i / 2), 0};
                dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, e, &pal, nullptr);
                if (pal) prim->SetPalette(pal);
            }
            snprintf(what, sizeof(what), "640x480x%lu (0x%08lx), the primary 0x%08lx (masks %lx %lx %lx)%s", bpp, hr, ch, pf.dwRBitMask,
                     pf.dwGBitMask, pf.dwBBitMask,
                     bpp != 8 ? "" : pal ? ", a palette of red i, green 255-i, blue i/2 on it" : ", no palette");
            if (prim) {
                if (bpp == 8) FillCorners(prim, 1, 0, 1, 2, 3, 255);
                else if (bpp == 16) FillCorners(prim, 2, pf.dwRBitMask == 0x7C00 ? 0x0421 : 0x0841, pf.dwRBitMask, pf.dwGBitMask,
                                                pf.dwBBitMask, 0xFFFF);
                else FillCorners(prim, bpp / 8, 0x102030, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF);
            }
            snapshot(what);
            if (bpp == 8 && !pal) {
                // What ddraw.dll's copy takes the colours from then: GDI's palette of the screen.
                PALETTEENTRY sys[256] = {};
                HDC sdc = GetDC(nullptr);
                const UINT got = GetSystemPaletteEntries(sdc, 0, 256, sys);
                ReleaseDC(nullptr, sdc);
                P("    GetSystemPaletteEntries: %u;", got);
                for (int i = 0; i < 256; i++) P("%s%02x%02x%02x", i % 16 ? " " : "\n       ", sys[i].peRed, sys[i].peGreen, sys[i].peBlue);
                P("\n");
            }
            if (pal) pal->Release();
            if (prim) prim->Release();
        }
        if (on) {
            // A flip chain: what the front shows after a flip.
            hr = dd->SetDisplayMode(640, 480, 16, 0, 0);
            Pump(300);
            DDSURFACEDESC2 fd = {};
            fd.dwSize = sizeof(fd);
            fd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
            fd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
            fd.dwBackBufferCount = 1;
            IDirectDrawSurface7* front = nullptr;
            const HRESULT ch = dd->CreateSurface(&fd, &front, nullptr);
            IDirectDrawSurface7* back = nullptr;
            DDSCAPS2 bc = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
            if (front) front->GetAttachedSurface(&bc, &back);
            if (front && back) {
                FillCorners(front, 2, 0x1111, 0x1111, 0x1111, 0x1111, 0x1111);
                FillCorners(back, 2, 0x2222, 0x2222, 0x2222, 0x2222, 0x2222);
                const HRESULT fl = front->Flip(nullptr, DDFLIP_WAIT);
                char what[128];
                snprintf(what, sizeof(what), "a flip chain (0x%08lx), the front filled 1111 and the back 2222, then a flip (0x%08lx)",
                         ch, fl);
                snapshot(what);
            }
            if (back) back->Release();
            if (front) front->Release();
            // The system's own PrintScreen (the key pressed and let go), with the primary.
            DDSURFACEDESC2 pd = {};
            pd.dwSize = sizeof(pd);
            pd.dwFlags = DDSD_CAPS;
            pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
            IDirectDrawSurface7* prim = nullptr;
            dd->CreateSurface(&pd, &prim, nullptr);
            if (prim) {
                FillCorners(prim, 2, 0x0841, 0xF800, 0x07E0, 0x001F, 0xFFFF);
                ClipboardSentinel(hwnd);
                SetForegroundWindow(hwnd);
                keybd_event(VK_SNAPSHOT, 0x37, 0, 0);
                keybd_event(VK_SNAPSHOT, 0x37, KEYEVENTF_KEYUP, 0);
                Pump(600);
                P("  the PrintScreen key pressed and let go (keybd_event), 640x480x16:\n");
                ClipboardDib(hwnd, 640, 480);
                prim->Release();
            }
        }
        // In normal mode (the window not hooked).
        dd->RestoreDisplayMode();
        dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
        Pump(300);
        DDSURFACEDESC2 pd = {};
        pd.dwSize = sizeof(pd);
        pd.dwFlags = DDSD_CAPS;
        pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        IDirectDrawSurface7* prim = nullptr;
        dd->CreateSurface(&pd, &prim, nullptr);
        snapshot("normal mode, a primary made");
        if (prim) prim->Release();
        dd->Release();
    }
    if (had) RegSetValueExA(key, "EnablePrintScreen", 0, oldType, old, oldSize);
    else RegDeleteValueA(key, "EnablePrintScreen");
    RegCloseKey(key);
    DestroyWindow(hwnd);
    ChangeDisplaySettingsA(nullptr, 0);
}

// ---------------------------------------------------------------- GDI's palette of the screen

// GDI's palette of an 8-bit screen in exclusive mode (GetSystemPaletteEntries, GetSystemPaletteUse,
// the screen's caps): with no palette on the primary, with one (made with and without
// DDPCAPS_ALLOW256), after SetEntries, with one on the back buffer or on a plain surface alone,
// after the palette and the primary are released; parts of it asked for; and in normal mode.
static void ScreenPalette() {
    P("\nGDI's palette of the screen (GetSystemPaletteEntries) with DirectDraw's palettes, 640x480x8 exclusive:\n");
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "fmtprobe screen palette";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "fmtprobe", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, nullptr, nullptr,
                                wc.hInstance, nullptr);
    Pump(200);
    auto state = [](const char* what) {
        PALETTEENTRY sys[256];
        memset(sys, 0xEE, sizeof(sys));
        HDC sdc = GetDC(nullptr);
        const UINT got = GetSystemPaletteEntries(sdc, 0, 256, sys);
        P("  %s:\n    GetSystemPaletteEntries %u, GetSystemPaletteUse %u; caps: %d bits, raster 0x%x, palette of %d, %d reserved, %d colours, "
          "resolution %d; entries (with their flags where not 0):",
          what, got, GetSystemPaletteUse(sdc), GetDeviceCaps(sdc, BITSPIXEL), GetDeviceCaps(sdc, RASTERCAPS),
          GetDeviceCaps(sdc, SIZEPALETTE), GetDeviceCaps(sdc, NUMRESERVED), GetDeviceCaps(sdc, NUMCOLORS), GetDeviceCaps(sdc, COLORRES));
        ReleaseDC(nullptr, sdc);
        for (int i = 0; i < 256; i++) {
            P("%s%02x%02x%02x", i % 16 ? " " : "\n       ", sys[i].peRed, sys[i].peGreen, sys[i].peBlue);
            if (sys[i].peFlags) P("/%x", sys[i].peFlags);
        }
        P("\n");
    };
    state("before DirectDraw (the desktop)");
    IDirectDraw7* dd = NewDD7();
    if (!dd) return;
    HRESULT hr = dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    const HRESULT mode = dd->SetDisplayMode(640, 480, 8, 0, 0);
    Pump(300);
    P("  SetCooperativeLevel 0x%08lx, SetDisplayMode(640, 480, 8) 0x%08lx\n", hr, mode);
    state("the mode set, no primary");
    DDSURFACEDESC2 fd = {};
    fd.dwSize = sizeof(fd);
    fd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    fd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    fd.dwBackBufferCount = 1;
    IDirectDrawSurface7 *front = nullptr, *back = nullptr, *plain = nullptr;
    hr = dd->CreateSurface(&fd, &front, nullptr);
    DDSCAPS2 bc = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
    if (front) front->GetAttachedSurface(&bc, &back);
    DDSURFACEDESC2 od = {};
    od.dwSize = sizeof(od);
    od.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    od.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
    od.dwWidth = od.dwHeight = 16;
    dd->CreateSurface(&od, &plain, nullptr);
    P("  a flip chain 0x%08lx\n", hr);
    if (front && back && plain) {
        state("a primary without a palette");
        // Palettes: A red i, green 255 - i, blue i / 2; B the same turned round (red 255 - i, green i / 2, blue i).
        PALETTEENTRY a[256], b[256];
        for (int i = 0; i < 256; i++) {
            a[i] = {BYTE(i), BYTE(255 - i), BYTE(i / 2), 0};
            b[i] = {BYTE(255 - i), BYTE(i / 2), BYTE(i), 0};
        }
        IDirectDrawPalette *pa = nullptr, *pb = nullptr, *pc = nullptr;
        dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, a, &pa, nullptr);
        dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, b, &pb, nullptr);
        dd->CreatePalette(DDPCAPS_8BIT, a, &pc, nullptr);
        if (pa && pb && pc) {
            hr = plain->SetPalette(pa);
            P("  palette A (red i, green 255-i, blue i/2; ALLOW256) on a plain surface 0x%08lx\n", hr);
            state("A on a plain surface alone");
            plain->SetPalette(nullptr);
            hr = back->SetPalette(pa);
            P("  A on the back buffer 0x%08lx\n", hr);
            state("A on the back buffer alone");
            back->SetPalette(nullptr);
            hr = front->SetPalette(pa);
            Pump(100);
            P("  A on the primary 0x%08lx\n", hr);
            state("A on the primary");
            PALETTEENTRY four[4] = {{1, 2, 3, 0}, {4, 5, 6, 0}, {7, 8, 9, 0}, {10, 11, 12, 0}};
            hr = pa->SetEntries(0, 0, 4, four);
            const HRESULT hr2 = pa->SetEntries(0, 100, 4, four);
            const HRESULT hr3 = pa->SetEntries(0, 252, 4, four);
            Pump(100);
            P("  SetEntries of 010203 040506 070809 0a0b0c at 0, 100 and 252: 0x%08lx 0x%08lx 0x%08lx\n", hr, hr2, hr3);
            state("A after SetEntries");
            // Parts of it.
            HDC sdc = GetDC(nullptr);
            PALETTEENTRY part[32];
            memset(part, 0xEE, sizeof(part));
            const UINT n1 = GetSystemPaletteEntries(sdc, 100, 4, part);
            P("  GetSystemPaletteEntries(100, 4): %u: %02x%02x%02x %02x%02x%02x %02x%02x%02x %02x%02x%02x, the next %02x%02x%02x\n", n1,
              part[0].peRed, part[0].peGreen, part[0].peBlue, part[1].peRed, part[1].peGreen, part[1].peBlue, part[2].peRed,
              part[2].peGreen, part[2].peBlue, part[3].peRed, part[3].peGreen, part[3].peBlue, part[4].peRed, part[4].peGreen,
              part[4].peBlue);
            memset(part, 0xEE, sizeof(part));
            const UINT n2 = GetSystemPaletteEntries(sdc, 250, 20, part);
            const UINT n3 = GetSystemPaletteEntries(sdc, 0, 256, nullptr), n4 = GetSystemPaletteEntries(sdc, 0, 0, part);
            const UINT n5 = GetSystemPaletteEntries(sdc, 256, 1, part), n6 = GetSystemPaletteEntries(sdc, 300, 4, part);
            P("  (250, 20): %u, entry 5 of them %02x%02x%02x, entry 6 %02x%02x%02x; (0, 256, NULL): %u; (0, 0): %u; (256, 1): %u; (300, 4): %u\n",
              n2, part[5].peRed, part[5].peGreen, part[5].peBlue, part[6].peRed, part[6].peGreen, part[6].peBlue, n3, n4, n5, n6);
            HDC wdc = GetDC(hwnd);
            HDC mdc = CreateCompatibleDC(wdc);
            memset(part, 0xEE, sizeof(part));
            const UINT n7 = GetSystemPaletteEntries(wdc, 0, 4, part);
            const UINT n8 = GetSystemPaletteEntries(mdc, 0, 4, part + 4);
            P("  on the window's DC (0, 4): %u: %02x%02x%02x %02x%02x%02x; on a memory DC: %u: %02x%02x%02x %02x%02x%02x; NULL DC: %u\n", n7,
              part[0].peRed, part[0].peGreen, part[0].peBlue, part[1].peRed, part[1].peGreen, part[1].peBlue, n8, part[4].peRed,
              part[4].peGreen, part[4].peBlue, part[5].peRed, part[5].peGreen, part[5].peBlue,
              GetSystemPaletteEntries(nullptr, 0, 4, part + 8));
            DeleteDC(mdc);
            ReleaseDC(hwnd, wdc);
            ReleaseDC(nullptr, sdc);
            hr = front->SetPalette(pb);
            Pump(100);
            P("  palette B (red 255-i, green i/2, blue i; ALLOW256) on the primary instead 0x%08lx\n", hr);
            state("B on the primary");
            hr = front->Flip(nullptr, DDFLIP_WAIT);
            Pump(100);
            P("  Flip 0x%08lx\n", hr);
            state("B on the primary, flipped");
            hr = front->SetPalette(pc);
            Pump(100);
            P("  palette C (A's colours, without ALLOW256) on the primary instead 0x%08lx\n", hr);
            state("C on the primary");
            PALETTEENTRY back4[256];
            memset(back4, 0xEE, sizeof(back4));
            hr = pc->GetEntries(0, 0, 256, back4);
            P("  C's GetEntries 0x%08lx: [0] %02x%02x%02x/%x [1] %02x%02x%02x/%x [254] %02x%02x%02x/%x [255] %02x%02x%02x/%x\n", hr,
              back4[0].peRed, back4[0].peGreen, back4[0].peBlue, back4[0].peFlags, back4[1].peRed, back4[1].peGreen, back4[1].peBlue,
              back4[1].peFlags, back4[254].peRed, back4[254].peGreen, back4[254].peBlue, back4[254].peFlags, back4[255].peRed,
              back4[255].peGreen, back4[255].peBlue, back4[255].peFlags);
            hr = front->SetPalette(nullptr);
            Pump(100);
            P("  SetPalette(NULL) on the primary 0x%08lx\n", hr);
            state("the palette taken off the primary");
            front->SetPalette(pb);
            Pump(100);
        }
        if (pa) pa->Release();
        if (pc) pc->Release();
        if (pb) {
            const ULONG left = pb->Release();
            Pump(100);
            P("  B on the primary again, then released (%lu left)\n", left);
            state("B released, still on the primary");
        }
    }
    if (plain) plain->Release();
    if (back) back->Release();
    if (front) front->Release();
    Pump(100);
    state("the primary released");
    dd->RestoreDisplayMode();
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    Pump(300);
    state("normal mode again (the desktop)");
    dd->Release();
    DestroyWindow(hwnd);
    ChangeDisplaySettingsA(nullptr, 0);
}

// A palette's entries as GetEntries gives them back, their peFlags above all: made with and
// without DDPCAPS_ALLOW256 from entries whose flags are 0 or set, before it is on a surface, on
// a plain surface, on the back buffer, on the primary, after SetEntries there, and taken off
// again; in an 8-bit exclusive mode, and on an 8-bit plain surface in normal mode.
static void PaletteFlags() {
    P("\nA palette's entries read back (GetEntries: colour/peFlags), 640x480x8 exclusive:\n");
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "fmtprobe palette flags";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "fmtprobe", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, nullptr, nullptr,
                                wc.hInstance, nullptr);
    Pump(200);
    IDirectDraw7* dd = NewDD7();
    if (!dd) return;
    auto show = [](const char* what, IDirectDrawPalette* pal) {
        PALETTEENTRY e[256];
        memset(e, 0xEE, sizeof(e));
        DWORD caps = 0;
        const HRESULT hc = pal->GetCaps(&caps), hr = pal->GetEntries(0, 0, 256, e);
        int count[256] = {};
        for (int i = 0; i < 256; i++) count[e[i].peFlags]++;
        P("    %s: caps 0x%lx (0x%08lx), GetEntries 0x%08lx:", what, caps, hc, hr);
        static const int kShown[] = {0, 1, 2, 9, 10, 100, 101, 245, 246, 253, 254, 255};
        for (int i : kShown) P(" [%d] %02x%02x%02x/%x", i, e[i].peRed, e[i].peGreen, e[i].peBlue, e[i].peFlags);
        P("; flags:");
        for (int f = 0; f < 256; f++)
            if (count[f]) P(" %d of %x", count[f], f);
        // (where the flags change along the palette)
        P("; runs:");
        for (int i = 0, from = 0; i <= 256; i++)
            if (i == 256 || e[i].peFlags != e[from].peFlags) {
                P(" %d-%d %x", from, i - 1, e[from].peFlags);
                from = i;
            }
        P("\n");
    };
    for (int pass = 0; pass < 2; pass++) {
        IDirectDrawSurface7 *front = nullptr, *back = nullptr, *plain = nullptr;
        HRESULT hr;
        if (pass == 0) {
            hr = dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
            const HRESULT mode = dd->SetDisplayMode(640, 480, 8, 0, 0);
            Pump(300);
            DDSURFACEDESC2 fd = {};
            fd.dwSize = sizeof(fd);
            fd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
            fd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
            fd.dwBackBufferCount = 1;
            const HRESULT ch = dd->CreateSurface(&fd, &front, nullptr);
            DDSCAPS2 bc = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
            if (front) front->GetAttachedSurface(&bc, &back);
            P("  SetCooperativeLevel 0x%08lx, SetDisplayMode(640, 480, 8) 0x%08lx, a flip chain 0x%08lx\n", hr, mode, ch);
        } else {
            dd->RestoreDisplayMode();
            hr = dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
            Pump(300);
            P("\n  normal mode (0x%08lx), the palettes on an 8-bit plain surface in system memory:\n", hr);
        }
        DDSURFACEDESC2 od = {};
        od.dwSize = sizeof(od);
        od.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        od.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        od.dwWidth = od.dwHeight = 16;
        od.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        od.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        od.ddpfPixelFormat.dwRGBBitCount = 8;
        hr = dd->CreateSurface(&od, &plain, nullptr);
        if (!plain || (pass == 0 && (!front || !back))) {
            P("  surfaces not made (0x%08lx)\n", hr);
        } else {
            struct Kind {
                const char* name;
                DWORD caps;
                BYTE flags;  // the entries' peFlags as given
            };
            static const Kind kKinds[] = {{"8BIT, flags 0", DDPCAPS_8BIT, 0},
                                          {"8BIT | ALLOW256, flags 0", DDPCAPS_8BIT | DDPCAPS_ALLOW256, 0},
                                          {"8BIT, flags 0xff", DDPCAPS_8BIT, 0xFF},
                                          {"8BIT | ALLOW256, flags 0xff", DDPCAPS_8BIT | DDPCAPS_ALLOW256, 0xFF},
                                          {"8BIT, flags 0x02 (PC_EXPLICIT)", DDPCAPS_8BIT, 0x02},
                                          {"8BIT | INITIALIZE, flags 0", DDPCAPS_8BIT | DDPCAPS_INITIALIZE, 0}};
            for (const Kind& k : kKinds) {
                PALETTEENTRY e[256];
                for (int i = 0; i < 256; i++) e[i] = {BYTE(i), BYTE(255 - i), BYTE(i / 2), k.flags};
                IDirectDrawPalette* pal = nullptr;
                hr = dd->CreatePalette(k.caps, e, &pal, nullptr);
                P("  a palette of %s (red i, green 255-i, blue i/2): 0x%08lx\n", k.name, hr);
                if (!pal) continue;
                show("just made", pal);
                hr = plain->SetPalette(pal);
                P("    on the plain surface 0x%08lx\n", hr);
                show("on a plain surface", pal);
                PALETTEENTRY four[4] = {{1, 2, 3, 0}, {4, 5, 6, 0x01}, {7, 8, 9, 0x04}, {10, 11, 12, 0xFF}};
                hr = pal->SetEntries(0, 100, 4, four);
                P("    SetEntries at 100 of 010203/0 040506/1 070809/4 0a0b0c/ff: 0x%08lx\n", hr);
                show("after SetEntries on the plain surface", pal);
                plain->SetPalette(nullptr);
                show("off the plain surface", pal);
                if (pass == 0) {
                    hr = back->SetPalette(pal);
                    P("    on the back buffer 0x%08lx\n", hr);
                    show("on the back buffer", pal);
                    back->SetPalette(nullptr);
                    hr = front->SetPalette(pal);
                    Pump(100);
                    P("    on the primary 0x%08lx\n", hr);
                    show("on the primary", pal);
                    const HRESULT h0 = pal->SetEntries(0, 0, 4, four), h1 = pal->SetEntries(0, 104, 4, four);
                    const HRESULT h2 = pal->SetEntries(0, 252, 4, four);
                    P("    SetEntries of the same four at 0, 104 and 252: 0x%08lx 0x%08lx 0x%08lx\n", h0, h1, h2);
                    show("after SetEntries on the primary", pal);
                    PALETTEENTRY one[1];
                    memset(one, 0xEE, sizeof(one));
                    hr = pal->GetEntries(0, 105, 1, one);
                    P("    GetEntries(105, 1) 0x%08lx: %02x%02x%02x/%x\n", hr, one[0].peRed, one[0].peGreen, one[0].peBlue, one[0].peFlags);
                    front->Flip(nullptr, DDFLIP_WAIT);
                    show("after a flip", pal);
                    hr = front->SetPalette(nullptr);
                    Pump(100);
                    P("    SetPalette(NULL) on the primary 0x%08lx\n", hr);
                    show("off the primary", pal);
                    hr = pal->SetEntries(0, 108, 4, four);
                    P("    SetEntries at 108, on no surface: 0x%08lx\n", hr);
                    show("after SetEntries on no surface", pal);
                }
                pal->Release();
            }
        }
        if (plain) plain->Release();
        if (back) back->Release();
        if (front) front->Release();
    }
    dd->Release();
    DestroyWindow(hwnd);
    ChangeDisplaySettingsA(nullptr, 0);
}

static void WithObjects(void (*run)(IDirectDraw7*, IDirectDraw*)) {
    IDirectDraw7* dd7 = NewDD7();
    IDirectDraw* dd1 = nullptr;
    DirectDrawCreate(nullptr, &dd1, nullptr);
    if (dd7 && dd1) {
        dd7->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
        dd1->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
        run(dd7, dd1);
    } else {
        P("\n(No DirectDraw objects for this section.)\n");
    }
    if (dd1) dd1->Release();
    if (dd7) dd7->Release();
}

struct Section {
    const char* name;
    void (*run)();
    DWORD seconds;  // how long it may take
};
static const Section kSections[] = {
    {"registry and files", RegistryAndFiles, 60},
    {"enumeration", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw* dd1) { Enumeration(dd7, dd1); }); }, 120},
    {"caps", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { Caps(dd7); }); }, 120},
    {"reference counts", References, 60},
    {"EnumSurfaces", EnumAfterQuery, 60},
    {"blits", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { Blits(dd7); }); }, 120},
    {"texture calls", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { TextureCalls(dd7); }); }, 60},
    {"palettes", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { Palettes(dd7); }); }, 120},
    {"viewport clears", LegacyClears, 300},
    {"default states", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { States(dd7); }); }, 120},
    {"exclusive mode", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { Exclusive(dd7); }); }, 300},
    {"emulation only", EmulationOnly, 60},
    {"more blits", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { MoreBlits(dd7); }); }, 120},
    {"the expected display mode", ExpectedMode, 600},
    {"more surfaces", [] { WithObjects(MoreSurfaces); }, 120},
    {"a Z buffer with stencil, where it is made", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { StencilZCreation(dd7); }); }, 60},
    {"a 15+1 Z buffer, RGB device",
     [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { StencilZDevice(dd7, "RGB", IID_IDirect3DRGBDevice, false); }); }, 120},
    {"a 15+1 Z buffer, MMX device",
     [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { StencilZDevice(dd7, "MMX", IID_IDirect3DMMXDevice, false); }); }, 120},
    {"a 15+1 Z buffer, Reference device",
     [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { StencilZDevice(dd7, "Reference", IID_IDirect3DRefDevice, false); }); }, 120},
    {"a 15+1 Z buffer, HAL device",
     [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { StencilZDevice(dd7, "HAL", IID_IDirect3DHALDevice, true); }); }, 120},
    {"a 15+1 Z buffer, T&L HAL device",
     [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { StencilZDevice(dd7, "T&L HAL", IID_IDirect3DTnLHalDevice, true); }); },
     120},
    {"large surfaces", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { LargeSurfaces(dd7); }); }, 900},
    {"Z buffer words, RGB device",
     [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { DepthWords(dd7, "RGB", IID_IDirect3DRGBDevice, false); }); }, 120},
    {"Z buffer words, MMX device",
     [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { DepthWords(dd7, "MMX", IID_IDirect3DMMXDevice, false); }); }, 120},
    {"Z buffer words, HAL device",
     [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { DepthWords(dd7, "HAL", IID_IDirect3DHALDevice, true); }); }, 120},
    {"bad arguments", BadArguments, 4 * 60 * 60},
    {"colour-key ranges", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { KeyRanges(dd7); }); }, 120},
    {"overlays: keys, bob, interleaving, field flips, alpha, effects; colour controls",
     [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { OverlayMore(dd7); }); }, 120},
    {"video ports", [] { WithObjects(VideoPorts); }, 120},
    {"Z buffers whose depth and stencil masks share bits",
     [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { SharedZMasks(dd7); }); }, 60},
    {"the device window and the mouse", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { DeviceWindowMouse(dd7); }); }, 120},
    {"DDSCAPS_OWNDC", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { OwnDc(dd7); }); }, 60},
    {"activation and focus loss", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { Activation(dd7); }); }, 180},
    {"YUV fills and planar surfaces", [] { WithObjects([](IDirectDraw7* dd7, IDirectDraw*) { YuvFills(dd7); }); }, 60},
    {"the gamma calibrator", Calibrator, 180},
    {"the gamma calibrator, registered", CalibratorRegistered, 120},
    {"PrintScreen", PrintScreenCopy, 180},
    {"GDI's palette of the screen", ScreenPalette, 120},
    {"palette entries read back", PaletteFlags, 120},
};

static int SectionIndex(void (*run)()) {
    for (int i = 0; i < int(sizeof(kSections) / sizeof(kSections[0])); i++)
        if (kSections[i].run == run) return i;
    return -1;
}

int FmtprobeSection(const char* number, const char* path, CreateExFn createEx) {
    g_createEx = createEx;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    const int n = atoi(number);
    if (n < 0 || n >= int(sizeof(kSections) / sizeof(kSections[0]))) return 2;
    g_path = path;
    g_out = fopen(path, "a");
    if (!g_out) return 3;
    setvbuf(g_out, nullptr, _IONBF, 0);
    kSections[n].run();
    if (g_out) fclose(g_out);  // (closed by a section that ran another, and not opened again)
    return 0;
}

void FmtprobeMore(const char* path) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    for (int i = 0; i < int(sizeof(kSections) / sizeof(kSections[0])); i++) {
        char cmd[MAX_PATH + 32], note[160] = "";
        snprintf(cmd, sizeof(cmd), "\"%s\" section %d", exe, i);
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            snprintf(note, sizeof(note), "could not be started (%lu)", GetLastError());
        } else {
            DWORD code = 0;
            if (WaitForSingleObject(pi.hProcess, kSections[i].seconds * 1000) != WAIT_OBJECT_0) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 5000);
                snprintf(note, sizeof(note), "did not finish in %lu seconds and was stopped", kSections[i].seconds);
            } else if (GetExitCodeProcess(pi.hProcess, &code) && code) {
                snprintf(note, sizeof(note), "ended early (exit code 0x%08lx)", code);
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        if (!note[0]) continue;
        // (A section stopped in a display mode of its own: the desktop's back.)
        ChangeDisplaySettingsA(nullptr, 0);
        if (FILE* f = fopen(path, "a")) {
            fprintf(f, "\n(The section \"%s\" %s.)\n", kSections[i].name, note);
            fclose(f);
        }
    }
    if (FILE* f = fopen(path, "a")) {
        fprintf(f, "\nEnd of the probe.\n");
        fclose(f);
    }
}
