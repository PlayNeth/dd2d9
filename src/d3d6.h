// dd2d9's own Direct3D 3-6 (IDirect3D/2/3 and their devices, viewports, materials, lights,
// textures, execute and vertex buffers), drawing through the shared Direct3D 9 device
// (gpures.h). Matched to XP SP3's d3dim.dll: re/spec/d3d6-direct3d.md, d3d6-device.md,
// d3d6-objects.md. Internals: d3d6int.h.
#pragma once
#include "common.h"

namespace dd2d9 {

class DDraw;
class Surface;

// DirectDraw's QueryInterface for an IID it doesn't know, on an object not made by
// DirectDrawCreateEx (`ddVersion`: the interface called): IDirect3D, IDirect3D2,
// IDirect3D3. E_NOINTERFACE for anything else. g_lock not held.
HRESULT D3DLegacyQuery(DDraw* dd, REFIID riid, void** out, int ddVersion);
// A surface's QueryInterface for an IID it doesn't know (`version`: the surface interface
// called): textures and Direct3D 1 devices; E_INVALIDARG for other IIDs, as d3dim.dll.
// E_NOINTERFACE on DirectDraw 7 objects or with Direct3D off.
HRESULT D3DLegacyQuerySurface(Surface* s, REFIID riid, void** out, int version);
// The surface is being destroyed (g_lock held): its textures and Direct3D 1 device go.
void D3DLegacySurfaceGone(Surface* s);
// The DirectDraw object is torn down: its Direct3D object goes first (g_lock held).
void D3DLegacyDestroy(DDraw* dd);
// Direct3D 3-6 is available for this DirectDraw object (for its caps).
bool D3DLegacyEnabled();

}  // namespace dd2d9
