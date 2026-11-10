// Paint areas and popups of the game's window for games shown scaled or in a window (see
// input.cpp): what to repaint in its mode's coordinates, and popups it puts over its screen.
#include "inputint.h"
#include "objects.h"

#include <algorithm>
#include <vector>

namespace dd2d9 {

namespace {

// The parts of the game's window to repaint (WM_PAINT), in its mode's coordinates as its
// client area is (GetClientRect): Road Rash repaints its menus by blitting the paint rectangle
// from its 640x480 back buffer, and with the real 1920x1080 client asked for 0,0-1920,480 (an
// invalid rectangle: the menus never showed). The real area is where the image is; parts of
// it are mapped back (clamped to the mode), and areas the game validates, invalidates or
// redraws are mapped there, both ways alike: a game validating the rectangle GetUpdateRect
// gave it validated only that much of the real client area, and WM_PAINT came again forever.
using BeginPaintFn = HDC(WINAPI*)(HWND, PAINTSTRUCT*);
using GetUpdateRectFn = BOOL(WINAPI*)(HWND, RECT*, BOOL);
using GetUpdateRgnFn = int(WINAPI*)(HWND, HRGN, BOOL);
using InvalidateRectFn = BOOL(WINAPI*)(HWND, const RECT*, BOOL);
using ValidateRectFn = BOOL(WINAPI*)(HWND, const RECT*);
using InvalidateRgnFn = BOOL(WINAPI*)(HWND, HRGN, BOOL);
using ValidateRgnFn = BOOL(WINAPI*)(HWND, HRGN);
using RedrawWindowFn = BOOL(WINAPI*)(HWND, const RECT*, HRGN, UINT);
BeginPaintFn g_beginPaint;
GetUpdateRectFn g_getUpdateRect;
GetUpdateRgnFn g_getUpdateRgn;
InvalidateRectFn g_invalidateRect;
ValidateRectFn g_validateRect;
InvalidateRgnFn g_invalidateRgn;
ValidateRgnFn g_validateRgn;
RedrawWindowFn g_redrawWindow;

// Paint rectangles are rounded outward both ways, unlike the cursor's (RectToGame and
// RectFromGame round down): mapped to the mode and back, a rectangle covers at least what it
// covered, so a game validating what GetUpdateRect gave it validates all of it. Rounded down,
// an inner rectangle at a scale of 2.25 came back a pixel short and WM_PAINT came forever.
LONG FloorDiv(int64_t a, int64_t b) { return LONG(a >= 0 ? a / b : -((-a + b - 1) / b)); }
LONG CeilDiv(int64_t a, int64_t b) { return -FloorDiv(-a, b); }

// One axis of a real screen rectangle [v0, v1) in the mode's [0, n), the image being [o, o + s).
// A non-empty one stays non-empty: a strip beside the image is the mode's pixel on that border.
void SpanToGame(LONG* v0, LONG* v1, LONG o, LONG s, LONG n) {
    const bool any = *v1 > *v0;
    s = std::max<LONG>(s, 1);
    LONG g0 = std::clamp<LONG>(FloorDiv(int64_t(*v0 - o) * n, s), 0, n);
    LONG g1 = std::clamp<LONG>(CeilDiv(int64_t(*v1 - o) * n, s), 0, n);
    if (any && g1 <= g0 && n > 0) {
        g0 = std::min<LONG>(g0, n - 1);
        g1 = g0 + 1;
    }
    *v0 = g0, *v1 = g1;
}

void SpanFromGame(LONG* g0, LONG* g1, LONG o, LONG s, LONG n) {
    *g0 = o + FloorDiv(int64_t(*g0) * s, n);
    *g1 = o + CeilDiv(int64_t(*g1) * s, n);
}

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
    SpanToGame(&rc->left, &rc->right, image.left, image.right - image.left, m.cx);
    SpanToGame(&rc->top, &rc->bottom, image.top, image.bottom - image.top, m.cy);
    return true;
}

// And back. An edge on the mode's border goes to the client area's: ClientRectToGame puts
// the parts around the image on that border, and the whole mode is the whole client area.
bool ClientRectFromGame(HWND h, RECT* rc) {
    RECT image;
    SIZE m;
    HWND w;
    if (!h || !GameImage(&image, &m, &w) || h != w) return false;
    RECT r = *rc;
    SpanFromGame(&r.left, &r.right, image.left, image.right - image.left, m.cx);
    SpanFromGame(&r.top, &r.bottom, image.top, image.bottom - image.top, m.cy);
    // (screen coordinates: into the client's)
    POINT o = {0, 0};
    ClientToScreen(h, &o);
    OffsetRect(&r, -o.x, -o.y);
    RECT client;
    if (!IsRectEmpty(rc) && GetClientRect(h, &client)) {
        if (rc->left <= 0) r.left = std::min(r.left, client.left);
        if (rc->top <= 0) r.top = std::min(r.top, client.top);
        if (rc->right >= m.cx) r.right = std::max(r.right, client.right);
        if (rc->bottom >= m.cy) r.bottom = std::max(r.bottom, client.bottom);
    }
    *rc = r;
    return true;
}

// A region of the game window, its rectangles mapped one by one into a new one (null: none
// made).
HRGN MapRgn(HWND h, HRGN rgn, bool (*map)(HWND, RECT*)) {
    const DWORD size = GetRegionData(rgn, 0, nullptr);
    if (!size) return nullptr;
    std::vector<BYTE> buf(size);
    RGNDATA* data = reinterpret_cast<RGNDATA*>(buf.data());
    if (GetRegionData(rgn, size, data) != size) return nullptr;
    const RECT* rects = reinterpret_cast<const RECT*>(data->Buffer);
    HRGN out = CreateRectRgn(0, 0, 0, 0);
    if (!out) return nullptr;
    for (DWORD i = 0; i < data->rdh.nCount; i++) {
        RECT r = rects[i];
        if (!map(h, &r) || IsRectEmpty(&r)) continue;
        if (HRGN one = CreateRectRgnIndirect(&r)) {
            CombineRgn(out, out, one, RGN_OR);
            DeleteObject(one);
        }
    }
    return out;
}

// The game's region (its mode's coordinates) as one in the client area's, while its window
// is translated (null: passed on as it is).
HRGN RgnFromGame(HWND h, HRGN rgn) {
    return rgn && IsGameWindow(h) ? MapRgn(h, rgn, ClientRectFromGame) : nullptr;
}

HDC WINAPI HookBeginPaint(HWND h, PAINTSTRUCT* ps) {
    DD2D9_PASS_WINDOWS(g_beginPaint, h, ps);
    const HDC dc = DD2D9_NEXT(g_beginPaint)(h, ps);
    if (dc && ps) ClientRectToGame(h, &ps->rcPaint);
    return dc;
}

BOOL WINAPI HookGetUpdateRect(HWND h, RECT* rc, BOOL erase) {
    DD2D9_PASS_WINDOWS(g_getUpdateRect, h, rc, erase);
    const BOOL any = DD2D9_NEXT(g_getUpdateRect)(h, rc, erase);
    if (any && rc) ClientRectToGame(h, rc);
    return any;
}

int WINAPI HookGetUpdateRgn(HWND h, HRGN rgn, BOOL erase) {
    DD2D9_PASS_WINDOWS(g_getUpdateRgn, h, rgn, erase);
    const int type = DD2D9_NEXT(g_getUpdateRgn)(h, rgn, erase);
    if (type != SIMPLEREGION && type != COMPLEXREGION) return type;
    if (!IsGameWindow(h)) return type;
    HRGN game = MapRgn(h, rgn, ClientRectToGame);
    if (!game) return type;
    const int out = CombineRgn(rgn, game, nullptr, RGN_COPY);
    DeleteObject(game);
    return out == ERROR ? type : out;
}

BOOL WINAPI HookInvalidateRect(HWND h, const RECT* rc, BOOL erase) {
    DD2D9_PASS_WINDOWS(g_invalidateRect, h, rc, erase);
    RECT r = rc ? *rc : RECT{};
    if (rc && ClientRectFromGame(h, &r)) return DD2D9_NEXT(g_invalidateRect)(h, &r, erase);
    return DD2D9_NEXT(g_invalidateRect)(h, rc, erase);
}

BOOL WINAPI HookValidateRect(HWND h, const RECT* rc) {
    DD2D9_PASS_WINDOWS(g_validateRect, h, rc);
    RECT r = rc ? *rc : RECT{};
    if (rc && ClientRectFromGame(h, &r)) return DD2D9_NEXT(g_validateRect)(h, &r);
    return DD2D9_NEXT(g_validateRect)(h, rc);
}

BOOL WINAPI HookInvalidateRgn(HWND h, HRGN rgn, BOOL erase) {
    DD2D9_PASS_WINDOWS(g_invalidateRgn, h, rgn, erase);
    HRGN real = RgnFromGame(h, rgn);
    const BOOL ok = DD2D9_NEXT(g_invalidateRgn)(h, real ? real : rgn, erase);
    if (real) DeleteObject(real);
    return ok;
}

BOOL WINAPI HookValidateRgn(HWND h, HRGN rgn) {
    DD2D9_PASS_WINDOWS(g_validateRgn, h, rgn);
    HRGN real = RgnFromGame(h, rgn);
    const BOOL ok = DD2D9_NEXT(g_validateRgn)(h, real ? real : rgn);
    if (real) DeleteObject(real);
    return ok;
}

// (The region, when given, is used and the rectangle not.)
BOOL WINAPI HookRedrawWindow(HWND h, const RECT* rc, HRGN rgn, UINT flags) {
    DD2D9_PASS_WINDOWS(g_redrawWindow, h, rc, rgn, flags);
    if (rgn) {
        HRGN real = RgnFromGame(h, rgn);
        const BOOL ok = DD2D9_NEXT(g_redrawWindow)(h, rc, real ? real : rgn, flags);
        if (real) DeleteObject(real);
        return ok;
    }
    RECT r = rc ? *rc : RECT{};
    if (rc && ClientRectFromGame(h, &r)) return DD2D9_NEXT(g_redrawWindow)(h, &r, nullptr, flags);
    return DD2D9_NEXT(g_redrawWindow)(h, rc, nullptr, flags);
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

// Painted black where nothing else paints it: on the switched screen such a popup only ever
// showed what the game had drawn under it (black, before a movie); a window here shows
// nothing of the screen under it, and parts it doesn't paint showed whatever the compositor
// had there (Road Rash's race intros: the menu above and below the movie). Only for a class
// without a background brush of its own, as that brush would have done it: the popup's
// background erase, when its window procedure left it undone: it returned 0 and drew nothing
// into the DC (many procedures draw their background there and return 0 all the same, which
// on Windows does no harm). (The popup's own procedure, not its class's brush: the game's
// other windows may share the class.)
const wchar_t kPopupProc[] = L"dd2d9 popup procedure";  // the window property holding the popup's own

LRESULT CALLBACK PopupProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const bool unicode = IsWindowUnicode(h);
    const auto own = reinterpret_cast<WNDPROC>(GetPropW(h, kPopupProc));
    if (!own) return unicode ? DefWindowProcW(h, msg, wp, lp) : DefWindowProcA(h, msg, wp, lp);
    const HDC dc = msg == WM_ERASEBKGND ? reinterpret_cast<HDC>(wp) : nullptr;
    const UINT bounds = dc ? SetBoundsRect(dc, nullptr, DCB_ENABLE | DCB_RESET) : 0;  // (0: not known)
    LRESULT r = unicode ? CallWindowProcW(own, h, msg, wp, lp) : CallWindowProcA(own, h, msg, wp, lp);
    if (dc) {
        RECT drawn;
        const bool drew = bounds && (GetBoundsRect(dc, &drawn, 0) & DCB_SET) == DCB_SET;  // (DCB_RESET is a bit of it)
        if (bounds & DCB_DISABLE) SetBoundsRect(dc, nullptr, DCB_DISABLE);
        if (!r && !drew) {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            r = 1;
        }
    } else if (msg == WM_NCDESTROY) {
        RemovePropW(h, kPopupProc);
    }
    return r;
}

void RememberPopup(HWND h) {
    if (!h) return;
    InitPopups();
    EnterCriticalSection(&g_popupLock);
    g_popups->erase(std::remove_if(g_popups->begin(), g_popups->end(), [](HWND p) { return !IsWindow(p); }),
                    g_popups->end());
    g_popups->push_back(h);
    LeaveCriticalSection(&g_popupLock);
    if (!GetClassLongPtrW(h, GCLP_HBRBACKGROUND)) {
        // (Its own character set kept: the A or W call as the window is.)
        const bool unicode = IsWindowUnicode(h);
        const LONG_PTR proc = reinterpret_cast<LONG_PTR>(PopupProc);
        const LONG_PTR own = unicode ? GetWindowLongPtrW(h, GWLP_WNDPROC) : GetWindowLongPtrA(h, GWLP_WNDPROC);
        if (own && own != proc && SetPropW(h, kPopupProc, reinterpret_cast<HANDLE>(own))) {
            if (unicode) SetWindowLongPtrW(h, GWLP_WNDPROC, proc);
            else SetWindowLongPtrA(h, GWLP_WNDPROC, proc);
        }
        DD2D9_LOG("window %p: a popup over the game's screen, put over the image (background black)", static_cast<void*>(h));
    } else {
        DD2D9_LOG("window %p: a popup over the game's screen, put over the image", static_cast<void*>(h));
    }
}

HWND WINAPI HookCreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                                HMENU menu, HINSTANCE inst, LPVOID param) {
    DD2D9_PASS_WINDOWS(g_createWindowExA, ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    const bool popup = PopupRect(style, parent, &x, &y, &w, &h);
    const HWND r = DD2D9_NEXT(g_createWindowExA)(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (popup) RememberPopup(r);
    return r;
}

HWND WINAPI HookCreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent,
                                HMENU menu, HINSTANCE inst, LPVOID param) {
    DD2D9_PASS_WINDOWS(g_createWindowExW, ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    const bool popup = PopupRect(style, parent, &x, &y, &w, &h);
    const HWND r = DD2D9_NEXT(g_createWindowExW)(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (popup) RememberPopup(r);
    return r;
}

}  // namespace

ImportHook kPaintHooks[] = {
    {"user32", "CreateWindowExA", reinterpret_cast<void*>(HookCreateWindowExA), reinterpret_cast<void**>(&g_createWindowExA)},
    {"user32", "CreateWindowExW", reinterpret_cast<void*>(HookCreateWindowExW), reinterpret_cast<void**>(&g_createWindowExW)},
    {"user32", "BeginPaint", reinterpret_cast<void*>(HookBeginPaint), reinterpret_cast<void**>(&g_beginPaint)},
    {"user32", "GetUpdateRect", reinterpret_cast<void*>(HookGetUpdateRect), reinterpret_cast<void**>(&g_getUpdateRect)},
    {"user32", "GetUpdateRgn", reinterpret_cast<void*>(HookGetUpdateRgn), reinterpret_cast<void**>(&g_getUpdateRgn)},
    {"user32", "InvalidateRect", reinterpret_cast<void*>(HookInvalidateRect), reinterpret_cast<void**>(&g_invalidateRect)},
    {"user32", "ValidateRect", reinterpret_cast<void*>(HookValidateRect), reinterpret_cast<void**>(&g_validateRect)},
    {"user32", "InvalidateRgn", reinterpret_cast<void*>(HookInvalidateRgn), reinterpret_cast<void**>(&g_invalidateRgn)},
    {"user32", "ValidateRgn", reinterpret_cast<void*>(HookValidateRgn), reinterpret_cast<void**>(&g_validateRgn)},
    {"user32", "RedrawWindow", reinterpret_cast<void*>(HookRedrawWindow), reinterpret_cast<void**>(&g_redrawWindow)},
};
const size_t kPaintHookCount = sizeof(kPaintHooks) / sizeof(kPaintHooks[0]);

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

}  // namespace dd2d9
