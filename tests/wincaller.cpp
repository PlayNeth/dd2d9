// ddtest's stand-in for one of Windows' own modules whose call reaches a dd2d9 hook through
// another hooker's patch (Windows' DirectInput 1-7 in Nerf Arena Blast and Unreal
// Tournament): ddtest copies it into the Windows folder, loads it from there and has it call
// dd2d9's GetCursorPos hook. A call, not a jump: the hook's return address is in here.
#include <windows.h>

using PointFn = BOOL(WINAPI*)(POINT*);

extern "C" __declspec(dllexport) BOOL WINAPI CallPointFn(PointFn fn, POINT* p) {
    volatile BOOL r = fn(p);  // (volatile: no tail call)
    return r;
}
