// dd2d9's loader: a winmm.dll for the game's folder that brings dd2d9 into a game that loads
// no ddraw.dll of its own, one that draws with GDI and switches the display with
// ChangeDisplaySettings (Pajama Sam 2). Nearly every Windows game of the era imports winmm.dll,
// and Windows looks in the game's folder for it first (Wine with winmm=n,b).
//
// Every export goes on to the next winmm.dll, unchanged (stubs.cpp: a jump through the address
// looked up here): winmm_next.dll beside this one if there is one (another winmm.dll wrapper,
// renamed, reAL's for one), else Windows' own from the system folder. Then dd2d9 (ddraw.dll
// beside this one, or the system's when that is dd2d9) is loaded, which hooks the game's
// modules as it does for a DirectDraw game,
// and told it came this way (DD2D9_GdiFullscreen: gdifullscreen=auto then shows the game's
// display changes itself). All of it before the game's code runs, in the loader's lock, as
// dd2d9's own start does. Nothing here if dd2d9 isn't there: winmm.dll only.
#include "loader.h"

#include <cwchar>

namespace {

// The folder this DLL is in, with a backslash at its end (empty if not found).
void OwnFolder(HINSTANCE self, wchar_t* out, DWORD size) {
    out[0] = 0;
    const DWORD n = GetModuleFileNameW(self, out, size);
    if (!n || n >= size) {
        out[0] = 0;
        return;
    }
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) slash[1] = 0;
    else out[0] = 0;
}

// `folder` + `name`, if it fits.
bool Join(wchar_t* out, size_t size, const wchar_t* folder, const wchar_t* name) {
    const size_t a = wcslen(folder), b = wcslen(name);
    if (a + b + 1 > size) return false;
    wmemcpy(out, folder, a);
    wmemcpy(out + a, name, b + 1);
    return true;
}

HMODULE LoadNext(const wchar_t* folder) {
    wchar_t path[MAX_PATH];
    if (folder[0] && Join(path, MAX_PATH, folder, L"winmm_next.dll") && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        if (HMODULE m = LoadLibraryW(path)) return m;
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH - 1);
    if (!n || n >= MAX_PATH - 1) return nullptr;
    sys[n] = L'\\';
    sys[n + 1] = 0;
    return Join(path, MAX_PATH, sys, L"winmm.dll") ? LoadLibraryW(path) : nullptr;
}

}  // namespace

extern "C" BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, void*) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    wchar_t folder[MAX_PATH];
    OwnFolder(self, folder, MAX_PATH);
    const HMODULE next = LoadNext(folder);
    if (!next) return FALSE;  // (no winmm at all: the game couldn't run either)
    for (unsigned i = 0; i < kLoaderExportCount; i++) {
        const LoaderExport& e = kLoaderExports[i];
        dd2d9_next[i] = reinterpret_cast<void*>(
            GetProcAddress(next, e.name ? e.name : reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(e.ordinal))));
    }
    // dd2d9, after the table above is filled: it imports winmm itself, which is this DLL. The
    // ddraw.dll beside this one, else the one the system loads (a Wine prefix dd2d9 was
    // installed in with install.sh); one that isn't dd2d9 (no DD2D9_GdiFullscreen) let go.
    wchar_t path[MAX_PATH];
    HMODULE dd = nullptr;
    if (folder[0] && Join(path, MAX_PATH, folder, L"ddraw.dll") && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        dd = LoadLibraryW(path);
    if (!dd) dd = LoadLibraryW(L"ddraw.dll");
    if (dd) {
        if (FARPROC f = GetProcAddress(dd, "DD2D9_GdiFullscreen")) reinterpret_cast<void(WINAPI*)()>(f)();
        else FreeLibrary(dd);
    }
    return TRUE;
}
