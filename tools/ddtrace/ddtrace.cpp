// ddtrace: a proxy ddraw.dll that logs the game's DirectDraw (1-7) and Direct3D (3-7) calls
// and passes them on to the real ddraw.dll (Windows' own, or another one named in
// ddtrace.ini: target=). The log, ddtrace.log, is written next to the game. The real objects'
// method tables are patched in place (the objects the game gets are the real ones), so
// nothing is wrapped; Direct3D devices that change their tables are followed (Repatch).
//
// Logged: every call except the frequent ones (draws, states, locks, blits, AddRef/Release),
// which are logged for the first 20000 such calls, in one whole frame every 300 frames, and
// whenever they fail.
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <utility>

namespace {

struct Method {
    const char* name;
    int args;
};
struct IfaceInfo {
    const char* name;
    const Method* methods;
    int count;
};

// ------------------------------------------------------------------ log
CRITICAL_SECTION g_logLock;
FILE* g_log;
DWORD g_start;
bool g_tracingFrame = false;
unsigned g_frame = 0;
long g_hotLogged = 0;
bool g_sawEndScene = false;
__thread int t_depth = 0;

void Log(const char* fmt, ...) {
    EnterCriticalSection(&g_logLock);
    if (g_log) {
        fprintf(g_log, "[%6lu %04lx] %*s", GetTickCount() - g_start, GetCurrentThreadId(), t_depth > 1 ? 2 * (t_depth - 1) : 0, "");
        va_list a;
        va_start(a, fmt);
        vfprintf(g_log, fmt, a);
        va_end(a);
        fputc('\n', g_log);
        fflush(g_log);
    }
    LeaveCriticalSection(&g_logLock);
}

// ------------------------------------------------------------------ the real ddraw.dll
HMODULE g_real;
enum {
    kAcquireDDThreadLock, kCompleteCreateSysmemSurface, kD3DParseUnknownCommand, kDDGetAttachedSurfaceLcl,
    kDDInternalLock, kDDInternalUnlock, kDSoundHelp, kDirectDrawCreate, kDirectDrawCreateClipper, kDirectDrawCreateEx,
    kDirectDrawEnumerateA, kDirectDrawEnumerateExA, kDirectDrawEnumerateExW, kDirectDrawEnumerateW, kDllCanUnloadNow,
    kDllGetClassObject, kGetDDSurfaceLocal, kGetOLEThunkData, kGetSurfaceFromDC, kRegisterSpecialCase,
    kReleaseDDThreadLock, kSetAppCompatData, kCheckFullscreen, kExports
};
const char* const kExportNames[kExports] = {
    "AcquireDDThreadLock", "CompleteCreateSysmemSurface", "D3DParseUnknownCommand", "DDGetAttachedSurfaceLcl",
    "DDInternalLock", "DDInternalUnlock", "DSoundHelp", "DirectDrawCreate", "DirectDrawCreateClipper", "DirectDrawCreateEx",
    "DirectDrawEnumerateA", "DirectDrawEnumerateExA", "DirectDrawEnumerateExW", "DirectDrawEnumerateW", "DllCanUnloadNow",
    "DllGetClassObject", "GetDDSurfaceLocal", "GetOLEThunkData", "GetSurfaceFromDC", "RegisterSpecialCase",
    "ReleaseDDThreadLock", "SetAppCompatData", "CheckFullscreen"};

}  // namespace

extern "C" void* g_fn[kExports];
void* g_fn[kExports];

namespace {

void Dir(HMODULE m, char* out) {
    GetModuleFileNameA(m, out, MAX_PATH);
    if (char* s = strrchr(out, '\\')) s[1] = 0;
}

}  // namespace

extern "C" void __cdecl EnsureLoaded() {
    static volatile LONG done = 0;
    if (done) return;
    EnterCriticalSection(&g_logLock);
    if (!done) {
        char exeDir[MAX_PATH], path[MAX_PATH], target[MAX_PATH] = "";
        Dir(nullptr, exeDir);
        g_start = GetTickCount();
        snprintf(path, MAX_PATH, "%sddtrace.log", exeDir);
        g_log = fopen(path, "w");
        snprintf(path, MAX_PATH, "%sddtrace.ini", exeDir);
        GetPrivateProfileStringA("ddtrace", "target", "", target, MAX_PATH, path);
        char real[MAX_PATH];
        if (target[0]) {
            if (strchr(target, ':') || target[0] == '\\') snprintf(real, MAX_PATH, "%s", target);
            else snprintf(real, MAX_PATH, "%s%s", exeDir, target);
        } else {
            GetSystemDirectoryA(real, MAX_PATH);
            strncat(real, "\\ddraw.dll", MAX_PATH - strlen(real) - 1);
        }
        g_real = LoadLibraryA(real);
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        if (g_log) {
            fprintf(g_log, "ddtrace 1.6: %s\nreal ddraw: %s (%s)\n", exe, real, g_real ? "loaded" : "NOT LOADED");
            fflush(g_log);
        }
        for (int i = 0; i < kExports; i++) g_fn[i] = g_real ? reinterpret_cast<void*>(GetProcAddress(g_real, kExportNames[i])) : nullptr;
        done = 1;
    }
    LeaveCriticalSection(&g_logLock);
}

namespace {

// ------------------------------------------------------------------ method hooks
template <size_t>
struct Dw {
    using t = DWORD;
};
HRESULT Dispatch(int iface, int slot, void* self, DWORD* args);

template <int T, int S, typename Seq>
struct H;
template <int T, int S, size_t... I>
struct H<T, S, std::index_sequence<I...>> {
    static HRESULT __stdcall Fn(void* self, typename Dw<I>::t... a) {
        DWORD args[sizeof...(I) + 1] = {a..., 0};
        return Dispatch(T, S, self, args);
    }
};

#include "methods.inc"

// Each patched method table and the real methods it had (shared by every object of that
// class in the real ddraw.dll, or one object's own: see Repatch).
struct Patched {
    void** vt;
    int iface;
    void* orig[kMax];
};
Patched g_patched[256];
int g_patchedCount;

// Whether a method is one of ddtrace's own hooks. (A table copied from one already patched
// carries them: d3dim700 copies its device's table from the one the object points to,
// which may be a shared table patched before. The real method is then taken from that one,
// never the hook itself, which would call itself.)
bool IsOurs(const void* f) {
    static uintptr_t lo, hi;
    if (!lo) {
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&IsOurs), &m);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(m) + dos->e_lfanew);
        hi = reinterpret_cast<uintptr_t>(m) + nt->OptionalHeader.SizeOfImage;
        lo = reinterpret_cast<uintptr_t>(m);
    }
    const uintptr_t a = reinterpret_cast<uintptr_t>(f);
    return a >= lo && a < hi;
}
// The real method for a slot whose table holds a hook already: another table of that
// interface's (null: none known).
void* RealFromOthers(int iface, int slot);

Patched* FindPatched(void** vt) {
    for (int i = 0; i < g_patchedCount; i++)
        if (g_patched[i].vt == vt) return &g_patched[i];
    return nullptr;
}

// Puts the hooks back where a patched table's methods changed since. Direct3D 7's device
// (d3dim700.dll) copies its table into the object itself and rewrites its draw and state
// methods as its state changes; a device made where a released one was has the same
// table address with the real methods again. The changed ones become the real methods.
// Returns how many were put back. (g_logLock held.)
int Repatch(Patched& p) {
    const int n = kInfo[p.iface].count;
    int changed = 0;
    for (int s = 0; s < n; s++) changed += p.vt[s] != kHooks[p.iface][s];
    if (!changed) return 0;
    DWORD old;
    if (!VirtualProtect(p.vt, n * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return 0;
    for (int s = 0; s < n; s++)
        if (p.vt[s] != kHooks[p.iface][s]) {
            if (!IsOurs(p.vt[s])) p.orig[s] = p.vt[s];  // (a hook of ours: the real one stays)
            p.vt[s] = kHooks[p.iface][s];
        }
    VirtualProtect(p.vt, n * sizeof(void*), old, &old);
    return changed;
}

// Devices: watched at every call, their tables patched again when they change (above) or
// when the object switches to another table.
struct Tracked {
    void* obj;
    int iface;
};
Tracked g_tracked[64];
int g_trackedCount;
bool IsDevice(int i) { return i == kDev1 || i == kDev2 || i == kDev3 || i == kDev7; }
// A table whose first method is code in a loaded module (not freed memory reused).
bool LooksLikeTable(void** vt) {
    if (IsBadReadPtr(vt, sizeof(void*))) return false;
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery(vt[0], &mi, sizeof(mi)) && mi.Type == MEM_IMAGE &&
           (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}
void Track(void* obj, int iface) {
    EnterCriticalSection(&g_logLock);
    bool known = false;
    for (int i = 0; i < g_trackedCount; i++) known = known || g_tracked[i].obj == obj;
    if (!known && g_trackedCount < 64) g_tracked[g_trackedCount++] = {obj, iface};
    LeaveCriticalSection(&g_logLock);
}
void Untrack(void* obj) {
    EnterCriticalSection(&g_logLock);
    for (int i = 0; i < g_trackedCount; i++)
        if (g_tracked[i].obj == obj) g_tracked[i] = g_tracked[--g_trackedCount];
    LeaveCriticalSection(&g_logLock);
}
void Hook(void* obj, int iface);
long g_repatchLogged;
void CheckTracked() {
    EnterCriticalSection(&g_logLock);
    for (int i = 0; i < g_trackedCount; i++) {
        const Tracked t = g_tracked[i];
        if (IsBadReadPtr(t.obj, sizeof(void*))) continue;
        void** vt = *static_cast<void***>(t.obj);
        if (Patched* p = FindPatched(vt)) {
            if (const int n = Repatch(*p))
                if (++g_repatchLogged <= 50)
                    Log("(device %p: %d of its methods changed by the real ddraw, hooked again)", t.obj, n);
        } else if (LooksLikeTable(vt)) {
            Log("(device %p switched to method table %p)", t.obj, static_cast<void*>(vt));
            Hook(t.obj, t.iface);
        }
    }
    LeaveCriticalSection(&g_logLock);
}

void Hook(void* obj, int iface) {
    if (!obj) return;
    if (IsDevice(iface)) Track(obj, iface);
    void** vt = *static_cast<void***>(obj);
    EnterCriticalSection(&g_logLock);
    if (Patched* p = FindPatched(vt)) {
        if (const int n = Repatch(*p))
            Log("(method table %p of %p: %d methods real again, hooked again)", static_cast<void*>(vt), obj, n);
        LeaveCriticalSection(&g_logLock);
        return;
    }
    if (g_patchedCount < 256) {
        Patched& p = g_patched[g_patchedCount];
        p.vt = vt;
        p.iface = iface;
        const int n = kInfo[iface].count;
        DWORD old;
        if (VirtualProtect(vt, n * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            for (int s = 0; s < n; s++) {
                p.orig[s] = IsOurs(vt[s]) ? RealFromOthers(iface, s) : vt[s];
                vt[s] = kHooks[iface][s];
            }
            VirtualProtect(vt, n * sizeof(void*), old, &old);
            g_patchedCount++;
            Log("(method table %p patched as %s)", static_cast<void*>(vt), kInfo[iface].name);
            LeaveCriticalSection(&g_logLock);
            return;
        }
    }
    Log("(could not patch method table %p as %s)", static_cast<void*>(vt), kInfo[iface].name);
    LeaveCriticalSection(&g_logLock);
}

void* RealFromOthers(int iface, int slot) {
    for (int i = 0; i < g_patchedCount; i++)
        if (g_patched[i].iface == iface && g_patched[i].orig[slot] && !IsOurs(g_patched[i].orig[slot]))
            return g_patched[i].orig[slot];
    return nullptr;
}

// The real method behind a hooked call. (A table patched as another interface first — the
// real ddraw sharing one table between two versions — keeps that one's names.)
void* Orig(void* self, int slot) {
    const Patched* p = FindPatched(*static_cast<void***>(self));
    return p ? p->orig[slot] : nullptr;
}

HRESULT CallN(void* f, void* self, const DWORD* a, int n) {
    switch (n) {
    case 0: return reinterpret_cast<HRESULT(__stdcall*)(void*)>(f)(self);
    case 1: return reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD)>(f)(self, a[0]);
    case 2: return reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD)>(f)(self, a[0], a[1]);
    case 3: return reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD)>(f)(self, a[0], a[1], a[2]);
    case 4: return reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD)>(f)(self, a[0], a[1], a[2], a[3]);
    case 5:
        return reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD)>(f)(self, a[0], a[1], a[2], a[3],
                                                                                                   a[4]);
    case 6:
        return reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD)>(f)(self, a[0], a[1], a[2],
                                                                                                          a[3], a[4], a[5]);
    default:
        return reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD)>(f)(
            self, a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    }
}

// ------------------------------------------------------------------ decoding
const char* Name(int iface, int slot) { return kInfo[iface].methods[slot].name; }
bool Is(int iface, int slot, const char* n) { return !strcmp(Name(iface, slot), n); }
bool IsDD(int i) { return i == kDD1 || i == kDD2 || i == kDD4 || i == kDD7; }
bool IsSurf(int i) { return i >= kSurf1 && i <= kSurf7; }
int SurfFor(int dd) { return dd == kDD7 ? kSurf7 : dd == kDD4 ? kSurf4 : kSurf1; }

struct Buf {
    char s[2048];
    int n = 0;
    void Add(const char* fmt, ...) {
        if (n >= int(sizeof(s)) - 1) return;
        va_list a;
        va_start(a, fmt);
        const int k = vsnprintf(s + n, sizeof(s) - n, fmt, a);
        va_end(a);
        if (k > 0) n = n + k < int(sizeof(s)) ? n + k : int(sizeof(s)) - 1;
    }
};

bool Readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }

void AddPF(Buf& b, const DDPIXELFORMAT* pf) {
    if (!Readable(pf, sizeof(*pf))) return b.Add("pf(?)");
    char cc[5] = {};
    if (pf->dwFlags & DDPF_FOURCC) memcpy(cc, &pf->dwFourCC, 4);
    b.Add("pf(flags 0x%lx%s%s bits %lu r %lx g %lx b %lx a %lx)", pf->dwFlags, cc[0] ? " fourcc " : "", cc, pf->dwRGBBitCount,
          pf->dwRBitMask, pf->dwGBitMask, pf->dwBBitMask, pf->dwRGBAlphaBitMask);
}

void AddDesc(Buf& b, const void* p) {
    const DDSURFACEDESC2* d = static_cast<const DDSURFACEDESC2*>(p);
    if (!Readable(d, sizeof(DDSURFACEDESC))) return b.Add("desc(?)");
    b.Add("desc(size %lu flags 0x%lx %lux%lu pitch %ld back %lu mips %lu caps 0x%lx", d->dwSize, d->dwFlags, d->dwWidth,
          d->dwHeight, d->lPitch, d->dwBackBufferCount, d->dwMipMapCount, d->ddsCaps.dwCaps);
    if (d->dwSize >= sizeof(DDSURFACEDESC2)) b.Add(" caps2 0x%lx stage %lu", d->ddsCaps.dwCaps2, d->dwTextureStage);
    if (d->dwFlags & DDSD_CKSRCBLT) b.Add(" cksrc %lx-%lx", d->ddckCKSrcBlt.dwColorSpaceLowValue, d->ddckCKSrcBlt.dwColorSpaceHighValue);
    b.Add(" ");
    AddPF(b, &d->ddpfPixelFormat);
    b.Add(")");
}

void AddRect(Buf& b, const RECT* r) {
    if (!r) return b.Add("full");
    if (!Readable(r, sizeof(*r))) return b.Add("rect?");
    b.Add("%ld,%ld-%ld,%ld", r->left, r->top, r->right, r->bottom);
}

void AddDwords(Buf& b, const void* p, size_t bytes) {
    if (!Readable(p, bytes)) return b.Add("(?)");
    const DWORD* d = static_cast<const DWORD*>(p);
    for (size_t i = 0; i < bytes / 4; i++) b.Add("%s%lx", i ? " " : "", d[i]);
}

void AddGuid(Buf& b, const GUID* g) {
    if (!g) return b.Add("NULL");
    if (!Readable(g, sizeof(*g))) return b.Add("%p", static_cast<const void*>(g));
    b.Add("{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1],
          g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
}

struct IidName {
    const GUID* g;
    const char* n;
    int iface;
};
const IidName kIids[] = {
    {&IID_IDirectDraw, "IDirectDraw", kDD1},
    {&IID_IDirectDraw2, "IDirectDraw2", kDD2},
    {&IID_IDirectDraw4, "IDirectDraw4", kDD4},
    {&IID_IDirectDraw7, "IDirectDraw7", kDD7},
    {&IID_IDirectDrawSurface, "IDirectDrawSurface", kSurf1},
    {&IID_IDirectDrawSurface2, "IDirectDrawSurface2", kSurf2},
    {&IID_IDirectDrawSurface3, "IDirectDrawSurface3", kSurf3},
    {&IID_IDirectDrawSurface4, "IDirectDrawSurface4", kSurf4},
    {&IID_IDirectDrawSurface7, "IDirectDrawSurface7", kSurf7},
    {&IID_IDirect3D7, "IDirect3D7", kD3D7},
    {&IID_IDirectDrawGammaControl, "IDirectDrawGammaControl", kGamma},
    {&IID_IDirectDrawPalette, "IDirectDrawPalette", kPal},
    {&IID_IDirectDrawClipper, "IDirectDrawClipper", kClip},
    {&IID_IDirect3D, "IDirect3D", kD3D1},
    {&IID_IDirect3D2, "IDirect3D2", kD3D2},
    {&IID_IDirect3D3, "IDirect3D3", kD3D3},
    {&IID_IDirect3DHALDevice, "IID_IDirect3DHALDevice", kDev1},
    {&IID_IDirect3DTnLHalDevice, "IID_IDirect3DTnLHalDevice", kDev1},
    {&IID_IDirect3DRGBDevice, "IID_IDirect3DRGBDevice", kDev1},
    {&IID_IDirect3DRefDevice, "IID_IDirect3DRefDevice", kDev1},
    {&IID_IDirect3DMMXDevice, "IID_IDirect3DMMXDevice", kDev1},
    {&IID_IDirect3DTexture2, "IDirect3DTexture2", kTex2},
    {&IID_IDirect3DTexture, "IDirect3DTexture", kTex1},
    {&IID_IDirect3DRampDevice, "IID_IDirect3DRampDevice", kDev1},
    {&IID_IDirect3DDevice, "IDirect3DDevice", kDev1},
    {&IID_IDirect3DDevice2, "IDirect3DDevice2", kDev2},
    {&IID_IDirect3DDevice3, "IDirect3DDevice3", kDev3},
    {&IID_IDirect3DDevice7, "IDirect3DDevice7", kDev7},
    {&IID_IDirect3DViewport, "IDirect3DViewport", kVP1},
    {&IID_IDirect3DViewport2, "IDirect3DViewport2", kVP2},
    {&IID_IDirect3DViewport3, "IDirect3DViewport3", kVP3},
    {&IID_IDirect3DMaterial, "IDirect3DMaterial", kMat1},
    {&IID_IDirect3DMaterial2, "IDirect3DMaterial2", kMat2},
    {&IID_IDirect3DMaterial3, "IDirect3DMaterial3", kMat3},
    {&IID_IDirect3DLight, "IDirect3DLight", kLight},
    {&IID_IDirect3DVertexBuffer, "IDirect3DVertexBuffer", kVB1},
    {&IID_IDirect3DVertexBuffer7, "IDirect3DVertexBuffer7", kVB7},
    {&IID_IDirect3DExecuteBuffer, "IDirect3DExecuteBuffer", kXBuf},
    {&IID_IUnknown, "IUnknown", -1},
};
const IidName* FindIid(const GUID* g) {
    if (!Readable(g, sizeof(GUID))) return nullptr;
    for (const IidName& i : kIids)
        if (IsEqualGUID(*i.g, *g)) return &i;
    return nullptr;
}
void AddIid(Buf& b, const GUID* g) {
    if (const IidName* i = FindIid(g)) b.Add("%s", i->n);
    else AddGuid(b, g);
}

// ------------------------------------------------------------------ enumeration callbacks
// (Enumerations are synchronous: the game's callback and context kept per thread.)
__thread void* t_cb;
__thread void* t_ctx;
__thread int t_surfIface;

HRESULT CALLBACK EnumModes(LPDDSURFACEDESC2 d, LPVOID) {
    const HRESULT r = reinterpret_cast<LPDDENUMMODESCALLBACK2>(t_cb)(d, t_ctx);
    Buf b;
    b.Add("  mode ");
    AddDesc(b, d);
    b.Add(" refresh %lu -> %ld", Readable(d, sizeof(DDSURFACEDESC)) ? d->dwRefreshRate : 0, long(r));
    Log("%s", b.s);
    return r;
}
HRESULT CALLBACK EnumSurf(LPDIRECTDRAWSURFACE7 s, LPDDSURFACEDESC2 d, LPVOID) {
    Hook(s, t_surfIface);
    Buf b;
    b.Add("  surface %p ", static_cast<void*>(s));
    AddDesc(b, d);
    const HRESULT r = reinterpret_cast<LPDDENUMSURFACESCALLBACK7>(t_cb)(s, d, t_ctx);
    b.Add(" -> %ld", long(r));
    Log("%s", b.s);
    return r;
}
HRESULT CALLBACK EnumDev(LPSTR desc, LPSTR name, LPD3DDEVICEDESC7 d, LPVOID) {
    Buf b;
    b.Add("  device \"%s\" \"%s\" ", desc ? desc : "", name ? name : "");
    if (Readable(d, sizeof(*d))) AddGuid(b, &d->deviceGUID);
    const HRESULT r = reinterpret_cast<LPD3DENUMDEVICESCALLBACK7>(t_cb)(desc, name, d, t_ctx);
    b.Add(" -> %ld | ", long(r));
    AddDwords(b, d, sizeof(*d));
    Log("%s", b.s);
    return r;
}
HRESULT CALLBACK EnumPF(LPDDPIXELFORMAT pf, LPVOID) {
    const HRESULT r = reinterpret_cast<LPD3DENUMPIXELFORMATSCALLBACK>(t_cb)(pf, t_ctx);
    Buf b;
    b.Add("  ");
    AddPF(b, pf);
    b.Add(" -> %ld", long(r));
    Log("%s", b.s);
    return r;
}

HRESULT CALLBACK EnumDevOld(GUID* g, LPSTR desc, LPSTR name, LPD3DDEVICEDESC hw, LPD3DDEVICEDESC hel, LPVOID) {
    Buf b;
    b.Add("  device ");
    AddGuid(b, g);
    b.Add(" \"%s\" \"%s\"", desc ? desc : "", name ? name : "");
    const HRESULT r = reinterpret_cast<LPD3DENUMDEVICESCALLBACK>(t_cb)(g, desc, name, hw, hel, t_ctx);
    b.Add(" -> %ld | hw ", long(r));
    if (Readable(hw, 4)) AddDwords(b, hw, hw->dwSize < 512 ? hw->dwSize : 512);
    Log("%s", b.s);
    Buf c;
    c.Add("    hel ");
    if (Readable(hel, 4)) AddDwords(c, hel, hel->dwSize < 512 ? hel->dwSize : 512);
    Log("%s", c.s);
    return r;
}
HRESULT CALLBACK EnumTexOld(LPDDSURFACEDESC d, LPVOID) {
    const HRESULT r = reinterpret_cast<LPD3DENUMTEXTUREFORMATSCALLBACK>(t_cb)(d, t_ctx);
    Buf b;
    b.Add("  ");
    AddDesc(b, d);
    b.Add(" -> %ld", long(r));
    Log("%s", b.s);
    return r;
}

// ------------------------------------------------------------------ the dispatcher
bool Hot(int iface, int slot) {
    const char* n = Name(iface, slot);
    if (slot == 1 || slot == 2) return true;  // AddRef, Release
    if (iface == kDev7)
        return !strncmp(n, "Draw", 4) || !strcmp(n, "SetRenderState") || !strcmp(n, "GetRenderState") ||
               !strcmp(n, "SetTextureStageState") || !strcmp(n, "GetTextureStageState") || !strcmp(n, "SetTexture") ||
               !strcmp(n, "GetTexture") || !strcmp(n, "SetTransform") || !strcmp(n, "GetTransform") ||
               !strcmp(n, "MultiplyTransform") || !strcmp(n, "SetViewport") || !strcmp(n, "GetViewport") ||
               !strcmp(n, "SetMaterial") || !strcmp(n, "BeginScene") || !strcmp(n, "EndScene") || !strcmp(n, "SetLight") ||
               !strcmp(n, "LightEnable") || !strcmp(n, "Clear") || !strcmp(n, "ComputeSphereVisibility");
    if (IsSurf(iface))
        return !strcmp(n, "Lock") || !strcmp(n, "Unlock") || !strcmp(n, "Blt") || !strcmp(n, "BltFast") || !strcmp(n, "Flip") ||
               !strcmp(n, "IsLost") || !strcmp(n, "GetFlipStatus") || !strcmp(n, "GetBltStatus") ||
               !strcmp(n, "GetSurfaceDesc") || !strcmp(n, "GetDC") || !strcmp(n, "ReleaseDC") ||
               !strcmp(n, "GetAttachedSurface");
    if (iface == kDev1 || iface == kDev2 || iface == kDev3)
        return !strncmp(n, "Draw", 4) || !strcmp(n, "SetRenderState") || !strcmp(n, "GetRenderState") ||
               !strcmp(n, "SetLightState") || !strcmp(n, "GetLightState") || !strcmp(n, "SetTransform") ||
               !strcmp(n, "GetTransform") || !strcmp(n, "MultiplyTransform") || !strcmp(n, "Begin") ||
               !strcmp(n, "BeginIndexed") || !strcmp(n, "Vertex") || !strcmp(n, "Index") || !strcmp(n, "End") ||
               !strcmp(n, "BeginScene") || !strcmp(n, "EndScene") || !strcmp(n, "SetTexture") || !strcmp(n, "GetTexture") ||
               !strcmp(n, "SetTextureStageState") || !strcmp(n, "GetTextureStageState") || !strcmp(n, "Execute") ||
               !strcmp(n, "SetMatrix") || !strcmp(n, "GetMatrix") || !strcmp(n, "SetCurrentViewport") ||
               !strcmp(n, "ComputeSphereVisibility");
    if (iface == kVP1 || iface == kVP2 || iface == kVP3)
        return !strcmp(n, "Clear") || !strcmp(n, "Clear2") || !strcmp(n, "TransformVertices") || !strcmp(n, "SetViewport") ||
               !strcmp(n, "SetViewport2") || !strcmp(n, "GetViewport") || !strcmp(n, "GetViewport2");
    if (iface == kTex1 || iface == kTex2) return !strcmp(n, "GetHandle") || !strcmp(n, "Load");
    if (iface == kMat1 || iface == kMat2 || iface == kMat3) return !strcmp(n, "SetMaterial") || !strcmp(n, "GetHandle");
    if (iface == kLight) return !strcmp(n, "SetLight");
    if (iface == kXBuf) return !strcmp(n, "Lock") || !strcmp(n, "Unlock") || !strcmp(n, "SetExecuteData");
    if (iface == kVB7 || iface == kVB1) return !strcmp(n, "Lock") || !strcmp(n, "Unlock");
    if (IsDD(iface)) return !strcmp(n, "GetVerticalBlankStatus") || !strcmp(n, "GetScanLine") || !strcmp(n, "TestCooperativeLevel");
    return false;
}

long g_quietCount[kIfaces][kMax];

HRESULT Dispatch(int iface, int slot, void* self, DWORD* a) {
    if (g_trackedCount) CheckTracked();
    void* f = Orig(self, slot);
    // A table not patched by ddtrace that holds its hooks anyway: a copy of a patched one
    // (d3dim700 copies its device's table while making it, and calls through the copy before
    // the device reaches ddtrace). The real method: the one that table had.
    if (!f || IsOurs(f)) f = RealFromOthers(iface, slot);
    if (!f || IsOurs(f)) {
        Log("!! %s::%s on %p: no real method (table not known)", kInfo[iface].name, Name(iface, slot), self);
        return E_FAIL;
    }
    const int n = kInfo[iface].methods[slot].args;
    DWORD in[8];
    memcpy(in, a, sizeof(DWORD) * (n < 8 ? n : 8));

    // Enumeration callbacks replaced by logging ones. (The game's callback may enumerate
    // again, a surface's own attachments from inside: the outer one's kept and put back.)
    void* const savedCb = t_cb;
    void* const savedCtx = t_ctx;
    const int savedSurf = t_surfIface;
    if (IsDD(iface) && Is(iface, slot, "EnumDisplayModes")) {
        t_cb = reinterpret_cast<void*>(a[3]);
        t_ctx = reinterpret_cast<void*>(a[2]);
        if (a[3]) a[3] = reinterpret_cast<DWORD>(&EnumModes);  // (none: the call refuses it)
    } else if (IsDD(iface) && Is(iface, slot, "EnumSurfaces")) {
        t_cb = reinterpret_cast<void*>(a[3]);
        t_ctx = reinterpret_cast<void*>(a[2]);
        t_surfIface = SurfFor(iface);
        if (a[3]) a[3] = reinterpret_cast<DWORD>(&EnumSurf);  // (none: the call refuses it)
    } else if (IsSurf(iface) && Is(iface, slot, "EnumAttachedSurfaces")) {
        t_cb = reinterpret_cast<void*>(a[1]);
        t_ctx = reinterpret_cast<void*>(a[0]);
        t_surfIface = iface;
        if (a[1]) a[1] = reinterpret_cast<DWORD>(&EnumSurf);  // (none: the call refuses it)
    } else if (iface == kD3D7 && Is(iface, slot, "EnumDevices")) {
        t_cb = reinterpret_cast<void*>(a[0]);
        t_ctx = reinterpret_cast<void*>(a[1]);
        if (a[0]) a[0] = reinterpret_cast<DWORD>(&EnumDev);  // (none: the call refuses it)
    } else if (iface == kD3D7 && Is(iface, slot, "EnumZBufferFormats")) {
        t_cb = reinterpret_cast<void*>(a[1]);
        t_ctx = reinterpret_cast<void*>(a[2]);
        if (a[1]) a[1] = reinterpret_cast<DWORD>(&EnumPF);  // (none: the call refuses it)
    } else if ((iface == kD3D1 || iface == kD3D2 || iface == kD3D3) && Is(iface, slot, "EnumDevices")) {
        t_cb = reinterpret_cast<void*>(a[0]);
        t_ctx = reinterpret_cast<void*>(a[1]);
        if (a[0]) a[0] = reinterpret_cast<DWORD>(&EnumDevOld);  // (none: the call refuses it)
    } else if (iface == kD3D3 && Is(iface, slot, "EnumZBufferFormats")) {
        t_cb = reinterpret_cast<void*>(a[1]);
        t_ctx = reinterpret_cast<void*>(a[2]);
        if (a[1]) a[1] = reinterpret_cast<DWORD>(&EnumPF);  // (none: the call refuses it)
    } else if ((iface == kDev1 || iface == kDev2) && Is(iface, slot, "EnumTextureFormats")) {
        t_cb = reinterpret_cast<void*>(a[0]);
        t_ctx = reinterpret_cast<void*>(a[1]);
        if (a[0]) a[0] = reinterpret_cast<DWORD>(&EnumTexOld);  // (none: the call refuses it)
    } else if ((iface == kDev7 || iface == kDev3) && Is(iface, slot, "EnumTextureFormats")) {
        t_cb = reinterpret_cast<void*>(a[0]);
        t_ctx = reinterpret_cast<void*>(a[1]);
        if (a[0]) a[0] = reinterpret_cast<DWORD>(&EnumPF);  // (none: the call refuses it)
    }

    const bool hot = Hot(iface, slot);
    // Before the call: what goes in (pointed-to data that the call may change).
    Buf pre;
    if (IsSurf(iface) && Is(iface, slot, "Blt")) {
        AddRect(pre, reinterpret_cast<RECT*>(in[0]));
        pre.Add(" <- %p ", reinterpret_cast<void*>(in[1]));
        AddRect(pre, reinterpret_cast<RECT*>(in[2]));
        const DDBLTFX* fx = reinterpret_cast<DDBLTFX*>(in[4]);
        if (Readable(fx, sizeof(*fx))) pre.Add(" fx(ddfx 0x%lx rop 0x%lx fill 0x%lx)", fx->dwDDFX, fx->dwROP, fx->dwFillColor);
    } else if (IsSurf(iface) && Is(iface, slot, "BltFast")) {
        pre.Add("%lu,%lu <- %p ", in[0], in[1], reinterpret_cast<void*>(in[2]));
        AddRect(pre, reinterpret_cast<RECT*>(in[3]));
    } else if ((IsSurf(iface) && Is(iface, slot, "Lock"))) {
        AddRect(pre, reinterpret_cast<RECT*>(in[0]));
    } else if (IsDD(iface) && (Is(iface, slot, "CreateSurface"))) {
        AddDesc(pre, reinterpret_cast<void*>(in[0]));
    } else if ((iface == kDev7 || iface == kDev3 || iface == kDev2) &&
               (Is(iface, slot, "DrawPrimitive") || Is(iface, slot, "DrawIndexedPrimitive"))) {
        // The first vertex. Transformed: x y z rhw, then its colours and first texture
        // coordinates as its FVF has them (Direct3D 5's D3DVT_TLVERTEX: XYZRHW | DIFFUSE |
        // SPECULAR | TEX1); otherwise x y z and the next five words raw.
        const DWORD fvf = iface == kDev2 ? (in[1] == D3DVT_TLVERTEX ? DWORD(0x1C4) : DWORD(D3DFVF_XYZ)) : in[1];
        const DWORD* v = reinterpret_cast<DWORD*>(in[2]);
        if (Readable(v, 32)) {
            const float* fl = reinterpret_cast<const float*>(v);
            if ((fvf & 0x0E) == D3DFVF_XYZRHW) {
                pre.Add("v0(%.2f %.2f %.5f %.5f", fl[0], fl[1], fl[2], fl[3]);
                int k = 4;
                if (fvf & D3DFVF_DIFFUSE) pre.Add(" diffuse %08lx", v[k++]);
                if (fvf & D3DFVF_SPECULAR) pre.Add(" specular %08lx", v[k++]);
                if ((fvf & D3DFVF_TEXCOUNT_MASK) && k + 2 <= 8) pre.Add(" uv %.3f %.3f", fl[k], fl[k + 1]);
                pre.Add(")");
            } else {
                pre.Add("v0(%.2f %.2f %.2f | %lx %lx %lx %lx %lx)", fl[0], fl[1], fl[2], v[3], v[4], v[5], v[6], v[7]);
            }
        }
    } else if ((iface == kDev7 || iface == kDev3 || iface == kDev2) &&
               (Is(iface, slot, "SetTransform") || Is(iface, slot, "MultiplyTransform"))) {
        AddDwords(pre, reinterpret_cast<void*>(in[1]), 64);
    } else if (iface == kDev7 && Is(iface, slot, "SetViewport")) {
        AddDwords(pre, reinterpret_cast<void*>(in[0]), sizeof(D3DVIEWPORT7));
    } else if ((iface == kVP1 || iface == kVP2 || iface == kVP3) &&
               (Is(iface, slot, "SetViewport") || Is(iface, slot, "SetViewport2"))) {
        const DWORD* vp = reinterpret_cast<DWORD*>(in[0]);
        if (Readable(vp, 4)) AddDwords(pre, vp, vp[0] < 64 ? vp[0] : 64);
    } else if ((iface == kVP1 || iface == kVP2 || iface == kVP3) && Is(iface, slot, "Clear")) {
        if (in[0] && Readable(reinterpret_cast<void*>(in[1]), 16)) AddRect(pre, reinterpret_cast<RECT*>(in[1]));
    } else if (iface == kVP3 && Is(iface, slot, "Clear2")) {
        if (in[0] && Readable(reinterpret_cast<void*>(in[1]), 16)) AddRect(pre, reinterpret_cast<RECT*>(in[1]));
    } else if ((iface == kMat1 || iface == kMat2 || iface == kMat3) && Is(iface, slot, "SetMaterial")) {
        const DWORD* m = reinterpret_cast<DWORD*>(in[0]);
        if (Readable(m, 4)) AddDwords(pre, m, m[0] < 128 ? m[0] : 128);
    } else if (iface == kLight && Is(iface, slot, "SetLight")) {
        const DWORD* m = reinterpret_cast<DWORD*>(in[0]);
        if (Readable(m, 4)) AddDwords(pre, m, m[0] < 128 ? m[0] : 128);
    } else if ((iface == kDev2 || iface == kDev3) && Is(iface, slot, "SetRenderTarget")) {
        pre.Add("%p", reinterpret_cast<void*>(in[0]));
    } else if (iface == kDev1 && Is(iface, slot, "SetMatrix")) {
        AddDwords(pre, reinterpret_cast<void*>(in[1]), 64);
    } else if (iface == kXBuf && Is(iface, slot, "SetExecuteData")) {
        const DWORD* m = reinterpret_cast<DWORD*>(in[0]);
        if (Readable(m, 4)) AddDwords(pre, m, m[0] < 64 ? m[0] : 64);
    } else if ((iface == kD3D2 || iface == kD3D3) && Is(iface, slot, "CreateDevice")) {
        AddIid(pre, reinterpret_cast<GUID*>(in[0]));
    } else if (iface == kD3D3 && Is(iface, slot, "CreateVertexBuffer")) {
        AddDwords(pre, reinterpret_cast<void*>(in[0]), sizeof(D3DVERTEXBUFFERDESC));
    } else if (iface == kDev7 && Is(iface, slot, "SetMaterial")) {
        AddDwords(pre, reinterpret_cast<void*>(in[0]), sizeof(D3DMATERIAL7));
    } else if (slot == 0) {
        AddIid(pre, reinterpret_cast<GUID*>(in[0]));
    } else if (iface == kD3D7 && Is(iface, slot, "CreateDevice")) {
        AddIid(pre, reinterpret_cast<GUID*>(in[0]));
    } else if (iface == kD3D7 && Is(iface, slot, "CreateVertexBuffer")) {
        AddDwords(pre, reinterpret_cast<void*>(in[0]), sizeof(D3DVERTEXBUFFERDESC));
    } else if (iface == kDev7 && Is(iface, slot, "Clear")) {
        if (in[0] && Readable(reinterpret_cast<void*>(in[1]), 16)) AddRect(pre, reinterpret_cast<RECT*>(in[1]));
    } else if (iface == kPal && Is(iface, slot, "SetEntries")) {
        AddDwords(pre, reinterpret_cast<void*>(in[3]), 4 * (in[2] < 4 ? in[2] : 4));
    }

    t_depth++;
    const HRESULT hr = CallN(f, self, a, n);
    t_depth--;
    t_cb = savedCb;
    t_ctx = savedCtx;
    t_surfIface = savedSurf;

    // Objects handed out: their method tables patched too.
    if (SUCCEEDED(hr)) {
        void** out = nullptr;
        int type = -1;
        if (slot == 0) {
            if (const IidName* i = FindIid(reinterpret_cast<GUID*>(in[0]))) type = i->iface;
            out = reinterpret_cast<void**>(in[1]);
        } else if (IsDD(iface)) {
            if (Is(iface, slot, "CreateSurface")) type = SurfFor(iface), out = reinterpret_cast<void**>(in[1]);
            else if (Is(iface, slot, "DuplicateSurface")) type = SurfFor(iface), out = reinterpret_cast<void**>(in[1]);
            else if (Is(iface, slot, "GetGDISurface")) type = SurfFor(iface), out = reinterpret_cast<void**>(in[0]);
            else if (Is(iface, slot, "GetSurfaceFromDC")) type = SurfFor(iface), out = reinterpret_cast<void**>(in[1]);
            else if (Is(iface, slot, "CreatePalette")) type = kPal, out = reinterpret_cast<void**>(in[2]);
            else if (Is(iface, slot, "CreateClipper")) type = kClip, out = reinterpret_cast<void**>(in[1]);
        } else if (IsSurf(iface)) {
            if (Is(iface, slot, "GetAttachedSurface")) type = iface, out = reinterpret_cast<void**>(in[1]);
            else if (Is(iface, slot, "GetPalette")) type = kPal, out = reinterpret_cast<void**>(in[0]);
            else if (Is(iface, slot, "GetClipper")) type = kClip, out = reinterpret_cast<void**>(in[0]);
        } else if (iface == kD3D7) {
            if (Is(iface, slot, "CreateDevice")) type = kDev7, out = reinterpret_cast<void**>(in[2]);
            else if (Is(iface, slot, "CreateVertexBuffer")) type = kVB7, out = reinterpret_cast<void**>(in[1]);
        } else if (iface == kD3D1 || iface == kD3D2 || iface == kD3D3) {
            const int v = iface == kD3D1 ? 0 : iface == kD3D2 ? 1 : 2;
            if (Is(iface, slot, "CreateLight")) type = kLight, out = reinterpret_cast<void**>(in[0]);
            else if (Is(iface, slot, "CreateMaterial")) type = kMat1 + v, out = reinterpret_cast<void**>(in[0]);
            else if (Is(iface, slot, "CreateViewport")) type = kVP1 + v, out = reinterpret_cast<void**>(in[0]);
            else if (Is(iface, slot, "CreateDevice")) type = iface == kD3D2 ? kDev2 : kDev3, out = reinterpret_cast<void**>(in[2]);
            else if (Is(iface, slot, "CreateVertexBuffer")) type = kVB1, out = reinterpret_cast<void**>(in[1]);
        } else if (iface == kDev1 || iface == kDev2 || iface == kDev3) {
            const int v = iface == kDev1 ? 0 : iface == kDev2 ? 1 : 2;
            if (Is(iface, slot, "CreateExecuteBuffer")) type = kXBuf, out = reinterpret_cast<void**>(in[1]);
            else if (Is(iface, slot, "GetDirect3D")) type = kD3D1 + v, out = reinterpret_cast<void**>(in[0]);
            else if (Is(iface, slot, "NextViewport")) type = kVP1 + v, out = reinterpret_cast<void**>(in[1]);
            else if (Is(iface, slot, "GetCurrentViewport")) type = kVP1 + v, out = reinterpret_cast<void**>(in[0]);
            else if (Is(iface, slot, "GetRenderTarget")) type = iface == kDev2 ? kSurf1 : kSurf4, out = reinterpret_cast<void**>(in[0]);
            else if (Is(iface, slot, "GetTexture")) type = kTex2, out = reinterpret_cast<void**>(in[1]);
        } else if (iface == kVP1 || iface == kVP2 || iface == kVP3) {
            if (Is(iface, slot, "NextLight")) type = kLight, out = reinterpret_cast<void**>(in[1]);
        } else if (iface == kDev7) {
            if (Is(iface, slot, "GetDirect3D")) type = kD3D7, out = reinterpret_cast<void**>(in[0]);
            else if (Is(iface, slot, "GetRenderTarget")) type = kSurf7, out = reinterpret_cast<void**>(in[0]);
            else if (Is(iface, slot, "GetTexture")) type = kSurf7, out = reinterpret_cast<void**>(in[1]);
        }
        if (type >= 0 && out && Readable(out, 4) && *out) Hook(*out, type);
    }

    if (slot == 2 && hr == 0 && IsDevice(iface)) Untrack(self);  // (the last Release)

    // Frames: counted at EndScene (or Flip, for games without Direct3D).
    bool frameEnd = false;
    if ((iface == kDev7 || iface == kDev3 || iface == kDev2 || iface == kDev1) && Is(iface, slot, "EndScene"))
        g_sawEndScene = true, frameEnd = true;
    if (IsSurf(iface) && Is(iface, slot, "Flip") && !g_sawEndScene) frameEnd = true;

    bool log;
    if (hot) {
        log = g_tracingFrame || FAILED(hr) || g_hotLogged < 20000;
        if (log && !g_tracingFrame && SUCCEEDED(hr)) g_hotLogged++;
        if (FAILED(hr) && !g_tracingFrame && g_hotLogged >= 20000 && ++g_quietCount[iface][slot] > 200) log = false;
    } else {
        log = ++g_quietCount[iface][slot] <= 5000 || FAILED(hr);
    }
    if (t_depth > 0 && (slot == 1 || slot == 2)) log = false;  // the real ddraw's own reference counting

    if (log) {
        Buf b;
        if (t_depth > 0) b.Add("(inside) ");
        b.Add("%s::%s(%p", kInfo[iface].name, Name(iface, slot), self);
        for (int i = 0; i < n; i++) b.Add(", %lx", in[i]);
        b.Add(") = ");
        if (slot == 1 || slot == 2) b.Add("%lu", static_cast<unsigned long>(hr));
        else b.Add("0x%08lx", static_cast<unsigned long>(hr));
        if (pre.n) b.Add(" | %s", pre.s);
        // After the call: what came out.
        if (SUCCEEDED(hr)) {
            if (IsSurf(iface) && Is(iface, slot, "Lock")) {
                b.Add(" | ");
                AddDesc(b, reinterpret_cast<void*>(in[1]));
                const DDSURFACEDESC2* d = reinterpret_cast<DDSURFACEDESC2*>(in[1]);
                if (Readable(d, sizeof(DDSURFACEDESC))) b.Add(" ptr %p", d->lpSurface);
            } else if ((IsSurf(iface) && Is(iface, slot, "GetSurfaceDesc")) || (IsDD(iface) && Is(iface, slot, "GetDisplayMode")) ||
                       (IsDD(iface) && Is(iface, slot, "CreateSurface"))) {
                b.Add(" | ");
                if (!Is(iface, slot, "CreateSurface")) AddDesc(b, reinterpret_cast<void*>(in[0]));
                else b.Add("-> %p", Readable(reinterpret_cast<void*>(in[1]), 4) ? *reinterpret_cast<void**>(in[1]) : nullptr);
            } else if (IsSurf(iface) && Is(iface, slot, "GetPixelFormat")) {
                b.Add(" | ");
                AddPF(b, reinterpret_cast<DDPIXELFORMAT*>(in[0]));
            } else if (IsSurf(iface) && Is(iface, slot, "GetCaps")) {
                b.Add(" | ");
                AddDwords(b, reinterpret_cast<void*>(in[0]), iface == kSurf7 || iface == kSurf4 ? 16 : 4);
            } else if (IsSurf(iface) && (Is(iface, slot, "GetAttachedSurface"))) {
                b.Add(" | caps ");
                AddDwords(b, reinterpret_cast<void*>(in[0]), iface == kSurf7 || iface == kSurf4 ? 16 : 4);
                b.Add(" -> %p", Readable(reinterpret_cast<void*>(in[1]), 4) ? *reinterpret_cast<void**>(in[1]) : nullptr);
            } else if (IsSurf(iface) && Is(iface, slot, "GetColorKey")) {
                b.Add(" | ");
                AddDwords(b, reinterpret_cast<void*>(in[1]), 8);
            } else if (IsDD(iface) && Is(iface, slot, "GetCaps")) {
                for (int k = 0; k < 2; k++) {
                    const DWORD* c = reinterpret_cast<DWORD*>(in[k]);
                    if (Readable(c, 4) && Readable(c, *c)) {
                        b.Add(k ? " | hel " : " | drv ");
                        AddDwords(b, c, *c);
                    }
                }
            } else if (IsDD(iface) && Is(iface, slot, "GetAvailableVidMem")) {
                b.Add(" | caps ");
                AddDwords(b, reinterpret_cast<void*>(in[0]), iface == kDD7 || iface == kDD4 ? 16 : 4);
                b.Add(" total %lu free %lu", Readable(reinterpret_cast<void*>(in[1]), 4) ? *reinterpret_cast<DWORD*>(in[1]) : 0,
                      Readable(reinterpret_cast<void*>(in[2]), 4) ? *reinterpret_cast<DWORD*>(in[2]) : 0);
            } else if (IsDD(iface) && Is(iface, slot, "GetDeviceIdentifier")) {
                const DDDEVICEIDENTIFIER2* id = reinterpret_cast<DDDEVICEIDENTIFIER2*>(in[0]);
                if (Readable(id, sizeof(DDDEVICEIDENTIFIER)))
                    b.Add(" | \"%.512s\" \"%.512s\" vendor %lx device %lx", id->szDriver, id->szDescription, id->dwVendorId,
                          id->dwDeviceId);
            } else if (IsDD(iface) && Is(iface, slot, "GetFourCCCodes")) {
                const DWORD* cnt = reinterpret_cast<DWORD*>(in[0]);
                if (Readable(cnt, 4)) {
                    b.Add(" | %lu", *cnt);
                    const char* codes = reinterpret_cast<const char*>(in[1]);
                    if (Readable(codes, 4 * *cnt))
                        for (DWORD k = 0; k < *cnt && k < 32; k++) b.Add(" %.4s", codes + 4 * k);
                }
            } else if (iface == kDev7 && Is(iface, slot, "GetCaps")) {
                b.Add(" | ");
                AddDwords(b, reinterpret_cast<void*>(in[0]), sizeof(D3DDEVICEDESC7));
            } else if ((iface == kDev1 || iface == kDev2 || iface == kDev3) && Is(iface, slot, "GetCaps")) {
                for (int k = 0; k < 2; k++) {
                    const DWORD* c = reinterpret_cast<DWORD*>(in[k]);
                    if (Readable(c, 4) && Readable(c, *c)) {
                        b.Add(k ? " | hel " : " | hw ");
                        AddDwords(b, c, *c < 512 ? *c : 512);
                    }
                }
            } else if ((iface == kTex1 || iface == kTex2 || iface == kMat1 || iface == kMat2 || iface == kMat3) &&
                       Is(iface, slot, "GetHandle")) {
                b.Add(" | handle %lx", Readable(reinterpret_cast<void*>(in[1]), 4) ? *reinterpret_cast<DWORD*>(in[1]) : 0);
            } else if (iface == kD3D1 && Is(iface, slot, "FindDevice")) {
                b.Add(" | ");
                AddDwords(b, reinterpret_cast<void*>(in[1]), 32);
            } else if ((iface == kDev2 || iface == kDev3) && (Is(iface, slot, "GetRenderState") || Is(iface, slot, "GetLightState"))) {
                const DWORD* v = reinterpret_cast<DWORD*>(in[1]);
                b.Add(" | %lx", Readable(v, 4) ? *v : 0);
            } else if (iface == kDev3 && Is(iface, slot, "ValidateDevice")) {
                b.Add(" | passes %lu", Readable(reinterpret_cast<void*>(in[0]), 4) ? *reinterpret_cast<DWORD*>(in[0]) : 0);
            } else if (iface == kXBuf && Is(iface, slot, "Lock")) {
                b.Add(" | ");
                AddDwords(b, reinterpret_cast<void*>(in[0]), 24);
            } else if (iface == kDev7 && Is(iface, slot, "ValidateDevice")) {
                b.Add(" | passes %lu", Readable(reinterpret_cast<void*>(in[0]), 4) ? *reinterpret_cast<DWORD*>(in[0]) : 0);
            } else if (iface == kDev7 && (Is(iface, slot, "GetRenderState") || Is(iface, slot, "GetTextureStageState"))) {
                const DWORD* v = reinterpret_cast<DWORD*>(in[Is(iface, slot, "GetRenderState") ? 1 : 2]);
                b.Add(" | %lx", Readable(v, 4) ? *v : 0);
            } else if (iface == kDev7 && (Is(iface, slot, "GetViewport"))) {
                b.Add(" | ");
                AddDwords(b, reinterpret_cast<void*>(in[0]), sizeof(D3DVIEWPORT7));
            } else if (iface == kDev7 && Is(iface, slot, "GetInfo")) {
                b.Add(" | ");
                AddDwords(b, reinterpret_cast<void*>(in[1]), in[2] < 64 ? in[2] : 64);
            } else if (iface == kVB7 && Is(iface, slot, "GetVertexBufferDesc")) {
                b.Add(" | ");
                AddDwords(b, reinterpret_cast<void*>(in[0]), sizeof(D3DVERTEXBUFFERDESC));
            } else if (iface == kPal && Is(iface, slot, "GetEntries")) {
                b.Add(" | ");
                AddDwords(b, reinterpret_cast<void*>(in[3]), 4 * (in[2] < 4 ? in[2] : 4));
            }
        }
        Log("%s", b.s);
    }
    if (frameEnd && t_depth == 0) {
        g_frame++;
        const bool was = g_tracingFrame;
        g_tracingFrame = g_frame % 300 == 150;
        if (g_tracingFrame) Log("==== frame %u: every call logged ====", g_frame);
        else if (was) Log("==== end of frame %u ====", g_frame - 1);
    }
    return hr;
}

}  // namespace

// ------------------------------------------------------------------ exports
#define PASS(name)                                                                \
    extern "C" __attribute__((naked)) void T_##name() {                           \
        asm volatile("pushal\n call _EnsureLoaded\n popal\n jmp *(_g_fn + 4 * %c0)" \
                     :                                                            \
                     : "i"(k##name));                                             \
    }
PASS(AcquireDDThreadLock)
PASS(CompleteCreateSysmemSurface)
PASS(D3DParseUnknownCommand)
PASS(DDGetAttachedSurfaceLcl)
PASS(DDInternalLock)
PASS(DDInternalUnlock)
PASS(DSoundHelp)
PASS(DirectDrawCreateClipper)
PASS(DllCanUnloadNow)
PASS(DllGetClassObject)
PASS(GetDDSurfaceLocal)
PASS(GetOLEThunkData)
PASS(GetSurfaceFromDC)
PASS(RegisterSpecialCase)
PASS(ReleaseDDThreadLock)
PASS(SetAppCompatData)
PASS(CheckFullscreen)

namespace {
__thread void* t_ecb;
__thread void* t_ectx;
BOOL CALLBACK EnumA(GUID* g, LPSTR desc, LPSTR name, LPVOID) {
    Buf b;
    b.Add("  ");
    AddGuid(b, g);
    const BOOL r = reinterpret_cast<LPDDENUMCALLBACKA>(t_ecb)(g, desc, name, t_ectx);
    b.Add(" \"%s\" \"%s\" -> %d", desc ? desc : "", name ? name : "", r);
    Log("%s", b.s);
    return r;
}
BOOL CALLBACK EnumExA(GUID* g, LPSTR desc, LPSTR name, LPVOID, HMONITOR m) {
    Buf b;
    b.Add("  ");
    AddGuid(b, g);
    const BOOL r = reinterpret_cast<LPDDENUMCALLBACKEXA>(t_ecb)(g, desc, name, t_ectx, m);
    b.Add(" \"%s\" \"%s\" monitor %p -> %d", desc ? desc : "", name ? name : "", static_cast<void*>(m), r);
    Log("%s", b.s);
    return r;
}
}  // namespace

extern "C" HRESULT WINAPI T_DirectDrawCreate(GUID* g, LPDIRECTDRAW* out, IUnknown* outer) {
    EnsureLoaded();
    auto f = reinterpret_cast<HRESULT(WINAPI*)(GUID*, LPDIRECTDRAW*, IUnknown*)>(g_fn[kDirectDrawCreate]);
    const HRESULT hr = f ? f(g, out, outer) : E_FAIL;
    Buf b;
    b.Add("DirectDrawCreate(");
    AddGuid(b, g);
    b.Add(") = 0x%08lx -> %p", static_cast<unsigned long>(hr), SUCCEEDED(hr) && out ? static_cast<void*>(*out) : nullptr);
    Log("%s", b.s);
    if (SUCCEEDED(hr) && out) Hook(*out, kDD1);
    return hr;
}

extern "C" HRESULT WINAPI T_DirectDrawCreateEx(GUID* g, void** out, REFIID iid, IUnknown* outer) {
    EnsureLoaded();
    auto f = reinterpret_cast<HRESULT(WINAPI*)(GUID*, void**, REFIID, IUnknown*)>(g_fn[kDirectDrawCreateEx]);
    const HRESULT hr = f ? f(g, out, iid, outer) : E_FAIL;
    Buf b;
    b.Add("DirectDrawCreateEx(");
    AddGuid(b, g);
    b.Add(", ");
    AddIid(b, &iid);
    b.Add(") = 0x%08lx -> %p", static_cast<unsigned long>(hr), SUCCEEDED(hr) && out ? *out : nullptr);
    Log("%s", b.s);
    if (SUCCEEDED(hr) && out) Hook(*out, kDD7);
    return hr;
}

extern "C" HRESULT WINAPI T_DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx) {
    EnsureLoaded();
    Log("DirectDrawEnumerateA:");
    t_ecb = reinterpret_cast<void*>(cb);
    t_ectx = ctx;
    auto f = reinterpret_cast<HRESULT(WINAPI*)(LPDDENUMCALLBACKA, LPVOID)>(g_fn[kDirectDrawEnumerateA]);
    const HRESULT hr = f ? f(cb ? EnumA : nullptr, ctx) : E_FAIL;
    Log("DirectDrawEnumerateA = 0x%08lx", static_cast<unsigned long>(hr));
    return hr;
}

extern "C" HRESULT WINAPI T_DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, LPVOID ctx, DWORD flags) {
    EnsureLoaded();
    Log("DirectDrawEnumerateExA(flags 0x%lx):", flags);
    t_ecb = reinterpret_cast<void*>(cb);
    t_ectx = ctx;
    auto f = reinterpret_cast<HRESULT(WINAPI*)(LPDDENUMCALLBACKEXA, LPVOID, DWORD)>(g_fn[kDirectDrawEnumerateExA]);
    const HRESULT hr = f ? f(cb ? EnumExA : nullptr, ctx, flags) : E_FAIL;
    Log("DirectDrawEnumerateExA = 0x%08lx", static_cast<unsigned long>(hr));
    return hr;
}

extern "C" HRESULT WINAPI T_DirectDrawEnumerateW(void* cb, LPVOID ctx) {
    EnsureLoaded();
    auto f = reinterpret_cast<HRESULT(WINAPI*)(void*, LPVOID)>(g_fn[kDirectDrawEnumerateW]);
    const HRESULT hr = f ? f(cb, ctx) : E_FAIL;
    Log("DirectDrawEnumerateW = 0x%08lx (not itemized)", static_cast<unsigned long>(hr));
    return hr;
}

extern "C" HRESULT WINAPI T_DirectDrawEnumerateExW(void* cb, LPVOID ctx, DWORD flags) {
    EnsureLoaded();
    auto f = reinterpret_cast<HRESULT(WINAPI*)(void*, LPVOID, DWORD)>(g_fn[kDirectDrawEnumerateExW]);
    const HRESULT hr = f ? f(cb, ctx, flags) : E_FAIL;
    Log("DirectDrawEnumerateExW(flags 0x%lx) = 0x%08lx (not itemized)", flags, static_cast<unsigned long>(hr));
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_logLock);
    return TRUE;
}
