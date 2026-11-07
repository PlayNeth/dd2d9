// shimtest: dd2d9 under a stand-in for Windows' compatibility shims (fakeshim.dll, which
// hooks ddraw.dll's GetProcAddress import as it is loaded, before it starts). N.I.C.E. 2's
// bn_setup.exe died of a stack overflow on Windows 11 this way: dd2d9's GetProcAddress hook
// and the shim's each passed calls on to the other. Checks that GetProcAddress calls return,
// and with the shim's answer (dd2d9 still passes calls on through the shim). Writes
// shimtest.txt, ending in "ALL PASSED" or "FAILED"; neither means the process died (the
// loop's stack overflow). "shimtest call": the shim passes calls on with a call, not a jump.
#include <windows.h>
#include <ddraw.h>

#include <cstdio>
#include <cstring>

extern "C" __declspec(dllimport) int FakeShimInstalled();

static FILE* g_out;
static int g_fail;

static void Check(bool ok, const char* what) {
    fprintf(g_out, "%s %s\n", ok ? "PASS" : "FAIL", what);
    fflush(g_out);
    if (!ok) g_fail++;
}

// An export's address from the DLL's export table (the lookups through GetProcAddress are
// what is tested).
static void* Export(HMODULE m, const char* name) {
    auto* b = reinterpret_cast<BYTE*>(m);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(b + reinterpret_cast<IMAGE_DOS_HEADER*>(b)->e_lfanew);
    const DWORD dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    auto* ed = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(b + dir);
    auto* names = reinterpret_cast<DWORD*>(b + ed->AddressOfNames);
    auto* ords = reinterpret_cast<WORD*>(b + ed->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<DWORD*>(b + ed->AddressOfFunctions);
    for (DWORD i = 0; i < ed->NumberOfNames; i++)
        if (strcmp(reinterpret_cast<char*>(b + names[i]), name) == 0) return b + funcs[ords[i]];
    return nullptr;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmd, int) {
    const bool call = cmd && strstr(cmd, "call");
    if (call) SetEnvironmentVariableA("FAKESHIM_CALL", "1");  // (read at the shim's first call)
    g_out = fopen(call ? "shimtest-call.txt" : "shimtest.txt", "w");
    if (!g_out) return 1;
    fprintf(g_out, "the shim passes calls on with a %s\n", call ? "call" : "jump");
    // Loaded after the stand-in shim is listening: it is told before dd2d9 starts.
    const HMODULE ddraw = LoadLibraryA("ddraw.dll");
    Check(ddraw != nullptr, "ddraw.dll loaded");
    Check(FakeShimInstalled() != 0, "the stand-in shim patched ddraw.dll's imports before it started");
    using CreateFn = HRESULT(WINAPI*)(GUID*, IDirectDraw**, IUnknown*);
    auto create = ddraw ? reinterpret_cast<CreateFn>(Export(ddraw, "DirectDrawCreate")) : nullptr;
    IDirectDraw* dd = nullptr;  // (DirectDrawCreate: dd2d9 patches the game's imports again)
    Check(create && SUCCEEDED(create(nullptr, &dd, nullptr)), "DirectDrawCreate");
    fprintf(g_out, "GetProcAddress through the shim (if the hooks pass calls on to each other, the process dies here)\n");
    fflush(g_out);
    // Through this exe's own GetProcAddress import, which dd2d9 hooks: a function the shim
    // passes on (where the hooks met), then one it answers itself.
    const HMODULE k32 = GetModuleHandleA("kernel32.dll");
    const FARPROC pid = GetProcAddress(k32, "GetCurrentProcessId");
    char line[160];
    const DWORD p = pid ? reinterpret_cast<DWORD(WINAPI*)()>(reinterpret_cast<void*>(pid))() : 0;
    snprintf(line, sizeof(line), "GetProcAddress returned GetCurrentProcessId (%lu, want %lu)", p, GetCurrentProcessId());
    Check(p == GetCurrentProcessId(), line);
    const FARPROC tick = GetProcAddress(k32, "GetTickCount");
    const DWORD v = tick ? reinterpret_cast<DWORD(WINAPI*)()>(reinterpret_cast<void*>(tick))() : 0;
    snprintf(line, sizeof(line), "the shim's GetTickCount handed out (0x%lx, want 0x5117)", v);
    Check(v == 0x5117, line);
    if (dd) dd->Release();
    fprintf(g_out, "%s\n", g_fail ? "FAILED" : "ALL PASSED");
    fclose(g_out);
    return g_fail;
}
