// DLL entry point, exported functions and COM class factory.
#include "objects.h"

using namespace dd2d9;

namespace {

HRESULT CreateDDraw(REFIID riid, void** out, bool initialized) {
    auto* dd = new DDraw();
    dd->initialized = initialized;
    HRESULT hr = dd->QueryInterface(riid, out);
    if (FAILED(hr)) {
        dd->AddRef();
        dd->Release();  // destroys it
    }
    return hr;
}

class ClassFactory final : public IClassFactory {
public:
    explicit ClassFactory(REFCLSID clsid) : clsid_(clsid) {}
    STDMETHOD(QueryInterface)(REFIID r, void** out) override {
        if (!out) return E_POINTER;
        if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IClassFactory)) {
            AddRef();
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ULONG(InterlockedIncrement(&ref_)); }
    STDMETHOD_(ULONG, Release)() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return ULONG(r);
    }
    STDMETHOD(CreateInstance)(IUnknown* outer, REFIID r, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        if (IsEqualGUID(clsid_, CLSID_DirectDrawClipper)) {
            auto* c = new Clipper(nullptr);
            HRESULT hr = c->QueryInterface(r, out);
            c->Release();
            return hr;
        }
        return CreateDDraw(r, out, false);
    }
    STDMETHOD(LockServer)(WINBOOL) override { return S_OK; }

private:
    ~ClassFactory() = default;
    LONG ref_ = 1;
    CLSID clsid_;
};

}  // namespace

extern "C" {

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void* reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_displayLock);
        LoadConfig(inst);
        LogInit();
    }
    if (reason == DLL_PROCESS_DETACH) {
        if (!reserved) CrashLoggerRemove();  // unloaded while the process goes on: its handler must go too
        LogFlush(reserved != nullptr);       // non-null: the process is exiting
    }
    return TRUE;
}

HRESULT WINAPI DD2D9_DirectDrawCreate(GUID* guid, LPDIRECTDRAW* out, IUnknown* outer) {
    DD2D9_LOG("DirectDrawCreate(%p)", static_cast<void*>(guid));
    if (!out) return DDERR_INVALIDPARAMS;
    if (outer) return CLASS_E_NOAGGREGATION;
    return CreateDDraw(IID_IDirectDraw, reinterpret_cast<void**>(out), true);
}

HRESULT WINAPI DD2D9_DirectDrawCreateEx(GUID* guid, LPVOID* out, REFIID riid, IUnknown* outer) {
    DD2D9_LOG("DirectDrawCreateEx(%p, %s)", static_cast<void*>(guid), GuidName(riid));
    if (!out) return DDERR_INVALIDPARAMS;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!IsEqualGUID(riid, IID_IDirectDraw7)) return DDERR_INVALIDPARAMS;
    return CreateDDraw(riid, out, true);
}

HRESULT WINAPI DD2D9_DirectDrawCreateClipper(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown* outer) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (outer) return CLASS_E_NOAGGREGATION;
    *out = new Clipper(nullptr);
    return DD_OK;
}

HRESULT WINAPI DD2D9_DirectDrawEnumerateA(LPDDENUMCALLBACKA cb, LPVOID ctx) {
    if (!cb) return DDERR_INVALIDPARAMS;
    char desc[] = "Primary Display Driver", name[] = "display";
    cb(nullptr, desc, name, ctx);
    return DD_OK;
}

HRESULT WINAPI DD2D9_DirectDrawEnumerateW(LPDDENUMCALLBACKW cb, LPVOID ctx) {
    if (!cb) return DDERR_INVALIDPARAMS;
    wchar_t desc[] = L"Primary Display Driver", name[] = L"display";
    cb(nullptr, desc, name, ctx);
    return DD_OK;
}

HRESULT WINAPI DD2D9_DirectDrawEnumerateExA(LPDDENUMCALLBACKEXA cb, LPVOID ctx, DWORD) {
    if (!cb) return DDERR_INVALIDPARAMS;
    char desc[] = "Primary Display Driver", name[] = "display";
    cb(nullptr, desc, name, ctx, nullptr);
    return DD_OK;
}

HRESULT WINAPI DD2D9_DirectDrawEnumerateExW(LPDDENUMCALLBACKEXW cb, LPVOID ctx, DWORD) {
    if (!cb) return DDERR_INVALIDPARAMS;
    wchar_t desc[] = L"Primary Display Driver", name[] = L"display";
    cb(nullptr, desc, name, ctx, nullptr);
    return DD_OK;
}

HRESULT WINAPI DD2D9_DllCanUnloadNow() { return S_FALSE; }

HRESULT WINAPI DD2D9_DllGetClassObject(REFCLSID clsid, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!IsEqualGUID(clsid, CLSID_DirectDraw) && !IsEqualGUID(clsid, CLSID_DirectDraw7) &&
        !IsEqualGUID(clsid, CLSID_DirectDrawClipper))
        return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new ClassFactory(clsid);
    HRESULT hr = f->QueryInterface(riid, out);
    f->Release();
    return hr;
}

void WINAPI DD2D9_AcquireDDThreadLock() { EnterCriticalSection(&g_lock); }
void WINAPI DD2D9_ReleaseDDThreadLock() { LeaveCriticalSection(&g_lock); }

// Internal exports used by d3dim.dll and display drivers. DD2D9 does not
// provide Direct3D, so these only exist to keep the export table compatible.
// Stack sizes match the original XP SP3 ddraw.dll.
HRESULT WINAPI DD2D9_CheckFullscreen() { return 0; }
HRESULT WINAPI DD2D9_CompleteCreateSysmemSurface(void*) { return DDERR_UNSUPPORTED; }
HRESULT WINAPI DD2D9_D3DParseUnknownCommand(void*, void**) { return DDERR_UNSUPPORTED; }
HRESULT WINAPI DD2D9_DDGetAttachedSurfaceLcl(void*, void*, void**) { return DDERR_UNSUPPORTED; }
HRESULT WINAPI DD2D9_DDInternalLock(void*, void**) { return DDERR_UNSUPPORTED; }
HRESULT WINAPI DD2D9_DDInternalUnlock(void*) { return DDERR_UNSUPPORTED; }
HRESULT WINAPI DD2D9_DSoundHelp(HWND, void*, DWORD) { return 0; }
HRESULT WINAPI DD2D9_GetDDSurfaceLocal(void*, void*, void**) { return DDERR_UNSUPPORTED; }
void* WINAPI DD2D9_GetOLEThunkData(DWORD) { return nullptr; }
HRESULT WINAPI DD2D9_GetSurfaceFromDC(HDC, void**, HDC*) { return DDERR_NOTFOUND; }
HRESULT WINAPI DD2D9_RegisterSpecialCase(DWORD, DWORD, DWORD, DWORD) { return DD_OK; }

}  // extern "C"
