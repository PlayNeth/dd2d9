// mousemerge=on (the default): a DirectInput mouse's buffered moves merged. A mouse polled
// 1000 to 8000 times a second puts that many moves of a pixel or two in DirectInput's buffer,
// where a mouse of the time put 100 to 125; a game that reads a few each frame (its buffer
// set to 16 or 32) falls behind and stutters, or overflows and loses moves. Here each read
// empties DirectInput's buffer (made 128 entries or more) and hands the game, in order, one
// entry per axis for the moves between two button changes, holding the distance of all of
// them: nothing is lost, and the game's reads keep up. As dxwrapper's FixHighFrequencyMouse
// (but keeping the moves and the buttons in the order they came). Immediate reads
// (GetDeviceState) need nothing: DirectInput adds the moves up for them itself.
//
// dd2d9 points the game's imports of DirectInputCreateA/W/Ex at its own, and in the
// interfaces those return, CreateDevice (and IDirectInput7's CreateDeviceEx); a device made
// for the system mouse gets GetDeviceData, SetProperty, GetProperty, SetDataFormat and
// Release patched in its interfaces' tables, which other devices of the same kind share:
// they pass straight through. DirectInput 1-7 (dinput.dll); DirectInput 8 is left alone.
#define DIRECTINPUT_VERSION 0x0700
#include <dinput.h>

#include "hooks.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace dd2d9 {

namespace {

// IDirectInputDevice(2, 7)A/W: the same slots in every version.
enum { kRelease = 2, kGetProperty = 5, kSetProperty = 6, kGetDeviceData = 10, kSetDataFormat = 11, kSlots = 12 };
// IDirectInput(2, 7)A/W.
enum { kCreateDevice = 3, kCreateDeviceEx = 9 };

const DWORD kMinBuffer = 128;  // DirectInput's own buffer at least this many entries

struct PatchedTable {  // a function table patched, and its functions before
    void** table;
    void* orig[kSlots];
};

struct Mouse {
    void* ifaces[6] = {};           // the device's interfaces (the game may call through any)
    LONG ofs[3] = {-1, -1, -1};     // X, Y, Z in the game's data format (-1: none)
    DWORD asked = 0;                // the buffer size the game set
    DWORD rec = 0;                  // the size of its entries (16: DirectInput 3-7's; 20: 8's)
    std::vector<BYTE> pending;      // merged entries not yet handed to the game
    int into[3] = {-1, -1, -1};     // the pending entry each axis's next move merges into
    bool overflow = false;          // an overflow to report
    DWORD read = 0, handed = 0;     // entries read from DirectInput, and handed out (logged)
    DWORD since = 0;                // when those counts were last logged
};

CRITICAL_SECTION g_cs;
bool g_init;
std::vector<PatchedTable>* g_tables;
std::vector<Mouse*>* g_mice;

struct Lock {
    Lock() { EnterCriticalSection(&g_cs); }
    ~Lock() { LeaveCriticalSection(&g_cs); }
};

void* Original(void* self, int slot) {  // (g_cs held)
    void** table = *static_cast<void***>(self);
    for (const PatchedTable& t : *g_tables)
        if (t.table == table) return t.orig[slot];
    return nullptr;
}

Mouse* FindMouse(void* self) {  // (g_cs held)
    for (Mouse* m : *g_mice)
        for (void* i : m->ifaces)
            if (i == self) return m;
    return nullptr;
}

// Points `slots` of `table` at `hooks` (once per table: a table noted whose first slot isn't
// dd2d9's any more belonged to a dinput.dll since unloaded, and is patched anew).
void PatchTable(void** table, const int* slots, void* const* hooks, int n) {  // (g_cs held)
    for (auto t = g_tables->begin(); t != g_tables->end(); ++t)
        if (t->table == table) {
            if (table[slots[0]] == hooks[0]) return;
            g_tables->erase(t);
            break;
        }
    PatchedTable t = {table, {}};
    for (int i = 0; i < n; i++) {
        DWORD old;
        if (!VirtualProtect(&table[slots[i]], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) continue;
        t.orig[slots[i]] = table[slots[i]];
        table[slots[i]] = hooks[i];
        VirtualProtect(&table[slots[i]], sizeof(void*), old, &old);
    }
    g_tables->push_back(t);
}

// ---- the mouse device's functions

using ReleaseFn = ULONG(WINAPI*)(void*);
using PropertyFn = HRESULT(WINAPI*)(void*, REFGUID, DIPROPHEADER*);
using SetPropertyFn = HRESULT(WINAPI*)(void*, REFGUID, const DIPROPHEADER*);
using GetDataFn = HRESULT(WINAPI*)(void*, DWORD, DIDEVICEOBJECTDATA*, DWORD*, DWORD);
using FormatFn = HRESULT(WINAPI*)(void*, const DIDATAFORMAT*);

bool IsBufferSize(REFGUID g) { return reinterpret_cast<uintptr_t>(&g) == 1; }  // DIPROP_BUFFERSIZE

ULONG WINAPI HookRelease(void* self) {
    Lock l;
    const ULONG n = reinterpret_cast<ReleaseFn>(Original(self, kRelease))(self);
    if (n == 0)
        if (Mouse* m = FindMouse(self)) {
            DD2D9_LOG("mousemerge: the mouse device %p released (%lu moves and buttons read, %lu handed to the game "
                      "since the last count)", self, m->read, m->handed);
            g_mice->erase(std::remove(g_mice->begin(), g_mice->end(), m), g_mice->end());
            delete m;
        }
    return n;
}

HRESULT WINAPI HookGetProperty(void* self, REFGUID g, DIPROPHEADER* h) {
    Lock l;
    const HRESULT hr = reinterpret_cast<PropertyFn>(Original(self, kGetProperty))(self, g, h);
    Mouse* m = FindMouse(self);
    if (SUCCEEDED(hr) && m && IsBufferSize(g) && h && h->dwSize == sizeof(DIPROPDWORD) && m->asked)
        reinterpret_cast<DIPROPDWORD*>(h)->dwData = m->asked;  // the size the game set
    return hr;
}

HRESULT WINAPI HookSetProperty(void* self, REFGUID g, const DIPROPHEADER* h) {
    Lock l;
    const auto orig = reinterpret_cast<SetPropertyFn>(Original(self, kSetProperty));
    Mouse* m = FindMouse(self);
    if (!m || !IsBufferSize(g) || !h || h->dwSize != sizeof(DIPROPDWORD)) return orig(self, g, h);
    DIPROPDWORD p = *reinterpret_cast<const DIPROPDWORD*>(h);
    const DWORD asked = p.dwData;
    if (asked) p.dwData = std::max(asked, kMinBuffer);  // (0: no buffer, as the game asks)
    const HRESULT hr = orig(self, g, &p.diph);
    if (SUCCEEDED(hr)) m->asked = asked;
    return hr;
}

HRESULT WINAPI HookSetDataFormat(void* self, const DIDATAFORMAT* f) {
    Lock l;
    const HRESULT hr = reinterpret_cast<FormatFn>(Original(self, kSetDataFormat))(self, f);
    Mouse* m = FindMouse(self);
    if (FAILED(hr) || !m || !f || !f->rgodf) return hr;
    // Where the axes are in the game's format: by their GUID, else by their instance; an axis
    // of any instance takes the next one not named yet.
    std::fill(m->ofs, m->ofs + 3, -1);
    m->pending.clear();
    std::fill(m->into, m->into + 3, -1);
    for (DWORD i = 0; i < f->dwNumObjs; i++) {
        const DIOBJECTDATAFORMAT& o = f->rgodf[i];
        int axis = -1;
        if (o.pguid) {
            if (IsEqualGUID(*o.pguid, GUID_XAxis)) axis = 0;
            else if (IsEqualGUID(*o.pguid, GUID_YAxis)) axis = 1;
            else if (IsEqualGUID(*o.pguid, GUID_ZAxis)) axis = 2;
        } else if (o.dwType & DIDFT_AXIS) {
            const DWORD instance = DIDFT_GETINSTANCE(o.dwType);
            if (instance < 3) axis = int(instance);
            else if (instance == 0xFFFF)
                for (int a = 0; a < 3 && axis < 0; a++)
                    if (m->ofs[a] < 0) axis = a;
        }
        if (axis >= 0 && m->ofs[axis] < 0) m->ofs[axis] = LONG(o.dwOfs);
    }
    DD2D9_LOG("mousemerge: the mouse's format: X at %ld, Y at %ld, the wheel at %ld", m->ofs[0], m->ofs[1], m->ofs[2]);
    return hr;
}

// One entry read from DirectInput, into the pending ones.
void Merge(Mouse* m, const BYTE* rec) {
    DIDEVICEOBJECTDATA d;  // (the first 16 bytes: the same in both sizes)
    memcpy(&d, rec, 16);
    int axis = -1;
    for (int a = 0; a < 3; a++)
        if (m->ofs[a] >= 0 && d.dwOfs == DWORD(m->ofs[a])) axis = a;
    if (axis >= 0 && m->into[axis] >= 0) {  // a move since the last button change: added on
        BYTE* p = m->pending.data() + size_t(m->into[axis]) * m->rec;
        DIDEVICEOBJECTDATA e;
        memcpy(&e, p, 16);
        e.dwData = DWORD(LONG(e.dwData) + LONG(d.dwData));
        e.dwTimeStamp = d.dwTimeStamp;  // (its sequence number stays the first's: the order is kept)
        memcpy(p, &e, 16);
        return;
    }
    if (axis < 0) std::fill(m->into, m->into + 3, -1);  // a button: the moves after it come after it
    const size_t count = m->pending.size() / m->rec;
    if (m->asked && count >= m->asked) {  // the game's buffer full: as DirectInput, the new entry lost
        m->overflow = true;
        return;
    }
    m->pending.insert(m->pending.end(), rec, rec + m->rec);
    if (axis >= 0) m->into[axis] = int(count);
}

HRESULT WINAPI HookGetDeviceData(void* self, DWORD size, DIDEVICEOBJECTDATA* out, DWORD* inOut, DWORD flags) {
    Lock l;
    const auto orig = reinterpret_cast<GetDataFn>(Original(self, kGetDeviceData));
    Mouse* m = FindMouse(self);
    if (!m || !inOut || (size != 16 && size != 20) || (m->ofs[0] < 0 && m->ofs[1] < 0 && m->ofs[2] < 0))
        return orig(self, size, out, inOut, flags);
    if (m->rec != size) {  // (the entries' size changed: those held back can't be handed out)
        m->pending.clear();
        std::fill(m->into, m->into + 3, -1);
        m->rec = size;
    }
    // Everything DirectInput has, merged in.
    BYTE buf[64 * 20];
    for (;;) {
        DWORD n = sizeof(buf) / size;
        const HRESULT hr = orig(self, size, reinterpret_cast<DIDEVICEOBJECTDATA*>(buf), &n, 0);
        if (FAILED(hr)) {  // not acquired, input lost, no buffer: as DirectInput says
            m->pending.clear();
            std::fill(m->into, m->into + 3, -1);
            *inOut = 0;
            return hr;
        }
        if (hr == DI_BUFFEROVERFLOW) m->overflow = true;
        for (DWORD i = 0; i < n; i++) Merge(m, buf + size_t(i) * size);
        m->read += n;
        if (n < sizeof(buf) / size) break;
    }
    // The game's share, as DirectInput hands it: a flush (no buffer, INFINITE), a count or
    // removal without copying (no buffer), or entries copied; DIGDD_PEEK leaves them.
    const DWORD count = DWORD(m->pending.size() / size);
    const bool peek = (flags & DIGDD_PEEK) != 0;
    const DWORD n = std::min(*inOut, count);
    if (out) memcpy(out, m->pending.data(), size_t(n) * size);
    if (!peek && n) {
        m->pending.erase(m->pending.begin(), m->pending.begin() + ptrdiff_t(size_t(n) * size));
        for (int& i : m->into) i = i >= int(n) ? i - int(n) : -1;
        m->handed += n;
    }
    *inOut = n;
    const DWORD now = GetTickCount();
    if (LogLevel() >= 1 && now - m->since >= 10000 && m->read) {
        DD2D9_LOG("mousemerge: %lu moves and buttons read from DirectInput in %lu s, handed to the game as %lu",
                  m->read, (now - m->since) / 1000, m->handed);
        m->read = m->handed = 0;
        m->since = now;
    }
    if (m->overflow && !peek) {
        m->overflow = false;
        return DI_BUFFEROVERFLOW;
    }
    return DI_OK;
}

// A device made for the system mouse: its interfaces' tables patched, it noted.
void AddMouse(IUnknown* dev) {
    static const IID* const kIids[] = {&IID_IDirectInputDeviceA,  &IID_IDirectInputDeviceW,  &IID_IDirectInputDevice2A,
                                       &IID_IDirectInputDevice2W, &IID_IDirectInputDevice7A, &IID_IDirectInputDevice7W};
    static const int kPatched[] = {kRelease, kGetProperty, kSetProperty, kGetDeviceData, kSetDataFormat};
    static void* const kHooks[] = {reinterpret_cast<void*>(HookRelease), reinterpret_cast<void*>(HookGetProperty),
                                   reinterpret_cast<void*>(HookSetProperty), reinterpret_cast<void*>(HookGetDeviceData),
                                   reinterpret_cast<void*>(HookSetDataFormat)};
    Lock l;
    Mouse* m = new Mouse;
    int k = 0;
    m->ifaces[k++] = dev;
    std::vector<IUnknown*> got;
    for (const IID* iid : kIids) {
        IUnknown* i = nullptr;
        if (FAILED(dev->QueryInterface(*iid, reinterpret_cast<void**>(&i))) || !i) continue;
        got.push_back(i);
        if (std::find(m->ifaces, m->ifaces + k, i) == m->ifaces + k && k < 6) m->ifaces[k++] = i;
    }
    for (int i = 0; i < k; i++) PatchTable(*static_cast<void***>(m->ifaces[i]), kPatched, kHooks, 5);
    m->since = GetTickCount();
    g_mice->push_back(m);
    for (IUnknown* i : got) i->Release();  // (through HookRelease now: the device lives on)
    DD2D9_LOG("mousemerge: the game's DirectInput mouse %p: its buffered moves merged", static_cast<void*>(dev));
}

bool IsMouse(REFGUID g) {
    return IsEqualGUID(g, GUID_SysMouse) || IsEqualGUID(g, GUID_SysMouseEm) || IsEqualGUID(g, GUID_SysMouseEm2);
}

// ---- IDirectInput

using CreateDeviceFn = HRESULT(WINAPI*)(void*, REFGUID, IUnknown**, IUnknown*);
using CreateDeviceExFn = HRESULT(WINAPI*)(void*, REFGUID, REFIID, void**, IUnknown*);

HRESULT WINAPI HookCreateDevice(void* self, REFGUID g, IUnknown** out, IUnknown* outer) {
    CreateDeviceFn orig;
    {
        Lock l;
        orig = reinterpret_cast<CreateDeviceFn>(Original(self, kCreateDevice));
    }
    const HRESULT hr = orig(self, g, out, outer);
    if (SUCCEEDED(hr) && out && *out && IsMouse(g)) AddMouse(*out);
    return hr;
}

HRESULT WINAPI HookCreateDeviceEx(void* self, REFGUID g, REFIID riid, void** out, IUnknown* outer) {
    CreateDeviceExFn orig;
    {
        Lock l;
        orig = reinterpret_cast<CreateDeviceExFn>(Original(self, kCreateDeviceEx));
    }
    const HRESULT hr = orig(self, g, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out && IsMouse(g)) AddMouse(static_cast<IUnknown*>(*out));
    return hr;
}

void PatchDirectInput(IUnknown* di) {
    struct Kind {
        const IID* iid;
        bool seven;  // IDirectInput7: CreateDeviceEx too
    };
    static const Kind kKinds[] = {{&IID_IDirectInputA, false},  {&IID_IDirectInputW, false},
                                  {&IID_IDirectInput2A, false}, {&IID_IDirectInput2W, false},
                                  {&IID_IDirectInput7A, true},  {&IID_IDirectInput7W, true}};
    static const int kSlotsAll[] = {kCreateDevice, kCreateDeviceEx};
    static void* const kHooksAll[] = {reinterpret_cast<void*>(HookCreateDevice), reinterpret_cast<void*>(HookCreateDeviceEx)};
    Lock l;
    for (const Kind& k : kKinds) {
        IUnknown* i = nullptr;
        if (FAILED(di->QueryInterface(*k.iid, reinterpret_cast<void**>(&i))) || !i) continue;
        PatchTable(*reinterpret_cast<void***>(i), kSlotsAll, kHooksAll, k.seven ? 2 : 1);
        i->Release();
    }
}

// ---- dinput.dll's entry points

using CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, void**, IUnknown*);
using CreateExFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
CreateFn g_createA, g_createW;
CreateExFn g_createEx;

HRESULT Created(HRESULT hr, void** out) {
    if (SUCCEEDED(hr) && out && *out) PatchDirectInput(static_cast<IUnknown*>(*out));
    return hr;
}

HRESULT WINAPI HookDirectInputCreateA(HINSTANCE inst, DWORD version, void** out, IUnknown* outer) {
    DD2D9_PASS_WINDOWS(g_createA, inst, version, out, outer);
    return Created(DD2D9_NEXT(g_createA)(inst, version, out, outer), out);
}
HRESULT WINAPI HookDirectInputCreateW(HINSTANCE inst, DWORD version, void** out, IUnknown* outer) {
    DD2D9_PASS_WINDOWS(g_createW, inst, version, out, outer);
    return Created(DD2D9_NEXT(g_createW)(inst, version, out, outer), out);
}
HRESULT WINAPI HookDirectInputCreateEx(HINSTANCE inst, DWORD version, REFIID riid, void** out, IUnknown* outer) {
    DD2D9_PASS_WINDOWS(g_createEx, inst, version, riid, out, outer);
    return Created(DD2D9_NEXT(g_createEx)(inst, version, riid, out, outer), out);
}

}  // namespace

ImportHook kMouseMergeHooks[] = {
    {"dinput", "DirectInputCreateA", reinterpret_cast<void*>(HookDirectInputCreateA), reinterpret_cast<void**>(&g_createA)},
    {"dinput", "DirectInputCreateW", reinterpret_cast<void*>(HookDirectInputCreateW), reinterpret_cast<void**>(&g_createW)},
    {"dinput", "DirectInputCreateEx", reinterpret_cast<void*>(HookDirectInputCreateEx),
     reinterpret_cast<void**>(&g_createEx)},
};
const size_t kMouseMergeHookCount = sizeof(kMouseMergeHooks) / sizeof(kMouseMergeHooks[0]);

void MouseMergeInit() {
    if (g_init) return;
    InitializeCriticalSection(&g_cs);
    g_tables = new std::vector<PatchedTable>;
    g_mice = new std::vector<Mouse*>;
    g_init = true;
}

// Before dd2d9 is unloaded while the process goes on: the tables pointed back.
void MouseMergeRestore() {
    if (!g_init) return;
    Lock l;
    static const int kAll[] = {kRelease, kCreateDevice, kGetProperty, kSetProperty, kCreateDeviceEx, kGetDeviceData,
                               kSetDataFormat};
    for (const PatchedTable& t : *g_tables)
        for (int s : kAll) {
            if (!t.orig[s]) continue;
            DWORD old;
            if (!VirtualProtect(&t.table[s], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) continue;
            t.table[s] = t.orig[s];
            VirtualProtect(&t.table[s], sizeof(void*), old, &old);
        }
    g_tables->clear();
}

}  // namespace dd2d9
