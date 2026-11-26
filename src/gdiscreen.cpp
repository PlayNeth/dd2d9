// GDI fullscreen (gdifullscreen=): a game that draws with GDI alone and goes fullscreen by
// switching the display with ChangeDisplaySettings (Pajama Sam 2: 640x480, its window over all
// of it) gets a display mode of dd2d9's own instead. The switch is made with a DirectDraw object
// of dd2d9's in exclusive mode on the game's window, as a DirectDraw game's would be, and the
// game's blits to that window (compatgdi.cpp's kWindowHooks) are drawn into its primary, which
// dd2d9 shows as any primary: scaled to the screen (display=, stretch=, filter=), its focus and
// mouse handled. Without it such a game asked for a mode the screen (or Wine) refused, and
// stayed a 640x480 window in the middle of the desktop, unless the desktop itself was set to
// 640x480. Such a game loads no ddraw.dll: dd2d9 comes in with the loader (loader/winmm.cpp, a
// winmm.dll in the game's folder that passes every call on to Windows' own), which switches
// this on (gdifullscreen=auto).
#include "ddint.h"

extern "C" HRESULT WINAPI DD2D9_DirectDrawCreate(GUID* guid, LPDIRECTDRAW* out, IUnknown* outer);  // (main.cpp)

namespace dd2d9 {

namespace {

// The loader asked for it (auto: on then).
volatile LONG g_loaderAsked;

// The display mode of dd2d9's own, made for the game (g_lock).
struct GdiScreen {
    IDirectDraw* dd = nullptr;
    IDirectDrawSurface* primary = nullptr;
    HWND hwnd = nullptr;
    DWORD w = 0, h = 0;
};
GdiScreen g_screen;

bool Enabled() {
    return g_config.gdiFullscreen == kMinimizeOn || (g_config.gdiFullscreen == kMinimizeAuto && g_loaderAsked);
}

// The window the game goes fullscreen in: its active one, else the largest visible top-level
// window of this thread, else of the process.
struct FindWindowCtx {
    HWND best;
    long area;
};
BOOL CALLBACK Consider(HWND w, LPARAM p) {
    auto* c = reinterpret_cast<FindWindowCtx*>(p);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || (GetWindowLongW(w, GWL_STYLE) & WS_CHILD)) return TRUE;
    RECT r;
    if (!GetWindowRect(w, &r)) return TRUE;
    const long area = long(r.right - r.left) * long(r.bottom - r.top);
    if (area > c->area) {
        c->best = w;
        c->area = area;
    }
    return TRUE;
}

HWND GameWindow() {
    if (HWND a = GetActiveWindow()) return GetAncestor(a, GA_ROOT);
    FindWindowCtx c = {nullptr, -1};
    EnumThreadWindows(GetCurrentThreadId(), Consider, reinterpret_cast<LPARAM>(&c));
    if (!c.best) EnumWindows(Consider, reinterpret_cast<LPARAM>(&c));
    return c.best;
}

// The display mode left (g_lock): the object released, as a game's would be, which puts the
// desktop's mode back.
void Leave() {
    if (!g_screen.dd) return;
    DD2D9_LOG("gdifullscreen: %lux%lu left", g_screen.w, g_screen.h);
    if (g_screen.primary) g_screen.primary->Release();
    // (DirectDraw 1 leaves a mode set as it is when exclusive mode ends: put back by hand.)
    g_screen.dd->RestoreDisplayMode();
    g_screen.dd->SetCooperativeLevel(g_screen.hwnd, DDSCL_NORMAL);
    g_screen.dd->Release();
    g_screen = GdiScreen{};
}

// A mode of w x h shown for the game in its window (g_lock). The primary is cleared to black:
// what the game hasn't drawn yet.
bool Enter(DWORD w, DWORD h) {
    const HWND hwnd = GameWindow();
    if (!hwnd) {
        DD2D9_LOG("gdifullscreen: %lux%lu asked for with no window of the game's: passed on", w, h);
        return false;
    }
    if (g_screen.dd && g_screen.hwnd != hwnd) Leave();
    HRESULT hr = S_OK;
    if (!g_screen.dd) {
        hr = DD2D9_DirectDrawCreate(nullptr, &g_screen.dd, nullptr);
        if (SUCCEEDED(hr)) {
            g_screen.hwnd = hwnd;
            hr = g_screen.dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        }
    }
    // (The colour depth: 32 bits whatever the game asked. It draws with GDI, which converts
    // its bitmaps to the primary's colours; an 8-bit mode would make GDI match them against a
    // palette no one sets.)
    // (Another size asked for while one is shown: the primary made again for it, as a game's
    // own would be after SetDisplayMode, which leaves the old one lost.)
    if (SUCCEEDED(hr) && g_screen.primary && (g_screen.w != w || g_screen.h != h)) {
        g_screen.primary->Release();
        g_screen.primary = nullptr;
    }
    if (SUCCEEDED(hr)) hr = g_screen.dd->SetDisplayMode(w, h, 32);
    if (SUCCEEDED(hr) && !g_screen.primary) {
        DDSURFACEDESC sd = {};
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS;
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        hr = g_screen.dd->CreateSurface(&sd, &g_screen.primary, nullptr);
    }
    if (FAILED(hr)) {
        DD2D9_LOG("gdifullscreen: %lux%lu for window %p failed: 0x%08lx", w, h, static_cast<void*>(hwnd),
                  static_cast<unsigned long>(hr));
        Leave();
        return false;
    }
    DDBLTFX fx = {};
    fx.dwSize = sizeof(fx);
    g_screen.primary->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    g_screen.w = w;
    g_screen.h = h;
    DD2D9_LOG("gdifullscreen: the game's %lux%lu shown by dd2d9 in window %p", w, h, static_cast<void*>(hwnd));
    return true;
}

}  // namespace

void WINAPI GdiFullscreenFromLoader() {
    if (InterlockedExchange(&g_loaderAsked, 1)) return;
    DD2D9_LOG("gdifullscreen: loaded by the loader (%s)",
              g_config.gdiFullscreen == kMinimizeOff ? "off: nothing done" : "the game's display changes shown by dd2d9");
}

bool GdiFullscreenChange(bool hasMode, bool sized, DWORD w, DWORD h, DWORD flags, LONG* result) {
    if (!Enabled()) return false;
    Guard l;
    // A DirectDraw game's own objects decide (one may well switch the display with
    // ChangeDisplaySettings and draw windowed): left to them.
    if (g_ddraws.size() > (g_screen.dd ? 1u : 0u)) return false;
    // A change of something else than the size (the depth alone) while dd2d9's mode is shown:
    // nothing to do, as the mode is 32-bit whatever was asked (see Enter).
    if (!sized && hasMode) {
        if (!g_screen.dd) return false;
        *result = DISP_CHANGE_SUCCESSFUL;
        return true;
    }
    // The desktop's mode asked for (or none: the registry's), the way back.
    DEVMODEW desk = {};
    desk.dmSize = sizeof(desk);
    const bool home = !sized || (EnumDisplaySettingsW(nullptr, ENUM_REGISTRY_SETTINGS, &desk) &&
                                 w == desk.dmPelsWidth && h == desk.dmPelsHeight);
    if (home) {
        if (!g_screen.dd) return false;  // nothing of dd2d9's to undo: as it is
        if (!(flags & CDS_TEST)) Leave();
        *result = DISP_CHANGE_SUCCESSFUL;
        return true;
    }
    if (w < 320 || h < 200 || w > 4096 || h > 4096) return false;
    if (flags & CDS_TEST) {
        *result = DISP_CHANGE_SUCCESSFUL;
        return true;
    }
    if (g_screen.dd && g_screen.w == w && g_screen.h == h) {
        *result = DISP_CHANGE_SUCCESSFUL;
        return true;
    }
    if (!Enter(w, h)) return false;
    *result = DISP_CHANGE_SUCCESSFUL;
    return true;
}

bool GdiFullscreenDraw(HDC dc, void (*draw)(HDC, void*), void* ctx) {
    if (!g_screen.primary || !dc || GetObjectType(dc) != OBJ_DC) return false;  // (read unlocked: set once in a while)
    const HWND w = WindowFromDC(dc);
    if (!w) return false;
    const HWND root = GetAncestor(w, GA_ROOT);
    if (GetMapMode(dc) != MM_TEXT || GetGraphicsMode(dc) != GM_COMPATIBLE) return false;
    POINT org, view, win, client = {0, 0};
    if (!GetDCOrgEx(dc, &org) || !GetViewportOrgEx(dc, &view) || !GetWindowOrgEx(dc, &win) || !ClientToScreen(root, &client))
        return false;
    // The DC's origin from the window's client area: the mode's coordinates (the game's view
    // of its window is the mode, see input.cpp).
    const int ox = org.x - client.x, oy = org.y - client.y;
    HRGN clip = nullptr;
    if (HRGN r = CreateRectRgn(0, 0, 0, 0)) {
        if (GetClipRgn(dc, r) == 1) {
            OffsetRgn(r, ox, oy);
            clip = r;
        } else {
            DeleteObject(r);
        }
    }
    bool done = false;
    {
        Guard l;
        if (g_screen.primary && root == g_screen.hwnd) {
            HDC p = nullptr;
            HRESULT hr = g_screen.primary->GetDC(&p);
            if (hr == DDERR_SURFACELOST && SUCCEEDED(g_screen.primary->Restore())) {
                // Back from the background: the game is asked to paint all of it again.
                InvalidateRect(g_screen.hwnd, nullptr, FALSE);
                hr = g_screen.primary->GetDC(&p);
            }
            if (SUCCEEDED(hr)) {
                const int saved = SaveDC(p);
                if (clip) SelectClipRgn(p, clip);
                SetViewportOrgEx(p, ox + view.x, oy + view.y, nullptr);
                SetWindowOrgEx(p, win.x, win.y, nullptr);
                SetStretchBltMode(p, GetStretchBltMode(dc));
                SetBkColor(p, GetBkColor(dc));
                SetTextColor(p, GetTextColor(dc));
                SelectObject(p, GetCurrentObject(dc, OBJ_BRUSH));
                POINT brushOrg;
                if (GetBrushOrgEx(dc, &brushOrg)) SetBrushOrgEx(p, ox + brushOrg.x, oy + brushOrg.y, nullptr);
                draw(p, ctx);
                GdiFlush();
                RestoreDC(p, saved);
                g_screen.primary->ReleaseDC(p);
                done = true;
                static bool logged;
                if (!logged) {
                    logged = true;
                    DD2D9_LOG("gdifullscreen: the game's blits to window %p drawn into dd2d9's primary", static_cast<void*>(w));
                }
            }
        }
    }
    if (clip) DeleteObject(clip);
    return done;
}

}  // namespace dd2d9

extern "C" void WINAPI DD2D9_GdiFullscreen() {
    dd2d9::GdiFullscreenFromLoader();
}
