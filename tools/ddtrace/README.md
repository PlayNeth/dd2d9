# ddtrace

A `ddraw.dll` that logs every DirectDraw (1–7) and Direct3D (3–7) call a game makes, with
what was asked and what came back, and passes each call on unchanged to a real `ddraw.dll`:
Windows' own, dd2d9, or another wrapper. It is a developer's tool for finding out why a
game behaves differently under one `ddraw.dll` than another. It is not part of dd2d9 and
is not needed to play anything.

Running a game once in front of a `ddraw.dll` where it works, and once in front of dd2d9,
gives two logs that line up call for call: the first answer that differs usually explains
the difference.

## Using it

1. Copy ddtrace's `ddraw.dll` and `ddtrace.ini.example` (renamed `ddtrace.ini`) into the
   game's folder, next to its exe.
2. Choose where the calls go, with `target=` in `ddtrace.ini`:
   - empty: Windows' own `ddraw.dll`;
   - dd2d9: rename dd2d9's `ddraw.dll` to `dd2d9.dll` (keep `dd2d9.ini`) and set
     `target=dd2d9.dll`;
   - another wrapper: rename its `ddraw.dll` (to `real.dll`, say), leave its other files where
     they are, and set `target=real.dll`.
3. Run the game. The log is `ddtrace.log` next to the exe, written over at each start, so
   rename it before the next run.

## What the log holds

Each line is one call: the interface and method, the object, the arguments, the result, and
decoded data where it helps: surface descriptions and pixel formats, rectangles, the first
vertex of a draw, matrices, viewports, device capabilities, display modes, texture formats
and devices as enumerated. Calls the real `ddraw.dll` makes to itself while handling one
are marked `(inside)` and indented.

Frequent calls (draws, render and texture states, locks, blits, flips, `AddRef` and
`Release`) are logged for the first 20000 of them, then for one whole frame every 300
frames (between `==== frame N ====` lines), and whenever they fail. Every other call is
logged up to 5000 times per method, and always when it fails.

## How it works

ddtrace hands the game the real objects and patches their method tables in place, so
nothing is wrapped and the real `ddraw.dll` sees exactly the game's calls. Objects are
followed as they are handed out: creation, `QueryInterface`, attached surfaces, devices,
viewports, textures and so on.

Windows' Direct3D 7 (`d3dim700.dll`) copies each device's method table into the device
itself and rewrites its draw and state methods as its state changes; a new device can also
land where a released one was. ddtrace watches every Direct3D device's table at each call
and patches it again when it changes (noted in the log, the first 50 times), and takes the
real method from the table a copy was made from when a copy already holds ddtrace's own.

## Building

It is built with dd2d9 (`ninja -C build`), as `build/tools/ddtrace/ddraw.dll`. The method
tables come from `gen_methods.py`, which writes `methods.inc`; run it again after changing
the lists.

## Limits

- Only the `ddraw.dll` exports are passed on as they are; DirectDraw objects made through COM
  (`CoCreateInstance`) are not followed.
- The wide-character enumerations (`DirectDrawEnumerateW`, `DirectDrawEnumerateExW`) are
  logged without their entries.
- A device call made right after Direct3D rewrites its table, before ddtrace's next check,
  goes unlogged (at most part of a frame).
