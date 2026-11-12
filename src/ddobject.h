// The DirectDraw object (ddraw.cpp and the dd*.cpp files beside it; their shared internals are
// in ddint.h). Included at the end of objects.h.
#pragma once
#include "objects.h"

namespace dd2d9 {

class DDraw {
public:
    DDraw();
    static DDraw* From(void* iface) { return static_cast<DDraw*>(ImplFromIface(iface)); }
    void* Iface(int version);

    // fromVersion: the interface version called (Direct3D 7 forwards its references there).
    HRESULT QueryInterface(REFIID riid, void** out, int fromVersion = 7);
    // Every interface version (1, 2, 4, 7) counts its own references, as in
    // ddraw.dll; the object is torn down when all of them are released.
    ULONG AddRef(int version);
    ULONG Release(int version);
    void IntAddRef() { InterlockedIncrement(&intRef_); }
    void IntRelease();

    // ddVersion: the IDirectDraw version called (1, 2, 4, 7).
    HRESULT CreateSurface(const void* desc, void** out, IUnknown* outer, int ddVersion);
    // describe: no surface handed out, its description written there instead (with no
    // pixel memory allocated).
    HRESULT CreateSurfaceImpl(const void* desc, void** out, IUnknown* outer, int ddVersion,
                              DDSURFACEDESC2* describe = nullptr);
    HRESULT CreatePalette(DWORD flags, PALETTEENTRY* entries, IDirectDrawPalette** out, IUnknown* outer,
                          int version);
    HRESULT CreateClipper(DWORD flags, IDirectDrawClipper** out, IUnknown* outer, int version);
    HRESULT DuplicateSurface(void* src, void** out, int ddVersion);
    HRESULT EnumDisplayModes(DWORD flags, const void* filter, void* ctx, void* callback, int descVersion);
    HRESULT EnumSurfaces(DWORD flags, const void* filter, void* ctx, void* callback, int version, int ddVersion);
    HRESULT GetCaps(DDCAPS* drv, DDCAPS* hel);
    HRESULT GetDisplayMode(void* desc, int version);
    HRESULT GetGDISurface(void** out, int version);
    HRESULT FlipToGDISurface();
    Surface* GdiSurface() const;  // g_lock held
    HRESULT GetMonitorFrequency(DWORD* freq);
    HRESULT GetScanLine(DWORD* line);
    HRESULT GetVerticalBlankStatus(BOOL* inVb);
    HRESULT Initialize(GUID* guid, int version);
    HRESULT SetCooperativeLevel(HWND hwnd, DWORD flags, int version);
    HRESULT SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags);
    HRESULT RestoreDisplayMode();
    HRESULT WaitForVerticalBlank(DWORD flags, HANDLE event);
    HRESULT GetAvailableVidMem(const DDSCAPS2* caps, DWORD* total, DWORD* free);
    HRESULT GetSurfaceFromDC(HDC dc, void** out, int version);
    static HRESULT FindSurfaceFromDC(HDC dc, void** out, HDC* deviceDC);
    HRESULT GetDeviceIdentifier(void* id, DWORD flags, bool v2);

    // Display state
    void DisplayMode(DWORD* w, DWORD* h, DWORD* bpp) const;
    DWORD RefreshRate() const { return refresh_; }
    HWND PresentWindow() const;
    RECT PresentSource(HWND hwnd) const;
    RECT PresentTarget(HWND hwnd) const;  // g_lock held
    // Where the game's display mode is shown on the real screen, when that isn't 1:1 at the
    // screen's origin (a scaled or windowed mode). g_lock held. See input.cpp.
    bool ImageOnScreen(RECT* image, SIZE* mode) const;
    HWND Window() const { return hwnd_; }
    bool Exclusive() const { return (coop_ & DDSCL_EXCLUSIVE) != 0; }
    // The whole visible image changed (flip, palette, window...). Any thread.
    void MarkDirty() {
        InterlockedExchange(&dirtyAll_, 1);
        if (presenter_) presenter_->MarkDirty();
    }
    // An overlay over the primary changed (overlay.cpp): presented again, the primary's own
    // pixels unchanged (the presenter lays overlays over the frame as it presents). Any thread.
    void MarkOverlayDirty() {
        if (presenter_) presenter_->MarkDirty();
    }
    // Part of the primary surface changed. Called with g_lock held.
    void MarkDirtyRect(const RECT& r) {
        if (r.right <= r.left || r.bottom <= r.top) return;
        if (dirtyRect_.right <= dirtyRect_.left || dirtyRect_.bottom <= dirtyRect_.top) {
            dirtyRect_ = r;
        } else {
            dirtyRect_.left = std::min(dirtyRect_.left, r.left);
            dirtyRect_.top = std::min(dirtyRect_.top, r.top);
            dirtyRect_.right = std::max(dirtyRect_.right, r.right);
            dirtyRect_.bottom = std::max(dirtyRect_.bottom, r.bottom);
        }
        if (presenter_) presenter_->MarkDirty();
    }
    // What changed since the last call, for the presenter (g_lock held). Returns
    // true if everything did; otherwise *r is the changed area (maybe empty).
    bool TakeDirty(RECT* r) {
        *r = dirtyRect_;
        SetRectEmpty(&dirtyRect_);
        return InterlockedExchange(&dirtyAll_, 0) != 0;
    }
    Presenter* GetPresenter() { return presenter_; }
    Gpu& GetGpu() { return *gpu_; }
    void OnActivate(bool active, DWORD thread);  // WM_ACTIVATEAPP while exclusive (thread: its lParam)
    void OnWindowActivate(bool regained = false);  // WM_ACTIVATE: which of the game's windows is in front (g_displayLock held)
    int GameWindowInFront(HWND fg) const;  // OnWindowActivate: 1 its window, 0 another of its windows, -1 neither
    void MinimizeGameWindow();  // on focus loss (OnActivate)
    void KeepTopmost();  // the 1.5 s timer while exclusive: topmost again if that was taken away
    bool WindowChanges() const;  // dd2d9 may move, size and restack the window (see ddwindow.cpp)
    void OnDisplayChange();        // WM_DISPLAYCHANGE while exclusive
    void CheckCover(HWND h, bool moved);  // the window's frame changed (or it moved) in exclusive mode; g_displayLock held
    bool FitCover(HWND h, WINDOWPOS* wp);  // WM_WINDOWPOSCHANGING: the game sizing its covering window back; g_displayLock held
    void OnPrimaryReleased();
    // The primary's gamma ramp while this object is exclusive, as ddraw.dll applies it;
    // the identity otherwise. g_lock held.
    void ApplyGamma();
    // Lost surfaces (see Surface::CanBeLost). kDevNotRestored: every surface that can be
    // lost is about to be marked lost; kDevLost: the exclusive game is in the background.
    enum DeviceState : LONG { kDevOk, kDevNotRestored, kDevLost };
    LONG DevState() const { return deviceState_; }
    bool FocusLossOnly() const { return focusLossOnly_; }
    // A display change: nothing stays drawable (see UpdateLostSurfaces), except in the
    // background, where the display isn't the game's anyway (switch mode's own change back
    // to the desktop's, its late second notice) and the game's comes back with it.
    void InvalidateSurfaces() {
        if (deviceState_ != kDevLost) focusLossOnly_ = false;
        InterlockedCompareExchange(&deviceState_, kDevNotRestored, kDevOk);
    }
    void UpdateLostSurfaces();  // apply a pending kDevNotRestored to the surfaces; g_lock held
    HRESULT TestCooperativeLevel();
    HRESULT RestoreAllSurfaces();
    bool Suspended() const { return suspended_; }  // the game is in the background (focus lost)
    bool Away() const { return suspended_ && !restoring_; }  // ...and not coming back now (OnActivate)
    // Another DirectDraw object has exclusive mode: this one's primary isn't shown.
    bool Superseded() const;
    bool OtherExclusive() const;  // another DirectDraw object has exclusive mode
    bool AnyLocked() const;
    void NoteDisplayMode();
    void PollDisplayChange();     // the display mode changed behind dd2d9's back: surfaces lost
    bool InOwnMode() const;  // the display is in the mode this object set and switched to
    // Back in front after Alt+Tab, the surfaces aren't lost: the game's mode is shown in a
    // window (window, borderless display), the display never left it (see UpdateLostSurfaces).
    bool KeepsSurfacesOnFocus() const;
    HMONITOR GameMonitor(HWND h) const;  // where the game's screen is (see ddwindow.cpp)
    bool EmulatedMode() const;  // exclusive, in a mode set but not switched to (scaled or windowed)
    bool ShowsMode() const;     // the display is in its mode, as far as anyone can see (see ddmode.cpp)
    bool CoverSize(HWND h, SIZE* out) const;  // the size CoverMonitor gave h, frame included
    bool QuietMove() const { return quietMove_; }  // CheckCover's move: hidden from the game
    void TakeOverExclusive(DDraw* prev, HWND hwnd);  // g_displayLock held
    void SleepUntilVBlank(int count);
    void PaceFlip();  // never let Flip() run faster than the refresh rate
    // The primary's flips with vsync=auto: whether this one passed DDFLIP_NOVSYNC and shows a
    // frame Direct3D drew. A game that keeps flipping such frames without vsync, and without
    // waiting for the vblank itself (8 flips in a row), has them presented at once
    // (FlipsWithoutVsync, see Gpu::PrepareTarget): its drawing shares the device with the
    // presents, and waited for them. 8 flips otherwise put the vsync back. g_lock held.
    void CountFlip(bool noVsync);
    // (A read, any thread. Off once the game stops flipping for a quarter second: a menu or
    // movie blitted to the primary, or windowed play, presents at vblanks again.)
    bool FlipsWithoutVsync() const;
    double VBlankPhase() const;  // 0..1 position inside the current refresh period
    bool VBlankClock(int64_t* vblank, int64_t* period) const;  // (see ddtiming.cpp)

    Surface* primary = nullptr;
    std::vector<Surface*> surfaces;
    std::vector<Surface*> detached;  // outlived their complex surface (Surface::Detach): not listed
    // What an 8-bit primary without a palette shows (and 8-bit surfaces without one convert
    // with): the display's hardware palette, as the last palette shown on a primary left it
    // (a game that set one, then made its primary again without, still showed those
    // colours); a grey ramp until any was.
    PALETTEENTRY defaultPalette[256];
    void KeepHardwarePalette(const PALETTEENTRY* entries);  // g_lock held
    void* d3d7 = nullptr;       // dd2d9's own Direct3D 7 object (d3d7obj.cpp), while it lives
    void* d3dLegacy = nullptr;  // dd2d9's own Direct3D 3-6 object (d3d6obj.cpp), while it lives
    bool dd7 = false;           // made by DirectDrawCreateEx (ddraw.dll's DDRAWILCL_DIRECTDRAW7)
    DWORD CoopFlags() const { return coop_; }
    bool alive = true;
    bool initialized = true;

private:
    ~DDraw();
    void ApplyWindow();
    void CoverMonitor(HWND insertAfter, UINT flags);  // client area over the monitor, frame outside
    void ConfineCursor(bool on, bool keepLock = false);  // g_displayLock held
    void Uncover();
    void RestoreSavedWindow(bool windowChanges);
    void HookWindow();
    void UnhookWindow();
    bool SwitchMode();
    LONG ChangeDisplay(DEVMODEW* dm, DWORD flags);  // ChangeDisplaySettingsEx, for every object
    void RestoreDisplay();
    void SaveWindow();
    void UpdateRefreshRate();
    // The display setting in effect: the one ApplyWindow used for the game's mode, or before
    // SetDisplayMode, the configured one.
    int DisplayKind() const { return modeSet_ ? activeDisplay_ : g_config.display; }

    LONG ref_ = 0;
    LONG tornDown_ = 0;
    LONG ifaceRef_[4] = {};  // per interface version: 1, 2, 4, 7
public:
    // Video memory as games see it: the configured total (vram=), minus the video-memory
    // surfaces (g_lock).
    static uint64_t VidMemTotal() { return uint64_t(g_config.vramMB) << 20; }
    enum CapsSide { kBoth, kHardwareOnly, kEmulationOnly };
    CapsSide capsSide = kBoth;  // from DirectDrawCreate's DDCREATE_HARDWAREONLY / DDCREATE_EMULATIONONLY
    uint64_t vidMemUsed = 0;
    DWORD VidMemFree() const {
        return DWORD(vidMemUsed < VidMemTotal() ? VidMemTotal() - vidMemUsed : 0);
    }
private:
    void TearDown();         // the last reference is gone (g_lock not held)
    volatile LONG dirtyAll_ = 1;
    RECT dirtyRect_ = {0, 0, 0, 0};  // guarded by g_lock
    LONG intRef_ = 1;  // released when the last external reference goes away
    void* wraps_[4] = {};
    HWND hwnd_ = nullptr;
    // ddraw.dll's own two windows, as its rules about them see them (hwnd_ above is the one
    // dd2d9 drives: the focus window, also when there is a device window): the object's window
    // (lcl->hWnd: NULL after SetCooperativeLevel(NULL, NORMAL)) and the focus window, and the
    // device window DDSCL_CREATEDEVICEWINDOW made (g_displayLock).
    HWND lclWnd_ = nullptr;
    HWND focusWnd_ = nullptr;
    HWND deviceWnd_ = nullptr;
    void DestroyDeviceWindow();
    void UpdateDeviceWindow();
    DWORD coop_ = 0;
    bool modeSet_ = false;
    DWORD modeW_ = 0, modeH_ = 0, modeBpp_ = 0, modeRefresh_ = 0;
    bool switched_ = false;     // the real display mode was changed
    bool ddraw1Coop_ = false;   // a SetCooperativeLevel through IDirectDraw succeeded (g_displayLock)
    bool suspended_ = false;    // display setup undone because the game lost focus
    bool restoring_ = false;    // ...and being set up again (OnActivate): the image counts as shown
    bool backgrounded_ = false;  // lost focus at some point during this exclusive mode
    bool hiddenWithOwner_ = false;  // hidden on focus loss, its owner minimized (MinimizeGameWindow)
    bool taskbarExChanged_ = false;  // given a taskbar button in the background (MinimizeGameWindow)
    LONG taskbarEx_ = 0;             // ...its own WS_EX_TOOLWINDOW / NOACTIVATE / APPWINDOW bits
    HWND coverHwnd_ = nullptr;   // window CoverMonitor grew past the screen (and gave a region)
    ULONG expectedMode_ = 0;  // NoteDisplayMode
    // The display settings uniqueness this object has accounted for, and when it last looked
    // (PollDisplayChange; g_lock).
    LONG seenUniqueness_ = 0;
    uint64_t pollTsc_ = 0;
    LONGLONG pollQpc_ = 0;
    DWORD recoverStart_ = 0;       // CheckCover: when its count started
    int recoverCount_ = 0;
    bool cursorConfined_ = false;  // ConfineCursor
    bool foregroundLocked_ = false;  // ConfineCursor: LockSetForegroundWindow
    bool dialogInFront_ = false;     // OnWindowActivate: another of the game's windows in front (as last logged)
    SIZE coverSize_ = {};        // its size, frame included
    POINT coverPos_ = {};        // and where it is (its frame's top left)
    RECT coverFrame_ = {};       // the frame of the style it was covered with (FrameOf)
    bool covering_ = false;      // CoverMonitor moving it (FitCover leaves dd2d9's own moves alone)
    volatile bool quietMove_ = false;  // inside CheckCover's CoverMonitor
    bool applying_ = false;     // inside ApplyWindow: focus changes are our own doing
    bool inCoop_ = false;       // inside SetCooperativeLevel (g_displayLock)
    int activeDisplay_ = kDisplaySwitch;  // the display mode ApplyWindow actually used
    HWND savedHwnd_ = nullptr;  // window whose style / position are saved
    bool windowSaved_ = false;  // window style / position saved before taking it over
    LONG savedStyle_ = 0, savedExStyle_ = 0;
    RECT savedRect_ = {};
    DWORD refresh_ = 60;
    LARGE_INTEGER qpcFreq_, qpcBase_;
    int64_t lastFlip_ = 0;
    int64_t paceNext_ = 0;  // under the compositor: when the next frame is due (PaceFlip)
    volatile bool flipsWithoutVsync_ = false;  // (CountFlip)
    int flipRun_ = 0;                          // flips in a row of the other kind than flipsWithoutVsync_
    // QueryPerformanceCounter at the last (CountFlip; read by the presenter's thread, so whole:
    // 64 bits in one interlocked access on 32-bit x86).
    volatile LONG64 lastCountedFlip_ = 0;
    Presenter* presenter_ = nullptr;
    Gpu* gpu_ = nullptr;  // the Direct3D 9 device (gpu.h)
    volatile LONG deviceState_ = kDevOk;
    // The pending kDevNotRestored comes from a focus loss alone (see UpdateLostSurfaces).
    volatile bool focusLossOnly_ = false;
    const Buffer* gdiBuf_ = nullptr;  // the primary's first memory (compared only)
};

}  // namespace dd2d9
