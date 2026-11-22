// GDI and DirectDraw: surfaces found by their DC, a windowed game's own GDI drawing into the
// primary shown in its window, and the primary copied to the clipboard on PrintScreen.
#include "ddint.h"

#include <algorithm>
#include <atomic>

namespace dd2d9 {

namespace {
// The surface whose GetDC handed out `dc`: any DirectDraw object's, as ddraw.dll searches
// them. g_lock held.
Surface* SurfaceWithDC(HDC dc) {
    if (!dc) return nullptr;
    for (DDraw* d : g_ddraws)
        for (Surface* s : d->surfaces)
            if (s->dcOut == dc || (s->dcKept && s->buf->dc == dc)) return s;
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

// ---------------------------------------------------------------- PrintScreen

namespace {
std::atomic<bool> g_printScreen{false};
}

// EnablePrintScreen under HKLM\Software\Microsoft\DirectDraw, read as ddraw.dll reads it each
// time a DirectDraw object is made (FUN_7373ad42, with the key's other values): any value of
// at most four bytes but 0. Where the key can't be opened, it stays as it was.
void ReadPrintScreenSetting() {
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\DirectDraw", 0, KEY_QUERY_VALUE, &k)) return;
    DWORD v = 0, size = sizeof(v);
    g_printScreen.store(!RegQueryValueExA(k, "EnablePrintScreen", nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &size) && v);
    RegCloseKey(k);
}

bool PrintScreenEnabled() { return g_printScreen.load(); }

// GDI's palette of an 8-bit screen (GetSystemPaletteEntries), kept as XP's GDI kept it beside
// DirectDraw (measured, fmtprobe's sections 36 and 37). A mode set starts it as kScreenPalette:
// XP's own then (0xRRGGBB), the static colours at its ends (two of them the theme's), what lay
// between varying from run to run. A palette put on the primary becomes it, as its entries
// then read back (see Palette::Realized), with no static colours (SYSPAL_NOSTATIC256);
// SetEntries on that palette writes its entries through as they are, the first and last too.
// The palette taken off, or the primary gone, the static colours come back at the ends and the
// rest stays. A palette on another surface, a flip, the palette object released while the
// primary holds it: nothing. One for the process (the screen's), under a lock of its own:
// GDI's callers never wait for g_lock.
static const DWORD kScreenPalette[256] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xc0c0c0, 0xaca899, 0xece9d8, 0x993300, 0xcc3300,
    0xff3300, 0x006600, 0x336600, 0x666600, 0x996600, 0xcc6600, 0xff6600, 0x009900, 0x339900, 0x669900, 0x999900, 0xcc9900,
    0xff9900, 0x00cc00, 0x33cc00, 0x66cc00, 0x99cc00, 0xcccc00, 0xffcc00, 0x66ff00, 0x99ff00, 0xccff00, 0x00ff33, 0x3300ff,
    0x660033, 0x990033, 0xcc0033, 0xff0033, 0x0033ff, 0x333333, 0x663333, 0x993333, 0xcc3333, 0xff3333, 0x006633, 0x336633,
    0x666633, 0x996633, 0xcc6633, 0xff6633, 0x009933, 0x339933, 0x669933, 0x999933, 0xcc9933, 0xff9933, 0x00cc33, 0x33cc33,
    0x66cc33, 0x99cc33, 0xcccc33, 0xffcc33, 0x33ff33, 0x66ff33, 0x99ff33, 0xccff33, 0xffff33, 0x000066, 0x330066, 0x660066,
    0x990066, 0xcc0066, 0xff0066, 0x003366, 0x333366, 0x663366, 0x993366, 0xcc3366, 0xff3366, 0x006666, 0x336666, 0x666666,
    0x996666, 0xcc6666, 0x009966, 0x339966, 0x669966, 0x999966, 0xcc9966, 0xff9966, 0x00cc66, 0x33cc66, 0x99cc66, 0xcccc66,
    0xffcc66, 0x00ff66, 0x33ff66, 0x99ff66, 0xccff66, 0xff00cc, 0xcc00ff, 0x009999, 0x993399, 0x990099, 0xcc0099, 0x000099,
    0x333399, 0x660099, 0xcc3399, 0xff0099, 0x006699, 0x336699, 0x663399, 0x996699, 0xcc6699, 0xff3399, 0x339999, 0x669999,
    0x999999, 0xcc9999, 0xff9999, 0x00cc99, 0x33cc99, 0x66cc66, 0x99cc99, 0xcccc99, 0xffcc99, 0x00ff99, 0x33ff99, 0x66cc99,
    0x99ff99, 0xccff99, 0xffff99, 0x0000cc, 0x330099, 0x6600cc, 0x9900cc, 0xcc00cc, 0x003399, 0x3333cc, 0x6633cc, 0x9933cc,
    0xcc33cc, 0xff33cc, 0x0066cc, 0x3366cc, 0x666699, 0x9966cc, 0xcc66cc, 0xff6699, 0x0099cc, 0x3399cc, 0x6699cc, 0x9999cc,
    0xcc99cc, 0xff99cc, 0x00cccc, 0x33cccc, 0x66cccc, 0x99cccc, 0xcccccc, 0xffcccc, 0x00ffcc, 0x33ffcc, 0x66ff99, 0x99ffcc,
    0xccffcc, 0xffffcc, 0x3300cc, 0x6600ff, 0x9900ff, 0x0033cc, 0x3333ff, 0x6633ff, 0x9933ff, 0xcc33ff, 0xff33ff, 0x0066ff,
    0x3366ff, 0x6666cc, 0x9966ff, 0xcc66ff, 0xff66cc, 0x0099ff, 0x3399ff, 0x6699ff, 0x9999ff, 0xcc99ff, 0xff99ff, 0x00ccff,
    0x33ccff, 0x66ccff, 0x99ccff, 0xccccff, 0xffccff, 0x33ffff, 0x66ffcc, 0x99ffff, 0xccffff, 0xff6666, 0x66ff66, 0xffff66,
    0x6666ff, 0xff66ff, 0x66ffff, 0xa50021, 0x5f5f5f, 0x777777, 0x868686, 0x969696, 0xcbcbcb, 0xb2b2b2, 0xd7d7d7, 0xdddddd,
    0xe3e3e3, 0xeaeaea, 0xc0dcc0, 0xa6caf0, 0x000033, 0x330000, 0x330033, 0x003333, 0x161616, 0x1c1c1c, 0x222222, 0x292929,
    0x555555, 0x4d4d4d, 0x424242, 0x393939, 0xff7c80, 0xff5050, 0xd60093, 0xccecff, 0xefd6c6, 0xe7e7d6, 0xada990, 0x33ff00,
    0x660000, 0x990000, 0xcc0000, 0x003300, 0x333300, 0x663300, 0xfffbf0, 0x004e98, 0x808080, 0xff0000, 0x00ff00, 0xffff00,
    0x0000ff, 0xff00ff, 0x00ffff, 0xffffff};

namespace {
SRWLOCK g_screenPaletteLock = SRWLOCK_INIT;
PALETTEENTRY g_screenPalette[256];  // (g_screenPaletteLock)
bool g_screenPaletteMade, g_screenPaletteNoStatic;

PALETTEENTRY ScreenDefault(int i) {
    return {BYTE(kScreenPalette[i] >> 16), BYTE(kScreenPalette[i] >> 8), BYTE(kScreenPalette[i]), 0};
}
// (g_screenPaletteLock held.) The static colours put back; `all`: the whole of XP's table.
void ScreenStatics(bool all) {
    for (int i = 0; i < 256; i++)
        if (all || i < 10 || i >= 246 || !g_screenPaletteMade) g_screenPalette[i] = ScreenDefault(i);
    g_screenPaletteMade = true;
    g_screenPaletteNoStatic = false;
}
}  // namespace

void ScreenPaletteReset() {
    AcquireSRWLockExclusive(&g_screenPaletteLock);
    ScreenStatics(true);
    ReleaseSRWLockExclusive(&g_screenPaletteLock);
}

void ScreenPaletteShown(const PALETTEENTRY* entries) {
    AcquireSRWLockExclusive(&g_screenPaletteLock);
    for (int i = 0; i < 256; i++) g_screenPalette[i] = {entries[i].peRed, entries[i].peGreen, entries[i].peBlue, 0};
    g_screenPaletteMade = g_screenPaletteNoStatic = true;
    ReleaseSRWLockExclusive(&g_screenPaletteLock);
}

void ScreenPaletteSet(const PALETTEENTRY* entries, DWORD start, DWORD count) {
    AcquireSRWLockExclusive(&g_screenPaletteLock);
    if (!g_screenPaletteMade) ScreenStatics(true);
    for (DWORD i = start; i < start + count && i < 256; i++)
        g_screenPalette[i] = {entries[i].peRed, entries[i].peGreen, entries[i].peBlue, 0};
    ReleaseSRWLockExclusive(&g_screenPaletteLock);
}

void ScreenPaletteGone() {
    AcquireSRWLockExclusive(&g_screenPaletteLock);
    ScreenStatics(false);
    ReleaseSRWLockExclusive(&g_screenPaletteLock);
}

bool ScreenPaletteGet(PALETTEENTRY out[256]) {
    AcquireSRWLockExclusive(&g_screenPaletteLock);
    if (!g_screenPaletteMade) ScreenStatics(true);
    memcpy(out, g_screenPalette, sizeof(g_screenPalette));
    const bool noStatic = g_screenPaletteNoStatic;
    ReleaseSRWLockExclusive(&g_screenPaletteLock);
    return noStatic;
}

// PrintScreen let go in the exclusive window with EnablePrintScreen set: the primary copied
// to the clipboard as a DIB, as ddraw.dll's window procedure copies it (FUN_7375dc1e,
// FUN_7375d82c): 8 bits with its own palette, or without one GDI's palette of the screen
// (GetSystemPaletteEntries on ddraw.dll's DC: ScreenPaletteGet here);
// 16 bits of 5-5-5, or of 5-6-5 with its masks, 24 bits, 32 bits with its masks (red high), or without them where red is low (as ddraw.dll
// writes it: its colours swapped); in any other format, nothing. The rows bottom up, each as
// many bytes as the DIB's, read through a whole lock (what Direct3D drew too). Unlike
// ddraw.dll, the memory is freed and the clipboard closed when the clipboard can't be opened
// or won't take it. g_lock held.
void CopyPrimaryToClipboard(DDraw* dd, HWND hwnd) {
    Surface* p = dd->primary;
    if (!p) return;
    const Format& f = p->buf->fmt;
    DWORD compression = BI_RGB, extra = 0;  // the colour table's entries, or the masks
    DWORD table[256];
    if (f.pal8) {
        if (p->palette && p->palette->Count() != 256) return;  // (ddraw.dll's check: an 8-bit palette)
        PALETTEENTRY screen[256];
        if (!p->palette) ScreenPaletteGet(screen);
        const PALETTEENTRY* e = p->palette ? p->palette->entries : screen;
        for (int i = 0; i < 256; i++) table[i] = DWORD(e[i].peRed) << 16 | DWORD(e[i].peGreen) << 8 | e[i].peBlue;
        extra = 256;
    } else if ((f.bpp == 16 && f.r == 0xF800 && f.g == 0x07E0 && f.b == 0x001F) ||
               (f.bpp == 32 && f.r == 0xFF0000 && f.g == 0xFF00 && f.b == 0xFF)) {
        compression = BI_BITFIELDS;
        table[0] = f.r, table[1] = f.g, table[2] = f.b;
        extra = 3;
    } else if (!(f.bpp == 16 && f.r == 0x7C00 && f.g == 0x03E0 && f.b == 0x001F) &&
               !(f.bpp == 24 && f.r == 0xFF0000 && f.g == 0xFF00 && f.b == 0xFF) &&
               !(f.bpp == 32 && f.r == 0xFF && f.g == 0xFF00 && f.b == 0xFF0000)) {
        return;
    }
    const LONG w = p->W(), h = p->H();
    const size_t stride = (size_t(w) * f.bpp + 31) / 32 * 4;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT | GMEM_DDESHARE, stride * h + extra * 4 + sizeof(BITMAPINFOHEADER));
    auto* bi = g ? static_cast<BITMAPINFOHEADER*>(GlobalLock(g)) : nullptr;
    if (!bi) {
        if (g) GlobalFree(g);
        return;
    }
    bi->biSize = sizeof(BITMAPINFOHEADER);
    bi->biWidth = w;
    bi->biHeight = h;
    bi->biPlanes = 1;
    bi->biBitCount = WORD(f.bpp);
    bi->biCompression = compression;
    bi->biXPelsPerMeter = bi->biYPelsPerMeter = 1;
    bi->biClrUsed = bi->biClrImportant = f.pal8 ? 256 : 0;
    memcpy(bi + 1, table, extra * 4);
    BYTE* bits = reinterpret_cast<BYTE*>(bi + 1) + extra * 4;
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    const bool locked = SUCCEEDED(p->Lock(nullptr, &d, DDLOCK_WAIT));
    if (locked) {
        const size_t n = std::min<size_t>(stride, size_t(d.lPitch));
        for (LONG y = 0; y < h; y++)
            memcpy(bits + stride * size_t(h - 1 - y), static_cast<const BYTE*>(d.lpSurface) + size_t(y) * d.lPitch, n);
        p->Unlock(nullptr, nullptr, 7);
    }
    GlobalUnlock(g);
    if (!locked || !OpenClipboard(hwnd)) {
        GlobalFree(g);
        return;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, g)) GlobalFree(g);
    CloseClipboard();
    DD2D9_LOG("PrintScreen: the primary (%ldx%ld, %lu bits) copied to the clipboard", w, h, static_cast<unsigned long>(f.bpp));
}

}  // namespace dd2d9
