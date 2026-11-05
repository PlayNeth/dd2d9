// Direct3D 9 presentation of the primary surface.
#include "convert.h"
#include "objects.h"
#include "gpu.h"
#include "gpures.h"

#include <d3d9.h>
#include "devstate.h"

#include <cmath>
#include <utility>
#include <vector>

namespace dd2d9 {

namespace {

// Objects in normal mode each have a primary of their own, where on Windows they share the
// one screen: a video renderer's beside the game's (VMR-7 makes one for the video and one
// for each display). A window shows the presenter that last drew a changed frame into it:
// the others' unchanged frames aren't presented again over it (the refresh four times a
// second flashed the game's last screen over Romance of the Three Kingdoms VII's intro).
SRWLOCK g_windowOwnersLock = SRWLOCK_INIT;
std::vector<std::pair<HWND, const Presenter*>> g_windowOwners;

void ClaimWindow(HWND h, const Presenter* p) {
    AcquireSRWLockExclusive(&g_windowOwnersLock);
    bool found = false;
    for (auto it = g_windowOwners.begin(); it != g_windowOwners.end();) {
        if (it->first == h) {
            it->second = p;
            found = true;
        } else if (!IsWindow(it->first)) {
            it = g_windowOwners.erase(it);
            continue;
        }
        ++it;
    }
    if (!found) g_windowOwners.emplace_back(h, p);
    ReleaseSRWLockExclusive(&g_windowOwnersLock);
}

bool OtherShowsWindow(HWND h, const Presenter* p) {
    AcquireSRWLockShared(&g_windowOwnersLock);
    bool other = false;
    for (const auto& o : g_windowOwners)
        if (o.first == h) other = o.second != p;
    ReleaseSRWLockShared(&g_windowOwnersLock);
    return other;
}

void ReleaseWindows(const Presenter* p) {
    AcquireSRWLockExclusive(&g_windowOwnersLock);
    for (auto it = g_windowOwners.begin(); it != g_windowOwners.end();) {
        if (it->second == p) it = g_windowOwners.erase(it);
        else ++it;
    }
    ReleaseSRWLockExclusive(&g_windowOwnersLock);
}

}  // namespace

// The display adapter as Direct3D 9 describes it, for GetDeviceIdentifier (ddraw.dll
// reports the real one too). Looked up once.
bool AdapterIdentifier(DDDEVICEIDENTIFIER2* out) {
    static DDDEVICEIDENTIFIER2 cached;
    static int state;  // 0 not looked up, 1 found, 2 failed
    if (!state) {
        state = 2;
        if (IDirect3D9* d3d = CreateD3D9()) {
            D3DADAPTER_IDENTIFIER9 id = {};
            if (SUCCEEDED(d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id))) {
                DDDEVICEIDENTIFIER2 d = {};
                lstrcpynA(d.szDriver, id.Driver, sizeof(d.szDriver));
                lstrcpynA(d.szDescription, id.Description, sizeof(d.szDescription));
                d.liDriverVersion.QuadPart = id.DriverVersion.QuadPart;
                d.dwVendorId = id.VendorId;
                d.dwDeviceId = id.DeviceId;
                d.dwSubSysId = id.SubSysId;
                d.dwRevision = id.Revision;
                d.guidDeviceIdentifier = id.DeviceIdentifier;
                d.dwWHQLLevel = id.WHQLLevel;
                cached = d;
                state = 1;
            }
            d3d->Release();
        }
    }
    if (state != 1) return false;
    *out = cached;
    return true;
}

Presenter::Presenter(DDraw* dd) : ovGpu_(NewOverlayGpu()), dd_(dd) {
    InitializeCriticalSection(&gammaLock_);
    for (int c = 0; c < 3; c++) {
        WORD* ch = c == 0 ? gammaRamp_.red : c == 1 ? gammaRamp_.green : gammaRamp_.blue;
        for (int i = 0; i < 256; i++) {
            ch[i] = WORD(i * 257);
            gammaLut_[c][i] = uint8_t(i);
        }
    }
}

Presenter::~Presenter() {
    Stop();
    ReleaseWindows(this);
    DeleteOverlayGpu(ovGpu_);
    if (gdiDc_ && !gdiPending_) {
        SelectObject(gdiDc_, gdiOld_);
        DeleteObject(gdiBm_);
        DeleteDC(gdiDc_);
    }
    if (dirtyEvent_) CloseHandle(dirtyEvent_);
    if (dropDone_) CloseHandle(dropDone_);
    DeleteCriticalSection(&gammaLock_);
}

void Presenter::Start() {
    if (thread_) return;
    if (!dirtyEvent_) dirtyEvent_ = CreateEventA(nullptr, FALSE, TRUE, nullptr);
    if (!dropDone_) dropDone_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    running_ = true;
    thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    DD2D9_LOG("presenter thread started");
}

// The event stays open until the destructor: MarkDirty() may race with this.
bool Presenter::Stop() {
    if (!thread_) return true;
    running_ = false;
    SetEvent(dirtyEvent_);
    // The thread releases the device on its way out, and D3D9 may then send
    // messages to the game window, possibly owned by this thread.
    const bool stopped = WaitPumping(thread_, 3000);
    if (!stopped) DD2D9_LOG("presenter thread did not stop in time; keeping its objects alive");
    CloseHandle(thread_);
    thread_ = nullptr;
    // Wake anyone still waiting in Flip().
    AcquireSRWLockExclusive(&seqLock_);
    presentedSeq_ = ~0ull;
    ReleaseSRWLockExclusive(&seqLock_);
    WakeAllConditionVariable(&seqCv_);
    return stopped;
}

void Presenter::DropDevice() {
    if (!thread_ || !dropDone_) return;
    ResetEvent(dropDone_);
    dropRequest_ = true;
    SetEvent(dirtyEvent_);
    if (!WaitPumping(dropDone_, 3000)) DD2D9_LOG("presenter did not release its device in time");
}

DWORD WINAPI Presenter::ThreadProc(void* self) {
    // Above the game's threads: it runs briefly when woken, and must then run at once. A
    // game drawing as fast as it can without waiting for a vertical blank (DDFLIP_NOVSYNC,
    // or blits to the primary in a loop) would otherwise share a processor with it at equal
    // priority (cpus=1 puts the whole process on one) and only be let run at the end of its
    // time slices, about 30 times a second: the picture moved at that rate whatever the
    // game's, and a fade the game blits in a few seconds showed few of its steps.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    static_cast<Presenter*>(self)->Loop();
    return 0;
}

bool Presenter::WaitPresented(uint64_t afterSeq, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    AcquireSRWLockExclusive(&seqLock_);
    bool ok = true;
    while (presentedSeq_ <= afterSeq) {
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeoutMs || !SleepConditionVariableSRW(&seqCv_, &seqLock_, timeoutMs - elapsed, 0)) {
            ok = presentedSeq_ > afterSeq;
            break;
        }
    }
    ReleaseSRWLockExclusive(&seqLock_);
    return ok;
}

void Presenter::SetGamma(const DDGAMMARAMP& ramp) {
    EnterCriticalSection(&gammaLock_);
    gammaRamp_ = ramp;
    gammaActive_ = false;
    for (int c = 0; c < 3; c++) {
        const WORD* ch = c == 0 ? ramp.red : c == 1 ? ramp.green : ramp.blue;
        for (int i = 0; i < 256; i++) {
            gammaLut_[c][i] = uint8_t(ch[i] >> 8);
            if (gammaLut_[c][i] != i) gammaActive_ = true;
        }
    }
    gammaVersion_++;
    LeaveCriticalSection(&gammaLock_);
}

// ---------------------------------------------------------------- device

// The staging surface and the shaders' resources (the shared device's).
void Presenter::ReleaseGpuResources() {
    if (ovGpu_) ReleaseOverlayGpu(ovGpu_);
    if (staging_) staging_->Release();
    staging_ = nullptr;
    if (gammaSrc_) gammaSrc_->Release();
    if (gammaOut_) gammaOut_->Release();
    if (gammaLutTex_) gammaLutTex_->Release();
    if (gammaPs_) gammaPs_->Release();
    gammaSrc_ = nullptr;
    gammaOut_ = nullptr;
    gammaLutTex_ = nullptr;
    gammaPs_ = nullptr;
    gammaW_ = gammaH_ = 0;
    gammaTexVersion_ = 0;
    if (prescaled_) prescaled_->Release();
    prescaled_ = nullptr;
    preW_ = preH_ = 0;
    if (sharpTex_) sharpTex_->Release();
    if (sharpPs_) sharpPs_->Release();
    sharpTex_ = nullptr;
    sharpPs_ = nullptr;
    sharpW_ = sharpH_ = 0;
    stW_ = stH_ = 0;
    stagingStale_ = true;
}

void Presenter::LeaveGpu() {
    if (!usingGpu_) return;
    Gpu& gpu = dd_->GetGpu();
    GpuGuard g(gpu);
    ReleaseGpuResources();
    gpu.DropSwapChain();  // another device (a wrapper's, or another object's) may present here now
    gpu.RemoveOwner(this);
    usingGpu_ = false;
    // Nothing else uses the device (no Direct3D of dd2d9's own yet): it goes, rather than
    // stay beside a wrapper's for nothing.
    if (gpu.Unused()) gpu.Release();
}

bool Presenter::EnsureStaging(IDirect3DDevice9* d, UINT w, UINT h) {
    if (staging_ && stW_ == w && stH_ == h) return true;
    if (staging_) staging_->Release();
    staging_ = nullptr;
    HRESULT hr = d->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &staging_, nullptr);
    if (FAILED(hr)) {
        DD2D9_LOG("CreateOffscreenPlainSurface(%u, %u) failed: 0x%08lx", w, h, hr);
        staging_ = nullptr;
        return false;
    }
    stW_ = w;
    stH_ = h;
    stagingStale_ = true;
    return true;
}

// ps_2_0: each channel of s0 looked up in s1, a 256x1 ramp (texel i = the ramp's red, green
// and blue for i). Assembled from:
//   def c0, 255/256, 0.5/256, 0.5, 1 / dcl t0.xy / dcl_2d s0 / dcl_2d s1 / texld r0, t0, s0
//   r1..r3 = (r0.x|y|z * c0.x + c0.y, 0.5) / texld r4..r6, r1..r3, s1
//   oC0 = (r4.x, r5.y, r6.z, 1)
static const DWORD kGammaPs[] = {0xffff0200, 0x0200001f, 0x80000000, 0xb0030000, 0x0200001f, 0x90000000, 0xa00f0800, 0x0200001f, 0x90000000, 0xa00f0801, 0x05000051, 0xa00f0000, 0x3f7f0000, 0x3b000000, 0x3f000000, 0x3f800000, 0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x02000001, 0x800f0001, 0xa0aa0000, 0x02000001, 0x800f0002, 0xa0aa0000, 0x02000001, 0x800f0003, 0xa0aa0000, 0x04000004, 0x80010001, 0x80000000, 0xa0000000, 0xa0550000, 0x04000004, 0x80010002, 0x80550000, 0xa0000000, 0xa0550000, 0x04000004, 0x80010003, 0x80aa0000, 0xa0000000, 0xa0550000, 0x03000042, 0x800f0004, 0x80e40001, 0xa0e40801, 0x03000042, 0x800f0005, 0x80e40002, 0xa0e40801, 0x03000042, 0x800f0006, 0x80e40003, 0xa0e40801, 0x02000001, 0x800f0007, 0xa0ff0000, 0x02000001, 0x80010007, 0x80000004, 0x02000001, 0x80020007, 0x80550005, 0x02000001, 0x80040007, 0x80aa0006, 0x02000001, 0x800f0800, 0x80e40007, 0x0000ffff};

// The frame (src's rectangle r) with the gamma ramp applied, or null (shown without it).
// Gpu lock held, outside a scene. The device's state is a GpuContext's: what this changes
// is put back after (SavedDeviceState).
IDirect3DSurface9* Presenter::ApplyGammaGpu(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT& r) {
    if (gammaFailed_) return nullptr;
    const UINT w = UINT(r.right - r.left), h = UINT(r.bottom - r.top);
    if (!gammaPs_ && FAILED(d->CreatePixelShader(kGammaPs, &gammaPs_))) {
        gammaPs_ = nullptr;
        gammaFailed_ = true;
        DD2D9_LOG("gamma: no pixel shader (the ramp isn't applied to Direct3D frames)");
        return nullptr;
    }
    if (!gammaLutTex_ && FAILED(d->CreateTexture(256, 1, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &gammaLutTex_, nullptr))) {
        gammaLutTex_ = nullptr;
        return nullptr;
    }
    if (gammaW_ != w || gammaH_ != h || !gammaSrc_ || !gammaOut_) {
        if (gammaSrc_) gammaSrc_->Release();
        if (gammaOut_) gammaOut_->Release();
        gammaSrc_ = nullptr;
        gammaOut_ = nullptr;
        gammaW_ = gammaH_ = 0;
        if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &gammaSrc_, nullptr)) ||
            FAILED(d->CreateRenderTarget(w, h, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &gammaOut_, nullptr))) {
            if (gammaSrc_) gammaSrc_->Release();
            gammaSrc_ = nullptr;
            gammaOut_ = nullptr;
            return nullptr;
        }
        gammaW_ = w;
        gammaH_ = h;
    }
    EnterCriticalSection(&gammaLock_);
    if (gammaTexVersion_ != gammaVersion_) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(gammaLutTex_->LockRect(0, &lr, nullptr, 0))) {
            auto* t = static_cast<uint32_t*>(lr.pBits);
            for (int i = 0; i < 256; i++)
                t[i] = 0xFF000000u | (uint32_t(gammaLut_[0][i]) << 16) | (uint32_t(gammaLut_[1][i]) << 8) | gammaLut_[2][i];
            gammaLutTex_->UnlockRect(0);
            gammaTexVersion_ = gammaVersion_;
        }
    }
    LeaveCriticalSection(&gammaLock_);
    IDirect3DSurface9* level = nullptr;
    if (FAILED(gammaSrc_->GetSurfaceLevel(0, &level))) return nullptr;
    d->StretchRect(src, &r, level, nullptr, D3DTEXF_POINT);
    level->Release();
    // The device's state as the game's Direct3D context left it: put back at the return.
    const SavedDeviceState saved(d);
    d->SetRenderTarget(0, gammaOut_);
    d->SetDepthStencilSurface(nullptr);
    const D3DVIEWPORT9 vp = {0, 0, w, h, 0.0f, 1.0f};
    d->SetViewport(&vp);
    d->SetPixelShader(gammaPs_);
    d->SetVertexShader(nullptr);
    SetFvf9(d, D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetRenderState(D3DRS_WRAP0, 0);
    SetTexture9(d, 0, gammaSrc_);
    SetTexture9(d, 1, gammaLutTex_);
    for (DWORD s = 0; s < 2; s++) {
        d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    const DWORD rs[][2] = {{D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE}, {D3DRS_CULLMODE, D3DCULL_NONE},
                           {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_FOGENABLE, FALSE},
                           {D3DRS_STENCILENABLE, FALSE}, {D3DRS_SCISSORTESTENABLE, FALSE},
                           {D3DRS_COLORWRITEENABLE, 0xF}, {D3DRS_FILLMODE, D3DFILL_SOLID}, {D3DRS_SRGBWRITEENABLE, FALSE},
                           {D3DRS_CLIPPLANEENABLE, 0}};
    for (const auto& s : rs) d->SetRenderState(D3DRENDERSTATETYPE(s[0]), s[1]);
    // Pixel centres on texel centres (D3D9's half-pixel offset).
    const float fw = float(w), fh = float(h);
    const float q[4][6] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {fw - 0.5f, -0.5f, 0, 1, 1, 0},
                           {-0.5f, fh - 0.5f, 0, 1, 0, 1}, {fw - 0.5f, fh - 0.5f, 0, 1, 1, 1}};
    Gpu& gpu = dd_->GetGpu();
    gpu.OpenScene();
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
    gpu.CloseScene();
    d->SetPixelShader(nullptr);
    SetTexture9(d, 0, nullptr);
    SetTexture9(d, 1, nullptr);
    return gammaOut_;
}

// filter=sharp without pixel shaders: the frame enlarged by a whole factor (a render
// target: StretchRect filters only into one).
bool Presenter::EnsurePrescaled(IDirect3DDevice9* d, UINT w, UINT h) {
    if (prescaled_ && preW_ == w && preH_ == h) return true;
    if (prescaled_) prescaled_->Release();
    prescaled_ = nullptr;
    preW_ = preH_ = 0;
    if (FAILED(d->CreateRenderTarget(w, h, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &prescaled_, nullptr))) {
        prescaled_ = nullptr;
        return false;
    }
    preW_ = w;
    preH_ = h;
    return true;
}

// ps_2_0, sharp bilinear: within each source pixel, flat colour, blended with the neighbour
// only over the last output pixel at its edge (libretro's sharp-bilinear-simple). Any
// enlargement, whole or not. c0 = source size, c1 = 0.5 - 0.5/scale (the flat region's
// half-width), c5 = -c1, c2 = scale, c3 = 1/source size. Assembled from:
//   def c4, 0.5, 0, -0.5, 0 / dcl t0 / dcl_2d s0 / mov r0..r3, c4.y
//   r0.xy = t0 * c0 (the texel position) / r1.xy = frc(r0) / r0.xy -= r1 / r1.xy += c4.z
//   r2.xy = min(max(r1, c5), c1) / r1.xy = (r1 - r2) * c2 + c4.x / r3.xy = (r0 + r1) * c3
//   texld r0, r3, s0 / oC0 = r0
// Written to stay clear of everything Windows' ps_2_0 validator may refuse (Wine checks
// none of it): every register set whole before it is read, no negated constants, one
// constant per instruction, the texture coordinate declared whole.
static const DWORD kSharpPs[] = {0xffff0200,0x0200001f,0x80000000,0xb00f0000,0x0200001f,0x90000000,
0xa00f0800,0x05000051,0xa00f0004,0x3f000000,0x00000000,0xbf000000,
0x00000000,0x02000001,0x800f0000,0xa0550004,0x02000001,0x800f0001,
0xa0550004,0x02000001,0x800f0002,0xa0550004,0x02000001,0x800f0003,
0xa0550004,0x03000005,0x80030000,0xb0e40000,0xa0e40000,0x02000013,
0x80030001,0x80e40000,0x03000002,0x80030000,0x80e40000,0x81e40001,
0x03000002,0x80030001,0x80e40001,0xa0aa0004,0x0300000b,0x80030002,
0x80e40001,0xa0e40005,0x0300000a,0x80030002,0x80e40002,0xa0e40001,
0x03000002,0x80030001,0x80e40001,0x81e40002,0x03000005,0x80030001,
0x80e40001,0xa0e40002,0x03000002,0x80030001,0x80e40001,0xa0000004,
0x03000002,0x80030000,0x80e40000,0x80e40001,0x03000005,0x80030003,
0x80e40000,0xa0e40003,0x03000042,0x800f0000,0x80e40003,0xa0e40800,
0x02000001,0x800f0800,0x80e40000,0x0000ffff,
};

// The frame (src's rectangle r) drawn into bb's rectangle dst through kSharpPs; false when it
// can't be (no pixel shaders), for plain linear filtering. Gpu lock held, outside a scene.
bool Presenter::DrawSharp(IDirect3DDevice9* d, IDirect3DSurface9* src, const RECT& r, IDirect3DSurface9* bb, const RECT& dst) {
    if (sharpFailed_) return false;
    const UINT w = UINT(r.right - r.left), h = UINT(r.bottom - r.top);
    if (!sharpPs_) {
        const HRESULT hr = d->CreatePixelShader(kSharpPs, &sharpPs_);
        if (FAILED(hr)) {
            sharpPs_ = nullptr;
            sharpFailed_ = true;
            DD2D9_LOG("filter=sharp: no pixel shader (0x%08lx), point scaling then linear instead", hr);
            // Why: whether the device takes any pixel shader at all (a pass-through one).
            static const DWORD kPassPs[] = {0xffff0200, 0x0200001f, 0x80000000, 0xb0030000, 0x0200001f, 0x90000000,
                                            0xa00f0800, 0x03000042, 0x800f0000, 0xb0e40000, 0xa0e40800, 0x02000001,
                                            0x800f0800, 0x80e40000, 0x0000ffff};
            // And one with frc alone (the instruction the pass-through one lacks).
            static const DWORD kFrcPs[] = {0xffff0200,0x0200001f,0x80000000,0xb00f0000,0x0200001f,0x90000000,
0xa00f0800,0x05000051,0xa00f0000,0x00000000,0x00000000,0x00000000,
0x00000000,0x02000001,0x800f0001,0xa0000000,0x02000013,0x80030001,
0xb0e40000,0x03000042,0x800f0000,0x80e40001,0xa0e40800,0x02000001,
0x800f0800,0x80e40000,0x0000ffff,
};
            IDirect3DPixelShader9* pass = nullptr;
            const HRESULT hp = d->CreatePixelShader(kPassPs, &pass);
            if (pass) pass->Release();
            pass = nullptr;
            const HRESULT hf = d->CreatePixelShader(kFrcPs, &pass);
            if (pass) pass->Release();
            D3DCAPS9 caps = {};
            d->GetDeviceCaps(&caps);
            DD2D9_LOG("  the device: pixel shaders %lu.%lu; a pass-through ps_2_0 shader: 0x%08lx; frc: 0x%08lx",
                      static_cast<unsigned long>((caps.PixelShaderVersion >> 8) & 0xFF),
                      static_cast<unsigned long>(caps.PixelShaderVersion & 0xFF), hp, hf);
            return false;
        }
    }
    if (!sharpTex_ || sharpW_ != w || sharpH_ != h) {
        if (sharpTex_) sharpTex_->Release();
        sharpTex_ = nullptr;
        sharpW_ = sharpH_ = 0;
        if (FAILED(d->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &sharpTex_, nullptr))) {
            sharpTex_ = nullptr;
            return false;
        }
        sharpW_ = w;
        sharpH_ = h;
    }
    IDirect3DSurface9* level = nullptr;
    if (FAILED(sharpTex_->GetSurfaceLevel(0, &level))) return false;
    d->StretchRect(src, &r, level, nullptr, D3DTEXF_POINT);
    level->Release();
    const SavedDeviceState saved(d);  // put back at the return
    d->SetRenderTarget(0, bb);
    d->SetDepthStencilSurface(nullptr);
    D3DSURFACE_DESC bd;
    bb->GetDesc(&bd);
    const D3DVIEWPORT9 vp = {0, 0, bd.Width, bd.Height, 0.0f, 1.0f};
    d->SetViewport(&vp);
    d->SetPixelShader(sharpPs_);
    d->SetVertexShader(nullptr);
    SetFvf9(d, D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetRenderState(D3DRS_WRAP0, 0);
    SetTexture9(d, 0, sharpTex_);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    const DWORD rs[][2] = {{D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE}, {D3DRS_CULLMODE, D3DCULL_NONE},
                           {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_FOGENABLE, FALSE},
                           {D3DRS_STENCILENABLE, FALSE}, {D3DRS_SCISSORTESTENABLE, FALSE},
                           {D3DRS_COLORWRITEENABLE, 0xF}, {D3DRS_FILLMODE, D3DFILL_SOLID}, {D3DRS_SRGBWRITEENABLE, FALSE},
                           {D3DRS_CLIPPLANEENABLE, 0}};
    for (const auto& s : rs) d->SetRenderState(D3DRENDERSTATETYPE(s[0]), s[1]);
    const float dw = float(dst.right - dst.left), dh = float(dst.bottom - dst.top);
    const float sx = dw / float(w), sy = dh / float(h);
    const float rx = 0.5f - 0.5f / sx, ry = 0.5f - 0.5f / sy;
    const float c[6][4] = {{float(w), float(h), 0, 0}, {rx, ry, 0, 0}, {sx, sy, 0, 0},
                           {1.0f / float(w), 1.0f / float(h), 0, 0}, {0.5f, 0, -0.5f, 0}, {-rx, -ry, 0, 0}};
    d->SetPixelShaderConstantF(0, &c[0][0], 6);
    // Pixel centres on the output's (D3D9's half-pixel offset).
    const float x0 = float(dst.left) - 0.5f, y0 = float(dst.top) - 0.5f, x1 = x0 + dw, y1 = y0 + dh;
    const float q[4][6] = {{x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0}, {x0, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    Gpu& gpu = dd_->GetGpu();
    gpu.OpenScene();
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
    gpu.CloseScene();
    d->SetPixelShader(nullptr);
    SetTexture9(d, 0, nullptr);
    return true;
}

// One draw call each frame, which draws nothing: RivaTuner (and other overlay tools) draws its
// overlay only into frames that had one, and a 2D game's are only copied into the back buffer
// (StretchRect): it showed in Direct3D games and with filter=sharp (a draw), not otherwise.
// A triangle of no area, colour writes off. Gpu lock held, outside a scene.
static void DrawCall(Gpu& gpu, IDirect3DDevice9* d) {
    const SavedDeviceState saved(d);  // put back at the return
    IDirect3DSurface9* bb = gpu.TargetBackBuffer();
    if (!bb) return;
    d->SetRenderTarget(0, bb);
    bb->Release();
    d->SetDepthStencilSurface(nullptr);
    d->SetPixelShader(nullptr);
    d->SetVertexShader(nullptr);
    SetFvf9(d, D3DFVF_XYZRHW);
    SetTexture9(d, 0, nullptr);
    const DWORD rs[][2] = {{D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE}, {D3DRS_CULLMODE, D3DCULL_NONE},
                           {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE}, {D3DRS_STENCILENABLE, FALSE},
                           {D3DRS_COLORWRITEENABLE, 0}, {D3DRS_CLIPPLANEENABLE, 0}};
    for (const auto& r : rs) d->SetRenderState(D3DRENDERSTATETYPE(r[0]), r[1]);
    const float t[3][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
    gpu.OpenScene();
    d->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, t, sizeof(t[0]));
    gpu.CloseScene();
}

RECT Presenter::FitRect(UINT sw, UINT sh, UINT bw, UINT bh) const {
    if (g_config.stretch == 1 || (sw == bw && sh == bh)) return RECT{0, 0, LONG(bw), LONG(bh)};
    double scale = std::fmin(double(bw) / sw, double(bh) / sh);
    if (g_config.stretch == 2 && scale >= 1.0) scale = std::floor(scale);
    LONG w = LONG(sw * scale + 0.5), h = LONG(sh * scale + 0.5);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    LONG x = (LONG(bw) - w) / 2, y = (LONG(bh) - h) / 2;
    return RECT{x, y, x + w, y + h};
}

static double Seconds() {
    static const double period = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / double(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * period;
}

// lockSeconds: time g_lock was held (the game's Blt / Flip / Lock wait for it);
// convertSeconds: time spent converting pixels; fraction: share of the frame converted.
void Presenter::CountPresent(double lockSeconds, double convertSeconds, double fraction) {
    const double now = Seconds();
    if (statsPresents_ == 0) statsStart_ = now;
    statsPresents_++;
    statsLock_ += lockSeconds;
    statsLockMax_ = std::max(statsLockMax_, lockSeconds);
    statsConvert_ += convertSeconds;
    statsFraction_ += fraction;
    if (now - statsStart_ < 10.0) return;
    const double n = statsPresents_;
    DD2D9_LOG("presenter: %u presents in %.1f s; converted %.1f%% of the frame, in %.2f ms; game lock held "
              "%.2f ms on average, %.2f ms at most",
              statsPresents_, now - statsStart_, statsFraction_ / n * 100.0, statsConvert_ / n * 1e3,
              statsLock_ / n * 1e3, statsLockMax_ * 1e3);
    statsPresents_ = 0;
    statsLock_ = statsLockMax_ = statsConvert_ = statsFraction_ = 0;
}

// ---------------------------------------------------------------- conversion

// The presenter thread's (the only one using lut16_), from a copy of the gamma ramp taken
// under gammaLock_: built without it, so a game setting its ramp every frame (a fade) doesn't
// wait for the 65536 conversions.
void Presenter::BuildLut16(const Format& f, const uint8_t (&gamma)[3][256], DWORD gammaVersion) {
    lut16_.resize(65536);
    if (f.pal8) {
        for (uint32_t v = 0; v < 65536; v++) {
            uint32_t c = PixelToARGB(f, v, nullptr);
            lut16_[v] = (uint32_t(gamma[0][(c >> 16) & 0xFF]) << 16) | (uint32_t(gamma[1][(c >> 8) & 0xFF]) << 8) |
                        gamma[2][c & 0xFF];
        }
    } else {
        // Each channel PixelToARGB gives depends on that channel's bits alone: a table per
        // channel (its values, through the gamma ramp), then three lookups an entry, not 65536
        // conversions of three divisions each (a fade sets a new ramp every frame).
        const DWORD masks[3] = {f.r, f.g, f.b};
        std::vector<uint32_t> part[3];
        int shift[3];
        for (int c = 0; c < 3; c++) {
            const uint32_t m = masks[c] & 0xFFFF;
            shift[c] = m ? __builtin_ctz(m) : 0;
            const uint32_t max = m >> shift[c];
            part[c].resize(max + 1);
            for (uint32_t k = 0; k <= max; k++) {
                const uint32_t argb = PixelToARGB(f, (k << shift[c]) & m, nullptr);
                part[c][k] = uint32_t(gamma[c][(argb >> (16 - 8 * c)) & 0xFF]) << (16 - 8 * c);
            }
        }
        const uint32_t mr = f.r & 0xFFFF, mg = f.g & 0xFFFF, mb = f.b & 0xFFFF;
        const uint32_t *pr = part[0].data(), *pg = part[1].data(), *pb = part[2].data();
        for (uint32_t v = 0; v < 65536; v++)
            lut16_[v] = pr[(v & mr) >> shift[0]] | pg[(v & mg) >> shift[1]] | pb[(v & mb) >> shift[2]];
    }
    lut16Fmt_ = f;
    lut16GammaVersion_ = gammaVersion;
}

// Converts `area` of the snapshot (primary surface coordinates; parts outside
// the surface are black) to X8R8G8B8 at `bits`, the staging surface at the
// area's top-left corner. Runs without g_lock: if the game draws on this
// buffer meanwhile, the frame may tear, and the next present shows it whole.
void Presenter::Convert(const Snapshot& s, const RECT& area, void* bits, int pitch) {
    const Buffer* b = s.buf;
    const Format& f = s.fmt;
    const int aw = area.right - area.left, ah = area.bottom - area.top;
    const int x0 = std::max<int>(area.left, 0), x1 = std::min<int>(area.right, b->w);

    uint8_t gamma[3][256];
    EnterCriticalSection(&gammaLock_);
    memcpy(gamma, gammaLut_, sizeof(gamma));
    const bool gammaOn = gammaActive_;
    const DWORD gammaVersion = gammaVersion_;
    LeaveCriticalSection(&gammaLock_);
    if (f.bpp == 16 && (lut16_.empty() || lut16Fmt_ != f || lut16GammaVersion_ != gammaVersion)) BuildLut16(f, gamma, gammaVersion);
    uint32_t lut8[256];
    if (f.pal8) {
        for (int i = 0; i < 256; i++)
            lut8[i] = (uint32_t(gamma[0][s.pal[i].peRed]) << 16) | (uint32_t(gamma[1][s.pal[i].peGreen]) << 8) |
                      gamma[2][s.pal[i].peBlue];
    }
    const bool fastCopy = f.IsStandardX8R8G8B8() && !gammaOn;
    const RowConverter conv(f, nullptr);

    for (int y = 0; y < ah; y++) {
        auto* out = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(bits) + size_t(y) * pitch);
        const int py = area.top + y;
        if (py < 0 || py >= b->h || x0 >= x1) {
            memset(out, 0, size_t(aw) * 4);
            continue;
        }
        if (x0 > area.left) memset(out, 0, size_t(x0 - area.left) * 4);
        if (x1 < area.right) memset(out + (x1 - area.left), 0, size_t(area.right - x1) * 4);
        uint32_t* o = out + (x0 - area.left);
        const uint8_t* row = b->Row(py);
        const int n = x1 - x0;
        if (f.pal8) {
            const uint8_t* src = row + x0;
            for (int i = 0; i < n; i++) o[i] = lut8[src[i]];
        } else if (f.bpp == 16) {  // the table: as fast as SSE2 here (measured), and it has the gamma in
            const uint16_t* src = reinterpret_cast<const uint16_t*>(row) + x0;
            for (int i = 0; i < n; i++) o[i] = lut16_[src[i]];
        } else if (fastCopy) {
            memcpy(o, reinterpret_cast<const uint32_t*>(row) + x0, size_t(n) * 4);
        } else {  // 24 and 32-bit: RowConverter, the gamma after
            conv.Run(row + x0 * int(f.Bytes()), o, n);
            if (gammaOn) {
                for (int i = 0; i < n; i++) {
                    const uint32_t c = o[i];
                    o[i] = (uint32_t(gamma[0][(c >> 16) & 0xFF]) << 16) | (uint32_t(gamma[1][(c >> 8) & 0xFF]) << 8) |
                           gamma[2][c & 0xFF];
                }
            } else {
                for (int i = 0; i < n; i++) o[i] &= 0x00FFFFFFu;
            }
        }
    }
}

// ---------------------------------------------------------------- render loop

bool Presenter::CaptureGdi(HWND hwnd, LONG w, LONG h) {
    const double now = Seconds();
    const bool have = hwnd == gdiWnd_ && w == gdiW_ && h == gdiH_;
    // At most every 0.1 s, answered or not (a window slow to answer costs up to 50 ms a try).
    if (now - gdiTime_ < 0.1) return have;
    gdiTime_ = now;
    if (w <= 0 || h <= 0) return false;
    if (!gdiDc_ || gdiDcW_ != w || gdiDcH_ != h) {
        // A new size. The old DC goes unless a timed-out WM_PRINT may still paint it (then it
        // is left: rare, and a crash in the game's painting would be worse).
        if (gdiDc_ && !gdiPending_) {
            SelectObject(gdiDc_, gdiOld_);
            DeleteObject(gdiBm_);
            DeleteDC(gdiDc_);
        }
        gdiDc_ = nullptr;
        gdiBm_ = nullptr;
        gdiPending_ = false;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = CreateCompatibleDC(nullptr);
        void* bits = nullptr;
        HBITMAP bm = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (!bm) {
            if (dc) DeleteDC(dc);
            return false;
        }
        gdiOld_ = SelectObject(dc, bm);
        gdiDc_ = dc;
        gdiBm_ = bm;
        gdiBits_ = bits;
        gdiDcW_ = w;
        gdiDcH_ = h;
    }
    // The window paints itself into the DC on its own thread (background, client, child
    // windows); one that doesn't answer in time keeps its last picture, if any.
    DWORD_PTR result = 0;
    const bool ok = SendMessageTimeoutW(hwnd, WM_PRINT, reinterpret_cast<WPARAM>(gdiDc_),
                                        PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN, SMTO_ABORTIFHUNG, 50, &result) != 0;
    gdiPending_ = !ok;
    if (!ok) return have;
    GdiFlush();
    const uint32_t* p = static_cast<const uint32_t*>(gdiBits_);
    gdi_.assign(p, p + size_t(w) * h);
    gdiSeq_++;
    gdiWnd_ = hwnd;
    gdiW_ = w;
    gdiH_ = h;
    return true;
}

void Presenter::Loop() {
    while (running_) {
        // Also refresh periodically (window exposed, moved...). D3D9 may create
        // windows on the thread that owns a device, so keep its messages flowing.
        MsgWaitForMultipleObjects(1, &dirtyEvent_, FALSE, 250, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (!running_) break;
        if (dropRequest_) {
            LeaveGpu();
            dropRequest_ = false;
            SetEvent(dropDone_);
            continue;
        }

        // When there is nothing to show (no primary, no window, minimized), mark the
        // presenter inactive so Flip() doesn't wait for presents that won't come.
        // Only those paths clear it: Flip() checks it right after waking us.
        HWND hwnd = nullptr;
        RECT src = {};
        RECT target = {};  // where the image goes in the back buffer (client coordinates)
        bool havePrimary;
        bool superseded;
        {
            Guard l;
            // In the background (focus lost) nothing is shown, and no device may be
            // created: the window is about to be minimized.
            // A game that made a new DirectDraw object without releasing this one
            // shows that one's primary: two presenters would fight over the window.
            // A lost primary (the display mode changed, or the game left exclusive mode
            // and went on with another DirectDraw object) holds nothing current until
            // the game restores it: showing it would cover what the game draws now.
            superseded = dd_->Superseded();
            havePrimary = dd_->primary != nullptr && !dd_->primary->IsLost() && !dd_->Suspended() && !superseded;
            if (havePrimary) hwnd = dd_->PresentWindow();
            if (hwnd) src = dd_->PresentSource(hwnd);
            if (hwnd) target = dd_->PresentTarget(hwnd);
        }
        if (!havePrimary || !hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) {
            if (superseded) LeaveGpu();  // the window belongs to the other object's device
            ReleaseWindows(this);  // showing nothing: the others' frames refresh again
            active_ = false;
            stagingStale_ = true;  // the device may go meanwhile: start over with a full frame
            continue;
        }
        RECT cr;
        GetClientRect(hwnd, &cr);
        const UINT bw = UINT(std::max<LONG>(cr.right, 1)), bh = UINT(std::max<LONG>(cr.bottom, 1));
        const UINT sw = UINT(src.right - src.left), sh = UINT(src.bottom - src.top);
        if (int(sw) <= 0 || int(sh) <= 0) {
            active_ = false;
            continue;
        }
        // The shared device, presenting into the window (see Gpu::PrepareTarget).
        bool ready;
        {
            Gpu& gpu = dd_->GetGpu();
            GpuGuard g(gpu);
            if (!usingGpu_) {
                gpu.AddOwner(this);
                usingGpu_ = true;
                stagingStale_ = true;
            }
            gpu.SetWindowHint(hwnd);
            IDirect3DDevice9* d = gpu.Ensure();
            ready = d && gpu.PrepareTarget(hwnd, bw, bh) && EnsureStaging(d, sw, sh);
        }
        if (!ready) {
            active_ = false;
            Sleep(50);
            continue;
        }
        // Creating the device takes a while: if meanwhile a 3D device appeared,
        // the game lost focus or the device is to be dropped, don't present.
        bool supersededNow;
        {
            Guard l;
            supersededNow = dd_->Superseded();
        }
        if (dropRequest_ || dd_->Suspended() || supersededNow) {
            SetEvent(dirtyEvent_);  // handle it right away in the next iteration
            continue;
        }

        // Under g_lock, only find out what changed and take a reference to the
        // pixels: they are converted without it, so the game's Blt, Lock and Flip
        // never wait for the conversion.
        Snapshot snap;
        std::vector<OverlayLayer*> overlays;  // shown over the primary (overlay.cpp)
        Buffer* keyPrimary = nullptr;  // the primary's pixels, for windowed destination keys (GPU path)
        struct Drop {  // both let go at the end of the iteration, however it ends
            std::vector<OverlayLayer*>& o;
            Buffer*& k;
            ~Drop() {
                DropOverlays(&o);
                if (k) k->Release();
            }
        } dropOverlays{overlays, keyPrimary};
        bool cpuOverlays = false;  // overlays laid over the frame on the CPU (no pixel shaders)
        bool gpuFrame = false;     // snap.buf's render target is the newest copy: presented from it
        RECT area = {0, 0, 0, 0};  // what to convert, in primary surface coordinates
        uint64_t seq = 0;
        bool primaryExists = false;
        bool windowed = false;  // overlays' destination keys also see what GDI drew (CaptureGdi)
        double lockTime;
        {
            Guard l;
            const double t0 = Seconds();
            Surface* p = dd_->primary;
            if (p) {
                primaryExists = true;
                seq = ++convertSeq_;
                RECT changed;
                bool all = dd_->TakeDirty(&changed);
                // The staging surface holds the last converted frame: a partial update
                // needs it to show this very surface and buffer, at the same position.
                all = all || stagingStale_ || !EqualRect(&src, &lastSrc_) || p != lastPrimary_ || p->buf != lastBuf_;
                // Overlays shown on it: drawn over the frame in the back buffer (DrawOverlaysGpu),
                // or without pixel shaders laid over the whole frame on the CPU, converted whole
                // (and the next frame too, see stagingStale_ below: what they covered comes back).
                TakeOverlays(p, &overlays);
                windowed = !dd_->Exclusive();
                cpuOverlays = !overlays.empty() && !OverlayGpuUsable(ovGpu_);
                if (cpuOverlays) all = true;
                if (!overlays.empty() && !cpuOverlays && windowed && OverlaysKeyedOnDest(overlays)) {
                    keyPrimary = p->buf;
                    keyPrimary->AddRef();
                }
                if (p->dcOut) all = true;  // GDI may be drawing anywhere
                else if (p->lockCount) UnionRect(&changed, &changed, &p->lockArea);  // being written right now
                if (all) area = src;
                else if (!IntersectRect(&area, &changed, &src)) SetRectEmpty(&area);
                // Direct3D drew into it since the CPU copy was made: shown straight from
                // its render target, the whole frame (the staging surface no longer holds it).
                // A palettized one goes through its CPU copy (read back now): the palette is
                // applied there. So does a source rectangle outside the surface (a window
                // partly off the screen), which StretchRect would refuse.
                const bool inside = src.left >= 0 && src.top >= 0 && src.right <= p->buf->w && src.bottom <= p->buf->h;
                // So does one with overlays laid over it on the CPU.
                if (!IsRectEmpty(&area) && p->buf->gpu && p->buf->cpuStale &&
                    (p->buf->fmt.pal8 || !inside || cpuOverlays)) {
                    GpuCpuAccess(p, false);
                    area = src;
                }
                if (!IsRectEmpty(&area) && p->buf->gpu && p->buf->cpuStale) {
                    gpuFrame = true;
                    area = src;
                    stagingStale_ = true;
                }
                if (!IsRectEmpty(&area)) {
                    snap.buf = p->buf;
                    snap.buf->AddRef();
                    snap.fmt = snap.buf->fmt;
                    if (snap.fmt.pal8) {
                        const Palette* sp = p->ShownPalette();
                        memcpy(snap.pal, sp ? sp->entries : dd_->defaultPalette, sizeof(snap.pal));
                    }
                }
                lastSrc_ = src;
                lastPrimary_ = p;
                lastBuf_ = p->buf;
                if (!gpuFrame) stagingStale_ = cpuOverlays;
                DD2D9_TRACE("present #%llu: primary %p, area %ld,%ld-%ld,%ld, palette %p v%lu",
                           static_cast<unsigned long long>(seq), static_cast<void*>(p), area.left, area.top,
                           area.right, area.bottom, static_cast<const void*>(p->ShownPalette()),
                           p->ShownPalette() ? p->ShownPalette()->version : 0);
            }
            lockTime = Seconds() - t0;
        }
        if (!primaryExists) continue;
        // A video Windows' player draws into the window (video.cpp): an unchanged frame isn't
        // presented again over it (the refresh four times a second would blank it).
        // Nor is it over another object's newer frame in the same window (see ClaimWindow).
        if (IsRectEmpty(&area) && !snap.buf && overlays.empty() && (VideoOnWindow(hwnd) || OtherShowsWindow(hwnd, this))) {
            AcquireSRWLockExclusive(&seqLock_);
            presentedSeq_ = seq;
            ReleaseSRWLockExclusive(&seqLock_);
            WakeAllConditionVariable(&seqCv_);
            continue;
        }
        // The CPU frame converted here, before the shared device is taken: the game's
        // Direct3D calls wait for that lock, and conversion is the slow part (the lock then
        // covers a row copy into the staging surface only). frame_ holds the last converted
        // frame, as staging_ does: a new size converts it whole.
        double convertTime = 0;
        if (frameW_ != sw || frameH_ != sh) {
            frame_.assign(size_t(sw) * sh, 0);
            frameW_ = sw;
            frameH_ = sh;
            if (snap.buf) area = src;  // (the reference to the pixels is held: all of them)
        }
        if (snap.buf && !gpuFrame) {
            const double t0 = Seconds();
            Convert(snap, area, frame_.data() + size_t(area.top - src.top) * sw + (area.left - src.left), int(sw * 4));
            if (cpuOverlays) {
                OverlayGdi g = {nullptr, 0, 0, src.left, src.top};
                if (windowed && OverlaysKeyedOnDest(overlays) && CaptureGdi(hwnd, LONG(sw), LONG(sh))) {
                    g.px = gdi_.data();
                    g.w = gdiW_;
                    g.h = gdiH_;
                }
                DrawOverlays(overlays, snap.buf, src, frame_.data(), sw, windowed ? &g : nullptr);
            }
            convertTime = Seconds() - t0;
        }
        // The GPU path: what changed in the overlays decoded now, without the device's lock.
        OverlayGdi ovGdi = {nullptr, 0, 0, src.left, src.top};
        if (!overlays.empty() && !cpuOverlays) {
            if (keyPrimary && CaptureGdi(hwnd, LONG(sw), LONG(sh))) {
                ovGdi.px = gdi_.data();
                ovGdi.w = gdiW_;
                ovGdi.h = gdiH_;
            }
            PrepareOverlays(ovGpu_, overlays, keyPrimary, src, keyPrimary ? &ovGdi : nullptr);
        }
        // The shared device again (taken after g_lock, never the other way round: the game's
        // Direct3D calls hold g_lock and then take it). It may have been lost meanwhile,
        // taking the staging surface with it.
        Gpu& gpu = dd_->GetGpu();
        gpu.Lock();
        gpu.CloseScene();  // a game's Direct3D scene: D3D9 presents only outside one
        IDirect3DDevice9* const d = gpu.Device();
        if (!d || !staging_ || !gpu.PrepareTarget(hwnd, bw, bh)) {
            gpu.Unlock();
            if (snap.buf) snap.buf->Release();
            stagingStale_ = true;
            active_ = false;
            continue;
        }
        // The frame's source: the render target as it is, or the staging surface the CPU copy
        // is converted into (also when the render target went meanwhile: read back then).
        IDirect3DSurface9* source = staging_;
        IDirect3DSurface9* heldRt = nullptr;
        RECT sourceRect = {0, 0, LONG(sw), LONG(sh)};
        // The frame as converted: what overlays' destination keys are compared with (the
        // staging surface has the gamma ramp in: the key gets it too).
        IDirect3DSurface9* keySrc = staging_;
        RECT keyRect = sourceRect;
        uint8_t keyGamma[3][256];
        bool keyGammaOn = false;
        if (!overlays.empty()) {
            EnterCriticalSection(&gammaLock_);
            keyGammaOn = gammaActive_;
            if (keyGammaOn) memcpy(keyGamma, gammaLut_, sizeof(keyGamma));
            LeaveCriticalSection(&gammaLock_);
        }
        if (gpuFrame && snap.buf) {
            if (IDirect3DSurface9* rt = GpuNewestSurface(snap.buf)) {
                rt->AddRef();  // snap.buf may hold the last reference to it: released after Present
                heldRt = rt;
                source = rt;
                sourceRect = src;
                keySrc = rt;
                keyRect = src;
                keyGammaOn = false;  // (before ApplyGammaGpu)
                EnterCriticalSection(&gammaLock_);
                const bool gammaOn = gammaActive_;
                LeaveCriticalSection(&gammaLock_);
                if (gammaOn) {
                    if (IDirect3DSurface9* g = ApplyGammaGpu(d, rt, src)) {
                        source = g;
                        sourceRect = {0, 0, src.right - src.left, src.bottom - src.top};
                    }
                }
                snap.buf->Release();
                snap.buf = nullptr;
            }
        }
        if (snap.buf) {
            // (A GPU frame whose render target went meanwhile: converted only now.)
            if (gpuFrame) {
                const double t0 = Seconds();
                Convert(snap, area, frame_.data() + size_t(area.top - src.top) * sw + (area.left - src.left), int(sw * 4));
                convertTime = Seconds() - t0;
            }
            RECT dst = {area.left - src.left, area.top - src.top, area.right - src.left, area.bottom - src.top};
            D3DLOCKED_RECT lr;
            if (!IsRectEmpty(&dst) && SUCCEEDED(staging_->LockRect(&lr, &dst, 0))) {
                const size_t rowBytes = size_t(dst.right - dst.left) * 4;
                for (LONG y = dst.top; y < dst.bottom; y++)
                    memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(y - dst.top) * lr.Pitch,
                           frame_.data() + size_t(y) * sw + dst.left, rowBytes);
                staging_->UnlockRect();
            } else if (!IsRectEmpty(&dst)) {
                stagingStale_ = true;  // convert everything next time
            }
            snap.buf->Release();
        }
        CountPresent(lockTime, convertTime,
                     double(area.right - area.left) * (area.bottom - area.top) / (double(sw) * sh));

        if (IDirect3DSurface9* bb = gpu.TargetBackBuffer()) {
            // Fitted into the target area of the back buffer (the client area's part on the
            // screen, see DDraw::PresentTarget).
            RECT tg;
            const RECT whole = {0, 0, LONG(bw), LONG(bh)};
            if (!IntersectRect(&tg, &target, &whole)) tg = whole;
            RECT dst = FitRect(sw, sh, UINT(tg.right - tg.left), UINT(tg.bottom - tg.top));
            OffsetRect(&dst, tg.left, tg.top);
            // ColorFill rather than Clear: Clear works on the device's render target, which the
            // back buffer needn't be. Only the frame's part (the device's back buffer is larger).
            if (dst.left > 0 || dst.top > 0 || dst.right < LONG(bw) || dst.bottom < LONG(bh))
                d->ColorFill(bb, &whole, D3DCOLOR_XRGB(0, 0, 0));
            const UINT dw = UINT(dst.right - dst.left), dh = UINT(dst.bottom - dst.top);
            const bool same = dw == sw && dh == sh;
            if (same || g_config.filter == 0) {
                d->StretchRect(source, &sourceRect, bb, &dst, D3DTEXF_POINT);
            } else if (g_config.filter == 2 && dw > sw && dh > sh && DrawSharp(d, source, sourceRect, bb, dst)) {
                // Sharp bilinear: drawn.
            } else if (g_config.filter == 2 && dw > sw && dh > sh &&
                       EnsurePrescaled(d, sw * ((std::max(dw, dh * sw / sh) + sw - 1) / sw),
                                       sh * ((std::max(dw, dh * sw / sh) + sw - 1) / sw))) {
                // Without pixel shaders: enlarged with point sampling by the whole factor at
                // or above the scale (sharp pixels), then linear down to the size: only the
                // pixel edges blend, as near as StretchRect gets to the shader.
                d->StretchRect(source, &sourceRect, prescaled_, nullptr, D3DTEXF_POINT);
                d->StretchRect(prescaled_, nullptr, bb, &dst, D3DTEXF_LINEAR);
            } else {
                d->StretchRect(source, &sourceRect, bb, &dst, D3DTEXF_LINEAR);
            }
            if (!overlays.empty() && !cpuOverlays &&
                !DrawOverlaysGpu(ovGpu_, gpu, d, overlays, bb, src, dst, keySrc, keyRect, keyPrimary ? &ovGdi : nullptr,
                                 gdiSeq_, keyGammaOn ? keyGamma : nullptr))
                MarkDirty();  // no pixel shaders after all: the CPU path in the next frame
            bb->Release();
        }
        DrawCall(gpu, d);
        const HRESULT hr = gpu.PresentTarget();
        if (heldRt) heldRt->Release();
        gpu.Unlock();
        active_ = SUCCEEDED(hr);
        if (FAILED(hr)) DD2D9_LOG("Present failed: 0x%08lx", hr);
        else if (!IsRectEmpty(&area)) ClaimWindow(hwnd, this);  // a changed frame: the window shows this one

        AcquireSRWLockExclusive(&seqLock_);
        presentedSeq_ = seq;
        ReleaseSRWLockExclusive(&seqLock_);
        WakeAllConditionVariable(&seqCv_);
    }
    active_ = false;
    LeaveGpu();
}

}  // namespace dd2d9
