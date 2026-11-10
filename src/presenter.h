// The presenter: a DirectDraw object's thread showing its primary surface (presenter.cpp; its
// GPU passes in presentpass.cpp, its conversion in presentconv.cpp).
#pragma once
#include "common.h"
#include "gpu.h"

#include <vector>

// Direct3D 9 types, defined in d3d9.h (only the presenter's and overlays' files include it).
struct IDirect3D9;
struct IDirect3DDevice9;
struct IDirect3DSurface9;
struct IDirect3DTexture9;
struct IDirect3DPixelShader9;

namespace dd2d9 {

class DDraw;
class Surface;
struct Buffer;
class OverlayGpu;

bool AdapterIdentifier(DDDEVICEIDENTIFIER2* out);  // presenter.cpp

// ---------------------------------------------------------------- Presenter
// A dedicated thread copies the primary surface into a D3D9 surface (converting
// 8/16/24/32-bit to X8R8G8B8), scales it into the game window and presents: through the
// DirectDraw object's shared device (gpu.h) and its swap chain on the window.
class Presenter final : public Gpu::Owner {
public:
    explicit Presenter(DDraw* dd);
    ~Presenter();
    void Start();
    bool Stop();  // false if the thread did not exit in time (it then still uses this object)
    void MarkDirty() { if (dirtyEvent_) SetEvent(dirtyEvent_); }
    uint64_t ConvertSeq() const { return convertSeq_; }  // read under g_lock
    bool WaitPresented(uint64_t afterSeq, DWORD timeoutMs);
    bool Active() const { return active_; }
    void SetGamma(const DDGAMMARAMP& ramp);
    // Where an sw x sh image goes in a bw x bh area, as the stretch setting says.
    RECT FitRect(UINT sw, UINT sh, UINT bw, UINT bh) const;

private:
    static DWORD WINAPI ThreadProc(void* self);
    void Loop();
    void PublishPresented(uint64_t seq);
    bool EnsureStaging(IDirect3DDevice9* d, UINT w, UINT h);
    void ReleaseGpuResources() override;     // staging surface and shader resources (Gpu lock held)
    void LeaveGpu();                         // stop presenting through the shared device
    // What the presenter copies from the primary surface under g_lock; the
    // pixels themselves are converted afterwards, without it.
    struct Snapshot {
        Buffer* buf = nullptr;  // referenced: the game may flip or release it meanwhile
        Format fmt;
        PALETTEENTRY pal[256];  // 8-bit formats
    };
    void Convert(const Snapshot& s, const RECT& area, void* bits, int pitch);
    // A windowed program's window as GDI draws it (WM_PRINT, refreshed every 0.1 s at most),
    // for overlays' destination keys (overlay.cpp). Presenter thread, no lock held.
    bool CaptureGdi(HWND hwnd, LONG w, LONG h);
    std::vector<uint32_t> gdi_;
    HWND gdiWnd_ = nullptr;
    LONG gdiW_ = 0, gdiH_ = 0;
    double gdiTime_ = -1;
    // The DC the window paints into, kept: a WM_PRINT that timed out is still handled later.
    HDC gdiDc_ = nullptr;
    HBITMAP gdiBm_ = nullptr;
    HGDIOBJ gdiOld_ = nullptr;
    void* gdiBits_ = nullptr;
    LONG gdiDcW_ = 0, gdiDcH_ = 0;
    bool gdiPending_ = false;  // the last WM_PRINT timed out (its DC may still be painted)
    unsigned gdiSeq_ = 0;      // counts captures (the GPU path uploads a new one)
    OverlayGpu* ovGpu_ = nullptr;
    void BuildLut16(const Format& f, const uint8_t (&gamma)[3][256], DWORD gammaVersion);
    void CountPresent(double lockSeconds, double convertSeconds, double fraction);

    DDraw* dd_;
    HANDLE thread_ = nullptr;
    HMODULE threadHold_ = nullptr;  // dd2d9, held loaded for the thread (null: not held; Start)
    HANDLE doneEvent_ = nullptr;    // set by the thread when it is done with the presenter (Stop)
    HANDLE dirtyEvent_ = nullptr;
    volatile bool running_ = false;
    volatile bool active_ = false;
    int64_t lastPresent_ = 0;  // QueryPerformanceCounter at the last present (the presenter's thread)

    bool usingGpu_ = false;            // presenting through the shared device (an owner of it)
    IDirect3DSurface9* staging_ = nullptr;
    std::vector<uint32_t> frame_;  // the CPU frame converted outside the Gpu lock (X8R8G8B8, frameW_ wide)
    UINT frameW_ = 0, frameH_ = 0;
    // filter=sharp: the frame copied into a texture, drawn through a sharp-bilinear shader.
    IDirect3DTexture9* sharpTex_ = nullptr;
    IDirect3DPixelShader9* sharpPs_ = nullptr;
    UINT sharpW_ = 0, sharpH_ = 0;
    bool sharpFailed_ = false;  // no pixel shaders: prescaled_ instead
    IDirect3DSurface9* prescaled_ = nullptr;  // without shaders: the frame enlarged by a whole factor
    UINT preW_ = 0, preH_ = 0;
    bool EnsurePrescaled(IDirect3DDevice9* d, UINT w, UINT h);
    bool DrawSharp(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT& r, IDirect3DSurface9* bb, const RECT& dst);
    // The gamma ramp on frames presented from a render target: the frame copied into a
    // texture, drawn through a pixel shader that looks each channel up in a 256x1 ramp.
    IDirect3DTexture9* gammaSrc_ = nullptr;
    IDirect3DSurface9* gammaOut_ = nullptr;
    IDirect3DTexture9* gammaLutTex_ = nullptr;
    IDirect3DPixelShader9* gammaPs_ = nullptr;
    UINT gammaW_ = 0, gammaH_ = 0;
    DWORD gammaTexVersion_ = 0;
    bool gammaFailed_ = false;     // no pixel shaders: GPU frames shown without the ramp
    bool gammaLutFailed_ = false;  // no ramp texture on this device: the same until another
    IDirect3DSurface9* ApplyGammaGpu(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT& r);
    // The size a render target or texture above couldn't be made at (on this device): not
    // tried again every frame, only at another size.
    struct FailedSize {
        UINT w = 0, h = 0;
        bool Is(UINT ww, UINT hh) const { return w == ww && h == hh; }
        void Set(UINT ww, UINT hh) { w = ww, h = hh; }
    };
    FailedSize gammaFail_, preFail_, sharpFail_;
    UINT stW_ = 0, stH_ = 0;

    // What the staging surface holds (presenter thread only): a partial update
    // is only possible while these match the primary surface.
    bool stagingStale_ = true;  // new staging surface, failed update, or content not shown for a while
    RECT lastSrc_ = {0, 0, 0, 0};
    const Surface* lastPrimary_ = nullptr;
    const Buffer* lastBuf_ = nullptr;

    uint64_t convertSeq_ = 0;
    uint64_t presentedSeq_ = 0;
    SRWLOCK seqLock_ = SRWLOCK_INIT;
    CONDITION_VARIABLE seqCv_ = CONDITION_VARIABLE_INIT;

    // Statistics, logged every 10 s while presenting (presenter thread only).
    double statsStart_ = 0, statsLock_ = 0, statsLockMax_ = 0, statsConvert_ = 0, statsFraction_ = 0;
    unsigned statsPresents_ = 0;

    std::vector<uint32_t> lut16_;
    Format lut16Fmt_;
    DWORD lut16GammaVersion_ = 0;

    CRITICAL_SECTION gammaLock_;
    DDGAMMARAMP gammaRamp_;
    uint8_t gammaLut_[3][256];
    bool gammaActive_ = false;
    DWORD gammaVersion_ = 1;
};

// presentpass.cpp: the draw call RivaTuner needs in a frame (see there).
void DrawCall(Gpu& gpu, IDirect3DDevice9* d);

}  // namespace dd2d9
