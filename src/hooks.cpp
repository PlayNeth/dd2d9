// See hooks.h.
#include "hooks.h"

#include <tlhelp32.h>
#include <winternl.h>

#include <algorithm>
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
// hooker's patch over it): walked again. (With the loader's notifications its records go
// when it is unloaded.)
struct Scanned {
    const ImportHook* hooks;
    HMODULE module;
    std::wstring path;
    void** probe;  // one slot patched (none: nothing there to patch, whichever copy)
    void* real;
};
std::vector<Scanned>* g_scanned;
// Every hook table PatchGameModules was given (g_hookLock): applied to each module that loads
// afterwards (OnDllNotification).
struct Table {
    const ImportHook* hooks;
    size_t n;
};
std::vector<Table>* g_tables;
// The loaded modules whose imports are bound, with their paths (g_hookLock): the modules listed
// in DllMain, then each one the loader announces as loaded (OnDllNotification). Walked instead of
// a tool-help snapshot, which lists a module another thread is still loading (its import slots
// still unbound: a hook written there was overwritten as the loader bound them, and the slot's
// page protection raced with the loader's own). Null: the loader's notifications aren't
// available, and a snapshot it is.
std::vector<std::pair<HMODULE, std::wstring>>* g_modules;
void* g_notifyCookie;
HMODULE g_self;

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

// Whether [p, p + n) is committed memory that can be read: a packed or protected module may
// have freed or protected parts of its image, and this runs in the loader's notification,
// where a fault takes the process down at the game's LoadLibrary.
bool Readable(const void* p, size_t n) {
    uintptr_t a = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(reinterpret_cast<const void*>(a), &mi, sizeof(mi)) || mi.State != MEM_COMMIT ||
            (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            return false;
        a = reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
    }
    return true;
}

// A DLL's export table, with its place and the image's size (ed null: none, or not all of
// it, its names and ordinals within the image and readable). Every module's is read, a
// game's own too (LeaveAlone), so nothing in it is trusted.
struct Exports {
    const IMAGE_EXPORT_DIRECTORY* ed = nullptr;
    DWORD rva = 0, size = 0, image = 0;
};

Exports ExportTable(HMODULE m) {
    Exports x;
    auto* base = reinterpret_cast<const uint8_t*>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!Readable(dos, sizeof(*dos)) || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < LONG(sizeof(*dos)) ||
        dos->e_lfanew > 0x10000)
        return x;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (!Readable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE) return x;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const DWORD image = nt->OptionalHeader.SizeOfImage;
    if (!dir.VirtualAddress || dir.VirtualAddress >= image || dir.Size > image - dir.VirtualAddress ||
        dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY) || !Readable(base + dir.VirtualAddress, dir.Size))
        return x;
    const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    // (Each array within the image: n entries of `size` bytes from `rva`.)
    auto within = [&](DWORD rva, DWORD n, DWORD size) {
        return n <= image / size && rva <= image - n * size && Readable(base + rva, size_t(n) * size);
    };
    if (!within(ed->AddressOfNames, ed->NumberOfNames, 4) || !within(ed->AddressOfNameOrdinals, ed->NumberOfNames, 2) ||
        !within(ed->AddressOfFunctions, ed->NumberOfFunctions, 4))
        return x;
    x.ed = ed;
    x.rva = dir.VirtualAddress;
    x.size = dir.Size;
    x.image = image;
    return x;
}

// The function index of the export named `name` (DWORD(-1): none). Names are sorted (the
// loader searches them so too).
DWORD NamedExport(HMODULE m, const Exports& x, const char* name) {
    auto* base = reinterpret_cast<const uint8_t*>(m);
    const auto* names = reinterpret_cast<const DWORD*>(base + x.ed->AddressOfNames);
    const auto* ords = reinterpret_cast<const WORD*>(base + x.ed->AddressOfNameOrdinals);
    const size_t len = strlen(name) + 1;  // (strncmp reads no further than this of the export's)
    DWORD lo = 0, hi = x.ed->NumberOfNames;
    while (lo < hi) {
        const DWORD mid = lo + (hi - lo) / 2;
        const DWORD rva = names[mid];
        // A name outside the export directory (checked whole) is checked as far as it is read.
        if (rva >= x.image) return DWORD(-1);
        const size_t n = std::min<size_t>(len, x.image - rva);
        if ((rva < x.rva || n > x.size || rva - x.rva > x.size - n) && !Readable(base + rva, n)) return DWORD(-1);
        const int c = strncmp(reinterpret_cast<const char*>(base + rva), name, n);
        if (c == 0) return n == len ? ords[mid] : DWORD(-1);  // (else running past the image)
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return DWORD(-1);
}

// A DLL's own export of `name`, read from its export table, forwarders followed: the real
// function even where GetProcAddress has been hooked (see NextFunction). nullptr: not found.
void* OwnExport(HMODULE m, const char* name, int depth = 0) {
    if (!m || depth > 8) return nullptr;
    const Exports x = ExportTable(m);
    const IMAGE_EXPORT_DIRECTORY* ed = x.ed;
    if (!ed) return nullptr;
    const DWORD dirRva = x.rva, dirSize = x.size;
    auto* base = reinterpret_cast<const uint8_t*>(m);
    const auto* funcs = reinterpret_cast<const DWORD*>(base + ed->AddressOfFunctions);
    DWORD index = DWORD(-1);
    if (IS_INTRESOURCE(name)) {
        const DWORD ord = DWORD(reinterpret_cast<uintptr_t>(name));
        if (ord >= ed->Base) index = ord - ed->Base;
    } else {
        index = NamedExport(m, x, name);
    }
    if (index >= ed->NumberOfFunctions || !funcs[index]) return nullptr;
    const DWORD rva = funcs[index];
    if (rva < dirRva || rva >= dirRva + dirSize) return const_cast<uint8_t*>(base + rva);
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

void RememberNext(void* const* original, void* real, const char* name) {
    // Without the real function the hook has no way out of a shim passing its call back (see
    // NextFunction): noted, as it would show as a stack overflow.
    if (!real) DD2D9_LOG("hooks: %s: the real function not found in its DLL's exports (no guard against a shim's loop)", name);
    uintptr_t base, size;
    ImageOf(*original, &base, &size);
    if (!g_selfSize) {
        uintptr_t sb, ss;
        ImageOf(reinterpret_cast<const void*>(&RememberNext), &sb, &ss);
        g_selfBase = sb;
        MemoryBarrier();
        g_selfSize = ss;
    }
    AcquireSRWLockExclusive(&g_hookLock);
    for (size_t i = NextSlot(original), n = 0; n < kNextSlots; i = (i + 1) % kNextSlots, n++) {
        Next& e = g_next[i];
        if (e.original == original) {  // (resolved once: *original never changes after)
            ReleaseSRWLockExclusive(&g_hookLock);
            return;
        }
        if (e.original) continue;
        e.real = real;
        e.base = base;
        e.size = size;
        MemoryBarrier();
        e.original = original;
        ReleaseSRWLockExclusive(&g_hookLock);
        return;
    }
    ReleaseSRWLockExclusive(&g_hookLock);
    DD2D9_LOG("hooks: %s: no room to remember its real function (no guard against a shim's loop)", name);
}

// "user32", "USER32.dll", "user32.DLL" all match "user32".
bool DllIs(const char* imported, const char* dll) {
    const size_t n = strlen(dll);
    return _strnicmp(imported, dll, n) == 0 && (imported[n] == 0 || _stricmp(imported + n, ".dll") == 0);
}

// Whether the loader has bound the module's imports from the DLLs `hooks` are for. Unbound,
// a named import's slot still holds what its name entry holds (the loader writes the function
// over it): a module another thread is still loading, or one loaded with
// DONT_RESOLVE_DLL_REFERENCES. Left alone for now, and not recorded as walked.
bool ImportsBound(const uint8_t* base, const IMAGE_IMPORT_DESCRIPTOR* first, const ImportHook* hooks, size_t n) {
    for (const IMAGE_IMPORT_DESCRIPTOR* d = first; d->Name; d++) {
        if (!d->OriginalFirstThunk) continue;  // (one table only: nothing to compare)
        const char* dllName = reinterpret_cast<const char*>(base + d->Name);
        bool mine = false;
        for (size_t h = 0; h < n && !mine; h++) mine = *hooks[h].original && DllIs(dllName, hooks[h].dll);
        if (!mine) continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
        const auto* iat = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        if (names[0].u1.AddressOfData && iat[0].u1.AddressOfData == names[0].u1.AddressOfData) return false;
    }
    return true;
}

// `bound`: false when the module's imports aren't bound yet (see ImportsBound), nothing patched.
unsigned PatchModule(HMODULE m, const ImportHook* hooks, size_t n, bool* bound) {
    *bound = true;
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
    const auto* first = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    if (!ImportsBound(base, first, hooks, n)) {
        *bound = false;
        return 0;
    }
    unsigned count = 0;
    std::vector<const ImportHook*> mine;  // the hooks for the DLL a descriptor imports from
    for (auto* d = first; d->Name; d++) {
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
// dgVoodoo, ...): those work with the real machine. Such a DLL is known by its name, or by
// exporting what the DLL it stands for does (one renamed, or chained behind another under
// another name). Reads only the module's memory: called under g_hookLock.
bool LeaveAlone(HMODULE m, const wchar_t* path) {
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
    static const wchar_t* kNames[] = {L"ddraw.dll", L"d3d8.dll", L"d3d9.dll", L"d3d10core.dll", L"d3d11.dll",
                                      L"dxgi.dll", L"opengl32.dll", L"dinput.dll", L"dinput8.dll", L"dsound.dll",
                                      L"winmm.dll", L"glide.dll", L"glide2x.dll", L"glide3x.dll"};
    for (const wchar_t* n : kNames)
        if (_wcsicmp(name, n) == 0) return true;
    static const char* kExports[] = {"DirectDrawCreate", "Direct3DCreate",     "Direct3DCreate8",   "Direct3DCreate9",
                                     "D3D11CreateDevice", "CreateDXGIFactory", "wglGetProcAddress", "vkGetInstanceProcAddr",
                                     "DirectInputCreateA", "DirectInput8Create", "DirectSoundCreate", "grGlideInit",
                                     "_grGlideInit@0"};
    if (!m) return false;
    const Exports x = ExportTable(m);
    if (x.ed)
        for (const char* e : kExports)
            if (NamedExport(m, x, e) != DWORD(-1)) return true;
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
    const bool windows = base == g_selfBase || (LeaveAlone(reinterpret_cast<HMODULE>(base), path) && !IsShimModule(path));
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
        DD2D9_LOG("hooks: %ls calls dd2d9's hooks (through another hooker's patch): passed on as they are",
                  name ? name + 1 : path);
    }
    return windows;
}

namespace {

// Fills each hook's `original` whose DLL is loaded (not yet done). Not under g_hookLock
// (GetProcAddress and RememberNext's lookups).
void Resolve(const ImportHook* hooks, size_t n) {
    for (size_t h = 0; h < n; h++) {
        if (*hooks[h].original) continue;
        if (HMODULE dll = GetModuleHandleA(hooks[h].dll)) {
            // GetProcAddress's answer: another hooker's (a compatibility shim's) where there
            // is one, so calls still go through it.
            *hooks[h].original = reinterpret_cast<void*>(GetProcAddress(dll, hooks[h].name));
            if (*hooks[h].original) RememberNext(hooks[h].original, OwnExport(dll, hooks[h].name), hooks[h].name);
        }
    }
}

// g_hookLock held from here on.

void MakeLists() {
    if (!g_patched) g_patched = new std::vector<Patched>;
    if (!g_scanned) g_scanned = new std::vector<Scanned>;
    if (!g_tables) g_tables = new std::vector<Table>;
}

// Applies `hooks` to the module `m`, unless done before (see Scanned) or the module is one
// left alone.
unsigned PatchOnce(HMODULE m, const std::wstring& path, const ImportHook* hooks, size_t n) {
    if (m == g_self || LeaveAlone(m, path.c_str())) return 0;
    auto sc = g_scanned->begin();
    for (; sc != g_scanned->end(); ++sc)
        if (sc->hooks == hooks && sc->module == m && sc->path == path) break;
    if (sc != g_scanned->end()) {
        if (!sc->probe || *sc->probe != sc->real) return 0;
        g_scanned->erase(sc);  // the same file loaded again: its imports are the real ones again
    }
    const size_t before = g_patched->size();
    bool bound;
    const unsigned count = PatchModule(m, hooks, n, &bound);
    if (!bound) return 0;  // (walked again next time)
    Scanned added = {hooks, m, path, nullptr, nullptr};
    if (g_patched->size() > before) added.probe = (*g_patched)[before].slot, added.real = (*g_patched)[before].original;
    g_scanned->push_back(std::move(added));
    return count;
}

// The loader's notifications of modules loaded and unloaded (Vista and later, and Wine):
// LdrRegisterDllNotification's, declared here (not in the SDK's headers).
struct DllNotificationData {
    ULONG flags;
    const UNICODE_STRING* fullDllName;
    const UNICODE_STRING* baseDllName;
    void* dllBase;
    ULONG sizeOfImage;
};
constexpr ULONG kDllLoaded = 1, kDllUnloaded = 2;
using DllNotificationFn = void(CALLBACK*)(ULONG reason, const DllNotificationData* data, void* context);
using LdrRegisterDllNotificationFn = LONG(NTAPI*)(ULONG flags, DllNotificationFn fn, void* context, void** cookie);
using LdrUnregisterDllNotificationFn = LONG(NTAPI*)(void* cookie);

// A module loaded (its imports bound, its DllMain not run yet) or unloaded (its DllMain run
// for the detach, its memory still there): with the loader lock held, so modules are patched
// as they come, before their own code runs, and an unload waits here while another thread
// patches the module (holding g_hookLock: see PatchGameModules).
void CALLBACK OnDllNotification(ULONG reason, const DllNotificationData* data, void*) {
    if (!data || !data->dllBase) return;
    const HMODULE m = static_cast<HMODULE>(data->dllBase);
    if (reason == kDllLoaded) {
        if (!data->fullDllName || !data->fullDllName->Buffer) return;
        const std::wstring path(data->fullDllName->Buffer, data->fullDllName->Length / sizeof(wchar_t));
        std::vector<Table> tables;
        AcquireSRWLockShared(&g_hookLock);
        if (g_tables) tables = *g_tables;
        ReleaseSRWLockShared(&g_hookLock);
        for (const Table& t : tables) Resolve(t.hooks, t.n);  // (a hooked DLL loaded since)
        unsigned count = 0;
        AcquireSRWLockExclusive(&g_hookLock);
        if (g_modules) {
            MakeLists();
            auto it = std::find_if(g_modules->begin(), g_modules->end(), [&](const auto& e) { return e.first == m; });
            if (it == g_modules->end()) g_modules->emplace_back(m, path);
            else it->second = path;
            for (const Table& t : *g_tables) count += PatchOnce(m, path, t.hooks, t.n);
        }
        ReleaseSRWLockExclusive(&g_hookLock);
        if (count) {
            const size_t slash = path.find_last_of(L'\\');
            DD2D9_LOG("hooks: %ls loaded: %u imports patched", path.c_str() + (slash == std::wstring::npos ? 0 : slash + 1), count);
        }
    } else if (reason == kDllUnloaded) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(m), size = data->sizeOfImage;
        AcquireSRWLockExclusive(&g_hookLock);
        if (g_modules) {
            g_modules->erase(std::remove_if(g_modules->begin(), g_modules->end(), [&](const auto& e) { return e.first == m; }),
                             g_modules->end());
            // Its records go with it: another module may be loaded at its place.
            if (g_scanned)
                g_scanned->erase(std::remove_if(g_scanned->begin(), g_scanned->end(), [&](const Scanned& e) { return e.module == m; }),
                                 g_scanned->end());
            if (g_patched)
                g_patched->erase(std::remove_if(g_patched->begin(), g_patched->end(),
                                                [&](const Patched& p) { return reinterpret_cast<uintptr_t>(p.slot) - base < size; }),
                                 g_patched->end());
        }
        ReleaseSRWLockExclusive(&g_hookLock);
    }
}

// In DllMain: the loader's notifications asked for, and the modules there are now (all bound:
// the loader lock is held, and a module still loading is one dd2d9 is loaded with, whose
// imports the loader bound before running any DllMain) taken as the list to walk.
void TrackModules(const std::vector<std::pair<HMODULE, std::wstring>>& modules) {
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto reg = reinterpret_cast<LdrRegisterDllNotificationFn>(
        reinterpret_cast<void*>(ntdll ? GetProcAddress(ntdll, "LdrRegisterDllNotification") : nullptr));
    if (reg) {
        // The list first: a notification (another thread's load) may come as soon as registered.
        AcquireSRWLockExclusive(&g_hookLock);
        g_modules = new std::vector<std::pair<HMODULE, std::wstring>>(modules);
        ReleaseSRWLockExclusive(&g_hookLock);
        if (reg(0, OnDllNotification, nullptr, &g_notifyCookie) >= 0) return;
        g_notifyCookie = nullptr;
        AcquireSRWLockExclusive(&g_hookLock);
        delete g_modules;
        g_modules = nullptr;
        ReleaseSRWLockExclusive(&g_hookLock);
    }
    DD2D9_LOG("hooks: no load notifications from the loader: modules loaded later are patched at the next DirectDraw object");
}

}  // namespace

unsigned PatchGameModules(const ImportHook* hooks, size_t n, bool inLoaderLock) {
    if (!g_self)
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&PatchGameModules), &g_self);
    Resolve(hooks, n);
    std::vector<std::pair<HMODULE, std::wstring>> modules;
    std::vector<HMODULE> pinned;
    AcquireSRWLockShared(&g_hookLock);
    const bool tracked = g_modules != nullptr;
    ReleaseSRWLockShared(&g_hookLock);
    if (inLoaderLock) {
        ModulesFromLoader(&modules);
        if (!tracked && !g_notifyCookie) TrackModules(modules);
    } else if (!tracked) {
        ModulesFromSnapshot(&modules);
        // Kept loaded while patched (another thread could unload one meanwhile); pinned before
        // the lock is taken, since pinning takes the loader lock. (In DllMain the loader lock,
        // held, keeps every module loaded; with the loader's notifications an unload waits for
        // g_hookLock, see OnDllNotification.)
        size_t kept = 0;
        for (size_t i = 0; i < modules.size(); i++) {
            HMODULE pin = nullptr;
            if (modules[i].first == g_self) continue;  // (left alone anyway)
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(modules[i].first), &pin))
                continue;  // gone since the snapshot
            pinned.push_back(pin);
            if (pin == modules[i].first) modules[kept++] = std::move(modules[i]);
        }
        modules.resize(kept);
    }
    unsigned count = 0;
    AcquireSRWLockExclusive(&g_hookLock);
    MakeLists();
    if (std::none_of(g_tables->begin(), g_tables->end(), [&](const Table& t) { return t.hooks == hooks; }))
        g_tables->push_back({hooks, n});
    // (Outside DllMain the tracked list, which loads change only under g_hookLock, held here.)
    const auto& walk = !inLoaderLock && g_modules ? *g_modules : modules;
    for (const auto& [m, path] : walk) count += PatchOnce(m, path, hooks, n);
    ReleaseSRWLockExclusive(&g_hookLock);
    for (HMODULE m : pinned) FreeLibrary(m);  // (outside the lock: it can run a DLL's detach)
    return count;
}

void UnpatchAll() {
    if (g_notifyCookie) {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto unreg = reinterpret_cast<LdrUnregisterDllNotificationFn>(
            reinterpret_cast<void*>(ntdll ? GetProcAddress(ntdll, "LdrUnregisterDllNotification") : nullptr));
        if (unreg) unreg(g_notifyCookie);
        g_notifyCookie = nullptr;
    }
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
    delete g_modules;
    g_modules = nullptr;
    ReleaseSRWLockExclusive(&g_hookLock);
}

}  // namespace dd2d9
