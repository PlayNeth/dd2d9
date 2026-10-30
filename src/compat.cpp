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

// On the way there: a whole-component prefix of the processor directory, or a processor.
bool Tracked(const std::wstring& path) {
    if (path.empty()) return false;
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

// The first cpus= processors the process may run on.
void SetAffinity() {
    DWORD_PTR process = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) return;
    DWORD_PTR mask = 0;
    int count = 0;
    for (unsigned bit = 0; bit < sizeof(DWORD_PTR) * 8 && count < g_config.cpus; bit++) {
        const DWORD_PTR b = DWORD_PTR(1) << bit;
        if (process & b) {
            mask |= b;
            count++;
        }
    }
    if (!mask || !SetProcessAffinityMask(GetCurrentProcess(), mask)) return;
    g_affinity = mask;
    g_cpuCount = count;
    DD2D9_LOG("cpus: the game runs on %d processor(s) (mask 0x%lx)", count, static_cast<unsigned long>(mask));
}

}  // namespace

void InstallCompatHooks(bool inLoaderLock) {
    if (inLoaderLock && g_config.cpus) SetAffinity();
    unsigned n = 0;
    if (g_config.ramMB) n += PatchGameModules(kMemoryHooks, sizeof(kMemoryHooks) / sizeof(kMemoryHooks[0]), inLoaderLock);
    if (g_config.cpuMHz) n += PatchGameModules(kCpuHooks, sizeof(kCpuHooks) / sizeof(kCpuHooks[0]), inLoaderLock);
    if (g_affinity) n += PatchGameModules(kCpusHooks, sizeof(kCpusHooks) / sizeof(kCpusHooks[0]), inLoaderLock);
    if (n) DD2D9_LOG("compat: %u imports patched (ram=%d cpu=%d cpus=%d)", n, g_config.ramMB, g_config.cpuMHz, g_config.cpus);
}

}  // namespace dd2d9
