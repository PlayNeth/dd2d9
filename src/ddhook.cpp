// The exclusive window's subclassing: dd2d9's window procedure in front of the game's.
#include "ddint.h"

#include <windowsx.h>

#include <algorithm>
#include <atomic>

namespace dd2d9 {

// Like ddraw.dll, exclusive mode subclasses the game's window: losing focus
// undoes the display mode switch (or the borderless cover) and minimizes the
// game, regaining it sets them up again. The screensaver and monitor power-off
// are blocked while exclusive.
//
// Locking: window and display-mode changes run under g_displayLock, never under
// g_lock, because they send messages to the window's thread, whose window
// procedure may call DirectDraw. Order: g_displayLock, then g_lock.
namespace {
struct WindowHook {
    HWND hwnd = nullptr;
    WNDPROC orig = nullptr;  // kept after unhooking: a message already in flight may still read it
    bool unicode = false;
    DDraw* owner = nullptr;  // null once released; the hook then only forwards
};
WindowHook g_hook;  // written under g_displayLock; owner and hwnd also under g_windowHookLock
// The window procedure reads the owner's state without the display lock, under this lock
// (shared), which is taken exclusively to change the owner: once UnhookWindow has let go of
// it, no window procedure still looks at the object, and TearDown may free it.
SRWLOCK g_windowHookLock = SRWLOCK_INIT;

void SetHook(HWND hwnd, DDraw* owner) {
    AcquireSRWLockExclusive(&g_windowHookLock);
    g_hook.hwnd = hwnd;
    g_hook.owner = owner;
    ReleaseSRWLockExclusive(&g_windowHookLock);
}

// What the window procedure needs of the owner where it can't wait for the display lock:
// WM_GETMINMAXINFO comes from CoverMonitor's SetWindowPos on a thread that holds it. Only
// plain fields are read under g_windowHookLock, nothing that waits or sends a message.
struct HookView {
    bool owned = false;      // h is the hooked window, and its DirectDraw object lives
    bool exclusive = false;
    bool quietMove = false;  // (see CheckCover)
    bool covered = false;    // CoverMonitor sized it past the screen: coverSize
    SIZE coverSize = {};
};
HookView ViewHook(HWND h) {
    HookView v;
    AcquireSRWLockShared(&g_windowHookLock);
    if (const DDraw* o = g_hook.owner; o && h == g_hook.hwnd) {
        v.owned = true;
        v.exclusive = o->Exclusive();
        v.quietMove = o->QuietMove();
        v.covered = o->CoverSize(h, &v.coverSize);
    }
    ReleaseSRWLockShared(&g_windowHookLock);
    return v;
}

// Work on the game's window that the window procedure does under the display lock, but never
// waits for it there (another thread may hold it while it waits for this one: SetWindowPos on
// the window): covering the screen again after the game changed its window (CheckCover), a
// focus change, a display change. With the lock held by another thread (a DirectDraw call on
// another of the game's threads, the topmost timer), the work is noted, and done once that
// thread lets go of the lock: LeaveDisplayLock posts the message that runs it.
enum : unsigned {
    kWorkCoverMoved = 1,  // CheckCover: the game moved or sized its window
    kWorkCoverFrame = 2,  // CheckCover: the game changed its window's frame
    kWorkActivate = 4,    // a focus change, taken as it is when it runs (the foreground window)
    kWorkDisplay = 8,     // a display change (WM_DISPLAYCHANGE)
    kWorkWindowActivate = 16,  // WM_ACTIVATE: which of the game's windows is in front (OnWindowActivate)
};
std::atomic<unsigned> g_windowWork{0};
std::atomic<DWORD> g_workThread{0};   // kWorkActivate: WM_ACTIVATEAPP's lParam (for the log)
std::atomic<HWND> g_workWindow{nullptr};
std::atomic<bool> g_workPosted{false};  // the message is on its way: no other is needed

UINT WindowWorkMsg() {
    static UINT msg = RegisterWindowMessageW(L"DD2D9_WindowWork");
    return msg;
}

void PostWindowWork() {
    if (g_workPosted.exchange(true)) return;
    if (!WindowWorkMsg() || !PostMessageW(g_workWindow.load(), WindowWorkMsg(), 0, 0)) g_workPosted.store(false);
}

// Noted first, then posted: a thread holding the lock now sees it when it lets go.
void RequestWindowWork(HWND h, unsigned work) {
    g_workWindow.store(h);
    g_windowWork.fetch_or(work);
    PostWindowWork();
}

bool DisplayLockHeldByMe() {
    return g_displayLock.RecursionCount > 0 &&
           g_displayLock.OwningThread == reinterpret_cast<HANDLE>(ULONG_PTR(GetCurrentThreadId()));
}

// WM_DISPLAYCHANGE on the hooked window (g_displayLock held). Before the game sees it, as
// ddraw.dll: the surfaces are lost and the window covers the screen in the new mode. Windows
// sends the notices twice, the second time late (after ChangeDisplaySettings returned and the
// game made its new surfaces), and between two game modes one may name the desktop's: only a
// display that really left the game's mode loses the surfaces. SetDisplayMode already lost
// those of the old mode.
void DisplayChanged(DDraw* owner) {
    if (owner->EmulatedMode()) DD2D9_LOG("display changed: the game's mode is dd2d9's own, its surfaces stay");
    else if (!owner->InOwnMode()) owner->InvalidateSurfaces();
    owner->OnDisplayChange();
}

// The noted work (WindowWorkMsg, on the window's thread). The lock busy again: its holder
// posts the message again as it lets go.
void RunWindowWork(HWND h) {
    if (!g_windowWork.load() || !TryEnterCriticalSection(&g_displayLock)) return;
    const unsigned work = g_windowWork.exchange(0);
    if (DDraw* owner = h == g_hook.hwnd ? g_hook.owner : nullptr) {
        if (work & kWorkDisplay) DisplayChanged(owner);
        if (work & kWorkActivate) {
            // As it is now, not as the message said: a deactivation noted behind a later
            // activation would otherwise minimize a game in front.
            DWORD pid = 0;
            const HWND fg = GetForegroundWindow();
            const bool active = fg && (GetWindowThreadProcessId(fg, &pid), pid == GetCurrentProcessId());
            owner->OnActivate(active, g_workThread.load());
        }
        if (work & kWorkWindowActivate) owner->OnWindowActivate();
        if (work & (kWorkCoverMoved | kWorkCoverFrame)) owner->CheckCover(h, !(work & kWorkCoverFrame));
    }
    LeaveDisplayLock();
}

// Window messages that tell how the game's window and focus change (log=2).
void TraceMessage(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const char* name = nullptr;
    switch (msg) {
        case WM_ACTIVATEAPP: name = "WM_ACTIVATEAPP"; break;
        case WM_ACTIVATE: name = "WM_ACTIVATE"; break;
        case WM_NCACTIVATE: name = "WM_NCACTIVATE"; break;
        case WM_SETFOCUS: name = "WM_SETFOCUS"; break;
        case WM_KILLFOCUS: name = "WM_KILLFOCUS"; break;
        case WM_SIZE: name = "WM_SIZE"; break;
        case WM_SHOWWINDOW: name = "WM_SHOWWINDOW"; break;
        case WM_WINDOWPOSCHANGED: name = "WM_WINDOWPOSCHANGED"; break;
        case WM_DISPLAYCHANGE: name = "WM_DISPLAYCHANGE"; break;
        case WM_SYSCOMMAND: name = "WM_SYSCOMMAND"; break;
        case WM_SYSKEYDOWN: name = "WM_SYSKEYDOWN"; break;
        case WM_CLOSE: name = "WM_CLOSE"; break;
        case WM_STYLECHANGED: name = "WM_STYLECHANGED"; break;
        default: return;
    }
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
    DD2D9_TRACE("window %p: %s wParam 0x%lx lParam 0x%lx (foreground %p, display %lux%lu)", static_cast<void*>(h),
               name, static_cast<unsigned long>(wp), static_cast<unsigned long>(lp),
               static_cast<void*>(GetForegroundWindow()), dm.dmPelsWidth, dm.dmPelsHeight);
    if (msg == WM_WINDOWPOSCHANGED || msg == WM_STYLECHANGED) {
        RECT w = {}, c = {};
        GetWindowRect(h, &w);
        GetClientRect(h, &c);
        MapWindowPoints(h, nullptr, reinterpret_cast<POINT*>(&c), 2);
        DD2D9_TRACE("  window %ld,%ld-%ld,%ld, client %ld,%ld-%ld,%ld, style 0x%08lx", w.left, w.top, w.right,
                   w.bottom, c.left, c.top, c.right, c.bottom, static_cast<unsigned long>(GetWindowLongW(h, GWL_STYLE)));
    }
}

LONG_PTR GetProc(HWND h, bool unicode);
LONG_PTR SetProc(HWND h, bool unicode, LONG_PTR proc);

// The window's own procedure, the one dd2d9's hook replaced (the default one if there was none).
LRESULT CallOrig(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const WNDPROC orig = g_hook.orig;
    const bool unicode = g_hook.unicode;
    if (!orig) return unicode ? DefWindowProcW(h, msg, wp, lp) : DefWindowProcA(h, msg, wp, lp);
    return unicode ? CallWindowProcW(orig, h, msg, wp, lp) : CallWindowProcA(orig, h, msg, wp, lp);
}
}  // namespace

// The size the game's window procedure was last told (WM_SIZE's lParam), and telling it the
// mode's when that differs. With a real switch Windows sizes the fullscreen window to the new
// mode and the game hears of it; a covering window already the monitor's size doesn't change,
// so it heard nothing: Sango 4 (Romance of the Three Kingdoms IV), told 1920x1080 when it took
// exclusive mode, blitted its 800x600 frame to 0,0-1920,1080 of its 800x600 primary, and
// showed its top left corner, enlarged.
volatile LONG g_toldSize = -1;

namespace {
// The game minimized its own window in exclusive mode with it still in front; the last
// WM_ACTIVATEAPP Windows sent; dd2d9 telling the game it's active (the window thread's: see
// HookProc).
bool g_minimizedInFront = false;
bool g_appActive = true;
bool g_tellingActive = false;

LRESULT CALLBACK HookProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (LogLevel() >= 2) TraceMessage(h, msg, wp, lp);
    switch (msg) {  // where the game's image is on the screen may have changed (GameImage)
    case WM_WINDOWPOSCHANGED: case WM_SIZE: case WM_MOVE: case WM_DISPLAYCHANGE: case WM_SHOWWINDOW:
    case WM_ACTIVATEAPP: case WM_DESTROY: case WM_DPICHANGED: GameImageChanged(); break;
    default: break;
    }
    if (WindowWorkMsg() && msg == WindowWorkMsg()) {  // (dd2d9's own: not the game's)
        g_workPosted.store(false);
        RunWindowWork(h);
        return 0;
    }
    // The game restyled its window (its frame changed): the client area covering the
    // screen was worked out with the old frame. Checked once the game has handled the
    // message (see CheckCover).
    const bool frameChanged =
        msg == WM_STYLECHANGED ||
        (msg == WM_WINDOWPOSCHANGED && lp && (reinterpret_cast<const WINDOWPOS*>(lp)->flags & SWP_FRAMECHANGED));
    // A move or resize by the game (not dd2d9's own quiet one): checked too.
    const bool resized = msg == WM_WINDOWPOSCHANGED && lp &&
                         (reinterpret_cast<const WINDOWPOS*>(lp)->flags & (SWP_NOSIZE | SWP_NOMOVE)) != (SWP_NOSIZE | SWP_NOMOVE);
    HookView view;
    if (msg == WM_WINDOWPOSCHANGING || msg == WM_WINDOWPOSCHANGED || msg == WM_SIZE || msg == WM_MOVE ||
        msg == WM_STYLECHANGED || msg == WM_GETMINMAXINFO || msg == WM_SYSCOMMAND)
        view = ViewHook(h);
    if ((frameChanged || resized) && view.owned && view.exclusive && !view.quietMove)
        RequestWindowWork(h, frameChanged ? kWorkCoverFrame : kWorkCoverMoved);
    if (msg == WM_ACTIVATEAPP) {
        // (The display lock busy: see RequestWindowWork.)
        if (TryEnterCriticalSection(&g_displayLock)) {
            if (h == g_hook.hwnd && g_hook.owner) g_hook.owner->OnActivate(wp != 0, DWORD(lp));
            LeaveDisplayLock();
        } else {
            g_workThread.store(DWORD(lp));
            RequestWindowWork(h, kWorkActivate);
        }
    } else if (msg == WM_ACTIVATE && h == g_hook.hwnd) {
        // Another of the game's windows in front, or the game's back (no WM_ACTIVATEAPP):
        // the cursor let go or confined again (OnWindowActivate). Posted: it runs as things are
        // once the activation is done.
        RequestWindowWork(h, kWorkWindowActivate);
    } else if (msg == WM_DISPLAYCHANGE) {
        // The owner only under the display lock: TearDown unhooks under it, then frees it.
        // (Busy: usually dd2d9 changing the mode, but also the topmost timer or
        // SetCooperativeLevel on another thread while someone else changes it.)
        if (TryEnterCriticalSection(&g_displayLock)) {
            if (h == g_hook.hwnd && g_hook.owner) DisplayChanged(g_hook.owner);
            LeaveDisplayLock();
        } else {
            RequestWindowWork(h, kWorkDisplay);
        }
    } else if (msg == WM_WINDOWPOSCHANGING && lp && view.owned && !view.quietMove) {
        // The game sizing its covering window back (see FitCover): kept covering, before the
        // game's own handling sees the position. (The display lock held by another thread:
        // left to CheckCover, run once the lock is free; logged, to tell from a miss.)
        if (TryEnterCriticalSection(&g_displayLock)) {
            if (h == g_hook.hwnd && g_hook.owner) g_hook.owner->FitCover(h, reinterpret_cast<WINDOWPOS*>(lp));
            LeaveDisplayLock();
        } else {
            const auto* p = reinterpret_cast<const WINDOWPOS*>(lp);
            if ((p->flags & (SWP_NOSIZE | SWP_NOMOVE)) != (SWP_NOSIZE | SWP_NOMOVE))
                DD2D9_LOG("the game moves or sizes its window (%dx%d at %d,%d, flags 0x%x) while the display is busy",
                          p->cx, p->cy, p->x, p->y, p->flags);
        }
    } else if ((msg == WM_WINDOWPOSCHANGING || msg == WM_WINDOWPOSCHANGED || msg == WM_SIZE || msg == WM_MOVE) &&
               view.owned && view.quietMove) {
        // dd2d9 covering the screen again behind the game's back (see CheckCover): the
        // window's own handling only, so that the game doesn't size it back.
        return g_hook.unicode ? DefWindowProcW(h, msg, wp, lp) : DefWindowProcA(h, msg, wp, lp);
    } else if (msg == WM_PAINT && h == g_hook.hwnd && g_hook.owner) {
        // Covering the screen with the focus lost (OnActivate): painted here, not by the game.
        // A game may take the foreground back as it paints (Need for Speed III's menus:
        // SetForegroundWindow in its WM_PAINT while fullscreen), and switching away paints
        // the window (Alt+Tab's switcher over it): the desktop flashed and the game came
        // back. Also where the window stays visible in the background (DDSCL_NOWINDOWCHANGES,
        // a hidden owner): ddraw.dll would let the game paint it, but what a fullscreen game
        // paints is its screen, shown again when it is back (invalidated then: see
        // OnActivate). Not while coming back (restoring_): its own paints then are the game's.
        // (The display busy: the game's, as before.)
        bool away = false;
        if (TryEnterCriticalSection(&g_displayLock)) {
            away = h == g_hook.hwnd && g_hook.owner && g_hook.owner->Away();
            LeaveDisplayLock();
        }
        if (away) {
            PAINTSTRUCT ps;
            if (BeginPaint(h, &ps)) EndPaint(h, &ps);
            else ValidateRect(h, nullptr);  // (or WM_PAINT comes again and again)
            return 0;
        }
    } else if (msg == WM_GETMINMAXINFO) {
        // A window covering its monitor has its frame outside the screen (see CoverMonitor):
        // the default maximum tracking size would shrink it.
        const LRESULT r = CallOrig(h, msg, wp, lp);
        if (view.covered && lp) {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMaxTrackSize.x = std::max<LONG>(mmi->ptMaxTrackSize.x, view.coverSize.cx);
            mmi->ptMaxTrackSize.y = std::max<LONG>(mmi->ptMaxTrackSize.y, view.coverSize.cy);
        }
        return r;
    } else if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
        // A display mode shown scaled or in a window: the game's window is its screen, at
        // 0,0 and the mode's size (see input.cpp). The wheel messages carry screen
        // coordinates, the others the client area's.
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL) ClientToScreen(h, &p);
        if (MouseToGame(&p, false)) lp = MAKELPARAM(p.x, p.y);
    } else if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_SCREENSAVE) {
        // As ddraw.dll (FUN_7375dcdc): no screensaver in exclusive mode. The monitor may
        // still power down (SC_MONITORPOWER goes through).
        if (view.owned && view.exclusive) return 0;
    }
    // The game's window in a display mode of dd2d9's own: the mode's size at 0,0, as
    // GetClientRect and GetSystemMetrics answer it (input.cpp) and as after a real mode switch,
    // not the monitor's it covers. (Xuan-Yuan Sword 3 EX, told the monitor's size back from
    // Alt+Tab, made its buffers that size and drew into its 640x480 ones as if they were: a
    // crash.)
    if (((msg == WM_SIZE && (wp == SIZE_RESTORED || wp == SIZE_MAXIMIZED)) || (msg == WM_MOVE && !IsIconic(h))) &&
        h == g_hook.hwnd) {
        int mw, mh;
        if (ShownDisplaySize(&mw, &mh)) lp = msg == WM_SIZE ? MAKELPARAM(mw, mh) : 0;
    }
    if (msg == WM_SIZE && (wp == SIZE_RESTORED || wp == SIZE_MAXIMIZED) && h == g_hook.hwnd)
        InterlockedExchange(&g_toldSize, LONG(lp));  // (what the game was last told: see ApplyWindow)
    // The game minimizing its own window in exclusive mode and showing it again, with no
    // WM_ACTIVATEAPP between: told it's active once it's back. On Windows 9x the minimized
    // window lost the activation and got it back when shown; Windows 11 and Wine leave it in
    // front throughout. Mega Man X, back from its first Alt+Tab, minimizes and maximizes
    // itself once (its SC_RESTORE); minimized, it marks its surfaces to be restored and draws
    // nothing until WM_ACTIVATEAPP has restored them: its screen stood still, black or the
    // intro's text, until the next Alt+Tab. Only the game's procedure hears it: for dd2d9
    // nothing changed. (Not dd2d9's own minimizing: that follows WM_ACTIVATEAPP (FALSE). And
    // not while the game handles the one dd2d9 sends: a game that minimizes and restores
    // itself there isn't told again.)
    if (msg == WM_ACTIVATEAPP) {
        g_appActive = wp != 0;
        g_minimizedInFront = false;
    } else if (msg == WM_NCDESTROY) {
        g_minimizedInFront = false;
    } else if (msg == WM_SIZE && h == g_hook.hwnd && !g_tellingActive) {
        if (wp == SIZE_MINIMIZED) {
            g_minimizedInFront = view.owned && view.exclusive && g_appActive && GetForegroundWindow() == h;
        } else if ((wp == SIZE_RESTORED || wp == SIZE_MAXIMIZED) && g_minimizedInFront) {
            g_minimizedInFront = false;
            const LRESULT r = CallOrig(h, msg, wp, lp);
            if (view.exclusive && GetForegroundWindow() == h && !IsIconic(h)) {
                static bool logged;
                if (!logged) {
                    logged = true;
                    DD2D9_LOG("the game minimized its window and showed it again while in front: told it's active "
                              "again (WM_ACTIVATEAPP)");
                }
                g_tellingActive = true;
                CallOrig(h, WM_ACTIVATEAPP, TRUE, 0);
                g_tellingActive = false;
            }
            return r;
        }
    }
    if (msg == WM_NCDESTROY && h == g_hook.hwnd) {
        // As ddraw.dll: the window is going, and the hook with it. The game's procedure
        // gets the message first; the window's procedure is put back (a later message
        // goes straight to the game's), and dd2d9 no longer counts the window as hooked.
        const WNDPROC orig = g_hook.orig;
        const bool unicode = g_hook.unicode;
        const LRESULT r = CallOrig(h, msg, wp, lp);
        if (orig && GetProc(h, unicode) == reinterpret_cast<LONG_PTR>(HookProc)) SetProc(h, unicode, reinterpret_cast<LONG_PTR>(orig));
        AcquireSRWLockExclusive(&g_windowHookLock);
        if (h == g_hook.hwnd) g_hook.hwnd = nullptr;
        ReleaseSRWLockExclusive(&g_windowHookLock);
        return r;
    }
    return CallOrig(h, msg, wp, lp);
}

LONG_PTR GetProc(HWND h, bool unicode) {
    return unicode ? GetWindowLongPtrW(h, GWLP_WNDPROC) : GetWindowLongPtrA(h, GWLP_WNDPROC);
}

LONG_PTR SetProc(HWND h, bool unicode, LONG_PTR proc) {
    return unicode ? SetWindowLongPtrW(h, GWLP_WNDPROC, proc) : SetWindowLongPtrA(h, GWLP_WNDPROC, proc);
}
}  // namespace

void LeaveDisplayLock() {
    LeaveCriticalSection(&g_displayLock);
    if (g_windowWork.load() && !DisplayLockHeldByMe()) PostWindowWork();
}

// As ddraw.dll's timer: about every 1.5 s while a game holds exclusive mode in front, its
// window is made topmost again if something took that away (the game changing its
// extended style, or hiding and showing its window around a mode change); a hidden window
// stays hidden. A thread-pool timer (not a timer on the window: its ticks would go through
// the game's message loop, and some games react to any WM_TIMER), started with the first
// hooked window and kept (the DLL is pinned then), only trying for the display lock; the
// window is changed asynchronously, by its own thread.
static void CALLBACK TopmostTick(void*, BOOLEAN) {
    if (!TryEnterCriticalSection(&g_displayLock)) return;  // next time
    if (g_hook.hwnd && g_hook.owner) g_hook.owner->KeepTopmost();
    LeaveDisplayLock();
}

void DDraw::HookWindow() {
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    // As ddraw.dll: no "Not Responding" ghost window replaces the game's while it is busy
    // (loading a level without pumping messages). For the process, once.
    static bool noGhosting;
    if (!noGhosting) {
        noGhosting = true;
        DisableProcessWindowsGhosting();
    }
    if (g_hook.hwnd == hwnd_) {  // already in this window's chain (maybe from an earlier DirectDraw object)
        SetHook(hwnd_, this);
        return;
    }
    if (g_hook.hwnd && IsWindow(g_hook.hwnd)) {
        if (GetProc(g_hook.hwnd, g_hook.unicode) != reinterpret_cast<LONG_PTR>(HookProc)) {
            DD2D9_LOG("window %p not hooked: still chained into window %p", static_cast<void*>(hwnd_),
                     static_cast<void*>(g_hook.hwnd));
            return;
        }
        SetProc(g_hook.hwnd, g_hook.unicode, reinterpret_cast<LONG_PTR>(g_hook.orig));
    }
    SetHook(nullptr, nullptr);
    // Set the forwarding target before installing, so a message that arrives
    // right after installation (from another thread) is forwarded correctly.
    const bool unicode = IsWindowUnicode(hwnd_) != FALSE;
    g_hook.unicode = unicode;
    g_hook.orig = reinterpret_cast<WNDPROC>(GetProc(hwnd_, unicode));
    LONG_PTR prev = SetProc(hwnd_, unicode, reinterpret_cast<LONG_PTR>(HookProc));
    if (!prev) {
        DD2D9_LOG("window %p not hooked: SetWindowLongPtr failed (%lu)", static_cast<void*>(hwnd_), GetLastError());
        return;
    }
    g_hook.orig = reinterpret_cast<WNDPROC>(prev);
    SetHook(hwnd_, this);
    // Work noted for an earlier window is not this one's (and its message, if the earlier
    // window was unhooked, went to the game's procedure).
    g_windowWork.store(0);
    g_workPosted.store(false);
    // A new window has been told nothing yet: what an earlier one was told doesn't count.
    InterlockedExchange(&g_toldSize, -1);
    static HANDLE topmostTimer;
    if (!topmostTimer && !CreateTimerQueueTimer(&topmostTimer, nullptr, TopmostTick, nullptr, 1500, 1500, WT_EXECUTEDEFAULT))
        topmostTimer = nullptr;
    // HookProc may stay in the window's chain for good (see UnhookWindow), and
    // wrappers such as D7VK free and reload this DLL (Direct3D <-> software
    // switches): keep the code loaded for the rest of the process.
    static bool pinned;
    if (!pinned) {
        HMODULE self = nullptr;
        pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    reinterpret_cast<LPCWSTR>(&HookProc), &self) != FALSE;
        if (!pinned) DD2D9_LOG("could not keep the DLL loaded (%lu): unloading it may crash", GetLastError());
    }
}

void DDraw::UnhookWindow() {
    if (g_hook.owner != this) return;
    HWND h = g_hook.hwnd;
    SetHook(h, nullptr);
    if (h && IsWindow(h)) {
        // Something subclassed the window after us: stay in its chain, forwarding only.
        if (GetProc(h, g_hook.unicode) != reinterpret_cast<LONG_PTR>(HookProc)) return;
        SetProc(h, g_hook.unicode, reinterpret_cast<LONG_PTR>(g_hook.orig));
    }
    SetHook(nullptr, nullptr);
}

}  // namespace dd2d9
