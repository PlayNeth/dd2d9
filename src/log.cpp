// Logging: the log file, the crash logger, names of interfaces.
#include "configint.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace dd2d9 {

static FILE* g_logFile = nullptr;
static CRITICAL_SECTION g_logLock;
int g_logLevel = 0;  // effective level (0 when no file could be opened)
static std::wstring g_logPath;   // log file actually opened
static PVOID g_crashHandler;     // CrashLogger registration
static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep);

static void DebugMsg(const std::wstring& msg) {
    // Shown by Wine with WINEDEBUG=warn+seh (Wine 9) or +debugstr, and by DebugView on Windows,
    // even when no log file can be written.
    OutputDebugStringW((L"dd2d9: " + msg + L"\n").c_str());
}

void LogInit() {
    InitializeCriticalSection(&g_logLock);
    std::wstring found = g_iniLoaded.empty() ? L"no dd2d9.ini found" : L"config: " + g_iniLoaded.back();
    if (!g_config.log) {
        DebugMsg(L"loaded from " + g_dllPath + L" (log off; " + found + L")");
        return;
    }
    const std::wstring exeDir = DirOf(ModulePath(nullptr));
    std::vector<std::wstring> candidates;
    if (!g_config.logFile.empty()) {
        std::wstring lf = g_config.logFile;
        bool absolute = lf.size() > 1 && (lf[1] == L':' || lf[0] == L'\\' || lf[0] == L'/');
        candidates.push_back(absolute ? lf : exeDir + lf);
    }
    candidates.push_back(exeDir + L"dd2d9.log");
    std::wstring cwd(kPathMax, L'\0');
    DWORD n = GetCurrentDirectoryW(DWORD(cwd.size()), &cwd[0]);
    if (n && n < cwd.size()) {
        cwd.resize(n);
        if (cwd.back() != L'\\') cwd += L'\\';
        candidates.push_back(cwd + L"dd2d9.log");
    }
    std::wstring tmp(MAX_PATH + 1, L'\0');
    n = GetTempPathW(DWORD(tmp.size()), &tmp[0]);
    if (n && n < tmp.size()) candidates.push_back(tmp.substr(0, n) + L"dd2d9.log");

    for (const auto& c : candidates) {
        g_logFile = _wfopen(c.c_str(), L"w");
        if (g_logFile) {
            g_logPath = c;
            break;
        }
        DebugMsg(L"cannot write log file " + c);
    }
    if (!g_logFile) {
        DebugMsg(L"loaded from " + g_dllPath + L", no writable log location");
        return;
    }
    // Buffered: trace logging writes thousands of lines a second (a syscall per
    // line is slow under Wine). Info lines are flushed immediately, trace lines
    // at most every 100 ms.
    setvbuf(g_logFile, nullptr, _IOFBF, 1 << 16);
    g_logLevel = g_config.log;
    DebugMsg(L"loaded from " + g_dllPath + L", logging to " + g_logPath);

    static const char* kDisplay[] = {"switch", "window", "borderless"};
    static const char* kStretch[] = {"aspect", "fill", "integer"};
    static const char* kVSync[] = {"auto", "on", "off"};
    Log(1, "dd2d9 " DD2D9_VERSION);
    Log(1, "process: %s", Utf8(ModulePath(nullptr)).c_str());
    Log(1, "module:  %s", Utf8(g_dllPath).c_str());
    if (g_iniLoaded.empty()) Log(1, "config:  none found");
    for (const auto& f : g_iniLoaded) Log(1, "config:  %s", Utf8(f).c_str());
    for (const auto& e : g_iniErrors) Log(1, "config:  %s", Utf8(e).c_str());
    static const char* kD3D[] = {"auto", "on", "off"};
    static const char* kFilter[] = {"point", "linear", "sharp"};
    Log(1, "settings: display=%s stretch=%s filter=%s vsync=%s d3d=%s d3d9=%s log=%d",
        kDisplay[g_config.display], kStretch[g_config.stretch], kFilter[g_config.filter], kVSync[g_config.vsync], kD3D[g_config.d3d],
        g_config.systemD3D9 ? "system" : "game", g_config.log);
    Log(1, "settings: fpslimit=%d ram=%d vram=%d cpu=%d cpus=%d desktopbpp=%d lockwhole=%s zaccess=%s mipmaps=%s anisotropy=%d alternatepixelcenter=%s render32=%s depth24=%s antialiasing=%d antialias2d=%s texfilter=%s wbuffer=%s overlaybias=%d videooverlay=%s palettizedtextures=%s heapzero=%s fairlocks=%s safetimers=%s joypoll=%s selfsuspend=%s", g_config.fpsLimit, g_config.ramMB,
        g_config.vramMB, g_config.cpuMHz, g_config.cpus, g_config.desktopBpp, g_config.lockWhole ? "on" : "off", g_config.zAccess ? "on" : "off", g_config.mipmaps ? "on" : "off", g_config.anisotropy, g_config.pixelCenter ? "on" : "off",
        g_config.render32 ? "on" : "off", g_config.depth24 ? "on" : "off", g_config.msaa, g_config.aa2d ? "on" : "off",
        g_config.texLinear ? "linear" : "game", g_config.wbuffer == 2 ? "force" : g_config.wbuffer ? "on" : "off",
        g_config.overlayBias, g_config.overlays ? "on" : "off", g_config.palTextures ? "on" : "off",
        g_config.heapZero ? "on" : "off", g_config.fairLocks ? "on" : "off", g_config.safeTimers ? "on" : "off", g_config.joyPoll ? "on" : "off", g_config.selfSuspend ? "on" : "off");
    if (LoadedBehindWrapper()) Log(1, "loaded behind a wrapper (as %s)", Utf8(BaseName(g_dllPath)).c_str());
    g_crashHandler = AddVectoredExceptionHandler(0, CrashLogger);  // 0: after the handlers registered first
}

void CrashLoggerRemove() {
    if (g_crashHandler) RemoveVectoredExceptionHandler(g_crashHandler);
    g_crashHandler = nullptr;
}

// The module whose image (past its headers) contains `addr`, or null.
static HMODULE ModuleAt(const void* addr) {
    if (reinterpret_cast<ULONG_PTR>(addr) < 0x10000) return nullptr;  // null and small values: no module
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(addr), &m) || !m)
        return nullptr;
    const auto* base = reinterpret_cast<const uint8_t*>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const ULONG_PTR off = static_cast<const uint8_t*>(addr) - base;
    if (static_cast<const uint8_t*>(addr) < base || off < nt->OptionalHeader.SizeOfHeaders ||
        off >= nt->OptionalHeader.SizeOfImage)
        return nullptr;
    return m;
}

// "module.dll+0x1234" for a code address, or the bare address outside any module.
static void DescribeAddress(const void* addr, char* out, size_t size) {
    // No allocation (it runs in the crash handler, perhaps with the heap damaged).
    if (HMODULE m = ModuleAt(addr)) {
        char path[MAX_PATH];
        const DWORD n = GetModuleFileNameA(m, path, MAX_PATH);
        path[n < MAX_PATH ? n : MAX_PATH - 1] = 0;
        const char* name = strrchr(path, '\\');
        snprintf(out, size, "%s+0x%lx", name ? name + 1 : path,
                 static_cast<unsigned long>(static_cast<const char*>(addr) - reinterpret_cast<const char*>(m)));
    } else {
        snprintf(out, size, "%p", addr);
    }
}

// Logs crashes: which module faulted, and the modules found on the stack (a
// rough call chain). Exceptions are seen before any handler runs, so one that
// the program then handles is logged too; the number logged is limited.
static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep) {
    static LONG logged;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const char* what = nullptr;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: what = "access violation"; break;
    case EXCEPTION_IN_PAGE_ERROR: what = "in-page error"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: what = "illegal instruction"; break;
    case EXCEPTION_PRIV_INSTRUCTION: what = "privileged instruction"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: what = "integer division by zero"; break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: what = "array bounds exceeded"; break;
    // Visual C++'s throw: a game's own error handling (Unreal engine games throw one for a
    // "critical error" and then show their error box), usually caught. Counted apart, so
    // a game that throws them routinely can't use up the crashes' log lines.
    case 0xE06D7363: what = "C++ exception"; break;
    default: return EXCEPTION_CONTINUE_SEARCH;  // not a crash (or, for stack overflows, no stack left to log with)
    }
    if (!g_logFile) return EXCEPTION_CONTINUE_SEARCH;
    static LONG loggedThrows;
    if (InterlockedIncrement(code == 0xE06D7363 ? &loggedThrows : &logged) > 5) return EXCEPTION_CONTINUE_SEARCH;
    char at[300];
    DescribeAddress(ep->ExceptionRecord->ExceptionAddress, at, sizeof(at));
    char detail[64] = "";
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR kind = ep->ExceptionRecord->ExceptionInformation[0];
        snprintf(detail, sizeof(detail), " %s 0x%08lx", kind == 0 ? "reading" : kind == 8 ? "executing" : "writing",
                 static_cast<unsigned long>(ep->ExceptionRecord->ExceptionInformation[1]));
    }
    Log(1, "exception 0x%08lx (%s%s) at %s - if the game crashed now, this is where", code, what, detail, at);
#if defined(_M_IX86) || defined(__i386__)
    // Return addresses: stack values that point into a loaded module. Only the
    // committed memory region holding the stack pointer is read.
    const auto* sp = reinterpret_cast<const ULONG_PTR*>(ep->ContextRecord->Esp);
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(sp, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return EXCEPTION_CONTINUE_SEARCH;
    const auto* top = reinterpret_cast<const ULONG_PTR*>(static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize);
    char chain[12 * 80] = "";
    size_t used = 0;
    int found = 0;
    for (int i = 0; i < 512 && sp + i < top && found < 12; i++) {
        if (!ModuleAt(reinterpret_cast<const void*>(sp[i]))) continue;
        DescribeAddress(reinterpret_cast<const void*>(sp[i]), at, sizeof(at));
        const int w = snprintf(chain + used, sizeof(chain) - used, "%s%s", found++ ? ", " : "", at);
        if (w < 0 || size_t(w) >= sizeof(chain) - used) break;
        used += size_t(w);
    }
    Log(1, "  stack (modules found, innermost first): %s", found ? chain : "none");
#endif
    return EXCEPTION_CONTINUE_SEARCH;
}

void Log(int level, const char* fmt, ...) {
    if (!g_logFile) return;
    EnterCriticalSection(&g_logLock);
    if (!g_logFile) {  // (closed meanwhile: LogFlush, as dd2d9 is unloaded)
        LeaveCriticalSection(&g_logLock);
        return;
    }
    fprintf(g_logFile, "[%8lu %04lx] ", GetTickCount(), GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fputc('\n', g_logFile);
    static DWORD last;
    DWORD now = GetTickCount();
    if (level <= 1 || now - last >= 100) {
        fflush(g_logFile);
        last = now;
    }
    LeaveCriticalSection(&g_logLock);
}

// At process exit the other threads are gone, and one may have died holding the
// log lock: only flush if it is free, so exit can't hang.
void LogFlush(bool processExit) {
    if (!g_logFile) return;
    if (processExit) {
        if (!TryEnterCriticalSection(&g_logLock)) return;
    } else {
        EnterCriticalSection(&g_logLock);
    }
    fflush(g_logFile);
    if (!processExit) {  // unloaded while the process goes on: a later load opens the file again
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    LeaveCriticalSection(&g_logLock);
}

const char* GuidName(REFIID r) {
    struct Entry { const GUID* g; const char* n; };
    static const Entry kNames[] = {
        {&IID_IUnknown, "IUnknown"},
        {&IID_IDirectDraw, "IDirectDraw"}, {&IID_IDirectDraw2, "IDirectDraw2"},
        {&IID_IDirectDraw4, "IDirectDraw4"}, {&IID_IDirectDraw7, "IDirectDraw7"},
        {&IID_IDirectDrawSurface, "IDirectDrawSurface"}, {&IID_IDirectDrawSurface2, "IDirectDrawSurface2"},
        {&IID_IDirectDrawSurface3, "IDirectDrawSurface3"}, {&IID_IDirectDrawSurface4, "IDirectDrawSurface4"},
        {&IID_IDirectDrawSurface7, "IDirectDrawSurface7"}, {&IID_IDirectDrawPalette, "IDirectDrawPalette"},
        {&IID_IDirectDrawClipper, "IDirectDrawClipper"}, {&IID_IDirectDrawColorControl, "IDirectDrawColorControl"},
        {&IID_IDirectDrawGammaControl, "IDirectDrawGammaControl"},
    };
    for (const auto& e : kNames)
        if (IsEqualGUID(r, *e.g)) return e.n;
    static thread_local char buf[40];
    snprintf(buf, sizeof(buf), "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", r.Data1, r.Data2, r.Data3,
             r.Data4[0], r.Data4[1], r.Data4[2], r.Data4[3], r.Data4[4], r.Data4[5], r.Data4[6], r.Data4[7]);
    return buf;
}

}  // namespace dd2d9
