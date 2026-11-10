// The global locks, work deferred until g_lock is released, writable interface tables.
#include "common.h"

#include <vector>

namespace dd2d9 {

CRITICAL_SECTION g_lock;
CRITICAL_SECTION g_displayLock;

void EnterDisplayLock() {
    while (!TryEnterCriticalSection(&g_displayLock)) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_SENDMESSAGE);
        MSG m;
        PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);  // runs the sent ones
    }
}

bool WaitPumping(HANDLE h, DWORD timeoutMs, HANDLE other) {
    const HANDLE hs[2] = {h, other};
    const DWORD n = other ? 2 : 1;
    const DWORD start = GetTickCount();
    for (;;) {
        const DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeoutMs) return WaitForMultipleObjects(n, hs, FALSE, 0) < WAIT_OBJECT_0 + n;
        DWORD r = MsgWaitForMultipleObjects(n, hs, FALSE, timeoutMs - elapsed, QS_SENDMESSAGE);
        if (r < WAIT_OBJECT_0 + n) return true;
        if (r != WAIT_OBJECT_0 + n) return false;
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);  // dispatches the sent messages
    }
}

volatile LONG g_deferredCount = 0;

namespace {
struct Deferred {
    void (*fn)(void*);
    void* arg;
};
thread_local std::vector<Deferred>* t_deferred;
}  // namespace

void RunWhenUnlocked(void (*fn)(void*), void* arg) {
    if (!LockHeldByMe()) {
        fn(arg);
        return;
    }
    if (!t_deferred) t_deferred = new std::vector<Deferred>();
    t_deferred->push_back({fn, arg});
    InterlockedIncrement(&g_deferredCount);
}

void RunDeferred() {
    while (t_deferred && !t_deferred->empty() && !LockHeldByMe()) {
        Deferred d = t_deferred->front();
        t_deferred->erase(t_deferred->begin());
        InterlockedDecrement(&g_deferredCount);
        d.fn(d.arg);
    }
    if (t_deferred && t_deferred->empty()) {  // (a thread's list goes when it has nothing left: none leaks with the thread)
        delete t_deferred;
        t_deferred = nullptr;
    }
}

void MakeVtblWritable(const void* obj) {
    void* const vtbl = *static_cast<void* const*>(obj);
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(vtbl, &mi, sizeof(mi))) return;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (mi.Protect & writable) return;
    DWORD old;
    // Up to 64 methods: the largest interface here (IDirectDrawSurface7) has 49.
    if (!VirtualProtect(vtbl, 64 * sizeof(void*), PAGE_READWRITE, &old))
        DD2D9_LOG("could not make an interface table writable (%lu)", GetLastError());
}

}  // namespace dd2d9
