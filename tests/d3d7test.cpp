// Direct3D 7 test: a 3D game as D7VK sees it. Renders a triangle with D3D7,
// draws 2D on top with DirectDraw (Blt and GDI), and flips.
//   d3d7test [<w> <h> [<bpp>]]     default 640x480x32
// Results are appended to d3d7test.txt next to the executable.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <cstdio>
#include <cstring>

static FILE* g_out;
static int g_fail;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        fprintf(g_out, (cond) ? "PASS " : "FAIL ");         \
        if (!(cond)) g_fail++;                              \
        fprintf(g_out, __VA_ARGS__);                        \
        fputc('\n', g_out);                                 \
        fflush(g_out);                                      \
    } while (0)

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcA(h, m, w, l);
}

static void Pump() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

struct Vertex {
    float x, y, z, rhw;
    DWORD color;
};

static bool g_gamma = false;
static bool g_hud = true;

static int Run(int w, int h, int bpp) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "d3d7test";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "d3d7test", "D3D7 test", WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN),
                                GetSystemMetrics(SM_CYSCREEN), nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(hwnd != nullptr, "window");
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);

    IDirectDraw7* dd = nullptr;
    HRESULT hr = DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    CHECK(SUCCEEDED(hr), "DirectDrawCreateEx -> 0x%08lx", hr);
    if (!dd) return 1;
    DDCAPS caps7 = {};
    caps7.dwSize = sizeof(caps7);
    hr = dd->GetCaps(&caps7, nullptr);
    CHECK(SUCCEEDED(hr) && (caps7.dwCaps & DDCAPS_3D) && (caps7.ddsCaps.dwCaps & DDSCAPS_3DDEVICE),
          "GetCaps reports a 3D accelerator (dwCaps 0x%08lx, ddsCaps 0x%08lx)", caps7.dwCaps, caps7.ddsCaps.dwCaps);
    hr = dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_FPUSETUP);
    CHECK(SUCCEEDED(hr), "SetCooperativeLevel -> 0x%08lx", hr);
    hr = dd->SetDisplayMode(w, h, bpp, 0, 0);
    CHECK(SUCCEEDED(hr), "SetDisplayMode %dx%dx%d -> 0x%08lx", w, h, bpp, hr);

    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
    d.dwBackBufferCount = 1;
    IDirectDrawSurface7* front = nullptr;
    hr = dd->CreateSurface(&d, &front, nullptr);
    CHECK(SUCCEEDED(hr), "3D flip chain -> 0x%08lx", hr);
    if (!front) return 1;
    DDSCAPS2 caps = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
    IDirectDrawSurface7* back = nullptr;
    hr = front->GetAttachedSurface(&caps, &back);
    CHECK(SUCCEEDED(hr) && back, "back buffer -> 0x%08lx", hr);
    if (!back) return 1;

    IDirect3D7* d3d = nullptr;
    hr = dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d));
    CHECK(SUCCEEDED(hr) && d3d, "QueryInterface(IDirect3D7) -> 0x%08lx", hr);
    if (!d3d) return 1;
    IDirect3DDevice7* dev = nullptr;
    hr = d3d->CreateDevice(IID_IDirect3DHALDevice, back, &dev);
    CHECK(SUCCEEDED(hr) && dev, "CreateDevice(HAL) -> 0x%08lx", hr);
    if (!dev) return 1;
    if (g_gamma) {  // "gamma": an inverting ramp on the primary (Direct3D frames shown through it)
        IDirectDrawGammaControl* gc = nullptr;
        if (SUCCEEDED(front->QueryInterface(IID_IDirectDrawGammaControl, reinterpret_cast<void**>(&gc)))) {
            DDGAMMARAMP ramp;
            for (int i = 0; i < 256; i++) ramp.red[i] = ramp.green[i] = ramp.blue[i] = WORD((255 - i) * 257);
            hr = gc->SetGammaRamp(0, &ramp);
            CHECK(SUCCEEDED(hr), "SetGammaRamp -> 0x%08lx", hr);
            gc->Release();
        }
    }
    D3DVIEWPORT7 vp = {0, 0, DWORD(w), DWORD(h), 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
    dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);

    // Render to texture: a red clear and a green quad into a texture surface, then that
    // texture drawn onto the back buffer (twice, the second time after drawing into it
    // again: the GPU copy must follow).
    {
        DDSURFACEDESC2 t = {};
        t.dwSize = sizeof(t);
        t.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        t.dwWidth = t.dwHeight = 64;
        t.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
        IDirectDrawSurface7* tex = nullptr;
        hr = dd->CreateSurface(&t, &tex, nullptr);
        CHECK(SUCCEEDED(hr) && tex, "render-target texture -> 0x%08lx", hr);
        auto pixel = [&](int x, int y) {
            DDSURFACEDESC2 l = {};
            l.dwSize = sizeof(l);
            DWORD c = 0;
            if (SUCCEEDED(back->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
                const BYTE* row = static_cast<BYTE*>(l.lpSurface) + y * l.lPitch;
                if (l.ddpfPixelFormat.dwRGBBitCount == 16) {  // 565 to 888 (bits replicated)
                    const DWORD v = reinterpret_cast<const WORD*>(row)[x];
                    const DWORD r = v >> 11, g = (v >> 5) & 63, b = v & 31;
                    c = (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
                } else {
                    c = reinterpret_cast<const DWORD*>(row)[x] & 0xFFFFFF;
                }
                back->Unlock(nullptr);
            }
            return c;
        };
        struct TV { float x, y, z, rhw; DWORD c; float u, v; };
        for (int pass = 0; tex && pass < 2; pass++) {
            const DWORD col = pass ? 0x0000FF : 0x00FF00;
            dev->SetRenderTarget(tex, 0);
            D3DVIEWPORT7 tv = {0, 0, 64, 64, 0.0f, 1.0f};
            dev->SetViewport(&tv);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFFFF0000, 1.0f, 0);
            dev->BeginScene();
            Vertex q[4] = {{0, 0, 0.5f, 1, col | 0xFF000000}, {32, 0, 0.5f, 1, col | 0xFF000000},
                           {0, 64, 0.5f, 1, col | 0xFF000000}, {32, 64, 0.5f, 1, col | 0xFF000000}};
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, q, 4, 0);
            dev->EndScene();
            dev->SetRenderTarget(back, 0);
            D3DVIEWPORT7 bv = {0, 0, DWORD(w), DWORD(h), 0.0f, 1.0f};
            dev->SetViewport(&bv);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            dev->SetTexture(0, tex);
            dev->BeginScene();
            TV quad[4] = {{0, 0, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {128, 0, 0.5f, 1, 0xFFFFFFFF, 1, 0},
                          {0, 128, 0.5f, 1, 0xFFFFFFFF, 0, 1}, {128, 128, 0.5f, 1, 0xFFFFFFFF, 1, 1}};
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, quad, 4, 0);
            dev->EndScene();
            dev->SetTexture(0, nullptr);
            const DWORD l = pixel(16, 64), r = pixel(112, 64);
            CHECK(l == col && r == 0xFF0000, "render to texture, pass %d: left 0x%06lx, right 0x%06lx", pass + 1, l, r);
        }
        if (tex) tex->Release();

        // A 64x64 texture of one-pixel black and white squares drawn 8x8, point-filtered and
        // with no mip filter: black or white without mipmaps; grey with mipmaps=on (the GPU's
        // smaller levels average it).
        DDSURFACEDESC2 c = {};
        c.dwSize = sizeof(c);
        c.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        c.dwWidth = c.dwHeight = 64;
        c.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
        c.ddpfPixelFormat.dwSize = sizeof(c.ddpfPixelFormat);
        c.ddpfPixelFormat.dwFlags = DDPF_RGB;
        c.ddpfPixelFormat.dwRGBBitCount = 32;
        c.ddpfPixelFormat.dwRBitMask = 0xFF0000;
        c.ddpfPixelFormat.dwGBitMask = 0xFF00;
        c.ddpfPixelFormat.dwBBitMask = 0xFF;
        IDirectDrawSurface7* checker = nullptr;
        hr = dd->CreateSurface(&c, &checker, nullptr);
        CHECK(SUCCEEDED(hr) && checker, "checker texture -> 0x%08lx", hr);
        DDSURFACEDESC2 l = {};
        l.dwSize = sizeof(l);
        if (checker && SUCCEEDED(checker->Lock(nullptr, &l, DDLOCK_WAIT, nullptr))) {
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++)
                    reinterpret_cast<DWORD*>(static_cast<BYTE*>(l.lpSurface) + y * l.lPitch)[x] = ((x + y) & 1) ? 0xFFFFFF : 0;
            checker->Unlock(nullptr);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFFFF0000, 1.0f, 0);
            dev->SetTexture(0, checker);
            dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_POINT);
            dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_POINT);
            dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTFP_NONE);
            dev->BeginScene();
            TV small[4] = {{0, 0, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {8, 0, 0.5f, 1, 0xFFFFFFFF, 1, 0},
                           {0, 8, 0.5f, 1, 0xFFFFFFFF, 0, 1}, {8, 8, 0.5f, 1, 0xFFFFFFFF, 1, 1}};
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, small, 4, 0);
            dev->EndScene();
            dev->SetTexture(0, nullptr);
            const DWORD p = pixel(4, 4), g = (p >> 8) & 0xFF;
            const char* what = p == 0 || p == 0xFFFFFF ? "no mipmaps" : (g > 0x40 && g < 0xC0) ? "mipmapped (mipmaps=on)" : "?";
            CHECK(what[0] != '?', "checker texture drawn 8x smaller: 0x%06lx, %s", p, what);
            // Drawn 1:1 at whole coordinates with bilinear filtering: Direct3D's pixel centres
            // fall on texel edges (each pixel blends two texels: grey); alternatepixelcenter=on
            // puts them on texel centres (exact black and white).
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFFFF0000, 1.0f, 0);
            dev->SetTexture(0, checker);
            dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
            dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
            dev->BeginScene();
            TV one[4] = {{0, 0, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {64, 0, 0.5f, 1, 0xFFFFFFFF, 1, 0},
                         {0, 64, 0.5f, 1, 0xFFFFFFFF, 0, 1}, {64, 64, 0.5f, 1, 0xFFFFFFFF, 1, 1}};
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, one, 4, 0);
            dev->EndScene();
            dev->SetTexture(0, nullptr);
            dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_POINT);
            dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_POINT);
            const DWORD q = pixel(10, 10), qg = (q >> 8) & 0xFF;
            const char* centre = q == 0 || q == 0xFFFFFF ? "texel centres (alternatepixelcenter=on)"
                                 : (qg > 0x40 && qg < 0xC0) ? "texel edges (Direct3D's)" : "?";
            CHECK(centre[0] != '?', "checker texture drawn 1:1, bilinear: 0x%06lx, pixel centres on %s", q, centre);
        }
        if (checker) checker->Release();
    }

    int flipErrors = 0, sceneErrors = 0, gdiErrors = 0;
    const int frames = 150;
    for (int i = 0; i < frames; i++) {
        Pump();
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF102040, 1.0f, 0);
        if (SUCCEEDED(dev->BeginScene())) {
            const float cx = w * (0.3f + 0.4f * (i % 60) / 60.0f), cy = h * 0.55f, r = h * 0.3f;
            Vertex v[3] = {{cx, cy - r, 0.5f, 1.0f, 0xFFFF4040},
                           {cx + r, cy + r, 0.5f, 1.0f, 0xFF40FF40},
                           {cx - r, cy + r, 0.5f, 1.0f, 0xFF4040FF}};
            if (FAILED(dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, v, 3, 0))) sceneErrors++;
            dev->EndScene();
        } else {
            sceneErrors++;
        }
        if (!g_hud) {
            if (FAILED(front->Flip(nullptr, DDFLIP_WAIT))) flipErrors++;
            continue;
        }
        // 2D on top of the 3D frame: a DirectDraw fill and GDI text, as HUDs and menus do.
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = 0xFFFF00;
        RECT bar = {20, h - 40, 20 + (w - 40) * (i + 1) / frames, h - 20};
        back->Blt(&bar, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        HDC dc;
        if (SUCCEEDED(back->GetDC(&dc))) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            char text[64];
            sprintf(text, "D3D7 + DirectDraw  frame %d", i);
            TextOutA(dc, 20, 20, text, int(strlen(text)));
            back->ReleaseDC(dc);
        } else {
            gdiErrors++;
        }
        if (FAILED(front->Flip(nullptr, DDFLIP_WAIT))) flipErrors++;
    }
    CHECK(sceneErrors == 0, "3D rendering (%d errors)", sceneErrors);
    CHECK(gdiErrors == 0, "GetDC on the 3D back buffer (%d errors)", gdiErrors);
    CHECK(flipErrors == 0, "Flip (%d errors)", flipErrors);

    for (int i = 0; i < 200; i++) {  // hold the last frame for screenshots
        Pump();
        Sleep(10);
    }
    dev->Release();
    d3d->Release();
    back->Release();
    front->Release();
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmd, int) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "d3d7test.txt");
    g_out = fopen(path, "a");
    int w = 640, h = 480, bpp = 32;
    sscanf(cmd, "%d %d %d", &w, &h, &bpp);
    g_hud = !strstr(cmd, "3d");
    g_gamma = strstr(cmd, "gamma") != nullptr;  // "3d": no 2D on top (the frames are presented from the GPU)
    fprintf(g_out, "== d3d7test %dx%dx%d\n", w, h, bpp);
    int r = Run(w, h, bpp);
    fprintf(g_out, "== %d failure(s)\n", g_fail);
    fclose(g_out);
    return r | (g_fail ? 2 : 0);
}
