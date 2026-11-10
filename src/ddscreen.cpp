// Where the game's image is on the real screen: the window it is presented in, and the
// snapshot the mouse and GDI functions read it from without waiting for g_lock.
#include "ddint.h"

#include <algorithm>
#include <atomic>

namespace dd2d9 {

HWND DDraw::PresentWindow() const {
    if (deviceWnd_ && IsWindowVisible(deviceWnd_)) return deviceWnd_;  // (see UpdateDeviceWindow)
    if (hwnd_ && IsWindow(hwnd_)) return hwnd_;
    if (primary && primary->clipper && primary->clipper->hwnd && IsWindow(primary->clipper->hwnd))
        return primary->clipper->hwnd;
    return nullptr;
}

RECT DDraw::PresentSource(HWND hwnd) const {
    RECT r = {0, 0, primary ? primary->W() : 0, primary ? primary->H() : 0};
    if (!Exclusive() && hwnd) {
        // Windowed: the primary surface is the desktop; show the part under the window.
        RECT cr;
        GetClientRect(hwnd, &cr);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        r = cr;
    }
    return r;
}

// Where in the window's client area the image goes (client coordinates). In exclusive
// mode with the window covering the screen, that is the screen: ddraw.dll shows the game
// there whatever the window's size. JJ2+ sizes its window past the screen itself while
// starting up (661x544 at 640x480), and the image centered in the whole client area
// left a black bar at the top. Otherwise the whole client area.
RECT DDraw::PresentTarget(HWND hwnd) const {
    RECT c = {};
    GetClientRect(hwnd, &c);
    const int display = DisplayKind();
    if (!Exclusive() || display == kDisplayWindow || hwnd != hwnd_) return c;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(GameMonitor(hwnd), &mi);
    RECT m = mi.rcMonitor;
    MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&m), 2);  // into client coordinates
    RECT t;
    return IntersectRect(&t, &m, &c) ? t : c;
}

bool DDraw::ImageOnScreen(RECT* image, SIZE* mode) const {
    // After a real mode switch the screen is the game's mode: nothing to translate (also
    // not while the display is between two modes).
    // Coming back from the background (OnActivate, restoring_), the window is set up again
    // under the game's messages (WM_SIZE, WM_ACTIVATE): those see its screen already.
    if (!Exclusive() || !modeSet_ || activeDisplay_ == kDisplaySwitch || (suspended_ && !restoring_) || !hwnd_ ||
        !IsWindow(hwnd_) || IsIconic(hwnd_))
        return false;
    const RECT t = PresentTarget(hwnd_);
    RECT r = presenter_->FitRect(modeW_, modeH_, UINT(t.right - t.left), UINT(t.bottom - t.top));
    OffsetRect(&r, t.left, t.top);
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&r), 2);
    // 1:1 (the window the mode's size at 0,0: a game sizing it back, before CheckCover covers
    // the screen again, or a monitor of the mode's size) is translated too: the cursor is kept
    // on the game's screen, as on a real one of that size. Xuan-Yuan Sword 3 EX draws its
    // cursor where GetCursorPos says, unclipped: a cursor past 640x480 wrote past its back
    // buffer (a crash back from Alt+Tab, while its window was 640x480).
    *image = r;
    *mode = SIZE{LONG(modeW_), LONG(modeH_)};
    return true;
}

// The exclusive object's image, for the mouse (see input.cpp): a snapshot, so the cursor
// functions and mouse messages (many a frame) neither wait for g_lock nor ask Windows for
// the window's geometry each time. Made again after GameImageChanged (the display state
// or the game window's position changed), and at least every 250 ms in case a change went
// unseen.
namespace {
struct ImageSnap {
    bool ok;
    RECT image;
    SIZE mode;
    HWND window;
};
SRWLOCK g_snapLock = SRWLOCK_INIT;  // (innermost: held for the copy only)
ImageSnap g_snap = {};
std::atomic<bool> g_snapDirty{true};
std::atomic<ULONGLONG> g_snapTime{0};

void TakeSnapshot(ULONGLONG now) {  // g_lock held
    g_snapDirty.store(false);  // first: a change while it's made marks it again
    ImageSnap s = {};
    const DDraw* d = g_exclusiveDD;
    if (d && d->ImageOnScreen(&s.image, &s.mode)) {
        s.ok = true;
        s.window = d->Window();
    }
    AcquireSRWLockExclusive(&g_snapLock);  // under g_lock: snapshots stored in the order made
    g_snap = s;
    ReleaseSRWLockExclusive(&g_snapLock);
    g_snapTime.store(now);
}
}  // namespace

void GameImageChanged() { g_snapDirty.store(true); }

// desktopbpp=: the colour depth GDI and EnumDisplaySettings report (compatdisplay.cpp): the game's
// display mode's while it has one, else the desktop's as desktopbpp= says. Never waits for
// g_lock (GDI is called from window procedures): the last answer when another thread has it.
namespace {
std::atomic<int> g_shownBpp{0};
std::atomic<DWORD> g_shownSize{0};  // a display mode of dd2d9's own (EmulatedMode): width << 16 | height

void UpdateShownDisplay() {
    TryGuard l;
    if (!l.owns()) return;  // the last answer
    const DDraw* d = g_exclusiveDD;
    DWORD w = 0, h = 0, bpp;
    if (d) d->DisplayMode(&w, &h, &bpp);
    else bpp = DWORD(g_config.desktopBpp);
    const bool own = d && d->EmulatedMode() && w && h && w < 0x10000 && h < 0x10000;
    g_shownBpp.store(int(bpp));
    g_shownSize.store(own ? w << 16 | h : 0);
}
}  // namespace

int ShownDisplayBpp() {
    UpdateShownDisplay();
    const int b = g_shownBpp.load();
    return b ? b : g_config.desktopBpp;
}

// The size of the game's display mode while it is one of dd2d9's own (shown scaled or in a
// window: the real screen is another size); false otherwise (the real screen is the mode).
// Never waits for g_lock, as ShownDisplayBpp.
bool ShownDisplaySize(int* w, int* h) {
    UpdateShownDisplay();
    const DWORD v = g_shownSize.load();
    if (!v) return false;
    *w = int(v >> 16);
    *h = int(v & 0xFFFF);
    return true;
}

bool GameImage(RECT* image, SIZE* mode, HWND* window, bool wait) {
    const ULONGLONG now = GetTickCount64();
    const bool dirty = g_snapDirty.load();
    if (dirty || now - g_snapTime.load() > 250) {
        TryGuard l;
        if (l.owns()) {
            TakeSnapshot(now);
        } else if (dirty && wait) {  // after a change: as it is now (as before the snapshot)
            Guard wl;
            TakeSnapshot(now);
        }  // else the last one (the periodic check, or a window procedure, which never waits)
    }
    AcquireSRWLockShared(&g_snapLock);
    const ImageSnap s = g_snap;
    ReleaseSRWLockShared(&g_snapLock);
    if (!s.ok) return false;
    *image = s.image;
    *mode = s.mode;
    if (window) *window = s.window;
    return true;
}

// The real screen to the game's display mode (its screen as the game sees it, at 0,0):
// scaled, and kept on the game's screen as the real cursor is kept on the real one.
bool MouseToGame(POINT* p, bool wait) {
    RECT r;
    SIZE m;
    if (!GameImage(&r, &m, nullptr, wait)) return false;
    const LONG w = std::max<LONG>(r.right - r.left, 1), h = std::max<LONG>(r.bottom - r.top, 1);
    p->x = std::clamp<LONG>(LONG(int64_t(p->x - r.left) * m.cx / w), 0, m.cx - 1);
    p->y = std::clamp<LONG>(LONG(int64_t(p->y - r.top) * m.cy / h), 0, m.cy - 1);
    return true;
}

// The game's display mode to the real screen (the middle of the pixels a game pixel covers).
bool MouseFromGame(POINT* p) {
    RECT r;
    SIZE m;
    if (!GameImage(&r, &m)) return false;
    const LONG w = r.right - r.left, h = r.bottom - r.top;
    p->x = r.left + LONG((int64_t(p->x) * w + w / 2) / std::max<LONG>(m.cx, 1));
    p->y = r.top + LONG((int64_t(p->y) * h + h / 2) / std::max<LONG>(m.cy, 1));
    return true;
}

}  // namespace dd2d9
