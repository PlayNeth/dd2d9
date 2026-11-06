// Focus test: an exclusive-mode game switched away from and back from outside (Alt+Tab).
// A 640x480x16 flip chain animated for 60 seconds; each second, focustest.txt gets the frames
// flipped and whether the window is minimized. Restored, the surfaces are restored and the
// animation goes on.
//   focustest [paintfg]   paintfg: the window takes the foreground back in its WM_PAINT, as
//                         Need for Speed III does while fullscreen; switched away, it must
//                         stay away (minimized until brought back).
#include <windows.h>
#include <ddraw.h>
#include <cstdio>
#include <cstring>
static bool g_paintfg;
static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT && g_paintfg) {
        PAINTSTRUCT ps;
        SetForegroundWindow(h);
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmd, int) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "focustest.txt");
    FILE* out = fopen(path, "w");
    if (!out) return 1;
    g_paintfg = strstr(cmd, "paintfg") != nullptr;
    WNDCLASSA wc = {};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = "game";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "game", "focus test", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, nullptr, nullptr, inst,
                                nullptr);
    SetForegroundWindow(hwnd);
    IDirectDraw* dd = nullptr;
    if (FAILED(DirectDrawCreate(nullptr, &dd, nullptr)) ||
        FAILED(dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN)) ||
        FAILED(dd->SetDisplayMode(640, 480, 16))) {
        fprintf(out, "setup failed\n");
        return 1;
    }
    DDSURFACEDESC sd = {};
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    sd.dwBackBufferCount = 1;
    IDirectDrawSurface* primary = nullptr;
    IDirectDrawSurface* back = nullptr;
    DDSCAPS caps = {DDSCAPS_BACKBUFFER};
    if (FAILED(dd->CreateSurface(&sd, &primary, nullptr)) || FAILED(primary->GetAttachedSurface(&caps, &back))) {
        fprintf(out, "surfaces failed\n");
        return 1;
    }
    const DWORD start = GetTickCount();
    DWORD second = start, frames = 0, restores = 0;
    bool quit = false;
    for (int f = 0; GetTickCount() - start < 60000 && !quit && IsWindow(hwnd); f++) {
        MSG m;
        while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT) quit = true;
            DispatchMessageA(&m);
        }
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = DWORD(f * 2654435761u) & 0xffff;
        HRESULT hr = back->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        if (hr == DDERR_SURFACELOST) {
            if (SUCCEEDED(primary->Restore())) restores++;
            Sleep(10);
        } else {
            hr = primary->Flip(nullptr, DDFLIP_WAIT);
            if (SUCCEEDED(hr)) frames++;
            else Sleep(10);
        }
        if (GetTickCount() - second >= 1000) {
            fprintf(out, "frames %lu iconic %d restores %lu\n", frames, IsIconic(hwnd), restores);
            fflush(out);
            second = GetTickCount();
            frames = 0;
        }
    }
    back->Release();
    primary->Release();
    dd->RestoreDisplayMode();
    dd->Release();
    fprintf(out, "done\n");
    fclose(out);
    return 0;
}
