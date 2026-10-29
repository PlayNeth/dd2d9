// DD2D9 test program.
//   ddtest unit     - pixel-exact checks of blits, keys, conversion, GDI, flipping, surface lifetimes
//   ddtest fs8      - exclusive 640x480x8 flip-chain animation with palette and GDI text
//   ddtest win      - windowed (DDSCL_NORMAL) 32-bit desktop, blits to the primary through a clipper
//   ddtest fuzz     - random same-format blits (keys, ranges, mirroring, stretching, overlap)
//                     compared with a reference implementation, for 8/16/32-bit formats
// Results are printed to ddtest.txt next to the executable.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>

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

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_USER + 1) g_userMsgs++;
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
        // Alone (as ddraw.dll), no Direct3D is offered, so no 3D support may be reported.
        DDCAPS caps = {};
        caps.dwSize = sizeof(caps);
        hr = dd->GetCaps(&caps, nullptr);
        CHECK(SUCCEEDED(hr) && !(caps.dwCaps & DDCAPS_3D) && !(caps.ddsCaps.dwCaps & DDSCAPS_3DDEVICE),
              "GetCaps reports no 3D support on its own (dwCaps 0x%08lx)", caps.dwCaps);
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
            yuy->Unlock(nullptr);
            yuy->Release();
        } else {
            CHECK(false, "YUY2 surface -> 0x%08lx", hr);
        }
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
        for (int y = 0; y < 480; y++) memset(static_cast<BYTE*>(d.lpSurface) + y * d.lPitch, (y / 2) & 0x7F, 640);
        back->Unlock(nullptr);
        for (int k = 0; k < 6; k++) {
            int x = (i * (3 + k) + k * 90) % 576, y = 60 + k * 60 + ((i * 2) % 40);
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
    SendMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
    Pump();
    CHECK(!IsIconic(hwnd), "window restored after focus returns (was minimized: %d)", minimized);
    // The surfaces were lost meanwhile, as in ddraw.dll: the game restores them.
    hr = front->Flip(nullptr, DDFLIP_WAIT);
    CHECK(hr == DDERR_SURFACELOST, "flip after focus returns: surface lost (0x%08lx)", hr);
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

    // Jazz Jackrabbit 2: back to exclusive mode and a new primary, the old one not released.
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
            CHECK(hr == DD_OK && p2, "JJ2 pattern: new primary while the lost one exists (0x%08lx)", hr);
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

    // Type Wild: a second DirectDraw object takes exclusive mode, the first not released.
    {
        IDirectDraw *a = nullptr, *b = nullptr;
        DirectDrawCreate(nullptr, &a, nullptr);
        DirectDrawCreate(nullptr, &b, nullptr);
        if (!a || !b) return 1;
        a->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        a->SetDisplayMode(640, 480, 16);
        HRESULT hr = b->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        CHECK(hr == DD_OK, "second DirectDraw object takes exclusive mode (0x%08lx)", hr);
        hr = b->SetDisplayMode(800, 600, 16);
        CHECK(hr == DD_OK, "...and sets the display mode (0x%08lx)", hr);
        hr = a->SetDisplayMode(640, 480, 16);
        CHECK(hr == DDERR_NOEXCLUSIVEMODE, "...which the first no longer can (0x%08lx)", hr);
        a->Release();
        DDSURFACEDESC d = {};
        d.dwSize = sizeof(d);
        b->GetDisplayMode(&d);
        CHECK(d.dwWidth == 800 && d.dwHeight == 600, "releasing the first leaves the second's mode (%lux%lu)",
              d.dwWidth, d.dwHeight);
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

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmd, int) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "ddtest.txt");
    g_out = fopen(path, "a");
    fprintf(g_out, "== ddtest %s\n", cmd);
    int r = 0;
    if (strstr(cmd, "fs8")) {
        // "fs8" runs at 640x480; "fs8 <w> <h>" uses another mode (a test display may only offer its own size)
        int w = 640, h = 480;
        sscanf(strstr(cmd, "fs8") + 3, "%d %d", &w, &h);
        r = RunFullscreen8(w, h);
    }
    else if (strstr(cmd, "win")) r = RunWindowed();
    else if (strstr(cmd, "fuzz")) r = RunFuzz();
    else r = RunUnit() | RunLifetime() | RunCoopModes();
    fprintf(g_out, "== %d failure(s)\n", g_fail);
    fclose(g_out);
    return r | (g_fail ? 2 : 0);
}
