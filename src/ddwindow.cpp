// The exclusive window: focus changes, covering the monitor, the window's saved state, and
// DDSCL_CREATEDEVICEWINDOW's device window.
#include "ddint.h"

#include <windowsx.h>

#include <algorithm>

namespace dd2d9 {

namespace {
// For the log: the program a process id belongs to ("?" if it can't be had).
void ProcessName(DWORD pid, char* out, size_t size) {
    snprintf(out, size, "?");
    if (!pid) return;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return;
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        snprintf(out, size, "%ls", name ? name + 1 : path);
    }
    CloseHandle(p);
}

// For the log: what took the focus from the game. WM_ACTIVATEAPP names the thread
// whose window is being activated; once the switch is done, the foreground window.
void LogFocusThief(DWORD thread, HWND fg) {
    DWORD pid = 0;
    if (thread) {
        if (HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread)) {
            pid = GetProcessIdOfThread(t);
            CloseHandle(t);
        }
    }
    char name[MAX_PATH];
    ProcessName(pid, name, sizeof(name));
    DD2D9_LOG("  activated: thread %04lx, process %lu (%s)%s", thread, pid, name,
              pid && pid == GetCurrentProcessId() ? " - the game's own" : "");
    if (!fg) return;
    DWORD fgPid = 0;
    const DWORD fgThread = GetWindowThreadProcessId(fg, &fgPid);
    char cls[128] = "", title[128] = "";
    GetClassNameA(fg, cls, sizeof(cls));
    GetWindowTextA(fg, title, sizeof(title));
    ProcessName(fgPid, name, sizeof(name));
    DD2D9_LOG("  foreground: window %p, class \"%s\", title \"%s\", thread %04lx, process %lu (%s)",
              static_cast<void*>(fg), cls, title, fgThread, fgPid, name);
}
}  // namespace

// DDSCL_NOWINDOWCHANGES asks ddraw.dll to leave the window alone: it changes the display
// mode only, and the game's window (made the mode's size) fills the screen through that. With
// display=borderless there is no mode change for it to rely on: the game's 640x480 window sat
// in the corner of a 1366x768 desktop (Hydro Thunder, coop 0x55). There the window covers the
// screen anyway, as the switched mode would have shown it, and is restacked and minimized as
// any covering window is; switch and window leave it alone, as ddraw.dll does.
bool DDraw::WindowChanges() const {
    if (!(coop_ & DDSCL_NOWINDOWCHANGES)) return true;
    return DisplayKind() == kDisplayBorderless;
}

// Called with g_displayLock held.
void DDraw::KeepTopmost() {
    if (!Exclusive() || !alive || applying_ || suspended_ || deviceState_ == kDevLost || !hwnd_ ||
        !WindowChanges() || IsIconic(hwnd_))
        return;
    if (DisplayKind() == kDisplayWindow) return;
    if (GetWindowLongW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) return;
    // In front only: a game that took exclusive mode behind another program (its
    // SetForegroundWindow refused) stays behind it, as ApplyWindow left it.
    DWORD pid = 0;
    const HWND fg = GetForegroundWindow();
    if (fg && fg != hwnd_ && (GetWindowThreadProcessId(fg, &pid), pid != GetCurrentProcessId())) return;
    DD2D9_LOG("window %p was no longer topmost: made topmost again", static_cast<void*>(hwnd_));
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

// Minimized on focus loss, as ddraw.dll does, so that it can be found again: a minimized
// window is left with its taskbar button and its place in Alt+Tab. A window that has neither
// would be lost, a small title bar in the corner of the desktop behind the other windows:
// - with an owner (Visual Basic's forms can be owned by its hidden main window, which has
//   the button), the owner is minimized, the window hidden with it and shown again with it;
//   an owner that isn't visible has no button either: the window is left as it is then;
// - a window the taskbar has no button for is given one while in the background
//   (WS_EX_APPWINDOW, shown again so that the taskbar sees it; its own style back with the
//   focus, see OnActivate): a tool window (WS_EX_TOOLWINDOW, or WS_EX_NOACTIVATE, which the
//   taskbar skips too), or one Windows minimized onto the desktop, where it puts a window it
//   has no button for (with a button, off the screen). Pal95's window (Visual Basic 4),
//   unowned and of no special style, had none and couldn't be reached again.
void DDraw::MinimizeGameWindow() {
    const HWND owner = GetAncestor(hwnd_, GA_ROOTOWNER);
    const LONG ex = GetWindowLongW(hwnd_, GWL_EXSTYLE);
    DD2D9_LOG("focus lost: window extended style 0x%lx, owner %p", static_cast<unsigned long>(ex),
              static_cast<void*>(owner != hwnd_ ? owner : nullptr));
    if (!owner || owner == hwnd_) {
        const LONG skipped = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        ShowWindow(hwnd_, SW_SHOWMINNOACTIVE);
        MINIMIZEDMETRICS mm = {};
        mm.cbSize = sizeof(mm);
        RECT r = {};
        const bool parked = SystemParametersInfoW(SPI_GETMINIMIZEDMETRICS, sizeof(mm), &mm, 0) &&
                            (mm.iArrange & ARW_HIDE) && IsIconic(hwnd_) && GetWindowRect(hwnd_, &r) && r.left > -10000;
        if (((ex & skipped) && !(ex & WS_EX_APPWINDOW)) || parked) {
            DD2D9_LOG("focus lost: no taskbar button (%s): given one while in the background",
                      parked ? "minimized onto the desktop" : "a tool window");
            ShowWindow(hwnd_, SW_HIDE);
            SetWindowLongW(hwnd_, GWL_EXSTYLE, (GetWindowLongW(hwnd_, GWL_EXSTYLE) & ~skipped) | WS_EX_APPWINDOW);
            taskbarEx_ = ex & (skipped | WS_EX_APPWINDOW);
            taskbarExChanged_ = true;
            ShowWindow(hwnd_, SW_SHOWMINNOACTIVE);
        }
    } else if (IsWindowVisible(owner)) {
        DD2D9_LOG("focus lost: minimizing the window's owner %p", static_cast<void*>(owner));
        ShowWindow(owner, SW_SHOWMINNOACTIVE);
        // Windows hides the owned windows with it; where it hasn't (Wine), hidden here.
        if (IsWindowVisible(hwnd_)) {
            ShowWindow(hwnd_, SW_HIDE);
            hiddenWithOwner_ = true;
        }
    } else {
        DD2D9_LOG("focus lost: the window's owner %p is hidden: not minimized", static_cast<void*>(owner));
    }
}

// Called with g_displayLock held.
void DDraw::OnActivate(bool active, DWORD thread) {
    if (applying_ || !Exclusive() || !alive) return;
    // Before SetDisplayMode, the window covers the screen at the desktop mode (see ApplyWindow).
    const int display = DisplayKind();
    if (!active) {
        backgrounded_ = true;  // see SetCooperativeLevel: leaving exclusive mode now loses the surfaces
        ConfineCursor(false);
        // Window mode is an ordinary window; switch and borderless cover the screen.
        const bool cover = display != kDisplayWindow && !suspended_;
        // DDSCL_NOWINDOWCHANGES: the mode goes back, the window is left alone (as ddraw.dll:
        // neither minimized nor made non-topmost; spec: re/spec/window-procedure.md), unless
        // borderless covered it anyway (WindowChanges).
        const bool windowChanges = WindowChanges();
        if (cover) {
            DD2D9_LOG("focus lost: %s", switched_ ? "restoring the desktop display mode" : "minimizing");
            suspended_ = true;      // first: the presenter must not create a new device from here on
            GameImageChanged();
            if (switched_) {
                ChangeDisplay(nullptr, 0);
                switched_ = false;
                UpdateRefreshRate();
            }
            if (windowChanges) SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        // From here on (as in ddraw.dll, once the desktop mode is back) the surfaces are
        // lost and can't be restored; back in front they can (see UpdateLostSurfaces).
        // Window mode stays an ordinary, visible window: the game keeps running.
        if (display != kDisplayWindow) {
            InterlockedExchange(&deviceState_, kDevLost);
            focusLossOnly_ = true;  // unless a mode change comes on top (InvalidateSurfaces)
        }
        // Minimizing takes the keyboard focus away (WM_KILLFOCUS): the game sees it lost.
        if (cover && windowChanges && IsWindowVisible(hwnd_)) MinimizeGameWindow();  // a hidden one stays so
        if (cover) UpdateDeviceWindow();  // hidden
        if (cover) LogFocusThief(thread, GetForegroundWindow());
        return;
    }
    InterlockedCompareExchange(&deviceState_, kDevNotRestored, kDevLost);
    if (suspended_) {
        DD2D9_LOG("focus regained: setting up %lux%lu again", modeW_, modeH_);
        // The game's screen from here on, for what it asks while its window comes back (see
        // ImageOnScreen): Heroes of the Three Kingdoms 3, handling its restore, read the
        // monitor's size and blitted its movie centred in a 1920x1080 screen.
        restoring_ = true;
        GameImageChanged();
        if (WindowChanges()) {
            if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
            // Its owner, minimized on focus loss (MinimizeGameWindow), back too, and the
            // window with it; restored from its taskbar button, the owner passes the focus on
            // to the window, as clicking the window would.
            const HWND owner = GetAncestor(hwnd_, GA_ROOTOWNER);
            if (owner && owner != hwnd_) {
                if (IsIconic(owner)) ShowWindow(owner, SW_SHOWNOACTIVATE);
                if (hiddenWithOwner_ && !IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOWNA);
                if (GetForegroundWindow() == owner) SetForegroundWindow(hwnd_);
            }
            hiddenWithOwner_ = false;
            // Its own style back, given a taskbar button in the background (MinimizeGameWindow;
            // the button stays until it is next hidden).
            if (taskbarExChanged_) {
                const LONG bits = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_APPWINDOW;
                SetWindowLongW(hwnd_, GWL_EXSTYLE, (GetWindowLongW(hwnd_, GWL_EXSTYLE) & ~bits) | taskbarEx_);
                taskbarExChanged_ = false;
            }
        }
        ApplyWindow();
        // Only now: until the mode is switched back the presenter must not create
        // a device (it would make a windowed one, then replace it right away).
        suspended_ = false;
        restoring_ = false;
        // Painted by the game again: what dd2d9 answered for it while away (see HookProc).
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateDeviceWindow();
        GameImageChanged();
        MarkDirty();
    } else if (display != kDisplayWindow && WindowChanges()) {
        // ddraw.dll makes the exclusive window topmost once it is in front.
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ConfineCursor(true);
    }
}

// WM_DISPLAYCHANGE on the exclusive window (ours or anyone's mode change). Called with
// g_displayLock held.
void DDraw::OnDisplayChange() {
    const ImageChangedAtExit imageChanged;  // (the window may move from another thread)
    if (!Exclusive() || !alive || !WindowChanges() || !hwnd_ || IsIconic(hwnd_)) return;
    const int display = DisplayKind();
    if (display == kDisplayWindow) return;
    CoverMonitor(HWND_TOP, SWP_NOACTIVATE);
    UpdateDeviceWindow();  // (the monitor's new size)
    // As ddraw.dll: the cursor stays on the (possibly resized) screen while in front.
    if (GetForegroundWindow() == hwnd_) ConfineCursor(true);
}

// The monitor the game's screen is on: the primary one, as with ddraw.dll, whose default
// device is the primary display (its fullscreen window goes there, wherever it was), for a
// real switch (whose mode ChangeDisplaySettings changes) and for display=borderless alike;
// the window's own for display=window. (A game on the second monitor had the first one
// switched and its picture on the second; Rockman X4's borderless screen came up on either
// monitor, wherever Windows had put its window.)
HMONITOR DDraw::GameMonitor(HWND h) const {
    if (DisplayKind() != kDisplayWindow) {
        const POINT origin = {0, 0};
        return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }
    return MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY);
}

// The window covers its monitor in exclusive mode. ddraw.dll leaves the window's style
// alone and draws over the whole screen, frame and all; dd2d9 presents into the client
// area, so the window keeps its style (as games read it) and grows until its client
// area is the monitor, the frame just outside it. A window region shows only that part,
// so the frame can't appear on a monitor next to this one. g_displayLock held.
void DDraw::CoverMonitor(HWND insertAfter, UINT flags) {
    const ImageChangedAtExit imageChanged;  // (the window may move from another thread)
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(GameMonitor(hwnd_), &mi);
    const RECT m = mi.rcMonitor;
    const LONG style = GetWindowLongW(hwnd_, GWL_STYLE), exStyle = GetWindowLongW(hwnd_, GWL_EXSTYLE);
    RECT r = m;
    AdjustWindowRectEx(&r, DWORD(style), GetMenu(hwnd_) != nullptr, DWORD(exStyle));
    coverHwnd_ = hwnd_;
    coverSize_ = {r.right - r.left, r.bottom - r.top};  // first: WM_GETMINMAXINFO lets it be this large
    struct Covering {
        bool& flag;
        explicit Covering(bool& f) : flag(f) { flag = true; }
        ~Covering() { flag = false; }
    } covering(covering_);
    SetWindowPos(hwnd_, insertAfter, r.left, r.top, coverSize_.cx, coverSize_.cy, flags);
    // Minimized (a game going exclusive again from its own activation, before Windows has
    // restored the window): its rectangle and client area are the icon's. No region from
    // them: one worked out at -32000,-32000 hid the whole restored window (FlyingSB after
    // Alt+Tab: nothing shown). CheckCover covers it again once it is restored.
    if (IsIconic(hwnd_)) {
        SetWindowRgn(hwnd_, nullptr, TRUE);
        return;
    }
    // A frame other than AdjustWindowRectEx's (a game handling WM_NCCALCSIZE, a menu bar
    // on two lines): corrected by where the client area really is.
    RECT c = {};
    GetClientRect(hwnd_, &c);
    POINT o = {0, 0};
    ClientToScreen(hwnd_, &o);
    const LONG dx = m.left - o.x, dy = m.top - o.y;
    const LONG dw = (m.right - m.left) - c.right, dh = (m.bottom - m.top) - c.bottom;
    if (dx || dy || dw || dh) {
        r.left += dx;
        r.top += dy;
        coverSize_.cx += dw;
        coverSize_.cy += dh;
        SetWindowPos(hwnd_, nullptr, r.left, r.top, coverSize_.cx, coverSize_.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    coverPos_ = {r.left, r.top};
    RECT w = {};
    GetWindowRect(hwnd_, &w);
    if (HRGN rgn = CreateRectRgn(m.left - w.left, m.top - w.top, m.right - w.left, m.bottom - w.top)) {
        if (!SetWindowRgn(hwnd_, rgn, TRUE)) DeleteObject(rgn);  // owned by the window once set
    }
}

// Undoes CoverMonitor: the window shrinks to the monitor, the size ddraw.dll leaves it
// (frame on screen again), and loses its region. Window mode then sizes it its own way.
void DDraw::Uncover() {
    ConfineCursor(false);
    if (coverHwnd_ && IsWindow(coverHwnd_)) {
        RECT w = {};
        GetWindowRect(coverHwnd_, &w);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(GameMonitor(coverHwnd_), &mi);
        const RECT& m = mi.rcMonitor;
        coverSize_ = {};  // first: WM_GETMINMAXINFO back to normal
        // Unless the game has moved or sized it since.
        if (!IsIconic(coverHwnd_) && w.left <= m.left && w.top <= m.top && w.right >= m.right && w.bottom >= m.bottom)
            SetWindowPos(coverHwnd_, nullptr, m.left, m.top, m.right - m.left, m.bottom - m.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowRgn(coverHwnd_, nullptr, TRUE);
    }
    coverHwnd_ = nullptr;
    coverSize_ = {};
}

// As ddraw.dll: while the exclusive window covers its monitor and is in front, the
// mouse stays on that monitor (with two monitors, a click on the other one would send
// the game to the background). on = false lets it go again, if dd2d9 confined it.
// Meanwhile other programs can't bring their windows to the front on their own (a
// notification or a background program's window taking the focus, which sends the
// game to the background as Alt+Tab would): the user still can, with Alt+Tab or a click.
void DDraw::ConfineCursor(bool on) {
    if (on && hwnd_) {
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(GameMonitor(hwnd_), &mi);
        cursorConfined_ = ClipCursor(&mi.rcMonitor) != FALSE;
        // Only the process in front may lock it; a failure leaves things as they were.
        if (!foregroundLocked_) foregroundLocked_ = LockSetForegroundWindow(LSFW_LOCK) != FALSE;
        return;
    }
    if (cursorConfined_) {
        ClipCursor(nullptr);
        cursorConfined_ = false;
    }
    if (foregroundLocked_) {
        LockSetForegroundWindow(LSFW_UNLOCK);
        foregroundLocked_ = false;
    }
}

// The game changed its window's frame in exclusive mode (posted from the window procedure;
// g_displayLock held). CoverMonitor fitted the client area to the screen for the old frame:
// JJ2+ takes its frame away while starting up, which left the client area larger than the
// screen and the image off-center (a black bar at the top). The window covers the screen
// again for the new frame. A move or resize alone (`moved`) stays the game's, as ddraw.dll
// leaves it, unless the game's display mode is shown scaled (borderless): a window that
// covers the game's screen there (at its corner, at least the mode's size: JJ2+ sizes its
// window to 661x544 now and then in a 640x480 mode, which a real 640x480 display shows as
// fullscreen) covers the real one again. A game that keeps changing it wins after a few
// tries rather than the two fighting.
void DDraw::CheckCover(HWND h, bool moved) {
    const ImageChangedAtExit imageChanged;  // (the window may move from another thread)
    if (!Exclusive() || !alive || h != coverHwnd_ || !coverSize_.cx || applying_ || suspended_ ||
        !WindowChanges() || IsIconic(h) || !IsWindowVisible(h))
        return;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(GameMonitor(h), &mi);
    RECT c = {};
    GetClientRect(h, &c);
    MapWindowPoints(h, nullptr, reinterpret_cast<POINT*>(&c), 2);
    // Covering, and its region showing the monitor (none: made while it was minimized).
    RECT w = {}, box = {};
    GetWindowRect(h, &w);
    const RECT& m = mi.rcMonitor;
    const RECT want = {m.left - w.left, m.top - w.top, m.right - w.left, m.bottom - w.top};
    const int region = GetWindowRgnBox(h, &box);
    const bool regionOk = region == SIMPLEREGION && EqualRect(&box, &want);
    const bool covers = EqualRect(&c, &mi.rcMonitor) != FALSE;
    if (covers && regionOk) return;
    if (moved && !covers) {
        if (!modeSet_ || activeDisplay_ != kDisplayBorderless) return;
        const LONG x = mi.rcMonitor.left, y = mi.rcMonitor.top;
        if (w.left > x || w.top > y || w.right < x + LONG(modeW_) || w.bottom < y + LONG(modeH_)) return;  // smaller: windowed
    }
    // Tries counted until 2 s pass without one: a game that keeps resizing wins.
    const DWORD now = GetTickCount();
    if (now - recoverStart_ > 2000) recoverCount_ = 0;
    recoverStart_ = now;
    if (++recoverCount_ > 5) return;
    DD2D9_LOG("the window no longer covers the screen (client %ld,%ld-%ld,%ld%s): covering it again", c.left, c.top,
              c.right, c.bottom, covers ? ", its region not the screen's" : "");
    // Without the game seeing the move: Unreal answers it by sizing its window back to its
    // own idea of fullscreen (after a style change), leaving the taskbar showing.
    quietMove_ = true;
    CoverMonitor(nullptr, SWP_NOZORDER | SWP_NOACTIVATE);
    quietMove_ = false;
}

// The game sizing its window while borderless covers the screen with it: one sized to cover
// the game's screen (at the monitor's corner, at least the mode's size: what a real switch to
// that mode shows as fullscreen) is given the covering size instead, before it changes.
// CheckCover does the same afterwards, but from a posted message, which a game that doesn't
// read its messages while it loads sees late: Hydro Thunder sizes its window to 640x480
// right after SetDisplayMode, and showed in the corner for its first two seconds. A smaller
// window (the game going windowed) is left alone. With a frame change (games set their style,
// then SetWindowPos with SWP_FRAMECHANGED: Hydro Thunder still started in the corner) the
// covering rectangle is worked out for the new frame (the style is set by now); CheckCover
// checks the client area once the game has handled it. Returns whether the position was
// changed. g_displayLock held.
bool DDraw::FitCover(HWND h, WINDOWPOS* wp) {
    if (!Exclusive() || !alive || covering_ || h != coverHwnd_ || !coverSize_.cx || applying_ || suspended_ ||
        !WindowChanges() || !modeSet_ || activeDisplay_ != kDisplayBorderless || IsIconic(h))
        return false;
    if ((wp->flags & (SWP_NOSIZE | SWP_NOMOVE)) == (SWP_NOSIZE | SWP_NOMOVE)) return false;
    RECT w = {};
    GetWindowRect(h, &w);
    const LONG x = (wp->flags & SWP_NOMOVE) ? w.left : wp->x, y = (wp->flags & SWP_NOMOVE) ? w.top : wp->y;
    const LONG cx = (wp->flags & SWP_NOSIZE) ? w.right - w.left : wp->cx;
    const LONG cy = (wp->flags & SWP_NOSIZE) ? w.bottom - w.top : wp->cy;
    if (x == coverPos_.x && y == coverPos_.y && cx == coverSize_.cx && cy == coverSize_.cy) return false;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(GameMonitor(h), &mi);
    const LONG mx = mi.rcMonitor.left, my = mi.rcMonitor.top;
    if (x > mx || y > my || x + cx < mx + LONG(modeW_) || y + cy < my + LONG(modeH_)) return false;  // smaller: windowed
    DD2D9_LOG("the game sizes its window to %ldx%ld at %ld,%ld: kept covering the screen", cx, cy, x, y);
    if (wp->flags & SWP_FRAMECHANGED) {
        RECT r = mi.rcMonitor;
        AdjustWindowRectEx(&r, DWORD(GetWindowLongW(h, GWL_STYLE)), GetMenu(h) != nullptr,
                           DWORD(GetWindowLongW(h, GWL_EXSTYLE)));
        coverPos_ = {r.left, r.top};
        coverSize_ = {r.right - r.left, r.bottom - r.top};  // (WM_GETMINMAXINFO lets it be this large)
    }
    wp->x = coverPos_.x;
    wp->y = coverPos_.y;
    wp->cx = coverSize_.cx;
    wp->cy = coverSize_.cy;
    wp->flags &= ~UINT(SWP_NOMOVE | SWP_NOSIZE);
    return true;
}

// For WM_GETMINMAXINFO: the window may grow past the screen by its frame.
bool DDraw::CoverSize(HWND h, SIZE* out) const {
    if (h != coverHwnd_ || !coverSize_.cx) return false;
    *out = coverSize_;
    return true;
}

// Exclusive mode: set the display up the way the configuration asks.
//   switch:     change the real display mode and cover the screen (original DirectDraw behaviour);
//               falls back to borderless if the monitor refuses the mode
//   window:     no mode change; a window with a client area of the mode size, centered
//   borderless: no mode change; a borderless window covering the monitor, image scaled
// Called with g_displayLock held and g_lock not held.
void DDraw::ApplyWindow() {
    const ImageChangedAtExit imageChanged;  // (the window may move from another thread)
    if (!Exclusive() || !hwnd_ || !IsWindow(hwnd_)) return;
    int display = g_config.display;
    // Before SetDisplayMode: as in ddraw.dll, switch and borderless cover the screen at the
    // desktop mode; window mode has nothing to show yet and leaves the window alone.
    if (!modeSet_ && display == kDisplayWindow) return;
    applying_ = true;  // focus changes caused by our own mode / window changes are not the user's
    // DDSCL_NOWINDOWCHANGES: the display mode still changes, the window doesn't (but with
    // borderless, which has no mode change, it does: see WindowChanges).
    const bool mayChange = !(coop_ & DDSCL_NOWINDOWCHANGES);
    // Saved before the mode switch, which may move or clamp the window.
    if (mayChange || display == kDisplayBorderless) SaveWindow();
    if (modeSet_ && display == kDisplaySwitch && !SwitchMode()) {
        DD2D9_LOG("display mode %lux%lu not available, using a borderless scaled window", modeW_, modeH_);
        display = kDisplayBorderless;
        if (!mayChange) SaveWindow();  // (borderless covers it after all)
    }
    if (display != kDisplaySwitch && switched_) {
        ChangeDisplay(nullptr, 0);
        switched_ = false;
        UpdateRefreshRate();
    }
    activeDisplay_ = display;
    GameImageChanged();
    // The game's mode shown scaled or in a window: its cursor functions are pointed at
    // dd2d9's (input.cpp), which show it the mode as its screen. A real mode switch
    // leaves its imports alone. Again for each mode: modules loaded since are covered.
    if (modeSet_ && display != kDisplaySwitch) {
        InstallMouseHooks();
        InstallVideoHooks();
    }
    if (!mayChange && display != kDisplayBorderless) {
        applying_ = false;
        return;
    }

    LONG style = GetWindowLongW(hwnd_, GWL_STYLE);
    LONG exStyle = GetWindowLongW(hwnd_, GWL_EXSTYLE);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(GameMonitor(hwnd_), &mi);
    if (display == kDisplayWindow) {
        Uncover();
        RECT r = {0, 0, LONG(modeW_), LONG(modeH_)};
        AdjustWindowRectEx(&r, DWORD(style), GetMenu(hwnd_) != nullptr, DWORD(exStyle));
        const RECT& wa = mi.rcWork;
        int w = r.right - r.left, h = r.bottom - r.top;
        int x = std::max<int>(wa.left, wa.left + ((wa.right - wa.left) - w) / 2);
        int y = std::max<int>(wa.top, wa.top + ((wa.bottom - wa.top) - h) / 2);
        SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_SHOWWINDOW);
    } else {
        // As ddraw.dll: if it's in front, shown (without activating it) and topmost; one
        // behind another window keeps its place, and stays hidden if it is, until it gets
        // there (see OnActivate).
        const HWND fg = GetForegroundWindow();
        const bool front = fg == hwnd_ || !fg || fg == GetDesktopWindow();  // not behind another window
        CoverMonitor(front ? HWND_TOPMOST : nullptr,
                     SWP_NOACTIVATE | (front ? SWP_SHOWWINDOW : SWP_NOZORDER));
        if (front) ConfineCursor(true);
        // The mode's size, as the window sized to it would have told the game (see g_toldSize).
        if (modeSet_ && g_toldSize != LONG(MAKELPARAM(modeW_, modeH_)) && !IsIconic(hwnd_)) {
            DD2D9_LOG("the game's window is told its mode's size, %lux%lu (WM_SIZE)", modeW_, modeH_);
            // (Sent from the window's own thread, as SetWindowPos would; posted from another,
            // which may be waiting for a lock held here.)
            if (GetWindowThreadProcessId(hwnd_, nullptr) == GetCurrentThreadId())
                SendMessageW(hwnd_, WM_SIZE, SIZE_RESTORED, MAKELPARAM(modeW_, modeH_));
            else
                PostMessageW(hwnd_, WM_SIZE, SIZE_RESTORED, MAKELPARAM(modeW_, modeH_));
        }
    }
    applying_ = false;
    static const char* kNames[] = {"display mode switched", "centered window", "borderless window"};
    if (modeSet_) DD2D9_LOG("%lux%lu: %s", modeW_, modeH_, kNames[display]);
    else DD2D9_LOG("exclusive mode: the window covers the screen");
}

// Leaving exclusive mode: the window gets its style back, keeping WS_VISIBLE and
// WS_EX_TOPMOST as they are now; then (unless DDSCL_NOWINDOWCHANGES) it stops being
// topmost. ddraw.dll leaves it the size it had; window mode, which moved it, puts it back.
void DDraw::RestoreSavedWindow(bool windowChanges) {
    Uncover();
    if (windowSaved_ && IsWindow(savedHwnd_)) {
        const LONG keep = WS_VISIBLE | WS_MINIMIZE | WS_MAXIMIZE;
        const LONG style = GetWindowLongW(savedHwnd_, GWL_STYLE), exStyle = GetWindowLongW(savedHwnd_, GWL_EXSTYLE);
        SetWindowLongW(savedHwnd_, GWL_STYLE, (savedStyle_ & ~keep) | (style & keep));
        SetWindowLongW(savedHwnd_, GWL_EXSTYLE, (savedExStyle_ & ~WS_EX_TOPMOST) | (exStyle & WS_EX_TOPMOST));
        UINT flags = SWP_FRAMECHANGED | SWP_NOACTIVATE | (windowChanges ? 0 : SWP_NOZORDER);
        const RECT& r = savedRect_;
        if (activeDisplay_ != kDisplayWindow) flags |= SWP_NOMOVE | SWP_NOSIZE;
        SetWindowPos(savedHwnd_, windowChanges ? HWND_NOTOPMOST : nullptr, r.left, r.top, r.right - r.left,
                     r.bottom - r.top, flags);
    }
    windowSaved_ = false;
    savedHwnd_ = nullptr;
}

void DDraw::SaveWindow() {
    if (windowSaved_ && savedHwnd_ == hwnd_) return;
    RestoreSavedWindow(true);  // exclusive mode moved to another window: give the old one back first
    savedHwnd_ = hwnd_;
    savedStyle_ = GetWindowLongW(hwnd_, GWL_STYLE);
    savedExStyle_ = GetWindowLongW(hwnd_, GWL_EXSTYLE);
    GetWindowRect(hwnd_, &savedRect_);
    windowSaved_ = true;
}

// DDSCL_CREATEDEVICEWINDOW's window, as ddraw.dll registers and makes it: WM_CREATE keeps the
// focus window (its extra bytes), WM_SETFOCUS passes the focus there, no cursor over it.
// Unlike ddraw.dll's, mouse messages go on to the focus window (in its client coordinates;
// the wheel's are the screen's already): the game handles them there, as it does when it
// covers the screen itself.
static LRESULT CALLBACK DeviceWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
        const HWND focus = reinterpret_cast<HWND>(GetWindowLongPtrA(w, 0));
        if (IsWindow(focus)) {
            // This class takes double-clicks; a focus window's without CS_DBLCLKS gets the
            // second click as a plain one, as its own clicks come.
            if (!(GetClassLongPtrA(focus, GCL_STYLE) & CS_DBLCLKS)) {
                if (msg == WM_LBUTTONDBLCLK) msg = WM_LBUTTONDOWN;
                else if (msg == WM_RBUTTONDBLCLK) msg = WM_RBUTTONDOWN;
                else if (msg == WM_MBUTTONDBLCLK) msg = WM_MBUTTONDOWN;
                else if (msg == WM_XBUTTONDBLCLK) msg = WM_XBUTTONDOWN;
            }
            if (msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL) {
                POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                MapWindowPoints(w, focus, &pt, 1);
                lp = MAKELPARAM(pt.x, pt.y);
            }
            PostMessageA(focus, msg, wp, lp);
            return 0;
        }
    }
    if (msg == WM_CREATE) {
        SetWindowLongPtrA(w, 0, LONG_PTR(reinterpret_cast<CREATESTRUCTA*>(lp)->lpCreateParams));
    } else if (msg == WM_SETFOCUS) {
        if (HWND focus = reinterpret_cast<HWND>(GetWindowLongPtrA(w, 0)); IsWindow(focus)) SetFocus(focus);
    } else if (msg == WM_SETCURSOR) {
        SetCursor(nullptr);
    }
    return DefWindowProcA(w, msg, wp, lp);
}

HWND CreateDeviceWindow(HWND focus) {
    static const char kClass[] = "DirectDrawDeviceWnd";
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&DeviceWndProc), &self);
    WNDCLASSA wc;
    if (!GetClassInfoA(self, kClass, &wc)) {
        wc = {};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = DeviceWndProc;
        wc.cbWndExtra = sizeof(LONG_PTR);
        wc.hInstance = self;
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kClass;
        if (!RegisterClassA(&wc)) return nullptr;
    }
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(focus, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& r = mi.rcMonitor;
    // ddraw.dll's style is WS_POPUP | WS_VISIBLE; shown here by UpdateDeviceWindow.
    return CreateWindowExA(0, kClass, kClass, WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top, focus,
                           nullptr, self, focus);
}

// The device window is where the picture goes in exclusive mode (see PresentWindow): shown
// over the monitor, as ddraw.dll's covers it, while the game is in front and the display
// covers the screen (switch or borderless); hidden otherwise (in the background, out of
// exclusive mode, display=window: there the focus window shows it). g_displayLock held.
void DDraw::UpdateDeviceWindow() {
    if (!deviceWnd_ || !IsWindow(deviceWnd_)) return;
    const int display = DisplayKind();
    const bool show = Exclusive() && !suspended_ && display != kDisplayWindow;
    if (show) {
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(GameMonitor(hwnd_ ? hwnd_ : deviceWnd_), &mi);
        const RECT& m = mi.rcMonitor;
        SetWindowPos(deviceWnd_, HWND_TOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else if (IsWindowVisible(deviceWnd_)) {
        ShowWindow(deviceWnd_, SW_HIDE);
    }
    GameImageChanged();
    MarkDirty();
}

void DDraw::DestroyDeviceWindow() {
    if (deviceWnd_ && IsWindow(deviceWnd_)) {
        // Hidden at once from any thread; destroyed by its own (DestroyWindow fails from
        // another: WM_CLOSE asks it to).
        SetWindowPos(deviceWnd_, nullptr, 0, 0, 0, 0,
                     SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        if (GetWindowThreadProcessId(deviceWnd_, nullptr) == GetCurrentThreadId()) DestroyWindow(deviceWnd_);
        else PostMessageA(deviceWnd_, WM_CLOSE, 0, 0);
    }
    if (lclWnd_ == deviceWnd_) lclWnd_ = nullptr;
    deviceWnd_ = nullptr;
}

}  // namespace dd2d9
