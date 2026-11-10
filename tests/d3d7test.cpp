// Direct3D 7 test: a 3D game as D7VK sees it. Renders a triangle with D3D7,
// draws 2D on top with DirectDraw (Blt and GDI), and flips.
//   d3d7test [<w> <h> [<bpp>]]     default 640x480x32
// Results are appended to d3d7test.txt next to the executable.
#define WIN32_LEAN_AND_MEAN
#include <algorithm>
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <cmath>
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
        auto pixelOf = [&](IDirectDrawSurface7* s, int x, int y) {
            DDSURFACEDESC2 l = {};
            l.dwSize = sizeof(l);
            DWORD c = 0;
            if (SUCCEEDED(s->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
                const BYTE* row = static_cast<BYTE*>(l.lpSurface) + y * l.lPitch;
                if (l.ddpfPixelFormat.dwRGBBitCount == 16) {  // 565 to 888 (bits replicated)
                    const DWORD v = reinterpret_cast<const WORD*>(row)[x];
                    const DWORD r = v >> 11, g = (v >> 5) & 63, b = v & 31;
                    c = (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
                } else {
                    c = reinterpret_cast<const DWORD*>(row)[x] & 0xFFFFFF;
                }
                s->Unlock(nullptr);
            }
            return c;
        };
        auto pixel = [&](int x, int y) { return pixelOf(back, x, y); };
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
        // Drawn into, then used colour-keyed (its red the key): the red half transparent. And a
        // mipmapped one drawn into at its top level. (Both copied on the GPU.)
        auto drawInto = [&](IDirectDrawSurface7* target) {
            dev->SetRenderTarget(target, 0);
            D3DVIEWPORT7 tv = {0, 0, 64, 64, 0.0f, 1.0f};
            dev->SetViewport(&tv);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFFFF0000, 1.0f, 0);
            dev->BeginScene();
            Vertex q[4] = {{0, 0, 0.5f, 1, 0xFF00FF00}, {32, 0, 0.5f, 1, 0xFF00FF00}, {0, 64, 0.5f, 1, 0xFF00FF00},
                           {32, 64, 0.5f, 1, 0xFF00FF00}};
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, q, 4, 0);
            dev->EndScene();
            dev->SetRenderTarget(back, 0);
            D3DVIEWPORT7 bv = {0, 0, DWORD(w), DWORD(h), 0.0f, 1.0f};
            dev->SetViewport(&bv);
        };
        auto drawWith = [&](IDirectDrawSurface7* texture) {
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            dev->SetTexture(0, texture);
            dev->BeginScene();
            TV quad[4] = {{0, 0, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {128, 0, 0.5f, 1, 0xFFFFFFFF, 1, 0},
                          {0, 128, 0.5f, 1, 0xFFFFFFFF, 0, 1}, {128, 128, 0.5f, 1, 0xFFFFFFFF, 1, 1}};
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, quad, 4, 0);
            dev->EndScene();
            dev->SetTexture(0, nullptr);
        };
        if (tex) {
            DDPIXELFORMAT pf = {};
            pf.dwSize = sizeof(pf);
            tex->GetPixelFormat(&pf);
            DDCOLORKEY red = {pf.dwRBitMask, pf.dwRBitMask};
            tex->SetColorKey(DDCKEY_SRCBLT, &red);
            drawInto(tex);
            dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, TRUE);
            drawWith(tex);
            dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE);
            tex->SetColorKey(DDCKEY_SRCBLT, nullptr);
            const DWORD l = pixel(16, 64), r = pixel(112, 64);
            CHECK(l == 0x00FF00 && r == 0, "colour-keyed render-target texture: left 0x%06lx, right 0x%06lx (keyed out)", l, r);
            // The key works on the texel alone (Wine's test_colorkey_precision): with the
            // stage's alpha from a vertex alpha of 0 and no blending, the rest is still drawn.
            tex->SetColorKey(DDCKEY_SRCBLT, &red);
            dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, TRUE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            dev->SetTexture(0, tex);
            dev->BeginScene();
            TV clear[4] = {{0, 0, 0.5f, 1, 0x00FFFFFF, 0, 0}, {128, 0, 0.5f, 1, 0x00FFFFFF, 1, 0},
                           {0, 128, 0.5f, 1, 0x00FFFFFF, 0, 1}, {128, 128, 0.5f, 1, 0x00FFFFFF, 1, 1}};
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, clear, 4, 0);
            dev->EndScene();
            dev->SetTexture(0, nullptr);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
            dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE);
            tex->SetColorKey(DDCKEY_SRCBLT, nullptr);
            const DWORD l2 = pixel(16, 64), r2 = pixel(112, 64);
            CHECK(l2 == 0x00FF00 && r2 == 0, "colour key with a vertex alpha of 0: left 0x%06lx, right 0x%06lx (keyed out)", l2, r2);
            // Stage 0's alpha from the vertices read by stage 1 (BLENDCURRENTALPHA: a splat's
            // blend weight) stays the vertices' with the key on: a weight of 1/255 shows stage
            // 1's blue, not the texture's green.
            tex->SetColorKey(DDCKEY_SRCBLT, &red);
            dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, TRUE);
            dev->SetRenderState(D3DRENDERSTATE_TEXTUREFACTOR, 0xFF0000FF);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_BLENDCURRENTALPHA);
            dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
            dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            dev->SetTexture(0, tex);
            dev->BeginScene();
            TV weight[4] = {{0, 0, 0.5f, 1, 0x01FFFFFF, 0, 0}, {128, 0, 0.5f, 1, 0x01FFFFFF, 1, 0},
                            {0, 128, 0.5f, 1, 0x01FFFFFF, 0, 1}, {128, 128, 0.5f, 1, 0x01FFFFFF, 1, 1}};
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, weight, 4, 0);
            dev->EndScene();
            dev->SetTexture(0, nullptr);
            dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            dev->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_CURRENT);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            dev->SetRenderState(D3DRENDERSTATE_TEXTUREFACTOR, 0xFFFFFFFF);
            dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE);
            tex->SetColorKey(DDCKEY_SRCBLT, nullptr);
            const DWORD l3 = pixel(16, 64);
            CHECK((l3 & 0xFF) >= 0xF0 && ((l3 >> 8) & 0xFF) < 0x10,
                  "colour key with stage 0's vertex alpha read by stage 1: left 0x%06lx (stage 1's blue)", l3);
        }
        if (tex) tex->Release();
        // A viewport larger than the render target (kept from the back buffer, S
        // §SetRenderTarget): untransformed vertices mapped by it, the target cutting what is
        // outside. A quad over the viewport's top-left quarter covers all of the 64x64 target.
        {
            IDirectDrawSurface7* small = nullptr;
            if (SUCCEEDED(dd->CreateSurface(&t, &small, nullptr))) {
                struct V { float x, y, z; DWORD c; };
                const V q[4] = {{-1.0f, 1.0f, 0.5f, 0xFF00FF00}, {-0.5f, 1.0f, 0.5f, 0xFF00FF00},
                                {-1.0f, 0.5f, 0.5f, 0xFF00FF00}, {-0.5f, 0.5f, 0.5f, 0xFF00FF00}};
                dev->SetRenderTarget(small, 0);
                dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
                dev->BeginScene();
                dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZ | D3DFVF_DIFFUSE, const_cast<V*>(q), 4, 0);
                dev->EndScene();
                dev->SetRenderTarget(back, 0);
                const DWORD a = pixelOf(small, 8, 8), b = pixelOf(small, 56, 56);
                CHECK(a == 0x00FF00 && b == 0x00FF00, "viewport larger than the render target: %06lx %06lx (all drawn)", a, b);
                small->Release();
            }
        }
        // A render target drawn into, blitted to the back buffer (copied on the GPU): as is, and
        // stretched 2x.
        {
            IDirectDrawSurface7* rt = nullptr;
            if (SUCCEEDED(dd->CreateSurface(&t, &rt, nullptr))) {
                drawInto(rt);
                dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
                RECT at = {100, 10, 164, 74}, big = {200, 10, 328, 138};
                const HRESULT h1 = back->Blt(&at, rt, nullptr, DDBLT_WAIT, nullptr);
                const HRESULT h2 = back->Blt(&big, rt, nullptr, DDBLT_WAIT, nullptr);
                const DWORD a = pixel(110, 40), b = pixel(160, 40), c = pixel(220, 70), e = pixel(320, 70), o = pixel(90, 40);
                CHECK(SUCCEEDED(h1) && SUCCEEDED(h2) && a == 0x00FF00 && b == 0xFF0000 && c == 0x00FF00 && e == 0xFF0000 && o == 0,
                      "render target blitted to the back buffer: %06lx %06lx, stretched %06lx %06lx, outside %06lx", a, b, c, e, o);
                rt->Release();
            }
        }
        {
            DDSURFACEDESC2 m = t;
            m.dwFlags |= DDSD_MIPMAPCOUNT;
            m.dwMipMapCount = 2;
            m.ddsCaps.dwCaps |= DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;
            IDirectDrawSurface7* mip = nullptr;
            hr = dd->CreateSurface(&m, &mip, nullptr);
            if (mip) {
                drawInto(mip);
                drawWith(mip);
                const DWORD l = pixel(16, 64), r = pixel(112, 64);
                CHECK(l == 0x00FF00 && r == 0xFF0000, "mipmapped render-target texture: left 0x%06lx, right 0x%06lx", l, r);
                mip->Release();
            } else {
                CHECK(false, "mipmapped render-target texture -> 0x%08lx", hr);
            }
        }

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
        checker = nullptr;
        // One-pixel vertical stripes drawn 64x8, squashed only vertically, with linear and mip
        // filtering: mipmapped (the game's levels or mipmaps=on), an isotropic filter picks a
        // level small enough for the vertical squash and blurs the stripes grey; anisotropy=8
        // and up keeps them.
        c.dwFlags |= DDSD_MIPMAPCOUNT;
        c.dwMipMapCount = 7;
        c.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX;
        hr = dd->CreateSurface(&c, &checker, nullptr);
        CHECK(SUCCEEDED(hr) && checker, "mipmapped stripes texture -> 0x%08lx", hr);
        IDirectDrawSurface7* level = checker;
        if (level) level->AddRef();
        for (int n = 64; level && n >= 1; n /= 2) {  // every level: stripes, the small ones grey
            DDSURFACEDESC2 m = {};
            m.dwSize = sizeof(m);
            if (SUCCEEDED(level->Lock(nullptr, &m, DDLOCK_WAIT, nullptr))) {
                for (int y = 0; y < n; y++)
                    for (int x = 0; x < n; x++)
                        reinterpret_cast<DWORD*>(static_cast<BYTE*>(m.lpSurface) + y * m.lPitch)[x] =
                            n == 64 ? ((x & 1) ? 0xFFFFFF : 0) : 0x808080;
                level->Unlock(nullptr);
            }
            DDSCAPS2 mc = {DDSCAPS_TEXTURE | DDSCAPS_MIPMAP, 0, 0, {0}};
            IDirectDrawSurface7* next = nullptr;
            level->GetAttachedSurface(&mc, &next);
            level->Release();
            level = next;
        }
        if (level) level->Release();
        if (checker) {
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFFFF0000, 1.0f, 0);
            dev->SetTexture(0, checker);
            dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
            dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
            dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTFP_LINEAR);
            dev->BeginScene();
            // Half a pixel left: 1:1 across, each pixel on a texel's centre (not between two).
            TV flat[4] = {{-0.5f, 0, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {63.5f, 0, 0.5f, 1, 0xFFFFFFFF, 1, 0},
                          {-0.5f, 8, 0.5f, 1, 0xFFFFFFFF, 0, 1}, {63.5f, 8, 0.5f, 1, 0xFFFFFFFF, 1, 1}};
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, flat, 4, 0);
            dev->EndScene();
            dev->SetTexture(0, nullptr);
            dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_POINT);
            dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_POINT);
            dev->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTFP_NONE);
            int lo = 255, hi = 0;
            for (int x = 20; x < 28; x++) {
                const int g = int((pixel(x, 4) >> 8) & 0xFF);
                lo = std::min(lo, g);
                hi = std::max(hi, g);
            }
            CHECK(true, "stripes squashed 8x vertically: green %d..%d, %s", lo, hi,
                  hi - lo > 0x60 ? "stripes kept (anisotropic)" : "blurred (isotropic)");
        }
        if (checker) checker->Release();

        // An 8-bit palettized texture whose palette has DDPCAPS_ALPHA (each entry's peFlags is
        // its alpha, as PopCap's SexyApp games use them: Bejeweled 2's menu). Drawn
        // alpha-blended on black, its left half (alpha 0x40) must come out dark grey and its
        // right half (alpha 0xFF) white.
        {
            D3DDEVICEDESC7 caps3d = {};
            const bool alphaPalCaps = SUCCEEDED(dev->GetCaps(&caps3d)) &&
                                      (caps3d.dpcTriCaps.dwTextureCaps & D3DPTEXTURECAPS_ALPHAPALETTE);
            DDSURFACEDESC2 pd = {};
            pd.dwSize = sizeof(pd);
            pd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            pd.dwWidth = pd.dwHeight = 64;
            pd.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
            pd.ddpfPixelFormat.dwSize = sizeof(pd.ddpfPixelFormat);
            pd.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
            pd.ddpfPixelFormat.dwRGBBitCount = 8;
            IDirectDrawSurface7* ptex = nullptr;
            IDirectDrawPalette* apal = nullptr;
            PALETTEENTRY ents[256] = {};
            ents[1] = {0xFF, 0xFF, 0xFF, 0x40};
            ents[2] = {0xFF, 0xFF, 0xFF, 0xFF};
            hr = dd->CreateSurface(&pd, &ptex, nullptr);
            const HRESULT hp = dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256 | DDPCAPS_ALPHA, ents, &apal, nullptr);
            CHECK(SUCCEEDED(hr) && ptex && SUCCEEDED(hp) && apal, "alpha-palette texture -> 0x%08lx, palette -> 0x%08lx", hr, hp);
            DDSURFACEDESC2 pl = {};
            pl.dwSize = sizeof(pl);
            HRESULT hs = E_FAIL, hl = E_FAIL;
            if (ptex && apal && SUCCEEDED(hs = ptex->SetPalette(apal))) hl = ptex->Lock(nullptr, &pl, DDLOCK_WAIT, nullptr);
            if (ptex && apal)
                CHECK(SUCCEEDED(hs) && SUCCEEDED(hl), "alpha-palette texture: SetPalette 0x%08lx, Lock 0x%08lx", hs, hl);
            if (SUCCEEDED(hl)) {
                for (int y = 0; y < 64; y++) {
                    memset(static_cast<BYTE*>(pl.lpSurface) + y * pl.lPitch, 1, 32);
                    memset(static_cast<BYTE*>(pl.lpSurface) + y * pl.lPitch + 32, 2, 32);
                }
                ptex->Unlock(nullptr);
                dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
                dev->SetTexture(0, ptex);
                dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
                dev->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
                dev->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
                dev->BeginScene();
                TV aq[4] = {{0, 0, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {128, 0, 0.5f, 1, 0xFFFFFFFF, 1, 0},
                            {0, 128, 0.5f, 1, 0xFFFFFFFF, 0, 1}, {128, 128, 0.5f, 1, 0xFFFFFFFF, 1, 1}};
                dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, aq, 4, 0);
                dev->EndScene();
                dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
                dev->SetTexture(0, nullptr);
                const DWORD dark = pixel(16, 64), light = pixel(112, 64), dg = (dark >> 8) & 0xFF;
                CHECK(!alphaPalCaps || (dg > 0x30 && dg < 0x50 && light == 0xFFFFFF),
                      "alpha-palette texture blended%s: alpha 0x40 half 0x%06lx (want about 0x404040), alpha 0xFF half 0x%06lx",
                      alphaPalCaps ? "" : " (no D3DPTEXTURECAPS_ALPHAPALETTE: not checked)", dark, light);
            }
            if (ptex) ptex->Release();
            if (apal) apal->Release();
        }

        // Eight additive passes of a faint blue (3 of 255): a 16-bit render target rounds
        // each one back to nothing; drawn in 32 bits (render32=on) they add up (24). (Wine's
        // OpenGL renderer keeps 16-bit targets at higher precision: they add up there too.)
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE);
        dev->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_ONE);
        dev->BeginScene();
        for (int i = 0; i < 8; i++) {
            Vertex f[4] = {{0, 0, 0.5f, 1, 0xFF000003}, {16, 0, 0.5f, 1, 0xFF000003},
                           {0, 16, 0.5f, 1, 0xFF000003}, {16, 16, 0.5f, 1, 0xFF000003}};
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, f, 4, 0);
        }
        dev->EndScene();
        dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
        const DWORD acc = pixel(8, 8) & 0xFF;
        CHECK(true, "eight faint additive passes: blue %lu, %s", acc,
              acc >= 0x10 ? "accumulated (the target keeps more than 16 bits)" : "rounded away (16-bit precision)");

        // A white triangle with a slanted edge on black: without antialiasing every pixel on
        // the edge is black or white; with it (antialiasing=N) some are in between. Depth
        // tested, so 3D (antialias2d=off leaves 2D draws single-sampled).
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
        dev->SetRenderState(D3DRENDERSTATE_ZENABLE, TRUE);
        dev->SetRenderState(D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
        dev->BeginScene();
        Vertex tri[3] = {{0, 0, 0.5f, 1, 0xFFFFFFFF}, {40, 0, 0.5f, 1, 0xFFFFFFFF}, {0, 31, 0.5f, 1, 0xFFFFFFFF}};
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, tri, 3, 0);
        dev->EndScene();
        int between = 0;
        for (int y = 0; y < 30; y++)
            for (int x = 0; x < 40; x++) {
                const DWORD g = (pixel(x, y) >> 8) & 0xFF;
                if (g > 0x10 && g < 0xF0) between++;
            }
        CHECK(true, "a slanted triangle edge: %d pixels in between, %s", between,
              between ? "antialiased" : "not antialiased");

        // 2D written into the back buffer between 3D draws, with no clear: the drawing after
        // it keeps it (with antialiasing, it is copied into the multisampled surface).
        DDBLTFX fill = {};
        fill.dwSize = sizeof(fill);
        fill.dwFillColor = 0xFFFFFFFF;  // white in any RGB layout
        RECT block = {100, 100, 108, 108};
        back->Blt(&block, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fill);
        dev->BeginScene();
        Vertex red[3] = {{200, 100, 0.5f, 1, 0xFFFF0000}, {240, 100, 0.5f, 1, 0xFFFF0000}, {200, 140, 0.5f, 1, 0xFFFF0000}};
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, red, 3, 0);
        dev->EndScene();
        const DWORD kept = pixel(104, 104), drawn = pixel(205, 105), triangle = pixel(8, 4);
        CHECK(kept == 0xFFFFFF && drawn == 0xFF0000 && triangle == 0xFFFFFF,
              "2D between 3D draws kept: block 0x%06lx, new triangle 0x%06lx, earlier triangle 0x%06lx", kept, drawn, triangle);

        // Transformed vertices with rhw 0 in a draw that also has valid ones (Moto Racer's menu
        // letters): hardware of the era drew them coloured; Direct3D 9 alone makes 0/0 of the
        // colour of a triangle whose rhw are all 0.
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        dev->BeginScene();
        Vertex mixed[6] = {{300, 100, 0.5f, 0, 0xFFFFFFFF}, {340, 100, 0.5f, 0, 0xFFFFFFFF}, {300, 140, 0.5f, 0, 0xFFFFFFFF},
                           {360, 100, 0.5f, 1, 0xFFFFFFFF}, {400, 100, 0.5f, 1, 0xFFFFFFFF}, {360, 140, 0.5f, 1, 0xFFFFFFFF}};
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, mixed, 6, 0);
        dev->EndScene();
        const DWORD zeroTri = pixel(305, 105), validTri = pixel(365, 105);
        CHECK(zeroTri == 0xFFFFFF && validTri == 0xFFFFFF, "rhw 0 beside valid rhw in one draw: 0x%06lx, valid 0x%06lx", zeroTri, validTri);

        // Transformed vertices past depth 1 with the depth test on (NFS Porsche Unleashed's
        // sky haze, z up to 11): the hardware of the era clamped the depth per pixel and drew
        // the whole triangle; Direct3D 9 with clipping cut it where z passed 1.
        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
        dev->SetRenderState(D3DRENDERSTATE_ZENABLE, TRUE);
        dev->SetRenderState(D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
        dev->BeginScene();
        Vertex deep[3] = {{420, 100, 0.5f, 1, 0xFFFFFFFF}, {520, 100, 3.0f, 1, 0xFFFFFFFF}, {420, 140, 0.5f, 1, 0xFFFFFFFF}};
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, deep, 3, 0);
        dev->EndScene();
        const DWORD nearPart = pixel(425, 104), farPart = pixel(500, 102);
        CHECK(nearPart == 0xFFFFFF && farPart == 0xFFFFFF, "depth past 1 clamped, not clipped: near 0x%06lx, far 0x%06lx", nearPart, farPart);

        // A 2D triangle (transformed, one depth, the depth test off): with antialiasing and
        // antialias2d off, its edge is drawn as without antialiasing (a HUD stays sharp).
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
        dev->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
        dev->BeginScene();
        Vertex flat2d[3] = {{0, 200, 0.5f, 1, 0xFFFFFFFF}, {40, 200, 0.5f, 1, 0xFFFFFFFF}, {0, 231, 0.5f, 1, 0xFFFFFFFF}};
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, flat2d, 3, 0);
        dev->EndScene();
        dev->SetRenderState(D3DRENDERSTATE_ZENABLE, TRUE);
        int between2d = 0;
        for (int y = 200; y < 230; y++)
            for (int x = 0; x < 40; x++) {
                const DWORD g = (pixel(x, y) >> 8) & 0xFF;
                if (g > 0x10 && g < 0xF0) between2d++;
            }
        CHECK(true, "a 2D triangle's edge: %d pixels in between, %s", between2d, between2d ? "antialiased" : "sharp");

        // A cut-out sprite (alpha tested) whose texture is transparent except a white column
        // at its left edge, drawn with its right edge at a fractional position: nothing may
        // show past that edge. (With antialiasing, the edge pixels' texture coordinate falls
        // just past 1, and wrapping reads the white column there unless dd2d9 clamps.)
        DDSURFACEDESC2 a = {};
        a.dwSize = sizeof(a);
        a.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        a.dwWidth = a.dwHeight = 16;
        a.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
        a.ddpfPixelFormat.dwSize = sizeof(a.ddpfPixelFormat);
        a.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
        a.ddpfPixelFormat.dwRGBBitCount = 32;
        a.ddpfPixelFormat.dwRBitMask = 0xFF0000;
        a.ddpfPixelFormat.dwGBitMask = 0xFF00;
        a.ddpfPixelFormat.dwBBitMask = 0xFF;
        a.ddpfPixelFormat.dwRGBAlphaBitMask = 0xFF000000;
        IDirectDrawSurface7* sprite = nullptr;
        hr = dd->CreateSurface(&a, &sprite, nullptr);
        DDSURFACEDESC2 sl = {};
        sl.dwSize = sizeof(sl);
        if (SUCCEEDED(hr) && sprite && SUCCEEDED(sprite->Lock(nullptr, &sl, DDLOCK_WAIT, nullptr))) {
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++)
                    reinterpret_cast<DWORD*>(static_cast<BYTE*>(sl.lpSurface) + y * sl.lPitch)[x] = x == 0 ? 0xFFFFFFFF : 0;
            sprite->Unlock(nullptr);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            dev->SetTexture(0, sprite);
            dev->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, TRUE);
            dev->SetRenderState(D3DRENDERSTATE_ALPHAREF, 0x80);
            dev->SetRenderState(D3DRENDERSTATE_ALPHAFUNC, D3DCMP_GREATER);
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_ADDRESS, D3DTADDRESS_WRAP);
            dev->BeginScene();
            TV spr[4] = {{300.3f, 100, 0.5f, 1, 0xFFFFFFFF, 0, 0}, {364.3f, 100, 0.5f, 1, 0xFFFFFFFF, 1, 0},
                         {300.3f, 164, 0.5f, 1, 0xFFFFFFFF, 0, 1}, {364.3f, 164, 0.5f, 1, 0xFFFFFFFF, 1, 1}};
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1, spr, 4, 0);
            dev->EndScene();
            dev->SetTexture(0, nullptr);
            dev->SetRenderState(D3DRENDERSTATE_ALPHATESTENABLE, FALSE);
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            int lit = 0;
            for (int y = 104; y < 160; y++)
                for (int x = 362; x < 368; x++)
                    if (pixel(x, y)) lit++;
            CHECK(lit == 0, "cut-out sprite's right edge: %d pixels lit past it (a line wrapped from the other side)", lit);
        }
        if (sprite) sprite->Release();

        // W buffering (D3DZB_USEW): a slanted floor drawn twice, first cut into an 8x8 grid
        // (red), then as two triangles (green, less-or-equal depth test), its depth linear in
        // w as games computed it. A Z buffer interpolates that differently for the two cuts
        // (they fight: red shows through); W buffering, the GPU's or dd2d9's emulation, takes
        // the depth from 1/w and the green, laid on top without writing depth, covers it all.
        DDSURFACEDESC2 zd = {};
        zd.dwSize = sizeof(zd);
        zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        zd.dwWidth = DWORD(w);
        zd.dwHeight = DWORD(h);
        zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
        zd.ddpfPixelFormat.dwSize = sizeof(zd.ddpfPixelFormat);
        zd.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
        zd.ddpfPixelFormat.dwZBufferBitDepth = 16;
        zd.ddpfPixelFormat.dwZBitMask = 0xFFFF;
        IDirectDrawSurface7* zbuf = nullptr;
        hr = dd->CreateSurface(&zd, &zbuf, nullptr);
        if (SUCCEEDED(hr) && zbuf && SUCCEEDED(back->AddAttachedSurface(zbuf))) {
            dev->SetRenderTarget(back, 0);
            D3DDEVICEDESC7 dc = {};
            dev->GetCaps(&dc);
            const bool wcap = (dc.dpcTriCaps.dwRasterCaps & D3DPRASTERCAPS_WBUFFER) != 0;
            dev->SetRenderState(D3DRENDERSTATE_ZENABLE, wcap ? D3DZB_USEW : D3DZB_TRUE);
            dev->SetRenderState(D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
            dev->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, TRUE);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
            // The floor: screen rows 200 (far, w 100) to 300 (near, w 2), 1/w linear in y.
            auto vtx = [](float fx, float fy, DWORD c) {
                const float x = 100 + 200 * fx, y = 200 + 100 * fy;
                const float rhw = 1.0f / 100 + fy * (1.0f / 2 - 1.0f / 100);
                return Vertex{x, y, (1.0f / rhw) / 200.0f, rhw, c};
            };
            dev->BeginScene();
            for (int gy = 0; gy < 8; gy++)
                for (int gx = 0; gx < 8; gx++) {
                    const float x0 = gx / 8.0f, x1 = (gx + 1) / 8.0f, y0 = gy / 8.0f, y1 = (gy + 1) / 8.0f;
                    Vertex q[4] = {vtx(x0, y0, 0xFFFF0000), vtx(x1, y0, 0xFFFF0000), vtx(x0, y1, 0xFFFF0000), vtx(x1, y1, 0xFFFF0000)};
                    dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, q, 4, 0);
                }
            // The second pass as the game's light maps: laid on top without writing depth.
            dev->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, FALSE);
            Vertex big[4] = {vtx(0, 0, 0xFF00FF00), vtx(1, 0, 0xFF00FF00), vtx(0, 1, 0xFF00FF00), vtx(1, 1, 0xFF00FF00)};
            dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, big, 4, 0);
            dev->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, TRUE);
            dev->EndScene();
            int red = 0;
            for (int y = 202; y < 298; y += 2)
                for (int x = 102; x < 298; x += 2)
                    if (pixel(x, y) & 0xFF0000) red++;
            CHECK(true, "coplanar floor cut two ways (%s): %d of 4802 sampled pixels lost to z-fighting",
                  wcap ? "W buffering" : "Z buffering", red);
            if (wcap) {
                // A vertex with a huge rhw (KOF 99 Evolution's low-life warning lines, drawn
                // first in the frame with an rhw of 13131952) is the nearest there can be and
                // nothing more: what is drawn after keeps its order. A near square (green,
                // w 50), then a far one over it (red, w 200): green stays, and the blue one
                // with the huge rhw, drawn first, stays in front of both.
                dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
                auto square = [&](float x0, float y0, float x1, float y1, float z, float rhw, DWORD c) {
                    Vertex q[4] = {{x0, y0, z, rhw, c}, {x1, y0, z, rhw, c}, {x0, y1, z, rhw, c}, {x1, y1, z, rhw, c}};
                    dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, q, 4, 0);
                };
                dev->BeginScene();
                square(400, 200, 420, 220, 0.0f, 13131952.0f, 0xFF0000FF);
                square(300, 150, 500, 250, 0.98f, 0.02f, 0xFF00FF00);
                square(300, 150, 500, 250, 0.995f, 0.005f, 0xFFFF0000);
                dev->EndScene();
                const DWORD nearPx = pixel(350, 180), hugePx = pixel(410, 210);
                CHECK((nearPx & 0x00FF00) && !(nearPx & 0xFF0000) && (hugePx & 0x0000FF) && !(hugePx & 0xFFFF00),
                      "W buffering after a vertex with a huge rhw: the near square 0x%06lx (green), the huge one 0x%06lx (blue)",
                      nearPx & 0xFFFFFF, hugePx & 0xFFFFFF);
            }
            // NFS III's light maps: everything alpha blended with depth writes on, the road a
            // grid (red), then two lights added on top (green, blue), each cut differently and
            // writing depth too. Their depths differ from the road's only by rounding (affine
            // in 1/w, as a Z buffer interpolates), which a 16-bit buffer hid and a deeper one
            // shows as a fine hatch, unless overlays get slack (overlaybias=on): all white then.
            dev->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
            auto vtx2 = [](float fx, float fy, DWORD c) {
                const float x = 100 + 200 * fx, y = 200 + 100 * fy;
                const float rhw = 1.0f / 100 + fy * (1.0f / 2 - 1.0f / 100);
                return Vertex{x, y, 1.0f - rhw, rhw, c};
            };
            auto grid = [&](int n, DWORD c) {
                for (int gy = 0; gy < n; gy++)
                    for (int gx = 0; gx < n; gx++) {
                        const float x0 = float(gx) / n, x1 = float(gx + 1) / n, y0 = float(gy) / n, y1 = float(gy + 1) / n;
                        Vertex q[4] = {vtx2(x0, y0, c), vtx2(x1, y0, c), vtx2(x0, y1, c), vtx2(x1, y1, c)};
                        dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, q, 4, 0);
                    }
            };
            dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
            dev->BeginScene();
            grid(8, 0xFFFF0000);
            dev->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_ONE);
            grid(1, 0xFF00FF00);
            grid(3, 0xFF0000FF);
            dev->EndScene();
            dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
            dev->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_ZERO);
            dev->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE);
            int red2 = 0;
            for (int y = 202; y < 298; y += 2)
                for (int x = 102; x < 298; x += 2)
                    if ((pixel(x, y) & 0xFFFF) != 0xFFFF) red2++;
            CHECK(true, "two lights added on a floor cut differently (Z buffering): %d of 4802 sampled pixels lost to z-fighting", red2);
            // The Z buffer read and written through DirectDraw in small locks (a lens flare's
            // occlusion test; dd2d9 reads back only the rectangle then) and in a whole one,
            // and drawing against what was written. Where DirectDraw can't reach the GPU's Z
            // buffer (antialiasing on, zaccess=off) the CPU copy is what DirectDraw itself wrote: noted only.
            {
                dev->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_TRUE);
                dev->SetRenderState(D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
                dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
                dev->BeginScene();
                Vertex nearQ[4] = {{300, 100, 0.25f, 1, 0xFFFFFFFF}, {340, 100, 0.25f, 1, 0xFFFFFFFF},
                                   {300, 140, 0.25f, 1, 0xFFFFFFFF}, {340, 140, 0.25f, 1, 0xFFFFFFFF}};
                dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, nearQ, 4, 0);
                dev->EndScene();
                auto zat = [&](RECT* r, int x, int y, DWORD flags) -> int {
                    DDSURFACEDESC2 l = {};
                    l.dwSize = sizeof(l);
                    if (FAILED(zbuf->Lock(r, &l, DDLOCK_WAIT | flags, nullptr))) return -1;
                    const int ox = r ? r->left : 0, oy = r ? r->top : 0;
                    const int v = reinterpret_cast<const WORD*>(static_cast<const BYTE*>(l.lpSurface) + (y - oy) * l.lPitch)[x - ox];
                    zbuf->Unlock(r);
                    return v;
                };
                RECT inQuad = {316, 116, 324, 124}, corner = {500, 300, 508, 308};
                const int zq = zat(&inQuad, 320, 120, DDLOCK_READONLY), zc = zat(&corner, 504, 304, DDLOCK_READONLY);
                const bool readable = zc == 0xFFFF;
                DDSURFACEDESC2 l = {};
                l.dwSize = sizeof(l);
                if (SUCCEEDED(zbuf->Lock(&corner, &l, DDLOCK_WAIT, nullptr))) {
                    for (int y = 0; y < 8; y++) memset(static_cast<BYTE*>(l.lpSurface) + y * l.lPitch, 0, 16);
                    zbuf->Unlock(&corner);
                }
                dev->BeginScene();
                Vertex farQ[4] = {{480, 280, 0.5f, 1, 0xFFFF0000}, {540, 280, 0.5f, 1, 0xFFFF0000},
                                  {480, 340, 0.5f, 1, 0xFFFF0000}, {540, 340, 0.5f, 1, 0xFFFF0000}};
                dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, farQ, 4, 0);
                dev->EndScene();
                const DWORD blocked = pixel(504, 304), passed = pixel(520, 320);
                const int wq = zat(nullptr, 320, 120, DDLOCK_READONLY), wf = zat(nullptr, 520, 320, DDLOCK_READONLY),
                          wz = zat(nullptr, 504, 304, DDLOCK_READONLY);
                const bool ok = std::abs(zq - 16384) <= 2 && readable && blocked == 0 && passed == 0xFF0000 &&
                                std::abs(wq - 16384) <= 2 && std::abs(wf - 32768) <= 2 && wz == 0;
                CHECK(ok || !readable, "Z buffer through DirectDraw%s: 8x8 locks read %d and %d, written 0 blocks 0x%06lx (beside 0x%06lx), whole lock reads %d, %d, %d",
                      readable ? "" : " (not readable here: antialiasing or zaccess=off)", zq, zc, blocked, passed, wq, wf, wz);
            }
            // A 2D label drawn with the depth test passing always but writing depth, then a
            // translucent panel behind it (NFS Porsche Unleashed's menu): the label's depth keeps
            // the panel off it. antialias2d=off draws such 2D without antialiasing, so it must
            // still write the depth (the labels came out dim under the panel).
            {
                dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
                dev->SetRenderState(D3DRENDERSTATE_ZENABLE, TRUE);
                dev->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE, TRUE);
                dev->SetRenderState(D3DRENDERSTATE_ZFUNC, D3DCMP_ALWAYS);
                dev->BeginScene();
                Vertex label[4] = {{540, 100, 0.1f, 1, 0xFFFFFFFF}, {580, 100, 0.1f, 1, 0xFFFFFFFF},
                                   {540, 140, 0.1f, 1, 0xFFFFFFFF}, {580, 140, 0.1f, 1, 0xFFFFFFFF}};
                dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, label, 4, 0);
                dev->SetRenderState(D3DRENDERSTATE_ZFUNC, D3DCMP_LESSEQUAL);
                dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
                dev->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_SRCALPHA);
                dev->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_INVSRCALPHA);
                // (Farther in w too: wbuffer=force takes transformed vertices' depth from it.)
                Vertex panel[4] = {{520, 90, 0.5f, 0.5f, 0x80FF0000}, {600, 90, 0.5f, 0.5f, 0x80FF0000},
                                   {520, 150, 0.5f, 0.5f, 0x80FF0000}, {600, 150, 0.5f, 0.5f, 0x80FF0000}};
                dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, panel, 4, 0);
                dev->EndScene();
                dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
                dev->SetRenderState(D3DRENDERSTATE_SRCBLEND, D3DBLEND_ONE);
                dev->SetRenderState(D3DRENDERSTATE_DESTBLEND, D3DBLEND_ZERO);
                const DWORD onLabel = pixel(560, 120), beside = pixel(530, 120);
                CHECK(onLabel == 0xFFFFFF && (beside >> 16) >= 0x70 && (beside & 0xFFFF) == 0,
                      "2D label writing depth, translucent panel behind: label 0x%06lx (white), panel 0x%06lx", onLabel, beside);
            }
            dev->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
            back->DeleteAttachedSurface(0, zbuf);
            dev->SetRenderTarget(back, 0);
        }
        if (zbuf) zbuf->Release();

        // The clip status a draw leaves (GetClipStatus once first: dd2d9 tracks it from then
        // on): a triangle with one corner past the left edge, the others inside.
        {
            D3DMATRIX id = {};
            id._11 = id._22 = id._33 = id._44 = 1.0f;
            dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &id);
            dev->SetTransform(D3DTRANSFORMSTATE_VIEW, &id);
            dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &id);
            dev->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
            dev->SetRenderState(D3DRENDERSTATE_EXTENTS, TRUE);
            D3DCLIPSTATUS cs = {};
            dev->GetClipStatus(&cs);
            cs.dwFlags = D3DCLIPSTATUS_STATUS | D3DCLIPSTATUS_EXTENTS2;
            cs.dwStatus = D3DSTATUS_DEFAULT;  // (intersection bits all set, union none)
            cs.minx = cs.miny = 1e9f;
            cs.maxx = cs.maxy = -1e9f;
            dev->SetClipStatus(&cs);
            struct UV { float x, y, z; DWORD c; };
            UV tri[3] = {{-2.0f, 0.0f, 0.5f, 0xFFFFFFFF}, {0.5f, 0.5f, 0.5f, 0xFFFFFFFF}, {0.5f, -0.5f, 0.5f, 0xFFFFFFFF}};
            dev->BeginScene();
            dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZ | D3DFVF_DIFFUSE, tri, 3, 0);
            dev->EndScene();
            dev->SetRenderState(D3DRENDERSTATE_EXTENTS, FALSE);
            D3DCLIPSTATUS got = {};
            dev->GetClipStatus(&got);
            const float hw = w * 0.5f, hh = h * 0.5f;  // (0.5, ±0.5) on the screen
            const bool ext = std::fabs(got.minx - (hw + 0.5f * hw)) < 1 && std::fabs(got.maxx - got.minx) < 1 &&
                             std::fabs(got.miny - (hh - 0.5f * hh)) < 1 && std::fabs(got.maxy - (hh + 0.5f * hh)) < 1;
            CHECK((got.dwStatus & 0xFFF) == D3DCLIP_LEFT && !((got.dwStatus >> 12) & 0xFFF) && ext,
                  "clip status of a draw: 0x%08lx (union LEFT, no intersection), extents %.1f..%.1f x %.1f..%.1f",
                  got.dwStatus, got.minx, got.maxx, got.miny, got.maxy);
            // Vertex blending and a user clip plane: one weight of 0, so the second world matrix
            // (10 to the left) places every vertex, all past the left edge and behind the plane
            // x >= 0 (in world space): union and intersection both LEFT | GEN0.
            D3DMATRIX left = id;
            left._41 = -10.0f;
            dev->SetTransform(D3DTRANSFORMSTATE_WORLD1, &left);
            dev->SetRenderState(D3DRENDERSTATE_VERTEXBLEND, D3DVBLEND_1WEIGHT);
            D3DVALUE plane[4] = {1.0f, 0.0f, 0.0f, 0.0f};
            dev->SetClipPlane(0, plane);
            dev->SetRenderState(D3DRENDERSTATE_CLIPPLANEENABLE, 1);
            cs.dwStatus = D3DSTATUS_DEFAULT;
            dev->SetClipStatus(&cs);
            struct BV { float x, y, z, b; DWORD c; };
            BV btri[3] = {{0.1f, 0.0f, 0.5f, 0.0f, 0xFFFFFFFF}, {0.5f, 0.5f, 0.5f, 0.0f, 0xFFFFFFFF},
                          {0.5f, -0.5f, 0.5f, 0.0f, 0xFFFFFFFF}};
            dev->BeginScene();
            dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZB1 | D3DFVF_DIFFUSE, btri, 3, 0);
            dev->EndScene();
            dev->SetRenderState(D3DRENDERSTATE_CLIPPLANEENABLE, 0);
            dev->SetRenderState(D3DRENDERSTATE_VERTEXBLEND, D3DVBLEND_DISABLE);
            got = {};
            dev->GetClipStatus(&got);
            const DWORD both = D3DCLIP_LEFT | D3DCLIP_GEN0;
            CHECK((got.dwStatus & 0xFFF) == both && ((got.dwStatus >> 12) & 0xFFF) == both,
                  "clip status with vertex blending and a clip plane: 0x%08lx (LEFT | GEN0 in both)", got.dwStatus);
        }

        // The 8-bit palettized texture format: listed, with D3DPTEXTURECAPS_ALPHAPALETTE, unless
        // palettizedtextures=off (N.I.C.E. 2 then loads its 16-bit textures).
        {
            char v[16] = "";
            GetPrivateProfileStringA("dd2d9", "palettizedtextures", "on", v, sizeof(v), ".\\dd2d9.ini");
            const bool want = !(!_stricmp(v, "off") || !strcmp(v, "0") || !_stricmp(v, "false") || !_stricmp(v, "no"));
            int pal8 = 0;
            struct Cb {
                static HRESULT WINAPI Fn(DDPIXELFORMAT* f, void* c) {
                    if ((f->dwFlags & DDPF_PALETTEINDEXED8) && f->dwRGBBitCount == 8) ++*static_cast<int*>(c);
                    return D3DENUMRET_OK;
                }
            };
            dev->EnumTextureFormats(Cb::Fn, &pal8);
            D3DDEVICEDESC7 dc = {};
            dev->GetCaps(&dc);
            const bool alphaPal = (dc.dpcTriCaps.dwTextureCaps & D3DPTEXTURECAPS_ALPHAPALETTE) != 0;
            CHECK(want ? pal8 == 1 : pal8 == 0 && !alphaPal,
                  "palettized textures %s: 8-bit palettized formats listed %d (want %d), ALPHAPALETTE %d", want ? "on" : "off",
                  pal8, want ? 1 : 0, alphaPal);
        }

        // Bump maps (environment-mapped bump mapping): listed with the bump-mapping operations,
        // made as textures, their format kept.
        {
            D3DDEVICEDESC7 dc = {};
            dev->GetCaps(&dc);
            struct Found { bool u8v8 = false; } found;
            struct Cb {
                static HRESULT WINAPI Fn(DDPIXELFORMAT* f, void* c) {
                    if ((f->dwFlags & DDPF_BUMPDUDV) && f->dwBumpBitCount == 16 && f->dwBumpDuBitMask == 0xFF &&
                        f->dwBumpDvBitMask == 0xFF00)
                        static_cast<Found*>(c)->u8v8 = true;
                    return D3DENUMRET_OK;
                }
            };
            dev->EnumTextureFormats(Cb::Fn, &found);
            const bool ops = (dc.dwTextureOpCaps & D3DTEXOPCAPS_BUMPENVMAP) != 0;
            bool made = false;
            if (found.u8v8) {
                DDSURFACEDESC2 b = {};
                b.dwSize = sizeof(b);
                b.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
                b.dwWidth = b.dwHeight = 16;
                b.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
                b.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
                b.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
                b.ddpfPixelFormat.dwFlags = DDPF_BUMPDUDV;
                b.ddpfPixelFormat.dwBumpBitCount = 16;
                b.ddpfPixelFormat.dwBumpDuBitMask = 0xFF;
                b.ddpfPixelFormat.dwBumpDvBitMask = 0xFF00;
                IDirectDrawSurface7* bm = nullptr;
                if (SUCCEEDED(dd->CreateSurface(&b, &bm, nullptr))) {
                    DDPIXELFORMAT pf = {};
                    pf.dwSize = sizeof(pf);
                    bm->GetPixelFormat(&pf);
                    made = (pf.dwFlags & DDPF_BUMPDUDV) && pf.dwBumpBitCount == 16 && pf.dwBumpDvBitMask == 0xFF00;
                    dev->SetTexture(1, bm);  // (uploaded at the next draw: nothing drawn here)
                    dev->SetTexture(1, nullptr);
                    bm->Release();
                }
            }
            CHECK(found.u8v8 == ops && (!found.u8v8 || made), "bump maps: listed %d (bump-mapping caps %d), made %d", found.u8v8,
                  ops, made);
        }

        // A cube map: six faces in six colours, sampled with 3-component coordinates pointing
        // at +X (red) and -Z (magenta). Where the caps offer cube maps.
        {
            D3DDEVICEDESC7 dc = {};
            dev->GetCaps(&dc);
            if (dc.dpcTriCaps.dwTextureCaps & D3DPTEXTURECAPS_CUBEMAP) {
                DDSURFACEDESC2 cd = {};
                cd.dwSize = sizeof(cd);
                cd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
                cd.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_COMPLEX;
                cd.ddsCaps.dwCaps2 = DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_ALLFACES | DDSCAPS2_TEXTUREMANAGE;
                cd.dwWidth = cd.dwHeight = 8;
                cd.ddpfPixelFormat.dwSize = sizeof(cd.ddpfPixelFormat);
                cd.ddpfPixelFormat.dwFlags = DDPF_RGB;
                cd.ddpfPixelFormat.dwRGBBitCount = 32;
                cd.ddpfPixelFormat.dwRBitMask = 0xFF0000;
                cd.ddpfPixelFormat.dwGBitMask = 0xFF00;
                cd.ddpfPixelFormat.dwBBitMask = 0xFF;
                IDirectDrawSurface7* cube = nullptr;
                hr = dd->CreateSurface(&cd, &cube, nullptr);
                static const DWORD kCol[6] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF};
                bool filled = SUCCEEDED(hr) && cube;
                for (int f = 0; filled && f < 6; f++) {
                    IDirectDrawSurface7* face = cube;
                    if (f) {
                        DDSCAPS2 fc = {0, DWORD(DDSCAPS2_CUBEMAP | (DDSCAPS2_CUBEMAP_POSITIVEX << f)), 0, {0}};
                        filled = SUCCEEDED(cube->GetAttachedSurface(&fc, &face));
                        if (!filled) break;
                    }
                    DDBLTFX ffx = {};
                    ffx.dwSize = sizeof(ffx);
                    ffx.dwFillColor = kCol[f];
                    face->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &ffx);
                    if (f) face->Release();
                }
                DWORD px = 0, nz = 0;
                if (filled) {
                    dev->SetTexture(0, cube);
                    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
                    dev->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
                    struct TV { float x, y, z, rhw; float u, v, w; };
                    const DWORD fvf = D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0);
                    dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
                    dev->BeginScene();
                    TV a[4] = {{20, 200, 0.5f, 1, 1, 0, 0}, {60, 200, 0.5f, 1, 1, 0, 0}, {20, 240, 0.5f, 1, 1, 0, 0},
                               {60, 240, 0.5f, 1, 1, 0, 0}};
                    dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, fvf, a, 4, 0);
                    TV b[4] = {{80, 200, 0.5f, 1, 0, 0, -1}, {120, 200, 0.5f, 1, 0, 0, -1}, {80, 240, 0.5f, 1, 0, 0, -1},
                               {120, 240, 0.5f, 1, 0, 0, -1}};
                    dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, fvf, b, 4, 0);
                    dev->EndScene();
                    dev->SetTexture(0, nullptr);
                    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
                    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                    px = pixel(40, 220);
                    nz = pixel(100, 220);
                }
                CHECK(filled && px == 0xFF0000 && nz == 0xFF00FF,
                      "cube map sampled: +X 0x%06lx (red), -Z 0x%06lx (magenta) (0x%08lx)", px, nz, hr);
                if (cube) cube->Release();
            }
        }

        // 2D over 3D every frame (NFS Porsche Unleashed's loading screen): drawn, flipped, the
        // back buffer locked. The lock finds the frame drawn into that buffer, and a clear
        // between the Flip and the lock.
        {
            dev->SetRenderState(D3DRENDERSTATE_ZENABLE, D3DZB_FALSE);
            static const DWORD cols[6] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF};
            int wrong = 0;
            char seen[96] = "";
            for (int f = 0; f < 6; f++) {
                dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000 | cols[f], 1.0f, 0);
                front->Flip(nullptr, DDFLIP_WAIT);
                DWORD want = f ? cols[f - 1] : 0xFFFFFFFF;  // (the first: whatever was there)
                if (f == 4) {
                    dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
                    want = 0;
                }
                const DWORD p = pixel(50, 50);
                sprintf(seen + strlen(seen), "%06lx ", p);
                if (f && p != want) wrong++;
            }
            CHECK(wrong == 0, "back buffer locked after every flip: %d of 5 frames wrong (%s)", wrong, seen);
        }

        // ProcessVertices(TRANSFORM | CLIP) into a transformed buffer, then DrawPrimitiveVB:
        // the vertices outside the frustum are kept in clip space (XP's format), and the draw
        // must clip them as XP's does. A floor triangle reaching off the screen's edges and
        // behind the camera, drawn so and drawn directly: the same picture.
        {
            D3DMATRIX id = {}, proj = {};
            id._11 = id._22 = id._33 = id._44 = 1;
            const float zn = 1, zf = 100;
            proj._11 = 1;
            proj._22 = 1;
            proj._33 = zf / (zf - zn);
            proj._34 = 1;
            proj._43 = -zn * zf / (zf - zn);
            dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &id);
            dev->SetTransform(D3DTRANSFORMSTATE_VIEW, &id);
            dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &proj);
            struct V3 { float x, y, z; DWORD c; };
            const V3 tri[3] = {{-50, -1, 0.5f, 0xFFFFFFFF}, {0, -1, 50, 0xFF00FF00}, {50, -1, 0.5f, 0xFFFF0000}};
            DWORD ref[24], got[24];
            auto sample = [&](DWORD* outPx) {
                for (int i = 0; i < 24; i++) outPx[i] = pixel(40 + (i % 6) * 110, 250 + (i / 6) * 50);
            };
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
            dev->BeginScene();
            dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_XYZ | D3DFVF_DIFFUSE, const_cast<V3*>(tri), 3, 0);
            dev->EndScene();
            sample(ref);
            D3DVERTEXBUFFERDESC vd = {};
            vd.dwSize = sizeof(vd);
            vd.dwCaps = D3DVBCAPS_SYSTEMMEMORY;
            vd.dwFVF = D3DFVF_XYZ | D3DFVF_DIFFUSE;
            vd.dwNumVertices = 3;
            IDirect3DVertexBuffer7 *src = nullptr, *dst = nullptr;
            d3d->CreateVertexBuffer(&vd, &src, 0);
            vd.dwFVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
            d3d->CreateVertexBuffer(&vd, &dst, 0);
            void* pv = nullptr;
            if (src && dst && SUCCEEDED(src->Lock(DDLOCK_WAIT, &pv, nullptr))) {
                memcpy(pv, tri, sizeof(tri));
                src->Unlock();
                hr = dst->ProcessVertices(D3DVOP_TRANSFORM | D3DVOP_CLIP, 0, 3, src, 0, dev, 0);
                dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
                dev->BeginScene();
                dev->DrawPrimitiveVB(D3DPT_TRIANGLELIST, dst, 0, 3, 0);
                dev->EndScene();
                sample(got);
                int differ = 0;
                for (int i = 0; i < 24; i++) {
                    const DWORD a = ref[i], b = got[i];
                    for (int sh = 0; sh < 24; sh += 8)
                        if (abs(int((a >> sh) & 0xFF) - int((b >> sh) & 0xFF)) > 24) {
                            differ++;
                            break;
                        }
                }
                CHECK(SUCCEEDED(hr) && differ == 0, "ProcessVertices(CLIP) then DrawPrimitiveVB: %d of 24 samples differ from the direct draw (hr 0x%08lx)",
                      differ, hr);
                // With a user clip plane (world space, x >= 0): the left half cut off.
                D3DVALUE plane[4] = {1.0f, 0.0f, 0.0f, 0.0f};
                dev->SetClipPlane(0, plane);
                dev->SetRenderState(D3DRENDERSTATE_CLIPPLANEENABLE, 1);
                hr = dst->ProcessVertices(D3DVOP_TRANSFORM | D3DVOP_CLIP, 0, 3, src, 0, dev, 0);
                dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
                dev->BeginScene();
                dev->DrawPrimitiveVB(D3DPT_TRIANGLELIST, dst, 0, 3, 0);
                dev->EndScene();
                dev->SetRenderState(D3DRENDERSTATE_CLIPPLANEENABLE, 0);
                const DWORD left = pixel(w / 2 - 40, h - 80), right = pixel(w / 2 + 40, h - 80);
                CHECK(SUCCEEDED(hr) && left == 0 && right != 0,
                      "ProcessVertices(CLIP) with a clip plane, then DrawPrimitiveVB: left 0x%06lx (cut), right 0x%06lx", left, right);
            }
            if (src) src->Release();
            if (dst) dst->Release();

            // A vertex buffer drawn from again and again: dd2d9 draws it from a Direct3D 9
            // copy. Every draw must show what the buffer holds then: unchanged, drawn from an
            // offset with indices, after the game wrote it again (Lock), written through the
            // pointer an earlier Lock gave (as XP allows), and rewritten before every draw (a
            // dynamic buffer).
            D3DMATRIX one = {};
            one._11 = one._22 = one._33 = one._44 = 1;
            dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &one);
            DWORD lighting = TRUE;
            dev->GetRenderState(D3DRENDERSTATE_LIGHTING, &lighting);
            dev->SetRenderState(D3DRENDERSTATE_LIGHTING, FALSE);
            DWORD zenable = FALSE;
            dev->GetRenderState(D3DRENDERSTATE_ZENABLE, &zenable);
            dev->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
            vd.dwCaps = 0;
            vd.dwFVF = D3DFVF_XYZ | D3DFVF_DIFFUSE;
            vd.dwNumVertices = 6;
            IDirect3DVertexBuffer7* vb = nullptr;
            d3d->CreateVertexBuffer(&vd, &vb, 0);
            // Two triangles: the first over the left of the screen, the second over the right.
            void* kept = nullptr;  // the pointer the last Lock gave
            auto fill = [&](DWORD left, DWORD right, bool lock = true) {
                const V3 v[6] = {{-1, -1, 0.5f, left}, {-1, 1, 0.5f, left}, {-0.1f, 0, 0.5f, left},
                                 {1, 1, 0.5f, right}, {1, -1, 0.5f, right}, {0.1f, 0, 0.5f, right}};
                if (lock && FAILED(vb->Lock(DDLOCK_WAIT, &kept, nullptr))) return false;
                memcpy(kept, v, sizeof(v));
                if (lock) vb->Unlock();
                return true;
            };
            auto frame = [&](bool indexed) {
                static WORD idx[3] = {0, 1, 2};
                dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
                dev->BeginScene();
                dev->DrawPrimitiveVB(D3DPT_TRIANGLELIST, vb, 0, 3, 0);
                if (indexed) dev->DrawIndexedPrimitiveVB(D3DPT_TRIANGLELIST, vb, 3, 3, idx, 3, 0);
                dev->EndScene();
                return std::make_pair(pixel(w / 8, h / 2), pixel(w - w / 8, h / 2));
            };
            if (vb && fill(0xFF00FF00, 0xFF0000FF)) {
                int wrong = 0;
                char seen[256] = "";
                auto expect = [&](std::pair<DWORD, DWORD> got, DWORD l, DWORD r) {
                    sprintf(seen + strlen(seen), "%06lx/%06lx ", got.first, got.second);
                    if (got.first != l || got.second != r) wrong++;
                };
                for (int i = 0; i < 3; i++) expect(frame(i == 2), 0x00FF00, i == 2 ? 0x0000FF : 0);
                CHECK(wrong == 0, "video-memory vertex buffer drawn 3 times, then with indices from vertex 3: %d wrong (%s)",
                      wrong, seen);
                wrong = 0;
                seen[0] = 0;
                fill(0xFFFF0000, 0xFFFFFF00);
                for (int i = 0; i < 2; i++) expect(frame(true), 0xFF0000, 0xFFFF00);
                CHECK(wrong == 0, "the vertex buffer written again (Lock), drawn twice: %d wrong (%s)", wrong, seen);
                wrong = 0;
                seen[0] = 0;
                fill(0xFF0000FF, 0xFF00FF00, false);
                for (int i = 0; i < 2; i++) expect(frame(true), 0x0000FF, 0x00FF00);
                CHECK(wrong == 0, "the vertex buffer written through an earlier Lock's pointer, drawn twice: %d wrong (%s)",
                      wrong, seen);
                wrong = 0;
                seen[0] = 0;
                for (int i = 0; i < 8; i++) {
                    const DWORD c = i & 1 ? 0xFF00FFFF : 0xFFFF00FF;
                    fill(c, c);
                    expect(frame(true), c & 0xFFFFFF, c & 0xFFFFFF);
                    expect(frame(true), c & 0xFFFFFF, c & 0xFFFFFF);
                }
                CHECK(wrong == 0, "the vertex buffer rewritten before every other draw, 8 times: %d wrong", wrong);
            } else {
                CHECK(false, "video-memory vertex buffer");
            }
            if (vb) vb->Release();
            dev->SetRenderState(D3DRENDERSTATE_LIGHTING, lighting);
            dev->SetRenderState(D3DRENDERSTATE_ZENABLE, zenable);
            dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &proj);
        }
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
