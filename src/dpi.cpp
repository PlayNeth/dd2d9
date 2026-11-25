// DPI awareness (dpiaware=on, the default). A program that doesn't say it handles the
// display's scaling (125%, 150%, ... in Windows' display settings) is shown by Windows in a
// pretend 96-DPI screen, scaled up: the monitor's size it is told is smaller than the real
// one (2560x1440 at 150%: 1707x960), and everything it shows, dd2d9's picture with it, is
// stretched again by Windows and blurred. Every game of the DirectDraw era is such a program.
// dd2d9 tells Windows the process handles the scaling itself, before the game makes its
// first window: the screen is then the real one, the picture shown 1:1, and the game's
// window, in window mode, is scaled by dd2d9 (ApplyWindow), its image with it.
#include "common.h"

namespace dd2d9 {

namespace {

using SetContextFn = BOOL(WINAPI*)(HANDLE);
using SetAwareFn = BOOL(WINAPI*)();
using DpiForWindowFn = UINT(WINAPI*)(HWND);
using AdjustForDpiFn = BOOL(WINAPI*)(RECT*, DWORD, BOOL, DWORD, UINT);

bool g_aware;
DpiForWindowFn g_dpiForWindow;
AdjustForDpiFn g_adjustForDpi;

BOOL CALLBACK FindOwnWindow(HWND h, LPARAM found) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    *reinterpret_cast<bool*>(found) = true;
    return FALSE;
}

}  // namespace

void MakeDpiAware() {
    const HMODULE user = GetModuleHandleW(L"user32.dll");
    if (!user) return;
    g_dpiForWindow = reinterpret_cast<DpiForWindowFn>(reinterpret_cast<void*>(GetProcAddress(user, "GetDpiForWindow")));
    g_adjustForDpi =
        reinterpret_cast<AdjustForDpiFn>(reinterpret_cast<void*>(GetProcAddress(user, "AdjustWindowRectExForDpi")));
    // Whether the process handles the scaling, however it came to (set here, or by the game's
    // manifest or compatibility settings): then window mode is scaled by dd2d9.
    const auto isAware = reinterpret_cast<SetAwareFn>(reinterpret_cast<void*>(GetProcAddress(user, "IsProcessDPIAware")));
    struct Done {
        SetAwareFn isAware;
        ~Done() { g_aware = isAware && isAware(); }
    } done{isAware};
    if (!g_config.dpiAware) return;
    // A window the game made before dd2d9 was loaded (a game that loads ddraw.dll itself,
    // late) keeps the awareness it was made with: the process is left to Windows' scaling,
    // all its windows alike.
    bool windows = false;
    EnumWindows(FindOwnWindow, reinterpret_cast<LPARAM>(&windows));
    if (windows) {
        DD2D9_LOG("dpiaware: the game has windows made before dd2d9 was loaded: left to Windows' scaling");
        return;
    }
    // Per monitor (Windows 10 1703 on): each monitor's own scaling; else the system's
    // (Vista on), the primary monitor's. The game's manifest or Windows' compatibility
    // settings ("Override high DPI scaling") may have set it already: then left as it is.
    const char* how = nullptr;
    DWORD error = 0;
    if (auto set = reinterpret_cast<SetContextFn>(
            reinterpret_cast<void*>(GetProcAddress(user, "SetProcessDpiAwarenessContext")))) {
        if (set(reinterpret_cast<HANDLE>(INT_PTR(-4)))) how = "per monitor";  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        else error = GetLastError();
    }
    if (!how && error != ERROR_ACCESS_DENIED) {
        if (auto set = reinterpret_cast<SetAwareFn>(reinterpret_cast<void*>(GetProcAddress(user, "SetProcessDPIAware"))))
            if (set()) how = "the system's";
    }
    const HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    if (how)
        DD2D9_LOG("dpiaware: the process handles the display's scaling (%s; the system's %d DPI, %d%%)", how, dpi,
                  dpi * 100 / 96);
    else if (error == ERROR_ACCESS_DENIED)
        DD2D9_LOG("dpiaware: the process's DPI awareness was set already (the game's manifest, or Windows' "
                  "compatibility settings): left as it is");
    else
        DD2D9_LOG("dpiaware: not available (error %lu)", error);
}

UINT WindowDpi(HWND h) {
    if (!g_aware) return 96;  // (then Windows scales the window, dd2d9 doesn't)
    if (g_dpiForWindow && h) {
        const UINT dpi = g_dpiForWindow(h);
        if (dpi) return dpi;
    }
    const HDC dc = GetDC(nullptr);
    const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return dpi > 0 ? UINT(dpi) : 96;
}

void AdjustWindowRectForDpi(RECT* r, DWORD style, bool menu, DWORD exStyle, UINT dpi) {
    if (g_adjustForDpi && g_adjustForDpi(r, style, menu, exStyle, dpi)) return;
    AdjustWindowRectEx(r, style, menu, exStyle);
}

}  // namespace dd2d9
