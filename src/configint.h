// What config.cpp found, for log.cpp's opening lines, and the path helpers both use.
#pragma once
#include "common.h"

#include <string>
#include <vector>

namespace dd2d9 {

// Paths are handled as UTF-16 throughout: the ANSI APIs mangle folder names
// with characters outside the current code page (accents, CJK, ...).
constexpr size_t kPathMax = 32768;
extern std::wstring g_dllPath;                 // where this ddraw.dll was loaded from
extern std::vector<std::wstring> g_iniLoaded;  // config files that were read
extern std::vector<std::wstring> g_iniErrors;  // unknown keys / bad values

std::wstring ModulePath(HMODULE m);
std::wstring DirOf(const std::wstring& path);
std::wstring BaseName(const std::wstring& path);
std::string Utf8(const std::wstring& w);

}  // namespace dd2d9
