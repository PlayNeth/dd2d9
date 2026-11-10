// The Direct3D 9 device of a DirectDraw object (see gpu.h).
#include "gpu.h"

#include "gpures.h"

#include <d3d9.h>
#include <dwmapi.h>

#include <algorithm>

namespace dd2d9 {

volatile LONG g_deviceStateEpoch = 0;

HRESULT SetFvf9(IDirect3DDevice9* d, DWORD fvf) {
    DeviceStateChanged();
    return d->SetFVF(fvf);
}

HRESULT SetTexture9(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* t) {
    DeviceStateChanged();
    return d->SetTexture(stage, t);
}

// Direct3D 9: by default Windows' own d3d9.dll, loaded by its full path. Next to a wrapper
// such as D7VK the game folder holds DXVK's d3d9.dll, whose Vulkan presentation can let
// the driver take the display over in a window that covers it, and switch the display mode
// back to the desktop's when it lets go.
IDirect3D9* CreateD3D9() {
    using Create = IDirect3D9*(WINAPI*)(UINT);
    static const Create create = [] {
        Create fn = &Direct3DCreate9;  // whichever d3d9.dll the process loaded
        HMODULE m = GetModuleHandleW(L"d3d9.dll");
        if (g_config.systemD3D9) {
            bool loaded = false;
            wchar_t path[MAX_PATH];
            const UINT n = GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 for a 32-bit game on 64-bit Windows
            if (n && n < MAX_PATH - 10) {
                lstrcatW(path, L"\\d3d9.dll");
                if (HMODULE sys = LoadLibraryW(path)) {
                    if (auto f = reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(sys, "Direct3DCreate9")))) {
                        fn = f;
                        m = sys;
                        loaded = true;
                    }
                }
            }
            if (!loaded) DD2D9_LOG("Windows' d3d9.dll could not be loaded: using the game's");
        }
        char name[MAX_PATH] = "?";
        if (m) GetModuleFileNameA(m, name, MAX_PATH);
        DD2D9_LOG("presenting through %s", name);
        return fn;
    }();
    return create(D3D_SDK_VERSION);
}

// The largest monitor's size, current or as the registry has it (the device may be made
// while a game has the display switched to a small mode): the implicit back buffer's.
static BOOL CALLBACK AddMonitor(HMONITOR h, HDC, RECT*, LPARAM p) {
    SIZE* m = reinterpret_cast<SIZE*>(p);
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(h, &mi)) return TRUE;
    m->cx = std::max(m->cx, mi.rcMonitor.right - mi.rcMonitor.left);
    m->cy = std::max(m->cy, mi.rcMonitor.bottom - mi.rcMonitor.top);
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm) && (dm.dmFields & DM_PELSWIDTH) &&
        (dm.dmFields & DM_PELSHEIGHT)) {
        m->cx = std::max(m->cx, LONG(dm.dmPelsWidth));
        m->cy = std::max(m->cy, LONG(dm.dmPelsHeight));
    }
    return TRUE;
}

static SIZE LargestMonitor() {
    SIZE m = {640, 480};
    EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&m));
    m.cx = std::min(m.cx, LONG(16384));
    m.cy = std::min(m.cy, LONG(16384));
    return m;
}

// Vsync under the desktop compositor (Windows 8 and later; Windows 7 with Aero on). A
// windowed frame is shown at the compositor's next pass whenever it is presented, so the
// pacing that matters is one frame a refresh, which dd2d9 keeps itself (DDraw::PaceFlip):
// Direct3D 9's own windowed vsync makes Present wait for a vblank, and Intel's driver
// spins the CPU for it (N.I.C.E. 2 on a UHD 620: Present 7-8 ms a frame, the game's own
// work slowed from 3-5 to 7-10 ms on the shared power budget, a frame now and then a
// refresh late; at once and paced by dd2d9, all 600 frames of 10 s on time). Not under
// Wine, whose compositor isn't Windows' (Present's vsync as before), nor without one: asked
// again at each device creation and reset (Windows 7's can be turned off while a game runs;
// then the timing call fails and PaceFlip keeps its old fallback until then).
namespace {
using DwmTimingFn = HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*);
using DwmEnabledFn = HRESULT(WINAPI*)(BOOL*);
struct Dwm {
    DwmTimingFn timing = nullptr;
    DwmEnabledFn enabled = nullptr;
};
const Dwm& DwmFunctions() {
    static const Dwm dwm = [] {
        Dwm r;
        if (g_config.vsync == kVSyncOff) return r;
        if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) return r;
        wchar_t path[MAX_PATH];
        const UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (!n || n >= MAX_PATH - 12) return r;
        lstrcatW(path, L"\\dwmapi.dll");
        const HMODULE m = LoadLibraryW(path);
        if (!m) return r;
        r.enabled = reinterpret_cast<DwmEnabledFn>(reinterpret_cast<void*>(GetProcAddress(m, "DwmIsCompositionEnabled")));
        r.timing = reinterpret_cast<DwmTimingFn>(reinterpret_cast<void*>(GetProcAddress(m, "DwmGetCompositionTimingInfo")));
        if (!r.enabled || !r.timing) r = Dwm();
        return r;
    }();
    return dwm;
}
}  // namespace

bool Gpu::CompositorPaced() {
    if (g_config.vsync == kVSyncOff) return false;
    const Dwm& dwm = DwmFunctions();
    BOOL on = FALSE;
    const bool paced = dwm.enabled && SUCCEEDED(dwm.enabled(&on)) && on;
    static volatile LONG logged = -1;  // what the log said last (-1: nothing yet)
    if (InterlockedExchange(&logged, paced) != LONG(paced))
        DD2D9_LOG(paced ? "vsync: frames paced by dd2d9 to the desktop compositor's refresh"
                        : "vsync: Direct3D 9's (Wine, or no desktop compositor)");
    return paced;
}

bool Gpu::CompositorClock(int64_t* vblank, int64_t* period) {
    const Dwm& dwm = DwmFunctions();
    if (!dwm.timing) return false;
    DWM_TIMING_INFO t = {};
    t.cbSize = sizeof(t);
    // (nullptr: the whole desktop's timing, as Windows 8.1 and later require; fails while
    // composition is off)
    if (FAILED(dwm.timing(nullptr, &t)) || !t.qpcRefreshPeriod || !t.qpcVBlank) return false;
    *vblank = int64_t(t.qpcVBlank);
    *period = int64_t(t.qpcRefreshPeriod);
    return true;
}

static UINT PresentInterval() {
    return g_config.vsync == kVSyncOff || Gpu::CompositorPaced() ? D3DPRESENT_INTERVAL_IMMEDIATE
                                                                  : D3DPRESENT_INTERVAL_ONE;
}

// The device's own presentation parameters: windowed, on `window` (the game's, see
// DeviceWindow), a w x h back buffer presented into the game's window (COPY: Present takes a
// source rectangle only then).
static D3DPRESENT_PARAMETERS DeviceParams(HWND window, UINT w, UINT h) {
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = PresentInterval();
    return pp;
}

Gpu::~Gpu() {
    Lock();
    Release();
    Unlock();
    DeleteCriticalSection(&lock_);
}

// The device's own window: hidden, never shown or presented into (Present goes to the game's
// window instead).
// It belongs to a thread of its own that does nothing but handle its messages: Windows
// sends every top-level window some (WM_DISPLAYCHANGE when a display mode changes,
// WM_ACTIVATEAPP), and the sender waits for an answer. Owned by the presenter thread, which
// can be busy or waiting for one of dd2d9's locks, it could hold up whoever changes the
// display mode, the game's own thread included (the likely cause of Nerf Arena Blast
// freezing behind D7VK in 0.7.17, with display=switch).
// Its class is the process's (registered on the program's module) with user32's
// DefWindowProcW as its window procedure, nothing of dd2d9's: dd2d9 may be unloaded and
// loaded again (a launcher, a DirectDrawEnumerate probe), and the class registered by the
// first load is then the one the next load finds, and uses.
namespace {

struct WindowThread {
    HWND hwnd = nullptr;
    HANDLE ready = nullptr;
};

DWORD WINAPI RunWindowThread(void* p) {
    auto* t = static_cast<WindowThread*>(p);
    static const bool registered = [] {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DD2D9_Gpu";
        return RegisterClassW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    HWND h = registered ? CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DD2D9_Gpu", L"", WS_POPUP, 0, 0, 1,
                                          1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)
                        : nullptr;
    t->hwnd = h;
    SetEvent(t->ready);  // t isn't touched after this
    if (!h) return 0;
    // Until the window is gone (WM_CLOSE, which DefWindowProcW answers with DestroyWindow).
    MSG msg;
    while (IsWindow(h) && GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
}

}  // namespace

HWND Gpu::MakeWindow() {
    WindowThread t;
    t.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!t.ready) return nullptr;
    HANDLE th = CreateThread(nullptr, 0, RunWindowThread, &t, 0, nullptr);
    if (th) WaitForSingleObject(t.ready, INFINITE);
    CloseHandle(t.ready);
    if (th && !t.hwnd) CloseHandle(th);  // (ended already)
    else windowThread_ = th;  // it ends with its window (see Release)
    return t.hwnd;
}

IDirect3DDevice9* Gpu::Ensure() {
    if (!dev_) {
        if (failed_ && GetTickCount() - failedAt_ < 2000) return nullptr;  // not every frame
        failed_ = false;
        if (!window_) window_ = MakeWindow();
        if (!d3d_) d3d_ = CreateD3D9();
        if (!window_ || !d3d_) {
            DD2D9_LOG("no Direct3D 9 (%s)", window_ ? "Direct3DCreate9 failed" : "no window");
            failed_ = true;
            failedAt_ = GetTickCount();
            return nullptr;
        }
        const SIZE size = LargestMonitor();
        devW_ = UINT(size.cx);
        devH_ = UINT(size.cy);
        D3DPRESENT_PARAMETERS pp = DeviceParams(DeviceWindow(), devW_, devH_);
        // Mixed vertex processing: hardware, with software available for ProcessVertices.
        // FPU_PRESERVE: games rely on the FPU state they set. NOWINDOWCHANGES: windows
        // are dd2d9's to manage.
        const DWORD common = D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED | D3DCREATE_NOWINDOWCHANGES;
        HRESULT hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window_,
                                        common | D3DCREATE_MIXED_VERTEXPROCESSING, &pp, &dev_);
        if (FAILED(hr))
            hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window_,
                                    common | D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev_);
        if (FAILED(hr)) {
            DD2D9_LOG("CreateDevice failed: 0x%08lx", hr);
            dev_ = nullptr;
            failed_ = true;
            failedAt_ = GetTickCount();
            return nullptr;
        }
        {
            D3DCAPS9 caps = {};
            D3DADAPTER_IDENTIFIER9 id = {};
            dev_->GetDeviceCaps(&caps);
            d3d_->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
            DD2D9_LOG("D3D9 device created: %s (%s), pixel shaders %lu.%lu, back buffer %ux%u", id.Description, id.Driver,
                      static_cast<unsigned long>((caps.PixelShaderVersion >> 8) & 0xFF),
                      static_cast<unsigned long>(caps.PixelShaderVersion & 0xFF), devW_, devH_);
        }
        immediateRefused_ = false;  // (a new device may present at once)
        generation_++;
        DeviceStateChanged();
        inScene_ = false;
        return dev_;
    }
    const HRESULT coop = dev_->TestCooperativeLevel();
    if (coop == D3DERR_DEVICELOST) return nullptr;  // until Windows gives it back
    if (coop == D3DERR_DEVICENOTRESET) {
        ReleaseResources();  // D3DPOOL_DEFAULT resources must go before a Reset
        D3DPRESENT_PARAMETERS pp = DeviceParams(DeviceWindow(), devW_, devH_);
        const HRESULT hr = dev_->Reset(&pp);
        if (hr == D3DERR_DEVICELOST) return nullptr;
        if (FAILED(hr)) {
            DD2D9_LOG("Reset failed: 0x%08lx; making a new device", hr);
            dev_->Release();
            dev_ = nullptr;
            return nullptr;
        }
        DD2D9_LOG("D3D9 device reset after a loss");
        generation_++;
        DeviceStateChanged();
        inScene_ = false;
        return dev_;
    }
    if (FAILED(coop)) {  // D3DERR_DRIVERINTERNALERROR: start over
        DD2D9_LOG("device unusable (0x%08lx): making a new one", coop);
        ReleaseResources();
        dev_->Release();
        dev_ = nullptr;
        return nullptr;
    }
    return dev_;
}

// The game's window if it has one that still exists (SetWindowHint), else the hidden one.
HWND Gpu::DeviceWindow() {
    HWND h = static_cast<HWND>(InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&hint_), nullptr, nullptr));
    if (!h || !IsWindow(h)) h = window_;
    if (h != devWindow_) DD2D9_LOG("device window %p%s", static_cast<void*>(h), h == window_ ? " (dd2d9's own)" : "");
    devWindow_ = h;
    return h;
}

bool Gpu::PrepareTarget(HWND hwnd, UINT w, UINT h, bool immediate) {
    if (!dev_ || !hwnd || !w || !h) return false;
    const bool fits = w <= devW_ && h <= devH_;
    // The device's own swap chain presents at a vblank unless vsync=off or the compositor
    // paces (PresentInterval): a game flipping without vsync gets a swap chain that doesn't
    // wait. Otherwise its frames, drawn on the same device, would wait for those presents.
    const UINT interval = immediate && !immediateRefused_ && PresentInterval() != D3DPRESENT_INTERVAL_IMMEDIATE
                              ? D3DPRESENT_INTERVAL_IMMEDIATE
                              : PresentInterval();
    const bool own = !fits || interval != PresentInterval();
    if (hwnd != targetWnd_ || w != targetW_ || h != targetH_)
        DD2D9_LOG("presenting into window %p, %ux%u%s", static_cast<void*>(hwnd), w, h,
                  fits ? "" : " (larger than the device's back buffer: a swap chain of its own)");
    targetWnd_ = hwnd;
    targetW_ = w;
    targetH_ = h;
    if (!own) {
        DropSwapChain();
        return true;
    }
    if (swap_ && swapWnd_ == hwnd && swapW_ == w && swapH_ == h && swapInterval_ == interval) return true;
    DropSwapChain();
    // A failure isn't tried again every frame, but is after a while: it may have been
    // passing (video memory another program held), and nothing is presented meanwhile
    // (or, when the device's own fits, it presents, at a vblank).
    const bool failedBefore =
        swapFailWnd_ == hwnd && swapFailW_ == w && swapFailH_ == h && swapFailInterval_ == interval;
    if (failedBefore && GetTickCount() - swapFailAt_ < 2000) return fits;
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = interval;
    const HRESULT hr = dev_->CreateAdditionalSwapChain(&pp, &swap_);
    if (FAILED(hr)) {
        if (!failedBefore) DD2D9_LOG("CreateAdditionalSwapChain(%ux%u) failed: 0x%08lx", w, h, hr);
        swap_ = nullptr;
        swapFailAt_ = GetTickCount();
        swapFailWnd_ = hwnd;
        swapFailW_ = w;
        swapFailH_ = h;
        swapFailInterval_ = interval;
        // Only made to present at once: the usual interval instead, from now on (DXVK may
        // refuse a second swap chain on the window).
        if (interval != PresentInterval()) {
            immediateRefused_ = true;
            return PrepareTarget(hwnd, w, h, false);
        }
        return fits;
    }
    swapWnd_ = hwnd;
    swapW_ = w;
    swapH_ = h;
    swapInterval_ = interval;
    swapFailWnd_ = nullptr;
    return true;
}

IDirect3DSurface9* Gpu::TargetBackBuffer() {
    IDirect3DSurface9* bb = nullptr;
    const HRESULT hr = swap_ ? swap_->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)
                       : dev_ ? dev_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)
                              : E_FAIL;
    return SUCCEEDED(hr) ? bb : nullptr;
}

HRESULT Gpu::PresentTarget() {
    // Overlay and capture tools that hook Present may draw with their own vertex format and
    // not put ours back: Draw sets its declaration again after every frame.
    DeviceStateChanged();
    drew_ = false;
    if (swap_) {
        const HRESULT hr = swap_->Present(nullptr, nullptr, nullptr, nullptr, 0);
        // A swap chain made only to present at once that can't present (other than a lost
        // device): the device's own from the next frame on, at vblanks.
        if (FAILED(hr) && hr != D3DERR_DEVICELOST && hr != D3DERR_DEVICENOTRESET && swapInterval_ != PresentInterval()) {
            DD2D9_LOG("an immediate swap chain's Present failed (0x%08lx): presenting at vblanks", hr);
            immediateRefused_ = true;
            DropSwapChain();
        }
        return hr;
    }
    if (!dev_ || !targetWnd_) return E_FAIL;
    const RECT src = {0, 0, LONG(targetW_), LONG(targetH_)};
    return dev_->Present(&src, nullptr, targetWnd_, nullptr);
}

void Gpu::OpenScene() {
    drew_ = true;  // (every draw opens the scene first)
    if (inScene_ || !dev_) return;
    inScene_ = SUCCEEDED(dev_->BeginScene());
}

void Gpu::CloseScene() {
    if (!inScene_) return;
    inScene_ = false;
    if (dev_) dev_->EndScene();
    DeviceStateChanged();  // (tools that hook EndScene, as PresentTarget's that hook Present)
}

void Gpu::DropSwapChain() {
    if (swap_) swap_->Release();
    swap_ = nullptr;
    swapWnd_ = nullptr;
    swapW_ = swapH_ = swapInterval_ = 0;
}

void Gpu::AddOwner(Owner* o) {
    if (std::find(owners_.begin(), owners_.end(), o) == owners_.end()) owners_.push_back(o);
}

void Gpu::RemoveOwner(Owner* o) { owners_.erase(std::remove(owners_.begin(), owners_.end(), o), owners_.end()); }

void Gpu::ReleaseResources() {
    DeviceStateChanged();
    CloseScene();
    // Nothing of the owners' left bound: a D3DPOOL_DEFAULT surface or texture the device
    // still holds keeps a native Reset from succeeding (then every loss means a new device).
    if (dev_) {
        for (DWORD i = 0; i < 8; i++) SetTexture9(dev_, i, nullptr);
        IDirect3DSurface9* own = nullptr;
        if (SUCCEEDED(dev_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &own))) {
            dev_->SetRenderTarget(0, own);
            own->Release();
        }
        dev_->SetDepthStencilSurface(nullptr);
        dev_->SetStreamSource(0, nullptr, 0, 0);
        dev_->SetIndices(nullptr);
        dev_->SetPixelShader(nullptr);
        stateOwner = nullptr;  // every context binds and sends its state again
    }
    // An owner may remove itself while releasing: iterate over a copy.
    const std::vector<Owner*> owners = owners_;
    for (Owner* o : owners) o->ReleaseGpuResources();
    DropSwapChain();
    swapFailWnd_ = nullptr;  // (the next device may make it)
}

void Gpu::Release() {
    CloseScene();
    DeviceStateChanged();
    const std::vector<Owner*> owners = owners_;  // an owner may remove itself meanwhile
    owners_.clear();
    for (Owner* o : owners) o->GpuReleased();
    DropSwapChain();
    if (dev_) dev_->Release();
    dev_ = nullptr;
    if (d3d_) d3d_->Release();
    d3d_ = nullptr;
    if (window_) {
        // Destroyed by its own thread, which then ends: waited for, as dd2d9 may be unloaded
        // next and the thread runs its code. The wait is capped for a release under the loader
        // lock (a game DLL's static destructor at FreeLibrary): the thread's exit needs that
        // lock, so there it always takes the 2 s.
        PostMessageW(window_, WM_CLOSE, 0, 0);
        window_ = nullptr;
        if (windowThread_ && WaitForSingleObject(windowThread_, 2000) != WAIT_OBJECT_0)
            DD2D9_LOG("the device window's thread did not end in time");
    }
    if (windowThread_) CloseHandle(windowThread_);
    windowThread_ = nullptr;
    swapFailWnd_ = nullptr;
    failed_ = false;
}

// What Direct3D 7 reports for its hardware devices (gpures.h), from the adapter.
GpuCaps GpuGetCaps() {
    static GpuCaps caps;
    static LONG done = 0;
    if (InterlockedCompareExchange(&done, 0, 0) == 2) return caps;
    static CRITICAL_SECTION* cs = [] {
        static CRITICAL_SECTION c;
        InitializeCriticalSection(&c);
        return &c;
    }();
    EnterCriticalSection(cs);
    if (done != 2) {
        if (IDirect3D9* d3d = CreateD3D9()) {
            D3DCAPS9 c = {};
            if (SUCCEEDED(d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &c))) {
                caps.valid = true;
                caps.devCaps = c.DevCaps;
                caps.primMiscCaps = c.PrimitiveMiscCaps;
                caps.rasterCaps = c.RasterCaps;
                caps.zCmpCaps = c.ZCmpCaps;
                caps.srcBlendCaps = c.SrcBlendCaps;
                caps.destBlendCaps = c.DestBlendCaps;
                caps.alphaCmpCaps = c.AlphaCmpCaps;
                caps.shadeCaps = c.ShadeCaps;
                caps.textureCaps = c.TextureCaps;
                caps.textureFilterCaps = c.TextureFilterCaps;
                caps.textureAddressCaps = c.TextureAddressCaps;
                caps.maxTextureWidth = c.MaxTextureWidth;
                caps.maxTextureHeight = c.MaxTextureHeight;
                caps.maxTextureRepeat = c.MaxTextureRepeat;
                caps.maxTextureAspectRatio = c.MaxTextureAspectRatio;
                caps.maxAnisotropy = c.MaxAnisotropy;
                caps.guardBandLeft = c.GuardBandLeft;
                caps.guardBandTop = c.GuardBandTop;
                caps.guardBandRight = c.GuardBandRight;
                caps.guardBandBottom = c.GuardBandBottom;
                caps.extentsAdjust = c.ExtentsAdjust;
                caps.maxVertexW = c.MaxVertexW;
                caps.stencilCaps = c.StencilCaps;
                caps.fvfCaps = c.FVFCaps;
                caps.textureOpCaps = c.TextureOpCaps;
                caps.maxTextureBlendStages = c.MaxTextureBlendStages;
                caps.maxSimultaneousTextures = c.MaxSimultaneousTextures;
                caps.maxActiveLights = c.MaxActiveLights;
                caps.maxUserClipPlanes = c.MaxUserClipPlanes;
                caps.maxVertexBlendMatrices = c.MaxVertexBlendMatrices;
                caps.vertexProcessingCaps = c.VertexProcessingCaps;
                caps.hwTnL = (c.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0;
            }
            D3DDISPLAYMODE mode = {};
            d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode);
            const D3DFORMAT disp = mode.Format ? mode.Format : D3DFMT_X8R8G8B8;
            auto depthOk = [&](D3DFORMAT f) {
                return SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, disp,
                                                        D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, f));
            };
            caps.depth16 = depthOk(D3DFMT_D16);
            caps.depth24x8 = depthOk(D3DFMT_D24X8);
            caps.depth24s8 = depthOk(D3DFMT_D24S8);
            caps.depth32 = depthOk(D3DFMT_D32);
            const D3DFORMAT dxt[5] = {D3DFMT_DXT1, D3DFMT_DXT2, D3DFMT_DXT3, D3DFMT_DXT4, D3DFMT_DXT5};
            for (int i = 0; i < 5; i++)
                caps.dxt[i] = SUCCEEDED(
                    d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, disp, 0, D3DRTYPE_TEXTURE, dxt[i]));
            auto texOk = [&](D3DFORMAT f) {
                return SUCCEEDED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, disp, 0, D3DRTYPE_TEXTURE, f));
            };
            // Bump maps only where the GPU also does the bump-mapping operations.
            const bool bumpOps = (caps.textureOpCaps & (D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE)) != 0;
            caps.bumpV8U8 = bumpOps && texOk(D3DFMT_V8U8);
            caps.bumpL6V5U5 = bumpOps && texOk(D3DFMT_L6V5U5);
            caps.bumpX8L8V8U8 = bumpOps && texOk(D3DFMT_X8L8V8U8);
            d3d->Release();
        }
        InterlockedExchange(&done, 2);
    }
    LeaveCriticalSection(cs);
    return caps;
}

}  // namespace dd2d9
