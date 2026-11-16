// fmtprobe's two parts (fmtprobe.cpp, fmtprobe2.cpp).
#pragma once
#include <windows.h>

// DirectDrawCreateEx, found by name (DirectX 6's ddraw.dll has none).
using CreateExFn = HRESULT(WINAPI*)(GUID*, void**, REFIID, IUnknown*);

// The second part: its sections run one by one, each in a process of its own that appends to
// the file at `path` (closed by then).
void FmtprobeMore(const char* path);
// "fmtprobe section N": section N of the second part, appended to the file at `path`.
int FmtprobeSection(const char* number, const char* path, CreateExFn createEx);
// "fmtprobe try N": call N of the second part's list made, its result returned (the
// process's exit code).
int FmtprobeTry(const char* number, CreateExFn createEx);
