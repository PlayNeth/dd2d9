// See hooks.h.
#include "hooks.h"

#include <tlhelp32.h>
#include <winternl.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace dd2d9 {

namespace {

struct Patched {
    void** slot;
    void* original;
    void* replacement;  // put back only while the slot still holds it
};
SRWLOCK g_hookLock = SRWLOCK_INIT;
std::vector<Patched>* g_patched;  // g_hookLock
// The modules a hook table was applied to (g_hookLock): not walked again for it (a game
// creating DirectDraw objects over and over would have every module's imports walked each
// time); a module loaded since, or another at the same address, is.
// A module unloaded and loaded again at the same address is told apart by a slot it was
// patched in (`probe`) holding the real function again (not the hook, nor another
// hooker's patch over it): walked again.
struct Scanned {
    const ImportHook* hooks;
    HMODULE module;
    std::wstring path;
    void** probe;  // one slot patched (none: nothing there to patch, whichever copy)
    void* real;
};
std::vector<Scanned>* g_scanned;

bool WriteSlot(void** slot, void* value) {
    DWORD old;
    // Executable too: some linkers put the import table in the code section.
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    // One aligned pointer-sized store: a thread calling through the slot meanwhile sees
    // the old function or the new one.
    InterlockedExchangePointer(slot, value);
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// A DLL's own export of `name`, read from its export table, forwarders followed: the real
// function even where GetProcAddress has been hooked (see NextFunction). nullptr: not found.
void* OwnExport(HMODULE m, const char* name, int depth = 0) {
    if (!m || depth > 8) return nullptr;
    auto* base = reinterpret_cast<const uint8_t*>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const DWORD image = nt->OptionalHeader.SizeOfImage;
    if (!dir.VirtualAddress || dir.VirtualAddress >= image || dir.Size > image - dir.VirtualAddress) return nullptr;
    const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + ed->AddressOfNames);
    const auto* ords = reinterpret_cast<const WORD*>(base + ed->AddressOfNameOrdinals);
    const auto* funcs = reinterpret_cast<const DWORD*>(base + ed->AddressOfFunctions);
    DWORD index = DWORD(-1);
    if (IS_INTRESOURCE(name)) {
        const DWORD ord = DWORD(reinterpret_cast<uintptr_t>(name));
        if (ord >= ed->Base) index = ord - ed->Base;
    } else {
        // Names are sorted (the loader searches them so too).
        DWORD lo = 0, hi = ed->NumberOfNames;
        while (lo < hi) {
            const DWORD mid = lo + (hi - lo) / 2;
            const int c = strcmp(reinterpret_cast<const char*>(base + names[mid]), name);
            if (c == 0) {
                index = ords[mid];
                break;
            }
            if (c < 0) lo = mid + 1;
            else hi = mid;
        }
    }
    if (index >= ed->NumberOfFunctions || !funcs[index]) return nullptr;
    const DWORD rva = funcs[index];
    if (rva < dir.VirtualAddress || rva >= dir.VirtualAddress + dir.Size) return const_cast<uint8_t*>(base + rva);
    // A forwarder, "dll.function" or "dll.#ordinal" (the dll an API set name on Windows 10
    // and later, which GetModuleHandle resolves to its host). Only a DLL already loaded:
    // this may run in DllMain.
    const char* fwd = reinterpret_cast<const char*>(base + rva);
    const char* dot = strrchr(fwd, '.');
    if (!dot || dot == fwd || size_t(dot - fwd) >= MAX_PATH) return nullptr;
    char dll[MAX_PATH + 4];
    memcpy(dll, fwd, size_t(dot - fwd));
    dll[dot - fwd] = 0;
    HMODULE target = GetModuleHandleA(dll);
    if (!target) return nullptr;
    if (dot[1] == '#') return OwnExport(target, MAKEINTRESOURCEA(atoi(dot + 2)), depth + 1);
    return OwnExport(target, dot + 1, depth + 1);
}

// For NextFunction: each hook's original, the module it lies in and the real function.
// Written once per original (under g_hookLock), read without the lock: an entry is filled
// before its `original` is published.
struct Next {
    void* const* volatile original;
    void* real;
    uintptr_t base, size;  // the module holding *original (size 0: none)
};
uintptr_t g_selfBase, g_selfSize;  // dd2d9's own image (set with the first entry)
constexpr size_t kNextSlots = 256;  // a power of two, well over the hooks there are
Next g_next[kNextSlots];

size_t NextSlot(void* const* original) { return (reinterpret_cast<uintptr_t>(original) >> 2) * 2654435761u % kNextSlots; }

// The image holding `p`: base and size (0, 0: none).
void ImageOf(const void* p, uintptr_t* base, uintptr_t* size) {
    *base = *size = 0;
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(p), &m) ||
        !m)
        return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(m) + dos->e_lfanew);
    *base = reinterpret_cast<uintptr_t>(m);
    *size = nt->OptionalHeader.SizeOfImage;
}

void Remember(void* const* original, void* real) {
    uintptr_t base, size;
    ImageOf(*original, &base, &size);
    if (!g_selfSize) {
        uintptr_t sb, ss;
        ImageOf(reinterpret_cast<const void*>(&Remember), &sb, &ss);
        g_selfBase = sb;
        MemoryBarrier();
        g_selfSize = ss;
    }
    AcquireSRWLockExclusive(&g_hookLock);
    for (size_t i = NextSlot(original), n = 0; n < kNextSlots; i = (i + 1) % kNextSlots, n++) {
        Next& e = g_next[i];
        if (e.original == original) break;  // (resolved once: *original never changes after)
        if (e.original) continue;
        e.real = real;
        e.base = base;
        e.size = size;
        MemoryBarrier();
        e.original = original;
        break;
    }
    ReleaseSRWLockExclusive(&g_hookLock);
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
    // A packed executable may have wiped or moved its import table after unpacking: only one
    // inside the image, in memory that can be read.
    const DWORD image = nt->OptionalHeader.SizeOfImage;
    if (dir.VirtualAddress >= image || dir.Size > image - dir.VirtualAddress) return 0;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(base + dir.VirtualAddress, &mi, sizeof(mi)) || mi.State != MEM_COMMIT ||
        (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return 0;
    unsigned count = 0;
    std::vector<const ImportHook*> mine;  // the hooks for the DLL a descriptor imports from
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; d++) {
        const char* dllName = reinterpret_cast<const char*>(base + d->Name);
        mine.clear();
        for (size_t h = 0; h < n; h++)
            if (*hooks[h].original && DllIs(dllName, hooks[h].dll)) mine.push_back(&hooks[h]);
        if (mine.empty()) continue;
        auto* iat = reinterpret_cast<void**>(base + d->FirstThunk);
        const bool named = d->OriginalFirstThunk != 0;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + (named ? d->OriginalFirstThunk : d->FirstThunk));
        for (size_t i = 0; names[i].u1.AddressOfData; i++) {
            for (const ImportHook* hp : mine) {
                const ImportHook& k = *hp;
                bool match;
                if (named && !IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) {
                    const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
                    match = strcmp(reinterpret_cast<const char*>(ibn->Name), k.name) == 0;
                } else {
                    match = iat[i] == *k.original;  // no names (bound) or by ordinal: by address
                }
                if (!match || iat[i] == k.replacement) continue;
                void* const original = iat[i];
                if (!WriteSlot(&iat[i], k.replacement)) break;
                g_patched->push_back({&iat[i], original, k.replacement});
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
    static wchar_t sys[MAX_PATH], win[MAX_PATH];  // (g_hookLock)
    static UINT ns, nw;
    static bool known;
    if (!known) {
        ns = GetSystemDirectoryW(sys, MAX_PATH);
        nw = GetWindowsDirectoryW(win, MAX_PATH);
        known = true;
    }
    // (A whole folder: C:\Windows matches C:\Windows\..., not C:\WindowsGames\...)
    auto under = [&](const wchar_t* dir, UINT n) {
        return n && n < MAX_PATH && _wcsnicmp(path, dir, n) == 0 && (path[n] == L'\\' || dir[n - 1] == L'\\');
    };
    if (under(sys, ns) || under(win, nw)) return true;
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
        // (CONTAINING_RECORD, with offsetof: the macro takes the member of a null pointer.)
        const auto* entry = reinterpret_cast<const LDR_DATA_TABLE_ENTRY*>(reinterpret_cast<const char*>(e) -
                                                                          offsetof(LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks));
        if (!entry->DllBase || !entry->FullDllName.Buffer) continue;
        out->emplace_back(static_cast<HMODULE>(entry->DllBase),
                          std::wstring(entry->FullDllName.Buffer, entry->FullDllName.Length / sizeof(wchar_t)));
    }
}

void ModulesFromSnapshot(std::vector<std::pair<HMODULE, std::wstring>>* out) {
    HANDLE snap = INVALID_HANDLE_VALUE;
    // ERROR_BAD_LENGTH while another thread loads a module: tried again, as documented.
    for (int i = 0; i < 5 && snap == INVALID_HANDLE_VALUE; i++) {
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (snap == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) return;
    }
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    for (BOOL more = Module32FirstW(snap, &me); more; more = Module32NextW(snap, &me))
        out->emplace_back(me.hModule, me.szExePath);
    CloseHandle(snap);
}

}  // namespace

void* NextFunction(void* const* original, const void* caller) {
    void* const next = *original;
    const uintptr_t at = reinterpret_cast<uintptr_t>(caller);
    for (size_t i = NextSlot(original), n = 0; n < kNextSlots; i = (i + 1) % kNextSlots, n++) {
        const Next& e = g_next[i];
        void* const* const o = e.original;
        if (!o) break;
        if (o != original) continue;
        MemoryBarrier();
        // Called from the module of the function it would call (that function passed the
        // call here, and would get it back), or from dd2d9 itself, which never calls its
        // hooks: the function this hook called jumped straight back into it (a tail call).
        if (e.real && (at - e.base < e.size || at - g_selfBase < g_selfSize)) return e.real;
        break;
    }
    return next;
}

namespace {
// The modules hooks were called from, and whether each is Windows' (CalledFromWindows). A
// module unloaded and another loaded at its place keeps the first one's answer: rare, and
// at worst one module's calls translated or not as the other's.
struct Caller {
    uintptr_t base, size;
    bool windows;
};
constexpr size_t kCallers = 32;
Caller g_callers[kCallers];  // g_callerLock
size_t g_callerCount, g_callerNext;
SRWLOCK g_callerLock = SRWLOCK_INIT;

// Windows' compatibility shims, which pass a game's calls on: the shim engine and its shim
// DLLs, in the AppPatch folder (Windows 7) or the system folder (Windows 8 and later).
bool IsShimModule(const wchar_t* path) {
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    static const wchar_t* kShims[] = {L"apphelp.dll",  L"aclayers.dll", L"acgenral.dll",
                                      L"acspecfc.dll", L"acxtrnal.dll", L"acwinrt.dll"};
    for (const wchar_t* s : kShims)
        if (_wcsicmp(name, s) == 0) return true;
    wchar_t dir[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH - 10) return false;
    lstrcatW(dir, dir[n - 1] == L'\\' ? L"AppPatch\\" : L"\\AppPatch\\");
    return _wcsnicmp(path, dir, wcslen(dir)) == 0;
}
}  // namespace

bool CalledFromWindows(const void* caller) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(caller);
    AcquireSRWLockShared(&g_callerLock);
    for (size_t i = 0; i < g_callerCount; i++) {
        if (at - g_callers[i].base < g_callers[i].size) {
            const bool windows = g_callers[i].windows;
            ReleaseSRWLockShared(&g_callerLock);
            return windows;
        }
    }
    ReleaseSRWLockShared(&g_callerLock);
    uintptr_t base, size;
    ImageOf(caller, &base, &size);
    if (!size) return false;  // not in a module (code the game generated): the game's
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    AcquireSRWLockExclusive(&g_hookLock);  // (LeaveAlone's)
    // (dd2d9 itself too: its own calls want the real answers, and come here when a shim that
    // patched its imports took a hook as the function it passes them on to)
    const bool windows = base == g_selfBase || (LeaveAlone(path) && !IsShimModule(path));
    ReleaseSRWLockExclusive(&g_hookLock);
    bool added = false;
    AcquireSRWLockExclusive(&g_callerLock);
    size_t i = 0;
    while (i < g_callerCount && g_callers[i].base != base) i++;
    if (i == g_callerCount) {  // (not added meanwhile by another thread)
        if (g_callerCount < kCallers) i = g_callerCount++;
        else i = g_callerNext++ % kCallers;  // full: one goes, in turn
        g_callers[i] = {base, size, windows};
        added = true;
    }
    ReleaseSRWLockExclusive(&g_callerLock);
    if (added && windows && base != g_selfBase) {
        const wchar_t* name = wcsrchr(path, L'\\');
        DD2D9_LOG("mouse: %ls calls dd2d9's cursor and window hooks (through another hooker's patch): passed on as they are",
                  name ? name + 1 : path);
    }
    return windows;
}

unsigned PatchGameModules(const ImportHook* hooks, size_t n, bool inLoaderLock) {
    for (size_t h = 0; h < n; h++) {
        if (*hooks[h].original) continue;
        if (HMODULE dll = GetModuleHandleA(hooks[h].dll)) {
            // GetProcAddress's answer: another hooker's (a compatibility shim's) where there
            // is one, so calls still go through it.
            *hooks[h].original = reinterpret_cast<void*>(GetProcAddress(dll, hooks[h].name));
            if (*hooks[h].original) Remember(hooks[h].original, OwnExport(dll, hooks[h].name));
        }
    }
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&PatchGameModules), &self);
    std::vector<std::pair<HMODULE, std::wstring>> modules;
    if (inLoaderLock) ModulesFromLoader(&modules);
    else ModulesFromSnapshot(&modules);
    // Kept loaded while patched (another thread could unload one meanwhile); pinned before
    // the lock is taken, since pinning takes the loader lock. In DllMain the loader lock,
    // held, keeps every module loaded.
    std::vector<HMODULE> pinned;
    if (!inLoaderLock) {
        size_t kept = 0;
        for (size_t i = 0; i < modules.size(); i++) {
            HMODULE pin = nullptr;
            if (modules[i].first == self) continue;  // (LeaveAlone's, under the lock)
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(modules[i].first), &pin))
                continue;  // gone since the snapshot
            pinned.push_back(pin);
            if (pin == modules[i].first) modules[kept++] = std::move(modules[i]);
        }
        modules.resize(kept);
    }
    unsigned count = 0;
    AcquireSRWLockExclusive(&g_hookLock);
    if (!g_patched) g_patched = new std::vector<Patched>;
    if (!g_scanned) g_scanned = new std::vector<Scanned>;
    for (const auto& [m, path] : modules) {
        if (m == self || LeaveAlone(path.c_str())) continue;
        auto sc = g_scanned->begin();
        for (; sc != g_scanned->end(); ++sc)
            if (sc->hooks == hooks && sc->module == m && sc->path == path) break;
        if (sc != g_scanned->end()) {
            if (!sc->probe || *sc->probe != sc->real) continue;
            g_scanned->erase(sc);  // the same file loaded again: its imports are the real ones again
        }
        const size_t before = g_patched->size();
        count += PatchModule(m, hooks, n);
        Scanned added = {hooks, m, path, nullptr, nullptr};
        if (g_patched->size() > before) added.probe = (*g_patched)[before].slot, added.real = (*g_patched)[before].original;
        g_scanned->push_back(std::move(added));
    }
    ReleaseSRWLockExclusive(&g_hookLock);
    for (HMODULE m : pinned) FreeLibrary(m);  // (outside the lock: it can run a DLL's detach)
    return count;
}

void UnpatchAll() {
    AcquireSRWLockExclusive(&g_hookLock);
    if (g_patched) {
        for (const Patched& p : *g_patched) {
            // Not into a module unloaded since (its memory gone or reused), nor over a
            // later hooker's own patch.
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(p.slot, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) continue;
            if (*p.slot == p.replacement) WriteSlot(p.slot, p.original);
        }
        g_patched->clear();
    }
    if (g_scanned) g_scanned->clear();
    ReleaseSRWLockExclusive(&g_hookLock);
}

}  // namespace dd2d9
