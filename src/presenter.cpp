// Direct3D 9 presentation of the primary surface.
#include "objects.h"

#include <cmath>

namespace dd2d9 {

Presenter::Presenter(DDraw* dd) : dd_(dd) {
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

void Presenter::GetGamma(DDGAMMARAMP* ramp) {
    EnterCriticalSection(&gammaLock_);
    *ramp = gammaRamp_;
    LeaveCriticalSection(&gammaLock_);
}

// ---------------------------------------------------------------- device

void Presenter::ReleaseDevice() {
    if (staging_) staging_->Release();
    staging_ = nullptr;
    stW_ = stH_ = 0;
    if (dev_) dev_->Release();
    dev_ = nullptr;
    if (d3d_) d3d_->Release();
    d3d_ = nullptr;
    devHwnd_ = nullptr;
    devFullscreen_ = false;
    active_ = false;
}

// fullscreen: a D3D9 exclusive-fullscreen device at the (already switched)
// display mode; otherwise a windowed device sized to the window's client area.
bool Presenter::EnsureDevice(HWND hwnd, UINT w, UINT h, bool fullscreen, UINT refresh) {
    if (dev_ && (devHwnd_ != hwnd || devFullscreen_ != fullscreen)) ReleaseDevice();

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = fullscreen ? FALSE : TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = w;
    pp.BackBufferHeight = h;
    pp.BackBufferCount = 1;
    pp.hDeviceWindow = hwnd;
    pp.FullScreen_RefreshRateInHz = fullscreen ? refresh : 0;
    pp.PresentationInterval = g_config.vsync == kVSyncOff ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;

    if (!dev_) {
        if (!d3d_) d3d_ = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d3d_) {
            DD2D9_LOG("Direct3DCreate9 failed");
            return false;
        }
        HRESULT hr = d3d_->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE |
                                            D3DCREATE_NOWINDOWCHANGES,  // the game's window is ours to manage
                                        &pp, &dev_);
        if (FAILED(hr)) {
            dev_ = nullptr;
            if (fullscreen) {
                DD2D9_LOG("exclusive fullscreen device (%ux%u @ %u Hz) failed: 0x%08lx; presenting in a window", w,
                          h, refresh, hr);
                exclusiveFailed_ = true;
            } else {
                DD2D9_LOG("CreateDevice failed: 0x%08lx", hr);
            }
            return false;
        }
        devHwnd_ = hwnd;
        devFullscreen_ = fullscreen;
        bbW_ = w;
        bbH_ = h;
        DD2D9_LOG("D3D9 device created for window %p, %ux%u%s", static_cast<void*>(hwnd), w, h,
                  fullscreen ? " (exclusive fullscreen)" : "");
        return true;
    }

    // A fullscreen device is lost while the game is in the background, and
    // needs a Reset when it comes back.
    HRESULT coop = dev_->TestCooperativeLevel();
    if (coop == D3DERR_DEVICELOST) return false;
    if (coop == D3DERR_DEVICENOTRESET || w != bbW_ || h != bbH_) {
        if (staging_) staging_->Release();
        staging_ = nullptr;
        stW_ = stH_ = 0;
        HRESULT hr = dev_->Reset(&pp);
        if (hr == D3DERR_DEVICELOST) return false;  // lost again before we got to it: retry later
        if (FAILED(hr)) {
            DD2D9_LOG("Reset failed: 0x%08lx", hr);
            if (fullscreen) exclusiveFailed_ = true;
            ReleaseDevice();
            return false;
        }
        bbW_ = w;
        bbH_ = h;
    }
    return true;
}

bool Presenter::EnsureStaging(UINT w, UINT h) {
    if (staging_ && stW_ == w && stH_ == h) return true;
    if (staging_) staging_->Release();
    staging_ = nullptr;
    HRESULT hr = dev_->CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &staging_, nullptr);
    if (FAILED(hr)) {
        DD2D9_LOG("CreateOffscreenPlainSurface(%u, %u) failed: 0x%08lx", w, h, hr);
        staging_ = nullptr;
        return false;
    }
    stW_ = w;
    stH_ = h;
    return true;
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

// ---------------------------------------------------------------- conversion

void Presenter::BuildLut16(const Format& f) {
    lut16_.resize(65536);
    for (uint32_t v = 0; v < 65536; v++) {
        uint32_t c = PixelToARGB(f, v, nullptr);
        lut16_[v] = (uint32_t(gammaLut_[0][(c >> 16) & 0xFF]) << 16) | (uint32_t(gammaLut_[1][(c >> 8) & 0xFF]) << 8) |
                    gammaLut_[2][c & 0xFF];
    }
    lut16Fmt_ = f;
    lut16GammaVersion_ = gammaVersion_;
}

// Called with g_lock held: copies the visible part of the primary surface.
void Presenter::Convert(Surface* p, const RECT& src, const D3DLOCKED_RECT& lr) {
    Buffer* b = p->buf;
    const Format& f = b->fmt;
    const int sw = src.right - src.left, sh = src.bottom - src.top;
    const int x0 = std::max<int>(src.left, 0), x1 = std::min<int>(src.right, b->w);

    EnterCriticalSection(&gammaLock_);
    uint32_t lut8[256];
    if (f.pal8) {
        const PALETTEENTRY* pal = p->palette ? p->palette->entries : dd_->defaultPalette;
        for (int i = 0; i < 256; i++)
            lut8[i] = (uint32_t(gammaLut_[0][pal[i].peRed]) << 16) | (uint32_t(gammaLut_[1][pal[i].peGreen]) << 8) |
                      gammaLut_[2][pal[i].peBlue];
    } else if (f.bpp == 16 && (lut16_.empty() || lut16Fmt_ != f || lut16GammaVersion_ != gammaVersion_)) {
        BuildLut16(f);
    }
    const bool gamma = gammaActive_;
    const bool fastCopy = f.IsStandardX8R8G8B8() && !gamma;

    for (int y = 0; y < sh; y++) {
        auto* out = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch);
        const int py = src.top + y;
        if (py < 0 || py >= b->h || x0 >= x1) {
            memset(out, 0, size_t(sw) * 4);
            continue;
        }
        if (x0 > src.left) memset(out, 0, size_t(x0 - src.left) * 4);
        if (x1 < src.right) memset(out + (x1 - src.left), 0, size_t(src.right - x1) * 4);
        uint32_t* o = out + (x0 - src.left);
        const uint8_t* row = b->Row(py);
        const int n = x1 - x0;
        if (f.pal8) {
            const uint8_t* s = row + x0;
            for (int i = 0; i < n; i++) o[i] = lut8[s[i]];
        } else if (f.bpp == 16) {
            const uint16_t* s = reinterpret_cast<const uint16_t*>(row) + x0;
            for (int i = 0; i < n; i++) o[i] = lut16_[s[i]];
        } else if (fastCopy) {
            memcpy(o, reinterpret_cast<const uint32_t*>(row) + x0, size_t(n) * 4);
        } else {
            const int bytes = int(f.Bytes());
            const uint8_t* s = row + x0 * bytes;
            for (int i = 0; i < n; i++, s += bytes) {
                uint32_t v = bytes == 3 ? (uint32_t(s[0]) | (uint32_t(s[1]) << 8) | (uint32_t(s[2]) << 16))
                                        : *reinterpret_cast<const uint32_t*>(s);
                uint32_t c = PixelToARGB(f, v, nullptr);
                o[i] = (uint32_t(gammaLut_[0][(c >> 16) & 0xFF]) << 16) |
                       (uint32_t(gammaLut_[1][(c >> 8) & 0xFF]) << 8) | gammaLut_[2][c & 0xFF];
            }
        }
    }
    LeaveCriticalSection(&gammaLock_);
}

// ---------------------------------------------------------------- render loop

void Presenter::Loop() {
    while (running_) {
        // Also refresh periodically (window exposed, moved...). D3D9 may create
        // windows on the thread that owns a device, so keep its messages flowing.
        MsgWaitForMultipleObjects(1, &dirtyEvent_, FALSE, 250, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        if (!running_) break;
        if (dropRequest_) {
            ReleaseDevice();
            dropRequest_ = false;
            SetEvent(dropDone_);
            continue;
        }

        // When there is nothing to show (no primary, no window, minimized), mark the
        // presenter inactive so Flip() doesn't wait for presents that won't come.
        // Only those paths clear it: Flip() checks it right after waking us.
        HWND hwnd = nullptr;
        RECT src = {};
        bool havePrimary;
        bool fullscreen = false;
        UINT fw = 0, fh = 0, fhz = 0;
        {
            Guard l;
            // In the background (focus lost) nothing is shown, and no device may be
            // created: the window is about to be minimized.
            havePrimary = dd_->primary != nullptr && !dd_->Suspended();
            if (havePrimary) hwnd = dd_->PresentWindow();
            if (hwnd) src = dd_->PresentSource(hwnd);
            if (hwnd && !exclusiveFailed_) fullscreen = dd_->ExclusivePresent(&fw, &fh, &fhz);
        }
        if (!havePrimary || !hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) {
            active_ = false;
            continue;
        }
        UINT bw = fw, bh = fh;
        if (!fullscreen) {
            RECT cr;
            GetClientRect(hwnd, &cr);
            bw = UINT(std::max<LONG>(cr.right, 1));
            bh = UINT(std::max<LONG>(cr.bottom, 1));
        }
        const UINT sw = UINT(src.right - src.left), sh = UINT(src.bottom - src.top);
        if (int(sw) <= 0 || int(sh) <= 0) {
            active_ = false;
            continue;
        }
        if (!EnsureDevice(hwnd, bw, bh, fullscreen, fhz) || !EnsureStaging(sw, sh)) {
            active_ = false;
            Sleep(50);
            continue;
        }

        // Lock the D3D surface before taking g_lock: the lock may wait for the
        // GPU, and the game must not wait for that in every Blt and Flip.
        D3DLOCKED_RECT lr;
        if (FAILED(staging_->LockRect(&lr, nullptr, 0))) continue;
        uint64_t seq = 0;
        bool converted = false;
        {
            Guard l;
            if (dd_->primary) {
                Convert(dd_->primary, src, lr);
                seq = ++convertSeq_;
                converted = true;
                DD2D9_TRACE("present #%llu: primary %p, palette %p v%lu", static_cast<unsigned long long>(seq),
                           static_cast<void*>(dd_->primary), static_cast<void*>(dd_->primary->palette),
                           dd_->primary->palette ? dd_->primary->palette->version : 0);
            }
        }
        staging_->UnlockRect();
        if (!converted) continue;

        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev_->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            RECT dst = FitRect(sw, sh, bw, bh);
            if (dst.left > 0 || dst.top > 0 || dst.right < LONG(bw) || dst.bottom < LONG(bh))
                dev_->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
            const bool same = UINT(dst.right - dst.left) == sw && UINT(dst.bottom - dst.top) == sh;
            dev_->StretchRect(staging_, nullptr, bb, &dst,
                              same || g_config.filter == 0 ? D3DTEXF_POINT : D3DTEXF_LINEAR);
            bb->Release();
        }
        HRESULT hr = dev_->Present(nullptr, nullptr, nullptr, nullptr);
        active_ = SUCCEEDED(hr);
        if (FAILED(hr)) DD2D9_LOG("Present failed: 0x%08lx", hr);

        AcquireSRWLockExclusive(&seqLock_);
        presentedSeq_ = seq;
        ReleaseSRWLockExclusive(&seqLock_);
        WakeAllConditionVariable(&seqCv_);
    }
    active_ = false;
    ReleaseDevice();
}

}  // namespace dd2d9
