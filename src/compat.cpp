// Compatibility options: what the game is told about the machine, as a PC of its era would
// answer. None of this is DirectDraw; it is here because dd2d9 is the DLL old games load.
//   ram=, cpu=, cpus= (compatsys.cpp): the memory, processor clock and processors.
//   desktopbpp= (compatdisplay.cpp), and the display mode and GDI in a mode of the game's own
//   (compatdisplay.cpp, compatgdi.cpp), with the game's GDI drawing on a windowed primary.
//   heapzero=, fairlocks=, safetimers=, joypoll=, selfsuspend= (here): the heap, the critical
//   sections, the multimedia timers, the joystick and sound callbacks as the games knew them.
// The imports are patched in the game's modules (hooks.cpp): from DllMain, before the game's
// own code runs, and in each DLL loaded later as it loads.
#include "compatint.h"

#include <mmsystem.h>

#include <algorithm>
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

// A multimedia timer's callback (timeSetEvent) is called with the registers it must keep
// (EBX, ESI, EDI, EBP), the stack and the direction flag put back after it returns
// (safetimers=on, the default). Windows up to XP called it through a helper that saved them
// itself, so a callback that didn't keep them did no harm. Windows 10 and 11 call it straight
// from their timer thread, which then goes on using ESI. Miles Sound System 3's timer callback
// (wail32.dll) leaves an argument on the stack for each handler of the game's it runs, as
// Mega Man X's (Watcom's calling convention) doesn't take it off, and so pops the wrong
// values into EBX, ESI and EDI: winmm crashed reading 0x20 a moment after the game started.
using TimeSetEventFn = MMRESULT(WINAPI*)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
using TimeKillEventFn = MMRESULT(WINAPI*)(UINT);
TimeSetEventFn g_timeSetEvent;
TimeKillEventFn g_timeKillEvent;

// The game's callback and its data, for each timer dd2d9 calls through. Handed out in turn
// (a freed slot comes back last): a callback still running as its timer is killed reads
// what it was given, not the next timer's. `owner` is the slot's use (a number of its own,
// 0: free), so that only that use frees it: a one-shot timer killed while its callback runs
// is freed once, not again after another timer has taken the slot.
struct TimerSlot {
    LPTIMECALLBACK proc;
    DWORD_PTR user;
    UINT flags;
    volatile UINT id;      // timeSetEvent's (0: not yet known)
    volatile LONG owner;
};
TimerSlot g_timerSlots[256];
volatile LONG g_timerNext, g_timerUses;

TimerSlot* TakeTimerSlot(LONG* use) {
    LONG u;
    do u = InterlockedIncrement(&g_timerUses);
    while (!u);
    for (int tries = 0; tries < 256; tries++) {
        TimerSlot& s = g_timerSlots[static_cast<unsigned>(InterlockedIncrement(&g_timerNext)) % 256];
        if (!InterlockedCompareExchange(&s.owner, u, 0)) {
            *use = u;
            return &s;
        }
    }
    static volatile LONG logged;
    if (!InterlockedExchange(&logged, 1)) DD2D9_LOG("winmm: 256 timers at once: the next ones are called as Windows calls them");
    return nullptr;
}

void FreeTimerSlot(TimerSlot& s, LONG use) {
    if (s.owner != use) return;
    s.id = 0;  // (no timeKillEvent matches a slot by an id it no longer has)
    InterlockedCompareExchange(&s.owner, 0, use);
}

}  // namespace
}  // namespace dd2d9

// Calls `proc(id, msg, user, dw1, dw2)` (stdcall) and puts back what it must have kept: EBX,
// ESI, EDI, EBP and the stack (a callback taken as cdecl leaves its arguments on it), and
// the direction flag cleared. The frame is found again through a TLS slot
// (dd2d9_timerTls), EBP being among what the callback may have lost; the slot's value before
// is put back after, so that a call made inside the callback leaves the outer one's.
extern "C" {
DWORD dd2d9_timerTls = TLS_OUT_OF_INDEXES;
DWORD dd2d9_timerOut[4];  // ESI, EDI, EBX, EBP as the last callback left them (for the log)
}
extern "C" void __cdecl dd2d9_CallTimeProc(LPTIMECALLBACK proc, UINT id, UINT msg, DWORD_PTR user, DWORD_PTR dw1,
                                           DWORD_PTR dw2);
asm(R"(
    .text
    .globl _dd2d9_CallTimeProc
    .def _dd2d9_CallTimeProc; .scl 2; .type 32; .endef
_dd2d9_CallTimeProc:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl _dd2d9_timerTls
    call *__imp__TlsGetValue@4
    pushl %eax                      # -16(%ebp): the slot's value before, put back after
    subl $32, %esp                  # (room for a callback that takes more than its arguments off)
    pushl %ebp
    pushl _dd2d9_timerTls
    call *__imp__TlsSetValue@8
    testl %eax, %eax
    jz 2f
    pushl 28(%ebp)
    pushl 24(%ebp)
    pushl 20(%ebp)
    pushl 16(%ebp)
    pushl 12(%ebp)
    call *8(%ebp)
    movl %esi, _dd2d9_timerOut
    movl %edi, _dd2d9_timerOut+4
    movl %ebx, _dd2d9_timerOut+8
    movl %ebp, _dd2d9_timerOut+12
    cld
    pushl _dd2d9_timerTls
    call *__imp__TlsGetValue@4
    movl %eax, %ebp
1:
    pushl -16(%ebp)
    pushl _dd2d9_timerTls
    call *__imp__TlsSetValue@8
    leal -12(%ebp), %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
2:                                  # (no TLS for this thread: the frame found from the stack,
    pushl 28(%ebp)                  # which holds for a stdcall callback)
    pushl 24(%ebp)
    pushl 20(%ebp)
    pushl 16(%ebp)
    pushl 12(%ebp)
    call *8(%ebp)
    movl %esi, _dd2d9_timerOut
    movl %edi, _dd2d9_timerOut+4
    movl %ebx, _dd2d9_timerOut+8
    movl %ebp, _dd2d9_timerOut+12
    cld
    leal 48(%esp), %ebp
    jmp 1b
)");

namespace dd2d9 {
namespace {

void CALLBACK SafeTimeProc(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR dw1, DWORD_PTR dw2) {
    TimerSlot& s = *reinterpret_cast<TimerSlot*>(user);
    const LONG use = s.owner;
    const LPTIMECALLBACK proc = s.proc;
    const DWORD_PTR gameUser = s.user;
    const bool once = !(s.flags & TIME_PERIODIC);
    // The first callback logged, with what it left in the registers it should have kept.
    static volatile LONG calls;
    const LONG call = InterlockedIncrement(&calls);
    if (call == 1) DD2D9_LOG("winmm: first timer callback (%p, timer %u)", reinterpret_cast<void*>(proc), id);
    const DWORD start = GetTickCount();
    dd2d9_CallTimeProc(proc, id, msg, gameUser, dw1, dw2);
    if (call == 1)
        DD2D9_LOG("winmm: it came back after %lu ms, leaving ESI 0x%lx EDI 0x%lx EBX 0x%lx EBP 0x%lx (put back)",
                  GetTickCount() - start, dd2d9_timerOut[0], dd2d9_timerOut[1], dd2d9_timerOut[2], dd2d9_timerOut[3]);
    // (And how many there were, every 10 s: a timer that stopped shows as no more lines.)
    static volatile LONG lastLog;
    const LONG now = static_cast<LONG>(GetTickCount());
    if (now - lastLog >= 10000) {
        lastLog = now;
        DD2D9_TRACE("winmm: %ld timer callbacks so far", call);
    }
    if (once) FreeTimerSlot(s, use);  // (its timer is gone)
}

MMRESULT WINAPI HookTimeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK proc, DWORD_PTR user, UINT flags) {
    // Events set or pulsed instead of a callback (TIME_CALLBACK_EVENT_SET/PULSE): nothing to keep.
    LONG use = 0;
    TimerSlot* s =
        proc && !(flags & (TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE)) ? TakeTimerSlot(&use) : nullptr;
    if (!s) return DD2D9_NEXT(g_timeSetEvent)(delay, resolution, proc, user, flags);
    s->proc = proc;
    s->user = user;
    s->flags = flags;
    s->id = 0;
    static volatile LONG logged;
    if (!InterlockedExchange(&logged, 1))
        DD2D9_LOG("winmm: timeSetEvent callbacks are called with their registers kept (safetimers)");
    const MMRESULT id = DD2D9_NEXT(g_timeSetEvent)(delay, resolution, SafeTimeProc, reinterpret_cast<DWORD_PTR>(s), flags);
    DD2D9_TRACE("winmm: timeSetEvent(%u ms, resolution %u, %p, flags 0x%x) -> timer %u", delay, resolution,
              reinterpret_cast<void*>(proc), flags, id);
    if (!id) FreeTimerSlot(*s, use);
    else if (s->owner == use) s->id = id;  // (a one-shot timer may have come and gone already)
    return id;
}

MMRESULT WINAPI HookTimeKillEvent(UINT id) {
    const MMRESULT r = DD2D9_NEXT(g_timeKillEvent)(id);
    DD2D9_TRACE("winmm: timeKillEvent(%u) -> %u", id, r);
    if (r == TIMERR_NOERROR)
        for (TimerSlot& s : g_timerSlots) {
            const LONG use = s.owner;
            if (use && s.id && s.id == id) {
                FreeTimerSlot(s, use);
                break;
            }
        }
    return r;
}

// The game's joystick reads (joyGetPosEx, joyGetPos, for joysticks 0-15) are answered from
// what a thread of dd2d9's last read (joypoll=on, the default), not inside winmm. Miles Sound
// System 3's timer (wail32.dll, 1995) suspends the thread that started it (the game's main
// thread) while it runs the game's timer handlers, as a DOS interrupt would have: Mega Man X
// reads the joystick from its handler, and from its main thread too. A joystick read takes
// locks (since Windows 8, and under Wine), and loads modules the first time: should the main
// thread be suspended while it holds one, the handler waits for it for good and the game
// stands still.
// dd2d9's thread reads each joystick the game asks for, with each set of flags it asks with,
// every 4 ms (every 100 ms while it isn't plugged in), for as long as the game went on asking
// in the last 2 s; it sleeps while no one asks, and a read it hasn't answered lately wakes
// it. Until it has answered, a read outside a timer or sound callback asks winmm itself; one
// inside finds no joystick for now.
using JoyGetPosExFn = MMRESULT(WINAPI*)(UINT, LPJOYINFOEX);
using JoyGetPosFn = MMRESULT(WINAPI*)(UINT, LPJOYINFO);
JoyGetPosExFn g_joyGetPosEx;
JoyGetPosFn g_joyGetPos;

// Milliseconds, from the performance counter: GetTickCount moves every 15.6 ms or so, which
// would have the poller read every 16 ms, not 4. (The frequency is read with the hooks.)
LONGLONG g_joyFreq = 1;
DWORD JoyMs() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<DWORD>(c.QuadPart * 1000 / g_joyFreq);
}

// One joystick read with one set of flags (a game may ask with more than one: raw or
// calibrated, with or without the POV), the poller's answer kept twice over: it writes the
// copy the readers aren't reading and then makes it the current one, so that a reader always
// finds a whole answer, even when it runs while the poller is in the middle of one (with
// cpus=1 the game's timer thread can stop it there).
struct JoyRead {
    volatile LONG key;          // id + 1 | flags << 8 (0: free)
    volatile LONG asked;        // JoyMs of the game's last read (| 1)
    volatile LONG answers;      // how many the poller wrote (0: none yet)
    volatile LONG current;      // the copy readers take (0 or 1)
    volatile DWORD next;        // JoyMs of the poller's next read
    struct Answer {
        volatile LONG writes;   // odd while the poller writes it
        MMRESULT result;
        JOYINFOEX info;
    } copy[2];
};
JoyRead g_joyReads[64];
HANDLE g_joyWake;               // a read the poller hasn't answered lately (auto-reset)

DWORD WINAPI JoyPoller(void*) {
    bool logged = false;
    bool fineTimer = false;  // timeBeginPeriod(1) while polling: a 4 ms wait, not 15.6
    for (;;) {
        const DWORD now = JoyMs();
        DWORD wait = INFINITE;  // (no one asking: asleep until someone does)
        for (JoyRead& r : g_joyReads) {
            const LONG key = r.key;
            // (Signed: a read on another thread may have stored a time later than `now`.)
            if (!key || static_cast<LONG>(now - static_cast<DWORD>(r.asked)) > 2000) continue;
            if (r.answers && static_cast<LONG>(r.next - now) > 0) {
                wait = std::min<DWORD>(wait, r.next - now);
                continue;
            }
            if (!logged) {
                logged = true;
                DD2D9_LOG("winmm: the game's joystick reads are answered from dd2d9's own (joypoll)");
            }
            JOYINFOEX info = {};
            info.dwSize = sizeof(info);
            info.dwFlags = static_cast<DWORD>(key) >> 8;
            const MMRESULT result = g_joyGetPosEx((key & 0xFF) - 1, &info);
            const LONG c = !r.current;
            JoyRead::Answer& a = r.copy[c];
            InterlockedIncrement(&a.writes);
            a.result = result;
            a.info = info;
            InterlockedIncrement(&a.writes);
            InterlockedExchange(&r.current, c);
            InterlockedIncrement(&r.answers);
            const DWORD every = result == JOYERR_NOERROR ? 4 : 100;
            r.next = JoyMs() + every;
            wait = std::min<DWORD>(wait, every);
        }
        if (fineTimer != (wait != INFINITE)) {
            fineTimer = wait != INFINITE;
            if (fineTimer) timeBeginPeriod(1);
            else timeEndPeriod(1);
        }
        WaitForSingleObject(g_joyWake, wait);
    }
}

// The poller's last answer for joystick `id` read with `flags`; with none yet, winmm's own
// outside a callback, none inside one. Never waits, takes no lock and allocates nothing (the
// game's main thread may be suspended holding the heap's): the poller is started with the
// hooks.
MMRESULT CachedJoy(UINT id, DWORD flags, JOYINFOEX* out) {
    const LONG key = static_cast<LONG>((id + 1) | (flags << 8));
    JoyRead* r = nullptr;
    for (JoyRead& e : g_joyReads)
        if (e.key == key) {
            r = &e;
            break;
        }
    if (!r)
        for (JoyRead& e : g_joyReads)
            if (!InterlockedCompareExchange(&e.key, key, 0) || e.key == key) {
                r = &e;
                break;
            }
    if (!r) {  // (64 sets of flags and joysticks at once: one taken over, its last answer
               // possibly read once more for the new one, as the poller may be writing it)
        static volatile LONG next;
        r = &g_joyReads[static_cast<unsigned>(InterlockedIncrement(&next)) % 64];
        InterlockedExchange(&r->answers, 0);
        InterlockedExchange(&r->key, key);
    }
    const DWORD now = JoyMs();
    const DWORD asked = static_cast<DWORD>(InterlockedExchange(&r->asked, static_cast<LONG>(now | 1)));
    const bool stale = static_cast<LONG>(now - asked) > 2000;  // (the poller stopped asking)
    if (!r->answers || stale) {
        r->next = now;
        SetEvent(g_joyWake);
        // The first read outside a timer or sound callback asks winmm itself (a game that
        // looks for a joystick once must find it; the poller's thread may not run yet), as
        // does one after 2 s without any, whose last answer may be long gone. Only the
        // callbacks dd2d9 calls through are known (safetimers=on, selfsuspend=off): a read in
        // another is taken for one outside.
        const bool callback = dd2d9_timerTls != TLS_OUT_OF_INDEXES && TlsGetValue(dd2d9_timerTls);
        if (!callback) {
            JOYINFOEX info = {};
            info.dwSize = sizeof(info);
            info.dwFlags = flags;
            const MMRESULT result = g_joyGetPosEx(id, &info);
            if (out) {
                info.dwSize = out->dwSize;
                info.dwFlags = out->dwFlags;
                *out = info;
            }
            return result;
        }
    }
    MMRESULT result = JOYERR_UNPLUGGED;  // (no answer yet: none)
    JOYINFOEX info = {};
    for (int tries = 0; tries < 8 && r->answers; tries++) {
        const JoyRead::Answer& a = r->copy[r->current];
        const LONG before = a.writes;
        asm volatile("" ::: "memory");
        result = a.result;
        info = a.info;
        asm volatile("" ::: "memory");
        if (!(before & 1) && a.writes == before) break;
        result = JOYERR_UNPLUGGED;
    }
    if (out) {
        info.dwSize = out->dwSize;
        info.dwFlags = out->dwFlags;
        *out = info;
    }
    return result;
}

// Passed on unchanged: joysticks past 15, the calibration flags (above the 24 bits the key
// keeps), and a call from another hooker's patch over dd2d9's (see NextFunction), which may
// be the poller's own read coming back.
MMRESULT WINAPI HookJoyGetPosEx(UINT id, LPJOYINFOEX info) {
    const JoyGetPosExFn next = DD2D9_NEXT(g_joyGetPosEx);
    if (next != g_joyGetPosEx || id >= 16 || !info || info->dwSize != sizeof(JOYINFOEX) || (info->dwFlags >> 24))
        return next(id, info);
    return CachedJoy(id, info->dwFlags, info);
}

MMRESULT WINAPI HookJoyGetPos(UINT id, LPJOYINFO info) {
    const JoyGetPosFn next = DD2D9_NEXT(g_joyGetPos);
    if (next != g_joyGetPos || id >= 16 || !info) return next(id, info);
    JOYINFOEX ex = {};
    ex.dwSize = sizeof(ex);
    ex.dwFlags = JOY_RETURNX | JOY_RETURNY | JOY_RETURNZ | JOY_RETURNBUTTONS;
    const MMRESULT r = CachedJoy(id, ex.dwFlags, &ex);
    if (r == JOYERR_NOERROR) {
        info->wXpos = ex.dwXpos;
        info->wYpos = ex.dwYpos;
        info->wZpos = ex.dwZpos;
        info->wButtons = ex.dwButtons & 0xF;
    }
    return r;
}

// A sound callback that suspends its own thread carries on instead (selfsuspend=off, the
// default). Miles Sound System 3's sound callback starts by suspending the game's main thread,
// as an interrupt would have stopped it, and resumes it at the end. Up to XP the callback came
// from the sound driver's own thread. Since Vista, winmm calls it on the thread that opened the
// device or queued the sound, from inside waveOutOpen: there it suspended itself, and nothing
// ever resumed it (Mega Man X stood still on a black screen, its timer still running). The
// callback's ResumeThread then finds the thread running and does nothing, as Windows' does.
// Only inside a sound callback (waveOutOpen's and midiOutOpen's CALLBACK_FUNCTION, called
// through SafeSoundProc, which counts them in a TLS slot): a thread that suspends itself
// elsewhere, for another to resume it, is suspended.
using SuspendThreadFn = DWORD(WINAPI*)(HANDLE);
using GetThreadIdFn = DWORD(WINAPI*)(HANDLE);
using WaveOutOpenFn = MMRESULT(WINAPI*)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
using MidiOutOpenFn = MMRESULT(WINAPI*)(LPHMIDIOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD);
SuspendThreadFn g_suspendThread;
GetThreadIdFn g_getThreadId;  // (Vista on: XP calls the callback from the driver's thread)
WaveOutOpenFn g_waveOutOpen;
MidiOutOpenFn g_midiOutOpen;
DWORD g_soundTls = TLS_OUT_OF_INDEXES;  // the sound callbacks this thread is in

// The game's callback and its data, for each device opened with one (freed with its close
// message, or when the open fails).
struct SoundSlot {
    LPTIMECALLBACK proc;  // (the same arguments as a timer callback's)
    DWORD_PTR user;
    volatile LONG used;
};
SoundSlot g_soundSlots[64];

void CALLBACK SafeSoundProc(UINT device, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR p2) {
    SoundSlot& s = *reinterpret_cast<SoundSlot*>(user);
    const LPTIMECALLBACK proc = s.proc;
    const DWORD_PTR gameUser = s.user;
    const DWORD_PTR depth = reinterpret_cast<DWORD_PTR>(TlsGetValue(g_soundTls));
    TlsSetValue(g_soundTls, reinterpret_cast<void*>(depth + 1));
    dd2d9_CallTimeProc(proc, device, msg, gameUser, p1, p2);
    TlsSetValue(g_soundTls, reinterpret_cast<void*>(depth));
    if (msg == WOM_CLOSE || msg == MOM_CLOSE) InterlockedExchange(&s.used, 0);
}

SoundSlot* TakeSoundSlot(DWORD_PTR callback, DWORD_PTR user, DWORD flags) {
    if ((flags & CALLBACK_TYPEMASK) != CALLBACK_FUNCTION || !callback) return nullptr;
    for (SoundSlot& s : g_soundSlots)
        if (!InterlockedCompareExchange(&s.used, 1, 0)) {
            s.proc = reinterpret_cast<LPTIMECALLBACK>(callback);
            s.user = user;
            return &s;
        }
    return nullptr;  // (64 devices at once: the next one is called as Windows calls it)
}

MMRESULT WINAPI HookWaveOutOpen(LPHWAVEOUT out, UINT device, LPCWAVEFORMATEX format, DWORD_PTR callback,
                                DWORD_PTR user, DWORD flags) {
    SoundSlot* s = flags & WAVE_FORMAT_QUERY ? nullptr : TakeSoundSlot(callback, user, flags);
    if (!s) return DD2D9_NEXT(g_waveOutOpen)(out, device, format, callback, user, flags);
    const MMRESULT r = DD2D9_NEXT(g_waveOutOpen)(out, device, format, reinterpret_cast<DWORD_PTR>(SafeSoundProc),
                                                 reinterpret_cast<DWORD_PTR>(s), flags);
    if (r != MMSYSERR_NOERROR) InterlockedExchange(&s->used, 0);
    return r;
}

MMRESULT WINAPI HookMidiOutOpen(LPHMIDIOUT out, UINT device, DWORD_PTR callback, DWORD_PTR user, DWORD flags) {
    SoundSlot* s = TakeSoundSlot(callback, user, flags);
    if (!s) return DD2D9_NEXT(g_midiOutOpen)(out, device, callback, user, flags);
    const MMRESULT r = DD2D9_NEXT(g_midiOutOpen)(out, device, reinterpret_cast<DWORD_PTR>(SafeSoundProc),
                                                 reinterpret_cast<DWORD_PTR>(s), flags);
    if (r != MMSYSERR_NOERROR) InterlockedExchange(&s->used, 0);
    return r;
}

DWORD WINAPI HookSuspendThread(HANDLE thread) {
    if (thread && TlsGetValue(g_soundTls) && g_getThreadId(thread) == GetCurrentThreadId()) {
        static volatile LONG logged;
        if (!InterlockedExchange(&logged, 1))
            DD2D9_LOG("compat: a sound callback suspended its own thread: it carries on (selfsuspend=off)");
        return 0;  // (its suspend count before: none)
    }
    return DD2D9_NEXT(g_suspendThread)(thread);
}

ImportHook kSuspendHooks[] = {
    {"kernel32", "SuspendThread", reinterpret_cast<void*>(HookSuspendThread), reinterpret_cast<void**>(&g_suspendThread)},
    {"winmm", "waveOutOpen", reinterpret_cast<void*>(HookWaveOutOpen), reinterpret_cast<void**>(&g_waveOutOpen)},
    {"winmm", "midiOutOpen", reinterpret_cast<void*>(HookMidiOutOpen), reinterpret_cast<void**>(&g_midiOutOpen)},
};

ImportHook kJoyHooks[] = {
    {"winmm", "joyGetPosEx", reinterpret_cast<void*>(HookJoyGetPosEx), reinterpret_cast<void**>(&g_joyGetPosEx)},
    {"winmm", "joyGetPos", reinterpret_cast<void*>(HookJoyGetPos), reinterpret_cast<void**>(&g_joyGetPos)},
};

ImportHook kTimerHooks[] = {
    {"winmm", "timeSetEvent", reinterpret_cast<void*>(HookTimeSetEvent), reinterpret_cast<void**>(&g_timeSetEvent)},
    {"winmm", "timeKillEvent", reinterpret_cast<void*>(HookTimeKillEvent), reinterpret_cast<void**>(&g_timeKillEvent)},
};

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
HookTable g_tables[10];

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
    // (Without a TLS slot for dd2d9_CallTimeProc, the timers are left as they are.)
    // (Its TLS slot is the sound callbacks' too, selfsuspend, and joypoll reads it to tell a
    // read inside a callback.)
    if ((g_config.safeTimers || !g_config.selfSuspend || g_config.joyPoll) && dd2d9_timerTls == TLS_OUT_OF_INDEXES)
        dd2d9_timerTls = TlsAlloc();
    g_tables[7] = {kTimerHooks, sizeof(kTimerHooks) / sizeof(kTimerHooks[0]),
                   g_config.safeTimers && dd2d9_timerTls != TLS_OUT_OF_INDEXES};
    // (joypoll: its thread started here, so that a read on a thread of the game's takes no
    // lock; it sleeps until the game reads a joystick.)
    if (g_config.joyPoll && !g_joyWake) {
        LARGE_INTEGER f;
        if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) g_joyFreq = f.QuadPart;
        g_joyWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (HANDLE t = g_joyWake ? CreateThread(nullptr, 0, JoyPoller, nullptr, 0, nullptr) : nullptr) {
            SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
            CloseHandle(t);
        } else if (g_joyWake) {
            CloseHandle(g_joyWake);
            g_joyWake = nullptr;
        }
    }
    g_tables[8] = {kJoyHooks, sizeof(kJoyHooks) / sizeof(kJoyHooks[0]), g_joyWake != nullptr};
    if (!g_config.selfSuspend && !g_getThreadId) {
        g_getThreadId = reinterpret_cast<GetThreadIdFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadId")));
        if (g_getThreadId && g_soundTls == TLS_OUT_OF_INDEXES) g_soundTls = TlsAlloc();
    }
    g_tables[9] = {kSuspendHooks, sizeof(kSuspendHooks) / sizeof(kSuspendHooks[0]),
                   !g_config.selfSuspend && g_getThreadId && g_soundTls != TLS_OUT_OF_INDEXES &&
                       dd2d9_timerTls != TLS_OUT_OF_INDEXES};
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
