// The DirectDraw object: display modes, cooperative level, object creation.
#include "wrappers.h"
#include "gpu.h"

#include "d3dproxy.h"

#include <mmsystem.h>
#include <windowsx.h>

#include <algorithm>
#include <intrin.h>

#include <new>

namespace dd2d9 {

namespace {
std::vector<DDraw*> g_ddraws;  // every DirectDraw object (g_lock), for the GetSurfaceFromDC export
// The DirectDraw object in exclusive mode, if any (g_displayLock): the display mode is
// its to change and restore, as in ddraw.dll.
DDraw* g_exclusiveDD = nullptr;
}

DDraw::DDraw() {
    {
        Guard l;
        g_ddraws.push_back(this);
    }
    NoteDisplayMode();
    wraps_[0] = new DDWrap<1>(this);
    wraps_[1] = new DDWrap<2>(this);
    wraps_[2] = new DDWrap<4>(this);
    wraps_[3] = new DDWrap<7>(this);
    for (int i = 0; i < 256; i++) defaultPalette[i] = {BYTE(i), BYTE(i), BYTE(i), 0};
    UpdateRefreshRate();
    QueryPerformanceFrequency(&qpcFreq_);
    QueryPerformanceCounter(&qpcBase_);
    gpu_ = new Gpu;
    presenter_ = new Presenter(this);
    timeBeginPeriod(1);  // precise Sleep() for vblank emulation and flip pacing
}

DDraw::~DDraw() {
    {
        Guard l;
        g_ddraws.erase(std::find(g_ddraws.begin(), g_ddraws.end(), this));
    }
    timeEndPeriod(1);
    delete presenter_;
    delete gpu_;  // after the presenter, which used it
    delete static_cast<DDWrap<1>*>(wraps_[0]);
    delete static_cast<DDWrap<2>*>(wraps_[1]);
    delete static_cast<DDWrap<4>*>(wraps_[2]);
    delete static_cast<DDWrap<7>*>(wraps_[3]);
}

void* DDraw::Iface(int v) {
    switch (v) {
    case 1: return wraps_[0];
    case 2: return wraps_[1];
    case 4: return wraps_[2];
    default: return wraps_[3];
    }
}

HRESULT DDraw::QueryInterface(REFIID r, void** out) {
    if (IsBadReadPtr(&r, sizeof(IID)) || !out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    int v = 0;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirectDraw)) v = 1;
    else if (IsEqualGUID(r, IID_IDirectDraw2)) v = 2;
    else if (IsEqualGUID(r, IID_IDirectDraw4)) v = 4;
    else if (IsEqualGUID(r, IID_IDirectDraw7)) v = 7;
    if (v) {
        AddRef(v);
        *out = Iface(v);
        return S_OK;
    }
    HRESULT hr = D3DProxyQueryDDraw(this, r, out);  // Direct3D objects for a wrapper such as D7VK
    if (hr != E_NOINTERFACE) return hr;
    DD2D9_LOG("DDraw::QueryInterface(%s): not supported", GuidName(r));
    return E_NOINTERFACE;
}

static int IfaceIndex(int version) { return version == 1 ? 0 : version == 2 ? 1 : version == 4 ? 2 : 3; }

// log=2 traces these (whether a game lets go of its old DirectDraw object before making
// another decides what the new one may do). Surfaces made through IDirectDraw4/7 hold a
// reference of their own, counted here too.
ULONG DDraw::AddRef(int version) {
    const LONG total = InterlockedIncrement(&ref_);
    const LONG r = InterlockedIncrement(&ifaceRef_[IfaceIndex(version)]);
    DD2D9_TRACE("AddRef DirectDraw %p through v%d -> %ld (total %ld)", static_cast<void*>(this), version, r, total);
    return ULONG(r);
}

ULONG DDraw::Release(int version) {
    LONG* own = &ifaceRef_[IfaceIndex(version)];
    LONG r = InterlockedDecrement(own);
    if (r < 0) {  // released more often than referenced: ignore
        InterlockedIncrement(own);
        return 0;
    }
    const LONG total = InterlockedDecrement(&ref_);
    DD2D9_TRACE("Release DirectDraw %p through v%d -> %ld (total %ld)", static_cast<void*>(this), version, r, total);
    // Tear down once: a stray AddRef/Release pair after the final release must
    // not run this (and IntRelease) a second time.
    if (total == 0 && InterlockedExchange(&tornDown_, 1) == 0) {
        // Teardown waits for the presenter thread, which takes g_lock: if this
        // thread holds it (a surface released inside another call, or the game's
        // AcquireDDThreadLock), wait until it lets go.
        RunWhenUnlocked([](void* self) { static_cast<DDraw*>(self)->TearDown(); }, this);
    }
    return ULONG(r);
}

void DDraw::TearDown() {
    {
        DD2D9_LOG("DirectDraw object released");
        const bool stopped = presenter_->Stop();  // must not hold g_lock: the presenter thread takes it
        {
            DisplayGuard dg;
            // Still hooked while the mode goes back (see OnDisplayChange), as when leaving
            // exclusive mode.
            RestoreDisplay();
            RestoreSavedWindow(true);
            UnhookWindow();
            if (g_exclusiveDD == this) g_exclusiveDD = nullptr;
        }
        Guard l;
        alive = false;
        if (primary) {
            primary->isPrimary = false;
            primary = nullptr;
        }
        // A presenter thread that didn't stop still uses this object and the
        // presenter: keep both alive rather than free them under it.
        if (!stopped) IntAddRef();
        IntRelease();
    }
}

void DDraw::IntRelease() {
    if (InterlockedDecrement(&intRef_) == 0) delete this;
}

// ---------------------------------------------------------------- display state

void DDraw::DisplayMode(DWORD* w, DWORD* h, DWORD* bpp) const {
    if (modeSet_) {
        *w = modeW_;
        *h = modeH_;
        *bpp = modeBpp_;
        return;
    }
    *w = DWORD(GetSystemMetrics(SM_CXSCREEN));
    *h = DWORD(GetSystemMetrics(SM_CYSCREEN));
    *bpp = 32;
}

HWND DDraw::PresentWindow() const {
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
    const int display = modeSet_ ? activeDisplay_ : g_config.display;
    if (!Exclusive() || display == kDisplayWindow || hwnd != hwnd_) return c;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT m = mi.rcMonitor;
    MapWindowPoints(nullptr, hwnd, reinterpret_cast<POINT*>(&m), 2);  // into client coordinates
    RECT t;
    return IntersectRect(&t, &m, &c) ? t : c;
}

bool DDraw::ImageOnScreen(RECT* image, SIZE* mode) const {
    // After a real mode switch the screen is the game's mode: nothing to translate (also
    // not while the display is between two modes).
    if (!Exclusive() || !modeSet_ || activeDisplay_ == kDisplaySwitch || suspended_ || Has3D() || !hwnd_ ||
        !IsWindow(hwnd_) || IsIconic(hwnd_))
        return false;
    const RECT t = PresentTarget(hwnd_);
    RECT r = presenter_->FitRect(modeW_, modeH_, UINT(t.right - t.left), UINT(t.bottom - t.top));
    OffsetRect(&r, t.left, t.top);
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&r), 2);
    if (r.left == 0 && r.top == 0 && r.right == LONG(modeW_) && r.bottom == LONG(modeH_)) return false;  // 1:1
    *image = r;
    *mode = SIZE{LONG(modeW_), LONG(modeH_)};
    return true;
}

// The exclusive object's image, for the mouse (see input.cpp).
bool GameImage(RECT* image, SIZE* mode, HWND* window) {
    if (!g_config.mouseScale) return false;
    Guard l;
    const DDraw* d = g_exclusiveDD;
    if (!d || !d->ImageOnScreen(image, mode)) return false;
    if (window) *window = d->Window();
    return true;
}

// The real screen to the game's display mode (its screen as the game sees it, at 0,0):
// scaled, and kept on the game's screen as the real cursor is kept on the real one.
bool MouseToGame(POINT* p) {
    RECT r;
    SIZE m;
    if (!GameImage(&r, &m)) return false;
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

void DDraw::Add3DDevice() {
    if (InterlockedIncrement(&devices3D_) != 1) return;
    DD2D9_LOG("a Direct3D device exists: the wrapper presents from now on, dd2d9 stops");
    presenter_->DropDevice();  // its D3D9 device must not compete with the wrapper's on this window
}

void DDraw::Remove3DDevice() {
    if (InterlockedDecrement(&devices3D_) != 0) return;
    DD2D9_LOG("no Direct3D device left: dd2d9 presents again once the game draws");
}


// ---------------------------------------------------------------- focus handling
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
WindowHook g_hook;  // written under g_displayLock

// The current display mode as one number (0 if it can't be had).
static ULONG ModeHash() {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return ((dm.dmPelsWidth * 4099u + dm.dmPelsHeight) * 31u + dm.dmBitsPerPel) | 1u;
}

// Windows' display settings uniqueness (gdi32's DdQueryDisplaySettingsUniqueness, exported
// as GdiEntry13): it changes with every display mode change. ddraw.dll checks it on each
// call and loses the surfaces when someone else changed the mode.
// Where that isn't kept (Wine), the current mode itself stands in for it.
ULONG DisplayUniqueness() {
    using Fn = ULONG(WINAPI*)();
    static const Fn fn = reinterpret_cast<Fn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "GdiEntry13")));
    if (const ULONG u = fn ? fn() : 0) return u;
    return ModeHash();
}
LONG g_displayUniqueness;  // the value after the last change that was accounted for

// A display mode change of dd2d9's own: its surfaces are lost by whoever made it.
LONG ChangeDisplay(DEVMODEW* dm, DWORD flags) {
    const LONG r = ChangeDisplaySettingsExW(nullptr, dm, nullptr, flags, nullptr);
    InterlockedExchange(&g_displayUniqueness, LONG(DisplayUniqueness()));
    return r;
}

// For the log: the program a process id belongs to ("?" if it can't be had).
void ProcessName(DWORD pid, char* out, size_t size) {
    snprintf(out, size, "?");
    using QueryFn = BOOL(WINAPI*)(HANDLE, DWORD, LPWSTR, PDWORD);
    static const QueryFn query = reinterpret_cast<QueryFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueryFullProcessImageNameW")));
    if (!query || !pid) return;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return;
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    if (query(p, 0, path, &n)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        snprintf(out, size, "%ls", name ? name + 1 : path);
    }
    CloseHandle(p);
}

// For the log: what took the focus from the game. WM_ACTIVATEAPP names the thread
// whose window is being activated; once the switch is done, the foreground window.
void LogFocusThief(DWORD thread, HWND fg) {
    using PidFn = DWORD(WINAPI*)(HANDLE);
    static const PidFn pidOfThread = reinterpret_cast<PidFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcessIdOfThread")));
    DWORD pid = 0;
    if (thread && pidOfThread) {
        if (HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread)) {
            pid = pidOfThread(t);
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

UINT DeferredActivateMsg() {
    static UINT msg = RegisterWindowMessageW(L"DD2D9_DeferredActivate");
    return msg;
}

UINT CheckCoverMsg() {
    static UINT msg = RegisterWindowMessageW(L"DD2D9_CheckCover");
    return msg;
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

LRESULT CALLBACK HookProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (LogLevel() >= 2) TraceMessage(h, msg, wp, lp);
    const UINT deferred = DeferredActivateMsg();
    const UINT checkCover = CheckCoverMsg();
    if (checkCover && msg == checkCover) {
        if (TryEnterCriticalSection(&g_displayLock)) {
            if (h == g_hook.hwnd && g_hook.owner) g_hook.owner->CheckCover(h);
            LeaveCriticalSection(&g_displayLock);
        } else if (wp < 500) {
            // Another thread has the display (the presenter setting a device up or down):
            // later, rather than never.
            PostMessageW(h, checkCover, wp + 1, 0);
        }
        return 0;
    }
    // The game restyled its window (its frame changed): the client area covering the
    // screen was worked out with the old frame. Checked once the game has handled the
    // message (see CheckCover).
    const bool frameChanged =
        msg == WM_STYLECHANGED ||
        (msg == WM_WINDOWPOSCHANGED && lp && (reinterpret_cast<const WINDOWPOS*>(lp)->flags & SWP_FRAMECHANGED));
    if (checkCover && frameChanged && h == g_hook.hwnd && g_hook.owner && g_hook.owner->Exclusive())
        PostMessageW(h, checkCover, 0, 0);
    if (msg == WM_ACTIVATEAPP || (deferred && msg == deferred)) {
        // Another thread may hold the display lock while it waits for this
        // thread (SetWindowPos on our window): never block here, retry later.
        if (TryEnterCriticalSection(&g_displayLock)) {
            if (h == g_hook.hwnd && g_hook.owner) g_hook.owner->OnActivate(wp != 0, DWORD(lp));
            LeaveCriticalSection(&g_displayLock);
        } else if (deferred) {
            PostMessageW(h, deferred, wp, lp);
        }
        if (msg == deferred) return 0;
    } else if (msg == WM_DISPLAYCHANGE) {
        // Before the game sees it, as ddraw.dll: the surfaces are lost and the window
        // covers the screen in the new mode. Windows sends the notices twice, the second
        // time late (after ChangeDisplaySettings returned and the game made its new
        // surfaces), and between two game modes one may name the desktop's: only a
        // display that really left the game's mode loses the surfaces. SetDisplayMode
        // already lost those of the old mode.
        DDraw* owner = g_hook.owner;
        if (h == g_hook.hwnd && owner) {
            if (owner->EmulatedMode()) DD2D9_LOG("display changed: the game's mode is dd2d9's own, its surfaces stay");
            else if (!owner->InOwnMode()) owner->InvalidateSurfaces();
            if (TryEnterCriticalSection(&g_displayLock)) {
                if (g_hook.owner == owner) owner->OnDisplayChange();
                LeaveCriticalSection(&g_displayLock);
            }
        }
    } else if (msg == WM_GETMINMAXINFO) {
        // A window covering its monitor has its frame outside the screen (see CoverMonitor):
        // the default maximum tracking size would shrink it.
        const WNDPROC orig = g_hook.orig;
        const bool unicode = g_hook.unicode;
        const LRESULT r = !orig ? (unicode ? DefWindowProcW(h, msg, wp, lp) : DefWindowProcA(h, msg, wp, lp))
                                : (unicode ? CallWindowProcW(orig, h, msg, wp, lp) : CallWindowProcA(orig, h, msg, wp, lp));
        DDraw* owner = g_hook.owner;
        SIZE s;
        if (h == g_hook.hwnd && owner && lp && owner->CoverSize(h, &s)) {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMaxTrackSize.x = std::max<LONG>(mmi->ptMaxTrackSize.x, s.cx);
            mmi->ptMaxTrackSize.y = std::max<LONG>(mmi->ptMaxTrackSize.y, s.cy);
        }
        return r;
    } else if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
        // A display mode shown scaled or in a window: the game's window is its screen, at
        // 0,0 and the mode's size (see input.cpp). The wheel messages carry screen
        // coordinates, the others the client area's.
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL) ClientToScreen(h, &p);
        if (MouseToGame(&p)) lp = MAKELPARAM(p.x, p.y);
    } else if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_SCREENSAVE) {
        // As ddraw.dll (FUN_7375dcdc): no screensaver in exclusive mode. The monitor may
        // still power down (SC_MONITORPOWER goes through).
        DDraw* owner = g_hook.owner;
        if (h == g_hook.hwnd && owner && owner->Exclusive()) return 0;
    }
    const WNDPROC orig = g_hook.orig;
    const bool unicode = g_hook.unicode;
    if (msg == WM_NCDESTROY && h == g_hook.hwnd) {
        // As ddraw.dll: the window is going, and the hook with it. The game's procedure
        // gets the message first; the window's procedure is put back (a later message
        // goes straight to the game's), and dd2d9 no longer counts the window as hooked.
        const LRESULT r = !orig ? (unicode ? DefWindowProcW(h, msg, wp, lp) : DefWindowProcA(h, msg, wp, lp))
                                : (unicode ? CallWindowProcW(orig, h, msg, wp, lp) : CallWindowProcA(orig, h, msg, wp, lp));
        if (orig && GetProc(h, unicode) == reinterpret_cast<LONG_PTR>(HookProc)) SetProc(h, unicode, reinterpret_cast<LONG_PTR>(orig));
        g_hook.hwnd = nullptr;
        return r;
    }
    if (!orig) return unicode ? DefWindowProcW(h, msg, wp, lp) : DefWindowProcA(h, msg, wp, lp);
    return unicode ? CallWindowProcW(orig, h, msg, wp, lp) : CallWindowProcA(orig, h, msg, wp, lp);
}

LONG_PTR GetProc(HWND h, bool unicode) {
    return unicode ? GetWindowLongPtrW(h, GWLP_WNDPROC) : GetWindowLongPtrA(h, GWLP_WNDPROC);
}

LONG_PTR SetProc(HWND h, bool unicode, LONG_PTR proc) {
    return unicode ? SetWindowLongPtrW(h, GWLP_WNDPROC, proc) : SetWindowLongPtrA(h, GWLP_WNDPROC, proc);
}

}  // namespace

void DDraw::HookWindow() {
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    // As ddraw.dll: no "Not Responding" ghost window replaces the game's while it is busy
    // (loading a level without pumping messages). For the process, once.
    static const bool noGhosting = [] {
        using Fn = void(WINAPI*)();
        if (const auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "DisableProcessWindowsGhosting"))))
            fn();
        return true;
    }();
    (void)noGhosting;
    if (g_hook.hwnd == hwnd_) {  // already in this window's chain (maybe from an earlier DirectDraw object)
        g_hook.owner = this;
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
    g_hook.hwnd = nullptr;
    g_hook.owner = nullptr;
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
    g_hook.hwnd = hwnd_;
    g_hook.owner = this;
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
    g_hook.owner = nullptr;
    HWND h = g_hook.hwnd;
    if (h && IsWindow(h)) {
        // Something subclassed the window after us: stay in its chain, forwarding only.
        if (GetProc(h, g_hook.unicode) != reinterpret_cast<LONG_PTR>(HookProc)) return;
        SetProc(h, g_hook.unicode, reinterpret_cast<LONG_PTR>(g_hook.orig));
    }
    g_hook.hwnd = nullptr;
}

// Called with g_displayLock held.
void DDraw::OnActivate(bool active, DWORD thread) {
    if (applying_ || !Exclusive() || !alive) return;
    // Before SetDisplayMode, the window covers the screen at the desktop mode (see ApplyWindow).
    const int display = modeSet_ ? activeDisplay_ : g_config.display;
    if (!active) {
        backgrounded_ = true;  // see SetCooperativeLevel: leaving exclusive mode now loses the surfaces
        ConfineCursor(false);
        // Window mode is an ordinary window; switch and borderless cover the screen.
        const bool cover = display != kDisplayWindow && !suspended_;
        // DDSCL_NOWINDOWCHANGES: the mode goes back, the window is left alone (as ddraw.dll:
        // neither minimized nor made non-topmost; spec: re/spec/window-procedure.md).
        const bool windowChanges = !(coop_ & DDSCL_NOWINDOWCHANGES);
        if (cover) {
            DD2D9_LOG("focus lost: %s", switched_ ? "restoring the desktop display mode" : "minimizing");
            suspended_ = true;      // first: the presenter must not create a new device from here on
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
        if (display != kDisplayWindow) InterlockedExchange(&deviceState_, kDevLost);
        // Minimizing takes the keyboard focus away (WM_KILLFOCUS): the game sees it lost.
        if (cover && windowChanges && IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOWMINNOACTIVE);  // a hidden one stays so
        if (cover) LogFocusThief(thread, GetForegroundWindow());
        return;
    }
    InterlockedCompareExchange(&deviceState_, kDevNotRestored, kDevLost);
    if (suspended_) {
        DD2D9_LOG("focus regained: setting up %lux%lu again", modeW_, modeH_);
        if (IsIconic(hwnd_) && !(coop_ & DDSCL_NOWINDOWCHANGES)) ShowWindow(hwnd_, SW_RESTORE);
        ApplyWindow();
        // Only now: until the mode is switched back the presenter must not create
        // a device (it would make a windowed one, then replace it right away).
        suspended_ = false;
        MarkDirty();
    } else if (display != kDisplayWindow && !(coop_ & DDSCL_NOWINDOWCHANGES)) {
        // ddraw.dll makes the exclusive window topmost once it is in front.
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ConfineCursor(true);
    }
}

// WM_DISPLAYCHANGE on the exclusive window (ours or anyone's mode change). Called with
// g_displayLock held.
void DDraw::OnDisplayChange() {
    if (!Exclusive() || !alive || (coop_ & DDSCL_NOWINDOWCHANGES) || !hwnd_ || IsIconic(hwnd_)) return;
    const int display = modeSet_ ? activeDisplay_ : g_config.display;
    if (display == kDisplayWindow) return;
    CoverMonitor(HWND_TOP, SWP_NOACTIVATE);
    // As ddraw.dll: the cursor stays on the (possibly resized) screen while in front.
    if (GetForegroundWindow() == hwnd_) ConfineCursor(true);
}

// The window covers its monitor in exclusive mode. ddraw.dll leaves the window's style
// alone and draws over the whole screen, frame and all; dd2d9 presents into the client
// area, so the window keeps its style (as games read it) and grows until its client
// area is the monitor, the frame just outside it. A window region shows only that part,
// so the frame can't appear on a monitor next to this one. g_displayLock held.
void DDraw::CoverMonitor(HWND insertAfter, UINT flags) {
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT m = mi.rcMonitor;
    const LONG style = GetWindowLongW(hwnd_, GWL_STYLE), exStyle = GetWindowLongW(hwnd_, GWL_EXSTYLE);
    RECT r = m;
    AdjustWindowRectEx(&r, DWORD(style), GetMenu(hwnd_) != nullptr, DWORD(exStyle));
    coverHwnd_ = hwnd_;
    coverSize_ = {r.right - r.left, r.bottom - r.top};  // first: WM_GETMINMAXINFO lets it be this large
    SetWindowPos(hwnd_, insertAfter, r.left, r.top, coverSize_.cx, coverSize_.cy, flags);
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
        GetMonitorInfoW(MonitorFromWindow(coverHwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
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
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
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
// again for the new frame. A move or resize alone stays the game's (ddraw.dll leaves it
// alone), and a game that keeps changing the frame wins after a few tries rather than the
// two fighting.
void DDraw::CheckCover(HWND h) {
    if (!Exclusive() || !alive || h != coverHwnd_ || !coverSize_.cx || applying_ || suspended_ ||
        (coop_ & DDSCL_NOWINDOWCHANGES) || IsIconic(h) || !IsWindowVisible(h))
        return;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT c = {};
    GetClientRect(h, &c);
    MapWindowPoints(h, nullptr, reinterpret_cast<POINT*>(&c), 2);
    if (EqualRect(&c, &mi.rcMonitor)) return;
    const DWORD now = GetTickCount();
    if (now - recoverStart_ > 2000) {
        recoverStart_ = now;
        recoverCount_ = 0;
    }
    if (++recoverCount_ > 5) return;
    DD2D9_LOG("the window no longer covers the screen (client %ld,%ld-%ld,%ld): covering it again", c.left, c.top,
              c.right, c.bottom);
    CoverMonitor(nullptr, SWP_NOZORDER | SWP_NOACTIVATE);
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
    if (!Exclusive() || !hwnd_ || !IsWindow(hwnd_)) return;
    int display = g_config.display;
    // Before SetDisplayMode: as in ddraw.dll, switch and borderless cover the screen at the
    // desktop mode; window mode has nothing to show yet and leaves the window alone.
    if (!modeSet_ && display == kDisplayWindow) return;
    // DDSCL_NOWINDOWCHANGES: the display mode still changes, the window doesn't.
    const bool windowChanges = !(coop_ & DDSCL_NOWINDOWCHANGES);
    applying_ = true;  // focus changes caused by our own mode / window changes are not the user's
    if (windowChanges) SaveWindow();  // before the mode switch, which may move or clamp the window
    if (modeSet_ && display == kDisplaySwitch && !SwitchMode()) {
        DD2D9_LOG("display mode %lux%lu not available, using a borderless scaled window", modeW_, modeH_);
        display = kDisplayBorderless;
    }
    if (display != kDisplaySwitch && switched_) {
        ChangeDisplay(nullptr, 0);
        switched_ = false;
        UpdateRefreshRate();
    }
    activeDisplay_ = display;
    // The game's mode shown scaled or in a window: its cursor functions are pointed at
    // dd2d9's (input.cpp), which show it the mode as its screen. A real mode switch
    // leaves its imports alone. Again for each mode: modules loaded since are covered.
    if (modeSet_ && display != kDisplaySwitch) InstallMouseHooks();
    if (!windowChanges) {
        applying_ = false;
        return;
    }

    LONG style = GetWindowLongW(hwnd_, GWL_STYLE);
    LONG exStyle = GetWindowLongW(hwnd_, GWL_EXSTYLE);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
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
    }
    applying_ = false;
    static const char* kNames[] = {"display mode switched", "centered window", "borderless window"};
    if (modeSet_) DD2D9_LOG("%lux%lu: %s", modeW_, modeH_, kNames[display]);
    else DD2D9_LOG("exclusive mode: the window covers the screen");
}

bool DDraw::SwitchMode() {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = modeW_;
    dm.dmPelsHeight = modeH_;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;  // the colour depth stays virtual: displays run at 32 bpp
    if (modeRefresh_ > 1) {
        dm.dmDisplayFrequency = modeRefresh_;
        dm.dmFields |= DM_DISPLAYFREQUENCY;
    }
    LONG r = ChangeDisplay(&dm, CDS_FULLSCREEN);
    if (r != DISP_CHANGE_SUCCESSFUL && (dm.dmFields & DM_DISPLAYFREQUENCY)) {
        dm.dmFields &= ~DM_DISPLAYFREQUENCY;
        r = ChangeDisplay(&dm, CDS_FULLSCREEN);
    }
    DD2D9_LOG("ChangeDisplaySettings(%lux%lu @ %lu Hz) -> %ld", modeW_, modeH_, modeRefresh_, r);
    if (r != DISP_CHANGE_SUCCESSFUL) return false;
    switched_ = true;
    UpdateRefreshRate();
    return true;
}

// Undoes the mode switch and gives the window back its style and position.
// Called with g_displayLock held and g_lock not held.
void DDraw::RestoreDisplay() {
    if (switched_ && g_exclusiveDD && g_exclusiveDD != this) {
        DD2D9_LOG("display mode left alone: another DirectDraw object is in exclusive mode");
        switched_ = false;
    }
    if (switched_) {
        LONG r = ChangeDisplay(nullptr, 0);
        DD2D9_LOG("display mode restored (%ld)", r);
        switched_ = false;
        UpdateRefreshRate();
    }
    suspended_ = false;
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

void DDraw::UpdateRefreshRate() {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        refresh_ = dm.dmDisplayFrequency;
}

double DDraw::VBlankPhase() const {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double period = double(qpcFreq_.QuadPart) / refresh_;
    double t = double(now.QuadPart - qpcBase_.QuadPart) / period;
    return t - double(int64_t(t));
}

void DDraw::SleepUntilVBlank(int count) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t period = qpcFreq_.QuadPart / refresh_;
    const int64_t elapsed = now.QuadPart - qpcBase_.QuadPart;
    const int64_t target = qpcBase_.QuadPart + (elapsed / period + count) * period;
    for (;;) {
        QueryPerformanceCounter(&now);
        int64_t left = target - now.QuadPart;
        if (left <= 0) break;
        double ms = double(left) * 1000.0 / double(qpcFreq_.QuadPart);
        if (ms > 2.0) Sleep(DWORD(ms - 1.5));
        else Sleep(0);
    }
}

void DDraw::PaceFlip() {
    // Fallback for when Present() does not block on vblank (vsync off or not
    // honored by the driver/compositor). 98% of a period leaves room for real
    // vsync jitter, so with working vsync this never sleeps.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t minGap = qpcFreq_.QuadPart * 98 / (100 * int64_t(refresh_));
    if (lastFlip_) {
        const int64_t target = lastFlip_ + minGap;
        for (;;) {
            int64_t left = target - now.QuadPart;
            if (left <= 0) break;
            double ms = double(left) * 1000.0 / double(qpcFreq_.QuadPart);
            if (ms > 2.0) Sleep(DWORD(ms - 1.5));
            else Sleep(0);
            QueryPerformanceCounter(&now);
        }
    }
    lastFlip_ = now.QuadPart;
}

// ---------------------------------------------------------------- API

HRESULT DDraw::Initialize(GUID*) {
    if (initialized) return DDERR_ALREADYINITIALIZED;
    initialized = true;
    return DD_OK;
}

HRESULT DDraw::SetCooperativeLevel(HWND hwnd, DWORD flags, int version) {
    DD2D9_LOG("SetCooperativeLevel(%p, 0x%lx)%s", static_cast<void*>(hwnd), flags, version == 1 ? " (DirectDraw 1)" : "");
    DisplayGuard dg;
    // Called again from the window procedure while this runs (WM_ACTIVATEAPP from the
    // activation below): dd2d9 ignores the inner call (ddraw.dll would run it).
    if (inCoop_) {
        DD2D9_LOG("  called from inside SetCooperativeLevel: ignored");
        return DD_OK;
    }
    struct Busy {
        bool& b;
        explicit Busy(bool& f) : b(f) { b = true; }
        ~Busy() { b = false; }
    } busy(inCoop_);
    // Validation as in ddraw.dll.
    if (flags & ~0x3FFFu) return DDERR_INVALIDPARAMS;
    if ((flags & DDSCL_FPUSETUP) && (flags & DDSCL_FPUPRESERVE)) return DDERR_INVALIDPARAMS;
    if (flags & DDSCL_CREATEDEVICEWINDOW) {
        // Not supported (no game known to use it): the focus window serves as the device window.
        if (!(flags & DDSCL_EXCLUSIVE)) return DDERR_INVALIDPARAMS;
        if (!(flags & DDSCL_FULLSCREEN)) return DDERR_INVALIDPARAMS;
        if (!hwnd_ && !(flags & DDSCL_SETFOCUSWINDOW)) return DDERR_NOFOCUSWINDOW;
        if ((flags & DDSCL_SETFOCUSWINDOW) && !hwnd) return DDERR_NOHWND;
        if (!hwnd) hwnd = hwnd_;
        DD2D9_LOG("  DDSCL_CREATEDEVICEWINDOW: using the focus window %p", static_cast<void*>(hwnd));
    }
    if (!(flags & (DDSCL_NORMAL | DDSCL_EXCLUSIVE | DDSCL_SETFOCUSWINDOW))) return DDERR_INVALIDPARAMS;
    if (hwnd && !IsWindow(hwnd)) return DDERR_INVALIDPARAMS;
    if (flags & DDSCL_SETDEVICEWINDOW) {
        // ddraw.dll's checks; the device window itself isn't kept apart from the focus
        // window here (dd2d9 presents into the focus window).
        if (flags & (DDSCL_SETFOCUSWINDOW | DDSCL_CREATEDEVICEWINDOW)) return DDERR_INVALIDPARAMS;
        if (hwnd && !hwnd_) return DDERR_NOFOCUSWINDOW;
    }
    if ((flags & DDSCL_SETFOCUSWINDOW) && !(flags & DDSCL_CREATEDEVICEWINDOW)) {
        // Only the focus window is set (DDSCL_NOWINDOWCHANGES aside, nothing goes with it).
        if (flags & (DDSCL_MULTITHREADED | DDSCL_FPUSETUP | DDSCL_FPUPRESERVE | DDSCL_ALLOWREBOOT |
                     DDSCL_ALLOWMODEX | DDSCL_SETDEVICEWINDOW | DDSCL_NORMAL | DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN))
            return DDERR_INVALIDPARAMS;
        if (Exclusive()) return DDERR_HWNDALREADYSET;
        if (version == 1) ddraw1Coop_ = true;
        Guard l;
        if (hwnd) hwnd_ = hwnd;
        return DD_OK;
    }
    if ((flags & (DDSCL_EXCLUSIVE | DDSCL_ALLOWMODEX)) && !(flags & DDSCL_FULLSCREEN)) return DDERR_INVALIDPARAMS;
    if ((flags & DDSCL_EXCLUSIVE) && (!hwnd || (GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD)))
        return DDERR_INVALIDPARAMS;
    // A call through IDirectDraw (version 1) that gets past the checks gives the object
    // the DirectDraw 1 behavior below for good, whatever interface is used afterwards;
    // as in ddraw.dll, even if the window then can't be taken (HWNDALREADYSET).
    if (version == 1) ddraw1Coop_ = true;
    // As ddraw.dll: exclusive mode stays on its window until the game goes back to normal.
    if ((flags & DDSCL_EXCLUSIVE) && Exclusive() && hwnd != hwnd_) return DDERR_HWNDALREADYSET;
    // A window with a dialog frame but no border is left alone, as ddraw.dll leaves it.
    if (hwnd && !(flags & (DDSCL_SETDEVICEWINDOW | DDSCL_CREATEDEVICEWINDOW))) {
        const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
        if ((style & WS_DLGFRAME) && !(style & WS_BORDER)) flags |= DDSCL_NOWINDOWCHANGES;
    }
    // One DirectDraw object at a time has exclusive mode. A new one takes it over from
    // the old, as in Wine (ddraw.dll refuses it while the old one is active, with
    // DDERR_EXCLUSIVEMODEALREADYSET); Type Wild, which recreates its DirectDraw object
    // without releasing the old one, hangs otherwise.
    DDraw* const prev = (flags & DDSCL_EXCLUSIVE) && g_exclusiveDD && g_exclusiveDD != this ? g_exclusiveDD : nullptr;

    const bool leaving = Exclusive() && !(flags & DDSCL_EXCLUSIVE);
    // Taking exclusive mode activates the window, as in ddraw.dll (asking again changes
    // nothing).
    const bool acquire = (flags & DDSCL_EXCLUSIVE) && !Exclusive();
    // Leaving exclusive mode restores the desktop mode, except with DirectDraw 1:
    // there the display stays as the game set it (games switch through NORMAL
    // between their screens) until RestoreDisplayMode or the final Release.
    const bool restore = leaving && !ddraw1Coop_;
    // The game's display mode gives way to the desktop's (restored, or virtual and dropped).
    const bool modeBack = restore && (modeSet_ || switched_);
    if (leaving) {
        FlipToGDISurface();  // as ddraw.dll: the screen GDI draws on is in front again
        if (restore) {
            // Still hooked: the window covers the screen in the desktop mode before the game
            // hears of the change (see OnDisplayChange).
            RestoreDisplay();
            RestoreSavedWindow(!(flags & DDSCL_NOWINDOWCHANGES));
        } else {
            suspended_ = false;
            // The window stays as it is but for being topmost, as with ddraw.dll.
            if (!(flags & DDSCL_NOWINDOWCHANGES) && hwnd_ && IsWindow(hwnd_))
                SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            DD2D9_LOG("leaving exclusive mode: the display mode stays (DirectDraw 1)");
        }
        UnhookWindow();
        // Once the game has been in the background in exclusive mode, or when the desktop's
        // display mode comes back, ddraw.dll's leaving it loses the surfaces (not with
        // DirectDraw 1, which leaves the display alone). A lost primary isn't presented: the
        // window, still covering the screen until the game sizes it, doesn't show the
        // fullscreen image placed on the desktop (a black flash on Alt+Enter, JJ2+).
        // Otherwise there is no background to be in out of exclusive mode, so they are
        // restorable again.
        if (restore && (backgrounded_ || modeBack)) InterlockedExchange(&deviceState_, kDevNotRestored);
        else InterlockedCompareExchange(&deviceState_, kDevNotRestored, kDevLost);
        backgrounded_ = false;
    }
    if (prev) TakeOverExclusive(prev, hwnd);
    // Entering exclusive mode loses the surfaces, every DirectDraw object's (those in system
    // memory aside), as in ddraw.dll.
    if (acquire) {
        InterlockedExchange(&deviceState_, kDevNotRestored);
        Guard l;
        for (DDraw* d : g_ddraws)
            if (d != this) d->InvalidateSurfaces();
    }
    if (flags & DDSCL_EXCLUSIVE) g_exclusiveDD = this;
    else if (g_exclusiveDD == this) g_exclusiveDD = nullptr;
    {
        Guard l;
        coop_ = flags;
        if (hwnd) hwnd_ = hwnd;
        if (restore) modeSet_ = false;
        ApplyGamma();
    }
    if (Exclusive()) HookWindow();
    ApplyWindow();
    // After the window is set up, as ddraw.dll: the activation (WM_ACTIVATEAPP, see
    // OnActivate) then makes it topmost.
    // From another thread than the window's, without waiting for it: its WM_ACTIVATEAPP
    // handler may call back into DirectDraw, which this thread holds.
    if (acquire) {
        const bool foreign = GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId();
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | (foreign ? SWP_ASYNCWINDOWPOS : 0));
    }
    NoteDisplayMode();  // leaving exclusive mode may have put the desktop's back
    MarkDirty();
    return DD_OK;
}

HRESULT DDraw::SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD rr, DWORD flags) {
    DD2D9_LOG("SetDisplayMode(%lu, %lu, %lu, %lu, 0x%lx)", w, h, bpp, rr, flags);
    if (flags & ~DWORD(DDSDM_STANDARDVGAMODE)) return DDERR_INVALIDPARAMS;
    if (AnyLocked()) return DDERR_SURFACEBUSY;
    DisplayGuard dg;
    if (g_exclusiveDD && g_exclusiveDD != this) return DDERR_NOEXCLUSIVEMODE;
    // ddraw.dll leaves impossible modes to the display, which refuses them.
    if (!w || !h || w > 16384 || h > 16384) return DDERR_UNSUPPORTED;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return DDERR_UNSUPPORTED;
    {
        Guard l;
        modeSet_ = true;
        modeW_ = w;
        modeH_ = h;
        modeBpp_ = bpp;
        modeRefresh_ = rr;
        suspended_ = false;
    }
    if (Exclusive()) {
        ApplyWindow();
    } else if (g_config.display == kDisplaySwitch && !SwitchMode()) {
        // ddraw.dll changes the display mode in normal mode too (the window stays as it is).
        DD2D9_LOG("display mode %lux%lu not available: it stays virtual", w, h);
    }
    InvalidateSurfaces();
    NoteDisplayMode();
    MarkDirty();
    return DD_OK;
}

// The primary surface is gone while the game's display mode is set: ddraw.dll has the
// window repainted. g_lock held.
void DDraw::ApplyGamma() {
    if (!presenter_) return;
    DDGAMMARAMP r;
    if (Exclusive() && primary && primary->gammaRamp) {
        r = *primary->gammaRamp;
    } else {
        for (int i = 0; i < 256; i++) r.red[i] = r.green[i] = r.blue[i] = WORD(i * 0x101);
    }
    presenter_->SetGamma(r);
}

void DDraw::OnPrimaryReleased() {
    if (modeSet_ && hwnd_ && IsWindow(hwnd_)) RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
}

// The display is in the mode the game set and dd2d9 switched to (read without a lock:
// a stale answer only means one more or one less loss of surfaces).
bool DDraw::InOwnMode() const {
    if (!switched_ || !modeSet_) return false;
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) return false;
    return dm.dmPelsWidth == modeW_ && dm.dmPelsHeight == modeH_;
}

// Someone else changed the display mode (a windowed game's desktop, another program):
// the surfaces are lost, as with ddraw.dll. In exclusive mode WM_DISPLAYCHANGE says so.
void DDraw::PollDisplayChange() {
    // At most every 2 ms: a mode change takes longer than that, so the game's next call
    // after one still sees it, and a blit-heavy game doesn't pay for a check per blit.
    // The clock is the processor's time stamp counter, calibrated against
    // QueryPerformanceCounter: under Wine that one is a system call, too slow for every
    // IsLost (Blt and BltFast check both surfaces).
    static uint64_t tscPer2ms, lastTsc, calTsc;
    static LONGLONG calQpc, lastQpc;
    const uint64_t t = __rdtsc();
    if (tscPer2ms) {
        if (t - lastTsc < tscPer2ms) return;  // another core's counter may be behind: then it polls
    } else {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        if (!calQpc) {
            calQpc = now.QuadPart;
            calTsc = t;
        } else if (now.QuadPart - calQpc >= freq.QuadPart / 20 && t > calTsc) {  // 50 ms measured
            tscPer2ms = (t - calTsc) * uint64_t(freq.QuadPart / 500) / uint64_t(now.QuadPart - calQpc);
        }
        if (lastQpc && now.QuadPart - lastQpc < freq.QuadPart / 500) return;
        lastQpc = now.QuadPart;
    }
    lastTsc = t;
    // Not this object's change to notice: the value stays for other objects to compare.
    if (EmulatedMode()) return;
    const LONG u = LONG(DisplayUniqueness());
    if (!u) return;  // not available
    const LONG before = InterlockedExchange(&g_displayUniqueness, u);
    if (before && before != u) InvalidateSurfaces();
}

// The game's display mode is shown scaled or in a window (borderless, window, or a mode
// the monitor lacks), the real display left alone: a change of the desktop's mode (by
// another program, or a laptop changing its refresh rate on its own) isn't a change of
// the game's, and doesn't lose its surfaces. ddraw.dll's loss frees video memory a new
// mode needs; dd2d9's surfaces need none. Nerf ArenaBlast (Unreal) quits with "Failed
// blt" when a texture upload finds them lost. Read without a lock, as InOwnMode.
bool DDraw::EmulatedMode() const { return Exclusive() && modeSet_ && !switched_; }

bool DDraw::OtherExclusive() const {
    const DDraw* x = g_exclusiveDD;
    return x && x != this;
}

// A video-memory surface is locked (ddraw.dll's SURFACEBUSY for Set- and RestoreDisplayMode).
bool DDraw::AnyLocked() const {
    Guard l;
    for (const Surface* s : surfaces)
        if (s->lockCount && (s->desc.ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY)) return true;
    return false;
}

// The display mode this object takes as its own (TestCooperativeLevel out of exclusive mode).
void DDraw::NoteDisplayMode() { expectedMode_ = ModeHash(); }

// g_lock held (the other object's primary is read).
bool DDraw::Superseded() const {
    const DDraw* x = g_exclusiveDD;
    return x && x != this && x->primary;
}

// `prev` had exclusive mode; this object takes it over (SetCooperativeLevel, g_displayLock
// held). The window's saved style and the desktop mode to go back to come along, and the
// old object drops to normal mode so it neither restores them nor presents.
void DDraw::TakeOverExclusive(DDraw* prev, HWND hwnd) {
    DD2D9_LOG("exclusive mode taken over from DirectDraw object %p", static_cast<void*>(prev));
    if (prev->windowSaved_ && prev->savedHwnd_ == hwnd) {
        if (!windowSaved_) {
            windowSaved_ = true;
            savedHwnd_ = prev->savedHwnd_;
            savedStyle_ = prev->savedStyle_;
            savedExStyle_ = prev->savedExStyle_;
            savedRect_ = prev->savedRect_;
        }
        prev->windowSaved_ = false;
        prev->savedHwnd_ = nullptr;
    }
    if (prev->switched_) {
        switched_ = true;
        prev->switched_ = false;
    }
    if (prev->coverHwnd_ == hwnd) {  // the window it grew past the screen: this one's to undo now
        coverHwnd_ = prev->coverHwnd_;
        coverSize_ = prev->coverSize_;
        prev->coverHwnd_ = nullptr;
        prev->coverSize_ = {};
    }
    prev->suspended_ = false;
    Guard l;
    prev->coop_ = (prev->coop_ & ~(DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWMODEX)) | DDSCL_NORMAL;
    prev->ApplyGamma();
}

HRESULT DDraw::RestoreDisplayMode() {
    DisplayGuard dg;
    if (g_exclusiveDD && g_exclusiveDD != this) return DDERR_NOEXCLUSIVEMODE;
    if (AnyLocked()) return DDERR_SURFACEBUSY;
    if (!modeSet_ && !switched_) return DD_OK;  // nothing set: nothing to restore, nothing lost
    RestoreDisplay();
    {
        Guard l;
        modeSet_ = false;
    }
    ApplyWindow();  // still exclusive: the window covers the screen at the desktop mode
    InvalidateSurfaces();
    NoteDisplayMode();
    MarkDirty();
    return DD_OK;
}

// g_lock held.
void DDraw::UpdateLostSurfaces() {
    // Railroad Tycoon 2 restores its surfaces from its WM_QUERYNEWPALETTE handler, before
    // WM_ACTIVATEAPP has told us the game is in front again (Wine's finding).
    if (deviceState_ == kDevLost && hwnd_ && GetForegroundWindow() == hwnd_)
        InterlockedCompareExchange(&deviceState_, kDevNotRestored, kDevLost);
    if (deviceState_ != kDevNotRestored) return;
    for (Surface* s : surfaces) s->lost = s->CanBeLost();
    InterlockedCompareExchange(&deviceState_, kDevOk, kDevNotRestored);
}

HRESULT DDraw::TestCooperativeLevel() {
    if (!Exclusive()) {
        // As ddraw.dll: another object's exclusive mode, or a display mode other than the
        // one this object last saw set (at creation, or by its own Set/RestoreDisplayMode).
        if (OtherExclusive()) return DDERR_EXCLUSIVEMODEALREADYSET;
        const ULONG m = ModeHash();
        return m && expectedMode_ && m != expectedMode_ ? DDERR_WRONGMODE : DD_OK;
    }
    // Back in front without WM_ACTIVATEAPP having said so yet (see UpdateLostSurfaces).
    if (deviceState_ == kDevLost && hwnd_ && GetForegroundWindow() == hwnd_)
        InterlockedCompareExchange(&deviceState_, kDevNotRestored, kDevLost);
    return deviceState_ == kDevLost ? DDERR_NOEXCLUSIVEMODE : DD_OK;
}

// As ddraw.dll: the surfaces the game created (members go with their root), oldest
// first, up to the first that can't be restored, whose error is the result.
HRESULT DDraw::RestoreAllSurfaces() {
    Guard l;
    const std::vector<Surface*> list = surfaces;  // Restore doesn't create or destroy any
    for (Surface* s : list) {
        if (s->root) continue;
        const HRESULT hr = s->Restore();
        if (FAILED(hr)) return hr;
    }
    return DD_OK;
}

HRESULT DDraw::GetDisplayMode(void* desc, int version) {
    if (!DescSizeOk(desc)) return DDERR_INVALIDPARAMS;
    Guard l;
    DWORD w, h, bpp;
    DisplayMode(&w, &h, &bpp);
    Format f = Format::ForDisplayBpp(bpp);
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
    d.dwWidth = w;
    d.dwHeight = h;
    d.lPitch = LONG(w * f.Bytes());  // as ddraw.dll: not rounded
    // The display's own report, as ddraw.dll gives it (0 for the driver default).
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    d.dwRefreshRate = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 0;
    f.ToDDPF(&d.ddpfPixelFormat);
    DescOut(d, desc);  // only as much as the caller's structure holds
    // ddraw.dll then reports the size of this interface version's structure.
    *static_cast<DWORD*>(desc) = version >= 4 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
    return DD_OK;
}

// The display sizes offered: the classic modes plus whatever the monitor offers. A
// classic mode the monitor lacks still works: it falls back to a scaled borderless window.
static std::vector<SIZE> ModeSizes() {
    static const SIZE kSizes[] = {{320, 200}, {320, 240}, {400, 300}, {512, 384}, {640, 400}, {640, 480},
                                  {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 960},
                                  {1280, 1024}, {1600, 1200}, {1920, 1080}};
    std::vector<SIZE> sizes;
    LONG maxW = GetSystemMetrics(SM_CXSCREEN), maxH = GetSystemMetrics(SM_CYSCREEN);
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &dm); i++) {
        SIZE s = {LONG(dm.dmPelsWidth), LONG(dm.dmPelsHeight)};
        if (s.cx < 320 || s.cy < 200) continue;
        maxW = std::max(maxW, s.cx);
        maxH = std::max(maxH, s.cy);
        sizes.push_back(s);
    }
    for (const SIZE& s : kSizes)
        if (s.cx <= maxW && s.cy <= maxH) sizes.push_back(s);
    sizes.push_back({GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)});
    std::sort(sizes.begin(), sizes.end(),
              [](const SIZE& a, const SIZE& b) { return a.cx != b.cx ? a.cx < b.cx : a.cy < b.cy; });
    sizes.erase(std::unique(sizes.begin(), sizes.end(),
                            [](const SIZE& a, const SIZE& b) { return a.cx == b.cx && a.cy == b.cy; }),
                sizes.end());
    return sizes;
}

HRESULT DDraw::EnumDisplayModes(DWORD flags, const void* filter, void* ctx, void* callback, int descVersion) {
    if (!callback || (flags & ~DWORD(DDEDM_REFRESHRATES | DDEDM_STANDARDVGAMODES))) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 f = {};
    if (filter && !DescIn(filter, &f)) return DDERR_INVALIDPARAMS;
    const std::vector<SIZE> sizes = ModeSizes();
    using CB = HRESULT(WINAPI*)(void*, void*);
    static const DWORD kBpps[] = {8, 16, 32};
    for (DWORD bpp : kBpps) {
        if (filter && (f.dwFlags & DDSD_PIXELFORMAT) && f.ddpfPixelFormat.dwRGBBitCount != bpp) continue;
        for (const SIZE& s : sizes) {
            if (filter && (f.dwFlags & DDSD_WIDTH) && f.dwWidth != DWORD(s.cx)) continue;
            if (filter && (f.dwFlags & DDSD_HEIGHT) && f.dwHeight != DWORD(s.cy)) continue;
            if (filter && (f.dwFlags & DDSD_REFRESHRATE) && f.dwRefreshRate != refresh_) continue;
            Format pf = Format::ForDisplayBpp(bpp);
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
            d.dwWidth = DWORD(s.cx);
            d.dwHeight = DWORD(s.cy);
            d.lPitch = LONG(DWORD(s.cx) * pf.Bytes());  // as ddraw.dll: not rounded
            d.dwRefreshRate = (flags & DDEDM_REFRESHRATES) ? refresh_ : 0;
            pf.ToDDPF(&d.ddpfPixelFormat);
            union {
                DDSURFACEDESC v1;
                DDSURFACEDESC2 v2;
            } out;
            out.v1.dwSize = descVersion >= 2 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
            DescOut(d, &out);
            if (reinterpret_cast<CB>(callback)(&out, ctx) == DDENUMRET_CANCEL) return DD_OK;
        }
    }
    return DD_OK;
}

HRESULT DDraw::GetCaps(DDCAPS* drv, DDCAPS* hel) {
    // As ddraw.dll: the structure of DirectX 1, 3, 5 or 6, both checked before either is written.
    auto sizeOk = [](const DDCAPS* c) {
        return !c || c->dwSize == 0xAC || c->dwSize == 0x13C || c->dwSize == 0x16C || c->dwSize == sizeof(DDCAPS);
    };
    if ((!drv && !hel) || !sizeOk(drv) || !sizeOk(hel)) return DDERR_INVALIDPARAMS;
    DDCAPS c = {};
    c.dwSize = sizeof(c);
    c.dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_PALETTE |
               DDCAPS_CANBLTSYSMEM | DDCAPS_BLTDEPTHFILL | DDCAPS_CANCLIP | DDCAPS_CANCLIPSTRETCHED;
    c.dwCaps2 = DDCAPS2_WIDESURFACES | DDCAPS2_NOPAGELOCKREQUIRED | DDCAPS2_CANRENDERWINDOWED |
                DDCAPS2_PRIMARYGAMMA | DDCAPS2_FLIPNOVSYNC | DDCAPS2_FLIPINTERVAL | DDCAPS2_CERTIFIED;
    c.dwCKeyCaps = DDCKEYCAPS_SRCBLT | DDCKEYCAPS_DESTBLT;  // no key ranges (see SetColorKey)
    c.dwFXCaps = DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY | DDFXCAPS_BLTSHRINKX | DDFXCAPS_BLTSHRINKY |
                 DDFXCAPS_BLTSTRETCHXN | DDFXCAPS_BLTSTRETCHYN | DDFXCAPS_BLTSHRINKXN | DDFXCAPS_BLTSHRINKYN |
                 DDFXCAPS_BLTMIRRORLEFTRIGHT | DDFXCAPS_BLTMIRRORUPDOWN;
    c.dwPalCaps = DDPCAPS_8BIT | DDPCAPS_PRIMARYSURFACE | DDPCAPS_ALLOW256;
    // The raster operations Blt does, as ddraw.dll's software blitter: BLACKNESS,
    // SRCCOPY, WHITENESS (bit n of the array is the ROP with n in its high word).
    for (DWORD rop : {DWORD(BLACKNESS), DWORD(SRCCOPY), DWORD(WHITENESS)}) {
        const DWORD n = (rop >> 16) & 0xFF;
        c.dwRops[n / 32] |= 1u << (n % 32);
    }
    {
        Guard l;
        c.dwVidMemTotal = DWORD(kVidMemTotal);
        c.dwVidMemFree = VidMemFree();
    }
    c.ddsCaps.dwCaps = DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER |
                       DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PALETTE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY |
                       DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_VISIBLE;
    if (D3DProxyEnabled()) {
        // A Direct3D wrapper (D7VK) renders 3D on top of dd2d9 and passes these caps
        // on to the game: report a 3D accelerator, as ddraw does with a 3D driver.
        // Games check this before they try Direct3D at all.
        c.dwCaps |= DDCAPS_3D;
        c.ddsCaps.dwCaps |= DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP;
        c.dwZBufferBitDepths = DDBD_16 | DDBD_24 | DDBD_32;
    }
    c.ddsOldCaps.dwCaps = c.ddsCaps.dwCaps;  // the DirectX 5 field, same value
    c.dwSVBCaps = c.dwCaps;
    c.dwSVBCKeyCaps = c.dwCKeyCaps;
    c.dwSVBFXCaps = c.dwFXCaps;
    c.dwVSBCaps = c.dwCaps;
    c.dwSSBCaps = c.dwCaps;
    for (int i = 0; i < DD_ROP_SPACE; i++) c.dwSVBRops[i] = c.dwVSBRops[i] = c.dwSSBRops[i] = c.dwRops[i];
    DD2D9_TRACE("GetCaps(%p, %p) -> dwCaps 0x%08lx, ddsCaps 0x%08lx%s", static_cast<void*>(drv),
               static_cast<void*>(hel), c.dwCaps, c.ddsCaps.dwCaps, (c.dwCaps & DDCAPS_3D) ? " (3D)" : "");
    // The emulation layer's (HEL) surface caps are a fixed set in ddraw.dll.
    DDCAPS helCaps = c;
    helCaps.ddsCaps.dwCaps = DDSCAPS_FLIP | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_PALETTE | DDSCAPS_PRIMARYSURFACE |
                             DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP;
    helCaps.ddsOldCaps.dwCaps = helCaps.ddsCaps.dwCaps;
    helCaps.dwZBufferBitDepths = DDBD_16;
    helCaps.dwVidMemFree = 0;  // as ddraw.dll
    // An object limited to one side reports no surface caps for the other.
    if (capsSide == kHardwareOnly) helCaps.ddsCaps.dwCaps = helCaps.ddsOldCaps.dwCaps = 0;
    if (capsSide == kEmulationOnly) c.ddsCaps.dwCaps = c.ddsOldCaps.dwCaps = 0;
    for (DDCAPS* out : {drv, hel}) {
        if (!out) continue;
        const DDCAPS& src = out == hel ? helCaps : c;
        const DWORD size = out->dwSize;
        memcpy(out, &src, size);
        out->dwSize = size;
    }
    return DD_OK;
}

HRESULT DDraw::GetMonitorFrequency(DWORD* freq) {
    if (!freq) return DDERR_INVALIDPARAMS;
    *freq = refresh_;
    return DD_OK;
}

HRESULT DDraw::GetScanLine(DWORD* line) {
    if (!line) return DDERR_INVALIDPARAMS;
    DWORD w, h, bpp;
    {
        Guard l;
        DisplayMode(&w, &h, &bpp);
    }
    const DWORD total = h + h / 20 + 1;  // ~5% of the period is vertical blank
    DWORD pos = DWORD(VBlankPhase() * total);
    *line = pos;  // as drivers do, a line past the visible ones during the blank
    return pos >= h ? DDERR_VERTICALBLANKINPROGRESS : DD_OK;
}

HRESULT DDraw::GetVerticalBlankStatus(BOOL* inVb) {
    if (!inVb) return DDERR_INVALIDPARAMS;
    *inVb = VBlankPhase() >= 0.95;
    return DD_OK;
}

// As ddraw.dll: the start or the end of the blank (the same moment here), nothing else.
HRESULT DDraw::WaitForVerticalBlank(DWORD flags, HANDLE event) {
    if (event || (flags & DDWAITVB_BLOCKBEGINEVENT)) return DDERR_UNSUPPORTED;
    if (flags != DDWAITVB_BLOCKBEGIN && flags != DDWAITVB_BLOCKEND) return DDERR_INVALIDPARAMS;
    SleepUntilVBlank(1);
    return DD_OK;
}

HRESULT DDraw::GetAvailableVidMem(const DDSCAPS2* caps, DWORD* total, DWORD* free) {
    // As ddraw.dll: the kinds of memory a game can ask about.
    if (!caps) return DDERR_INVALIDPARAMS;
    if (caps->dwCaps & 0x03000000) return DDERR_INVALIDCAPS;  // reserved bits
    const DWORD notMemory = DDSCAPS_BACKBUFFER | DDSCAPS_COMPLEX | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_PALETTE |
                            DDSCAPS_SYSTEMMEMORY | DDSCAPS_VISIBLE | DDSCAPS_WRITEONLY | DDSCAPS_OWNDC;
    if ((caps->dwCaps & notMemory) || (!total && !free)) return DDERR_INVALIDPARAMS;
    Guard l;
    if (total) *total = DWORD(kVidMemTotal);
    if (free) *free = VidMemFree();
    return DD_OK;
}

HRESULT DDraw::GetDeviceIdentifier(void* id, DWORD flags, bool v2) {
    if (!id) return DDERR_INVALIDPARAMS;
    if (flags & ~DWORD(DDGDI_GETHOSTIDENTIFIER)) {  // as ddraw.dll: the structure zeroed first
        memset(id, 0, v2 ? sizeof(DDDEVICEIDENTIFIER2) : sizeof(DDDEVICEIDENTIFIER));
        return DDERR_INVALIDPARAMS;
    }
    DDDEVICEIDENTIFIER2 d = {};
    if (!AdapterIdentifier(&d)) {  // no Direct3D 9: something that still looks like a driver
        strcpy(d.szDriver, "dd2d9.dll");
        strcpy(d.szDescription, "dd2d9: DirectDraw on Direct3D 9");
        d.liDriverVersion.QuadPart = 0x0006000E00000001LL;
    }
    memcpy(id, &d, v2 ? sizeof(DDDEVICEIDENTIFIER2) : sizeof(DDDEVICEIDENTIFIER));
    return DD_OK;
}

HRESULT DDraw::CreatePalette(DWORD flags, PALETTEENTRY* entries, IDirectDrawPalette** out, IUnknown* outer,
                             int version) {
    // Checks in ddraw.dll's order (spec: re/spec/palette-clipper-exports.md).
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (flags & ~0x7FFu) return DDERR_INVALIDPARAMS;
    if (!coop_) return DDERR_NOCOOPERATIVELEVELSET;
    // ddraw.dll refuses DDPCAPS_PRIMARYSURFACE too; dd2d9 lets it through, for games
    // made on Windows 9x that pass it.
    if (flags & DDPCAPS_PRIMARYSURFACE) DD2D9_LOG("CreatePalette: DDPCAPS_PRIMARYSURFACE (ddraw.dll refuses it)");
    if (flags & (DDPCAPS_PRIMARYSURFACELEFT | DDPCAPS_VSYNC)) return DDERR_INVALIDPARAMS;
    const DWORD size = flags & (DDPCAPS_1BIT | DDPCAPS_2BIT | DDPCAPS_4BIT | DDPCAPS_8BIT);
    if (!size || (size & (size - 1))) return DDERR_INVALIDPARAMS;  // exactly one size
    if ((flags & DDPCAPS_8BITENTRIES) && (flags & (DDPCAPS_ALPHA | DDPCAPS_8BIT))) return DDERR_INVALIDPARAMS;
    if ((flags & DDPCAPS_ALLOW256) && !(flags & DDPCAPS_8BIT)) return DDERR_INVALIDPARAMS;
    if (!entries) return DDERR_INVALIDPARAMS;
    Guard l;
    auto* p = new Palette(this, flags, entries);
    if (version >= 4) {  // made through IDirectDraw4/7: references it, as in ddraw.dll
        AddRef(version);
        p->ddRef = version;
    }
    *out = p;
    return DD_OK;
}

HRESULT DDraw::CreateClipper(DWORD flags, IDirectDrawClipper** out, IUnknown* outer, int version) {
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (flags) return DDERR_INVALIDPARAMS;
    Guard l;
    auto* c = new Clipper(this);
    c->fromDD1 = version < 2;
    if (version >= 4) {  // made through IDirectDraw4/7: references it, as in ddraw.dll
        AddRef(version);
        c->ddRef = version;
    }
    *out = c;
    return DD_OK;
}

// The GDI surface is the one holding the memory the primary surface started with (the
// screen GDI draws on): after a flip, a back buffer.
Surface* DDraw::GdiSurface() const {
    if (!primary) return nullptr;
    Surface* s = primary;
    for (int i = 0; i < 4096; i++) {
        if (s->buf == gdiBuf_) return s;
        s = s->chainNext;
        if (!s || s == primary) break;
    }
    return primary;
}

HRESULT DDraw::GetGDISurface(void** out, int version) {
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    *out = nullptr;
    Surface* s = GdiSurface();
    if (!s) return DDERR_NOTFOUND;
    s->AddRef(Surface::SlotFor(version));
    *out = s->Iface(version);
    return DD_OK;
}

// Flips until the GDI surface is in front again.
HRESULT DDraw::FlipToGDISurface() {
    Surface* front;
    Surface* gdi;
    {
        Guard l;
        front = primary;
        if (!front) return DDERR_NOTFOUND;
        if (!(front->desc.ddsCaps.dwCaps & DDSCAPS_FLIP) || !front->chainNext) return DDERR_NOTFLIPPABLE;
        gdi = GdiSurface();
        if (gdi == front) return DD_OK;
        front->AddRef();  // Flip takes the lock itself
    }
    HRESULT hr = front->Flip(gdi, DDFLIP_WAIT);
    front->Release();
    return hr;
}

HRESULT DDraw::GetSurfaceFromDC(HDC dc, void** out, int version) {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    if (!dc) return DDERR_NOTFOUND;
    Guard l;
    for (DDraw* d : g_ddraws) {  // as ddraw.dll: any DirectDraw object's
        for (Surface* s : d->surfaces) {
            if (s->dcOut == dc) {
                s->AddRef(Surface::SlotFor(version));
                *out = s->Iface(version);
                return DD_OK;
            }
        }
    }
    return DDERR_NOTFOUND;
}

// The GetSurfaceFromDC export: any DirectDraw object's surface, as IDirectDrawSurface.
HRESULT DDraw::FindSurfaceFromDC(HDC dc, void** out, HDC* deviceDC) {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    if (!deviceDC) return E_INVALIDARG;
    *deviceDC = nullptr;
    Guard l;
    for (DDraw* d : g_ddraws) {
        if (FAILED(d->GetSurfaceFromDC(dc, out, 1))) continue;
        // ddraw.dll also gives a display DC (one for the process, kept for good).
        static HDC display = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
        *deviceDC = display;
        return DD_OK;
    }
    return DDERR_NOTFOUND;
}

// Does surface description `have` match what `want` asks for (the fields it flags)?
// ddraw.dll's EnumSurfaces matching (FUN_737637e9): only these fields count, and the
// filter's flags needn't be among the surface's. g_lock held.
static bool DescMatches(const DDSURFACEDESC2& want, const Surface* s) {
    const DWORD F = want.dwFlags;
    const DDSCAPS2& c = s->desc.ddsCaps;
    if ((F & DDSD_CAPS) && (want.ddsCaps.dwCaps != c.dwCaps || want.ddsCaps.dwCaps2 != c.dwCaps2 ||
                            want.ddsCaps.dwCaps3 != c.dwCaps3 || want.ddsCaps.dwCaps4 != c.dwCaps4))
        return false;
    if ((F & DDSD_HEIGHT) && want.dwHeight != DWORD(s->H())) return false;
    if ((F & DDSD_WIDTH) && want.dwWidth != DWORD(s->W())) return false;
    if ((F & DDSD_LPSURFACE) && want.lpSurface != s->buf->bits) return false;
    // A key the surface hasn't set compares as 0.
    auto key = [&](int i, const DDCOLORKEY& k) {
        const DDCOLORKEY have = s->hasCk[i] ? s->ck[i] : DDCOLORKEY{};
        return k.dwColorSpaceLowValue == have.dwColorSpaceLowValue && k.dwColorSpaceHighValue == have.dwColorSpaceHighValue;
    };
    if ((F & DDSD_CKDESTBLT) && !key(kCkDestBlt, want.ddckCKDestBlt)) return false;
    if ((F & DDSD_CKSRCBLT) && !key(kCkSrcBlt, want.ddckCKSrcBlt)) return false;
    if (F & (DDSD_CKDESTOVERLAY | DDSD_CKSRCOVERLAY)) return false;  // no overlay data here
    const bool hasCount = (s->desc.dwFlags & DDSD_BACKBUFFERCOUNT) != 0;
    if ((F & DDSD_BACKBUFFERCOUNT) && want.dwBackBufferCount != (hasCount ? s->desc.dwBackBufferCount : 0)) return false;
    const bool mip = (c.dwCaps & DDSCAPS_MIPMAP) != 0;
    if ((F & DDSD_MIPMAPCOUNT) && want.dwMipMapCount != (mip ? s->desc.dwMipMapCount : 0)) return false;
    if (F & DDSD_PIXELFORMAT) {
        // Only a surface created with a pixel format of its own has one to compare.
        if (!s->ownFormat) return false;
        DDPIXELFORMAT pf;
        s->buf->fmt.ToDDPF(&pf);
        if (memcmp(&want.ddpfPixelFormat, &pf, sizeof(pf)) != 0) return false;
    }
    return true;
}

// Checks in ddraw.dll's order (spec: re/spec/ddraw-object.md, EnumSurfaces). The wrapper
// checked the filter's size for the interface version.
HRESULT DDraw::EnumSurfaces(DWORD flags, const void* filter, void* ctx, void* callback, int version, int ddVersion) {
    if (!callback || IsBadCodePtr(reinterpret_cast<FARPROC>(callback)) || (flags & ~0x1Fu)) return DDERR_INVALIDPARAMS;
    using CB = HRESULT(WINAPI*)(void*, void*, void*);
    DDSURFACEDESC2 want = {};
    if (filter && !DescIn(filter, &want)) return DDERR_INVALIDPARAMS;
    const bool all = flags & DDENUMSURFACES_ALL, match = flags & DDENUMSURFACES_MATCH;
    const bool nomatch = flags & DDENUMSURFACES_NOMATCH, create = flags & DDENUMSURFACES_CANBECREATED;
    if ((all && (match || nomatch)) || (!all && !filter) || (match && nomatch)) return DDERR_INVALIDPARAMS;
    if (create && (!match || all || nomatch)) return DDERR_INVALIDPARAMS;
    if (filter) {
        const DWORD keys = DDSD_CKDESTBLT | DDSD_CKSRCBLT | DDSD_CKDESTOVERLAY | DDSD_CKSRCOVERLAY;
        if ((want.dwFlags & DDSD_LPSURFACE) || (want.ddsCaps.dwCaps & 0x03000000)) return DDERR_INVALIDPARAMS;
        if (want.ddsCaps.dwCaps & DDSCAPS_OPTIMIZED) return DD_OK;  // there are none
        if ((want.ddsCaps.dwCaps2 & 1) || (want.ddsCaps.dwCaps3 & 0xFFFFE000) || want.ddsCaps.dwCaps4)
            return DDERR_INVALIDCAPS;
        if (want.ddsCaps.dwCaps & 0x00800000) return DDERR_INVALIDPARAMS;  // execute buffers
        if (!(want.dwFlags & DDSD_HEIGHT) != !(want.dwFlags & DDSD_WIDTH)) return DDERR_INVALIDPARAMS;
        if (create && (want.dwFlags & keys)) return DDERR_INVALIDPARAMS;
    }
    if (!create && !(flags & DDENUMSURFACES_DOESEXIST)) return DDERR_INVALIDPARAMS;
    auto call = [&](Surface* s, const DDSURFACEDESC2& d) {
        union {
            DDSURFACEDESC v1;
            DDSURFACEDESC2 v2;
        } out;
        if (version >= 4) {
            out.v2.dwSize = sizeof(DDSURFACEDESC2);
            DescOut(d, &out);
        } else {
            DescOutEnum(d, &out.v1);  // as ddraw.dll: the new structure cut short
        }
        return reinterpret_cast<CB>(callback)(s ? s->Iface(version) : nullptr, &out, ctx);
    };
    if (flags & DDENUMSURFACES_CANBECREATED) {
        // The descriptions surfaces could be created with (no surface handed out): the one
        // asked for, or without a size, one for each display mode. As in ddraw.dll, the
        // callback's answer doesn't stop it.
        auto tryCreate = [&](const DDSURFACEDESC2& d) -> bool {
            if (d.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) {
                // Not really created (that would start presenting): the display's description.
                DDSURFACEDESC2 got = {};
                got.dwSize = sizeof(got);
                got.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT;
                DWORD w, h, bpp;
                {
                    Guard l;
                    DisplayMode(&w, &h, &bpp);
                }
                const Format f = Format::ForDisplayBpp(bpp);
                got.dwWidth = w;
                got.dwHeight = h;
                got.lPitch = LONG(((w * f.Bytes()) + 7) & ~7u);
                got.ddsCaps.dwCaps = d.ddsCaps.dwCaps | DDSCAPS_VISIBLE | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
                f.ToDDPF(&got.ddpfPixelFormat);
                call(nullptr, got);
                return true;
            }
            union {
                DDSURFACEDESC v1;
                DDSURFACEDESC2 v2;
            } in;
            in.v1.dwSize = version >= 4 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
            DescOut(d, &in);
            void* iface = nullptr;
            // With the rules of the interface called (IDirectDraw2's, not IDirectDraw's).
            if (FAILED(CreateSurfaceImpl(&in, &iface, nullptr, ddVersion))) return true;
            Surface* s = Surface::From(iface);
            DDSURFACEDESC2 got;
            {
                Guard l;
                s->GetSurfaceDesc(&got);
            }
            static_cast<IUnknown*>(iface)->Release();
            call(nullptr, got);
            return true;
        };
        if (want.dwFlags & (DDSD_WIDTH | DDSD_HEIGHT)) {
            tryCreate(want);
        } else {  // in the display's format, unless the game asks for another
            for (const SIZE& sz : ModeSizes()) {
                DDSURFACEDESC2 d = want;
                d.dwFlags |= DDSD_WIDTH | DDSD_HEIGHT;
                d.dwWidth = DWORD(sz.cx);
                d.dwHeight = DWORD(sz.cy);
                if (!tryCreate(d)) break;
            }
        }
        return DD_OK;
    }
    // The surfaces that exist, newest first.
    std::vector<Surface*> list;
    std::vector<DDSURFACEDESC2> descs;
    {
        Guard l;
        for (size_t k = surfaces.size(); k-- > 0;) {
            Surface* s = surfaces[k];
            if (!all && nomatch == DescMatches(want, s)) continue;
            DDSURFACEDESC2 d;
            s->GetSurfaceDesc(&d);
            s->AddRef(Surface::SlotFor(version));  // the callback releases it
            list.push_back(s);
            descs.push_back(d);
        }
    }
    size_t i = 0;
    for (; i < list.size(); i++) {
        if (call(list[i], descs[i]) == DDENUMRET_CANCEL) {
            i++;
            break;
        }
    }
    for (; i < list.size(); i++) list[i]->Release(Surface::SlotFor(version));  // never handed to the callback
    return DD_OK;
}

HRESULT DDraw::DuplicateSurface(void* src, void** out, int ddVersion) {
    Surface* s = Surface::From(src);
    if (!s || !out) return DDERR_INVALIDPARAMS;
    Guard l;
    if (s->IsLost()) return DDERR_SURFACELOST;
    // A flip chain's front can be duplicated (alone); its members and the primary can't.
    if (s->isPrimary || s->root) return DDERR_CANTDUPLICATE;
    // As ddraw.dll: the duplicate is handed out through the source's interface version,
    // and holds no reference on the DirectDraw object.
    int version = 1;
    for (int v : {2, 3, 4, 7})
        if (src == s->Iface(v)) version = v;
    s->buf->AddRef();
    auto* n = new Surface(this, s->desc, s->buf, version);
    n->sysmemRequested = s->sysmemRequested;
    n->ownFormat = s->ownFormat;
    n->uniqueness = s->uniqueness;  // ddraw.dll keeps one for the memory both share
    n->modeW = s->modeW;
    n->modeH = s->modeH;
    n->modeBpp = s->modeBpp;
    n->AddRef(Surface::SlotFor(version));
    n->ddVersion = ddVersion;
    *out = n->Iface(version);
    return DD_OK;
}

// ---------------------------------------------------------------- CreateSurface

static void FinishCaps(DDSURFACEDESC2* d) {
    // Managed textures live in system memory as far as the game can tell.
    if (d->ddsCaps.dwCaps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE))
        d->ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
    if (!(d->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY))
        d->ddsCaps.dwCaps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    if (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) d->ddsCaps.dwCaps |= DDSCAPS_VISIBLE;
    d->dwFlags |= DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
}

HRESULT DDraw::CreateSurface(const void* inDesc, void** out, IUnknown* outer, int ddVersion) {
    const HRESULT hr = CreateSurfaceImpl(inDesc, out, outer, ddVersion);
    if (FAILED(hr)) DD2D9_LOG("CreateSurface failed: 0x%08lx", hr);
    return hr;
}

static bool PowerOfTwo(DWORD v) { return v && !(v & (v - 1)); }

static DWORD Log2(DWORD v) {
    DWORD n = 0;
    while (v >>= 1) n++;
    return n;
}

// ddraw.dll's caps rules (FUN_73736a8e; spec: re/spec/ddraw-object.md, CreateSurface).
// C has its memory type filled in already (video memory unless system memory was asked for).
static HRESULT CheckCaps(DWORD C, DWORD C2, DWORD C3, DWORD C4, int ddVersion) {
    const DWORD managed = DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE;
    const DWORD kExecute = 0x00800000;  // DDSCAPS_EXECUTEBUFFER
    const bool complex = C & DDSCAPS_COMPLEX, flipping = C & DDSCAPS_FLIP, cube = C2 & DDSCAPS2_CUBEMAP;
    const bool z = C & DDSCAPS_ZBUFFER;
    auto all = [](DWORD v, DWORD bits) { return (v & bits) == bits; };
    if (!C || (C & 0x03000000) || (C2 & 1) || (C3 & 0xFFFFE000)) return DDERR_INVALIDCAPS;  // bit 0: reserved
    if ((!(C2 & DDSCAPS2_VOLUME) && C4) || (C & (DDSCAPS_PALETTE | DDSCAPS_VISIBLE)) ||
        ((C & DDSCAPS_WRITEONLY) && !(C & kExecute) && !(C2 & managed)) ||
        ((C2 & DDSCAPS2_DONOTPERSIST) && !(C2 & managed)) || ((C2 & managed) && !(C & DDSCAPS_TEXTURE)) ||
        all(C2, managed) ||
        ((C2 & (DDSCAPS2_HINTDYNAMIC | DDSCAPS2_HINTSTATIC | DDSCAPS2_OPAQUE)) && !(C & DDSCAPS_TEXTURE)) ||
        all(C2, DDSCAPS2_HINTDYNAMIC | DDSCAPS2_HINTSTATIC) || all(C2, DDSCAPS2_HINTDYNAMIC | DDSCAPS2_OPAQUE) ||
        all(C2, DDSCAPS2_HINTSTATIC | DDSCAPS2_OPAQUE) || (C & DDSCAPS_OPTIMIZED) ||
        ((C & (DDSCAPS_LOCALVIDMEM | DDSCAPS_NONLOCALVIDMEM)) &&
         ((C & DDSCAPS_SYSTEMMEMORY) || all(C, DDSCAPS_LOCALVIDMEM | DDSCAPS_NONLOCALVIDMEM))) ||
        (ddVersion > 1 && (C & (DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER))) || ((C & kExecute) && (C & 0xFF7EB7FF)))
        return DDERR_INVALIDCAPS;
    if (complex &&
        ((C & DDSCAPS_FRONTBUFFER) ||
         ((C & DDSCAPS_BACKBUFFER) && (!(C & (DDSCAPS_ALPHA | DDSCAPS_ZBUFFER)) || flipping)) ||
         !(C & (DDSCAPS_TEXTURE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_OVERLAY | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_BACKBUFFER)) ||
         (!(C & (DDSCAPS_MIPMAP | DDSCAPS_ZBUFFER | DDSCAPS_FLIP | DDSCAPS_ALPHA)) && !cube)))
        return DDERR_INVALIDCAPS;
    if (((C & DDSCAPS_BACKBUFFER) && (C & (DDSCAPS_FRONTBUFFER | DDSCAPS_ALPHA))) || (flipping && !complex) ||
        ((C & DDSCAPS_PRIMARYSURFACE) &&
         ((C & (DDSCAPS_TEXTURE | DDSCAPS_OVERLAY | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_BACKBUFFER)) || (z && !flipping))) ||
        ((C & DDSCAPS_OFFSCREENPLAIN) && (C & (DDSCAPS_TEXTURE | DDSCAPS_OVERLAY | DDSCAPS_BACKBUFFER))) ||
        ((C & DDSCAPS_OVERLAY) &&
         ((C & (DDSCAPS_TEXTURE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_BACKBUFFER)) || (z && !flipping))))
        return DDERR_INVALIDCAPS;
    if (C & DDSCAPS_MIPMAP) {
        if (!(C & DDSCAPS_TEXTURE) || z) return DDERR_INVALIDCAPS;
    } else if (z && (C & DDSCAPS_BACKBUFFER) && !complex) {
        return DDERR_INVALIDCAPS;
    }
    if ((C & DDSCAPS_ALLOCONLOAD) && !(C & DDSCAPS_TEXTURE)) return DDERR_INVALIDCAPS;
    if ((C2 & DDSCAPS2_CUBEMAP_ALLFACES) && !cube) return DDERR_INVALIDCAPS;
    if (cube) {
        if ((C & 0xC3BA87F7) || (C2 & 0xF7A90060) || (C3 & 0xFFFFE5FF) || C4 ||
            !(C & (DDSCAPS_TEXTURE | DDSCAPS_3DDEVICE)))
            return DDERR_INVALIDPARAMS;
        if (!complex) return DDERR_DDSCAPSCOMPLEXREQUIRED;
        if (!(C2 & DDSCAPS2_CUBEMAP_ALLFACES)) return DDERR_INVALIDPARAMS;
    }
    if (C2 & DDSCAPS2_STEREOSURFACELEFT) return DDERR_UNSUPPORTED;  // no stereo here
    return DD_OK;
}

// CreateSurface's checks of the description, in ddraw.dll's order (FUN_73737051,
// FUN_73737235 and its validator FUN_73736cad). `in` loses what the interface version
// doesn't take (the pitch through IDirectDraw/2, or without client memory).
static HRESULT CheckSurfaceDesc(DDSURFACEDESC2& in, int ddVersion, DWORD displayBpp) {
    DWORD& F = in.dwFlags;
    const DWORD C2 = in.ddsCaps.dwCaps2, C3 = in.ddsCaps.dwCaps3, C4 = in.ddsCaps.dwCaps4;
    DWORD C = in.ddsCaps.dwCaps;
    const bool v1 = ddVersion < 4;  // IDirectDraw and IDirectDraw2: the DDSURFACEDESC
    if ((C & DDSCAPS_SYSTEMMEMORY) && (C & DDSCAPS_VIDEOMEMORY)) return DDERR_INVALIDCAPS;
    if (v1 || !(F & DDSD_LPSURFACE)) F &= ~(DDSD_PITCH | DDSD_LINEARSIZE);
    if (F & DDSD_LPSURFACE) {
        // Client memory (IDirectDrawSurface4+ SetSurfaceDesc's sibling).
        if (!(C & DDSCAPS_SYSTEMMEMORY)) return DDERR_INVALIDCAPS;
        if (!in.lpSurface || IsBadReadPtr(in.lpSurface, 4)) return DDERR_INVALIDPARAMS;
        if (C & (DDSCAPS_OPTIMIZED | DDSCAPS_COMPLEX | 0x00800000 | DDSCAPS_OVERLAY | DDSCAPS_PRIMARYSURFACE |
                 DDSCAPS_PRIMARYSURFACELEFT | DDSCAPS_VIDEOPORT))
            return DDERR_INVALIDCAPS;
        if (!(F & DDSD_WIDTH) || !(F & DDSD_HEIGHT)) return DDERR_INVALIDPARAMS;
        if ((reinterpret_cast<uintptr_t>(in.lpSurface) & 3) && ddVersion != 4) return DDERR_INVALIDPARAMS;
        if ((F & DDSD_PIXELFORMAT) && (in.ddpfPixelFormat.dwFlags & DDPF_FOURCC)) {
            DD2D9_LOG("CreateSurface: FourCC surfaces in client memory aren't supported");
            return DDERR_UNSUPPORTED;
        }
        const DWORD bpp = (F & DDSD_PIXELFORMAT) ? in.ddpfPixelFormat.dwRGBBitCount : displayBpp;
        if (!(F & DDSD_PITCH) || in.lPitch < LONG((bpp * in.dwWidth + 7) / 8) || (in.lPitch & 3))
            return DDERR_INVALIDPARAMS;
    }
    const DWORD managed = DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE;
    if (C2 & 1) return DDERR_INVALIDCAPS;  // reserved
    if ((C & (DDSCAPS_LOCALVIDMEM | DDSCAPS_NONLOCALVIDMEM)) && !(C & DDSCAPS_VIDEOMEMORY)) return DDERR_INVALIDCAPS;
    if ((C & DDSCAPS_OWNDC) && !(C & DDSCAPS_SYSTEMMEMORY)) return DDERR_UNSUPPORTED;
    if ((C2 & managed) &&
        (C & (DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_NONLOCALVIDMEM)))
        return DDERR_INVALIDCAPS;
    if (!(C & DDSCAPS_SYSTEMMEMORY) && !(C2 & managed)) C |= DDSCAPS_VIDEOMEMORY;  // ddraw.dll tries it first

    const bool complex = C & DDSCAPS_COMPLEX, primary = C & DDSCAPS_PRIMARYSURFACE, z = C & DDSCAPS_ZBUFFER;
    // A complex mipmap without a level count: as many levels as the size halves.
    if (complex && (C & DDSCAPS_MIPMAP) && !(F & DDSD_MIPMAPCOUNT) &&
        (((F & DDSD_WIDTH) && !PowerOfTwo(in.dwWidth)) || ((F & DDSD_HEIGHT) && !PowerOfTwo(in.dwHeight))))
        return DDERR_INVALIDPARAMS;
    if (C2 & DDSCAPS2_VOLUME) return DDERR_INVALIDCAPS;

    // The validator.
    if (!complex) {
        if (F & (DDSD_BACKBUFFERCOUNT | DDSD_MIPMAPCOUNT)) return DDERR_INVALIDCAPS;  // even a count of 1
    } else if ((F & DDSD_BACKBUFFERCOUNT) && (F & DDSD_MIPMAPCOUNT)) {
        return DDERR_INVALIDPARAMS;
    }
    if ((C & DDSCAPS_FLIP) && (!complex || !(F & DDSD_BACKBUFFERCOUNT) || !in.dwBackBufferCount))
        return DDERR_INVALIDCAPS;
    if (C2 & 2) return DDERR_INVALIDCAPS;  // DX7's DDSCAPS2_RESERVED4 (kernel-mode video ports only)
    if (HRESULT hr = CheckCaps(C, C2, C3, C4, ddVersion); FAILED(hr)) return hr;
    if ((C & DDSCAPS_ALPHA) || (F & DDSD_ALPHABITDEPTH)) return DDERR_INVALIDPARAMS;
    // Z buffers: through the old structure by depth or format, through the new one by format.
    DDPIXELFORMAT& pf = in.ddpfPixelFormat;
    if (!z) {
        if ((F & DDSD_ZBUFFERBITDEPTH) ||
            ((F & DDSD_PIXELFORMAT) && (pf.dwFlags & (DDPF_ZBUFFER | DDPF_STENCILBUFFER))))
            return DDERR_INVALIDPARAMS;
    } else {
        DWORD depth;
        if (v1) {
            if (!(F & DDSD_PIXELFORMAT)) {
                if (!(F & DDSD_ZBUFFERBITDEPTH)) return DDERR_INVALIDPARAMS;
                depth = in.dwMipMapCount;  // dwZBufferBitDepth
            } else {
                if (pf.dwSize != sizeof(DDPIXELFORMAT) || (pf.dwFlags & DDPF_STENCILBUFFER) ||
                    !(pf.dwFlags & DDPF_ZBUFFER))
                    return DDERR_INVALIDPARAMS;
                depth = pf.dwZBufferBitDepth;
            }
        } else {
            if (complex || (F & DDSD_ZBUFFERBITDEPTH) || !(F & DDSD_PIXELFORMAT) || pf.dwSize != sizeof(DDPIXELFORMAT) ||
                !(pf.dwFlags & DDPF_ZBUFFER) || !pf.dwZBitMask)
                return DDERR_INVALIDPARAMS;
            if (pf.dwFlags & DDPF_STENCILBUFFER) return DDERR_UNSUPPORTED;  // no stencil formats here
            depth = pf.dwZBufferBitDepth;
        }
        if (depth < 8) return DDERR_INVALIDPARAMS;
    }
    // Size: none for the primary (it is the screen's), both for anything else.
    if (primary) {
        if (F & (DDSD_WIDTH | DDSD_HEIGHT)) return DDERR_INVALIDPARAMS;
    } else if (!(F & DDSD_WIDTH) || !(F & DDSD_HEIGHT) || !in.dwWidth || !in.dwHeight) {
        return DDERR_INVALIDPARAMS;
    }
    if (C & DDSCAPS_MIPMAP) {
        if ((F & DDSD_MIPMAPCOUNT) && !in.dwMipMapCount) return DDERR_INVALIDPARAMS;
        if (!PowerOfTwo(in.dwWidth) || !PowerOfTwo(in.dwHeight)) return DDERR_INVALIDPARAMS;
        if ((F & DDSD_MIPMAPCOUNT) && Log2(std::max(in.dwWidth, in.dwHeight)) + 1 < in.dwMipMapCount)
            return DDERR_INVALIDPARAMS;
    }
    if (F & DDSD_PIXELFORMAT) {
        if (primary) return DDERR_INVALIDPARAMS;
        Format f;
        if (!z && !Format::FromDDPF(pf, &f)) {
            DD2D9_LOG("CreateSurface: unsupported pixel format (flags 0x%lx, %lu bpp)", pf.dwFlags, pf.dwRGBBitCount);
            return DDERR_INVALIDPIXELFORMAT;
        }
        if (f.BlockCompressed() && ((in.dwWidth | in.dwHeight) & 3)) return DDERR_INVALIDPARAMS;
    }
    // Color keys, as SetColorKey takes them: no overlay keys, no ranges.
    static const DWORD kCkFlags[4] = {DDSD_CKDESTOVERLAY, DDSD_CKDESTBLT, DDSD_CKSRCOVERLAY, DDSD_CKSRCBLT};
    const DDCOLORKEY* keys[4] = {&in.ddckCKDestOverlay, &in.ddckCKDestBlt, &in.ddckCKSrcOverlay, &in.ddckCKSrcBlt};
    for (int i = 0; i < 4; i++) {
        if (!(F & kCkFlags[i])) continue;
        if (kCkFlags[i] & (DDSD_CKDESTOVERLAY | DDSD_CKSRCOVERLAY)) return DDERR_NOCOLORKEYHW;
        if (keys[i]->dwColorSpaceLowValue != keys[i]->dwColorSpaceHighValue) return DDERR_NOCOLORKEYHW;
        if (z) return DDERR_INVALIDSURFACETYPE;  // SetColorKey's answer for a Z buffer
    }
    // Cube maps: through IDirectDraw7, square, a power of two, no back buffers.
    if ((C2 & DDSCAPS2_CUBEMAP) &&
        (ddVersion != 7 || in.dwWidth != in.dwHeight || !PowerOfTwo(in.dwWidth) || (F & DDSD_BACKBUFFERCOUNT)))
        return DDERR_INVALIDPARAMS;
    // What this implementation doesn't have.
    if (C & DDSCAPS_OVERLAY) return DDERR_NOOVERLAYHW;
    if (C & DDSCAPS_VIDEOPORT) return DDERR_INVALIDPARAMS;
    return DD_OK;
}

HRESULT DDraw::CreateSurfaceImpl(const void* inDesc, void** out, IUnknown* outer, int ddVersion) {
    const int version = ddVersion == 2 ? 1 : ddVersion;  // IDirectDraw2 hands out IDirectDrawSurface
    // In ddraw.dll's order: through IDirectDraw4/7 the aggregation check comes first,
    // through IDirectDraw/2 after the cooperative level and the description's size.
    if (outer && ddVersion >= 4) return CLASS_E_NOAGGREGATION;
    Guard l;
    if (!(coop_ & (DDSCL_NORMAL | DDSCL_EXCLUSIVE))) return DDERR_NOCOOPERATIVELEVELSET;
    DDSURFACEDESC2 in;
    if (!inDesc || IsBadReadPtr(inDesc, sizeof(DWORD))) return DDERR_INVALIDPARAMS;
    // Each interface takes exactly its own description.
    if (static_cast<const DDSURFACEDESC*>(inDesc)->dwSize != (ddVersion < 4 ? sizeof(DDSURFACEDESC) : sizeof(DDSURFACEDESC2)))
        return DDERR_INVALIDPARAMS;
    if (!DescIn(inDesc, &in)) return DDERR_INVALIDPARAMS;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    UpdateLostSurfaces();  // the surfaces lost so far, not this new one
    DD2D9_LOG("CreateSurface(flags 0x%lx, caps 0x%lx, %lux%lu, backbuffers %lu)", in.dwFlags, in.ddsCaps.dwCaps,
             (in.dwFlags & DDSD_WIDTH) ? in.dwWidth : 0, (in.dwFlags & DDSD_HEIGHT) ? in.dwHeight : 0,
             (in.dwFlags & DDSD_BACKBUFFERCOUNT) ? in.dwBackBufferCount : 0);
    {
        DWORD dw, dh, bpp;
        DisplayMode(&dw, &dh, &bpp);
        if (HRESULT hr = CheckSurfaceDesc(in, ddVersion, bpp); FAILED(hr)) return hr;
    }
    // ddraw.dll reads the caps even without DDSD_CAPS. None at all makes an offscreen
    // surface, whose caps still say nothing but where its memory is.
    const DWORD caps = in.ddsCaps.dwCaps;
    const DWORD caps2 = in.ddsCaps.dwCaps2;
    const bool flip = (caps & DDSCAPS_FLIP) != 0;
    const bool complex = (caps & DDSCAPS_COMPLEX) != 0;
    const bool cube = (caps2 & DDSCAPS2_CUBEMAP) != 0;

    int w, h;
    Format fmt;
    Surface* replaced = nullptr;  // a lost primary surface the new one takes the place of
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        // As ddraw.dll: a flip chain needs exclusive mode, and while another object has
        // it, this one can't have a primary at all.
        if (flip && !Exclusive()) return DDERR_NOEXCLUSIVEMODE;
        if (!Exclusive() && OtherExclusive()) return DDERR_NOEXCLUSIVEMODE;
        if (primary) {
            // One the display mode change made lost can be replaced (Jazz Jackrabbit 2
            // sets its mode again without releasing its primary). ddraw.dll refuses any
            // second primary; how JJ2 gets past that there wasn't determined.
            // The old one stays lost; this object no longer shows it.
            if (!primary->IsLost()) {
                DD2D9_LOG("CreateSurface: the primary surface %p still exists", static_cast<void*>(primary));
                return DDERR_PRIMARYSURFACEALREADYEXISTS;
            }
            replaced = primary;
        }
        DWORD dw, dh, bpp;
        DisplayMode(&dw, &dh, &bpp);
        w = int(dw);
        h = int(dh);
        fmt = Format::ForDisplayBpp(bpp);
    } else {
        // CheckSurfaceDesc has seen the size, and the format or Z depth.
        w = int(in.dwWidth);
        h = int(in.dwHeight);
        if (in.dwFlags & DDSD_PIXELFORMAT) {
            if (!Format::FromDDPF(in.ddpfPixelFormat, &fmt)) {
                DD2D9_LOG("CreateSurface: unsupported pixel format (flags 0x%lx, %lu bpp)",
                         in.ddpfPixelFormat.dwFlags, in.ddpfPixelFormat.dwRGBBitCount);
                return DDERR_INVALIDPIXELFORMAT;
            }
        } else if (caps & DDSCAPS_ZBUFFER) {
            const DWORD depth = in.dwMipMapCount;  // dwZBufferBitDepth (IDirectDraw/2)
            fmt.zbuf = true;
            fmt.bpp = depth;
            if (depth != 16 && depth != 24 && depth != 32) return DDERR_INVALIDPIXELFORMAT;
        } else {
            DWORD dw, dh, bpp;
            DisplayMode(&dw, &dh, &bpp);
            fmt = Format::ForDisplayBpp(bpp);
        }
    }

    DDSURFACEDESC2 base = in;
    base.dwFlags &= ~(DDSD_LPSURFACE | DDSD_PITCH | DDSD_ZBUFFERBITDEPTH | DDSD_LINEARSIZE);
    // Fields the game didn't flag hold whatever its structure did: not taken over.
    if (!(in.dwFlags & DDSD_BACKBUFFERCOUNT)) base.dwBackBufferCount = 0;
    if (!(in.dwFlags & (DDSD_MIPMAPCOUNT | DDSD_REFRESHRATE))) base.dwMipMapCount = 0;
    if (!(in.dwFlags & DDSD_ALPHABITDEPTH)) base.dwAlphaBitDepth = 0;
    base.lpSurface = nullptr;
    base.dwWidth = DWORD(w);
    base.dwHeight = DWORD(h);
    base.ddsCaps.dwCaps = caps;
    fmt.ToDDPF(&base.ddpfPixelFormat);

    // Mip levels: as many as asked for (CheckSurfaceDesc saw they fit), or (without
    // DDSD_MIPMAPCOUNT) down to a side of 1: the longer side through IDirectDraw7, the
    // shorter one before it.
    DWORD levels = 1;
    if (caps & DDSCAPS_MIPMAP) {
        const int longest = std::max(w, h), shortest = std::min(w, h);
        if (complex) {
            if (in.dwFlags & DDSD_MIPMAPCOUNT) {
                levels = in.dwMipMapCount;
            } else {
                int n = 1;
                while (((ddVersion == 7 ? longest : shortest) >> n) > 0) n++;
                levels = DWORD(n);
            }
        }
        base.dwFlags |= DDSD_MIPMAPCOUNT;
        base.dwMipMapCount = levels;
    }
    // Main surface memory: client memory when DDSD_LPSURFACE is given (CheckSurfaceDesc
    // saw it is system memory with a pitch that holds a row).
    const bool client = (in.dwFlags & DDSD_LPSURFACE) != 0;
    Buffer* b;
    if (client) {
        b = Buffer::CreateExternal(in.lpSurface, w, h, in.lPitch, fmt);
    } else {
        b = Buffer::Create(w, h, fmt);
    }
    if (!b) return DDERR_OUTOFMEMORY;

    const DWORD backs = flip ? in.dwBackBufferCount : 0;  // validated above

    // A cube map's root is its first face; the other faces hang off it, each with its own
    // mip levels. ddraw.dll lists the faces on the root from the last one back.
    static const DWORD kFaces[6] = {DDSCAPS2_CUBEMAP_POSITIVEX, DDSCAPS2_CUBEMAP_NEGATIVEX, DDSCAPS2_CUBEMAP_POSITIVEY,
                                    DDSCAPS2_CUBEMAP_NEGATIVEY, DDSCAPS2_CUBEMAP_POSITIVEZ, DDSCAPS2_CUBEMAP_NEGATIVEZ};
    std::vector<DWORD> faces;
    if (cube)
        for (DWORD f : kFaces)
            if (caps2 & f) faces.push_back(f);
    auto faceDesc = [&](DWORD face) {
        DDSURFACEDESC2 d = base;
        if (face) d.ddsCaps.dwCaps2 = (caps2 & ~DDSCAPS2_CUBEMAP_ALLFACES) | face;
        return d;
    };

    DDSURFACEDESC2 frontDesc = faceDesc(cube ? faces[0] : 0);
    if (flip) frontDesc.ddsCaps.dwCaps |= DDSCAPS_FRONTBUFFER;
    FinishCaps(&frontDesc);
    auto* front = new Surface(this, frontDesc, b, version);
    front->sysmemRequested = (caps & DDSCAPS_SYSTEMMEMORY) != 0;
    front->ownFormat = !(caps & DDSCAPS_PRIMARYSURFACE) && (in.dwFlags & (DDSD_PIXELFORMAT | DDSD_ZBUFFERBITDEPTH));
    if (caps & DDSCAPS_OWNDC) front->uniqueness = 0;
    const int frontSlot = Surface::SlotFor(version);
    front->AddRef(frontSlot);

    // A member of the complex surface, owned by the root (front).
    auto addMember = [&](const DDSURFACEDESC2& d, int mw, int mh) -> Surface* {
        Buffer* mb = Buffer::Create(mw, mh, fmt);
        if (!mb) return nullptr;
        DDSURFACEDESC2 md = d;
        md.dwWidth = DWORD(mw);
        md.dwHeight = DWORD(mh);
        FinishCaps(&md);
        auto* m = new Surface(this, md, mb, version);
        m->sysmemRequested = front->sysmemRequested;
        m->ownFormat = front->ownFormat;
        m->uniqueness = front->uniqueness;
        m->AddRef(Surface::SlotFor(version));  // the root's reference, counted as ddraw.dll does
        m->root = front;
        front->children.push_back(m);
        return m;
    };
    // Mip levels 1.. below a face (or a plain texture), each attached to the one above.
    auto addLevels = [&](Surface* top, const DDSURFACEDESC2& d) -> bool {
        Surface* prev = top;
        int mw = w, mh = h;
        for (DWORD i = 1; i < levels; i++) {
            mw = std::max(mw / 2, 1);
            mh = std::max(mh / 2, 1);
            DDSURFACEDESC2 md = d;
            md.dwMipMapCount = levels - i;
            md.ddsCaps.dwCaps2 |= DDSCAPS2_MIPMAPSUBLEVEL;
            Surface* level = addMember(md, mw, mh);
            if (!level) return false;
            prev->attached.push_back(level);
            prev = level;
        }
        return true;
    };

    if (flip) {
        front->flipChain.push_back(front);
        Surface* prev = front;
        for (DWORD i = 0; i < backs; i++) {
            DDSURFACEDESC2 bd = base;
            bd.dwFlags &= ~DDSD_BACKBUFFERCOUNT;
            bd.dwBackBufferCount = 0;
            bd.ddsCaps.dwCaps = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE)) |
                                (i == 0 ? DDSCAPS_BACKBUFFER : 0);
            Surface* back = addMember(bd, w, h);
            if (!back) {
                front->Release(frontSlot);
                return DDERR_OUTOFMEMORY;
            }
            front->flipChain.push_back(back);
            prev->chainNext = back;
            prev = back;
        }
        prev->chainNext = front;
    } else {
        std::vector<Surface*> others;  // the other cube faces, in order
        for (size_t i = 1; i < faces.size(); i++) {
            const DDSURFACEDESC2 fd = faceDesc(faces[i]);
            Surface* face = addMember(fd, w, h);
            if (!face || !addLevels(face, fd)) {
                front->Release(frontSlot);
                return DDERR_OUTOFMEMORY;
            }
            others.push_back(face);
        }
        for (size_t i = others.size(); i-- > 0;) front->attached.push_back(others[i]);
        if (!addLevels(front, frontDesc)) {
            front->Release(frontSlot);
            return DDERR_OUTOFMEMORY;
        }
    }

    front->ddVersion = ddVersion;
    if (version >= 4) {  // made through IDirectDraw4/7: references it, as in ddraw.dll
        AddRef(version);
        front->ddRef = version;
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        if (replaced) {
            DD2D9_LOG("CreateSurface: replacing the lost primary surface %p", static_cast<void*>(replaced));
            replaced->lost = true;  // lost until the game restores it; no longer shown either way
        }
        front->isPrimary = true;
        primary = front;
        // The exclusive object's new primary makes the other objects' primaries lost, as
        // in ddraw.dll.
        if (Exclusive()) {
            for (DDraw* d : g_ddraws) {
                if (d == this || !d->primary) continue;
                d->primary->lost = true;
                for (Surface* c : d->primary->children) c->lost = true;
            }
        }
        ApplyGamma();  // a new primary starts without a ramp
        gdiBuf_ = front->buf;
        presenter_->Start();
        MarkDirty();
    }
    DD2D9_LOG("  -> %p %dx%d %lubpp%s", static_cast<void*>(front), w, h, fmt.bpp, front->isPrimary ? " (primary)" : "");
    for (size_t i = 1; i < front->flipChain.size(); i++)
        DD2D9_LOG("  -> back buffer %p", static_cast<void*>(front->flipChain[i]));
    *out = front->Iface(version);
    return DD_OK;
}

}  // namespace dd2d9
