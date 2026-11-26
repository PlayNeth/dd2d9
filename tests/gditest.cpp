// GDI fullscreen test (gdifullscreen=, loader/winmm.cpp): a game that draws with GDI alone and
// goes fullscreen with ChangeDisplaySettings, as Pajama Sam 2 does. Run from a folder holding
// dd2d9's winmm.dll and ddraw.dll (under Wine: winmm=n,b and ddraw=n,b). Writes gditest.txt;
// exits 0 if every check passed.
//
// Like the game: it links winmm (which brings dd2d9 in through the loader) and no ddraw, makes
// a borderless popup of 640x480, looks ChangeDisplaySettingsA up at run time, tests and sets
// 640x480, and copies an 8-bit DIB section to its window with BitBlt and StretchBlt, a DIB
// with palette indices with StretchDIBits, and a WinG bitmap with WinGStretchBlt (each of the
// game's DisplayAPI= ways).
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

FILE* g_out;
// (ddraw.h's, the test not including it)
constexpr DWORD DDSCL_FULLSCREEN_ = 0x1, DDSCL_NORMAL_ = 0x8, DDSCL_EXCLUSIVE_ = 0x10;
int g_fail;

void Check(bool ok, const char* what) {
    fprintf(g_out, "%s %s\n", ok ? "PASS" : "FAIL", what);
    fflush(g_out);
    if (!ok) g_fail++;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

void Pump(DWORD ms) {
    const DWORD end = GetTickCount() + ms;
    do {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        Sleep(10);
    } while (int(end - GetTickCount()) > 0);
}

// The colour on the real screen at a point, read through GDI (the frame dd2d9 presented).
COLORREF ScreenAt(int x, int y) {
    HDC s = GetDC(nullptr);
    const COLORREF c = GetPixel(s, x, y);
    ReleaseDC(nullptr, s);
    return c;
}

bool Near(COLORREF a, COLORREF b) {
    return abs(int(GetRValue(a)) - int(GetRValue(b))) <= 8 && abs(int(GetGValue(a)) - int(GetGValue(b))) <= 8 &&
           abs(int(GetBValue(a)) - int(GetBValue(b))) <= 8;
}

// A function's address from a module's export table, read directly: GetProcAddress would give
// dd2d9's hook, as the game gets it.
void* RealExport(HMODULE m, const char* name) {
    if (!m) return nullptr;
    const auto* base = reinterpret_cast<const BYTE*>(m);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return nullptr;
    const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + ed->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + ed->AddressOfNameOrdinals);
    const auto* functions = reinterpret_cast<const DWORD*>(base + ed->AddressOfFunctions);
    for (DWORD i = 0; i < ed->NumberOfNames; i++) {
        if (strcmp(reinterpret_cast<const char*>(base + names[i]), name) != 0) continue;
        const DWORD rva = functions[ordinals[i]];
        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) return nullptr;  // (forwarded)
        return const_cast<BYTE*>(base + rva);
    }
    return nullptr;
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    g_out = fopen("gditest.txt", "w");
    if (!g_out) return 2;
    timeGetTime();  // (winmm imported, as the game does)
    Check(GetModuleHandleA("ddraw.dll") != nullptr, "dd2d9 loaded by the loader (winmm.dll)");

    // The real screen's size, before any mode: read by the game too.
    DEVMODEA desk = {};
    desk.dmSize = sizeof(desk);
    EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &desk);
    const int realW = int(desk.dmPelsWidth), realH = int(desk.dmPelsHeight);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = "dd2d9gditest";
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "gditest", WS_POPUP | WS_VISIBLE, (realW - 640) / 2,
                                (realH - 480) / 2, 640, 480, nullptr, nullptr, inst, nullptr);
    SetForegroundWindow(hwnd);
    Pump(200);

    using CdsFn = LONG(WINAPI*)(DEVMODEA*, DWORD);
    const auto cds = reinterpret_cast<CdsFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("USER32.DLL"), "ChangeDisplaySettingsA")));
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    dm.dmPelsWidth = 640;
    dm.dmPelsHeight = 480;
    Check(cds && cds(&dm, CDS_TEST) == DISP_CHANGE_SUCCESSFUL, "ChangeDisplaySettingsA(640x480, CDS_TEST): successful");
    Check(cds && cds(&dm, 0) == DISP_CHANGE_SUCCESSFUL, "ChangeDisplaySettingsA(640x480): successful");
    SetWindowPos(hwnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), 0);
    Pump(300);
    char line[160];
    snprintf(line, sizeof(line), "the screen 640x480 to the game (GetSystemMetrics %dx%d)", GetSystemMetrics(SM_CXSCREEN),
             GetSystemMetrics(SM_CYSCREEN));
    Check(GetSystemMetrics(SM_CXSCREEN) == 640 && GetSystemMetrics(SM_CYSCREEN) == 480, line);
    RECT client = {};
    GetClientRect(hwnd, &client);
    snprintf(line, sizeof(line), "the window's client area 640x480 to the game (%ldx%ld)", client.right, client.bottom);
    Check(client.right == 640 && client.bottom == 480, line);

    // An 8-bit DIB section: the left half red (index 1), the right half green (index 2), a
    // blue 64x64 square at its top left (index 3), as the game draws its picture.
    struct {
        BITMAPINFOHEADER h;
        RGBQUAD colors[256];
    } bi = {};
    bi.h.biSize = sizeof(bi.h);
    bi.h.biWidth = 640;
    bi.h.biHeight = -480;
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 8;
    bi.colors[1] = {0, 0, 255, 0};
    bi.colors[2] = {0, 255, 0, 0};
    bi.colors[3] = {255, 0, 0, 0};
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(mem, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(mem, dib);
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++)
            static_cast<BYTE*>(bits)[y * 640 + x] = BYTE(x < 64 && y < 64 ? 3 : x < 320 ? 1 : 2);
    HDC wdc = GetDC(hwnd);
    BitBlt(wdc, 0, 0, 320, 480, mem, 0, 0, SRCCOPY);
    StretchBlt(wdc, 320, 0, 320, 480, mem, 320, 0, 320, 480, SRCCOPY);

    // The bottom left strip: StretchDIBits with palette indices (DIB_PAL_COLORS), looked up in
    // the palette selected into the window DC: yellow (entry 5). Pajama Sam 2's DisplayAPI=2.
    struct {
        WORD version, count;
        PALETTEENTRY e[8];
    } lp = {0x300, 8, {}};
    lp.e[5] = {255, 255, 0, 0};
    HPALETTE pal = CreatePalette(reinterpret_cast<LOGPALETTE*>(&lp));
    HPALETTE oldPal = SelectPalette(wdc, pal, FALSE);
    RealizePalette(wdc);
    struct {
        BITMAPINFOHEADER h;
        WORD index[256];
    } pi = {};
    pi.h.biSize = sizeof(pi.h);
    pi.h.biWidth = 4;
    pi.h.biHeight = -4;
    pi.h.biPlanes = 1;
    pi.h.biBitCount = 8;
    pi.index[0] = 5;
    const BYTE palBits[16] = {};
    StretchDIBits(wdc, 0, 400, 320, 80, 0, 0, 4, 4, palBits, reinterpret_cast<BITMAPINFO*>(&pi), DIB_PAL_COLORS, SRCCOPY);
    SelectPalette(wdc, oldPal, FALSE);

    // The bottom right strip: WinG (WinGStretchBlt), when there's a wing32.dll: magenta. The
    // game's WinG is Windows' module under Wine (its own WING32.DLL beside it on Windows), which
    // calls StretchBlt for it. Pajama Sam 2's DisplayAPI=1, its first choice.
    using WinGCreateDCFn = HDC(WINAPI*)();
    using WinGCreateBitmapFn = HBITMAP(WINAPI*)(HDC, BITMAPINFO*, void**);
    using WinGStretchBltFn = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, int, int);
    HMODULE wing = LoadLibraryA("wing32.dll");
    const auto wingDC = wing ? reinterpret_cast<WinGCreateDCFn>(reinterpret_cast<void*>(GetProcAddress(wing, "WinGCreateDC"))) : nullptr;
    const auto wingBitmap = wing ? reinterpret_cast<WinGCreateBitmapFn>(reinterpret_cast<void*>(GetProcAddress(wing, "WinGCreateBitmap"))) : nullptr;
    const auto wingStretch = wing ? reinterpret_cast<WinGStretchBltFn>(reinterpret_cast<void*>(GetProcAddress(wing, "WinGStretchBlt"))) : nullptr;
    HDC wdcWinG = wingDC && wingBitmap && wingStretch ? wingDC() : nullptr;
    HBITMAP wingBmp = nullptr;
    if (wdcWinG) {
        struct {
            BITMAPINFOHEADER h;
            RGBQUAD colors[256];
        } wi = {};
        wi.h = pi.h;
        wi.h.biClrUsed = 2;
        wi.colors[1] = {255, 0, 255, 0};
        void* wbits = nullptr;
        wingBmp = wingBitmap(wdcWinG, reinterpret_cast<BITMAPINFO*>(&wi), &wbits);
        if (wingBmp && wbits) {
            memset(wbits, 1, 16);
            SelectObject(wdcWinG, wingBmp);
            wingStretch(wdc, 320, 400, 320, 80, wdcWinG, 0, 0, 4, 4);
        }
    }
    ReleaseDC(hwnd, wdc);
    Pump(500);

    // The picture scaled over the real screen (stretch=aspect: 4:3 centred).
    const double scale = realW * 3 >= realH * 4 ? realH / 480.0 : realW / 640.0;
    const int ix = int((realW - 640 * scale) / 2), iy = int((realH - 480 * scale) / 2);
    auto at = [&](int x, int y) { return ScreenAt(ix + int(x * scale), iy + int(y * scale)); };
    const COLORREF red = at(160, 240), green = at(480, 240), blue = at(32, 32);
    snprintf(line, sizeof(line), "BitBlt to the window: on the screen, scaled (red %06lx at 160,240)", red);
    Check(Near(red, RGB(255, 0, 0)), line);
    snprintf(line, sizeof(line), "StretchBlt to the window: on the screen, scaled (green %06lx at 480,240)", green);
    Check(Near(green, RGB(0, 255, 0)), line);
    snprintf(line, sizeof(line), "the top left corner where the image is (blue %06lx at 32,32)", blue);
    Check(Near(blue, RGB(0, 0, 255)), line);
    const COLORREF yellow = at(160, 440);
    snprintf(line, sizeof(line), "StretchDIBits with palette indices: on the screen, scaled (yellow %06lx at 160,440)", yellow);
    Check(Near(yellow, RGB(255, 255, 0)), line);
    if (wingBmp) {
        const COLORREF magenta = at(480, 440);
        snprintf(line, sizeof(line), "WinGStretchBlt (wing32.dll): on the screen, scaled (magenta %06lx at 480,440)", magenta);
        Check(Near(magenta, RGB(255, 0, 255)), line);
    } else {
        fprintf(g_out, "SKIP WinGStretchBlt: no wing32.dll\n");
    }

    // The mouse in the game's coordinates: its middle the image's.
    SetCursorPos(320, 240);
    POINT real = {};
    using PhysFn = BOOL(WINAPI*)(POINT*);
    const auto phys = reinterpret_cast<PhysFn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("user32.dll"), "GetPhysicalCursorPos")));
    if (phys && phys(&real)) {
        const int ex = ix + int(320 * scale), ey = iy + int(240 * scale);
        snprintf(line, sizeof(line), "SetCursorPos(320, 240): the image's middle on the screen (%ld,%ld; want %d,%d)", real.x,
                 real.y, ex, ey);
        Check(labs(real.x - ex) <= 2 && labs(real.y - ey) <= 2, line);
    }
    POINT back = {};
    GetCursorPos(&back);
    snprintf(line, sizeof(line), "GetCursorPos: the game's 320,240 again (%ld,%ld)", back.x, back.y);
    Check(labs(back.x - 320) <= 1 && labs(back.y - 240) <= 1, line);

    // The window manager shrinking the window (i3 floated Pajama Sam 2's to 75x50, its smallest
    // floating size, when the game's quit prompt came up): user32's own SetWindowPos, not
    // through the game's imports. The window covers the screen again; the picture is still there.
    if (const auto real = reinterpret_cast<BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT)>(
            RealExport(GetModuleHandleA("user32.dll"), "SetWindowPos"))) {
        real(hwnd, nullptr, (realW - 75) / 2, (realH - 50) / 2, 75, 50, SWP_NOZORDER | SWP_NOACTIVATE);
        Pump(300);
        const COLORREF after = at(160, 240);
        snprintf(line, sizeof(line), "sized to 75x50 by someone else than the game: still covering (red %06lx at 160,240)",
                 after);
        Check(Near(after, RGB(255, 0, 0)), line);
    }

    // Back to the desktop's mode, as the game does when it quits.
    Check(cds && cds(nullptr, 0) == DISP_CHANGE_SUCCESSFUL, "ChangeDisplaySettingsA(NULL): successful");
    Pump(200);
    snprintf(line, sizeof(line), "the screen the desktop's again (GetSystemMetrics %dx%d)", GetSystemMetrics(SM_CXSCREEN),
             GetSystemMetrics(SM_CYSCREEN));
    Check(GetSystemMetrics(SM_CXSCREEN) == realW && GetSystemMetrics(SM_CYSCREEN) == realH, line);

    // DirectDraw objects of the game's own while dd2d9's GDI mode is shown (a game that also
    // draws with DirectDraw, loaded with dd2d9 by another wrapper): a windowed primary, then
    // exclusive mode, the GDI mode made for each. Each gets what it asks for.
    // (IDirectDraw's vtable: Release is entry 2, CreateSurface 6, SetCooperativeLevel 20.)
    using CreateFn = HRESULT(WINAPI*)(GUID*, void**, IUnknown*);
    using CoopFn = HRESULT(WINAPI*)(void*, HWND, DWORD);
    using SurfaceFn = HRESULT(WINAPI*)(void*, void*, void**, IUnknown*);
    using ReleaseFn = ULONG(WINAPI*)(void*);
    const auto create = reinterpret_cast<CreateFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("ddraw.dll"), "DirectDrawCreate")));
    auto vt = [](void* o) { return *static_cast<void***>(o); };
    for (int exclusive = 0; exclusive < 2; exclusive++) {
        cds(&dm, 0);  // (the GDI mode)
        void* dd = nullptr;
        if (!create || FAILED(create(nullptr, &dd, nullptr)) || !dd) {
            Check(false, "DirectDrawCreate from dd2d9");
            break;
        }
        HRESULT hr;
        if (exclusive) {
            hr = reinterpret_cast<CoopFn>(vt(dd)[20])(dd, hwnd, DDSCL_EXCLUSIVE_ | DDSCL_FULLSCREEN_);
            snprintf(line, sizeof(line), "the game's own DirectDraw object takes exclusive mode (0x%08lx)",
                     static_cast<unsigned long>(hr));
        } else {
            reinterpret_cast<CoopFn>(vt(dd)[20])(dd, hwnd, DDSCL_NORMAL_);
            // (DDSURFACEDESC: dwSize, dwFlags = DDSD_CAPS; ddsCaps at 104 = DDSCAPS_PRIMARYSURFACE)
            DWORD sd[27] = {sizeof(sd), 1};
            sd[26] = 0x200;
            void* primary = nullptr;
            hr = reinterpret_cast<SurfaceFn>(vt(dd)[6])(dd, sd, &primary, nullptr);
            snprintf(line, sizeof(line), "the game's own DirectDraw object makes a windowed primary (0x%08lx)",
                     static_cast<unsigned long>(hr));
            if (primary) reinterpret_cast<ReleaseFn>(vt(primary)[2])(primary);
        }
        Check(hr == 0, line);
        reinterpret_cast<CoopFn>(vt(dd)[20])(dd, hwnd, DDSCL_NORMAL_);
        reinterpret_cast<ReleaseFn>(vt(dd)[2])(dd);
    }

    DestroyWindow(hwnd);
    DeleteDC(mem);
    DeleteObject(dib);
    DeleteObject(pal);
    if (wdcWinG) DeleteDC(wdcWinG);
    if (wingBmp) DeleteObject(wingBmp);
    if (wing) FreeLibrary(wing);
    fprintf(g_out, "== %d failure(s)\n", g_fail);
    fclose(g_out);
    return g_fail ? 1 : 0;
}
