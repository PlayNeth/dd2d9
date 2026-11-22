// The video port container (IDDVideoPortContainer, from dvp.h, which the headers used here
// lack): ddraw.dll's answers with the Voodoo3's driver, as measured on XP (fmtprobe section
// 28), in its order of checks (the container's methods, FUN_73767955 and the three after it).
// One port where overlays are offered, with the caps, the four connections and the status the
// driver reported, none in use. A port can't be made: there's nothing to capture from, and
// the Voodoo3's driver refused one too (E_FAIL). One container per DirectDraw object, handed
// out from any of its interfaces and counted apart from them (ddraw.dll's interface object for
// it holds a reference of its own).
#include "ddint.h"

#include <algorithm>
#include <iterator>

namespace dd2d9 {
namespace {

const GUID kIidContainer = {0x6C142760, 0xA733, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};

struct VpConnect {  // DDVIDEOPORTCONNECT
    DWORD dwSize, dwPortWidth;
    GUID guidTypeID;
    DWORD dwFlags;
    ULONG_PTR dwReserved1;
};
struct VpCaps {  // DDVIDEOPORTCAPS
    DWORD dwSize, dwFlags, dwMaxWidth, dwMaxVBIWidth, dwMaxHeight, dwVideoPortID, dwCaps, dwFX, dwNumAutoFlipSurfaces,
        dwAlignVideoPortBoundary, dwAlignVideoPortPrescaleWidth, dwAlignVideoPortCropBoundary, dwAlignVideoPortCropWidth,
        dwPreshrinkXStep, dwPreshrinkYStep, dwNumVBIAutoFlipSurfaces, dwNumPreferredAutoflip;
    WORD wNumFilterTapsX, wNumFilterTapsY;
};
struct VpDesc {  // DDVIDEOPORTDESC
    DWORD dwSize, dwFieldWidth, dwVBIWidth, dwFieldHeight, dwMicrosecondsPerField, dwMaxPixelsPerSecond, dwVideoPortID,
        dwReserved1;
    VpConnect VideoPortType;
    ULONG_PTR dwReserved2, dwReserved3;
};
struct VpStatus {  // DDVIDEOPORTSTATUS
    DWORD dwSize;
    BOOL bInUse;
    DWORD dwFlags, dwReserved1;
    VpConnect VideoPortType;
    ULONG_PTR dwReserved2, dwReserved3;
};
static_assert(sizeof(void*) != 4 || (sizeof(VpConnect) == 0x20 && sizeof(VpCaps) == 0x48 && sizeof(VpDesc) == 0x48 &&
                                     sizeof(VpStatus) == 0x38),
              "dvp.h's sizes, which ddraw.dll checks");
using VpEnumCb = HRESULT(WINAPI*)(VpCaps*, void*);

struct IVideoPortContainer : IUnknown {
    virtual HRESULT WINAPI CreateVideoPort(DWORD, VpDesc*, IUnknown**, IUnknown*) = 0;
    virtual HRESULT WINAPI EnumVideoPorts(DWORD, VpCaps*, void*, VpEnumCb) = 0;
    virtual HRESULT WINAPI GetVideoPortConnectInfo(DWORD, DWORD*, VpConnect*) = 0;
    virtual HRESULT WINAPI QueryVideoPortStatus(DWORD, VpStatus*) = 0;
};

// The port: the Voodoo3's caps as ddraw.dll hands them to EnumVideoPorts' callback (it adds
// DDVPD_PREFERREDAUTOFLIP with 3 surfaces, and DDVPCAPS_VBIANDVIDEOINDEPENDENT), and its four
// connections (8 bits wide, any of the external sync polarities; DDVPCONNECT_VACT,
// INVERTPOLARITY, DISCARDSVREFDATA and HALFLINE).
constexpr VpCaps kPortCaps = {0x48, 0xBF, 0x1000, 0x1000, 0x800, 0, 0x2A0F, 0xF00C9, 10, 0, 0, 0, 0, 0, 0, 10, 3, 0, 0};
constexpr DWORD kConnectFlags = 0x1E;
const VpConnect kConnections[] = {  // DDVPTYPE_E_HREFH_VREFH, E_HREFH_VREFL, E_HREFL_VREFH, E_HREFL_VREFL
    {0x20, 8, {0x54F39980, 0xDA60, 0x11CF, {0x9B, 0x06, 0x00, 0xA0, 0xC9, 0x03, 0xA3, 0xB8}}, kConnectFlags, 0},
    {0x20, 8, {0x92783220, 0xDA60, 0x11CF, {0x9B, 0x06, 0x00, 0xA0, 0xC9, 0x03, 0xA3, 0xB8}}, kConnectFlags, 0},
    {0x20, 8, {0xA07A02E0, 0xDA60, 0x11CF, {0x9B, 0x06, 0x00, 0xA0, 0xC9, 0x03, 0xA3, 0xB8}}, kConnectFlags, 0},
    {0x20, 8, {0xE09C77E0, 0xDA60, 0x11CF, {0x9B, 0x06, 0x00, 0xA0, 0xC9, 0x03, 0xA3, 0xB8}}, kConnectFlags, 0},
};
constexpr DWORD kConnectionCount = DWORD(std::size(kConnections));

// The DDVIDEOPORTCAPS flags and caps EnumVideoPorts' filter and CreateVideoPort look at.
constexpr DWORD kFilterWidth = 1, kFilterHeight = 2, kFilterId = 4, kFilterCaps = 8, kFilterFx = 0x10;
constexpr DWORD kPortInterlaced = 2, kPortNonInterlaced = 4, kPortShareable = 0x20;
constexpr DWORD kConnectInterlaced = 0x20, kConnectShareEven = 0x40, kConnectShareOdd = 0x80;

// In EnumVideoPorts' callback, the container's methods answer DDERR_CURRENTLYNOTAVAIL, as
// ddraw.dll's (a flag of its, under its lock: only its own thread can get there meanwhile).
thread_local int t_enumerating;

class Container final : public IVideoPortContainer {
public:
    uint32_t magic = kMagic;
    DDraw* d;
    explicit Container(DDraw* impl) : d(impl) { WritableVtbl(this); }

    // The ports: one where the driver side offers overlays.
    DWORD Ports() const { return OverlaysOffered(d) && d->capsSide != DDraw::kEmulationOnly ? 1 : 0; }

    STDMETHOD(QueryInterface)(REFIID r, void** p) override {
        if (!IsBadReadPtr(&r, sizeof(IID)) && p && IsEqualGUID(r, kIidContainer)) {
            AddRef();
            *p = this;
            return S_OK;
        }
        return d->QueryInterface(r, p, 0);
    }
    STDMETHOD_(ULONG, AddRef)() override { return d->AddRef(DDraw::kVideoPortContainer); }
    STDMETHOD_(ULONG, Release)() override { return d->Release(DDraw::kVideoPortContainer); }

    STDMETHOD(CreateVideoPort)(DWORD flags, VpDesc* desc, IUnknown** out, IUnknown* outer) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        if (!d->initialized) return DDERR_NOTINITIALIZED;
        if (t_enumerating) return DDERR_CURRENTLYNOTAVAIL;
        // DDVPCREATE_VBIONLY and DDVPCREATE_VIDEOONLY (both: neither), a description of
        // dvp.h's size for a port there with its reserved fields zero, somewhere to put it.
        if ((flags & ~DWORD(3)) || !desc || IsBadReadPtr(desc, sizeof(VpDesc)) || desc->dwSize != sizeof(VpDesc) || !out ||
            desc->dwVideoPortID >= Ports() || desc->VideoPortType.dwReserved1 || desc->dwReserved1 || desc->dwReserved2 ||
            desc->dwReserved3)
            return DDERR_INVALIDPARAMS;
        // What the port can take (no port in use): interlaced or not as it can, shared fields
        // only from an interlaced one that can share them, one of them, the field no bigger
        // than its largest, a field rate and a pixel rate; and one of its connections (type
        // and width).
        const DWORD c = desc->VideoPortType.dwFlags, can = kPortCaps.dwCaps, share = kConnectShareEven | kConnectShareOdd;
        const bool interlaced = (c & kConnectInterlaced) != 0;
        const bool fits = (can & (interlaced ? kPortInterlaced : kPortNonInterlaced)) != 0 &&
                          (!(c & share) || (interlaced && (can & kPortShareable))) && (c & share) != share &&
                          desc->dwFieldWidth <= kPortCaps.dwMaxWidth && desc->dwFieldHeight <= kPortCaps.dwMaxHeight &&
                          desc->dwMicrosecondsPerField && desc->dwMaxPixelsPerSecond;
        const VpConnect& asked = desc->VideoPortType;
        bool connected = false;
        for (const VpConnect& k : kConnections)
            connected |= IsEqualGUID(k.guidTypeID, asked.guidTypeID) && k.dwPortWidth == asked.dwPortWidth;
        if (!fits || !connected) return DDERR_INVALIDPARAMS;
        *out = nullptr;
        DD2D9_LOG("CreateVideoPort: no video port to make (E_FAIL, as the driver that had one answered)");
        return E_FAIL;
    }

    STDMETHOD(EnumVideoPorts)(DWORD, VpCaps* filter, void* ctx, VpEnumCb cb) override {
        if (!d->initialized) return DDERR_NOTINITIALIZED;
        if (t_enumerating) return DDERR_CURRENTLYNOTAVAIL;
        if (IsBadCodePtr(reinterpret_cast<FARPROC>(reinterpret_cast<void*>(cb)))) return DDERR_INVALIDPARAMS;
        // A filter: dvp.h's size, a width and a height together or neither, a port there
        // (looked at whether DDVPD_ID is set or not).
        DWORD f = 0;
        if (filter) {
            if (IsBadReadPtr(filter, sizeof(VpCaps)) || filter->dwSize != sizeof(VpCaps)) return DDERR_INVALIDPARAMS;
            f = filter->dwFlags;
            if (!(f & kFilterWidth) != !(f & kFilterHeight) || filter->dwVideoPortID >= Ports()) return DDERR_INVALIDPARAMS;
        }
        if (!Ports()) return DDERR_UNSUPPORTED;  // (the driver's table of ports: none)
        for (DWORD i = 0; i < Ports(); i++) {
            VpCaps caps = kPortCaps;  // (a copy for each callback: ddraw.dll hands over its table's)
            caps.dwVideoPortID = i;
            if (((f & kFilterWidth) && caps.dwMaxWidth < filter->dwMaxWidth) ||
                ((f & kFilterHeight) && caps.dwMaxHeight < filter->dwMaxHeight) ||
                ((f & kFilterId) && filter->dwVideoPortID != caps.dwVideoPortID) ||
                ((f & kFilterCaps) && (caps.dwCaps & filter->dwCaps) != filter->dwCaps) ||
                ((f & kFilterFx) && (caps.dwFX & filter->dwFX) != filter->dwFX))
                continue;
            t_enumerating++;
            const HRESULT go = cb(&caps, ctx);
            t_enumerating--;
            if (go == DDENUMRET_CANCEL) break;
        }
        return DD_OK;
    }

    STDMETHOD(GetVideoPortConnectInfo)(DWORD port, DWORD* count, VpConnect* info) override {
        if (!d->initialized) return DDERR_NOTINITIALIZED;
        if (t_enumerating) return DDERR_CURRENTLYNOTAVAIL;
        if (port >= Ports() || !count || IsBadWritePtr(count, sizeof(DWORD)) || (info && !*count)) return DDERR_INVALIDPARAMS;
        if (!info) {
            *count = kConnectionCount;
            return DD_OK;
        }
        // Room for fewer than there are: as many as there's room for, and DDERR_MOREDATA (the
        // count left as it was). Otherwise all of them, and the count.
        const DWORD n = std::min(*count, kConnectionCount);
        if (IsBadWritePtr(info, sizeof(VpConnect) * n)) return DDERR_INVALIDPARAMS;
        std::copy(kConnections, kConnections + n, info);
        if (n < kConnectionCount) return DDERR_MOREDATA;
        *count = kConnectionCount;
        return DD_OK;
    }

    STDMETHOD(QueryVideoPortStatus)(DWORD port, VpStatus* status) override {
        if (!d->initialized) return DDERR_NOTINITIALIZED;
        if (t_enumerating) return DDERR_CURRENTLYNOTAVAIL;
        if (!status || IsBadWritePtr(status, sizeof(VpStatus)) || status->dwSize != sizeof(VpStatus)) return DDERR_INVALIDOBJECT;
        *status = {};  // (cleared before the port is checked, as ddraw.dll's)
        status->dwSize = sizeof(VpStatus);
        if (port >= Ports()) return DDERR_INVALIDPARAMS;
        return DD_OK;  // none in use
    }
};

}  // namespace

void* NewVideoPortContainer(DDraw* d) { return new Container(d); }
void DeleteVideoPortContainer(void* c) { delete static_cast<Container*>(c); }
bool IsVideoPortContainerIid(REFIID r) { return IsEqualGUID(r, kIidContainer) != FALSE; }

}  // namespace dd2d9
