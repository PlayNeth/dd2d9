// DD2D9 test program.
//   ddtest unit     - pixel-exact checks of blits, keys, conversion, GDI, flipping, surface lifetimes
//   ddtest fs8      - exclusive 640x480x8 flip-chain animation with palette and GDI text
//   ddtest pattern  - a still test image in exclusive 640x480x8 (or "pattern <w> <h>") for comparing filters
//   ddtest overlay  - overlay surfaces laid over an exclusive-mode primary (read back from the screen)
//   ddtest win      - windowed (DDSCL_NORMAL) 32-bit desktop, blits to the primary through a clipper
//   ddtest fuzz     - random same-format blits (keys, ranges, mirroring, stretching, overlap)
//                     compared with a reference implementation, for 8/16/32-bit formats
// Results are printed to ddtest.txt next to the executable.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <mmsystem.h>
#include <d3d.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static FILE* g_out;
static int g_fail;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (cond) {                                         \
            fprintf(g_out, "PASS ");                        \
        } else {                                            \
            fprintf(g_out, "FAIL ");                        \
            g_fail++;                                       \
        }                                                   \
        fprintf(g_out, __VA_ARGS__);                        \
        fputc('\n', g_out);                                 \
        fflush(g_out);                                      \
    } while (0)

static int g_userMsgs, g_screensaverMsgs;

static LPARAM g_lastMouseMove = -1;
static LPARAM g_lastSize = -1;  // WM_SIZE (restored or maximized)

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_USER + 1) g_userMsgs++;
    if (m == WM_MOUSEMOVE) g_lastMouseMove = l;
    if (m == WM_SIZE && (w == SIZE_RESTORED || w == SIZE_MAXIMIZED)) g_lastSize = l;
    if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_SCREENSAVE) {
        g_screensaverMsgs++;
        return 0;
    }
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcA(h, m, w, l);
}

static HWND MakeWindow(DWORD style, int w, int h) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ddtest";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "ddtest", "DD2D9 test", style, 0, 0, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return hwnd;
}

static void Pump() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static IDirectDrawSurface7* MakeSurface(IDirectDraw7* dd, int w, int h, DWORD bpp, DWORD r, DWORD g, DWORD b) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    d.dwWidth = w;
    d.dwHeight = h;
    d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB;
    d.ddpfPixelFormat.dwRGBBitCount = bpp;
    d.ddpfPixelFormat.dwRBitMask = r;
    d.ddpfPixelFormat.dwGBitMask = g;
    d.ddpfPixelFormat.dwBBitMask = b;
    IDirectDrawSurface7* s = nullptr;
    HRESULT hr = dd->CreateSurface(&d, &s, nullptr);
    CHECK(SUCCEEDED(hr), "CreateSurface %dx%d %lubpp -> 0x%08lx", w, h, bpp, hr);
    return s;
}

static IDirectDrawSurface7* Make32(IDirectDraw7* dd, int w, int h) { return MakeSurface(dd, w, h, 32, 0xFF0000, 0xFF00, 0xFF); }

static void Put32(IDirectDrawSurface7* s, const DWORD* px, int n) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    s->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
    memcpy(d.lpSurface, px, n * 4);
    s->Unlock(nullptr);
}

static bool Expect32(IDirectDrawSurface7* s, const DWORD* px, int n, const char* what) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    s->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
    bool ok = memcmp(d.lpSurface, px, n * 4) == 0;
    char got[160] = "";
    for (int i = 0; i < n && i < 8; i++)
        sprintf(got + strlen(got), "%08lx ", static_cast<DWORD*>(d.lpSurface)[i]);
    s->Unlock(nullptr);
    CHECK(ok, "%s (got %s)", what, got);
    return ok;
}

static HRESULT WINAPI CountModes(LPDDSURFACEDESC2, LPVOID ctx) {
    ++*static_cast<int*>(ctx);
    return DDENUMRET_OK;
}

static int RunUnit() {
    IDirectDraw7* dd = nullptr;
    HRESULT hr = DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    CHECK(SUCCEEDED(hr) && dd, "DirectDrawCreateEx -> 0x%08lx", hr);
    if (!dd) return 1;
    HWND hwnd = MakeWindow(WS_OVERLAPPEDWINDOW, 320, 240);
    CHECK(SUCCEEDED(dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL)), "SetCooperativeLevel(NORMAL)");

    {
        // A DirectDrawCreateEx object offers dd2d9's Direct3D 7: 3D is reported.
        DDCAPS caps = {};
        caps.dwSize = sizeof(caps);
        hr = dd->GetCaps(&caps, nullptr);
        CHECK(SUCCEEDED(hr) && (caps.dwCaps & DDCAPS_3D) && (caps.ddsCaps.dwCaps & DDSCAPS_3DDEVICE),
              "GetCaps reports 3D with Direct3D 7 (dwCaps 0x%08lx)", caps.dwCaps);
        IDirect3D7* d3d = nullptr;
        hr = dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d));
        CHECK(SUCCEEDED(hr) && d3d, "IDirect3D7 offered (0x%08lx)", hr);
        if (d3d) d3d->Release();
        // A DirectDrawCreate one reports 3D too and offers IDirect3D/2/3, not IDirect3D7.
        IDirectDraw* dd1 = nullptr;
        if (SUCCEEDED(DirectDrawCreate(nullptr, &dd1, nullptr))) {
            DDCAPS c1 = {};
            c1.dwSize = sizeof(c1);
            dd1->GetCaps(&c1, nullptr);
            CHECK(c1.dwCaps & DDCAPS_3D, "DirectDrawCreate's object reports 3D (dwCaps 0x%08lx)", c1.dwCaps);
            IDirect3D2* d2 = nullptr;
            hr = dd1->QueryInterface(IID_IDirect3D2, reinterpret_cast<void**>(&d2));
            CHECK(SUCCEEDED(hr) && d2, "IDirect3D2 offered by DirectDrawCreate's object (0x%08lx)", hr);
            if (d2) d2->Release();
            IDirect3D7* d7 = nullptr;
            hr = dd1->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d7));
            CHECK(hr == E_NOINTERFACE, "no IDirect3D7 there (0x%08lx)", hr);
            if (d7) d7->Release();
            dd1->Release();
        }
    }

    const DWORD A = 0x00112233, B = 0x00445566, C = 0x00778899, D = 0x00AABBCC, K = 0x00FF00FF;

    // Color fill
    IDirectDrawSurface7* s = Make32(dd, 4, 1);
    DDBLTFX fx = {};
    fx.dwSize = sizeof(fx);
    fx.dwFillColor = A;
    s->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    DWORD e1[] = {A, A, A, A};
    Expect32(s, e1, 4, "color fill");

    // Source color key
    IDirectDrawSurface7* src = Make32(dd, 4, 1);
    DWORD sp[] = {K, B, K, C};
    Put32(src, sp, 4);
    DDCOLORKEY key = {K, K};
    src->SetColorKey(DDCKEY_SRCBLT, &key);
    fx.dwFillColor = D;
    s->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL, &fx);
    s->BltFast(0, 0, src, nullptr, DDBLTFAST_SRCCOLORKEY);
    DWORD e2[] = {D, B, D, C};
    Expect32(s, e2, 4, "BltFast with source color key");

    // Key ignores the X8 byte
    DWORD sp2[] = {0xAAFF00FF, B, 0x00FF00FF, C};
    Put32(src, sp2, 4);
    s->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL, &fx);
    s->Blt(nullptr, src, nullptr, DDBLT_KEYSRC, nullptr);
    Expect32(s, e2, 4, "color key ignores unused high byte");

    // Stretch 2 -> 4
    IDirectDrawSurface7* two = Make32(dd, 2, 1);
    DWORD tp[] = {A, B};
    Put32(two, tp, 2);
    s->Blt(nullptr, two, nullptr, DDBLT_WAIT, nullptr);
    DWORD e3[] = {A, A, B, B};
    Expect32(s, e3, 4, "stretch 2x");

    // Mirror left-right
    DWORD mp[] = {A, B, C, D};
    Put32(src, mp, 4);
    DDBLTFX mfx = {};
    mfx.dwSize = sizeof(mfx);
    mfx.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
    s->Blt(nullptr, src, nullptr, DDBLT_DDFX, &mfx);
    DWORD e4[] = {D, C, B, A};
    Expect32(s, e4, 4, "mirror left-right");

    // Without a clipper, a destination rectangle past the surface is an error (ddraw.dll behaviour)
    fx.dwFillColor = 0;
    s->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL, &fx);
    RECT dr = {2, 0, 6, 1};
    hr = s->Blt(&dr, src, nullptr, 0, nullptr);
    CHECK(hr == DDERR_INVALIDRECT, "Blt past the surface without a clipper -> DDERR_INVALIDRECT (0x%08lx)", hr);
    DWORD e5[] = {0, 0, 0, 0};
    Expect32(s, e5, 4, "surface untouched by the rejected Blt");

    // With a clipper, the blit is clipped to its clip list
    IDirectDrawClipper* clip = nullptr;
    dd->CreateClipper(0, &clip, nullptr);
    struct {
        RGNDATAHEADER h;
        RECT r[1];
    } rgn = {{sizeof(RGNDATAHEADER), RDH_RECTANGLES, 1, sizeof(RECT), {0, 0, 3, 1}}, {{0, 0, 3, 1}}};
    clip->SetClipList(reinterpret_cast<RGNDATA*>(&rgn), 0);
    s->SetClipper(clip);
    RECT dr2 = {1, 0, 5, 1};
    hr = s->Blt(&dr2, src, nullptr, 0, nullptr);
    DWORD e5b[] = {0, A, B, 0};
    CHECK(hr == DD_OK, "Blt with a clip list -> 0x%08lx", hr);
    Expect32(s, e5b, 4, "Blt clipped to the clip list");
    s->SetClipper(nullptr);
    clip->Release();

    // BltFast never clips, and requires the color key it is asked to use
    hr = s->BltFast(2, 0, src, nullptr, DDBLTFAST_NOCOLORKEY);
    CHECK(hr == DDERR_INVALIDRECT, "BltFast past the surface -> DDERR_INVALIDRECT (0x%08lx)", hr);
    RECT srcBad = {0, 0, 5, 1};
    hr = s->BltFast(0, 0, src, &srcBad, DDBLTFAST_NOCOLORKEY);
    CHECK(hr == DDERR_INVALIDRECT, "BltFast with a source rect past the source -> DDERR_INVALIDRECT (0x%08lx)", hr);
    hr = s->BltFast(0, 0, two, nullptr, DDBLTFAST_SRCCOLORKEY);
    CHECK(hr == DDERR_INVALIDPARAMS, "BltFast SRCCOLORKEY without a key -> DDERR_INVALIDPARAMS (0x%08lx)", hr);

    // Overlapping copy within one surface
    IDirectDrawSurface7* ov = Make32(dd, 8, 1);
    DWORD op[] = {1, 2, 3, 4, 5, 0, 0, 0};
    Put32(ov, op, 8);
    RECT osr = {0, 0, 5, 1}, odr = {3, 0, 8, 1};
    ov->Blt(&odr, ov, &osr, 0, nullptr);
    DWORD e6[] = {1, 2, 3, 1, 2, 3, 4, 5};
    Expect32(ov, e6, 8, "overlapping blit");

    // 32-bit -> RGB565 conversion
    IDirectDrawSurface7* s16 = MakeSurface(dd, 2, 1, 16, 0xF800, 0x07E0, 0x001F);
    IDirectDrawSurface7* c32 = Make32(dd, 2, 1);
    DWORD cp[] = {0x00FF0000, 0x0000FF00};
    Put32(c32, cp, 2);
    s16->Blt(nullptr, c32, nullptr, 0, nullptr);
    {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        s16->Lock(nullptr, &d, 0, nullptr);
        WORD* p = static_cast<WORD*>(d.lpSurface);
        CHECK(p[0] == 0xF800 && p[1] == 0x07E0, "32-bit to RGB565 conversion (got %04x %04x)", p[0], p[1]);
        CHECK(d.ddpfPixelFormat.dwRGBBitCount == 16 && d.lPitch >= 4, "Lock reports format and pitch");
        s16->Unlock(nullptr);
    }

    // GDI on a surface
    HDC dc = nullptr;
    hr = s->GetDC(&dc);
    CHECK(SUCCEEDED(hr) && dc, "GetDC -> 0x%08lx", hr);
    if (dc) {
        SetPixel(dc, 1, 0, RGB(255, 0, 0));
        s->ReleaseDC(dc);
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        s->Lock(nullptr, &d, 0, nullptr);
        DWORD v = static_cast<DWORD*>(d.lpSurface)[1] & 0xFFFFFF;
        s->Unlock(nullptr);
        CHECK(v == 0xFF0000, "GDI SetPixel visible through Lock (got %06lx)", v);
    }

    // Interface versions share one object
    IDirectDrawSurface* s1 = nullptr;
    hr = s->QueryInterface(IID_IDirectDrawSurface, reinterpret_cast<void**>(&s1));
    CHECK(SUCCEEDED(hr) && s1, "QueryInterface surface7 -> surface1");
    if (s1) {
        DDSURFACEDESC d1 = {};
        d1.dwSize = sizeof(d1);
        hr = s1->GetSurfaceDesc(&d1);
        CHECK(SUCCEEDED(hr) && d1.dwWidth == 4 && d1.ddpfPixelFormat.dwRGBBitCount == 32, "v1 GetSurfaceDesc");
        s1->Release();
    }
    IDirectDraw* dd1 = nullptr;
    hr = dd->QueryInterface(IID_IDirectDraw, reinterpret_cast<void**>(&dd1));
    CHECK(SUCCEEDED(hr) && dd1, "QueryInterface DirectDraw7 -> DirectDraw");
    if (dd1) dd1->Release();

    // Palette round trip
    PALETTEENTRY pe[256];
    for (int i = 0; i < 256; i++) pe[i] = {BYTE(i), BYTE(255 - i), BYTE(i / 2), 0};
    IDirectDrawPalette* pal = nullptr;
    hr = dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, nullptr);
    CHECK(SUCCEEDED(hr) && pal, "CreatePalette");
    if (pal) {
        PALETTEENTRY got[4];
        pal->GetEntries(0, 100, 4, got);
        CHECK(got[0].peRed == 100 && got[0].peGreen == 155 && got[3].peBlue == 51, "palette GetEntries");
        pal->Release();
    }

    // Display modes
    int modes = 0;
    dd->EnumDisplayModes(0, nullptr, &modes, CountModes);
    CHECK(modes >= 3, "EnumDisplayModes (%d modes)", modes);

    // Argument checks found in review
    hr = s->Unlock(nullptr);
    CHECK(hr == DDERR_NOTLOCKED, "Unlock without Lock -> DDERR_NOTLOCKED (0x%08lx)", hr);
    {
        IDirectDrawPalette* p2 = nullptr;
        dd->CreatePalette(DDPCAPS_8BIT, pe, &p2, nullptr);
        PALETTEENTRY tmp[16];
        HRESULT a = p2->GetEntries(0, 250, 10, tmp), b = p2->GetEntries(0, 0xFFFFFFFFu, 2, tmp);
        CHECK(a == DDERR_INVALIDPARAMS && b == DDERR_INVALIDPARAMS, "palette range checks (0x%08lx 0x%08lx)", a, b);
        p2->Release();
    }
    {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        d.dwWidth = 64;
        d.dwHeight = 4;
        d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        d.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
        d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('Y', 'U', 'Y', '2');
        IDirectDrawSurface7* yuy = nullptr;
        hr = dd->CreateSurface(&d, &yuy, nullptr);
        if (yuy) {
            DDSURFACEDESC2 l = {};
            l.dwSize = sizeof(l);
            yuy->Lock(nullptr, &l, 0, nullptr);
            CHECK((l.dwFlags & DDSD_PITCH) && l.lPitch >= 128, "YUY2 surface has 2 bytes per pixel (pitch %ld)", l.lPitch);
            // White, black, then two of BT.601 red (video levels), as Y0 U Y1 V.
            static const BYTE kYuy[8] = {235, 128, 16, 128, 81, 90, 81, 240};
            memcpy(l.lpSurface, kYuy, 8);
            yuy->Unlock(nullptr);
            // Blitted to RGB (as a video player shows a frame): converted; stretched too.
            auto nearRgb = [](DWORD a, DWORD b) {
                for (int sh = 0; sh < 24; sh += 8)
                    if (abs(int((a >> sh) & 255) - int((b >> sh) & 255)) > 2) return false;
                return true;
            };
            IDirectDrawSurface7* rgb = Make32(dd, 8, 1);
            RECT four = {0, 0, 4, 1}, eight = {0, 0, 8, 1};
            hr = rgb->Blt(&four, yuy, &four, DDBLT_WAIT, nullptr);
            DDSURFACEDESC2 r = {};
            r.dwSize = sizeof(r);
            rgb->Lock(nullptr, &r, DDLOCK_WAIT, nullptr);
            DWORD got[8];
            memcpy(got, r.lpSurface, 32);
            rgb->Unlock(nullptr);
            CHECK(SUCCEEDED(hr) && nearRgb(got[0], 0xFFFFFF) && nearRgb(got[1], 0) && nearRgb(got[2], 0xFF0000) && nearRgb(got[3], 0xFF0000),
                  "YUY2 blitted to RGB (0x%08lx): %06lx %06lx %06lx %06lx", hr, got[0] & 0xFFFFFF, got[1] & 0xFFFFFF,
                  got[2] & 0xFFFFFF, got[3] & 0xFFFFFF);
            hr = rgb->Blt(&eight, yuy, &four, DDBLT_WAIT, nullptr);
            rgb->Lock(nullptr, &r, DDLOCK_WAIT, nullptr);
            memcpy(got, r.lpSurface, 32);
            rgb->Unlock(nullptr);
            CHECK(SUCCEEDED(hr) && nearRgb(got[0], 0xFFFFFF) && nearRgb(got[1], 0xFFFFFF) && nearRgb(got[2], 0) && nearRgb(got[3], 0) &&
                      nearRgb(got[5], 0xFF0000),
                  "YUY2 stretched 2x into RGB (0x%08lx): %06lx %06lx %06lx", hr, got[0] & 0xFFFFFF, got[2] & 0xFFFFFF, got[5] & 0xFFFFFF);
            // RGB into UYVY (U Y0 V Y1): the pair's chroma averaged.
            d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('U', 'Y', 'V', 'Y');
            IDirectDrawSurface7* uyvy = nullptr;
            hr = dd->CreateSurface(&d, &uyvy, nullptr);
            if (uyvy) {
                hr = uyvy->Blt(&four, rgb, &eight, DDBLT_WAIT, nullptr);  // (shrunk back: white, black, red, red)
                DDSURFACEDESC2 u = {};
                u.dwSize = sizeof(u);
                uyvy->Lock(nullptr, &u, DDLOCK_WAIT, nullptr);
                BYTE ub[8];
                memcpy(ub, u.lpSurface, 8);
                uyvy->Unlock(nullptr);
                auto close = [](int a, int b) { return abs(a - b) <= 2; };
                CHECK(SUCCEEDED(hr) && close(ub[1], 235) && close(ub[3], 16) && close(ub[0], 128) && close(ub[2], 128) &&
                          close(ub[5], 81) && close(ub[7], 81) && close(ub[4], 90) && close(ub[6], 240),
                      "RGB blitted into UYVY (0x%08lx): %u %u %u %u  %u %u %u %u", hr, ub[0], ub[1], ub[2], ub[3], ub[4], ub[5],
                      ub[6], ub[7]);
                uyvy->Release();
            } else {
                CHECK(false, "UYVY surface -> 0x%08lx", hr);
            }
            // YUY2 to YUY2 unstretched: copied as it is.
            d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('Y', 'U', 'Y', '2');
            IDirectDrawSurface7* yuy2 = nullptr;
            hr = dd->CreateSurface(&d, &yuy2, nullptr);
            if (yuy2) {
                RECT at = {8, 2, 12, 3};
                hr = yuy2->Blt(&at, yuy, &four, DDBLT_WAIT, nullptr);
                DDSURFACEDESC2 u = {};
                u.dwSize = sizeof(u);
                yuy2->Lock(nullptr, &u, DDLOCK_WAIT, nullptr);
                const bool same = memcmp(static_cast<BYTE*>(u.lpSurface) + 2 * u.lPitch + 16, kYuy, 8) == 0;
                yuy2->Unlock(nullptr);
                CHECK(SUCCEEDED(hr) && same, "YUY2 to YUY2 copied as it is (0x%08lx)", hr);
                yuy2->Release();
            }
            // A colour key on the YUY2 source, in its own packing (the first pixel of the
            // pair: white): white pixels skipped, the others drawn.
            {
                DDCOLORKEY k = {0x8010'80EBu, 0x8010'80EBu};  // Y0 235, U 128, Y1 16, V 128
                hr = yuy->SetColorKey(DDCKEY_SRCBLT, &k);
                DDBLTFX bfx = {};
                bfx.dwSize = sizeof(bfx);
                bfx.dwFillColor = 0x0000FF;
                rgb->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &bfx);
                const HRESULT hk = rgb->Blt(&four, yuy, &four, DDBLT_KEYSRC | DDBLT_WAIT, nullptr);
                rgb->Lock(nullptr, &r, DDLOCK_WAIT, nullptr);
                memcpy(got, r.lpSurface, 32);
                rgb->Unlock(nullptr);
                CHECK(SUCCEEDED(hr) && SUCCEEDED(hk) && (got[0] & 0xFFFFFF) == 0x0000FF && nearRgb(got[1], 0) &&
                          nearRgb(got[2], 0xFF0000),
                      "YUY2 source colour key (0x%08lx 0x%08lx): %06lx %06lx %06lx", hr, hk, got[0] & 0xFFFFFF,
                      got[1] & 0xFFFFFF, got[2] & 0xFFFFFF);
            }
            rgb->Release();
            yuy->Release();
        } else {
            CHECK(false, "YUY2 surface -> 0x%08lx", hr);
        }
        // Planar YV12 / I420 (4:2:0): a luma plane, then the chroma planes at half the pitch.
        {
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
            d.dwWidth = 4;
            d.dwHeight = 2;
            d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            d.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
            d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('Y', 'V', '1', '2');
            IDirectDrawSurface7* yv = nullptr;
            hr = dd->CreateSurface(&d, &yv, nullptr);
            CHECK(SUCCEEDED(hr), "YV12 surface -> 0x%08lx", hr);
            if (yv) {
                DDSURFACEDESC2 l = {};
                l.dwSize = sizeof(l);
                yv->Lock(nullptr, &l, DDLOCK_WAIT, nullptr);
                const LONG pitch = l.lPitch;
                BYTE* b = static_cast<BYTE*>(l.lpSurface);
                // White, white, red, red on both rows: V plane first (128, 240), then U (128, 90).
                for (int y = 0; y < 2; y++) {
                    b[y * pitch + 0] = b[y * pitch + 1] = 235;
                    b[y * pitch + 2] = b[y * pitch + 3] = 81;
                }
                BYTE* v = b + 2 * pitch;
                BYTE* u = v + pitch / 2;
                v[0] = 128, v[1] = 240, u[0] = 128, u[1] = 90;
                yv->Unlock(nullptr);
                CHECK(!(pitch & 0xFF) && (l.dwFlags & DDSD_PITCH), "YV12: a byte of luma per pixel, pitch %ld", pitch);
                auto nearRgb = [](DWORD a, DWORD c) {
                    for (int sh = 0; sh < 24; sh += 8)
                        if (abs(int((a >> sh) & 255) - int((c >> sh) & 255)) > 2) return false;
                    return true;
                };
                IDirectDrawSurface7* rgb = Make32(dd, 4, 2);
                hr = rgb->Blt(nullptr, yv, nullptr, DDBLT_WAIT, nullptr);
                DDSURFACEDESC2 r = {};
                r.dwSize = sizeof(r);
                rgb->Lock(nullptr, &r, DDLOCK_WAIT, nullptr);
                DWORD got[4];
                for (int x = 0; x < 4; x++) got[x] = reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.lpSurface) + r.lPitch)[x];
                rgb->Unlock(nullptr);
                CHECK(SUCCEEDED(hr) && nearRgb(got[0], 0xFFFFFF) && nearRgb(got[1], 0xFFFFFF) && nearRgb(got[2], 0xFF0000) &&
                          nearRgb(got[3], 0xFF0000),
                      "YV12 blitted to RGB (0x%08lx): %06lx %06lx %06lx %06lx", hr, got[0] & 0xFFFFFF, got[1] & 0xFFFFFF,
                      got[2] & 0xFFFFFF, got[3] & 0xFFFFFF);
                // RGB into I420 (U plane first).
                d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('I', '4', '2', '0');
                IDirectDrawSurface7* i420 = nullptr;
                if (SUCCEEDED(dd->CreateSurface(&d, &i420, nullptr))) {
                    hr = i420->Blt(nullptr, rgb, nullptr, DDBLT_WAIT, nullptr);
                    DDSURFACEDESC2 q = {};
                    q.dwSize = sizeof(q);
                    i420->Lock(nullptr, &q, DDLOCK_WAIT, nullptr);
                    const BYTE* qb = static_cast<BYTE*>(q.lpSurface);
                    const BYTE* qu = qb + 2 * q.lPitch;
                    const BYTE* qv = qu + q.lPitch / 2;
                    auto close = [](int a, int c) { return abs(a - c) <= 2; };
                    const bool ok = close(qb[0], 235) && close(qb[q.lPitch + 3], 81) && close(qu[0], 128) && close(qu[1], 90) &&
                                    close(qv[0], 128) && close(qv[1], 240);
                    CHECK(SUCCEEDED(hr) && ok, "RGB blitted into I420 (0x%08lx): Y %u %u, U %u %u, V %u %u", hr, qb[0],
                          qb[q.lPitch + 3], qu[0], qu[1], qv[0], qv[1]);
                    i420->Unlock(nullptr);
                    i420->Release();
                }
                // YV12 to YV12: copied as it is, plane by plane.
                d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('Y', 'V', '1', '2');
                IDirectDrawSurface7* yv2 = nullptr;
                if (SUCCEEDED(dd->CreateSurface(&d, &yv2, nullptr))) {
                    hr = yv2->Blt(nullptr, yv, nullptr, DDBLT_WAIT, nullptr);
                    DDSURFACEDESC2 a = {}, c = {};
                    a.dwSize = c.dwSize = sizeof(a);
                    yv->Lock(nullptr, &a, DDLOCK_WAIT, nullptr);
                    yv2->Lock(nullptr, &c, DDLOCK_WAIT, nullptr);
                    const bool same = memcmp(a.lpSurface, c.lpSurface, size_t(a.lPitch) * 3) == 0;
                    yv2->Unlock(nullptr);
                    yv->Unlock(nullptr);
                    CHECK(SUCCEEDED(hr) && same, "YV12 to YV12 copied as it is (0x%08lx)", hr);
                    yv2->Release();
                }
                rgb->Release();
                yv->Release();
            }
        }
        // RGB into an 8-bit surface: each pixel the nearest palette entry.
        {
            IDirectDrawSurface7* rgb8 = Make32(dd, 4, 1);
            IDirectDrawSurface7* p8 = MakeSurface(dd, 4, 1, 8, 0, 0, 0);
            PALETTEENTRY pe[256] = {};
            for (int i = 0; i < 256; i++) pe[i] = {BYTE(i & 0x40), BYTE(i & 0x20), BYTE(i & 0x10), 0};
            pe[5] = {255, 0, 0, 0};
            pe[9] = {0, 255, 0, 0};
            IDirectDrawPalette* pal8 = nullptr;
            dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal8, nullptr);
            if (rgb8 && p8 && pal8) {
                p8->SetPalette(pal8);
                const DWORD px[4] = {0xFF0000, 0x00FF00, 0xF01010, 0x000000};
                Put32(rgb8, px, 4);
                hr = p8->Blt(nullptr, rgb8, nullptr, DDBLT_WAIT, nullptr);
                DDSURFACEDESC2 l8 = {};
                l8.dwSize = sizeof(l8);
                p8->Lock(nullptr, &l8, DDLOCK_WAIT, nullptr);
                BYTE ix[4];
                memcpy(ix, l8.lpSurface, 4);
                p8->Unlock(nullptr);
                CHECK(SUCCEEDED(hr) && ix[0] == 5 && ix[1] == 9 && ix[2] == 5 && ix[3] == 0,
                      "RGB blitted into an 8-bit surface: nearest palette entries (0x%08lx): %u %u %u %u", hr, ix[0], ix[1],
                      ix[2], ix[3]);
            }
            if (pal8) pal8->Release();
            if (p8) p8->Release();
            if (rgb8) rgb8->Release();
        }
        DWORD codes[16] = {}, n = 16, count = 0;
        dd->GetFourCCCodes(&count, nullptr);
        dd->GetFourCCCodes(&n, codes);
        CHECK(count == 8 && n == 8 && codes[7] == MAKEFOURCC('Y', 'V', '1', '2') && codes[0] == MAKEFOURCC('Y', 'U', 'Y', '2'), "GetFourCCCodes: %lu codes (%lu copied)", count, n);
        d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('X', 'Y', 'Z', 'W');
        IDirectDrawSurface7* unk = nullptr;
        hr = dd->CreateSurface(&d, &unk, nullptr);
        CHECK(hr == DDERR_INVALIDPIXELFORMAT, "unknown FourCC refused (0x%08lx)", hr);
        if (unk) unk->Release();
    }

    // Flip rings put together with AddAttachedSurface, as ddraw.dll builds them: through
    // IDirectDrawSurface4 too, a surface attached to the front becomes the back buffer.
    {
        IDirectDrawSurface7* m7[3] = {Make32(dd, 8, 8), Make32(dd, 8, 8), Make32(dd, 8, 8)};
        IDirectDrawSurface4* m[3] = {};
        for (int i = 0; i < 3; i++) m7[i]->QueryInterface(IID_IDirectDrawSurface4, reinterpret_cast<void**>(&m[i]));
        auto caps = [](IDirectDrawSurface4* x) {
            DDSCAPS2 c = {};
            x->GetCaps(&c);
            return c.dwCaps & (DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER);
        };
        hr = m[0]->AddAttachedSurface(m[1]);
        CHECK(hr == DD_OK && caps(m[0]) == (DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER) &&
                  caps(m[1]) == (DDSCAPS_FLIP | DDSCAPS_BACKBUFFER),
              "IDirectDrawSurface4 ring of two (0x%08lx)", hr);
        hr = m[0]->AddAttachedSurface(m[2]);
        CHECK(hr == DD_OK && caps(m[2]) == (DDSCAPS_FLIP | DDSCAPS_BACKBUFFER) && caps(m[1]) == DDSCAPS_FLIP,
              "attached to the front: the new back buffer (0x%08lx)", hr);
        DDSCAPS2 want = {DDSCAPS_BACKBUFFER};
        IDirectDrawSurface4* got = nullptr;
        hr = m[0]->GetAttachedSurface(&want, &got);
        CHECK(hr == DD_OK && got == m[2], "GetAttachedSurface(BACKBUFFER) finds it (0x%08lx)", hr);
        if (got) got->Release();
        DDSCAPS2 none = {};
        hr = m[0]->GetAttachedSurface(&none, &got);
        CHECK(hr == DDERR_NOTFOUND, "GetAttachedSurface without caps -> DDERR_NOTFOUND (0x%08lx)", hr);
        hr = m[0]->DeleteAttachedSurface(1, m[2]);
        CHECK(hr == DDERR_INVALIDPARAMS, "DeleteAttachedSurface flags -> DDERR_INVALIDPARAMS (0x%08lx)", hr);
        hr = m[1]->DeleteAttachedSurface(0, m[2]);
        CHECK(hr == DDERR_SURFACENOTATTACHED, "not attached there -> DDERR_SURFACENOTATTACHED (0x%08lx)", hr);
        hr = m[0]->DeleteAttachedSurface(0, nullptr);
        CHECK(hr == DD_OK && caps(m[1]) == (DDSCAPS_FLIP | DDSCAPS_BACKBUFFER),
              "DeleteAttachedSurface(NULL) detaches the newest (0x%08lx)", hr);
        hr = m[0]->DeleteAttachedSurface(0, nullptr);
        CHECK(hr == DD_OK && caps(m[0]) == 0 && caps(m[1]) == 0, "and the ring comes apart (0x%08lx)", hr);
        for (int i = 0; i < 3; i++) {
            if (m[i]) m[i]->Release();
            m7[i]->Release();
        }
    }

    // The old surface interfaces, as ddraw.dll (spec: re/spec/legacy-interfaces.md): an
    // attachment is the pointer passed (a v1 one through IDirectDrawSurface3 here), and
    // IDirectDrawSurface2/3 hand out IDirectDrawSurface (v1) pointers.
    {
        IDirectDrawSurface7* a7 = Make32(dd, 8, 8);
        IDirectDrawSurface7* b7 = Make32(dd, 8, 8);
        IDirectDrawSurface3* a3 = nullptr;
        IDirectDrawSurface* a1 = nullptr;
        IDirectDrawSurface* b1 = nullptr;
        a7->QueryInterface(IID_IDirectDrawSurface3, reinterpret_cast<void**>(&a3));
        a7->QueryInterface(IID_IDirectDrawSurface, reinterpret_cast<void**>(&a1));
        b7->QueryInterface(IID_IDirectDrawSurface, reinterpret_cast<void**>(&b1));
        auto* b1as3 = reinterpret_cast<IDirectDrawSurface3*>(b1);
        hr = a3->AddAttachedSurface(b1as3);
        CHECK(hr == DD_OK, "v1 pointer attached through IDirectDrawSurface3 (0x%08lx)", hr);
        DDSCAPS want = {DDSCAPS_BACKBUFFER};
        IDirectDrawSurface3* got = nullptr;
        hr = a3->GetAttachedSurface(&want, &got);
        CHECK(hr == DD_OK && static_cast<void*>(got) == static_cast<void*>(b1),
              "IDirectDrawSurface3::GetAttachedSurface hands out the v1 pointer (0x%08lx)", hr);
        if (got) got->Release();
        hr = a3->AddAttachedSurface(b1as3);
        CHECK(hr == DDERR_SURFACEALREADYATTACHED, "the same pointer again -> SURFACEALREADYATTACHED (0x%08lx)", hr);
        hr = a1->DeleteAttachedSurface(0, b1);
        CHECK(hr == DD_OK, "detached through IDirectDrawSurface with the same pointer (0x%08lx)", hr);
        b1->Release();
        a1->Release();
        a3->Release();
        b7->Release();
        a7->Release();
    }

    // Flip chain swaps memory
    IDirectDrawSurface7* prim = nullptr;
    {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS;
        d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        hr = dd->CreateSurface(&d, &prim, nullptr);
        CHECK(SUCCEEDED(hr), "windowed primary -> 0x%08lx", hr);
        if (prim) {
            DDSURFACEDESC2 l = {};
            l.dwSize = sizeof(l);
            prim->Lock(nullptr, &l, 0, nullptr);
            // As in ddraw.dll: no mode change while a video-memory surface is locked.
            hr = dd->SetDisplayMode(640, 480, 16, 0, 0);
            CHECK(hr == DDERR_SURFACEBUSY, "SetDisplayMode with the primary locked (0x%08lx)", hr);
            prim->Unlock(nullptr);
            hr = dd->SetDisplayMode(640, 480, 16, 0, 0);
            CHECK(hr == DD_OK, "SetDisplayMode once unlocked (0x%08lx)", hr);
            CHECK(prim->IsLost() == DDERR_SURFACELOST, "primary lost after SetDisplayMode");
            DDSURFACEDESC2 now = {};
            now.dwSize = sizeof(now);
            prim->GetSurfaceDesc(&now);
            CHECK(now.ddpfPixelFormat.dwRGBBitCount == 16, "the lost primary reports the display's format (%lu bpp)",
                  now.ddpfPixelFormat.dwRGBBitCount);
            hr = prim->Restore();
            CHECK(hr == DDERR_WRONGMODE, "primary not restorable in another mode (0x%08lx)", hr);
            dd->RestoreDisplayMode();
            hr = prim->Restore();
            CHECK(hr == DD_OK, "primary restorable in its own mode again (0x%08lx)", hr);
        }
        IDirectDrawSurface7* dup = nullptr;
        hr = dd->CreateSurface(&d, &dup, nullptr);
        CHECK(hr == DDERR_PRIMARYSURFACEALREADYEXISTS, "second primary refused");
        if (prim) prim->Release();
    }

    s->Release();
    src->Release();
    two->Release();
    ov->Release();
    s16->Release();
    c32->Release();
    ULONG left = dd->Release();
    CHECK(left == 0, "DirectDraw released (refcount %lu)", left);
    DestroyWindow(hwnd);
    return 0;
}

// "pattern <w> <h>": a still test image in an exclusive 8-bit mode, held for a few seconds so a
// screenshot can compare the scaling filters (1-pixel lines, a checkerboard, a diagonal).
static int RunPattern(int modeW, int modeH) {
    HWND hwnd = MakeWindow(WS_POPUP, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    if (!hwnd) return 1;
    IDirectDraw7* dd = nullptr;
    if (FAILED(DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr))) return 1;
    dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    CHECK(SUCCEEDED(dd->SetDisplayMode(modeW, modeH, 8, 0, 0)), "pattern: SetDisplayMode %dx%dx8", modeW, modeH);
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* front = nullptr;
    CHECK(SUCCEEDED(dd->CreateSurface(&d, &front, nullptr)), "pattern: primary");
    if (!front) return 1;
    PALETTEENTRY pe[256] = {};
    pe[1] = {255, 255, 255, 0};
    pe[2] = {255, 0, 0, 0};
    IDirectDrawPalette* pal = nullptr;
    dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, nullptr);
    front->SetPalette(pal);
    d = {};
    d.dwSize = sizeof(d);
    if (SUCCEEDED(front->Lock(nullptr, &d, DDLOCK_WAIT, nullptr))) {
        for (int y = 0; y < modeH; y++) {
            BYTE* row = static_cast<BYTE*>(d.lpSurface) + y * d.lPitch;
            for (int x = 0; x < modeW; x++) {
                BYTE c = 0;
                if (x < modeW / 2) c = ((x / 4 + y / 4) & 1) ? 1 : 0;      // 4-pixel checkerboard
                else if ((x & 7) == 0 || (y & 7) == 0) c = 1;                // 1-pixel grid
                if (x == y) c = 2;                                           // diagonal
                row[x] = c;
            }
        }
        front->Unlock(nullptr);
    }
    for (int i = 0; i < 40; i++) {  // ~4 s for the screenshot
        Pump();
        Sleep(100);
    }
    if (pal) pal->Release();
    front->Release();
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

static int RunFullscreen8(int modeW, int modeH) {
    HWND hwnd = MakeWindow(WS_POPUP, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    const DWORD createError = hwnd ? 0 : GetLastError();
    CHECK(hwnd != nullptr, "test window created (error %lu)", createError);
    if (!hwnd) return 1;  // no display: nothing below can work
    IDirectDraw* dd1 = nullptr;
    HRESULT hr = DirectDrawCreate(nullptr, &dd1, nullptr);
    CHECK(SUCCEEDED(hr), "DirectDrawCreate");
    IDirectDraw7* dd = nullptr;
    dd1->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&dd));
    dd1->Release();
    hr = dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    CHECK(SUCCEEDED(hr), "exclusive mode -> 0x%08lx (hwnd %p, style 0x%08lx)", hr, static_cast<void*>(hwnd),
          GetWindowLongA(hwnd, GWL_STYLE));
    // Exclusive mode subclasses the window (focus handling), forwarding everything else.
    CHECK(GetWindowLongPtrA(hwnd, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(WndProc), "window subclassed in exclusive mode");
    SendMessageA(hwnd, WM_USER + 1, 0, 0);
    CHECK(g_userMsgs == 1, "subclassed window still forwards messages");
    SendMessageA(hwnd, WM_SYSCOMMAND, SC_SCREENSAVE, 0);
    CHECK(g_screensaverMsgs == 0, "screensaver blocked in exclusive mode");
    CHECK(SUCCEEDED(dd->SetDisplayMode(modeW, modeH, 8, 0, 0)), "SetDisplayMode %dx%dx8", modeW, modeH);
    {
        // An 8-bit mode makes the screen a palette device, as on XP (FlyingSB sets its palette
        // up only then).
        HDC screen = GetDC(nullptr);
        CHECK(GetDeviceCaps(screen, BITSPIXEL) == 8 && (GetDeviceCaps(screen, RASTERCAPS) & RC_PALETTE) &&
                  GetDeviceCaps(screen, SIZEPALETTE) == 256 && GetDeviceCaps(screen, NUMRESERVED) == 20,
              "GDI: an 8-bit palette device in the 8-bit mode (bits %d, raster caps 0x%x, palette %d)",
              GetDeviceCaps(screen, BITSPIXEL), GetDeviceCaps(screen, RASTERCAPS), GetDeviceCaps(screen, SIZEPALETTE));
        ReleaseDC(nullptr, screen);
    }
    // A game changing its window's frame in exclusive mode (JJ2+ while starting up): the
    // client area still covers the screen.
    // Then sizing it to its own idea of fullscreen (Unreal after a style change): still covering.
    for (LONG style : {LONG(WS_OVERLAPPEDWINDOW), LONG(WS_POPUP), LONG(WS_OVERLAPPEDWINDOW)}) {
        static int pass = 0;
        SetWindowLongA(hwnd, GWL_STYLE, style | WS_VISIBLE);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        if (pass++ == 2) SetWindowPos(hwnd, nullptr, 0, 0, modeW, modeH - 40, SWP_NOZORDER);
        Pump();
        // The real window (dd2d9 shows a scaled mode's window to the game as its screen).
        using RectFn = BOOL(WINAPI*)(HWND, RECT*);
        using MapFn = int(WINAPI*)(HWND, HWND, POINT*, UINT);
        HMODULE user32 = GetModuleHandleA("user32.dll");
        const auto realClientRect = reinterpret_cast<RectFn>(reinterpret_cast<void*>(GetProcAddress(user32, "GetClientRect")));
        const auto realMap = reinterpret_cast<MapFn>(reinterpret_cast<void*>(GetProcAddress(user32, "MapWindowPoints")));
        RECT cr;
        realClientRect(hwnd, &cr);
        realMap(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        CHECK(EqualRect(&cr, &mi.rcMonitor), "client area covers the screen after a frame change (%ld,%ld-%ld,%ld)",
              cr.left, cr.top, cr.right, cr.bottom);
    }

    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    d.dwBackBufferCount = 1;
    IDirectDrawSurface7* front = nullptr;
    hr = dd->CreateSurface(&d, &front, nullptr);
    CHECK(SUCCEEDED(hr), "flip chain -> 0x%08lx", hr);
    DDSCAPS2 caps = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
    IDirectDrawSurface7* back = nullptr;
    hr = front->GetAttachedSurface(&caps, &back);
    CHECK(SUCCEEDED(hr) && back, "GetAttachedSurface(BACKBUFFER)");
    if (!front || !back) return 1;

    PALETTEENTRY pe[256];
    for (int i = 0; i < 256; i++) pe[i] = {BYTE(i), BYTE((i * 3) & 255), BYTE(255 - i), 0};
    pe[0] = {0, 0, 40, 0};
    pe[255] = {255, 255, 255, 0};
    IDirectDrawPalette* pal = nullptr;
    dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, nullptr);
    front->SetPalette(pal);

    // Sprite: 64x64 disc on color-key 0
    d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
    d.dwWidth = 64;
    d.dwHeight = 64;
    IDirectDrawSurface7* sprite = nullptr;
    dd->CreateSurface(&d, &sprite, nullptr);
    d = {};
    d.dwSize = sizeof(d);
    sprite->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int dx = x - 32, dy = y - 32;
            static_cast<BYTE*>(d.lpSurface)[y * d.lPitch + x] = dx * dx + dy * dy < 900 ? BYTE(160 + (x + y) / 4) : 0;
        }
    sprite->Unlock(nullptr);
    DDCOLORKEY key = {0, 0};
    sprite->SetColorKey(DDCKEY_SRCBLT, &key);

    // Frame (1st Flip) is checked against the pacing target.
    const int frames = 180;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < frames; i++) {
        Pump();
        // Background: vertical palette-index gradient
        d = {};
        d.dwSize = sizeof(d);
        back->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
        for (int y = 0; y < modeH; y++) memset(static_cast<BYTE*>(d.lpSurface) + y * d.lPitch, (y / 2) & 0x7F, modeW);
        back->Unlock(nullptr);
        for (int k = 0; k < 6; k++) {
            int x = (i * (3 + k) + k * 90) % (modeW - 64), y = (60 + k * 60 + ((i * 2) % 40)) % (modeH - 64);
            back->BltFast(x, y, sprite, nullptr, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT);
        }
        HDC dc;
        if (SUCCEEDED(back->GetDC(&dc))) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            char text[64];
            sprintf(text, "DD2D9 640x480x8  frame %d", i);
            TextOutA(dc, 20, 20, text, int(strlen(text)));
            back->ReleaseDC(dc);
        }
        front->Flip(nullptr, DDFLIP_WAIT);
    }
    QueryPerformanceCounter(&t1);
    double secs = double(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    DWORD hz = 60;
    dd->GetMonitorFrequency(&hz);
    CHECK(secs > frames / double(hz) * 0.8, "Flip paced to the refresh rate: %d frames in %.2fs (%.1f fps, %lu Hz)",
          frames, secs, frames / secs, hz);

    // Focus loss and return, as with Alt+Tab. When the display mode was really
    // switched, the game is minimized and the desktop mode restored; coming back
    // sets everything up again and presenting must resume.
    SendMessageA(hwnd, WM_ACTIVATEAPP, FALSE, 0);
    Pump();
    const bool minimized = IsIconic(hwnd) != FALSE;
    // In the background the primary is lost, but can still be locked (a deliberate
    // difference: Captain Claw writes through the pointer of a failed lock).
    {
        CHECK(front->IsLost() == DDERR_SURFACELOST, "primary lost in the background");
        DDSURFACEDESC2 ld = {};
        ld.dwSize = sizeof(ld);
        hr = front->Lock(nullptr, &ld, DDLOCK_WAIT, nullptr);
        CHECK(hr == DD_OK && ld.lpSurface, "primary still lockable in the background (0x%08lx)", hr);
        if (SUCCEEDED(hr)) front->Unlock(nullptr);
        // Its back buffer isn't, as with ddraw.dll (NFS Porsche Unleashed, finding it locked,
        // flipped without unlocking).
        DDSURFACEDESC2 bd = {};
        bd.dwSize = sizeof(bd);
        hr = back->Lock(nullptr, &bd, DDLOCK_WAIT, nullptr);
        CHECK(hr == DDERR_SURFACELOST && !bd.lpSurface, "back buffer not lockable in the background (0x%08lx)", hr);
        if (SUCCEEDED(hr)) back->Unlock(nullptr);
    }
    SendMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
    Pump();
    CHECK(!IsIconic(hwnd), "window restored after focus returns (was minimized: %d)", minimized);
    // The surfaces were lost meanwhile, as in ddraw.dll: the game restores them (display=switch,
    // even where the test display lacks the mode and dd2d9 shows it scaled).
    hr = front->Flip(nullptr, DDFLIP_WAIT);
    CHECK(hr == DDERR_SURFACELOST, "flip after focus returns: surface lost (0x%08lx)", hr);
    // Its other surfaces report lost too, as in ddraw.dll, but can still be drawn with (a
    // deliberate difference: dd2d9 keeps their pixels, and Captain Claw never restores some
    // of its sprites).
    hr = sprite->IsLost();
    CHECK(hr == DDERR_SURFACELOST, "offscreen sprite reported lost after the focus loss (0x%08lx)", hr);
    {
        IDirectDrawSurface7* tmp = nullptr;
        DDSURFACEDESC2 td = {};
        td.dwSize = sizeof(td);
        td.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        td.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
        td.dwWidth = 64;
        td.dwHeight = 64;
        hr = dd->CreateSurface(&td, &tmp, nullptr);
        if (SUCCEEDED(hr)) {
            hr = tmp->BltFast(0, 0, sprite, nullptr, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT);
            tmp->Release();
        }
    }
    CHECK(hr == DD_OK, "offscreen sprite still drawable after the focus loss (0x%08lx)", hr);
    hr = dd->TestCooperativeLevel();
    CHECK(hr == DD_OK, "cooperative level fine after focus returns (0x%08lx)", hr);
    hr = dd->RestoreAllSurfaces();
    CHECK(hr == DD_OK && front->IsLost() == DD_OK, "surfaces restored (0x%08lx)", hr);
    int flipErrors = 0;
    for (int i = 0; i < 60; i++) {
        Pump();
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = BYTE(i);
        back->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        if (FAILED(front->Flip(nullptr, DDFLIP_WAIT))) flipErrors++;
    }
    CHECK(flipErrors == 0, "flipping after focus returns (%d errors)", flipErrors);

    // Hold the last frame so it can be captured.
    for (int i = 0; i < 300; i++) {
        Pump();
        Sleep(10);
    }
    sprite->Release();
    pal->Release();
    back->Release();
    front->Release();
    dd->Release();
    CHECK(GetWindowLongPtrA(hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(WndProc), "window procedure restored on release");
    DestroyWindow(hwnd);
    return 0;
}

static int RunWindowed() {
    HWND hwnd = MakeWindow(WS_OVERLAPPEDWINDOW, 400, 300);
    IDirectDraw7* dd = nullptr;
    DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    CHECK(SUCCEEDED(dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL)), "normal mode");
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* prim = nullptr;
    dd->CreateSurface(&d, &prim, nullptr);
    IDirectDrawClipper* clip = nullptr;
    dd->CreateClipper(0, &clip, nullptr);
    clip->SetHWnd(0, hwnd);
    prim->SetClipper(clip);

    IDirectDrawSurface7* img = Make32(dd, 64, 64);
    d = {};
    d.dwSize = sizeof(d);
    img->Lock(nullptr, &d, 0, nullptr);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) static_cast<DWORD*>(d.lpSurface)[y * d.lPitch / 4 + x] = (x * 4) << 16 | (y * 4) << 8 | 0x80;
    img->Unlock(nullptr);

    for (int i = 0; i < 400; i++) {
        Pump();
        RECT cr;
        GetClientRect(hwnd, &cr);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = 0x202040;
        prim->Blt(&cr, nullptr, nullptr, DDBLT_COLORFILL, &fx);
        RECT dst = {cr.left + 20 + (i % 200), cr.top + 40, cr.left + 20 + (i % 200) + 128, cr.top + 40 + 128};
        prim->Blt(&dst, img, nullptr, DDBLT_WAIT, nullptr);
        dd->WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, nullptr);
    }
    img->Release();
    clip->Release();
    prim->Release();
    dd->Release();
    DestroyWindow(hwnd);
    CHECK(true, "windowed run finished");
    return 0;
}

// Lifetimes of complex surfaces, as in ddraw: each member counts its own
// references, and releasing the root destroys the structure. A member the game
// still references stays usable on its own until released.
// Direct3D 1 execute buffers: Pick (the triangle under a point, its offset and depth) and
// D3DOP_SPAN (drawn on XP's driver path).
static int RunExecute() {
    IDirectDraw* dd = nullptr;
    if (FAILED(DirectDrawCreate(nullptr, &dd, nullptr))) return 1;
    HWND hwnd = MakeWindow(WS_OVERLAPPEDWINDOW, 320, 240);
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    DDSURFACEDESC sd = {};
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    sd.dwWidth = sd.dwHeight = 64;
    sd.ddpfPixelFormat.dwSize = sizeof(sd.ddpfPixelFormat);
    sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
    sd.ddpfPixelFormat.dwRGBBitCount = 32;
    sd.ddpfPixelFormat.dwRBitMask = 0xFF0000;
    sd.ddpfPixelFormat.dwGBitMask = 0xFF00;
    sd.ddpfPixelFormat.dwBBitMask = 0xFF;
    IDirectDrawSurface* rt = nullptr;
    IDirect3D* d3d = nullptr;
    IDirect3DDevice* dev = nullptr;
    IDirect3DViewport* vp = nullptr;
    IDirect3DExecuteBuffer* eb = nullptr;
    HRESULT hr = dd->CreateSurface(&sd, &rt, nullptr);
    if (SUCCEEDED(hr)) hr = dd->QueryInterface(IID_IDirect3D, reinterpret_cast<void**>(&d3d));
    if (SUCCEEDED(hr)) hr = rt->QueryInterface(IID_IDirect3DHALDevice, reinterpret_cast<void**>(&dev));
    if (SUCCEEDED(hr)) hr = d3d->CreateViewport(&vp, nullptr);
    if (SUCCEEDED(hr)) hr = dev->AddViewport(vp);
    D3DVIEWPORT vd = {};
    vd.dwSize = sizeof(vd);
    vd.dwWidth = vd.dwHeight = 64;
    vd.dvScaleX = vd.dvScaleY = 32.0f;
    vd.dvMaxX = vd.dvMaxY = 1.0f;
    vd.dvMaxZ = 1.0f;
    if (SUCCEEDED(hr)) hr = vp->SetViewport(&vd);
    D3DEXECUTEBUFFERDESC ed = {};
    ed.dwSize = sizeof(ed);
    ed.dwFlags = D3DDEB_BUFSIZE;
    ed.dwBufferSize = 1024;
    if (SUCCEEDED(hr)) hr = dev->CreateExecuteBuffer(&ed, &eb, nullptr);
    CHECK(SUCCEEDED(hr), "Direct3D 1 device, viewport and execute buffer (0x%08lx)", hr);
    if (FAILED(hr)) {
        if (eb) eb->Release();
        if (vp) vp->Release();
        if (dev) dev->Release();
        if (d3d) d3d->Release();
        if (rt) rt->Release();
        dd->Release();
        DestroyWindow(hwnd);
        return 1;
    }
    // Three pretransformed vertices (a triangle in the top-left half, z 0.5) and two for a span.
    ed.dwSize = sizeof(ed);
    eb->Lock(&ed);
    auto* base = static_cast<BYTE*>(ed.lpData);
    auto* v = reinterpret_cast<D3DTLVERTEX*>(base);
    const D3DTLVERTEX tv[5] = {{{10}, {10}, {0.5f}, {1}, {0xFFFFFFFF}, {0}, {0}, {0}},
                               {{50}, {10}, {0.5f}, {1}, {0xFFFFFFFF}, {0}, {0}, {0}},
                               {{10}, {50}, {0.5f}, {1}, {0xFFFFFFFF}, {0}, {0}, {0}},
                               {{60}, {60}, {0.5f}, {1}, {0xFF00FF00}, {0}, {0}, {0}},
                               {{61}, {60}, {0.5f}, {1}, {0xFF00FF00}, {0}, {0}, {0}}};
    memcpy(v, tv, sizeof(tv));
    BYTE* p = base + sizeof(tv);
    const DWORD instrStart = DWORD(p - base);
    auto ins = [&](BYTE op, BYTE size, WORD count) {
        D3DINSTRUCTION i = {op, size, count};
        memcpy(p, &i, sizeof(i));
        p += sizeof(i);
    };
    ins(D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
    D3DPROCESSVERTICES pv = {D3DPROCESSVERTICES_COPY, 0, 0, 5};
    memcpy(p, &pv, sizeof(pv));
    p += sizeof(pv);
    ins(D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 1);
    const DWORD triOffset = DWORD(p - base) - instrStart;
    D3DTRIANGLE t = {};
    t.v1 = 0, t.v2 = 1, t.v3 = 2;
    t.wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE;
    memcpy(p, &t, sizeof(t));
    p += sizeof(t);
    ins(D3DOP_SPAN, sizeof(D3DSPAN), 1);
    D3DSPAN sp = {2, 3};
    memcpy(p, &sp, sizeof(sp));
    p += sizeof(sp);
    ins(D3DOP_EXIT, 0, 0);
    eb->Unlock();
    D3DEXECUTEDATA xd = {};
    xd.dwSize = sizeof(xd);
    xd.dwVertexCount = 5;
    xd.dwInstructionOffset = instrStart;
    xd.dwInstructionLength = DWORD(p - base) - instrStart;
    eb->SetExecuteData(&xd);

    D3DRECT in = {20, 20, 21, 21}, out = {55, 55, 56, 56};
    hr = dev->Pick(eb, vp, 0, &in);
    DWORD n = 0;
    dev->GetPickRecords(&n, nullptr);
    D3DPICKRECORD rec[4] = {};
    DWORD room = 4;
    if (n) dev->GetPickRecords(&room, rec);
    CHECK(SUCCEEDED(hr) && n == 1 && rec[0].bOpcode == D3DOP_TRIANGLE && rec[0].dwOffset == triOffset &&
              rec[0].dvZ > 0.49f && rec[0].dvZ < 0.51f,
          "Pick inside a triangle: %lu record(s), opcode %u, offset %lu (expected %lu), z %.3f", n, rec[0].bOpcode,
          rec[0].dwOffset, triOffset, rec[0].dvZ);
    dev->Pick(eb, vp, 0, &out);
    n = 9;
    dev->GetPickRecords(&n, nullptr);
    CHECK(n == 0, "Pick outside it: %lu records", n);

    // A triangle the clip volume cuts (a vertex far left): picked in its visible part. Lit
    // vertices transformed by the default (identity) matrices; the viewport maps -1..1 to 0..64.
    {
        IDirect3DExecuteBuffer* eb2 = nullptr;
        D3DEXECUTEBUFFERDESC ed2 = {};
        ed2.dwSize = sizeof(ed2);
        ed2.dwFlags = D3DDEB_BUFSIZE;
        ed2.dwBufferSize = 512;
        if (SUCCEEDED(dev->CreateExecuteBuffer(&ed2, &eb2, nullptr)) && SUCCEEDED(eb2->Lock(&ed2))) {
            BYTE* b2 = static_cast<BYTE*>(ed2.lpData);
            const D3DLVERTEX lv[3] = {{{-3.0f}, {1.0f}, {0.5f}, 0, {0xFFFFFFFF}, {0}, {0}, {0}},
                                      {{1.0f}, {1.0f}, {0.5f}, 0, {0xFFFFFFFF}, {0}, {0}, {0}},
                                      {{1.0f}, {-1.0f}, {0.5f}, 0, {0xFFFFFFFF}, {0}, {0}, {0}}};
            memcpy(b2, lv, sizeof(lv));
            p = b2 + sizeof(lv);
            const DWORD start2 = DWORD(p - b2);
            ins(D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
            D3DPROCESSVERTICES pv2 = {D3DPROCESSVERTICES_TRANSFORM, 0, 0, 3};
            memcpy(p, &pv2, sizeof(pv2));
            p += sizeof(pv2);
            ins(D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 1);
            D3DTRIANGLE t2 = {};
            t2.v1 = 0, t2.v2 = 1, t2.v3 = 2;
            t2.wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE;
            memcpy(p, &t2, sizeof(t2));
            p += sizeof(t2);
            ins(D3DOP_EXIT, 0, 0);
            eb2->Unlock();
            D3DEXECUTEDATA xd2 = {};
            xd2.dwSize = sizeof(xd2);
            xd2.dwVertexCount = 3;
            xd2.dwInstructionOffset = start2;
            xd2.dwInstructionLength = DWORD(p - b2) - start2;
            eb2->SetExecuteData(&xd2);
            D3DRECT visible = {48, 16, 49, 17}, beyond = {8, 40, 9, 41};
            dev->Pick(eb2, vp, 0, &visible);
            n = 0;
            dev->GetPickRecords(&n, nullptr);
            room = 4;
            if (n) dev->GetPickRecords(&room, rec);
            const DWORD n2 = n;
            dev->Pick(eb2, vp, 0, &beyond);
            n = 9;
            dev->GetPickRecords(&n, nullptr);
            CHECK(n2 == 1 && rec[0].dvZ > 0.4f && rec[0].dvZ < 0.6f && n == 0,
                  "Pick a triangle the clipper cuts: %lu record(s) in its visible part (z %.3f), %lu outside it", n2,
                  rec[0].dvZ, n);
        }
        if (eb2) eb2->Release();
    }

    // The span is drawn on XP's driver path: executed unclipped, or (as here) clipped after a
    // PROCESSVERTICES COPY. (Clipped otherwise it's skipped.)
    auto pixel = [&](int x, int y) {
        DDSURFACEDESC l = {};
        l.dwSize = sizeof(l);
        DWORD c = 0;
        if (SUCCEEDED(rt->Lock(nullptr, &l, DDLOCK_WAIT, nullptr))) {
            c = reinterpret_cast<const DWORD*>(static_cast<BYTE*>(l.lpSurface) + y * l.lPitch)[x] & 0xFFFFFF;
            rt->Unlock(nullptr);
        }
        return c;
    };
    DDBLTFX fx = {};
    fx.dwSize = sizeof(fx);
    rt->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    dev->BeginScene();
    dev->Execute(eb, vp, D3DEXECUTE_CLIPPED);
    dev->EndScene();
    const DWORD clippedSpan = pixel(60, 60), tri = pixel(20, 20);
    rt->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    dev->BeginScene();
    dev->Execute(eb, vp, D3DEXECUTE_UNCLIPPED);
    dev->EndScene();
    const DWORD unclippedSpan = pixel(60, 60);
    CHECK(tri == 0xFFFFFF && clippedSpan == 0x00FF00 && unclippedSpan == 0x00FF00,
          "D3DOP_SPAN after a COPY: clipped 0x%06lx, unclipped 0x%06lx (both drawn), triangle 0x%06lx", clippedSpan,
          unclippedSpan, tri);

    eb->Release();
    vp->Release();
    dev->Release();
    d3d->Release();
    rt->Release();
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

static int RunLifetime() {
    IDirectDraw7* dd = nullptr;
    HRESULT hr = DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    if (!dd) return 1;
    HWND hwnd = MakeWindow(WS_POPUP, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    hr = dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    CHECK(SUCCEEDED(hr), "lifetime: exclusive mode -> 0x%08lx", hr);

    // Flip chain: release the front buffer while holding the back buffer (Jazz Jackrabbit 2).
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    d.dwBackBufferCount = 2;
    IDirectDrawSurface7* front = nullptr;
    hr = dd->CreateSurface(&d, &front, nullptr);
    CHECK(SUCCEEDED(hr) && front, "lifetime: flip chain -> 0x%08lx", hr);
    if (!front) return 1;
    DDSCAPS2 caps = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
    IDirectDrawSurface7* back = nullptr;
    front->GetAttachedSurface(&caps, &back);
    const ULONG backRef = back ? back->AddRef() - 1 : 0;
    if (back) back->Release();
    CHECK(backRef == 2, "lifetime: back buffer counts its own references (%lu: the root's and ours)", backRef);
    ULONG r = front->Release();
    CHECK(r == 0, "lifetime: front buffer released (refcount %lu)", r);
    IDirectDrawSurface7* front2 = nullptr;
    hr = dd->CreateSurface(&d, &front2, nullptr);
    CHECK(SUCCEEDED(hr), "lifetime: new primary while the old back buffer is still referenced -> 0x%08lx", hr);
    DDSURFACEDESC2 l = {};
    l.dwSize = sizeof(l);
    hr = back ? back->Lock(nullptr, &l, DDLOCK_WAIT, nullptr) : E_FAIL;
    CHECK(SUCCEEDED(hr), "lifetime: old back buffer still usable -> 0x%08lx", hr);
    if (SUCCEEDED(hr)) back->Unlock(nullptr);
    r = back ? back->Release() : 1;
    CHECK(r == 0, "lifetime: old back buffer released (refcount %lu)", r);
    if (front2) front2->Release();

    // Mipmaps: hold only the smallest level, release the others and the root.
    DDSURFACEDESC2 m = {};
    m.dwSize = sizeof(m);
    m.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_MIPMAPCOUNT | DDSD_PIXELFORMAT;
    m.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX | DDSCAPS_SYSTEMMEMORY;
    m.dwWidth = m.dwHeight = 16;
    m.dwMipMapCount = 3;
    m.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    m.ddpfPixelFormat.dwFlags = DDPF_RGB;
    m.ddpfPixelFormat.dwRGBBitCount = 32;
    m.ddpfPixelFormat.dwRBitMask = 0xFF0000;
    m.ddpfPixelFormat.dwGBitMask = 0xFF00;
    m.ddpfPixelFormat.dwBBitMask = 0xFF;
    IDirectDrawSurface7* tex = nullptr;
    hr = dd->CreateSurface(&m, &tex, nullptr);
    CHECK(SUCCEEDED(hr) && tex, "lifetime: mipmapped texture -> 0x%08lx", hr);
    if (tex) {
        DDSCAPS2 mc = {DDSCAPS_TEXTURE | DDSCAPS_MIPMAP, DDSCAPS2_MIPMAPSUBLEVEL, 0, {0}};
        IDirectDrawSurface7 *l1 = nullptr, *l2 = nullptr;
        tex->GetAttachedSurface(&mc, &l1);
        if (l1) l1->GetAttachedSurface(&mc, &l2);
        CHECK(l1 && l2, "lifetime: mip levels 1 and 2 found");
        if (l1) l1->Release();
        r = tex->Release();
        CHECK(r == 0, "lifetime: texture released with level 2 still referenced (refcount %lu)", r);
        DDSURFACEDESC2 ld = {};
        ld.dwSize = sizeof(ld);
        hr = l2 ? l2->Lock(nullptr, &ld, DDLOCK_WAIT, nullptr) : E_FAIL;
        CHECK(SUCCEEDED(hr) && ld.dwWidth == 4, "lifetime: level 2 still usable (0x%08lx, %lux%lu)", hr, ld.dwWidth,
              ld.dwHeight);
        if (SUCCEEDED(hr)) l2->Unlock(nullptr);
        r = l2 ? l2->Release() : 1;
        CHECK(r == 0, "lifetime: level 2 released (refcount %lu)", r);
    }

    // Writes a few rows past either end of a surface's memory (Xuan-Yuan Sword 3 EX's loose
    // sprite clipping): no crash, and the surface still frees.
    DDSURFACEDESC2 o = {};
    o.dwSize = sizeof(o);
    o.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    o.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    o.dwWidth = 640;
    o.dwHeight = 480;
    IDirectDrawSurface7* off = nullptr;
    hr = dd->CreateSurface(&o, &off, nullptr);
    CHECK(SUCCEEDED(hr) && off, "lifetime: 640x480 offscreen -> 0x%08lx", hr);
    if (off) {
        DDSURFACEDESC2 ld = {};
        ld.dwSize = sizeof(ld);
        if (SUCCEEDED(off->Lock(nullptr, &ld, DDLOCK_WAIT, nullptr))) {
            auto* p = static_cast<uint8_t*>(ld.lpSurface);
            const size_t pitch = size_t(ld.lPitch);
            memset(p + pitch * ld.dwHeight, 0x5A, pitch * 8);  // 8 rows below
            memset(p - pitch * 4, 0x5A, pitch * 4);             // 4 rows above
            off->Unlock(nullptr);
        }
        r = off->Release();
        CHECK(r == 0, "lifetime: offscreen written past its ends released (refcount %lu)", r);
    }
    r = dd->Release();
    CHECK(r == 0, "lifetime: DirectDraw released (refcount %lu)", r);
    DestroyWindow(hwnd);
    return 0;
}

// Leaving exclusive mode: DirectDraw 2+ restores the desktop mode, DirectDraw 1
// keeps the game's mode (as Windows does; Jazz Jackrabbit 2 relies on it).
static int RunCoopModes() {
    HWND hwnd = MakeWindow(WS_POPUP, 640, 480);
    const int deskW = GetSystemMetrics(SM_CXSCREEN), deskH = GetSystemMetrics(SM_CYSCREEN);
    for (int v : {1, 7}) {
        DWORD w = 0, h = 0;
        HRESULT hr;
        if (v == 1) {
            IDirectDraw* dd = nullptr;
            DirectDrawCreate(nullptr, &dd, nullptr);
            if (!dd) return 1;
            dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
            hr = dd->SetDisplayMode(640, 480, 16);
            dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
            DDSURFACEDESC d = {};
            d.dwSize = sizeof(d);
            dd->GetDisplayMode(&d);
            w = d.dwWidth;
            h = d.dwHeight;
            dd->Release();
        } else {
            IDirectDraw7* dd = nullptr;
            DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
            if (!dd) return 1;
            dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
            hr = dd->SetDisplayMode(640, 480, 16, 0, 0);
            dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            dd->GetDisplayMode(&d);
            w = d.dwWidth;
            h = d.dwHeight;
            dd->Release();
        }
        const bool keeps = v == 1;
        CHECK(SUCCEEDED(hr) && (keeps ? (w == 640 && h == 480) : (int(w) == deskW && int(h) == deskH)),
              "DirectDraw %d: exclusive -> NORMAL %s the display mode (%lux%lu)", v, keeps ? "keeps" : "restores", w, h);
    }

    // Back to exclusive mode and a new primary: refused while the old one exists (lost or
    // not, as ddraw.dll), fine once it is released (Jazz Jackrabbit 2 releases it).
    {
        IDirectDraw* dd = nullptr;
        DirectDrawCreate(nullptr, &dd, nullptr);
        if (!dd) return 1;
        DDSURFACEDESC d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
        d.dwBackBufferCount = 2;
        IDirectDrawSurface *p1 = nullptr, *p2 = nullptr;
        dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT);
        dd->SetDisplayMode(640, 480, 8);
        HRESULT hr = dd->CreateSurface(&d, &p1, nullptr);
        CHECK(hr == DD_OK, "JJ2 pattern: first primary (0x%08lx)", hr);
        IDirectDraw2* dd2 = nullptr;  // through IDirectDraw2, as JJ2: leaving restores the mode
        dd->QueryInterface(IID_IDirectDraw2, reinterpret_cast<void**>(&dd2));
        if (dd2) {
            dd2->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
            dd2->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT);
            dd2->SetDisplayMode(640, 480, 8, 0, 0);
            hr = dd->CreateSurface(&d, &p2, nullptr);
            CHECK(hr == DDERR_PRIMARYSURFACEALREADYEXISTS && !p2, "no new primary while the lost one exists (0x%08lx)", hr);
            if (p1) p1->Release();
            p1 = nullptr;
            hr = dd->CreateSurface(&d, &p2, nullptr);
            CHECK(hr == DD_OK && p2, "JJ2 pattern: a new primary once the old one is released (0x%08lx)", hr);
            if (p2) {
                hr = p2->Flip(nullptr, DDFLIP_WAIT);
                CHECK(hr == DD_OK, "JJ2 pattern: the new primary flips (0x%08lx)", hr);
            }
            dd2->Release();
        }
        if (p2) p2->Release();
        if (p1) p1->Release();
        dd->Release();
    }

    // A second DirectDraw object: exclusive mode refused while the first holds it in front
    // (as ddraw.dll); once the first is released, it can have it.
    {
        IDirectDraw *a = nullptr, *b = nullptr;
        DirectDrawCreate(nullptr, &a, nullptr);
        DirectDrawCreate(nullptr, &b, nullptr);
        if (!a || !b) return 1;
        a->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        a->SetDisplayMode(640, 480, 16);
        HRESULT hr = b->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        CHECK(hr == DDERR_EXCLUSIVEMODEALREADYSET, "second DirectDraw object: exclusive mode refused (0x%08lx)", hr);
        a->Release();
        hr = b->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        CHECK(hr == DD_OK, "...and granted once the first is released (0x%08lx)", hr);
        hr = b->SetDisplayMode(800, 600, 16);
        CHECK(hr == DD_OK, "...with its own display mode (0x%08lx)", hr);
        b->Release();
    }
    DestroyWindow(hwnd);
    return 0;
}

// ---------------------------------------------------------------- fuzz

// A surface and a model of what it should contain.
struct FuzzSurface {
    IDirectDrawSurface7* s = nullptr;
    int w = 0, h = 0, bytes = 0;
    std::vector<uint32_t> px;  // expected pixels, row-major

    void Read(std::vector<uint32_t>& out) const {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        out.assign(size_t(w) * h, 0);
        if (FAILED(s->Lock(nullptr, &d, DDLOCK_WAIT, nullptr))) return;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                memcpy(&out[size_t(y) * w + x], static_cast<uint8_t*>(d.lpSurface) + y * d.lPitch + x * bytes,
                       size_t(bytes));
        s->Unlock(nullptr);
    }
    void Write() {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        if (FAILED(s->Lock(nullptr, &d, DDLOCK_WAIT, nullptr))) return;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                memcpy(static_cast<uint8_t*>(d.lpSurface) + y * d.lPitch + x * bytes, &px[size_t(y) * w + x],
                       size_t(bytes));
        s->Unlock(nullptr);
    }
};

static uint32_t g_rng = 12345;
static uint32_t Rand() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static int RandIn(int lo, int hi) { return lo + int(Rand() % uint32_t(hi - lo + 1)); }

// Runs of key-range values and of random values, so that 16-byte chunks are
// sometimes all transparent, all opaque, or mixed.
// Bits outside the key mask (alpha, the top bit of 555) are random everywhere.
static void FuzzFill(FuzzSurface& f, uint32_t valueMask, uint32_t keyMask, uint32_t keyLo, uint32_t keyHi) {
    size_t i = 0;
    while (i < f.px.size()) {
        const int run = RandIn(1, 48);
        const bool key = Rand() % 2;
        for (int k = 0; k < run && i < f.px.size(); k++, i++) {
            uint32_t v = Rand() & valueMask;
            if (key) v = (v & ~keyMask) | (keyLo + Rand() % (keyHi - keyLo + 1));
            f.px[i] = v & valueMask;
        }
    }
    f.Write();
}

static int RunFuzz() {
    IDirectDraw7* dd = nullptr;
    HRESULT hr = DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    CHECK(SUCCEEDED(hr), "DirectDrawCreateEx -> 0x%08lx", hr);
    if (!dd) return 1;
    dd->SetCooperativeLevel(nullptr, DDSCL_NORMAL);

    struct Fmt {
        const char* name;
        DWORD bpp, flags, r, g, b;
        uint32_t keyMask;  // bits compared by color keys (no alpha / unused bits)
    };
    const Fmt fmts[] = {
        {"8-bit", 8, DDPF_RGB | DDPF_PALETTEINDEXED8, 0, 0, 0, 0xFF},
        {"16-bit 565", 16, DDPF_RGB, 0xF800, 0x07E0, 0x001F, 0xFFFF},
        {"16-bit 555", 16, DDPF_RGB, 0x7C00, 0x03E0, 0x001F, 0x7FFF},
        {"32-bit", 32, DDPF_RGB, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF},
    };
    for (const Fmt& fm : fmts) {
        const uint32_t valueMask = fm.bpp == 32 ? 0xFFFFFFFFu : ((1u << fm.bpp) - 1);
        FuzzSurface surf[2];
        const int sizes[2][2] = {{83, 41}, {97, 53}};
        for (int k = 0; k < 2; k++) {
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
            d.dwWidth = DWORD(sizes[k][0]);
            d.dwHeight = DWORD(sizes[k][1]);
            d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            d.ddpfPixelFormat.dwFlags = fm.flags;
            d.ddpfPixelFormat.dwRGBBitCount = fm.bpp;
            d.ddpfPixelFormat.dwRBitMask = fm.r;
            d.ddpfPixelFormat.dwGBitMask = fm.g;
            d.ddpfPixelFormat.dwBBitMask = fm.b;
            hr = dd->CreateSurface(&d, &surf[k].s, nullptr);
            if (FAILED(hr)) {
                CHECK(false, "%s: CreateSurface -> 0x%08lx", fm.name, hr);
                return 1;
            }
            surf[k].w = sizes[k][0];
            surf[k].h = sizes[k][1];
            surf[k].bytes = int(fm.bpp / 8);
            surf[k].px.assign(size_t(surf[k].w) * surf[k].h, 0);
        }
        int mismatches = 0, blits = 0;
        char firstBad[200] = "";
        std::vector<uint32_t> got;
        for (int iter = 0; iter < 3000; iter++) {
            // Keys: a single color, or a range (DDCKEY_COLORSPACE).
            uint32_t skLo = Rand() & fm.keyMask, skHi = skLo;
            if (Rand() % 4 == 0) skHi = std::min<uint32_t>(fm.keyMask, skLo + Rand() % 300);
            uint32_t dkLo = Rand() & fm.keyMask, dkHi = dkLo;
            if (Rand() % 3 == 0) dkHi = std::min<uint32_t>(fm.keyMask, dkLo + Rand() % 5000);
            if (iter % 40 == 0) {  // new contents now and then
                FuzzFill(surf[0], valueMask, fm.keyMask, skLo, skHi);
                FuzzFill(surf[1], valueMask, fm.keyMask, dkLo, dkHi);
            }
            const bool same = Rand() % 8 == 0;  // within one surface (overlapping copies)
            FuzzSurface& src = surf[same ? 1 : 0];
            FuzzSurface& dst = surf[1];
            DDCOLORKEY sk = {skLo, skHi}, dk = {dkLo, dkHi};
            src.s->SetColorKey(DDCKEY_SRCBLT | (skHi != skLo ? DDCKEY_COLORSPACE : 0), &sk);
            dst.s->SetColorKey(DDCKEY_DESTBLT | (dkHi != dkLo ? DDCKEY_COLORSPACE : 0), &dk);

            const int sw = RandIn(1, std::min(src.w, 70)), sh = RandIn(1, std::min(src.h, 30));
            const bool stretch = Rand() % 4 == 0;
            const int dw = stretch ? RandIn(1, std::min(dst.w, 90)) : sw, dh = stretch ? RandIn(1, dst.h) : sh;
            RECT sr = {0, 0, sw, sh};
            OffsetRect(&sr, RandIn(0, src.w - sw), RandIn(0, src.h - sh));
            RECT dr = {0, 0, dw, dh};
            OffsetRect(&dr, RandIn(0, dst.w - dw), RandIn(0, dst.h - dh));

            const int keyMode = RandIn(0, 4);  // none, src, src override, dest, src + dest
            DWORD flags = DDBLT_WAIT;
            DDBLTFX fx = {};
            fx.dwSize = sizeof(fx);
            bool useSrc = false, useDst = false;
            uint32_t eLo = skLo, eHi = skHi;
            if (keyMode == 1 || keyMode == 4) {
                flags |= DDBLT_KEYSRC;
                useSrc = true;
            }
            if (keyMode == 2) {
                // An override key is one color: the high value is ignored.
                flags |= DDBLT_KEYSRCOVERRIDE;
                fx.ddckSrcColorkey.dwColorSpaceLowValue = Rand() % 2 ? skLo : (Rand() & valueMask);
                fx.ddckSrcColorkey.dwColorSpaceHighValue = Rand();
                eLo = eHi = fx.ddckSrcColorkey.dwColorSpaceLowValue & fm.keyMask;
                useSrc = true;
            }
            if (keyMode == 3 || keyMode == 4) {
                flags |= DDBLT_KEYDEST;
                useDst = true;
            }
            const bool mirrorX = Rand() % 5 == 0, mirrorY = Rand() % 5 == 0;
            if (mirrorX || mirrorY) {
                flags |= DDBLT_DDFX;
                fx.dwDDFX = (mirrorX ? DDBLTFX_MIRRORLEFTRIGHT : 0) | (mirrorY ? DDBLTFX_MIRRORUPDOWN : 0);
            }

            // Reference, reading the source as it was before the blit.
            const std::vector<uint32_t> srcBefore = src.px;
            for (int y = dr.top; y < dr.bottom; y++) {
                int64_t ry = y - dr.top;
                if (mirrorY) ry = dh - 1 - ry;
                const int sy = int(sr.top + ry * sh / dh);
                for (int x = dr.left; x < dr.right; x++) {
                    int64_t rx = x - dr.left;
                    if (mirrorX) rx = dw - 1 - rx;
                    const int sx = int(sr.left + rx * sw / dw);
                    const uint32_t v = srcBefore[size_t(sy) * src.w + sx];
                    if (useSrc && (v & fm.keyMask) >= eLo && (v & fm.keyMask) <= eHi) continue;
                    const uint32_t d = dst.px[size_t(y) * dst.w + x] & fm.keyMask;
                    if (useDst && (d < dkLo || d > dkHi)) continue;
                    dst.px[size_t(y) * dst.w + x] = v;
                }
            }
            hr = dst.s->Blt(&dr, src.s, &sr, flags, &fx);
            blits++;
            dst.Read(got);
            if (FAILED(hr) || got != dst.px) {
                if (!mismatches)
                    sprintf(firstBad, "hr 0x%08lx, %ldx%ld -> %ldx%ld, keys %d, mirror %d%d, same %d", hr,
                            sr.right - sr.left, sr.bottom - sr.top, dr.right - dr.left, dr.bottom - dr.top, keyMode,
                            mirrorX, mirrorY, same);
                mismatches++;
                dst.px = got;  // continue from what is there
            }
        }
        CHECK(mismatches == 0, "%s: %d random blits match the reference (%d mismatches%s%s)", fm.name, blits,
              mismatches, mismatches ? "; first: " : "", firstBad);
        surf[0].s->Release();
        surf[1].s->Release();
    }
    dd->Release();
    return 0;
}

// Through the import table each time (gcc would keep the function's address in a
// register across the whole test, from before dd2d9 translated the import).
__attribute__((noinline)) static int ScreenMetric(int i) { return GetSystemMetrics(i); }

// The mouse with the display mode shown scaled (run with display=borderless): the game
// sees its own mode's coordinates, its window as its screen.
// A one-frame, 32x24, 24-bit uncompressed AVI (for the MCI video checks).
static bool WriteTinyAvi(const char* path) {
    const DWORD w = 32, h = 24, frame = w * 3 * h;
    std::vector<uint8_t> f;
    auto u32 = [&](DWORD v) { for (int i = 0; i < 4; i++) f.push_back(uint8_t(v >> (8 * i))); };
    auto u16 = [&](WORD v) { f.push_back(uint8_t(v)); f.push_back(uint8_t(v >> 8)); };
    auto tag = [&](const char* t) { f.insert(f.end(), t, t + 4); };
    auto patch = [&](size_t at, DWORD v) { for (int i = 0; i < 4; i++) f[at + i] = uint8_t(v >> (8 * i)); };
    tag("RIFF"); const size_t riff = f.size(); u32(0); tag("AVI ");
    tag("LIST"); const size_t hdrl = f.size(); u32(0); tag("hdrl");
    tag("avih"); u32(56);
    u32(100000); u32(0); u32(0); u32(0x10); u32(1); u32(0); u32(1); u32(frame); u32(w); u32(h); u32(0); u32(0); u32(0); u32(0);
    tag("LIST"); const size_t strl = f.size(); u32(0); tag("strl");
    tag("strh"); u32(56);
    tag("vids"); u32(0); u32(0); u16(0); u16(0); u32(0); u32(1); u32(10); u32(0); u32(1); u32(frame); u32(DWORD(-1)); u32(0);
    u16(0); u16(0); u16(WORD(w)); u16(WORD(h));
    tag("strf"); u32(40);
    u32(40); u32(w); u32(h); u16(1); u16(24); u32(0); u32(frame); u32(0); u32(0); u32(0); u32(0);
    patch(strl, DWORD(f.size() - strl - 4));
    patch(hdrl, DWORD(f.size() - hdrl - 4));
    tag("LIST"); const size_t movi = f.size(); u32(0); tag("movi");
    const DWORD chunkAt = DWORD(f.size() - movi - 4);
    tag("00db"); u32(frame);
    for (DWORD i = 0; i < frame; i++) f.push_back(uint8_t(i * 7));
    patch(movi, DWORD(f.size() - movi - 4));
    tag("idx1"); u32(16); tag("00db"); u32(0x10); u32(chunkAt); u32(frame);
    patch(riff, DWORD(f.size() - 8));
    FILE* out = fopen(path, "wb");
    if (!out) return false;
    const bool ok = fwrite(f.data(), 1, f.size(), out) == f.size();
    fclose(out);
    return ok;
}

static int RunMouse() {
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);  // the real screen, before
    HWND hwnd = MakeWindow(WS_POPUP, sw, sh);
    CHECK(hwnd != nullptr, "test window created");
    if (!hwnd) return 1;
    IDirectDraw7* dd = nullptr;
    HRESULT hr = DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    CHECK(SUCCEEDED(hr) && dd, "DirectDrawCreateEx");
    if (!dd) return 1;
    hr = dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    CHECK(SUCCEEDED(hr), "exclusive mode (0x%08lx)", hr);
    hr = dd->SetDisplayMode(640, 480, 32, 0, 0);
    CHECK(SUCCEEDED(hr), "SetDisplayMode 640x480 (0x%08lx)", hr);
    Pump();
    CHECK(ScreenMetric(SM_CXSCREEN) == 640 && ScreenMetric(SM_CYSCREEN) == 480, "the screen is the game's mode (%d x %d)",
          ScreenMetric(SM_CXSCREEN), ScreenMetric(SM_CYSCREEN));
    RECT cr = {};
    GetClientRect(hwnd, &cr);
    CHECK(cr.right == 640 && cr.bottom == 480, "the window's client area is the mode (%ld x %ld)", cr.right, cr.bottom);
    RECT wr = {};
    GetWindowRect(hwnd, &wr);
    CHECK(wr.left == 0 && wr.top == 0 && wr.right == 640 && wr.bottom == 480, "the window is the game's screen");
    POINT pt = {100, 50};
    ScreenToClient(hwnd, &pt);
    CHECK(pt.x == 100 && pt.y == 50, "client and screen coordinates are the same (%ld,%ld)", pt.x, pt.y);
    SetCursorPos(320, 240);
    POINT c = {};
    GetCursorPos(&c);
    CHECK(c.x == 320 && c.y == 240, "SetCursorPos/GetCursorPos in the mode's coordinates (%ld,%ld)", c.x, c.y);
    SetCursorPos(639, 479);
    GetCursorPos(&c);
    CHECK(c.x == 639 && c.y == 479, "the far corner (%ld,%ld)", c.x, c.y);
    RECT clip = {0, 0, 640, 480};
    ClipCursor(&clip);
    RECT got = {};
    GetClipCursor(&got);
    CHECK(got.left == 0 && got.top == 0 && got.right == 640 && got.bottom == 480, "ClipCursor/GetClipCursor (%ld,%ld-%ld,%ld)",
          got.left, got.top, got.right, got.bottom);
    ClipCursor(nullptr);
    // A mouse message at the real middle of the screen arrives at the mode's middle.
    g_lastMouseMove = -1;
    SendMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(sw / 2, sh / 2));
    CHECK(LOWORD(g_lastMouseMove) == 320 && HIWORD(g_lastMouseMove) == 240, "WM_MOUSEMOVE in the mode's coordinates (%d,%d)",
          LOWORD(g_lastMouseMove), HIWORD(g_lastMouseMove));
    if (WriteTinyAvi("dd2d9test.avi")) {
        // A video Windows' player draws into the window (Street Fighter Zero's movies): the
        // destination the game gives in its mode's coordinates is put where the image is, and
        // one shown without a destination gets its natural size there, scaled too.
        const double scale = std::min(double(sw) / 640, double(sh) / 480);
        const long ix = long((sw - 640 * scale) / 2), iy = long((sh - 480 * scale) / 2);
        char cmd[256], out[128] = {};
        long r[4] = {};
        snprintf(cmd, sizeof(cmd), "window v handle %lu", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(hwnd)));
        MCIERROR e = mciSendStringA("open dd2d9test.avi type avivideo alias v", nullptr, 0, nullptr);
        if (e == 0) {
            mciSendStringA(cmd, nullptr, 0, nullptr);
            mciSendStringA("put v destination at 160 120 320 240", nullptr, 0, nullptr);
            mciSendStringA("where v destination", out, sizeof(out), nullptr);
            sscanf(out, "%ld %ld %ld %ld", &r[0], &r[1], &r[2], &r[3]);
            const long ex = ix + long(160 * scale), ey = iy + long(120 * scale), ew = long(320 * scale), eh = long(240 * scale);
            CHECK(labs(r[0] - ex) <= 1 && labs(r[1] - ey) <= 1 && labs(r[2] - ew) <= 1 && labs(r[3] - eh) <= 1,
                  "MCI video: destination where the image is (%s; want %ld %ld %ld %ld)", out, ex, ey, ew, eh);
            mciSendStringA("close v", nullptr, 0, nullptr);
            mciSendStringA("open dd2d9test.avi type avivideo alias w", nullptr, 0, nullptr);
            snprintf(cmd, sizeof(cmd), "window w handle %lu", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(hwnd)));
            mciSendStringA(cmd, nullptr, 0, nullptr);
            mciSendStringA("play w wait", nullptr, 0, nullptr);
            out[0] = 0;
            mciSendStringA("where w destination", out, sizeof(out), nullptr);
            sscanf(out, "%ld %ld %ld %ld", &r[0], &r[1], &r[2], &r[3]);
            CHECK(labs(r[0] - ix) <= 1 && labs(r[1] - iy) <= 1 && labs(r[2] - long(32 * scale)) <= 1 &&
                      labs(r[3] - long(24 * scale)) <= 1,
                  "MCI video without a destination: its natural size, scaled, where the image is (%s)", out);
            mciSendStringA("close w", nullptr, 0, nullptr);
        } else {
            printf("SKIP MCI video: no AVI player (%lu)\n", static_cast<unsigned long>(e));
        }
        DeleteFileA("dd2d9test.avi");
    }
    {
        // The game sizing its window to the mode at 0,0 (Xuan-Yuan Sword 3 EX back from
        // Alt+Tab), before dd2d9 covers the screen again: the cursor is still kept on the
        // game's screen (the game draws it there unclipped).
        using SetPosFn = BOOL(WINAPI*)(int, int);
        const auto realSetCursorPos =
            reinterpret_cast<SetPosFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("user32.dll"), "SetCursorPos")));
        SetWindowPos(hwnd, nullptr, 0, 0, 640, 480, SWP_NOZORDER | SWP_NOACTIVATE);  // (not pumped: not covered yet)
        realSetCursorPos(sw - 1, sh - 1);
        POINT p = {};
        GetCursorPos(&p);
        CHECK(p.x >= 0 && p.x < 640 && p.y >= 0 && p.y < 480, "window the mode's size: the cursor on the game's screen (%ld,%ld)",
              p.x, p.y);
        for (int i = 0; i < 10; i++) {
            Pump();
            Sleep(20);
        }
    }
    {
        // Borderless: back in front after Alt+Tab, the primary isn't lost (the display never
        // left the game's mode; NFS High Stakes never restores its flip chain).
        DDSURFACEDESC2 pd = {};
        pd.dwSize = sizeof(pd);
        pd.dwFlags = DDSD_CAPS;
        pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        IDirectDrawSurface7* prim = nullptr;
        if (SUCCEEDED(dd->CreateSurface(&pd, &prim, nullptr))) {
            SendMessageA(hwnd, WM_ACTIVATEAPP, FALSE, 0);
            Pump();
            CHECK(prim->IsLost() == DDERR_SURFACELOST, "borderless: primary lost in the background");
            g_lastSize = -1;
            SendMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
            Pump();
            hr = prim->IsLost();
            CHECK(hr == DD_OK, "borderless: primary not lost back in front (0x%08lx)", hr);
            prim->Release();
            // The window covering the monitor again: the game is told the mode's size, as by
            // GetClientRect (Xuan-Yuan Sword 3 EX made its buffers the monitor's size).
            CHECK(g_lastSize == -1 || (LOWORD(g_lastSize) == 640 && HIWORD(g_lastSize) == 480),
                  "borderless: WM_SIZE back in front in the mode's size (%d,%d)", LOWORD(g_lastSize), HIWORD(g_lastSize));
            HDC screen = GetDC(nullptr);
            CHECK(GetDeviceCaps(screen, HORZRES) == 640 && GetDeviceCaps(screen, VERTRES) == 480,
                  "borderless: GDI's screen size the mode's (%dx%d)", GetDeviceCaps(screen, HORZRES),
                  GetDeviceCaps(screen, VERTRES));
            ReleaseDC(nullptr, screen);
        }
    }
    {
        // FlyingSB's Alt+Tab: out of exclusive mode while minimized, back into it (and its
        // mode) before Windows restores the window. Restored, the window shows the screen
        // (its region was once made from the icon's rectangle, hiding everything).
        ShowWindow(hwnd, SW_MINIMIZE);
        Pump();
        dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
        dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        dd->SetDisplayMode(640, 480, 32, 0, 0);
        ShowWindow(hwnd, SW_RESTORE);
        for (int i = 0; i < 10; i++) {
            Pump();
            Sleep(20);
        }
        using RectFn = BOOL(WINAPI*)(HWND, RECT*);
        const auto realWindowRect =
            reinterpret_cast<RectFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("user32.dll"), "GetWindowRect")));
        RECT w = {}, box = {};
        realWindowRect(hwnd, &w);
        const int region = GetWindowRgnBox(hwnd, &box);
        OffsetRect(&box, w.left, w.top);  // (relative to the window)
        CHECK(region == ERROR || (box.left <= 0 && box.top <= 0 && box.right >= sw && box.bottom >= sh),
              "restored after going exclusive while minimized: its region shows the screen (%d: %ld,%ld-%ld,%ld)", region,
              box.left, box.top, box.right, box.bottom);
    }
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    CHECK(ScreenMetric(SM_CXSCREEN) == sw, "the real screen again out of exclusive mode");
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

// Overlays (emulated): none offered to a windowed program; in exclusive mode (at the screen's
// own size) UpdateOverlay and its siblings, and the presenter laying them over the primary
// (read back from the screen): a YUY2 overlay stretched behind a destination key, an RGB one
// with a source key, z-order, flip chains.
static IDirectDrawSurface7* MakeOverlay(IDirectDraw7* dd, int w, int h, DWORD fourCC, DWORD extraCaps = 0, DWORD backs = 0) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | (backs ? DDSD_BACKBUFFERCOUNT : 0);
    d.dwWidth = w;
    d.dwHeight = h;
    d.dwBackBufferCount = backs;
    d.ddsCaps.dwCaps = DDSCAPS_OVERLAY | extraCaps;
    d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    if (fourCC) {
        d.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
        d.ddpfPixelFormat.dwFourCC = fourCC;
    } else {
        d.ddpfPixelFormat.dwFlags = DDPF_RGB;
        d.ddpfPixelFormat.dwRGBBitCount = 16;
        d.ddpfPixelFormat.dwRBitMask = 0xF800;
        d.ddpfPixelFormat.dwGBitMask = 0x07E0;
        d.ddpfPixelFormat.dwBBitMask = 0x001F;
    }
    IDirectDrawSurface7* s = nullptr;
    HRESULT hr = dd->CreateSurface(&d, &s, nullptr);
    CHECK(SUCCEEDED(hr), "overlay: CreateSurface %dx%d fourcc 0x%08lx -> 0x%08lx", w, h, fourCC, hr);
    return s;
}

static HRESULT WINAPI ListZ(IDirectDrawSurface7* s, DDSURFACEDESC2*, void* ctx) {
    static_cast<std::vector<IDirectDrawSurface7*>*>(ctx)->push_back(s);
    s->Release();
    return DDENUMRET_OK;
}

// A windowed video player: its window's background is the key colour (painted by GDI, which
// dd2d9's primary doesn't see), its left half painted black over it. The overlay shows only
// over the key, as on a real screen.
static LRESULT CALLBACK KeyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto paintLeft = [h](HDC dc) {
        RECT c;
        GetClientRect(h, &c);
        c.right /= 2;
        FillRect(dc, &c, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    };
    if (m == WM_PRINTCLIENT) {
        paintLeft(reinterpret_cast<HDC>(w));
        return 0;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paintLeft(dc);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static int RunOverlayWindowed() {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = KeyWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ddtestkey";
    wc.hbrBackground = CreateSolidBrush(RGB(255, 0, 255));
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "ddtestkey", "overlay", WS_POPUP, 0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    IDirectDraw7* dd = nullptr;
    DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* prim = nullptr;
    dd->CreateSurface(&d, &prim, nullptr);
    IDirectDrawSurface7* ov = MakeOverlay(dd, 32, 32, MAKEFOURCC('Y', 'U', 'Y', '2'));
    if (!prim || !ov) return 1;
    d = {};
    d.dwSize = sizeof(d);
    ov->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 16; x++) reinterpret_cast<DWORD*>(static_cast<BYTE*>(d.lpSurface) + y * d.lPitch)[x] = 0x80EB80EB;  // white
    ov->Unlock(nullptr);
    DDOVERLAYFX ofx = {};
    ofx.dwSize = sizeof(ofx);
    ofx.dckDestColorkey.dwColorSpaceLowValue = ofx.dckDestColorkey.dwColorSpaceHighValue = 0xFF00FF;  // (32-bit desktop)
    RECT r = {0, 0, 320, 240};
    CHECK(ov->UpdateOverlay(nullptr, prim, &r, DDOVER_SHOW | DDOVER_KEYDESTOVERRIDE | DDOVER_DDFX, &ofx) == DD_OK,
          "overlay (windowed): shown behind the window's key colour");
    for (int i = 0; i < 10; i++) {
        Pump();
        Sleep(100);
    }
    HDC sdc = GetDC(nullptr);
    const COLORREF left = GetPixel(sdc, 60, 120) & 0xFFFFFF, right = GetPixel(sdc, 260, 120) & 0xFFFFFF;
    ReleaseDC(nullptr, sdc);
    CHECK(right == RGB(255, 255, 255) && left != RGB(255, 255, 255),
          "overlay (windowed): over the GDI key colour only (left %06lx, right %06lx)", left, right);
    ov->Release();
    prim->Release();
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

// DDSCL_CREATEDEVICEWINDOW with DDSCL_NOWINDOWCHANGES: the focus window stays small, the
// device window covers the screen and shows the picture.
static int RunDeviceWindow() {
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND focus = MakeWindow(WS_POPUP, 100, 100);
    IDirectDraw7* dd = nullptr;
    DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    HRESULT hr = dd->SetCooperativeLevel(focus, DDSCL_SETFOCUSWINDOW);
    HRESULT hr2 = dd->SetCooperativeLevel(nullptr, DDSCL_CREATEDEVICEWINDOW | DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_NOWINDOWCHANGES);
    CHECK(SUCCEEDED(hr) && SUCCEEDED(hr2), "device window: focus window set (0x%08lx), device window made (0x%08lx)", hr, hr2);
    dd->SetDisplayMode(sw, sh, 32, 0, 0);
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* prim = nullptr;
    dd->CreateSurface(&d, &prim, nullptr);
    if (prim) {
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = 0x00FF00;
        prim->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    }
    for (int i = 0; i < 10; i++) {
        Pump();
        Sleep(100);
    }
    HWND dev = FindWindowA("DirectDrawDeviceWnd", nullptr);
    HDC sdc = GetDC(nullptr);
    const COLORREF far_ = GetPixel(sdc, sw - 10, sh - 10) & 0xFFFFFF;
    ReleaseDC(nullptr, sdc);
    RECT fr = {};
    GetWindowRect(focus, &fr);
    CHECK(dev && IsWindowVisible(dev) && far_ == RGB(0, 255, 0) && fr.right - fr.left == 100,
          "device window: shown, the picture on it over the screen (%06lx), the focus window left alone (%ld wide)", far_,
          fr.right - fr.left);
    if (prim) prim->Release();
    dd->RestoreDisplayMode();
    dd->SetCooperativeLevel(focus, DDSCL_NORMAL);
    CHECK(!IsWindow(dev), "device window: gone with normal mode");
    dd->Release();
    DestroyWindow(focus);
    return 0;
}

static int RunOverlay() {
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = MakeWindow(WS_POPUP, sw, sh);
    IDirectDraw7* dd = nullptr;
    DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    DDCAPS hal = {}, hel = {};
    hal.dwSize = hel.dwSize = sizeof(DDCAPS);
    dd->GetCaps(&hal, nullptr);
    CHECK((hal.dwCaps & DDCAPS_OVERLAY) && (hal.dwCKeyCaps & DDCKEYCAPS_DESTOVERLAY), "overlay: offered to a windowed program");
    dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    CHECK(SUCCEEDED(dd->SetDisplayMode(sw, sh, 32, 0, 0)), "overlay: exclusive at %dx%dx32", sw, sh);
    hal = {};
    hal.dwSize = sizeof(DDCAPS);
    dd->GetCaps(&hal, &hel);
    CHECK((hal.dwCaps & DDCAPS_OVERLAY) && (hal.dwCKeyCaps & DDCKEYCAPS_DESTOVERLAY) && (hal.ddsCaps.dwCaps & DDSCAPS_OVERLAY) &&
              !(hel.dwCaps & DDCAPS_OVERLAY),
          "overlay: caps 0x%08lx, key caps 0x%08lx, emulation layer 0x%08lx", hal.dwCaps, hal.dwCKeyCaps, hel.dwCaps);
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* prim = nullptr;
    dd->CreateSurface(&d, &prim, nullptr);
    if (!prim) return 1;
    DDBLTFX fx = {};
    fx.dwSize = sizeof(fx);
    RECT all = {0, 0, 320, 240}, keyArea = {40, 40, 160, 160};
    fx.dwFillColor = 0x000040;
    prim->Blt(&all, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    fx.dwFillColor = 0xFF00FF;
    prim->Blt(&keyArea, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    DDCOLORKEY key = {0xFF00FF, 0xFF00FF};
    CHECK(prim->SetColorKey(DDCKEY_DESTOVERLAY, &key) == DD_OK, "overlay: destination key on the primary");
    IDirectDrawSurface7* plain = Make32(dd, 8, 8);
    CHECK(plain->SetColorKey(DDCKEY_DESTOVERLAY, &key) == DDERR_NOTAOVERLAYSURFACE, "overlay: no destination key on an offscreen surface");

    // A: YUY2 32x32, left half white, right half black.
    IDirectDrawSurface7* a = MakeOverlay(dd, 32, 32, MAKEFOURCC('Y', 'U', 'Y', '2'));
    // B: R5G6B5 16x16, red with a green 8x8 middle (its source key).
    IDirectDrawSurface7* b = MakeOverlay(dd, 16, 16, 0);
    if (!a || !b) return 1;
    d = {};
    d.dwSize = sizeof(d);
    a->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
    CHECK(d.ddsCaps.dwCaps == (DDSCAPS_OVERLAY | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM), "overlay: caps 0x%08lx", d.ddsCaps.dwCaps);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 16; x++)
            reinterpret_cast<DWORD*>(static_cast<BYTE*>(d.lpSurface) + y * d.lPitch)[x] = x < 8 ? 0x80EB80EB : 0x80108010;
    a->Unlock(nullptr);
    d = {};
    d.dwSize = sizeof(d);
    b->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            reinterpret_cast<WORD*>(static_cast<BYTE*>(d.lpSurface) + y * d.lPitch)[x] =
                (x >= 4 && x < 12 && y >= 4 && y < 12) ? 0x07E0 : 0xF800;
    b->Unlock(nullptr);
    DDCOLORKEY green = {0x07E0, 0x07E0};
    CHECK(b->SetColorKey(DDCKEY_SRCOVERLAY, &green) == DD_OK, "overlay: source key");

    LONG x = -1, y = -1;
    CHECK(a->GetOverlayPosition(&x, &y) == DDERR_OVERLAYNOTVISIBLE && x == 0 && y == 0, "overlay: hidden, no position");
    CHECK(plain->UpdateOverlay(nullptr, prim, nullptr, DDOVER_SHOW, nullptr) == DDERR_NOTAOVERLAYSURFACE, "overlay: not an overlay");
    RECT r = {20, 20, 180, 180};
    CHECK(a->UpdateOverlay(nullptr, prim, &r, DDOVER_SHOW | DDOVER_KEYSRC, nullptr) == DDERR_INVALIDPARAMS,
          "overlay: DDOVER_KEYSRC without a source key");
    CHECK(a->UpdateOverlay(nullptr, prim, &r, DDOVER_SHOW | DDOVER_AUTOFLIP, nullptr) == DDERR_INVALIDPARAMS, "overlay: no autoflip");
    RECT bad = {300, 0, 200, 10};
    CHECK(a->UpdateOverlay(nullptr, prim, &bad, DDOVER_SHOW, nullptr) == DDERR_INVALIDRECT, "overlay: bad rectangle");
    CHECK(a->UpdateOverlay(nullptr, prim, &r, DDOVER_SHOW | DDOVER_KEYDEST, nullptr) == DD_OK, "overlay: A shown behind the destination key");
    RECT rb = {200, 40, 264, 104};
    CHECK(b->UpdateOverlay(nullptr, prim, &rb, DDOVER_SHOW | DDOVER_KEYSRC, nullptr) == DD_OK, "overlay: B shown with its source key");
    CHECK(a->GetOverlayPosition(&x, &y) == DD_OK && x == 20 && y == 20, "overlay: position %ld,%ld", x, y);
    CHECK(a->SetOverlayPosition(100000, 0) == DDERR_INVALIDPOSITION, "overlay: position off the screen");
    CHECK(a->SetOverlayPosition(10, 10) == DD_OK && a->GetOverlayPosition(&x, &y) == DD_OK && x == 10 && y == 10,
          "overlay: moved to %ld,%ld", x, y);
    a->SetOverlayPosition(20, 20);
    DDSCAPS2 caps = {};
    a->GetCaps(&caps);
    CHECK(caps.dwCaps & DDSCAPS_VISIBLE, "overlay: shown, DDSCAPS_VISIBLE");
    hal = {};
    hal.dwSize = sizeof(hal);
    dd->GetCaps(&hal, nullptr);
    CHECK(hal.dwCurrVisibleOverlays == 2, "overlay: %lu visible", hal.dwCurrVisibleOverlays);

    // Z-order: the newer in front; then A sent to the front.
    std::vector<IDirectDrawSurface7*> z;
    a->EnumOverlayZOrders(1 /* DDENUMOVERLAYZ_FRONTTOBACK */, &z, ListZ);
    CHECK(z.size() == 2 && z[0] == b && z[1] == a, "overlay: z-order B, A");
    CHECK(a->UpdateOverlayZOrder(0 /* DDOVERZ_SENDTOFRONT */, nullptr) == DD_OK, "overlay: A to the front");
    z.clear();
    b->EnumOverlayZOrders(0 /* DDENUMOVERLAYZ_BACKTOFRONT */, &z, ListZ);
    CHECK(z.size() == 2 && z[0] == b && z[1] == a, "overlay: back to front B, A");

    // On the screen.
    for (int i = 0; i < 10; i++) {
        Pump();
        Sleep(100);
    }
    HDC sdc = GetDC(nullptr);
    auto px = [&](int sx, int sy) { return GetPixel(sdc, sx, sy) & 0xFFFFFF; };
    const COLORREF blue = RGB(0, 0, 0x40), white = RGB(255, 255, 255), black = RGB(0, 0, 0), red = RGB(255, 0, 0);
    CHECK(px(30, 30) == blue, "overlay: outside the key, the primary (%06lx)", px(30, 30));
    CHECK(px(60, 100) == white, "overlay: A's left half in the key (%06lx)", px(60, 100));
    CHECK(px(150, 100) == black, "overlay: A's right half in the key (%06lx)", px(150, 100));
    CHECK(px(170, 100) == blue, "overlay: past the key, the primary (%06lx)", px(170, 100));
    CHECK(px(210, 50) == red, "overlay: B (%06lx)", px(210, 50));
    CHECK(px(232, 72) == blue, "overlay: B's keyed middle shows the primary (%06lx)", px(232, 72));
    // Mirrored left to right: A's black half on the left.
    DDOVERLAYFX ofx = {};
    ofx.dwSize = sizeof(ofx);
    ofx.dwDDFX = DDOVERFX_MIRRORLEFTRIGHT;
    CHECK(a->UpdateOverlay(nullptr, prim, &r, DDOVER_SHOW | DDOVER_KEYDEST | DDOVER_DDFX, &ofx) == DD_OK, "overlay: A mirrored");
    for (int i = 0; i < 5; i++) {
        Pump();
        Sleep(100);
    }
    CHECK(px(60, 100) == black && px(150, 100) == white, "overlay: mirrored (%06lx %06lx)", px(60, 100), px(150, 100));
    // A blit into a visible overlay shows.
    fx.dwFillColor = 0xFFE0;  // yellow
    b->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    for (int i = 0; i < 5; i++) {
        Pump();
        Sleep(100);
    }
    CHECK(px(210, 50) == RGB(255, 255, 0) && px(232, 72) == RGB(255, 255, 0), "overlay: B filled (%06lx %06lx)", px(210, 50),
          px(232, 72));
    CHECK(a->UpdateOverlay(nullptr, a, nullptr, DDOVER_SHOW, nullptr) == DDERR_INVALIDSURFACETYPE, "overlay: not on an overlay");
    CHECK(a->UpdateOverlay(nullptr, prim, nullptr, DDOVER_HIDE, nullptr) == DD_OK, "overlay: A hidden");
    for (int i = 0; i < 5; i++) {
        Pump();
        Sleep(100);
    }
    CHECK(px(60, 100) == RGB(255, 0, 255), "overlay: hidden, the key colour (%06lx)", px(60, 100));
    CHECK(a->GetOverlayPosition(&x, &y) == DDERR_OVERLAYNOTVISIBLE, "overlay: hidden again");
    ReleaseDC(nullptr, sdc);

    // A flip chain of overlays; DDFLIP_EVEN/ODD only for those.
    IDirectDrawSurface7* chain = MakeOverlay(dd, 16, 16, 0, DDSCAPS_FLIP | DDSCAPS_COMPLEX, 1);
    if (chain) {
        CHECK(chain->Flip(nullptr, DDFLIP_WAIT) == DD_OK, "overlay: flip chain flips");
        CHECK(chain->Flip(nullptr, DDFLIP_EVEN) == DD_OK, "overlay: DDFLIP_EVEN");
        chain->Release();
    }
    // System memory: no overlays (ddraw.dll's answer).
    IDirectDrawSurface7* sys = nullptr;
    d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.dwWidth = d.dwHeight = 16;
    d.ddsCaps.dwCaps = DDSCAPS_OVERLAY | DDSCAPS_SYSTEMMEMORY;
    CHECK(dd->CreateSurface(&d, &sys, nullptr) == DDERR_NOOVERLAYHW, "overlay: none in system memory");

    // The primary released first: the visible overlay still holds it, as in ddraw.dll.
    prim->Release();
    b->Release();
    a->Release();
    plain->Release();
    dd->RestoreDisplayMode();
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmd, int) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "ddtest.txt");
    g_out = fopen(path, "a");
    fprintf(g_out, "== ddtest %s\n", cmd);
    int r = 0;
    if (strstr(cmd, "pattern")) {
        int w = 640, h = 480;
        sscanf(strstr(cmd, "pattern") + 7, "%d %d", &w, &h);
        r = RunPattern(w, h);
    }
    else if (strstr(cmd, "fs8")) {
        // "fs8" runs at 640x480; "fs8 <w> <h>" uses another mode (a test display may only offer its own size)
        int w = 640, h = 480;
        sscanf(strstr(cmd, "fs8") + 3, "%d %d", &w, &h);
        r = RunFullscreen8(w, h);
    }
    else if (strstr(cmd, "mouse")) r = RunMouse();
    else if (strstr(cmd, "overlay")) r = RunOverlay() | RunOverlayWindowed() | RunDeviceWindow();
    else if (strstr(cmd, "win")) r = RunWindowed();
    else if (strstr(cmd, "fuzz")) r = RunFuzz();
    else r = RunUnit() | RunExecute() | RunLifetime() | RunCoopModes();
    fprintf(g_out, "== %d failure(s)\n", g_fail);
    fclose(g_out);
    return r | (g_fail ? 2 : 0);
}
