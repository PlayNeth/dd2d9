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
#include <cstring>
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

BOOL WINAPI HookGetClientRect(HWND h, RECT* rc) {
    SIZE m;
    if (rc && IsGameWindow(h, &m)) {
        *rc = RECT{0, 0, m.cx, m.cy};
        return TRUE;
    }
    return g_getClientRect(h, rc);
}

BOOL WINAPI HookGetWindowRect(HWND h, RECT* rc) {
    SIZE m;
    if (rc && IsGameWindow(h, &m)) {
        *rc = RECT{0, 0, m.cx, m.cy};
        return TRUE;
    }
    return g_getWindowRect(h, rc);
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
};

}  // namespace

void InstallMouseHooks() {
    if (!g_config.mouseScale) return;
    if (const unsigned n = PatchGameModules(kHooks, sizeof(kHooks) / sizeof(kHooks[0])))
        DD2D9_LOG("mouse: %u cursor imports translated for the game's display mode", n);
}

}  // namespace dd2d9
