// Pointing a game's imports at dd2d9's functions (import address table patching), shared by
// the mouse translation (input.cpp) and the compatibility options (compat.cpp). Only the
// game's own modules are patched: its executable and the DLLs it brought, not Windows'
// modules, not other graphics, sound or input wrappers, and not dd2d9 itself.
#pragma once
#include "common.h"

namespace dd2d9 {

struct ImportHook {
    const char* dll;    // without ".dll", matched case-insensitively ("user32")
    const char* name;   // the function
    void* replacement;  // dd2d9's
    void** original;    // filled by PatchGameModules: the real function (null: not loaded)
};

// Patches every game module's imports of the hooked functions; returns how many imports
// were redirected. Modules loaded later are patched as they load, with every `hooks` array
// given so far (the loader's notifications, asked for in DllMain; without them, at the next
// call). inLoaderLock: called from DllMain, where the module list is read from the loader's
// own. Hooks whose DLL isn't loaded are skipped. A module is patched for a given `hooks` array
// once (later calls skip it): `hooks` must stay where it is.
unsigned PatchGameModules(const ImportHook* hooks, size_t n, bool inLoaderLock = false);
// Restores every patched import (before dd2d9 is unloaded while the process goes on).
void UnpatchAll();
// dd2d9's replacement for `real`, if a hook table given to PatchGameModules so far hooks that
// function (nullptr: none): what a game that looks the function up at run time is handed.
void* InstalledHookFor(const void* real);

// The function a hook passes its call on to: the one it replaced (`*original`), unless the
// hook was called from inside the module that function is in, or from dd2d9 itself (where
// that function passed the call on with a jump, not a call). That is another hooker's
// patch which, after dd2d9 put its hook over it, took dd2d9's hook as the function *it*
// passes calls on to: Windows' compatibility shims do so when they patch imports again as a
// DLL loads (N.I.C.E. 2's bn_setup.exe on Windows 11). Passing the call back would go round
// the two until the stack ran out; the real function, read from its DLL's export table, is
// called instead. `caller`: the hook's return address.
void* NextFunction(void* const* original, const void* caller);
template <class F>
inline F NextOf(void* const* original, const void* caller) {
    return reinterpret_cast<F>(NextFunction(original, caller));
}
// In a hook: DD2D9_NEXT(g_original)(arguments).
#define DD2D9_NEXT(g) \
    ::dd2d9::NextOf<decltype(g)>(reinterpret_cast<void* const*>(&(g)), __builtin_return_address(0))

// Whether `caller` (a hook's return address) is in one of the modules dd2d9 doesn't patch:
// Windows' own (but for its compatibility shims, which pass a game's calls on),
// other wrappers (see PatchGameModules) and dd2d9 itself. Their imports aren't dd2d9's, but
// another hooker's patch can still send their calls through dd2d9's hooks: Windows'
// DirectInput 1-7 in Nerf Arena Blast and Unreal Tournament (its mouse read from
// GetCursorPos, which came back in the game's coordinates). Logs each such module once.
bool CalledFromWindows(const void* caller);
// In a hook: passes the call on unchanged when it came from such a module.
#define DD2D9_PASS_WINDOWS(g, ...) \
    do { \
        if (::dd2d9::CalledFromWindows(__builtin_return_address(0))) return DD2D9_NEXT(g)(__VA_ARGS__); \
    } while (0)

}  // namespace dd2d9
