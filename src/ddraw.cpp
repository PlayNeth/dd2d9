// The DirectDraw object: creation, references, teardown, palettes, clippers, the GDI surface.
#include "ddint.h"

#include "d3d6.h"
#include "d3d7.h"

#include <mmsystem.h>

#include <algorithm>

namespace dd2d9 {

std::vector<DDraw*> g_ddraws;
DDraw* g_exclusiveDD = nullptr;

namespace {
// The display's hardware palette as the last palette shown on a primary left it (g_lock):
// what a new DirectDraw object's 8-bit primary without a palette shows too.
PALETTEENTRY g_hwPalette[256];
bool g_hwPaletteKept;
}  // namespace

DDraw::DDraw() {
    seenUniqueness_ = LONG(DisplayUniqueness());  // (before other objects' changes can reach it)
    {
        Guard l;
        g_ddraws.push_back(this);
    }
    NoteDisplayMode();
    wraps_[0] = new DDWrap<1>(this);
    wraps_[1] = new DDWrap<2>(this);
    wraps_[2] = new DDWrap<4>(this);
    wraps_[3] = new DDWrap<7>(this);
    {
        Guard l;
        if (g_hwPaletteKept) memcpy(defaultPalette, g_hwPalette, sizeof(defaultPalette));
        else
            for (int i = 0; i < 256; i++) defaultPalette[i] = {BYTE(i), BYTE(i), BYTE(i), 0};
    }
    UpdateRefreshRate();
    QueryPerformanceFrequency(&qpcFreq_);
    QueryPerformanceCounter(&qpcBase_);
    gpu_ = new Gpu;
    presenter_ = new Presenter(this);
    timeBeginPeriod(1);  // precise Sleep() for vblank emulation and flip pacing
}

DDraw::~DDraw() {
    {
        Guard l;
        g_ddraws.erase(std::find(g_ddraws.begin(), g_ddraws.end(), this));
    }
    timeEndPeriod(1);
    delete presenter_;
    delete gpu_;  // after the presenter, which used it
    delete static_cast<DDWrap<1>*>(wraps_[0]);
    delete static_cast<DDWrap<2>*>(wraps_[1]);
    delete static_cast<DDWrap<4>*>(wraps_[2]);
    delete static_cast<DDWrap<7>*>(wraps_[3]);
}

void* DDraw::Iface(int v) {
    switch (v) {
    case 1: return wraps_[0];
    case 2: return wraps_[1];
    case 4: return wraps_[2];
    default: return wraps_[3];
    }
}

HRESULT DDraw::QueryInterface(REFIID r, void** out, int fromVersion) {
    if (IsBadReadPtr(&r, sizeof(IID)) || !out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    int v = 0;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirectDraw)) v = 1;
    else if (IsEqualGUID(r, IID_IDirectDraw2)) v = 2;
    else if (IsEqualGUID(r, IID_IDirectDraw4)) v = 4;
    else if (IsEqualGUID(r, IID_IDirectDraw7)) v = 7;
    if (v) {
        AddRef(v);
        *out = Iface(v);
        return S_OK;
    }
    // Direct3D: 7 on DirectDrawCreateEx's objects, 3-6 on the others (as ddraw.dll loads
    // d3dim700.dll or d3dim.dll).
    HRESULT hr = dd7 ? D3D7Query(this, r, out, fromVersion) : D3DLegacyQuery(this, r, out, fromVersion);
    if (hr != E_NOINTERFACE) return hr;
    DD2D9_LOG("DDraw::QueryInterface(%s): not supported", GuidName(r));
    return E_NOINTERFACE;
}

static int IfaceIndex(int version) { return version == 1 ? 0 : version == 2 ? 1 : version == 4 ? 2 : 3; }

// log=2 traces these (whether a game lets go of its old DirectDraw object before making
// another decides what the new one may do). Surfaces made through IDirectDraw4/7 hold a
// reference of their own, counted here too.
ULONG DDraw::AddRef(int version) {
    const LONG total = InterlockedIncrement(&ref_);
    const LONG r = InterlockedIncrement(&ifaceRef_[IfaceIndex(version)]);
    DD2D9_TRACE("AddRef DirectDraw %p through v%d -> %ld (total %ld)", static_cast<void*>(this), version, r, total);
    return ULONG(r);
}

ULONG DDraw::Release(int version) {
    LONG* own = &ifaceRef_[IfaceIndex(version)];
    LONG r = InterlockedDecrement(own);
    if (r < 0) {  // released more often than referenced: ignore
        InterlockedIncrement(own);
        return 0;
    }
    const LONG total = InterlockedDecrement(&ref_);
    DD2D9_TRACE("Release DirectDraw %p through v%d -> %ld (total %ld)", static_cast<void*>(this), version, r, total);
    // Tear down once: a stray AddRef/Release pair after the final release must
    // not run this (and IntRelease) a second time.
    if (total == 0 && InterlockedExchange(&tornDown_, 1) == 0) {
        // Teardown waits for the presenter thread, which takes g_lock: if this
        // thread holds it (a surface released inside another call, or the game's
        // AcquireDDThreadLock), wait until it lets go.
        RunWhenUnlocked([](void* self) { static_cast<DDraw*>(self)->TearDown(); }, this);
    }
    return ULONG(r);
}

void DDraw::TearDown() {
    {
        DD2D9_LOG("DirectDraw object released");
        const bool stopped = presenter_->Stop();  // must not hold g_lock: the presenter thread takes it
        {
            DisplayGuard dg;
            // Still hooked while the mode goes back (see OnDisplayChange), as when leaving
            // exclusive mode.
            RestoreDisplay();
            RestoreSavedWindow(true);
            UnhookWindow();
            {
                Guard l;
                if (g_exclusiveDD == this) g_exclusiveDD = nullptr;
            }
            DestroyDeviceWindow();  // as ddraw.dll's release
            GameImageChanged();
        }
        Guard l;
        alive = false;
        D3D7Destroy(this);  // before the surfaces, as ddraw.dll releases its Direct3D object
        D3DLegacyDestroy(this);
        if (primary) {
            primary->isPrimary = false;
            primary = nullptr;
        }
        // A presenter thread that didn't stop still uses this object and the
        // presenter: keep both alive rather than free them under it.
        if (!stopped) IntAddRef();
        IntRelease();
    }
}

void DDraw::IntRelease() {
    if (InterlockedDecrement(&intRef_) == 0) delete this;
}

// An object from CoCreateInstance becomes a DirectDraw 7 one (Direct3D 7, not 1-6) when it is
// initialized through IDirectDraw7, whatever its class or the interface first asked for, as
// ddraw.dll's Initialize decides it by the interface it is called on.
HRESULT DDraw::Initialize(GUID*, int version) {
    if (initialized) return DDERR_ALREADYINITIALIZED;
    initialized = true;
    dd7 = version == 7;
    return DD_OK;
}

HRESULT DDraw::CreatePalette(DWORD flags, PALETTEENTRY* entries, IDirectDrawPalette** out, IUnknown* outer,
                             int version) {
    // Checks in ddraw.dll's order (spec: re/spec/palette-clipper-exports.md).
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (flags & ~0x7FFu) return DDERR_INVALIDPARAMS;
    if (!coop_) return DDERR_NOCOOPERATIVELEVELSET;
    // ddraw.dll refuses DDPCAPS_PRIMARYSURFACE too; dd2d9 lets it through, for games
    // made on Windows 9x that pass it.
    if (flags & DDPCAPS_PRIMARYSURFACE) DD2D9_LOG("CreatePalette: DDPCAPS_PRIMARYSURFACE (ddraw.dll refuses it)");
    if (flags & (DDPCAPS_PRIMARYSURFACELEFT | DDPCAPS_VSYNC)) return DDERR_INVALIDPARAMS;
    const DWORD size = flags & (DDPCAPS_1BIT | DDPCAPS_2BIT | DDPCAPS_4BIT | DDPCAPS_8BIT);
    if (!size || (size & (size - 1))) return DDERR_INVALIDPARAMS;  // exactly one size
    if ((flags & DDPCAPS_8BITENTRIES) && (flags & (DDPCAPS_ALPHA | DDPCAPS_8BIT))) return DDERR_INVALIDPARAMS;
    if ((flags & DDPCAPS_ALLOW256) && !(flags & DDPCAPS_8BIT)) return DDERR_INVALIDPARAMS;
    if (!entries) return DDERR_INVALIDPARAMS;
    Guard l;
    auto* p = new Palette(this, flags, entries);
    if (version >= 4) {  // made through IDirectDraw4/7: references it, as in ddraw.dll
        AddRef(version);
        p->ddRef = version;
    }
    *out = p;
    return DD_OK;
}

HRESULT DDraw::CreateClipper(DWORD flags, IDirectDrawClipper** out, IUnknown* outer, int version) {
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!out) return DDERR_INVALIDPARAMS;
    *out = nullptr;
    if (flags) return DDERR_INVALIDPARAMS;
    Guard l;
    auto* c = new Clipper(this);
    c->fromDD1 = version < 2;
    if (version >= 4) {  // made through IDirectDraw4/7: references it, as in ddraw.dll
        AddRef(version);
        c->ddRef = version;
    }
    *out = c;
    return DD_OK;
}

// The GDI surface is the one holding the memory the primary surface started with (the
// screen GDI draws on): after a flip, a back buffer.
Surface* DDraw::GdiSurface() const {
    if (!primary) return nullptr;
    Surface* s = primary;
    for (int i = 0; i < 4096; i++) {
        if (s->buf == gdiBuf_) return s;
        s = s->chainNext;
        if (!s || s == primary) break;
    }
    return primary;
}

HRESULT DDraw::GetGDISurface(void** out, int version) {
    if (!out) return DDERR_INVALIDPARAMS;
    Guard l;
    *out = nullptr;
    Surface* s = GdiSurface();
    if (!s) return DDERR_NOTFOUND;
    s->AddRef(Surface::SlotFor(version));
    *out = s->Iface(version);
    return DD_OK;
}

// Flips until the GDI surface is in front again.
HRESULT DDraw::FlipToGDISurface() {
    Surface* front;
    Surface* gdi;
    {
        Guard l;
        front = primary;
        if (!front) return DDERR_NOTFOUND;
        if (!(front->desc.ddsCaps.dwCaps & DDSCAPS_FLIP) || !front->chainNext) return DDERR_NOTFLIPPABLE;
        gdi = GdiSurface();
        if (gdi == front) return DD_OK;
        front->AddRef();  // Flip takes the lock itself
    }
    HRESULT hr = front->Flip(gdi, DDFLIP_WAIT);
    front->Release();
    return hr;
}

void DDraw::KeepHardwarePalette(const PALETTEENTRY* entries) {
    memcpy(defaultPalette, entries, sizeof(defaultPalette));
    memcpy(g_hwPalette, entries, sizeof(g_hwPalette));
    g_hwPaletteKept = true;
}

}  // namespace dd2d9
