// DirectDraw clippers.
#include "surfaceint.h"

namespace dd2d9 {

// ddraw.dll checks that a clipper method is called on a live clipper: a released or
// made-up pointer gets an error (or 0 from AddRef/Release) instead of a crash. Games
// that release a clipper once too often survive that here as well.
static LiveSet<Clipper> g_clippers;

bool LiveClipper(const Clipper* c) { return g_clippers.Has(c); }

Clipper::Clipper(DDraw* dd) : dd_(dd) {
    WritableVtbl(this);
    if (dd_) dd_->IntAddRef();
    g_clippers.Add(this);
}

Clipper::~Clipper() {
    // Called with g_lock held.
    g_clippers.Remove(this);
    for (Surface* s : users) {
        s->clipper = nullptr;
        s->MarkDirtyIfVisible();
    }
    if (dd_) dd_->IntRelease();
}

// A clipper the class factory made answers only Initialize until it gets one.
static HRESULT ClipperReady(const Clipper* c) {
    if (!LiveClipper(c)) return DDERR_INVALIDPARAMS;
    return c->initialized ? DD_OK : DDERR_NOTINITIALIZED;
}

HRESULT Clipper::QueryInterface(REFIID r, void** out) {
    if (!out || !LiveClipper(this)) return DDERR_INVALIDPARAMS;
    if (IsEqualGUID(r, IID_IUnknown) || IsEqualGUID(r, IID_IDirectDrawClipper)) {
        AddRef();
        *out = this;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG Clipper::AddRef() {
    if (!LiveClipper(this)) return 0;
    return ULONG(InterlockedIncrement(&ref_));
}

ULONG Clipper::Release() {
    DDraw* owner = nullptr;
    int ownerRef = 0;
    LONG r;
    {
        Guard l;  // the check and the decrement go together
        // Also a no-op with the clipper's vtable overwritten (as ddraw.dll).
        if (!g_clippers.Has(this) || !g_clippers.Genuine(this)) return 0;
        r = InterlockedDecrement(&ref_);
        if (r == 0) {
            owner = dd_;
            ownerRef = ddRef;
            if (owner) owner->IntAddRef();  // see Surface::Release
            delete this;
        }
    }
    // The DirectDraw interface it referenced (made through IDirectDraw4/7): released
    // last and without g_lock, as it may tear the object down.
    if (ownerRef) owner->Release(ownerRef);
    if (owner) owner->IntRelease();
    return ULONG(r < 0 ? 0 : r);
}

// The rectangles in `in` that meet `clip`, clipped to it (ddraw.dll's FUN_737364ff).
static std::vector<RECT> ClipRects(const RECT* in, size_t n, const RECT& clip) {
    std::vector<RECT> out;
    for (size_t i = 0; i < n; i++) {
        RECT r;
        if (IntersectRect(&r, &in[i], &clip)) out.push_back(r);
    }
    return out;
}

// As ddraw.dll (spec: re/spec/palette-clipper-exports.md): with a window, the part of
// its client area that is visible on the screen, in screen coordinates; otherwise the
// list SetClipList gave. Either clipped to `rect`.
HRESULT Clipper::GetClipList(LPRECT rect, LPRGNDATA data, LPDWORD size) {
    if (HRESULT hr = ClipperReady(this); FAILED(hr)) return hr;
    if (data) data->rdh.nCount = 0;  // ddraw.dll's first step, whatever follows
    Guard l;
    std::vector<uint8_t> list;
    if (hwnd) {
        RECT cr;
        if (!GetClientRect(hwnd, &cr)) return E_FAIL;  // the window is gone
        MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
        std::vector<RECT> rects;
        HRGN rgn = CreateRectRgn(0, 0, 0, 0);
        HDC dc = GetDC(hwnd);
        // The window's visible region (SYSRGN is in screen coordinates).
        if (dc && rgn && GetRandomRgn(dc, rgn, SYSRGN) == 1) {
            const DWORD bytes = GetRegionData(rgn, 0, nullptr);
            std::vector<uint8_t> raw(bytes);
            if (bytes && GetRegionData(rgn, bytes, reinterpret_cast<RGNDATA*>(raw.data()))) {
                const auto* rd = reinterpret_cast<const RGNDATA*>(raw.data());
                rects = ClipRects(reinterpret_cast<const RECT*>(rd->Buffer), rd->rdh.nCount, cr);
            }
        } else {
            // No region to be had: the client area on the screen.
            RECT screen = {GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN)};
            screen.right = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
            screen.bottom = screen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
            rects = ClipRects(&cr, 1, screen);
        }
        if (dc) ReleaseDC(hwnd, dc);
        if (rgn) DeleteObject(rgn);
        if (rect) rects = ClipRects(rects.data(), rects.size(), *rect);
        list.resize(sizeof(RGNDATAHEADER) + rects.size() * sizeof(RECT));
        auto* rd = reinterpret_cast<RGNDATA*>(list.data());
        rd->rdh.dwSize = sizeof(RGNDATAHEADER);
        rd->rdh.iType = RDH_RECTANGLES;
        rd->rdh.nCount = DWORD(rects.size());
        rd->rdh.nRgnSize = DWORD(rects.size() * sizeof(RECT));
        SetRectEmpty(&rd->rdh.rcBound);
        for (const RECT& r : rects) UnionRect(&rd->rdh.rcBound, &rd->rdh.rcBound, &r);
        if (!rects.empty()) memcpy(rd->Buffer, rects.data(), rects.size() * sizeof(RECT));
    } else {
        if (clipList_.empty()) return DDERR_NOCLIPLIST;
        list = clipList_;
        if (rect) {
            // Clipped to `rect`; the header's bounds and size stay as they were.
            auto* rd = reinterpret_cast<RGNDATA*>(list.data());
            const size_t n = std::min<size_t>(rd->rdh.nCount, (list.size() - sizeof(RGNDATAHEADER)) / sizeof(RECT));
            const std::vector<RECT> rects = ClipRects(reinterpret_cast<const RECT*>(rd->Buffer), n, *rect);
            rd->rdh.nCount = DWORD(rects.size());
            if (!rects.empty()) memcpy(rd->Buffer, rects.data(), rects.size() * sizeof(RECT));
        }
    }
    if (!size) return DDERR_INVALIDPARAMS;
    if (!data) {
        *size = DWORD(list.size());
        return DD_OK;
    }
    if (*size < list.size()) {
        *size = DWORD(list.size());
        return DDERR_REGIONTOOSMALL;
    }
    memcpy(data, list.data(), list.size());
    return DD_OK;
}

HRESULT Clipper::Usable() const {
    if (hwnd) return IsWindow(hwnd) ? DD_OK : E_FAIL;
    return clipList_.empty() ? DDERR_NOCLIPLIST : DD_OK;
}

std::vector<RECT> Clipper::Region(const Surface* target) const {
    std::vector<RECT> out;
    if (hwnd) {
        if (!IsWindow(hwnd)) return out;
        if (target->isPrimary && target->dd->Exclusive()) {
            // The window covers the (virtual) screen, whatever its real position.
            out.push_back(RECT{0, 0, target->W(), target->H()});
            return out;
        }
        RECT cr;
        GetClientRect(hwnd, &cr);
        if (target->isPrimary) MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);  // screen coordinates
        out.push_back(cr);
        return out;
    }
    if (clipList_.size() >= sizeof(RGNDATAHEADER)) {
        const auto* rd = reinterpret_cast<const RGNDATA*>(clipList_.data());
        const auto* r = reinterpret_cast<const RECT*>(rd->Buffer);
        const size_t n = (clipList_.size() - sizeof(RGNDATAHEADER)) / sizeof(RECT);
        for (size_t i = 0; i < rd->rdh.nCount && i < n; i++) out.push_back(r[i]);
    }
    return out;
}

HRESULT Clipper::GetHWnd(HWND* out) {
    if (HRESULT hr = ClipperReady(this); FAILED(hr)) return hr;
    if (!out) return DDERR_INVALIDPARAMS;
    *out = hwnd;
    return DD_OK;
}

// Only a clipper from the class factory needs it: it then belongs to that DirectDraw
// object (without a reference, as in ddraw.dll).
HRESULT Clipper::Initialize(LPDIRECTDRAW dd, DWORD flags) {
    if (!LiveClipper(this)) return DDERR_INVALIDPARAMS;
    Guard l;
    if (initialized) return DDERR_ALREADYINITIALIZED;
    if (flags) return DDERR_INVALIDPARAMS;
    if (DDraw* d = DDraw::From(dd)) {
        dd_ = d;
        d->IntAddRef();  // keeps dd2d9's object allocated, not a game reference
    }
    initialized = true;
    return DD_OK;
}

// ddraw.dll keeps no track of changes: always "changed".
HRESULT Clipper::IsClipListChanged(WINBOOL* changed) {
    if (HRESULT hr = ClipperReady(this); FAILED(hr)) return hr;
    if (!changed) return DDERR_INVALIDPARAMS;
    *changed = TRUE;
    return DD_OK;
}

// Checks in ddraw.dll's order.
HRESULT Clipper::SetClipList(LPRGNDATA data, DWORD flags) {
    if (HRESULT hr = ClipperReady(this); FAILED(hr)) return hr;
    if (flags) return DDERR_INVALIDPARAMS;
    if (data) {
        if (!data->rdh.nCount) return DDERR_INVALIDCLIPLIST;
        // A clipper made through IDirectDraw takes any header.
        if (!fromDD1 && (data->rdh.dwSize < sizeof(RGNDATAHEADER) || data->rdh.iType != RDH_RECTANGLES))
            return DDERR_INVALIDCLIPLIST;
    }
    Guard l;
    if (hwnd) return DDERR_CLIPPERISUSINGHWND;
    if (!data) {
        clipList_.clear();
        return DD_OK;
    }
    // dd2d9's own limit, which also keeps the size below from overflowing.
    if (data->rdh.nCount > 65536) return DDERR_INVALIDCLIPLIST;
    size_t size = sizeof(RGNDATAHEADER) + size_t(data->rdh.nCount) * sizeof(RECT);
    clipList_.assign(reinterpret_cast<uint8_t*>(data), reinterpret_cast<uint8_t*>(data) + size);
    return DD_OK;
}

HRESULT Clipper::SetHWnd(DWORD flags, HWND h) {
    if (HRESULT hr = ClipperReady(this); FAILED(hr)) return hr;
    if (flags || (h && !IsWindow(h))) return DDERR_INVALIDPARAMS;
    Guard l;
    hwnd = h;
    if (dd_) dd_->MarkDirty();
    return DD_OK;
}

}  // namespace dd2d9
