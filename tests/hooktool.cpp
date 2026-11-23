// hooktool.dll: a DLL that asks Windows how much memory the machine has, for ddtest's check
// of which modules dd2d9's hooks reach. Loaded under its own name it is a DLL of the game's
// (answered with dd2d9's ram=); copied as RTSSHooks.dll, an overlay tool's hook, left alone.
#include <windows.h>

extern "C" __declspec(dllexport) DWORD WINAPI ToolTotalRamMB() {
    MEMORYSTATUS ms = {};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    return DWORD(ms.dwTotalPhys >> 20);
}
