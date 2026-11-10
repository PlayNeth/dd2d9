// The DirectDraw object's cooperative level and display mode, and the surfaces they lose.
#include "ddint.h"

#include <algorithm>
#include <intrin.h>

namespace dd2d9 {

void DDraw::DisplayMode(DWORD* w, DWORD* h, DWORD* bpp) const {
    if (modeSet_) {
        *w = modeW_;
        *h = modeH_;
        *bpp = modeBpp_;
        return;
    }
    // Another object (of this process) has put the display in its mode: that is the display's
    // mode for this one too, as after a real switch. DirectShow's video renderer, tried out
    // with an object of its own as KOF 99's intro movie starts, was told the desktop's
    // 1920x1080x32 while the game drew in 640x480x16. (g_lock held by the callers.)
    if (const DDraw* other = g_exclusiveDD; other && other != this && other->ShowsMode()) {
        *w = other->modeW_;
        *h = other->modeH_;
        *bpp = other->modeBpp_;
        return;
    }
    *w = DWORD(GetSystemMetrics(SM_CXSCREEN));
    *h = DWORD(GetSystemMetrics(SM_CYSCREEN));
    *bpp = DWORD(g_config.desktopBpp);  // desktopbpp= (dd2d9 shows any depth on any desktop)
}

// The current display mode as one number (0 if it can't be had).
ULONG ModeHash() {
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

namespace {
// Every DirectDraw object but `except` loses its surfaces, as ddraw.dll's display changes lose
// the video memory of every object on the device (g_lock taken).
void InvalidateOtherObjects(const DDraw* except) {
    Guard l;
    for (DDraw* d : g_ddraws)
        if (d != except) d->InvalidateSurfaces();
}
}  // namespace

// A display mode change of dd2d9's own. Every other DirectDraw object loses its surfaces (this
// one's are lost by the caller, as its state says), and every object takes the new display as
// seen, so that PollDisplayChange doesn't count it again as someone else's change. A change
// that failed changes nothing: an outside change made before it is still each object's to
// notice.
LONG DDraw::ChangeDisplay(DEVMODEW* dm, DWORD flags) {
    const LONG r = ChangeDisplaySettingsExW(nullptr, dm, nullptr, flags, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) return r;
    const LONG u = LONG(DisplayUniqueness());
    Guard l;
    for (DDraw* d : g_ddraws) {
        if (d != this) d->InvalidateSurfaces();
        d->seenUniqueness_ = u;
    }
    return r;
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
    if (suspended_ && hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);  // (see HookProc's WM_PAINT)
    suspended_ = false;
    GameImageChanged();
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
    // In ddraw.dll's order (re/spec/ddraw-object.md, SetCooperativeLevel 3-7).
    if (!(flags & (DDSCL_NORMAL | DDSCL_EXCLUSIVE | DDSCL_SETFOCUSWINDOW))) return DDERR_INVALIDPARAMS;
    if ((flags & (DDSCL_EXCLUSIVE | DDSCL_ALLOWMODEX)) && !(flags & DDSCL_FULLSCREEN)) return DDERR_INVALIDPARAMS;
    if (hwnd && !IsWindow(hwnd)) return DDERR_INVALIDPARAMS;
    const bool create = (flags & DDSCL_CREATEDEVICEWINDOW) != 0;
    if (flags & DDSCL_SETFOCUSWINDOW) {
        // Without CREATEDEVICEWINDOW only the focus window is set (DDSCL_NOWINDOWCHANGES aside,
        // nothing goes with it).
        if (!create && (flags & (DDSCL_MULTITHREADED | DDSCL_FPUSETUP | DDSCL_FPUPRESERVE | DDSCL_ALLOWREBOOT |
                                 DDSCL_ALLOWMODEX | DDSCL_SETDEVICEWINDOW | DDSCL_NORMAL | DDSCL_EXCLUSIVE |
                                 DDSCL_FULLSCREEN)))
            return DDERR_INVALIDPARAMS;
        if (Exclusive()) return DDERR_HWNDALREADYSET;
    }
    if (flags & DDSCL_SETDEVICEWINDOW) {
        // ddraw.dll's checks; the device window itself isn't kept apart from the focus
        // window here (dd2d9 presents into the focus window).
        if (flags & (DDSCL_SETFOCUSWINDOW | DDSCL_CREATEDEVICEWINDOW)) return DDERR_INVALIDPARAMS;
        if (hwnd && !focusWnd_) return DDERR_NOFOCUSWINDOW;
    }
    if (create) {
        // Exclusive mode only, on an object without a window of the game's; without a new focus
        // window, the one set before (and no window given).
        if (!(flags & DDSCL_EXCLUSIVE) || (lclWnd_ && lclWnd_ != deviceWnd_)) return DDERR_INVALIDPARAMS;
        if (!lclWnd_ && !(flags & DDSCL_SETFOCUSWINDOW)) {
            if (hwnd) return DDERR_INVALIDPARAMS;
            if (!focusWnd_) return DDERR_NOFOCUSWINDOW;
        }
    }
    if ((flags & DDSCL_EXCLUSIVE) && !create && (!hwnd || (GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD)))
        return DDERR_INVALIDPARAMS;
    if ((flags & DDSCL_SETFOCUSWINDOW) && !create) {
        if (version == 1) ddraw1Coop_ = true;
        Guard l;
        focusWnd_ = hwnd;  // (NULL too)
        if (hwnd) {
            hwnd_ = hwnd;
            gpu_->SetWindowHint(hwnd);
        }
        GameImageChanged();
        return DD_OK;
    }
    // A call through IDirectDraw (version 1) that gets past the checks gives the object
    // the DirectDraw 1 behavior below for good, whatever interface is used afterwards;
    // as in ddraw.dll, even if the window then can't be taken (HWNDALREADYSET).
    if (version == 1) ddraw1Coop_ = true;
    // A window with a dialog frame but no border is left alone, as ddraw.dll leaves it.
    if (hwnd && !(flags & (DDSCL_SETDEVICEWINDOW | DDSCL_CREATEDEVICEWINDOW))) {
        const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
        if ((style & WS_DLGFRAME) && !(style & WS_BORDER)) flags |= DDSCL_NOWINDOWCHANGES;
    }
    // One DirectDraw object at a time has exclusive mode, and ddraw.dll refuses it to a
    // second while the holder is in front (re/spec/ddraw-object.md, SetCooperativeLevel 10).
    // A holder that lost activation no longer owns it there; here the new object takes over
    // its mode and window state then (and when its window is gone or hidden).
    DDraw* const prev = (flags & DDSCL_EXCLUSIVE) && g_exclusiveDD && g_exclusiveDD != this ? g_exclusiveDD : nullptr;
    if (prev && !prev->suspended_ && prev->hwnd_ && IsWindow(prev->hwnd_) && IsWindowVisible(prev->hwnd_))
        return DDERR_EXCLUSIVEMODEALREADYSET;
    // DDSCL_CREATEDEVICEWINDOW: ddraw.dll's "DirectDrawDeviceWnd", a popup over the monitor
    // owned by the focus window, handing it the keyboard focus. The picture goes there while it
    // is shown (UpdateDeviceWindow); the focus window gets the rest of dd2d9's window handling.
    // The next call with another window (NORMAL with none too) or the object's release
    // destroys it.
    if (create) {
        if (flags & DDSCL_SETFOCUSWINDOW) {
            focusWnd_ = hwnd;
            if (deviceWnd_ && IsWindow(deviceWnd_)) SetWindowLongPtrA(deviceWnd_, 0, LONG_PTR(hwnd));
        }
        // A second call keeps the device window made by the first (lclWnd_ is it: checked
        // above); destroyed only when the window changes (spec, SetCooperativeLevel 12).
        HWND made = lclWnd_;
        if (!made) {
            made = CreateDeviceWindow(focusWnd_);
            if (!made) return DDERR_INVALIDPARAMS;
        }
        if (made != lclWnd_ && deviceWnd_) DestroyDeviceWindow();
        lclWnd_ = made;
        deviceWnd_ = made;
        DD2D9_LOG("  DDSCL_CREATEDEVICEWINDOW: device window %p, focus window %p", static_cast<void*>(made),
                  static_cast<void*>(focusWnd_));
        // ddraw.dll's window hook needs the focus window.
        if (!focusWnd_) return DDERR_NOHWND;
        hwnd = focusWnd_;  // the window dd2d9 drives
    } else {
        if (hwnd != lclWnd_ && deviceWnd_) DestroyDeviceWindow();
        lclWnd_ = hwnd;
        if (!(flags & DDSCL_SETDEVICEWINDOW)) focusWnd_ = hwnd;
    }
    // As ddraw.dll: exclusive mode stays on its window until the game goes back to normal.
    if ((flags & DDSCL_EXCLUSIVE) && Exclusive() && hwnd != hwnd_) return DDERR_HWNDALREADYSET;

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
    // The window gets back its place in the stacking order unless the game asks for it to be
    // left alone, and borderless didn't cover it regardless (WindowChanges; coop_ is still
    // the exclusive mode's here).
    const bool restack = !(flags & DDSCL_NOWINDOWCHANGES) || ((coop_ & DDSCL_NOWINDOWCHANGES) && WindowChanges());
    if (leaving) {
        FlipToGDISurface();  // as ddraw.dll: the screen GDI draws on is in front again
        if (restore) {
            // Still hooked: the window covers the screen in the desktop mode before the game
            // hears of the change (see OnDisplayChange).
            RestoreDisplay();
            RestoreSavedWindow(restack);
        } else {
            if (suspended_ && hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);  // (see HookProc's WM_PAINT)
            suspended_ = false;
            GameImageChanged();
            // The cursor is let go and the foreground unlocked as in ddraw.dll's leaving (even
            // with DirectDraw 1): nothing else would (Uncover isn't run, and the window is
            // unhooked below, so no focus change reaches OnActivate).
            ConfineCursor(false);
            // The window stays as it is but for being topmost, as with ddraw.dll.
            if (restack && hwnd_ && IsWindow(hwnd_))
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
        focusLossOnly_ = false;  // the display mode goes back: everything
        backgrounded_ = false;
    }
    if (prev) TakeOverExclusive(prev, hwnd);
    // Entering exclusive mode loses the surfaces, every DirectDraw object's (those in system
    // memory aside), as in ddraw.dll.
    if (acquire) {
        InterlockedExchange(&deviceState_, kDevNotRestored);
        focusLossOnly_ = false;
        InvalidateOtherObjects(this);
    }
    {
        Guard l;
        if (flags & DDSCL_EXCLUSIVE) g_exclusiveDD = this;
        else if (g_exclusiveDD == this) g_exclusiveDD = nullptr;
        // As ddraw.dll: MULTITHREADED, FPUSETUP and FPUPRESERVE stay once given (spec,
        // SetCooperativeLevel 16). A game that asked to keep its FPU precision and then went
        // windowed would otherwise have its D3D device switch the FPU to single precision.
        coop_ = flags | (coop_ & (DDSCL_MULTITHREADED | DDSCL_FPUSETUP | DDSCL_FPUPRESERVE));
        if (hwnd) {
            hwnd_ = hwnd;
            gpu_->SetWindowHint(hwnd);
        }
        if (restore) modeSet_ = false;
        GameImageChanged();
        ApplyGamma();
    }
    if (Exclusive()) HookWindow();
    ApplyWindow();
    UpdateDeviceWindow();
    // After the window is set up, as ddraw.dll: the activation (WM_ACTIVATEAPP, see
    // OnActivate) then makes it topmost.
    // From another thread than the window's, without waiting for it: its WM_ACTIVATEAPP
    // handler may call back into DirectDraw, which this thread holds.
    if (acquire) {
        const bool foreign = GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId();
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | (foreign ? SWP_ASYNCWINDOWPOS : 0));
        // As ddraw.dll (FUN_7375d557): into the foreground past Windows' foreground lock, its
        // timeout zeroed and a do-nothing mouse event making this process the last to get
        // input. Without it a game started from Explorer can stay behind it, the keyboard
        // going there (Nerf ArenaBlast ignored all input until Alt+Tab).
        if (GetForegroundWindow() != hwnd) {
            PVOID timeout = nullptr;
            SystemParametersInfoA(SPI_GETFOREGROUNDLOCKTIMEOUT, 0, &timeout, 0);
            SystemParametersInfoA(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, nullptr, 0);
            mouse_event(MOUSEEVENTF_WHEEL, 0, 0, 0, 0);
            SetForegroundWindow(hwnd);
            SystemParametersInfoA(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, timeout, 0);
            DD2D9_LOG("exclusive mode: window %p brought to the foreground (%s)", static_cast<void*>(hwnd),
                      GetForegroundWindow() == hwnd ? "done" : "refused");
        }
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
    // In the background (Alt+Tab: OnActivate set it all aside), the mode is only noted: it is
    // set up when the game comes back (OnActivate). A game handling WM_ACTIVATEAPP with a
    // mode change had the display switched and its device window shown topmost over the
    // desktop, and coming back then skipped the restore (its owner, its taskbar style).
    const bool away = Exclusive() && suspended_;
    {
        Guard l;
        modeSet_ = true;
        modeW_ = w;
        modeH_ = h;
        modeBpp_ = bpp;
        modeRefresh_ = rr;
        if (!away) {
            if (suspended_ && hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);  // (see HookProc's WM_PAINT)
            suspended_ = false;
        }
        GameImageChanged();
    }
    if (away) {
        DD2D9_LOG("  in the background: set up when the game comes back");
    } else if (Exclusive()) {
        ApplyWindow();
        UpdateDeviceWindow();
    } else if (g_config.display == kDisplaySwitch && !SwitchMode()) {
        // ddraw.dll changes the display mode in normal mode too (the window stays as it is).
        DD2D9_LOG("display mode %lux%lu not available: it stays virtual", w, h);
    }
    // Every object's surfaces, as ddraw.dll's mode change loses them: the display, as any of
    // them sees it (DisplayMode), is another one now, whether it was switched or not.
    InvalidateSurfaces();
    InvalidateOtherObjects(this);
    NoteDisplayMode();
    MarkDirty();
    return DD_OK;
}

// g_lock held.
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

// The primary surface is gone while the game's display mode is set: ddraw.dll has the
// window repainted.
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
    // Each object keeps its own time and its own last value: one object's polls must neither
    // hide a change from another nor keep it from polling. (g_lock held, out of exclusive
    // mode: Surface::IsLost.)
    static uint64_t tscPer2ms, calTsc;
    static LONGLONG calQpc;
    const uint64_t t = __rdtsc();
    if (tscPer2ms) {
        if (t - pollTsc_ < tscPer2ms) return;  // another core's counter may be behind: then it polls
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
        if (pollQpc_ && now.QuadPart - pollQpc_ < freq.QuadPart / 500) return;
        pollQpc_ = now.QuadPart;
    }
    pollTsc_ = t;
    const LONG u = LONG(DisplayUniqueness());
    if (!u) return;  // not available
    const LONG before = seenUniqueness_;
    seenUniqueness_ = u;
    if (before && before != u) InvalidateSurfaces();
}

// The game's display mode is shown scaled or in a window (borderless, window, or a mode
// the monitor lacks), the real display left alone: a change of the desktop's mode (by
// another program, or a laptop changing its refresh rate on its own) isn't a change of
// the game's, and doesn't lose its surfaces. ddraw.dll's loss frees video memory a new
// mode needs; dd2d9's surfaces need none. Nerf ArenaBlast (Unreal) quits with "Failed
// blt" when a texture upload finds them lost. Read without a lock, as InOwnMode.
bool DDraw::EmulatedMode() const { return Exclusive() && modeSet_ && !switched_; }

// The display is in this object's mode as far as anyone can see: switched to it and still
// there, or shown scaled or in a window over its (still existing) window.
bool DDraw::ShowsMode() const {
    if (!modeSet_ || !hwnd_ || !IsWindow(hwnd_)) return false;
    return EmulatedMode() || InOwnMode();
}

bool DDraw::KeepsSurfacesOnFocus() const { return EmulatedMode() && g_config.display != kDisplaySwitch; }

// g_lock or g_displayLock held.
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
    if (prev->suspended_ && prev->hwnd_) InvalidateRect(prev->hwnd_, nullptr, FALSE);  // (see HookProc's WM_PAINT)
    prev->suspended_ = false;
    Guard l;
    prev->coop_ = (prev->coop_ & ~(DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWMODEX)) | DDSCL_NORMAL;
    GameImageChanged();
    prev->ApplyGamma();
}

HRESULT DDraw::RestoreDisplayMode() {
    DisplayGuard dg;
    if (g_exclusiveDD && g_exclusiveDD != this) return DDERR_NOEXCLUSIVEMODE;
    if (AnyLocked()) return DDERR_SURFACEBUSY;
    if (!modeSet_ && !switched_) return DD_OK;  // nothing set: nothing to restore, nothing lost
    const bool away = Exclusive() && suspended_;  // (see SetDisplayMode: it stays set aside)
    RestoreDisplay();
    {
        Guard l;
        modeSet_ = false;
        if (away) suspended_ = true;
        GameImageChanged();
    }
    if (!away) ApplyWindow();  // still exclusive: the window covers the screen at the desktop mode
    // Every object's surfaces, as ddraw.dll's mode change loses them: the display, as any of
    // them sees it (DisplayMode), is another one now, whether it was switched or not.
    InvalidateSurfaces();
    InvalidateOtherObjects(this);
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
    // Every surface that can be is lost, as in ddraw.dll, which frees their video memory.
    // Back from the background only (Alt+Tab), dd2d9 still has every surface's pixels: those
    // outside the primary's flip chain report lost (IsLost, Restore, ...) but can still be
    // drawn with (Blt, BltFast, Lock, GetDC), so games that don't restore all of theirs keep
    // working (Captain Claw never restores its text sprites: missing text, 2 fps). A
    // deliberate difference; display mode changes leave nothing drawable, as in ddraw.dll.
    const bool focusOnly = focusLossOnly_;
    focusLossOnly_ = false;
    // Shown in a window (window, borderless) the display never left the game's mode: back in
    // front, nothing is lost (the background only made them report it). Need for Speed: High
    // Stakes never restores its flip chain after Alt+Tab: every Flip failed and it quit. A
    // deliberate difference from ddraw.dll, whose switched display did lose them (as
    // display=switch does, even where dd2d9 can't switch to the mode and shows it scaled).
    const bool keep = focusOnly && KeepsSurfacesOnFocus();
    for (Surface* s : surfaces) {
        s->lost = s->CanBeLost() && !keep;
        s->drawableLost = s->lost && focusOnly && !s->InPrimaryChain();
    }
    if (keep) DD2D9_LOG("back in front in dd2d9's own display mode: the surfaces kept");
    InterlockedCompareExchange(&deviceState_, kDevOk, kDevNotRestored);
}

HRESULT DDraw::TestCooperativeLevel() {
    if (!Exclusive()) {
        // As ddraw.dll: another object's exclusive mode, or a display mode other than the
        // one this object last saw set (at creation, or by its own Set/RestoreDisplayMode).
        {
            Guard l;
            if (OtherExclusive()) return DDERR_EXCLUSIVEMODEALREADYSET;
        }
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

}  // namespace dd2d9
