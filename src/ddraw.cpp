// The DirectDraw object: display modes, cooperative level, object creation.
#include "wrappers.h"

#include "d3dproxy.h"

#include <mmsystem.h>

#include <algorithm>

#include <new>

namespace dd2d9 {

DDraw::DDraw() {
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
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    int v = 0;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirectDraw)) v = 1;
    else if (IsEqualGUID(r, IID_IDirectDraw2)) v = 2;
    else if (IsEqualGUID(r, IID_IDirectDraw4)) v = 4;
    else if (IsEqualGUID(r, IID_IDirectDraw7)) v = 7;
    if (v) {
        AddRef();
        *out = Iface(v);
        return S_OK;
    }
    HRESULT hr = D3DProxyQueryDDraw(this, r, out);  // Direct3D objects for a wrapper such as D7VK
    if (hr != E_NOINTERFACE) return hr;
    DD2D9_LOG("DDraw::QueryInterface(%s): not supported", GuidName(r));
    return E_NOINTERFACE;
}

ULONG DDraw::AddRef() { return ULONG(InterlockedIncrement(&ref_)); }

ULONG DDraw::Release() {
    LONG r = InterlockedDecrement(&ref_);
    // Tear down once: a stray AddRef/Release pair after the final release must
    // not run this (and IntRelease) a second time.
    if (r == 0 && InterlockedExchange(&tornDown_, 1) == 0) {
        DD2D9_LOG("DirectDraw object released");
        const bool stopped = presenter_->Stop();  // must not hold g_lock: the presenter thread takes it
        {
            DisplayGuard dg;
            UnhookWindow();
            RestoreDisplay();
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
    return ULONG(r < 0 ? 0 : r);
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

bool DDraw::PrimaryBusy() const {
    if (!primary) return false;
    if (primary->lockCount || primary->dcOut) return true;
    for (const Surface* s : primary->flipChain)
        if (s->lockCount || s->dcOut) return true;
    return false;
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

UINT DeferredActivateMsg() {
    static UINT msg = RegisterWindowMessageW(L"DD2D9_DeferredActivate");
    return msg;
}

LRESULT CALLBACK HookProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
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
    if (applying_ || !Exclusive() || !modeSet_ || !alive) return;
    if (!active) {
        // Window mode is an ordinary window; switch and borderless cover the screen.
        if (activeDisplay_ == kDisplayWindow || suspended_) return;
        DD2D9_LOG("focus lost: %s", switched_ ? "restoring the desktop display mode" : "minimizing");
        suspended_ = true;      // first: the presenter must not create a new device from here on
        DropExclusiveDevice();  // D3D9 wants its fullscreen device gone before the mode changes
        if (switched_) {
            ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
            switched_ = false;
            UpdateRefreshRate();
        }
        SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(hwnd_, SW_SHOWMINNOACTIVE);
    } else if (suspended_) {
        DD2D9_LOG("focus regained: setting up %lux%lu again", modeW_, modeH_);
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        ApplyWindow();
        // Only now: until the mode is switched back the presenter must not create
        // a device (it would make a windowed one, then replace it right away).
        suspended_ = false;
        MarkDirty();
    }
}

// Exclusive mode: set the display up the way the configuration asks.
//   switch:     change the real display mode and cover the screen (original DirectDraw behaviour);
//               falls back to borderless if the monitor refuses the mode
//   window:     no mode change; a window with a client area of the mode size, centered
//   borderless: no mode change; a borderless window covering the monitor, image scaled
// Called with g_displayLock held and g_lock not held.
void DDraw::ApplyWindow() {
    if (!Exclusive() || !modeSet_ || !hwnd_ || !IsWindow(hwnd_)) return;
    applying_ = true;  // focus changes caused by our own mode / window changes are not the user's
    SaveWindow();      // before the mode switch, which may move or clamp the window
    int display = g_config.display;
    if (display == kDisplaySwitch && !SwitchMode()) {
        DD2D9_LOG("display mode %lux%lu not available, using a borderless scaled window", modeW_, modeH_);
        display = kDisplayBorderless;
    }
    if (display != kDisplaySwitch && switched_) {
        DropExclusiveDevice();
        ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        switched_ = false;
        UpdateRefreshRate();
    }
    activeDisplay_ = display;

    LONG style = GetWindowLongW(hwnd_, GWL_STYLE);
    LONG exStyle = GetWindowLongW(hwnd_, GWL_EXSTYLE);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
    if (display == kDisplayWindow) {
        RECT r = {0, 0, LONG(modeW_), LONG(modeH_)};
        AdjustWindowRectEx(&r, DWORD(style), GetMenu(hwnd_) != nullptr, DWORD(exStyle));
        const RECT& wa = mi.rcWork;
        int w = r.right - r.left, h = r.bottom - r.top;
        int x = std::max<int>(wa.left, wa.left + ((wa.right - wa.left) - w) / 2);
        int y = std::max<int>(wa.top, wa.top + ((wa.bottom - wa.top) - h) / 2);
        SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_SHOWWINDOW);
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowLongW(hwnd_, GWL_EXSTYLE, exStyle & ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME));
        const RECT& m = mi.rcMonitor;
        SetWindowPos(hwnd_, HWND_TOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    applying_ = false;
    static const char* kNames[] = {"display mode switched", "centered window", "borderless window"};
    DD2D9_LOG("%lux%lu: %s", modeW_, modeH_, kNames[display]);
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
    LONG r = ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_FULLSCREEN, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL && (dm.dmFields & DM_DISPLAYFREQUENCY)) {
        dm.dmFields &= ~DM_DISPLAYFREQUENCY;
        r = ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_FULLSCREEN, nullptr);
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
    if (switched_) {
        LONG r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        DD2D9_LOG("display mode restored (%ld)", r);
        switched_ = false;
        UpdateRefreshRate();
    }
    RestoreSavedWindow();
    suspended_ = false;
}

void DDraw::RestoreSavedWindow() {
    if (windowSaved_ && IsWindow(savedHwnd_)) {
        SetWindowLongW(savedHwnd_, GWL_STYLE, savedStyle_);
        SetWindowLongW(savedHwnd_, GWL_EXSTYLE, savedExStyle_);
        const RECT& r = savedRect_;
        SetWindowPos(savedHwnd_, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
    windowSaved_ = false;
    savedHwnd_ = nullptr;
}

void DDraw::SaveWindow() {
    if (windowSaved_ && savedHwnd_ == hwnd_) return;
    RestoreSavedWindow();  // exclusive mode moved to another window: give the old one back first
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

// The display mode changed while a primary exists: give the chain new memory.
// Called with g_lock held; the caller has checked PrimaryBusy().
void DDraw::ResizePrimary() {
    if (!primary) return;
    DWORD w, h, bpp;
    DisplayMode(&w, &h, &bpp);
    Format f = Format::ForDisplayBpp(bpp);
    std::vector<Surface*> chain = primary->flipChain;
    if (chain.empty()) chain.push_back(primary);
    for (Surface* s : chain) {
        if (s->buf->w == int(w) && s->buf->h == int(h) && s->buf->fmt == f) continue;
        Buffer* b = Buffer::Create(int(w), int(h), f);
        if (!b) continue;
        s->ReplaceBuffer(b);
        s->desc.dwWidth = w;
        s->desc.dwHeight = h;
        f.ToDDPF(&s->desc.ddpfPixelFormat);
    }
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
    // Validation as in ddraw.dll.
    if (flags & ~0x3FFFu) return DDERR_INVALIDPARAMS;
    if ((flags & (DDSCL_EXCLUSIVE | DDSCL_ALLOWMODEX)) && !(flags & DDSCL_FULLSCREEN)) return DDERR_INVALIDPARAMS;
    if (!(flags & (DDSCL_NORMAL | DDSCL_EXCLUSIVE | DDSCL_SETFOCUSWINDOW | DDSCL_SETDEVICEWINDOW)))
        return DDERR_INVALIDPARAMS;
    if (hwnd && !IsWindow(hwnd)) return DDERR_INVALIDPARAMS;
    if ((flags & DDSCL_EXCLUSIVE) && (!hwnd || (GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD)))
        return DDERR_INVALIDPARAMS;

    DisplayGuard dg;
    // Once a call through IDirectDraw (version 1) succeeds, the object keeps the
    // DirectDraw 1 behavior below, whatever interface is used afterwards.
    if (version == 1) ddraw1Coop_ = true;
    if ((flags & (DDSCL_SETFOCUSWINDOW | DDSCL_SETDEVICEWINDOW)) && !(flags & (DDSCL_NORMAL | DDSCL_EXCLUSIVE))) {
        Guard l;
        if (hwnd) hwnd_ = hwnd;
        return DD_OK;
    }
    const bool leaving = Exclusive() && !(flags & DDSCL_EXCLUSIVE);
    // Leaving exclusive mode restores the desktop mode, except with DirectDraw 1:
    // there the display stays as the game set it (games switch through NORMAL
    // between their screens) until RestoreDisplayMode or the final Release.
    const bool restore = leaving && !ddraw1Coop_;
    if (leaving) {
        UnhookWindow();
        if (restore) {
            RestoreDisplay();
        } else {
            DropExclusiveDevice();  // a fullscreen device needs exclusive mode
            suspended_ = false;
            DD2D9_LOG("leaving exclusive mode: the display mode stays (DirectDraw 1)");
        }
    }
    {
        Guard l;
        coop_ = flags;
        if (hwnd) hwnd_ = hwnd;
        if (restore) {
            modeSet_ = false;
            if (!PrimaryBusy()) ResizePrimary();
        }
    }
    if (Exclusive()) HookWindow();
    ApplyWindow();
    MarkDirty();
    return DD_OK;
}

HRESULT DDraw::SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD rr, DWORD flags) {
    DD2D9_LOG("SetDisplayMode(%lu, %lu, %lu, %lu, 0x%lx)", w, h, bpp, rr, flags);
    if (!w || !h || w > 16384 || h > 16384) return DDERR_INVALIDPARAMS;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return DDERR_INVALIDMODE;
    DisplayGuard dg;
    {
        Guard l;
        if (PrimaryBusy()) return DDERR_SURFACEBUSY;  // its memory is about to be replaced
        modeSet_ = true;
        modeW_ = w;
        modeH_ = h;
        modeBpp_ = bpp;
        modeRefresh_ = rr;
        suspended_ = false;
        ResizePrimary();
    }
    ApplyWindow();
    MarkDirty();
    return DD_OK;
}

HRESULT DDraw::RestoreDisplayMode() {
    DisplayGuard dg;
    RestoreDisplay();
    Guard l;
    modeSet_ = false;
    if (!PrimaryBusy()) ResizePrimary();
    MarkDirty();
    return DD_OK;
}

HRESULT DDraw::GetDisplayMode(void* desc) {
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
    d.lPitch = LONG(((w * f.Bytes()) + 3) & ~3u);
    d.dwRefreshRate = refresh_;
    f.ToDDPF(&d.ddpfPixelFormat);
    DescOut(d, desc);
    return DD_OK;
}

HRESULT DDraw::EnumDisplayModes(DWORD flags, const void* filter, void* ctx, void* callback, int descVersion) {
    if (!callback) return DDERR_INVALIDPARAMS;
    DDSURFACEDESC2 f = {};
    if (filter && !DescIn(filter, &f)) return DDERR_INVALIDPARAMS;
    static const SIZE kSizes[] = {{320, 200}, {320, 240}, {400, 300}, {512, 384}, {640, 400}, {640, 480},
                                  {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 960},
                                  {1280, 1024}, {1600, 1200}, {1920, 1080}};
    // The classic modes plus whatever the monitor offers. A classic mode the
    // monitor lacks still works: it falls back to a scaled borderless window.
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

    using CB = HRESULT(WINAPI*)(void*, void*);
    static const DWORD kBpps[] = {8, 16, 32};
    for (DWORD bpp : kBpps) {
        if (filter && (f.dwFlags & DDSD_PIXELFORMAT) && f.ddpfPixelFormat.dwRGBBitCount != bpp) continue;
        for (const SIZE& s : sizes) {
            if (filter && (f.dwFlags & DDSD_WIDTH) && f.dwWidth != DWORD(s.cx)) continue;
            if (filter && (f.dwFlags & DDSD_HEIGHT) && f.dwHeight != DWORD(s.cy)) continue;
            Format pf = Format::ForDisplayBpp(bpp);
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE;
            d.dwWidth = DWORD(s.cx);
            d.dwHeight = DWORD(s.cy);
            d.lPitch = LONG(((DWORD(s.cx) * pf.Bytes()) + 3) & ~3u);
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
    if (!drv && !hel) return DDERR_INVALIDPARAMS;
    DDCAPS c = {};
    c.dwSize = sizeof(c);
    c.dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_COLORKEY | DDCAPS_PALETTE |
               DDCAPS_CANBLTSYSMEM | DDCAPS_BLTDEPTHFILL | DDCAPS_CANCLIP | DDCAPS_CANCLIPSTRETCHED;
    c.dwCaps2 = DDCAPS2_WIDESURFACES | DDCAPS2_NOPAGELOCKREQUIRED | DDCAPS2_CANRENDERWINDOWED |
                DDCAPS2_PRIMARYGAMMA | DDCAPS2_FLIPNOVSYNC | DDCAPS2_FLIPINTERVAL | DDCAPS2_CERTIFIED;
    c.dwCKeyCaps = DDCKEYCAPS_SRCBLT | DDCKEYCAPS_SRCBLTCLRSPACE | DDCKEYCAPS_DESTBLT | DDCKEYCAPS_DESTBLTCLRSPACE;
    c.dwFXCaps = DDFXCAPS_BLTSTRETCHX | DDFXCAPS_BLTSTRETCHY | DDFXCAPS_BLTSHRINKX | DDFXCAPS_BLTSHRINKY |
                 DDFXCAPS_BLTSTRETCHXN | DDFXCAPS_BLTSTRETCHYN | DDFXCAPS_BLTSHRINKXN | DDFXCAPS_BLTSHRINKYN |
                 DDFXCAPS_BLTMIRRORLEFTRIGHT | DDFXCAPS_BLTMIRRORUPDOWN;
    c.dwPalCaps = DDPCAPS_8BIT | DDPCAPS_PRIMARYSURFACE | DDPCAPS_ALLOW256;
    c.dwVidMemTotal = 512u << 20;
    c.dwVidMemFree = 480u << 20;
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
    c.dwSVBCaps = c.dwCaps;
    c.dwSVBCKeyCaps = c.dwCKeyCaps;
    c.dwSVBFXCaps = c.dwFXCaps;
    c.dwVSBCaps = c.dwCaps;
    c.dwSSBCaps = c.dwCaps;
    DD2D9_TRACE("GetCaps(%p, %p) -> dwCaps 0x%08lx, ddsCaps 0x%08lx%s", static_cast<void*>(drv),
               static_cast<void*>(hel), c.dwCaps, c.ddsCaps.dwCaps, (c.dwCaps & DDCAPS_3D) ? " (3D)" : "");
    for (DDCAPS* out : {drv, hel}) {
        if (!out) continue;
        DWORD size = out->dwSize;
        if (size < sizeof(DWORD) || size > sizeof(DDCAPS)) return DDERR_INVALIDPARAMS;
        memcpy(out, &c, size);
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
    if (pos >= h) return DDERR_VERTICALBLANKINPROGRESS;
    *line = pos;
    return DD_OK;
}

HRESULT DDraw::GetVerticalBlankStatus(BOOL* inVb) {
    if (!inVb) return DDERR_INVALIDPARAMS;
    *inVb = VBlankPhase() >= 0.95;
    return DD_OK;
}

HRESULT DDraw::WaitForVerticalBlank(DWORD flags, HANDLE) {
    if (flags & DDWAITVB_BLOCKBEGINEVENT) return DDERR_UNSUPPORTED;
    if (flags & DDWAITVB_BLOCKEND) {
        SleepUntilVBlank(1);
        return DD_OK;
    }
    SleepUntilVBlank(1);
    return DD_OK;
}

HRESULT DDraw::GetAvailableVidMem(DWORD* total, DWORD* free) {
    if (total) *total = 512u << 20;
    if (free) *free = 480u << 20;
    return DD_OK;
}

HRESULT DDraw::GetDeviceIdentifier(void* id, DWORD, bool v2) {
    if (!id) return DDERR_INVALIDPARAMS;
    DDDEVICEIDENTIFIER2 d = {};
    strcpy(d.szDriver, "dd2d9");
    strcpy(d.szDescription, "dd2d9: DirectDraw on Direct3D 9");
    d.liDriverVersion.QuadPart = 0x0006000E00000001LL;
    memcpy(id, &d, v2 ? sizeof(DDDEVICEIDENTIFIER2) : sizeof(DDDEVICEIDENTIFIER));
    return DD_OK;
}

HRESULT DDraw::CreatePalette(DWORD flags, PALETTEENTRY* entries, IDirectDrawPalette** out, IUnknown* outer) {
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!out) return DDERR_INVALIDPARAMS;
    if (!(flags & (DDPCAPS_1BIT | DDPCAPS_2BIT | DDPCAPS_4BIT | DDPCAPS_8BIT))) return DDERR_INVALIDPARAMS;
    Guard l;
    *out = new Palette(this, flags, entries);
    return DD_OK;
}

HRESULT DDraw::CreateClipper(DWORD, IDirectDrawClipper** out, IUnknown* outer) {
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    *out = new Clipper(this);
    return DD_OK;
}

HRESULT DDraw::GetGDISurface(void** out, int version) {
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    if (!primary) return DDERR_NOTFOUND;
    primary->AddRef();
    *out = primary->Iface(version);
    return DD_OK;
}

HRESULT DDraw::GetSurfaceFromDC(HDC dc, void** out, int version) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (!dc) return DDERR_INVALIDPARAMS;
    Guard l;
    for (Surface* s : surfaces) {
        if (s->dcOut == dc) {
            s->AddRef();
            *out = s->Iface(version);
            return DD_OK;
        }
    }
    *out = nullptr;
    return DDERR_NOTFOUND;
}

HRESULT DDraw::EnumSurfaces(DWORD flags, const void*, void* ctx, void* callback, int version) {
    if (!callback) return DDERR_INVALIDPARAMS;
    if (!(flags & DDENUMSURFACES_DOESEXIST)) {
        DD2D9_STUB("EnumSurfaces(CANBECREATED)");
        return DDERR_UNSUPPORTED;
    }
    std::vector<Surface*> list;
    {
        Guard l;
        list = surfaces;
        for (Surface* s : list) s->AddRef();
    }
    using CB = HRESULT(WINAPI*)(void*, void*, void*);
    size_t i = 0;
    for (; i < list.size(); i++) {
        DDSURFACEDESC2 d;
        {
            Guard l;
            list[i]->GetSurfaceDesc(&d);
        }
        union {
            DDSURFACEDESC v1;
            DDSURFACEDESC2 v2;
        } out;
        out.v1.dwSize = version >= 4 ? sizeof(DDSURFACEDESC2) : sizeof(DDSURFACEDESC);
        DescOut(d, &out);
        if (reinterpret_cast<CB>(callback)(list[i]->Iface(version), &out, ctx) == DDENUMRET_CANCEL) {
            i++;
            break;
        }
    }
    for (; i < list.size(); i++) list[i]->Release();
    return DD_OK;
}

HRESULT DDraw::DuplicateSurface(void* src, void** out, int version) {
    Surface* s = Surface::From(src);
    if (!s || !out) return DDERR_INVALIDPARAMS;
    Guard l;
    if (s->isPrimary || s->root || !s->flipChain.empty()) return DDERR_CANTDUPLICATE;
    s->buf->AddRef();
    auto* n = new Surface(this, s->desc, s->buf, version);
    n->AddRef();
    *out = n->Iface(version);
    return DD_OK;
}

// ---------------------------------------------------------------- CreateSurface

static void FinishCaps(DDSURFACEDESC2* d) {
    if (!(d->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY))
        d->ddsCaps.dwCaps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    if (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) d->ddsCaps.dwCaps |= DDSCAPS_VISIBLE;
    d->dwFlags |= DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
}

HRESULT DDraw::CreateSurface(const void* inDesc, void** out, IUnknown* outer, int version) {
    const HRESULT hr = CreateSurfaceImpl(inDesc, out, outer, version);
    if (FAILED(hr)) DD2D9_LOG("CreateSurface failed: 0x%08lx", hr);
    return hr;
}

HRESULT DDraw::CreateSurfaceImpl(const void* inDesc, void** out, IUnknown* outer, int version) {
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    DDSURFACEDESC2 in;
    if (!DescIn(inDesc, &in)) return DDERR_INVALIDPARAMS;
    Guard l;
    const DWORD caps = (in.dwFlags & DDSD_CAPS) ? in.ddsCaps.dwCaps : DDSCAPS_OFFSCREENPLAIN;
    DD2D9_LOG("CreateSurface(flags 0x%lx, caps 0x%lx, %lux%lu, backbuffers %lu)", in.dwFlags, caps,
             (in.dwFlags & DDSD_WIDTH) ? in.dwWidth : 0, (in.dwFlags & DDSD_HEIGHT) ? in.dwHeight : 0,
             (in.dwFlags & DDSD_BACKBUFFERCOUNT) ? in.dwBackBufferCount : 0);

    int w, h;
    Format fmt;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        if (primary) {
            DD2D9_LOG("CreateSurface: the primary surface %p still exists", static_cast<void*>(primary));
            return DDERR_PRIMARYSURFACEALREADYEXISTS;
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
    base.dwFlags &= ~(DDSD_LPSURFACE | DDSD_PITCH | DDSD_ZBUFFERBITDEPTH);
    base.dwWidth = DWORD(w);
    base.dwHeight = DWORD(h);
    base.ddsCaps.dwCaps = caps;
    fmt.ToDDPF(&base.ddpfPixelFormat);

    // Main surface memory (client memory when DDSD_LPSURFACE is given).
    Buffer* b;
    if ((in.dwFlags & DDSD_LPSURFACE) && in.lpSurface && !(caps & DDSCAPS_PRIMARYSURFACE)) {
        LONG pitch = (in.dwFlags & DDSD_PITCH) ? in.lPitch : LONG(w * fmt.Bytes());
        if (fmt.fourcc || pitch < LONG(w * fmt.Bytes())) return DDERR_INVALIDPARAMS;
        b = Buffer::CreateExternal(in.lpSurface, w, h, pitch, fmt);
    } else {
        b = Buffer::Create(w, h, fmt);
    }
    if (!b) return DDERR_OUTOFMEMORY;

    const bool flip = (caps & DDSCAPS_FLIP) != 0;
    const DWORD backs = (in.dwFlags & DDSD_BACKBUFFERCOUNT) ? in.dwBackBufferCount : 0;
    if (flip && (backs == 0 || !(caps & DDSCAPS_COMPLEX))) {
        b->Release();
        return DDERR_INVALIDCAPS;
    }

    DDSURFACEDESC2 frontDesc = base;
    if (flip) frontDesc.ddsCaps.dwCaps |= DDSCAPS_FRONTBUFFER;
    FinishCaps(&frontDesc);
    auto* front = new Surface(this, frontDesc, b, version);
    front->AddRef();

    if (flip) {
        front->flipChain.push_back(front);
        Surface* prev = front;
        for (DWORD i = 0; i < backs; i++) {
            Buffer* bb = Buffer::Create(w, h, fmt);
            if (!bb) {
                front->Release();
                return DDERR_OUTOFMEMORY;
            }
            DDSURFACEDESC2 bd = base;
            bd.dwFlags &= ~DDSD_BACKBUFFERCOUNT;
            bd.dwBackBufferCount = 0;
            bd.ddsCaps.dwCaps = (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER | DDSCAPS_VISIBLE)) |
                                (i == 0 ? DDSCAPS_BACKBUFFER : 0);
            FinishCaps(&bd);
            auto* back = new Surface(this, bd, bb, version);
            back->AddRef();  // the root's reference
            back->root = front;
            front->children.push_back(back);
            front->flipChain.push_back(back);
            prev->chainNext = back;
            prev = back;
        }
        prev->chainNext = front;
    }

    // Mipmap levels for textures.
    if ((caps & DDSCAPS_MIPMAP) && (in.dwFlags & DDSD_MIPMAPCOUNT) && in.dwMipMapCount > 1 && !flip) {
        Surface* prev = front;
        int mw = w, mh = h;
        for (DWORD i = 1; i < in.dwMipMapCount && (mw > 1 || mh > 1); i++) {
            mw = mw > 1 ? mw / 2 : 1;
            mh = mh > 1 ? mh / 2 : 1;
            Buffer* mb = Buffer::Create(mw, mh, fmt);
            if (!mb) break;
            DDSURFACEDESC2 md = base;
            md.dwWidth = DWORD(mw);
            md.dwHeight = DWORD(mh);
            md.dwMipMapCount = in.dwMipMapCount - i;
            md.ddsCaps.dwCaps2 |= DDSCAPS2_MIPMAPSUBLEVEL;
            FinishCaps(&md);
            auto* level = new Surface(this, md, mb, version);
            level->AddRef();  // the root's reference
            level->root = front;
            front->children.push_back(level);
            prev->attached.push_back(level);
            prev = level;
        }
    }

    if (caps & DDSCAPS_PRIMARYSURFACE) {
        front->isPrimary = true;
        primary = front;
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
