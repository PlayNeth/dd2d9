// DD2D9 test program.
//   ddtest unit     - pixel-exact checks of blits, keys, conversion, GDI, flipping, surface lifetimes
//   ddtest fs8      - exclusive 640x480x8 flip-chain animation with palette and GDI text
//   ddtest pattern  - a still test image in exclusive 640x480x8 (or "pattern <w> <h>") for comparing filters
//   ddtest overlay  - overlay surfaces laid over an exclusive-mode primary (read back from the screen)
//   ddtest win      - windowed (DDSCL_NORMAL) 32-bit desktop, blits to the primary through a clipper
//   ddtest fuzz     - random same-format blits (keys, ranges where taken, mirroring, stretching, overlap)
//                     compared with a reference implementation, for 8/16/32-bit formats
// Results are printed to ddtest.txt next to the executable.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <mmsystem.h>
#include <digitalv.h>
#include <vfw.h>
#include <d3d.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// A timer callback that keeps none of the registers it must (ESI, EDI, EBX, EBP), sets the
// direction flag and returns as cdecl (its arguments left on the stack).
extern "C" {
volatile LONG g_badTicks = 0;
void CALLBACK BadTimeProc(UINT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);
}
asm(R"(
    .text
    .globl _BadTimeProc@20
_BadTimeProc@20:
    lock incl _g_badTicks
    movl $0x10, %esi
    movl $0x11, %edi
    movl $0x12, %ebx
    movl $0x14, %ebp
    std
    ret
)");

static FILE* g_out;
static int g_fail;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (cond) {                                         \
            fprintf(g_out, "PASS ");                        \
        } else {                                            \
            fprintf(g_out, "FAIL ");                        \
            g_fail++;                                       \
        }                                                   \
        fprintf(g_out, __VA_ARGS__);                        \
        fputc('\n', g_out);                                 \
        fflush(g_out);                                      \
    } while (0)

static int g_userMsgs, g_screensaverMsgs;

static LPARAM g_lastMouseMove = -1;
static LPARAM g_lastSize = -1;  // WM_SIZE (restored or maximized)
static RECT g_lastPaint = {-1, -1, -1, -1};  // WM_PAINT's rectangle (BeginPaint, as Road Rash asks)

// A game that keeps its window the mode's size from its own window procedure: any other size
// is set back, at once, from WM_SIZE (Hydro Thunder, as its logs show it: 0 = off).
static int g_keepW, g_keepH;
// What the game's window procedure reads of its screen while handling WM_SIZE (restored), and
// of the screen's width: Heroes of the Three Kingdoms 3 centres its movie by them.
static RECT g_sizeClient = {-1, -1, -1, -1};
static int g_sizeScreenW = -1;

// WM_ACTIVATEAPP (TRUE) heard, and whether the window was in front and not minimized then, as
// Mega Man X checks.
static int g_activateTrue;
static bool g_activeSeen;
static LONG g_styleChanges;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ACTIVATEAPP && w) {
        g_activateTrue++;
        g_activeSeen = GetForegroundWindow() == h && !IsIconic(h);
    }
    if (m == WM_SIZE && g_keepW && (LOWORD(l) != g_keepW || HIWORD(l) != g_keepH))
        SetWindowPos(h, nullptr, 0, 0, g_keepW, g_keepH, SWP_NOZORDER | SWP_NOACTIVATE);
    if (m == WM_USER + 1) g_userMsgs++;
    if (m == WM_STYLECHANGED) g_styleChanges++;
    if (m == WM_MOUSEMOVE) g_lastMouseMove = l;
    if (m == WM_SIZE && (w == SIZE_RESTORED || w == SIZE_MAXIMIZED)) {
        g_lastSize = l;
        GetClientRect(h, &g_sizeClient);
        g_sizeScreenW = GetSystemMetrics(SM_CXSCREEN);
    }
    if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_SCREENSAVE) {
        g_screensaverMsgs++;
        return 0;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        g_lastPaint = ps.rcPaint;
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcA(h, m, w, l);
}

// A DLL's own function, read from its export table: Windows' real one, where the test
// compares with it (GetProcAddress hands a game dd2d9's hook for a hooked function).
static void* RealExport(HMODULE m, const char* name) {
    auto* b = reinterpret_cast<BYTE*>(m);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(b + reinterpret_cast<IMAGE_DOS_HEADER*>(b)->e_lfanew);
    const DWORD dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    if (!dir) return nullptr;
    auto* ed = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(b + dir);
    auto* names = reinterpret_cast<DWORD*>(b + ed->AddressOfNames);
    auto* ords = reinterpret_cast<WORD*>(b + ed->AddressOfNameOrdinals);
    auto* funcs = reinterpret_cast<DWORD*>(b + ed->AddressOfFunctions);
    for (DWORD i = 0; i < ed->NumberOfNames; i++)
        if (strcmp(reinterpret_cast<char*>(b + names[i]), name) == 0) return b + funcs[ords[i]];
    return nullptr;
}

static HWND MakeWindow(DWORD style, int w, int h) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ddtest";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "ddtest", "DD2D9 test", style, 0, 0, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return hwnd;
}

static void Pump() {
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static IDirectDrawSurface7* MakeSurface(IDirectDraw7* dd, int w, int h, DWORD bpp, DWORD r, DWORD g, DWORD b) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    d.dwWidth = w;
    d.dwHeight = h;
    d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    // (8 bits without masks: palettized.)
    d.ddpfPixelFormat.dwFlags = DDPF_RGB | (bpp == 8 && !r ? DDPF_PALETTEINDEXED8 : 0);
    d.ddpfPixelFormat.dwRGBBitCount = bpp;
    d.ddpfPixelFormat.dwRBitMask = r;
    d.ddpfPixelFormat.dwGBitMask = g;
    d.ddpfPixelFormat.dwBBitMask = b;
    IDirectDrawSurface7* s = nullptr;
    HRESULT hr = dd->CreateSurface(&d, &s, nullptr);
    CHECK(SUCCEEDED(hr), "CreateSurface %dx%d %lubpp -> 0x%08lx", w, h, bpp, hr);
    return s;
}

static IDirectDrawSurface7* Make32(IDirectDraw7* dd, int w, int h) { return MakeSurface(dd, w, h, 32, 0xFF0000, 0xFF00, 0xFF); }

static void Put32(IDirectDrawSurface7* s, const DWORD* px, int n) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    s->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
    memcpy(d.lpSurface, px, n * 4);
    s->Unlock(nullptr);
}

static bool Expect32(IDirectDrawSurface7* s, const DWORD* px, int n, const char* what) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    s->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
    bool ok = memcmp(d.lpSurface, px, n * 4) == 0;
    char got[160] = "";
    for (int i = 0; i < n && i < 8; i++)
        sprintf(got + strlen(got), "%08lx ", static_cast<DWORD*>(d.lpSurface)[i]);
    s->Unlock(nullptr);
    CHECK(ok, "%s (got %s)", what, got);
    return ok;
}

static HRESULT WINAPI CountModes(LPDDSURFACEDESC2, LPVOID ctx) {
    ++*static_cast<int*>(ctx);
    return DDENUMRET_OK;
}

struct EnumEntry {
    bool hasGuid;
    GUID guid;
    char name[64];
    HMONITOR monitor;
};

static BOOL WINAPI CollectDevice(GUID* guid, LPSTR, LPSTR name, LPVOID ctx, HMONITOR monitor) {
    EnumEntry e = {};
    e.hasGuid = guid != nullptr;
    if (guid) e.guid = *guid;
    lstrcpynA(e.name, name ? name : "", sizeof(e.name));
    e.monitor = monitor;
    static_cast<std::vector<EnumEntry>*>(ctx)->push_back(e);
    return TRUE;
}

static int RunUnit() {
    IDirectDraw7* dd = nullptr;
    HRESULT hr = DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    CHECK(SUCCEEDED(hr) && dd, "DirectDrawCreateEx -> 0x%08lx", hr);
    if (!dd) return 1;
    HWND hwnd = MakeWindow(WS_OVERLAPPEDWINDOW, 320, 240);
    CHECK(SUCCEEDED(dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL)), "SetCooperativeLevel(NORMAL)");

    {
        // DirectDrawEnumerateEx lists the primary display driver, then with
        // DDENUM_ATTACHEDSECONDARYDEVICES each display by GUID, \\.\DISPLAYn and monitor,
        // as ddraw.dll does (the Video Mixing Renderer 7 lists its monitors from them).
        std::vector<EnumEntry> plain, all;
        DirectDrawEnumerateExA(CollectDevice, &plain, 0);
        CHECK(plain.size() == 1 && !plain[0].hasGuid && !plain[0].monitor && !strcmp(plain[0].name, "display"),
              "DirectDrawEnumerateEx(0): the primary display driver alone (got %u)", unsigned(plain.size()));
        DirectDrawEnumerateExA(CollectDevice, &all, DDENUM_ATTACHEDSECONDARYDEVICES);
        const HMONITOR mine = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        bool named = all.size() >= 2 && !all[0].hasGuid, found = false;
        for (size_t i = 1; i < all.size(); i++) {
            named = named && all[i].hasGuid && all[i].monitor && !strncmp(all[i].name, "\\\\.\\DISPLAY", 11);
            found = found || all[i].monitor == mine;
        }
        CHECK(named && found, "DirectDrawEnumerateEx(ATTACHEDSECONDARYDEVICES): %u devices, each display named, "
              "the window's monitor among them", unsigned(all.size()));
        if (all.size() >= 2) {
            IDirectDraw7* other = nullptr;
            hr = DirectDrawCreateEx(&all[1].guid, reinterpret_cast<void**>(&other), IID_IDirectDraw7, nullptr);
            CHECK(SUCCEEDED(hr) && other, "DirectDrawCreateEx(a display's GUID) -> 0x%08lx", hr);
            if (other) other->Release();
        }
    }

    {
        // A multimedia timer's callback that keeps none of EBX, ESI, EDI and EBP, leaves the
        // direction flag set and returns as cdecl (Mega Man X's left ESI 0x10, and the timer
        // thread of Windows 10 and 11, Wine's too, crashed reading 0x20): periodic and
        // one-shot timers, dd2d9 putting back what it should have kept (safetimers=on).
        UINT id = timeSetEvent(10, 1, BadTimeProc, 0, TIME_PERIODIC | TIME_KILL_SYNCHRONOUS);
        Sleep(300);
        timeKillEvent(id);
        Sleep(50);
        const LONG periodic = g_badTicks;
        timeSetEvent(10, 1, BadTimeProc, 0, TIME_ONESHOT);
        Sleep(200);
        // (And timeSetEvent looked up by name, as a game that also ran without winmm would.)
        using TimeSetEventFn = MMRESULT(WINAPI*)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
        const auto byName = reinterpret_cast<TimeSetEventFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("winmm.dll"), "timeSetEvent")));
        if (byName) byName(10, 1, BadTimeProc, 0, TIME_ONESHOT);
        Sleep(200);
        CHECK(id && byName && periodic >= 5 && g_badTicks - periodic == 2,
              "timeSetEvent: a callback that doesn't keep its registers (%ld periodic ticks, %ld one-shot, "
              "by name too)", periodic, g_badTicks - periodic);
    }

    {
        // Miles Sound System 3's timer, as Mega Man X has it: its callback suspends the thread
        // that started Miles (the game's main thread) while it runs the game's handler, which
        // reads the joystick, as that thread does too. A read takes locks (Windows 8 on, and
        // Wine): one held by the suspended thread stopped the callback, and the game, for good.
        // The game's reads come from dd2d9's own (joypoll=on).
        struct Miles {
            HANDLE main;
            volatile LONG ticks, reads, stop;
            static void CALLBACK Tick(UINT, UINT, DWORD_PTR user, DWORD_PTR, DWORD_PTR) {
                Miles& m = *reinterpret_cast<Miles*>(user);
                if (m.stop || SuspendThread(m.main) == static_cast<DWORD>(-1)) return;
                JOYINFOEX j = {};
                j.dwSize = sizeof(j);
                j.dwFlags = 0x83;
                joyGetPosEx(0, &j);
                InterlockedIncrement(&m.ticks);
                ResumeThread(m.main);
            }
            static DWORD WINAPI Game(void* p) {
                Miles& m = *static_cast<Miles*>(p);
                DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &m.main, 0, FALSE,
                                DUPLICATE_SAME_ACCESS);
                JOYINFOEX first = {};  // (as the game does before its timer starts: the joystick found)
                first.dwSize = sizeof(first);
                first.dwFlags = 0x83;
                joyGetPosEx(0, &first);
                const UINT id = timeSetEvent(5, 0, Tick, reinterpret_cast<DWORD_PTR>(&m), TIME_PERIODIC | TIME_KILL_SYNCHRONOUS);
                const DWORD end = GetTickCount() + 1000;
                while (GetTickCount() < end) {
                    JOYINFOEX j = {};
                    j.dwSize = sizeof(j);
                    j.dwFlags = 0x83;
                    joyGetPosEx(0, &j);
                    InterlockedIncrement(&m.reads);
                }
                timeKillEvent(id);
                return 0;
            }
        };
        static Miles m = {};
        HANDLE game = CreateThread(nullptr, 0, Miles::Game, &m, 0, nullptr);
        const bool done = game && WaitForSingleObject(game, 5000) == WAIT_OBJECT_0;
        if (game && !done) {  // (stalled: let it go, for the tests after this one)
            InterlockedExchange(&m.stop, 1);
            while (m.main && ResumeThread(m.main) > 1) {}
            WaitForSingleObject(game, 5000);
        }
        CHECK(done && m.ticks > 50,
              "a Miles-style timer that suspends the game's thread to read the joystick doesn't stall it (%ld ticks, "
              "%ld reads%s)", m.ticks, m.reads, done ? "" : ", stalled");
        if (game) CloseHandle(game);
        if (m.main) CloseHandle(m.main);
    }

    {
        // A sound callback that suspends its own thread carries on: Miles Sound System 3's
        // suspends "the main thread", and since Vista it is called on the thread inside
        // waveOutOpen, there itself (Mega Man X). A thread suspending itself elsewhere is
        // suspended, for another to resume it (selfsuspend=off).
        struct Self {
            HANDLE me;
            volatile LONG calls;
            static void CALLBACK Sound(HWAVEOUT, UINT msg, DWORD_PTR user, DWORD_PTR, DWORD_PTR) {
                Self& t = *reinterpret_cast<Self*>(user);
                if (msg != WOM_OPEN) return;
                SuspendThread(t.me);  // (Miles' first instruction)
                InterlockedIncrement(&t.calls);
                ResumeThread(t.me);
            }
            static DWORD WINAPI Open(void* p) {
                Self& t = *static_cast<Self*>(p);
                DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t.me, 0, FALSE,
                                DUPLICATE_SAME_ACCESS);
                WAVEFORMATEX f = {};
                f.wFormatTag = WAVE_FORMAT_PCM;
                f.nChannels = 2;
                f.nSamplesPerSec = 22050;
                f.wBitsPerSample = 16;
                f.nBlockAlign = 4;
                f.nAvgBytesPerSec = 22050 * 4;
                HWAVEOUT w = nullptr;
                const MMRESULT r = waveOutOpen(&w, WAVE_MAPPER, &f, reinterpret_cast<DWORD_PTR>(Sound),
                                               reinterpret_cast<DWORD_PTR>(&t), CALLBACK_FUNCTION);
                if (r == MMSYSERR_NOERROR) waveOutClose(w);
                return r;
            }
            struct Waiting {
                HANDLE ready;  // (set just before the thread suspends itself)
                DWORD slept;
            };
            static DWORD WINAPI Wait(void* p) {
                Waiting& w = *static_cast<Waiting*>(p);
                HANDLE me = nullptr;
                DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &me, 0, FALSE,
                                DUPLICATE_SAME_ACCESS);
                const DWORD start = GetTickCount();
                SetEvent(w.ready);
                SuspendThread(me);  // (until the test resumes it)
                w.slept = GetTickCount() - start;
                CloseHandle(me);
                return 0;
            }
        };
        static Self t = {};
        HANDLE open = CreateThread(nullptr, 0, Self::Open, &t, 0, nullptr);
        const bool done = open && WaitForSingleObject(open, 3000) == WAIT_OBJECT_0;
        DWORD opened = MMSYSERR_ERROR;
        if (open && !done) ResumeThread(open);  // (rescued, to go on)
        else if (open) GetExitCodeThread(open, &opened);
        if (open) WaitForSingleObject(open, 2000);
        static Self::Waiting waiting = {};
        waiting.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE wait = CreateThread(nullptr, 0, Self::Wait, &waiting, 0, nullptr);
        if (wait) {
            WaitForSingleObject(waiting.ready, 2000);
            Sleep(150);
            // (Resumed once it's suspended: it may be between the event and SuspendThread.)
            for (int i = 0; i < 200 && WaitForSingleObject(wait, 0) == WAIT_TIMEOUT; i++) {
                if (ResumeThread(wait) >= 1) break;
                Sleep(10);
            }
            WaitForSingleObject(wait, 2000);
        }
        if (waiting.ready) CloseHandle(waiting.ready);
        const DWORD slept = waiting.slept;
        if (opened == MMSYSERR_NOERROR || !done)
            CHECK(done && t.calls == 1, "waveOutOpen's callback suspending its own thread carries on (%d)", int(done));
        else
            fprintf(g_out, "SKIP waveOutOpen: no sound device (0x%lx)\n", opened);
        CHECK(slept >= 100, "a thread suspending itself outside a sound callback is suspended (%lu ms)", slept);
        if (open) CloseHandle(open);
        if (wait) CloseHandle(wait);
        if (t.me) CloseHandle(t.me);
    }

    {
        // A file opened with FILE_FLAG_NO_BUFFERING is opened without it (nobuffering=off), as
        // Windows 9x ignored the flag: Biko reads a 60-byte header into a stack buffer from a
        // file it opens so, which fails since NT. The file's mode, where ntdll tells it, has no
        // FILE_NO_INTERMEDIATE_BUFFERING; and the unaligned read works (Wine never enforced the
        // flag: there only the mode tells).
        char path[MAX_PATH], dir[MAX_PATH];
        GetTempPathA(sizeof(dir), dir);
        GetTempFileNameA(dir, "dd2", 0, path);
        if (HANDLE w = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr); w != INVALID_HANDLE_VALUE) {
            static BYTE data[4096];
            for (size_t i = 0; i < sizeof(data); i++) data[i] = BYTE(i * 7);
            DWORD wrote = 0;
            WriteFile(w, data, sizeof(data), &wrote, nullptr);
            CloseHandle(w);
        }
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS, nullptr);
        BYTE header[64 + 3];
        DWORD got = 0;
        const BOOL read = f != INVALID_HANDLE_VALUE && ReadFile(f, header + 3, 60, &got, nullptr);
        bool same = read && got == 60;
        for (DWORD i = 0; same && i < 60; i++) same = header[3 + i] == BYTE(i * 7);
        // (FileModeInformation: 16; FILE_NO_INTERMEDIATE_BUFFERING: 8.)
        using QueryFn = LONG(WINAPI*)(HANDLE, void*, void*, ULONG, int);
        const auto query = reinterpret_cast<QueryFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationFile")));
        ULONG_PTR iosb[2] = {};
        ULONG mode = 0;
        const bool told = f != INVALID_HANDLE_VALUE && query && query(f, iosb, &mode, sizeof(mode), 16) >= 0;
        CHECK(same && (!told || !(mode & 8)),
              "a file opened with FILE_FLAG_NO_BUFFERING reads 60 bytes into an unaligned buffer (read %d, %lu bytes, "
              "mode 0x%lx%s)",
              int(read), got, mode, told ? "" : ", not told");
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        DeleteFileA(path);
    }

    {
        // Process heap memory comes zeroed, as fresh memory did on Windows 9x (Daytona USA
        // Deluxe released "textures" from a table it hadn't cleared yet): a block freed full of
        // data, allocated again without HEAP_ZERO_MEMORY.
        const SIZE_T n = 0x12800;
        bool zero = true;
        for (int round = 0; round < 4 && zero; round++) {
            BYTE* a = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), 0, n));
            if (!a) break;
            memset(a, 0xCD, n);
            HeapFree(GetProcessHeap(), 0, a);
            const BYTE* b = static_cast<const BYTE*>(HeapAlloc(GetProcessHeap(), 0, n));
            if (!b) break;
            for (SIZE_T i = 0; i < n && zero; i++) zero = b[i] == 0;
            HeapFree(GetProcessHeap(), 0, const_cast<BYTE*>(b));
        }
        CHECK(zero, "HeapAlloc: the game's memory comes zeroed");
    }

    {
        // A released critical section goes to the thread waiting for it, as up to Windows XP
        // (KOF 99 Evolution's intro: its main thread holds the movie's section and lets go of
        // it for an instant each frame, for the decoder's thread to hand a frame over; since
        // Vista, and under Wine, the releasing thread takes it straight back).
        struct Movie {
            CRITICAL_SECTION cs;
            volatile LONG frames, stop;
            static DWORD WINAPI Decoder(void* p) {
                Movie& m = *static_cast<Movie*>(p);
                while (!m.stop) {
                    EnterCriticalSection(&m.cs);
                    m.frames++;
                    LeaveCriticalSection(&m.cs);
                    Sleep(1);
                }
                return 0;
            }
        };
        static Movie movie;
        InitializeCriticalSection(&movie.cs);
        EnterCriticalSection(&movie.cs);
        HANDLE decoder = CreateThread(nullptr, 0, Movie::Decoder, &movie, 0, nullptr);
        Sleep(20);  // (the decoder is waiting)
        const int kFrames = 100;
        for (int i = 0; i < kFrames; i++) {
            LeaveCriticalSection(&movie.cs);
            EnterCriticalSection(&movie.cs);
            Sleep(3);
        }
        // Nested: only the last Leave releases. One that doesn't returns at once (waiting
        // for the hand-over, it would take the 4 ms and more until that is given up).
        const LONG before = movie.frames;
        EnterCriticalSection(&movie.cs);
        Sleep(5);  // (the decoder is waiting again)
        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        LeaveCriticalSection(&movie.cs);
        QueryPerformanceCounter(&t1);
        const double nestedMs = double(t1.QuadPart - t0.QuadPart) * 1000 / double(freq.QuadPart);
        const LONG nested = movie.frames;
        movie.stop = 1;
        LeaveCriticalSection(&movie.cs);
        const bool ended = decoder && WaitForSingleObject(decoder, 2000) == WAIT_OBJECT_0;
        if (decoder) CloseHandle(decoder);
        CHECK(before >= kFrames * 9 / 10, "critical section: the waiting thread got it at %ld of %d releases", before, kFrames);
        CHECK(nested == before && nestedMs < 3 && ended, "critical section: a nested Leave hands nothing over (%ld, %ld) and doesn't wait (%.2f ms); the waiter ends", before, nested, nestedMs);
        if (ended) DeleteCriticalSection(&movie.cs);
    }

    {
        // A DirectDrawCreateEx object offers dd2d9's Direct3D 7: 3D is reported.
        DDCAPS caps = {};
        caps.dwSize = sizeof(caps);
        hr = dd->GetCaps(&caps, nullptr);
        CHECK(SUCCEEDED(hr) && (caps.dwCaps & DDCAPS_3D) && (caps.ddsCaps.dwCaps & DDSCAPS_3DDEVICE),
              "GetCaps reports 3D with Direct3D 7 (dwCaps 0x%08lx)", caps.dwCaps);
        IDirect3D7* d3d = nullptr;
        hr = dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d));
        CHECK(SUCCEEDED(hr) && d3d, "IDirect3D7 offered (0x%08lx)", hr);
        if (d3d) d3d->Release();
        // A DirectDrawCreate one reports 3D too and offers IDirect3D/2/3, not IDirect3D7.
        IDirectDraw* dd1 = nullptr;
        if (SUCCEEDED(DirectDrawCreate(nullptr, &dd1, nullptr))) {
            DDCAPS c1 = {};
            c1.dwSize = sizeof(c1);
            dd1->GetCaps(&c1, nullptr);
            CHECK(c1.dwCaps & DDCAPS_3D, "DirectDrawCreate's object reports 3D (dwCaps 0x%08lx)", c1.dwCaps);
            IDirect3D2* d2 = nullptr;
            hr = dd1->QueryInterface(IID_IDirect3D2, reinterpret_cast<void**>(&d2));
            CHECK(SUCCEEDED(hr) && d2, "IDirect3D2 offered by DirectDrawCreate's object (0x%08lx)", hr);
            if (d2) d2->Release();
            IDirect3D7* d7 = nullptr;
            hr = dd1->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d7));
            CHECK(hr == E_NOINTERFACE, "no IDirect3D7 there (0x%08lx)", hr);
            if (d7) d7->Release();
            dd1->Release();
        }
        // The class factory's object is a DirectDraw 7 one when initialized through
        // IDirectDraw7, whatever interface it was made with (as ddraw.dll's Initialize).
        using GetClassFn = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
        const auto getClass = reinterpret_cast<GetClassFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("ddraw.dll"), "DllGetClassObject")));
        IClassFactory* cf = nullptr;
        IUnknown* unk = nullptr;
        hr = getClass ? getClass(CLSID_DirectDraw7, IID_IClassFactory, reinterpret_cast<void**>(&cf)) : E_FAIL;
        if (SUCCEEDED(hr) && cf) hr = cf->CreateInstance(nullptr, IID_IUnknown, reinterpret_cast<void**>(&unk));
        CHECK(getClass && cf && unk, "the class factory makes a DirectDraw object (0x%08lx)", hr);
        if (unk) {
            IDirectDraw7* co = nullptr;
            IDirect3D7* d7 = nullptr;
            hr = unk->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&co));
            if (SUCCEEDED(hr)) {
                co->Initialize(nullptr);
                hr = co->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d7));
                co->Release();
            }
            CHECK(d7, "class factory object initialized through IDirectDraw7 offers IDirect3D7 (0x%08lx)", hr);
            if (d7) d7->Release();
            unk->Release();
        }
        if (cf) cf->Release();
    }

    {
        // A surface in non-local (AGP) video memory says so, and not local video memory too.
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        d.dwWidth = d.dwHeight = 16;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY | DDSCAPS_NONLOCALVIDMEM;
        IDirectDrawSurface7* agp = nullptr;
        hr = dd->CreateSurface(&d, &agp, nullptr);
        DDSCAPS2 caps = {};
        if (agp) agp->GetCaps(&caps);
        CHECK(agp && (caps.dwCaps & DDSCAPS_NONLOCALVIDMEM) && !(caps.dwCaps & DDSCAPS_LOCALVIDMEM),
              "non-local video memory surface: caps 0x%08lx (0x%08lx)", caps.dwCaps, hr);
        if (agp) agp->Release();
    }

    const DWORD A = 0x00112233, B = 0x00445566, C = 0x00778899, D = 0x00AABBCC, K = 0x00FF00FF;

    // Color fill
    IDirectDrawSurface7* s = Make32(dd, 4, 1);
    DDBLTFX fx = {};
    fx.dwSize = sizeof(fx);
    fx.dwFillColor = A;
    s->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    DWORD e1[] = {A, A, A, A};
    Expect32(s, e1, 4, "color fill");

    // Source color key
    IDirectDrawSurface7* src = Make32(dd, 4, 1);
    DWORD sp[] = {K, B, K, C};
    Put32(src, sp, 4);
    DDCOLORKEY key = {K, K};
    src->SetColorKey(DDCKEY_SRCBLT, &key);
    fx.dwFillColor = D;
    s->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL, &fx);
    s->BltFast(0, 0, src, nullptr, DDBLTFAST_SRCCOLORKEY);
    DWORD e2[] = {D, B, D, C};
    Expect32(s, e2, 4, "BltFast with source color key");

    // Key ignores the X8 byte
    DWORD sp2[] = {0xAAFF00FF, B, 0x00FF00FF, C};
    Put32(src, sp2, 4);
    s->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL, &fx);
    s->Blt(nullptr, src, nullptr, DDBLT_KEYSRC, nullptr);
    Expect32(s, e2, 4, "color key ignores unused high byte");

    // Stretch 2 -> 4
    IDirectDrawSurface7* two = Make32(dd, 2, 1);
    DWORD tp[] = {A, B};
    Put32(two, tp, 2);
    s->Blt(nullptr, two, nullptr, DDBLT_WAIT, nullptr);
    DWORD e3[] = {A, A, B, B};
    Expect32(s, e3, 4, "stretch 2x");

    // Mirror left-right
    DWORD mp[] = {A, B, C, D};
    Put32(src, mp, 4);
    DDBLTFX mfx = {};
    mfx.dwSize = sizeof(mfx);
    mfx.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
    s->Blt(nullptr, src, nullptr, DDBLT_DDFX, &mfx);
    DWORD e4[] = {D, C, B, A};
    Expect32(s, e4, 4, "mirror left-right");

    // Without a clipper, a destination rectangle past the surface is an error (ddraw.dll behaviour)
    fx.dwFillColor = 0;
    s->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL, &fx);
    RECT dr = {2, 0, 6, 1};
    hr = s->Blt(&dr, src, nullptr, 0, nullptr);
    CHECK(hr == DDERR_INVALIDRECT, "Blt past the surface without a clipper -> DDERR_INVALIDRECT (0x%08lx)", hr);
    DWORD e5[] = {0, 0, 0, 0};
    Expect32(s, e5, 4, "surface untouched by the rejected Blt");

    // With a clipper, the blit is clipped to its clip list
    IDirectDrawClipper* clip = nullptr;
    dd->CreateClipper(0, &clip, nullptr);
    struct {
        RGNDATAHEADER h;
        RECT r[1];
    } rgn = {{sizeof(RGNDATAHEADER), RDH_RECTANGLES, 1, sizeof(RECT), {0, 0, 3, 1}}, {{0, 0, 3, 1}}};
    clip->SetClipList(reinterpret_cast<RGNDATA*>(&rgn), 0);
    s->SetClipper(clip);
    RECT dr2 = {1, 0, 5, 1};
    hr = s->Blt(&dr2, src, nullptr, 0, nullptr);
    DWORD e5b[] = {0, A, B, 0};
    CHECK(hr == DD_OK, "Blt with a clip list -> 0x%08lx", hr);
    Expect32(s, e5b, 4, "Blt clipped to the clip list");
    s->SetClipper(nullptr);
    clip->Release();
    // A clipper released once too often (Wine's test_clipper_refcount): it leaves the surface
    // as it goes, which then blits as one without a clipper.
    {
        IDirectDrawClipper* gone = nullptr;
        dd->CreateClipper(0, &gone, nullptr);
        gone->SetClipList(reinterpret_cast<RGNDATA*>(&rgn), 0);
        s->SetClipper(gone);
        gone->Release();
        gone->Release();
        IDirectDrawClipper* left = nullptr;
        const HRESULT hg = s->GetClipper(&left);
        hr = s->Blt(&dr, src, nullptr, 0, nullptr);
        CHECK(hg == DDERR_NOCLIPPERATTACHED && !left && hr == DDERR_INVALIDRECT,
              "a clipper released too often leaves its surface (0x%08lx 0x%08lx)", hg, hr);
    }

    // BltFast never clips, and requires the color key it is asked to use
    hr = s->BltFast(2, 0, src, nullptr, DDBLTFAST_NOCOLORKEY);
    CHECK(hr == DDERR_INVALIDRECT, "BltFast past the surface -> DDERR_INVALIDRECT (0x%08lx)", hr);
    RECT srcBad = {0, 0, 5, 1};
    hr = s->BltFast(0, 0, src, &srcBad, DDBLTFAST_NOCOLORKEY);
    CHECK(hr == DDERR_INVALIDRECT, "BltFast with a source rect past the source -> DDERR_INVALIDRECT (0x%08lx)", hr);
    hr = s->BltFast(0, 0, two, nullptr, DDBLTFAST_SRCCOLORKEY);
    CHECK(hr == DDERR_INVALIDPARAMS, "BltFast SRCCOLORKEY without a key -> DDERR_INVALIDPARAMS (0x%08lx)", hr);

    // Overlapping copy within one surface
    IDirectDrawSurface7* ov = Make32(dd, 8, 1);
    DWORD op[] = {1, 2, 3, 4, 5, 0, 0, 0};
    Put32(ov, op, 8);
    RECT osr = {0, 0, 5, 1}, odr = {3, 0, 8, 1};
    ov->Blt(&odr, ov, &osr, 0, nullptr);
    DWORD e6[] = {1, 2, 3, 1, 2, 3, 4, 5};
    Expect32(ov, e6, 8, "overlapping blit");

    // 32-bit -> RGB565 conversion
    IDirectDrawSurface7* s16 = MakeSurface(dd, 2, 1, 16, 0xF800, 0x07E0, 0x001F);
    IDirectDrawSurface7* c32 = Make32(dd, 2, 1);
    DWORD cp[] = {0x00FF0000, 0x0000FF00};
    Put32(c32, cp, 2);
    s16->Blt(nullptr, c32, nullptr, 0, nullptr);
    {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        s16->Lock(nullptr, &d, 0, nullptr);
        WORD* p = static_cast<WORD*>(d.lpSurface);
        CHECK(p[0] == 0xF800 && p[1] == 0x07E0, "32-bit to RGB565 conversion (got %04x %04x)", p[0], p[1]);
        CHECK(d.ddpfPixelFormat.dwRGBBitCount == 16 && d.lPitch >= 4, "Lock reports format and pitch");
        s16->Unlock(nullptr);
    }

    // GDI on a surface
    HDC dc = nullptr;
    hr = s->GetDC(&dc);
    CHECK(SUCCEEDED(hr) && dc, "GetDC -> 0x%08lx", hr);
    if (dc) {
        SetPixel(dc, 1, 0, RGB(255, 0, 0));
        s->ReleaseDC(dc);
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        s->Lock(nullptr, &d, 0, nullptr);
        DWORD v = static_cast<DWORD*>(d.lpSurface)[1] & 0xFFFFFF;
        s->Unlock(nullptr);
        CHECK(v == 0xFF0000, "GDI SetPixel visible through Lock (got %06lx)", v);
    }

    // Interface versions share one object
    IDirectDrawSurface* s1 = nullptr;
    hr = s->QueryInterface(IID_IDirectDrawSurface, reinterpret_cast<void**>(&s1));
    CHECK(SUCCEEDED(hr) && s1, "QueryInterface surface7 -> surface1");
    if (s1) {
        DDSURFACEDESC d1 = {};
        d1.dwSize = sizeof(d1);
        hr = s1->GetSurfaceDesc(&d1);
        CHECK(SUCCEEDED(hr) && d1.dwWidth == 4 && d1.ddpfPixelFormat.dwRGBBitCount == 32, "v1 GetSurfaceDesc");
        s1->Release();
    }
    IDirectDraw* dd1 = nullptr;
    hr = dd->QueryInterface(IID_IDirectDraw, reinterpret_cast<void**>(&dd1));
    CHECK(SUCCEEDED(hr) && dd1, "QueryInterface DirectDraw7 -> DirectDraw");
    if (dd1) dd1->Release();

    // Palette round trip
    PALETTEENTRY pe[256];
    for (int i = 0; i < 256; i++) pe[i] = {BYTE(i), BYTE(255 - i), BYTE(i / 2), 0};
    IDirectDrawPalette* pal = nullptr;
    hr = dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, nullptr);
    CHECK(SUCCEEDED(hr) && pal, "CreatePalette");
    if (pal) {
        PALETTEENTRY got[4];
        pal->GetEntries(0, 100, 4, got);
        CHECK(got[0].peRed == 100 && got[0].peGreen == 155 && got[3].peBlue == 51, "palette GetEntries");
        pal->Release();
    }

    // Display modes
    int modes = 0;
    dd->EnumDisplayModes(0, nullptr, &modes, CountModes);
    CHECK(modes >= 3, "EnumDisplayModes (%d modes)", modes);

    // Argument checks found in review
    hr = s->Unlock(nullptr);
    CHECK(hr == DDERR_NOTLOCKED, "Unlock without Lock -> DDERR_NOTLOCKED (0x%08lx)", hr);
    {
        IDirectDrawPalette* p2 = nullptr;
        dd->CreatePalette(DDPCAPS_8BIT, pe, &p2, nullptr);
        PALETTEENTRY tmp[16];
        HRESULT a = p2->GetEntries(0, 250, 10, tmp), b = p2->GetEntries(0, 0xFFFFFFFFu, 2, tmp);
        CHECK(a == DDERR_INVALIDPARAMS && b == DDERR_INVALIDPARAMS, "palette range checks (0x%08lx 0x%08lx)", a, b);
        p2->Release();
    }
    {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        d.dwWidth = 64;
        d.dwHeight = 4;
        d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        d.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
        d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('Y', 'U', 'Y', '2');
        IDirectDrawSurface7* yuy = nullptr;
        hr = dd->CreateSurface(&d, &yuy, nullptr);
        if (yuy) {
            DDSURFACEDESC2 l = {};
            l.dwSize = sizeof(l);
            yuy->Lock(nullptr, &l, 0, nullptr);
            CHECK((l.dwFlags & DDSD_PITCH) && l.lPitch >= 128, "YUY2 surface has 2 bytes per pixel (pitch %ld)", l.lPitch);
            // White, black, then two of BT.601 red (video levels), as Y0 U Y1 V.
            static const BYTE kYuy[8] = {235, 128, 16, 128, 81, 90, 81, 240};
            memcpy(l.lpSurface, kYuy, 8);
            yuy->Unlock(nullptr);
            // Blitted to RGB (as a video player shows a frame): converted; stretched too.
            auto nearRgb = [](DWORD a, DWORD b) {
                for (int sh = 0; sh < 24; sh += 8)
                    if (abs(int((a >> sh) & 255) - int((b >> sh) & 255)) > 2) return false;
                return true;
            };
            IDirectDrawSurface7* rgb = Make32(dd, 8, 1);
            RECT four = {0, 0, 4, 1}, eight = {0, 0, 8, 1};
            hr = rgb->Blt(&four, yuy, &four, DDBLT_WAIT, nullptr);
            DDSURFACEDESC2 r = {};
            r.dwSize = sizeof(r);
            rgb->Lock(nullptr, &r, DDLOCK_WAIT, nullptr);
            DWORD got[8];
            memcpy(got, r.lpSurface, 32);
            rgb->Unlock(nullptr);
            CHECK(SUCCEEDED(hr) && nearRgb(got[0], 0xFFFFFF) && nearRgb(got[1], 0) && nearRgb(got[2], 0xFF0000) && nearRgb(got[3], 0xFF0000),
                  "YUY2 blitted to RGB (0x%08lx): %06lx %06lx %06lx %06lx", hr, got[0] & 0xFFFFFF, got[1] & 0xFFFFFF,
                  got[2] & 0xFFFFFF, got[3] & 0xFFFFFF);
            hr = rgb->Blt(&eight, yuy, &four, DDBLT_WAIT, nullptr);
            rgb->Lock(nullptr, &r, DDLOCK_WAIT, nullptr);
            memcpy(got, r.lpSurface, 32);
            rgb->Unlock(nullptr);
            CHECK(SUCCEEDED(hr) && nearRgb(got[0], 0xFFFFFF) && nearRgb(got[1], 0xFFFFFF) && nearRgb(got[2], 0) && nearRgb(got[3], 0) &&
                      nearRgb(got[5], 0xFF0000),
                  "YUY2 stretched 2x into RGB (0x%08lx): %06lx %06lx %06lx", hr, got[0] & 0xFFFFFF, got[2] & 0xFFFFFF, got[5] & 0xFFFFFF);
            // RGB into UYVY (U Y0 V Y1): the pair's chroma averaged.
            d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('U', 'Y', 'V', 'Y');
            IDirectDrawSurface7* uyvy = nullptr;
            hr = dd->CreateSurface(&d, &uyvy, nullptr);
            if (uyvy) {
                hr = uyvy->Blt(&four, rgb, &eight, DDBLT_WAIT, nullptr);  // (shrunk back: white, black, red, red)
                DDSURFACEDESC2 u = {};
                u.dwSize = sizeof(u);
                uyvy->Lock(nullptr, &u, DDLOCK_WAIT, nullptr);
                BYTE ub[8];
                memcpy(ub, u.lpSurface, 8);
                uyvy->Unlock(nullptr);
                auto close = [](int a, int b) { return abs(a - b) <= 2; };
                CHECK(SUCCEEDED(hr) && close(ub[1], 235) && close(ub[3], 16) && close(ub[0], 128) && close(ub[2], 128) &&
                          close(ub[5], 81) && close(ub[7], 81) && close(ub[4], 90) && close(ub[6], 240),
                      "RGB blitted into UYVY (0x%08lx): %u %u %u %u  %u %u %u %u", hr, ub[0], ub[1], ub[2], ub[3], ub[4], ub[5],
                      ub[6], ub[7]);
                uyvy->Release();
            } else {
                CHECK(false, "UYVY surface -> 0x%08lx", hr);
            }
            // YUY2 to YUY2 unstretched: copied as it is.
            d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('Y', 'U', 'Y', '2');
            IDirectDrawSurface7* yuy2 = nullptr;
            hr = dd->CreateSurface(&d, &yuy2, nullptr);
            if (yuy2) {
                RECT at = {8, 2, 12, 3};
                hr = yuy2->Blt(&at, yuy, &four, DDBLT_WAIT, nullptr);
                DDSURFACEDESC2 u = {};
                u.dwSize = sizeof(u);
                yuy2->Lock(nullptr, &u, DDLOCK_WAIT, nullptr);
                const bool same = memcmp(static_cast<BYTE*>(u.lpSurface) + 2 * u.lPitch + 16, kYuy, 8) == 0;
                yuy2->Unlock(nullptr);
                CHECK(SUCCEEDED(hr) && same, "YUY2 to YUY2 copied as it is (0x%08lx)", hr);
                yuy2->Release();
            }
            // A colour key on the YUY2 source, in its own packing (the first pixel of the
            // pair: white): white pixels skipped, the others drawn.
            {
                DDCOLORKEY k = {0x8010'80EBu, 0x8010'80EBu};  // Y0 235, U 128, Y1 16, V 128
                hr = yuy->SetColorKey(DDCKEY_SRCBLT, &k);
                DDBLTFX bfx = {};
                bfx.dwSize = sizeof(bfx);
                bfx.dwFillColor = 0x0000FF;
                rgb->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &bfx);
                const HRESULT hk = rgb->Blt(&four, yuy, &four, DDBLT_KEYSRC | DDBLT_WAIT, nullptr);
                rgb->Lock(nullptr, &r, DDLOCK_WAIT, nullptr);
                memcpy(got, r.lpSurface, 32);
                rgb->Unlock(nullptr);
                CHECK(SUCCEEDED(hr) && SUCCEEDED(hk) && (got[0] & 0xFFFFFF) == 0x0000FF && nearRgb(got[1], 0) &&
                          nearRgb(got[2], 0xFF0000),
                      "YUY2 source colour key (0x%08lx 0x%08lx): %06lx %06lx %06lx", hr, hk, got[0] & 0xFFFFFF,
                      got[1] & 0xFFFFFF, got[2] & 0xFFFFFF);
            }
            // A YUY2 key is compared in Y, U and V, not as the RGB it decodes to: video black
            // (Y 16) keyed, a pixel below black (Y 10, the same RGB black once decoded) drawn.
            {
                static const BYTE kDark[8] = {16, 128, 10, 128, 81, 90, 81, 240};
                DDSURFACEDESC2 l = {};
                l.dwSize = sizeof(l);
                yuy->Lock(nullptr, &l, DDLOCK_WAIT, nullptr);
                memcpy(l.lpSurface, kDark, 8);
                yuy->Unlock(nullptr);
                DDCOLORKEY k = {0x8010'8010u, 0x8010'8010u};  // Y0 16, U 128, Y1 16, V 128
                hr = yuy->SetColorKey(DDCKEY_SRCBLT, &k);
                DDBLTFX bfx = {};
                bfx.dwSize = sizeof(bfx);
                bfx.dwFillColor = 0x0000FF;
                rgb->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &bfx);
                const HRESULT hk = rgb->Blt(&four, yuy, &four, DDBLT_KEYSRC | DDBLT_WAIT, nullptr);
                rgb->Lock(nullptr, &r, DDLOCK_WAIT, nullptr);
                memcpy(got, r.lpSurface, 32);
                rgb->Unlock(nullptr);
                CHECK(SUCCEEDED(hr) && SUCCEEDED(hk) && (got[0] & 0xFFFFFF) == 0x0000FF && nearRgb(got[1], 0) &&
                          nearRgb(got[2], 0xFF0000),
                      "YUY2 source key compared in YUV (0x%08lx 0x%08lx): %06lx %06lx %06lx", hr, hk, got[0] & 0xFFFFFF,
                      got[1] & 0xFFFFFF, got[2] & 0xFFFFFF);
                // The same key as the YUY2 destination's: white blitted onto black only, not
                // onto below black (re-encoded from RGB after the blit, which keeps it dark:
                // Y 16, not 10).
                hr = yuy->SetColorKey(DDCKEY_DESTBLT, &k);
                IDirectDrawSurface7* white = Make32(dd, 4, 1);
                bfx.dwFillColor = 0xFFFFFF;
                white->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &bfx);
                RECT two = {0, 0, 2, 1};
                const HRESULT hd = yuy->Blt(&two, white, &two, DDBLT_KEYDEST | DDBLT_WAIT, nullptr);
                DDSURFACEDESC2 l2 = {};
                l2.dwSize = sizeof(l2);
                yuy->Lock(nullptr, &l2, DDLOCK_WAIT, nullptr);
                BYTE yb[4];
                memcpy(yb, l2.lpSurface, 4);
                yuy->Unlock(nullptr);
                white->Release();
                CHECK(SUCCEEDED(hr) && SUCCEEDED(hd) && yb[0] >= 233 && yb[2] <= 20,
                      "YUY2 destination key compared in YUV (0x%08lx 0x%08lx): Y %u %u", hr, hd, yb[0], yb[2]);
            }
            rgb->Release();
            yuy->Release();
        } else {
            CHECK(false, "YUY2 surface -> 0x%08lx", hr);
        }
        // DXT to DXT of one format: the blocks copied as they are, Blt and BltFast. These
        // blocks use only the colours between their endpoints (as a good encoder's do), which
        // decoding and encoding again would change.
        {
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            d.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_SYSTEMMEMORY;
            d.dwWidth = 8;
            d.dwHeight = 8;
            d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            d.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
            d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('D', 'X', 'T', '1');
            IDirectDrawSurface7 *a = nullptr, *b = nullptr;
            dd->CreateSurface(&d, &a, nullptr);
            dd->CreateSurface(&d, &b, nullptr);
            CHECK(a && b, "DXT1 surfaces");
            if (a && b) {
                static const BYTE kBlocks[4][8] = {{0x00, 0xF8, 0x1F, 0x00, 0xAA, 0xFF, 0xAA, 0xFF},
                                                   {0xE0, 0x07, 0x00, 0xF8, 0xFA, 0xAF, 0xFA, 0xAF},
                                                   {0x1F, 0x00, 0xE0, 0x07, 0xEE, 0xBB, 0xEE, 0xBB},
                                                   {0xFF, 0xFF, 0x00, 0x00, 0xAF, 0xFA, 0xAF, 0xFA}};
                DDSURFACEDESC2 l = {};
                l.dwSize = sizeof(l);
                a->Lock(nullptr, &l, DDLOCK_WAIT, nullptr);
                memcpy(l.lpSurface, kBlocks, sizeof(kBlocks));
                a->Unlock(nullptr);
                auto blocksOf = [&](IDirectDrawSurface7* s, BYTE (*out)[8]) {
                    DDSURFACEDESC2 k = {};
                    k.dwSize = sizeof(k);
                    s->Lock(nullptr, &k, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
                    memcpy(out, k.lpSurface, 32);
                    s->Unlock(nullptr);
                };
                BYTE got[4][8];
                hr = b->Blt(nullptr, a, nullptr, DDBLT_WAIT, nullptr);
                blocksOf(b, got);
                CHECK(SUCCEEDED(hr) && !memcmp(got, kBlocks, sizeof(kBlocks)), "DXT1 Blt copies the blocks (0x%08lx)", hr);
                RECT second = {4, 0, 8, 8};
                hr = b->BltFast(0, 0, a, &second, DDBLTFAST_WAIT);
                blocksOf(b, got);
                CHECK(SUCCEEDED(hr) && !memcmp(got[0], kBlocks[1], 8) && !memcmp(got[2], kBlocks[3], 8),
                      "DXT1 BltFast copies the blocks (0x%08lx)", hr);
            }
            if (a) a->Release();
            if (b) b->Release();
            // A system-memory surface made with an RGB format pointed at DXT1 memory
            // (SetSurfaceDesc): the blocks laid out for its own width, not the old pitch.
            IDirectDrawSurface7* rgbSurf = MakeSurface(dd, 16, 16, 32, 0xFF0000, 0xFF00, 0xFF);
            if (rgbSurf) {
                static BYTE mem[128 + 64];
                memset(mem, 0xCD, sizeof(mem));
                DDSURFACEDESC2 sd = {};
                sd.dwSize = sizeof(sd);
                sd.dwFlags = DDSD_LPSURFACE | DDSD_PIXELFORMAT;
                sd.lpSurface = mem;
                sd.ddpfPixelFormat = d.ddpfPixelFormat;
                hr = rgbSurf->SetSurfaceDesc(&sd, 0);
                DDSURFACEDESC2 l = {};
                l.dwSize = sizeof(l);
                rgbSurf->Lock(nullptr, &l, DDLOCK_WAIT, nullptr);
                rgbSurf->Unlock(nullptr);
                IDirectDrawSurface7* fillSrc = Make32(dd, 16, 16);
                DDBLTFX bfx = {};
                bfx.dwSize = sizeof(bfx);
                bfx.dwFillColor = 0x00FF00;
                fillSrc->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL, &bfx);
                const HRESULT hb = rgbSurf->Blt(nullptr, fillSrc, nullptr, DDBLT_WAIT, nullptr);
                bool guard = true;
                for (int i = 128; i < int(sizeof(mem)); i++) guard &= mem[i] == 0xCD;
                CHECK(SUCCEEDED(hr) && (l.dwFlags & DDSD_LINEARSIZE) && l.dwLinearSize == 128 && SUCCEEDED(hb) && guard,
                      "SetSurfaceDesc to DXT1: linear size %lu, Blt into it stays inside (0x%08lx 0x%08lx)", l.dwLinearSize,
                      hr, hb);
                fillSrc->Release();
                rgbSurf->Release();
            }
        }
        // Planar YV12 / I420 (4:2:0): a luma plane, then the chroma planes at half the pitch.
        {
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
            d.dwWidth = 4;
            d.dwHeight = 2;
            d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            d.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
            d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('Y', 'V', '1', '2');
            IDirectDrawSurface7* yv = nullptr;
            hr = dd->CreateSurface(&d, &yv, nullptr);
            CHECK(SUCCEEDED(hr), "YV12 surface -> 0x%08lx", hr);
            if (yv) {
                DDSURFACEDESC2 l = {};
                l.dwSize = sizeof(l);
                yv->Lock(nullptr, &l, DDLOCK_WAIT, nullptr);
                const LONG pitch = l.lPitch;
                BYTE* b = static_cast<BYTE*>(l.lpSurface);
                // White, white, red, red on both rows: V plane first (128, 240), then U (128, 90).
                for (int y = 0; y < 2; y++) {
                    b[y * pitch + 0] = b[y * pitch + 1] = 235;
                    b[y * pitch + 2] = b[y * pitch + 3] = 81;
                }
                BYTE* v = b + 2 * pitch;
                BYTE* u = v + pitch / 2;
                v[0] = 128, v[1] = 240, u[0] = 128, u[1] = 90;
                yv->Unlock(nullptr);
                CHECK(!(pitch & 0xFF) && (l.dwFlags & DDSD_PITCH), "YV12: a byte of luma per pixel, pitch %ld", pitch);
                // Its bits per pixel described, as drivers do: VMR-7 sizes its frames by them
                // and showed none without (Romance of the Three Kingdoms VII's intro).
                DDSURFACEDESC2 g = {};
                g.dwSize = sizeof(g);
                yv->GetSurfaceDesc(&g);
                CHECK(l.ddpfPixelFormat.dwYUVBitCount == 12 && g.ddpfPixelFormat.dwYUVBitCount == 12,
                      "YV12: 12 bits per pixel described (Lock %lu, GetSurfaceDesc %lu)", l.ddpfPixelFormat.dwYUVBitCount,
                      g.ddpfPixelFormat.dwYUVBitCount);
                auto nearRgb = [](DWORD a, DWORD c) {
                    for (int sh = 0; sh < 24; sh += 8)
                        if (abs(int((a >> sh) & 255) - int((c >> sh) & 255)) > 2) return false;
                    return true;
                };
                IDirectDrawSurface7* rgb = Make32(dd, 4, 2);
                hr = rgb->Blt(nullptr, yv, nullptr, DDBLT_WAIT, nullptr);
                DDSURFACEDESC2 r = {};
                r.dwSize = sizeof(r);
                rgb->Lock(nullptr, &r, DDLOCK_WAIT, nullptr);
                DWORD got[4];
                for (int x = 0; x < 4; x++) got[x] = reinterpret_cast<DWORD*>(static_cast<BYTE*>(r.lpSurface) + r.lPitch)[x];
                rgb->Unlock(nullptr);
                CHECK(SUCCEEDED(hr) && nearRgb(got[0], 0xFFFFFF) && nearRgb(got[1], 0xFFFFFF) && nearRgb(got[2], 0xFF0000) &&
                          nearRgb(got[3], 0xFF0000),
                      "YV12 blitted to RGB (0x%08lx): %06lx %06lx %06lx %06lx", hr, got[0] & 0xFFFFFF, got[1] & 0xFFFFFF,
                      got[2] & 0xFFFFFF, got[3] & 0xFFFFFF);
                // RGB into I420 (U plane first).
                d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('I', '4', '2', '0');
                IDirectDrawSurface7* i420 = nullptr;
                if (SUCCEEDED(dd->CreateSurface(&d, &i420, nullptr))) {
                    hr = i420->Blt(nullptr, rgb, nullptr, DDBLT_WAIT, nullptr);
                    DDSURFACEDESC2 q = {};
                    q.dwSize = sizeof(q);
                    i420->Lock(nullptr, &q, DDLOCK_WAIT, nullptr);
                    const BYTE* qb = static_cast<BYTE*>(q.lpSurface);
                    const BYTE* qu = qb + 2 * q.lPitch;
                    const BYTE* qv = qu + q.lPitch / 2;
                    auto close = [](int a, int c) { return abs(a - c) <= 2; };
                    const bool ok = close(qb[0], 235) && close(qb[q.lPitch + 3], 81) && close(qu[0], 128) && close(qu[1], 90) &&
                                    close(qv[0], 128) && close(qv[1], 240);
                    CHECK(SUCCEEDED(hr) && ok, "RGB blitted into I420 (0x%08lx): Y %u %u, U %u %u, V %u %u", hr, qb[0],
                          qb[q.lPitch + 3], qu[0], qu[1], qv[0], qv[1]);
                    i420->Unlock(nullptr);
                    i420->Release();
                }
                // YV12 to YV12: copied as it is, plane by plane.
                d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('Y', 'V', '1', '2');
                IDirectDrawSurface7* yv2 = nullptr;
                if (SUCCEEDED(dd->CreateSurface(&d, &yv2, nullptr))) {
                    hr = yv2->Blt(nullptr, yv, nullptr, DDBLT_WAIT, nullptr);
                    DDSURFACEDESC2 a = {}, c = {};
                    a.dwSize = c.dwSize = sizeof(a);
                    yv->Lock(nullptr, &a, DDLOCK_WAIT, nullptr);
                    yv2->Lock(nullptr, &c, DDLOCK_WAIT, nullptr);
                    const bool same = memcmp(a.lpSurface, c.lpSurface, size_t(a.lPitch) * 3) == 0;
                    yv2->Unlock(nullptr);
                    yv->Unlock(nullptr);
                    CHECK(SUCCEEDED(hr) && same, "YV12 to YV12 copied as it is (0x%08lx)", hr);
                    yv2->Release();
                }
                // A YV12 source key, compared in Y, U and V. Four 2x2 blocks: (Y 235, U 110, V 150),
                // the same with U and V swapped, white (Y 235, grey) and above white (Y 245, grey:
                // the same RGB white once decoded). Keyed with the first: only it skipped (the
                // planes and the key's bytes read the right way round). Keyed with white: the
                // white block alone (the RGB comparison skipped the one above white too).
                {
                    DDSURFACEDESC2 kd = d;
                    kd.dwWidth = 8;
                    IDirectDrawSurface7* yk = nullptr;
                    hr = dd->CreateSurface(&kd, &yk, nullptr);
                    IDirectDrawSurface7* wide = Make32(dd, 8, 2);
                    DDSURFACEDESC2 l = {};
                    l.dwSize = sizeof(l);
                    if (SUCCEEDED(hr) && SUCCEEDED(yk->Lock(nullptr, &l, DDLOCK_WAIT, nullptr))) {
                        BYTE* yb = static_cast<BYTE*>(l.lpSurface);
                        static const BYTE kY[4] = {235, 235, 235, 245}, kU[4] = {110, 150, 128, 128}, kV[4] = {150, 110, 128, 128};
                        for (int y = 0; y < 2; y++)
                            for (int x = 0; x < 8; x++) yb[y * l.lPitch + x] = kY[x / 2];
                        BYTE* vp = yb + 2 * l.lPitch;  // (YV12: V plane first)
                        BYTE* up = vp + l.lPitch / 2;
                        for (int c = 0; c < 4; c++) vp[c] = kV[c], up[c] = kU[c];
                        yk->Unlock(nullptr);
                    }
                    const DWORD keys[2] = {0x0096'6EEBu, 0x0080'80EBu};  // Y 235, U 110, V 150; Y 235, U 128, V 128
                    const int skipped[2] = {0, 2};
                    for (int t = 0; t < 2 && yk; t++) {
                        DDCOLORKEY k = {keys[t], keys[t]};
                        const HRESULT hs = yk->SetColorKey(DDCKEY_SRCBLT, &k);
                        DDBLTFX bfx = {};
                        bfx.dwSize = sizeof(bfx);
                        bfx.dwFillColor = 0x0000FF;
                        wide->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &bfx);
                        const HRESULT hk = wide->Blt(nullptr, yk, nullptr, DDBLT_KEYSRC | DDBLT_WAIT, nullptr);
                        DWORD px[8] = {};
                        DDSURFACEDESC2 w = {};
                        w.dwSize = sizeof(w);
                        if (SUCCEEDED(wide->Lock(nullptr, &w, DDLOCK_WAIT, nullptr))) {
                            memcpy(px, static_cast<BYTE*>(w.lpSurface) + w.lPitch, sizeof(px));
                            wide->Unlock(nullptr);
                        }
                        bool ok = SUCCEEDED(hs) && SUCCEEDED(hk);
                        for (int blk = 0; blk < 4; blk++) {
                            const bool blue = (px[2 * blk] & 0xFFFFFF) == 0x0000FF && (px[2 * blk + 1] & 0xFFFFFF) == 0x0000FF;
                            ok = ok && blue == (blk == skipped[t]);
                        }
                        CHECK(ok, "YV12 source key %s compared in YUV (0x%08lx 0x%08lx): %06lx %06lx %06lx %06lx",
                              t ? "white" : "(235, 110, 150)", hs, hk, px[0] & 0xFFFFFF, px[2] & 0xFFFFFF, px[4] & 0xFFFFFF,
                              px[6] & 0xFFFFFF);
                    }
                    CHECK(yk != nullptr, "YV12 8x2 surface -> 0x%08lx", hr);
                    if (yk) yk->Release();
                    wide->Release();
                }
                rgb->Release();
                yv->Release();
            }
        }
        // RGB into an 8-bit surface: each pixel the nearest palette entry.
        {
            IDirectDrawSurface7* rgb8 = Make32(dd, 4, 1);
            IDirectDrawSurface7* p8 = MakeSurface(dd, 4, 1, 8, 0, 0, 0);
            PALETTEENTRY pe[256] = {};
            for (int i = 0; i < 256; i++) pe[i] = {BYTE(i & 0x40), BYTE(i & 0x20), BYTE(i & 0x10), 0};
            pe[5] = {255, 0, 0, 0};
            pe[9] = {0, 255, 0, 0};
            IDirectDrawPalette* pal8 = nullptr;
            dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal8, nullptr);
            if (rgb8 && p8 && pal8) {
                p8->SetPalette(pal8);
                const DWORD px[4] = {0xFF0000, 0x00FF00, 0xF01010, 0x000000};
                Put32(rgb8, px, 4);
                hr = p8->Blt(nullptr, rgb8, nullptr, DDBLT_WAIT, nullptr);
                DDSURFACEDESC2 l8 = {};
                l8.dwSize = sizeof(l8);
                p8->Lock(nullptr, &l8, DDLOCK_WAIT, nullptr);
                BYTE ix[4];
                memcpy(ix, l8.lpSurface, 4);
                p8->Unlock(nullptr);
                CHECK(SUCCEEDED(hr) && ix[0] == 5 && ix[1] == 9 && ix[2] == 5 && ix[3] == 0,
                      "RGB blitted into an 8-bit surface: nearest palette entries (0x%08lx): %u %u %u %u", hr, ix[0], ix[1],
                      ix[2], ix[3]);
            }
            if (pal8) pal8->Release();
            if (p8) p8->Release();
            if (rgb8) rgb8->Release();
        }
        DWORD codes[16] = {}, n = 16, count = 0;
        dd->GetFourCCCodes(&count, nullptr);
        dd->GetFourCCCodes(&n, codes);
        CHECK(count == 8 && n == 8 && codes[7] == MAKEFOURCC('Y', 'V', '1', '2') && codes[0] == MAKEFOURCC('Y', 'U', 'Y', '2'), "GetFourCCCodes: %lu codes (%lu copied)", count, n);
        d.ddpfPixelFormat.dwFourCC = MAKEFOURCC('X', 'Y', 'Z', 'W');
        IDirectDrawSurface7* unk = nullptr;
        hr = dd->CreateSurface(&d, &unk, nullptr);
        CHECK(hr == DDERR_INVALIDPIXELFORMAT, "unknown FourCC refused (0x%08lx)", hr);
        if (unk) unk->Release();
    }

    // Flip rings put together with AddAttachedSurface, as ddraw.dll builds them: through
    // IDirectDrawSurface4 too, a surface attached to the front becomes the back buffer.
    {
        IDirectDrawSurface7* m7[3] = {Make32(dd, 8, 8), Make32(dd, 8, 8), Make32(dd, 8, 8)};
        IDirectDrawSurface4* m[3] = {};
        for (int i = 0; i < 3; i++) m7[i]->QueryInterface(IID_IDirectDrawSurface4, reinterpret_cast<void**>(&m[i]));
        auto caps = [](IDirectDrawSurface4* x) {
            DDSCAPS2 c = {};
            x->GetCaps(&c);
            return c.dwCaps & (DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER);
        };
        hr = m[0]->AddAttachedSurface(m[1]);
        CHECK(hr == DD_OK && caps(m[0]) == (DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER) &&
                  caps(m[1]) == (DDSCAPS_FLIP | DDSCAPS_BACKBUFFER),
              "IDirectDrawSurface4 ring of two (0x%08lx)", hr);
        hr = m[0]->AddAttachedSurface(m[2]);
        CHECK(hr == DD_OK && caps(m[2]) == (DDSCAPS_FLIP | DDSCAPS_BACKBUFFER) && caps(m[1]) == DDSCAPS_FLIP,
              "attached to the front: the new back buffer (0x%08lx)", hr);
        DDSCAPS2 want = {DDSCAPS_BACKBUFFER};
        IDirectDrawSurface4* got = nullptr;
        hr = m[0]->GetAttachedSurface(&want, &got);
        CHECK(hr == DD_OK && got == m[2], "GetAttachedSurface(BACKBUFFER) finds it (0x%08lx)", hr);
        if (got) got->Release();
        // Rockman X5: dwCaps set, the other words left as they were on the stack.
        DDSCAPS2 junk = {DDSCAPS_BACKBUFFER, 0x0019F7B4, 0x00402A10, 0x7FFDF000};
        got = nullptr;
        hr = m[0]->GetAttachedSurface(&junk, &got);
        CHECK(hr == DD_OK && got == m[2], "GetAttachedSurface(BACKBUFFER) with leftovers in the other caps words (0x%08lx)", hr);
        if (got) got->Release();
        DDSCAPS2 face = {DDSCAPS_BACKBUFFER, DDSCAPS2_CUBEMAP_NEGATIVEZ};
        got = nullptr;
        hr = m[0]->GetAttachedSurface(&face, &got);
        CHECK(hr == DDERR_NOTFOUND, "... but a real dwCaps2 nothing has -> DDERR_NOTFOUND (0x%08lx)", hr);
        if (got) got->Release();
        DDSCAPS2 none = {};
        hr = m[0]->GetAttachedSurface(&none, &got);
        CHECK(hr == DDERR_NOTFOUND, "GetAttachedSurface without caps -> DDERR_NOTFOUND (0x%08lx)", hr);
        hr = m[0]->DeleteAttachedSurface(1, m[2]);
        CHECK(hr == DDERR_INVALIDPARAMS, "DeleteAttachedSurface flags -> DDERR_INVALIDPARAMS (0x%08lx)", hr);
        hr = m[1]->DeleteAttachedSurface(0, m[2]);
        CHECK(hr == DDERR_SURFACENOTATTACHED, "not attached there -> DDERR_SURFACENOTATTACHED (0x%08lx)", hr);
        hr = m[0]->DeleteAttachedSurface(0, nullptr);
        CHECK(hr == DD_OK && caps(m[1]) == (DDSCAPS_FLIP | DDSCAPS_BACKBUFFER),
              "DeleteAttachedSurface(NULL) detaches the newest (0x%08lx)", hr);
        hr = m[0]->DeleteAttachedSurface(0, nullptr);
        CHECK(hr == DD_OK && caps(m[0]) == 0 && caps(m[1]) == 0, "and the ring comes apart (0x%08lx)", hr);
        for (int i = 0; i < 3; i++) {
            if (m[i]) m[i]->Release();
            m7[i]->Release();
        }
    }

    // The old surface interfaces, as ddraw.dll (spec: re/spec/legacy-interfaces.md): an
    // attachment is the pointer passed (a v1 one through IDirectDrawSurface3 here), and
    // IDirectDrawSurface2/3 hand out IDirectDrawSurface (v1) pointers.
    {
        IDirectDrawSurface7* a7 = Make32(dd, 8, 8);
        IDirectDrawSurface7* b7 = Make32(dd, 8, 8);
        IDirectDrawSurface3* a3 = nullptr;
        IDirectDrawSurface* a1 = nullptr;
        IDirectDrawSurface* b1 = nullptr;
        a7->QueryInterface(IID_IDirectDrawSurface3, reinterpret_cast<void**>(&a3));
        a7->QueryInterface(IID_IDirectDrawSurface, reinterpret_cast<void**>(&a1));
        b7->QueryInterface(IID_IDirectDrawSurface, reinterpret_cast<void**>(&b1));
        auto* b1as3 = reinterpret_cast<IDirectDrawSurface3*>(b1);
        hr = a3->AddAttachedSurface(b1as3);
        CHECK(hr == DD_OK, "v1 pointer attached through IDirectDrawSurface3 (0x%08lx)", hr);
        DDSCAPS want = {DDSCAPS_BACKBUFFER};
        IDirectDrawSurface3* got = nullptr;
        hr = a3->GetAttachedSurface(&want, &got);
        CHECK(hr == DD_OK && static_cast<void*>(got) == static_cast<void*>(b1),
              "IDirectDrawSurface3::GetAttachedSurface hands out the v1 pointer (0x%08lx)", hr);
        if (got) got->Release();
        hr = a3->AddAttachedSurface(b1as3);
        CHECK(hr == DDERR_SURFACEALREADYATTACHED, "the same pointer again -> SURFACEALREADYATTACHED (0x%08lx)", hr);
        hr = a1->DeleteAttachedSurface(0, b1);
        CHECK(hr == DD_OK, "detached through IDirectDrawSurface with the same pointer (0x%08lx)", hr);
        b1->Release();
        a1->Release();
        a3->Release();
        b7->Release();
        a7->Release();
    }

    // Flip chain swaps memory
    IDirectDrawSurface7* prim = nullptr;
    {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS;
        d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        hr = dd->CreateSurface(&d, &prim, nullptr);
        CHECK(SUCCEEDED(hr), "windowed primary -> 0x%08lx", hr);
        if (prim) {
            DDSURFACEDESC2 l = {};
            l.dwSize = sizeof(l);
            prim->Lock(nullptr, &l, 0, nullptr);
            // As in ddraw.dll: no mode change while a video-memory surface is locked.
            hr = dd->SetDisplayMode(640, 480, 16, 0, 0);
            CHECK(hr == DDERR_SURFACEBUSY, "SetDisplayMode with the primary locked (0x%08lx)", hr);
            prim->Unlock(nullptr);
            hr = dd->SetDisplayMode(640, 480, 16, 0, 0);
            CHECK(hr == DD_OK, "SetDisplayMode once unlocked (0x%08lx)", hr);
            CHECK(prim->IsLost() == DDERR_SURFACELOST, "primary lost after SetDisplayMode");
            DDSURFACEDESC2 now = {};
            now.dwSize = sizeof(now);
            prim->GetSurfaceDesc(&now);
            CHECK(now.ddpfPixelFormat.dwRGBBitCount == 16, "the lost primary reports the display's format (%lu bpp)",
                  now.ddpfPixelFormat.dwRGBBitCount);
            hr = prim->Restore();
            CHECK(hr == DDERR_WRONGMODE, "primary not restorable in another mode (0x%08lx)", hr);
            dd->RestoreDisplayMode();
            hr = prim->Restore();
            CHECK(hr == DD_OK, "primary restorable in its own mode again (0x%08lx)", hr);
        }
        IDirectDrawSurface7* dup = nullptr;
        hr = dd->CreateSurface(&d, &dup, nullptr);
        CHECK(hr == DDERR_PRIMARYSURFACEALREADYEXISTS, "second primary refused");
        if (prim) prim->Release();
    }

    s->Release();
    src->Release();
    two->Release();
    ov->Release();
    s16->Release();
    c32->Release();
    ULONG left = dd->Release();
    CHECK(left == 0, "DirectDraw released (refcount %lu)", left);
    DestroyWindow(hwnd);
    return 0;
}

// "pattern <w> <h>": a still test image in an exclusive 8-bit mode, held for a few seconds so a
// screenshot can compare the scaling filters (1-pixel lines, a checkerboard, a diagonal).
static int RunPattern(int modeW, int modeH) {
    HWND hwnd = MakeWindow(WS_POPUP, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    if (!hwnd) return 1;
    IDirectDraw7* dd = nullptr;
    if (FAILED(DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr))) return 1;
    dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    CHECK(SUCCEEDED(dd->SetDisplayMode(modeW, modeH, 8, 0, 0)), "pattern: SetDisplayMode %dx%dx8", modeW, modeH);
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* front = nullptr;
    CHECK(SUCCEEDED(dd->CreateSurface(&d, &front, nullptr)), "pattern: primary");
    if (!front) return 1;
    PALETTEENTRY pe[256] = {};
    pe[1] = {255, 255, 255, 0};
    pe[2] = {255, 0, 0, 0};
    IDirectDrawPalette* pal = nullptr;
    dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, nullptr);
    front->SetPalette(pal);
    d = {};
    d.dwSize = sizeof(d);
    if (SUCCEEDED(front->Lock(nullptr, &d, DDLOCK_WAIT, nullptr))) {
        for (int y = 0; y < modeH; y++) {
            BYTE* row = static_cast<BYTE*>(d.lpSurface) + y * d.lPitch;
            for (int x = 0; x < modeW; x++) {
                BYTE c = 0;
                if (x < modeW / 2) c = ((x / 4 + y / 4) & 1) ? 1 : 0;      // 4-pixel checkerboard
                else if ((x & 7) == 0 || (y & 7) == 0) c = 1;                // 1-pixel grid
                if (x == y) c = 2;                                           // diagonal
                row[x] = c;
            }
        }
        front->Unlock(nullptr);
    }
    for (int i = 0; i < 40; i++) {  // ~4 s for the screenshot
        Pump();
        Sleep(100);
    }
    if (pal) pal->Release();
    front->Release();
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

static int RunFullscreen8(int modeW, int modeH) {
    HWND hwnd = MakeWindow(WS_POPUP, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    const DWORD createError = hwnd ? 0 : GetLastError();
    CHECK(hwnd != nullptr, "test window created (error %lu)", createError);
    if (!hwnd) return 1;  // no display: nothing below can work
    IDirectDraw* dd1 = nullptr;
    HRESULT hr = DirectDrawCreate(nullptr, &dd1, nullptr);
    CHECK(SUCCEEDED(hr), "DirectDrawCreate");
    IDirectDraw7* dd = nullptr;
    dd1->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&dd));
    dd1->Release();
    hr = dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    CHECK(SUCCEEDED(hr), "exclusive mode -> 0x%08lx (hwnd %p, style 0x%08lx)", hr, static_cast<void*>(hwnd),
          GetWindowLongA(hwnd, GWL_STYLE));
    // Exclusive mode subclasses the window (focus handling), forwarding everything else.
    CHECK(GetWindowLongPtrA(hwnd, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(WndProc), "window subclassed in exclusive mode");
    SendMessageA(hwnd, WM_USER + 1, 0, 0);
    CHECK(g_userMsgs == 1, "subclassed window still forwards messages");
    SendMessageA(hwnd, WM_SYSCOMMAND, SC_SCREENSAVE, 0);
    CHECK(g_screensaverMsgs == 0, "screensaver blocked in exclusive mode");
    CHECK(SUCCEEDED(dd->SetDisplayMode(modeW, modeH, 8, 0, 0)), "SetDisplayMode %dx%dx8", modeW, modeH);
    {
        // An 8-bit mode makes the screen a palette device, as on XP (FlyingSB sets its palette
        // up only then).
        HDC screen = GetDC(nullptr);
        CHECK(GetDeviceCaps(screen, BITSPIXEL) == 8 && (GetDeviceCaps(screen, RASTERCAPS) & RC_PALETTE) &&
                  GetDeviceCaps(screen, SIZEPALETTE) == 256 && GetDeviceCaps(screen, NUMRESERVED) == 20,
              "GDI: an 8-bit palette device in the 8-bit mode (bits %d, raster caps 0x%x, palette %d)",
              GetDeviceCaps(screen, BITSPIXEL), GetDeviceCaps(screen, RASTERCAPS), GetDeviceCaps(screen, SIZEPALETTE));
        ReleaseDC(nullptr, screen);
    }
    // A game changing its window's frame in exclusive mode (JJ2+ while starting up): the
    // client area still covers the screen.
    // Then sizing it to its own idea of fullscreen (Unreal after a style change): still covering.
    for (LONG style : {LONG(WS_OVERLAPPEDWINDOW), LONG(WS_POPUP), LONG(WS_OVERLAPPEDWINDOW)}) {
        static int pass = 0;
        SetWindowLongA(hwnd, GWL_STYLE, style | WS_VISIBLE);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        if (pass++ == 2) SetWindowPos(hwnd, nullptr, 0, 0, modeW, modeH - 40, SWP_NOZORDER);
        Pump();
        // The real window (dd2d9 shows a scaled mode's window to the game as its screen).
        using RectFn = BOOL(WINAPI*)(HWND, RECT*);
        using MapFn = int(WINAPI*)(HWND, HWND, POINT*, UINT);
        HMODULE user32 = GetModuleHandleA("user32.dll");
        const auto realClientRect = reinterpret_cast<RectFn>(RealExport(user32, "GetClientRect"));
        const auto realMap = reinterpret_cast<MapFn>(RealExport(user32, "MapWindowPoints"));
        RECT cr;
        realClientRect(hwnd, &cr);
        realMap(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        // (The real monitor too: dd2d9 shows the game its mode as the monitor.)
        using MonFn = BOOL(WINAPI*)(HMONITOR, MONITORINFO*);
        const auto realMonitorInfo = reinterpret_cast<MonFn>(RealExport(user32, "GetMonitorInfoA"));
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        realMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        CHECK(EqualRect(&cr, &mi.rcMonitor), "client area covers the screen after a frame change (%ld,%ld-%ld,%ld)",
              cr.left, cr.top, cr.right, cr.bottom);
    }
    {
        // The frame taken away with SetWindowLong alone (Unreal Tournament, WS_VISIBLE with it):
        // the window covers the screen again for its new frame, which Windows then applies (under
        // Wine the old one left the image lower and to the right). Still without WS_VISIBLE.
        SetWindowLongA(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW & ~(WS_CAPTION | WS_THICKFRAME));
        Pump();
        using RectFn = BOOL(WINAPI*)(HWND, RECT*);
        using MonFn = BOOL(WINAPI*)(HMONITOR, MONITORINFO*);
        HMODULE user32 = GetModuleHandleA("user32.dll");
        const auto realWindowRect = reinterpret_cast<RectFn>(RealExport(user32, "GetWindowRect"));
        const auto realMonitorInfo = reinterpret_cast<MonFn>(RealExport(user32, "GetMonitorInfoA"));
        RECT wr = {};
        realWindowRect(hwnd, &wr);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        realMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        CHECK(EqualRect(&wr, &mi.rcMonitor) && !IsWindowVisible(hwnd),
              "frame taken away with SetWindowLong alone: the window is the screen (%ld,%ld-%ld,%ld), still hidden (%d)",
              wr.left, wr.top, wr.right, wr.bottom, IsWindowVisible(hwnd));
        SetWindowLongA(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        Pump();
    }

    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    d.dwBackBufferCount = 1;
    IDirectDrawSurface7* front = nullptr;
    hr = dd->CreateSurface(&d, &front, nullptr);
    CHECK(SUCCEEDED(hr), "flip chain -> 0x%08lx", hr);
    DDSCAPS2 caps = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
    IDirectDrawSurface7* back = nullptr;
    hr = front->GetAttachedSurface(&caps, &back);
    CHECK(SUCCEEDED(hr) && back, "GetAttachedSurface(BACKBUFFER)");
    if (!front || !back) return 1;

    PALETTEENTRY pe[256];
    for (int i = 0; i < 256; i++) pe[i] = {BYTE(i), BYTE((i * 3) & 255), BYTE(255 - i), 0};
    pe[0] = {0, 0, 40, 0};
    pe[255] = {255, 255, 255, 0};
    IDirectDrawPalette* pal = nullptr;
    dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, nullptr);
    front->SetPalette(pal);

    // Sprite: 64x64 disc on color-key 0
    d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
    d.dwWidth = 64;
    d.dwHeight = 64;
    IDirectDrawSurface7* sprite = nullptr;
    dd->CreateSurface(&d, &sprite, nullptr);
    d = {};
    d.dwSize = sizeof(d);
    sprite->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            int dx = x - 32, dy = y - 32;
            static_cast<BYTE*>(d.lpSurface)[y * d.lPitch + x] = dx * dx + dy * dy < 900 ? BYTE(160 + (x + y) / 4) : 0;
        }
    sprite->Unlock(nullptr);
    DDCOLORKEY key = {0, 0};
    sprite->SetColorKey(DDCKEY_SRCBLT, &key);
    {
        // A DC out on an 8-bit surface drawing with the display's palette follows a change to
        // it, as GDI on a palettized display did (Pal95: entry 1 made white, then its text
        // drawn white, with the DC out).
        IDirectDrawSurface7* strip = nullptr;
        DDSURFACEDESC2 sd = {};
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
        sd.dwWidth = 8;
        sd.dwHeight = 2;
        dd->CreateSurface(&sd, &strip, nullptr);
        HDC dc = nullptr;
        if (strip && SUCCEEDED(strip->GetDC(&dc))) {
            PALETTEENTRY c = {10, 200, 30, 0}, old = pe[1];
            pal->SetEntries(0, 1, 1, &c);
            SetPixel(dc, 0, 0, RGB(10, 200, 30));
            strip->ReleaseDC(dc);
            pal->SetEntries(0, 1, 1, &old);
            sd = {};
            sd.dwSize = sizeof(sd);
            strip->Lock(nullptr, &sd, DDLOCK_WAIT, nullptr);
            const BYTE got = static_cast<const BYTE*>(sd.lpSurface)[0];
            strip->Unlock(nullptr);
            CHECK(got == 1, "8-bit DC: a colour drawn after the palette set it is its index (%u, want 1)", got);
        }
        // A DC taken while the surface is locked: what the game writes through the lock
        // with the DC out stays after ReleaseDC (on XP the DC draws into the memory itself).
        sd = {};
        sd.dwSize = sizeof(sd);
        if (strip && SUCCEEDED(strip->Lock(nullptr, &sd, DDLOCK_WAIT, nullptr))) {
            BYTE* px = static_cast<BYTE*>(sd.lpSurface);
            px[2] = 7;
            dc = nullptr;
            if (SUCCEEDED(strip->GetDC(&dc))) {
                px[3] = 9;  // through the lock, with the DC out
                SetPixel(dc, 4, 0, RGB(pe[1].peRed, pe[1].peGreen, pe[1].peBlue));
                strip->ReleaseDC(dc);
            }
            const BYTE a = px[2], b = px[3], g = px[4];
            strip->Unlock(nullptr);
            CHECK(a == 7 && b == 9 && g == 1, "8-bit DC out on a locked surface: writes through the lock kept (%u %u, want 7 9), "
                  "GDI's too (%u, want 1)", a, b, g);
        }
        if (strip) strip->Release();
    }

    // Frame (1st Flip) is checked against the pacing target.
    const int frames = 180;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < frames; i++) {
        Pump();
        // Background: vertical palette-index gradient
        d = {};
        d.dwSize = sizeof(d);
        back->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
        for (int y = 0; y < modeH; y++) memset(static_cast<BYTE*>(d.lpSurface) + y * d.lPitch, (y / 2) & 0x7F, modeW);
        back->Unlock(nullptr);
        for (int k = 0; k < 6; k++) {
            int x = (i * (3 + k) + k * 90) % (modeW - 64), y = (60 + k * 60 + ((i * 2) % 40)) % (modeH - 64);
            back->BltFast(x, y, sprite, nullptr, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT);
        }
        HDC dc;
        if (SUCCEEDED(back->GetDC(&dc))) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            char text[64];
            sprintf(text, "DD2D9 640x480x8  frame %d", i);
            TextOutA(dc, 20, 20, text, int(strlen(text)));
            back->ReleaseDC(dc);
        }
        front->Flip(nullptr, DDFLIP_WAIT);
    }
    QueryPerformanceCounter(&t1);
    double secs = double(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    DWORD hz = 60;
    dd->GetMonitorFrequency(&hz);
    CHECK(secs > frames / double(hz) * 0.8, "Flip paced to the refresh rate: %d frames in %.2fs (%.1f fps, %lu Hz)",
          frames, secs, frames / secs, hz);

    // Focus loss and return, as with Alt+Tab. When the display mode was really
    // switched, the game is minimized and the desktop mode restored; coming back
    // sets everything up again and presenting must resume.
    SendMessageA(hwnd, WM_ACTIVATEAPP, FALSE, 0);
    Pump();
    const bool minimized = IsIconic(hwnd) != FALSE;
    // In the background the primary is lost, but can still be locked (a deliberate
    // difference: Captain Claw writes through the pointer of a failed lock).
    {
        CHECK(front->IsLost() == DDERR_SURFACELOST, "primary lost in the background");
        DDSURFACEDESC2 ld = {};
        ld.dwSize = sizeof(ld);
        hr = front->Lock(nullptr, &ld, DDLOCK_WAIT, nullptr);
        CHECK(hr == DD_OK && ld.lpSurface, "primary still lockable in the background (0x%08lx)", hr);
        if (SUCCEEDED(hr)) front->Unlock(nullptr);
        // Its back buffer isn't, as with ddraw.dll (NFS Porsche Unleashed, finding it locked,
        // flipped without unlocking).
        DDSURFACEDESC2 bd = {};
        bd.dwSize = sizeof(bd);
        hr = back->Lock(nullptr, &bd, DDLOCK_WAIT, nullptr);
        CHECK(hr == DDERR_SURFACELOST && !bd.lpSurface, "back buffer not lockable in the background (0x%08lx)", hr);
        if (SUCCEEDED(hr)) back->Unlock(nullptr);
    }
    SendMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
    Pump();
    CHECK(!IsIconic(hwnd), "window restored after focus returns (was minimized: %d)", minimized);
    // The surfaces were lost meanwhile, as in ddraw.dll: the game restores them (display=switch,
    // even where the test display lacks the mode and dd2d9 shows it scaled).
    hr = front->Flip(nullptr, DDFLIP_WAIT);
    CHECK(hr == DDERR_SURFACELOST, "flip after focus returns: surface lost (0x%08lx)", hr);
    // Its other surfaces report lost too, as in ddraw.dll, but can still be drawn with (a
    // deliberate difference: dd2d9 keeps their pixels, and Captain Claw never restores some
    // of its sprites).
    hr = sprite->IsLost();
    CHECK(hr == DDERR_SURFACELOST, "offscreen sprite reported lost after the focus loss (0x%08lx)", hr);
    {
        IDirectDrawSurface7* tmp = nullptr;
        DDSURFACEDESC2 td = {};
        td.dwSize = sizeof(td);
        td.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        td.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
        td.dwWidth = 64;
        td.dwHeight = 64;
        hr = dd->CreateSurface(&td, &tmp, nullptr);
        if (SUCCEEDED(hr)) {
            hr = tmp->BltFast(0, 0, sprite, nullptr, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT);
            tmp->Release();
        }
    }
    CHECK(hr == DD_OK, "offscreen sprite still drawable after the focus loss (0x%08lx)", hr);
    hr = dd->TestCooperativeLevel();
    CHECK(hr == DD_OK, "cooperative level fine after focus returns (0x%08lx)", hr);
    hr = dd->RestoreAllSurfaces();
    CHECK(hr == DD_OK && front->IsLost() == DD_OK, "surfaces restored (0x%08lx)", hr);
    int flipErrors = 0;
    for (int i = 0; i < 60; i++) {
        Pump();
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = BYTE(i);
        back->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        if (FAILED(front->Flip(nullptr, DDFLIP_WAIT))) flipErrors++;
    }
    CHECK(flipErrors == 0, "flipping after focus returns (%d errors)", flipErrors);

    // flipcopy=auto: a flip leaves the back buffer the frame before, as ddraw.dll's; with
    // Direct3D Retained Mode loaded it gets the frame just shown (Retained Mode redraws only
    // the squares that changed, into whatever the back buffer holds: Biko's mosaic of two
    // frames). Without d3drm.dll again (unloaded), a flip is a flip again.
    {
        auto fill = [&](BYTE c) {
            DDBLTFX fx = {};
            fx.dwSize = sizeof(fx);
            fx.dwFillColor = c;
            back->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        };
        auto backPixel = [&] {
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            int c = -1;
            if (SUCCEEDED(back->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
                c = static_cast<const BYTE*>(d.lpSurface)[(modeH / 2) * d.lPitch + modeW / 2];
                back->Unlock(nullptr);
            }
            return c;
        };
        fill(21);
        front->Flip(nullptr, DDFLIP_WAIT);
        fill(22);
        front->Flip(nullptr, DDFLIP_WAIT);
        const int plain = backPixel();  // the frame before the one shown: 21
        const HMODULE rm = LoadLibraryA("d3drm.dll");
        int copied = -1, after = -1;
        if (rm) {
            fill(23);
            front->Flip(nullptr, DDFLIP_WAIT);
            copied = backPixel();  // the frame just shown: 23
            FreeLibrary(rm);
            fill(24);
            front->Flip(nullptr, DDFLIP_WAIT);
            after = backPixel();  // 23 again, as the frame before
        }
        CHECK(plain == 21 && (!rm || (copied == 23 && after == 23)),
              "after a flip the back buffer holds the frame before (%d: 21); with d3drm.dll loaded the one just shown "
              "(%d: 23), and the one before again once it is unloaded (%d: 23)%s",
              plain, copied, after, rm ? "" : " [no d3drm.dll here: not tried]");
    }

    // Hold the last frame so it can be captured.
    for (int i = 0; i < 300; i++) {
        Pump();
        Sleep(10);
    }
    sprite->Release();
    pal->Release();
    back->Release();
    front->Release();
    dd->Release();
    CHECK(GetWindowLongPtrA(hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(WndProc), "window procedure restored on release");
    DestroyWindow(hwnd);
    return 0;
}

static int RunWindowed() {
    HWND hwnd = MakeWindow(WS_OVERLAPPEDWINDOW, 400, 300);
    IDirectDraw7* dd = nullptr;
    DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    CHECK(SUCCEEDED(dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL)), "normal mode");
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* prim = nullptr;
    dd->CreateSurface(&d, &prim, nullptr);
    IDirectDrawClipper* clip = nullptr;
    dd->CreateClipper(0, &clip, nullptr);
    clip->SetHWnd(0, hwnd);
    prim->SetClipper(clip);

    IDirectDrawSurface7* img = Make32(dd, 64, 64);
    d = {};
    d.dwSize = sizeof(d);
    img->Lock(nullptr, &d, 0, nullptr);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) static_cast<DWORD*>(d.lpSurface)[y * d.lPitch / 4 + x] = (x * 4) << 16 | (y * 4) << 8 | 0x80;
    img->Unlock(nullptr);

    for (int i = 0; i < 400; i++) {
        Pump();
        RECT cr;
        GetClientRect(hwnd, &cr);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = 0x202040;
        prim->Blt(&cr, nullptr, nullptr, DDBLT_COLORFILL, &fx);
        RECT dst = {cr.left + 20 + (i % 200), cr.top + 40, cr.left + 20 + (i % 200) + 128, cr.top + 40 + 128};
        prim->Blt(&dst, img, nullptr, DDBLT_WAIT, nullptr);
        dd->WaitForVerticalBlank(DDWAITVB_BLOCKBEGIN, nullptr);
    }
    {
        // A second object in normal mode drawing into the same window, as a video renderer
        // does beside the game: its frame stays, the game's last one isn't shown again over
        // it (Romance of the Three Kingdoms VII's intro flickered).
        IDirectDraw7* dd2 = nullptr;
        DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd2), IID_IDirectDraw7, nullptr);
        dd2->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
        d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS;
        d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        IDirectDrawSurface7* prim2 = nullptr;
        dd2->CreateSurface(&d, &prim2, nullptr);
        IDirectDrawClipper* clip2 = nullptr;
        dd2->CreateClipper(0, &clip2, nullptr);
        clip2->SetHWnd(0, hwnd);
        prim2->SetClipper(clip2);
        d = {};
        d.dwSize = sizeof(d);
        prim2->GetSurfaceDesc(&d);
        RECT cr;
        GetClientRect(hwnd, &cr);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = d.ddpfPixelFormat.dwBBitMask;
        const HRESULT hr = prim2->Blt(&cr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        int blue = 0, other = 0;
        for (int i = 0; i < 70; i++) {
            Pump();
            Sleep(25);
            if (i < 20) continue;  // (shown by then: its presenter's device made)
            HDC sdc = GetDC(nullptr);
            const COLORREF c = GetPixel(sdc, (cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2) & 0xFFFFFF;
            ReleaseDC(nullptr, sdc);
            (c == 0xFF0000 ? blue : other)++;
        }
        CHECK(SUCCEEDED(hr) && blue == 50 && !other,
              "two objects' primaries in one window: the newer frame stays (blue %d, other %d)", blue, other);
        clip2->Release();
        prim2->Release();
        dd2->Release();
    }
    {
        // The program's own GDI drawing into the window its normal-mode primary is shown in
        // stays: drawn into that primary, not covered by its older picture at the next refresh
        // (Swordman's windowed versions: StretchDIBits after a Bink movie in the primary).
        RECT cr;
        GetClientRect(hwnd, &cr);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = 0x400000;  // the "movie"
        prim->Blt(&cr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        for (int i = 0; i < 12; i++) {
            Pump();
            Sleep(25);
        }
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = 2;
        bi.bmiHeader.biHeight = -2;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        const DWORD green[4] = {0xFF00, 0xFF00, 0xFF00, 0xFF00};
        void* bits = nullptr;
        HBITMAP bm = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HDC mem = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(mem, bm);
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++) static_cast<DWORD*>(bits)[y * 2 + x] = 0xFF00FF;  // magenta
        const int w = cr.right - cr.left, h = cr.bottom - cr.top;
        const char* how[2] = {"StretchDIBits", "BitBlt"};
        for (int k = 0; k < 2; k++) {
            HDC wdc = GetDC(hwnd);
            BOOL drawn;
            if (k == 0) drawn = StretchDIBits(wdc, 0, 0, w, h, 0, 0, 2, 2, green, &bi, DIB_RGB_COLORS, SRCCOPY) == 2;
            else drawn = StretchBlt(wdc, 0, 0, w / 2, h, mem, 0, 0, 2, 2, SRCCOPY) && BitBlt(wdc, w / 2, 0, 2, 2, mem, 0, 0, SRCCOPY);
            ReleaseDC(hwnd, wdc);
            const COLORREF want = k == 0 ? 0x00FF00 : 0xFF00FF;
            int ok = 0, other = 0;
            COLORREF seen = 0;
            for (int i = 0; i < 40; i++) {  // a second: four refreshes
                Pump();
                Sleep(25);
                HDC sdc = GetDC(nullptr);
                const COLORREF c = GetPixel(sdc, cr.left + w / 4, cr.top + h / 2) & 0xFFFFFF;
                ReleaseDC(nullptr, sdc);
                if (c == want) ok++;
                else {
                    other++;
                    seen = c;
                }
            }
            CHECK(drawn && ok == 40, "the program's %s into its window stays over the primary (%d of 40, other 0x%06lx)",
                  how[k], ok, seen);
        }
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(bm);
    }
    img->Release();
    clip->Release();
    prim->Release();
    dd->Release();
    DestroyWindow(hwnd);
    CHECK(true, "windowed run finished");
    return 0;
}

// Lifetimes of complex surfaces, as in ddraw: each member counts its own
// references, and releasing the root destroys the structure. A member the game
// still references stays usable on its own until released.
// Direct3D 1 execute buffers: Pick (the triangle under a point, its offset and depth) and
// D3DOP_SPAN (drawn on XP's driver path).
static int RunExecute() {
    IDirectDraw* dd = nullptr;
    if (FAILED(DirectDrawCreate(nullptr, &dd, nullptr))) return 1;
    HWND hwnd = MakeWindow(WS_OVERLAPPEDWINDOW, 320, 240);
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    DDSURFACEDESC sd = {};
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    sd.dwWidth = sd.dwHeight = 64;
    sd.ddpfPixelFormat.dwSize = sizeof(sd.ddpfPixelFormat);
    sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
    sd.ddpfPixelFormat.dwRGBBitCount = 32;
    sd.ddpfPixelFormat.dwRBitMask = 0xFF0000;
    sd.ddpfPixelFormat.dwGBitMask = 0xFF00;
    sd.ddpfPixelFormat.dwBBitMask = 0xFF;
    IDirectDrawSurface* rt = nullptr;
    IDirect3D* d3d = nullptr;
    IDirect3DDevice* dev = nullptr;
    IDirect3DViewport* vp = nullptr;
    IDirect3DExecuteBuffer* eb = nullptr;
    HRESULT hr = dd->CreateSurface(&sd, &rt, nullptr);
    if (SUCCEEDED(hr)) hr = dd->QueryInterface(IID_IDirect3D, reinterpret_cast<void**>(&d3d));
    if (SUCCEEDED(hr)) hr = rt->QueryInterface(IID_IDirect3DHALDevice, reinterpret_cast<void**>(&dev));
    if (SUCCEEDED(hr)) hr = d3d->CreateViewport(&vp, nullptr);
    if (SUCCEEDED(hr)) hr = dev->AddViewport(vp);
    D3DVIEWPORT vd = {};
    vd.dwSize = sizeof(vd);
    vd.dwWidth = vd.dwHeight = 64;
    vd.dvScaleX = vd.dvScaleY = 32.0f;
    vd.dvMaxX = vd.dvMaxY = 1.0f;
    vd.dvMaxZ = 1.0f;
    if (SUCCEEDED(hr)) hr = vp->SetViewport(&vd);
    D3DEXECUTEBUFFERDESC ed = {};
    ed.dwSize = sizeof(ed);
    ed.dwFlags = D3DDEB_BUFSIZE;
    ed.dwBufferSize = 1024;
    if (SUCCEEDED(hr)) hr = dev->CreateExecuteBuffer(&ed, &eb, nullptr);
    CHECK(SUCCEEDED(hr), "Direct3D 1 device, viewport and execute buffer (0x%08lx)", hr);
    if (FAILED(hr)) {
        if (eb) eb->Release();
        if (vp) vp->Release();
        if (dev) dev->Release();
        if (d3d) d3d->Release();
        if (rt) rt->Release();
        dd->Release();
        DestroyWindow(hwnd);
        return 1;
    }
    // Three pretransformed vertices (a triangle in the top-left half, z 0.5) and two for a span.
    ed.dwSize = sizeof(ed);
    eb->Lock(&ed);
    auto* base = static_cast<BYTE*>(ed.lpData);
    auto* v = reinterpret_cast<D3DTLVERTEX*>(base);
    const D3DTLVERTEX tv[5] = {{{10}, {10}, {0.5f}, {1}, {0xFFFFFFFF}, {0}, {0}, {0}},
                               {{50}, {10}, {0.5f}, {1}, {0xFFFFFFFF}, {0}, {0}, {0}},
                               {{10}, {50}, {0.5f}, {1}, {0xFFFFFFFF}, {0}, {0}, {0}},
                               {{60}, {60}, {0.5f}, {1}, {0xFF00FF00}, {0}, {0}, {0}},
                               {{61}, {60}, {0.5f}, {1}, {0xFF00FF00}, {0}, {0}, {0}}};
    memcpy(v, tv, sizeof(tv));
    BYTE* p = base + sizeof(tv);
    const DWORD instrStart = DWORD(p - base);
    auto ins = [&](BYTE op, BYTE size, WORD count) {
        D3DINSTRUCTION i = {op, size, count};
        memcpy(p, &i, sizeof(i));
        p += sizeof(i);
    };
    ins(D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
    D3DPROCESSVERTICES pv = {D3DPROCESSVERTICES_COPY, 0, 0, 5};
    memcpy(p, &pv, sizeof(pv));
    p += sizeof(pv);
    ins(D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 1);
    const DWORD triOffset = DWORD(p - base) - instrStart;
    D3DTRIANGLE t = {};
    t.v1 = 0, t.v2 = 1, t.v3 = 2;
    t.wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE;
    memcpy(p, &t, sizeof(t));
    p += sizeof(t);
    ins(D3DOP_SPAN, sizeof(D3DSPAN), 1);
    D3DSPAN sp = {2, 3};
    memcpy(p, &sp, sizeof(sp));
    p += sizeof(sp);
    ins(D3DOP_EXIT, 0, 0);
    eb->Unlock();
    D3DEXECUTEDATA xd = {};
    xd.dwSize = sizeof(xd);
    xd.dwVertexCount = 5;
    xd.dwInstructionOffset = instrStart;
    xd.dwInstructionLength = DWORD(p - base) - instrStart;
    eb->SetExecuteData(&xd);

    D3DRECT in = {20, 20, 21, 21}, out = {55, 55, 56, 56};
    hr = dev->Pick(eb, vp, 0, &in);
    DWORD n = 0;
    dev->GetPickRecords(&n, nullptr);
    D3DPICKRECORD rec[4] = {};
    DWORD room = 4;
    if (n) dev->GetPickRecords(&room, rec);
    CHECK(SUCCEEDED(hr) && n == 1 && rec[0].bOpcode == D3DOP_TRIANGLE && rec[0].dwOffset == triOffset &&
              rec[0].dvZ > 0.49f && rec[0].dvZ < 0.51f,
          "Pick inside a triangle: %lu record(s), opcode %u, offset %lu (expected %lu), z %.3f", n, rec[0].bOpcode,
          rec[0].dwOffset, triOffset, rec[0].dvZ);
    dev->Pick(eb, vp, 0, &out);
    n = 9;
    dev->GetPickRecords(&n, nullptr);
    CHECK(n == 0, "Pick outside it: %lu records", n);

    // A triangle the clip volume cuts (a vertex far left): picked in its visible part. Lit
    // vertices transformed by the default (identity) matrices; the viewport maps -1..1 to 0..64.
    {
        IDirect3DExecuteBuffer* eb2 = nullptr;
        D3DEXECUTEBUFFERDESC ed2 = {};
        ed2.dwSize = sizeof(ed2);
        ed2.dwFlags = D3DDEB_BUFSIZE;
        ed2.dwBufferSize = 512;
        if (SUCCEEDED(dev->CreateExecuteBuffer(&ed2, &eb2, nullptr)) && SUCCEEDED(eb2->Lock(&ed2))) {
            BYTE* b2 = static_cast<BYTE*>(ed2.lpData);
            const D3DLVERTEX lv[3] = {{{-3.0f}, {1.0f}, {0.5f}, 0, {0xFFFFFFFF}, {0}, {0}, {0}},
                                      {{1.0f}, {1.0f}, {0.5f}, 0, {0xFFFFFFFF}, {0}, {0}, {0}},
                                      {{1.0f}, {-1.0f}, {0.5f}, 0, {0xFFFFFFFF}, {0}, {0}, {0}}};
            memcpy(b2, lv, sizeof(lv));
            p = b2 + sizeof(lv);
            const DWORD start2 = DWORD(p - b2);
            ins(D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
            D3DPROCESSVERTICES pv2 = {D3DPROCESSVERTICES_TRANSFORM, 0, 0, 3};
            memcpy(p, &pv2, sizeof(pv2));
            p += sizeof(pv2);
            ins(D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 1);
            D3DTRIANGLE t2 = {};
            t2.v1 = 0, t2.v2 = 1, t2.v3 = 2;
            t2.wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE;
            memcpy(p, &t2, sizeof(t2));
            p += sizeof(t2);
            ins(D3DOP_EXIT, 0, 0);
            eb2->Unlock();
            D3DEXECUTEDATA xd2 = {};
            xd2.dwSize = sizeof(xd2);
            xd2.dwVertexCount = 3;
            xd2.dwInstructionOffset = start2;
            xd2.dwInstructionLength = DWORD(p - b2) - start2;
            eb2->SetExecuteData(&xd2);
            D3DRECT visible = {48, 16, 49, 17}, beyond = {8, 40, 9, 41};
            dev->Pick(eb2, vp, 0, &visible);
            n = 0;
            dev->GetPickRecords(&n, nullptr);
            room = 4;
            if (n) dev->GetPickRecords(&room, rec);
            const DWORD n2 = n;
            dev->Pick(eb2, vp, 0, &beyond);
            n = 9;
            dev->GetPickRecords(&n, nullptr);
            CHECK(n2 == 1 && rec[0].dvZ > 0.4f && rec[0].dvZ < 0.6f && n == 0,
                  "Pick a triangle the clipper cuts: %lu record(s) in its visible part (z %.3f), %lu outside it", n2,
                  rec[0].dvZ, n);
        }
        if (eb2) eb2->Release();
    }

    // The span is drawn on XP's driver path: executed unclipped, or (as here) clipped after a
    // PROCESSVERTICES COPY. (Clipped otherwise it's skipped.)
    auto pixel = [&](int x, int y) {
        DDSURFACEDESC l = {};
        l.dwSize = sizeof(l);
        DWORD c = 0;
        if (SUCCEEDED(rt->Lock(nullptr, &l, DDLOCK_WAIT, nullptr))) {
            c = reinterpret_cast<const DWORD*>(static_cast<BYTE*>(l.lpSurface) + y * l.lPitch)[x] & 0xFFFFFF;
            rt->Unlock(nullptr);
        }
        return c;
    };
    DDBLTFX fx = {};
    fx.dwSize = sizeof(fx);
    rt->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    dev->BeginScene();
    dev->Execute(eb, vp, D3DEXECUTE_CLIPPED);
    dev->EndScene();
    const DWORD clippedSpan = pixel(60, 60), tri = pixel(20, 20);
    rt->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    dev->BeginScene();
    dev->Execute(eb, vp, D3DEXECUTE_UNCLIPPED);
    dev->EndScene();
    const DWORD unclippedSpan = pixel(60, 60);
    CHECK(tri == 0xFFFFFF && clippedSpan == 0x00FF00 && unclippedSpan == 0x00FF00,
          "D3DOP_SPAN after a COPY: clipped 0x%06lx, unclipped 0x%06lx (both drawn), triangle 0x%06lx", clippedSpan,
          unclippedSpan, tri);

    // The extents of a clipped execute grow by the part of a triangle that is drawn, where
    // the clip volume cuts it: a quad over the whole viewport whose corners are all off it
    // reports the whole viewport. Retained Mode clears and redraws the squares the extents
    // touch (Biko's black squares). A quad wholly off to the left still reports nothing.
    {
        IDirect3DExecuteBuffer* eb3 = nullptr;
        D3DEXECUTEBUFFERDESC ed3 = {};
        ed3.dwSize = sizeof(ed3);
        ed3.dwFlags = D3DDEB_BUFSIZE;
        ed3.dwBufferSize = 512;
        D3DRECT whole = {}, left = {};
        bool ran = false;
        for (int pass = 0; pass < 2; pass++) {
            if (eb3) eb3->Release();
            eb3 = nullptr;
            if (FAILED(dev->CreateExecuteBuffer(&ed3, &eb3, nullptr)) || FAILED(eb3->Lock(&ed3))) break;
            BYTE* b3 = static_cast<BYTE*>(ed3.lpData);
            // Pass 0: corners at -3..3 (the viewport is -1..1). Pass 1: x from -9 to -3.
            const float x0 = pass ? -9.0f : -3.0f, x1 = pass ? -3.0f : 3.0f;
            const D3DLVERTEX qv[4] = {{{x0}, {3.0f}, {0.5f}, 0, {0xFFFFFFFF}, {0}, {0}, {0}},
                                      {{x1}, {3.0f}, {0.5f}, 0, {0xFFFFFFFF}, {0}, {0}, {0}},
                                      {{x0}, {-3.0f}, {0.5f}, 0, {0xFFFFFFFF}, {0}, {0}, {0}},
                                      {{x1}, {-3.0f}, {0.5f}, 0, {0xFFFFFFFF}, {0}, {0}, {0}}};
            memcpy(b3, qv, sizeof(qv));
            p = b3 + sizeof(qv);
            const DWORD start3 = DWORD(p - b3);
            ins(D3DOP_SETSTATUS, sizeof(D3DSTATUS), 1);
            D3DSTATUS st = {};
            st.dwFlags = D3DSETSTATUS_ALL;
            st.dwStatus = 0x01FFF000;
            st.drExtent = {2048, 2048, 0, 0};  // (empty, as Retained Mode resets it)
            memcpy(p, &st, sizeof(st));
            p += sizeof(st);
            ins(D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
            D3DPROCESSVERTICES pv3 = {D3DPROCESSVERTICES_TRANSFORM | D3DPROCESSVERTICES_UPDATEEXTENTS, 0, 0, 4};
            memcpy(p, &pv3, sizeof(pv3));
            p += sizeof(pv3);
            ins(D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 2);
            D3DTRIANGLE q[2] = {};
            q[0].v1 = 0, q[0].v2 = 1, q[0].v3 = 2, q[0].wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE;
            q[1].v1 = 2, q[1].v2 = 1, q[1].v3 = 3, q[1].wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE;
            memcpy(p, q, sizeof(q));
            p += sizeof(q);
            ins(D3DOP_EXIT, 0, 0);
            eb3->Unlock();
            D3DEXECUTEDATA xd3 = {};
            xd3.dwSize = sizeof(xd3);
            xd3.dwVertexCount = 4;
            xd3.dwInstructionOffset = start3;
            xd3.dwInstructionLength = DWORD(p - b3) - start3;
            eb3->SetExecuteData(&xd3);
            dev->BeginScene();
            const HRESULT xr = dev->Execute(eb3, vp, D3DEXECUTE_CLIPPED);
            dev->EndScene();
            D3DEXECUTEDATA got = {};
            got.dwSize = sizeof(got);
            eb3->GetExecuteData(&got);
            (pass ? left : whole) = got.dsStatus.drExtent;
            ran = SUCCEEDED(xr);
        }
        if (eb3) eb3->Release();
        CHECK(ran && whole.x1 == 0 && whole.y1 == 0 && whole.x2 == 64 && whole.y2 == 64 && left.x2 - left.x1 <= 1,
              "a clipped execute's extents count the drawn part of a triangle the viewport cuts: the quad over the "
              "viewport %ld,%ld-%ld,%ld (0,0-64,64), the one off it %ld,%ld-%ld,%ld (none)",
              whole.x1, whole.y1, whole.x2, whole.y2, left.x1, left.y1, left.x2, left.y2);
    }

    // A frame as Direct3D Retained Mode builds one: matrices by handle, a material and a light,
    // and one execute buffer that sets the transforms, the light state and render states, lights
    // a quad (TRANSFORMLIGHT), changes the world matrix behind its handle (MATRIXMULTIPLY), then
    // transforms lit vertices with it, branching over a quad on the status it has just set.
    {
        D3DMATRIX ident = {};
        ident._11 = ident._22 = ident._33 = ident._44 = 1.0f;
        D3DMATRIX move = ident;
        move._41 = 1.0f;  // one unit to the right
        D3DMATRIXHANDLE hWorld = 0, hView = 0, hProj = 0, hMove = 0;
        dev->CreateMatrix(&hWorld);
        dev->CreateMatrix(&hView);
        dev->CreateMatrix(&hProj);
        dev->CreateMatrix(&hMove);
        dev->SetMatrix(hWorld, &ident);
        dev->SetMatrix(hView, &ident);
        dev->SetMatrix(hProj, &ident);
        dev->SetMatrix(hMove, &move);
        IDirect3DMaterial* mat = nullptr;
        IDirect3DLight* light = nullptr;
        D3DMATERIALHANDLE hMat = 0;
        d3d->CreateMaterial(&mat, nullptr);
        d3d->CreateLight(&light, nullptr);
        D3DMATERIAL md = {};
        md.dwSize = sizeof(md);
        md.diffuse.g = md.diffuse.a = 1.0f;
        md.dwRampSize = 1;
        D3DLIGHT ld = {};
        ld.dwSize = sizeof(ld);
        ld.dltType = D3DLIGHT_DIRECTIONAL;
        ld.dcvColor.r = ld.dcvColor.g = ld.dcvColor.b = ld.dcvColor.a = 1.0f;
        ld.dvDirection.z = 1.0f;
        IDirect3DExecuteBuffer* eb4 = nullptr;
        D3DEXECUTEBUFFERDESC ed4 = {};
        ed4.dwSize = sizeof(ed4);
        ed4.dwFlags = D3DDEB_BUFSIZE;
        ed4.dwBufferSize = 2048;
        DWORD seen[4] = {0xBAD, 0xBAD, 0xBAD, 0xBAD};
        HRESULT xr = E_FAIL;
        if (mat && light && SUCCEEDED(mat->SetMaterial(&md)) && SUCCEEDED(mat->GetHandle(dev, &hMat)) &&
            SUCCEEDED(light->SetLight(&ld)) && SUCCEEDED(vp->AddLight(light)) &&
            SUCCEEDED(dev->CreateExecuteBuffer(&ed4, &eb4, nullptr)) && SUCCEEDED(eb4->Lock(&ed4))) {
            BYTE* b4 = static_cast<BYTE*>(ed4.lpData);
            // 0-3: unlit vertices facing the light, the left of the view. 4-15: lit ones, placed one
            // unit to the left of where the moved world matrix shows them: a blue quad (top
            // right), a white one over the whole view (branched over), a yellow one (bottom right).
            const D3DVERTEX uv[4] = {{{-1.0f}, {1.0f}, {0.5f}, {0}, {0}, {-1.0f}, {0}, {0}},
                                     {{-0.25f}, {1.0f}, {0.5f}, {0}, {0}, {-1.0f}, {0}, {0}},
                                     {{-1.0f}, {-1.0f}, {0.5f}, {0}, {0}, {-1.0f}, {0}, {0}},
                                     {{-0.25f}, {-1.0f}, {0.5f}, {0}, {0}, {-1.0f}, {0}, {0}}};
            auto lit = [](float x, float y, DWORD c) { return D3DLVERTEX{{x}, {y}, {0.5f}, 0, {c}, {0}, {0}, {0}}; };
            const D3DLVERTEX lv[12] = {lit(-0.75f, 1, 0xFF0000FF), lit(-0.25f, 1, 0xFF0000FF), lit(-0.75f, 0, 0xFF0000FF),
                                       lit(-0.25f, 0, 0xFF0000FF), lit(-2, 1, 0xFFFFFFFF), lit(0, 1, 0xFFFFFFFF),
                                       lit(-2, -1, 0xFFFFFFFF), lit(0, -1, 0xFFFFFFFF), lit(-0.75f, -0.5f, 0xFFFFFF00),
                                       lit(-0.25f, -0.5f, 0xFFFFFF00), lit(-0.75f, -1, 0xFFFFFF00), lit(-0.25f, -1, 0xFFFFFF00)};
            memcpy(b4, uv, sizeof(uv));
            memcpy(b4 + sizeof(uv), lv, sizeof(lv));
            p = b4 + sizeof(uv) + sizeof(lv);
            const DWORD start4 = DWORD(p - b4);
            auto put = [&](const void* rec, size_t n) {
                memcpy(p, rec, n);
                p += n;
            };
            auto state = [&](DWORD type, DWORD arg) {
                D3DSTATE st = {};
                st.drstRenderStateType = D3DRENDERSTATETYPE(type);
                st.dwArg[0] = arg;
                put(&st, sizeof(st));
            };
            auto quadAt = [&](WORD first) {
                ins(D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 2);
                D3DTRIANGLE q[2] = {};
                q[0].v1 = first, q[0].v2 = WORD(first + 1), q[0].v3 = WORD(first + 2), q[0].wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE;
                q[1].v1 = WORD(first + 2), q[1].v2 = WORD(first + 1), q[1].v3 = WORD(first + 3), q[1].wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE;
                put(q, sizeof(q));
            };
            ins(D3DOP_STATETRANSFORM, sizeof(D3DSTATE), 3);
            state(D3DTRANSFORMSTATE_WORLD, hWorld);
            state(D3DTRANSFORMSTATE_VIEW, hView);
            state(D3DTRANSFORMSTATE_PROJECTION, hProj);
            ins(D3DOP_STATELIGHT, sizeof(D3DSTATE), 2);
            state(D3DLIGHTSTATE_MATERIAL, hMat);
            state(D3DLIGHTSTATE_AMBIENT, 0);
            ins(D3DOP_STATERENDER, sizeof(D3DSTATE), 3);
            state(D3DRENDERSTATE_ZENABLE, FALSE);
            state(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
            state(D3DRENDERSTATE_SHADEMODE, D3DSHADE_GOURAUD);
            ins(D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
            const D3DPROCESSVERTICES pvLight = {D3DPROCESSVERTICES_TRANSFORMLIGHT, 0, 0, 4, 0};
            put(&pvLight, sizeof(pvLight));
            quadAt(0);
            ins(D3DOP_MATRIXMULTIPLY, sizeof(D3DMATRIXMULTIPLY), 1);
            const D3DMATRIXMULTIPLY mm = {hWorld, hWorld, hMove};
            put(&mm, sizeof(mm));
            ins(D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
            const D3DPROCESSVERTICES pvMoved = {D3DPROCESSVERTICES_TRANSFORM, 4, 4, 12, 0};
            put(&pvMoved, sizeof(pvMoved));
            quadAt(4);
            ins(D3DOP_SETSTATUS, sizeof(D3DSTATUS), 1);
            D3DSTATUS st = {};
            st.dwFlags = D3DSETSTATUS_STATUS;
            st.dwStatus = D3DSTATUS_CLIPUNIONLEFT;
            put(&st, sizeof(st));
            // The status isn't 0: a branch on its being 0 isn't taken; the same one negated is,
            // over the white quad.
            const DWORD quadBytes = sizeof(D3DINSTRUCTION) + 2 * sizeof(D3DTRIANGLE);
            ins(D3DOP_BRANCHFORWARD, sizeof(D3DBRANCH), 1);
            const D3DBRANCH stay = {0xFFFFFFFF, 0, FALSE, 2 * (sizeof(D3DINSTRUCTION) + sizeof(D3DBRANCH)) + 2 * quadBytes};
            put(&stay, sizeof(stay));
            ins(D3DOP_BRANCHFORWARD, sizeof(D3DBRANCH), 1);
            const D3DBRANCH skip = {0xFFFFFFFF, 0, TRUE, sizeof(D3DINSTRUCTION) + sizeof(D3DBRANCH) + quadBytes};
            put(&skip, sizeof(skip));
            quadAt(8);
            quadAt(12);
            ins(D3DOP_EXIT, 0, 0);
            eb4->Unlock();
            D3DEXECUTEDATA xd4 = {};
            xd4.dwSize = sizeof(xd4);
            xd4.dwVertexCount = 16;
            xd4.dwInstructionOffset = start4;
            xd4.dwInstructionLength = DWORD(p - b4) - start4;
            eb4->SetExecuteData(&xd4);
            DDBLTFX black = {};
            black.dwSize = sizeof(black);
            rt->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &black);
            dev->BeginScene();
            xr = dev->Execute(eb4, vp, D3DEXECUTE_CLIPPED);
            dev->EndScene();
            DDSURFACEDESC ld4 = {};
            ld4.dwSize = sizeof(ld4);
            if (SUCCEEDED(rt->Lock(nullptr, &ld4, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
                static const int kAt[4][2] = {{10, 32}, {48, 16}, {48, 56}, {32, 40}};
                for (int k = 0; k < 4; k++)
                    seen[k] = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(ld4.lpSurface) + kAt[k][1] * ld4.lPitch)[kAt[k][0]] & 0xFFFFFF;
                rt->Unlock(nullptr);
            }
        }
        CHECK(SUCCEEDED(xr) && seen[0] == 0x00FF00 && seen[1] == 0x0000FF && seen[2] == 0xFFFF00 && seen[3] == 0,
              "a Retained Mode frame's execute buffer (0x%08lx): the lit quad 0x%06lx (green), the one after the world "
              "matrix moved 0x%06lx (blue), the one after the branches 0x%06lx (yellow), the one branched over 0x%06lx (none)",
              xr, seen[0], seen[1], seen[2], seen[3]);
        if (eb4) eb4->Release();
        if (light) {
            vp->DeleteLight(light);
            light->Release();
        }
        if (mat) mat->Release();
        for (D3DMATRIXHANDLE h : {hWorld, hView, hProj, hMove})
            if (h) dev->DeleteMatrix(h);
    }

    eb->Release();
    vp->Release();
    dev->Release();
    d3d->Release();
    rt->Release();
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

// A Direct3D 2 device whose render target was set to a Z buffer (refused, but the device has
// switched to it, without a reference) and the Z buffer released: state changes after that,
// and drawing again once it has a target.
static int RunTargetGone() {
    IDirectDraw* dd = nullptr;
    if (FAILED(DirectDrawCreate(nullptr, &dd, nullptr))) return 1;
    HWND hwnd = MakeWindow(WS_OVERLAPPEDWINDOW, 320, 240);
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    DDSURFACEDESC sd = {};
    sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    sd.dwWidth = sd.dwHeight = 64;
    sd.ddpfPixelFormat.dwSize = sizeof(sd.ddpfPixelFormat);
    sd.ddpfPixelFormat.dwFlags = DDPF_RGB;
    sd.ddpfPixelFormat.dwRGBBitCount = 32;
    sd.ddpfPixelFormat.dwRBitMask = 0xFF0000;
    sd.ddpfPixelFormat.dwGBitMask = 0xFF00;
    sd.ddpfPixelFormat.dwBBitMask = 0xFF;
    DDSURFACEDESC zd = {};
    zd.dwSize = sizeof(zd);
    zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_ZBUFFERBITDEPTH;
    zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
    zd.dwWidth = zd.dwHeight = 64;
    zd.dwZBufferBitDepth = 16;
    IDirectDrawSurface *rt = nullptr, *zs = nullptr;
    IDirect3D2* d3d = nullptr;
    IDirect3DDevice2* dev = nullptr;
    IDirect3DViewport2* vp = nullptr;
    HRESULT hr = dd->CreateSurface(&sd, &rt, nullptr);
    if (SUCCEEDED(hr)) hr = dd->CreateSurface(&zd, &zs, nullptr);
    if (SUCCEEDED(hr)) hr = dd->QueryInterface(IID_IDirect3D2, reinterpret_cast<void**>(&d3d));
    if (SUCCEEDED(hr)) hr = d3d->CreateDevice(IID_IDirect3DHALDevice, rt, &dev);
    if (SUCCEEDED(hr)) hr = d3d->CreateViewport(&vp, nullptr);
    if (SUCCEEDED(hr)) hr = dev->AddViewport(vp);
    D3DVIEWPORT2 vd = {};
    vd.dwSize = sizeof(vd);
    vd.dwWidth = vd.dwHeight = 64;
    vd.dvClipX = -1.0f, vd.dvClipY = 1.0f, vd.dvClipWidth = vd.dvClipHeight = 2.0f;
    vd.dvMaxZ = 1.0f;
    if (SUCCEEDED(hr)) hr = vp->SetViewport2(&vd);
    if (SUCCEEDED(hr)) hr = dev->SetCurrentViewport(vp);
    CHECK(SUCCEEDED(hr), "Direct3D 2 device, viewport and a Z buffer surface (0x%08lx)", hr);
    if (SUCCEEDED(hr)) {
        const HRESULT toZ = dev->SetRenderTarget(zs, 0);
        IDirectDrawSurface* now = nullptr;
        dev->GetRenderTarget(&now);
        const bool switched = now == zs;
        if (now) now->Release();
        const ULONG left = zs->Release();
        zs = nullptr;
        // (The texture stages are worked out again from these: with no target to look at.)
        dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_MODULATEALPHA);
        dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, TRUE);
        dev->SetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, 0);
        dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRENDERSTATE_COLORKEYENABLE, FALSE);
        dev->SetRenderState(D3DRENDERSTATE_TEXTUREMAPBLEND, D3DTBLEND_MODULATE);
        const HRESULT back = dev->SetRenderTarget(rt, 0);
        D3DTLVERTEX tri[3] = {{{4}, {4}, {0.5f}, {1}, {0xFFFFFFFF}, {0}, {0}, {0}},
                              {{60}, {4}, {0.5f}, {1}, {0xFFFFFFFF}, {0}, {0}, {0}},
                              {{4}, {60}, {0.5f}, {1}, {0xFFFFFFFF}, {0}, {0}, {0}}};
        dev->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
        dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
        dev->BeginScene();
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, tri, 3, 0);
        dev->EndScene();
        DWORD px = 0xBAD;
        DDSURFACEDESC l = {};
        l.dwSize = sizeof(l);
        if (SUCCEEDED(rt->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
            px = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(l.lpSurface) + 16 * l.lPitch)[16] & 0xFFFFFF;
            rt->Unlock(nullptr);
        }
        CHECK(toZ == DDERR_INVALIDPIXELFORMAT && switched && left == 0 && SUCCEEDED(back) && px == 0xFFFFFF,
              "SetRenderTarget to a Z buffer (0x%08lx, the device's target now: %d), the Z buffer released (%lu left), "
              "state changes, then the first target again (0x%08lx): a triangle drawn 0x%06lx (white)",
              toZ, switched, left, back, px);
    }
    if (vp) vp->Release();
    if (dev) dev->Release();
    if (d3d) d3d->Release();
    if (zs) zs->Release();
    if (rt) rt->Release();
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

// A texture format as a short name ("565", "P4", "A8L8"), and a device's list of them in order.
static void ListFormat(const DDPIXELFORMAT& f, std::string* list) {
    const char* name = "?";
    if (f.dwFlags & DDPF_PALETTEINDEXED4) name = "P4";
    else if (f.dwFlags & DDPF_PALETTEINDEXED8) name = "P8";
    else if (f.dwFlags == DDPF_LUMINANCE) name = f.dwLuminanceBitCount == 8 ? "L8" : "?";
    else if (f.dwFlags == (DDPF_LUMINANCE | DDPF_ALPHAPIXELS)) name = f.dwLuminanceBitCount == 16 ? "A8L8" : "A4L4";
    else if (f.dwFlags == DDPF_ALPHA) name = "A";
    else if (f.dwFlags == DDPF_RGB && f.dwRGBBitCount == 32) name = "X8";
    else if (f.dwFlags == (DDPF_RGB | DDPF_ALPHAPIXELS) && f.dwRGBBitCount == 32) name = "A8";
    else if (f.dwFlags == DDPF_RGB && f.dwRGBBitCount == 16) name = f.dwGBitMask == 0x07E0 ? "565" : "555";
    else if (f.dwFlags == (DDPF_RGB | DDPF_ALPHAPIXELS) && f.dwRGBBitCount == 16)
        name = f.dwRGBAlphaBitMask == 0x8000 ? "1555" : "4444";
    if (!list->empty()) *list += ' ';
    *list += name;
}
static HRESULT WINAPI ListFormatCb(DDPIXELFORMAT* f, void* list) {
    ListFormat(*f, static_cast<std::string*>(list));
    return D3DENUMRET_OK;
}
static HRESULT WINAPI ListFormatCb2(DDSURFACEDESC* d, void* list) {
    ListFormat(d->ddpfPixelFormat, static_cast<std::string*>(list));
    return D3DENUMRET_OK;
}

// 1-, 2- and 4-bit palettized surfaces, luminance and alpha-only textures: what ddraw.dll makes
// of them (memory to lock, a palette of their size, a DC where GDI has the format and the
// surface a palette; no blits for the low depths, copies between the same format for the others).
static int RunFormats() {
    IDirectDraw7* dd = nullptr;
    if (FAILED(DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr))) return 1;
    HWND hwnd = MakeWindow(WS_OVERLAPPEDWINDOW, 320, 240);
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    auto make = [&](const DDPIXELFORMAT& pf, int w, int h, HRESULT* hrOut = nullptr, bool texture = false) {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        d.ddsCaps.dwCaps = (texture ? DDSCAPS_TEXTURE : DDSCAPS_OFFSCREENPLAIN) | DDSCAPS_SYSTEMMEMORY;
        d.dwWidth = DWORD(w);
        d.dwHeight = DWORD(h);
        d.ddpfPixelFormat = pf;
        d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        IDirectDrawSurface7* s = nullptr;
        const HRESULT hr = dd->CreateSurface(&d, &s, nullptr);
        if (hrOut) *hrOut = hr;
        return s;
    };
    auto indexed = [](int bits, DWORD count) {
        DDPIXELFORMAT pf = {};
        pf.dwFlags = DDPF_RGB | (bits == 1 ? DDPF_PALETTEINDEXED1 : bits == 2 ? DDPF_PALETTEINDEXED2 : DDPF_PALETTEINDEXED4);
        pf.dwRGBBitCount = count;
        return pf;
    };
    // Rows of `n` bytes written through Lock and read back through another.
    auto rows = [](IDirectDrawSurface7* s, int h, int n, BYTE seed, LONG* pitch) {
        DDSURFACEDESC2 l = {};
        l.dwSize = sizeof(l);
        if (FAILED(s->Lock(nullptr, &l, DDLOCK_WAIT, nullptr))) return false;
        *pitch = l.lPitch;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < n; x++) (static_cast<BYTE*>(l.lpSurface) + y * l.lPitch)[x] = BYTE(seed + y * 16 + x);
        s->Unlock(nullptr);
        bool same = SUCCEEDED(s->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr));
        if (!same) return false;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < n; x++) same = same && (static_cast<BYTE*>(l.lpSurface) + y * l.lPitch)[x] == BYTE(seed + y * 16 + x);
        s->Unlock(nullptr);
        return same;
    };
    for (int bits : {1, 2, 4}) {
        HRESULT hr = E_FAIL;
        IDirectDrawSurface7* s = make(indexed(bits, DWORD(bits)), 20, 3, &hr);
        const int rowBytes = (20 * bits + 7) / 8;
        DDPIXELFORMAT got = {};
        got.dwSize = sizeof(got);
        LONG pitch = 0;
        const bool kept = s && SUCCEEDED(s->GetPixelFormat(&got)) && rows(s, 3, rowBytes, BYTE(bits), &pitch);
        CHECK(kept && got.dwFlags == indexed(bits, 0).dwFlags && got.dwRGBBitCount == DWORD(bits) && pitch >= rowBytes,
              "a %d-bit palettized surface (0x%08lx): format flags 0x%lx, %lu bits, pitch %ld for rows of %d bytes, rows kept %d",
              bits, hr, got.dwFlags, got.dwRGBBitCount, pitch, rowBytes, kept);
        if (!s) continue;
        // No DC before it has a palette (the display isn't palettized).
        HDC bare = nullptr;
        const HRESULT noPal = s->GetDC(&bare);
        if (SUCCEEDED(noPal)) s->ReleaseDC(bare);
        CHECK(noPal == DDERR_CANTCREATEDC, "its DC without a palette: 0x%08lx (DDERR_CANTCREATEDC)", noPal);
        // Its palette: one of its own size.
        PALETTEENTRY pe[256] = {};
        pe[1] = {10, 200, 30, 0};
        IDirectDrawPalette *own = nullptr, *big = nullptr;
        dd->CreatePalette(bits == 1 ? DDPCAPS_1BIT : bits == 2 ? DDPCAPS_2BIT : DDPCAPS_4BIT, pe, &own, nullptr);
        dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &big, nullptr);
        const HRESULT wrong = big ? s->SetPalette(big) : E_FAIL, right = own ? s->SetPalette(own) : E_FAIL;
        CHECK(wrong == DDERR_INVALIDPIXELFORMAT && right == DD_OK,
              "its palette: an 8-bit one 0x%08lx (DDERR_INVALIDPIXELFORMAT), one of %d entries 0x%08lx", wrong, 1 << bits, right);
        // No blits, as ddraw.dll's emulation layer: fills, copies out and in.
        IDirectDrawSurface7* twin = make(indexed(bits, DWORD(bits)), 20, 3);
        IDirectDrawSurface7* rgb = Make32(dd, 20, 3);
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        const HRESULT fill = s->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        const HRESULT same = twin ? twin->Blt(nullptr, s, nullptr, DDBLT_WAIT, nullptr) : E_FAIL;
        const HRESULT out = rgb ? rgb->Blt(nullptr, s, nullptr, DDBLT_WAIT, nullptr) : E_FAIL;
        const HRESULT in = rgb ? s->BltFast(0, 0, rgb, nullptr, DDBLTFAST_WAIT) : E_FAIL;
        CHECK(fill == DDERR_UNSUPPORTED && same == DDERR_UNSUPPORTED && out == DDERR_UNSUPPORTED && in == DDERR_UNSUPPORTED,
              "its blits are DDERR_UNSUPPORTED: a fill 0x%08lx, a copy to its like 0x%08lx, to 32 bits 0x%08lx, from 32 bits 0x%08lx",
              fill, same, out, in);
        // A DC where GDI has the format (1 and 4 bits), drawing with the surface's palette.
        DDSURFACEDESC2 l = {};
        l.dwSize = sizeof(l);
        if (SUCCEEDED(s->Lock(nullptr, &l, DDLOCK_WAIT, nullptr))) {
            memset(l.lpSurface, bits == 1 ? 0xFF : bits == 2 ? 0x55 : 0x11, size_t(l.lPitch) * 3);  // index 1 everywhere
            s->Unlock(nullptr);
        }
        HDC dc = nullptr;
        const HRESULT dr = s->GetDC(&dc);
        COLORREF c = 0xBAD;
        if (SUCCEEDED(dr)) {
            c = GetPixel(dc, 5, 1);
            s->ReleaseDC(dc);
        }
        CHECK(bits == 2 ? FAILED(dr) : SUCCEEDED(dr) && c == RGB(10, 200, 30),
              "its DC: 0x%08lx, a pixel of index 1 is 0x%06lx through it (%s)", dr, c,
              bits == 2 ? "none: GDI has no 2-bit bitmaps" : "the palette's 0x1ec80a");
        if (rgb) rgb->Release();
        if (twin) twin->Release();
        s->Release();
        if (big) big->Release();
        if (own) own->Release();
    }
    {
        HRESULT mixed = E_FAIL, count = E_FAIL, bare = E_FAIL;
        DDPIXELFORMAT two = indexed(4, 4), noRgb = indexed(4, 4);
        two.dwFlags |= DDPF_PALETTEINDEXED1;
        noRgb.dwFlags &= ~DWORD(DDPF_RGB);
        IDirectDrawSurface7* a = make(two, 8, 8, &mixed);
        IDirectDrawSurface7* b = make(indexed(4, 8), 8, 8, &count);
        IDirectDrawSurface7* c = make(noRgb, 8, 8, &bare);
        CHECK(!a && !b && !c && FAILED(mixed) && FAILED(count) && bare == DDERR_INVALIDPIXELFORMAT,
              "palettized formats refused: two depths flagged 0x%08lx, 4-bit flagged with a count of 8 0x%08lx, "
              "4-bit without DDPF_RGB 0x%08lx (DDERR_INVALIDPIXELFORMAT)", mixed, count, bare);
        if (a) a->Release();
        if (b) b->Release();
        if (c) c->Release();
        // 8 bits: palettized with both its flags; DDPF_RGB without masks is no format.
        DDPIXELFORMAT flagOnly = {}, noMasks = {}, both = {}, rgb332 = {};
        flagOnly.dwFlags = DDPF_PALETTEINDEXED8;
        noMasks.dwFlags = DDPF_RGB;
        both.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        flagOnly.dwRGBBitCount = noMasks.dwRGBBitCount = both.dwRGBBitCount = 8;
        HRESULT h1 = E_FAIL, h2 = E_FAIL, h3 = E_FAIL;
        a = make(flagOnly, 8, 8, &h1);
        b = make(noMasks, 8, 8, &h2);
        c = make(both, 8, 8, &h3);
        CHECK(!a && !b && c && h1 == DDERR_INVALIDPIXELFORMAT && h2 == DDERR_INVALIDPIXELFORMAT,
              "8 bits: palettized without DDPF_RGB 0x%08lx, DDPF_RGB without masks 0x%08lx (DDERR_INVALIDPIXELFORMAT), "
              "palettized with both flags 0x%08lx", h1, h2, h3);
        if (a) a->Release();
        if (b) b->Release();
        if (c) c->Release();
        // 8-bit RGB 3-3-2: a texture's format alone.
        rgb332.dwFlags = DDPF_RGB;
        rgb332.dwRGBBitCount = 8;
        rgb332.dwRBitMask = 0xE0;
        rgb332.dwGBitMask = 0x1C;
        rgb332.dwBBitMask = 0x03;
        a = make(rgb332, 8, 8, &h1);
        b = make(rgb332, 8, 8, &h2, true);
        CHECK(!a && h1 == DDERR_INVALIDPIXELFORMAT && b, "8-bit RGB 3-3-2: a plain surface 0x%08lx (DDERR_INVALIDPIXELFORMAT), a texture 0x%08lx",
              h1, h2);
        if (a) a->Release();
        if (b) b->Release();
    }
    // Luminance (with alpha or not) and alpha-only: textures only, as ddraw.dll.
    struct Kind {
        const char* name;
        DWORD flags, bits, lum, alpha;
    };
    static const Kind kKinds[] = {{"8-bit luminance", DDPF_LUMINANCE, 8, 0xFF, 0},
                                  {"luminance with alpha, 16 bits", DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 16, 0xFF, 0xFF00},
                                  {"luminance with alpha, 8 bits", DDPF_LUMINANCE | DDPF_ALPHAPIXELS, 8, 0x0F, 0xF0},
                                  {"alpha-only", DDPF_ALPHA, 8, 0, 0}};
    for (const Kind& k : kKinds) {
        DDPIXELFORMAT pf = {};
        pf.dwFlags = k.flags;
        pf.dwLuminanceBitCount = k.bits;  // (dwAlphaBitDepth, the same field)
        pf.dwLuminanceBitMask = k.lum;
        pf.dwLuminanceAlphaBitMask = k.alpha;
        HRESULT hr = E_FAIL, plainHr = E_FAIL;
        IDirectDrawSurface7* plain = make(pf, 8, 4, &plainHr);
        CHECK(!plain && plainHr == DDERR_INVALIDPIXELFORMAT, "a plain surface of %s: 0x%08lx (DDERR_INVALIDPIXELFORMAT)", k.name,
              plainHr);
        if (plain) plain->Release();
        IDirectDrawSurface7* s = make(pf, 8, 4, &hr, true);
        IDirectDrawSurface7* twin = make(pf, 8, 4, nullptr, true);
        IDirectDrawSurface7* rgb = Make32(dd, 8, 4);
        DDPIXELFORMAT got = {};
        got.dwSize = sizeof(got);
        LONG pitch = 0;
        const int rowBytes = int(8 * k.bits / 8);
        const bool kept = s && SUCCEEDED(s->GetPixelFormat(&got)) && rows(s, 4, rowBytes, 0x40, &pitch);
        CHECK(kept && got.dwFlags == k.flags && got.dwLuminanceBitCount == k.bits && got.dwLuminanceBitMask == k.lum &&
                  (k.flags == DDPF_ALPHA || got.dwLuminanceAlphaBitMask == k.alpha),
              "a texture of %s (0x%08lx): format flags 0x%lx, %lu bits, masks 0x%lx and 0x%lx, rows kept %d", k.name, hr,
              got.dwFlags, got.dwLuminanceBitCount, got.dwLuminanceBitMask, got.dwLuminanceAlphaBitMask, kept);
        // Copied as it is to another of its format; nothing converts it.
        const HRESULT copy = s && twin ? twin->Blt(nullptr, s, nullptr, DDBLT_WAIT, nullptr) : E_FAIL;
        bool copied = SUCCEEDED(copy);
        DDSURFACEDESC2 l = {};
        l.dwSize = sizeof(l);
        if (copied && SUCCEEDED(twin->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < rowBytes; x++)
                    copied = copied && (static_cast<BYTE*>(l.lpSurface) + y * l.lPitch)[x] == BYTE(0x40 + y * 16 + x);
            twin->Unlock(nullptr);
        }
        const HRESULT out = s && rgb ? rgb->Blt(nullptr, s, nullptr, DDBLT_WAIT, nullptr) : E_FAIL;
        const HRESULT in = s && rgb ? s->Blt(nullptr, rgb, nullptr, DDBLT_WAIT, nullptr) : E_FAIL;
        HDC dc = nullptr;
        const HRESULT dr = s ? s->GetDC(&dc) : E_FAIL;
        if (SUCCEEDED(dr)) s->ReleaseDC(dc);
        CHECK(copied && out == DDERR_UNSUPPORTED && in == DDERR_UNSUPPORTED && FAILED(dr),
              "its blits: to its like 0x%08lx (copied %d), to 32 bits 0x%08lx and from 32 bits 0x%08lx (DDERR_UNSUPPORTED); "
              "no DC (0x%08lx)", copy, copied, out, in, dr);
        if (rgb) rgb->Release();
        if (twin) twin->Release();
        if (s) s->Release();
    }
    // Not made: luminance flagged as RGB too (as ddraw.dll), and layouts dd2d9 has no texture
    // for (its own limit: the ones Direct3D 9 has are made).
    {
        DDPIXELFORMAT lumRgb = {}, alpha4 = {}, lum24 = {}, alphaRgb = {}, lumBump = {};
        lumRgb.dwFlags = DDPF_LUMINANCE | DDPF_RGB;
        lumRgb.dwLuminanceBitCount = 8;
        lumRgb.dwLuminanceBitMask = 0xFF;
        alpha4.dwFlags = DDPF_ALPHA;
        alpha4.dwAlphaBitDepth = 4;
        lum24.dwFlags = DDPF_LUMINANCE;
        lum24.dwLuminanceBitCount = 24;
        lum24.dwLuminanceBitMask = 0xFFFFFF;
        HRESULT h1 = E_FAIL, h2 = E_FAIL, h3 = E_FAIL;
        IDirectDrawSurface7* a = make(lumRgb, 8, 8, &h1, true);
        IDirectDrawSurface7* b = make(alpha4, 8, 8, &h2, true);
        IDirectDrawSurface7* c = make(lum24, 8, 8, &h3, true);
        CHECK(!a && !b && !c, "refused: luminance flagged with RGB 0x%08lx, 4-bit alpha-only 0x%08lx, 24-bit luminance 0x%08lx", h1, h2, h3);
        if (a) a->Release();
        if (b) b->Release();
        if (c) c->Release();
        // DDPF_ALPHA with another flag is DDERR_INVALIDPARAMS (as ddraw.dll); a bump map
        // flagged luminance too is no format.
        alphaRgb.dwFlags = DDPF_ALPHA | DDPF_RGB;
        alphaRgb.dwRGBBitCount = 16;
        alphaRgb.dwRBitMask = 0xF800;
        alphaRgb.dwGBitMask = 0x07E0;
        alphaRgb.dwBBitMask = 0x001F;
        lumBump.dwFlags = DDPF_LUMINANCE | DDPF_BUMPDUDV;
        lumBump.dwBumpBitCount = 16;
        lumBump.dwBumpDuBitMask = 0xFF;
        lumBump.dwBumpDvBitMask = 0xFF00;
        HRESULT h4 = E_FAIL, h5 = E_FAIL, h6 = E_FAIL;
        a = make(alphaRgb, 8, 8, &h4, true);
        b = make(alphaRgb, 8, 8, &h5);
        c = make(lumBump, 8, 8, &h6, true);
        CHECK(!a && !b && !c && h4 == DDERR_INVALIDPARAMS && h5 == DDERR_INVALIDPARAMS && h6 == DDERR_INVALIDPIXELFORMAT,
              "5-6-5 flagged alpha-only too: a texture 0x%08lx, a plain surface 0x%08lx (DDERR_INVALIDPARAMS); a bump map flagged "
              "luminance 0x%08lx (DDERR_INVALIDPIXELFORMAT)", h4, h5, h6);
        if (a) a->Release();
        if (b) b->Release();
        if (c) c->Release();
    }
    // The texture formats the devices list, in order: the software ones as Windows XP's, the
    // hardware ones with luminance from Direct3D 3 on.
    {
        const char* const kFull = "X8 A8 565 555 P4 P8 1555 4444 L8 A8L8";
        auto has = [](const std::string& list, const char* name) {
            return (" " + list + " ").find(std::string(" ") + name + " ") != std::string::npos;
        };
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        d.dwWidth = d.dwHeight = 32;
        IDirectDrawSurface7 *soft7 = nullptr, *hard7 = nullptr;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY;
        dd->CreateSurface(&d, &soft7, nullptr);
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
        dd->CreateSurface(&d, &hard7, nullptr);
        IDirect3D7* d3d7 = nullptr;
        dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d7));
        std::string rgb7, hal7;
        IDirect3DDevice7* dev7 = nullptr;
        if (d3d7 && soft7 && SUCCEEDED(d3d7->CreateDevice(IID_IDirect3DRGBDevice, soft7, &dev7))) {
            dev7->EnumTextureFormats(ListFormatCb, &rgb7);
            dev7->Release();
        }
        if (d3d7 && hard7 && SUCCEEDED(d3d7->CreateDevice(IID_IDirect3DHALDevice, hard7, &dev7))) {
            dev7->EnumTextureFormats(ListFormatCb, &hal7);
            dev7->Release();
        }
        CHECK(rgb7 == kFull, "Direct3D 7's RGB device lists: %s (%s)", rgb7.c_str(), kFull);
        // The RGB device's clear, as XP's software rasterizer leaves it: 4-bit channels and
        // the alpha bit scaled down and cut, the byte no channel has clear.
        struct Soft {
            const char* name;
            DWORD bits, r, g, b, a, first, second;  // held after 0x40c02010 and 0xc0c0c0c0
        };
        static const Soft kSoft[] = {
            {"1-5-5-5", 16, 0x7C00, 0x03E0, 0x001F, 0x8000, 0x6082, 0x6318},
            {"4-4-4-4", 16, 0x0F00, 0x00F0, 0x000F, 0xF000, 0x3B10, 0xBBBB},
            {"8-8-8 in 32 bits", 32, 0xFF0000, 0xFF00, 0xFF, 0, 0x00C02010, 0x00C0C0C0},
            {"8-8-8-8", 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000, 0x40C02010, 0xC0C0C0C0},
        };
        for (const Soft& k : kSoft) {
            DDSURFACEDESC2 td = d;
            td.dwFlags |= DDSD_PIXELFORMAT;
            td.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY;
            td.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            td.ddpfPixelFormat.dwFlags = DDPF_RGB | (k.a ? DDPF_ALPHAPIXELS : 0);
            td.ddpfPixelFormat.dwRGBBitCount = k.bits;
            td.ddpfPixelFormat.dwRBitMask = k.r;
            td.ddpfPixelFormat.dwGBitMask = k.g;
            td.ddpfPixelFormat.dwBBitMask = k.b;
            td.ddpfPixelFormat.dwRGBAlphaBitMask = k.a;
            IDirectDrawSurface7* target = nullptr;
            DWORD held[2] = {0xBAD, 0xBAD};
            dev7 = nullptr;
            if (d3d7 && SUCCEEDED(dd->CreateSurface(&td, &target, nullptr)) &&
                SUCCEEDED(d3d7->CreateDevice(IID_IDirect3DRGBDevice, target, &dev7))) {
                for (int i = 0; i < 2; i++) {
                    dev7->Clear(0, nullptr, D3DCLEAR_TARGET, i ? 0xC0C0C0C0 : 0x40C02010, 1.0f, 0);
                    DDSURFACEDESC2 l = {};
                    l.dwSize = sizeof(l);
                    if (FAILED(target->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) continue;
                    held[i] = k.bits == 16 ? *static_cast<const WORD*>(l.lpSurface) : *static_cast<const DWORD*>(l.lpSurface);
                    target->Unlock(nullptr);
                }
            }
            CHECK(held[0] == k.first && held[1] == k.second,
                  "Direct3D 7's RGB device clearing a %s target to 0x40c02010 leaves 0x%lx (0x%lx), to 0xc0c0c0c0 0x%lx (0x%lx)", k.name,
                  held[0], k.first, held[1], k.second);
            if (dev7) dev7->Release();
            if (target) target->Release();
        }
        CHECK(has(hal7, "L8") && has(hal7, "A8L8") && has(hal7, "A4L4") && has(hal7, "A") && !has(hal7, "P4"),
              "Direct3D 7's hardware device lists: %s (with L8, A8L8, A4L4 and A; without P4)", hal7.c_str());
        if (d3d7) d3d7->Release();
        if (hard7) hard7->Release();
        if (soft7) soft7->Release();
        // Direct3D 3 and 2, on a DirectDraw object of their time.
        IDirectDraw* dd1 = nullptr;
        IDirectDraw4* dd4 = nullptr;
        IDirect3D3* d3d3 = nullptr;
        IDirect3D2* d3d2 = nullptr;
        if (SUCCEEDED(DirectDrawCreate(nullptr, &dd1, nullptr))) {
            dd1->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
            dd1->QueryInterface(IID_IDirectDraw4, reinterpret_cast<void**>(&dd4));
            dd1->QueryInterface(IID_IDirect3D3, reinterpret_cast<void**>(&d3d3));
            dd1->QueryInterface(IID_IDirect3D2, reinterpret_cast<void**>(&d3d2));
        }
        std::string rgb3, hal3, rgb2, hal2, mmx2;
        DWORD cleared[2] = {0xBAD, 0xBAD};  // an 8-8-8-8 target after Clear2 to 0xc0c0c0c0: software, hardware
        for (int hard = 0; hard < 2 && dd4 && d3d3 && d3d2; hard++) {
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | (hard ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
            IDirectDrawSurface4* s4 = nullptr;
            IDirectDrawSurface* s1 = nullptr;
            if (FAILED(dd4->CreateSurface(&d, &s4, nullptr))) continue;
            s4->QueryInterface(IID_IDirectDrawSurface, reinterpret_cast<void**>(&s1));
            IDirect3DDevice3* dev3 = nullptr;
            if (SUCCEEDED(d3d3->CreateDevice(hard ? IID_IDirect3DHALDevice : IID_IDirect3DRGBDevice, s4, &dev3, nullptr))) {
                dev3->EnumTextureFormats(ListFormatCb, hard ? &hal3 : &rgb3);
                dev3->Release();
            }
            IDirect3DDevice2* dev2 = nullptr;
            if (s1 && SUCCEEDED(d3d2->CreateDevice(hard ? IID_IDirect3DHALDevice : IID_IDirect3DRGBDevice, s1, &dev2))) {
                dev2->EnumTextureFormats(ListFormatCb2, hard ? &hal2 : &rgb2);
                dev2->Release();
            }
            if (!hard && s1 && SUCCEEDED(d3d2->CreateDevice(IID_IDirect3DMMXDevice, s1, &dev2))) {
                dev2->EnumTextureFormats(ListFormatCb2, &mmx2);
                dev2->Release();
            }
            if (s1) s1->Release();
            s4->Release();
            // A clear through the viewport, into a target with alpha.
            DDSURFACEDESC2 ad = d;
            ad.dwFlags |= DDSD_PIXELFORMAT;
            ad.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            ad.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
            ad.ddpfPixelFormat.dwRGBBitCount = 32;
            ad.ddpfPixelFormat.dwRBitMask = 0xFF0000;
            ad.ddpfPixelFormat.dwGBitMask = 0xFF00;
            ad.ddpfPixelFormat.dwBBitMask = 0xFF;
            ad.ddpfPixelFormat.dwRGBAlphaBitMask = 0xFF000000;
            IDirectDrawSurface4* target = nullptr;
            IDirect3DViewport3* vp = nullptr;
            dev3 = nullptr;
            if (SUCCEEDED(dd4->CreateSurface(&ad, &target, nullptr)) &&
                SUCCEEDED(d3d3->CreateDevice(hard ? IID_IDirect3DHALDevice : IID_IDirect3DRGBDevice, target, &dev3, nullptr)) &&
                SUCCEEDED(d3d3->CreateViewport(&vp, nullptr)) && SUCCEEDED(dev3->AddViewport(vp))) {
                D3DVIEWPORT2 vd = {};
                vd.dwSize = sizeof(vd);
                vd.dwWidth = vd.dwHeight = 32;
                vd.dvClipX = -1.0f, vd.dvClipY = 1.0f, vd.dvClipWidth = vd.dvClipHeight = 2.0f;
                vd.dvMaxZ = 1.0f;
                vp->SetViewport2(&vd);
                dev3->SetCurrentViewport(vp);
                D3DRECT all = {{0}, {0}, {32}, {32}};
                vp->Clear2(1, &all, D3DCLEAR_TARGET, 0xC0C0C0C0, 1.0f, 0);
                DDSURFACEDESC2 l = {};
                l.dwSize = sizeof(l);
                if (SUCCEEDED(target->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) {
                    cleared[hard] = *static_cast<const DWORD*>(l.lpSurface);
                    target->Unlock(nullptr);
                }
            }
            if (vp) vp->Release();
            if (dev3) dev3->Release();
            if (target) target->Release();
        }
        CHECK(cleared[0] == 0x00C0C0C0 && cleared[1] == 0xC0C0C0C0,
              "Direct3D 3's Clear2 to 0xc0c0c0c0 on an 8-8-8-8 target: the RGB device leaves 0x%08lx (0x00c0c0c0: no alpha written), "
              "the hardware device 0x%08lx (0xc0c0c0c0)", cleared[0], cleared[1]);
        CHECK(rgb3 == kFull, "Direct3D 3's RGB device lists: %s (%s)", rgb3.c_str(), kFull);
        CHECK(has(hal3, "L8") && has(hal3, "A8L8") && !has(hal3, "A4L4") && !has(hal3, "A") && !has(hal3, "P4"),
              "Direct3D 3's hardware device lists: %s (with L8 and A8L8; without A4L4, A and P4)", hal3.c_str());
        CHECK(rgb2 == "X8 A8 565 555 P4 P8" && mmx2 == "P8", "Direct3D 2's RGB device lists: %s (X8 A8 565 555 P4 P8); its MMX one: %s (P8)",
              rgb2.c_str(), mmx2.c_str());
        CHECK(!hal2.empty() && !has(hal2, "L8") && !has(hal2, "A8L8") && !has(hal2, "P4"),
              "Direct3D 2's hardware device lists: %s (no luminance, no P4)", hal2.c_str());
        if (d3d2) d3d2->Release();
        if (d3d3) d3d3->Release();
        if (dd4) dd4->Release();
        if (dd1) dd1->Release();
    }
    const ULONG left = dd->Release();
    CHECK(left == 0, "DirectDraw released (refcount %lu)", left);
    DestroyWindow(hwnd);
    return 0;
}

// "desktop8", with desktopbpp=8 in dd2d9.ini: the desktop a 256-colour one to the game, through
// DirectDraw, GDI's device capabilities and the display settings alike.
static int RunDesktop8() {
    IDirectDraw7* dd = nullptr;
    if (FAILED(DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr))) return 1;
    HWND hwnd = MakeWindow(WS_OVERLAPPEDWINDOW, 320, 240);
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    DDSURFACEDESC2 mode = {};
    mode.dwSize = sizeof(mode);
    HRESULT hr = dd->GetDisplayMode(&mode);
    CHECK(SUCCEEDED(hr) && (mode.ddpfPixelFormat.dwFlags & DDPF_PALETTEINDEXED8) && mode.ddpfPixelFormat.dwRGBBitCount == 8,
          "desktopbpp=8: GetDisplayMode says %lu bits, format flags 0x%lx (8, palettized)", mode.ddpfPixelFormat.dwRGBBitCount,
          mode.ddpfPixelFormat.dwFlags);
    HDC screen = GetDC(nullptr);
    const int bits = GetDeviceCaps(screen, BITSPIXEL), raster = GetDeviceCaps(screen, RASTERCAPS),
              entries = GetDeviceCaps(screen, SIZEPALETTE);
    ReleaseDC(nullptr, screen);
    CHECK(bits == 8 && (raster & RC_PALETTE) && entries == 256,
          "desktopbpp=8: the screen's DC has %d bits a pixel, a palette %d, of %d entries (8, yes, 256)", bits,
          (raster & RC_PALETTE) != 0, entries);
    DEVMODEA dm = {};
    dm.dmSize = sizeof(dm);
    const BOOL got = EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm);
    // (The mode read put back: its made-up depth isn't asked of the real display.)
    const LONG back = got ? ChangeDisplaySettingsA(&dm, CDS_TEST) : -99;
    CHECK(got && dm.dmBitsPerPel == 8 && back == DISP_CHANGE_SUCCESSFUL,
          "desktopbpp=8: the current display settings say %lu bits (8); asking for them again gives %ld (0)", dm.dmBitsPerPel,
          back);
    // The primary and a plain surface are 8-bit palettized; the primary takes a palette, and
    // a surface without one has a DC (the screen is palettized).
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7 *prim = nullptr, *plain = nullptr;
    hr = dd->CreateSurface(&d, &prim, nullptr);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    d.dwWidth = d.dwHeight = 16;
    const HRESULT hr2 = dd->CreateSurface(&d, &plain, nullptr);
    DDPIXELFORMAT pf = {}, pf2 = {};
    pf.dwSize = pf2.dwSize = sizeof(pf);
    if (prim) prim->GetPixelFormat(&pf);
    if (plain) plain->GetPixelFormat(&pf2);
    PALETTEENTRY pe[256] = {};
    pe[7] = {200, 100, 50, 0};
    IDirectDrawPalette* pal = nullptr;
    dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, pe, &pal, nullptr);
    const HRESULT sp = prim && pal ? prim->SetPalette(pal) : E_FAIL;
    HDC dc = nullptr;
    const HRESULT dr = plain ? plain->GetDC(&dc) : E_FAIL;
    if (SUCCEEDED(dr)) plain->ReleaseDC(dc);
    CHECK(SUCCEEDED(hr) && SUCCEEDED(hr2) && (pf.dwFlags & DDPF_PALETTEINDEXED8) && (pf2.dwFlags & DDPF_PALETTEINDEXED8) &&
              sp == DD_OK && dr == DD_OK,
          "desktopbpp=8: the primary (0x%08lx, format flags 0x%lx) and a plain surface (0x%08lx, 0x%lx) are 8-bit palettized; "
          "SetPalette on the primary 0x%08lx, GetDC on the plain one without a palette 0x%08lx",
          hr, pf.dwFlags, hr2, pf2.dwFlags, sp, dr);
    // (A 4-bit one without a palette has none on a palettized display either.)
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    d.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED4;
    d.ddpfPixelFormat.dwRGBBitCount = 4;
    IDirectDrawSurface7* four = nullptr;
    dd->CreateSurface(&d, &four, nullptr);
    const HRESULT dr4 = four ? four->GetDC(&dc) : E_FAIL;
    if (SUCCEEDED(dr4)) four->ReleaseDC(dc);
    CHECK(dr4 == DDERR_CANTCREATEDC, "desktopbpp=8: GetDC on a 4-bit surface without a palette 0x%08lx (DDERR_CANTCREATEDC)", dr4);
    if (four) four->Release();
    // Drawn into the window through the primary: shown in its palette's colours.
    IDirectDrawClipper* clip = nullptr;
    dd->CreateClipper(0, &clip, nullptr);
    int right = 0, other = 0;
    HRESULT fr = E_FAIL;
    if (prim && clip && SUCCEEDED(sp)) {
        clip->SetHWnd(0, hwnd);
        prim->SetClipper(clip);
        RECT cr;
        GetClientRect(hwnd, &cr);
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = 7;
        fr = prim->Blt(&cr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        for (int i = 0; i < 50; i++) {
            Pump();
            Sleep(25);
            if (i < 20) continue;  // (shown by then: the presenter's device made)
            HDC sdc = GetDC(nullptr);
            const COLORREF c = GetPixel(sdc, (cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2) & 0xFFFFFF;
            ReleaseDC(nullptr, sdc);
            (c == RGB(200, 100, 50) ? right : other)++;
        }
    }
    CHECK(SUCCEEDED(fr) && right == 30 && !other,
          "desktopbpp=8: index 7 filled into the window through the primary (0x%08lx) shows its palette's colour (%d of 30 reads, "
          "%d other)", fr, right, other);
    if (clip) clip->Release();
    if (pal) pal->Release();
    if (plain) plain->Release();
    if (prim) prim->Release();
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

static int RunLifetime() {
    IDirectDraw7* dd = nullptr;
    HRESULT hr = DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    if (!dd) return 1;
    HWND hwnd = MakeWindow(WS_POPUP, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    hr = dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    CHECK(SUCCEEDED(hr), "lifetime: exclusive mode -> 0x%08lx", hr);

    // Flip chain: release the front buffer while holding the back buffer (Jazz Jackrabbit 2).
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
    d.dwBackBufferCount = 2;
    IDirectDrawSurface7* front = nullptr;
    hr = dd->CreateSurface(&d, &front, nullptr);
    CHECK(SUCCEEDED(hr) && front, "lifetime: flip chain -> 0x%08lx", hr);
    if (!front) return 1;
    DDSCAPS2 caps = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
    IDirectDrawSurface7* back = nullptr;
    front->GetAttachedSurface(&caps, &back);
    const ULONG backRef = back ? back->AddRef() - 1 : 0;
    if (back) back->Release();
    CHECK(backRef == 2, "lifetime: back buffer counts its own references (%lu: the root's and ours)", backRef);
    ULONG r = front->Release();
    CHECK(r == 0, "lifetime: front buffer released (refcount %lu)", r);
    IDirectDrawSurface7* front2 = nullptr;
    hr = dd->CreateSurface(&d, &front2, nullptr);
    CHECK(SUCCEEDED(hr), "lifetime: new primary while the old back buffer is still referenced -> 0x%08lx", hr);
    DDSURFACEDESC2 l = {};
    l.dwSize = sizeof(l);
    hr = back ? back->Lock(nullptr, &l, DDLOCK_WAIT, nullptr) : E_FAIL;
    CHECK(SUCCEEDED(hr), "lifetime: old back buffer still usable -> 0x%08lx", hr);
    if (SUCCEEDED(hr)) back->Unlock(nullptr);
    r = back ? back->Release() : 1;
    CHECK(r == 0, "lifetime: old back buffer released (refcount %lu)", r);
    if (front2) front2->Release();

    // Mipmaps: hold only the smallest level, release the others and the root.
    DDSURFACEDESC2 m = {};
    m.dwSize = sizeof(m);
    m.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_MIPMAPCOUNT | DDSD_PIXELFORMAT;
    m.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX | DDSCAPS_SYSTEMMEMORY;
    m.dwWidth = m.dwHeight = 16;
    m.dwMipMapCount = 3;
    m.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    m.ddpfPixelFormat.dwFlags = DDPF_RGB;
    m.ddpfPixelFormat.dwRGBBitCount = 32;
    m.ddpfPixelFormat.dwRBitMask = 0xFF0000;
    m.ddpfPixelFormat.dwGBitMask = 0xFF00;
    m.ddpfPixelFormat.dwBBitMask = 0xFF;
    IDirectDrawSurface7* tex = nullptr;
    hr = dd->CreateSurface(&m, &tex, nullptr);
    CHECK(SUCCEEDED(hr) && tex, "lifetime: mipmapped texture -> 0x%08lx", hr);
    if (tex) {
        DDSCAPS2 mc = {DDSCAPS_TEXTURE | DDSCAPS_MIPMAP, DDSCAPS2_MIPMAPSUBLEVEL, 0, {0}};
        IDirectDrawSurface7 *l1 = nullptr, *l2 = nullptr;
        tex->GetAttachedSurface(&mc, &l1);
        if (l1) l1->GetAttachedSurface(&mc, &l2);
        CHECK(l1 && l2, "lifetime: mip levels 1 and 2 found");
        if (l1) l1->Release();
        r = tex->Release();
        CHECK(r == 0, "lifetime: texture released with level 2 still referenced (refcount %lu)", r);
        DDSURFACEDESC2 ld = {};
        ld.dwSize = sizeof(ld);
        hr = l2 ? l2->Lock(nullptr, &ld, DDLOCK_WAIT, nullptr) : E_FAIL;
        CHECK(SUCCEEDED(hr) && ld.dwWidth == 4, "lifetime: level 2 still usable (0x%08lx, %lux%lu)", hr, ld.dwWidth,
              ld.dwHeight);
        if (SUCCEEDED(hr)) l2->Unlock(nullptr);
        r = l2 ? l2->Release() : 1;
        CHECK(r == 0, "lifetime: level 2 released (refcount %lu)", r);
    }

    // Writes a few rows past either end of a surface's memory (Xuan-Yuan Sword 3 EX's loose
    // sprite clipping): no crash, and the surface still frees.
    DDSURFACEDESC2 o = {};
    o.dwSize = sizeof(o);
    o.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    o.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    o.dwWidth = 640;
    o.dwHeight = 480;
    IDirectDrawSurface7* off = nullptr;
    hr = dd->CreateSurface(&o, &off, nullptr);
    CHECK(SUCCEEDED(hr) && off, "lifetime: 640x480 offscreen -> 0x%08lx", hr);
    if (off) {
        DDSURFACEDESC2 ld = {};
        ld.dwSize = sizeof(ld);
        if (SUCCEEDED(off->Lock(nullptr, &ld, DDLOCK_WAIT, nullptr))) {
            auto* p = static_cast<uint8_t*>(ld.lpSurface);
            const size_t pitch = size_t(ld.lPitch);
            memset(p + pitch * ld.dwHeight, 0x5A, pitch * 8);  // 8 rows below
            memset(p - pitch * 4, 0x5A, pitch * 4);             // 4 rows above
            off->Unlock(nullptr);
        }
        r = off->Release();
        CHECK(r == 0, "lifetime: offscreen written past its ends released (refcount %lu)", r);
    }
    r = dd->Release();
    CHECK(r == 0, "lifetime: DirectDraw released (refcount %lu)", r);
    DestroyWindow(hwnd);

    {
        // A game DLL that releases DirectDraw as it is unloaded (from its DllMain, under the
        // loader lock): dd2d9's threads can't end until that lock is free, and waiting for
        // their end took 3 s at every such unload (and left the presenter behind). The object
        // has shown something, so its presenter and device are up.
        char path[MAX_PATH];
        const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
        char* slash = n && n < MAX_PATH ? strrchr(path, '\\') : nullptr;
        HMODULE wc = nullptr;
        if (slash && size_t(slash - path) < MAX_PATH - 16) {
            strcpy(slash + 1, "wincaller.dll");
            wc = LoadLibraryA(path);
        }
        using HoldFn = void(WINAPI*)(IUnknown*, IUnknown*);
        const auto hold = wc ? reinterpret_cast<HoldFn>(reinterpret_cast<void*>(GetProcAddress(wc, "ReleaseAtUnload"))) : nullptr;
        IDirectDraw7* dd2 = nullptr;
        if (hold) DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd2), IID_IDirectDraw7, nullptr);
        if (dd2) {
            HWND w2 = MakeWindow(WS_OVERLAPPEDWINDOW, 320, 240);
            dd2->SetCooperativeLevel(w2, DDSCL_NORMAL);
            DDSURFACEDESC2 pd = {};
            pd.dwSize = sizeof(pd);
            pd.dwFlags = DDSD_CAPS;
            pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
            IDirectDrawSurface7* prim = nullptr;
            hr = dd2->CreateSurface(&pd, &prim, nullptr);
            if (prim) {
                DDBLTFX fx = {};
                fx.dwSize = sizeof(fx);
                fx.dwFillColor = 0x00FF00;
                RECT fill = {0, 0, 100, 100};
                for (int i = 0; i < 10; i++) {
                    prim->Blt(&fill, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
                    Pump();
                    Sleep(20);
                }
            }
            hold(prim, dd2);
            const DWORD t0 = GetTickCount();
            FreeLibrary(wc);
            wc = nullptr;
            const DWORD took = GetTickCount() - t0;
            CHECK(prim && took < 1000, "lifetime: DirectDraw released by a DLL as it is unloaded, in %lu ms (primary 0x%08lx)", took, hr);
            DestroyWindow(w2);
        } else {
            fprintf(g_out, "SKIP DirectDraw released by a DLL as it is unloaded: wincaller.dll not next to ddtest\n");
        }
        if (wc) FreeLibrary(wc);
    }
    return 0;
}

// Leaving exclusive mode: DirectDraw 2+ restores the desktop mode, DirectDraw 1
// keeps the game's mode (as Windows does; Jazz Jackrabbit 2 relies on it).
static int RunCoopModes() {
    HWND hwnd = MakeWindow(WS_POPUP, 640, 480);
    const int deskW = GetSystemMetrics(SM_CXSCREEN), deskH = GetSystemMetrics(SM_CYSCREEN);
    for (int v : {1, 7}) {
        DWORD w = 0, h = 0;
        HRESULT hr;
        if (v == 1) {
            IDirectDraw* dd = nullptr;
            DirectDrawCreate(nullptr, &dd, nullptr);
            if (!dd) return 1;
            dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
            hr = dd->SetDisplayMode(640, 480, 16);
            dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
            DDSURFACEDESC d = {};
            d.dwSize = sizeof(d);
            dd->GetDisplayMode(&d);
            w = d.dwWidth;
            h = d.dwHeight;
            dd->Release();
        } else {
            IDirectDraw7* dd = nullptr;
            DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
            if (!dd) return 1;
            dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
            hr = dd->SetDisplayMode(640, 480, 16, 0, 0);
            dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            dd->GetDisplayMode(&d);
            w = d.dwWidth;
            h = d.dwHeight;
            dd->Release();
        }
        const bool keeps = v == 1;
        CHECK(SUCCEEDED(hr) && (keeps ? (w == 640 && h == 480) : (int(w) == deskW && int(h) == deskH)),
              "DirectDraw %d: exclusive -> NORMAL %s the display mode (%lux%lu)", v, keeps ? "keeps" : "restores", w, h);
    }

    // DDSCL_CREATEDEVICEWINDOW twice: the device window the first call made stays (ddraw.dll
    // destroys it only when the window changes); it went, and came back with every other call.
    {
        HWND focus = MakeWindow(WS_POPUP, 100, 100);
        IDirectDraw7* dd = nullptr;
        DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
        if (dd) {
            const DWORD fs = DDSCL_CREATEDEVICEWINDOW | DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN;
            dd->SetCooperativeLevel(focus, DDSCL_SETFOCUSWINDOW);
            const HRESULT hr1 = dd->SetCooperativeLevel(nullptr, fs);
            const HWND first = FindWindowA("DirectDrawDeviceWnd", nullptr);
            const HRESULT hr2 = dd->SetCooperativeLevel(nullptr, fs);
            const HWND second = FindWindowA("DirectDrawDeviceWnd", nullptr);
            CHECK(SUCCEEDED(hr1) && SUCCEEDED(hr2) && first && first == second && IsWindow(first),
                  "CREATEDEVICEWINDOW twice keeps the device window (0x%08lx 0x%08lx, %p then %p)", hr1, hr2,
                  static_cast<void*>(first), static_cast<void*>(second));
            dd->SetCooperativeLevel(focus, DDSCL_NORMAL);
            dd->Release();
        }
        DestroyWindow(focus);
    }

    // Back to exclusive mode and a new primary: refused while the old one exists (lost or
    // not, as ddraw.dll), fine once it is released (Jazz Jackrabbit 2 releases it).
    {
        IDirectDraw* dd = nullptr;
        DirectDrawCreate(nullptr, &dd, nullptr);
        if (!dd) return 1;
        DDSURFACEDESC d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
        d.dwBackBufferCount = 2;
        IDirectDrawSurface *p1 = nullptr, *p2 = nullptr;
        dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT);
        dd->SetDisplayMode(640, 480, 8);
        HRESULT hr = dd->CreateSurface(&d, &p1, nullptr);
        CHECK(hr == DD_OK, "JJ2 pattern: first primary (0x%08lx)", hr);
        IDirectDraw2* dd2 = nullptr;  // through IDirectDraw2, as JJ2: leaving restores the mode
        dd->QueryInterface(IID_IDirectDraw2, reinterpret_cast<void**>(&dd2));
        if (dd2) {
            dd2->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
            dd2->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT);
            dd2->SetDisplayMode(640, 480, 8, 0, 0);
            hr = dd->CreateSurface(&d, &p2, nullptr);
            CHECK(hr == DDERR_PRIMARYSURFACEALREADYEXISTS && !p2, "no new primary while the lost one exists (0x%08lx)", hr);
            if (p1) p1->Release();
            p1 = nullptr;
            hr = dd->CreateSurface(&d, &p2, nullptr);
            CHECK(hr == DD_OK && p2, "JJ2 pattern: a new primary once the old one is released (0x%08lx)", hr);
            if (p2) {
                hr = p2->Flip(nullptr, DDFLIP_WAIT);
                CHECK(hr == DD_OK, "JJ2 pattern: the new primary flips (0x%08lx)", hr);
            }
            dd2->Release();
        }
        if (p2) p2->Release();
        if (p1) p1->Release();
        dd->Release();
    }

    // A second DirectDraw object: exclusive mode refused while the first holds it in front
    // (as ddraw.dll); once the first is released, it can have it.
    {
        IDirectDraw *a = nullptr, *b = nullptr;
        DirectDrawCreate(nullptr, &a, nullptr);
        DirectDrawCreate(nullptr, &b, nullptr);
        if (!a || !b) return 1;
        a->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        a->SetDisplayMode(640, 480, 16);
        HRESULT hr = b->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        CHECK(hr == DDERR_EXCLUSIVEMODEALREADYSET, "second DirectDraw object: exclusive mode refused (0x%08lx)", hr);
        a->Release();
        hr = b->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        CHECK(hr == DD_OK, "...and granted once the first is released (0x%08lx)", hr);
        hr = b->SetDisplayMode(800, 600, 16);
        CHECK(hr == DD_OK, "...with its own display mode (0x%08lx)", hr);
        b->Release();
    }
    {
        // An owned fullscreen window (a Visual Basic form can be owned by its hidden main
        // window, which has the taskbar button): on focus loss its owner is minimized, hiding
        // it, and both come back with the focus (minimized itself, it couldn't be reached).
        HWND owner = CreateWindowExA(0, "ddtest", "owner", WS_POPUP | WS_VISIBLE, -100, -100, 0, 0, nullptr, nullptr,
                                     GetModuleHandleA(nullptr), nullptr);
        HWND form = CreateWindowExA(0, "ddtest", "form", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, owner, nullptr,
                                    GetModuleHandleA(nullptr), nullptr);
        IDirectDraw7* dd = nullptr;
        DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
        if (!dd) return 1;
        SetForegroundWindow(form);
        dd->SetCooperativeLevel(form, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        dd->SetDisplayMode(640, 480, 16, 0, 0);
        Pump();
        SendMessageA(form, WM_ACTIVATEAPP, FALSE, 0);
        Pump();
        const bool ownerMin = IsIconic(owner) != FALSE, formMin = IsIconic(form) != FALSE,
                   formShown = IsWindowVisible(form) != FALSE;
        // (display=window minimizes nothing: an ordinary window.)
        char display[32] = "";
        GetPrivateProfileStringA("dd2d9", "display", "", display, sizeof(display), ".\\dd2d9.ini");
        const bool windowMode = !strcmp(display, "window");
        CHECK(windowMode ? !ownerMin && !formMin && formShown : ownerMin && !formMin && !formShown,
              "owned exclusive window, focus lost: its owner minimized (%d), it hidden with it (minimized %d, shown %d)",
              ownerMin, formMin, formShown);
        SendMessageA(form, WM_ACTIVATEAPP, TRUE, 0);
        Pump();
        CHECK(!IsIconic(owner) && IsWindowVisible(form) && !IsIconic(form),
              "...and both back with the focus (owner minimized %d, form shown %d)", IsIconic(owner) != FALSE,
              IsWindowVisible(form) != FALSE);
        dd->Release();
        DestroyWindow(form);
        DestroyWindow(owner);
    }
    {
        // A 16-bit bitmap the game makes (CreateBitmapIndirect) in a 16-bit mode, selected into
        // a memory DC (CreateCompatibleDC(NULL)) and blitted into a 16-bit surface's DC: its
        // pixels arrive unchanged, as on XP's 16-bit screen. Sakura Taisen draws each letter
        // so; on Windows' 32-bit screen the bitmap couldn't be selected and nothing arrived.
        HWND w = MakeWindow(WS_POPUP | WS_VISIBLE, 640, 480);
        IDirectDraw7* dd = nullptr;
        DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
        if (!dd) return 1;
        dd->SetCooperativeLevel(w, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        dd->SetDisplayMode(640, 480, 16, 0, 0);
        IDirectDrawSurface7* s = MakeSurface(dd, 8, 2, 16, 0xF800, 0x7E0, 0x1F);
        WORD px[16];
        for (int i = 0; i < 16; i++) px[i] = WORD(0x07FF + i * 0x0841);  // the key colour, then others
        BITMAP bm = {0, 8, 2, 16, 1, 16, px};
        HBITMAP b = CreateBitmapIndirect(&bm);
        HDC mem = CreateCompatibleDC(nullptr);
        const HGDIOBJ old = mem && b ? SelectObject(mem, b) : nullptr;
        HDC dc = nullptr;
        if (s && old && SUCCEEDED(s->GetDC(&dc))) {
            BitBlt(dc, 0, 0, 8, 2, mem, 0, 0, SRCCOPY);
            s->ReleaseDC(dc);
        }
        int same = 0;
        DDSURFACEDESC2 sd = {};
        sd.dwSize = sizeof(sd);
        if (s && SUCCEEDED(s->Lock(nullptr, &sd, DDLOCK_WAIT, nullptr))) {
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 8; x++)
                    same += reinterpret_cast<const WORD*>(static_cast<const BYTE*>(sd.lpSurface) + y * sd.lPitch)[x] == px[y * 8 + x];
            s->Unlock(nullptr);
        }
        CHECK(b && old && same == 16, "16-bit mode: a 16-bit CreateBitmapIndirect bitmap goes into a memory DC (%d) and "
              "blits into a 16-bit surface unchanged (%d of 16)", old != nullptr, same);
        if (old) SelectObject(mem, old);
        if (mem) DeleteDC(mem);
        if (b) DeleteObject(b);
        if (s) s->Release();
        dd->Release();
        DestroyWindow(w);
    }
    {
        // A tool window (no taskbar button, no place in Alt+Tab): minimized with a taskbar
        // button given to it, and its own style back with the focus. (Pal95's window, which
        // the taskbar had no button for, Windows minimized onto the desktop: Wine doesn't
        // place minimized windows so, the same repair confirmed in the game.)
        HWND tool = CreateWindowExA(WS_EX_TOOLWINDOW, "ddtest", "tool", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, nullptr,
                                    nullptr, GetModuleHandleA(nullptr), nullptr);
        IDirectDraw7* dd = nullptr;
        DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
        if (!dd) return 1;
        SetForegroundWindow(tool);
        dd->SetCooperativeLevel(tool, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        dd->SetDisplayMode(640, 480, 16, 0, 0);
        Pump();
        SendMessageA(tool, WM_ACTIVATEAPP, FALSE, 0);
        Pump();
        char display[32] = "";
        GetPrivateProfileStringA("dd2d9", "display", "", display, sizeof(display), ".\\dd2d9.ini");
        const bool windowMode = !strcmp(display, "window");
        const LONG bg = GetWindowLongA(tool, GWL_EXSTYLE);
        const bool min = IsIconic(tool) != FALSE;
        CHECK(windowMode ? !min && (bg & WS_EX_TOOLWINDOW) : min && (bg & WS_EX_APPWINDOW) && !(bg & WS_EX_TOOLWINDOW),
              "tool window, focus lost: minimized (%d) with a taskbar button (ex style 0x%lx)", min, bg);
        SendMessageA(tool, WM_ACTIVATEAPP, TRUE, 0);
        Pump();
        const LONG fg = GetWindowLongA(tool, GWL_EXSTYLE);
        CHECK(!IsIconic(tool) && (fg & WS_EX_TOOLWINDOW) && !(fg & WS_EX_APPWINDOW),
              "...and back with the focus, its own style again (ex style 0x%lx)", fg);
        dd->Release();
        DestroyWindow(tool);
    }
    DestroyWindow(hwnd);
    return 0;
}

// ---------------------------------------------------------------- fuzz

// A surface and a model of what it should contain.
struct FuzzSurface {
    IDirectDrawSurface7* s = nullptr;
    int w = 0, h = 0, bytes = 0;
    std::vector<uint32_t> px;  // expected pixels, row-major

    void Read(std::vector<uint32_t>& out) const {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        out.assign(size_t(w) * h, 0);
        if (FAILED(s->Lock(nullptr, &d, DDLOCK_WAIT, nullptr))) return;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                memcpy(&out[size_t(y) * w + x], static_cast<uint8_t*>(d.lpSurface) + y * d.lPitch + x * bytes,
                       size_t(bytes));
        s->Unlock(nullptr);
    }
    void Write() {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        if (FAILED(s->Lock(nullptr, &d, DDLOCK_WAIT, nullptr))) return;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                memcpy(static_cast<uint8_t*>(d.lpSurface) + y * d.lPitch + x * bytes, &px[size_t(y) * w + x],
                       size_t(bytes));
        s->Unlock(nullptr);
    }
};

// The source pixel each of a stretched blit's `n` destination pixels takes from `w` source
// ones, as measured on Windows XP: a shrink steps through the source in whole pixels with
// the remainder carried, a stretch samples at pixel centres (in 16.16 steps across, exactly
// down); mirrored, the far end's first; `edges` (a surface onto an overlapping part of
// itself), at the pixels' near edges.
static std::vector<int> StretchMap(int w, int n, bool mirror, bool edges, bool down) {
    std::vector<int> map(static_cast<size_t>(n));
    if (mirror || edges) {
        for (int i = 0; i < n; i++) map[size_t(i)] = int(int64_t(mirror ? n - 1 - i : i) * w / n);
    } else if (n < w) {
        int v = w / n / 2, sum = n / 2;
        for (int i = 0; i < n; i++) {
            map[size_t(i)] = v;
            v += w / n;
            sum += w % n;
            if (sum >= n) v++, sum -= n;
        }
    } else if (down) {
        for (int i = 0; i < n; i++) map[size_t(i)] = int((2 * int64_t(i) + 1) * w / (2 * int64_t(n)));
    } else {
        const uint32_t step = uint32_t((int64_t(w) << 16) / n);
        uint32_t pos = step >> 1;
        for (int i = 0; i < n; i++, pos += step) map[size_t(i)] = int(pos >> 16);
    }
    return map;
}

static uint32_t g_rng = 12345;
static uint32_t Rand() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static int RandIn(int lo, int hi) { return lo + int(Rand() % uint32_t(hi - lo + 1)); }

// Runs of key-range values and of random values, so that 16-byte chunks are
// sometimes all transparent, all opaque, or mixed.
// Bits outside the key mask (alpha, the top bit of 555) are random everywhere.
static void FuzzFill(FuzzSurface& f, uint32_t valueMask, uint32_t keyMask, uint32_t keyLo, uint32_t keyHi) {
    size_t i = 0;
    while (i < f.px.size()) {
        const int run = RandIn(1, 48);
        const bool key = Rand() % 2;
        for (int k = 0; k < run && i < f.px.size(); k++, i++) {
            uint32_t v = Rand() & valueMask;
            if (key) v = (v & ~keyMask) | (keyLo + Rand() % (keyHi - keyLo + 1));
            f.px[i] = v & valueMask;
        }
    }
    f.Write();
}

static int RunFuzz() {
    IDirectDraw7* dd = nullptr;
    HRESULT hr = DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    CHECK(SUCCEEDED(hr), "DirectDrawCreateEx -> 0x%08lx", hr);
    if (!dd) return 1;
    dd->SetCooperativeLevel(nullptr, DDSCL_NORMAL);

    struct Fmt {
        const char* name;
        DWORD bpp, flags, r, g, b;
        uint32_t keyMask;  // bits compared by color keys (no alpha / unused bits)
    };
    const Fmt fmts[] = {
        {"8-bit", 8, DDPF_RGB | DDPF_PALETTEINDEXED8, 0, 0, 0, 0xFF},
        {"16-bit 565", 16, DDPF_RGB, 0xF800, 0x07E0, 0x001F, 0xFFFF},
        {"16-bit 555", 16, DDPF_RGB, 0x7C00, 0x03E0, 0x001F, 0x7FFF},
        {"32-bit", 32, DDPF_RGB, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF},
    };
    // Small surfaces; then surfaces as wide and as tall as they come.
    const struct {
        const char* name;
        int sizes[2][2];
        int iters, maxSw, maxSh, maxDw;
    } passes[] = {
        {"", {{83, 41}, {97, 53}}, 3000, 70, 30, 90},
        {", 65535 wide", {{65535, 2}, {65535, 3}}, 150, 65535, 3, 65535},
        {", 65535 tall", {{2, 65535}, {3, 65535}}, 150, 3, 65535, 3},
    };
    for (const auto& pass : passes) {
        for (const Fmt& fm : fmts) {
            const uint32_t valueMask = fm.bpp == 32 ? 0xFFFFFFFFu : ((1u << fm.bpp) - 1);
            FuzzSurface surf[2];
            const auto& sizes = pass.sizes;
            for (int k = 0; k < 2; k++) {
                DDSURFACEDESC2 d = {};
                d.dwSize = sizeof(d);
                d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
                d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
                d.dwWidth = DWORD(sizes[k][0]);
                d.dwHeight = DWORD(sizes[k][1]);
                d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
                d.ddpfPixelFormat.dwFlags = fm.flags;
                d.ddpfPixelFormat.dwRGBBitCount = fm.bpp;
                d.ddpfPixelFormat.dwRBitMask = fm.r;
                d.ddpfPixelFormat.dwGBitMask = fm.g;
                d.ddpfPixelFormat.dwBBitMask = fm.b;
                hr = dd->CreateSurface(&d, &surf[k].s, nullptr);
                if (FAILED(hr)) {
                    CHECK(false, "%s: CreateSurface -> 0x%08lx", fm.name, hr);
                    return 1;
                }
                surf[k].w = sizes[k][0];
                surf[k].h = sizes[k][1];
                surf[k].bytes = int(fm.bpp / 8);
                surf[k].px.assign(size_t(surf[k].w) * surf[k].h, 0);
            }
            int mismatches = 0, blits = 0;
            char firstBad[200] = "";
            std::vector<uint32_t> got;
            for (int iter = 0; iter < pass.iters; iter++) {
                // Keys: a single color, or a range (DDCKEY_COLORSPACE).
                uint32_t skLo = Rand() & fm.keyMask, skHi = skLo;
                if (Rand() % 4 == 0) skHi = std::min<uint32_t>(fm.keyMask, skLo + Rand() % 300);
                uint32_t dkLo = Rand() & fm.keyMask, dkHi = dkLo;
                if (Rand() % 3 == 0) dkHi = std::min<uint32_t>(fm.keyMask, dkLo + Rand() % 5000);
                if (iter % 40 == 0) {  // new contents now and then
                    FuzzFill(surf[0], valueMask, fm.keyMask, skLo, skHi);
                    FuzzFill(surf[1], valueMask, fm.keyMask, dkLo, dkHi);
                }
                const bool same = Rand() % 8 == 0;  // within one surface (overlapping copies)
                FuzzSurface& src = surf[same ? 1 : 0];
                FuzzSurface& dst = surf[1];
                // A range refused (DDERR_NOCOLORKEYHW: ddraw.dll's software blitter has none, nor
                // has dd2d9): its low value alone, as a game falling back would set it.
                DDCOLORKEY sk = {skLo, skHi}, dk = {dkLo, dkHi};
                if (FAILED(src.s->SetColorKey(DDCKEY_SRCBLT | (skHi != skLo ? DDCKEY_COLORSPACE : 0), &sk))) {
                    sk.dwColorSpaceHighValue = skHi = skLo;
                    src.s->SetColorKey(DDCKEY_SRCBLT, &sk);
                }
                if (FAILED(dst.s->SetColorKey(DDCKEY_DESTBLT | (dkHi != dkLo ? DDCKEY_COLORSPACE : 0), &dk))) {
                    dk.dwColorSpaceHighValue = dkHi = dkLo;
                    dst.s->SetColorKey(DDCKEY_DESTBLT, &dk);
                }

                const int sw = RandIn(1, std::min(src.w, pass.maxSw)), sh = RandIn(1, std::min(src.h, pass.maxSh));
                const bool stretch = Rand() % 4 == 0;
                const int dw = stretch ? RandIn(1, std::min(dst.w, pass.maxDw)) : sw, dh = stretch ? RandIn(1, dst.h) : sh;
                RECT sr = {0, 0, sw, sh};
                OffsetRect(&sr, RandIn(0, src.w - sw), RandIn(0, src.h - sh));
                RECT dr = {0, 0, dw, dh};
                OffsetRect(&dr, RandIn(0, dst.w - dw), RandIn(0, dst.h - dh));

                const int keyMode = RandIn(0, 4);  // none, src, src override, dest, src + dest
                DWORD flags = DDBLT_WAIT;
                DDBLTFX fx = {};
                fx.dwSize = sizeof(fx);
                bool useSrc = false, useDst = false;
                uint32_t eLo = skLo, eHi = skHi;
                if (keyMode == 1 || keyMode == 4) {
                    flags |= DDBLT_KEYSRC;
                    useSrc = true;
                }
                if (keyMode == 2) {
                    // An override key is one color: the high value is ignored.
                    flags |= DDBLT_KEYSRCOVERRIDE;
                    fx.ddckSrcColorkey.dwColorSpaceLowValue = Rand() % 2 ? skLo : (Rand() & valueMask);
                    fx.ddckSrcColorkey.dwColorSpaceHighValue = Rand();
                    eLo = eHi = fx.ddckSrcColorkey.dwColorSpaceLowValue & fm.keyMask;
                    useSrc = true;
                }
                if (keyMode == 3 || keyMode == 4) {
                    flags |= DDBLT_KEYDEST;
                    useDst = true;
                }
                const bool mirrorX = Rand() % 5 == 0, mirrorY = Rand() % 5 == 0;
                if (mirrorX || mirrorY) {
                    flags |= DDBLT_DDFX;
                    fx.dwDDFX = (mirrorX ? DDBLTFX_MIRRORLEFTRIGHT : 0) | (mirrorY ? DDBLTFX_MIRRORUPDOWN : 0);
                }

                // Reference, reading the source as it was before the blit.
                const std::vector<uint32_t> srcBefore = src.px;
                RECT both;
                const bool edges = same && IntersectRect(&both, &sr, &dr);
                const std::vector<int> xmap = StretchMap(sw, dw, mirrorX, edges, false), ymap = StretchMap(sh, dh, mirrorY, edges, true);
                for (int y = dr.top; y < dr.bottom; y++) {
                    const int sy = sr.top + ymap[size_t(y - dr.top)];
                    for (int x = dr.left; x < dr.right; x++) {
                        const int sx = sr.left + xmap[size_t(x - dr.left)];
                        const uint32_t v = srcBefore[size_t(sy) * src.w + sx];
                        if (useSrc && (v & fm.keyMask) >= eLo && (v & fm.keyMask) <= eHi) continue;
                        const uint32_t d = dst.px[size_t(y) * dst.w + x] & fm.keyMask;
                        if (useDst && (d < dkLo || d > dkHi)) continue;
                        dst.px[size_t(y) * dst.w + x] = v;
                    }
                }
                hr = dst.s->Blt(&dr, src.s, &sr, flags, &fx);
                blits++;
                dst.Read(got);
                if (FAILED(hr) || got != dst.px) {
                    if (!mismatches)
                        sprintf(firstBad, "hr 0x%08lx, %ldx%ld -> %ldx%ld, keys %d, mirror %d%d, same %d", hr,
                                sr.right - sr.left, sr.bottom - sr.top, dr.right - dr.left, dr.bottom - dr.top, keyMode,
                                mirrorX, mirrorY, same);
                    mismatches++;
                    dst.px = got;  // continue from what is there
                }
            }
            CHECK(mismatches == 0, "%s%s: %d random blits match the reference (%d mismatches%s%s)", fm.name, pass.name, blits,
                  mismatches, mismatches ? "; first: " : "", firstBad);
            surf[0].s->Release();
            surf[1].s->Release();
        }
    }
    dd->Release();
    return 0;
}

// Through the import table each time (gcc would keep the function's address in a
// register across the whole test, from before dd2d9 translated the import).
__attribute__((noinline)) static int ScreenMetric(int i) { return GetSystemMetrics(i); }

// The mouse with the display mode shown scaled (run with display=borderless): the game
// sees its own mode's coordinates, its window as its screen.
// A one-frame, 32x24, 24-bit uncompressed AVI (for the MCI video checks).
static bool WriteTinyAvi(const char* path) {
    const DWORD w = 32, h = 24, frame = w * 3 * h;
    std::vector<uint8_t> f;
    auto u32 = [&](DWORD v) { for (int i = 0; i < 4; i++) f.push_back(uint8_t(v >> (8 * i))); };
    auto u16 = [&](WORD v) { f.push_back(uint8_t(v)); f.push_back(uint8_t(v >> 8)); };
    auto tag = [&](const char* t) { f.insert(f.end(), t, t + 4); };
    auto patch = [&](size_t at, DWORD v) { for (int i = 0; i < 4; i++) f[at + i] = uint8_t(v >> (8 * i)); };
    tag("RIFF"); const size_t riff = f.size(); u32(0); tag("AVI ");
    tag("LIST"); const size_t hdrl = f.size(); u32(0); tag("hdrl");
    tag("avih"); u32(56);
    u32(100000); u32(0); u32(0); u32(0x10); u32(1); u32(0); u32(1); u32(frame); u32(w); u32(h); u32(0); u32(0); u32(0); u32(0);
    tag("LIST"); const size_t strl = f.size(); u32(0); tag("strl");
    tag("strh"); u32(56);
    tag("vids"); u32(0); u32(0); u16(0); u16(0); u32(0); u32(1); u32(10); u32(0); u32(1); u32(frame); u32(DWORD(-1)); u32(0);
    u16(0); u16(0); u16(WORD(w)); u16(WORD(h));
    tag("strf"); u32(40);
    u32(40); u32(w); u32(h); u16(1); u16(24); u32(0); u32(frame); u32(0); u32(0); u32(0); u32(0);
    patch(strl, DWORD(f.size() - strl - 4));
    patch(hdrl, DWORD(f.size() - hdrl - 4));
    tag("LIST"); const size_t movi = f.size(); u32(0); tag("movi");
    const DWORD chunkAt = DWORD(f.size() - movi - 4);
    tag("00db"); u32(frame);
    for (DWORD i = 0; i < frame; i++) f.push_back(uint8_t(i * 7));
    patch(movi, DWORD(f.size() - movi - 4));
    tag("idx1"); u32(16); tag("00db"); u32(0x10); u32(chunkAt); u32(frame);
    patch(riff, DWORD(f.size() - 8));
    FILE* out = fopen(path, "wb");
    if (!out) return false;
    const bool ok = fwrite(f.data(), 1, f.size(), out) == f.size();
    fclose(out);
    return ok;
}

// A popup's procedure that paints its background (blue) and returns 0, as many do.
static LRESULT CALLBACK OwnErasePopupProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc = {0, 0, 4, 4};
        HBRUSH blue = CreateSolidBrush(RGB(0, 0, 255));
        FillRect(reinterpret_cast<HDC>(wp), &rc, blue);
        DeleteObject(blue);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

// MCI commands from a second thread, which Windows refuses (a device answers only the thread
// that opened it): the commands' answers.
struct OtherThreadMci {
    MCIDEVICEID id;      // MCI_STOP and MCI_CLOSE for this device, or
    const char* string;  // this command string
    MCIERROR stop, close;
};

static DWORD WINAPI OtherThreadMciProc(void* p) {
    auto* m = static_cast<OtherThreadMci*>(p);
    if (m->string) {
        m->close = mciSendStringA(m->string, nullptr, 0, nullptr);
    } else {
        m->stop = mciSendCommandA(m->id, MCI_STOP, 0, 0);
        m->close = mciSendCommandA(m->id, MCI_CLOSE, 0, 0);
    }
    return 0;
}

static void RunOnOtherThread(OtherThreadMci* m) {
    if (HANDLE t = CreateThread(nullptr, 0, OtherThreadMciProc, m, 0, nullptr)) {
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
    }
}

static int RunMouse() {
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);  // the real screen, before
    HWND hwnd = MakeWindow(WS_POPUP, sw, sh);
    CHECK(hwnd != nullptr, "test window created");
    if (!hwnd) return 1;
    IDirectDraw7* dd = nullptr;
    HRESULT hr = DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    CHECK(SUCCEEDED(hr) && dd, "DirectDrawCreateEx");
    if (!dd) return 1;
    hr = dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    CHECK(SUCCEEDED(hr), "exclusive mode (0x%08lx)", hr);
    g_lastSize = -1;
    hr = dd->SetDisplayMode(640, 480, 32, 0, 0);
    CHECK(SUCCEEDED(hr), "SetDisplayMode 640x480 (0x%08lx)", hr);
    Pump();
    // A window already covering the screen isn't resized: told the mode's size all the same, as
    // a window sized to the switched mode is (Sango 4 blitted to the size it was last told).
    CHECK(g_lastSize == MAKELPARAM(640, 480), "WM_SIZE: the window told the mode's size (%dx%d)", LOWORD(g_lastSize),
          HIWORD(g_lastSize));
    {
        // A second DirectDraw object of the process (DirectShow's, as KOF 99's intro starts): the
        // display is in the game's mode, as after a real switch.
        IDirectDraw7* other = nullptr;
        if (SUCCEEDED(DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&other), IID_IDirectDraw7, nullptr)) && other) {
            other->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
            DDSURFACEDESC2 md = {};
            md.dwSize = sizeof(md);
            other->GetDisplayMode(&md);
            CHECK(md.dwWidth == 640 && md.dwHeight == 480 && md.ddpfPixelFormat.dwRGBBitCount == 32,
                  "another object's GetDisplayMode: the game's mode (%lux%lu, %lu bpp)", md.dwWidth, md.dwHeight,
                  md.ddpfPixelFormat.dwRGBBitCount);
            other->Release();
        }
    }
    CHECK(ScreenMetric(SM_CXSCREEN) == 640 && ScreenMetric(SM_CYSCREEN) == 480, "the screen is the game's mode (%d x %d)",
          ScreenMetric(SM_CXSCREEN), ScreenMetric(SM_CYSCREEN));
    RECT cr = {};
    GetClientRect(hwnd, &cr);
    CHECK(cr.right == 640 && cr.bottom == 480, "the window's client area is the mode (%ld x %ld)", cr.right, cr.bottom);
    RECT wr = {};
    GetWindowRect(hwnd, &wr);
    CHECK(wr.left == 0 && wr.top == 0 && wr.right == 640 && wr.bottom == 480, "the window is the game's screen");
    POINT pt = {100, 50};
    ScreenToClient(hwnd, &pt);
    CHECK(pt.x == 100 && pt.y == 50, "client and screen coordinates are the same (%ld,%ld)", pt.x, pt.y);
    SetCursorPos(320, 240);
    POINT c = {};
    GetCursorPos(&c);
    CHECK(c.x == 320 && c.y == 240, "SetCursorPos/GetCursorPos in the mode's coordinates (%ld,%ld)", c.x, c.y);
    SetCursorPos(639, 479);
    GetCursorPos(&c);
    CHECK(c.x == 639 && c.y == 479, "the far corner (%ld,%ld)", c.x, c.y);
    RECT clip = {0, 0, 640, 480};
    ClipCursor(&clip);
    RECT got = {};
    GetClipCursor(&got);
    CHECK(got.left == 0 && got.top == 0 && got.right == 640 && got.bottom == 480, "ClipCursor/GetClipCursor (%ld,%ld-%ld,%ld)",
          got.left, got.top, got.right, got.bottom);
    ClipCursor(nullptr);
    {
        // One of Windows' own modules whose call reaches dd2d9's hook through another hooker's
        // patch (Windows' DirectInput 1-7 in Nerf Arena Blast and Unreal Tournament: its mouse
        // came back in the game's coordinates, the camera turning on its own) gets the real
        // screen's answer; the game still its mode's. wincaller.dll, copied into the Windows
        // folder, calls the hook ddtest's own import points at.
        char dir[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
        const UINT nd = GetWindowsDirectoryA(dir, MAX_PATH);
        const DWORD ns = GetModuleFileNameA(nullptr, src, MAX_PATH);
        char* slash = ns && ns < MAX_PATH ? strrchr(src, '\\') : nullptr;
        HMODULE wc = nullptr;
        bool copied = false;
        if (nd && nd < MAX_PATH - 32 && slash && size_t(slash - src) < MAX_PATH - 16) {
            strcpy(slash + 1, "wincaller.dll");
            snprintf(dst, sizeof(dst), "%s\\dd2d9test-wincaller.dll", dir);
            copied = CopyFileA(src, dst, FALSE) != 0;
            if (copied) wc = LoadLibraryA(dst);
        }
        using PointFn = BOOL(WINAPI*)(POINT*);
        using CallFn = BOOL(WINAPI*)(PointFn, POINT*);
        const auto call = wc ? reinterpret_cast<CallFn>(reinterpret_cast<void*>(GetProcAddress(wc, "CallPointFn"))) : nullptr;
        {
            // A game that looks a translated function up at run time (Windows 95 and NT code
            // paths picked by name) gets dd2d9's translation too, as through its imports.
            const HMODULE u = GetModuleHandleA("user32.dll");
            const void* looked = reinterpret_cast<void*>(GetProcAddress(u, "GetCursorPos"));
            const void* viaImport = reinterpret_cast<void*>(&GetCursorPos);
            const void* lookedMove = reinterpret_cast<void*>(GetProcAddress(u, "MoveWindow"));
            const void* untouched = reinterpret_cast<void*>(GetProcAddress(u, "GetDesktopWindow"));
            CHECK(looked == viaImport && looked != RealExport(u, "GetCursorPos") && lookedMove != RealExport(u, "MoveWindow") &&
                      untouched == RealExport(u, "GetDesktopWindow"),
                  "GetProcAddress: GetCursorPos and MoveWindow looked up at run time are dd2d9's (%d %d %d), another function Windows' (%d)",
                  looked == viaImport, looked != RealExport(u, "GetCursorPos"), lookedMove != RealExport(u, "MoveWindow"),
                  untouched == RealExport(u, "GetDesktopWindow"));
        }
        if (call) {
            const PointFn hook = &GetCursorPos;  // (read from ddtest's import: dd2d9's)
            const auto real =
                reinterpret_cast<PointFn>(RealExport(GetModuleHandleA("user32.dll"), "GetCursorPos"));
            SetCursorPos(320, 240);
            POINT game = {}, windows = {}, truth = {};
            GetCursorPos(&game);
            call(hook, &windows);
            real(&truth);
            CHECK(hook != real && game.x == 320 && game.y == 240 && windows.x == truth.x && windows.y == truth.y &&
                      (truth.x != 320 || truth.y != 240),
                  "a Windows module's call through dd2d9's hook: the real cursor (%ld,%ld; real %ld,%ld), the game's "
                  "translated (%ld,%ld)",
                  windows.x, windows.y, truth.x, truth.y, game.x, game.y);
        } else {
            fprintf(g_out, "SKIP a Windows module's call through dd2d9's hook: wincaller.dll not next to ddtest, or the "
                           "Windows folder not writable\n");
        }
        if (wc) FreeLibrary(wc);
        if (copied) DeleteFileA(dst);
    }
    // A mouse message at the real middle of the screen arrives at the mode's middle.
    g_lastMouseMove = -1;
    SendMessageA(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(sw / 2, sh / 2));
    CHECK(LOWORD(g_lastMouseMove) == 320 && HIWORD(g_lastMouseMove) == 240, "WM_MOUSEMOVE in the mode's coordinates (%d,%d)",
          LOWORD(g_lastMouseMove), HIWORD(g_lastMouseMove));
    {
        // Repainting the window: the area in the mode's coordinates (Road Rash blits it from
        // its 640x480 buffer), and an area the game invalidates mapped where the image is.
        Pump();
        g_lastPaint = RECT{-1, -1, -1, -1};
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        CHECK(g_lastPaint.left == 0 && g_lastPaint.top == 0 && g_lastPaint.right == 640 && g_lastPaint.bottom == 480,
              "WM_PAINT: the whole window is the mode (%ld,%ld-%ld,%ld)", g_lastPaint.left, g_lastPaint.top, g_lastPaint.right,
              g_lastPaint.bottom);
        RECT part = {100, 50, 300, 150};
        InvalidateRect(hwnd, &part, FALSE);
        UpdateWindow(hwnd);
        CHECK(labs(g_lastPaint.left - 100) <= 1 && labs(g_lastPaint.top - 50) <= 1 && labs(g_lastPaint.right - 300) <= 1 &&
                  labs(g_lastPaint.bottom - 150) <= 1,
              "WM_PAINT: an invalidated part comes back as given (%ld,%ld-%ld,%ld)", g_lastPaint.left, g_lastPaint.top,
              g_lastPaint.right, g_lastPaint.bottom);
        // Validating the rectangle GetUpdateRect gave validates all there was, the parts
        // around the image too (else WM_PAINT comes again forever).
        InvalidateRect(hwnd, nullptr, FALSE);
        RECT upd = {}, left = {};
        GetUpdateRect(hwnd, &upd, FALSE);
        ValidateRect(hwnd, &upd);
        const BOOL still = GetUpdateRect(hwnd, &left, FALSE);
        CHECK(!still, "ValidateRect of the update rectangle (%ld,%ld-%ld,%ld) leaves nothing to repaint (left %ld,%ld-%ld,%ld)",
              upd.left, upd.top, upd.right, upd.bottom, left.left, left.top, left.right, left.bottom);
        // The same for a part of the window that isn't on the image's border, at odd real
        // coordinates the scale doesn't divide (Windows invalidated it: a dialog over it), and
        // for a strip beside the image where there is one. Rounded down both ways, the mode's
        // rectangle mapped back a pixel short and that pixel stayed to repaint.
        using InvalidateFn = BOOL(WINAPI*)(HWND, const RECT*, BOOL);
        const auto realInvalidate = reinterpret_cast<InvalidateFn>(
            RealExport(GetModuleHandleA("user32.dll"), "InvalidateRect"));
        const double scale = std::min(double(sw) / 640, double(sh) / 480);
        const long ix = long((sw - 640 * scale) / 2), iy = long((sh - 480 * scale) / 2);
        RECT parts[2] = {{ix + 141, iy + 77, ix + 244, iy + 151}, {0, iy + 77, ix / 2, iy + 151}};
        for (int i = 0; i < (ix >= 2 ? 2 : 1); i++) {
            realInvalidate(hwnd, &parts[i], FALSE);
            upd = left = RECT{};
            GetUpdateRect(hwnd, &upd, FALSE);
            ValidateRect(hwnd, &upd);
            const BOOL rest = GetUpdateRect(hwnd, &left, FALSE);
            CHECK(!rest && !IsRectEmpty(&upd),
                  "ValidateRect of the update rectangle of real %ld,%ld-%ld,%ld (%ld,%ld-%ld,%ld) leaves nothing to repaint "
                  "(left %ld,%ld-%ld,%ld)",
                  parts[i].left, parts[i].top, parts[i].right, parts[i].bottom, upd.left, upd.top, upd.right, upd.bottom,
                  left.left, left.top, left.right, left.bottom);
        }
    }
    if (WriteTinyAvi("dd2d9test.avi")) {
        // A video Windows' player draws into the window (Street Fighter Zero's movies): the
        // destination the game gives in its mode's coordinates is put where the image is, and
        // one shown without a destination gets its natural size there, scaled too.
        const double scale = std::min(double(sw) / 640, double(sh) / 480);
        const long ix = long((sw - 640 * scale) / 2), iy = long((sh - 480 * scale) / 2);
        char cmd[256], out[128] = {};
        long r[4] = {};
        snprintf(cmd, sizeof(cmd), "window v handle %lu", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(hwnd)));
        MCIERROR e = mciSendStringA("open dd2d9test.avi type avivideo alias v", nullptr, 0, nullptr);
        if (e == 0) {
            mciSendStringA(cmd, nullptr, 0, nullptr);
            mciSendStringA("put v destination at 160 120 320 240", nullptr, 0, nullptr);
            mciSendStringA("where v destination", out, sizeof(out), nullptr);
            sscanf(out, "%ld %ld %ld %ld", &r[0], &r[1], &r[2], &r[3]);
            const long ex = ix + long(160 * scale), ey = iy + long(120 * scale), ew = long(320 * scale), eh = long(240 * scale);
            CHECK(labs(r[0] - ex) <= 1 && labs(r[1] - ey) <= 1 && labs(r[2] - ew) <= 1 && labs(r[3] - eh) <= 1,
                  "MCI video: destination where the image is (%s; want %ld %ld %ld %ld)", out, ex, ey, ew, eh);
            // A close that fails (from a second thread) leaves the device known: its next
            // destination is translated too.
            OtherThreadMci other = {0, "close v", 0, 0};
            RunOnOtherThread(&other);
            mciSendStringA("put v destination at 0 0 320 240", nullptr, 0, nullptr);
            out[0] = 0;
            mciSendStringA("where v destination", out, sizeof(out), nullptr);
            sscanf(out, "%ld %ld %ld %ld", &r[0], &r[1], &r[2], &r[3]);
            if (other.close != 0)
                CHECK(labs(r[0] - ix) <= 1 && labs(r[1] - iy) <= 1 && labs(r[2] - ew) <= 1 && labs(r[3] - eh) <= 1,
                      "MCI video after a close that failed (%lu): a destination still where the image is (%s)",
                      static_cast<unsigned long>(other.close), out);
            else
                printf("SKIP MCI video, a close from a second thread: not refused here\n");
            mciSendStringA("close v", nullptr, 0, nullptr);
            mciSendStringA("open dd2d9test.avi type avivideo alias w", nullptr, 0, nullptr);
            snprintf(cmd, sizeof(cmd), "window w handle %lu", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(hwnd)));
            mciSendStringA(cmd, nullptr, 0, nullptr);
            mciSendStringA("play w window wait", nullptr, 0, nullptr);  // (Richman 4 plays with "window")
            out[0] = 0;
            mciSendStringA("where w destination", out, sizeof(out), nullptr);
            sscanf(out, "%ld %ld %ld %ld", &r[0], &r[1], &r[2], &r[3]);
            CHECK(labs(r[0] - ix) <= 1 && labs(r[1] - iy) <= 1 && labs(r[2] - long(32 * scale)) <= 1 &&
                      labs(r[3] - long(24 * scale)) <= 1,
                  "MCI video without a destination: its natural size, scaled, where the image is (%s)", out);
            mciSendStringA("close w", nullptr, 0, nullptr);
            // The player's window control (Rockman X4): a child window the game moves and sizes,
            // and the picture it places in it, in the mode's coordinates: both scaled.
            if (HWND mw = MCIWndCreateA(hwnd, GetModuleHandleA(nullptr),
                                        WS_CHILD | WS_VISIBLE | MCIWNDF_NOPLAYBAR | MCIWNDF_NOMENU |
                                            MCIWNDF_NOAUTOSIZEWINDOW | MCIWNDF_NOAUTOSIZEMOVIE,
                                        "dd2d9test.avi")) {
                MoveWindow(mw, 160, 120, 320, 240, FALSE);
                RECT wr = {};
                GetWindowRect(mw, &wr);  // (a child's: not translated)
                // Into the game window's real client coordinates (its ClientToScreen is translated).
                using PointFn = BOOL(WINAPI*)(HWND, POINT*);
                const auto realClientToScreen = reinterpret_cast<PointFn>(
                    RealExport(GetModuleHandleA("user32.dll"), "ClientToScreen"));
                POINT o = {0, 0};
                realClientToScreen(hwnd, &o);
                OffsetRect(&wr, -o.x, -o.y);
                const long ex = ix + long(160 * scale), ey = iy + long(120 * scale);
                CHECK(labs(wr.left - ex) <= 1 && labs(wr.top - ey) <= 1 && labs(wr.right - wr.left - long(320 * scale)) <= 1 &&
                          labs(wr.bottom - wr.top - long(240 * scale)) <= 1,
                      "MCIWnd window: moved and sized where the image is (%ld,%ld-%ld,%ld)", wr.left, wr.top, wr.right,
                      wr.bottom);
                RECT d = {0, 30, 320, 210}, got = {};
                SendMessageA(mw, MCIWNDM_PUT_DEST, 0, reinterpret_cast<LPARAM>(&d));
                SendMessageA(mw, MCIWNDM_GET_DEST, 0, reinterpret_cast<LPARAM>(&got));
                CHECK(labs(got.top - long(30 * scale)) <= 1 && labs(got.right - long(320 * scale)) <= 1 &&
                          labs(got.bottom - long(210 * scale)) <= 1,
                      "MCIWnd picture placed in the window scaled (%ld,%ld-%ld,%ld)", got.left, got.top, got.right, got.bottom);
                MCIWndDestroy(mw);
            }
            // The command interface (Road Rash): the player's own child window of the game's
            // (opened with it as parent), the picture put in it in the mode's coordinates: the
            // window over the image, the picture scaled in it.
            MCI_DGV_OPEN_PARMSA op = {};
            char aviName[] = "dd2d9test.avi";
            op.lpstrElementName = aviName;
            op.dwStyle = WS_CHILD;
            op.hWndParent = hwnd;
            if (mciSendCommandA(0, MCI_OPEN, MCI_OPEN_ELEMENT | MCI_DGV_OPEN_PARENT | MCI_DGV_OPEN_WS,
                                reinterpret_cast<DWORD_PTR>(&op)) == 0) {
                const MCIDEVICEID id = op.wDeviceID;
                MCI_DGV_WINDOW_PARMSA wp = {};
                wp.nCmdShow = SW_SHOW;
                mciSendCommandA(id, MCI_WINDOW, MCI_DGV_WINDOW_STATE, reinterpret_cast<DWORD_PTR>(&wp));
                MCI_DGV_PUT_PARMS pp = {};
                pp.rc = RECT{0, 20, 640, 440};  // (x, y, width, height)
                mciSendCommandA(id, MCI_PUT, MCI_DGV_PUT_DESTINATION | MCI_DGV_RECT, reinterpret_cast<DWORD_PTR>(&pp));
                MCI_DGV_RECT_PARMS wr2 = {};
                mciSendCommandA(id, MCI_WHERE, MCI_DGV_WHERE_DESTINATION, reinterpret_cast<DWORD_PTR>(&wr2));
                CHECK(wr2.rc.left == 0 && wr2.rc.top == 0 && labs(wr2.rc.right - long(640 * scale)) <= 1 &&
                          labs(wr2.rc.bottom - long(440 * scale)) <= 1,
                      "MCI command: the picture fills the player's window, scaled (%ld,%ld %ldx%ld)", wr2.rc.left,
                      wr2.rc.top, wr2.rc.right, wr2.rc.bottom);
                MCI_DGV_STATUS_PARMSA sp = {};
                sp.dwItem = MCI_DGV_STATUS_HWND;
                mciSendCommandA(id, MCI_STATUS, MCI_STATUS_ITEM, reinterpret_cast<DWORD_PTR>(&sp));
                RECT cw = {};
                if (HWND child = reinterpret_cast<HWND>(sp.dwReturn)) {
                    using RectFn = BOOL(WINAPI*)(HWND, RECT*);
                    const auto realWindowRect = reinterpret_cast<RectFn>(
                        RealExport(GetModuleHandleA("user32.dll"), "GetWindowRect"));
                    realWindowRect(child, &cw);
                }
                // (Only where the picture goes: the rest of the game's window stays the game's.)
                CHECK(labs(cw.left - ix) <= 1 && labs(cw.top - (iy + long(20 * scale))) <= 1 &&
                          labs(cw.right - cw.left - long(640 * scale)) <= 1 && labs(cw.bottom - cw.top - long(440 * scale)) <= 1,
                      "MCI command: the player's window where the picture goes (%ld,%ld-%ld,%ld)", cw.left, cw.top, cw.right,
                      cw.bottom);
                mciSendCommandA(id, MCI_CLOSE, 0, 0);
            }
            // Road Rash's way: a topmost 640x480 popup at 0,0 of its screen, the player's window
            // in it: the popup over the image, the movie scaled in it.
            WNDCLASSA pc = {};  // (as Road Rash's: no background brush)
            pc.lpfnWndProc = DefWindowProcA;
            pc.hInstance = GetModuleHandleA(nullptr);
            pc.lpszClassName = "dd2d9popup";
            RegisterClassA(&pc);
            HWND popup = CreateWindowExA(WS_EX_TOPMOST, "dd2d9popup", "AVI Window", WS_POPUP, 0, 0, 640, 480, nullptr,
                                         nullptr, GetModuleHandleA(nullptr), nullptr);
            if (popup) {
                // (Its background erased black, the class left as it is: the game's other windows
                // may share it.)
                auto erasedPixel = [](HWND w) {  // a white DC's first pixel after the window's erase
                    BITMAPINFO bi = {};
                    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), 4, -4, 1, 32, BI_RGB};
                    void* bits = nullptr;
                    HDC mdc = CreateCompatibleDC(nullptr);
                    HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                    DWORD erased = 1;
                    if (bmp && bits) {
                        HGDIOBJ old = SelectObject(mdc, bmp);
                        memset(bits, 0xFF, 4 * 4 * 4);
                        SendMessageA(w, WM_ERASEBKGND, reinterpret_cast<WPARAM>(mdc), 0);
                        GdiFlush();
                        erased = *static_cast<DWORD*>(bits) & 0xFFFFFF;
                        SelectObject(mdc, old);
                        DeleteObject(bmp);
                    }
                    DeleteDC(mdc);
                    return erased;
                };
                const DWORD erased = erasedPixel(popup);
                CHECK(!GetClassLongPtrA(popup, GCLP_HBRBACKGROUND) && erased == 0,
                      "the popup is black where nothing paints it (the menu showed around Road Rash's race intros): "
                      "erased 0x%06lx", erased);
                // One whose procedure paints its background itself and returns 0 keeps that.
                WNDCLASSA oc = pc;
                oc.lpfnWndProc = OwnErasePopupProc;
                oc.lpszClassName = "dd2d9popup2";
                RegisterClassA(&oc);
                if (HWND own = CreateWindowExA(WS_EX_TOPMOST, "dd2d9popup2", "", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
                                               GetModuleHandleA(nullptr), nullptr)) {
                    const DWORD kept = erasedPixel(own);
                    CHECK(kept == 0x0000FF, "a popup painting its own background and returning 0 keeps it: 0x%06lx", kept);
                    DestroyWindow(own);
                }
                UnregisterClassA("dd2d9popup2", GetModuleHandleA(nullptr));
                ShowWindow(popup, SW_SHOW);
                using RectFn = BOOL(WINAPI*)(HWND, RECT*);
                const auto realWindowRect = reinterpret_cast<RectFn>(
                    RealExport(GetModuleHandleA("user32.dll"), "GetWindowRect"));
                RECT pr = {};
                realWindowRect(popup, &pr);
                CHECK(labs(pr.left - ix) <= 1 && labs(pr.top - iy) <= 1 && labs(pr.right - pr.left - long(640 * scale)) <= 1 &&
                          labs(pr.bottom - pr.top - long(480 * scale)) <= 1,
                      "a popup at the game's 0,0, the mode's size: over the image (%ld,%ld-%ld,%ld)", pr.left, pr.top,
                      pr.right, pr.bottom);
                MCI_DGV_OPEN_PARMSA po = {};
                char aviName2[] = "dd2d9test.avi";
                po.lpstrElementName = aviName2;
                po.dwStyle = WS_CHILD;
                po.hWndParent = popup;
                if (mciSendCommandA(0, MCI_OPEN, MCI_OPEN_ELEMENT | MCI_DGV_OPEN_PARENT | MCI_DGV_OPEN_WS,
                                    reinterpret_cast<DWORD_PTR>(&po)) == 0) {
                    MCI_DGV_PUT_PARMS pp2 = {};
                    pp2.rc = RECT{0, 20, 640, 440};
                    mciSendCommandA(po.wDeviceID, MCI_PUT, MCI_DGV_PUT_DESTINATION | MCI_DGV_RECT,
                                    reinterpret_cast<DWORD_PTR>(&pp2));
                    MCI_DGV_RECT_PARMS w3 = {};
                    mciSendCommandA(po.wDeviceID, MCI_WHERE, MCI_DGV_WHERE_DESTINATION, reinterpret_cast<DWORD_PTR>(&w3));
                    MCI_DGV_STATUS_PARMSA sp2 = {};
                    sp2.dwItem = MCI_DGV_STATUS_HWND;
                    mciSendCommandA(po.wDeviceID, MCI_STATUS, MCI_STATUS_ITEM, reinterpret_cast<DWORD_PTR>(&sp2));
                    RECT cw2 = {};
                    if (HWND child = reinterpret_cast<HWND>(sp2.dwReturn)) realWindowRect(child, &cw2);
                    // Race intros (Road Rash): the picture's window from 20 rows down, the rest of
                    // the popup its own (black), not the menu behind.
                    CHECK(w3.rc.left == 0 && w3.rc.top == 0 && labs(w3.rc.right - long(640 * scale)) <= 1 &&
                              labs(w3.rc.bottom - long(440 * scale)) <= 1 && labs(cw2.left - pr.left) <= 1 &&
                              labs(cw2.top - (pr.top + long(20 * scale))) <= 1,
                          "MCI command in the game's popup: the player's window where the picture goes (%ld,%ld; picture "
                          "%ld,%ld %ldx%ld)",
                          cw2.left, cw2.top, w3.rc.left, w3.rc.top, w3.rc.right, w3.rc.bottom);
                    mciSendCommandA(po.wDeviceID, MCI_CLOSE, 0, 0);
                }
                DestroyWindow(popup);
            }
            // Rockman X Math's way: opened by command string in a child window of the game's,
            // the player's window asked for and centred with MoveWindow in the mode's
            // coordinates, then played: the window over the image, the movie scaled in it.
            snprintf(cmd, sizeof(cmd), "open dd2d9test.avi type avivideo alias x parent %lu style child",
                     static_cast<unsigned long>(reinterpret_cast<uintptr_t>(hwnd)));
            if (mciSendStringA(cmd, nullptr, 0, nullptr) == 0) {
                out[0] = 0;
                mciSendStringA("status x window handle", out, sizeof(out), nullptr);
                const HWND child = reinterpret_cast<HWND>(uintptr_t(strtoul(out, nullptr, 10)));
                MoveWindow(child, 304, 228, 32, 24, FALSE);
                mciSendStringA("play x wait", nullptr, 0, nullptr);
                using RectFn = BOOL(WINAPI*)(HWND, RECT*);
                const auto realWindowRect = reinterpret_cast<RectFn>(
                    RealExport(GetModuleHandleA("user32.dll"), "GetWindowRect"));
                RECT cw = {};
                realWindowRect(child, &cw);
                out[0] = 0;
                mciSendStringA("where x destination", out, sizeof(out), nullptr);
                sscanf(out, "%ld %ld %ld %ld", &r[0], &r[1], &r[2], &r[3]);
                const long ex = ix + long(304 * scale), ey = iy + long(228 * scale);
                CHECK(child && labs(cw.left - ex) <= 1 && labs(cw.top - ey) <= 1 &&
                          labs(cw.right - cw.left - long(32 * scale)) <= 2 && labs(cw.bottom - cw.top - long(24 * scale)) <= 2 &&
                          r[0] == 0 && r[1] == 0 && labs(r[2] - long(32 * scale)) <= 1 && labs(r[3] - long(24 * scale)) <= 1,
                      "MCI string, child window moved by the game: over the image (%ld,%ld-%ld,%ld), the movie scaled in "
                      "it (%s)",
                      cw.left, cw.top, cw.right, cw.bottom, out);
                mciSendStringA("close x", nullptr, 0, nullptr);
            }
            // Harukaze's way: the same through the command interface, with the player's window
            // handle kept in 16 bits (Windows takes the low half for the window).
            MCI_DGV_OPEN_PARMSA ho = {};
            char aviName3[] = "dd2d9test.avi";
            ho.lpstrElementName = aviName3;
            ho.dwStyle = WS_CHILD;
            ho.hWndParent = hwnd;
            if (mciSendCommandA(0, MCI_OPEN, MCI_OPEN_ELEMENT | MCI_DGV_OPEN_PARENT | MCI_DGV_OPEN_WS,
                                reinterpret_cast<DWORD_PTR>(&ho)) == 0) {
                MCI_DGV_STATUS_PARMSA hs = {};
                hs.dwItem = MCI_DGV_STATUS_HWND;
                mciSendCommandA(ho.wDeviceID, MCI_STATUS, MCI_STATUS_ITEM, reinterpret_cast<DWORD_PTR>(&hs));
                const HWND whole = reinterpret_cast<HWND>(hs.dwReturn);
                const HWND half = reinterpret_cast<HWND>(uintptr_t(hs.dwReturn & 0xFFFF));
                if (IsWindow(half)) {
                    MoveWindow(half, 304, 228, 32, 24, FALSE);
                    MCI_PLAY_PARMS hp = {};
                    mciSendCommandA(ho.wDeviceID, MCI_PLAY, MCI_WAIT, reinterpret_cast<DWORD_PTR>(&hp));
                    using RectFn = BOOL(WINAPI*)(HWND, RECT*);
                    const auto realWindowRect = reinterpret_cast<RectFn>(
                        RealExport(GetModuleHandleA("user32.dll"), "GetWindowRect"));
                    RECT cw = {};
                    realWindowRect(whole, &cw);
                    MCI_DGV_RECT_PARMS hw = {};
                    mciSendCommandA(ho.wDeviceID, MCI_WHERE, MCI_DGV_WHERE_DESTINATION, reinterpret_cast<DWORD_PTR>(&hw));
                    const long ex = ix + long(304 * scale), ey = iy + long(228 * scale);
                    CHECK(labs(cw.left - ex) <= 1 && labs(cw.top - ey) <= 1 && labs(cw.right - cw.left - long(32 * scale)) <= 2 &&
                              labs(cw.bottom - cw.top - long(24 * scale)) <= 2 && hw.rc.left == 0 && hw.rc.top == 0 &&
                              labs(hw.rc.right - long(32 * scale)) <= 1 && labs(hw.rc.bottom - long(24 * scale)) <= 1,
                          "MCI command, child window moved by a handle of 16 bits: over the image (%ld,%ld-%ld,%ld), the "
                          "movie scaled in it (%ld,%ld %ldx%ld)",
                          cw.left, cw.top, cw.right, cw.bottom, hw.rc.left, hw.rc.top, hw.rc.right, hw.rc.bottom);
                    // And its second thread's way: the movie's ID, which had been its CD player's,
                    // closed from there as the movie plays. Windows refuses, and the movie is
                    // still dd2d9's to place: its window moved again is translated again.
                    OtherThreadMci other = {ho.wDeviceID, nullptr, 0, 0};
                    RunOnOtherThread(&other);
                    MoveWindow(half, 100, 60, 32, 24, FALSE);
                    realWindowRect(whole, &cw);
                    if (other.close != 0)
                        CHECK(labs(cw.left - (ix + long(100 * scale))) <= 1 && labs(cw.top - (iy + long(60 * scale))) <= 1,
                              "MCI command after a close that failed (stop %lu, close %lu): the window still moved over "
                              "the image (%ld,%ld)",
                              static_cast<unsigned long>(other.stop), static_cast<unsigned long>(other.close), cw.left, cw.top);
                    else
                        printf("SKIP MCI command, a close from a second thread: not refused here\n");
                } else {
                    printf("SKIP MCI command, a window handle of 16 bits: not taken here\n");
                }
                mciSendCommandA(ho.wDeviceID, MCI_CLOSE, 0, 0);
            }
        } else {
            printf("SKIP MCI video: no AVI player (%lu)\n", static_cast<unsigned long>(e));
        }
        DeleteFileA("dd2d9test.avi");
    }
    {
        // The game sizing its window to the mode at 0,0 (Xuan-Yuan Sword 3 EX back from
        // Alt+Tab), before dd2d9 covers the screen again: the cursor is still kept on the
        // game's screen (the game draws it there unclipped).
        using SetPosFn = BOOL(WINAPI*)(int, int);
        const auto realSetCursorPos =
            reinterpret_cast<SetPosFn>(RealExport(GetModuleHandleA("user32.dll"), "SetCursorPos"));
        SetWindowPos(hwnd, nullptr, 0, 0, 640, 480, SWP_NOZORDER | SWP_NOACTIVATE);  // (not pumped: not covered yet)
        realSetCursorPos(sw - 1, sh - 1);
        POINT p = {};
        GetCursorPos(&p);
        CHECK(p.x >= 0 && p.x < 640 && p.y >= 0 && p.y < 480, "window the mode's size: the cursor on the game's screen (%ld,%ld)",
              p.x, p.y);
        for (int i = 0; i < 10; i++) {
            Pump();
            Sleep(20);
        }
    }
    {
        // Borderless: back in front after Alt+Tab, the primary isn't lost (the display never
        // left the game's mode; NFS High Stakes never restores its flip chain).
        DDSURFACEDESC2 pd = {};
        pd.dwSize = sizeof(pd);
        pd.dwFlags = DDSD_CAPS;
        pd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        IDirectDrawSurface7* prim = nullptr;
        if (SUCCEEDED(dd->CreateSurface(&pd, &prim, nullptr))) {
            SendMessageA(hwnd, WM_ACTIVATEAPP, FALSE, 0);
            Pump();
            CHECK(prim->IsLost() == DDERR_SURFACELOST, "borderless: primary lost in the background");
            g_lastSize = -1;
            g_sizeClient = RECT{-1, -1, -1, -1};
            g_sizeScreenW = -1;
            SendMessageA(hwnd, WM_ACTIVATEAPP, TRUE, 0);
            Pump();
            // While its window is restored, the game asks of its screen already in the mode
            // (Heroes of the Three Kingdoms 3 blitted its movie centred in 1920x1080).
            CHECK(g_sizeScreenW == -1 || (g_sizeClient.right == 640 && g_sizeClient.bottom == 480 && g_sizeScreenW == 640),
                  "borderless: coming back, the window procedure sees the mode (client %ldx%ld, screen width %d)",
                  g_sizeClient.right, g_sizeClient.bottom, g_sizeScreenW);
            hr = prim->IsLost();
            CHECK(hr == DD_OK, "borderless: primary not lost back in front (0x%08lx)", hr);
            prim->Release();
            // The window covering the monitor again: the game is told the mode's size, as by
            // GetClientRect (Xuan-Yuan Sword 3 EX made its buffers the monitor's size).
            CHECK(g_lastSize == -1 || (LOWORD(g_lastSize) == 640 && HIWORD(g_lastSize) == 480),
                  "borderless: WM_SIZE back in front in the mode's size (%d,%d)", LOWORD(g_lastSize), HIWORD(g_lastSize));
            HDC screen = GetDC(nullptr);
            CHECK(GetDeviceCaps(screen, HORZRES) == 640 && GetDeviceCaps(screen, VERTRES) == 480,
                  "borderless: GDI's screen size the mode's (%dx%d)", GetDeviceCaps(screen, HORZRES),
                  GetDeviceCaps(screen, VERTRES));
            ReleaseDC(nullptr, screen);
        }
    }
    {
        // The game minimizing and maximizing its own window while in front (Mega Man X, once,
        // back from its first Alt+Tab, with SC_MINIMIZE and SC_MAXIMIZE): back, it's told it's
        // active, once. Minimized without giving up the activation, as Windows 11 left Mega
        // Man X (and Wine does with SC_MINIMIZE too). (Minimized, Mega Man X draws nothing
        // until then.)
        SetForegroundWindow(hwnd);
        Pump();
        const bool front = GetForegroundWindow() == hwnd;
        g_activateTrue = 0;
        g_activeSeen = false;
        ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
        const bool frontMinimized = GetForegroundWindow() == hwnd;
        SendMessageA(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
        for (int i = 0; i < 5; i++) {
            Pump();
            Sleep(20);
        }
        if (!front) {
            fprintf(g_out, "SKIP minimized and maximized in front: the test window isn't in front\n");
        } else {
            CHECK(g_activateTrue == 1 && g_activeSeen,
                  "the game minimizing and maximizing its window: told it's active once back (%d WM_ACTIVATEAPP, in front "
                  "while minimized: %d)",
                  g_activateTrue, frontMinimized);
        }
        ShowWindow(hwnd, SW_RESTORE);
        Pump();
    }
    {
        // FlyingSB's Alt+Tab: out of exclusive mode while minimized, back into it (and its
        // mode) before Windows restores the window. Restored, the window shows the screen
        // (its region was once made from the icon's rectangle, hiding everything).
        ShowWindow(hwnd, SW_MINIMIZE);
        Pump();
        dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
        dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        dd->SetDisplayMode(640, 480, 32, 0, 0);
        ShowWindow(hwnd, SW_RESTORE);
        for (int i = 0; i < 10; i++) {
            Pump();
            Sleep(20);
        }
        using RectFn = BOOL(WINAPI*)(HWND, RECT*);
        const auto realWindowRect =
            reinterpret_cast<RectFn>(RealExport(GetModuleHandleA("user32.dll"), "GetWindowRect"));
        RECT w = {}, box = {};
        realWindowRect(hwnd, &w);
        const int region = GetWindowRgnBox(hwnd, &box);
        OffsetRect(&box, w.left, w.top);  // (relative to the window)
        CHECK(region == ERROR || (box.left <= 0 && box.top <= 0 && box.right >= sw && box.bottom >= sh),
              "restored after going exclusive while minimized: its region shows the screen (%d: %ld,%ld-%ld,%ld)", region,
              box.left, box.top, box.right, box.bottom);
        // One monitor: no region at all (the frame is off the screen; under Wine a region
        // became an X11 shape that cut into the picture once the game changed its frame).
        CHECK(GetSystemMetrics(SM_CMONITORS) > 1 || region == ERROR,
              "exclusive mode on the only monitor: the window has no region (%d, %d monitor(s))", region,
              GetSystemMetrics(SM_CMONITORS));
    }
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    CHECK(ScreenMetric(SM_CXSCREEN) == sw, "the real screen again out of exclusive mode");
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

// Overlays (emulated): none offered to a windowed program; in exclusive mode (at the screen's
// own size) UpdateOverlay and its siblings, and the presenter laying them over the primary
// (read back from the screen): a YUY2 overlay stretched behind a destination key, an RGB one
// with a source key, z-order, flip chains.
static IDirectDrawSurface7* MakeOverlay(IDirectDraw7* dd, int w, int h, DWORD fourCC, DWORD extraCaps = 0, DWORD backs = 0) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | (backs ? DDSD_BACKBUFFERCOUNT : 0);
    d.dwWidth = w;
    d.dwHeight = h;
    d.dwBackBufferCount = backs;
    d.ddsCaps.dwCaps = DDSCAPS_OVERLAY | extraCaps;
    d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
    if (fourCC) {
        d.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
        d.ddpfPixelFormat.dwFourCC = fourCC;
    } else {
        d.ddpfPixelFormat.dwFlags = DDPF_RGB;
        d.ddpfPixelFormat.dwRGBBitCount = 16;
        d.ddpfPixelFormat.dwRBitMask = 0xF800;
        d.ddpfPixelFormat.dwGBitMask = 0x07E0;
        d.ddpfPixelFormat.dwBBitMask = 0x001F;
    }
    IDirectDrawSurface7* s = nullptr;
    HRESULT hr = dd->CreateSurface(&d, &s, nullptr);
    CHECK(SUCCEEDED(hr), "overlay: CreateSurface %dx%d fourcc 0x%08lx -> 0x%08lx", w, h, fourCC, hr);
    return s;
}

static HRESULT WINAPI ListZ(IDirectDrawSurface7* s, DDSURFACEDESC2*, void* ctx) {
    static_cast<std::vector<IDirectDrawSurface7*>*>(ctx)->push_back(s);
    s->Release();
    return DDENUMRET_OK;
}

// A windowed video player: its window's background is the key colour (painted by GDI, which
// dd2d9's primary doesn't see), its left half painted black over it. The overlay shows only
// over the key, as on a real screen.
static LRESULT CALLBACK KeyWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto paintLeft = [h](HDC dc) {
        RECT c;
        GetClientRect(h, &c);
        c.right /= 2;
        FillRect(dc, &c, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    };
    if (m == WM_PRINTCLIENT) {
        paintLeft(reinterpret_cast<HDC>(w));
        return 0;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paintLeft(dc);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static int RunOverlayWindowed() {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = KeyWndProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ddtestkey";
    wc.hbrBackground = CreateSolidBrush(RGB(255, 0, 255));
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "ddtestkey", "overlay", WS_POPUP, 0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    IDirectDraw7* dd = nullptr;
    DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* prim = nullptr;
    dd->CreateSurface(&d, &prim, nullptr);
    IDirectDrawSurface7* ov = MakeOverlay(dd, 32, 32, MAKEFOURCC('Y', 'U', 'Y', '2'));
    if (!prim || !ov) return 1;
    d = {};
    d.dwSize = sizeof(d);
    ov->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 16; x++) reinterpret_cast<DWORD*>(static_cast<BYTE*>(d.lpSurface) + y * d.lPitch)[x] = 0x80EB80EB;  // white
    ov->Unlock(nullptr);
    DDOVERLAYFX ofx = {};
    ofx.dwSize = sizeof(ofx);
    ofx.dckDestColorkey.dwColorSpaceLowValue = ofx.dckDestColorkey.dwColorSpaceHighValue = 0xFF00FF;  // (32-bit desktop)
    RECT r = {0, 0, 320, 240};
    CHECK(ov->UpdateOverlay(nullptr, prim, &r, DDOVER_SHOW | DDOVER_KEYDESTOVERRIDE | DDOVER_DDFX, &ofx) == DD_OK,
          "overlay (windowed): shown behind the window's key colour");
    // (Messages answered all the while, as a program's loop does: dd2d9 asks the window to
    // paint itself, WM_PRINT, and waits 50 ms for it; unanswered, the overlay shows whole.)
    for (int i = 0; i < 100; i++) {
        Pump();
        Sleep(10);
    }
    HDC sdc = GetDC(nullptr);
    const COLORREF left = GetPixel(sdc, 60, 120) & 0xFFFFFF, right = GetPixel(sdc, 260, 120) & 0xFFFFFF;
    ReleaseDC(nullptr, sdc);
    CHECK(right == RGB(255, 255, 255) && left != RGB(255, 255, 255),
          "overlay (windowed): over the GDI key colour only (left %06lx, right %06lx)", left, right);
    ov->Release();
    prim->Release();
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

// DDSCL_CREATEDEVICEWINDOW with DDSCL_NOWINDOWCHANGES: the focus window stays small, the
// device window covers the screen and shows the picture.
static int RunDeviceWindow() {
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND focus = MakeWindow(WS_POPUP, 100, 100);
    IDirectDraw7* dd = nullptr;
    DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    HRESULT hr = dd->SetCooperativeLevel(focus, DDSCL_SETFOCUSWINDOW);
    HRESULT hr2 = dd->SetCooperativeLevel(nullptr, DDSCL_CREATEDEVICEWINDOW | DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_NOWINDOWCHANGES);
    CHECK(SUCCEEDED(hr) && SUCCEEDED(hr2), "device window: focus window set (0x%08lx), device window made (0x%08lx)", hr, hr2);
    dd->SetDisplayMode(sw, sh, 32, 0, 0);
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* prim = nullptr;
    dd->CreateSurface(&d, &prim, nullptr);
    if (prim) {
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = 0x00FF00;
        prim->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    }
    for (int i = 0; i < 10; i++) {
        Pump();
        Sleep(100);
    }
    HWND dev = FindWindowA("DirectDrawDeviceWnd", nullptr);
    HDC sdc = GetDC(nullptr);
    const COLORREF far_ = GetPixel(sdc, sw - 10, sh - 10) & 0xFFFFFF;
    ReleaseDC(nullptr, sdc);
    RECT fr = {};
    GetWindowRect(focus, &fr);
    CHECK(dev && IsWindowVisible(dev) && far_ == RGB(0, 255, 0) && fr.right - fr.left == 100,
          "device window: shown, the picture on it over the screen (%06lx), the focus window left alone (%ld wide)", far_,
          fr.right - fr.left);
    if (prim) prim->Release();
    dd->RestoreDisplayMode();
    dd->SetCooperativeLevel(focus, DDSCL_NORMAL);
    CHECK(!IsWindow(dev), "device window: gone with normal mode");
    dd->Release();
    DestroyWindow(focus);
    return 0;
}

// DDSCL_NOWINDOWCHANGES with the mode shown scaled (run with display=borderless): Hydro
// Thunder takes exclusive mode with 0x55 and its 640x480 window. ddraw.dll leaves the window
// alone, the switched 640x480 mode filling the screen with it; borderless switches nothing,
// so the window covers the screen instead. Back in normal mode (0xC) it isn't topmost.
static int RunNoWindowChanges() {
    // The real functions: dd2d9 shows the game its mode's coordinates through the imports.
    HMODULE u32 = GetModuleHandleA("user32.dll");
    using MetricsFn = int(WINAPI*)(int);
    using RectFn = BOOL(WINAPI*)(HWND, RECT*);
    const auto realMetrics = reinterpret_cast<MetricsFn>(RealExport(u32, "GetSystemMetrics"));
    const auto realWindowRect = reinterpret_cast<RectFn>(RealExport(u32, "GetWindowRect"));
    const int sw = realMetrics(SM_CXSCREEN), sh = realMetrics(SM_CYSCREEN);
    HWND hwnd = MakeWindow(WS_POPUP, 640, 480);
    IDirectDraw7* dd = nullptr;
    DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    const DWORD coop = DDSCL_FULLSCREEN | DDSCL_NOWINDOWCHANGES | DDSCL_EXCLUSIVE | DDSCL_ALLOWMODEX;  // 0x55
    HRESULT hr = dd->SetCooperativeLevel(hwnd, coop);
    HRESULT hr2 = dd->SetDisplayMode(640, 480, 16, 0, 0);
    for (int i = 0; i < 10; i++) {
        Pump();
        Sleep(20);
    }
    RECT c = {};  // (a popup: its window rectangle is its client area)
    realWindowRect(hwnd, &c);
    CHECK(SUCCEEDED(hr) && SUCCEEDED(hr2) && c.left == 0 && c.top == 0 && c.right == sw && c.bottom == sh,
          "no window changes, borderless: the window covers the screen (0x%08lx, 0x%08lx; %ld,%ld-%ld,%ld)", hr, hr2,
          c.left, c.top, c.right, c.bottom);
    // Hydro Thunder then sizes its window to the mode at 0,0 and loads for two seconds
    // without reading its messages: still covering, without a message pumped.
    SetWindowPos(hwnd, nullptr, 0, 0, 640, 480, SWP_NOZORDER | SWP_NOACTIVATE);
    realWindowRect(hwnd, &c);
    CHECK(c.left == 0 && c.top == 0 && c.right == sw && c.bottom == sh,
          "borderless: the game sizing its window to the mode keeps it covering, messages unread (%ld,%ld-%ld,%ld)", c.left,
          c.top, c.right, c.bottom);
    // The same with its style set again and SWP_FRAMECHANGED, as games set their window up:
    // still covering, no message read.
    SetWindowLongA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, nullptr, 0, 0, 640, 480, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    realWindowRect(hwnd, &c);
    CHECK(c.left == 0 && c.top == 0 && c.right == sw && c.bottom == sh,
          "borderless: the same with a frame change (SWP_FRAMECHANGED) keeps it covering (%ld,%ld-%ld,%ld)", c.left, c.top,
          c.right, c.bottom);
    for (int i = 0; i < 5; i++) {  // (the check dd2d9 posts after a frame change, done)
        Pump();
        Sleep(20);
    }
    // A smaller window (the game going windowed) is the game's.
    SetWindowPos(hwnd, nullptr, 100, 100, 400, 300, SWP_NOZORDER | SWP_NOACTIVATE);
    for (int i = 0; i < 5; i++) {
        Pump();
        Sleep(20);
    }
    realWindowRect(hwnd, &c);
    CHECK(c.left == 100 && c.top == 100 && c.right == 500 && c.bottom == 400,
          "borderless: a smaller window stays as the game sized it (%ld,%ld-%ld,%ld)", c.left, c.top, c.right, c.bottom);
    // A game setting its window back to the mode's size from its window procedure, in the
    // middle of dd2d9's own covering (Hydro Thunder: the check above didn't catch it).
    g_keepW = 640, g_keepH = 480;
    dd->SetDisplayMode(800, 600, 16, 0, 0);  // (dd2d9 covers again, the game answers)
    dd->SetDisplayMode(640, 480, 16, 0, 0);
    realWindowRect(hwnd, &c);
    g_keepW = g_keepH = 0;
    CHECK(c.left == 0 && c.top == 0 && c.right == sw && c.bottom == sh,
          "borderless: a game sizing its window back from its window procedure keeps it covering (%ld,%ld-%ld,%ld)",
          c.left, c.top, c.right, c.bottom);
    dd->RestoreDisplayMode();
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL | DDSCL_NOWINDOWCHANGES);  // 0xC
    Pump();
    CHECK(!(GetWindowLongA(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST), "no window changes, borderless: not topmost in normal mode");
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

// ---- audit: how the game's picture lands on the real screen, scenario by scenario, under
// the display setting the run has (borderless: covering the screen, scaled; window: a
// centered window of the mode's size). Each scenario fills the primary green and looks at
// the real screen (the real functions: dd2d9 shows the game its mode's coordinates).
namespace audit {
using MetricsFn = int(WINAPI*)(int);
using RectFn = BOOL(WINAPI*)(HWND, RECT*);
MetricsFn realMetrics;
RectFn realWindowRect, realClientRect;
int sw, sh;
bool borderless;

void Init(bool isBorderless) {
    HMODULE u32 = GetModuleHandleA("user32.dll");
    realMetrics = reinterpret_cast<MetricsFn>(RealExport(u32, "GetSystemMetrics"));
    realWindowRect = reinterpret_cast<RectFn>(RealExport(u32, "GetWindowRect"));
    realClientRect = reinterpret_cast<RectFn>(RealExport(u32, "GetClientRect"));
    sw = realMetrics(SM_CXSCREEN);
    sh = realMetrics(SM_CYSCREEN);
    borderless = isBorderless;
}
void Settle(int n = 10) {
    for (int i = 0; i < n; i++) {
        Pump();
        Sleep(30);
    }
}
COLORREF Screen(int x, int y) {
    HDC dc = GetDC(nullptr);
    const COLORREF c = GetPixel(dc, x, y) & 0xFFFFFF;
    ReleaseDC(nullptr, dc);
    return c;
}
void FillGreen(IDirectDraw7* dd, IDirectDrawSurface7** keep) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    *keep = nullptr;
    if (FAILED(dd->CreateSurface(&d, keep, nullptr)) || !*keep) return;
    DDBLTFX fx = {};
    fx.dwSize = sizeof(fx);
    fx.dwFillColor = 0x07E0;  // green in 565, and enough of it in 8888 too
    (*keep)->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
}
// The picture where the setting puts it: borderless, the screen's middle and the middle of its
// bottom edge (scaled to the full height, not the mode's 480 lines in the corner); window, the
// middle of a centered window whose client area is the mode's size.
void Expect(HWND hwnd, int mw, int mh, const char* what) {
    Settle();
    if (borderless) {
        const COLORREF mid = Screen(sw / 2, sh / 2), low = Screen(sw / 2, sh - 4);
        CHECK(mid & 0xFF00 && low & 0xFF00, "audit %s: the picture covers the screen (middle %06lx, bottom %06lx)", what,
              mid, low);
    } else {
        RECT c = {};
        realClientRect(hwnd, &c);
        RECT w = {};
        realWindowRect(hwnd, &w);
        CHECK(c.right == mw && c.bottom == mh, "audit %s: a window of the mode's size (%ldx%ld at %ld,%ld)", what, c.right,
              c.bottom, w.left, w.top);
    }
}
IDirectDraw7* Make() {
    IDirectDraw7* dd = nullptr;
    DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    return dd;
}
void Done(IDirectDraw7* dd, IDirectDrawSurface7* prim, HWND hwnd, HWND coopWnd = nullptr) {
    if (prim) prim->Release();
    dd->RestoreDisplayMode();
    dd->SetCooperativeLevel(coopWnd ? coopWnd : hwnd, DDSCL_NORMAL);
    dd->Release();
    if (hwnd) DestroyWindow(hwnd);
    Settle(3);
}
}  // namespace audit

static int RunAudit(bool borderless) {
    using namespace audit;
    Init(borderless);
    const DWORD fs = DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN;
    IDirectDrawSurface7* prim = nullptr;
    {  // A. The usual: a popup the desktop's size.
        HWND h = MakeWindow(WS_POPUP, sw, sh);
        IDirectDraw7* dd = Make();
        dd->SetCooperativeLevel(h, fs);
        dd->SetDisplayMode(640, 480, 16, 0, 0);
        FillGreen(dd, &prim);
        Expect(h, 640, 480, "A usual");
        Done(dd, prim, h);
    }
    {  // B. A popup the mode's size, NOWINDOWCHANGES (Hydro Thunder).
        HWND h = MakeWindow(WS_POPUP, 640, 480);
        IDirectDraw7* dd = Make();
        dd->SetCooperativeLevel(h, fs | DDSCL_NOWINDOWCHANGES | DDSCL_ALLOWMODEX);
        dd->SetDisplayMode(640, 480, 16, 0, 0);
        FillGreen(dd, &prim);
        Expect(h, 640, 480, "B no window changes");
        Done(dd, prim, h);
    }
    {  // C. An overlapped window with its frame, small, exclusive.
        HWND h = MakeWindow(WS_OVERLAPPEDWINDOW, 400, 300);
        IDirectDraw7* dd = Make();
        dd->SetCooperativeLevel(h, fs);
        dd->SetDisplayMode(640, 480, 16, 0, 0);
        FillGreen(dd, &prim);
        Expect(h, 640, 480, "C framed window");
        Done(dd, prim, h);
    }
    {  // D. The mode changed while running: 640x480, 800x600, back.
        HWND h = MakeWindow(WS_POPUP, sw, sh);
        IDirectDraw7* dd = Make();
        dd->SetCooperativeLevel(h, fs);
        dd->SetDisplayMode(640, 480, 16, 0, 0);
        dd->SetDisplayMode(800, 600, 16, 0, 0);
        FillGreen(dd, &prim);
        Expect(h, 800, 600, "D mode change 800x600");
        if (prim) prim->Release();
        dd->SetDisplayMode(640, 480, 16, 0, 0);
        FillGreen(dd, &prim);
        Expect(h, 640, 480, "D mode change back to 640x480");
        Done(dd, prim, h);
    }
    {  // E. The window hidden after the mode is set, shown again.
        HWND h = MakeWindow(WS_POPUP, sw, sh);
        IDirectDraw7* dd = Make();
        dd->SetCooperativeLevel(h, fs);
        dd->SetDisplayMode(640, 480, 16, 0, 0);
        ShowWindow(h, SW_HIDE);
        Settle(3);
        ShowWindow(h, SW_SHOW);
        FillGreen(dd, &prim);
        Expect(h, 640, 480, "E hidden and shown");
        Done(dd, prim, h);
    }
    {  // F. The game's own window sized to the mode at the centre of the desktop.
        HWND h = MakeWindow(WS_POPUP, 640, 480);
        SetWindowPos(h, nullptr, (sw - 640) / 2, (sh - 480) / 2, 640, 480, SWP_NOZORDER);
        IDirectDraw7* dd = Make();
        dd->SetCooperativeLevel(h, fs);
        dd->SetDisplayMode(640, 480, 16, 0, 0);
        FillGreen(dd, &prim);
        Expect(h, 640, 480, "F centered mode-size window");
        Done(dd, prim, h);
    }
    {  // G. Focus and device windows (SETFOCUSWINDOW, then CREATEDEVICEWINDOW).
        HWND focus = MakeWindow(WS_POPUP, 100, 100);
        IDirectDraw7* dd = Make();
        dd->SetCooperativeLevel(focus, DDSCL_SETFOCUSWINDOW);
        dd->SetCooperativeLevel(nullptr, DDSCL_CREATEDEVICEWINDOW | fs);
        dd->SetDisplayMode(640, 480, 16, 0, 0);
        FillGreen(dd, &prim);
        // (Borderless: the device window covers the screen. Window: the game's focus window is
        // the centered window, the device window hidden.)
        Expect(focus, 640, 480, "G device window");
        if (prim) prim->Release();
        dd->RestoreDisplayMode();
        dd->SetCooperativeLevel(focus, DDSCL_NORMAL);
        dd->Release();
        DestroyWindow(focus);
        Settle(3);
    }
    {  // H. Normal mode with SetDisplayMode, a mode-size popup at 0,0 drawn into (ddraw.dll
       //    changes the real mode in normal mode too: the window then fills the screen).
        HWND h = MakeWindow(WS_POPUP, 640, 480);
        IDirectDraw7* dd = Make();
        dd->SetCooperativeLevel(h, DDSCL_NORMAL);
        dd->SetDisplayMode(640, 480, 16, 0, 0);
        FillGreen(dd, &prim);
        // Known: borderless shows this in the corner (the window, normal mode, is the game's:
        // nothing covers). No game is known to do it; recorded, not checked.
        Settle();
        fprintf(g_out, "INFO audit H normal mode with a display mode (%s): screen middle %06lx\n",
                borderless ? "borderless" : "window", Screen(sw / 2, sh / 2));
        Done(dd, prim, h);
    }
    {  // I. What the game reads of the screen after SetDisplayMode, by the ways it may ask.
        HWND h = MakeWindow(WS_POPUP, sw, sh);
        IDirectDraw7* dd = Make();
        dd->SetCooperativeLevel(h, fs);
        dd->SetDisplayMode(640, 480, 16, 0, 0);
        Settle(3);
        RECT desk = {};
        GetWindowRect(GetDesktopWindow(), &desk);
        MONITORINFO mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
        DEVMODEA dm = {};
        dm.dmSize = sizeof(dm);
        EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm);
        HDC dc = GetDC(nullptr);
        const int hres = GetDeviceCaps(dc, HORZRES);
        ReleaseDC(nullptr, dc);
        const int cx = GetSystemMetrics(SM_CXSCREEN);
        CHECK(cx == 640 && hres == 640 && desk.right - desk.left == 640 && mi.rcMonitor.right - mi.rcMonitor.left == 640 &&
                  dm.dmPelsWidth == 640 && dm.dmPelsHeight == 480,
              "audit I: every way of asking says the mode's 640 (SM_CXSCREEN %d, HORZRES %d, desktop window %ld, monitor "
              "%ld, current mode %lux%lu)",
              cx, hres, desk.right - desk.left, mi.rcMonitor.right - mi.rcMonitor.left, dm.dmPelsWidth, dm.dmPelsHeight);
        // J. The game putting the "current" mode it read back with ChangeDisplaySettings: done,
        //    the real display untouched.
        const LONG cr = ChangeDisplaySettingsA(&dm, CDS_FULLSCREEN);
        Settle(3);
        CHECK(cr == DISP_CHANGE_SUCCESSFUL && realMetrics(SM_CXSCREEN) == sw,
              "audit J: ChangeDisplaySettings to the mode shown: succeeds, the real display stays (%ld, %d wide)", cr,
              realMetrics(SM_CXSCREEN));
        Done(dd, nullptr, h);
    }
    return 0;
}

static int RunOverlay() {
    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = MakeWindow(WS_POPUP, sw, sh);
    IDirectDraw7* dd = nullptr;
    DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr);
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    DDCAPS hal = {}, hel = {};
    hal.dwSize = hel.dwSize = sizeof(DDCAPS);
    dd->GetCaps(&hal, nullptr);
    CHECK((hal.dwCaps & DDCAPS_OVERLAY) && (hal.dwCKeyCaps & DDCKEYCAPS_DESTOVERLAY), "overlay: offered to a windowed program");
    dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
    CHECK(SUCCEEDED(dd->SetDisplayMode(sw, sh, 32, 0, 0)), "overlay: exclusive at %dx%dx32", sw, sh);
    hal = {};
    hal.dwSize = sizeof(DDCAPS);
    dd->GetCaps(&hal, &hel);
    CHECK((hal.dwCaps & DDCAPS_OVERLAY) && (hal.dwCKeyCaps & DDCKEYCAPS_DESTOVERLAY) && (hal.ddsCaps.dwCaps & DDSCAPS_OVERLAY) &&
              !(hel.dwCaps & DDCAPS_OVERLAY),
          "overlay: caps 0x%08lx, key caps 0x%08lx, emulation layer 0x%08lx", hal.dwCaps, hal.dwCKeyCaps, hel.dwCaps);
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS;
    d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    IDirectDrawSurface7* prim = nullptr;
    dd->CreateSurface(&d, &prim, nullptr);
    if (!prim) return 1;
    DDBLTFX fx = {};
    fx.dwSize = sizeof(fx);
    RECT all = {0, 0, 320, 240}, keyArea = {40, 40, 160, 160};
    fx.dwFillColor = 0x000040;
    prim->Blt(&all, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    fx.dwFillColor = 0xFF00FF;
    prim->Blt(&keyArea, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    DDCOLORKEY key = {0xFF00FF, 0xFF00FF};
    CHECK(prim->SetColorKey(DDCKEY_DESTOVERLAY, &key) == DD_OK, "overlay: destination key on the primary");
    IDirectDrawSurface7* plain = Make32(dd, 8, 8);
    CHECK(plain->SetColorKey(DDCKEY_DESTOVERLAY, &key) == DDERR_NOTAOVERLAYSURFACE, "overlay: no destination key on an offscreen surface");

    // A: YUY2 32x32, left half white, right half black.
    IDirectDrawSurface7* a = MakeOverlay(dd, 32, 32, MAKEFOURCC('Y', 'U', 'Y', '2'));
    // B: R5G6B5 16x16, red with a green 8x8 middle (its source key).
    IDirectDrawSurface7* b = MakeOverlay(dd, 16, 16, 0);
    if (!a || !b) return 1;
    d = {};
    d.dwSize = sizeof(d);
    a->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
    CHECK(d.ddsCaps.dwCaps == (DDSCAPS_OVERLAY | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM), "overlay: caps 0x%08lx", d.ddsCaps.dwCaps);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 16; x++)
            reinterpret_cast<DWORD*>(static_cast<BYTE*>(d.lpSurface) + y * d.lPitch)[x] = x < 8 ? 0x80EB80EB : 0x80108010;
    a->Unlock(nullptr);
    d = {};
    d.dwSize = sizeof(d);
    b->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            reinterpret_cast<WORD*>(static_cast<BYTE*>(d.lpSurface) + y * d.lPitch)[x] =
                (x >= 4 && x < 12 && y >= 4 && y < 12) ? 0x07E0 : 0xF800;
    b->Unlock(nullptr);
    DDCOLORKEY green = {0x07E0, 0x07E0};
    CHECK(b->SetColorKey(DDCKEY_SRCOVERLAY, &green) == DD_OK, "overlay: source key");

    LONG x = -1, y = -1;
    CHECK(a->GetOverlayPosition(&x, &y) == DDERR_OVERLAYNOTVISIBLE && x == 0 && y == 0, "overlay: hidden, no position");
    CHECK(plain->UpdateOverlay(nullptr, prim, nullptr, DDOVER_SHOW, nullptr) == DDERR_NOTAOVERLAYSURFACE, "overlay: not an overlay");
    RECT r = {20, 20, 180, 180};
    CHECK(a->UpdateOverlay(nullptr, prim, &r, DDOVER_SHOW | DDOVER_KEYSRC, nullptr) == DDERR_INVALIDPARAMS,
          "overlay: DDOVER_KEYSRC without a source key");
    CHECK(a->UpdateOverlay(nullptr, prim, &r, DDOVER_SHOW | DDOVER_AUTOFLIP, nullptr) == DDERR_INVALIDPARAMS, "overlay: no autoflip");
    RECT bad = {300, 0, 200, 10};
    CHECK(a->UpdateOverlay(nullptr, prim, &bad, DDOVER_SHOW, nullptr) == DDERR_INVALIDRECT, "overlay: bad rectangle");
    CHECK(a->UpdateOverlay(nullptr, prim, &r, DDOVER_SHOW | DDOVER_KEYDEST, nullptr) == DD_OK, "overlay: A shown behind the destination key");
    RECT rb = {200, 40, 264, 104};
    CHECK(b->UpdateOverlay(nullptr, prim, &rb, DDOVER_SHOW | DDOVER_KEYSRC, nullptr) == DD_OK, "overlay: B shown with its source key");
    CHECK(a->GetOverlayPosition(&x, &y) == DD_OK && x == 20 && y == 20, "overlay: position %ld,%ld", x, y);
    CHECK(a->SetOverlayPosition(100000, 0) == DDERR_INVALIDPOSITION, "overlay: position off the screen");
    CHECK(a->SetOverlayPosition(10, 10) == DD_OK && a->GetOverlayPosition(&x, &y) == DD_OK && x == 10 && y == 10,
          "overlay: moved to %ld,%ld", x, y);
    a->SetOverlayPosition(20, 20);
    DDSCAPS2 caps = {};
    a->GetCaps(&caps);
    CHECK(caps.dwCaps & DDSCAPS_VISIBLE, "overlay: shown, DDSCAPS_VISIBLE");
    hal = {};
    hal.dwSize = sizeof(hal);
    dd->GetCaps(&hal, nullptr);
    CHECK(hal.dwCurrVisibleOverlays == 2, "overlay: %lu visible", hal.dwCurrVisibleOverlays);

    // Z-order: the newer in front; then A sent to the front.
    std::vector<IDirectDrawSurface7*> z;
    a->EnumOverlayZOrders(1 /* DDENUMOVERLAYZ_FRONTTOBACK */, &z, ListZ);
    CHECK(z.size() == 2 && z[0] == b && z[1] == a, "overlay: z-order B, A");
    CHECK(a->UpdateOverlayZOrder(0 /* DDOVERZ_SENDTOFRONT */, nullptr) == DD_OK, "overlay: A to the front");
    z.clear();
    b->EnumOverlayZOrders(0 /* DDENUMOVERLAYZ_BACKTOFRONT */, &z, ListZ);
    CHECK(z.size() == 2 && z[0] == b && z[1] == a, "overlay: back to front B, A");

    // On the screen.
    for (int i = 0; i < 10; i++) {
        Pump();
        Sleep(100);
    }
    HDC sdc = GetDC(nullptr);
    auto px = [&](int sx, int sy) { return GetPixel(sdc, sx, sy) & 0xFFFFFF; };
    const COLORREF blue = RGB(0, 0, 0x40), white = RGB(255, 255, 255), black = RGB(0, 0, 0), red = RGB(255, 0, 0);
    CHECK(px(30, 30) == blue, "overlay: outside the key, the primary (%06lx)", px(30, 30));
    CHECK(px(60, 100) == white, "overlay: A's left half in the key (%06lx)", px(60, 100));
    CHECK(px(150, 100) == black, "overlay: A's right half in the key (%06lx)", px(150, 100));
    CHECK(px(170, 100) == blue, "overlay: past the key, the primary (%06lx)", px(170, 100));
    CHECK(px(210, 50) == red, "overlay: B (%06lx)", px(210, 50));
    CHECK(px(232, 72) == blue, "overlay: B's keyed middle shows the primary (%06lx)", px(232, 72));
    // Mirrored left to right: A's black half on the left.
    DDOVERLAYFX ofx = {};
    ofx.dwSize = sizeof(ofx);
    ofx.dwDDFX = DDOVERFX_MIRRORLEFTRIGHT;
    CHECK(a->UpdateOverlay(nullptr, prim, &r, DDOVER_SHOW | DDOVER_KEYDEST | DDOVER_DDFX, &ofx) == DD_OK, "overlay: A mirrored");
    for (int i = 0; i < 5; i++) {
        Pump();
        Sleep(100);
    }
    CHECK(px(60, 100) == black && px(150, 100) == white, "overlay: mirrored (%06lx %06lx)", px(60, 100), px(150, 100));
    // A blit into a visible overlay shows.
    fx.dwFillColor = 0xFFE0;  // yellow
    b->Blt(nullptr, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    for (int i = 0; i < 5; i++) {
        Pump();
        Sleep(100);
    }
    CHECK(px(210, 50) == RGB(255, 255, 0) && px(232, 72) == RGB(255, 255, 0), "overlay: B filled (%06lx %06lx)", px(210, 50),
          px(232, 72));
    CHECK(a->UpdateOverlay(nullptr, a, nullptr, DDOVER_SHOW, nullptr) == DDERR_INVALIDSURFACETYPE, "overlay: not on an overlay");
    CHECK(a->UpdateOverlay(nullptr, prim, nullptr, DDOVER_HIDE, nullptr) == DD_OK, "overlay: A hidden");
    for (int i = 0; i < 5; i++) {
        Pump();
        Sleep(100);
    }
    CHECK(px(60, 100) == RGB(255, 0, 255), "overlay: hidden, the key colour (%06lx)", px(60, 100));
    CHECK(a->GetOverlayPosition(&x, &y) == DDERR_OVERLAYNOTVISIBLE, "overlay: hidden again");
    ReleaseDC(nullptr, sdc);

    // A flip chain of overlays; DDFLIP_EVEN/ODD only for those.
    IDirectDrawSurface7* chain = MakeOverlay(dd, 16, 16, 0, DDSCAPS_FLIP | DDSCAPS_COMPLEX, 1);
    if (chain) {
        CHECK(chain->Flip(nullptr, DDFLIP_WAIT) == DD_OK, "overlay: flip chain flips");
        CHECK(chain->Flip(nullptr, DDFLIP_EVEN) == DD_OK, "overlay: DDFLIP_EVEN");
        chain->Release();
    }
    // System memory: no overlays (ddraw.dll's answer).
    IDirectDrawSurface7* sys = nullptr;
    d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    d.dwWidth = d.dwHeight = 16;
    d.ddsCaps.dwCaps = DDSCAPS_OVERLAY | DDSCAPS_SYSTEMMEMORY;
    CHECK(dd->CreateSurface(&d, &sys, nullptr) == DDERR_NOOVERLAYHW, "overlay: none in system memory");

    // The primary released first: the visible overlay still holds it, as in ddraw.dll.
    prim->Release();
    b->Release();
    a->Release();
    plain->Release();
    dd->RestoreDisplayMode();
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    dd->Release();
    DestroyWindow(hwnd);
    return 0;
}

static HRESULT WINAPI CountSurfaces(IDirectDrawSurface* s, DDSURFACEDESC*, void* n) {
    ++*static_cast<int*>(n);
    if (s) s->Release();
    return DDENUMRET_OK;
}

static HRESULT WINAPI CountDevices(LPSTR, LPSTR, LPD3DDEVICEDESC7, void* n) {
    ++*static_cast<int*>(n);
    return D3DENUMRET_OK;
}

// What fmtprobe's second part measured on Windows XP SP3 (dd2d9 1.8.2): the pixels a stretched
// Blt takes, and the answers to calls dd2d9 answered otherwise.
static int RunMeasured() {
    IDirectDraw7* dd = nullptr;
    if (FAILED(DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr))) return 1;
    HWND hwnd = MakeWindow(WS_POPUP, 640, 480);
    dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
    HRESULT hr;

    // Stretches: XP's rows (source pixels numbered from 1), across and down.
    struct Stretch {
        int from, to;
        const char* across;
        const char* down;
    };
    static const Stretch kStretch[] = {
        {4, 7, "1 1 2 2 3 4 4", "1 1 2 3 3 4 4"},
        {4, 8, "1 1 2 2 3 3 4 4", "1 1 2 2 3 3 4 4"},
        {5, 3, "1 3 4", "1 3 4"},
        {3, 8, "1 1 1 2 2 3 3 3", "1 1 1 2 2 3 3 3"},
        {8, 3, "2 5 7", "2 5 7"},
        {7, 4, "1 3 5 6", "1 3 5 6"},
        {2, 5, "1 1 1 2 2", "1 1 2 2 2"},
        {16, 5, "2 5 8 12 15", "2 5 8 12 15"},
        {5, 16, "1 1 1 2 2 2 3 3 3 3 4 4 4 5 5 5", "1 1 1 2 2 2 3 3 3 3 4 4 4 5 5 5"},
        {3, 2, "1 3", "1 3"},
        {9, 4, "2 4 7 9", "2 4 7 9"},
        {1, 5, "1 1 1 1 1", "1 1 1 1 1"},
        {8, 4, "2 4 6 8", "2 4 6 8"},
        {16, 4, "3 7 11 15", "3 7 11 15"},
        {4, 1, "3", "3"},
        {16, 3, "3 8 14", "3 8 14"},
        {32, 5, "4 10 17 23 30", "4 10 17 23 30"},
        {16, 9, "1 3 5 6 8 10 12 13 15", "1 3 5 6 8 10 12 13 15"},
    };
    auto fill = [](IDirectDrawSurface7* s, int w, int h, bool numbered) {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        s->Lock(nullptr, &d, DDLOCK_WAIT, nullptr);
        for (int i = 0; i < w * h; i++)
            reinterpret_cast<DWORD*>(static_cast<BYTE*>(d.lpSurface) + (i / w) * d.lPitch)[i % w] = numbered ? DWORD(i + 1) : 0;
        s->Unlock(nullptr);
    };
    auto row = [](IDirectDrawSurface7* s, int w, int h) {
        std::string out;
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        s->Lock(nullptr, &d, DDLOCK_WAIT | DDLOCK_READONLY, nullptr);
        for (int i = 0; i < w * h; i++) {
            const DWORD v = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(d.lpSurface) + (i / w) * d.lPitch)[i % w] & 0xFFFFFF;
            out += (i ? " " : "") + (v ? std::to_string(v) : std::string("."));
        }
        s->Unlock(nullptr);
        return out;
    };
    for (const Stretch& c : kStretch) {
        IDirectDrawSurface7* src = Make32(dd, c.from, 1);
        IDirectDrawSurface7* dst = Make32(dd, c.to, 1);
        IDirectDrawSurface7* srcV = Make32(dd, 1, c.from);
        IDirectDrawSurface7* dstV = Make32(dd, 1, c.to);
        if (src && dst && srcV && dstV) {
            fill(src, c.from, 1, true);
            fill(srcV, 1, c.from, true);
            dst->Blt(nullptr, src, nullptr, DDBLT_WAIT, nullptr);
            dstV->Blt(nullptr, srcV, nullptr, DDBLT_WAIT, nullptr);
            const std::string a = row(dst, c.to, 1), d = row(dstV, 1, c.to);
            CHECK(a == c.across && d == c.down, "Blt %d to %d takes source pixels %s across (XP: %s) and %s down (XP: %s)", c.from, c.to,
                  a.c_str(), c.across, d.c_str(), c.down);
        }
        if (src) src->Release();
        if (dst) dst->Release();
        if (srcV) srcV->Release();
        if (dstV) dstV->Release();
    }
    {
        IDirectDrawSurface7* src = Make32(dd, 8, 1);
        IDirectDrawSurface7* dst = Make32(dd, 12, 1);
        IDirectDrawSurface7* wide = Make32(dd, 16, 1);
        IDirectDrawClipper* clip = nullptr;
        dd->CreateClipper(0, &clip, nullptr);
        if (src && dst && wide && clip) {
            fill(src, 8, 1, true);
            fill(dst, 12, 1, false);
            RECT sr = {2, 0, 5, 1}, dr = {1, 0, 8, 1};
            dst->Blt(&dr, src, &sr, DDBLT_WAIT, nullptr);
            std::string got = row(dst, 12, 1);
            CHECK(got == ". 3 3 4 4 4 5 5 . . . .", "Blt of source pixels 3-5 to destination pixels 2-8: %s", got.c_str());
            fill(dst, 12, 1, false);
            RECT sr2 = {1, 0, 8, 1}, dr2 = {4, 0, 7, 1};
            dst->Blt(&dr2, src, &sr2, DDBLT_WAIT, nullptr);
            got = row(dst, 12, 1);
            CHECK(got == ". . . . 3 5 8 . . . . .", "Blt of source pixels 2-8 to destination pixels 5-7: %s", got.c_str());
            // Through a clip list each rectangle is a stretch of its own part of the source.
            struct {
                RGNDATAHEADER h;
                RECT r[2];
            } rgn = {};
            rgn.h.dwSize = sizeof(RGNDATAHEADER);
            rgn.h.iType = RDH_RECTANGLES;
            rgn.h.nCount = 2;
            rgn.h.nRgnSize = sizeof(rgn.r);
            rgn.h.rcBound = {3, 0, 14, 1};
            rgn.r[0] = {3, 0, 9, 1};
            rgn.r[1] = {11, 0, 14, 1};
            clip->SetClipList(reinterpret_cast<RGNDATA*>(&rgn), 0);
            wide->SetClipper(clip);
            fill(wide, 16, 1, false);
            RECT five = {0, 0, 5, 1};
            wide->Blt(nullptr, src, &five, DDBLT_WAIT, nullptr);
            got = row(wide, 16, 1);
            CHECK(got == ". . . 2 2 2 3 3 3 . . 4 4 4 . .", "Blt 5 to 16 clipped to pixels 4-9 and 12-14: %s", got.c_str());
            fill(wide, 16, 1, false);
            wide->Blt(nullptr, src, nullptr, DDBLT_WAIT, nullptr);
            got = row(wide, 16, 1);
            CHECK(got == ". . . 3 3 4 4 5 5 . . 7 7 7 . .", "Blt 8 to 16 clipped the same: %s", got.c_str());
            wide->SetClipper(nullptr);
            // A shrink through a clip list: 16 to 8, and 12 to 8 from the wide surface's first 12.
            IDirectDrawSurface7* small = Make32(dd, 8, 1);
            if (small) {
                rgn.h.rcBound = {1, 0, 8, 1};
                rgn.r[0] = {1, 0, 3, 1};
                rgn.r[1] = {5, 0, 8, 1};
                clip->SetClipList(reinterpret_cast<RGNDATA*>(&rgn), 0);
                small->SetClipper(clip);
                fill(wide, 16, 1, true);
                fill(small, 8, 1, false);
                small->Blt(nullptr, wide, nullptr, DDBLT_WAIT, nullptr);
                got = row(small, 8, 1);
                CHECK(got == ". 4 6 . . 12 14 16", "Blt 16 to 8 clipped to pixels 2-3 and 6-8: %s", got.c_str());
                fill(small, 8, 1, false);
                RECT twelve = {0, 0, 12, 1};
                small->Blt(nullptr, wide, &twelve, DDBLT_WAIT, nullptr);
                got = row(small, 8, 1);
                CHECK(got == ". 3 5 . . 9 10 12", "Blt 12 to 8 clipped the same: %s", got.c_str());
                small->SetClipper(nullptr);
                small->Release();
            }
            // Onto itself: the source is read as it was, and sampled at the destination pixels' near edges.
            fill(src, 8, 1, true);
            RECT inner = {2, 0, 6, 1};
            src->Blt(&inner, src, nullptr, DDBLT_WAIT, nullptr);
            got = row(src, 8, 1);
            CHECK(got == "1 2 1 3 5 7 7 8", "Blt of a surface's 8 pixels shrunk onto its pixels 3-6: %s", got.c_str());
        }
        if (clip) clip->Release();
        if (wide) wide->Release();
        if (dst) dst->Release();
        if (src) src->Release();
    }

    {  // Flags the blitter isn't meant to get.
        IDirectDrawSurface7* src = Make32(dd, 4, 1);
        IDirectDrawSurface7* dst = Make32(dd, 4, 1);
        if (src && dst) {
            fill(src, 4, 1, true);
            fill(dst, 4, 1, false);
            hr = dst->BltFast(0, 0, src, nullptr, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_DESTCOLORKEY);
            const std::string got = row(dst, 4, 1);
            CHECK(hr == DD_OK && got == "1 2 3 4", "BltFast with both colour-key flags and no keys copies (0x%08lx: %s)", hr, got.c_str());
            DDBLTFX fx = {};
            fx.dwSize = sizeof(fx);
            fx.dwDDFX = DDBLTFX_MIRRORLEFTRIGHT;
            hr = dst->Blt(nullptr, nullptr, nullptr, DDBLT_WAIT | DDBLT_DDFX, &fx);
            CHECK(hr == DDERR_EXCEPTION, "Blt with DDBLT_DDFX and no source: 0x%08lx (DDERR_EXCEPTION)", hr);
        }
        if (src) src->Release();
        if (dst) dst->Release();
    }

    {  // Managed textures.
        auto texture = [&](DWORD levels, DWORD caps2, DWORD memory) {
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | (levels > 1 ? DDSD_MIPMAPCOUNT : 0);
            d.ddsCaps.dwCaps = DDSCAPS_TEXTURE | memory | (levels > 1 ? DDSCAPS_MIPMAP | DDSCAPS_COMPLEX : 0);
            d.ddsCaps.dwCaps2 = caps2;
            d.dwWidth = d.dwHeight = 32;
            d.dwMipMapCount = levels > 1 ? levels : 0;
            IDirectDrawSurface7* t = nullptr;
            dd->CreateSurface(&d, &t, nullptr);
            return t;
        };
        IDirectDrawSurface7* one = texture(1, DDSCAPS2_TEXTUREMANAGE, 0);
        IDirectDrawSurface7* three = texture(3, DDSCAPS2_TEXTUREMANAGE, 0);
        IDirectDrawSurface7* video = texture(1, 0, DDSCAPS_VIDEOMEMORY);
        if (one && three && video) {
            DWORD lod = 99;
            const HRESULT l0 = one->SetLOD(0), l1 = one->SetLOD(1);
            const HRESULT gh = one->GetLOD(&lod);
            CHECK(l0 == DDERR_INVALIDPARAMS && l1 == DDERR_INVALIDPARAMS && gh == DD_OK && lod == 0,
                  "SetLOD on a managed texture that isn't a mipmap: 0 0x%08lx, 1 0x%08lx (DDERR_INVALIDPARAMS); GetLOD 0x%08lx, %lu", l0,
                  l1, gh, lod);
            const HRESULT h2 = three->SetLOD(2), h3 = three->SetLOD(3);
            three->GetLOD(&lod);
            CHECK(h2 == DD_OK && h3 == DDERR_INVALIDPARAMS && lod == 2,
                  "SetLOD on a managed texture of 3 levels: 2 0x%08lx, 3 0x%08lx (DDERR_INVALIDPARAMS); GetLOD %lu", h2, h3, lod);
            RECT r = {0, 0, 8, 8};
            const HRESULT om = one->AddOverlayDirtyRect(&r), ov = video->AddOverlayDirtyRect(&r);
            CHECK(om == DDERR_UNSUPPORTED && ov == DD_OK,
                  "AddOverlayDirtyRect: a managed texture 0x%08lx (DDERR_UNSUPPORTED, as any system-memory surface), "
                  "a video-memory one 0x%08lx",
                  om, ov);
            // Never lost: not by exclusive mode, not by a display mode.
            dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
            dd->SetDisplayMode(640, 480, 16, 0, 0);
            const HRESULT lm = one->IsLost(), lv = video->IsLost();
            dd->RestoreDisplayMode();
            dd->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
            CHECK(lm == DD_OK && lv == DDERR_SURFACELOST,
                  "after exclusive mode and a display mode: a managed texture's IsLost 0x%08lx (not lost), a video-memory one's 0x%08lx", lm,
                  lv);
        } else {
            CHECK(false, "managed textures created");
        }
        if (one) one->Release();
        if (three) three->Release();
        if (video) video->Release();
    }

    {  // Creation.
        PALETTEENTRY entries[256] = {};
        IDirectDrawPalette* pal = nullptr;
        hr = dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_PRIMARYSURFACE, entries, &pal, nullptr);
        CHECK(hr == DDERR_INVALIDPARAMS && !pal, "CreatePalette with DDPCAPS_PRIMARYSURFACE: 0x%08lx (DDERR_INVALIDPARAMS)", hr);
        if (pal) pal->Release();
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        d.dwWidth = 70000;
        d.dwHeight = 1;
        IDirectDrawSurface7* s = nullptr;
        hr = dd->CreateSurface(&d, &s, nullptr);
        CHECK(hr == DDERR_INVALIDPARAMS, "CreateSurface 70000 pixels wide: 0x%08lx (DDERR_INVALIDPARAMS: sizes are 16 bits)", hr);
        if (s) s->Release();
        // Up to 65535 a side (measured on XP; in video memory the driver decides: VMware's
        // made 32768x1).
        const struct {
            DWORD w, h, caps;
            HRESULT want;
        } sized[] = {
            {65535, 1, DDSCAPS_SYSTEMMEMORY, DD_OK},
            {65536, 1, DDSCAPS_SYSTEMMEMORY, DDERR_INVALIDPARAMS},
            {1, 65535, DDSCAPS_SYSTEMMEMORY, DD_OK},
            {1, 65536, DDSCAPS_SYSTEMMEMORY, DDERR_INVALIDPARAMS},
            {32768, 1, DDSCAPS_SYSTEMMEMORY, DD_OK},
            {32768, 1, DDSCAPS_VIDEOMEMORY, DD_OK},
        };
        for (const auto& z : sized) {
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | z.caps;
            d.dwWidth = z.w;
            d.dwHeight = z.h;
            s = nullptr;
            hr = dd->CreateSurface(&d, &s, nullptr);
            DDSURFACEDESC2 got = {};
            got.dwSize = sizeof(got);
            if (s) s->GetSurfaceDesc(&got);
            CHECK(hr == z.want && (FAILED(hr) || (got.dwWidth == z.w && got.dwHeight == z.h)),
                  "CreateSurface %lux%lu in %s memory: 0x%08lx, described as %lux%lu (0x%08lx)", z.w, z.h,
                  z.caps == DDSCAPS_SYSTEMMEMORY ? "system" : "video", hr, got.dwWidth, got.dwHeight, z.want);
            if (s) s->Release();
        }
        HRESULT z[2];
        for (int i = 0; i < 2; i++) {
            d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            d.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_SYSTEMMEMORY;
            d.dwWidth = d.dwHeight = 64;
            d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            d.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
            d.ddpfPixelFormat.dwZBufferBitDepth = i ? 32 : 24;
            d.ddpfPixelFormat.dwZBitMask = i ? 0xFFFFFF00 : 0xFFFFFF;
            s = nullptr;
            z[i] = dd->CreateSurface(&d, &s, nullptr);
            if (s) s->Release();
        }
        CHECK(z[0] == DDERR_INVALIDPIXELFORMAT && z[1] == DD_OK,
              "a system-memory Z buffer: 24 bits in 24 0x%08lx (DDERR_INVALIDPIXELFORMAT), 24 bits in 32 0x%08lx", z[0], z[1]);
        // 15 bits with 1 of stencil: the emulation layer's, in system memory (in video memory
        // the driver decides: VMware's, which doesn't list it, refused it).
        for (int i = 0; i < 2; i++) {
            d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            d.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | (i ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
            d.dwWidth = d.dwHeight = 64;
            d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            d.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER | DDPF_STENCILBUFFER;
            d.ddpfPixelFormat.dwZBufferBitDepth = 16;
            d.ddpfPixelFormat.dwStencilBitDepth = 1;
            d.ddpfPixelFormat.dwZBitMask = 0xFFFE;
            d.ddpfPixelFormat.dwStencilBitMask = 1;
            s = nullptr;
            hr = dd->CreateSurface(&d, &s, nullptr);
            DDSURFACEDESC2 got = {};
            got.dwSize = sizeof(got);
            if (s) s->GetSurfaceDesc(&got);
            const DDPIXELFORMAT& pf = got.ddpfPixelFormat;
            if (i)
                CHECK(hr == DDERR_INVALIDPIXELFORMAT, "a Z buffer of 15 bits and 1 of stencil in video memory: 0x%08lx (0x88760091)", hr);
            else
                CHECK(hr == DD_OK && got.lPitch == 128 && pf.dwFlags == (DDPF_ZBUFFER | DDPF_STENCILBUFFER) &&
                          pf.dwZBufferBitDepth == 16 && pf.dwZBitMask == 0xFFFE && pf.dwStencilBitMask == 1,
                      "a Z buffer of 15 bits and 1 of stencil in system memory: 0x%08lx, pitch %ld, flags 0x%lx, %lu bits, "
                      "Z mask 0x%lx, stencil mask 0x%lx (made, 128, 0x4400, 16, 0xfffe, 0x1)",
                      hr, got.lPitch, pf.dwFlags, pf.dwZBufferBitDepth, pf.dwZBitMask, pf.dwStencilBitMask);
            if (s) s->Release();
        }
    }

    {  // TestCooperativeLevel out of exclusive mode: the mode the object set itself is the one it expects.
        IDirectDraw7* d2 = nullptr;
        DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&d2), IID_IDirectDraw7, nullptr);
        if (d2) {
            d2->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
            const HRESULT t0 = d2->TestCooperativeLevel();
            d2->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
            d2->SetDisplayMode(640, 480, 8, 0, 0);
            d2->RestoreDisplayMode();
            const HRESULT t1 = d2->TestCooperativeLevel();
            d2->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
            const HRESULT t2 = d2->TestCooperativeLevel();
            CHECK(t0 == DD_OK && t1 == DD_OK && t2 == DDERR_WRONGMODE,
                  "TestCooperativeLevel: new 0x%08lx; exclusive, after SetDisplayMode and RestoreDisplayMode 0x%08lx; back in normal mode "
                  "0x%08lx (DDERR_WRONGMODE: the mode it set is not the display's)",
                  t0, t1, t2);
            d2->Release();
        }
    }

    {  // A window whose style exclusive mode didn't change is told of no style change on leaving it.
        IDirectDraw7* d2 = nullptr;
        DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&d2), IID_IDirectDraw7, nullptr);
        if (d2) {
            d2->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
            Pump();
            const LONG before = g_styleChanges;
            d2->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
            Pump();
            CHECK(g_styleChanges == before, "leaving exclusive mode sends a WS_POPUP window %ld WM_STYLECHANGED (none)",
                  g_styleChanges - before);
            d2->Release();
        }
    }

    {  // DirectDraw 1: EnumSurfaces lists a surface once for each interface version held; devices not through IDirectDrawSurface4.
        IDirectDraw* dd1 = nullptr;
        DirectDrawCreate(nullptr, &dd1, nullptr);
        if (dd1) {
            dd1->SetCooperativeLevel(hwnd, DDSCL_NORMAL);
            DDSURFACEDESC d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY;
            d.dwWidth = d.dwHeight = 64;
            IDirectDrawSurface* s = nullptr;
            dd1->CreateSurface(&d, &s, nullptr);
            if (s) {
                auto count = [&] {
                    int n = 0;
                    dd1->EnumSurfaces(DDENUMSURFACES_DOESEXIST | DDENUMSURFACES_ALL, nullptr, &n, CountSurfaces);
                    return n;
                };
                IDirectDrawSurface2* s2 = nullptr;
                IDirectDrawSurface3* s3 = nullptr;
                IDirectDrawSurface4* s4 = nullptr;
                const int n1 = count();
                s->QueryInterface(IID_IDirectDrawSurface2, reinterpret_cast<void**>(&s2));
                s->QueryInterface(IID_IDirectDrawSurface3, reinterpret_cast<void**>(&s3));
                const int n3 = count();
                if (s3) s3->Release();
                if (s2) s2->Release();
                const int n1again = count();
                CHECK(n1 == 1 && n3 == 3 && n1again == 1,
                      "EnumSurfaces lists a surface %d time(s); held as IDirectDrawSurface2 and 3 too: %d; those released: %d (1, 3, 1)", n1,
                      n3, n1again);
                s->QueryInterface(IID_IDirectDrawSurface4, reinterpret_cast<void**>(&s4));
                void* dev = nullptr;
                const HRESULT h4 = s4 ? s4->QueryInterface(IID_IDirect3DRGBDevice, &dev) : E_FAIL;
                IUnknown* dev1 = nullptr;
                const HRESULT h1 = s->QueryInterface(IID_IDirect3DRGBDevice, reinterpret_cast<void**>(&dev1));
                CHECK(h4 == DDERR_INVALIDPARAMS && !dev && h1 == DD_OK,
                      "QueryInterface for the software device: through IDirectDrawSurface4 0x%08lx (DDERR_INVALIDPARAMS), through "
                      "IDirectDrawSurface 0x%08lx",
                      h4, h1);
                if (dev1) dev1->Release();
                if (s4) s4->Release();
                s->Release();
            }
            // Direct3D 3's Begin/Vertex/End: Vertex fails until the device's first Begin (its
            // batch is made there), then is taken outside one too.
            IDirectDraw4* dd4 = nullptr;
            IDirect3D3* d3d3 = nullptr;
            IDirectDrawSurface4* rt = nullptr;
            IDirect3DDevice3* dev3 = nullptr;
            IDirect3DViewport3* vp = nullptr;
            dd1->QueryInterface(IID_IDirectDraw4, reinterpret_cast<void**>(&dd4));
            dd1->QueryInterface(IID_IDirect3D3, reinterpret_cast<void**>(&d3d3));
            DDSURFACEDESC2 rd = {};
            rd.dwSize = sizeof(rd);
            rd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            rd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_SYSTEMMEMORY;
            rd.dwWidth = rd.dwHeight = 64;
            if (dd4) dd4->CreateSurface(&rd, &rt, nullptr);
            if (d3d3 && rt) d3d3->CreateDevice(IID_IDirect3DRGBDevice, rt, &dev3, nullptr);
            if (dev3 && SUCCEEDED(d3d3->CreateViewport(&vp, nullptr))) {
                dev3->AddViewport(vp);
                D3DVIEWPORT2 vd = {};
                vd.dwSize = sizeof(vd);
                vd.dwWidth = vd.dwHeight = 64;
                vd.dvClipX = -1.0f, vd.dvClipY = 1.0f, vd.dvClipWidth = vd.dvClipHeight = 2.0f;
                vd.dvMaxZ = 1.0f;
                vp->SetViewport2(&vd);
                dev3->SetCurrentViewport(vp);
                D3DTLVERTEX v[3] = {};
                const HRESULT v0 = dev3->Vertex(&v[0]), i0 = dev3->Index(0);
                dev3->BeginScene();
                dev3->Begin(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, 0);
                for (D3DTLVERTEX& x : v) dev3->Vertex(&x);
                dev3->End(0);
                const HRESULT v1 = dev3->Vertex(&v[0]), i1 = dev3->Index(0);
                dev3->Begin(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, 0);
                const HRESULT i2 = dev3->Index(0);
                dev3->End(0);
                const HRESULT f0 = dev3->DrawPrimitive(D3DPT_TRIANGLELIST, 0, v, 3, 0);
                dev3->EndScene();
                CHECK(v0 == DDERR_INVALIDPARAMS && i0 == DDERR_INVALIDPARAMS && v1 == D3D_OK && i1 == D3D_OK,
                      "IDirect3DDevice3 before its first Begin: Vertex 0x%08lx, Index 0x%08lx (DDERR_INVALIDPARAMS); after a Begin and its "
                      "End: Vertex 0x%08lx, Index 0x%08lx",
                      v0, i0, v1, i1);
                CHECK(i2 == DDERR_INVALIDPARAMS, "IDirect3DDevice3::Index inside a Begin that isn't indexed: 0x%08lx (DDERR_INVALIDPARAMS)", i2);
                D3DVERTEXBUFFERDESC vbd = {};
                vbd.dwSize = sizeof(vbd);
                vbd.dwCaps = D3DVBCAPS_SYSTEMMEMORY;
                vbd.dwFVF = D3DFVF_XYZ;
                IDirect3DVertexBuffer* vb = nullptr;
                const HRESULT vh = d3d3->CreateVertexBuffer(&vbd, &vb, 0, nullptr);
                CHECK(vh == DDERR_INVALIDPARAMS && !vb, "IDirect3D3::CreateVertexBuffer with no vertices: 0x%08lx (DDERR_INVALIDPARAMS)", vh);
                if (vb) vb->Release();
                CHECK(f0 == D3D_OK, "IDirect3DDevice3::DrawPrimitive with a vertex format of no position: 0x%08lx (D3D_OK)", f0);
            } else {
                CHECK(false, "a Direct3D 3 software device and viewport made");
            }
            if (vp) vp->Release();
            if (dev3) dev3->Release();
            if (rt) rt->Release();
            if (d3d3) d3d3->Release();
            if (dd4) dd4->Release();
            dd1->Release();
        }
    }

    {  // Direct3D 7.
        IDirect3D7* d3d = nullptr;
        dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d));
        for (int hard = 0; hard < 2 && d3d; hard++) {
            DDSURFACEDESC2 rd = {};
            rd.dwSize = sizeof(rd);
            rd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            rd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | (hard ? DDSCAPS_VIDEOMEMORY : DDSCAPS_SYSTEMMEMORY);
            rd.dwWidth = rd.dwHeight = 64;
            IDirectDrawSurface7* rt = nullptr;
            IDirect3DDevice7* dev = nullptr;
            dd->CreateSurface(&rd, &rt, nullptr);
            if (rt) d3d->CreateDevice(hard ? IID_IDirect3DHALDevice : IID_IDirect3DRGBDevice, rt, &dev);
            if (!dev) {
                CHECK(false, "a Direct3D 7 %s device made", hard ? "hardware" : "software");
                if (rt) rt->Release();
                continue;
            }
            DWORD aa = 1, edge = 1;
            dev->GetRenderState(D3DRENDERSTATE_ANTIALIAS, &aa);
            dev->GetRenderState(D3DRENDERSTATE_EDGEANTIALIAS, &edge);
            const DWORD want = hard ? 0xFFFFFFFF : 0;
            CHECK(aa == want && edge == want, "the %s device's ANTIALIAS and EDGEANTIALIAS states at first: 0x%lx, 0x%lx (0x%lx)",
                  hard ? "hardware" : "software", aa, edge, want);
            if (!hard) {
                BYTE info[16];
                memset(info, 0xEE, sizeof(info));
                const HRESULT g2 = dev->GetInfo(2, info, sizeof(info)), g9 = dev->GetInfo(9, info, sizeof(info));
                // (The generic failure code, not by its name: a line with that in it would count as a failed check.)
                CHECK(g2 == S_FALSE && g9 == HRESULT(0x80004005) && !info[0],
                      "GetInfo: id 2 0x%08lx (S_FALSE), id 9 0x%08lx (0x80004005), the buffer zeroed %d", g2, g9, !info[0]);
                D3DTLVERTEX tri[3] = {};
                dev->BeginScene();
                const HRESULT p0 = dev->DrawPrimitive(D3DPRIMITIVETYPE(0), D3DFVF_TLVERTEX, tri, 3, 0);
                const HRESULT p7 = dev->DrawPrimitive(D3DPRIMITIVETYPE(7), D3DFVF_TLVERTEX, tri, 3, 0);
                dev->EndScene();
                CHECK(p0 == D3D_OK && p7 == D3D_OK, "DrawPrimitive with primitive types 0 and 7: 0x%08lx, 0x%08lx (D3D_OK)", p0, p7);
                D3DVERTEXBUFFERDESC vd = {};
                vd.dwSize = sizeof(vd);
                vd.dwCaps = D3DVBCAPS_SYSTEMMEMORY;
                vd.dwFVF = D3DFVF_XYZ;
                IDirect3DVertexBuffer7* vb = nullptr;
                hr = d3d->CreateVertexBuffer(&vd, &vb, 0);
                CHECK(hr == DDERR_INVALIDPARAMS && !vb, "CreateVertexBuffer with no vertices: 0x%08lx (DDERR_INVALIDPARAMS)", hr);
                if (vb) vb->Release();
            }
            dev->Release();
            rt->Release();
        }
        if (d3d) d3d->Release();
    }

    {  // The emulation layer's caps are ddraw.dll's own, and an emulation-only object has nothing else.
        DDCAPS drv = {}, hel = {};
        drv.dwSize = hel.dwSize = sizeof(DDCAPS);
        dd->GetCaps(&drv, &hel);
        CHECK(hel.dwCaps == 0xF4C08241 && hel.dwCaps2 == DDCAPS2_CERTIFIED && hel.dwCKeyCaps == DDCKEYCAPS_SRCBLT &&
                  hel.dwFXCaps == 0x3FCE3 && hel.dwPalCaps == 0x347 && hel.dwZBufferBitDepths == DDBD_16 &&
                  hel.ddsCaps.dwCaps == 0x421350 && hel.ddsCaps.dwCaps2 == DDSCAPS2_CUBEMAP && hel.dwSSBCaps == hel.dwCaps,
              "the emulation's caps are XP's: caps 0x%lx, caps2 0x%lx, colour keys 0x%lx, effects 0x%lx, palettes 0x%lx, surfaces 0x%lx",
              hel.dwCaps, hel.dwCaps2, hel.dwCKeyCaps, hel.dwFXCaps, hel.dwPalCaps, hel.ddsCaps.dwCaps);
        IDirectDraw7* emu = nullptr;
        DirectDrawCreateEx(reinterpret_cast<GUID*>(DDCREATE_EMULATIONONLY), reinterpret_cast<void**>(&emu), IID_IDirectDraw7, nullptr);
        if (emu) {
            emu->SetCooperativeLevel(nullptr, DDSCL_NORMAL);
            drv = {};
            drv.dwSize = sizeof(DDCAPS);
            emu->GetCaps(&drv, nullptr);
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
            d.dwWidth = d.dwHeight = 16;
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
            IDirectDrawSurface7* v = nullptr;
            const HRESULT vh = emu->CreateSurface(&d, &v, nullptr);
            if (v) v->Release();
            d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
            IDirectDrawSurface7* s = nullptr;
            const HRESULT sh = emu->CreateSurface(&d, &s, nullptr);
            if (s) s->GetSurfaceDesc(&d);
            if (s) s->Release();
            int devices = 0;
            IDirect3D7* d3d = nullptr;
            emu->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d));
            if (d3d) {
                d3d->EnumDevices(CountDevices, &devices);
                d3d->Release();
            }
            CHECK(drv.dwCaps == DDCAPS_NOHARDWARE && vh == DDERR_NODIRECTDRAWHW && sh == DD_OK &&
                      (d.ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) && devices == 1,
                  "an emulation-only object: driver caps 0x%lx (DDCAPS_NOHARDWARE), a video-memory surface 0x%08lx "
                  "(DDERR_NODIRECTDRAWHW), one with no memory type 0x%08lx with caps 0x%lx, %d Direct3D 7 device(s) (the software one)",
                  drv.dwCaps, vh, sh, d.ddsCaps.dwCaps, devices);
            emu->Release();
        } else {
            CHECK(false, "an emulation-only object made");
        }
    }
    DestroyWindow(hwnd);
    dd->Release();
    return 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmd, int) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "ddtest.txt");
    g_out = fopen(path, "a");
    fprintf(g_out, "== ddtest %s\n", cmd);
    int r = 0;
    if (strstr(cmd, "desktop8")) r = RunDesktop8();
    else if (strstr(cmd, "pattern")) {
        int w = 640, h = 480;
        sscanf(strstr(cmd, "pattern") + 7, "%d %d", &w, &h);
        r = RunPattern(w, h);
    }
    else if (strstr(cmd, "fs8")) {
        // "fs8" runs at 640x480; "fs8 <w> <h>" uses another mode (a test display may only offer its own size)
        int w = 640, h = 480;
        sscanf(strstr(cmd, "fs8") + 3, "%d %d", &w, &h);
        r = RunFullscreen8(w, h);
    }
    else if (strstr(cmd, "mouse")) r = RunMouse() | RunNoWindowChanges();
    else if (strstr(cmd, "audit")) r = RunAudit(strstr(cmd, "borderless") != nullptr);
    else if (strstr(cmd, "overlay")) r = RunOverlay() | RunOverlayWindowed() | RunDeviceWindow();
    else if (strstr(cmd, "win")) r = RunWindowed();
    else if (strstr(cmd, "fuzz")) r = RunFuzz();
    else r = RunUnit() | RunExecute() | RunTargetGone() | RunFormats() | RunLifetime() | RunCoopModes() | RunMeasured();
    fprintf(g_out, "== %d failure(s)\n", g_fail);
    fclose(g_out);
    return r | (g_fail ? 2 : 0);
}
