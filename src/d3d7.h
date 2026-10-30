// dd2d9's own Direct3D 7 (IDirect3D7, IDirect3DDevice7, IDirect3DVertexBuffer7), drawing
// through the shared Direct3D 9 device (gpures.h). Matched to XP SP3's d3dim700.dll:
// re/spec/d3d7-direct3d.md, d3d7-device-state.md, d3d7-draw-vb.md.
#pragma once
#include "common.h"

namespace dd2d9 {

class DDraw;

// DirectDraw's QueryInterface for an IID it doesn't know, from interface version
// `ddVersion` of the object: IDirect3D7 on objects made by DirectDrawCreateEx.
// E_NOINTERFACE for anything else. g_lock not held.
HRESULT D3D7Query(DDraw* dd, REFIID riid, void** out, int ddVersion);
// The DirectDraw object is torn down: its Direct3D object goes first (g_lock held).
void D3D7Destroy(DDraw* dd);

}  // namespace dd2d9
