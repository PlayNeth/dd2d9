// Internals the DirectDraw object's files share: ddraw.cpp (creation, references), ddmode.cpp
// (cooperative level, display mode), ddwindow.cpp (the exclusive window), ddhook.cpp (its
// window procedure), ddscreen.cpp (the image on the real screen), ddtiming.cpp (vblank, pacing),
// ddcaps.cpp (queries, enumeration), ddcreate.cpp (CreateSurface), ddgdi.cpp (GDI).
#pragma once
#include "wrappers.h"
#include "gpu.h"

namespace dd2d9 {

// Caps bits ddraw.dll checks by value: bits 24-25 have no DDSCAPS_ flag (reserved), and bit
// 23 is Direct3D's execute buffers (DDSCAPS_EXECUTEBUFFER, not in ddraw.h).
constexpr DWORD kCapsReserved = 0x03000000;
constexpr DWORD kCapsExecuteBuffer = DDSCAPS_RESERVED2;
// The overlay stretching and mirroring dd2d9's overlays do (GetCaps).
constexpr DWORD kOverlayFxCaps = DDFXCAPS_OVERLAYSTRETCHX | DDFXCAPS_OVERLAYSTRETCHY | DDFXCAPS_OVERLAYSHRINKX |
                                 DDFXCAPS_OVERLAYSHRINKY | DDFXCAPS_OVERLAYSTRETCHXN | DDFXCAPS_OVERLAYSTRETCHYN |
                                 DDFXCAPS_OVERLAYSHRINKXN | DDFXCAPS_OVERLAYSHRINKYN | DDFXCAPS_OVERLAYMIRRORLEFTRIGHT |
                                 DDFXCAPS_OVERLAYMIRRORUPDOWN;

extern std::vector<DDraw*> g_ddraws;  // every DirectDraw object (g_lock), for the GetSurfaceFromDC export
// The DirectDraw object in exclusive mode, if any: the display mode is its to change and
// restore, as in ddraw.dll. Written with both g_displayLock and g_lock held, so either
// lock is enough to read it.
extern DDraw* g_exclusiveDD;

ULONG ModeHash();            // the current display mode as one number (ddmode.cpp)
ULONG DisplayUniqueness();   // Windows' display settings uniqueness (ddmode.cpp)
extern volatile LONG g_toldSize;  // what the game's window was last told its size is (ddhook.cpp)
HWND CreateDeviceWindow(HWND focus);  // DDSCL_CREATEDEVICEWINDOW's window (ddwindow.cpp)

// GameImageChanged at the end of a scope that moves or resizes the game's window.
struct ImageChangedAtExit {
    ImageChangedAtExit() = default;
    ~ImageChangedAtExit() { GameImageChanged(); }
};

}  // namespace dd2d9
