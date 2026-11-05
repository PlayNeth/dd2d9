// The Direct3D 9 device of a DirectDraw object, shared by presentation and Direct3D.
//
// Created once, windowed, its focus window a hidden one of its own and its device window
// the game's (overlay tools may take that one for the program's window), and never Reset for
// the game window's sake: its implicit back buffer is as large as the largest monitor, the
// frame is drawn into its top left and presented into the game's window with
// IDirect3DDevice9::Present (a source rectangle, the window as destination). So the game's
// GPU resources (render targets, textures) survive window changes, and overlay and capture
// tools that hook the device's Present (RivaTuner, Afterburner) see the frames. A window
// larger than that back buffer gets an additional swap chain. See re/d3d-design.md, section 2.
// d3d9.h is not included here: it conflicts with the Direct3D 7 headers (d3d.h), so only
// the Direct3D 9 side's own source files include it (gpu.cpp, presenter.cpp, ...).
#pragma once
#include "common.h"

#include <vector>

struct IDirect3D9;
struct IDirect3DDevice9;
struct IDirect3DSwapChain9;
struct IDirect3DBaseTexture9;
struct IDirect3DSurface9;

namespace dd2d9 {

IDirect3D9* CreateD3D9();  // Windows' d3d9.dll or the game's, as configured

// GpuContext::Draw sets a vertex declaration or a stage's texture only when it isn't the one
// it set last on that device (gpures.cpp). Anything else that sets a device's vertex format or
// textures (GPU blits, the presenter, overlays) goes through SetFvf9 and SetTexture9, and a
// device made, reset, released or presented bumps the epoch too: Draw's record is then
// stale. Gpu lock held.
extern volatile LONG g_deviceStateEpoch;
inline void DeviceStateChanged() { InterlockedIncrement(&g_deviceStateEpoch); }
HRESULT SetFvf9(IDirect3DDevice9* d, DWORD fvf);
HRESULT SetTexture9(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* t);

class Gpu {
public:
    // Something holding D3DPOOL_DEFAULT resources of this device: they must go before a
    // Reset (device lost) or when the device is released.
    struct Owner {
        virtual void ReleaseGpuResources() = 0;
        // The device itself goes (Release): its resources go, and the owner is no longer one.
        virtual void GpuReleased() { ReleaseGpuResources(); }
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
    // The game's window: the device's window when it is made (see Ensure). Any thread.
    void SetWindowHint(HWND h) { InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&hint_), h); }
    IDirect3DDevice9* Device() const { return dev_; }  // Lock held; may be lost

    // A frame for `hwnd`'s client area at w x h: whether it can be drawn (into
    // TargetBackBuffer's top left w x h) and presented (PresentTarget). Lock held, after Ensure().
    bool PrepareTarget(HWND hwnd, UINT w, UINT h);
    IDirect3DSurface9* TargetBackBuffer();  // referenced; nullptr if it can't be had
    HRESULT PresentTarget();
    void DropSwapChain();  // Lock held: another device (a wrapper's) presents into the window

    // Counts device creations and resets: the device's states and D3DPOOL_DEFAULT resources
    // are new since a different value. Lock held.
    unsigned Generation() const { return generation_; }
    // The GpuContext (gpures.h) whose states the device holds now: another one taking over
    // sends all of its own again. Lock held.
    const void* stateOwner = nullptr;
    // Direct3D 9 draws only inside BeginScene/EndScene, and presents only outside: a scene
    // opens with the first draw and closes before anything presents or reads back. Lock held.
    void OpenScene();
    void CloseScene();

    void AddOwner(Owner* o);     // Lock held
    void RemoveOwner(Owner* o);  // Lock held
    bool Unused() const { return owners_.empty(); }  // Lock held
    void Release();              // Lock held: everything goes (the DirectDraw object is going)

private:
    void ReleaseResources();  // owners' resources and the swap chain
    HWND MakeWindow();

    CRITICAL_SECTION lock_;
    IDirect3D9* d3d_ = nullptr;
    IDirect3DDevice9* dev_ = nullptr;
    HWND window_ = nullptr;  // the device's own (hidden) window, on a thread of its own
    UINT devW_ = 0, devH_ = 0;  // its implicit back buffer (decided at creation)
    HWND hint_ = nullptr;       // SetWindowHint's
    HWND devWindow_ = nullptr;  // the implicit swap chain's window (the game's, or window_)
    HWND DeviceWindow();        // for a creation or Reset
    HWND targetWnd_ = nullptr;  // PrepareTarget's
    UINT targetW_ = 0, targetH_ = 0;
    IDirect3DSwapChain9* swap_ = nullptr;  // for a target larger than the implicit back buffer
    HWND swapWnd_ = nullptr;
    UINT swapW_ = 0, swapH_ = 0;
    std::vector<Owner*> owners_;
    unsigned generation_ = 0;
    bool inScene_ = false;
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
