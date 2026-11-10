// Compatibility (see compat.cpp): the display settings' depth (desktopbpp=), and the game's
// own mode as the current one.
#include "compatint.h"

#include <algorithm>
#include <cstring>
#include <cwctype>

namespace dd2d9 {

namespace {

// ---------------------------------------------------------------- desktopbpp=

// What a 16-bit (or 24-bit) desktop answered through GDI and EnumDisplaySettings: the colour
// depth, for display device contexts and the current (or registry) display settings. Games
// check it there too, as Moto Racer checks it through DirectDraw (GetDisplayMode, answered in
// ddcaps.cpp). While the game has a display mode of its own, that mode's depth, as on XP. A
// palettized depth (8 bits) isn't faked there (GetDeviceCaps does answer one for an 8-bit mode
// of the game's own, below).
using EnumDisplaySettingsAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DEVMODEA*);
using EnumDisplaySettingsWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);
using EnumDisplaySettingsExAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DEVMODEA*, DWORD);
using EnumDisplaySettingsExWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*, DWORD);
EnumDisplaySettingsAFn g_enumDisplaySettingsA;
EnumDisplaySettingsWFn g_enumDisplaySettingsW;
EnumDisplaySettingsExAFn g_enumDisplaySettingsExA;
EnumDisplaySettingsExWFn g_enumDisplaySettingsExW;

int ShownBppOrZero() {
    const int b = ShownDisplayBpp();
    return b >= 16 ? b : 0;
}

// The current or registry settings of the primary display (null or its name): the depth.
template <typename Dev, typename Ch>
void FixDepth(const Ch* device, DWORD mode, Dev* dm, BOOL ok) {
    if (!ok || !dm || (mode != ENUM_CURRENT_SETTINGS && mode != ENUM_REGISTRY_SETTINGS)) return;
    if (device && device[0]) {  // named: only if that is the primary display
        bool primary = false;
        DISPLAY_DEVICEW dd = {};
        dd.cb = sizeof(dd);
        for (DWORD n = 0; !primary && EnumDisplayDevicesW(nullptr, n, &dd, 0); n++, dd.cb = sizeof(dd)) {
            if (!(dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) continue;
            bool same = true;  // compared as text, case-insensitively (ASCII names)
            for (int i = 0; i < 32 && same; i++) {
                const wchar_t a = dd.DeviceName[i];
                const wchar_t b = wchar_t(static_cast<unsigned>(device[i]) & (sizeof(Ch) == 1 ? 0xFFu : 0xFFFFu));
                if (towupper(a) != towupper(b)) same = false;
                else if (!a) break;
            }
            primary = same;
        }
        if (!primary) return;
    }
    if (const int bpp = ShownBppOrZero()) dm->dmBitsPerPel = DWORD(bpp);
    // The current mode the game's own while it is shown scaled or in a window (borderless,
    // window), as after a real switch: a game checking that its mode took saw the desktop's.
    RECT r;
    SIZE m;
    if (mode == ENUM_CURRENT_SETTINGS && GameImage(&r, &m)) {
        dm->dmPelsWidth = DWORD(m.cx);
        dm->dmPelsHeight = DWORD(m.cy);
    }
}

BOOL WINAPI HookEnumDisplaySettingsA(LPCSTR device, DWORD mode, DEVMODEA* dm) {
    const BOOL ok = DD2D9_NEXT(g_enumDisplaySettingsA)(device, mode, dm);
    FixDepth(device, mode, dm, ok);
    return ok;
}
BOOL WINAPI HookEnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW* dm) {
    const BOOL ok = DD2D9_NEXT(g_enumDisplaySettingsW)(device, mode, dm);
    FixDepth(device, mode, dm, ok);
    return ok;
}
BOOL WINAPI HookEnumDisplaySettingsExA(LPCSTR device, DWORD mode, DEVMODEA* dm, DWORD flags) {
    const BOOL ok = DD2D9_NEXT(g_enumDisplaySettingsExA)(device, mode, dm, flags);
    FixDepth(device, mode, dm, ok);
    return ok;
}
BOOL WINAPI HookEnumDisplaySettingsExW(LPCWSTR device, DWORD mode, DEVMODEW* dm, DWORD flags) {
    const BOOL ok = DD2D9_NEXT(g_enumDisplaySettingsExW)(device, mode, dm, flags);
    FixDepth(device, mode, dm, ok);
    return ok;
}

// A mode change asking for the depth EnumDisplaySettings made up (a game putting back the
// "current" mode it read): the depth left as it is, not switched to that one for real.
template <typename Dev> const Dev* RealDepth(const Dev* dm, Dev* copy) {
    const int bpp = ShownBppOrZero();
    if (!dm || !bpp || !(dm->dmFields & DM_BITSPERPEL) || dm->dmBitsPerPel != DWORD(bpp)) return dm;
    // The public part as the game sized it (an older, shorter DEVMODE: not read past), its
    // dmDriverExtra bytes after it not passed on.
    *copy = Dev{};
    const size_t size = std::min<size_t>(dm->dmSize ? dm->dmSize : sizeof(Dev), sizeof(Dev));
    memcpy(copy, dm, size);
    copy->dmSize = WORD(size);  // (a longer one's size would have the call read past the copy)
    copy->dmDriverExtra = 0;
    copy->dmFields &= ~DWORD(DM_BITSPERPEL);
    return copy;
}

using ChangeDisplaySettingsAFn = LONG(WINAPI*)(DEVMODEA*, DWORD);
using ChangeDisplaySettingsWFn = LONG(WINAPI*)(DEVMODEW*, DWORD);
using ChangeDisplaySettingsExAFn = LONG(WINAPI*)(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID);
using ChangeDisplaySettingsExWFn = LONG(WINAPI*)(LPCWSTR, DEVMODEW*, HWND, DWORD, LPVOID);
ChangeDisplaySettingsAFn g_changeDisplaySettingsA;
ChangeDisplaySettingsWFn g_changeDisplaySettingsW;
ChangeDisplaySettingsExAFn g_changeDisplaySettingsExA;
ChangeDisplaySettingsExWFn g_changeDisplaySettingsExW;

// The mode the game is shown in already (borderless, window: EnumDisplaySettings reports it
// as current): a game putting it back, or asking for it again, would really switch the
// display under the scaled picture. Nothing to do, as on a display in that mode.
template <typename Dev> bool AlreadyShown(const Dev* dm) {
    RECT r;
    SIZE m;
    return dm && (dm->dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT)) == (DM_PELSWIDTH | DM_PELSHEIGHT) &&
           GameImage(&r, &m) && dm->dmPelsWidth == DWORD(m.cx) && dm->dmPelsHeight == DWORD(m.cy);
}

LONG WINAPI HookChangeDisplaySettingsA(DEVMODEA* dm, DWORD flags) {
    if (AlreadyShown(dm)) return DISP_CHANGE_SUCCESSFUL;
    DEVMODEA c;
    return DD2D9_NEXT(g_changeDisplaySettingsA)(const_cast<DEVMODEA*>(RealDepth(dm, &c)), flags);
}
LONG WINAPI HookChangeDisplaySettingsW(DEVMODEW* dm, DWORD flags) {
    if (AlreadyShown(dm)) return DISP_CHANGE_SUCCESSFUL;
    DEVMODEW c;
    return DD2D9_NEXT(g_changeDisplaySettingsW)(const_cast<DEVMODEW*>(RealDepth(dm, &c)), flags);
}
// (A display named: only the primary's depth is made up; the name left to the system.)
LONG WINAPI HookChangeDisplaySettingsExA(LPCSTR device, DEVMODEA* dm, HWND h, DWORD flags, LPVOID p) {
    if (!(device && device[0]) && AlreadyShown(dm)) return DISP_CHANGE_SUCCESSFUL;
    DEVMODEA c;
    DEVMODEA* const sent = device && device[0] ? dm : const_cast<DEVMODEA*>(RealDepth(dm, &c));
    return DD2D9_NEXT(g_changeDisplaySettingsExA)(device, sent, h, flags, p);
}
LONG WINAPI HookChangeDisplaySettingsExW(LPCWSTR device, DEVMODEW* dm, HWND h, DWORD flags, LPVOID p) {
    if (!(device && device[0]) && AlreadyShown(dm)) return DISP_CHANGE_SUCCESSFUL;
    DEVMODEW c;
    DEVMODEW* const sent = device && device[0] ? dm : const_cast<DEVMODEW*>(RealDepth(dm, &c));
    return DD2D9_NEXT(g_changeDisplaySettingsExW)(device, sent, h, flags, p);
}

}  // namespace

ImportHook kDepthHooks[] = {
    {"user32", "ChangeDisplaySettingsA", reinterpret_cast<void*>(HookChangeDisplaySettingsA),
     reinterpret_cast<void**>(&g_changeDisplaySettingsA)},
    {"user32", "ChangeDisplaySettingsW", reinterpret_cast<void*>(HookChangeDisplaySettingsW),
     reinterpret_cast<void**>(&g_changeDisplaySettingsW)},
    {"user32", "ChangeDisplaySettingsExA", reinterpret_cast<void*>(HookChangeDisplaySettingsExA),
     reinterpret_cast<void**>(&g_changeDisplaySettingsExA)},
    {"user32", "ChangeDisplaySettingsExW", reinterpret_cast<void*>(HookChangeDisplaySettingsExW),
     reinterpret_cast<void**>(&g_changeDisplaySettingsExW)},
    {"user32", "EnumDisplaySettingsA", reinterpret_cast<void*>(HookEnumDisplaySettingsA),
     reinterpret_cast<void**>(&g_enumDisplaySettingsA)},
    {"user32", "EnumDisplaySettingsW", reinterpret_cast<void*>(HookEnumDisplaySettingsW),
     reinterpret_cast<void**>(&g_enumDisplaySettingsW)},
    {"user32", "EnumDisplaySettingsExA", reinterpret_cast<void*>(HookEnumDisplaySettingsExA),
     reinterpret_cast<void**>(&g_enumDisplaySettingsExA)},
    {"user32", "EnumDisplaySettingsExW", reinterpret_cast<void*>(HookEnumDisplaySettingsExW),
     reinterpret_cast<void**>(&g_enumDisplaySettingsExW)},
};
const size_t kDepthHookCount = sizeof(kDepthHooks) / sizeof(kDepthHooks[0]);

}  // namespace dd2d9
