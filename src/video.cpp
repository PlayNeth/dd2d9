// Videos that Windows' own player (MCI: mciavi32, the "avivideo" and "digitalvideo" devices)
// draws into the game's window, in a display mode of dd2d9's own (shown scaled or in a
// window). The game places the picture with "put <alias> destination at x y w h" in its
// window's coordinates as it sees them (its mode's: GetClientRect answers so, input.cpp),
// and the player draws there in the window's real ones: the movie came out at its size in
// the corner of the screen (Street Fighter Zero: 320x240 at the top left of 1920x1080).
// dd2d9 points the game's imports of mciSendStringA/W at its own, which put the picture
// where the game's image is: the rectangle scaled and moved as the image is. A device the
// game shows in that window without a destination gets its natural size there, scaled too.
// The player's own window control (MCIWnd, msvfw32's MCIWndCreate) likewise: a child window
// of the game's, which the game moves and sizes in its mode's coordinates (MoveWindow,
// SetWindowPos) and in which it places the picture (MCIWNDM_PUT_DEST): both scaled too
// (Rockman X4's movies, at their size in the corner). Only the command strings and those
// calls are translated, and the command interface (mciSendCommand) likewise, only for the
// window dd2d9 shows the game in.
#include "hooks.h"

#include <mmsystem.h>
#include <digitalv.h>

#include <cctype>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace dd2d9 {

namespace {

using SendStringAFn = MCIERROR(WINAPI*)(LPCSTR, LPSTR, UINT, HWND);
using SendStringWFn = MCIERROR(WINAPI*)(LPCWSTR, LPWSTR, UINT, HWND);
SendStringAFn g_sendStringA;
SendStringWFn g_sendStringW;

// winmm's mciSendStringA, for dd2d9's own commands (the game may import only the W one).
SendStringAFn RealSendStringA() {
    if (g_sendStringA) return g_sendStringA;
    static const SendStringAFn f = [] {
        HMODULE m = GetModuleHandleW(L"winmm.dll");
        return m ? reinterpret_cast<SendStringAFn>(reinterpret_cast<void*>(GetProcAddress(m, "mciSendStringA"))) : nullptr;
    }();
    return f;
}

// The game's MCI devices drawing into a window: by alias (lower case), the window and
// whether the game placed the picture itself. Under g_videoLock.
struct Device {
    HWND window;
    bool placed;
};
CRITICAL_SECTION g_videoLock;
std::map<std::string, Device>* g_devices;
volatile LONG g_devicesShown;  // g_devices' size (read without the lock)

void InitOnce() {
    static const bool done = [] {
        InitializeCriticalSection(&g_videoLock);
        g_devices = new std::map<std::string, Device>;
        return true;
    }();
    (void)done;
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> t;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
        size_t j = i;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) j++;
        if (j > i) t.push_back(s.substr(i, j - i));
        i = j;
    }
    return t;
}

// A decimal number (a window handle printed with %u or %d, a coordinate).
bool Number(const std::string& s, long* v) {
    if (s.empty()) return false;
    char* end = nullptr;
    *v = long(std::strtoll(s.c_str(), &end, 10));
    return end && *end == 0;
}

// The game's rectangle (x, y, width, height in its window's coordinates as it sees them) in
// the window's real client coordinates, where the image is. False when `window` isn't the
// one the game's mode is shown scaled in.
bool ToReal(HWND window, long r[4]) {
    RECT image;
    SIZE mode;
    HWND shown;
    if (!ShownImage(&image, &mode, &shown) || shown != window || mode.cx <= 0 || mode.cy <= 0) return false;
    MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&image), 2);  // into client coordinates
    const LONG iw = image.right - image.left, ih = image.bottom - image.top;
    auto x = [&](long v) { return long(image.left + LONG(int64_t(v) * iw / mode.cx)); };
    auto y = [&](long v) { return long(image.top + LONG(int64_t(v) * ih / mode.cy)); };
    const long left = x(r[0]), top = y(r[1]), right = x(r[0] + r[2]), bottom = y(r[1] + r[3]);
    r[0] = left;
    r[1] = top;
    r[2] = right - left;
    r[3] = bottom - top;
    return true;
}

void Recount() {  // g_videoLock held: g_devicesShown updated
    InterlockedExchange(&g_devicesShown, LONG(g_devices->size()));
}

// The device's natural size placed where the image is (a device shown without a
// destination of the game's). Not under g_videoLock (MCI may call back into the window).
void PlaceNatural(const std::string& alias, HWND window) {
    const SendStringAFn send = RealSendStringA();
    if (!send) return;
    char out[128] = {};
    if (send(("where " + alias + " source").c_str(), out, sizeof(out), nullptr) != 0) return;
    const std::vector<std::string> t = Split(out);
    long r[4];
    if (t.size() != 4 || !Number(t[0], &r[0]) || !Number(t[1], &r[1]) || !Number(t[2], &r[2]) || !Number(t[3], &r[3]))
        return;
    r[0] = r[1] = 0;
    if (!ToReal(window, r)) return;
    const std::string cmd = "put " + alias + " destination at " + std::to_string(r[0]) + " " + std::to_string(r[1]) +
                            " " + std::to_string(r[2]) + " " + std::to_string(r[3]);
    const MCIERROR e = send(cmd.c_str(), nullptr, 0, nullptr);
    DD2D9_LOG("video \"%s\": shown at its natural size where the image is (%s): %lu", alias.c_str(), cmd.c_str(),
              static_cast<unsigned long>(e));
}

// The game's command, as dd2d9 sends it on (the same string unless translated), and what
// to do around it. `natural`: the alias to place at its natural size first (play).
std::string Translate(const std::string& cmd, std::string* natural, HWND* naturalWindow) {
    const std::vector<std::string> t = Split(cmd);
    if (t.size() < 2) return cmd;
    const std::string verb = Lower(t[0]), alias = Lower(t[1]);
    InitOnce();
    EnterCriticalSection(&g_videoLock);
    std::string out = cmd;
    if (verb == "window" && t.size() >= 4 && Lower(t[2]) == "handle") {
        long h;
        if (Lower(t[3]) == "default") g_devices->erase(alias);
        else if (Number(t[3], &h)) (*g_devices)[alias] = Device{reinterpret_cast<HWND>(intptr_t(h)), false};
        Recount();
    } else if (verb == "close") {
        if (alias == "all") g_devices->clear();
        else g_devices->erase(alias);
        Recount();
    } else if (verb == "put" && g_devices->count(alias)) {
        Device& d = (*g_devices)[alias];
        // put <alias> destination [at x y w h] [wait|notify...]
        for (size_t i = 2; i < t.size(); i++) {
            if (Lower(t[i]) != "destination") continue;
            d.placed = true;
            long r[4];
            if (i + 5 < t.size() && Lower(t[i + 1]) == "at" && Number(t[i + 2], &r[0]) && Number(t[i + 3], &r[1]) &&
                Number(t[i + 4], &r[2]) && Number(t[i + 5], &r[3])) {
                const long g[4] = {r[0], r[1], r[2], r[3]};
                if (ToReal(d.window, r)) {
                    std::string s = t[0] + " " + t[1];
                    for (size_t k = 2; k < t.size(); k++) {
                        s += " ";
                        if (k >= i + 2 && k <= i + 5) s += std::to_string(r[k - i - 2]);
                        else s += t[k];
                    }
                    DD2D9_LOG("video \"%s\": destination %ld,%ld %ldx%ld in the game's coordinates, %ld,%ld %ldx%ld on the screen",
                              alias.c_str(), g[0], g[1], g[2], g[3], r[0], r[1], r[2], r[3]);
                    out = s;
                }
            } else if (i + 1 >= t.size() || Lower(t[i + 1]) != "at") {
                d.placed = false;  // back to the default: the natural size, placed at the next play
            }
            break;
        }
    } else if (verb == "play" && g_devices->count(alias)) {
        const Device& d = (*g_devices)[alias];
        if (!d.placed) {  // (placed only if that window shows the game's image: PlaceNatural)
            *natural = alias;
            *naturalWindow = d.window;
        }
        // "play ... window" (not full screen) for a device already drawing in the game's
        // window: the word is left out. The old player played there all the same; Windows 11's
        // opened a player window of its own at the movie's size in the corner of the screen,
        // with its controls (Richman 4's "play vfw window from 0 notify").
        std::string s = t[0] + " " + t[1];
        bool dropped = false;
        for (size_t k = 2; k < t.size(); k++) {
            if (Lower(t[k]) == "window") {
                dropped = true;
                continue;
            }
            s += " " + t[k];
        }
        if (dropped) {
            DD2D9_LOG("video \"%s\": played in the game's window (\"window\" left out of \"%s\")", alias.c_str(), cmd.c_str());
            out = s;
        }
    }
    LeaveCriticalSection(&g_videoLock);
    return out;
}

MCIERROR WINAPI HookSendStringA(LPCSTR cmd, LPSTR ret, UINT len, HWND callback) {
    if (!cmd) return g_sendStringA(cmd, ret, len, callback);
    std::string natural;
    HWND naturalWindow = nullptr;
    const std::string sent = Translate(cmd, &natural, &naturalWindow);
    if (!natural.empty()) PlaceNatural(natural, naturalWindow);
    return g_sendStringA(sent.c_str(), ret, len, callback);
}

MCIERROR WINAPI HookSendStringW(LPCWSTR cmd, LPWSTR ret, UINT len, HWND callback) {
    if (!cmd) return g_sendStringW(cmd, ret, len, callback);
    // Commands are ASCII (aliases and file names aside, passed on as they were): translated
    // only when every character is.
    std::string a;
    for (const wchar_t* p = cmd; *p; p++) {
        if (*p > 0x7F) return g_sendStringW(cmd, ret, len, callback);
        a += char(*p);
    }
    std::string natural;
    HWND naturalWindow = nullptr;
    const std::string sent = Translate(a, &natural, &naturalWindow);
    if (!natural.empty()) PlaceNatural(natural, naturalWindow);
    if (sent == a) return g_sendStringW(cmd, ret, len, callback);
    const std::wstring w(sent.begin(), sent.end());
    return g_sendStringW(w.c_str(), ret, len, callback);
}

// ---------------------------------------------------------------- mciSendCommand
// The same through the command interface (Road Rash: its movies at their size, doubled, in
// the corner of the screen). Devices by ID. A device opened with a child window style in a
// window of the game's (MCI_DGV_OPEN_PARENT, MCI_DGV_OPEN_WS: the player's own window inside
// the game's, or inside a popup the game made over its screen, see ScaledPopup) gets that
// window where the picture goes, scaled, and the picture filling it: the rest of the window
// it is in stays the game's (a window covering it all, the picture in part of it, left the
// rest unpainted: Road Rash's menu showed through around its race intros). Unless the game
// places the player's window itself: then that is scaled, and the picture in it. A device
// drawing in the game's window itself (MCI_WINDOW with its handle) gets the picture placed as
// with "put ... destination".
using SendCommandFn = MCIERROR(WINAPI*)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
SendCommandFn g_sendCommandA, g_sendCommandW;

struct CmdDevice {
    HWND window;      // the game's window (or popup) it draws in or inside
    bool child;       // in its own child window of `window` (else in `window` itself)
    HWND parent;      // opened in a child window of this one (MCI_WINDOW's default: back to it)
    bool placed;      // the game gave a destination
    bool gameWindow;  // the game placed the child window itself (MCI_PUT with MCI_DGV_PUT_WINDOW)
};
std::map<MCIDEVICEID, CmdDevice>* g_cmdDevices;  // under g_videoLock
volatile LONG g_cmdShown;                         // its size (read without the lock)

void RecountAll() {  // g_videoLock held
    InterlockedExchange(&g_cmdShown, LONG(g_cmdDevices->size()));
}

SendCommandFn RealSendCommandA() {
    if (g_sendCommandA) return g_sendCommandA;
    static const SendCommandFn f = [] {
        HMODULE m = GetModuleHandleW(L"winmm.dll");
        return m ? reinterpret_cast<SendCommandFn>(reinterpret_cast<void*>(GetProcAddress(m, "mciSendCommandA"))) : nullptr;
    }();
    return f;
}

// A rectangle of the game's (x, y, width, height) in `window`'s real client coordinates: in a
// popup of the game's (placed and sized already), scaled; in the game's window, scaled and
// moved as the image is.
bool InWindowToReal(HWND window, long r[4]) {
    double sx, sy;
    if (!ScaledPopup(window, &sx, &sy)) return ToReal(window, r);
    const long left = long(r[0] * sx), top = long(r[1] * sy), right = long((r[0] + r[2]) * sx),
               bottom = long((r[1] + r[3]) * sy);
    r[0] = left, r[1] = top, r[2] = right - left, r[3] = bottom - top;
    return true;
}

// The real destination for the game's picture rectangle r (x, y, width, height) on device d;
// window: where to put its child window first (width 0: leave it). False: not translated.
bool PictureToReal(const CmdDevice& d, long r[4], long window[4]) {
    window[2] = 0;
    if (!d.child) return InWindowToReal(d.window, r);
    if (d.gameWindow) {
        // In the child window the game placed (scaled with it): the picture scaled, not moved.
        long s[4] = {0, 0, 1000, 1000};
        if (!InWindowToReal(d.window, s)) return false;
        const double sx = s[2] / 1000.0, sy = s[3] / 1000.0;
        const long left = long(r[0] * sx), top = long(r[1] * sy), right = long((r[0] + r[2]) * sx),
                   bottom = long((r[1] + r[3]) * sy);
        r[0] = left, r[1] = top, r[2] = right - left, r[3] = bottom - top;
        return true;
    }
    // The child window where the picture goes (as the game sees it, the window at its 0,0 the
    // size of the window it is in), the picture filling it.
    if (!InWindowToReal(d.window, r)) return false;
    for (int i = 0; i < 4; i++) window[i] = r[i];
    r[0] = r[1] = 0;
    return true;
}

void PutChildWindow(MCIDEVICEID id, const long w[4]) {  // g_videoLock not held
    const SendCommandFn send = RealSendCommandA();
    if (!send || w[2] <= 0) return;
    MCI_DGV_PUT_PARMS p = {};
    p.rc = RECT{w[0], w[1], w[2], w[3]};  // (left, top, width, height)
    const MCIERROR e = send(id, MCI_PUT, MCI_DGV_PUT_WINDOW | MCI_DGV_RECT, reinterpret_cast<DWORD_PTR>(&p));
    DD2D9_LOG("video device %u: its window where the picture goes, %ld,%ld %ldx%ld: %lu", id, w[0], w[1], w[2], w[3],
              static_cast<unsigned long>(e));
}

// The device's natural size, placed (a device played without a destination of the game's).
void PlaceNaturalCmd(MCIDEVICEID id, const CmdDevice& d) {  // g_videoLock not held
    const SendCommandFn send = RealSendCommandA();
    if (!send) return;
    MCI_DGV_RECT_PARMS where = {};
    if (send(id, MCI_WHERE, MCI_DGV_WHERE_SOURCE, reinterpret_cast<DWORD_PTR>(&where)) != 0) return;
    long r[4] = {0, 0, where.rc.right, where.rc.bottom}, w[4];  // (MCI's rectangles: width and height)
    if (!PictureToReal(d, r, w)) return;
    PutChildWindow(id, w);
    MCI_DGV_PUT_PARMS p = {};
    p.rc = RECT{r[0], r[1], r[2], r[3]};
    const MCIERROR e = send(id, MCI_PUT, MCI_DGV_PUT_DESTINATION | MCI_DGV_RECT, reinterpret_cast<DWORD_PTR>(&p));
    DD2D9_LOG("video device %u: shown at its natural size where the image is (%ld,%ld %ldx%ld): %lu", id, r[0], r[1],
              r[2], r[3], static_cast<unsigned long>(e));
}

MCIERROR SendCommand(SendCommandFn real, MCIDEVICEID id, UINT msg, DWORD_PTR flags, DWORD_PTR parms) {
    InitOnce();
    if (msg == MCI_OPEN) {
        const MCIERROR e = real(id, msg, flags, parms);
        // (The A and W structures have the same layout: the strings aren't read.)
        const auto* o = reinterpret_cast<const MCI_DGV_OPEN_PARMSA*>(parms);
        if (e == 0 && o && (flags & MCI_DGV_OPEN_PARENT) && o->hWndParent && (flags & MCI_DGV_OPEN_WS) &&
            (o->dwStyle & WS_CHILD)) {
            EnterCriticalSection(&g_videoLock);
            if (!g_cmdDevices) g_cmdDevices = new std::map<MCIDEVICEID, CmdDevice>;
            (*g_cmdDevices)[o->wDeviceID] = CmdDevice{o->hWndParent, true, o->hWndParent, false, false};
            RecountAll();
            LeaveCriticalSection(&g_videoLock);
            DD2D9_LOG("video device %u: opened in a child window of %p", o->wDeviceID, static_cast<void*>(o->hWndParent));
        }
        return e;
    }
    EnterCriticalSection(&g_videoLock);
    if (!g_cmdDevices) g_cmdDevices = new std::map<MCIDEVICEID, CmdDevice>;
    auto& devs = *g_cmdDevices;
    MCI_DGV_PUT_PARMS put = {};
    bool translated = false, natural = false;
    long window[4] = {0, 0, 0, 0};  // where to put the child window first (width 0: nowhere)
    CmdDevice dev = {};
    if (msg == MCI_CLOSE) {
        if (id == MCI_ALL_DEVICE_ID) devs.clear();
        else devs.erase(id);
        RecountAll();
    } else if (msg == MCI_WINDOW && parms && (flags & MCI_DGV_WINDOW_HWND)) {
        const HWND h = reinterpret_cast<const MCI_DGV_WINDOW_PARMSA*>(parms)->hWnd;
        auto it = devs.find(id);
        const HWND parent = it != devs.end() ? it->second.parent : nullptr;
        if (h != MCI_DGV_WINDOW_DEFAULT) devs[id] = CmdDevice{h, false, parent, false, false};
        else if (parent) devs[id] = CmdDevice{parent, true, parent, false, false};  // (its own window again)
        else if (it != devs.end()) devs.erase(it);
        RecountAll();
    } else if (msg == MCI_PUT && parms && devs.count(id)) {
        CmdDevice& d = devs[id];
        // (The rectangle is read only when the game says it gave one: MCI_DGV_RECT.)
        if (flags & MCI_DGV_RECT) put = *reinterpret_cast<const MCI_DGV_PUT_PARMS*>(parms);
        // (One call may place both the window and the picture, the same rectangle for both.)
        const RECT given = put.rc;
        if ((flags & MCI_DGV_PUT_WINDOW) && d.child && (flags & MCI_DGV_RECT)) {
            // The game placing the player's window inside its own: as an MCIWnd window is.
            long r[4] = {given.left, given.top, given.right, given.bottom};
            if (InWindowToReal(d.window, r)) {
                put.rc = RECT{r[0], r[1], r[2], r[3]};
                d.gameWindow = translated = true;
            }
        }
        if ((flags & MCI_DGV_PUT_DESTINATION) && !((flags & MCI_DGV_PUT_WINDOW) && translated)) {
            d.placed = (flags & MCI_DGV_RECT) != 0;
            if (d.placed) {
                const long g[4] = {given.left, given.top, given.right, given.bottom};
                long r[4] = {g[0], g[1], g[2], g[3]};
                if (PictureToReal(d, r, window)) {
                    put.rc = RECT{r[0], r[1], r[2], r[3]};
                    translated = true;
                    DD2D9_LOG("video device %u: destination %ld,%ld %ldx%ld in the game's coordinates, %ld,%ld %ldx%ld %s", id,
                              g[0], g[1], g[2], g[3], r[0], r[1], r[2], r[3],
                              d.child ? "in its window" : "on the screen");
                }
            }
        }
    } else if (msg == MCI_PLAY && devs.count(id)) {
        dev = devs[id];
        natural = !dev.placed;
    }
    LeaveCriticalSection(&g_videoLock);
    PutChildWindow(id, window);
    if (natural) PlaceNaturalCmd(id, dev);
    if (translated) return real(id, msg, flags, reinterpret_cast<DWORD_PTR>(&put));
    return real(id, msg, flags, parms);
}

MCIERROR WINAPI HookSendCommandA(MCIDEVICEID id, UINT msg, DWORD_PTR flags, DWORD_PTR parms) {
    return SendCommand(g_sendCommandA, id, msg, flags, parms);
}

MCIERROR WINAPI HookSendCommandW(MCIDEVICEID id, UINT msg, DWORD_PTR flags, DWORD_PTR parms) {
    return SendCommand(g_sendCommandW, id, msg, flags, parms);
}

// ---------------------------------------------------------------- MCIWnd
using MoveWindowFn = BOOL(WINAPI*)(HWND, int, int, int, int, BOOL);
using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
using SendMessageFn = LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM);
MoveWindowFn g_moveWindow;
SetWindowPosFn g_setWindowPos;
SendMessageFn g_sendMessageA, g_sendMessageW;
constexpr UINT kPutDest = WM_USER + 143;  // MCIWNDM_PUT_DEST

// An MCIWnd window inside the window the game's mode is shown in (its parent): that window.
HWND MciWndParent(HWND h) {
    if (!h || !(GetWindowLongW(h, GWL_STYLE) & WS_CHILD)) return nullptr;
    char cls[16] = {};
    if (!GetClassNameA(h, cls, sizeof(cls)) || lstrcmpiA(cls, "MCIWndClass") != 0) return nullptr;
    return GetParent(h);
}

// Where the image is in `parent`'s client area, and the scale from the game's coordinates.
bool Scale(HWND parent, double* sx, double* sy) {
    RECT image;
    SIZE mode;
    HWND shown;
    if (!ShownImage(&image, &mode, &shown) || shown != parent || mode.cx <= 0 || mode.cy <= 0) return false;
    *sx = double(image.right - image.left) / mode.cx;
    *sy = double(image.bottom - image.top) / mode.cy;
    return true;
}

BOOL WINAPI HookMoveWindow(HWND h, int x, int y, int w, int ht, BOOL repaint) {
    if (const HWND parent = MciWndParent(h)) {
        long r[4] = {x, y, w, ht};
        if (ToReal(parent, r)) {
            DD2D9_LOG("video window %p: %d,%d %dx%d in the game's coordinates, %ld,%ld %ldx%ld on the screen",
                      static_cast<void*>(h), x, y, w, ht, r[0], r[1], r[2], r[3]);
            return g_moveWindow(h, int(r[0]), int(r[1]), int(r[2]), int(r[3]), repaint);
        }
    }
    return g_moveWindow(h, x, y, w, ht, repaint);
}

BOOL WINAPI HookSetWindowPos(HWND h, HWND after, int x, int y, int w, int ht, UINT flags) {
    if ((flags & (SWP_NOMOVE | SWP_NOSIZE)) != (SWP_NOMOVE | SWP_NOSIZE)) {
        if (const HWND parent = MciWndParent(h)) {
            long r[4] = {x, y, w, ht};
            if (ToReal(parent, r)) {
                // A size alone scales; a position alone moves with the image (its own size kept).
                if (!(flags & SWP_NOMOVE)) x = int(r[0]), y = int(r[1]);
                if (!(flags & SWP_NOSIZE)) w = int(r[2]), ht = int(r[3]);
            }
        }
    }
    return g_setWindowPos(h, after, x, y, w, ht, flags);
}

// MCIWNDM_PUT_DEST: the picture's rectangle in the MCIWnd window's client area, scaled as the
// window was (no offset: the window itself moved with the image).
LRESULT SendPutDest(SendMessageFn send, HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kPutDest && lp) {
        double sx, sy;
        if (const HWND parent = MciWndParent(h); parent && Scale(parent, &sx, &sy)) {
            const RECT& g = *reinterpret_cast<const RECT*>(lp);
            RECT r = {LONG(g.left * sx), LONG(g.top * sy), LONG(g.right * sx), LONG(g.bottom * sy)};
            DD2D9_LOG("video window %p: picture %ld,%ld-%ld,%ld, %ld,%ld-%ld,%ld on the screen", static_cast<void*>(h), g.left,
                      g.top, g.right, g.bottom, r.left, r.top, r.right, r.bottom);
            return send(h, msg, wp, reinterpret_cast<LPARAM>(&r));
        }
    }
    return send(h, msg, wp, lp);
}

LRESULT WINAPI HookSendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    return msg == kPutDest ? SendPutDest(g_sendMessageA, h, msg, wp, lp) : g_sendMessageA(h, msg, wp, lp);
}

LRESULT WINAPI HookSendMessageW(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    return msg == kPutDest ? SendPutDest(g_sendMessageW, h, msg, wp, lp) : g_sendMessageW(h, msg, wp, lp);
}

ImportHook kHooks[] = {
    {"user32", "MoveWindow", reinterpret_cast<void*>(HookMoveWindow), reinterpret_cast<void**>(&g_moveWindow)},
    {"user32", "SetWindowPos", reinterpret_cast<void*>(HookSetWindowPos), reinterpret_cast<void**>(&g_setWindowPos)},
    {"user32", "SendMessageA", reinterpret_cast<void*>(HookSendMessageA), reinterpret_cast<void**>(&g_sendMessageA)},
    {"user32", "SendMessageW", reinterpret_cast<void*>(HookSendMessageW), reinterpret_cast<void**>(&g_sendMessageW)},
    {"winmm", "mciSendStringA", reinterpret_cast<void*>(HookSendStringA), reinterpret_cast<void**>(&g_sendStringA)},
    {"winmm", "mciSendStringW", reinterpret_cast<void*>(HookSendStringW), reinterpret_cast<void**>(&g_sendStringW)},
    {"winmm", "mciSendCommandA", reinterpret_cast<void*>(HookSendCommandA), reinterpret_cast<void**>(&g_sendCommandA)},
    {"winmm", "mciSendCommandW", reinterpret_cast<void*>(HookSendCommandW), reinterpret_cast<void**>(&g_sendCommandW)},
};

}  // namespace

bool VideoOnWindow(HWND hwnd) {
    if (!hwnd) return false;
    // An MCIWnd window showing in it.
    for (HWND c = FindWindowExA(hwnd, nullptr, "MCIWndClass", nullptr); c; c = FindWindowExA(hwnd, c, "MCIWndClass", nullptr))
        if (IsWindowVisible(c)) return true;
    if (!g_devicesShown && !g_cmdShown) return false;
    InitOnce();
    EnterCriticalSection(&g_videoLock);
    bool on = false;
    for (const auto& d : *g_devices)
        if (d.second.window == hwnd) on = true;
    if (g_cmdDevices)
        for (const auto& d : *g_cmdDevices)
            if (d.second.window == hwnd) on = true;
    LeaveCriticalSection(&g_videoLock);
    return on;
}

void InstallVideoHooks() {
    InitOnce();
    if (const unsigned n = PatchGameModules(kHooks, sizeof(kHooks) / sizeof(kHooks[0])))
        DD2D9_LOG("video: %u MCI imports translated for the game's display mode", n);
}

}  // namespace dd2d9
