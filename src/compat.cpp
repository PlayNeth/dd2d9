// Compatibility options: what the game is told about the machine, as a PC of its era would
// answer. None of this is DirectDraw; it is here because dd2d9 is the DLL old games load.
//   ram=  GlobalMemoryStatus(Ex): Windows caps physical memory at 2 GB for programs that
//         aren't large-address-aware, and games of the era that add to it or scale it
//         overflow their signed arithmetic and refuse to run ("not enough memory").
//   cpu=  the processor clock in the registry (~MHz under HKLM, HARDWARE\DESCRIPTION\System
//         \CentralProcessor\<n>). Games that time the processor with RDTSC measure the
//         real one: a DLL can't intercept that instruction.
//   cpus= the processors the game runs on (its affinity) and is told about (GetSystemInfo):
//         games of the single-core era can race or stutter across cores.
// The imports are patched in the game's modules (hooks.cpp): from DllMain, before the game's
// own code runs, and again when it creates a DirectDraw object (DLLs loaded since).
#include "hooks.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

namespace dd2d9 {

namespace {

// ---------------------------------------------------------------- ram=

using GlobalMemoryStatusFn = void(WINAPI*)(MEMORYSTATUS*);
using GlobalMemoryStatusExFn = BOOL(WINAPI*)(MEMORYSTATUSEX*);
GlobalMemoryStatusFn g_globalMemoryStatus;
GlobalMemoryStatusExFn g_globalMemoryStatusEx;

// The configured machine: ram= of physical memory, at most 3/4 of it free (less if the real
// machine has less free), a commit limit (memory plus page file) of 1.5x the memory. Every
// value stays below 2 GB, as on the machines these games knew.
struct FakeMemory {
    uint64_t totalPhys, availPhys, totalPage, availPage;
    DWORD load;
};

FakeMemory Fake(uint64_t realAvailPhys, uint64_t realAvailPage) {
    constexpr uint64_t kMax = 0x7FFFFFFF;
    FakeMemory m;
    m.totalPhys = uint64_t(g_config.ramMB) << 20;
    m.availPhys = std::min(realAvailPhys, m.totalPhys / 4 * 3);
    m.totalPage = std::min(m.totalPhys + m.totalPhys / 2, kMax);
    m.availPage = std::min(m.totalPage - (m.totalPhys - m.availPhys), realAvailPage);
    m.load = DWORD((m.totalPhys - m.availPhys) * 100 / m.totalPhys);
    return m;
}

void WINAPI HookGlobalMemoryStatus(MEMORYSTATUS* s) {
    g_globalMemoryStatus(s);
    if (!s) return;
    const FakeMemory m = Fake(s->dwAvailPhys, s->dwAvailPageFile);
    s->dwMemoryLoad = m.load;
    s->dwTotalPhys = SIZE_T(m.totalPhys);
    s->dwAvailPhys = SIZE_T(m.availPhys);
    s->dwTotalPageFile = SIZE_T(m.totalPage);
    s->dwAvailPageFile = SIZE_T(m.availPage);
    // dwTotalVirtual / dwAvailVirtual: the game's own address space, true as it is.
}

BOOL WINAPI HookGlobalMemoryStatusEx(MEMORYSTATUSEX* s) {
    if (!g_globalMemoryStatusEx(s)) return FALSE;
    const FakeMemory m = Fake(s->ullAvailPhys, s->ullAvailPageFile);
    s->dwMemoryLoad = m.load;
    s->ullTotalPhys = m.totalPhys;
    s->ullAvailPhys = m.availPhys;
    s->ullTotalPageFile = m.totalPage;
    s->ullAvailPageFile = m.availPage;
    return TRUE;
}

ImportHook kMemoryHooks[] = {
    {"kernel32", "GlobalMemoryStatus", reinterpret_cast<void*>(HookGlobalMemoryStatus),
     reinterpret_cast<void**>(&g_globalMemoryStatus)},
    {"kernel32", "GlobalMemoryStatusEx", reinterpret_cast<void*>(HookGlobalMemoryStatusEx),
     reinterpret_cast<void**>(&g_globalMemoryStatusEx)},
};

// ---------------------------------------------------------------- cpu=

using RegOpenKeyExAFn = LONG(WINAPI*)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
using RegOpenKeyExWFn = LONG(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
using RegOpenKeyAFn = LONG(WINAPI*)(HKEY, LPCSTR, PHKEY);
using RegOpenKeyWFn = LONG(WINAPI*)(HKEY, LPCWSTR, PHKEY);
using RegQueryValueExAFn = LONG(WINAPI*)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegQueryValueExWFn = LONG(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegCloseKeyFn = LONG(WINAPI*)(HKEY);
RegOpenKeyExAFn g_regOpenKeyExA;
RegOpenKeyExWFn g_regOpenKeyExW;
RegOpenKeyAFn g_regOpenKeyA;
RegOpenKeyWFn g_regOpenKeyW;
RegQueryValueExAFn g_regQueryValueExA;
RegQueryValueExWFn g_regQueryValueExW;
RegCloseKeyFn g_regCloseKey;

// The keys the game has open on the way to the processor keys, with their path under
// HKEY_LOCAL_MACHINE, in lower case: from "hardware" to a processor's key,
// "hardware\description\system\centralprocessor\0". Any other key isn't tracked.
SRWLOCK g_keyLock = SRWLOCK_INIT;
std::vector<std::pair<HKEY, std::wstring>>* g_keys;  // g_keyLock
const std::wstring kCpuPath = L"hardware\\description\\system\\centralprocessor";

// The key's path, if it is HKEY_LOCAL_MACHINE ("") or tracked.
bool PathOf(HKEY k, std::wstring* path) {
    if (k == HKEY_LOCAL_MACHINE) {
        path->clear();
        return true;
    }
    bool found = false;
    AcquireSRWLockShared(&g_keyLock);
    if (g_keys)
        for (const auto& [h, p] : *g_keys)
            if (h == k) {
                *path = p;
                found = true;
                break;
            }
    ReleaseSRWLockShared(&g_keyLock);
    return found;
}

void Remember(HKEY k, const std::wstring& path) {
    AcquireSRWLockExclusive(&g_keyLock);
    if (!g_keys) g_keys = new std::vector<std::pair<HKEY, std::wstring>>;
    // A handle value seen before (closed some way not hooked, and reused): the new key.
    for (auto& e : *g_keys)
        if (e.first == k) {
            e.second = path;
            ReleaseSRWLockExclusive(&g_keyLock);
            return;
        }
    g_keys->emplace_back(k, path);
    ReleaseSRWLockExclusive(&g_keyLock);
}

void Forget(HKEY k) {
    AcquireSRWLockExclusive(&g_keyLock);
    if (g_keys)
        for (size_t i = 0; i < g_keys->size(); i++)
            if ((*g_keys)[i].first == k) {
                (*g_keys)[i] = g_keys->back();
                g_keys->pop_back();
                break;
            }
    ReleaseSRWLockExclusive(&g_keyLock);
}

bool AllDigits(const std::wstring& s, size_t from) {
    if (from >= s.size()) return false;
    for (size_t i = from; i < s.size(); i++)
        if (s[i] < L'0' || s[i] > L'9') return false;
    return true;
}

// A processor's key: ...\centralprocessor\<n>.
bool IsCpuKey(const std::wstring& path) {
    return path.size() > kCpuPath.size() + 1 && path.compare(0, kCpuPath.size(), kCpuPath) == 0 &&
           path[kCpuPath.size()] == L'\\' && AllDigits(path, kCpuPath.size() + 1);
}

// On the way there: a whole-component prefix of the processor directory, or a processor
// ("": a new handle to HKEY_LOCAL_MACHINE itself, opened with no subkey).
bool Tracked(const std::wstring& path) {
    if (path.empty()) return true;
    if (path.size() <= kCpuPath.size())
        return kCpuPath.compare(0, path.size(), path) == 0 && (path.size() == kCpuPath.size() || kCpuPath[path.size()] == L'\\');
    return IsCpuKey(path);
}

bool IsCpuHandle(HKEY k) {
    std::wstring path;
    return k != HKEY_LOCAL_MACHINE && PathOf(k, &path) && IsCpuKey(path);
}

std::wstring Wide(LPCSTR s) {
    if (!s) return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(size_t(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

LONG Opened(LONG r, HKEY parent, std::wstring sub, PHKEY out) {
    if (r != ERROR_SUCCESS || !out || !*out) return r;
    std::wstring path;
    if (!PathOf(parent, &path)) return r;
    for (wchar_t& c : sub) c = towlower(c);
    while (!sub.empty() && sub.front() == L'\\') sub.erase(0, 1);
    while (!sub.empty() && sub.back() == L'\\') sub.pop_back();
    if (!sub.empty()) path = path.empty() ? sub : path + L'\\' + sub;
    if (Tracked(path)) Remember(*out, path);
    return r;
}

LONG WINAPI HookRegOpenKeyExA(HKEY k, LPCSTR sub, DWORD opt, REGSAM sam, PHKEY out) {
    return Opened(g_regOpenKeyExA(k, sub, opt, sam, out), k, Wide(sub), out);
}
LONG WINAPI HookRegOpenKeyExW(HKEY k, LPCWSTR sub, DWORD opt, REGSAM sam, PHKEY out) {
    return Opened(g_regOpenKeyExW(k, sub, opt, sam, out), k, sub ? sub : L"", out);
}
LONG WINAPI HookRegOpenKeyA(HKEY k, LPCSTR sub, PHKEY out) {
    return Opened(g_regOpenKeyA(k, sub, out), k, Wide(sub), out);
}
LONG WINAPI HookRegOpenKeyW(HKEY k, LPCWSTR sub, PHKEY out) {
    return Opened(g_regOpenKeyW(k, sub, out), k, sub ? sub : L"", out);
}

// RegCreateKey(Ex) opens existing keys too (some games open the processor keys that way).
using RegCreateKeyExAFn = LONG(WINAPI*)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM, const SECURITY_ATTRIBUTES*, PHKEY, LPDWORD);
using RegCreateKeyExWFn = LONG(WINAPI*)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM, const SECURITY_ATTRIBUTES*, PHKEY, LPDWORD);
using RegCreateKeyAFn = LONG(WINAPI*)(HKEY, LPCSTR, PHKEY);
using RegCreateKeyWFn = LONG(WINAPI*)(HKEY, LPCWSTR, PHKEY);
RegCreateKeyExAFn g_regCreateKeyExA;
RegCreateKeyExWFn g_regCreateKeyExW;
RegCreateKeyAFn g_regCreateKeyA;
RegCreateKeyWFn g_regCreateKeyW;

LONG WINAPI HookRegCreateKeyExA(HKEY k, LPCSTR sub, DWORD res, LPSTR cls, DWORD opt, REGSAM sam,
                                const SECURITY_ATTRIBUTES* sa, PHKEY out, LPDWORD disp) {
    return Opened(g_regCreateKeyExA(k, sub, res, cls, opt, sam, sa, out, disp), k, Wide(sub), out);
}
LONG WINAPI HookRegCreateKeyExW(HKEY k, LPCWSTR sub, DWORD res, LPWSTR cls, DWORD opt, REGSAM sam,
                                const SECURITY_ATTRIBUTES* sa, PHKEY out, LPDWORD disp) {
    return Opened(g_regCreateKeyExW(k, sub, res, cls, opt, sam, sa, out, disp), k, sub ? sub : L"", out);
}
LONG WINAPI HookRegCreateKeyA(HKEY k, LPCSTR sub, PHKEY out) { return Opened(g_regCreateKeyA(k, sub, out), k, Wide(sub), out); }
LONG WINAPI HookRegCreateKeyW(HKEY k, LPCWSTR sub, PHKEY out) {
    return Opened(g_regCreateKeyW(k, sub, out), k, sub ? sub : L"", out);
}

LONG WINAPI HookRegCloseKey(HKEY k) {
    const LONG r = g_regCloseKey(k);
    if (r == ERROR_SUCCESS) Forget(k);
    return r;
}

// ~MHz as a REG_DWORD, with RegQueryValueEx's size rules.
LONG FakeMHz(LPDWORD type, LPBYTE data, LPDWORD cb) {
    if (data && !cb) return ERROR_INVALID_PARAMETER;
    if (type) *type = REG_DWORD;
    if (!cb) return ERROR_SUCCESS;
    const DWORD need = sizeof(DWORD);
    if (!data) {
        *cb = need;
        return ERROR_SUCCESS;
    }
    if (*cb < need) {
        *cb = need;
        return ERROR_MORE_DATA;
    }
    const DWORD mhz = DWORD(g_config.cpuMHz);
    memcpy(data, &mhz, need);
    *cb = need;
    return ERROR_SUCCESS;
}

LONG WINAPI HookRegQueryValueExA(HKEY k, LPCSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD cb) {
    if (name && _stricmp(name, "~MHz") == 0 && IsCpuHandle(k)) return FakeMHz(type, data, cb);
    return g_regQueryValueExA(k, name, reserved, type, data, cb);
}

LONG WINAPI HookRegQueryValueExW(HKEY k, LPCWSTR name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD cb) {
    if (name && _wcsicmp(name, L"~MHz") == 0 && IsCpuHandle(k)) return FakeMHz(type, data, cb);
    return g_regQueryValueExW(k, name, reserved, type, data, cb);
}

ImportHook kCpuHooks[] = {
    {"advapi32", "RegOpenKeyExA", reinterpret_cast<void*>(HookRegOpenKeyExA), reinterpret_cast<void**>(&g_regOpenKeyExA)},
    {"advapi32", "RegOpenKeyExW", reinterpret_cast<void*>(HookRegOpenKeyExW), reinterpret_cast<void**>(&g_regOpenKeyExW)},
    {"advapi32", "RegOpenKeyA", reinterpret_cast<void*>(HookRegOpenKeyA), reinterpret_cast<void**>(&g_regOpenKeyA)},
    {"advapi32", "RegOpenKeyW", reinterpret_cast<void*>(HookRegOpenKeyW), reinterpret_cast<void**>(&g_regOpenKeyW)},
    {"advapi32", "RegQueryValueExA", reinterpret_cast<void*>(HookRegQueryValueExA),
     reinterpret_cast<void**>(&g_regQueryValueExA)},
    {"advapi32", "RegQueryValueExW", reinterpret_cast<void*>(HookRegQueryValueExW),
     reinterpret_cast<void**>(&g_regQueryValueExW)},
    {"advapi32", "RegCloseKey", reinterpret_cast<void*>(HookRegCloseKey), reinterpret_cast<void**>(&g_regCloseKey)},
    {"advapi32", "RegCreateKeyExA", reinterpret_cast<void*>(HookRegCreateKeyExA), reinterpret_cast<void**>(&g_regCreateKeyExA)},
    {"advapi32", "RegCreateKeyExW", reinterpret_cast<void*>(HookRegCreateKeyExW), reinterpret_cast<void**>(&g_regCreateKeyExW)},
    {"advapi32", "RegCreateKeyA", reinterpret_cast<void*>(HookRegCreateKeyA), reinterpret_cast<void**>(&g_regCreateKeyA)},
    {"advapi32", "RegCreateKeyW", reinterpret_cast<void*>(HookRegCreateKeyW), reinterpret_cast<void**>(&g_regCreateKeyW)},
};

// ---------------------------------------------------------------- cpus=

using GetSystemInfoFn = void(WINAPI*)(SYSTEM_INFO*);
GetSystemInfoFn g_getSystemInfo;
DWORD_PTR g_affinity;  // the processors the game was given (0: not set)
int g_cpuCount;

void WINAPI HookGetSystemInfo(SYSTEM_INFO* si) {
    g_getSystemInfo(si);
    if (!si || !g_affinity) return;
    si->dwNumberOfProcessors = DWORD(g_cpuCount);
    si->dwActiveProcessorMask = g_affinity;
}

ImportHook kCpusHooks[] = {
    {"kernel32", "GetSystemInfo", reinterpret_cast<void*>(HookGetSystemInfo), reinterpret_cast<void**>(&g_getSystemInfo)},
};

// cpus= processors the process may run on: one logical processor per physical core (two
// hyperthreads of one core would be about one core's worth), the first core last (it takes
// most of the system's interrupts) when there are cores to spare.
void SetAffinity() {
    DWORD_PTR process = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) return;
    std::vector<DWORD_PTR> cores;  // each core's first logical processor the process may use
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
    len = DWORD(info.size() * sizeof(info[0]));
    if (GetLogicalProcessorInformation(info.data(), &len)) {
        for (DWORD i = 0; i < len / sizeof(info[0]); i++) {
            if (info[i].Relationship != RelationProcessorCore) continue;
            const DWORD_PTR usable = info[i].ProcessorMask & process;
            if (usable) cores.push_back(usable & (~usable + 1));  // its lowest bit
        }
    }
    if (cores.empty())  // no topology: the logical processors themselves
        for (unsigned bit = 0; bit < sizeof(DWORD_PTR) * 8; bit++)
            if (process & (DWORD_PTR(1) << bit)) cores.push_back(DWORD_PTR(1) << bit);
    if (int(cores.size()) > g_config.cpus) std::rotate(cores.begin(), cores.begin() + 1, cores.end());  // core 0 last
    DWORD_PTR mask = 0;
    int count = 0;
    for (DWORD_PTR c : cores) {
        if (count >= g_config.cpus) break;
        mask |= c;
        count++;
    }
    if (!mask || !SetProcessAffinityMask(GetCurrentProcess(), mask)) return;
    g_affinity = mask;
    g_cpuCount = count;
    DD2D9_LOG("cpus: the game runs on %d processor(s) (mask 0x%lx)", count, static_cast<unsigned long>(mask));
}

// ---------------------------------------------------------------- desktopbpp=

// What a 16-bit (or 24-bit) desktop answered through GDI and EnumDisplaySettings: the colour
// depth, for display device contexts and the current (or registry) display settings. Games
// check it there too, as Moto Racer checks it through DirectDraw (GetDisplayMode, answered in
// ddraw.cpp). While the game has a display mode of its own, that mode's depth, as on XP. A
// palettized depth (8 bits) isn't faked there (GetDeviceCaps does answer one for an 8-bit mode
// of the game's own, below).
using GetDeviceCapsFn = int(WINAPI*)(HDC, int);
using EnumDisplaySettingsAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DEVMODEA*);
using EnumDisplaySettingsWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);
using EnumDisplaySettingsExAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DEVMODEA*, DWORD);
using EnumDisplaySettingsExWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*, DWORD);
GetDeviceCapsFn g_getDeviceCaps;
EnumDisplaySettingsAFn g_enumDisplaySettingsA;
EnumDisplaySettingsWFn g_enumDisplaySettingsW;
EnumDisplaySettingsExAFn g_enumDisplaySettingsExA;
EnumDisplaySettingsExWFn g_enumDisplaySettingsExW;

int ShownBppOrZero() {
    const int b = ShownDisplayBpp();
    return b >= 16 ? b : 0;
}

// Always on (not only with desktopbpp=): in a display mode of the game's own, that mode's
// depth and size, as XP's GDI answered after ddraw.dll switched the display. An 8-bit one makes the
// display a palette device (RC_PALETTE, 256 entries, the 20 static colours): FlyingSB (and
// other games) set up their DirectDraw palette only when the screen is one, and showed
// dd2d9's grey ramp instead of their colours.
int WINAPI HookGetDeviceCaps(HDC dc, int index) {
    const int real = g_getDeviceCaps(dc, index);
    const bool size = index == HORZRES || index == VERTRES || index == DESKTOPHORZRES || index == DESKTOPVERTRES;
    if (!size && index != BITSPIXEL && index != NUMCOLORS && index != RASTERCAPS && index != SIZEPALETTE &&
        index != NUMRESERVED && index != COLORRES)
        return real;
    const DWORD type = GetObjectType(dc);
    if ((type != OBJ_DC && type != OBJ_MEMDC) || g_getDeviceCaps(dc, TECHNOLOGY) != DT_RASDISPLAY) return real;
    if (size) {
        // The screen's size: the mode's, as GetSystemMetrics answers it (input.cpp).
        int w, h;
        if (!ShownDisplaySize(&w, &h)) return real;
        return index == HORZRES || index == DESKTOPHORZRES ? w : h;
    }
    const int bpp = ShownDisplayBpp();
    if (bpp == 8) {
        switch (index) {
        case BITSPIXEL: return 8;
        case NUMCOLORS: return 20;
        case RASTERCAPS: return real | RC_PALETTE;
        case SIZEPALETTE: return 256;
        case NUMRESERVED: return 20;
        default: return 18;  // COLORRES: the DAC's 6 bits a channel
        }
    }
    if (bpp < 16) return real;
    if (index == BITSPIXEL) return bpp;
    if (index == NUMCOLORS) return -1;  // above 8 bits
    return real;
}

// The current or registry settings of the primary display (null or its name): the depth.
template <typename Dev, typename Ch>
void FixDepth(const Ch* device, DWORD mode, Dev* dm, BOOL ok) {
    if (!ok || !dm || (mode != ENUM_CURRENT_SETTINGS && mode != ENUM_REGISTRY_SETTINGS)) return;
    if (device && device[0]) {  // named: only if that is the primary display
        bool primary = false;
        DISPLAY_DEVICEW dd = {};
        dd.cb = sizeof(dd);
        for (DWORD n = 0; !primary && EnumDisplayDevicesW(nullptr, n, &dd, 0); n++, dd.cb = sizeof(dd)) {
            if (!(dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) continue;
            bool same = true;  // compared as text, case-insensitively (ASCII names)
            for (int i = 0; i < 32 && same; i++) {
                const wchar_t a = dd.DeviceName[i];
                const wchar_t b = wchar_t(static_cast<unsigned>(device[i]) & (sizeof(Ch) == 1 ? 0xFFu : 0xFFFFu));
                if (towupper(a) != towupper(b)) same = false;
                else if (!a) break;
            }
            primary = same;
        }
        if (!primary) return;
    }
    if (const int bpp = ShownBppOrZero()) dm->dmBitsPerPel = DWORD(bpp);
    // The current mode the game's own while it is shown scaled or in a window (borderless,
    // window), as after a real switch: a game checking that its mode took saw the desktop's.
    RECT r;
    SIZE m;
    if (mode == ENUM_CURRENT_SETTINGS && GameImage(&r, &m)) {
        dm->dmPelsWidth = DWORD(m.cx);
        dm->dmPelsHeight = DWORD(m.cy);
    }
}

BOOL WINAPI HookEnumDisplaySettingsA(LPCSTR device, DWORD mode, DEVMODEA* dm) {
    const BOOL ok = g_enumDisplaySettingsA(device, mode, dm);
    FixDepth(device, mode, dm, ok);
    return ok;
}
BOOL WINAPI HookEnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW* dm) {
    const BOOL ok = g_enumDisplaySettingsW(device, mode, dm);
    FixDepth(device, mode, dm, ok);
    return ok;
}
BOOL WINAPI HookEnumDisplaySettingsExA(LPCSTR device, DWORD mode, DEVMODEA* dm, DWORD flags) {
    const BOOL ok = g_enumDisplaySettingsExA(device, mode, dm, flags);
    FixDepth(device, mode, dm, ok);
    return ok;
}
BOOL WINAPI HookEnumDisplaySettingsExW(LPCWSTR device, DWORD mode, DEVMODEW* dm, DWORD flags) {
    const BOOL ok = g_enumDisplaySettingsExW(device, mode, dm, flags);
    FixDepth(device, mode, dm, ok);
    return ok;
}

// A mode change asking for the depth EnumDisplaySettings made up (a game putting back the
// "current" mode it read): the depth left as it is, not switched to that one for real.
template <typename Dev> const Dev* RealDepth(const Dev* dm, Dev* copy) {
    const int bpp = ShownBppOrZero();
    if (!dm || !bpp || !(dm->dmFields & DM_BITSPERPEL) || dm->dmBitsPerPel != DWORD(bpp)) return dm;
    *copy = *dm;  // (the public part: dmDriverExtra bytes after it aren't passed on)
    copy->dmDriverExtra = 0;
    copy->dmFields &= ~DWORD(DM_BITSPERPEL);
    return copy;
}

using ChangeDisplaySettingsAFn = LONG(WINAPI*)(DEVMODEA*, DWORD);
using ChangeDisplaySettingsWFn = LONG(WINAPI*)(DEVMODEW*, DWORD);
using ChangeDisplaySettingsExAFn = LONG(WINAPI*)(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID);
using ChangeDisplaySettingsExWFn = LONG(WINAPI*)(LPCWSTR, DEVMODEW*, HWND, DWORD, LPVOID);
ChangeDisplaySettingsAFn g_changeDisplaySettingsA;
ChangeDisplaySettingsWFn g_changeDisplaySettingsW;
ChangeDisplaySettingsExAFn g_changeDisplaySettingsExA;
ChangeDisplaySettingsExWFn g_changeDisplaySettingsExW;

// The mode the game is shown in already (borderless, window: EnumDisplaySettings reports it
// as current): a game putting it back, or asking for it again, would really switch the
// display under the scaled picture. Nothing to do, as on a display in that mode.
template <typename Dev> bool AlreadyShown(const Dev* dm) {
    RECT r;
    SIZE m;
    return dm && (dm->dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT)) == (DM_PELSWIDTH | DM_PELSHEIGHT) &&
           GameImage(&r, &m) && dm->dmPelsWidth == DWORD(m.cx) && dm->dmPelsHeight == DWORD(m.cy);
}

LONG WINAPI HookChangeDisplaySettingsA(DEVMODEA* dm, DWORD flags) {
    if (AlreadyShown(dm)) return DISP_CHANGE_SUCCESSFUL;
    DEVMODEA c;
    return g_changeDisplaySettingsA(const_cast<DEVMODEA*>(RealDepth(dm, &c)), flags);
}
LONG WINAPI HookChangeDisplaySettingsW(DEVMODEW* dm, DWORD flags) {
    if (AlreadyShown(dm)) return DISP_CHANGE_SUCCESSFUL;
    DEVMODEW c;
    return g_changeDisplaySettingsW(const_cast<DEVMODEW*>(RealDepth(dm, &c)), flags);
}
// (A display named: only the primary's depth is made up; the name left to the system.)
LONG WINAPI HookChangeDisplaySettingsExA(LPCSTR device, DEVMODEA* dm, HWND h, DWORD flags, LPVOID p) {
    if (!(device && device[0]) && AlreadyShown(dm)) return DISP_CHANGE_SUCCESSFUL;
    DEVMODEA c;
    return g_changeDisplaySettingsExA(device, device && device[0] ? dm : const_cast<DEVMODEA*>(RealDepth(dm, &c)), h, flags, p);
}
LONG WINAPI HookChangeDisplaySettingsExW(LPCWSTR device, DEVMODEW* dm, HWND h, DWORD flags, LPVOID p) {
    if (!(device && device[0]) && AlreadyShown(dm)) return DISP_CHANGE_SUCCESSFUL;
    DEVMODEW c;
    return g_changeDisplaySettingsExW(device, device && device[0] ? dm : const_cast<DEVMODEW*>(RealDepth(dm, &c)), h, flags, p);
}

ImportHook kDepthHooks[] = {
    {"user32", "ChangeDisplaySettingsA", reinterpret_cast<void*>(HookChangeDisplaySettingsA),
     reinterpret_cast<void**>(&g_changeDisplaySettingsA)},
    {"user32", "ChangeDisplaySettingsW", reinterpret_cast<void*>(HookChangeDisplaySettingsW),
     reinterpret_cast<void**>(&g_changeDisplaySettingsW)},
    {"user32", "ChangeDisplaySettingsExA", reinterpret_cast<void*>(HookChangeDisplaySettingsExA),
     reinterpret_cast<void**>(&g_changeDisplaySettingsExA)},
    {"user32", "ChangeDisplaySettingsExW", reinterpret_cast<void*>(HookChangeDisplaySettingsExW),
     reinterpret_cast<void**>(&g_changeDisplaySettingsExW)},
    {"user32", "EnumDisplaySettingsA", reinterpret_cast<void*>(HookEnumDisplaySettingsA),
     reinterpret_cast<void**>(&g_enumDisplaySettingsA)},
    {"user32", "EnumDisplaySettingsW", reinterpret_cast<void*>(HookEnumDisplaySettingsW),
     reinterpret_cast<void**>(&g_enumDisplaySettingsW)},
    {"user32", "EnumDisplaySettingsExA", reinterpret_cast<void*>(HookEnumDisplaySettingsExA),
     reinterpret_cast<void**>(&g_enumDisplaySettingsExA)},
    {"user32", "EnumDisplaySettingsExW", reinterpret_cast<void*>(HookEnumDisplaySettingsExW),
     reinterpret_cast<void**>(&g_enumDisplaySettingsExW)},
};

// LockWindowUpdate on the window dd2d9 shows the primary in: not done. ddraw.dll drew a
// windowed primary straight on the screen, which a window locked from painting didn't stop;
// here the picture goes through that window, and a lock would hide all of it. 3DMark 2000's
// splash locks its full-screen window (so that Windows doesn't paint over the desktop
// behind) and blits its logo to the primary: nothing showed. Unlocking (NULL) after a lock
// not done isn't passed on either. (This table is always on: not a compatibility option.)
using LockWindowUpdateFn = BOOL(WINAPI*)(HWND);
LockWindowUpdateFn g_lockWindowUpdate;
volatile LONG g_lockSkipped;  // the last lock was one of those

BOOL WINAPI HookLockWindowUpdate(HWND hwnd) {
    if (hwnd && PresentsOnWindow(hwnd)) {
        InterlockedExchange(&g_lockSkipped, 1);
        DD2D9_LOG("LockWindowUpdate(%p): not done (the primary is shown in that window)", static_cast<void*>(hwnd));
        return TRUE;
    }
    if (!hwnd && InterlockedExchange(&g_lockSkipped, 0)) return TRUE;
    if (hwnd) InterlockedExchange(&g_lockSkipped, 0);
    return g_lockWindowUpdate(hwnd);
}

ImportHook kWindowHooks[] = {
    {"user32", "LockWindowUpdate", reinterpret_cast<void*>(HookLockWindowUpdate), reinterpret_cast<void**>(&g_lockWindowUpdate)},
    {"gdi32", "GetDeviceCaps", reinterpret_cast<void*>(HookGetDeviceCaps), reinterpret_cast<void**>(&g_getDeviceCaps)},
};

// Memory from the process heap comes zeroed, as fresh heap memory did on Windows 9x, where
// games were tested: today's heap hands back blocks other code freed (Direct3D's, the
// driver's) with their old contents. Daytona USA Deluxe allocates its texture table with
// HeapAlloc and, before clearing it, releases every "texture" it finds in it: on a block
// full of old data, it called through a stale pointer and crashed as its 3D device came up.
// (Always on: zeroing is what HEAP_ZERO_MEMORY does anyway, and no working game depends on
// garbage.)
using HeapAllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
HeapAllocFn g_heapAlloc;

LPVOID WINAPI HookHeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes) {
    return g_heapAlloc(heap, flags | HEAP_ZERO_MEMORY, bytes);
}

ImportHook kHeapHooks[] = {
    {"kernel32", "HeapAlloc", reinterpret_cast<void*>(HookHeapAlloc), reinterpret_cast<void**>(&g_heapAlloc)},
};

// Functions looked up at run time (games that also ran on Windows 95 find
// GlobalMemoryStatusEx this way): the hooked ones are answered with dd2d9's.
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
GetProcAddressFn g_getProcAddress;
struct HookTable {
    ImportHook* hooks;
    size_t n;
    bool on;
};
HookTable g_tables[6];

FARPROC WINAPI HookGetProcAddress(HMODULE m, LPCSTR name) {
    const FARPROC p = g_getProcAddress(m, name);
    if (p)
        for (const HookTable& t : g_tables)
            for (size_t i = 0; t.on && i < t.n; i++)
                if (*t.hooks[i].original == reinterpret_cast<void*>(p)) return reinterpret_cast<FARPROC>(t.hooks[i].replacement);
    return p;
}

ImportHook kProcHooks[] = {
    {"kernel32", "GetProcAddress", reinterpret_cast<void*>(HookGetProcAddress), reinterpret_cast<void**>(&g_getProcAddress)},
};

}  // namespace

void InstallCompatHooks(bool inLoaderLock) {
    if (inLoaderLock && g_config.cpus) SetAffinity();
    g_tables[0] = {kMemoryHooks, sizeof(kMemoryHooks) / sizeof(kMemoryHooks[0]), g_config.ramMB != 0};
    g_tables[1] = {kCpuHooks, sizeof(kCpuHooks) / sizeof(kCpuHooks[0]), g_config.cpuMHz != 0};
    g_tables[2] = {kCpusHooks, sizeof(kCpusHooks) / sizeof(kCpusHooks[0]), g_affinity != 0};
    // (Always: besides desktopbpp's depth, the current mode is the game's while it is shown
    // scaled or in a window, and a change to that mode does nothing; see FixDepth.)
    g_tables[3] = {kDepthHooks, sizeof(kDepthHooks) / sizeof(kDepthHooks[0]), true};
    g_tables[4] = {kWindowHooks, sizeof(kWindowHooks) / sizeof(kWindowHooks[0]), true};
    g_tables[5] = {kHeapHooks, sizeof(kHeapHooks) / sizeof(kHeapHooks[0]), true};
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
