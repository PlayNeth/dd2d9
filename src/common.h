// dd2d9 - DirectDraw (DX1-7) 2D implementation on top of Direct3D 9.
// Shared declarations: logging, configuration, global lock, pixel formats.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ddraw.h>
// d3d9.h is only included by presenter.cpp: it conflicts with d3d.h (d3dproxy.cpp).

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dd2d9 {

// ---------------------------------------------------------------- config
#define DD2D9_VERSION "0.7.17"

enum { kDisplaySwitch, kDisplayWindow, kDisplayBorderless };
enum { kVSyncAuto, kVSyncOn, kVSyncOff };
enum { kD3DAuto, kD3DOn, kD3DOff };

// Defaults behave like the original DirectDraw.
struct Config {
    int display = kDisplaySwitch;  // exclusive-mode games: real mode switch / centered window / borderless scaled
    int stretch = 0;               // window & borderless scaling: 0 aspect, 1 fill, 2 integer
    int filter = 0;                // 0 point, 1 linear, 2 sharp (integer point prescale, then linear)
    int vsync = kVSyncAuto;        // auto: Flip() follows the game's flags (DDFLIP_NOVSYNC); on / off: forced
    int d3d = kD3DAuto;            // Direct3D objects for a wrapper (D7VK): auto = when loaded behind one
    bool mouseScale = true;        // map the mouse to the game's display mode when it is scaled or windowed
    bool systemD3D9 = true;        // present through Windows' own d3d9.dll, not one in the game folder (DXVK)
    int log = 0;                   // 0 off, 1 info, 2 trace (every blit, lock, flip and palette change)
    std::wstring logFile;          // empty: dd2d9.log next to the game's executable
};
extern Config g_config;
void LoadConfig(HMODULE self);
// dd2d9 was loaded under another name than ddraw.dll (as ddraw_.dll behind D7VK).
bool LoadedBehindWrapper();

// ---------------------------------------------------------------- logging
void LogInit();
int LogLevel();
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
struct DisplayGuard {
    DisplayGuard() { EnterCriticalSection(&g_displayLock); }
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

    DWORD Bytes() const { return (bpp + 7) / 8; }
    DWORD ValueMask() const { return bpp >= 32 ? 0xFFFFFFFFu : ((1u << bpp) - 1); }
    // Bits that take part in color-key comparisons (ignore X / alpha bits).
    DWORD KeyMask() const { return (pal8 || zbuf || fourcc) ? ValueMask() : (r | g | b); }
    // DXT1-5: memory is described by a linear size instead of a pitch.
    bool BlockCompressed() const { return fourcc && bpp == 8; }
    bool IsStandardX8R8G8B8() const { return bpp == 32 && r == 0xFF0000 && g == 0xFF00 && b == 0xFF; }
    bool operator==(const Format& o) const {
        return bpp == o.bpp && r == o.r && g == o.g && b == o.b && a == o.a && pal8 == o.pal8 &&
               zbuf == o.zbuf && fourcc == o.fourcc && fourCC == o.fourCC;
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
void RemoveMouseHooks();  // the DLL is unloaded while the process goes on
// The game's view of a point on the real screen and back; false when 1:1.
bool MouseToGame(POINT* p);
bool MouseFromGame(POINT* p);
// Where the game's display mode is on the real screen, and the window showing it; false
// when 1:1 (or mapping is off).
bool GameImage(RECT* image, SIZE* mode, HWND* window = nullptr);

}  // namespace dd2d9
