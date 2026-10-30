// The Direct3D 9 device of a DirectDraw object (see gpu.h).
#include "gpu.h"

#include <d3d9.h>

#include <algorithm>

namespace dd2d9 {

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

// The device's own presentation parameters: windowed, a 1x1 back buffer on its hidden
// window, never presented (the swap chains present).
static D3DPRESENT_PARAMETERS DeviceParams(HWND window) {
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 1;
    pp.BackBufferHeight = 1;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = window;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    return pp;
}

Gpu::~Gpu() {
    Lock();
    Release();
    Unlock();
    DeleteCriticalSection(&lock_);
}

// The device's own window: hidden, never shown or presented into. The game's window only
// ever gets a swap chain, which another device (a wrapper's) can replace (DropSwapChain).
HWND Gpu::MakeWindow() {
    static const ATOM cls = [] {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DD2D9_Gpu";
        return RegisterClassW(&wc);
    }();
    if (!cls) return nullptr;
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"DD2D9_Gpu", L"", WS_POPUP, 0, 0, 1, 1, nullptr,
                           nullptr, GetModuleHandleW(nullptr), nullptr);
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
        D3DPRESENT_PARAMETERS pp = DeviceParams(window_);
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
        DD2D9_LOG("D3D9 device created");
        return dev_;
    }
    const HRESULT coop = dev_->TestCooperativeLevel();
    if (coop == D3DERR_DEVICELOST) return nullptr;  // until Windows gives it back
    if (coop == D3DERR_DEVICENOTRESET) {
        ReleaseResources();  // D3DPOOL_DEFAULT resources must go before a Reset
        D3DPRESENT_PARAMETERS pp = DeviceParams(window_);
        const HRESULT hr = dev_->Reset(&pp);
        if (hr == D3DERR_DEVICELOST) return nullptr;
        if (FAILED(hr)) {
            DD2D9_LOG("Reset failed: 0x%08lx; making a new device", hr);
            dev_->Release();
            dev_ = nullptr;
            return nullptr;
        }
        DD2D9_LOG("D3D9 device reset after a loss");
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

IDirect3DSwapChain9* Gpu::SwapChain(HWND hwnd, UINT w, UINT h) {
    if (!dev_) return nullptr;
    const int interval = g_config.vsync == kVSyncOff ? 0 : 1;
    if (swap_ && swapWnd_ == hwnd && swapW_ == w && swapH_ == h && swapInterval_ == interval) return swap_;
    DropSwapChain();
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = interval ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    const HRESULT hr = dev_->CreateAdditionalSwapChain(&pp, &swap_);
    if (FAILED(hr)) {
        DD2D9_LOG("CreateAdditionalSwapChain(%ux%u) failed: 0x%08lx", w, h, hr);
        swap_ = nullptr;
        return nullptr;
    }
    swapWnd_ = hwnd;
    swapW_ = w;
    swapH_ = h;
    swapInterval_ = interval;
    DD2D9_LOG("swap chain for window %p, %ux%u", static_cast<void*>(hwnd), w, h);
    return swap_;
}

void Gpu::DropSwapChain() {
    if (swap_) swap_->Release();
    swap_ = nullptr;
    swapWnd_ = nullptr;
    swapW_ = swapH_ = 0;
    swapInterval_ = -1;
}

void Gpu::AddOwner(Owner* o) {
    if (std::find(owners_.begin(), owners_.end(), o) == owners_.end()) owners_.push_back(o);
}

void Gpu::RemoveOwner(Owner* o) { owners_.erase(std::remove(owners_.begin(), owners_.end(), o), owners_.end()); }

void Gpu::ReleaseResources() {
    // An owner may remove itself while releasing: iterate over a copy.
    const std::vector<Owner*> owners = owners_;
    for (Owner* o : owners) o->ReleaseGpuResources();
    DropSwapChain();
}

void Gpu::Release() {
    ReleaseResources();
    owners_.clear();
    if (dev_) dev_->Release();
    dev_ = nullptr;
    if (d3d_) d3d_->Release();
    d3d_ = nullptr;
    if (window_) {
        // A window can only be destroyed by its own thread; another one asks it to.
        if (GetWindowThreadProcessId(window_, nullptr) == GetCurrentThreadId()) DestroyWindow(window_);
        else PostMessageW(window_, WM_CLOSE, 0, 0);
        window_ = nullptr;
    }
    failed_ = false;
}

}  // namespace dd2d9
