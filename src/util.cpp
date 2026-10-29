// Logging, configuration, pixel-format and surface-description helpers.
#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <string>

namespace dd2d9 {

Config g_config;
CRITICAL_SECTION g_lock;
CRITICAL_SECTION g_displayLock;

static FILE* g_logFile = nullptr;
static CRITICAL_SECTION g_logLock;
static int g_logLevel = 0;  // effective level (0 when no file could be opened)

// Paths are handled as UTF-16 throughout: the ANSI APIs mangle folder names
// with characters outside the current code page (accents, CJK, ...).
static const size_t kPathMax = 32768;
static std::wstring g_dllPath;                 // where this ddraw.dll was loaded from
static std::wstring g_logPath;                 // log file actually opened
static std::vector<std::wstring> g_iniLoaded;  // config files that were read
static std::vector<std::wstring> g_iniErrors;  // unknown keys / bad values

static std::wstring ModulePath(HMODULE m) {
    std::wstring s(kPathMax, L'\0');
    DWORD n = GetModuleFileNameW(m, &s[0], DWORD(s.size()));
    s.resize(n < s.size() ? n : 0);
    return s;
}

static std::wstring DirOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

static std::wstring BaseName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

static std::wstring Env(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(name, &v[0], n);
    v.resize(n);
    return v;
}

static std::wstring FromUtf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

static std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

static void DebugMsg(const std::wstring& msg) {
    // Shown by Wine with WINEDEBUG=warn+seh (Wine 9) or +debugstr, and by DebugView on Windows,
    // even when no log file can be written.
    OutputDebugStringW((L"dd2d9: " + msg + L"\n").c_str());
}

// ---------------------------------------------------------------- config parsing

static std::string Lower(std::string s) {
    for (char& c : s) c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

static bool ParseBool(const std::string& v, int* out) {
    if (v == "1" || v == "true" || v == "yes" || v == "on") { *out = 1; return true; }
    if (v == "0" || v == "false" || v == "no" || v == "off") { *out = 0; return true; }
    return false;
}

static void Apply(const std::string& key, const std::string& rawVal, const std::wstring& where) {
    const std::string v = Lower(rawVal);
    bool ok = true;
    if (key == "display") {
        if (v == "switch") g_config.display = kDisplaySwitch;
        else if (v == "window" || v == "windowed") g_config.display = kDisplayWindow;
        else if (v == "borderless") g_config.display = kDisplayBorderless;
        else ok = false;
    } else if (key == "stretch") {
        if (v == "aspect" || v == "0") g_config.stretch = 0;
        else if (v == "fill" || v == "1") g_config.stretch = 1;
        else if (v == "integer" || v == "2") g_config.stretch = 2;
        else ok = false;
    } else if (key == "filter") {
        if (v == "point" || v == "nearest" || v == "0") g_config.filter = 0;
        else if (v == "linear" || v == "bilinear" || v == "1") g_config.filter = 1;
        else ok = false;
    } else if (key == "present") {
        if (v == "window" || v == "windowed") g_config.present = kPresentWindow;
        else if (v == "exclusive" || v == "fullscreen") g_config.present = kPresentExclusive;
        else ok = false;
    } else if (key == "d3d") {
        int b;
        if (v == "auto") g_config.d3d = kD3DAuto;
        else if (ParseBool(v, &b)) g_config.d3d = b ? kD3DOn : kD3DOff;
        else ok = false;
    } else if (key == "vsync") {
        int b;
        if (v == "auto" || v == "game") g_config.vsync = kVSyncAuto;
        else if (ParseBool(v, &b)) g_config.vsync = b ? kVSyncOn : kVSyncOff;
        else ok = false;
    } else if (key == "log") {
        int b;
        if (v == "2" || v == "trace") g_config.log = 2;
        else if (ParseBool(v, &b)) g_config.log = b;
        else ok = false;
    } else if (key == "logfile") {
        g_config.logFile = FromUtf8(Trim(rawVal));
    } else {
        ok = false;
    }
    if (!ok) g_iniErrors.push_back(where + L": ignored " + FromUtf8(key + "=" + rawVal));
}

// Reads one ini file. Keys outside any section, in [dd2d9] and in [default]
// apply to every game; a [<game>.exe] section applies to that executable only.
static bool LoadIni(const std::wstring& path, const std::string& exeName) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.size() >= 3 && data.compare(0, 3, "\xEF\xBB\xBF") == 0) data.erase(0, 3);  // UTF-8 BOM

    bool active = true;
    size_t pos = 0, lineNo = 0;
    while (pos < data.size()) {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        std::string line = Trim(data.substr(pos, eol - pos));
        pos = eol + 1;
        lineNo++;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            std::string name = Lower(Trim(line.substr(1, line.find(']') - 1)));
            active = name == "dd2d9" || name == "default" || name == exeName;
            continue;
        }
        if (!active) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string val = Trim(line.substr(eq + 1));
        size_t comment = val.find(" ;");  // allow "key=value ; comment"
        if (comment != std::string::npos) val = Trim(val.substr(0, comment));
        Apply(Lower(Trim(line.substr(0, eq))), val, BaseName(path) + L":" + std::to_wstring(lineNo));
    }
    return true;
}

void LoadConfig(HMODULE self) {
    g_dllPath = ModulePath(self);
    const std::wstring exe = ModulePath(nullptr);
    const std::string exeName = Lower(Utf8(BaseName(exe)));

    // Lowest to highest priority.
    std::vector<std::wstring> files;
    std::wstring win(MAX_PATH + 1, L'\0');
    UINT n = GetWindowsDirectoryW(&win[0], UINT(win.size()));
    if (n && n < win.size()) files.push_back(win.substr(0, n) + L"\\dd2d9.ini");
    const std::wstring dllIni = DirOf(g_dllPath) + L"dd2d9.ini", exeIni = DirOf(exe) + L"dd2d9.ini";
    if (_wcsicmp(dllIni.c_str(), exeIni.c_str()) != 0) files.push_back(dllIni);
    files.push_back(exeIni);
    std::wstring envIni = Env(L"DD2D9_INI");
    if (!envIni.empty()) files.push_back(envIni);
    for (const auto& f : files)
        if (LoadIni(f, exeName)) g_iniLoaded.push_back(f);

    // DD2D9_LOG=0/1/2 sets the level; any other value is a log file path (and enables logging).
    std::wstring envLog = Env(L"DD2D9_LOG");
    if (!envLog.empty()) {
        if (envLog == L"0" || envLog == L"1" || envLog == L"2") {
            g_config.log = envLog[0] - L'0';
        } else {
            g_config.log = g_config.log ? g_config.log : 1;
            g_config.logFile = envLog;
        }
    }
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
    Log(1, "settings: display=%s present=%s stretch=%s filter=%s vsync=%s d3d=%s log=%d", kDisplay[g_config.display],
        g_config.present == kPresentExclusive ? "exclusive" : "window", kStretch[g_config.stretch],
        g_config.filter ? "linear" : "point", kVSync[g_config.vsync], kD3D[g_config.d3d], g_config.log);
    if (LoadedBehindWrapper()) Log(1, "loaded behind a wrapper (as %s)", Utf8(BaseName(g_dllPath)).c_str());
}

bool WaitPumping(HANDLE h, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    for (;;) {
        const DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeoutMs) return WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
        DWORD r = MsgWaitForMultipleObjects(1, &h, FALSE, timeoutMs - elapsed, QS_SENDMESSAGE);
        if (r == WAIT_OBJECT_0) return true;
        if (r != WAIT_OBJECT_0 + 1) return false;
        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);  // dispatches the sent messages
    }
}

bool LoadedBehindWrapper() {
    return _wcsicmp(BaseName(g_dllPath).c_str(), L"ddraw.dll") != 0;
}

int LogLevel() { return g_logLevel; }

void Log(int level, const char* fmt, ...) {
    if (!g_logFile) return;
    EnterCriticalSection(&g_logLock);
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

// ---------------------------------------------------------------- formats

Format Format::ForDisplayBpp(DWORD bpp) {
    Format f;
    f.bpp = bpp;
    switch (bpp) {
    case 8: f.pal8 = true; break;
    case 15: f.bpp = 16; f.r = 0x7C00; f.g = 0x03E0; f.b = 0x001F; break;
    case 16: f.r = 0xF800; f.g = 0x07E0; f.b = 0x001F; break;
    default: f.bpp = bpp == 24 ? 24 : 32; f.r = 0xFF0000; f.g = 0xFF00; f.b = 0xFF; break;
    }
    return f;
}

bool Format::FromDDPF(const DDPIXELFORMAT& pf, Format* out) {
    Format f;
    if (pf.dwFlags & DDPF_FOURCC) {
        // Only formats whose memory layout we know; GetFourCCCodes reports none,
        // so games that check first never ask for these.
        f.fourcc = true;
        f.fourCC = pf.dwFourCC;
        switch (pf.dwFourCC) {
        case MAKEFOURCC('D', 'X', 'T', '1'): case MAKEFOURCC('D', 'X', 'T', '2'):
        case MAKEFOURCC('D', 'X', 'T', '3'): case MAKEFOURCC('D', 'X', 'T', '4'):
        case MAKEFOURCC('D', 'X', 'T', '5'):
            f.bpp = 8;  // marks block compression (see BlockCompressed)
            break;
        case MAKEFOURCC('Y', 'U', 'Y', '2'): case MAKEFOURCC('U', 'Y', 'V', 'Y'):
            f.bpp = 16;  // packed 4:2:2, two bytes per pixel
            break;
        default:
            return false;
        }
    } else if (pf.dwFlags & DDPF_ZBUFFER) {
        f.zbuf = true;
        f.bpp = pf.dwZBufferBitDepth;
        if (f.bpp != 16 && f.bpp != 24 && f.bpp != 32) return false;
    } else if (pf.dwFlags & DDPF_PALETTEINDEXED8) {
        f.pal8 = true;
        f.bpp = 8;
    } else if (pf.dwFlags & (DDPF_PALETTEINDEXED1 | DDPF_PALETTEINDEXED2 | DDPF_PALETTEINDEXED4)) {
        return false;
    } else if (pf.dwFlags & DDPF_RGB) {
        f.bpp = pf.dwRGBBitCount;
        f.r = pf.dwRBitMask;
        f.g = pf.dwGBitMask;
        f.b = pf.dwBBitMask;
        if (pf.dwFlags & DDPF_ALPHAPIXELS) f.a = pf.dwRGBAlphaBitMask;
        if (f.bpp != 8 && f.bpp != 16 && f.bpp != 24 && f.bpp != 32) return false;
        if (f.bpp == 8 && !f.r) f.pal8 = true;  // some games omit PALETTEINDEXED8
    } else {
        return false;
    }
    *out = f;
    return true;
}

void Format::ToDDPF(DDPIXELFORMAT* pf) const {
    memset(pf, 0, sizeof(*pf));
    pf->dwSize = sizeof(DDPIXELFORMAT);
    if (fourcc) {
        pf->dwFlags = DDPF_FOURCC;
        pf->dwFourCC = fourCC;
    } else if (zbuf) {
        pf->dwFlags = DDPF_ZBUFFER;
        pf->dwZBufferBitDepth = bpp;
        pf->dwZBitMask = bpp >= 32 ? 0xFFFFFFFFu : ((1u << bpp) - 1);
    } else if (pal8) {
        pf->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        pf->dwRGBBitCount = 8;
    } else {
        pf->dwFlags = DDPF_RGB | (a ? DDPF_ALPHAPIXELS : 0);
        pf->dwRGBBitCount = bpp;
        pf->dwRBitMask = r;
        pf->dwGBitMask = g;
        pf->dwBBitMask = b;
        pf->dwRGBAlphaBitMask = a;
    }
}

static inline uint32_t Extract8(uint32_t v, uint32_t mask) {
    if (!mask) return 0;
    uint32_t shift = __builtin_ctz(mask);
    uint32_t max = mask >> shift;
    uint32_t c = (v & mask) >> shift;
    return max == 255 ? c : (c * 255 + max / 2) / max;
}

static inline uint32_t Insert8(uint32_t c8, uint32_t mask) {
    if (!mask) return 0;
    uint32_t shift = __builtin_ctz(mask);
    uint32_t max = mask >> shift;
    uint32_t c = max == 255 ? c8 : (c8 * max + 127) / 255;
    return (c << shift) & mask;
}

uint32_t PixelToARGB(const Format& f, uint32_t v, const PALETTEENTRY* pal) {
    if (f.pal8) {
        if (!pal) return 0xFF000000u | (v & 0xFF) * 0x010101u;
        const PALETTEENTRY& e = pal[v & 0xFF];
        return 0xFF000000u | (uint32_t(e.peRed) << 16) | (uint32_t(e.peGreen) << 8) | e.peBlue;
    }
    uint32_t a = f.a ? Extract8(v, f.a) : 0xFF;
    return (a << 24) | (Extract8(v, f.r) << 16) | (Extract8(v, f.g) << 8) | Extract8(v, f.b);
}

uint32_t ARGBToPixel(const Format& f, uint32_t argb) {
    if (f.pal8) return 0;  // no reverse palette lookup
    return Insert8((argb >> 16) & 0xFF, f.r) | Insert8((argb >> 8) & 0xFF, f.g) | Insert8(argb & 0xFF, f.b) |
           Insert8(argb >> 24, f.a);
}

// ---------------------------------------------------------------- descriptions

bool DescSizeOk(const void* desc) {
    if (!desc) return false;
    DWORD size = *static_cast<const DWORD*>(desc);
    return size == sizeof(DDSURFACEDESC) || size == sizeof(DDSURFACEDESC2);
}

bool DescIn(const void* in, DDSURFACEDESC2* out) {
    if (!DescSizeOk(in)) return false;
    if (*static_cast<const DWORD*>(in) == sizeof(DDSURFACEDESC2)) {
        *out = *static_cast<const DDSURFACEDESC2*>(in);
        return true;
    }
    const auto* d = static_cast<const DDSURFACEDESC*>(in);
    memset(out, 0, sizeof(*out));
    out->dwSize = sizeof(DDSURFACEDESC2);
    out->dwFlags = d->dwFlags;
    out->dwHeight = d->dwHeight;
    out->dwWidth = d->dwWidth;
    out->lPitch = d->lPitch;
    out->dwBackBufferCount = d->dwBackBufferCount;
    out->dwMipMapCount = d->dwMipMapCount;  // same slot as dwZBufferBitDepth / dwRefreshRate
    out->dwAlphaBitDepth = d->dwAlphaBitDepth;
    out->lpSurface = d->lpSurface;
    out->ddckCKDestOverlay = d->ddckCKDestOverlay;
    out->ddckCKDestBlt = d->ddckCKDestBlt;
    out->ddckCKSrcOverlay = d->ddckCKSrcOverlay;
    out->ddckCKSrcBlt = d->ddckCKSrcBlt;
    out->ddpfPixelFormat = d->ddpfPixelFormat;
    out->ddsCaps.dwCaps = d->ddsCaps.dwCaps;
    return true;
}

void DescOut(const DDSURFACEDESC2& in, void* out) {
    if (*static_cast<DWORD*>(out) == sizeof(DDSURFACEDESC2)) {
        *static_cast<DDSURFACEDESC2*>(out) = in;
        static_cast<DDSURFACEDESC2*>(out)->dwSize = sizeof(DDSURFACEDESC2);
        return;
    }
    auto* d = static_cast<DDSURFACEDESC*>(out);
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(DDSURFACEDESC);
    d->dwFlags = in.dwFlags;
    d->dwHeight = in.dwHeight;
    d->dwWidth = in.dwWidth;
    d->lPitch = in.lPitch;
    d->dwBackBufferCount = in.dwBackBufferCount;
    d->dwMipMapCount = in.dwMipMapCount;
    d->dwAlphaBitDepth = in.dwAlphaBitDepth;
    d->lpSurface = in.lpSurface;
    d->ddckCKDestOverlay = in.ddckCKDestOverlay;
    d->ddckCKDestBlt = in.ddckCKDestBlt;
    d->ddckCKSrcOverlay = in.ddckCKSrcOverlay;
    d->ddckCKSrcBlt = in.ddckCKSrcBlt;
    d->ddpfPixelFormat = in.ddpfPixelFormat;
    d->ddsCaps.dwCaps = in.ddsCaps.dwCaps;
}

}  // namespace dd2d9
