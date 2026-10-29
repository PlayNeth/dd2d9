/* Stand-ins for Wine's debug helpers, for running Wine's tests on Windows / with mingw. */
#include <windows.h>
#include <stdio.h>
const char* __cdecl __wine_dbg_strdup(const char* s) {
    static char buf[32][1024];
    static LONG i;
    char* b = buf[InterlockedIncrement(&i) & 31];
    lstrcpynA(b, s, sizeof(buf[0]));
    return b;
}
int __cdecl __wine_dbg_output(const char* s) { return fputs(s, stderr); }
const char* __cdecl debugstr_a(const char* s) {
    char tmp[1024];
    if (!s) return "(null)";
    snprintf(tmp, sizeof(tmp), "\"%s\"", s);
    return __wine_dbg_strdup(tmp);
}
