// The surface objects: pixel memory (Buffer), palettes, clippers, gamma controls and surfaces
// (surface.cpp and the files surfaceint.h lists). Included by objects.h.
#pragma once
#include "common.h"

namespace dd2d9 {

class DDraw;
class Surface;
struct GpuSurf;
struct GpuTex;
class Palette;
class Presenter;
class OverlayGpu;
class Gpu;

// ---------------------------------------------------------------- Buffer
// Pixel memory of a surface (16-byte aligned, or the game's own with DDSD_LPSURFACE); GetDC
// makes a DC over it. Flip() swaps buffers between the surfaces of a chain.
inline unsigned NextCpuSerial() {
    static volatile LONG n;
    return unsigned(InterlockedIncrement(&n));
}

struct Buffer {
    LONG ref = 1;
    int w = 0, h = 0;
    LONG pitch = 0;
    Format fmt;
    uint8_t* bits = nullptr;
    HDC dc = nullptr;  // from GetDC, kept until the buffer goes
    bool ownsMemory = false;  // allocated here (_aligned_malloc of base, bits inside it)
    uint8_t* base = nullptr;
    const Palette* dcPal = nullptr;  // palette last loaded into the DIB color table
    DWORD dcPalVersion = 0;

    static Buffer* Create(int w, int h, const Format& f);
    // The pitch Create gives a buffer of width w.
    static LONG PitchFor(int w, const Format& f);
    // Create's layout without the memory: only to describe the surface Create's buffer
    // would make (EnumSurfaces' DDENUMSURFACES_CANBECREATED); bits is null.
    static Buffer* CreateLayout(int w, int h, const Format& f);
    // A DXT surface's pitch in dd2d9's layout (its memory is described by a linear size
    // instead): a block row's bytes / 4.
    static LONG BlockPitch(int w, const Format& f) {
        const LONG bw = LONG((w + 3) & ~3);
        return f.fourCC == MAKEFOURCC('D', 'X', 'T', '1') ? bw / 2 : bw;
    }
    static Buffer* CreateExternal(void* mem, int w, int h, LONG pitch, const Format& f);
    void AddRef() { InterlockedIncrement(&ref); }
    void Release();
    HDC GetDC();
    void EndDC();  // after the game's ReleaseDC: shadow DIB contents back to memory
    HBITMAP shadow = nullptr;   // for GetDC where GDI can't use the memory itself
    uint8_t* shadowBits = nullptr;
    std::vector<uint8_t> shadowWas;  // the shadow as GetDC filled it (EndDC: what GDI changed)
    HANDLE kmtBitmap = nullptr;  // dc came from D3DKMTCreateDCFromMemory
    void CopyToShadow();
    uint8_t* Row(int y) const { return bits + y * pitch; }
    // Direct3D: the GPU copy of a render target or Z buffer (gpures.h), and which copy is
    // behind. g_lock (and the Gpu lock for gpu).
    GpuSurf* gpu = nullptr;
    bool gpuStale = true;   // the CPU copy changed since the render target's GPU copy was made
    // Where (gpuStale): the union of what the CPU wrote since; kAllRect: all of it.
    static constexpr RECT kAllRect = {0, 0, 0x7FFFFFFF, 0x7FFFFFFF};
    RECT gpuDirty = kAllRect;
    void MarkGpuStale(const RECT* area) {
        const RECT a = area ? *area : kAllRect;
        if (!gpuStale) gpuDirty = a;
        else UnionRect(&gpuDirty, &gpuDirty, &a);
        gpuStale = true;
    }
    // Changes to the CPU copy (a texture's GPU copy is current while equal). Drawn from one
    // counter for every buffer: a buffer made where a freed one was can't match its value.
    unsigned cpuSerial = NextCpuSerial();
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
    // Changes to the entries, drawn from the counter every buffer's serial comes from: a
    // palette made where a freed one was can't match what was cached of it (GetDC's colour
    // table, the presenter's).
    DWORD version = NextCpuSerial();

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
    // The surfaces it is set on (g_lock). Released once too often, it leaves them as it goes
    // (ddraw.dll leaves them a dangling pointer, which Blt then reads).
    std::vector<Surface*> users;

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
    HRESULT BltFourCC(RECT* dstRect, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx, bool useClipper,
                      bool destKeyFromSource);
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
    // An 8-bit DC's colour table loaded from the surface's colours, `p` the palette they come
    // from (null: the display's).
    void LoadDcColorTable(HDC dc, const Palette* p);
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
    GpuTex* gpuTex = nullptr;    // Direct3D's GPU copy of this texture and its mip levels (gpures.h)
    void* d3dLegacy = nullptr;   // Direct3D 3-6's textures and device made through QueryInterface (d3d6int.h)
    // The CPU copy is about to be read (written): Direct3D's drawing read back first.
    void CpuAccess(bool write, const RECT* area = nullptr);  // area: what a write covers (null: all)

    // Overlays (DDSCAPS_OVERLAY, overlay.cpp): emulated, the presenter lays the visible ones
    // over the primary. What UpdateOverlay set, on every member of the overlay's flip chain
    // (g_lock); DDSCAPS_VISIBLE in desc says it is shown.
    Surface* overlayDest = nullptr;  // the surface overlaid, referenced (kRefInternal) as ddraw.dll does
    RECT overlaySrc = {}, overlayDst = {};
    DWORD overlayFlags = 0;          // DDOVER_* (keys, DDOVER_DDFX)
    DDOVERLAYFX overlayFx = {};      // with DDOVER_DDFX: the mirroring; the override keys
    bool OverlayShown() const {
        return (desc.ddsCaps.dwCaps & (DDSCAPS_OVERLAY | DDSCAPS_VISIBLE)) == (DDSCAPS_OVERLAY | DDSCAPS_VISIBLE) &&
               overlayDest;
    }
    // ddraw.dll's DDRAWISURF_HASOVERLAYDATA: overlays, and the primary while overlays are
    // offered (where SetColorKey takes a DDCKEY_DESTOVERLAY key).
    bool HasOverlayData() const;
    HRESULT UpdateOverlay(RECT* src, Surface* dest, RECT* dst, DWORD flags, DDOVERLAYFX* fx);
    HRESULT GetOverlayPosition(LONG* x, LONG* y);
    HRESULT SetOverlayPosition(LONG x, LONG y);
    HRESULT UpdateOverlayZOrder(DWORD flags, Surface* ref);
    HRESULT EnumOverlayZOrders(DWORD flags, int version, void* ctx, void* callback);
    void OverlayGone();  // the destructor: out of the z-order, the destination released

private:
    struct PrivateData {
        GUID tag;
        std::vector<uint8_t> data;
        IUnknown* unk;
        bool isVolatile;  // DDSPD_VOLATILE: valid while the contents stay as they were (stamp)
        DWORD stamp;
    };
    void FillCore(RECT* dst, DWORD value);
    HRESULT BltCore(RECT* dst, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx, bool useClipper,
                    bool destKeyFromSource);
    void DropAttachRef();  // releases the reference attachSlot holds
    HRESULT AttachForFlip(Surface* s, int slot);
    HRESULT DetachOne(Surface* s, int slot);
    void LeaveFlipRing(std::vector<Surface*>* drop);
    LONG refs_[kRefSlots] = {};
    LONG total_ = 0;  // sum of refs_
    void* wraps_[5] = {};
    std::vector<PrivateData> priv_;
};

// A cube map's face (DDSCAPS2_CUBEMAP_POSITIVEX ... NEGATIVEZ) under its root: the root, or the
// member with that face's caps that isn't a mip level; null if there is none.
Surface* CubeFace(Surface* root, DWORD face);

}  // namespace dd2d9
