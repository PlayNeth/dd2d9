// dfshim.dll: a Direct3D 9 without INTZ but with AMD's DF24 and DF16 depth textures, as older
// Radeons and Intel's GPUs have them, to run dd2d9's path for those (Wine's Direct3D has INTZ
// alone). Copied as d3d9.dll beside the program, with d3d9=game in dd2d9.ini (and Wine's
// d3d9=n,b), it loads the system's d3d9.dll and patches its objects: INTZ and RESZ aren't
// there, and a DF24 or DF16 depth texture is made as INTZ. At the end it adds the depth
// textures it made to dfshim.txt beside it.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>

namespace {
char g_dir[MAX_PATH];
LONG g_made24, g_made16;

constexpr D3DFORMAT kIntz = D3DFORMAT(MAKEFOURCC('I', 'N', 'T', 'Z'));
constexpr D3DFORMAT kResz = D3DFORMAT(MAKEFOURCC('R', 'E', 'S', 'Z'));
constexpr D3DFORMAT kDf24 = D3DFORMAT(MAKEFOURCC('D', 'F', '2', '4'));
constexpr D3DFORMAT kDf16 = D3DFORMAT(MAKEFOURCC('D', 'F', '1', '6'));
// The methods' places in the vtables: IDirect3D9's CheckDeviceFormat and CreateDevice,
// IDirect3DDevice9's CreateTexture.
constexpr int kCheckDeviceFormat = 10, kCreateDevice = 16, kCreateTexture = 23;

using CheckFn = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT);
using CreateDeviceFn = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                        IDirect3DDevice9**);
using CreateTextureFn = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                         IDirect3DTexture9**, HANDLE*);
CheckFn g_check;
CreateDeviceFn g_createDevice;
CreateTextureFn g_createTexture;

HRESULT WINAPI CheckDeviceFormat(IDirect3D9* d, UINT adapter, D3DDEVTYPE type, D3DFORMAT display, DWORD usage,
                                 D3DRESOURCETYPE rtype, D3DFORMAT f) {
    if (f == kIntz || f == kResz) return D3DERR_NOTAVAILABLE;
    if (f == kDf24 || f == kDf16) {
        if (!(usage & D3DUSAGE_DEPTHSTENCIL) || rtype != D3DRTYPE_TEXTURE) return D3DERR_NOTAVAILABLE;
        f = kIntz;
    }
    return g_check(d, adapter, type, display, usage, rtype, f);
}

HRESULT WINAPI CreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT f, D3DPOOL pool,
                             IDirect3DTexture9** out, HANDLE* shared) {
    if (f == kIntz) return D3DERR_INVALIDCALL;
    if (f == kDf24 || f == kDf16) {
        if (!(usage & D3DUSAGE_DEPTHSTENCIL)) return D3DERR_INVALIDCALL;
        InterlockedIncrement(f == kDf24 ? &g_made24 : &g_made16);
        f = kIntz;
    }
    return g_createTexture(d, w, h, levels, usage, f, pool, out, shared);
}

// A vtable entry replaced (once: the vtable is shared by the objects of its kind).
template <typename Fn>
void Patch(void* object, int slot, Fn fn, Fn* old) {
    void** entry = *static_cast<void***>(object) + slot;
    if (*entry == reinterpret_cast<void*>(fn)) return;
    DWORD prot;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &prot)) return;
    *old = reinterpret_cast<Fn>(*entry);
    *entry = reinterpret_cast<void*>(fn);
    VirtualProtect(entry, sizeof(void*), prot, &prot);
}

HRESULT WINAPI CreateDevice(IDirect3D9* d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                            D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    const HRESULT hr = g_createDevice(d, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr)) Patch(*out, kCreateTexture, &CreateTexture, &g_createTexture);
    return hr;
}
}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        if (char* slash = strrchr(g_dir, '\\')) slash[1] = 0;
    } else if (reason == DLL_PROCESS_DETACH) {
        char path[MAX_PATH + 16];
        snprintf(path, sizeof(path), "%sdfshim.txt", g_dir);
        if (FILE* f = fopen(path, "a")) {
            fprintf(f, "depth textures made as INTZ: %ld DF24, %ld DF16\n", g_made24, g_made16);
            fclose(f);
        }
    }
    return TRUE;
}

extern "C" __declspec(dllexport) IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    using Create = IDirect3D9*(WINAPI*)(UINT);
    static Create create = [] {
        char path[MAX_PATH + 16];
        const UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (!n || n >= MAX_PATH) return Create(nullptr);
        strcat(path, "\\d3d9.dll");
        HMODULE self = nullptr, sys = LoadLibraryA(path);
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&Direct3DCreate9), &self);
        return sys && sys != self ? reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(sys, "Direct3DCreate9")))
                                  : Create(nullptr);
    }();
    IDirect3D9* d = create ? create(sdk) : nullptr;
    if (d) {
        Patch(d, kCheckDeviceFormat, &CheckDeviceFormat, &g_check);
        Patch(d, kCreateDevice, &CreateDevice, &g_createDevice);
    }
    return d;
}
