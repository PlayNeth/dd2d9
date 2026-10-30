// Implementation objects. COM interface wrappers (one per interface version)
// live in wrappers.h and forward to these.
#pragma once
#include "common.h"
#include "gpu.h"

// Direct3D 9 types, defined in d3d9.h (presenter.cpp only).
struct IDirect3D9;
struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace dd2d9 {

class DDraw;
class Surface;
struct GpuSurf;
struct GpuTex;
class Palette;
class Presenter;
class Gpu;

// ---------------------------------------------------------------- Buffer
// Pixel memory of a surface. RGB / palettized buffers are GDI DIB sections so
// GetDC() works natively; Flip() swaps buffers between the surfaces of a chain.
struct Buffer {
    LONG ref = 1;
    int w = 0, h = 0;
    LONG pitch = 0;
    Format fmt;
    uint8_t* bits = nullptr;
    HDC dc = nullptr;  // from GetDC, kept until the buffer goes
    bool ownsMemory = false;  // allocated here (_aligned_malloc)
    const Palette* dcPal = nullptr;  // palette last loaded into the DIB color table
    DWORD dcPalVersion = 0;

    static Buffer* Create(int w, int h, const Format& f);
    static Buffer* CreateExternal(void* mem, int w, int h, LONG pitch, const Format& f);
    void AddRef() { InterlockedIncrement(&ref); }
    void Release();
    HDC GetDC();
    void EndDC();  // after the game's ReleaseDC: shadow DIB contents back to memory
    HBITMAP shadow = nullptr;   // for GetDC where GDI can't use the memory itself
    uint8_t* shadowBits = nullptr;
    HANDLE kmtBitmap = nullptr;  // dc came from D3DKMTCreateDCFromMemory
    void CopyToShadow();
    uint8_t* Row(int y) const { return bits + y * pitch; }
    // Direct3D: the GPU copy of a render target or Z buffer (gpures.h), and which copy is
    // behind. g_lock (and the Gpu lock for gpu).
    GpuSurf* gpu = nullptr;
    bool gpuStale = true;   // the CPU copy changed since the GPU copy was made
    bool cpuStale = false;  // the GPU copy changed since the CPU copy was read back
};

// ---------------------------------------------------------------- Palette
class Palette final : public IDirectDrawPalette {
public:
    uint32_t magic = kMagic;
    Palette* self = this;

    void ReserveSystemEntries();
    Palette(DDraw* dd, DWORD caps, const PALETTEENTRY* entries);
    static Palette* From(void* iface) { return static_cast<Palette*>(ImplFromIface(iface)); }

    STDMETHOD(QueryInterface)(REFIID riid, void** out) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetCaps)(LPDWORD caps) override;
    STDMETHOD(GetEntries)(DWORD flags, DWORD base, DWORD count, LPPALETTEENTRY entries) override;
    STDMETHOD(Initialize)(LPDIRECTDRAW dd, DWORD flags, LPPALETTEENTRY entries) override;
    STDMETHOD(SetEntries)(DWORD flags, DWORD start, DWORD count, LPPALETTEENTRY entries) override;

    int Count() const;
    PALETTEENTRY entries[256] = {};
    DWORD caps = 0;
    DWORD version = 1;

private:
    ~Palette();
    LONG ref_ = 1;
    DDraw* dd_;
public:
    int ddRef = 0;  // DirectDraw interface version (4 or 7) this palette holds a reference to
};

// ---------------------------------------------------------------- Clipper
class Clipper final : public IDirectDrawClipper {
public:
    uint32_t magic = kMagic;
    Clipper* self = this;

    explicit Clipper(DDraw* dd);
    static Clipper* From(void* iface) { return static_cast<Clipper*>(ImplFromIface(iface)); }

    STDMETHOD(QueryInterface)(REFIID riid, void** out) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetClipList)(LPRECT rect, LPRGNDATA data, LPDWORD size) override;
    STDMETHOD(GetHWnd)(HWND* hwnd) override;
    STDMETHOD(Initialize)(LPDIRECTDRAW dd, DWORD flags) override;
    STDMETHOD(IsClipListChanged)(WINBOOL* changed) override;
    STDMETHOD(SetClipList)(LPRGNDATA data, DWORD flags) override;
    STDMETHOD(SetHWnd)(DWORD flags, HWND hwnd) override;

    HRESULT Usable() const;  // what GetClipList fails with, if it does (g_lock held)
    // Rectangles a blit to `target` may touch, in the target's coordinates.
    std::vector<RECT> Region(const Surface* target) const;

    HWND hwnd = nullptr;
    bool fromDD1 = false;     // made through IDirectDraw: SetClipList takes any header
    bool initialized = true;  // false for one made by the class factory, until Initialize
    int ddRef = 0;            // the DirectDraw interface version it references (made through 4/7)

private:
    ~Clipper();
    LONG ref_ = 1;
    DDraw* dd_;  // may be null for clippers from DirectDrawCreateClipper
    std::vector<uint8_t> clipList_;
};

// ---------------------------------------------------------------- GammaControl
class GammaControl final : public IDirectDrawGammaControl {
public:
    uint32_t magic = kMagic;
    Surface* surface;

    explicit GammaControl(Surface* s) : surface(s) { WritableVtbl(this); }
    STDMETHOD(QueryInterface)(REFIID riid, void** out) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetGammaRamp)(DWORD flags, LPDDGAMMARAMP ramp) override;
    STDMETHOD(SetGammaRamp)(DWORD flags, LPDDGAMMARAMP ramp) override;
};

// ---------------------------------------------------------------- Surface
enum { kCkDestBlt, kCkDestOverlay, kCkSrcBlt, kCkSrcOverlay };

class Surface {
public:
    Surface(DDraw* dd, const DDSURFACEDESC2& desc, Buffer* buf, int version);
    ~Surface();
    static Surface* From(void* iface) { return static_cast<Surface*>(ImplFromIface(iface)); }
    void* Iface(int version);

    // version: the surface interface called (Direct3D 1-6 objects count their references there).
    HRESULT QueryInterface(REFIID riid, void** out, int version = 7);
    // As in ddraw.dll, every interface (IDirectDrawSurface .. 7, the gamma control)
    // counts its own references, and AddRef/Release return that count; the surface
    // lives while any is left. A complex surface's root holds its members through the
    // interface they were created with (as ddraw.dll counts it); dd2d9's own references
    // (Direct3D render targets) have a count of their own.
    enum RefSlot { kRefV1, kRefV2, kRefV3, kRefV4, kRefV7, kRefGamma, kRefInternal, kRefSlots };
    // The interface GetAttachedSurface and EnumAttachedSurfaces hand out through `version`:
    // IDirectDrawSurface2/3 give IDirectDrawSurface (v1) ones, as ddraw.dll does.
    static int HandedOut(int version) { return version >= 4 ? version : 1; }
    static int SlotFor(int version) {
        return version == 1 ? kRefV1 : version == 2 ? kRefV2 : version == 3 ? kRefV3 : version == 4 ? kRefV4 : kRefV7;
    }
    ULONG AddRef(int slot = kRefInternal);
    ULONG Release(int slot = kRefInternal);
    void TraceRef(const char* what, int slot, LONG r) const;  // log=2, primary chain only

    HRESULT AddAttachedSurface(Surface* s, int version, int attacheeVersion);
    HRESULT DeleteAttachedSurface(DWORD flags, Surface* s, int ifaceVersion);
    HRESULT EnumAttachedSurfaces(int version, void* ctx, void* callback);
    HRESULT GetAttachedSurface(const DDSCAPS2& caps, Surface** out, int version);
    // version: the IDirectDrawSurface version called; srcOldIface: the source was passed as an
    // IDirectDrawSurface or IDirectDrawSurface2 (DDBLT_KEYDEST then takes the source's key).
    HRESULT Blt(RECT* dst, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx, int version, bool srcOldIface);
    HRESULT BltFast(DWORD x, DWORD y, Surface* src, RECT* srcRect, DWORD trans, int version);
    HRESULT BltLocked(RECT* dst, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx, bool useClipper,
                      bool destKeyFromSource = false);
    HRESULT Flip(Surface* target, DWORD flags);
    HRESULT GetCaps(DDSCAPS2* caps);
    HRESULT GetClipper(IDirectDrawClipper** out);
    HRESULT SetClipper(IDirectDrawClipper* c);
    HRESULT GetColorKey(DWORD flags, DDCOLORKEY* key);
    HRESULT SetColorKey(DWORD flags, DDCOLORKEY* key, int version);
    HRESULT GetFlipStatus(DWORD flags);
    HRESULT GetDC(HDC* dc);
    HRESULT ReleaseDC(HDC dc, int version);
    HRESULT GetPalette(IDirectDrawPalette** out);
    HRESULT SetPalette(IDirectDrawPalette* p, int version);
    // The palette the screen shows for this surface: its own, or on the front of a flip
    // chain without one, a back buffer's (palettes stay with the surface they were set on).
    const Palette* ShownPalette() const;
    Format ReportedFormat() const;
    HRESULT GetPixelFormat(DDPIXELFORMAT* pf);
    HRESULT GetSurfaceDesc(DDSURFACEDESC2* desc);
    HRESULT SetSurfaceDesc(const DDSURFACEDESC2& desc, DWORD flags);
    // desc: the caller's DDSURFACEDESC or DDSURFACEDESC2, filled in place as ddraw.dll does.
    HRESULT Lock(RECT* rect, void* desc, DWORD flags);
    // version 1-3 identify the lock by the pointer Lock returned, 4 and 7 by its rectangle.
    HRESULT Unlock(void* pointer, const RECT* rect, int version);
    HRESULT PageLock(DWORD flags);  // and PageUnlock: neither does anything here
    void FillLockDesc(void* desc, uint8_t* bits) const;
    HRESULT GetDDInterface(void** out);
    HRESULT SetPrivateData(REFGUID tag, void* data, DWORD size, DWORD flags);
    HRESULT GetPrivateData(REFGUID tag, void* data, DWORD* size);
    HRESULT FreePrivateData(REFGUID tag);
    // Lost surfaces, as in ddraw.dll: video-memory surfaces (and the primary and Z
    // buffers) are lost when the display mode or exclusive mode changes, or the
    // exclusive game loses focus, until restored. Their content is kept here.
    bool CanBeLost() const;
    // g_lock held. Out of exclusive mode it first looks for display changes others made,
    // as ddraw.dll does once per call: a second surface of the same call passes poll = false.
    bool InPrimaryChain() const { return isPrimary || (root && root->isPrimary); }
    bool IsLost(bool poll = true) const;  // as ddraw.dll reports it (IsLost, Restore, ...)
    bool CantDraw(bool poll = true) const;  // lost for Blt, BltFast, Lock and GetDC
    HRESULT Restore();    // g_lock held
    bool sysmemRequested = false;  // created with DDSCAPS_SYSTEMMEMORY asked for (such surfaces aren't lost)
    bool ownFormat = false;        // created with a pixel format (ddraw.dll's HASPIXELFORMAT)
    DWORD modeW = 0, modeH = 0, modeBpp = 0;  // the display mode it was created in (Restore checks it)

    int W() const { return buf->w; }
    int H() const { return buf->h; }
    bool IsFront() const { return isPrimary; }
    const PALETTEENTRY* EffectivePalette() const;
    // Tells the presenter that `area` of the primary surface changed (g_lock
    // held), or with null, the whole image (any thread).
    void MarkDirtyIfVisible(const RECT* area = nullptr);
    void ReplaceBuffer(Buffer* b);  // takes ownership of b
    void FillRect(const RECT& r, DWORD value);  // raw pixel value, clipped to the surface; g_lock held
    Surface* NextMipLevel() const;              // next smaller level of a mipmap chain, or null

    DDraw* dd;
    DDSURFACEDESC2 desc;  // creation description (size/format/caps); pitch & lpSurface filled on demand
    Buffer* buf;
    int version;          // interface version the surface was created through
    Surface* root = nullptr;           // owner of a complex surface (flip chain / mipmaps)
    // While oldRoot (this surface's root) is destroyed, with g_lock held: drop the
    // unreferenced links to the other members; then, if still referenced, live on alone.
    void ForgetMembers(const Surface* oldRoot);
    void Detach(const Surface* oldRoot);
    bool lost = false;    // marked lost (see DDraw::UpdateLostSurfaces); g_lock
    bool drawableLost = false;  // lost by Alt+Tab only: still drawable (see UpdateLostSurfaces); g_lock
    int ddRef = 0;      // DirectDraw interface version (4 or 7) this surface holds a reference to
    int ddVersion = 1;  // DirectDraw interface version it was created through (GetDDInterface returns it)
    uint64_t vidMemBytes = 0;  // counted in the DirectDraw object's video memory use
    std::vector<Surface*> children;    // complex children owned by this root
    std::vector<Surface*> flipChain;   // on a flip-chain root: [front, back1, ..., backN]
    Surface* chainNext = nullptr;      // next surface in the flip chain (for GetAttachedSurface)
    std::vector<Surface*> attached;    // explicitly attached surfaces, plus mip levels
    struct AttachRef {
        Surface* s;
        int slot;  // the reference taken, through the interface AddAttachedSurface was called on
        uint32_t seq;  // when (attach lists are newest first)
    };
    struct AttachEntry {
        Surface* s;
        int slot;  // -1: made with the surface (it can't be detached)
        uint32_t seq;
    };
    std::vector<AttachEntry> AttachList() const;
    uint32_t ringSeq = 0;  // when attachSlot's link was made
    std::vector<AttachRef> attachRefs;  // the explicitly attached ones (a surface may be attached to several)
    // A back buffer attached to a flip ring: the reference that holds, whoever detaches it.
    int attachSlot = -1;
    // Flip chains built with AddAttachedSurface (IDirectDrawSurface, as ddraw.dll allows):
    // a ring through chainNext, like a created chain's.
    bool flipStart = false;     // the surface the ring was formed on (it can't be detached)
    DWORD attachSavedCaps = 0;  // caps before joining the ring
    Palette* palette = nullptr;
    Clipper* clipper = nullptr;
    DDCOLORKEY ck[4] = {};
    bool hasCk[4] = {};
    bool isPrimary = false;
    // ddraw.dll's usage count: outstanding locks, plus one while a DC is out (lockCount),
    // with the rectangles of rectangle locks (a whole-surface lock excludes everything).
    struct LockRec {
        RECT r;
        uint8_t* ptr;   // what Lock returned (Unlock through IDirectDrawSurface 1-3 names it)
        bool readOnly;  // DDLOCK_READONLY: Unlock leaves the contents unchanged
    };
    LONG lockCount = 0;
    std::vector<LockRec> rectLocks;
    bool wholeLocked = false, wholeReadOnly = false;
    bool fastLocked = false;  // a DirectDraw 7 system-memory texture's lock (no nesting, no checks)
    bool dcCounted = false;   // the DC out is part of lockCount (Unlock through version 1/2 can drop it)
    RECT lockArea = {0, 0, 0, 0};  // union of the areas locked now (with lockCount > 0)
    HDC dcOut = nullptr;
    DWORD uniqueness = 1, priority = 0, lod = 0;
    // The contents changed. As ddraw.dll: 0 (OWNDC surfaces, after SetSurfaceDesc) stays 0,
    // and counting skips 0.
    void BumpUniqueness() {
        if (uniqueness && !++uniqueness) uniqueness = 1;
    }
    GammaControl* gamma = nullptr;
    DDGAMMARAMP* gammaRamp = nullptr;  // the primary's ramp (SetGammaRamp), applied while exclusive
    HRESULT ManagedTextureCheck() const;  // Set/GetPriority, Set/GetLOD
    void* d3dTexture = nullptr;  // Direct3D texture object (d3dproxy.cpp), created on demand
    GpuTex* gpuTex = nullptr;    // Direct3D's GPU copy of this texture and its mip levels (gpures.h)
    void* d3dLegacy = nullptr;   // Direct3D 3-6's textures and device made through QueryInterface (d3d6int.h)
    // The CPU copy is about to be read (written): Direct3D's drawing read back first.
    void CpuAccess(bool write);

private:
    struct PrivateData {
        GUID tag;
        std::vector<uint8_t> data;
        IUnknown* unk;
        bool isVolatile;  // DDSPD_VOLATILE: valid while the contents stay as they were (stamp)
        DWORD stamp;
    };
    void FillCore(RECT* dst, DWORD value);
    void DropAttachRef();  // releases the reference attachSlot holds
    HRESULT AttachForFlip(Surface* s, int slot);
    HRESULT DetachOne(Surface* s, int slot);
    void LeaveFlipRing(std::vector<Surface*>* drop);
    LONG refs_[kRefSlots] = {};
    LONG total_ = 0;  // sum of refs_
    void* wraps_[5] = {};
    std::vector<PrivateData> priv_;
};

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
    // Stops presenting into the window (its swap chain goes) on the presenter thread and
    // waits for it, handling window messages meanwhile: another device (a wrapper's)
    // presents there from now on.
    void DropDevice();
    void SetGamma(const DDGAMMARAMP& ramp);
    void GetGamma(DDGAMMARAMP* ramp);
    // Where an sw x sh image goes in a bw x bh area, as the stretch setting says.
    RECT FitRect(UINT sw, UINT sh, UINT bw, UINT bh) const;

private:
    static DWORD WINAPI ThreadProc(void* self);
    void Loop();
    bool EnsureStaging(IDirect3DDevice9* d, UINT w, UINT h);
    bool EnsurePrescaled(IDirect3DDevice9* d, UINT w, UINT h);
    void ReleaseGpuResources() override;     // staging and prescaled surfaces (Gpu lock held)
    void LeaveGpu();                         // stop presenting through the shared device
    // What the presenter copies from the primary surface under g_lock; the
    // pixels themselves are converted afterwards, without it.
    struct Snapshot {
        Buffer* buf = nullptr;  // referenced: the game may flip or release it meanwhile
        Format fmt;
        PALETTEENTRY pal[256];  // 8-bit formats
    };
    void Convert(const Snapshot& s, const RECT& area, void* bits, int pitch);
    void BuildLut16(const Format& f);
    void CountPresent(double lockSeconds, double convertSeconds, double fraction);

    DDraw* dd_;
    HANDLE thread_ = nullptr;
    HANDLE dirtyEvent_ = nullptr;
    HANDLE dropDone_ = nullptr;          // signaled when a DropDevice() request was handled
    volatile bool dropRequest_ = false;
    bool waitForContent_ = false;  // presenter thread only: see Loop()
    volatile bool running_ = false;
    volatile bool active_ = false;

    bool usingGpu_ = false;            // presenting through the shared device (an owner of it)
    IDirect3DSurface9* staging_ = nullptr;
    IDirect3DSurface9* prescaled_ = nullptr;  // filter=sharp: the image enlarged by a whole factor
    UINT preW_ = 0, preH_ = 0;
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

// ---------------------------------------------------------------- DDraw
class DDraw {
public:
    DDraw();
    static DDraw* From(void* iface) { return static_cast<DDraw*>(ImplFromIface(iface)); }
    void* Iface(int version);

    // fromVersion: the interface version called (Direct3D 7 forwards its references there).
    HRESULT QueryInterface(REFIID riid, void** out, int fromVersion = 7);
    // Every interface version (1, 2, 4, 7) counts its own references, as in
    // ddraw.dll; the object is torn down when all of them are released.
    ULONG AddRef(int version);
    ULONG Release(int version);
    void IntAddRef() { InterlockedIncrement(&intRef_); }
    void IntRelease();

    // ddVersion: the IDirectDraw version called (1, 2, 4, 7).
    HRESULT CreateSurface(const void* desc, void** out, IUnknown* outer, int ddVersion);
    HRESULT CreateSurfaceImpl(const void* desc, void** out, IUnknown* outer, int ddVersion);
    HRESULT CreatePalette(DWORD flags, PALETTEENTRY* entries, IDirectDrawPalette** out, IUnknown* outer,
                          int version);
    HRESULT CreateClipper(DWORD flags, IDirectDrawClipper** out, IUnknown* outer, int version);
    HRESULT DuplicateSurface(void* src, void** out, int ddVersion);
    HRESULT EnumDisplayModes(DWORD flags, const void* filter, void* ctx, void* callback, int descVersion);
    HRESULT EnumSurfaces(DWORD flags, const void* filter, void* ctx, void* callback, int version, int ddVersion);
    HRESULT GetCaps(DDCAPS* drv, DDCAPS* hel);
    HRESULT GetDisplayMode(void* desc, int version);
    HRESULT GetGDISurface(void** out, int version);
    HRESULT FlipToGDISurface();
    Surface* GdiSurface() const;  // g_lock held
    HRESULT GetMonitorFrequency(DWORD* freq);
    HRESULT GetScanLine(DWORD* line);
    HRESULT GetVerticalBlankStatus(BOOL* inVb);
    HRESULT Initialize(GUID* guid);
    HRESULT SetCooperativeLevel(HWND hwnd, DWORD flags, int version);
    HRESULT SetDisplayMode(DWORD w, DWORD h, DWORD bpp, DWORD refresh, DWORD flags);
    HRESULT RestoreDisplayMode();
    HRESULT WaitForVerticalBlank(DWORD flags, HANDLE event);
    HRESULT GetAvailableVidMem(const DDSCAPS2* caps, DWORD* total, DWORD* free);
    HRESULT GetSurfaceFromDC(HDC dc, void** out, int version);
    static HRESULT FindSurfaceFromDC(HDC dc, void** out, HDC* deviceDC);
    HRESULT GetDeviceIdentifier(void* id, DWORD flags, bool v2);

    // Display state
    void DisplayMode(DWORD* w, DWORD* h, DWORD* bpp) const;
    DWORD RefreshRate() const { return refresh_; }
    HWND PresentWindow() const;
    RECT PresentSource(HWND hwnd) const;
    RECT PresentTarget(HWND hwnd) const;  // g_lock held
    // Where the game's display mode is shown on the real screen, when that isn't 1:1 at the
    // screen's origin (a scaled or windowed mode). g_lock held. See input.cpp.
    bool ImageOnScreen(RECT* image, SIZE* mode) const;
    HWND Window() const { return hwnd_; }
    bool Exclusive() const { return (coop_ & DDSCL_EXCLUSIVE) != 0; }
    // The whole visible image changed (flip, palette, window...). Any thread.
    void MarkDirty() {
        InterlockedExchange(&dirtyAll_, 1);
        if (presenter_) presenter_->MarkDirty();
    }
    // Part of the primary surface changed. Called with g_lock held.
    void MarkDirtyRect(const RECT& r) {
        if (r.right <= r.left || r.bottom <= r.top) return;
        if (dirtyRect_.right <= dirtyRect_.left || dirtyRect_.bottom <= dirtyRect_.top) {
            dirtyRect_ = r;
        } else {
            dirtyRect_.left = std::min(dirtyRect_.left, r.left);
            dirtyRect_.top = std::min(dirtyRect_.top, r.top);
            dirtyRect_.right = std::max(dirtyRect_.right, r.right);
            dirtyRect_.bottom = std::max(dirtyRect_.bottom, r.bottom);
        }
        if (presenter_) presenter_->MarkDirty();
    }
    // What changed since the last call, for the presenter (g_lock held). Returns
    // true if everything did; otherwise *r is the changed area (maybe empty).
    bool TakeDirty(RECT* r) {
        *r = dirtyRect_;
        SetRectEmpty(&dirtyRect_);
        return InterlockedExchange(&dirtyAll_, 0) != 0;
    }
    Presenter* GetPresenter() { return presenter_; }
    Gpu& GetGpu() { return *gpu_; }
    void OnActivate(bool active, DWORD thread);  // WM_ACTIVATEAPP while exclusive (thread: its lParam)
    void OnDisplayChange();        // WM_DISPLAYCHANGE while exclusive
    void CheckCover(HWND h);       // the window moved in exclusive mode; g_displayLock held
    void OnPrimaryReleased();
    // The primary's gamma ramp while this object is exclusive, as ddraw.dll applies it;
    // the identity otherwise. g_lock held.
    void ApplyGamma();
    // Lost surfaces (see Surface::CanBeLost). kDevNotRestored: every surface that can be
    // lost is about to be marked lost; kDevLost: the exclusive game is in the background.
    enum DeviceState : LONG { kDevOk, kDevNotRestored, kDevLost };
    LONG DevState() const { return deviceState_; }
    bool FocusLossOnly() const { return focusLossOnly_; }
    // A display change: nothing stays drawable (see UpdateLostSurfaces), except in the
    // background, where the display isn't the game's anyway (switch mode's own change back
    // to the desktop's, its late second notice) and the game's comes back with it.
    void InvalidateSurfaces() {
        if (deviceState_ != kDevLost) focusLossOnly_ = false;
        InterlockedCompareExchange(&deviceState_, kDevNotRestored, kDevOk);
    }
    void UpdateLostSurfaces();  // apply a pending kDevNotRestored to the surfaces; g_lock held
    HRESULT TestCooperativeLevel();
    HRESULT RestoreAllSurfaces();
    bool Suspended() const { return suspended_; }  // the game is in the background (focus lost)
    // Another DirectDraw object has exclusive mode: this one's primary isn't shown.
    bool Superseded() const;
    bool OtherExclusive() const;  // another DirectDraw object has exclusive mode
    bool AnyLocked() const;
    void NoteDisplayMode();
    void PollDisplayChange();     // the display mode changed behind dd2d9's back: surfaces lost
    bool InOwnMode() const;  // the display is in the mode this object set and switched to
    bool EmulatedMode() const;  // exclusive, in a mode set but not switched to (scaled or windowed)
    bool CoverSize(HWND h, SIZE* out) const;  // the size CoverMonitor gave h, frame included
    bool QuietMove() const { return quietMove_; }  // CheckCover's move: hidden from the game
    void TakeOverExclusive(DDraw* prev, HWND hwnd);  // g_displayLock held
    // Direct3D devices created through dd2d9 (by a wrapper such as D7VK, which
    // then presents itself): while any exist, dd2d9 doesn't present.
    void Add3DDevice();
    void Remove3DDevice();
    bool Has3D() const { return devices3D_ > 0; }
    void SleepUntilVBlank(int count);
    void PaceFlip();  // never let Flip() run faster than the refresh rate
    double VBlankPhase() const;  // 0..1 position inside the current refresh period

    Surface* primary = nullptr;
    std::vector<Surface*> surfaces;
    PALETTEENTRY defaultPalette[256];
    void* d3dObject = nullptr;  // Direct3D object (d3dproxy.cpp), shared by its interfaces
    void* d3d7 = nullptr;       // dd2d9's own Direct3D 7 object (d3d7.cpp), while it lives
    void* d3dLegacy = nullptr;  // dd2d9's own Direct3D 3-6 object (d3d6obj.cpp), while it lives
    bool dd7 = false;           // made by DirectDrawCreateEx (ddraw.dll's DDRAWILCL_DIRECTDRAW7)
    DWORD CoopFlags() const { return coop_; }
    bool alive = true;
    bool initialized = true;

private:
    ~DDraw();
    void ApplyWindow();
    void CoverMonitor(HWND insertAfter, UINT flags);  // client area over the monitor, frame outside
    void ConfineCursor(bool on);  // g_displayLock held
    void Uncover();
    void RestoreSavedWindow(bool windowChanges);
    void HookWindow();
    void UnhookWindow();
    bool SwitchMode();
    void RestoreDisplay();
    void SaveWindow();
    void UpdateRefreshRate();

    LONG ref_ = 0;
    LONG tornDown_ = 0;
    LONG devices3D_ = 0;
    LONG ifaceRef_[4] = {};  // per interface version: 1, 2, 4, 7
public:
    // Video memory as games see it: a fixed total, minus the video-memory surfaces (g_lock).
    static constexpr uint64_t kVidMemTotal = 512ull << 20;
    enum CapsSide { kBoth, kHardwareOnly, kEmulationOnly };
    CapsSide capsSide = kBoth;  // from DirectDrawCreate's DDCREATE_HARDWAREONLY / DDCREATE_EMULATIONONLY
    uint64_t vidMemUsed = 0;
    DWORD VidMemFree() const {
        return DWORD(vidMemUsed < kVidMemTotal ? kVidMemTotal - vidMemUsed : 0);
    }
private:
    void TearDown();         // the last reference is gone (g_lock not held)
    volatile LONG dirtyAll_ = 1;
    RECT dirtyRect_ = {0, 0, 0, 0};  // guarded by g_lock
    LONG intRef_ = 1;  // released when the last external reference goes away
    void* wraps_[4] = {};
    HWND hwnd_ = nullptr;
    DWORD coop_ = 0;
    bool modeSet_ = false;
    DWORD modeW_ = 0, modeH_ = 0, modeBpp_ = 0, modeRefresh_ = 0;
    bool switched_ = false;     // the real display mode was changed
    bool ddraw1Coop_ = false;   // a SetCooperativeLevel through IDirectDraw succeeded (g_displayLock)
    bool suspended_ = false;    // display setup undone because the game lost focus
    bool backgrounded_ = false;  // lost focus at some point during this exclusive mode
    HWND coverHwnd_ = nullptr;   // window CoverMonitor grew past the screen (and gave a region)
    ULONG expectedMode_ = 0;  // NoteDisplayMode
    DWORD recoverStart_ = 0;       // CheckCover: when its count started
    int recoverCount_ = 0;
    bool cursorConfined_ = false;  // ConfineCursor
    bool foregroundLocked_ = false;  // ConfineCursor: LockSetForegroundWindow
    SIZE coverSize_ = {};        // its size, frame included
    volatile bool quietMove_ = false;  // inside CheckCover's CoverMonitor
    bool applying_ = false;     // inside ApplyWindow: focus changes are our own doing
    bool inCoop_ = false;       // inside SetCooperativeLevel (g_displayLock)
    int activeDisplay_ = kDisplaySwitch;  // the display mode ApplyWindow actually used
    HWND savedHwnd_ = nullptr;  // window whose style / position are saved
    bool windowSaved_ = false;  // window style / position saved before taking it over
    LONG savedStyle_ = 0, savedExStyle_ = 0;
    RECT savedRect_ = {};
    DWORD refresh_ = 60;
    LARGE_INTEGER qpcFreq_, qpcBase_;
    int64_t lastFlip_ = 0;
    Presenter* presenter_ = nullptr;
    Gpu* gpu_ = nullptr;  // the Direct3D 9 device (gpu.h)
    volatile LONG deviceState_ = kDevOk;
    // The pending kDevNotRestored comes from a focus loss alone (see UpdateLostSurfaces).
    volatile bool focusLossOnly_ = false;
    const Buffer* gdiBuf_ = nullptr;  // the primary's first memory (compared only)
};

}  // namespace dd2d9
