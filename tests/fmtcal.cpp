// fmtcal.dll: a gamma calibrator for fmtprobe's calibrator sections. ddraw.dll loads the one
// registered under HKLM\Software\Microsoft\DirectDraw\GammaCalibrator ("Path") and calls its
// CalibrateGammaRamp on SetGammaRamp(DDSGR_CALIBRATE). This one writes what it was given to
// fmtcal.txt beside it and inverts the ramp, or fails (DDERR_GENERIC, the ramp untouched)
// while fmtcal-fail.txt is there.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>

#include <cstdio>
#include <cstring>

static char g_dir[MAX_PATH];

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        if (char* slash = strrchr(g_dir, '\\')) slash[1] = 0;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) HRESULT WINAPI CalibrateGammaRamp(LPDDGAMMARAMP ramp, LPBYTE driver) {
    char log[MAX_PATH + 16], fail[MAX_PATH + 16];
    snprintf(log, sizeof(log), "%sfmtcal.txt", g_dir);
    snprintf(fail, sizeof(fail), "%sfmtcal-fail.txt", g_dir);
    const bool failing = GetFileAttributesA(fail) != INVALID_FILE_ATTRIBUTES;
    if (FILE* f = fopen(log, "a")) {
        if (ramp)
            fprintf(f, "driver \"%s\", red %04x %04x %04x .. %04x, green %04x, blue %04x%s\n",
                    driver ? reinterpret_cast<const char*>(driver) : "(null)", ramp->red[0], ramp->red[1], ramp->red[2], ramp->red[255],
                    ramp->green[255], ramp->blue[255], failing ? " (failing)" : "");
        else
            fprintf(f, "no ramp\n");
        fclose(f);
    }
    if (failing) return DDERR_GENERIC;
    if (ramp)
        for (int i = 0; i < 256; i++) {
            ramp->red[i] = WORD(0xFFFF - ramp->red[i]);
            ramp->green[i] = WORD(0xFFFF - ramp->green[i]);
            ramp->blue[i] = WORD(0xFFFF - ramp->blue[i]);
        }
    return DD_OK;
}
