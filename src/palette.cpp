// DirectDraw palettes.
#include "surfaceint.h"

namespace dd2d9 {

// As for clippers: a released or made-up palette pointer gets 0 from AddRef/Release, as in
// ddraw.dll, and is refused by SetPalette, instead of a crash.
static LiveSet<Palette> g_palettes;

bool LivePalette(const Palette* p) { return g_palettes.Has(p); }

Palette::Palette(DDraw* dd, DWORD c, const PALETTEENTRY* e) : caps(c), dd_(dd) {
    WritableVtbl(this);
    if (dd_) dd_->IntAddRef();
    g_palettes.Add(this);
    int n = Count();
    if (e) {
        if (caps & DDPCAPS_8BITENTRIES) {
            const uint8_t* idx = reinterpret_cast<const uint8_t*>(e);
            for (int i = 0; i < n; i++) entries[i] = {idx[i], 0, 0, 0};
        } else {
            memcpy(entries, e, sizeof(PALETTEENTRY) * n);
        }
    }
    ReserveSystemEntries();
}

// Without DDPCAPS_ALLOW256, an 8-bit palette's first and last entries belong to
// the system: always black and white, whatever the game sets.
void Palette::ReserveSystemEntries() {
    if ((caps & DDPCAPS_8BIT) && !(caps & (DDPCAPS_ALLOW256 | DDPCAPS_8BITENTRIES))) {
        // Only the colors: ddraw.dll leaves the game's peFlags.
        entries[0].peRed = entries[0].peGreen = entries[0].peBlue = 0;
        entries[255].peRed = entries[255].peGreen = entries[255].peBlue = 255;
    }
}

// Released more often than referenced (the surfaces it's attached to hold references):
// ddraw.dll detaches it, so GetPalette then says DDERR_NOPALETTEATTACHED instead of
// handing out freed memory. g_lock held.
Palette::~Palette() {
    g_palettes.Remove(this);
    if (dd_) {
        if (dd_->primary && dd_->primary->ShownPalette() == this) dd_->KeepHardwarePalette(entries);
        for (const std::vector<Surface*>* list : {&dd_->surfaces, &dd_->detached})
            for (Surface* s : *list)
                if (s->palette == this) s->palette = nullptr;
        dd_->IntRelease();
    }
}

int Palette::Count() const {
    if (caps & DDPCAPS_1BIT) return 2;
    if (caps & DDPCAPS_2BIT) return 4;
    if (caps & DDPCAPS_4BIT) return 16;
    return 256;
}

HRESULT Palette::QueryInterface(REFIID r, void** out) {
    if (!out) return DDERR_INVALIDPARAMS;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirectDrawPalette)) {
        AddRef();
        *out = this;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG Palette::AddRef() {
    if (!LivePalette(this)) return 0;
    return ULONG(InterlockedIncrement(&ref_));
}

ULONG Palette::Release() {
    DDraw* owner = nullptr;
    int ownerRef = 0;
    LONG r;
    {
        Guard l;  // the check and the decrement go together
        if (!g_palettes.Has(this) || !g_palettes.Genuine(this)) return 0;
        r = InterlockedDecrement(&ref_);
        if (r == 0) {
            owner = dd_;
            ownerRef = ddRef;
            if (owner) owner->IntAddRef();  // see Surface::Release
            delete this;
        }
    }
    if (ownerRef) owner->Release(ownerRef);
    if (owner) owner->IntRelease();
    return ULONG(r < 0 ? 0 : r);
}

HRESULT Palette::GetCaps(LPDWORD c) {
    if (!c) return DDERR_INVALIDPARAMS;
    Guard l;
    // As ddraw.dll: the size and entry layout (not DDPCAPS_ALPHA or the creation-only
    // flags), and PRIMARYSURFACE while the palette is the primary surface's.
    *c = caps & (DDPCAPS_1BIT | DDPCAPS_2BIT | DDPCAPS_4BIT | DDPCAPS_8BIT | DDPCAPS_8BITENTRIES | DDPCAPS_ALLOW256);
    if (dd_ && dd_->primary && dd_->primary->palette == this) *c |= DDPCAPS_PRIMARYSURFACE;
    return DD_OK;
}

HRESULT Palette::GetEntries(DWORD flags, DWORD base, DWORD count, LPPALETTEENTRY out) {
    const DWORD n = DWORD(Count());
    if (flags || !out || !count || base >= n || count > n - base) return DDERR_INVALIDPARAMS;
    Guard l;
    if (caps & DDPCAPS_8BITENTRIES) {
        auto* idx = reinterpret_cast<uint8_t*>(out);
        for (DWORD i = 0; i < count; i++) idx[i] = entries[base + i].peRed;
    } else {
        memcpy(out, entries + base, sizeof(PALETTEENTRY) * count);
    }
    return DD_OK;
}

HRESULT Palette::Initialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) { return DDERR_ALREADYINITIALIZED; }

HRESULT Palette::SetEntries(DWORD flags, DWORD start, DWORD count, LPPALETTEENTRY in) {
    const DWORD n = DWORD(Count());
    if (flags || !in || !count || start >= n || count > n - start) return DDERR_INVALIDPARAMS;
    Guard l;
    if (caps & DDPCAPS_8BITENTRIES) {
        auto* idx = reinterpret_cast<const uint8_t*>(in);
        for (DWORD i = 0; i < count; i++) entries[start + i] = {idx[i], 0, 0, 0};
    } else {
        memcpy(entries + start, in, sizeof(PALETTEENTRY) * count);
    }
    ReserveSystemEntries();
    version = NextCpuSerial();
    DD2D9_TRACE("Palette %p SetEntries start=%lu count=%lu%s", static_cast<void*>(this), start, count,
               dd_ && dd_->primary && dd_->primary->ShownPalette() == this ? " (primary)" : "");
    if (dd_ && dd_->primary && dd_->primary->ShownPalette() == this) dd_->MarkDirty();
    // A device context out on an 8-bit surface without a palette of its own (drawing with the
    // display's, this one shown on the primary) follows the change, as GDI drawing on a
    // palettized display followed the hardware palette. (A surface's own palette doesn't: its
    // DC keeps the colours of its GetDC, as Wine's test_palette_gdi finds.) Pal95 draws its text
    // white after setting entry 1 to white with the DC out: matched against the table taken
    // at GetDC, white became another index (the transparent one), and its letters lost
    // strokes.
    if (dd_)
        for (Surface* sf : dd_->surfaces)
            if (sf->dcOut && !sf->palette && sf->buf && sf->buf->fmt.pal8 && sf->buf->dcPal == this)
                sf->LoadDcColorTable(sf->dcOut, this);
    return DD_OK;
}

}  // namespace dd2d9
