// Surface memory (Buffer) and the DCs GetDC puts on it.
#include "objects.h"
#include "gpures.h"

#include <new>

namespace dd2d9 {

static bool DibFormat(const Format& f) {
    // Red in the low byte (A8B8G8R8, X8B8G8R8): ddraw.dll on Vista and later has no DC for it.
    if (f.bpp == 32 && f.r == 0xFF && f.b == 0xFF0000) return false;
    return !f.zbuf && !f.fourcc && !f.bump && (f.bpp == 8 || f.bpp == 16 || f.bpp == 24 || f.bpp == 32);
}

// A top-down DIB section for format f (DibFormat(f)). As ddraw.dll's: 8-bit with a
// colour table, 24- and 32-bit as BI_RGB (whatever the 32-bit masks), 16-bit with its masks.
static HBITMAP MakeDib(int w, int h, const Format& f, uint8_t** bits) {
    struct {
        BITMAPINFOHEADER h;
        DWORD colors[256];
    } bi = {};
    bi.h.biSize = sizeof(BITMAPINFOHEADER);
    bi.h.biWidth = w;
    bi.h.biHeight = -h;  // top-down
    bi.h.biPlanes = 1;
    bi.h.biBitCount = WORD(f.bpp);
    if (f.pal8) {
        bi.h.biCompression = BI_RGB;
        bi.h.biClrUsed = 256;
        for (int i = 0; i < 256; i++) bi.colors[i] = DWORD(i) * 0x010101u;
    } else if (f.bpp == 24 || f.bpp == 32) {
        bi.h.biCompression = BI_RGB;
    } else {
        bi.h.biCompression = BI_BITFIELDS;
        bi.colors[0] = f.r;
        bi.colors[1] = f.g;
        bi.colors[2] = f.b;
    }
    void* p = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &p, nullptr, 0);
    *bits = static_cast<uint8_t*>(p);
    return dib;
}

static LONG DibPitch(int w, const Format& f) { return ((w * LONG(f.bpp) + 31) / 32) * 4; }

// Surface memory is plain memory with ddraw.dll's pitch (rows aligned to 8 bytes in a
// 32-bit process); GetDC puts a DC right on it (see Buffer::GetDC).
LONG Buffer::PitchFor(int w, const Format& f) {
    // DXT1 is 4 bits/pixel, the others 8.
    if (f.BlockCompressed()) return BlockPitch(w, f);
    // The luma rows (pitch 256-byte aligned, as overlay hardware had it: Wine's
    // test_yv12_overlay), then the two chroma planes at half the pitch.
    if (f.Planar()) return (w + 255) & ~255;
    return ((w * LONG(f.Bytes())) + 7) & ~7;
}

Buffer* Buffer::Create(int w, int h, const Format& f) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return nullptr;
    auto* b = new (std::nothrow) Buffer();
    if (!b) return nullptr;
    b->w = w;
    b->h = h;
    b->fmt = f;
    b->pitch = PitchFor(w, f);
    size_t size;
    if (f.BlockCompressed()) size = size_t(b->pitch) * size_t((h + 3) & ~3);
    else if (f.Planar()) size = size_t(b->pitch) * h + size_t(b->pitch / 2) * ((h + 1) / 2) * 2;
    else size = size_t(b->pitch) * h;
    // Slack around the pixels: games clip sprites loosely and write a few rows past
    // the bottom (Xuan-Yuan Sword 3 EX, 6 rows of its 640x480 back buffer) or above the
    // top, which the video memory or heap slack of XP's drivers took without a crash.
    int rows = h < 64 ? h : 64;
    size_t before = (size_t(b->pitch) * (rows < 16 ? rows : 16) + 15) & ~size_t(15);
    size_t after = size_t(b->pitch) * rows + 4096;
    b->base = static_cast<uint8_t*>(_aligned_malloc(before + size + after, 16));
    if (!b->base) {
        delete b;
        return nullptr;
    }
    memset(b->base, 0, before + size + after);
    b->bits = b->base + before;
    b->ownsMemory = true;
    return b;
}

Buffer* Buffer::CreateLayout(int w, int h, const Format& f) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return nullptr;  // (as Create)
    auto* b = new (std::nothrow) Buffer();
    if (!b) return nullptr;
    b->w = w;
    b->h = h;
    b->fmt = f;
    b->pitch = PitchFor(w, f);
    b->ownsMemory = true;  // as Create's (GetSurfaceDesc tells them apart); base is null
    return b;
}

Buffer* Buffer::CreateExternal(void* mem, int w, int h, LONG pitch, const Format& f) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return nullptr;  // (as Create: sizes the blitter can take)
    auto* b = new (std::nothrow) Buffer();
    if (!b) return nullptr;
    b->w = w;
    b->h = h;
    b->fmt = f;
    b->pitch = pitch;
    b->bits = static_cast<uint8_t*>(mem);
    return b;
}

// gdi32's D3DKMTCreateDCFromMemory (Vista and later, Wine): a DC drawing straight
// into memory of any pitch, as ddraw.dll's surface DCs do.
namespace {
struct KmtCreateDC {
    void* pMemory;
    UINT Format;  // D3DDDIFORMAT
    UINT Width, Height, Pitch;
    HDC hDeviceDc;
    PALETTEENTRY* pColorTable;
    HDC hDc;         // out
    HANDLE hBitmap;  // out
};
struct KmtDestroyDC {
    HDC hDc;
    HANDLE hBitmap;
};
using KmtCreateFn = LONG(WINAPI*)(KmtCreateDC*);
using KmtDestroyFn = LONG(WINAPI*)(const KmtDestroyDC*);
KmtCreateFn g_kmtCreate;
KmtDestroyFn g_kmtDestroy;

void LoadKmt() {
    static bool done;
    if (done) return;
    done = true;
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) return;
    g_kmtCreate = reinterpret_cast<KmtCreateFn>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTCreateDCFromMemory")));
    g_kmtDestroy = reinterpret_cast<KmtDestroyFn>(reinterpret_cast<void*>(GetProcAddress(gdi, "D3DKMTDestroyDCFromMemory")));
    if (!g_kmtCreate || !g_kmtDestroy) g_kmtCreate = nullptr;
}

// The D3DDDIFORMAT for f, or 0 if GDI can't take it.
UINT KmtFormat(const Format& f) {
    if (f.zbuf || f.fourcc || f.bump) return 0;
    // 8-bit surfaces get a DIB copy instead (Buffer::GetDC): their DC's colour table has to
    // follow the palette (GetDC, Palette::SetEntries), and on Windows SetDIBColorTable leaves
    // a DC from D3DKMTCreateDCFromMemory with the table it was made with (Wine's follows).
    // Pal95's text strip kept the grey ramp it was made with: its white text became index
    // 255, its transparent colour, and only the shadow showed. (paltext on Windows 11: white
    // drawn as 255 whatever the palette; Wine: as the palette's white.)
    if (f.pal8) return 0;
    if (f.bpp == 32 && f.r == 0xFF0000 && f.g == 0xFF00 && f.b == 0xFF) return f.a == 0xFF000000u ? 21 : 22;
    if (f.bpp == 32 && f.r == 0x3FF00000 && f.g == 0xFFC00 && f.b == 0x3FF) return 35;  // A2R10G10B10
    if (f.bpp == 24 && f.r == 0xFF0000 && f.g == 0xFF00 && f.b == 0xFF) return 20;
    if (f.bpp == 16 && f.r == 0xF800 && f.g == 0x7E0 && f.b == 0x1F) return 23;
    if (f.bpp == 16 && f.r == 0x7C00 && f.g == 0x3E0 && f.b == 0x1F) return f.a == 0x8000 ? 25 : 24;
    if (f.bpp == 16 && f.r == 0xF00 && f.g == 0xF0 && f.b == 0xF) return f.a == 0xF000 ? 26 : 30;
    return 0;
}
}  // namespace

void Buffer::Release() {
    if (InterlockedDecrement(&ref) != 0) return;
    if (gpu) GpuBufferGone(this);
    if (kmtBitmap) {
        KmtDestroyDC d = {dc, kmtBitmap};
        g_kmtDestroy(&d);
        dc = nullptr;
    }
    if (dc) DeleteDC(dc);
    if (shadow) DeleteObject(shadow);
    if (ownsMemory) _aligned_free(base);
    delete this;
}

// A DC on the memory itself where GDI can do that; otherwise (no D3DKMT, or a format
// GDI only takes as a DIB) through a shadow DIB: the pixels are copied in here and
// back in EndDC().
HDC Buffer::GetDC() {
    if (dc) {
        if (shadow) CopyToShadow();
        return dc;
    }
    LoadKmt();
    const UINT kmt = KmtFormat(fmt);
    if (g_kmtCreate && kmt && pitch > 0) {
        PALETTEENTRY gray[256];
        for (int i = 0; i < 256; i++) gray[i] = {BYTE(i), BYTE(i), BYTE(i), 0};
        HDC screen = CreateCompatibleDC(nullptr);
        KmtCreateDC c = {bits, kmt, UINT(w), UINT(h), UINT(pitch), screen, fmt.pal8 ? gray : nullptr, nullptr, nullptr};
        const LONG status = g_kmtCreate(&c);
        if (screen) DeleteDC(screen);
        if (status == 0 && c.hDc) {
            dc = c.hDc;
            kmtBitmap = c.hBitmap;
            return dc;
        }
        DD2D9_LOG("D3DKMTCreateDCFromMemory(%dx%d, format %u, pitch %ld) failed (0x%lx): using a DIB copy", w, h,
                  kmt, pitch, status);
    }
    if (!DibFormat(fmt)) return nullptr;
    if (!(shadow = MakeDib(w, h, fmt, &shadowBits))) return nullptr;
    if (!(dc = CreateCompatibleDC(nullptr))) {
        DeleteObject(shadow);
        shadow = nullptr;
        return nullptr;
    }
    SelectObject(dc, shadow);
    CopyToShadow();
    return dc;
}

void Buffer::CopyToShadow() {
    const LONG sp = DibPitch(w, fmt), row = LONG(w * fmt.Bytes());
    for (int y = 0; y < h; y++) memcpy(shadowBits + y * sp, Row(y), size_t(row));
    shadowWas.assign(shadowBits, shadowBits + size_t(sp) * size_t(h));
}

// Only what GDI changed goes back: GetDC is allowed on a locked surface, and on XP the DC
// draws into the surface memory itself, so the game's writes through the lock while the DC
// is out stay (copied back whole, the shadow's older copy of them overwrote them).
void Buffer::EndDC() {  // (GdiFlush done by the caller)
    if (!shadow) return;
    const LONG sp = DibPitch(w, fmt), row = LONG(w * fmt.Bytes());
    const bool snapshot = shadowWas.size() >= size_t(sp) * size_t(h);
    for (int y = 0; y < h; y++) {
        const uint8_t* now = shadowBits + y * sp;
        uint8_t* mem = Row(y);
        if (!snapshot) {
            memcpy(mem, now, size_t(row));
            continue;
        }
        const uint8_t* was = shadowWas.data() + size_t(y) * size_t(sp);
        if (!memcmp(now, was, size_t(row))) continue;
        for (LONG i = 0; i < row; i++)
            if (now[i] != was[i]) mem[i] = now[i];
    }
}

}  // namespace dd2d9
