// dd2d9 - DirectDraw (DX1-7) 2D implementation on top of Direct3D 9.
// Shared declarations: logging, configuration, global lock, pixel formats.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ddraw.h>
// d3d9.h is only included by the Direct3D 9 side (gpu.cpp, gpuint.h, the presenter, quadpass.h,
// devstate.h, main.cpp): it conflicts with d3d.h.

#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace dd2d9 {

// ---------------------------------------------------------------- config
// (DD2D9_VERSION: the project's version, from meson.build.)

enum { kDisplaySwitch, kDisplayWindow, kDisplayBorderless };
enum { kVSyncAuto, kVSyncOn, kVSyncOff };
enum { kD3DAuto, kD3DOn, kD3DOff };
enum { kFlipCopyAuto, kFlipCopyOn, kFlipCopyOff };
enum { kMinimizeAuto, kMinimizeOn, kMinimizeOff };

// Defaults behave like the original DirectDraw.
struct Config {
    int display = kDisplaySwitch;  // exclusive-mode games: real mode switch / centered window / borderless scaled
    int minimize = kMinimizeAuto;  // the fullscreen window minimized on focus loss: auto (not under i3, sway), on, off
    int stretch = 0;               // window & borderless scaling: 0 aspect, 1 fill, 2 integer
    int filter = 2;                // 0 point, 1 linear, 2 sharp (integer point prescale, then linear; default)
    int vsync = kVSyncAuto;        // auto: Flip() follows the game's flags (DDFLIP_NOVSYNC); on / off: forced
    int d3d = kD3DAuto;            // dd2d9's Direct3D (3-7): auto / on = offered, off = none (2D only)
    bool systemD3D9 = true;        // present through Windows' own d3d9.dll, not one in the game folder (DXVK)
    bool dpiAware = true;          // dpiaware=on: the process handles the display's scaling, window mode scaled (dpi.cpp)
    int gdiFullscreen = kMinimizeAuto;  // gdifullscreen=: a GDI game's display switch shown by dd2d9 (gdiscreen.cpp): auto (when the loader loaded dd2d9), on, off
    bool mouseMerge = true;        // mousemerge=on: a DirectInput mouse's buffered moves merged (dinputmouse.cpp)
    // Compatibility (compatsys.cpp): what the game is told about the machine.
    int ramMB = 1024;              // GlobalMemoryStatus(Ex) physical memory; 0: the real values
    int vramMB = 512;              // DirectDraw's video memory (GetCaps, GetAvailableVidMem)
    int cpuMHz = 2000;             // the processor clock in the registry (~MHz); 0: the real one
    int cpus = 1;                  // processors the game's threads run on and it is told about; 0: all
    int desktopBpp = 32;           // the desktop's colour depth DirectDraw reports (8, 16, 24 or 32)
    bool lockWhole = false;        // a Lock of a rectangle counts as a write to the whole surface
    bool zAccess = true;           // zaccess=off: Z buffers on the GPU only, DirectDraw can't read or write them
    int fpsLimit = 0;              // frames per second at most (Flip, full blits to the primary); 0: off
    bool texLinear = false;        // texfilter=linear: point-filtered textures drawn linear
    int msaa = 0;                  // antialiasing: samples per pixel (2-16); 0/1 off
    bool aa2d = false;             // antialias2d=on: 2D draws (HUDs) antialiased too; off: drawn single-sampled
    bool render32 = false;         // 16-bit render targets drawn in 32 bits on the GPU
    bool depth24 = false;          // 16-bit Z buffers made 24-bit on the GPU
    bool overlays = true;          // videooverlay=on: overlay surfaces offered (emulated: laid over the primary when shown)
    bool palTextures = true;       // palettizedtextures=off: no 8-bit palettized texture format offered to Direct3D
    bool heapZero = true;          // heapzero=on: the game's HeapAlloc memory comes zeroed (compat.cpp)
    bool fairLocks = true;         // fairlocks=on: a released critical section goes to the thread waiting for it (compat.cpp)
    bool selfSuspend = false;      // selfsuspend=off: a sound callback suspending its own thread carries on (compat.cpp)
    int flipCopy = kFlipCopyAuto;  // flipcopy=auto: after a flip the back buffer gets the frame just shown, with Retained Mode loaded (attach.cpp)
    bool noBuffering = false;      // nobuffering=off: FILE_FLAG_NO_BUFFERING dropped from the game's CreateFile (compat.cpp)
    bool safeTerminate = true;     // safeterminate=on: a terminated thread doesn't take the loader's lock with it (compat.cpp)
    bool joyPoll = true;           // joypoll=on: the game's joystick reads answered from dd2d9's own (compat.cpp)
    bool safeTimers = true;        // safetimers=on: timeSetEvent callbacks called with their registers kept (compat.cpp)
    bool drawTrace = false;        // log one frame's draws (render states, depths) every ~10 s
    int overlayBias = 1;           // passes laid on drawn surfaces pulled forward N 16-bit depth steps (gpudraw.cpp)
    int wbuffer = 1;               // W buffering: 0 off, 1 offered (emulated where the GPU lacks it), 2 forced on Z too
    bool mipmaps = false;          // the GPU makes smaller levels for textures the game gave none
    int anisotropy = 0;            // anisotropic filtering forced on smooth (linear) textures: 2-16; 0/1 off
    bool pixelCenter = false;      // alternatepixelcenter: Direct3D drawn half a pixel up and left
    int log = 0;                   // 0 off, 1 info, 2 trace (every blit, lock, flip and palette change)
    std::wstring logFile;          // empty: dd2d9.log next to the game's executable
};
extern Config g_config;
void LoadConfig(HMODULE self);
// The window manager, if it is i3 or sway, which don't minimize windows: its name (config.cpp).
extern const char* g_tilingWm;
// minimize= as it applies: auto minimizes, except under i3 or sway.
inline bool MinimizeOnFocusLoss() {
    return g_config.minimize == kMinimizeOn || (g_config.minimize == kMinimizeAuto && !g_tilingWm);
}
// dd2d9 was loaded under another name than ddraw.dll (behind a wrapper).
bool LoadedBehindWrapper();

// ---------------------------------------------------------------- logging
void LogInit();
extern int g_logLevel;  // (log.cpp) read at every log and trace site: inline
inline int LogLevel() { return g_logLevel; }
void Log(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void LogFlush(bool processExit);
void CrashLoggerRemove();  // before the DLL is unloaded (FreeLibrary)
// "module+0xoffset" for an address in a module (else the address), and the modules found on a
// stack from an address in it (log.cpp).
void DescribeAddress(const void* addr, char* out, size_t size);
void LogStack(const void* from);
const char* GuidName(REFIID riid);

#define DD2D9_LOG(...) do { if (::dd2d9::LogLevel() >= 1) ::dd2d9::Log(1, __VA_ARGS__); } while (0)
#define DD2D9_TRACE(...) do { if (::dd2d9::LogLevel() >= 2) ::dd2d9::Log(2, __VA_ARGS__); } while (0)
// ddraw.dll's interface tables (vtables) are writable, and programs patch them to hook
// a method (overlays, fan patches); ours sit in read-only memory. Made writable once per
// class, from its first object.
void MakeVtblWritable(const void* obj);
template <class T>
inline void WritableVtbl(const T* obj) {
    static bool done;
    if (!done) {
        done = true;
        MakeVtblWritable(obj);
    }
}

// Room for one more element in v, so that the push_back after it can't fail: false without
// the memory for it (an exception mustn't leave a COM method).
template <class T> bool Room(std::vector<T>& v) {
    try {
        if (v.size() == v.capacity()) v.reserve(v.empty() ? 8 : v.size() * 2);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

// A call's failure, traced (log=2) with what it was called on.
inline HRESULT TraceFail(const char* what, const void* obj, HRESULT hr) {
    if (FAILED(hr)) DD2D9_TRACE("%s %p failed: 0x%08lx", what, obj, static_cast<unsigned long>(hr));
    return hr;
}
// Logs the first call of an unimplemented or partially implemented path.
#define DD2D9_STUB(what) do { static bool once_; if (!once_) { once_ = true; DD2D9_LOG("stub: %s", what); } } while (0)

// ---------------------------------------------------------------- global lock
// Equivalent of the DirectDraw thread lock; taken by every API entry point and
// by the presenter thread while it reads the primary surface.
extern CRITICAL_SECTION g_lock;
// Work that must not run while g_lock is held (DirectDraw teardown waits for the
// presenter thread, which takes it): a thread holding it queues the work, which
// runs as soon as that thread has released g_lock completely.
inline bool LockHeldByMe() {
    return g_lock.RecursionCount > 0 && g_lock.OwningThread == reinterpret_cast<HANDLE>(ULONG_PTR(GetCurrentThreadId()));
}
extern volatile LONG g_deferredCount;  // queued work on any thread: the fast check
void RunWhenUnlocked(void (*fn)(void*), void* arg);
void RunDeferred();  // this thread's queued work, once it no longer holds g_lock

struct Guard {
    Guard() { EnterCriticalSection(&g_lock); }
    ~Guard() {
        LeaveCriticalSection(&g_lock);
        if (g_deferredCount && !LockHeldByMe()) RunDeferred();
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

// g_lock if it is free (owns()), for callers that must never wait for it.
struct TryGuard {
    TryGuard() : owns_(TryEnterCriticalSection(&g_lock) != FALSE) {}
    ~TryGuard() {
        if (!owns_) return;
        LeaveCriticalSection(&g_lock);
        if (g_deferredCount && !LockHeldByMe()) RunDeferred();
    }
    bool owns() const { return owns_; }
    TryGuard(const TryGuard&) = delete;
    TryGuard& operator=(const TryGuard&) = delete;

private:
    bool owns_;
};

// Serializes display-mode and window changes. Taken before g_lock, never while
// holding it: those changes send messages to the game window's thread, whose
// window procedure may call DirectDraw (and so take g_lock).
extern CRITICAL_SECTION g_displayLock;
// Waiting for it, messages sent to this thread's windows are handled meanwhile: the holder
// may be in SetWindowPos on a window of this thread, waiting for it (a game with a render
// thread changing the mode while its window thread calls DirectDraw).
void EnterDisplayLock();
// Lets go of it; once it is free, work the game's window procedure couldn't do meanwhile (it
// never waits for the lock) is posted to the window (ddhook.cpp).
void LeaveDisplayLock();
struct DisplayGuard {
    DisplayGuard() { EnterDisplayLock(); }
    ~DisplayGuard() { LeaveDisplayLock(); }
    DisplayGuard(const DisplayGuard&) = delete;
    DisplayGuard& operator=(const DisplayGuard&) = delete;
};

// Waits for `h` (or `other`, if given: whichever comes first) while still handling messages
// sent to this thread's windows (another thread may be waiting on one of them). Returns
// false on timeout.
bool WaitPumping(HANDLE h, DWORD timeoutMs, HANDLE other = nullptr);

// Every COM object we hand out starts with { vtable, kMagic, impl* } so an
// incoming interface pointer can be validated and mapped back to its object.
constexpr uint32_t kMagic = 0x39443244;  // "D2D9"
struct WrapHeader {
    void* vtbl;
    uint32_t magic;
    void* impl;
};
inline void* ImplFromIface(void* iface) {
    if (!iface) return nullptr;
    auto* h = static_cast<WrapHeader*>(iface);
    return h->magic == kMagic ? h->impl : nullptr;
}

// ---------------------------------------------------------------- pixel formats
struct Format {
    DWORD bpp = 0;
    DWORD r = 0, g = 0, b = 0, a = 0;
    bool pal8 = false;
    // Palettized at 1, 2 or 4 bits (bpp says which), the pixels packed from each byte's high
    // bits down. ddraw.dll makes such surfaces, locks them and gives a DC on them (not at 2
    // bits: GDI has no such bitmap); its emulation layer blits none of them, fills included
    // (re/spec/surface-blt.md, section 0), and Direct3D's texture loads copy their rows.
    bool palLow = false;
    // Luminance (DDPF_LUMINANCE): r = the luminance mask, a = the alpha mask (0 without): L8,
    // A8L8 and A4L4. Alpha alone (DDPF_ALPHA): 8 bits, a = 0xFF. Textures.
    bool lum = false, alphaOnly = false;
    bool zbuf = false;
    bool fourcc = false;
    DWORD fourCC = 0;
    // A bump map (DDPF_BUMPDUDV, environment-mapped bump mapping): r = the du mask, g = dv,
    // b = luminance (0 without). U8V8 and U5V5L6 (16 bits) and U8V8L8 (24).
    bool bump = false;

    // Bytes a pixel takes in a row (a planar format: its luma plane's one).
    DWORD Bytes() const { return bpp == 12 && fourcc ? 1 : (bpp + 7) / 8; }
    // The bytes w pixels take in a row, and the byte pixel x of a row is in (or starts at).
    DWORD RowBytes(DWORD w) const { return palLow ? (w * bpp + 7) / 8 : w * Bytes(); }
    DWORD Offset(DWORD x) const { return palLow ? x * bpp / 8 : x * Bytes(); }
    bool Palettized() const { return pal8 || palLow; }
    // Formats that aren't colours as the blitter converts them: copied as they are only.
    bool CopyOnly() const { return bump || lum || alphaOnly; }
    DWORD ValueMask() const { return bpp >= 32 ? 0xFFFFFFFFu : ((1u << bpp) - 1); }
    // Bits that take part in color-key comparisons (ignore X / alpha bits).
    DWORD KeyMask() const { return (Palettized() || zbuf || fourcc || bump || alphaOnly) ? ValueMask() : (r | g | b); }
    // DXT1-5: memory is described by a linear size instead of a pitch.
    bool BlockCompressed() const { return fourcc && bpp == 8; }
    // YUY2, UYVY: packed 4:2:2, two pixels in four bytes sharing their chroma.
    bool Yuv() const { return fourcc && bpp == 16; }
    // YV12, I420 (IYUV): planar 4:2:0, a byte of luma a pixel, then two chroma planes of half
    // the width and height (half the pitch), each a byte per 2x2 block.
    bool Planar() const { return fourcc && bpp == 12; }
    bool IsStandardX8R8G8B8() const { return bpp == 32 && r == 0xFF0000 && g == 0xFF00 && b == 0xFF; }
    bool operator==(const Format& o) const {
        return bpp == o.bpp && r == o.r && g == o.g && b == o.b && a == o.a && pal8 == o.pal8 &&
               palLow == o.palLow && lum == o.lum && alphaOnly == o.alphaOnly && zbuf == o.zbuf && fourcc == o.fourcc &&
               fourCC == o.fourCC && bump == o.bump;
    }
    bool operator!=(const Format& o) const { return !(*this == o); }

    static Format ForDisplayBpp(DWORD bpp);
    static bool FromDDPF(const DDPIXELFORMAT& pf, Format* out);
    void ToDDPF(DDPIXELFORMAT* pf) const;
};

// Converts between raw pixel values and 0xAARRGGBB.
uint32_t PixelToARGB(const Format& f, uint32_t v, const PALETTEENTRY* pal);
uint32_t ARGBToPixel(const Format& f, uint32_t argb);

// ---------------------------------------------------------------- surface descriptions
// Internally every description is a DDSURFACEDESC2; these convert from/to the
// caller's structure, chosen by its dwSize (DirectDraw accepts both sizes).
bool DescSizeOk(const void* desc);
bool DescIn(const void* in, DDSURFACEDESC2* out);
void DescOut(const DDSURFACEDESC2& in, void* out);
void DescOutEnum(const DDSURFACEDESC2& in, DDSURFACEDESC* out);  // old-interface enumerations

// ---------------------------------------------------------------- mouse (input.cpp)
// The game's user32 imports of the cursor functions point at dd2d9's, which translate
// between the real screen and the game's display mode while it is shown scaled or in a
// window, and pass everything on otherwise. Installed with the compatibility hooks (see
// InstallCompatHooks) when dd2d9 is loaded, not when the mode is set: a function that reads
// an import's address before the mode is set and calls through it afterwards (compilers
// keep it in a register over several calls) would go on calling Windows' function.
struct ImportHook;
ImportHook* MouseHooks(size_t* n);
// mousemerge= (dinputmouse.cpp): DirectInputCreate; set up before the hooks are installed,
// and the patched interface tables restored before dd2d9 is unloaded.
extern ImportHook kMouseMergeHooks[];
extern const size_t kMouseMergeHookCount;
void MouseMergeInit();
void MouseMergeRestore();
// dpiaware= (dpi.cpp): set from DllMain, before the game makes a window. A window's DPI (96
// when Windows scales the process itself), and its frame for a client area at a DPI.
void MakeDpiAware();
UINT WindowDpi(HWND h);
void AdjustWindowRectForDpi(RECT* r, DWORD style, bool menu, DWORD exStyle, UINT dpi);
// Videos that Windows' player (MCI) draws into the game's window, placed where the image is
// (video.cpp): installed with the mouse's.
ImportHook* VideoHooks(size_t* n);
// Whether such a video draws into `hwnd` now (an MCI device "window ... handle" of it).
bool VideoOnWindow(HWND hwnd);
// The game itself moving, sizing or showing a window right now (SetWindowPos, MoveWindow,
// ShowWindow, SetWindowPlacement, EndDeferWindowPos; video.cpp): a change to its window
// without it came from Windows or the window manager.
bool GameSizingWindow();
// A top-level window the game made in its mode's coordinates (a popup over its screen), shown
// scaled over the image (inputpaint.cpp): the scale from the game's coordinates to its real ones.
bool ScaledPopup(HWND h, double* sx, double* sy);

// ---------------------------------------------------------------- compatibility (compat.cpp)
// Every import hook of dd2d9's: ram=, cpu=, cpus= (what the game is told about the machine)
// and the other compatibility settings', the mouse's and the videos'. From DllMain
// (inLoaderLock: sets the affinity too) and again for each new DirectDraw object and display
// mode (DLLs loaded since, where the loader doesn't announce them: see PatchGameModules).
void InstallCompatHooks(bool inLoaderLock);
// cpus=: the game's threads are pinned one by one as they start (compatsys.cpp). PinNewThread
// from DllMain's DLL_THREAD_ATTACH, in the new thread; PinsThreads: whether it has anything to do.
void PinNewThread();
bool PinsThreads();
// Whether a DirectDraw object's primary is shown in `hwnd` (ddgdi.cpp). Any thread.
bool PresentsOnWindow(HWND hwnd);
// The game's own GDI drawing into a window a normal-mode object shows its primary in (see
// compatgdi.cpp's kWindowHooks): `draw` called with that primary's DC, set up as `dc` is
// (origin, clipping, the state blits use), which then goes out with the primary's frames.
// False, `draw` not called: `dc` isn't such a window's, or the primary gave no DC. Any thread.
bool DrawIntoShownPrimary(HDC dc, void (*draw)(HDC, void*), void* ctx);
// gdifullscreen= (gdiscreen.cpp). A display change of the game's (ChangeDisplaySettings on the
// primary display: `hasMode` with a DEVMODE, `sized` if it gives w x h; no DEVMODE: back to the
// desktop's), shown by dd2d9 instead: true with what to answer in *result; false, passed on.
// Any thread.
bool GdiFullscreenChange(bool hasMode, bool sized, DWORD w, DWORD h, DWORD flags, LONG* result);
// The game's blit to the window of that display mode drawn into its primary, as
// DrawIntoShownPrimary does for a windowed one. Any thread.
bool GdiFullscreenDraw(HDC dc, void (*draw)(HDC, void*), void* ctx);
// gdiscreen.cpp's lock, made in DllMain.
void GdiScreenInit();
class DDraw;
// A DirectDraw object of the game's taking exclusive mode (SetCooperativeLevel) or a primary
// (CreateSurface) while dd2d9's gdifullscreen mode is shown: that mode makes way for it (true:
// it did). Holding none of dd2d9's locks.
bool GdiFullscreenYield(const DDraw* taker);
// The game's GetProcAddress on module `m` (compat.cpp): one of OpenGL's, Glide's or Direct3D 8's
// tells gdifullscreen= the game doesn't draw with GDI.
void GdiFullscreenProcLookup(HMODULE m);
// fpslimit=: waits until the next frame is due (the game shows a frame: Flip, or a blit
// covering most of the primary). Called without g_lock.
void LimitFrame();
// Sleeps until QueryPerformanceCounter reaches `target` (ddtiming.cpp).
void SleepUntilQpc(int64_t target);
// The game's view of a point on the real screen and back; false when 1:1.
bool MouseToGame(POINT* p, bool wait = true);
bool MouseFromGame(POINT* p);
// Where the game's display mode is on the real screen, and the window showing it; false
// when the screen is the mode (a real mode switch), or mapping is off.
bool GameImage(RECT* image, SIZE* mode, HWND* window = nullptr, bool wait = true);
// Something GameImage's result depends on changed (display state, the game window's place).
void GameImageChanged();
// desktopbpp=: the colour depth the game is told the screen has (its mode's, or desktopbpp=).
int ShownDisplayBpp();
bool ShownDisplaySize(int* w, int* h);  // a display mode of dd2d9's own (ddscreen.cpp)

// The identity gamma ramp as XP has it: i << 8 (GetGammaRamp once the ramp was dropped).
inline void IdentityRamp(DDGAMMARAMP* r) {
    for (int i = 0; i < 256; i++) r->red[i] = r->green[i] = r->blue[i] = WORD(i << 8);
}
// The gamma calibrator (surface.cpp): read from the registry as the DLL loads; registered
// (and, once loaded, loadable).
void ReadGammaCalibrator();
bool GammaCalibratorRegistered();
// The system settings exclusive mode changed, put back as the process ends (ddwindow.cpp).
void ExclusiveSettingsAtExit();
// EnablePrintScreen under HKLM\Software\Microsoft\DirectDraw, read as each DirectDraw object is
// made, as ddraw.dll reads it (ddgdi.cpp).
void ReadPrintScreenSetting();
// GDI's palette of an 8-bit screen, as XP's GDI kept it beside DirectDraw (ddgdi.cpp): started
// again by a mode set; a palette put on the primary (Shown), its entries set (`entries`: the
// palette's own, from `start`), and taken off or gone with the primary (Gone). Get: all of
// it; true while a primary's palette is it (SYSPAL_NOSTATIC256).
void ScreenPaletteReset();
void ScreenPaletteShown(const PALETTEENTRY* entries);
void ScreenPaletteSet(const PALETTEENTRY* entries, DWORD start, DWORD count);
void ScreenPaletteGone();
bool ScreenPaletteGet(PALETTEENTRY out[256]);

}  // namespace dd2d9
