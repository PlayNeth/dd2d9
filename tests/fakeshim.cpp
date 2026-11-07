// A stand-in for Windows' compatibility shims (AcLayers.dll), for shimtest.exe, which loads
// ddraw.dll after it. Told by the loader when ddraw.dll is loaded, before it starts (a DLL
// load notification, as the shim engine is told), it does to dd2d9 what the shim engine did
// to N.I.C.E. 2's bn_setup.exe on Windows 11:
// - its GetProcAddress hook goes into ddraw.dll's imports before ddraw.dll starts, and,
//   asked for GetProcAddress, hands back itself;
// - when called, it puts itself back into the exe's GetProcAddress import if something else
//   has replaced it there (the shim engine patching imports again as a DLL loads), and from
//   then on passes calls on to whatever it found there;
// - its answer for GetTickCount is a function of its own (a shimmed function), which a
//   caller that goes through it receives.
// It passes calls on with a jump, or, with FAKESHIM_CALL set (shimtest's "call"), with a call.
#include <windows.h>

#include <cstring>

namespace {

using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_next;  // what the shim passes calls on to
void** g_exeSlot;         // the exe's GetProcAddress import

void** ImportSlot(HMODULE m, const char* dll, const char* name) {
    auto* base = reinterpret_cast<BYTE*>(m);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp(reinterpret_cast<char*>(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
        auto* iat = reinterpret_cast<void**>(base + d->FirstThunk);
        for (size_t i = 0; names[i].u1.AddressOfData; i++) {
            if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(ibn->Name), name) == 0) return &iat[i];
        }
    }
    return nullptr;
}

void Write(void** slot, void* v) {
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    *slot = v;
    VirtualProtect(slot, sizeof(void*), old, &old);
}

DWORD WINAPI ShimGetTickCount() { return 0x5117; }

FARPROC WINAPI ShimGetProcAddress(HMODULE m, LPCSTR name) {
    if (g_exeSlot && *g_exeSlot != reinterpret_cast<void*>(ShimGetProcAddress)) {
        g_next = reinterpret_cast<GetProcAddressFn>(*g_exeSlot);
        Write(g_exeSlot, reinterpret_cast<void*>(ShimGetProcAddress));
    }
    if (!IS_INTRESOURCE(name) && strcmp(name, "GetProcAddress") == 0)
        return reinterpret_cast<FARPROC>(reinterpret_cast<void*>(ShimGetProcAddress));
    if (!IS_INTRESOURCE(name) && strcmp(name, "GetTickCount") == 0)
        return reinterpret_cast<FARPROC>(reinterpret_cast<void*>(ShimGetTickCount));
    // Passed on with a jump (the compiler's tail call), or, run as "shimtest call", with a
    // call the shim comes back to.
    static const bool call = GetEnvironmentVariableA("FAKESHIM_CALL", nullptr, 0) != 0;
    if (!call) return g_next(m, name);
    static volatile LONG passed;
    const FARPROC r = g_next(m, name);
    InterlockedIncrement(&passed);
    return r;
}

// LdrRegisterDllNotification's callback (the structures as ntdll has them).
struct UnicodeStr {
    USHORT length, maximum;
    PWSTR buffer;
};
struct LoadedData {
    ULONG flags;
    const UnicodeStr* fullName;
    const UnicodeStr* baseName;
    void* base;
    ULONG size;
};
void CALLBACK OnDll(ULONG reason, const LoadedData* d, void*) {
    if (reason != 1 || !d || !d->baseName || g_exeSlot) return;  // 1: loaded
    const UnicodeStr& n = *d->baseName;
    if (n.length != 9 * sizeof(wchar_t) || _wcsnicmp(n.buffer, L"ddraw.dll", 9) != 0) return;
    void** ddSlot = ImportSlot(static_cast<HMODULE>(d->base), "kernel32.dll", "GetProcAddress");
    if (!ddSlot) return;  // (FakeShimInstalled() then says so)
    Write(ddSlot, reinterpret_cast<void*>(ShimGetProcAddress));
    g_exeSlot = ImportSlot(GetModuleHandleA(nullptr), "kernel32.dll", "GetProcAddress");
}

}  // namespace

extern "C" __declspec(dllexport) int FakeShimInstalled() { return g_exeSlot != nullptr; }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void*) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_next = reinterpret_cast<GetProcAddressFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcAddress")));
    using RegisterFn = LONG(NTAPI*)(ULONG, void*, void*, void**);
    auto reg = reinterpret_cast<RegisterFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrRegisterDllNotification")));
    static void* cookie;
    if (reg) reg(0, reinterpret_cast<void*>(OnDll), nullptr, &cookie);
    return TRUE;
}
