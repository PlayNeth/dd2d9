// Videos that Windows' own player (MCI: mciavi32, the "avivideo" and "digitalvideo" devices)
// draws into the game's window, in a display mode of dd2d9's own (shown scaled or in a
// window). The game places the picture with "put <alias> destination at x y w h" in its
// window's coordinates as it sees them (its mode's: GetClientRect answers so, input.cpp),
// and the player draws there in the window's real ones: the movie came out at its size in
// the corner of the screen (Street Fighter Zero: 320x240 at the top left of 1920x1080).
// dd2d9 points the game's imports of mciSendStringA/W at its own, which put the picture
// where the game's image is: the rectangle scaled and moved as the image is. A device the
// game shows in that window without a destination gets its natural size there, scaled too.
// Only the command strings are translated (mciSendCommand's MCI_PUT isn't), only for the
// window dd2d9 shows the game in, and only with mouse=scale (with mouse=raw the game sees its
// window's real size, and places the picture in real coordinates itself).
#include "hooks.h"

#include <mmsystem.h>

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

ImportHook kHooks[] = {
    {"winmm", "mciSendStringA", reinterpret_cast<void*>(HookSendStringA), reinterpret_cast<void**>(&g_sendStringA)},
    {"winmm", "mciSendStringW", reinterpret_cast<void*>(HookSendStringW), reinterpret_cast<void**>(&g_sendStringW)},
};

}  // namespace

bool VideoOnWindow(HWND hwnd) {
    if (!g_devicesShown || !hwnd) return false;
    InitOnce();
    EnterCriticalSection(&g_videoLock);
    bool on = false;
    for (const auto& d : *g_devices)
        if (d.second.window == hwnd) on = true;
    LeaveCriticalSection(&g_videoLock);
    return on;
}

void InstallVideoHooks() {
    // With mouse=raw the game's window coordinates are the real ones (GetClientRect isn't
    // translated): its destinations are already there.
    if (!g_config.mouseScale) return;
    InitOnce();
    if (const unsigned n = PatchGameModules(kHooks, sizeof(kHooks) / sizeof(kHooks[0])))
        DD2D9_LOG("video: %u MCI imports translated for the game's display mode", n);
}

}  // namespace dd2d9
