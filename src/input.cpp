// Mouse coordinates for games shown scaled or in a window (display=borderless or window,
// or a mode the monitor lacks). The game asks for and sets the cursor in its display
// mode's coordinates, as if that mode filled the screen; the real cursor is on a screen
// of another size, the image somewhere on it. dd2d9 points the game's imports of the
// cursor functions at its own, which translate (see MouseToGame / MouseFromGame in
// ddraw.cpp); the mouse messages to the game's window are translated in its window
// procedure (ddraw.cpp, HookProc). DirectInput's relative mouse needs nothing.
#include "hooks.h"
#include "objects.h"

#include <algorithm>
#include <vector>

namespace dd2d9 {

namespace {

using GetCursorPosFn = BOOL(WINAPI*)(POINT*);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using GetMessagePosFn = DWORD(WINAPI*)();
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using GetClipCursorFn = BOOL(WINAPI*)(RECT*);
using GetSystemMetricsFn = int(WINAPI*)(int);
using PointFn = BOOL(WINAPI*)(HWND, POINT*);
using RectFn = BOOL(WINAPI*)(HWND, RECT*);
using MapWindowPointsFn = int(WINAPI*)(HWND, HWND, POINT*, UINT);

GetCursorPosFn g_getCursorPos;
SetCursorPosFn g_setCursorPos;
GetMessagePosFn g_getMessagePos;
ClipCursorFn g_clipCursor;
GetClipCursorFn g_getClipCursor;
GetSystemMetricsFn g_getSystemMetrics;
PointFn g_screenToClient, g_clientToScreen;
RectFn g_getClientRect, g_getWindowRect;
MapWindowPointsFn g_mapWindowPoints;

// The game's window, while its display mode is translated: as with a real mode switch, it
// covers the game's screen at 0,0, so its client and screen coordinates are the same.
bool IsGameWindow(HWND h, SIZE* mode = nullptr) {
    RECT r;
    SIZE m;
    HWND w;
    if (!h || !GameImage(&r, &m, &w) || h != w) return false;
    if (mode) *mode = m;
    return true;
}

// The game's rectangle (exclusive right/bottom edges) on the real screen and back.
void RectFromGame(RECT* rc, const RECT& r, SIZE m) {
    const LONG w = r.right - r.left, h = r.bottom - r.top;
    rc->left = r.left + LONG(int64_t(rc->left) * w / m.cx);
    rc->top = r.top + LONG(int64_t(rc->top) * h / m.cy);
    rc->right = r.left + LONG(int64_t(rc->right) * w / m.cx);
    rc->bottom = r.top + LONG(int64_t(rc->bottom) * h / m.cy);
}

void RectToGame(RECT* rc, const RECT& r, SIZE m) {
    const LONG w = std::max<LONG>(r.right - r.left, 1), h = std::max<LONG>(r.bottom - r.top, 1);
    auto x = [&](LONG v) { return std::clamp<LONG>(LONG(int64_t(v - r.left) * m.cx / w), 0, m.cx); };
    auto y = [&](LONG v) { return std::clamp<LONG>(LONG(int64_t(v - r.top) * m.cy / h), 0, m.cy); };
    *rc = RECT{x(rc->left), y(rc->top), x(rc->right), y(rc->bottom)};
}

BOOL WINAPI HookGetCursorPos(POINT* p) {
    const BOOL ok = g_getCursorPos(p);
    if (ok && p) MouseToGame(p);
    return ok;
}

BOOL WINAPI HookSetCursorPos(int x, int y) {
    POINT p = {x, y};
    MouseFromGame(&p);
    return g_setCursorPos(p.x, p.y);
}

DWORD WINAPI HookGetMessagePos() {
    const DWORD v = g_getMessagePos();
    POINT p = {static_cast<short>(LOWORD(v)), static_cast<short>(HIWORD(v))};
    if (!MouseToGame(&p)) return v;
    return MAKELONG(static_cast<short>(p.x), static_cast<short>(p.y));
}

// A game confining the cursor to its screen confines it to the image.
BOOL WINAPI HookClipCursor(const RECT* rc) {
    RECT r;
    SIZE m;
    if (!rc || !GameImage(&r, &m)) return g_clipCursor(rc);
    RECT real = *rc;
    RectFromGame(&real, r, m);
    return g_clipCursor(&real);
}

BOOL WINAPI HookGetClipCursor(RECT* rc) {
    const BOOL ok = g_getClipCursor(rc);
    RECT r;
    SIZE m;
    if (ok && rc && GameImage(&r, &m)) RectToGame(rc, r, m);
    return ok;
}

// The screen is the game's display mode, as with a real mode switch.
int WINAPI HookGetSystemMetrics(int index) {
    if (index == SM_CXSCREEN || index == SM_CYSCREEN) {
        RECT r;
        SIZE m;
        if (GameImage(&r, &m)) return index == SM_CXSCREEN ? m.cx : m.cy;
    }
    return g_getSystemMetrics(index);
}

BOOL WINAPI HookScreenToClient(HWND h, POINT* p) {
    if (p && IsGameWindow(h)) return TRUE;
    return g_screenToClient(h, p);
}

BOOL WINAPI HookClientToScreen(HWND h, POINT* p) {
    if (p && IsGameWindow(h)) return TRUE;
    return g_clientToScreen(h, p);
}

int WINAPI HookMapWindowPoints(HWND from, HWND to, POINT* p, UINT n) {
    // Between the game's window and the screen (NULL or the desktop): nothing moves.
    const HWND desktop = GetDesktopWindow();
    if ((IsGameWindow(from) && (!to || to == desktop)) || (IsGameWindow(to) && (!from || from == desktop))) {
        SetLastError(0);  // 0 with no error: nothing moved (0 with an error is a failure)
        return 0;
    }
    return g_mapWindowPoints(from, to, p, n);
}

// The desktop window is the screen, the game's mode's size, as after a real switch (a game
// centering or sizing its window from it, or checking the switch, saw the real desktop).
bool IsDesktopShownAsMode(HWND h, SIZE* mode) {
    RECT r;
    return h && h == GetDesktopWindow() && GameImage(&r, mode);
}

BOOL WINAPI HookGetClientRect(HWND h, RECT* rc) {
    SIZE m;
    if (rc && (IsGameWindow(h, &m) || IsDesktopShownAsMode(h, &m))) {
        *rc = RECT{0, 0, m.cx, m.cy};
        return TRUE;
    }
    return g_getClientRect(h, rc);
}

BOOL WINAPI HookGetWindowRect(HWND h, RECT* rc) {
    SIZE m;
    if (rc && (IsGameWindow(h, &m) || IsDesktopShownAsMode(h, &m))) {
        *rc = RECT{0, 0, m.cx, m.cy};
        return TRUE;
    }
    return g_getWindowRect(h, rc);
}

// The monitor the game's screen is on: the mode's size at 0,0, work area and all (a switched
// display showed the game nothing else: its fullscreen window covers the taskbar).
using GetMonitorInfoAFn = BOOL(WINAPI*)(HMONITOR, MONITORINFO*);
using GetMonitorInfoWFn = BOOL(WINAPI*)(HMONITOR, MONITORINFO*);
GetMonitorInfoAFn g_getMonitorInfoA;
GetMonitorInfoWFn g_getMonitorInfoW;
void MonitorAsMode(HMONITOR mon, MONITORINFO* mi, BOOL ok) {
    RECT r;
    SIZE m;
    HWND w;
    if (!ok || !mi || mi->cbSize < sizeof(MONITORINFO) || !GameImage(&r, &m, &w)) return;
    const POINT mid = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    if (mon != MonitorFromPoint(mid, MONITOR_DEFAULTTOPRIMARY)) return;
    mi->rcMonitor = mi->rcWork = RECT{0, 0, m.cx, m.cy};
}
BOOL WINAPI HookGetMonitorInfoA(HMONITOR mon, MONITORINFO* mi) {
    const BOOL ok = g_getMonitorInfoA(mon, mi);
    MonitorAsMode(mon, mi, ok);
    return ok;
}
BOOL WINAPI HookGetMonitorInfoW(HMONITOR mon, MONITORINFO* mi) {
    const BOOL ok = g_getMonitorInfoW(mon, mi);
    MonitorAsMode(mon, mi, ok);
    return ok;
}

// The parts of the game's window to repaint (WM_PAINT), in its mode's coordinates as its
// client area is (GetClientRect): Road Rash repaints its menus by blitting the paint rectangle
// from its 640x480 back buffer, and with the real 1920x1080 client asked for 0,0-1920,480 (an
// invalid rectangle: the menus never showed). The real area is where the image is; parts of
// it are mapped back (clamped to the mode), and areas the game invalidates are mapped there.
using BeginPaintFn = HDC(WINAPI*)(HWND, PAINTSTRUCT*);
using GetUpdateRectFn = BOOL(WINAPI*)(HWND, RECT*, BOOL);
using GetUpdateRgnFn = int(WINAPI*)(HWND, HRGN, BOOL);
using InvalidateRectFn = BOOL(WINAPI*)(HWND, const RECT*, BOOL);
BeginPaintFn g_beginPaint;
GetUpdateRectFn g_getUpdateRect;
GetUpdateRgnFn g_getUpdateRgn;
InvalidateRectFn g_invalidateRect;

// A rectangle in the game window's real client coordinates, in its mode's (false: not the
// game's window while its mode is translated).
bool ClientRectToGame(HWND h, RECT* rc) {
    RECT image;
    SIZE m;
    HWND w;
    if (!h || !GameImage(&image, &m, &w) || h != w) return false;
    POINT o = {0, 0};
    ClientToScreen(h, &o);  // (dd2d9's own call: the real one)
    OffsetRect(rc, o.x, o.y);
    RectToGame(rc, image, m);
    return true;
}

HDC WINAPI HookBeginPaint(HWND h, PAINTSTRUCT* ps) {
    const HDC dc = g_beginPaint(h, ps);
    if (dc && ps) ClientRectToGame(h, &ps->rcPaint);
    return dc;
}

BOOL WINAPI HookGetUpdateRect(HWND h, RECT* rc, BOOL erase) {
    const BOOL any = g_getUpdateRect(h, rc, erase);
    if (any && rc) ClientRectToGame(h, rc);
    return any;
}

int WINAPI HookGetUpdateRgn(HWND h, HRGN rgn, BOOL erase) {
    const int type = g_getUpdateRgn(h, rgn, erase);
    if (type != SIMPLEREGION && type != COMPLEXREGION) return type;
    RECT image;
    SIZE m;
    HWND w;
    if (!GameImage(&image, &m, &w) || h != w) return type;
    const DWORD size = GetRegionData(rgn, 0, nullptr);
    if (!size) return type;
    std::vector<BYTE> buf(size);
    RGNDATA* data = reinterpret_cast<RGNDATA*>(buf.data());
    if (GetRegionData(rgn, size, data) != size) return type;
    const RECT* rects = reinterpret_cast<const RECT*>(data->Buffer);
    HRGN game = CreateRectRgn(0, 0, 0, 0);
    if (!game) return type;
    for (DWORD i = 0; i < data->rdh.nCount; i++) {
        RECT r = rects[i];
        if (!ClientRectToGame(h, &r) || IsRectEmpty(&r)) continue;
        if (HRGN one = CreateRectRgnIndirect(&r)) {
            CombineRgn(game, game, one, RGN_OR);
            DeleteObject(one);
        }
    }
    const int out = CombineRgn(rgn, game, nullptr, RGN_COPY);
    DeleteObject(game);
    return out == ERROR ? type : out;
}

BOOL WINAPI HookInvalidateRect(HWND h, const RECT* rc, BOOL erase) {
    RECT image;
    SIZE m;
    HWND w;
    if (!rc || !h || !GameImage(&image, &m, &w) || h != w) return g_invalidateRect(h, rc, erase);
    RECT r = *rc;
    RectFromGame(&r, image, m);  // (screen coordinates: into the client's)
    POINT o = {0, 0};
    ClientToScreen(h, &o);
    OffsetRect(&r, -o.x, -o.y);
    return g_invalidateRect(h, &r, erase);
}

// A popup window the game makes over its screen, in its mode's coordinates: Road Rash plays
// its movies in a 640x480 popup it puts at 0,0, topmost (filling the switched screen), which
// came up in the corner of the real one. A top-level popup within the mode's screen is put
// where that part of the image is, its size scaled (ScaledPopup: for the video player's
// window inside it, video.cpp). Remembered while it exists.
using CreateWindowExAFn = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
using CreateWindowExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
CreateWindowExAFn g_createWindowExA;
CreateWindowExWFn g_createWindowExW;
CRITICAL_SECTION g_popupLock;
std::vector<HWND>* g_popups;  // under g_popupLock

void InitPopups() {
    static const bool done = [] {
        InitializeCriticalSection(&g_popupLock);
        g_popups = new std::vector<HWND>;
        return true;
    }();
    (void)done;
}

// The rectangle to create a popup at (screen coordinates), when it is one over the game's
// screen: top level, WS_POPUP, not WS_CHILD, a size given, within the mode.
bool PopupRect(DWORD style, HWND parent, int* x, int* y, int* w, int* h) {
    if ((style & WS_CHILD) || !(style & WS_POPUP) || *x == CW_USEDEFAULT || *w == CW_USEDEFAULT || *w <= 0 || *h <= 0)
        return false;
    RECT image;
    SIZE m;
    HWND gw;
    if (!GameImage(&image, &m, &gw)) return false;
    if (parent && parent != gw) return false;  // (owned by the game's window, or by none)
    if (*x < 0 || *y < 0 || *x + *w > m.cx || *y + *h > m.cy) return false;
    RECT r = {*x, *y, *x + *w, *y + *h};
    RectFromGame(&r, image, m);
    *x = r.left, *y = r.top, *w = r.right - r.left, *h = r.bottom - r.top;
    return true;
}

void RememberPopup(HWND h) {
    if (!h) return;
    InitPopups();
    EnterCriticalSection(&g_popupLock);
    g_popups->erase(std::remove_if(g_popups->begin(), g_popups->end(), [](HWND p) { return !IsWindow(p); }),
                    g_popups->end());
    g_popups->push_back(h);
    LeaveCriticalSection(&g_popupLock);
    // Painted black where nothing else paints it: on the switched screen such a popup only
    // ever showed what the game had drawn under it (black, before a movie); a window here
    // shows nothing of the screen under it, and parts it doesn't paint showed whatever the
    // compositor had there (Road Rash's race intros: the menu above and below the movie).
    // Only for a class without a background brush of its own.
    if (!GetClassLongPtrW(h, GCLP_HBRBACKGROUND)) {
        SetClassLongPtrW(h, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH)));
        DD2D9_LOG("window %p: a popup over the game's screen, put over the image (background black)", static_cast<void*>(h));
    } else {
        DD2D9_LOG("window %p: a popup over the game's screen, put over the image", static_cast<void*>(h));
    }
}

HWND WINAPI HookCreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                                HMENU menu, HINSTANCE inst, LPVOID param) {
    const bool popup = PopupRect(style, parent, &x, &y, &w, &h);
    const HWND r = g_createWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (popup) RememberPopup(r);
    return r;
}

HWND WINAPI HookCreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                                HMENU menu, HINSTANCE inst, LPVOID param) {
    const bool popup = PopupRect(style, parent, &x, &y, &w, &h);
    const HWND r = g_createWindowExW(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (popup) RememberPopup(r);
    return r;
}

ImportHook kHooks[] = {
    {"user32", "CreateWindowExA", reinterpret_cast<void*>(HookCreateWindowExA), reinterpret_cast<void**>(&g_createWindowExA)},
    {"user32", "CreateWindowExW", reinterpret_cast<void*>(HookCreateWindowExW), reinterpret_cast<void**>(&g_createWindowExW)},
    {"user32", "BeginPaint", reinterpret_cast<void*>(HookBeginPaint), reinterpret_cast<void**>(&g_beginPaint)},
    {"user32", "GetUpdateRect", reinterpret_cast<void*>(HookGetUpdateRect), reinterpret_cast<void**>(&g_getUpdateRect)},
    {"user32", "GetUpdateRgn", reinterpret_cast<void*>(HookGetUpdateRgn), reinterpret_cast<void**>(&g_getUpdateRgn)},
    {"user32", "InvalidateRect", reinterpret_cast<void*>(HookInvalidateRect), reinterpret_cast<void**>(&g_invalidateRect)},
    {"user32", "GetCursorPos", reinterpret_cast<void*>(HookGetCursorPos), reinterpret_cast<void**>(&g_getCursorPos)},
    {"user32", "SetCursorPos", reinterpret_cast<void*>(HookSetCursorPos), reinterpret_cast<void**>(&g_setCursorPos)},
    {"user32", "GetMessagePos", reinterpret_cast<void*>(HookGetMessagePos), reinterpret_cast<void**>(&g_getMessagePos)},
    {"user32", "ClipCursor", reinterpret_cast<void*>(HookClipCursor), reinterpret_cast<void**>(&g_clipCursor)},
    {"user32", "GetClipCursor", reinterpret_cast<void*>(HookGetClipCursor), reinterpret_cast<void**>(&g_getClipCursor)},
    {"user32", "GetSystemMetrics", reinterpret_cast<void*>(HookGetSystemMetrics), reinterpret_cast<void**>(&g_getSystemMetrics)},
    {"user32", "ScreenToClient", reinterpret_cast<void*>(HookScreenToClient), reinterpret_cast<void**>(&g_screenToClient)},
    {"user32", "ClientToScreen", reinterpret_cast<void*>(HookClientToScreen), reinterpret_cast<void**>(&g_clientToScreen)},
    {"user32", "MapWindowPoints", reinterpret_cast<void*>(HookMapWindowPoints), reinterpret_cast<void**>(&g_mapWindowPoints)},
    {"user32", "GetClientRect", reinterpret_cast<void*>(HookGetClientRect), reinterpret_cast<void**>(&g_getClientRect)},
    {"user32", "GetWindowRect", reinterpret_cast<void*>(HookGetWindowRect), reinterpret_cast<void**>(&g_getWindowRect)},
    {"user32", "GetMonitorInfoA", reinterpret_cast<void*>(HookGetMonitorInfoA), reinterpret_cast<void**>(&g_getMonitorInfoA)},
    {"user32", "GetMonitorInfoW", reinterpret_cast<void*>(HookGetMonitorInfoW), reinterpret_cast<void**>(&g_getMonitorInfoW)},
};

}  // namespace

bool ScaledPopup(HWND h, double* sx, double* sy) {
    if (!h || !g_popups) return false;
    EnterCriticalSection(&g_popupLock);
    const bool known = std::find(g_popups->begin(), g_popups->end(), h) != g_popups->end();
    LeaveCriticalSection(&g_popupLock);
    RECT image;
    SIZE m;
    HWND gw;
    // (Not the window the game's mode is shown in, should the game have made that one as a
    // popup too: it covers the screen, the image somewhere in it.)
    if (!known || !IsWindow(h) || !GameImage(&image, &m, &gw) || h == gw || m.cx <= 0 || m.cy <= 0) return false;
    *sx = double(image.right - image.left) / m.cx;
    *sy = double(image.bottom - image.top) / m.cy;
    return true;
}

void InstallMouseHooks() {
    if (const unsigned n = PatchGameModules(kHooks, sizeof(kHooks) / sizeof(kHooks[0])))
        DD2D9_LOG("mouse: %u cursor and paint imports translated for the game's display mode", n);
}

}  // namespace dd2d9
