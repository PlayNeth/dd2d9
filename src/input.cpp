// Mouse coordinates for games shown scaled or in a window (display=borderless or window,
// or a mode the monitor lacks). The game asks for and sets the cursor in its display
// mode's coordinates, as if that mode filled the screen; the real cursor is on a screen
// of another size, the image somewhere on it. dd2d9 points the game's imports of the
// cursor functions at its own, which translate (see MouseToGame / MouseFromGame in
// ddscreen.cpp); the mouse messages to the game's window are translated in its window
// procedure (ddhook.cpp, HookProc). DirectInput's relative mouse needs nothing.
#include "inputint.h"
#include "objects.h"

#include <algorithm>

namespace dd2d9 {

bool IsGameWindow(HWND h, SIZE* mode) {
    RECT r;
    SIZE m;
    HWND w;
    if (!h || !GameImage(&r, &m, &w) || h != w) return false;
    if (mode) *mode = m;
    return true;
}

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

BOOL WINAPI HookGetCursorPos(POINT* p) {
    DD2D9_PASS_WINDOWS(g_getCursorPos, p);
    const BOOL ok = DD2D9_NEXT(g_getCursorPos)(p);
    if (ok && p) MouseToGame(p);
    return ok;
}

BOOL WINAPI HookSetCursorPos(int x, int y) {
    DD2D9_PASS_WINDOWS(g_setCursorPos, x, y);
    POINT p = {x, y};
    MouseFromGame(&p);
    return DD2D9_NEXT(g_setCursorPos)(p.x, p.y);
}

DWORD WINAPI HookGetMessagePos() {
    DD2D9_PASS_WINDOWS(g_getMessagePos);
    const DWORD v = DD2D9_NEXT(g_getMessagePos)();
    POINT p = {static_cast<short>(LOWORD(v)), static_cast<short>(HIWORD(v))};
    if (!MouseToGame(&p)) return v;
    return MAKELONG(static_cast<short>(p.x), static_cast<short>(p.y));
}

// A game confining the cursor to its screen confines it to the image.
BOOL WINAPI HookClipCursor(const RECT* rc) {
    DD2D9_PASS_WINDOWS(g_clipCursor, rc);
    RECT r;
    SIZE m;
    if (!rc || !GameImage(&r, &m)) return DD2D9_NEXT(g_clipCursor)(rc);
    RECT real = *rc;
    RectFromGame(&real, r, m);
    return DD2D9_NEXT(g_clipCursor)(&real);
}

BOOL WINAPI HookGetClipCursor(RECT* rc) {
    DD2D9_PASS_WINDOWS(g_getClipCursor, rc);
    const BOOL ok = DD2D9_NEXT(g_getClipCursor)(rc);
    RECT r;
    SIZE m;
    if (ok && rc && GameImage(&r, &m)) RectToGame(rc, r, m);
    return ok;
}

// The screen is the game's display mode, as with a real mode switch: its size, the area a
// maximized or full-screen window gets (the work area the mode's, as GetMonitorInfo answers
// it below; the frame and caption as they are), and the virtual screen of one monitor.
int WINAPI HookGetSystemMetrics(int index) {
    DD2D9_PASS_WINDOWS(g_getSystemMetrics, index);
    const GetSystemMetricsFn real = DD2D9_NEXT(g_getSystemMetrics);
    RECT r;
    SIZE m;
    switch (index) {
    case SM_CXSCREEN:
    case SM_CYSCREEN:
    case SM_CXFULLSCREEN:
    case SM_CYFULLSCREEN:
    case SM_CXMAXIMIZED:
    case SM_CYMAXIMIZED:
    case SM_XVIRTUALSCREEN:
    case SM_YVIRTUALSCREEN:
    case SM_CXVIRTUALSCREEN:
    case SM_CYVIRTUALSCREEN: break;
    default: return real(index);
    }
    if (!GameImage(&r, &m)) return real(index);
    switch (index) {
    case SM_CXSCREEN: return m.cx;
    case SM_CYSCREEN: return m.cy;
    case SM_CXFULLSCREEN: return m.cx;
    case SM_CYFULLSCREEN: return m.cy - real(SM_CYCAPTION);
    case SM_CXMAXIMIZED: return m.cx + real(SM_CXMAXIMIZED) - real(SM_CXFULLSCREEN);
    case SM_CYMAXIMIZED: return m.cy + real(SM_CYMAXIMIZED) - real(SM_CYFULLSCREEN) - real(SM_CYCAPTION);
    default:  // the virtual screen: other monitors' parts in it kept as they are
        if (real(SM_CMONITORS) > 1) return real(index);
        return index == SM_CXVIRTUALSCREEN ? m.cx : index == SM_CYVIRTUALSCREEN ? m.cy : 0;
    }
}

BOOL WINAPI HookScreenToClient(HWND h, POINT* p) {
    DD2D9_PASS_WINDOWS(g_screenToClient, h, p);
    if (p && IsGameWindow(h)) return TRUE;
    return DD2D9_NEXT(g_screenToClient)(h, p);
}

BOOL WINAPI HookClientToScreen(HWND h, POINT* p) {
    DD2D9_PASS_WINDOWS(g_clientToScreen, h, p);
    if (p && IsGameWindow(h)) return TRUE;
    return DD2D9_NEXT(g_clientToScreen)(h, p);
}

int WINAPI HookMapWindowPoints(HWND from, HWND to, POINT* p, UINT n) {
    DD2D9_PASS_WINDOWS(g_mapWindowPoints, from, to, p, n);
    // Between the game's window and the screen (NULL or the desktop): nothing moves.
    const HWND desktop = GetDesktopWindow();
    if ((IsGameWindow(from) && (!to || to == desktop)) || (IsGameWindow(to) && (!from || from == desktop))) {
        SetLastError(0);  // 0 with no error: nothing moved (0 with an error is a failure)
        return 0;
    }
    return DD2D9_NEXT(g_mapWindowPoints)(from, to, p, n);
}

// The desktop window is the screen, the game's mode's size, as after a real switch (a game
// centering or sizing its window from it, or checking the switch, saw the real desktop).
bool IsDesktopShownAsMode(HWND h, SIZE* mode) {
    RECT r;
    return h && h == GetDesktopWindow() && GameImage(&r, mode);
}

// The game's window and the desktop window, while the mode is translated: the mode's size at
// 0,0, window and client area alike (false: neither).
bool ModeRect(HWND h, RECT* rc) {
    SIZE m;
    if (!rc || !(IsGameWindow(h, &m) || IsDesktopShownAsMode(h, &m))) return false;
    *rc = RECT{0, 0, m.cx, m.cy};
    return true;
}

BOOL WINAPI HookGetClientRect(HWND h, RECT* rc) {
    DD2D9_PASS_WINDOWS(g_getClientRect, h, rc);
    return ModeRect(h, rc) ? TRUE : DD2D9_NEXT(g_getClientRect)(h, rc);
}

BOOL WINAPI HookGetWindowRect(HWND h, RECT* rc) {
    DD2D9_PASS_WINDOWS(g_getWindowRect, h, rc);
    return ModeRect(h, rc) ? TRUE : DD2D9_NEXT(g_getWindowRect)(h, rc);
}

// The monitor the game's screen is on: the mode's size at 0,0, work area and all (a switched
// display showed the game nothing else: its fullscreen window covers the taskbar).
using GetMonitorInfoFn = BOOL(WINAPI*)(HMONITOR, MONITORINFO*);  // (A and W alike)
GetMonitorInfoFn g_getMonitorInfoA, g_getMonitorInfoW;
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
    DD2D9_PASS_WINDOWS(g_getMonitorInfoA, mon, mi);
    const BOOL ok = DD2D9_NEXT(g_getMonitorInfoA)(mon, mi);
    MonitorAsMode(mon, mi, ok);
    return ok;
}
BOOL WINAPI HookGetMonitorInfoW(HMONITOR mon, MONITORINFO* mi) {
    DD2D9_PASS_WINDOWS(g_getMonitorInfoW, mon, mi);
    const BOOL ok = DD2D9_NEXT(g_getMonitorInfoW)(mon, mi);
    MonitorAsMode(mon, mi, ok);
    return ok;
}

ImportHook kHooks[] = {
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

void InstallMouseHooks() {
    const unsigned n =
        PatchGameModules(kHooks, sizeof(kHooks) / sizeof(kHooks[0])) + PatchGameModules(kPaintHooks, kPaintHookCount);
    if (n) DD2D9_LOG("mouse: %u cursor and paint imports translated for the game's display mode", n);
}

}  // namespace dd2d9
