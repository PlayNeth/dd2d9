// Performance benchmark.
//   ddbench blit            software blitter throughput on offscreen surfaces (8/16/32-bit)
//   ddbench flip <w> <h> <bpp> [seconds]
//                           exclusive-mode game loop: background copy, color-keyed sprites,
//                           Flip(DDFLIP_NOVSYNC); reports frame times and the slowest Blt
//   ddbench window [seconds]
//                           windowed primary: small Blts to the primary (a cursor / HUD
//                           pattern), to see how much the presenter converts
//   ddbench zlock [w h]     Direct3D 7 frames (clear, a triangle) on an offscreen target
//                           with a 16-bit Z buffer, each frame waited for (a 1x1 read of
//                           the target): the frame time without locking the Z buffer, with
//                           an 8x8 lock of it (a lens flare's occlusion test) and with a
//                           whole-surface lock
// Results are appended to ddbench.txt next to the executable.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static FILE* g_out;

static double Now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void Pump() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
}

static void SetFormat(DDPIXELFORMAT* pf, int bpp) {
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(*pf);
    pf->dwFlags = bpp == 8 ? (DDPF_RGB | DDPF_PALETTEINDEXED8) : DDPF_RGB;
    pf->dwRGBBitCount = DWORD(bpp);
    if (bpp == 16) {
        pf->dwRBitMask = 0xF800;
        pf->dwGBitMask = 0x07E0;
        pf->dwBBitMask = 0x001F;
    } else if (bpp == 32) {
        pf->dwRBitMask = 0xFF0000;
        pf->dwGBitMask = 0x00FF00;
        pf->dwBBitMask = 0x0000FF;
    }
}

static IDirectDrawSurface7* Offscreen(IDirectDraw7* dd, int w, int h, int bpp) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | (bpp ? DDSD_PIXELFORMAT : 0);
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    d.dwWidth = DWORD(w);
    d.dwHeight = DWORD(h);
    if (bpp) SetFormat(&d.ddpfPixelFormat, bpp);
    IDirectDrawSurface7* s = nullptr;
    if (FAILED(dd->CreateSurface(&d, &s, nullptr))) return nullptr;
    return s;
}

// A FourCC offscreen surface (system memory), its memory zeroed by DirectDraw.
static IDirectDrawSurface7* FourCC(IDirectDraw7* dd, int w, int h, DWORD fourCC) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    d.dwWidth = DWORD(w);
    d.dwHeight = DWORD(h);
    d.ddpfPixelFormat.dwSize = sizeof(d.ddpfPixelFormat);
    d.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
    d.ddpfPixelFormat.dwFourCC = fourCC;
    IDirectDrawSurface7* s = nullptr;
    if (FAILED(dd->CreateSurface(&d, &s, nullptr))) return nullptr;
    return s;
}

// Fills a surface with a pattern; in sprites, the key color (0) covers about half the pixels.
static void Paint(IDirectDrawSurface7* s, bool sprite) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    if (FAILED(s->Lock(nullptr, &d, DDLOCK_WAIT, nullptr))) return;
    const int bytes = int(d.ddpfPixelFormat.dwRGBBitCount / 8);
    for (DWORD y = 0; y < d.dwHeight; y++) {
        auto* row = static_cast<uint8_t*>(d.lpSurface) + y * d.lPitch;
        for (DWORD x = 0; x < d.dwWidth; x++) {
            uint32_t v = (x * 7 + y * 13) | 1;  // never the key
            if (sprite) {
                const int cx = int(x % 32) - 16, cy = int(y % 32) - 16;
                if (cx * cx + cy * cy > 140) v = 0;  // a disc in each 32x32 cell
            }
            memcpy(row + x * bytes, &v, size_t(bytes));
        }
    }
    s->Unlock(nullptr);
}

static void SetKey0(IDirectDrawSurface7* s) {
    DDCOLORKEY k = {0, 0};
    s->SetColorKey(DDCKEY_SRCBLT, &k);
}

// Runs `op` repeatedly for ~0.4 s, five times; prints the best rate in megapixels/s.
template <class F>
static void Measure(const char* name, int bpp, double pixelsPerOp, F op) {
    double best = 0;
    for (int run = 0; run < 5; run++) {
        int ops = 0;
        const double t0 = Now();
        double t;
        do {
            for (int i = 0; i < 16; i++) op(ops + i);
            ops += 16;
            t = Now() - t0;
        } while (t < 0.4);
        best = std::max(best, ops * pixelsPerOp / t / 1e6);
    }
    fprintf(g_out, "%2d-bit %-34s %9.1f Mpix/s\n", bpp, name, best);
    fflush(g_out);
}

static int RunBlit() {
    IDirectDraw7* dd = nullptr;
    if (FAILED(DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr))) return 1;
    dd->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
    const int W = 640, H = 480;
    for (int bpp : {8, 16, 32}) {
        IDirectDrawSurface7* dst = Offscreen(dd, W, H, bpp);
        IDirectDrawSurface7* bg = Offscreen(dd, W, H, bpp);
        IDirectDrawSurface7* sheet = Offscreen(dd, 256, 256, bpp);
        if (!dst || !bg || !sheet) {
            fprintf(g_out, "%d-bit: CreateSurface failed\n", bpp);
            return 1;
        }
        Paint(bg, false);
        Paint(sheet, true);
        SetKey0(sheet);
        auto spritePos = [&](int i, int size, RECT* src, LONG* x, LONG* y) {
            const int cell = i % 64;
            *src = {LONG(cell % 8 * 32), LONG(cell / 8 * 32), LONG(cell % 8 * 32 + 32), LONG(cell / 8 * 32 + 32)};
            *x = LONG((i * 37) % (W - size));
            *y = LONG((i * 53) % (H - size));
        };
        Measure("copy 640x480 (BltFast)", bpp, W * H, [&](int) {
            dst->BltFast(0, 0, bg, nullptr, DDBLTFAST_NOCOLORKEY | DDBLTFAST_WAIT);
        });
        Measure("sprite 32x32 src key (BltFast)", bpp, 32 * 32, [&](int i) {
            RECT r;
            LONG x, y;
            spritePos(i, 32, &r, &x, &y);
            dst->BltFast(DWORD(x), DWORD(y), sheet, &r, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT);
        });
        Measure("sprite 32x32 key override (Blt)", bpp, 32 * 32, [&](int i) {
            RECT r;
            LONG x, y;
            spritePos(i, 32, &r, &x, &y);
            RECT d = {x, y, x + 32, y + 32};
            DDBLTFX fx = {};
            fx.dwSize = sizeof(fx);
            dst->Blt(&d, sheet, &r, DDBLT_KEYSRCOVERRIDE | DDBLT_WAIT, &fx);
        });
        Measure("sprite 32x32 src key mirrored (Blt)", bpp, 32 * 32, [&](int i) {
            RECT r;
            LONG x, y;
            spritePos(i, 32, &r, &x, &y);
            RECT d = {x, y, x + 32, y + 32};
            DDBLTFX fx = {};
            fx.dwSize = sizeof(fx);
            fx.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
            dst->Blt(&d, sheet, &r, DDBLT_KEYSRC | DDBLT_DDFX | DDBLT_WAIT, &fx);
        });
        Measure("sprite 32->48 src key stretched (Blt)", bpp, 48 * 48, [&](int i) {
            RECT r;
            LONG x, y;
            spritePos(i, 48, &r, &x, &y);
            RECT d = {x, y, x + 48, y + 48};
            dst->Blt(&d, sheet, &r, DDBLT_KEYSRC | DDBLT_WAIT, nullptr);
        });
        Measure("tile 32x32 no key (BltFast)", bpp, 32 * 32, [&](int i) {
            RECT r;
            LONG x, y;
            spritePos(i, 32, &r, &x, &y);
            dst->BltFast(DWORD(x), DWORD(y), sheet, &r, DDBLTFAST_NOCOLORKEY | DDBLTFAST_WAIT);
        });
        Measure("stretch 320x240 -> 640x480 (Blt)", bpp, W * H, [&](int) {
            RECT r = {0, 0, W / 2, H / 2};
            dst->Blt(nullptr, bg, &r, DDBLT_WAIT, nullptr);
        });
        Measure("scroll 640x479 up a row (Blt)", bpp, W * (H - 1), [&](int) {
            RECT s = {0, 1, W, H}, d = {0, 0, W, H - 1};
            dst->Blt(&d, dst, &s, DDBLT_WAIT, nullptr);
        });
        if (bpp != 8) {
            IDirectDrawSurface7* other = Offscreen(dd, W, H, bpp == 16 ? 32 : 16);
            if (other) {
                Paint(other, false);
                Measure(bpp == 16 ? "convert 640x480 from 32-bit (Blt)" : "convert 640x480 from 16-bit (Blt)", bpp, W * H,
                        [&](int) { dst->Blt(nullptr, other, nullptr, DDBLT_WAIT, nullptr); });
                other->Release();
            }
        }
        // Video: a YUY2 frame decoded and stretched to the screen's size, as a player does.
        if (IDirectDrawSurface7* yuy2 = FourCC(dd, 320, 240, MAKEFOURCC('Y', 'U', 'Y', '2'))) {
            Measure("YUY2 320x240 -> 640x480 (Blt)", bpp, W * H,
                    [&](int) { dst->Blt(nullptr, yuy2, nullptr, DDBLT_WAIT, nullptr); });
            yuy2->Release();
        }
        // A sprite out of a big DXT1 atlas.
        if (IDirectDrawSurface7* atlas = FourCC(dd, 1024, 1024, MAKEFOURCC('D', 'X', 'T', '1'))) {
            Measure("sprite 32x32 from a 1024x1024 DXT1 (Blt)", bpp, 32 * 32, [&](int i) {
                RECT r;
                LONG x, y;
                spritePos(i, 32, &r, &x, &y);
                RECT d = {x, y, x + 32, y + 32};
                dst->Blt(&d, atlas, &r, DDBLT_WAIT, nullptr);
            });
            atlas->Release();
        }
        Measure("fill 640x480", bpp, W * H, [&](int i) {
            DDBLTFX fx = {};
            fx.dwSize = sizeof(fx);
            fx.dwFillColor = DWORD(i);
            dst->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        });
        sheet->Release();
        bg->Release();
        dst->Release();
    }
    dd->Release();
    return 0;
}

static HWND MakeWindow(bool popup, int w, int h) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ddbench";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    HWND hwnd = popup ? CreateWindowExA(0, "ddbench", "ddbench", WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                                        GetSystemMetrics(SM_CYSCREEN), nullptr, nullptr, wc.hInstance, nullptr)
                      : CreateWindowExA(0, "ddbench", "ddbench", WS_OVERLAPPEDWINDOW, 50, 50, w, h, nullptr,
                                        nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return hwnd;
}

static int RunFlip(int w, int h, int bpp, double seconds) {
    HWND hwnd = MakeWindow(true, w, h);
    IDirectDraw7* dd = nullptr;
    if (FAILED(DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr))) return 1;
    if (FAILED(dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN)) ||
        FAILED(dd->SetDisplayMode(DWORD(w), DWORD(h), DWORD(bpp), 0, 0))) {
        fprintf(g_out, "flip %dx%dx%d: mode set failed\n", w, h, bpp);
        return 1;
    }
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    d.dwBackBufferCount = 1;
    IDirectDrawSurface7* front = nullptr;
    if (FAILED(dd->CreateSurface(&d, &front, nullptr))) return 1;
    DDSCAPS2 caps = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
    IDirectDrawSurface7* back = nullptr;
    front->GetAttachedSurface(&caps, &back);
    if (bpp == 8) {
        PALETTEENTRY pe[256];
        for (int i = 0; i < 256; i++) pe[i] = {BYTE(i), BYTE(i * 3), BYTE(255 - i), 0};
        IDirectDrawPalette* pal = nullptr;
        dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, nullptr);
        front->SetPalette(pal);
        pal->Release();
    }
    IDirectDrawSurface7* bg = Offscreen(dd, w, h, 0);
    IDirectDrawSurface7* sheet = Offscreen(dd, 256, 256, 0);
    Paint(bg, false);
    Paint(sheet, true);
    SetKey0(sheet);

    const int sprites = 500;
    std::vector<double> frames;
    double worstBlt = 0, worstFlip = 0, drawTotal = 0;
    const double start = Now();
    for (int f = 0; Now() - start < seconds; f++) {
        Pump();
        const double t0 = Now();
        back->BltFast(0, 0, bg, nullptr, DDBLTFAST_NOCOLORKEY | DDBLTFAST_WAIT);
        for (int i = 0; i < sprites; i++) {
            const int cell = (i + f) % 64;
            RECT r = {LONG(cell % 8 * 32), LONG(cell / 8 * 32), LONG(cell % 8 * 32 + 32), LONG(cell / 8 * 32 + 32)};
            const double b0 = Now();
            back->BltFast(DWORD((i * 37 + f * 3) % (w - 32)), DWORD((i * 53 + f) % (h - 32)), sheet, &r,
                          DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT);
            worstBlt = std::max(worstBlt, Now() - b0);
        }
        const double t1 = Now();
        front->Flip(nullptr, DDFLIP_NOVSYNC | DDFLIP_WAIT);
        const double t2 = Now();
        worstFlip = std::max(worstFlip, t2 - t1);
        drawTotal += t1 - t0;
        frames.push_back(t2 - t0);
    }
    if (frames.empty()) return 1;
    std::sort(frames.begin(), frames.end());
    const double total = Now() - start;
    fprintf(g_out,
            "flip %dx%dx%d, %d sprites: %.1f fps, draw %.2f ms/frame, frame p50 %.2f ms p99 %.2f ms, "
            "slowest Blt %.2f ms, slowest Flip %.2f ms\n",
            w, h, bpp, sprites, frames.size() / total, drawTotal / frames.size() * 1e3,
            frames[frames.size() / 2] * 1e3, frames[frames.size() * 99 / 100] * 1e3, worstBlt * 1e3,
            worstFlip * 1e3);
    fflush(g_out);
    sheet->Release();
    bg->Release();
    back->Release();
    front->Release();
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

static int RunWindow(double seconds) {
    HWND hwnd = MakeWindow(false, 800, 600);
    IDirectDraw7* dd = nullptr;
    if (FAILED(DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr))) return 1;
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* primary = nullptr;
    if (FAILED(dd->CreateSurface(&d, &primary, nullptr))) return 1;
    IDirectDrawClipper* clip = nullptr;
    dd->CreateClipper(0, &clip, nullptr);
    clip->SetHWnd(0, hwnd);
    primary->SetClipper(clip);
    IDirectDrawSurface7* sheet = Offscreen(dd, 256, 256, 0);
    Paint(sheet, false);
    // One full draw, then a small moving square, as a cursor or a ticking HUD does.
    POINT origin = {0, 0};
    ClientToScreen(hwnd, &origin);
    RECT full = {origin.x, origin.y, origin.x + 640, origin.y + 480};
    primary->Blt(&full, sheet, nullptr, DDBLT_WAIT, nullptr);
    int updates = 0;
    const double start = Now();
    while (Now() - start < seconds) {
        Pump();
        RECT r = {origin.x + (updates * 4) % 600, origin.y + 100, origin.x + (updates * 4) % 600 + 32, origin.y + 132};
        RECT s = {0, 0, 32, 32};
        primary->Blt(&r, sheet, &s, DDBLT_WAIT, nullptr);
        updates++;
        Sleep(5);
    }
    fprintf(g_out, "window: %d small Blts to the primary in %.1f s (see dd2d9.log for the presenter's work)\n",
            updates, seconds);
    fflush(g_out);
    sheet->Release();
    clip->Release();
    primary->Release();
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

static int RunZLock(int w, int h) {
    IDirectDraw7* dd = nullptr;
    if (FAILED(DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr))) return 1;
    dd->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    d.dwWidth = DWORD(w);
    d.dwHeight = DWORD(h);
    SetFormat(&d.ddpfPixelFormat, 32);
    IDirectDrawSurface7* rt = nullptr;
    if (FAILED(dd->CreateSurface(&d, &rt, nullptr))) return 2;
    DDSURFACEDESC2 z = {};
    z.dwSize = sizeof(z);
    z.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    z.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
    z.dwWidth = DWORD(w);
    z.dwHeight = DWORD(h);
    z.ddpfPixelFormat.dwSize = sizeof(z.ddpfPixelFormat);
    z.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
    z.ddpfPixelFormat.dwZBufferBitDepth = 16;
    z.ddpfPixelFormat.dwZBitMask = 0xFFFF;
    IDirectDrawSurface7* zs = nullptr;
    if (FAILED(dd->CreateSurface(&z, &zs, nullptr)) || FAILED(rt->AddAttachedSurface(zs))) return 3;
    IDirect3D7* d3d = nullptr;
    IDirect3DDevice7* dev = nullptr;
    if (FAILED(dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d))) ||
        FAILED(d3d->CreateDevice(IID_IDirect3DHALDevice, rt, &dev)))
        return 4;
    dev->SetRenderState(D3DRENDERSTATE_ZENABLE, TRUE);
    dev->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
    struct V { float x, y, z, rhw; DWORD c; };
    const float fw = float(w), fh = float(h);
    V tri[3] = {{fw * 0.5f, fh * 0.1f, 0.5f, 1, 0xFFFF0000}, {fw * 0.9f, fh * 0.9f, 0.5f, 1, 0xFF00FF00},
                {fw * 0.1f, fh * 0.9f, 0.5f, 1, 0xFF0000FF}};
    const char* names[3] = {"no Z lock", "8x8 Z lock (READONLY)", "whole Z lock"};
    volatile DWORD sink = 0;
    // A first round unmeasured: the first use of each path makes its resources (and the
    // driver may compile shaders then).
    for (int pass = 0; pass < 6; pass++) {
        const int mode = pass % 3;
        const bool measured = pass >= 3;
        int frames = 0;
        double worst = 0;
        const double t0 = Now();
        while (measured ? Now() - t0 < 2.0 : frames < 3) {
            const double f0 = Now();
            dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
            dev->BeginScene();
            dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, tri, 3, 0);
            dev->EndScene();
            if (mode) {
                RECT r = {w / 2 - 4, h / 2 - 4, w / 2 + 4, h / 2 + 4};
                DDSURFACEDESC2 l = {};
                l.dwSize = sizeof(l);
                if (SUCCEEDED(zs->Lock(mode == 1 ? &r : nullptr, &l, DDLOCK_WAIT | (mode == 1 ? DDLOCK_READONLY : 0), nullptr))) {
                    sink = sink + *static_cast<volatile WORD*>(l.lpSurface);
                    zs->Unlock(mode == 1 ? &r : nullptr);
                }
            }
            {  // every mode ends the frame waiting for the GPU, as presenting it would
                RECT r = {0, 0, 1, 1};
                DDSURFACEDESC2 l = {};
                l.dwSize = sizeof(l);
                if (SUCCEEDED(rt->Lock(&r, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
                    sink = sink + *static_cast<volatile DWORD*>(l.lpSurface);
                    rt->Unlock(&r);
                }
            }
            worst = std::max(worst, Now() - f0);
            frames++;
        }
        const double t = Now() - t0;
        if (!measured) continue;
        fprintf(g_out, "zlock %dx%d: %-22s %7.3f ms/frame (worst %.3f)\n", w, h, names[mode], t / frames * 1000, worst * 1000);
        fflush(g_out);
    }
    dev->Release();
    d3d->Release();
    zs->Release();
    rt->Release();
    dd->Release();
    return int(sink & 0);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmd, int) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "ddbench.txt");
    g_out = fopen(path, "a");
    if (!g_out) return 1;
    char mode[16] = "blit";
    int w = 640, h = 480, bpp = 32;
    double seconds = 5;
    sscanf(cmd, "%15s", mode);
    int r = 1;
    if (!strcmp(mode, "blit")) {
        fprintf(g_out, "== ddbench blit\n");
        r = RunBlit();
    } else if (!strcmp(mode, "flip")) {
        sscanf(cmd, "%*s %d %d %d %lf", &w, &h, &bpp, &seconds);
        fprintf(g_out, "== ddbench flip\n");
        r = RunFlip(w, h, bpp, seconds);
    } else if (!strcmp(mode, "window")) {
        sscanf(cmd, "%*s %lf", &seconds);
        fprintf(g_out, "== ddbench window\n");
        r = RunWindow(seconds);
    } else if (!strcmp(mode, "zlock")) {
        sscanf(cmd, "%*s %d %d", &w, &h);
        fprintf(g_out, "== ddbench zlock\n");
        r = RunZLock(w, h);
    }
    fclose(g_out);
    return r;
}
