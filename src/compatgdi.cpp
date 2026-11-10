// Compatibility (see compat.cpp): GDI in a display mode of the game's own (its depth and size,
// 16-bit bitmaps), and the game's GDI drawing on the window a windowed primary is shown in.
#include "compatint.h"

#include <cstring>

namespace dd2d9 {

namespace {

using GetDeviceCapsFn = int(WINAPI*)(HDC, int);
GetDeviceCapsFn g_getDeviceCaps;

// Always on (not only with desktopbpp=): in a display mode of the game's own, that mode's
// depth and size, as XP's GDI answered after ddraw.dll switched the display. An 8-bit one makes the
// display a palette device (RC_PALETTE, 256 entries, the 20 static colours): FlyingSB (and
// other games) set up their DirectDraw palette only when the screen is one, and showed
// dd2d9's grey ramp instead of their colours.
int WINAPI HookGetDeviceCaps(HDC dc, int index) {
    const int real = DD2D9_NEXT(g_getDeviceCaps)(dc, index);
    const bool size = index == HORZRES || index == VERTRES || index == DESKTOPHORZRES || index == DESKTOPVERTRES;
    if (!size && index != BITSPIXEL && index != NUMCOLORS && index != RASTERCAPS && index != SIZEPALETTE &&
        index != NUMRESERVED && index != COLORRES)
        return real;
    const DWORD type = GetObjectType(dc);
    if ((type != OBJ_DC && type != OBJ_MEMDC) || DD2D9_NEXT(g_getDeviceCaps)(dc, TECHNOLOGY) != DT_RASDISPLAY) return real;
    if (size) {
        // The screen's size: the mode's, as GetSystemMetrics answers it (input.cpp).
        int w, h;
        if (!ShownDisplaySize(&w, &h)) return real;
        return index == HORZRES || index == DESKTOPHORZRES ? w : h;
    }
    const int bpp = ShownDisplayBpp();
    if (bpp == 8) {
        switch (index) {
        case BITSPIXEL: return 8;
        case NUMCOLORS: return 20;
        case RASTERCAPS: return real | RC_PALETTE;
        case SIZEPALETTE: return 256;
        case NUMRESERVED: return 20;
        default: return 18;  // COLORRES: the DAC's 6 bits a channel
        }
    }
    if (bpp < 16) return real;
    if (index == BITSPIXEL) return bpp;
    if (index == NUMCOLORS) return -1;  // above 8 bits
    return real;
}

// LockWindowUpdate on the window dd2d9 shows the primary in: not done. ddraw.dll drew a
// windowed primary straight on the screen, which a window locked from painting didn't stop;
// here the picture goes through that window, and a lock would hide all of it. 3DMark 2000's
// splash locks its full-screen window (so that Windows doesn't paint over the desktop
// behind) and blits its logo to the primary: nothing showed. Unlocking (NULL) after a lock
// not done isn't passed on either. One window is locked at a time, as by Windows: a lock while
// another (done or not) holds fails, and unlocking ends whichever holds. (This table is always
// on: not a compatibility option.)
using LockWindowUpdateFn = BOOL(WINAPI*)(HWND);
LockWindowUpdateFn g_lockWindowUpdate;
SRWLOCK g_lockWindowLock = SRWLOCK_INIT;
bool g_lockSkipped;   // a lock not done holds (g_lockWindowLock)
HWND g_lockReal;      // the window a lock done by the game holds (g_lockWindowLock)

BOOL WINAPI HookLockWindowUpdate(HWND hwnd) {
    if (hwnd && PresentsOnWindow(hwnd)) {
        AcquireSRWLockExclusive(&g_lockWindowLock);
        // (Windows ends a real lock when its window is destroyed, as without an unlock here.)
        const bool held = g_lockSkipped || (g_lockReal && IsWindow(g_lockReal));
        if (!held) g_lockSkipped = true;
        ReleaseSRWLockExclusive(&g_lockWindowLock);
        if (held) return FALSE;
        DD2D9_LOG("LockWindowUpdate(%p): not done (the primary is shown in that window)", static_cast<void*>(hwnd));
        return TRUE;
    }
    AcquireSRWLockExclusive(&g_lockWindowLock);
    const bool skipped = g_lockSkipped;
    if (!hwnd) g_lockSkipped = false;
    ReleaseSRWLockExclusive(&g_lockWindowLock);
    if (skipped) return hwnd ? FALSE : TRUE;
    const BOOL ok = DD2D9_NEXT(g_lockWindowUpdate)(hwnd);
    if (ok) {
        AcquireSRWLockExclusive(&g_lockWindowLock);
        g_lockReal = hwnd;
        ReleaseSRWLockExclusive(&g_lockWindowLock);
    }
    return ok;
}

// A 16-bit bitmap made with CreateBitmap or CreateBitmapIndirect while the game is in a 16-bit
// mode of its own: made as a DIB section in the mode's format (5:6:5, as dd2d9's 16-bit
// surfaces) with the game's pixels. On XP the screen really was 16-bit, so such a bitmap was
// the screen's own format and went into any memory DC; here the screen stays 32-bit, and
// Windows refuses to select a 16-bit device-dependent bitmap into a DC made for it
// (SelectObject fails), so what the game then blits from that DC never arrives. Sakura Taisen
// builds each letter of its text that way (CreateBitmapIndirect, CreateCompatibleDC(NULL),
// BitBlt into the surface's DC) and its text came out empty. A DIB section goes into any
// memory DC and copies to a 16-bit surface pixel for pixel. (A bitmap compatible with the
// screen, CreateCompatibleBitmap's or CreateDIBitmap's, stays the real screen's 32 bits: no game
// has been seen to depend on its depth.)
using CreateBitmapFn = HBITMAP(WINAPI*)(int, int, UINT, UINT, const void*);
using CreateBitmapIndirectFn = HBITMAP(WINAPI*)(const BITMAP*);
CreateBitmapFn g_createBitmap;
CreateBitmapIndirectFn g_createBitmapIndirect;

// The DIB section for a w x h 16-bit bitmap whose rows are `stride` bytes apart (null bits: left
// zeroed); nullptr if this isn't such a bitmap (the real call then).
HBITMAP ModeBitmap16(int w, int h, UINT planes, UINT bpp, const void* bits, LONG stride) {
    if (bpp != 16 || planes != 1 || w <= 0 || h <= 0 || w > 32767 || h > 32767 || ShownDisplayBpp() != 16) return nullptr;
    struct {
        BITMAPINFOHEADER h;
        DWORD masks[3];
    } bi = {};
    bi.h.biSize = sizeof(bi.h);
    bi.h.biWidth = w;
    bi.h.biHeight = -h;  // top-down, as a bitmap's bits are
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 16;
    bi.h.biCompression = BI_BITFIELDS;
    bi.masks[0] = 0xF800;
    bi.masks[1] = 0x07E0;
    bi.masks[2] = 0x001F;
    void* dst = nullptr;
    HBITMAP b = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &dst, nullptr, 0);
    if (!b || !dst) return nullptr;
    if (bits) {
        const size_t row = size_t(w) * 2, dstStride = (row + 3) & ~size_t(3);
        for (int y = 0; y < h; y++)
            memcpy(static_cast<BYTE*>(dst) + size_t(y) * dstStride, static_cast<const BYTE*>(bits) + size_t(y) * size_t(stride), row);
    }
    return b;
}

HBITMAP WINAPI HookCreateBitmap(int w, int h, UINT planes, UINT bpp, const void* bits) {
    // (CreateBitmap's rows are a whole number of 16-bit words: 16-bit pixels, packed.)
    if (HBITMAP b = ModeBitmap16(w, h, planes, bpp, bits, LONG(w) * 2)) return b;
    return DD2D9_NEXT(g_createBitmap)(w, h, planes, bpp, bits);
}

HBITMAP WINAPI HookCreateBitmapIndirect(const BITMAP* bm) {
    if (bm && int64_t(bm->bmWidthBytes) >= int64_t(bm->bmWidth) * 2)  // (rows that hold the pixels)
        if (HBITMAP b = ModeBitmap16(bm->bmWidth, bm->bmHeight, bm->bmPlanes, bm->bmBitsPixel, bm->bmBits, bm->bmWidthBytes))
            return b;
    return DD2D9_NEXT(g_createBitmapIndirect)(bm);
}

// The game's own blits into the window dd2d9 shows a windowed primary in: drawn into that
// primary (DrawIntoShownPrimary). ddraw.dll's windowed primary was the screen, where GDI
// drew too; here the window shows dd2d9's frames, and what GDI drew there was covered by
// the next one (the refresh four times a second included), or with the desktop compositor
// shown in turn with them. Swordman (神魔至尊傳)'s windowed versions draw the game with
// StretchDIBits and play their movies through Bink's DirectDraw primary: after the first
// movie its last frame flashed over the game. Calls from Windows' modules, a DIB with
// palette indices (the window DC's palette) and a source on the screen are passed on.
using StretchDIBitsFn = int(WINAPI*)(HDC, int, int, int, int, int, int, int, int, const void*, const BITMAPINFO*, UINT,
                                     DWORD);
using SetDIBitsToDeviceFn = int(WINAPI*)(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT, const void*,
                                         const BITMAPINFO*, UINT);
using BitBltFn = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, DWORD);
using StretchBltFn = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
using PatBltFn = BOOL(WINAPI*)(HDC, int, int, int, int, DWORD);
StretchDIBitsFn g_stretchDIBits;
SetDIBitsToDeviceFn g_setDIBitsToDevice;
BitBltFn g_bitBlt;
StretchBltFn g_stretchBlt;
PatBltFn g_patBlt;

template <class F>
bool IntoShownPrimary(HDC dc, F& f) {
    return DrawIntoShownPrimary(dc, [](HDC p, void* c) { (*static_cast<F*>(c))(p); }, &f);
}

// A blit's source on the screen (a window DC) isn't in the primary: such a blit is passed on.
bool ScreenDC(HDC dc) {
    return dc && GetObjectType(dc) == OBJ_DC;
}

int WINAPI HookStretchDIBits(HDC dc, int x, int y, int w, int h, int sx, int sy, int sw, int sh, const void* bits,
                             const BITMAPINFO* bi, UINT usage, DWORD rop) {
    DD2D9_PASS_WINDOWS(g_stretchDIBits, dc, x, y, w, h, sx, sy, sw, sh, bits, bi, usage, rop);
    const StretchDIBitsFn next = DD2D9_NEXT(g_stretchDIBits);
    int r = 0;
    auto f = [&](HDC p) { r = next(p, x, y, w, h, sx, sy, sw, sh, bits, bi, usage, rop); };
    if (usage == DIB_RGB_COLORS && IntoShownPrimary(dc, f)) return r;
    return next(dc, x, y, w, h, sx, sy, sw, sh, bits, bi, usage, rop);
}

int WINAPI HookSetDIBitsToDevice(HDC dc, int x, int y, DWORD w, DWORD h, int sx, int sy, UINT start, UINT lines,
                                 const void* bits, const BITMAPINFO* bi, UINT usage) {
    DD2D9_PASS_WINDOWS(g_setDIBitsToDevice, dc, x, y, w, h, sx, sy, start, lines, bits, bi, usage);
    const SetDIBitsToDeviceFn next = DD2D9_NEXT(g_setDIBitsToDevice);
    int r = 0;
    auto f = [&](HDC p) { r = next(p, x, y, w, h, sx, sy, start, lines, bits, bi, usage); };
    if (usage == DIB_RGB_COLORS && IntoShownPrimary(dc, f)) return r;
    return next(dc, x, y, w, h, sx, sy, start, lines, bits, bi, usage);
}

BOOL WINAPI HookBitBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop) {
    DD2D9_PASS_WINDOWS(g_bitBlt, dc, x, y, w, h, src, sx, sy, rop);
    const BitBltFn next = DD2D9_NEXT(g_bitBlt);
    BOOL r = FALSE;
    auto f = [&](HDC p) { r = next(p, x, y, w, h, src, sx, sy, rop); };
    if (!ScreenDC(src) && IntoShownPrimary(dc, f)) return r;
    return next(dc, x, y, w, h, src, sx, sy, rop);
}

BOOL WINAPI HookStretchBlt(HDC dc, int x, int y, int w, int h, HDC src, int sx, int sy, int sw, int sh, DWORD rop) {
    DD2D9_PASS_WINDOWS(g_stretchBlt, dc, x, y, w, h, src, sx, sy, sw, sh, rop);
    const StretchBltFn next = DD2D9_NEXT(g_stretchBlt);
    BOOL r = FALSE;
    auto f = [&](HDC p) { r = next(p, x, y, w, h, src, sx, sy, sw, sh, rop); };
    if (!ScreenDC(src) && IntoShownPrimary(dc, f)) return r;
    return next(dc, x, y, w, h, src, sx, sy, sw, sh, rop);
}

BOOL WINAPI HookPatBlt(HDC dc, int x, int y, int w, int h, DWORD rop) {
    DD2D9_PASS_WINDOWS(g_patBlt, dc, x, y, w, h, rop);
    const PatBltFn next = DD2D9_NEXT(g_patBlt);
    BOOL r = FALSE;
    auto f = [&](HDC p) { r = next(p, x, y, w, h, rop); };
    if (IntoShownPrimary(dc, f)) return r;
    return next(dc, x, y, w, h, rop);
}

}  // namespace

ImportHook kWindowHooks[] = {
    {"user32", "LockWindowUpdate", reinterpret_cast<void*>(HookLockWindowUpdate), reinterpret_cast<void**>(&g_lockWindowUpdate)},
    {"gdi32", "StretchDIBits", reinterpret_cast<void*>(HookStretchDIBits), reinterpret_cast<void**>(&g_stretchDIBits)},
    {"gdi32", "SetDIBitsToDevice", reinterpret_cast<void*>(HookSetDIBitsToDevice),
     reinterpret_cast<void**>(&g_setDIBitsToDevice)},
    {"gdi32", "BitBlt", reinterpret_cast<void*>(HookBitBlt), reinterpret_cast<void**>(&g_bitBlt)},
    {"gdi32", "StretchBlt", reinterpret_cast<void*>(HookStretchBlt), reinterpret_cast<void**>(&g_stretchBlt)},
    {"gdi32", "PatBlt", reinterpret_cast<void*>(HookPatBlt), reinterpret_cast<void**>(&g_patBlt)},
    {"gdi32", "GetDeviceCaps", reinterpret_cast<void*>(HookGetDeviceCaps), reinterpret_cast<void**>(&g_getDeviceCaps)},
    {"gdi32", "CreateBitmap", reinterpret_cast<void*>(HookCreateBitmap), reinterpret_cast<void**>(&g_createBitmap)},
    {"gdi32", "CreateBitmapIndirect", reinterpret_cast<void*>(HookCreateBitmapIndirect),
     reinterpret_cast<void**>(&g_createBitmapIndirect)},
};
const size_t kWindowHookCount = sizeof(kWindowHooks) / sizeof(kWindowHooks[0]);

}  // namespace dd2d9
