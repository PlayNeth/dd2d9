// Reload test: a wrapper's Direct3D <-> software switch (Hydro Thunder behind
// D7VK). dd2d9 hooks the window, the wrapper subclasses it on top, DirectDraw is
// released and the DLL freed and loaded again; window messages must still work.
//   reloadtest [<dll>]     default ddraw_.dll; results in reloadtest.txt next to the exe
#include <windows.h>
#include <unknwn.h>
#include <ddraw.h>
#include <cstdio>
#include <cstring>
typedef HRESULT (WINAPI *Create)(GUID*, IDirectDraw**, IUnknown*);
static WNDPROC g_prev;
static LRESULT CALLBACK Wrapper(HWND h, UINT m, WPARAM w, LPARAM l) { return CallWindowProcA(g_prev, h, m, w, l); }
static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmd, int) {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    strcpy(strrchr(path, '\\') + 1, "reloadtest.txt");
    FILE* out = fopen(path, "w");
    const char* dll = cmd[0] ? cmd : "ddraw_.dll";
    WNDCLASSA wc = {}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "u";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "u", "u", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
    for (int round = 0; round < 2; round++) {
        HMODULE m = LoadLibraryA(dll);
        auto create = m ? reinterpret_cast<Create>(reinterpret_cast<void*>(GetProcAddress(m, "DirectDrawCreate"))) : nullptr;
        if (!create) {
            fprintf(out, "cannot load %s\n", dll);
            return 1;
        }
        IDirectDraw* dd = nullptr;
        create(nullptr, &dd, nullptr);
        dd->SetCooperativeLevel(hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
        if (round == 0) g_prev = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)Wrapper);  // the wrapper's subclass
        dd->Release();
        FreeLibrary(m);
        fprintf(out, "round %d: freed\n", round); fflush(out);
        SendMessageA(hwnd, WM_USER, 0, 0);  // goes through the wrapper's subclass to whatever it saved
        SendMessageA(hwnd, WM_ACTIVATEAPP, FALSE, 0);
        fprintf(out, "round %d: messages delivered\n", round); fflush(out);
    }
    fprintf(out, "PASS window messages work after the DLL was freed and reloaded\n");
    fclose(out);
    return 0;
}
