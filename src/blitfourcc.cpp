// Blits with a FourCC side (DXT, YUY2/UYVY, YV12/I420): Surface::BltFourCC.
#include "blitint.h"
#include "fourcc.h"

#include <algorithm>
#include <new>
#include <vector>

namespace dd2d9 {

// A copy between two surfaces of one FourCC format that can move whole blocks of bw x bh
// pixels as they are: the same size, both rectangles inside their surfaces with their left
// and top on a block boundary, and a size that isn't whole blocks only up to both surfaces'
// edges (the last block then goes whole).
static bool WholeBlocks(const RECT& d, const Buffer* db, const RECT& s, const Buffer* sb, int bw, int bh) {
    const LONG w = d.right - d.left, h = d.bottom - d.top;
    if (w <= 0 || h <= 0 || w != s.right - s.left || h != s.bottom - s.top) return false;
    if (d.left < 0 || d.top < 0 || d.right > db->w || d.bottom > db->h || s.left < 0 || s.top < 0 || s.right > sb->w ||
        s.bottom > sb->h)
        return false;
    if (d.left % bw || s.left % bw || d.top % bh || s.top % bh) return false;
    return (w % bw == 0 || (d.right == db->w && s.right == sb->w)) && (h % bh == 0 || (d.bottom == db->h && s.bottom == sb->h));
}

// A blit with a FourCC side (DXT or YUV): each such side decoded (A8R8G8B8) for the blit
// itself, the destination's part the rectangle touches encoded back. A plain copy between
// two surfaces of one format, on whole blocks (DXT) or pixel pairs (YUV), is copied as it is:
// a Direct3D texture Load or Blt of DXT levels from system memory (decoding and encoding
// again changed the blocks: the encoder doesn't give back what a good encoder made), and a
// video player's double buffering. Always a copy from a source (BltLocked sends fills on).
HRESULT Surface::BltFourCC(RECT* dstRect, Surface* src, RECT* srcRect, DWORD flags, DDBLTFX* fx, bool useClipper,
                           bool destKeyFromSource) {
    static const Format argb = [] {
        DDPIXELFORMAT pf = {sizeof(pf), DDPF_RGB | DDPF_ALPHAPIXELS, 0, {32}, {0x00FF0000}, {0x0000FF00}, {0x000000FF}, {0xFF000000}};
        Format f;
        Format::FromDDPF(pf, &f);
        return f;
    }();
    Buffer* const db = buf;
    Buffer* const sb = src->buf;
    const RECT dr = dstRect ? *dstRect : RECT{0, 0, db->w, db->h};
    const RECT sr = srcRect ? *srcRect : RECT{0, 0, sb->w, sb->h};
    const bool plainCopy = sb->fmt == db->fmt && !(flags & ~DWORD(DDBLT_WAIT | DDBLT_ASYNC | DDBLT_DONOTWAIT)) &&
                           !(useClipper && clipper);
    // Planar (YV12, I420): by 2x2 block, within one surface only apart (overlapping blocks
    // would copy in the wrong order).
    RECT overlap;
    if (plainCopy && db->fmt.Planar() && WholeBlocks(dr, db, sr, sb, 2, 2) &&
        (sb != db || (dr.left == sr.left && dr.top == sr.top) || !IntersectRect(&overlap, &dr, &sr))) {
        src->CpuAccess(false);
        CpuAccess(true, &dr);
        CopyPlanar(db, dr, sb, sr);
        return DD_OK;
    }
    if (plainCopy && db->fmt.BlockCompressed() && WholeBlocks(dr, db, sr, sb, 4, 4)) {
        src->CpuAccess(false);
        CpuAccess(true, &dr);
        CopyBlocks(db, dr, sb, sr);
        return DD_OK;
    }
    if (plainCopy && db->fmt.Yuv() && WholeBlocks(dr, db, sr, sb, 2, 1)) {
        src->CpuAccess(false);
        CpuAccess(true, &dr);
        const LONG w = dr.right - dr.left, h = dr.bottom - dr.top;
        const size_t bytes = size_t((w + 1) & ~1) * 2;
        const bool down = sb == db && sr.top < dr.top;  // overlapping: rows in the safe order
        for (LONG i = 0; i < h; i++) {
            const LONG y = down ? h - 1 - i : i;
            memmove(db->Row(dr.top + y) + size_t(dr.left) * 2, sb->Row(sr.top + y) + size_t(sr.left) * 2,
                    std::min<size_t>(bytes, size_t(std::min(db->pitch - dr.left * 2, sb->pitch - sr.left * 2))));
        }
        return DD_OK;
    }
    const bool dstConv = db->fmt.fourcc, srcConv = sb->fmt.fourcc;
    // Bump maps (du/dv, not colours) only copy as they are: never through a decoding.
    if ((!dstConv && db->fmt.bump) || (!srcConv && sb->fmt.bump)) return DDERR_UNSUPPORTED;
    std::vector<RECT>& clips = g_clips;
    if (HRESULT hr = ClipsFor(this, dr, useClipper, &clips); FAILED(hr)) return hr;

    // Colour keys of a FourCC side as the decoded pixels they match (a source key is in the
    // source's format, a destination key in the destination's). On YUV sides they are
    // compared in the surface's own Y, U and V instead (MarkYuvKey): the decoded pixels'
    // alpha is then 0 where the key matched, which the blit's key tests alone. A surface that
    // is both sides with both keys has one decoding for the two: its keys are converted end
    // by end instead (as DXT keys).
    Surface* const keyDst = destKeyFromSource ? src : this;
    auto yuv = [](const Buffer* b) { return b->fmt.Yuv() || b->fmt.Planar(); };
    const bool shared = dstConv && sb == db;
    const KeyRange srcKey = SourceKeyOf(flags, fx, src), dstKey = DestKeyOf(flags, fx, keyDst);
    bool markSrc = srcKey.on && srcConv && yuv(sb), markDst = dstKey.on && dstConv && yuv(db);
    if (shared && markSrc && dstKey.on) markSrc = markDst = false;
    auto keyFor = [](const KeyRange& k, bool mark, const Format& f) {
        if (mark) return BlitKey{true, 0, 0, 0xFF000000u};  // alpha 0: matched
        if (!f.fourcc) return Masked(k, f.KeyMask());
        return Masked({k.on, FourCCKeyToArgb(f.fourCC, k.lo), FourCCKeyToArgb(f.fourCC, k.hi)}, argb.KeyMask());
    };
    BlitParams p;
    p.srcKey = keyFor(srcKey, markSrc, sb->fmt);
    p.dstKey = keyFor(dstKey, markDst, db->fmt);
    Mirroring(flags, fx, &p);
    p.spal = src->EffectivePalette();
    p.dpal = db->fmt.pal8 ? EffectivePalette() : nullptr;
    // A source without alpha is opaque (Wine's test_surface_format_conversion_alpha, RGBX
    // into DXT1-3): the pixels the blit writes get alpha 255, the others (keyed, clipped)
    // keep theirs.
    p.orMask = dstConv && !srcConv && !sb->fmt.a ? 0xFF000000u : 0;

    // Each FourCC side decoded into a scratch buffer (kept from blit to blit, g_lock held): the
    // window of whole DXT blocks, YUV pairs or 2x2 YV12 blocks the blit reads or the encoding
    // after it rewrites (a sprite from a big texture atlas decodes its own blocks). The blit's
    // rectangles move with the window.
    struct Decoded {
        Buffer t;
        ArgbWindow w{nullptr, 0};
    };
    const DWORD srcMark[2] = {srcKey.lo, srcKey.hi}, dstMark[2] = {dstKey.lo, dstKey.hi};
    auto decode = [&](Surface* s, Decoded* out, std::vector<uint8_t>& mem, const RECT& area, const DWORD* mark) -> bool {
        s->CpuAccess(false);
        const Buffer* b = s->buf;
        const int down = b->fmt.BlockCompressed() ? 4 : b->fmt.Planar() ? 2 : 1;
        const int across = b->fmt.BlockCompressed() ? 4 : 2;  // (YUV: pairs, 2x2 blocks)
        const int y0 = int(std::max<LONG>(area.top, 0)) / down * down;
        const int y1 = int(std::min<LONG>((std::min<LONG>(area.bottom, b->h) + down - 1) / down * down, b->h));
        const int x0 = int(std::max<LONG>(area.left, 0)) / across * across;
        const int x1 = int(std::min<LONG>((std::min<LONG>(area.right, b->w) + across - 1) / across * across, b->w));
        const int w = std::max(x1 - x0, 0), h = std::max(y1 - y0, 0);
        const size_t size = size_t(w) * 4 * size_t(h);
        if (mem.size() < size) {
            try {
                mem.resize(size);
            } catch (const std::bad_alloc&) {
                return false;
            }
        }
        out->t.w = w;
        out->t.h = h;
        out->t.fmt = argb;
        out->t.pitch = LONG(w) * 4;
        out->t.bits = mem.data();
        out->w = {mem.data(), LONG(w) * 4, x0, y0};
        if (w && h) {
            DecodeFourCC(b, out->w, y0, y1, x0, x1);
            if (mark) MarkYuvKey(b, out->w, mark[0], mark[1], y0, y1, x0, x1);
        }
        return true;
    };
    // (A rare large blit doesn't keep its scratch memory for the rest of the process: trimmed
    // however this returns, DrawBlit's out-of-memory exception included.)
    static std::vector<uint8_t> dstMem, srcMem;
    struct Trim {
        ~Trim() {
            for (std::vector<uint8_t>* mem : {&dstMem, &srcMem})
                if (mem->capacity() > (16u << 20)) std::vector<uint8_t>().swap(*mem);
        }
    } trim;
    Decoded dstDec, srcDec;
    Decoded* td = nullptr;
    const Decoded* ts = nullptr;
    if (dstConv) {
        // (Its own source too: the window of both.)
        RECT area = dr;
        if (shared) UnionRect(&area, &dr, &sr);
        const DWORD* mark = markDst ? dstMark : shared && markSrc ? srcMark : nullptr;
        if (!decode(this, &dstDec, dstMem, area, mark)) return DDERR_OUTOFMEMORY;
        td = &dstDec;
        if (shared) ts = td;
    }
    if (srcConv && !ts) {
        if (!decode(src, &srcDec, srcMem, sr, markSrc ? srcMark : nullptr)) return DDERR_OUTOFMEMORY;
        ts = &srcDec;
    }
    // What Direct3D drew into a side that isn't decoded is read back first.
    if (!srcConv) src->CpuAccess(false);
    if (!dstConv) CpuAccess(true, &dr);
    MarkClipsDirty(this, clips);
    RECT d = dr, s = sr;
    if (td) {
        OffsetRect(&d, -td->w.left, -td->w.top);
        for (RECT& c : clips) OffsetRect(&c, -td->w.left, -td->w.top);
    }
    if (ts) OffsetRect(&s, -ts->w.left, -ts->w.top);
    if (!clips.empty()) DrawBlit(td ? &td->t : db, d, ts ? &ts->t : sb, s, p, clips);
    if (td) {
        EncodeFourCC(td->w, db, dr);
        CpuAccess(true, &dr);  // (a texture's GPU copy made again from it)
    }
    return DD_OK;
}

}  // namespace dd2d9
