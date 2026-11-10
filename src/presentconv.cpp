// The presenter's conversion of the primary surface's pixels to X8R8G8B8 (see presenter.h).
#include "objects.h"
#include "convert.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace dd2d9 {

// The presenter thread's (the only one using lut16_), from a copy of the gamma ramp taken
// under gammaLock_: built without it, so a game setting its ramp every frame (a fade) doesn't
// wait for the 65536 conversions.
void Presenter::BuildLut16(const Format& f, const uint8_t (&gamma)[3][256], DWORD gammaVersion) {
    lut16_.resize(65536);
    // Each channel PixelToARGB gives depends on that channel's bits alone: a table per channel
    // (its values, through the gamma ramp), then three lookups an entry, not 65536 conversions
    // of three divisions each (a fade sets a new ramp every frame). (16-bit formats only: an
    // 8-bit one is converted through its palette.)
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
            conv.Run(row + x0 * int(f.Bytes()), o, n);  // (any alpha left in: the staging surface is X8R8G8B8)
            if (gammaOn) {
                for (int i = 0; i < n; i++) {
                    const uint32_t c = o[i];
                    o[i] = (uint32_t(gamma[0][(c >> 16) & 0xFF]) << 16) | (uint32_t(gamma[1][(c >> 8) & 0xFF]) << 8) |
                           gamma[2][c & 0xFF];
                }
            }
        }
    }
}

}  // namespace dd2d9
