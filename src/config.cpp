// Configuration: dd2d9.ini files and the DD2D9_INI / DD2D9_LOG environment variables.
#include "configint.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace dd2d9 {

Config g_config;
const char* g_tilingWm = nullptr;
std::wstring g_dllPath;
std::vector<std::wstring> g_iniLoaded;
std::vector<std::wstring> g_iniErrors;

std::wstring ModulePath(HMODULE m) {
    std::wstring s(kPathMax, L'\0');
    DWORD n = GetModuleFileNameW(m, &s[0], DWORD(s.size()));
    s.resize(n < s.size() ? n : 0);
    return s;
}

std::wstring DirOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

std::wstring BaseName(const std::wstring& path) {
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

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// A variable of the Unix environment: Wine hands the XDG_ ones on renamed, as WINE_HOST_XDG_...
// (XDG_RUNTIME_DIR, XDG_CURRENT_DESKTOP, ...: measured under Wine 11.22); others as they are.
static std::wstring HostEnv(const wchar_t* name) {
    const std::wstring renamed = Env((std::wstring(L"WINE_HOST_") + name).c_str());
    return renamed.empty() ? Env(name) : renamed;
}

// Under Wine, a Unix path as Windows sees it (kernel32's wine_get_dos_file_name; empty elsewhere).
static std::wstring DosPath(const std::string& unix) {
    using Fn = WCHAR*(CDECL*)(LPCSTR);
    static const auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name")));
    if (!fn) return std::wstring();
    WCHAR* p = fn(unix.c_str());
    if (!p) return std::wstring();
    std::wstring r = p;
    HeapFree(GetProcessHeap(), 0, p);
    if (!unix.empty() && unix.back() == '/' && (r.empty() || r.back() != L'\\')) r += L'\\';  // (a directory: ends in one)
    return r;
}

// Under Wine, i3 or sway running, found by its IPC socket, for a game started where neither's
// variables reach (a file manager D-Bus started, a launcher systemd started). i3 makes its socket
// as $XDG_RUNTIME_DIR/i3/ipc-socket.<pid> (or /tmp/i3-<user>.XXXXXX/ipc-socket.<pid> without
// that variable: i3's get_process_filename), sway as $XDG_RUNTIME_DIR/sway-ipc.<uid>.<pid>.sock;
// a socket counts while its process lives (/proc/<pid>: one left behind by a crash doesn't).
static const char* TilingWmBySocket() {
    const std::wstring proc = DosPath("/proc/");
    if (proc.empty()) return nullptr;  // (not Wine)
    auto alive = [&](const std::wstring& pid) {
        if (pid.empty() || pid.find_first_not_of(L"0123456789") != std::wstring::npos) return false;
        return GetFileAttributesW((proc + pid).c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    // Each name matching `pattern` in `dir`, its process id between `prefix` and `suffix`.
    auto found = [&](const std::wstring& dir, const wchar_t* pattern, size_t prefix, size_t suffix) {
        WIN32_FIND_DATAW fd;
        const HANDLE h = FindFirstFileW((dir + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return false;
        bool any = false;
        do {
            const std::wstring n = fd.cFileName;
            if (n.size() <= suffix || (prefix != size_t(-1) && n.size() <= prefix + suffix)) continue;
            std::wstring pid = n.substr(0, n.size() - suffix);
            if (prefix == size_t(-1)) pid = pid.substr(pid.find_last_of(L'.') + 1);  // (sway: after its uid)
            else pid = pid.substr(prefix);
            any = alive(pid);
        } while (!any && FindNextFileW(h, &fd));
        FindClose(h);
        return any;
    };
    std::vector<std::wstring> runtime;
    if (const std::wstring x = HostEnv(L"XDG_RUNTIME_DIR"); !x.empty()) runtime.push_back(DosPath(Utf8(x) + "/"));
    else if (const std::wstring users = DosPath("/run/user/"); !users.empty()) {
        WIN32_FIND_DATAW fd;
        const HANDLE h = FindFirstFileW((users + L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
                    runtime.push_back(users + fd.cFileName + L"\\");
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    for (const std::wstring& dir : runtime) {
        if (dir.empty()) continue;
        if (found(dir, L"sway-ipc.*.sock", size_t(-1), 5)) return "sway";
        if (found(dir + L"i3\\", L"ipc-socket.*", 11, 0)) return "i3";
    }
    if (const std::wstring tmp = DosPath("/tmp/"); !tmp.empty()) {
        WIN32_FIND_DATAW fd;
        const HANDLE h = FindFirstFileW((tmp + L"i3-*").c_str(), &fd);
        bool i3 = false;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    i3 = found(tmp + fd.cFileName + L"\\", L"ipc-socket.*", 11, 0);
            } while (!i3 && FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (i3) return "i3";
    }
    return nullptr;
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
    // On or off.
    static const struct { const char* key; bool* field; } kBools[] = {
        {"antialias2d", &g_config.aa2d},
        {"aa2d", &g_config.aa2d},
        {"palettizedtextures", &g_config.palTextures},
        {"videooverlay", &g_config.overlays},
        {"videooverlays", &g_config.overlays},
        {"lockwhole", &g_config.lockWhole},
        {"zaccess", &g_config.zAccess},
        {"mipmaps", &g_config.mipmaps},
        {"alternatepixelcenter", &g_config.pixelCenter},
        {"render32", &g_config.render32},
        {"depth24", &g_config.depth24},
        {"drawtrace", &g_config.drawTrace},
        {"heapzero", &g_config.heapZero},
        {"fairlocks", &g_config.fairLocks},
        {"safetimers", &g_config.safeTimers},
        {"joypoll", &g_config.joyPoll},
        {"selfsuspend", &g_config.selfSuspend},
        {"nobuffering", &g_config.noBuffering},
        {"safeterminate", &g_config.safeTerminate},
        {"dpiaware", &g_config.dpiAware},
        {"mousemerge", &g_config.mouseMerge},
    };
    for (const auto& b : kBools) {
        if (key != b.key) continue;
        int on;
        if (ParseBool(v, &on)) *b.field = on != 0;
        else g_iniErrors.push_back(where + L": ignored " + FromUtf8(key + "=" + rawVal));
        return;
    }
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
        else if (v == "sharp" || v == "bilinear-sharp" || v == "sharp-bilinear" || v == "2") g_config.filter = 2;
        else ok = false;
    } else if (key == "present") {
        // Gone in 0.7.17 (the shared Direct3D 9 device presents through a window's swap
        // chain): accepted and ignored, so old ini files don't log an error.
    } else if (key == "d3d") {
        int b;
        if (v == "auto") g_config.d3d = kD3DAuto;
        else if (ParseBool(v, &b)) g_config.d3d = b ? kD3DOn : kD3DOff;
        else ok = false;
    } else if (key == "flipcopy") {
        int b;
        if (v == "auto") g_config.flipCopy = kFlipCopyAuto;
        else if (ParseBool(v, &b)) g_config.flipCopy = b ? kFlipCopyOn : kFlipCopyOff;
        else ok = false;
    } else if (key == "minimize") {
        int b;
        if (v == "auto") g_config.minimize = kMinimizeAuto;
        else if (ParseBool(v, &b)) g_config.minimize = b ? kMinimizeOn : kMinimizeOff;
        else ok = false;
    } else if (key == "mouse") {
        // Gone in 1.3.0: the game always sees its mode's coordinates when it is shown scaled or
        // in a window (mouse=raw put clicks in the wrong place, and its screen size at odds
        // with what it was told). Noted, not an error.
        g_iniErrors.push_back(where + L": mouse= no longer exists (always scaled): ignored");
    } else if (key == "d3d9") {
        if (v == "system") g_config.systemD3D9 = true;
        else if (v == "game" || v == "local") g_config.systemD3D9 = false;
        else ok = false;
    } else if (key == "vsync") {
        int b;
        if (v == "auto" || v == "game") g_config.vsync = kVSyncAuto;
        else if (ParseBool(v, &b)) g_config.vsync = b ? kVSyncOn : kVSyncOff;
        else ok = false;
    } else if (key == "ram" || key == "vram" || key == "cpu" || key == "cpus") {
        // Whole numbers; ram, cpu and cpus take 0 or off for the real values. Capped below
        // 2 GB: values from 2048 MB up overflow the signed arithmetic of games of the era.
        static const struct { const char* key; int* field; int lo, hi; } kNums[] = {
            {"ram", &g_config.ramMB, 0, 2047},
            {"vram", &g_config.vramMB, 1, 2047},
            {"cpu", &g_config.cpuMHz, 0, 100000},
            {"cpus", &g_config.cpus, 0, 64},
        };
        for (const auto& n : kNums) {
            if (key != n.key) continue;
            char* end = nullptr;
            const long x = v == "off" || v == "real" ? 0 : strtol(v.c_str(), &end, 10);
            if (end && (end == v.c_str() || *end)) ok = false;
            else if (x < n.lo || x > n.hi) ok = false;
            else *n.field = int(x);
        }
    } else if (key == "desktopbpp" || key == "desktopdepth") {
        // Games that check the desktop is 16-bit before switching to their own mode (Moto
        // Racer: GetDisplayMode before SetCooperativeLevel, "16 bit screen mode required"), or
        // that it has 256 colours.
        const long x = strtol(v.c_str(), nullptr, 10);
        if (v == "8" || v == "16" || v == "24" || v == "32") g_config.desktopBpp = int(x);
        else ok = false;
    } else if (key == "fpslimit" || key == "fps") {
        char* end = nullptr;
        const long x = v == "off" ? 0 : strtol(v.c_str(), &end, 10);
        if (end && (end == v.c_str() || *end)) ok = false;
        else if (x < 0 || x > 1000) ok = false;
        else g_config.fpsLimit = int(x);
    } else if (key == "overlaybias") {
        int b;
        char* end = nullptr;
        const long x = ParseBool(v, &b) ? b : strtol(v.c_str(), &end, 10);
        if (end && (end == v.c_str() || *end)) ok = false;
        else if (x < 0 || x > 4096) ok = false;
        else g_config.overlayBias = int(x);
    } else if (key == "wbuffer") {
        int b;
        if (v == "force") g_config.wbuffer = 2;
        else if (v == "on" || v == "emulate") g_config.wbuffer = 1;
        else if (ParseBool(v, &b)) g_config.wbuffer = b;
        else ok = false;
    } else if (key == "texfilter") {
        if (v == "game" || v == "off" || v == "0") g_config.texLinear = false;
        else if (v == "linear" || v == "bilinear" || v == "on" || v == "1") g_config.texLinear = true;
        else ok = false;
    } else if (key == "antialiasing" || key == "msaa" || key == "aa") {
        int b;
        char* end = nullptr;
        const long x = ParseBool(v, &b) && !b ? 0 : strtol(v.c_str(), &end, 10);
        if (end && (end == v.c_str() || *end)) ok = false;
        else if (x < 0 || x > 16) ok = false;
        else g_config.msaa = int(x);
    } else if (key == "anisotropy" || key == "af") {
        int b;
        char* end = nullptr;
        const long x = ParseBool(v, &b) && !b ? 0 : strtol(v.c_str(), &end, 10);
        if (end && (end == v.c_str() || *end)) ok = false;
        else if (x < 0 || x > 16) ok = false;
        else g_config.anisotropy = int(x);
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
// gameSections: this pass reads the game's own sections only, else only the general ones
// (LoadConfig reads every file for the general ones first, so a game's section overrides
// them wherever they are).
static bool LoadIni(const std::wstring& path, const std::string& exeName, bool gameSections) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    if (data.size() >= 3 && data.compare(0, 3, "\xEF\xBB\xBF") == 0) data.erase(0, 3);  // UTF-8 BOM

    bool active = !gameSections;  // keys before any section: general
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
            active = gameSections ? name == exeName : name == "dd2d9" || name == "default";
            continue;
        }
        if (!active) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            g_iniErrors.push_back(BaseName(path) + L":" + std::to_wstring(lineNo) + L": ignored " + FromUtf8(line));
            continue;
        }
        std::string val = Trim(line.substr(eq + 1));
        // "key=value ; comment" (a space or tab before the ;), "key= ; comment" (no value).
        for (size_t i = 0; i < val.size(); i++)
            if (val[i] == ';' && (i == 0 || val[i - 1] == ' ' || val[i - 1] == '\t')) {
                val = Trim(val.substr(0, i));
                break;
            }
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
        if (LoadIni(f, exeName, false)) g_iniLoaded.push_back(f);
    for (const auto& f : g_iniLoaded) LoadIni(f, exeName, true);

    // i3 gives the programs it starts its IPC socket's path in I3SOCK; sway in SWAYSOCK, and in
    // I3SOCK too, for i3's tools (Wine passes both on to the game).
    if (!Env(L"SWAYSOCK").empty()) g_tilingWm = "sway";
    else if (!Env(L"I3SOCK").empty()) g_tilingWm = "i3";
    // A game started from elsewhere than i3 (a launcher started by systemd or D-Bus) has neither:
    // the session's desktop then, as display managers name it (XDG_CURRENT_DESKTOP, a list
    // separated by colons; XDG_SESSION_DESKTOP; DESKTOP_SESSION), "i3", "sway", or one of their
    // variants ("i3-with-shmlog").
    if (!g_tilingWm) {
        for (const wchar_t* name : {L"XDG_CURRENT_DESKTOP", L"XDG_SESSION_DESKTOP", L"DESKTOP_SESSION"}) {
            const std::string v = Lower(Utf8(HostEnv(name)));
            for (size_t at = 0; at <= v.size() && !g_tilingWm;) {
                const size_t end = std::min(v.find(':', at), v.size());
                const std::string d = v.substr(at, end - at);
                if (d == "i3" || d.rfind("i3-", 0) == 0) g_tilingWm = "i3";
                else if (d == "sway" || d.rfind("sway-", 0) == 0) g_tilingWm = "sway";
                at = end + 1;
            }
            if (g_tilingWm) break;
        }
    }
    if (!g_tilingWm && g_config.minimize == kMinimizeAuto) g_tilingWm = TilingWmBySocket();  // (only auto reads it)

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

bool LoadedBehindWrapper() {
    return _wcsicmp(BaseName(g_dllPath).c_str(), L"ddraw.dll") != 0;
}

}  // namespace dd2d9
