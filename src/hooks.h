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
// were redirected. inLoaderLock: called from DllMain, where the module list is read without
// the tool-help snapshot. Hooks whose DLL isn't loaded are skipped. A module is patched for
// a given `hooks` array once (later calls skip it): `hooks` must stay where it is.
unsigned PatchGameModules(const ImportHook* hooks, size_t n, bool inLoaderLock = false);
// Restores every patched import (before dd2d9 is unloaded while the process goes on).
void UnpatchAll();

}  // namespace dd2d9
