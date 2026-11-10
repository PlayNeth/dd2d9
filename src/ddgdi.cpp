// GDI and DirectDraw: surfaces found by their DC, and a windowed game's own GDI drawing into
// the primary shown in its window.
#include "ddint.h"

namespace dd2d9 {

namespace {
// The surface whose GetDC handed out `dc`: any DirectDraw object's, as ddraw.dll searches
// them. g_lock held.
Surface* SurfaceWithDC(HDC dc) {
    if (!dc) return nullptr;
    for (DDraw* d : g_ddraws)
        for (Surface* s : d->surfaces)
            if (s->dcOut == dc) return s;
    return nullptr;
}

// The primary a normal-mode object shows in `hwnd` now, or null. g_lock held.
Surface* ShownPrimary(HWND hwnd) {
    for (DDraw* d : g_ddraws)
        if (d->primary && !d->Exclusive() && !d->Superseded() && !d->Suspended() && !d->primary->IsLost(false) &&
            d->PresentWindow() == hwnd)
            return d->primary;
    return nullptr;
}
}  // namespace

HRESULT DDraw::GetSurfaceFromDC(HDC dc, void** out, int version) {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    Guard l;
    Surface* s = SurfaceWithDC(dc);
    if (!s) return DDERR_NOTFOUND;
    s->AddRef(Surface::SlotFor(version));
    *out = s->Iface(version);
    return DD_OK;
}

bool PresentsOnWindow(HWND hwnd) {
    if (!hwnd) return false;
    Guard l;
    for (DDraw* d : g_ddraws)
        if (d->primary && d->PresentWindow() == hwnd) return true;
    return false;
}

bool DrawIntoShownPrimary(HDC dc, void (*draw)(HDC, void*), void* ctx) {
    if (!dc || GetObjectType(dc) != OBJ_DC) return false;  // (a memory DC: not the screen)
    const HWND hwnd = WindowFromDC(dc);
    if (!hwnd) return false;
    // Most such calls go to a window no primary is shown in (every one, in exclusive mode):
    // answered before the regions are built. Looked for again below, with them.
    {
        Guard l;
        if (!ShownPrimary(hwnd)) return false;
    }
    // The window DC's origin is copied, not a mapping mode or a world transform.
    if (GetMapMode(dc) != MM_TEXT || GetGraphicsMode(dc) != GM_COMPATIBLE) return false;
    POINT org, view, win;
    if (!GetDCOrgEx(dc, &org) || !GetViewportOrgEx(dc, &view) || !GetWindowOrgEx(dc, &win)) return false;
    // Where the call may draw, in screen coordinates (which are the windowed primary's, see
    // PresentSource): the client area, within the DC's visible region and the game's own
    // clipping region (device coordinates, from the DC's origin).
    RECT client;
    if (!GetClientRect(hwnd, &client)) return false;
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&client), 2);
    HRGN clip = CreateRectRgnIndirect(&client);
    if (!clip) return false;
    if (HRGN r = CreateRectRgn(0, 0, 0, 0)) {
        if (GetRandomRgn(dc, r, SYSRGN) == 1) CombineRgn(clip, clip, r, RGN_AND);
        if (GetClipRgn(dc, r) == 1) {
            OffsetRgn(r, org.x, org.y);
            CombineRgn(clip, clip, r, RGN_AND);
        }
        DeleteObject(r);
    }
    bool done = false;
    {
        Guard l;
        Surface* const s = ShownPrimary(hwnd);
        HDC p = nullptr;
        if (s && SUCCEEDED(s->GetDC(&p))) {
            const int saved = SaveDC(p);
            SelectClipRgn(p, clip);
            SetViewportOrgEx(p, org.x + view.x, org.y + view.y, nullptr);
            SetWindowOrgEx(p, win.x, win.y, nullptr);
            // What the blits use of the DC: the stretch mode, the colours a monochrome
            // source takes, the brush (PatBlt, and raster operations with a pattern).
            SetStretchBltMode(p, GetStretchBltMode(dc));
            SetBkColor(p, GetBkColor(dc));
            SetTextColor(p, GetTextColor(dc));
            SelectObject(p, GetCurrentObject(dc, OBJ_BRUSH));
            POINT brushOrg;
            if (GetBrushOrgEx(dc, &brushOrg)) SetBrushOrgEx(p, org.x + brushOrg.x, org.y + brushOrg.y, nullptr);
            draw(p, ctx);
            GdiFlush();
            RestoreDC(p, saved);
            s->ReleaseDC(p, 7);
            done = true;
            static HWND logged;
            if (logged != hwnd) {
                logged = hwnd;
                DD2D9_LOG("gdi: the game draws into window %p itself: drawn into the primary shown there",
                          static_cast<void*>(hwnd));
            }
        }
    }
    DeleteObject(clip);
    return done;
}

// The GetSurfaceFromDC export: any DirectDraw object's surface, as IDirectDrawSurface.
HRESULT DDraw::FindSurfaceFromDC(HDC dc, void** out, HDC* deviceDC) {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    if (!deviceDC) return E_INVALIDARG;
    *deviceDC = nullptr;
    Guard l;
    Surface* s = SurfaceWithDC(dc);
    if (!s) return DDERR_NOTFOUND;
    s->AddRef(Surface::SlotFor(1));
    *out = s->Iface(1);
    // ddraw.dll also gives a display DC (one for the process, kept for good).
    static HDC display = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
    *deviceDC = display;
    return DD_OK;
}

}  // namespace dd2d9
