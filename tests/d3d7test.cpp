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
    D3DVIEWPORT7 vp = {0, 0, DWORD(w), DWORD(h), 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
    dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);

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
    fprintf(g_out, "== d3d7test %dx%dx%d\n", w, h, bpp);
    int r = Run(w, h, bpp);
    fprintf(g_out, "== %d failure(s)\n", g_fail);
    fclose(g_out);
    return r | (g_fail ? 2 : 0);
}
