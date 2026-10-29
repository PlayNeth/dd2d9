// Legacy Direct3D (3/5/6/7) objects for a wrapper loaded on top of dd2d9.
//
// D7VK implements Direct3D 3-7 itself (on D3D9), but wraps the DirectDraw it
// loads as ddraw_.dll: it creates its devices, viewports, materials and
// textures through that DirectDraw's Direct3D objects and forwards a few calls
// (Clear, Load, viewport and render-target changes) so the DirectDraw surfaces
// stay consistent. These objects provide exactly that. They don't render; the
// wrapper does. Without a wrapper (dd2d9 loaded as ddraw.dll) they are not
// offered, so games see no Direct3D and use their other renderers.
#pragma once
#include "common.h"

namespace dd2d9 {

class DDraw;
class Surface;

bool D3DProxyEnabled();
// QueryInterface fallbacks: Direct3D interfaces on the DirectDraw object, and
// textures / Direct3D 1 devices on surfaces. E_NOINTERFACE for anything else.
HRESULT D3DProxyQueryDDraw(DDraw* dd, REFIID riid, void** out);
HRESULT D3DProxyQuerySurface(Surface* s, REFIID riid, void** out);
// Called from the Surface destructor (with g_lock held).
void D3DProxySurfaceDestroyed(Surface* s);

}  // namespace dd2d9
