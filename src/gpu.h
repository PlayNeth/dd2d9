// The Direct3D 9 device of a DirectDraw object, shared by presentation and Direct3D.
//
// Created once, windowed, on a hidden window of its own with a 1x1 implicit back buffer,
// and never Reset for the game window's sake: the image reaches the game's window through
// an additional swap chain, which a resize only recreates. So the game's GPU resources
// (render targets, textures) survive window changes. See re/d3d-design.md, section 2.
// d3d9.h is not included here: it conflicts with the Direct3D 7 headers (d3d.h), so only
// the Direct3D 9 side's own source files include it (gpu.cpp, presenter.cpp, ...).
#pragma once
#include "common.h"

#include <vector>

struct IDirect3D9;
struct IDirect3DDevice9;
struct IDirect3DSwapChain9;

namespace dd2d9 {

IDirect3D9* CreateD3D9();  // Windows' d3d9.dll or the game's, as configured

class Gpu {
public:
    // Something holding D3DPOOL_DEFAULT resources of this device: they must go before a
    // Reset (device lost) or when the device is released.
    struct Owner {
        virtual void ReleaseGpuResources() = 0;
    };

    Gpu() { InitializeCriticalSection(&lock_); }
    ~Gpu();
    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;

    // Device use from any thread: in between these (D3D9's own MULTITHREADED lock covers
    // single calls, this one sequences of them).
    void Lock() { EnterCriticalSection(&lock_); }
    void Unlock() { LeaveCriticalSection(&lock_); }

    // The device, created if needed and usable now (Reset if it was lost and can be);
    // nullptr while it is lost or can't be created. Lock held.
    IDirect3DDevice9* Ensure();
    IDirect3DDevice9* Device() const { return dev_; }  // Lock held; may be lost

    // The swap chain presenting into `hwnd`'s client area at w x h (recreated when either
    // changes). Lock held, after Ensure().
    IDirect3DSwapChain9* SwapChain(HWND hwnd, UINT w, UINT h);
    void DropSwapChain();  // Lock held: another device (a wrapper's) presents into the window

    void AddOwner(Owner* o);     // Lock held
    void RemoveOwner(Owner* o);  // Lock held
    void Release();              // Lock held: everything goes (the DirectDraw object is going)

private:
    void ReleaseResources();  // owners' resources and the swap chain
    HWND MakeWindow();

    CRITICAL_SECTION lock_;
    IDirect3D9* d3d_ = nullptr;
    IDirect3DDevice9* dev_ = nullptr;
    HWND window_ = nullptr;  // the device's own (hidden) window
    IDirect3DSwapChain9* swap_ = nullptr;
    HWND swapWnd_ = nullptr;
    UINT swapW_ = 0, swapH_ = 0;
    int swapInterval_ = -1;
    std::vector<Owner*> owners_;
    bool failed_ = false;  // creation failed: not retried every frame
    DWORD failedAt_ = 0;
};

// Holds a Gpu's lock for a scope.
class GpuGuard {
public:
    explicit GpuGuard(Gpu& g) : g_(g) { g_.Lock(); }
    ~GpuGuard() { g_.Unlock(); }
    GpuGuard(const GpuGuard&) = delete;
    GpuGuard& operator=(const GpuGuard&) = delete;

private:
    Gpu& g_;
};

}  // namespace dd2d9
