// Mouse coordinates for games shown scaled or in a window (display=borderless or window,
// or a mode the monitor lacks). The game asks for and sets the cursor in its display
// mode's coordinates, as if that mode filled the screen; the real cursor is on a screen
// of another size, the image somewhere on it. dd2d9 points the game's imports of the
// cursor functions at its own, which translate (see MouseToGame / MouseFromGame in
// ddraw.cpp); the mouse messages to the game's window are translated in its window
// procedure (ddraw.cpp, HookProc). DirectInput's relative mouse needs nothing.
#include "objects.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace dd2d9 {

namespace {

using GetCursorPosFn = BOOL(WINAPI*)(POINT*);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using GetMessagePosFn = DWORD(WINAPI*)();
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using GetClipCursorFn = BOOL(WINAPI*)(RECT*);
using GetSystemMetricsFn = int(WINAPI*)(int);
using PointFn = BOOL(WINAPI*)(HWND, POINT*);
using RectFn = BOOL(WINAPI*)(HWND, RECT*);
using MapWindowPointsFn = int(WINAPI*)(HWND, HWND, POINT*, UINT);

GetCursorPosFn g_getCursorPos;
SetCursorPosFn g_setCursorPos;
GetMessagePosFn g_getMessagePos;
ClipCursorFn g_clipCursor;
GetClipCursorFn g_getClipCursor;
GetSystemMetricsFn g_getSystemMetrics;
PointFn g_screenToClient, g_clientToScreen;
RectFn g_getClientRect, g_getWindowRect;
MapWindowPointsFn g_mapWindowPoints;

// The game's window, while its display mode is translated: as with a real mode switch, it
// covers the game's screen at 0,0, so its client and screen coordinates are the same.
bool IsGameWindow(HWND h, SIZE* mode = nullptr) {
    RECT r;
    SIZE m;
    HWND w;
    if (!h || !GameImage(&r, &m, &w) || h != w) return false;
    if (mode) *mode = m;
    return true;
}

// The game's rectangle (exclusive right/bottom edges) on the real screen and back.
void RectFromGame(RECT* rc, const RECT& r, SIZE m) {
    const LONG w = r.right - r.left, h = r.bottom - r.top;
    rc->left = r.left + LONG(int64_t(rc->left) * w / m.cx);
    rc->top = r.top + LONG(int64_t(rc->top) * h / m.cy);
    rc->right = r.left + LONG(int64_t(rc->right) * w / m.cx);
    rc->bottom = r.top + LONG(int64_t(rc->bottom) * h / m.cy);
}

void RectToGame(RECT* rc, const RECT& r, SIZE m) {
    const LONG w = std::max<LONG>(r.right - r.left, 1), h = std::max<LONG>(r.bottom - r.top, 1);
    auto x = [&](LONG v) { return std::clamp<LONG>(LONG(int64_t(v - r.left) * m.cx / w), 0, m.cx); };
    auto y = [&](LONG v) { return std::clamp<LONG>(LONG(int64_t(v - r.top) * m.cy / h), 0, m.cy); };
    *rc = RECT{x(rc->left), y(rc->top), x(rc->right), y(rc->bottom)};
}

BOOL WINAPI HookGetCursorPos(POINT* p) {
    const BOOL ok = g_getCursorPos(p);
    if (ok && p) MouseToGame(p);
    return ok;
}

BOOL WINAPI HookSetCursorPos(int x, int y) {
    POINT p = {x, y};
    MouseFromGame(&p);
    return g_setCursorPos(p.x, p.y);
}

DWORD WINAPI HookGetMessagePos() {
    const DWORD v = g_getMessagePos();
    POINT p = {static_cast<short>(LOWORD(v)), static_cast<short>(HIWORD(v))};
    if (!MouseToGame(&p)) return v;
    return MAKELONG(static_cast<short>(p.x), static_cast<short>(p.y));
}

// A game confining the cursor to its screen confines it to the image.
BOOL WINAPI HookClipCursor(const RECT* rc) {
    RECT r;
    SIZE m;
    if (!rc || !GameImage(&r, &m)) return g_clipCursor(rc);
    RECT real = *rc;
    RectFromGame(&real, r, m);
    return g_clipCursor(&real);
}

BOOL WINAPI HookGetClipCursor(RECT* rc) {
    const BOOL ok = g_getClipCursor(rc);
    RECT r;
    SIZE m;
    if (ok && rc && GameImage(&r, &m)) RectToGame(rc, r, m);
    return ok;
}

// The screen is the game's display mode, as with a real mode switch.
int WINAPI HookGetSystemMetrics(int index) {
    if (index == SM_CXSCREEN || index == SM_CYSCREEN) {
        RECT r;
        SIZE m;
        if (GameImage(&r, &m)) return index == SM_CXSCREEN ? m.cx : m.cy;
    }
    return g_getSystemMetrics(index);
}

BOOL WINAPI HookScreenToClient(HWND h, POINT* p) {
    if (p && IsGameWindow(h)) return TRUE;
    return g_screenToClient(h, p);
}

BOOL WINAPI HookClientToScreen(HWND h, POINT* p) {
    if (p && IsGameWindow(h)) return TRUE;
    return g_clientToScreen(h, p);
}

int WINAPI HookMapWindowPoints(HWND from, HWND to, POINT* p, UINT n) {
    // Between the game's window and the screen (NULL or the desktop): nothing moves.
    const HWND desktop = GetDesktopWindow();
    if ((IsGameWindow(from) && (!to || to == desktop)) || (IsGameWindow(to) && (!from || from == desktop))) return 0;
    return g_mapWindowPoints(from, to, p, n);
}

BOOL WINAPI HookGetClientRect(HWND h, RECT* rc) {
    SIZE m;
    if (rc && IsGameWindow(h, &m)) {
        *rc = RECT{0, 0, m.cx, m.cy};
        return TRUE;
    }
    return g_getClientRect(h, rc);
}

BOOL WINAPI HookGetWindowRect(HWND h, RECT* rc) {
    SIZE m;
    if (rc && IsGameWindow(h, &m)) {
        *rc = RECT{0, 0, m.cx, m.cy};
        return TRUE;
    }
    return g_getWindowRect(h, rc);
}

struct Hook {
    const char* name;
    void* replacement;
    void** original;
};

const Hook kHooks[] = {
    {"GetCursorPos", reinterpret_cast<void*>(HookGetCursorPos), reinterpret_cast<void**>(&g_getCursorPos)},
    {"SetCursorPos", reinterpret_cast<void*>(HookSetCursorPos), reinterpret_cast<void**>(&g_setCursorPos)},
    {"GetMessagePos", reinterpret_cast<void*>(HookGetMessagePos), reinterpret_cast<void**>(&g_getMessagePos)},
    {"ClipCursor", reinterpret_cast<void*>(HookClipCursor), reinterpret_cast<void**>(&g_clipCursor)},
    {"GetClipCursor", reinterpret_cast<void*>(HookGetClipCursor), reinterpret_cast<void**>(&g_getClipCursor)},
    {"GetSystemMetrics", reinterpret_cast<void*>(HookGetSystemMetrics), reinterpret_cast<void**>(&g_getSystemMetrics)},
    {"ScreenToClient", reinterpret_cast<void*>(HookScreenToClient), reinterpret_cast<void**>(&g_screenToClient)},
    {"ClientToScreen", reinterpret_cast<void*>(HookClientToScreen), reinterpret_cast<void**>(&g_clientToScreen)},
    {"MapWindowPoints", reinterpret_cast<void*>(HookMapWindowPoints), reinterpret_cast<void**>(&g_mapWindowPoints)},
    {"GetClientRect", reinterpret_cast<void*>(HookGetClientRect), reinterpret_cast<void**>(&g_getClientRect)},
    {"GetWindowRect", reinterpret_cast<void*>(HookGetWindowRect), reinterpret_cast<void**>(&g_getWindowRect)},
};

struct Patched {
    void** slot;
    void* original;
};
std::vector<Patched>* g_patched;  // g_lock

void WriteSlot(void** slot, void* value) {
    DWORD old;
    // Executable too: some linkers put the import table in the code section.
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
}

bool IsUser32(const char* name) {
    return _strnicmp(name, "user32", 6) == 0 && (name[6] == 0 || _stricmp(name + 6, ".dll") == 0);
}

// The module's imports of the cursor functions from user32 go to dd2d9's.
void PatchModule(HMODULE m) {
    auto* base = reinterpret_cast<uint8_t*>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; d++) {
        if (!IsUser32(reinterpret_cast<const char*>(base + d->Name))) continue;
        auto* iat = reinterpret_cast<void**>(base + d->FirstThunk);
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk
                                                                                                    : d->FirstThunk));
        for (size_t i = 0; names[i].u1.AddressOfData; i++) {
            for (const Hook& h : kHooks) {
                bool match;
                if (d->OriginalFirstThunk && !IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) {
                    const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
                    match = strcmp(reinterpret_cast<const char*>(ibn->Name), h.name) == 0;
                } else {
                    match = iat[i] == *h.original;  // no names (bound): by address
                }
                if (!match || iat[i] == h.replacement) continue;
                g_patched->push_back({&iat[i], iat[i]});
                WriteSlot(&iat[i], h.replacement);
            }
        }
    }
}

// Windows' modules, and the graphics, sound and input DLLs a game folder may hold (D7VK,
// DXVK, dgVoodoo, ...): those work with the real screen.
bool LeaveAlone(const wchar_t* path, const wchar_t* name) {
    wchar_t sys[MAX_PATH], win[MAX_PATH];
    const UINT ns = GetSystemDirectoryW(sys, MAX_PATH), nw = GetWindowsDirectoryW(win, MAX_PATH);
    if ((ns && _wcsnicmp(path, sys, ns) == 0) || (nw && _wcsnicmp(path, win, nw) == 0)) return true;
    static const wchar_t* kNames[] = {L"ddraw.dll", L"ddraw_.dll", L"d3d8.dll", L"d3d9.dll", L"d3d10core.dll",
                                      L"d3d11.dll", L"dxgi.dll", L"opengl32.dll", L"dinput.dll", L"dinput8.dll",
                                      L"dsound.dll", L"winmm.dll", L"glide.dll", L"glide2x.dll", L"glide3x.dll"};
    for (const wchar_t* n : kNames)
        if (_wcsicmp(name, n) == 0) return true;
    return false;
}

}  // namespace

void InstallMouseHooks() {
    if (!g_config.mouseScale) return;
    Guard l;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    if (!g_patched) {
        g_patched = new std::vector<Patched>;
        for (const Hook& h : kHooks) *h.original = reinterpret_cast<void*>(GetProcAddress(user32, h.name));
    }
    for (const Hook& h : kHooks)
        if (!*h.original) return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&InstallMouseHooks), &self);
    // The game's own modules (its executable and the DLLs it brought), not Windows'
    // and not dd2d9 itself.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    const size_t before = g_patched->size();
    for (BOOL more = Module32FirstW(snap, &me); more; more = Module32NextW(snap, &me)) {
        if (me.hModule == self || LeaveAlone(me.szExePath, me.szModule)) continue;
        PatchModule(me.hModule);
    }
    CloseHandle(snap);
    if (g_patched->size() != before)
        DD2D9_LOG("mouse: %u cursor imports translated for the game's display mode", unsigned(g_patched->size() - before));
}

void RemoveMouseHooks() {
    if (!g_patched) return;
    for (const Patched& p : *g_patched) WriteSlot(p.slot, p.original);
    g_patched->clear();
}

}  // namespace dd2d9
