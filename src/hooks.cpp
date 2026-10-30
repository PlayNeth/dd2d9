// See hooks.h.
#include "hooks.h"

#include <tlhelp32.h>
#include <winternl.h>

#include <cstring>
#include <cwchar>
#include <vector>

namespace dd2d9 {

namespace {

struct Patched {
    void** slot;
    void* original;
};
SRWLOCK g_hookLock = SRWLOCK_INIT;
std::vector<Patched>* g_patched;  // g_hookLock

void WriteSlot(void** slot, void* value) {
    DWORD old;
    // Executable too: some linkers put the import table in the code section.
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
}

// "user32", "USER32.dll", "user32.DLL" all match "user32".
bool DllIs(const char* imported, const char* dll) {
    const size_t n = strlen(dll);
    return _strnicmp(imported, dll, n) == 0 && (imported[n] == 0 || _stricmp(imported + n, ".dll") == 0);
}

unsigned PatchModule(HMODULE m, const ImportHook* hooks, size_t n) {
    auto* base = reinterpret_cast<uint8_t*>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;
    unsigned count = 0;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; d++) {
        const char* dllName = reinterpret_cast<const char*>(base + d->Name);
        auto* iat = reinterpret_cast<void**>(base + d->FirstThunk);
        const bool named = d->OriginalFirstThunk != 0;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + (named ? d->OriginalFirstThunk : d->FirstThunk));
        for (size_t i = 0; names[i].u1.AddressOfData; i++) {
            for (size_t h = 0; h < n; h++) {
                const ImportHook& k = hooks[h];
                if (!*k.original || !DllIs(dllName, k.dll)) continue;
                bool match;
                if (named && !IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) {
                    const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
                    match = strcmp(reinterpret_cast<const char*>(ibn->Name), k.name) == 0;
                } else {
                    match = iat[i] == *k.original;  // no names (bound) or by ordinal: by address
                }
                if (!match || iat[i] == k.replacement) continue;
                g_patched->push_back({&iat[i], iat[i]});
                WriteSlot(&iat[i], k.replacement);
                count++;
                break;
            }
        }
    }
    return count;
}

// Windows' modules, and the graphics, sound and input DLLs a game folder may hold (DXVK,
// dgVoodoo, ...): those work with the real machine.
bool LeaveAlone(const wchar_t* path) {
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    wchar_t sys[MAX_PATH], win[MAX_PATH];
    const UINT ns = GetSystemDirectoryW(sys, MAX_PATH), nw = GetWindowsDirectoryW(win, MAX_PATH);
    if ((ns && ns < MAX_PATH && _wcsnicmp(path, sys, ns) == 0) || (nw && nw < MAX_PATH && _wcsnicmp(path, win, nw) == 0))
        return true;
    static const wchar_t* kNames[] = {L"ddraw.dll", L"ddraw_.dll", L"d3d8.dll", L"d3d9.dll", L"d3d10core.dll",
                                      L"d3d11.dll", L"dxgi.dll", L"opengl32.dll", L"dinput.dll", L"dinput8.dll",
                                      L"dsound.dll", L"winmm.dll", L"glide.dll", L"glide2x.dll", L"glide3x.dll"};
    for (const wchar_t* n : kNames)
        if (_wcsicmp(name, n) == 0) return true;
    return false;
}

// The loaded modules, from the loader's own list: in DllMain, where the loader lock (held
// by this thread) keeps the list still and the tool-help snapshot shouldn't be used.
void ModulesFromLoader(std::vector<std::pair<HMODULE, std::wstring>>* out) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"  // mingw's NtCurrentTeb reads fs:[0x18]
    const PEB* peb = NtCurrentTeb()->ProcessEnvironmentBlock;
#pragma GCC diagnostic pop
    if (!peb || !peb->Ldr) return;
    const LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    for (const LIST_ENTRY* e = head->Flink; e && e != head; e = e->Flink) {
        const auto* entry = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (!entry->DllBase || !entry->FullDllName.Buffer) continue;
        out->emplace_back(static_cast<HMODULE>(entry->DllBase),
                          std::wstring(entry->FullDllName.Buffer, entry->FullDllName.Length / sizeof(wchar_t)));
    }
}

void ModulesFromSnapshot(std::vector<std::pair<HMODULE, std::wstring>>* out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    for (BOOL more = Module32FirstW(snap, &me); more; more = Module32NextW(snap, &me))
        out->emplace_back(me.hModule, me.szExePath);
    CloseHandle(snap);
}

}  // namespace

unsigned PatchGameModules(const ImportHook* hooks, size_t n, bool inLoaderLock) {
    for (size_t h = 0; h < n; h++) {
        if (*hooks[h].original) continue;
        if (HMODULE dll = GetModuleHandleA(hooks[h].dll))
            *hooks[h].original = reinterpret_cast<void*>(GetProcAddress(dll, hooks[h].name));
    }
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&PatchGameModules), &self);
    std::vector<std::pair<HMODULE, std::wstring>> modules;
    if (inLoaderLock) ModulesFromLoader(&modules);
    else ModulesFromSnapshot(&modules);
    unsigned count = 0;
    AcquireSRWLockExclusive(&g_hookLock);
    if (!g_patched) g_patched = new std::vector<Patched>;
    for (const auto& [m, path] : modules) {
        if (m == self || LeaveAlone(path.c_str())) continue;
        count += PatchModule(m, hooks, n);
    }
    ReleaseSRWLockExclusive(&g_hookLock);
    return count;
}

void UnpatchAll() {
    AcquireSRWLockExclusive(&g_hookLock);
    if (g_patched) {
        for (const Patched& p : *g_patched) WriteSlot(p.slot, p.original);
        g_patched->clear();
    }
    ReleaseSRWLockExclusive(&g_hookLock);
}

}  // namespace dd2d9
