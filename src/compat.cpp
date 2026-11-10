// Compatibility options: what the game is told about the machine, as a PC of its era would
// answer. None of this is DirectDraw; it is here because dd2d9 is the DLL old games load.
//   ram=, cpu=, cpus= (compatsys.cpp): the memory, processor clock and processors.
//   desktopbpp= (compatdisplay.cpp), and the display mode and GDI in a mode of the game's own
//   (compatdisplay.cpp, compatgdi.cpp), with the game's GDI drawing on a windowed primary.
//   heapzero=, fairlocks= (here): the heap and the critical sections as the games knew them.
// The imports are patched in the game's modules (hooks.cpp): from DllMain, before the game's
// own code runs, and in each DLL loaded later as it loads.
#include "compatint.h"

#include <iterator>
#include <vector>

namespace dd2d9 {

namespace {

// Memory from the process heap comes zeroed, as fresh heap memory did on Windows 9x, where
// games were tested: today's heap hands back blocks other code freed (Direct3D's, the
// driver's) with their old contents. Daytona USA Deluxe allocates its texture table with
// HeapAlloc and, before clearing it, releases every "texture" it finds in it: on a block
// full of old data, it called through a stale pointer and crashed as its 3D device came up.
// A compatibility behaviour, not 9x's exact one (there only fresh pages came zeroed): on by
// default (heapzero=on), since no working game depends on garbage; heapzero=off saves the
// clearing for a game that allocates a lot.
using HeapAllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
HeapAllocFn g_heapAlloc;

LPVOID WINAPI HookHeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes) {
    return DD2D9_NEXT(g_heapAlloc)(heap, flags | HEAP_ZERO_MEMORY, bytes);
}

ImportHook kHeapHooks[] = {
    {"kernel32", "HeapAlloc", reinterpret_cast<void*>(HookHeapAlloc), reinterpret_cast<void**>(&g_heapAlloc)},
};

// A critical section a thread releases goes to the thread waiting for it, as on Windows up to
// XP, where the waiter owned it from that moment. Since Vista it is only woken: the releasing
// thread can take the section again before the waiter runs, and one that does so at once
// nearly always does. KOF 99 Evolution's main thread holds its movie renderer's section for the whole
// intro and lets go of it once a frame (Leave, then Enter) for the decoder's thread to copy a
// frame into the surface the game draws: on Windows 10 the decoder got it twice in an
// 18-second intro, or never (as under Wine), and the intro stayed black.
// Here the releasing thread waits, after a release another of the game's threads is waiting
// for, until the section has been taken (fairlocks=on, the default). The game's own imports
// only: a waiter is one that came through HookEnterCriticalSection.
using CriticalSectionFn = void(WINAPI*)(CRITICAL_SECTION*);
CriticalSectionFn g_enterCriticalSection, g_leaveCriticalSection;

// The critical sections threads are waiting for (seldom more than one at a time).
struct Waited {
    CRITICAL_SECTION* volatile cs;
    volatile LONG threads;       // waiting for it (0: the slot is free)
    DWORD ids[8];                // those threads (0: none); g_waitedLock
    volatile LONG quiet;         // a hand-over ran out of time: none until `quietUntil`
    volatile DWORD quietUntil;   // (GetTickCount)
};
Waited g_waited[16];                      // changed under g_waitedLock, read without it
SRWLOCK g_waitedLock = SRWLOCK_INIT;
volatile LONG g_waiters;                  // the threads waiting, over all the sections

// nullptr: every slot is taken by another section, or this one's by eight threads (this wait
// isn't known to the releases).
Waited* AddWaiter(CRITICAL_SECTION* cs, DWORD thread) {
    Waited* found = nullptr;
    AcquireSRWLockExclusive(&g_waitedLock);
    for (Waited& w : g_waited)
        if (w.threads && w.cs == cs) found = &w;
    for (Waited& w : g_waited)
        if (!found && !w.threads) {
            found = &w;
            w.cs = cs;
            w.quiet = 0;
        }
    DWORD* id = nullptr;
    if (found)
        for (DWORD& i : found->ids)
            if (!id && !i) id = &i;
    if (id) {
        *id = thread;
        InterlockedIncrement(&found->threads);
        InterlockedIncrement(&g_waiters);
    }
    ReleaseSRWLockExclusive(&g_waitedLock);
    return id ? found : nullptr;
}

// (g_waitedLock held.) Whether `thread` was waiting for `cs` in `w`; it no longer is.
bool DropWaiter(Waited* w, const CRITICAL_SECTION* cs, DWORD thread) {
    if (!w->threads || w->cs != cs) return false;
    for (DWORD& i : w->ids)
        if (i == thread) {
            i = 0;
            InterlockedDecrement(&w->threads);
            InterlockedDecrement(&g_waiters);
            return true;
        }
    return false;
}

void RemoveWaiter(Waited* w, const CRITICAL_SECTION* cs, DWORD thread) {
    AcquireSRWLockExclusive(&g_waitedLock);
    DropWaiter(w, cs, thread);  // (already dropped if it was taken for dead: see GiveUp)
    ReleaseSRWLockExclusive(&g_waitedLock);
}

// Nobody took the released section in time. Waiters that no longer exist (killed as they
// waited: the real EnterCriticalSection never came back to count them out) are dropped. For
// one that can't run now (suspended, or starved), the releases stop waiting for a quarter
// second, then try again. On XP such a waiter owned the section and the game hung.
void GiveUp(Waited* w, const CRITICAL_SECTION* cs) {
    AcquireSRWLockExclusive(&g_waitedLock);
    if (w->threads && w->cs == cs) {
        for (const DWORD id : w->ids) {
            if (!id) continue;
            const HANDLE thread = OpenThread(SYNCHRONIZE, FALSE, id);
            const bool gone = !thread || WaitForSingleObject(thread, 0) == WAIT_OBJECT_0;
            if (thread) CloseHandle(thread);
            if (gone) DropWaiter(w, cs, id);
        }
        if (w->threads) {
            w->quietUntil = GetTickCount() + 250;
            w->quiet = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_waitedLock);
}

void WINAPI HookEnterCriticalSection(CRITICAL_SECTION* cs) {
    if (TryEnterCriticalSection(cs)) return;
    const DWORD lastError = GetLastError(), thread = GetCurrentThreadId();
    Waited* const w = AddWaiter(cs, thread);
    DD2D9_NEXT(g_enterCriticalSection)(cs);
    if (w) RemoveWaiter(w, cs, thread);
    SetLastError(lastError);
}

void WINAPI HookLeaveCriticalSection(CRITICAL_SECTION* cs) {
    // (Read while the section is still this thread's: the last of its nested entries?)
    const bool released = cs->RecursionCount == 1;
    DD2D9_NEXT(g_leaveCriticalSection)(cs);
    if (!released || !g_waiters) return;
    Waited* slot = nullptr;
    for (Waited& w : g_waited)
        if (w.threads && w.cs == cs) slot = &w;
    if (!slot) return;
    if (slot->quiet) {
        if (LONG(GetTickCount() - slot->quietUntil) < 0) return;
        slot->quiet = 0;
    }
    // The release woke a waiter: this thread stands aside until someone owns the section
    // (yielding for a millisecond, then asleep a few times, for a waiter of a lower priority
    // on the same processor).
    // (`cs` is read after its release, while a thread is counted as waiting for it. A waiter
    // that took it, left it and freed its memory between two of these reads would leave a
    // stale pointer here: only memory given back to the system could fault, and a section
    // freed while another thread uses it is the game's own error.)
    const DWORD lastError = GetLastError();  // (the game's, as LeaveCriticalSection leaves it)
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (int sleeps = 0; slot->threads && slot->cs == cs && !cs->OwningThread;) {
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - start.QuadPart) * 1000 < freq.QuadPart) {
            SwitchToThread();
        } else if (sleeps++ < 3) {
            Sleep(1);
        } else {
            GiveUp(slot, cs);
            break;
        }
    }
    SetLastError(lastError);
}

ImportHook kLockHooks[] = {
    {"kernel32", "EnterCriticalSection", reinterpret_cast<void*>(HookEnterCriticalSection),
     reinterpret_cast<void**>(&g_enterCriticalSection)},
    {"kernel32", "LeaveCriticalSection", reinterpret_cast<void*>(HookLeaveCriticalSection),
     reinterpret_cast<void**>(&g_leaveCriticalSection)},
};

// Functions looked up at run time (games that also ran on Windows 95 find
// GlobalMemoryStatusEx this way): the hooked ones are answered with dd2d9's. Every table
// installed so far counts (hooks.cpp keeps them): the options' here, and the mouse, paint
// and movie translations made when the game sets a display mode (a game that picks
// GetCursorPos or MoveWindow by name got Windows' own, untranslated). A function looked up
// before its table was installed stays Windows'.
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_getProcAddress;
struct HookTable {
    ImportHook* hooks;
    size_t n;
    bool on;
};
HookTable g_tables[7];

FARPROC WINAPI HookGetProcAddress(HMODULE m, LPCSTR name) {
    const FARPROC p = DD2D9_NEXT(g_getProcAddress)(m, name);
    if (p)
        if (void* hook = InstalledHookFor(reinterpret_cast<void*>(p))) return reinterpret_cast<FARPROC>(hook);
    return p;
}

ImportHook kProcHooks[] = {
    {"kernel32", "GetProcAddress", reinterpret_cast<void*>(HookGetProcAddress), reinterpret_cast<void**>(&g_getProcAddress)},
};

}  // namespace

void InstallCompatHooks(bool inLoaderLock) {
    if (inLoaderLock && g_config.cpus) SetAffinity();
    g_tables[0] = {kMemoryHooks, kMemoryHookCount, g_config.ramMB != 0};
    g_tables[1] = {kCpuHooks, kCpuHookCount, g_config.cpuMHz != 0};
    g_tables[2] = {kCpusHooks, kCpusHookCount, g_affinity != 0};
    // (Always: besides desktopbpp's depth, the current mode is the game's while it is shown
    // scaled or in a window, and a change to that mode does nothing; see FixDepth.)
    g_tables[3] = {kDepthHooks, kDepthHookCount, true};
    g_tables[4] = {kWindowHooks, kWindowHookCount, true};
    g_tables[5] = {kHeapHooks, sizeof(kHeapHooks) / sizeof(kHeapHooks[0]), g_config.heapZero};
    g_tables[6] = {kLockHooks, sizeof(kLockHooks) / sizeof(kLockHooks[0]), g_config.fairLocks};
    // The tables switched on, and GetProcAddress with them, patched together (the modules
    // listed once): made once, the setting fixed for the process.
    static std::vector<ImportHook> all;
    static bool made;
    if (!made) {
        made = true;
        for (const HookTable& t : g_tables)
            if (t.on) all.insert(all.end(), t.hooks, t.hooks + t.n);
        if (!all.empty()) {
            all.insert(all.end(), std::begin(kProcHooks), std::end(kProcHooks));
            // GetProcAddress hands out dd2d9's functions, which a game may keep calling after
            // a FreeLibrary of ddraw.dll: dd2d9 stays loaded.
            HMODULE self;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCWSTR>(&InstallCompatHooks), &self);
        }
    }
    if (all.empty()) return;
    if (const unsigned n = PatchGameModules(all.data(), all.size(), inLoaderLock))
        DD2D9_LOG("compat: %u imports patched (ram=%d cpu=%d cpus=%d desktopbpp=%d)", n, g_config.ramMB, g_config.cpuMHz,
                  g_config.cpus, g_config.desktopBpp);
}

}  // namespace dd2d9
