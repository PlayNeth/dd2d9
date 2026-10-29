// dd2d9 - DirectDraw (DX1-7) 2D implementation on top of Direct3D 9.
// Shared declarations: logging, configuration, global lock, pixel formats.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ddraw.h>
#include <d3d9.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dd2d9 {

// ---------------------------------------------------------------- config
#define DD2D9_VERSION "0.4.1"

enum { kDisplaySwitch, kDisplayWindow, kDisplayBorderless };
enum { kVSyncAuto, kVSyncOn, kVSyncOff };
enum { kPresentWindow, kPresentExclusive };

// Defaults behave like the original DirectDraw.
struct Config {
    int display = kDisplaySwitch;  // exclusive-mode games: real mode switch / centered window / borderless scaled
    int stretch = 0;               // window & borderless scaling: 0 aspect, 1 fill, 2 integer
    int filter = 0;                // 0 point, 1 linear
    int vsync = kVSyncAuto;        // auto: Flip() follows the game's flags (DDFLIP_NOVSYNC); on / off: forced
    int present = kPresentWindow;  // after a real mode switch: borderless window / D3D9 exclusive fullscreen
    int log = 0;                   // 0 off, 1 info, 2 trace (every blit, lock, flip and palette change)
    std::wstring logFile;          // empty: dd2d9.log next to the game's executable
};
extern Config g_config;
void LoadConfig(HMODULE self);

// ---------------------------------------------------------------- logging
void LogInit();
int LogLevel();
void Log(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void LogFlush(bool processExit);
const char* GuidName(REFIID riid);

#define DD2D9_LOG(...) do { if (::dd2d9::LogLevel() >= 1) ::dd2d9::Log(1, __VA_ARGS__); } while (0)
#define DD2D9_TRACE(...) do { if (::dd2d9::LogLevel() >= 2) ::dd2d9::Log(2, __VA_ARGS__); } while (0)
// Logs the first call of an unimplemented or partially implemented path.
#define DD2D9_STUB(what) do { static bool once_; if (!once_) { once_ = true; DD2D9_LOG("stub: %s", what); } } while (0)

// ---------------------------------------------------------------- global lock
// Equivalent of the DirectDraw thread lock; taken by every API entry point and
// by the presenter thread while it reads the primary surface.
extern CRITICAL_SECTION g_lock;
struct Guard {
    Guard() { EnterCriticalSection(&g_lock); }
    ~Guard() { LeaveCriticalSection(&g_lock); }
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

}  // namespace dd2d9
