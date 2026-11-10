// The compatibility options' hook tables, each in the file of its topic (compat.cpp installs
// them).
#pragma once
#include "hooks.h"

namespace dd2d9 {

// compatsys.cpp: ram=, cpu=, cpus=.
extern ImportHook kMemoryHooks[], kCpuHooks[], kCpusHooks[];
extern const size_t kMemoryHookCount, kCpuHookCount, kCpusHookCount;
extern DWORD_PTR g_affinity;  // the processors the game was given (0: not set)
void SetAffinity();
// compatdisplay.cpp: the display settings' depth and mode.
extern ImportHook kDepthHooks[];
extern const size_t kDepthHookCount;
// compatgdi.cpp: GDI in a mode of the game's own, and on the window a primary is shown in.
extern ImportHook kWindowHooks[];
extern const size_t kWindowHookCount;

}  // namespace dd2d9
