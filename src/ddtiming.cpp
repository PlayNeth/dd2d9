// The vertical blank and frame pacing: WaitForVerticalBlank, GetScanLine, Flip's pacing,
// fpslimit=.
#include "ddint.h"

namespace dd2d9 {

void DDraw::UpdateRefreshRate() {
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        refresh_ = dm.dmDisplayFrequency;
}

// The vertical blanks, as a QueryPerformanceCounter time one fell on and the period: the
// desktop compositor's (gpu.cpp), the refreshes the presenter's frames are shown at, where
// there is one; otherwise the refresh rate counted from when this object was made. Returns
// whether the clock is the compositor's. WaitForVerticalBlank, GetScanLine and PaceFlip all
// go by it: a game pacing its blits to the primary with WaitForVerticalBlank runs in step
// with the presents, as one pacing with Flip does.
// The compositor's answer is kept for 100 ms (some games poll GetScanLine in a loop, and the
// question goes to another process); the clock it gives runs on regardless.
bool DDraw::VBlankClock(int64_t* vblank, int64_t* period) const {
    static SRWLOCK lock = SRWLOCK_INIT;
    static int64_t askedAt, compVblank, compPeriod;  // (askedAt 0: not asked yet)
    static bool compOk;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    AcquireSRWLockShared(&lock);
    bool fresh = askedAt && now.QuadPart - askedAt < qpcFreq_.QuadPart / 10;
    bool ok = compOk;
    *vblank = compVblank;
    *period = compPeriod;
    ReleaseSRWLockShared(&lock);
    if (!fresh) {
        ok = Gpu::CompositorClock(vblank, period);
        AcquireSRWLockExclusive(&lock);
        askedAt = now.QuadPart | 1;
        compOk = ok;
        compVblank = *vblank;
        compPeriod = *period;
        ReleaseSRWLockExclusive(&lock);
    }
    if (ok) return true;
    *vblank = qpcBase_.QuadPart;
    *period = qpcFreq_.QuadPart / refresh_;
    return false;
}

double DDraw::VBlankPhase() const {
    int64_t vblank, period;
    VBlankClock(&vblank, &period);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t off = ((now.QuadPart - vblank) % period + period) % period;
    return double(off) / double(period);
}

// Sleeps until QueryPerformanceCounter reaches `target`: a high-resolution waitable timer
// (Windows 10 1803 and later) for all but the last half millisecond, which is spun; without
// one, Sleep for all but the last ~1.5 ms. (Spinning the last 2 ms on Sleep(0) cost the game
// its only core with cpus=1.)
void SleepUntilQpc(int64_t target) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    // A timer for this wait only, made when there is a wait long enough for it. A timer kept
    // per thread (thread_local) was never closed: DllMain turns thread notifications off, and
    // with them the thread-exit callbacks that would have run its destructor.
    struct Timer {
        HANDLE h = nullptr;
        ~Timer() {
            if (h) CloseHandle(h);
        }
    } waitable;
    bool tried = false;
    for (;;) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const int64_t left = target - now.QuadPart;
        if (left <= 0) return;
        const double ms = double(left) * 1000.0 / double(freq.QuadPart);
        if (!tried && ms > 0.6) {
            tried = true;
            waitable.h = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        }
        const HANDLE timer = waitable.h;
        if (timer && ms > 0.6) {
            LARGE_INTEGER due;
            due.QuadPart = -int64_t((ms - 0.5) * 10000.0);  // relative, in 100 ns
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
                continue;
            }
        }
        if (!timer && ms > 2.0) Sleep(DWORD(ms - 1.5));
        else YieldProcessor();
    }
}

void DDraw::SleepUntilVBlank(int count) {
    int64_t vblank, period;
    VBlankClock(&vblank, &period);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const int64_t off = ((now.QuadPart - vblank) % period + period) % period;  // past the last vblank
    SleepUntilQpc(now.QuadPart - off + count * period);
}

void DDraw::PaceFlip() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // Under the desktop compositor Present doesn't wait (see gpu.cpp): frames one refresh
    // period apart, on a schedule as fpslimit's. A frame late by less than a period doesn't
    // move the next one: snapping to the vblank after it instead (DwmFlush, as tried) had
    // N.I.C.E. 2 two or three refreshes a frame for good, the game's timing catching up. The
    // schedule leans toward the vblanks, an eighth of the way a frame: the game starts its
    // frame at one and its present is shown at the next, whatever the clocks' drift.
    int64_t vblank, period;
    if (VBlankClock(&vblank, &period)) {
        int64_t due = paceNext_;
        if (!due || now.QuadPart > due + period) due = now.QuadPart;  // the first, or a whole period late
        int64_t off = ((due - vblank) % period + period) % period;  // due is off past a vblank
        if (off > period / 2) off -= period;                          // or -off before the next
        due -= off / 8;
        SleepUntilQpc(due);
        paceNext_ = due + period;
        lastFlip_ = due;
        return;
    }
    // Fallback for when Present() does not block on vblank (vsync off or not
    // honored by the driver/compositor). 98% of a period leaves room for real
    // vsync jitter, so with working vsync this never sleeps.
    const int64_t minGap = qpcFreq_.QuadPart * 98 / (100 * int64_t(refresh_));
    if (lastFlip_ && now.QuadPart < lastFlip_ + minGap) {
        SleepUntilQpc(lastFlip_ + minGap);
        QueryPerformanceCounter(&now);
    }
    lastFlip_ = now.QuadPart;
}

namespace {
// The game asked about or waited for the vblank since its last flip (CountFlip), through any
// of its DirectDraw objects. Set from any thread without a lock: a hint, read at the next flip.
volatile bool g_vblankAsked = false;
}  // namespace

bool DDraw::FlipsWithoutVsync() const {
    if (!flipsWithoutVsync_) return false;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONG64 last = InterlockedCompareExchange64(const_cast<LONG64*>(&lastCountedFlip_), 0, 0);
    return now.QuadPart - last < qpcFreq_.QuadPart / 4;
}

void DDraw::CountFlip(bool noVsync) {
    // A game that passes DDFLIP_NOVSYNC but waits for the vblank itself first
    // (WaitForVerticalBlank, GetScanLine, GetVerticalBlankStatus) flipped in the blank on
    // the hardware, tear-free: its frames stay presented at vblanks. (Judged by the game's
    // calls, not by how fast it flips: while presents wait, they hold its Direct3D frames to
    // the refresh, so it never flips faster than that.)
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    InterlockedExchange64(&lastCountedFlip_, now.QuadPart);
    const bool unsynced = noVsync && !g_vblankAsked;
    g_vblankAsked = false;
    if (unsynced != flipsWithoutVsync_) {
        if (++flipRun_ < 8) return;
        flipRun_ = 0;
        flipsWithoutVsync_ = unsynced;
        DD2D9_LOG(unsynced ? "vsync: the game flips without vsync (DDFLIP_NOVSYNC): presented at once"
                           : "vsync: the game flips with vsync again, or waits for the vblank itself: presented at vblanks");
        return;
    }
    flipRun_ = 0;
}

// fpslimit=N: frames spaced 1/N s apart. A frame later than a whole period starts the
// count again (no burst of frames to catch up).
void LimitFrame() {
    if (g_config.fpsLimit <= 0) return;
    static SRWLOCK lock = SRWLOCK_INIT;
    static int64_t next;
    static LARGE_INTEGER freq;
    AcquireSRWLockExclusive(&lock);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    const int64_t period = freq.QuadPart / g_config.fpsLimit;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!next || now.QuadPart > next + period) next = now.QuadPart;
    SleepUntilQpc(next);
    next += period;
    ReleaseSRWLockExclusive(&lock);
}

HRESULT DDraw::GetMonitorFrequency(DWORD* freq) {
    if (!freq) return DDERR_INVALIDPARAMS;
    *freq = refresh_;
    return DD_OK;
}

HRESULT DDraw::GetScanLine(DWORD* line) {
    if (!line) return DDERR_INVALIDPARAMS;
    DWORD w, h, bpp;
    {
        Guard l;
        DisplayMode(&w, &h, &bpp);
    }
    g_vblankAsked = true;  // (CountFlip)
    const DWORD total = h + h / 20 + 1;  // ~5% of the period is vertical blank
    DWORD pos = DWORD(VBlankPhase() * total);
    *line = pos;  // as drivers do, a line past the visible ones during the blank
    return pos >= h ? DDERR_VERTICALBLANKINPROGRESS : DD_OK;
}

HRESULT DDraw::GetVerticalBlankStatus(BOOL* inVb) {
    if (!inVb) return DDERR_INVALIDPARAMS;
    g_vblankAsked = true;
    *inVb = VBlankPhase() >= 0.95;
    return DD_OK;
}

// As ddraw.dll: the start or the end of the blank (the same moment here), nothing else.
HRESULT DDraw::WaitForVerticalBlank(DWORD flags, HANDLE event) {
    if (event || (flags & DDWAITVB_BLOCKBEGINEVENT)) return DDERR_UNSUPPORTED;
    if (flags != DDWAITVB_BLOCKBEGIN && flags != DDWAITVB_BLOCKEND) return DDERR_INVALIDPARAMS;
    g_vblankAsked = true;
    SleepUntilVBlank(1);
    return DD_OK;
}

}  // namespace dd2d9
