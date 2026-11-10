// ddtest's stand-in for one of Windows' own modules whose call reaches a dd2d9 hook through
// another hooker's patch (Windows' DirectInput 1-7 in Nerf Arena Blast and Unreal
// Tournament): ddtest copies it into the Windows folder, loads it from there and has it call
// dd2d9's GetCursorPos hook. A call, not a jump: the hook's return address is in here.
#include <windows.h>
#include <unknwn.h>

using PointFn = BOOL(WINAPI*)(POINT*);

extern "C" __declspec(dllexport) BOOL WINAPI CallPointFn(PointFn fn, POINT* p) {
    volatile BOOL r = fn(p);  // (volatile: no tail call)
    return r;
}

// Also ddtest's stand-in for a game DLL that releases its DirectDraw objects as it is
// unloaded (from DllMain, under the loader lock): the two given are released then, `first`
// first.
static IUnknown *g_first, *g_then;

extern "C" __declspec(dllexport) void WINAPI ReleaseAtUnload(IUnknown* first, IUnknown* then) {
    g_first = first;
    g_then = then;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && !reserved) {
        if (g_first) g_first->Release();
        if (g_then) g_then->Release();
    }
    return TRUE;
}
