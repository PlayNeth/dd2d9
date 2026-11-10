// What input.cpp (the cursor) and inputpaint.cpp (paint areas, popups) share: the game's
// window and rectangles between its mode and the real screen.
#pragma once
#include "hooks.h"

namespace dd2d9 {

// The game's window, while its display mode is translated: as with a real mode switch, it
// covers the game's screen at 0,0, so its client and screen coordinates are the same.
bool IsGameWindow(HWND h, SIZE* mode = nullptr);
// The game's rectangle (exclusive right/bottom edges) on the real screen and back.
void RectFromGame(RECT* rc, const RECT& r, SIZE m);
void RectToGame(RECT* rc, const RECT& r, SIZE m);

// inputpaint.cpp's hooks, installed with input.cpp's.
extern ImportHook kPaintHooks[];
extern const size_t kPaintHookCount;

}  // namespace dd2d9
