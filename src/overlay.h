// Overlay surfaces (DDSCAPS_OVERLAY): what DirectDraw and the presenter call (overlay.cpp).
#pragma once
#include "common.h"

#include <vector>

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace dd2d9 {

class DDraw;
class Surface;
struct Buffer;
class Gpu;
class OverlayGpu;

// Overlays (overlay.cpp). Offered (DDCAPS_OVERLAY reported, overlay surfaces made) with
// videooverlay=on.
bool OverlaysOffered(const DDraw* dd);
// Every overlay surface of the process, front to back, as ddraw.dll keeps the device's
// z-order list (g_lock). A new overlay goes in front.
std::vector<Surface*>& OverlayZOrder();
// What the presenter lays over the primary: the overlays shown on it, back to front,
// taken under g_lock (buffers referenced), drawn without it into the frame converted from
// the primary (`frame`: X8R8G8B8, the primary's area `area`, `pitch` pixels a row);
// `primaryBits` is the primary's buffer, whose raw pixels destination keys compare.
// gdi: a windowed program's window as GDI drew it (its key colour, painted there, is what a
// destination key compares too; a key shown nowhere under an overlay doesn't hide it).
struct OverlayLayer;
struct OverlayGdi {
    const uint32_t* px;  // X8R8G8B8, w a row
    LONG w, h;
    LONG left, top;      // where it lies in the primary's coordinates
};
void TakeOverlays(const Surface* primary, std::vector<OverlayLayer*>* out);
bool OverlaysKeyedOnDest(const std::vector<OverlayLayer*>& layers);
void DrawOverlays(const std::vector<OverlayLayer*>& layers, const Buffer* primaryBits, const RECT& area,
                  uint32_t* frame, size_t pitch, const OverlayGdi* gdi);
void DropOverlays(std::vector<OverlayLayer*>* layers);
// The GPU path (the presenter's): the overlays drawn into the back buffer over the frame,
// each uploaded when its pixels change. PrepareOverlays (no lock) decodes what changed and,
// with `gdi`, decides windowed keys shown nowhere; DrawOverlaysGpu (Gpu lock) draws them over
// `dst` of `bb`, where the primary's `src` was drawn; `keySrc`'s `keyRect` is that frame as
// converted (for destination keys; `keyGamma`: the gamma ramp it was converted with, or
// null). false: no pixel shaders, the CPU path from now on.
OverlayGpu* NewOverlayGpu();
void DeleteOverlayGpu(OverlayGpu* g);
void ReleaseOverlayGpu(OverlayGpu* g);  // its D3D9 resources (Gpu lock)
bool OverlayGpuUsable(const OverlayGpu* g);
void PrepareOverlays(OverlayGpu* g, const std::vector<OverlayLayer*>& layers, const Buffer* keyPrimary,
                     const RECT& area, const OverlayGdi* gdi);
bool DrawOverlaysGpu(OverlayGpu* g, Gpu& gpu, IDirect3DDevice9* d, const std::vector<OverlayLayer*>& layers,
                     IDirect3DSurface9* bb, const RECT& src, const RECT& dst, IDirect3DSurface9* keySrc,
                     const RECT& keyRect, const OverlayGdi* gdi, unsigned gdiSeq, const uint8_t (*keyGamma)[256]);

}  // namespace dd2d9
