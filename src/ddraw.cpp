// The DirectDraw object: display modes, cooperative level, object creation.
#include "wrappers.h"

#include "d3dproxy.h"

#include <mmsystem.h>

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

ULONG DDraw::AddRef(int version) {
    InterlockedIncrement(&ref_);
    return ULONG(InterlockedIncrement(&ifaceRef_[IfaceIndex(version)]));
}

ULONG DDraw::Release(int version) {
    LONG* own = &ifaceRef_[IfaceIndex(version)];
    LONG r = InterlockedDecrement(own);
    if (r < 0) {  // released more often than referenced: ignore
        InterlockedIncrement(own);
        return 0;
    }
    // Tear down once: a stray AddRef/Release pair after the final release must
    // not run this (and IntRelease) a second time.
    if (InterlockedDecrement(&ref_) == 0 && InterlockedExchange(&tornDown_, 1) == 0) {
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

bool DDraw::ExclusivePresent(UINT* w, UINT* h, UINT* refresh) const {
    if (g_config.present != kPresentExclusive || !Exclusive() || !switched_ || suspended_ || !primary) return false;
    *w = modeW_;
    *h = modeH_;
    *refresh = refresh_;
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

UINT DeferredActivateMsg() {
    static UINT msg = RegisterWindowMessageW(L"DD2D9_DeferredActivate");
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
        default: return;
    }
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
    DD2D9_TRACE("window %p: %s wParam 0x%lx lParam 0x%lx (foreground %p, display %lux%lu)", static_cast<void*>(h),
               name, static_cast<unsigned long>(wp), static_cast<unsigned long>(lp),
               static_cast<void*>(GetForegroundWindow()), dm.dmPelsWidth, dm.dmPelsHeight);
}

LRESULT CALLBACK HookProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (LogLevel() >= 2) TraceMessage(h, msg, wp, lp);
    const UINT deferred = DeferredActivateMsg();
    if (msg == WM_ACTIVATEAPP || (deferred && msg == deferred)) {
        // Another thread may hold the display lock while it waits for this
        // thread (SetWindowPos on our window): never block here, retry later.
        if (TryEnterCriticalSection(&g_displayLock)) {
            if (h == g_hook.hwnd && g_hook.owner) g_hook.owner->OnActivate(wp != 0);
            LeaveCriticalSection(&g_displayLock);
        } else if (deferred) {
            PostMessageW(h, deferred, wp, 0);
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
            if (!owner->InOwnMode()) owner->InvalidateSurfaces();
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
    } else if (msg == WM_SYSCOMMAND && ((wp & 0xFFF0) == SC_SCREENSAVE || (wp & 0xFFF0) == SC_MONITORPOWER)) {
        DDraw* owner = g_hook.owner;
        if (h == g_hook.hwnd && owner && owner->Exclusive()) return 0;
    }
    const WNDPROC orig = g_hook.orig;
    const bool unicode = g_hook.unicode;
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
void DDraw::OnActivate(bool active) {
    if (applying_ || !Exclusive() || !alive) return;
    // Before SetDisplayMode, the window covers the screen at the desktop mode (see ApplyWindow).
    const int display = modeSet_ ? activeDisplay_ : g_config.display;
    if (!active) {
        backgrounded_ = true;  // see SetCooperativeLevel: leaving exclusive mode now loses the surfaces
        ConfineCursor(false);
        // Window mode is an ordinary window; switch and borderless cover the screen.
        const bool cover = display != kDisplayWindow && !suspended_;
        if (cover) {
            DD2D9_LOG("focus lost: %s", switched_ ? "restoring the desktop display mode" : "minimizing");
            suspended_ = true;      // first: the presenter must not create a new device from here on
            DropExclusiveDevice();  // D3D9 wants its fullscreen device gone before the mode changes
            if (switched_) {
                ChangeDisplay(nullptr, 0);
                switched_ = false;
                UpdateRefreshRate();
            }
            SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        // From here on (as in ddraw.dll, once the desktop mode is back) the surfaces are
        // lost and can't be restored; back in front they can (see UpdateLostSurfaces).
        // Window mode stays an ordinary, visible window: the game keeps running.
        if (display != kDisplayWindow) InterlockedExchange(&deviceState_, kDevLost);
        // Minimizing takes the keyboard focus away (WM_KILLFOCUS): the game sees it lost.
        if (cover && IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOWMINNOACTIVE);  // a hidden one stays so
        return;
    }
    InterlockedCompareExchange(&deviceState_, kDevNotRestored, kDevLost);
    if (suspended_) {
        DD2D9_LOG("focus regained: setting up %lux%lu again", modeW_, modeH_);
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
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
void DDraw::ConfineCursor(bool on) {
    if (on && hwnd_) {
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
        cursorConfined_ = ClipCursor(&mi.rcMonitor) != FALSE;
    } else if (cursorConfined_) {
        ClipCursor(nullptr);
        cursorConfined_ = false;
    }
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
        DropExclusiveDevice();
        ChangeDisplay(nullptr, 0);
        switched_ = false;
        UpdateRefreshRate();
    }
    activeDisplay_ = display;
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
// Unconditional with present=exclusive: the presenter thread may be in the
// middle of creating the device, before HasExclusiveDevice() says so.
void DDraw::DropExclusiveDevice() {
    if (presenter_ && g_config.present == kPresentExclusive) {
        if (presenter_->HasExclusiveDevice()) DD2D9_LOG("releasing the exclusive fullscreen device");
        presenter_->DropDevice();
    }
}

void DDraw::RestoreDisplay() {
    DropExclusiveDevice();
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
    // One DirectDraw object at a time has exclusive mode. A new one takes it over from
    // the old, as in Wine (ddraw.dll refuses it while the old one is active, with
    // DDERR_EXCLUSIVEMODEALREADYSET); Type Wild, which recreates its DirectDraw object
    // without releasing the old one, hangs otherwise.
    DDraw* const prev = (flags & DDSCL_EXCLUSIVE) && g_exclusiveDD && g_exclusiveDD != this ? g_exclusiveDD : nullptr;

    // Once a call through IDirectDraw (version 1) succeeds, the object keeps the
    // DirectDraw 1 behavior below, whatever interface is used afterwards.
    if (version == 1) ddraw1Coop_ = true;
    const bool leaving = Exclusive() && !(flags & DDSCL_EXCLUSIVE);
    // Taking a window as the exclusive (focus) window activates it, as in ddraw.dll.
    const bool acquire = (flags & DDSCL_EXCLUSIVE) && (!Exclusive() || hwnd != hwnd_);
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
            DropExclusiveDevice();  // a fullscreen device needs exclusive mode
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
    // Entering exclusive mode (or moving it to another window) loses the surfaces.
    if (acquire) InterlockedExchange(&deviceState_, kDevNotRestored);
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
    const LONG u = LONG(DisplayUniqueness());
    if (!u) return;  // not available
    const LONG before = InterlockedExchange(&g_displayUniqueness, u);
    if (before && before != u) InvalidateSurfaces();
}

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
    prev->DropExclusiveDevice();
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
static bool DescMatches(const DDSURFACEDESC2& want, const DDSURFACEDESC2& have) {
    if ((want.dwFlags & have.dwFlags) != want.dwFlags) return false;
    auto same = [&](DWORD flag, const void* a, const void* b, size_t n) {
        return !(want.dwFlags & flag) || memcmp(a, b, n) == 0;
    };
#define DD2D9_SAME(FLAG, FIELD) same(FLAG, &want.FIELD, &have.FIELD, sizeof(want.FIELD))
    if (!DD2D9_SAME(DDSD_ALPHABITDEPTH, dwAlphaBitDepth) || !DD2D9_SAME(DDSD_BACKBUFFERCOUNT, dwBackBufferCount) ||
        !DD2D9_SAME(DDSD_CAPS, ddsCaps) || !DD2D9_SAME(DDSD_CKDESTBLT, ddckCKDestBlt) ||
        !DD2D9_SAME(DDSD_CKDESTOVERLAY, ddckCKDestOverlay) || !DD2D9_SAME(DDSD_CKSRCBLT, ddckCKSrcBlt) ||
        !DD2D9_SAME(DDSD_CKSRCOVERLAY, ddckCKSrcOverlay) || !DD2D9_SAME(DDSD_HEIGHT, dwHeight) ||
        !DD2D9_SAME(DDSD_LINEARSIZE, dwLinearSize) || !DD2D9_SAME(DDSD_LPSURFACE, lpSurface) ||
        !DD2D9_SAME(DDSD_MIPMAPCOUNT, dwMipMapCount) || !DD2D9_SAME(DDSD_PITCH, lPitch) ||
        !DD2D9_SAME(DDSD_REFRESHRATE, dwRefreshRate) || !DD2D9_SAME(DDSD_TEXTURESTAGE, dwTextureStage) ||
        !DD2D9_SAME(DDSD_WIDTH, dwWidth))
        return false;
#undef DD2D9_SAME
    if (want.dwFlags & DDSD_PIXELFORMAT) {
        Format a, b;
        if (!Format::FromDDPF(want.ddpfPixelFormat, &a) || !Format::FromDDPF(have.ddpfPixelFormat, &b) || !(a == b))
            return false;
    }
    return true;
}

HRESULT DDraw::EnumSurfaces(DWORD flags, const void* filter, void* ctx, void* callback, int version) {
    if (!callback) return DDERR_INVALIDPARAMS;
    using CB = HRESULT(WINAPI*)(void*, void*, void*);
    DDSURFACEDESC2 want = {};
    if (filter && !DescIn(filter, &want)) return DDERR_INVALIDPARAMS;
    const DWORD how = flags & (DDENUMSURFACES_ALL | DDENUMSURFACES_MATCH | DDENUMSURFACES_NOMATCH);
    auto call = [&](Surface* s, const DDSURFACEDESC2& d) {
        union {
            DDSURFACEDESC v1;
            DDSURFACEDESC2 v2;
        } out;
        out.v1.dwSize = version >= 4 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
        DescOut(d, &out);
        return reinterpret_cast<CB>(callback)(s ? s->Iface(version) : nullptr, &out, ctx);
    };
    if (flags & DDENUMSURFACES_CANBECREATED) {
        // The descriptions surfaces could be created with (no surface handed out): the one
        // asked for, or without a size, one for each display mode.
        if (how != DDENUMSURFACES_MATCH || !filter) return DDERR_INVALIDPARAMS;
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
                return call(nullptr, got) != DDENUMRET_CANCEL;
            }
            union {
                DDSURFACEDESC v1;
                DDSURFACEDESC2 v2;
            } in;
            in.v1.dwSize = version >= 4 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
            DescOut(d, &in);
            void* iface = nullptr;
            if (FAILED(CreateSurfaceImpl(&in, &iface, nullptr, version))) return true;
            Surface* s = Surface::From(iface);
            DDSURFACEDESC2 got;
            {
                Guard l;
                s->GetSurfaceDesc(&got);
            }
            static_cast<IUnknown*>(iface)->Release();
            return call(nullptr, got) != DDENUMRET_CANCEL;
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
    if (!(flags & DDENUMSURFACES_DOESEXIST)) return DDERR_INVALIDPARAMS;
    const bool nomatch = (flags & DDENUMSURFACES_NOMATCH) != 0;
    std::vector<Surface*> list;
    std::vector<DDSURFACEDESC2> descs;
    {
        Guard l;
        for (Surface* s : surfaces) {
            DDSURFACEDESC2 d;
            s->GetSurfaceDesc(&d);
            if (!(flags & DDENUMSURFACES_ALL) && filter && nomatch == DescMatches(want, d)) continue;
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

HRESULT DDraw::CreateSurfaceImpl(const void* inDesc, void** out, IUnknown* outer, int ddVersion) {
    const int version = ddVersion == 2 ? 1 : ddVersion;  // IDirectDraw2 hands out IDirectDrawSurface
    if (outer) return CLASS_E_NOAGGREGATION;
    Guard l;
    // In ddraw.dll's order; *out stays untouched until the description is accepted.
    if (!(coop_ & (DDSCL_NORMAL | DDSCL_EXCLUSIVE))) return DDERR_NOCOOPERATIVELEVELSET;
    DDSURFACEDESC2 in;
    if (!DescIn(inDesc, &in)) return DDERR_INVALIDPARAMS;
    // Each interface takes exactly its own description.
    if (static_cast<const DDSURFACEDESC*>(inDesc)->dwSize != (ddVersion < 4 ? sizeof(DDSURFACEDESC) : sizeof(DDSURFACEDESC2)))
        return DDERR_INVALIDPARAMS;
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    UpdateLostSurfaces();  // the surfaces lost so far, not this new one
    // ddraw.dll reads the caps even without DDSD_CAPS. None at all makes an offscreen
    // surface, whose caps still say nothing but where its memory is.
    const DWORD caps = in.ddsCaps.dwCaps;
    const DWORD caps2 = in.ddsCaps.dwCaps2;
    const bool flip = (caps & DDSCAPS_FLIP) != 0;
    const bool complex = (caps & DDSCAPS_COMPLEX) != 0;
    const bool cube = (caps2 & DDSCAPS2_CUBEMAP) != 0;
    // Caps as ddraw.dll validates them, in its order.
    if (complex && !(caps & (DDSCAPS_FLIP | DDSCAPS_MIPMAP)) && !cube) return DDERR_INVALIDCAPS;
    if ((in.dwFlags & DDSD_MIPMAPCOUNT) && !complex) return DDERR_INVALIDCAPS;  // even a count of 1
    if (flip) {
        // A flip chain: complex, with at least one back buffer.
        if (!(in.dwFlags & DDSD_BACKBUFFERCOUNT) || !in.dwBackBufferCount || !complex) return DDERR_INVALIDCAPS;
        if (cube) return DDERR_INVALIDPARAMS;
    } else if (in.dwFlags & DDSD_BACKBUFFERCOUNT) {
        return cube ? DDERR_INVALIDPARAMS : DDERR_INVALIDCAPS;
    }
    // DirectDraw sets these itself; IDirectDraw (1) still accepts (and keeps) them, except
    // a primary back buffer or a flipping front buffer.
    if (ddVersion > 1 ? (caps & (DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER)) != 0
                      : (caps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) ==
                                (DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER) ||
                            (caps & (DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER)) == (DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER))
        return DDERR_INVALIDCAPS;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        if (caps & DDSCAPS_TEXTURE) return DDERR_INVALIDCAPS;
        if (complex && !flip) return DDERR_INVALIDCAPS;
    }
    if ((caps & DDSCAPS_SYSTEMMEMORY) && (caps & DDSCAPS_VIDEOMEMORY)) return DDERR_INVALIDCAPS;
    if ((caps & (DDSCAPS_ALLOCONLOAD | DDSCAPS_MIPMAP)) && !(caps & DDSCAPS_TEXTURE)) return DDERR_INVALIDCAPS;
    if ((caps2 & DDSCAPS2_CUBEMAP_ALLFACES) && !cube) return DDERR_INVALIDCAPS;
    if (cube && !(caps2 & DDSCAPS2_CUBEMAP_ALLFACES)) return DDERR_INVALIDPARAMS;
    const bool managed = (caps2 & (DDSCAPS2_TEXTUREMANAGE | DDSCAPS2_D3DTEXTUREMANAGE)) != 0;
    if (managed && (!(caps & DDSCAPS_TEXTURE) || (caps & (DDSCAPS_VIDEOMEMORY | DDSCAPS_SYSTEMMEMORY))))
        return DDERR_INVALIDCAPS;
    if ((caps & DDSCAPS_WRITEONLY) && !managed) return DDERR_INVALIDCAPS;
    DD2D9_LOG("CreateSurface(flags 0x%lx, caps 0x%lx, %lux%lu, backbuffers %lu)", in.dwFlags, caps,
             (in.dwFlags & DDSD_WIDTH) ? in.dwWidth : 0, (in.dwFlags & DDSD_HEIGHT) ? in.dwHeight : 0,
             (in.dwFlags & DDSD_BACKBUFFERCOUNT) ? in.dwBackBufferCount : 0);

    int w, h;
    Format fmt;
    Surface* replaced = nullptr;  // a lost primary surface the new one takes the place of
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        // As ddraw.dll: a flip chain needs exclusive mode, and while another object has
        // it, this one can't have a primary at all.
        if (coop_ && flip && !Exclusive()) return DDERR_NOEXCLUSIVEMODE;
        if (coop_ && !Exclusive() && OtherExclusive()) return DDERR_NOEXCLUSIVEMODE;
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
        if (!coop_) return DDERR_NOCOOPERATIVELEVELSET;
        if (in.dwFlags & (DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT))
            DD2D9_LOG("CreateSurface: ignoring size/format given for the primary surface");
        DWORD dw, dh, bpp;
        DisplayMode(&dw, &dh, &bpp);
        w = int(dw);
        h = int(dh);
        fmt = Format::ForDisplayBpp(bpp);
    } else {
        if (!(in.dwFlags & DDSD_WIDTH) || !(in.dwFlags & DDSD_HEIGHT) || !in.dwWidth || !in.dwHeight)
            return DDERR_INVALIDPARAMS;
        w = int(in.dwWidth);
        h = int(in.dwHeight);
        if (in.dwFlags & DDSD_PIXELFORMAT) {
            if (!Format::FromDDPF(in.ddpfPixelFormat, &fmt)) {
                DD2D9_LOG("CreateSurface: unsupported pixel format (flags 0x%lx, %lu bpp)",
                         in.ddpfPixelFormat.dwFlags, in.ddpfPixelFormat.dwRGBBitCount);
                return DDERR_INVALIDPIXELFORMAT;
            }
        } else if (caps & DDSCAPS_ZBUFFER) {
            DWORD depth = (in.dwFlags & DDSD_ZBUFFERBITDEPTH) ? in.dwMipMapCount : 16;
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

    // Mip levels: as many as asked for, or (without DDSD_MIPMAPCOUNT) down to a side of 1:
    // the longer side through IDirectDraw7, the shorter one before it. More than the size
    // allows are clamped (ddraw.dll fails those).
    DWORD levels = 1;
    if (caps & DDSCAPS_MIPMAP) {
        const int longest = std::max(w, h), shortest = std::min(w, h);
        int full = 1;
        while ((longest >> full) > 0) full++;  // levels down to 1x1
        if (complex) {
            if (in.dwFlags & DDSD_MIPMAPCOUNT) {
                if (!in.dwMipMapCount) return DDERR_INVALIDPARAMS;
                levels = std::min<DWORD>(in.dwMipMapCount, DWORD(full));
            } else {
                int n = 1;
                while (((ddVersion == 7 ? longest : shortest) >> n) > 0) n++;
                levels = DWORD(n);
            }
        }
        base.dwFlags |= DDSD_MIPMAPCOUNT;
        base.dwMipMapCount = levels;
    }
    // No overlays (GetCaps says so): games then fall back to blitting.
    if (caps & DDSCAPS_OVERLAY) return DDERR_NOOVERLAYHW;
    // Color key ranges need hardware support, which ddraw.dll reports none of.
    {
        static const DWORD kCkFlags[4] = {DDSD_CKDESTOVERLAY, DDSD_CKDESTBLT, DDSD_CKSRCOVERLAY, DDSD_CKSRCBLT};
        const DDCOLORKEY* keys[4] = {&in.ddckCKDestOverlay, &in.ddckCKDestBlt, &in.ddckCKSrcOverlay, &in.ddckCKSrcBlt};
        for (int i = 0; i < 4; i++)
            if ((in.dwFlags & kCkFlags[i]) && keys[i]->dwColorSpaceLowValue != keys[i]->dwColorSpaceHighValue)
                return DDERR_NOCOLORKEYHW;
    }

    // Main surface memory (client memory when DDSD_LPSURFACE is given).
    const bool client = (in.dwFlags & DDSD_LPSURFACE) && !(caps & DDSCAPS_PRIMARYSURFACE);
    if (client) {
        // In system memory only, with an explicit pitch: a multiple of 4 that holds a row.
        if (caps & DDSCAPS_VIDEOMEMORY) return DDERR_INVALIDCAPS;
        if (ddVersion < 4) return DDERR_INVALIDPARAMS;  // client memory came with IDirectDraw4
        const LONG minPitch = LONG(w * fmt.Bytes());
        if (!in.lpSurface || !(in.dwFlags & DDSD_PITCH) || fmt.fourcc || in.lPitch < minPitch || (in.lPitch & 3))
            return DDERR_INVALIDPARAMS;
        base.ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
    }
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
