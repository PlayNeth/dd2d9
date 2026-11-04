// dd2d9 - DirectDraw (DX1-7) 2D implementation on top of Direct3D 9.
// Shared declarations: logging, configuration, global lock, pixel formats.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ddraw.h>
// d3d9.h is only included by gpu.cpp, gpures.cpp and presenter.cpp: it conflicts with d3d.h.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dd2d9 {

// ---------------------------------------------------------------- config
#define DD2D9_VERSION "1.3.1"

enum { kDisplaySwitch, kDisplayWindow, kDisplayBorderless };
enum { kVSyncAuto, kVSyncOn, kVSyncOff };
enum { kD3DAuto, kD3DOn, kD3DOff };

// Defaults behave like the original DirectDraw.
struct Config {
    int display = kDisplaySwitch;  // exclusive-mode games: real mode switch / centered window / borderless scaled
    int stretch = 0;               // window & borderless scaling: 0 aspect, 1 fill, 2 integer
    int filter = 0;                // 0 point, 1 linear, 2 sharp (integer point prescale, then linear)
    int vsync = kVSyncAuto;        // auto: Flip() follows the game's flags (DDFLIP_NOVSYNC); on / off: forced
    int d3d = kD3DAuto;            // dd2d9's Direct3D (3-7): auto / on = offered, off = none (2D only)
    bool systemD3D9 = true;        // present through Windows' own d3d9.dll, not one in the game folder (DXVK)
    // Compatibility (compat.cpp): what the game is told about the machine.
    int ramMB = 1024;              // GlobalMemoryStatus(Ex) physical memory; 0: the real values
    int vramMB = 512;              // DirectDraw's video memory (GetCaps, GetAvailableVidMem)
    int cpuMHz = 2000;             // the processor clock in the registry (~MHz); 0: the real one
    int cpus = 1;                  // processors the game runs on and is told about; 0: all
    int desktopBpp = 32;           // the desktop's colour depth DirectDraw reports (16, 24 or 32)
    bool lockWhole = false;        // a Lock of a rectangle counts as a write to the whole surface
    bool zAccess = true;           // zaccess=off: Z buffers on the GPU only, DirectDraw can't read or write them
    int fpsLimit = 0;              // frames per second at most (Flip, full blits to the primary); 0: off
    bool texLinear = false;        // texfilter=linear: point-filtered textures drawn linear
    int msaa = 0;                  // antialiasing: samples per pixel (2-16); 0/1 off
    bool aa2d = false;             // antialias2d=on: 2D draws (HUDs) antialiased too; off: drawn single-sampled
    bool render32 = false;         // 16-bit render targets drawn in 32 bits on the GPU
    bool depth24 = false;          // 16-bit Z buffers made 24-bit on the GPU
    bool overlays = true;          // videooverlay=on: overlay surfaces offered (emulated: laid over the primary when shown)
    bool drawTrace = false;        // log one frame's draws (render states, depths) every ~10 s
    int overlayBias = 1;           // passes laid on drawn surfaces pulled forward N 16-bit depth steps (gpures.cpp)
    int wbuffer = 1;               // W buffering: 0 off, 1 offered (emulated where the GPU lacks it), 2 forced on Z too
    bool mipmaps = false;          // the GPU makes smaller levels for textures the game gave none
    int anisotropy = 0;            // anisotropic filtering forced on smooth (linear) textures: 2-16; 0/1 off
    bool pixelCenter = false;      // alternatepixelcenter: Direct3D drawn half a pixel up and left
    int log = 0;                   // 0 off, 1 info, 2 trace (every blit, lock, flip and palette change)
    std::wstring logFile;          // empty: dd2d9.log next to the game's executable
};
extern Config g_config;
void LoadConfig(HMODULE self);
// dd2d9 was loaded under another name than ddraw.dll (behind a wrapper).
bool LoadedBehindWrapper();

// ---------------------------------------------------------------- logging
void LogInit();
extern int g_logLevel;  // (util.cpp) read at every log and trace site: inline
inline int LogLevel() { return g_logLevel; }
void Log(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void LogFlush(bool processExit);
void CrashLoggerRemove();  // before the DLL is unloaded (FreeLibrary)
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

// Serializes display-mode and window changes. Taken before g_lock, never while
// holding it: those changes send messages to the game window's thread, whose
// window procedure may call DirectDraw (and so take g_lock).
extern CRITICAL_SECTION g_displayLock;
// Waiting for it, messages sent to this thread's windows are handled meanwhile: the holder
// may be in SetWindowPos on a window of this thread, waiting for it (a game with a render
// thread changing the mode while its window thread calls DirectDraw).
void EnterDisplayLock();
struct DisplayGuard {
    DisplayGuard() { EnterDisplayLock(); }
    ~DisplayGuard() { LeaveCriticalSection(&g_displayLock); }
    DisplayGuard(const DisplayGuard&) = delete;
    DisplayGuard& operator=(const DisplayGuard&) = delete;
};

// Waits for `h` while still handling messages sent to this thread's windows
// (another thread may be waiting on one of them). Returns false on timeout.
bool WaitPumping(HANDLE h, DWORD timeoutMs);

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
    bool zbuf = false;
    bool fourcc = false;
    DWORD fourCC = 0;
    // A bump map (DDPF_BUMPDUDV, environment-mapped bump mapping): r = the du mask, g = dv,
    // b = luminance (0 without). U8V8 and U5V5L6 (16 bits) and U8V8L8 (24).
    bool bump = false;

    // Bytes a pixel takes in a row (a planar format: its luma plane's one).
    DWORD Bytes() const { return bpp == 12 && fourcc ? 1 : (bpp + 7) / 8; }
    DWORD ValueMask() const { return bpp >= 32 ? 0xFFFFFFFFu : ((1u << bpp) - 1); }
    // Bits that take part in color-key comparisons (ignore X / alpha bits).
    DWORD KeyMask() const { return (pal8 || zbuf || fourcc || bump) ? ValueMask() : (r | g | b); }
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
               zbuf == o.zbuf && fourcc == o.fourcc && fourCC == o.fourCC && bump == o.bump;
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
// window. Installed in every module loaded so far (each time exclusive mode starts).
void InstallMouseHooks();
// Videos that Windows' player (MCI) draws into the game's window, placed where the image is
// (video.cpp): installed with the mouse's, in a display mode of dd2d9's own.
void InstallVideoHooks();
// Whether such a video draws into `hwnd` now (an MCI device "window ... handle" of it).
bool VideoOnWindow(HWND hwnd);
// A top-level window the game made in its mode's coordinates (a popup over its screen), shown
// scaled over the image (input.cpp): the scale from the game's coordinates to its real ones.
bool ScaledPopup(HWND h, double* sx, double* sy);

// ---------------------------------------------------------------- compatibility (compat.cpp)
// ram=, cpu=, cpus=: what the game is told about the machine. From DllMain (inLoaderLock:
// sets the affinity too) and again for each new DirectDraw object (DLLs loaded since).
void InstallCompatHooks(bool inLoaderLock);
// Whether a DirectDraw object's primary is shown in `hwnd` (ddraw.cpp). Any thread.
bool PresentsOnWindow(HWND hwnd);
// Where the exclusive object's display mode is shown on the real screen (screen
// coordinates), its size and the window; false after a real mode switch.
bool ShownImage(RECT* image, SIZE* mode, HWND* window);
// fpslimit=: waits until the next frame is due (the game shows a frame: Flip, or a blit
// covering most of the primary). Called without g_lock.
void LimitFrame();
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
bool ShownDisplaySize(int* w, int* h);  // a display mode of dd2d9's own (ddraw.cpp)

}  // namespace dd2d9
