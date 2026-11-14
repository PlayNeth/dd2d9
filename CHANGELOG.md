# Changelog

Each entry says what changed and, for behaviour fixes, the game or test that showed it,
so a later change doesn't undo it without knowing why.

## 1.7.3
Mega Man X starts and runs on Windows 10 and 11. Its sound library, Miles Sound System 3
(`wail32.dll`, 1995), relied on three things Windows no longer does; each is now handled by
default, with a setting to turn it off. Its first Alt+Tab needed one more fix, in the window
handling.

- **Multimedia timer callbacks get back the registers they should have kept**
  (`safetimers=on`). Miles' timer callback leaves an argument on the stack for each of the
  game's timer handlers it runs (the game's, built with Watcom, doesn't take it off) and pops
  the wrong values into EBX, ESI and EDI. Up to XP, winmm called callbacks through a helper
  that put the registers back; Windows 10 and 11 call them directly and go on using ESI: the
  game crashed in winmm a moment after it started (Wine's timer thread crashes the same way).
  dd2d9 now calls the game's timer callbacks through a short piece of code that puts back
  EBX, ESI, EDI, EBP and the stack (a callback written as cdecl leaves its arguments on it)
  and clears the direction flag. Timers that set or pulse an event are left as they are, and
  `timeKillEvent` takes the same timer IDs. A game that looks `timeSetEvent` up by name gets
  the same.
- **A sound callback that suspends its own thread carries on** (`selfsuspend=off`). Miles'
  sound callback starts by suspending the game's main thread, as an interrupt would have
  stopped it, and resumes it at the end. Up to XP that callback came from the sound driver's
  thread; since Vista winmm calls it on the thread inside `waveOutOpen`, here the main thread
  itself, which then never woke up: a black screen, with the game's timer still running.
  Inside a `waveOutOpen` or `midiOutOpen` callback, `SuspendThread` on the calling thread now
  returns at once, and the callback's `ResumeThread` finds the thread running, as Windows'
  does. Those callbacks also get back their registers, as the timer callbacks do. A thread
  suspending itself anywhere else is suspended, as before.
- **The game's joystick reads are answered from a thread of dd2d9's** (`joypoll=on`).
  Miles' timer suspends the main thread while it runs the game's timer handler, and Mega Man
  X reads the joystick from both. winmm holds a lock through a joystick read (XP's and
  Windows 11's alike, and Wine's): the main thread suspended while it held it left the
  handler waiting for good.
  dd2d9 reads the joysticks the game asks for every few milliseconds, with the game's flags,
  and `joyGetPosEx` and `joyGetPos` answer from that. The first read of a joystick, and one
  after 2 s without any, goes to winmm itself when it isn't made inside a timer or sound
  callback, so the game never waits for that thread. A joystick nobody has read for 2 s is
  no longer polled, and one that isn't plugged in is polled every 100 ms.
- **A game that minimizes and maximizes its own window while in front is told it's active
  again** (`WM_ACTIVATEAPP`). Back from its first Alt+Tab, Mega Man X minimizes and maximizes
  itself once. Minimized, it marks its surfaces to be restored and draws nothing until
  `WM_ACTIVATEAPP` has restored them. On Windows 9x the minimized window lost the activation
  and got it back; Windows 11 (and Wine) leave it in front, so nothing came, and its screen
  stood still until the next Alt+Tab: black, or the intro's text over the title screen. Only
  the game's window procedure hears it; for dd2d9 nothing changes.
- The log names the first timer callback and what it left in the registers it should have
  kept; `log=2` adds each `timeSetEvent` and `timeKillEvent`, and the callbacks counted every
  10 s.
- `ddtest` checks each: a timer callback that keeps none of the registers, sets the
  direction flag and returns as cdecl (1.7.2 crashed in Wine's timer thread); a Miles-style
  timer reading the joystick while it suspends the thread that also reads it (stalled for
  good without `joypoll`); a sound callback suspending its own thread, and a thread outside
  one suspended for real; and a window minimized and maximized by the game while in front,
  told it's active once back (1.7.2 told it nothing).

## 1.7.2
- **A dialog of the game's own gets the mouse.** In exclusive mode dd2d9 keeps the cursor on
  the game's screen while the game is in front. It kept it there while another of the game's
  windows was in front too, and Claw's custom level menu, a dialog over the game, didn't
  take the mouse under Wine with Openbox (Wine's own ddraw doesn't keep the cursor on the
  screen). Now the cursor is let go while another of the game's windows is in front, and
  kept on the screen again once the game's window is back in front. Meanwhile the game's
  window isn't raised over the dialog (nor by the timer that keeps it topmost), and other
  programs still can't take the front on their own. The log says which, each time it
  changes.

## 1.7.1
Two fixes for Unreal Tournament: the picture shifted down and to the right when the game
started, and parts of the weapon drawn over the HUD.

- **A window whose frame the game takes away with `SetWindowLong` alone covers the screen
  again for its new frame.** Without `SWP_FRAMECHANGED`, Windows keeps the old frame, so the
  client area still covered the screen and dd2d9 left the window alone (it also skipped
  windows without `WS_VISIBLE`, and Unreal Tournament clears that in the same call while
  its window stays on the screen). Under Wine the window manager placed the window by its
  new frame: the picture started 30 rows lower and a few columns to the right, with its
  bottom off the screen, until a change of resolution or colour depth covered the screen
  again. Now the window is placed for the new frame, with `SWP_FRAMECHANGED` so that Windows
  applies it, and a window without `WS_VISIBLE` stays that way. A frame changed while the
  window is minimized is caught once it is restored, and a frame the game applied itself
  (with `SWP_FRAMECHANGED`) is only noted. `ddtest` checks it (`fs8`:
  the window is the screen afterwards, still hidden). It failed on 1.7.0.
- **Emulated W buffering keeps one depth scale for what is in the depth buffer, and takes
  its near end from the projection.** Depth is 1 − k/w. Without a projection, k was fitted
  to the nearest vertex so far and shrank whenever a nearer one came. In a player's Unreal
  Tournament trace, the game clears the depth buffer, draws its weapon (part of it nearer
  than w 1), then draws its HUD and text at w 1. The scale shrank partway through the
  weapon, so the parts drawn before then no longer compared with the ones drawn after: they
  came out in front of nearer parts of the weapon and in front of the HUD.
  - k now shrinks only while nothing has been drawn into the depth buffer since it was last
    cleared (a whole `Clear`, or now also a depth fill of the whole buffer done on the GPU). A vertex nearer
    than k reaches gets depth 0, as at a W buffer's near end, and what is drawn later at
    that depth comes in front (`LESSEQUAL`).
  - A game that asks for W buffering, draws only transformed vertices and sets a perspective
    projection now gets k from the projection's near plane, as Direct3D gave the driver its
    W range from that matrix: what is nearer than the near plane is the nearest there is.
    With a near plane at w 1, the HUD is in front of the weapon whatever order the weapon's
    parts come in. The log names the near plane the first time. The depth buffer's steps
    are then spread from the near plane out, as the projection's own depth is for a game
    that also draws untransformed vertices, where the scale started from the nearest vertex:
    with a near plane far below the nearest geometry, distant surfaces get fewer steps.
  - `d3d7test` draws both cases: the weapon's parts at w 1.19, 1.67, 0.83 and 1, then the
    HUD at w 1, without a projection; and with a near plane at 1, the nearest part drawn
    first. Both failed on 1.7.0.

## 1.7.0
Faster stretched blits, Direct3D 3–6 mesh draws and Direct3D 7 vertex buffers, and fixes: a 3-second stall
when a game DLL releases DirectDraw as it is unloaded, more lights than the GPU takes,
`ValidateDevice`, functions looked up at run time, and emulated W buffering of transformed
vertices among untransformed ones.

- **A game DLL that releases DirectDraw as it is unloaded no longer stalls for 3 seconds.**
  Released from a DLL's `DllMain` (under the loader lock), DirectDraw waited for its presenter
  thread and its device window's thread to end, which they can't until that lock is free: 3 s
  at every such unload, after which the presenter was left behind with its device. Now the
  presenter's thread says when it is done with the presenter, which is all the release waits
  for, and neither thread's end is waited for: each keeps dd2d9 loaded until it is out of
  dd2d9's code. `ddtest` checks it (a DLL releasing a DirectDraw object that has shown
  something, as it is unloaded: 3,003 ms before, 8 ms now, under Wine). A release at the
  process's exit, and an ordinary one, are as before.
- **Direct3D 7's vertex buffers are sent with every draw again.** 1.6.0 drew them from a
  Direct3D 9 copy, comparing the vertices a draw uses with the copy at every draw (so that
  whatever the game wrote was drawn). The compare read as much memory as sending the vertices
  does, and the copy's own calls came on top: on Wine with DXVK, wrapbench's spheres from a
  vertex buffer ran at 5,711 frames a second from the copy and 6,314 without it (the same
  spheres from memory: 6,308 and 6,292). The copy, its index buffer and their bookkeeping are
  gone; d3d7test's vertex buffer checks stay.
- **Direct3D 7: more lights enabled than the GPU lights at once.** XP lit a HAL or software
  device's vertices itself, with any number of lights, and those devices report no limit;
  Direct3D 9 lights 8 at once (as a rule) and refused to enable a ninth, so which lights a
  game lost depended on the order it enabled them in. With more enabled than the GPU takes,
  dd2d9 now gives it the ones that light the object most (by colour, and for point and spot
  lights by their attenuation at the object's place, none past their range), chosen again
  when a light or the world matrix changes. An approximation, where 1.6.0 dropped lights by
  order. d3d7test: eight dim lights, then a bright one (it was missing: 0x002900; now
  0xe62400). No game is known to need it.
- **Exclusive mode: the window gets a region only where there is another monitor** (Moto
  Racer under Wine: the top of the picture cut off, the desktop showing there). dd2d9 grows
  the game's window until its client area is the screen and gave it a region showing only that
  part, so that the frame could not appear on a neighbouring monitor. Wine turns a region into
  an X11 shape placed by the window's frame at that moment; Moto Racer takes its window's
  caption away between two of its screens (with `SetWindowLong` alone), and the shape stayed
  where the caption's frame had put it: 29 rows at the top and 3 columns at the left of the
  picture were cut away. With one monitor the frame is off the screen anyway, so no region is
  set there (one set before is removed); with several it is as before. A test that replays
  the game's window calls shows the whole picture now under Wine 11, with and without a window
  manager (openbox); `ddtest` checks that there is no region on a single monitor. 1.6.0 had
  it too.
- **Emulated W buffering: transformed vertices drawn among untransformed ones take their
  depth from the projection matrix** (Direct3D 7). Untransformed vertices were given the
  projection's own depth and transformed ones a depth from their `rhw` on a scale of its own,
  so the two didn't agree on what is in front: a model the game transformed itself could
  vanish behind a wall Direct3D transformed, though nearer. Once a game has drawn
  untransformed vertices with W buffering, its transformed ones get the depth the projection
  matrix gives their w (nearer than the near plane: the nearest there is; past the far plane:
  the farthest), as a W buffer's range came from that matrix. A game that draws only
  transformed vertices, or sets no perspective projection, keeps the scale found from its
  vertices, which uses the depth buffer better than a near plane does. d3d7test: a wall at w
  100, a transformed square at w 50 (it was hidden) and one at w 200. No game is known to
  need it.
- **`ValidateDevice` checks the stage setup** (Direct3D 6 and 7). It answered one pass for
  anything. XP asks the driver, and dd2d9 can draw what Direct3D 7 can express, so one pass
  stays the answer for a setup within what the device reports; a colour or alpha operation
  the device's caps don't list, an argument that names nothing, or more stages or textures
  than it reports now get the stage setup's own error (`D3DERR_UNSUPPORTEDCOLOROPERATION`,
  `..COLORARG`, `..ALPHAOPERATION`, `..ALPHAARG`, `D3DERR_TOOMANYOPERATIONS`) and no passes,
  as a driver of the time answered a game probing what it may use. d3d7test checks each.
- **Functions a game looks up at run time get every translation, not only the options'.**
  A game's `GetProcAddress` was answered with dd2d9's function for the compatibility
  options' hooks (memory, processors, display settings), but not for the mouse, paint and
  movie-window translations made when the game sets a display mode: a game picking
  `GetCursorPos`, `ClientToScreen` or `MoveWindow` by name (code with a Windows 95 and an NT
  path) got Windows' own, untranslated in a scaled mode. Every installed table counts now. A
  function looked up before the game set a mode stays Windows'. No game is known to need it.

Optimizations:
- Stretched blits without colour keys (`Blt` to another size, mirrored or not): a row
  stretched to three times its width or more is written a run of equal pixels at a time, with
  16-byte stores, where every pixel was looked up on its own; the source column of each
  destination column and the source row of each destination row are stepped, not divided for
  (a 64-bit division each). wrapbench's `2d-stretch` blits (64x64 to up to 640x480, 16-bit):
  15.6 to 10.5 µs each under Wine; 32-bit: 19.9 to 17.2 (there the time is the rows repeated
  down the picture, as before). The pictures are the same byte for byte (18,000 stretches of
  every size, part and mirroring in 8, 16 and 32 bits, hashed with 1.6.0's blitter and this
  one).
- Direct3D 3–6 mesh draws (`DrawIndexedPrimitive`, execute buffers): the indices are scanned
  for their range sixteen at a time (three times a draw, as long as packing its vertices
  took); where the transform found every vertex inside the clip volume, the guard-band pass
  and the per-vertex test for projecting are skipped; the vertices are packed for the GPU with
  fixed-size copies. A sphere of 1,089 vertices: 48–54 to 39–43 µs a draw under Wine;
  d3dimbench's `dip sphere` 796 to 624 µs a frame in the call, `dip rows (dev2)` 1,712 to
  1,485, `exec-rows` 250 to 220. Every hash, clip status and check as in 1.6.0. (The transform
  and lighting loop itself was left: its remaining time is the lighting model's divisions and
  square roots, and eight vertices at a time with AVX2 gained 1.36 times on that arithmetic,
  about a tenth of a draw, for a second copy of the loop.)

## 1.6.0
A windowed game's own GDI drawing into its window kept on screen beside dd2d9's frames
(Swordman's windowed versions no longer flicker after a movie), then fixes across the whole
code, each checked against XP's behaviour (DXT surfaces copied without loss, Direct3D 7's
colour key and user clip planes, paint areas in scaled modes that never validated, DLLs
loaded later patched as they load, a device-reset race in the presenter), workarounds replaced by fixes, faster Direct3D 2
indexed draws, Direct3D 7 state changes, vertex buffers and converting blits, a `heapzero`
setting, and the largest source files split. Every d3dimbench hash, every pixel check and the
audits give what 1.5.1 gave, but where a bullet below says otherwise.
(Prepared as 1.5.1, never released: all of it is in 1.6.0.)

- **A windowed game's own drawing into its window goes into the primary shown there.**
  `ddraw.dll`'s windowed primary was the screen itself, where a game's GDI drawing landed too;
  dd2d9 shows its primary through the window instead, so what the game drew there with GDI was
  covered by dd2d9's next frame (its refresh four times a second included), and with the
  desktop compositor the two were shown in turn. Swordman (神魔至尊傳)'s windowed versions draw
  the game with `StretchDIBits` and play their movies through Bink's DirectDraw primary: after
  the first movie its last frame, on black, flashed over the game a few times a second. A
  game's `StretchDIBits`, `SetDIBitsToDevice`, `BitBlt`, `StretchBlt` and `PatBlt` into a
  window a normal-mode DirectDraw object shows its primary in now draw into that primary, as
  they would have on the screen (the window's position, its visible area and the game's
  clipping, the DC's stretch mode, colours and brush), and are shown with its frames. Calls
  from Windows' modules, DIBs of palette indices and copies from the screen go to the window
  as before, as do exclusive mode and windows without a primary shown in them. Confirmed in
  Swordman (`Sm_win2.exe`, Windows 11): the game no longer flickers. `ddtest win` checks it
  (a fill of the primary, then `StretchDIBits`, `StretchBlt` and `BitBlt` into the window: 1.5.0
  showed the fill again 33 times in 40).
- **A critical section the game releases goes to the thread waiting for it**, as on Windows
  up to XP, where the waiter owned it from that moment; since Vista (and under Wine) it is
  only woken, and a thread that releases a section and takes it again at once nearly always
  gets it.
  KOF 99 Evolution's main thread holds its movie renderer's section through the whole intro
  and lets go of it once a frame, for the decoder's thread to copy a frame into the surface
  the game draws: a player's logs show that surface written twice in an 18-second intro
  with `cpus=0`, and never with `cpus=1`, so the intro was black. Now a
  thread releasing a section another of the game's threads is waiting for stands aside
  until the section has been taken (the `fairlocks` setting, on by default; the game's own
  `EnterCriticalSection` and `LeaveCriticalSection` imports). `ddtest` checks the pattern
  (100 releases: the waiter gets the section at each; at 0 to 2 with `fairlocks=off`, under
  Wine 11). A waiter that was killed is dropped, and one that can't run is not waited for
  again for a quarter second. Confirmed in the game by a player: the intro
  plays.
- **Emulated W buffering: a vertex with a huge rhw no longer flattens the frame's depth.**
  The emulation takes depth from 1/w with a scale that fits the nearest vertex of the frame
  so far. KOF 99 Evolution draws a blinking warning outline around a life bar that is low,
  first in the frame, with an rhw of 13131952 (the rest of the game uses 0.005 to 0.02): the
  scale it set gave every transformed vertex after it the same depth, so in each frame with
  the outline the fighters and the whole HUD fell behind the stage, and flickered with the
  outline's blinking until the round ended. A W buffer has a range, and such a vertex is just
  the nearest in it: an rhw above 65536 now gets depth 0 and leaves the scale alone. Found in
  a player's trace (the outline's lines are the first draws of those frames); d3d7test draws
  the case (a near square stays in front of a far one drawn after such a vertex), failing
  before. Confirmed in the game by a player.

Other fixes:
- **DXT surfaces are copied block for block between surfaces of one format.** A `Blt`,
  `BltFast` or texture `Load` (Direct3D 3–7) from a DXT1–5 surface to another of the same
  format was decoded and encoded again by dd2d9's own encoder, which changed the blocks (a
  block using only interpolated colours came out different). Whole blocks with no clipper
  and no effects are now copied as they are, mip chains included; anything else goes through
  the decoding as before. New ddtest checks (Blt and BltFast): the blocks changed in 1.5.1,
  they arrive unchanged now.
- **`SetSurfaceDesc` gives a DXT surface the pitch of its blocks.** It kept the old format's
  pitch (or took the description's, which for DXT is the linear size): a 16x16 surface
  pointed at 128 bytes of DXT1 reported a linear size of 1024, and a blit into it wrote past
  the game's memory. New ddtest check.
- **A clipper released once too often leaves the surfaces it is set on**, which kept a
  pointer to the freed clipper. `GetClipper` then gives `DDERR_NOCLIPPERATTACHED` and blits
  behave as without a clipper, as Wine's `test_clipper_refcount` has Windows do. New ddtest
  check.
- **Direct3D 7's colour key drops the key's texels whatever stage 0's alpha ops say.** Only
  the alpha test was set, on whatever alpha stage 0 gave: with the vertices' alpha in it,
  keyed texels could be kept, or the whole texture dropped (a vertex alpha of 0). Stage 0's
  alpha op is now changed where the game's would defeat the key and nothing else reads it
  (a later stage through `D3DTA_CURRENT`, or a render target with alpha: there it is kept,
  as in 1.5.1), an untextured pass (`COLOROP` `DISABLE`) gets no key, and a game alpha test
  that lets alpha 0 through is replaced by the key's. New d3d7test checks (keyed texture,
  vertex alpha 0: all black in 1.5.1, right now; stage 1 blending by stage 0's vertex alpha
  with the key on: right in 1.5.1 and now).
- **Direct3D 7: `ProcessVertices` with `D3DVOP_CLIP` clips against the user clip planes**
  (only the frustum's six were coded, so a vertex buffer drawn afterwards wasn't cut by
  them), with fixed-size vertices instead of a heap allocation each. The clip status follows
  XP's `d3dim700.dll`: not tracked on the T&L HAL device; with `CLIPPING` off transformed
  vertices still grow the extents; transformed vertices from the game's memory are coded.
  New d3d7test check (a clip plane through ProcessVertices): the left half drawn in 1.5.1,
  cut now.
- **Direct3D 7: a viewport larger than a newer, smaller render target** (Direct3D 7 keeps
  it, and doesn't check its Z range) reaches Direct3D 9 cut to the target, Z clamped to 0–1,
  the difference folded into the projection so the vertices land on the same pixels. Native
  Direct3D 9 refused such a viewport and kept the previous one. New d3d7test check (Wine's
  Direct3D 9 accepts the viewport, so it passed before there too; it checks the fold). A
  viewport wholly past the target draws and clears nothing, as on XP.
- **Direct3D 2's `DrawIndexedPrimitive` transforms only the vertices its indices use**, as
  XP's `DereferenceIndexedPrim` does with fewer indices than half the vertices: the clip
  status and extents now come from those vertices only (a row of a sphere reported clip
  codes of other rows). `IDirect3DDevice3`'s FVF version still transforms every vertex, as
  XP.
- **Direct3D 3–6:** `ProcessVertices` without `D3DVOP_CLIP` clears the destination's clip
  codes (old ones were left there); devices and viewports still alive after their
  DirectDraw object forget surfaces as they are destroyed (they kept pointers to them).
- **Out of memory is returned, not thrown through COM** (blits, Direct3D 3–6 and 7):
  `DDERR_OUTOFMEMORY`/`E_OUTOFMEMORY`, as XP does when its buffers can't grow.
- **Paint areas are mapped both ways alike in a scaled mode.** `GetUpdateRect`,
  `GetUpdateRgn` and `BeginPaint` gave the game its mode's coordinates and `InvalidateRect`
  mapped them back, but `ValidateRect`, `ValidateRgn`, `InvalidateRgn` and `RedrawWindow`
  were passed on as they were: a game validating the rectangle `GetUpdateRect` gave it
  validated only that much of the real client area, and got `WM_PAINT` again forever. Paint
  rectangles are rounded outward both ways, so one mapped to the mode and back covers all it
  did (rounded down, an inner one at a scale such as 1.5 or 2.25 came back a pixel short),
  and a strip beside the image is the mode's border pixel. New ddtest checks (`mouse`):
  0,0-640,480 left to repaint in 1.5.1, nothing now; an odd inner rectangle and a strip
  beside the image, a row or column left before, nothing now.
- **A movie popup's black background is its own.** dd2d9 set the black brush on the popup's
  window class (Road Rash), so every window of that class, the game's main window too when
  it shares it, was erased black from then on. The popup's own erase is now done black where
  its window procedure leaves it undone (returns 0 having drawn nothing); the class is left
  alone. The ddtest check changed with it, and a new one has a popup that paints its own
  background keep it.
- **DLLs are patched as Windows loads them.** dd2d9 patched the modules it found when a
  DirectDraw object was made, from a snapshot: a DLL loaded later wasn't patched until the
  next one, and one listed while another thread was still loading it could be skipped for
  good (and its import table written while the loader was writing it). dd2d9 now has the
  loader tell it of each DLL as it loads (`LdrRegisterDllNotification`, after its imports
  are bound and before its `DllMain`) and of each unloaded. Under Wine 11 a DLL loaded with
  `LoadLibrary` after the start, with no DirectDraw object made after it, kept its real
  `HeapAlloc` in 1.5.1 and is patched now, again after being freed and loaded again;
  shimtest and reloadtest still pass. Not tried on Windows yet.
- **Wrappers are left alone by what they export, not only their name**: a module exporting
  `DirectDrawCreate`, `Direct3DCreate8`/`9`, `D3D11CreateDevice`, `CreateDXGIFactory`,
  `wglGetProcAddress`, `vkGetInstanceProcAddr`, `DirectInputCreateA`, `DirectInput8Create`,
  `DirectSoundCreate` or `grGlideInit` (DXVK, dinputto8 or gl2d9 under any name) isn't
  patched and counts as Windows' for the cursor hooks.
- **The DirectDraw object:** a second `SetCooperativeLevel` keeps `DDSCL_FPUSETUP`,
  `DDSCL_FPUPRESERVE` and `DDSCL_MULTITHREADED`, as XP; an object from the class factory
  initialized through `IDirectDraw7` is a DirectDraw 7 object (it offers `IDirect3D7`; XP
  goes by the interface `Initialize` is called through); a surface in non-local video memory
  no longer also reports `DDSCAPS_LOCALVIDMEM` (new ddtest checks for both, each failing in
  1.5.1); a thread's wait timer is closed after each wait (one leaked per thread that waited,
  thread exits not reaching dd2d9); the window procedure reads the object it serves under a
  lock of its own (it raced with the object's teardown); the size last told the game is
  forgotten for a new window.
- **A focus or display change that finds the display busy waits for it, never dropped.**
  The window procedure retried such a change from posted messages with a millisecond's
  sleep, for up to two seconds (1.4.1); it now records it, and whoever releases the display
  lock posts one message that does it. `GetScanLine`, `GetVerticalBlankStatus` and
  `WaitForVerticalBlank` follow the desktop compositor's vblank wherever there is one
  (Windows with composition on, `vsync=off` too), the clock `Flip` is paced by (1.5.0): one
  clock instead of two.
- **The presenter:** after a device reset the presenter thread wrote surface state the
  game's threads guard with another lock; the loss is now taken over where both are held. A
  frame the presenter skipped as unchanged (or couldn't show, the device lost) left a `Flip`
  waiting for it up to 100 ms. More than 64 depth fills before a draw lost the oldest; a
  fill now replaces only the earlier ones it covers. The device window's class no longer
  points into dd2d9, and its thread is waited for, so no dd2d9 code runs once the DLL may be
  unloaded. A swap chain, scaling texture or gamma texture that failed is no longer made
  again every frame (the swap chain logged every 50 ms): the swap chain is tried again 2 s
  on, the others with the next device or at another size.
- **Smaller fixes:** a `GetSystemMetrics` in the game's mode also answers
  `SM_CXFULLSCREEN`/`SM_CYFULLSCREEN`, `SM_CXMAXIMIZED`/`SM_CYMAXIMIZED` and, with one
  monitor, the virtual screen in the mode's terms; `LockWindowUpdate`'s skipped and real locks
  are kept apart (with a real lock held, a skipped one and then the unlock left the real one
  in place); a `DEVMODE` copied for `desktopbpp` gives its copied size in `dmSize`; a
  registry key is forgotten before it is closed; `DrawIndexedPrimitiveUP` with an index past
  the vertices draws nothing instead of a wrong range (both Direct3D front ends already
  replace such indices).

Behaviour changes to watch:
- **A Direct3D game flipping without vsync (`DDFLIP_NOVSYNC`) is presented at once where the
  device waits for vblanks** (under Wine, or Windows without the desktop compositor, `vsync=auto`).
  `Flip` already returned at once, but dd2d9's presents still waited for a vblank on the
  device the game draws on, and its Direct3D frames waited with them: wrapbench's Direct3D 7
  spheres ran at 120 fps under Wine and DXVK, 2491 with `vsync=off`. After 8 flips in a row
  without vsync of a buffer Direct3D draws into (a 2D game's presents never held it back),
  and without a `WaitForVerticalBlank`, `GetScanLine` or `GetVerticalBlankStatus` before them
  (a game that waits for the blank itself and flips with the flag keeps its tear-free
  presents), the frames are presented through a swap chain that doesn't wait, at most two a
  refresh (the newest frame shown, not thousands); 8 flips otherwise put the vsync back, and so
  does a quarter second without flips (a menu or movie blitted to the primary presents at
  vblanks again). With that, the spheres ran at about 2600 fps under Wine and DXVK. Should that swap chain's `Present` fail, the
  device's own presents again. The log says when (`vsync: the game flips without vsync
  (DDFLIP_NOVSYNC): presented at once`). Tearing is what such a game asked for; `vsync=on`
  still forces it.
- **`vsync=off` presents at most two frames a refresh.** It presented after every flip,
  thousands a second for a 2D game that flips as fast as it can, each one converted and
  uploaded for nothing the display could show, taking that time from the game: wrapbench's 2D
  tests under Wine and DXVK ran at 1645 fps colour fills and 2081 Lock frames with
  `vsync=off`, 3922 and 11750 with `auto`; with this change `vsync=off` runs as fast as `auto`
  (measured under Wine's own Direct3D 9 here). Presents still don't wait for a vblank (uncapped,
  may tear), as before.
- **A mode change loses every DirectDraw object's surfaces, as on XP.** A display change
  made by one object (`SetDisplayMode`, `RestoreDisplayMode`, Alt+Tab out of a switched mode,
  emulated modes included) now loses the surfaces of the process's other objects too (a
  DirectShow movie's), and each object notices outside changes on its own. Games written for
  XP restore them; a report of a movie or second window going black after a mode change
  would point here.
- **Direct3D 2–6's colour key now works as Direct3D 7's** (the same shared code): a stage 0
  with `COLOROP` `DISABLE` gets no key, and a game alpha test that lets alpha 0 through
  (`ALWAYS`, `GREATEREQUAL` 0) gives way to the key's, so keyed texels are dropped; stage
  0's alpha op is no longer replaced where a later stage or a render target with alpha reads
  it. No d3dimbench hash changed, nor Wine's colour key and alpha test results.
- **`LockWindowUpdate` holds one lock at a time, as Windows**: a real lock of another window
  while a skipped one holds now fails (it was passed on).
- **`EnumSurfaces` with `DDENUMSURFACES_CANBECREATED`** describes each surface without
  allocating it: a mode size whose memory couldn't be allocated is now listed.

New settings:
- **`heapzero`** (default `on`): the zeroing of the game's `HeapAlloc` memory Daytona USA
  Deluxe needed (1.3.0) was always on; `heapzero=off` saves the clearing for a game that
  allocates a lot. It covers the `HeapAlloc` imports of the game's own modules, as before.
- **`fairlocks`** (default `on`): the critical sections handed to the waiting thread (above);
  `fairlocks=off` leaves them as Windows has them now.

Optimizations:
- Direct3D 2's indexed draws (above): the sphere drawn a row at a time through
  `IDirect3DDevice2` (d3dimbench's new `dip rows (dev2)`), 8–18 ms → 1.7–4 ms a frame in the
  calls under Wine (noisy runs; about a fifth to a quarter of the time), the same pixels.
- Direct3D 7's vertex buffers are drawn from a Direct3D 9 vertex buffer: a static mesh is
  copied to the GPU once, not sent again with every draw, as all of them were (on native
  Direct3D 9 also its slow path). Each draw compares the vertices it uses with the copy and
  copies again those that changed, however the game wrote them (through `Lock`, through the
  pointer an earlier `Lock` gave, as XP allows, or `ProcessVertices`). A buffer rewritten for
  every draw, and transformed vertices, are sent as before. Not measured: Wine's Direct3D 9
  doesn't show a GPU's upload cost.
- Direct3D 7's state calls only store the state, and a draw or clear sends what changed (the
  Direct3D 9 lock and the device check were taken at every call): 87–127 → 15–29 ns a call
  under Wine. Direct3D 3–6 sends stage 0's alpha op and arguments once per draw (they went
  twice with the default stage states and an opaque texture, a new shader key each under
  DXVK; not measurable under Wine's own Direct3D 9).
- Stretched and mirrored blits without keys write the source columns straight into the
  destination row, in a loop of their own (they went through a row of their own and were
  copied): ddbench's stretches and shrinks 4-22% faster (320x240 to 640x480 at 32 bits 4.3 →
  5.2 Gpix/s, 640x480 to 400x300 2.3 → 2.8), measured with both builds' code alignment
  pinned. Keyed blits run the same code as before.
- Converting blits keep their row converter from one blit to the next (an 8-bit 16x16 sprite
  onto a 16-bit surface 1.0–1.17 → 0.59–0.66 µs); an overlapping blit copies only its source
  rectangle aside (a mirrored 64x600 column moved within a 1024x768x32 surface 204–213 →
  29–30 µs); a FourCC blit decodes into memory the size of what it uses and frees more than
  16 MB of it after (1170 random FourCC and RGB blits give the same hashes as before).
- An 8-bit primary Direct3D drew into is matched back to its palette through a small table
  instead of a hash map: 640x480 5.5 → 0.65 ms, 10.0 → 2.7 ms with a quarter of the colours
  not in the palette (measured on the host).
- Writes into render targets between draws upload through up to three surfaces, waiting only
  when all are busy (not measured: a real GPU's stall doesn't show under Wine).
  RivaTuner's empty draw call is made only in frames that drew nothing.
- `DirectDrawEnumerateEx` keeps its display list until the monitors change (it made a
  Direct3D 9 object at every call, slow under DXVK (not measured), once a movie for
  DirectShow's Video Mixing Renderer 7). `EnumSurfaces`' `CANBECREATED` allocates nothing
  (8 MB a mode for 1920x1080x32, back buffers and mip levels not counted). A GDI blit into a
  window no primary is shown in (every one in exclusive mode) builds no regions. No module
  snapshot at each `DirectDrawCreate` (the loader notification above).

- **Source:** the largest files are split by subject, the code moved as it was: `ddraw.cpp`
  (the DirectDraw object: `ddmode`, `ddwindow`, `ddhook`, `ddscreen`, `ddtiming`, `ddcaps`,
  `ddcreate`, `ddgdi`), `surface.cpp` (`buffer`, `attach`, `lock`, `palette`, `clipper`),
  `blit.cpp` (`blitcore`, `blitfourcc`; `dxt.cpp` is `fourcc.cpp`), `gpures.cpp` (`gpu*`),
  `presenter.cpp` (`presentpass`, `presentconv`), `d3d7.cpp` (`d3d7obj`, `d3d7state`,
  `d3d7sb`, `d3d7draw`, `d3d7pipe`), `d3d6pipe.cpp` and `d3d6dev.cpp` (`d3d6xform`,
  `d3d6draw`, `d3d6exec`, `d3d6drv`), `util.cpp`, `compat.cpp` and `input.cpp` (`config`,
  `log`, `format`, `compat*`, `inputpaint`); the classes in `objects.h` have headers of their
  own. Duplicated code is merged (the full-screen passes in `quadpass.cpp`, the translation
  Direct3D 3–6 and 7 share in `d3dxlate.cpp`), dead code is gone (the unused Direct3D stubs
  and their generator among it).
- **Tests:** ddtest 386 → 414 checks, d3d7test 44 → 53, d3dimbench's `dip rows (dev2)`
  (the checks named above), all passing under Wine 11; the Wine ddraw sub-tests run for the
  areas changed (blits, clippers, DXT, colour keys, viewports, lighting, `EnumSurfaces`) give
  the same failures as with 1.5.1.

## 1.5.0
Vsync paced by dd2d9 under Windows' desktop compositor (N.I.C.E. 2 a steady 60 fps), the
mouse in Unreal engine games in borderless (Windows' DirectInput given the real screen),
colour keying switched on and off without remaking textures, N.I.C.E. 2's setup opens (Windows'
compatibility shims), a setting to hide palettized textures from Direct3D (N.I.C.E. 2's
16-bit textures), and sharp scaling by default.

- **Vsync under the desktop compositor: frames paced by dd2d9, Present doesn't wait.**
  dd2d9 presents into a window, which Windows' compositor (DWM) shows at its next pass; with
  vsync, Direct3D 9 made each Present wait for a vblank of its own, and Intel's driver spins
  the CPU for it. N.I.C.E. 2 on a laptop's UHD 620 (Windows 11): Present took 7–8 ms a frame,
  the game's own work slowed from 3–5 ms to 7–10 ms on the CPU and GPU's shared power budget,
  and of each 10 s's 600 frames 40–220 came 1–3 ms late and some a whole refresh late; it
  looked choppy beside Windows' ddraw. Where composition is on (Windows 8 and later, Windows 7
  with Aero) and `vsync` isn't `off`, Present no longer waits, and a Flip that waits for vsync
  waits instead for its slot on a schedule one refresh period apart (the compositor's own
  period, not a rounded rate), as `fpslimit` does: a frame late by less than a period doesn't
  move the next one. The schedule leans toward the compositor's vblanks, an eighth of the way
  a frame, so the game starts each frame at one whatever the clocks' drift. The presenter
  presents at least half a refresh apart (a game that blits to its primary as fast as it can,
  without Flip, is presented about twice a refresh, not hundreds of times). N.I.C.E. 2: every
  10 s window after the start 600 frames, all of them one refresh apart, 595–600 within
  16.2–17.1 ms; Present 0.4–0.6 ms. A first try that presented at once and then waited for the
  compositor's pass (`DwmFlush`) made it worse: a frame that missed a pass by a little waited
  a whole refresh, the game's timing caught up with more work a frame, and from 20 s in every
  frame took two or three refreshes. Not under Wine (its compositor isn't Windows') nor
  without composition: Present's vsync, as before. Asked again at each device creation and
  reset (Windows 7's composition can be turned off while a game runs); the log says which
  (`vsync: frames paced by dd2d9 to the desktop compositor's refresh`, or `vsync: Direct3D
  9's`).
- **Windows' own modules get the real screen from dd2d9's cursor and window hooks.** In a
  mode dd2d9 shows scaled (borderless, windowed), the game asks for and sets the cursor in its
  mode's coordinates, and dd2d9's hooks on its imports translate. dd2d9 never patches Windows'
  modules, but another hooker's patch (Windows' compatibility shims, most likely) sent
  Windows' DirectInput 1–7 calls through those hooks too. Nerf Arena Blast and Unreal
  Tournament (DirectInput mouse, `display=borderless`) turned on their own, the camera
  spinning and Windows' cursor in the middle of the screen: DirectInput reads the mouse by
  putting the cursor at the window's middle and reading where it is. It put it at the game's
  512,384 (683,384 on the real screen), held it where it was, and read it back as the game's
  600,410 at every poll, 88,26 from the middle with the mouse still. A switched display
  translates nothing, and dinputto8 doesn't read the mouse so, which is why both played
  right. dd2d9's 19 mouse and window hooks now pass a call on unchanged when it comes from a
  module dd2d9 doesn't patch (Windows', but for its compatibility shims, which pass the
  game's own calls on: `apphelp.dll`, the `Ac*.dll` shim DLLs, anything in `AppPatch`; other
  wrappers) or from dd2d9 itself; the log names each
  such module once (`mouse: dinput.dll calls dd2d9's cursor and window hooks`). Found with a
  diagnostic build logging every cursor call and its caller (in 1.4.2 too). New ddtest check
  (`mouse`): `wincaller.dll`, copied into the Windows folder, calls dd2d9's `GetCursorPos`
  hook: 1.5.0 before this gave it the game's 320,240, now the real 640,360 (the game still
  gets 320,240). Confirmed in Nerf Arena Blast and Unreal Tournament.
- **A texture drawn with colour keying and then without isn't made again each time.** dd2d9
  makes a colour-keyed texture with the key's texels transparent; drawn without the key it
  needs the texels as they are. N.I.C.E. 2 draws two of its race textures keyed and unkeyed in
  turn several times a frame, and each switch made the texture again (4–8 ms of a frame). The
  texture now keeps both copies and swaps between them; a copy made for an older version of the
  surface's pixels or palette is still made again. New d3dimbench test, "keyed toggle" (256
  quads a frame on one texture, keying on and off in turn, its key colour checked transparent
  with the key and shown without): about 150 ms a frame before, 5–7 ms now, the same picture.

- **dd2d9's hooks and Windows' compatibility shims no longer pass calls to each other
  forever.** Windows runs every exe with "setup" in its name as an installer, as
  administrator and with compatibility shims (`VistaSetup DetectorsAdminProtection
  Installer`); shims patch the imports of every DLL outside the Windows folder, dd2d9's
  among them, and Windows applies them to many old games from its compatibility database
  too. N.I.C.E. 2's `bn_setup.exe` died at start (a stack overflow, with no window and no
  crash logged): asked for `GetProcAddress`, the shim gave dd2d9 its own hook, which dd2d9
  kept as the function its hook passes calls on to; when the setup loaded `d3d9.dll`, the
  shim engine patched the setup's imports again and kept dd2d9's hook as the function *it*
  passes calls on to, and the next `GetProcAddress` call went round the two until the stack
  ran out. A hook still passes calls on to the function it replaced (a shim's, where there
  is one, so the shim keeps working), except when it was called from that function's own
  module, or from dd2d9 itself (a shim that passes the call on with a jump): then to the real
  function, read from its DLL's export table (forwarders followed into DLLs already loaded),
  which a shim can't redirect. Found with diagnostic builds on the player's Windows 11 (a
  watchdog thread and a first-chance exception log); under Wine, which has no shims, the
  setup always opened. New `shimtest`, with a stand-in shim (`fakeshim.dll`) that hooks
  ddraw.dll's imports as it loads and patches the program's again as the shim engine did,
  passing calls on with a jump or with a call: 1.4.2 died in both; a first fix that always
  called the real functions survived, but the shim's own answers no longer reached the
  program; now both pass. The export table reading gives what `GetProcAddress` gives for
  all 6257 exports of kernel32, kernelbase, ntdll, gdi32, user32 and advapi32 under Wine and
  the 9121 of the player's Windows 11 (none differ; 1 and 30 forwarders into DLLs not loaded
  aren't followed). Confirmed on the player's Windows 11: the setup opens (its log goes on
  past where it died, as under Wine).
- **`palettizedtextures=off`: no 8-bit palettized texture format for Direct3D 3–7.** Some
  games choose their textures by the formats offered: N.I.C.E. 2 loads its 256-colour
  textures (`Textures\256col`) when an 8-bit palettized format is listed and its 16-bit ones
  otherwise, which is why players of the time found the game looked better on Matrox's G200
  and G400 (no palettized textures). With `off`, `EnumTextureFormats` (Direct3D 3–7) leaves
  the format out and the device caps drop `D3DPTEXTURECAPS_ALPHAPALETTE`; a game that asks
  for a palettized texture anyway still gets one. Default `on`, as before. New d3d7test check
  (the format listed with `on`; absent, and the cap off, with `off`). Confirmed in N.I.C.E. 2:
  with `off` it loads its 16-bit textures.
- **`filter=sharp` is the default** (it was `point`). A scaled image (borderless, windowed,
  a mode the monitor lacks) keeps its pixels as flat blocks, blended with their neighbours
  only over one screen pixel at their edges, so they come out even at any scale instead of
  some rows and columns doubled; at whole-number scales it looks as `point` did. Shrinking
  (an image larger than the space it gets) is linear, as before. `filter=point` gives the
  old look.
- **README: dinputto8 for games whose DirectInput 1–7 mouse freezes.** N.I.C.E. 2 sometimes
  froze the mouse at start, its own cursor stuck in the bottom-right corner: when its
  `GetDeviceState` on the mouse fails with anything but `DIERR_INPUTLOST`, it uses the
  uninitialized buffer as the mouse's movement and buttons. Not dd2d9's to fix; the
  Troubleshooting section points to dinputto8.

## 1.4.2
Sakura Taisen's text (letters drawn into 16-bit bitmaps of the game's own).

- **A 16-bit bitmap the game makes goes into a memory DC in a 16-bit mode.** Sakura Taisen
  builds each letter of its text as a 16-bit picture of its own, makes it a bitmap
  (`CreateBitmapIndirect`), selects it into a memory DC (`CreateCompatibleDC(NULL)`) and blits
  it into a surface's DC. On XP the screen really was 16-bit, so the bitmap was the screen's
  own format; with dd2d9 the screen stays 32-bit, Windows refuses to select a 16-bit
  device-dependent bitmap into a DC made for it (`SelectObject` fails), and the blit copied
  nothing: every letter came out empty. While the game is in a 16-bit mode, `CreateBitmap`
  and `CreateBitmapIndirect` now make a 16-bit bitmap as a DIB section in the mode's 5:6:5
  format with the game's pixels, which goes into any memory DC and copies to a 16-bit surface
  pixel for pixel. New ddtest check (the game's steps, in a 16-bit mode): the bitmap couldn't
  be selected and none of its 16 pixels arrived before, all of them now (Wine refuses the
  bitmap as Windows does). Confirmed in the game.

## 1.4.1
Alt+Tab out of a fullscreen game stays out (Need for Speed III's menus).

- **A game that takes the foreground as it paints stays in the background.** Need for
  Speed III calls `SetForegroundWindow` in its `WM_PAINT` while fullscreen, and switching away
  paints the window (Alt+Tab's switcher over it): the desktop flashed and the game came back.
  While a covering game has lost the focus, dd2d9 now answers `WM_PAINT` itself (the window
  is invalidated when the game is back, so the game paints it again).
- **A focus change is no longer dropped while the display is busy.** With the display lock
  held by another thread (a DirectDraw call on another of the game's threads, or the topmost
  timer), the focus change (and the cover and display-change checks) was posted to the window
  again at once, up to 500 times, which took about a millisecond; the window's thread
  spinning on them kept that thread from running (`cpus=1`). A millisecond now passes between
  tries, the focus change tried for up to two seconds (and logged if dropped).

## 1.4.0
Direct3D 3-6 games get faster vertex processing and lighting and far fewer draws from
execute buffers, every draw costs a little less, Bejeweled 2's menu is fixed, and colour keys
in blits of YUV video surfaces match only what they name.

- **8-bit textures with an alpha palette are blended by the palette's alpha.** dd2d9
  reports `D3DPTEXTURECAPS_ALPHAPALETTE`, but uploaded every 8-bit texture as opaque: the
  `peFlags` of a palette made with `DDPCAPS_ALPHA` were ignored. Bejeweled 2 (PopCap's
  SexyApp) draws its menu's glow with such textures, blended `SRCALPHA` → `ONE`, so each
  layer was added at full strength and the lower menu washed out to flat pale colours.
  The palette's `peFlags` are now the texels' alpha, and the code that treats a texture as
  having no alpha (automatic mipmaps, the colour-key cutout, the Direct3D 3–6 `MODULATE` alpha
  fix-up) counts these textures as having alpha; a texture given such a palette after it
  was first drawn is made again without automatic mipmaps. New d3d7test check: a texture whose
  palette gives alpha 0x40 on white, blended over black, gives 0xffffff before and about
  0x404040 now. Confirmed in the game.
- **Direct3D 3–6 transforms and lights four vertices at a time (SSE2).** Each vertex goes
  through the same operations in the same order as before, so the output is the same bit
  for bit, but for the specular and spot-light powers (next): checked with the new
  `d3dimbench` on ordinary vertices and on odd ones (NaN, infinities, zero normals), with
  every light type, vertex colours, specular, and linear, EXP and EXP2 fog. The four-lane
  code divides on every lane and picks the answers after, where the scalar code skipped a
  vertex or light: floating-point exceptions are masked while it runs (a game that unmasks
  them doesn't trap), and the game's state put back after. Built with `-ffp-contract=off`,
  so no compiler setting fuses its multiplies and adds differently from the scalar code's.
  `ProcessVertices` in `d3dimbench` (Wine): one directional light 19.9 → 43.3 million
  vertices a second; the lit sphere drawn with `DrawIndexedPrimitive` 289 → 331 frames a
  second.
- **Specular and spot-light powers computed four at a time.** `powf` was most of the time
  of lighting with specular or spot lights; an SSE2 `exp2(y log2 x)` replaces it, within
  1e-5 of it (115 ulps at most, at a power of 128): an 8-bit colour moved by one step in 6
  of 4.2 million lighting terms tried, and none of `d3dimbench`'s 31000 vertices changed.
  Zero, negative, NaN and infinite inputs, and powers too large, still go through `powf`;
  a power of 1 gives the value itself, and the rounding mode doesn't change the result.
  Three lights with specular and fog: 8.3 → 23.4 million vertices a second (12.1 with the
  vector code and `powf`).
- **Execute buffers draw consecutive points, lines or triangles as one draw.** A
  `TRIANGLE` instruction was a Direct3D 9 draw of its own, and some games put one triangle
  in each. Anything else in the buffer (a state, a matrix, `PROCESSVERTICES`, a branch, the
  end) draws what is waiting first, so every primitive is drawn in the same order and with
  the same states. Each instruction keeps the vertices and the clipping it had drawn alone,
  and some choices are made once for a whole draw: the draw is split back into one per
  instruction where joining them would change any of them (an untextured draw's rhw
  flattened, bad rhw replaced, the emulated W buffer's scale, a 2D draw kept out of
  antialiasing, cut-outs' texture addressing clamped). `d3dimbench`: one instruction per
  triangle 16 → 395 frames a second, one per row 581 → 853, cut by the near plane
  34 → 334; the same pixels.
- **The vertex declaration and the textures are set only when they change.** Both were set
  for every draw (all 8 texture stages, most of them empty). GPU blits, the presenter and
  overlays, which set their own, and a device made, reset or presented, or a scene ended,
  make the next draw set them again (overlay tools hooking `Present` or `EndScene` may set
  their own too). Binding a draw's textures under Wine: 0.83 → 0.52 µs.
- **The checks of transformed vertices are one SSE2 pass.** Each draw of transformed
  vertices looked at every vertex twice (whether it is flat 2D, and for a bad rhw or a z
  to clamp): 4 → 1.4 ns a vertex, the same answers (NaN and the zeros' signs included,
  checked on 200000 random draws).
- **YUV colour keys in blits are compared in Y, U and V.** A key on a YUY2, UYVY, YV12 or
  I420 surface was converted to RGB and compared with the decoded pixels; YUV to RGB clamps,
  so video black (Y 16) also matched everything below black (Y under 16, the same RGB
  black). Overlays' keys, and a blit within one surface using both its keys, still compare
  in RGB. New ddtest checks: a YUY2 source key and a YUY2 destination key on black, beside
  a pixel below black, keyed both pixels before and only the black one now; a YV12 source
  key on white beside a pixel above white keyed both before and only the white one now,
  and one with different U and V keys only its own block.
- **The 16-bit gamma table is built outside the gamma lock, and three times faster.** A
  game setting its gamma ramp every frame (a fade) waited for the presenter's 65536
  conversions; the table is now made from a table per channel (each colour channel depends
  on its own bits only): 319 → 99 µs per ramp, the same table (checked for eight 16-bit
  layouts and three ramps).
- **Undefined behaviour removed** (clang's undefined-behaviour sanitizer over every test
  suite): a NaN colour or fog value converted to an integer now gives, explicitly, what x86
  gives (the output is unchanged, but no longer left to the compiler), and the walk of the
  loader's module list no longer takes a member of a null pointer (`CONTAINING_RECORD`). The
  sanitizer finds nothing else in ddtest, `ddtest fuzz`, d3d7test or d3dimbench.
- **Tidying:** three functions nothing called are gone; the README says dd2d9 needs Windows
  Vista or later (it uses Vista's locks and timers); `tests/wine/build.sh` builds Wine's
  tests with GCC 14 and later, which turned two of their warnings into errors.
- **Tests:** `d3dimbench` (new) checks and times Direct3D 3–6's vertex pipeline and
  execute buffers by hashes of their output, and checks that consecutive `TRIANGLE`
  instructions draw the same pixels as the same instructions drawn one by one (a near and
  a far untextured triangle; two triangles with unused vertices of rhw 0 between them). `ddtest fuzz` falls back to single colour
  keys when a key range is refused (dd2d9, as `ddraw.dll`'s software blitter, takes no
  ranges): it compared its blits against ranges that were never set, and failed on 1.3.1
  too.

## 1.3.1
Three fixes from players, confirmed in the games (Pal95's dialogue text, a movie cut in
half after Alt+Tab, movies played in the game's own child window), a call tracer for
developers, then a line-by-line review of the whole code: the bugs it found, and faster
software blits (the same pixels as before, checked over 6000 random blits).

- **Text drawn with GDI on an 8-bit surface follows the palette on Windows.** Pal95 draws
  its dialogue into a 320x20 strip: filled with index 255 (transparent), a black shadow,
  palette entry 1 set to white, the text in white. On Windows the strip's DC (from
  `D3DKMTCreateDCFromMemory`) kept the colour table it was made with whatever
  `SetDIBColorTable` said, so white was drawn as 255 and only the shadow showed. 8-bit
  surfaces now get their DC through a DIB copy, whose table does follow (`paltext`, run on
  Windows 11 with Windows' `ddraw.dll` and with dd2d9).
- **Back from Alt+Tab, the game sees its own screen while its window comes back.** Windows
  restores the window first, sending the game `WM_SIZE` and `WM_ACTIVATE`; dd2d9 still had
  its setup marked as away then, so `GetClientRect` and `GetSystemMetrics` answered with
  the monitor's size. Heroes of the Three Kingdoms 3 read it there and blitted its movie
  centred in a 1920x1080 screen from then on: the 640x480 picture showed its right part
  enlarged beside the last good frame. The image counts as shown from the start of the
  setup.
- **Movies opened by command string in the player's own child window are scaled.** Rockman X
  Math opens its movies with `open ... parent <window> style child`, asks for the player's
  window (`status ... window handle`) and centres it with `MoveWindow` in its 640x480: the
  movie played at its size near the corner of the screen, and the screen's refresh flickered
  over it. Such a device is now handled as one opened through the command interface
  (`MCI_DGV_OPEN_PARENT`, Road Rash): the game moving its window (`MoveWindow`,
  `SetWindowPos`) is scaled, the picture placed in it, and the window counts as showing a
  movie (not refreshed over).
- **New developer tool: ddtrace** (`tools/ddtrace`), a separate `ddraw.dll` that logs every
  DirectDraw and Direct3D (3–7) call a game makes and passes it on to Windows' own
  `ddraw.dll`, dd2d9 or another wrapper, so two runs can be compared call for call. It found
  that a KOF 99 missing its health bars and timer was a damaged copy of the game, not dd2d9.
  Not part of dd2d9's own `ddraw.dll`.
- A DC out on a locked surface: writes made through the lock meanwhile are kept (only what
  GDI drew is copied back from the DIB copy; the older copy overwrote them).
- Surfaces: a surface referenced again by another thread while its last `Release` waited for
  the lock is kept; `Lock`'s fast path for textures goes by the DirectDraw object's kind
  (made by `DirectDrawCreateEx`), not the interface; `Flip` checks `DDFLIP_STEREO` after a
  lost surface, as ddraw.dll; a client-memory surface's size and pitch are checked in 64
  bits; `DuplicateSurface` clears its result after the lost check.
- Palette versions come from the counter buffers use, so a palette made where a freed one
  was can't match what was cached of the old one.
- An overlay locked or with its DC out while shown isn't cached as finished (it was decoded
  half written, and not again until the game next touched it).
- `SetCooperativeLevel` with `DDSCL_CREATEDEVICEWINDOW` twice keeps the first device
  window; leaving exclusive mode lets the cursor go with DirectDraw 1 too; a
  `SetDisplayMode` or `RestoreDisplayMode` while Alt+Tabbed away is noted and set up on the
  way back (the display was switched with the device window topmost over the desktop);
  a `WM_DISPLAYCHANGE` that arrives while another thread holds the display lock is checked
  again shortly after, not dropped.
- Direct3D: emulated W buffering takes its scale once per draw from the nearest vertex (a
  triangle's corners could get different scales, and an infinite rhw zeroed it for the
  rest of the frame); a clear with one rectangle over the whole target counts as whole (the
  W scale restarts, no upload of what it covers); a failed vertex declaration isn't
  released; Direct3D 2's device holds and releases its Z buffer through the same interface
  after `SetRenderTarget` changes the target's; transformed indexed draws (Direct3D 3–6)
  with indices past the vertices use vertex 0, as Direct3D 7 does; a viewport whose
  position plus size wraps 32 bits is refused.
- Hooks: a game DLL unloaded and loaded again is patched again; modules are kept loaded
  while patched; `ChangeDisplaySettings` isn't handed a copy read past a short `DEVMODE`.
  The crash logger allocates nothing (it runs with the heap perhaps damaged) and does
  nothing with `log=0`. A thread's sleep timer is closed with the thread.
- Faster: a sprite from a DXT texture atlas decodes only its own blocks (15 to 180–380
  Mpix/s, 32x32 from 1024x1024), YUY2 and YV12 only the columns blitted; converting blits
  (16↔32-bit, YUY2 into 16-bit) a row at a time with SSE2 (3–4 times faster); stretched or
  mirrored keyed sprites, key ranges and destination keys with SSE2 (10–40% faster);
  24-bit fills four pixels at a time; `GetDisplayMode` asks for the refresh rate at most
  twice a second.
- Tested: ddtest 365/365 (new checks for the two movie fixes and the locked-surface
  DC, each failing before it, and for the device window), ddtest audit, d3d7test, Wine's ddraw
  tests: no regressions. The three player fixes confirmed in the games.

## 1.3.0
Borderless and window mode, made to match what a game expects of a real mode switch. Fixes
from Hydro Thunder, then an audit of the ways a game can take or check fullscreen (new
`ddtest audit`, run with `display=borderless` and with `display=window`), then fixes from
players' logs of Road Rash, Rockman X5, Sango 4, KOF 99, Daytona USA, Pal95 and Richman 4,
and DirectShow's Video Mixing Renderer 7 (Romance of the Three Kingdoms VII's intro movie), and a minimized game that couldn't be
brought back (Pal95).
(Prepared as 1.2.3, never released: all of it is in 1.3.0.)

- **`display=borderless` covers the screen for games that ask for their window to be left
  alone.** Hydro Thunder takes exclusive mode with `DDSCL_NOWINDOWCHANGES` (0x55) and a
  640x480 window: `ddraw.dll` leaves the window alone and the switched 640x480 mode fills
  the screen with it, but borderless switches nothing, so the game sat in the corner of
  the desktop. With borderless the window now covers the screen anyway (and is restacked,
  minimized on Alt+Tab and put back on leaving exclusive mode as any covering window is);
  `display=switch` and `window` still leave such a window alone, as `ddraw.dll` does.
- **A game sizing its window back to its mode keeps covering the screen at once.** With
  borderless, a window the game sizes to cover its own screen (at the monitor's corner, at
  least the mode's size) is given the covering size before it changes
  (`WM_WINDOWPOSCHANGING`). dd2d9 covered it again afterwards before, from a posted
  message, which a game that doesn't read its messages while it loads sees late: Hydro
  Thunder sizes its window to 640x480 right after `SetDisplayMode` and showed in the
  corner for its first two seconds. Also when the game sets its style and resizes with a
  frame change (`SWP_FRAMECHANGED`): the covering size is worked out for the new frame
  there and then. A smaller window (the game going windowed) stays the game's. A resize
  that comes while another thread has the display is logged (left to the later check).
- **Every way of asking for the screen's size gives the game's mode** in borderless and
  window mode, as after a real switch: besides `GetSystemMetrics` and `GetDeviceCaps`
  (already so), the desktop window's rectangle, `GetMonitorInfo` (the monitor and its work
  area) and `EnumDisplaySettings`' current mode. A game checking that its mode took, or
  sizing or centering its window from the monitor, saw the real desktop. A
  `ChangeDisplaySettings` to the mode the game is shown in does nothing (a game putting
  back the "current" mode it read would have switched the display under the scaled
  picture). The display-settings imports are therefore always patched, not only with
  `desktopbpp`.
- **`mouse=` is gone**: the game always sees its mode's coordinates when it is shown scaled
  or in a window. `mouse=raw` only put clicks in the wrong place (and would now put the
  screen's size at odds with the mouse's); an ini that still has it gets a note in the log.
- **A game repainting its window gets the area in its mode's coordinates.** Road Rash draws
  its menus on `WM_PAINT` by blitting the area to repaint from its 640x480 buffer to the
  primary; with the window covering a 1920x1080 screen it was asked for 0,0-1920,480, an
  invalid rectangle, and its menus never showed. The area (`BeginPaint`, `GetUpdateRect`,
  `GetUpdateRgn`) is now where the image is, mapped to the mode's coordinates, and an area
  the game invalidates (`InvalidateRect`) is mapped back there.
- **Movies played through the video player's command interface are scaled too.** Road Rash
  makes a topmost 640x480 popup at 0,0 (filling its switched screen), opens its movies with
  `mciSendCommand` in a child window of it (`MCI_DGV_OPEN_PARENT`) and puts the picture at
  twice its size (`MCI_PUT`): it played 640x480 in the corner of the screen. A popup the
  game makes within its screen is now put over that part of the image, scaled, and the
  player's window goes where the picture does, scaled, the picture filling it. Where a
  popup without a background of its own isn't painted it is black, as the screen the game
  blacked under it was (the menu showed through above and below the race intros). A movie played without a destination gets its natural size there, and one drawing
  straight into the game's window (`MCI_WINDOW`) is placed as the image is.
- **`GetAttachedSurface` with leftovers in the extra caps words.** Rockman X5 asks its
  primary for the back buffer through `IDirectDrawSurface4` with `DDSCAPS_BACKBUFFER` in
  `dwCaps` and the other three words of its `DDSCAPS2` not cleared: no surface had those
  bits, and it stopped at start with "GetAttachSurface error". When the other words can't be
  meant (`dwCaps3` or `dwCaps4` set, both reserved in DirectX 7, or `dwCaps2` bits DirectX 7
  doesn't define) and nothing matches all four, the first surface matching `dwCaps` is
  returned, once logged. A real `dwCaps2` that matches nothing (a missing cube map face)
  still gets `DDERR_NOTFOUND`. The trace now shows all four words.
- **The game's window hears of its mode's size.** With a real switch, Windows sizes the
  fullscreen window to the new mode and the game gets `WM_SIZE`; a window already covering
  the monitor doesn't change, so it heard nothing (or only the desktop's size, from taking
  exclusive mode before setting its mode). Sango 4 blits its 800x600 frame to the size it
  was last told: 0,0-1920,1080 of its 800x600 primary, its top left corner enlarged. When
  the game's mode is shown and the window was last told another size, dd2d9 tells it the
  mode's.
- **A second DirectDraw object sees the display in the game's mode.** Another object of the
  process (DirectShow's, made as KOF 99's intro movie starts) was told the desktop's
  1920x1080x32 by `GetDisplayMode` (and made its surfaces in that format) while the game's
  object had the display in 640x480x16, as a real switch would have shown it. Now the
  exclusive object's mode is the display's for every object. (This was taken for the reason
  KOF 99's intro stayed black; that was the game's critical section, fixed in 1.6.0.)
- **The game's heap memory comes zeroed**, as fresh memory did on Windows 9x. Daytona USA
  Deluxe allocates its texture table with `HeapAlloc` and, before clearing it, releases
  every texture it finds there: today's heap hands back blocks freed with their old
  contents (Direct3D's and the driver's, as its 3D device comes up), and it called through
  a stale pointer and crashed at once. The game's `HeapAlloc` now always zeroes.
- **Text drawn with the display's palette follows palette changes.** A device context out
  on an 8-bit surface without a palette of its own took the display's colours when it was
  made, and kept them: Pal95 blanks its text strip to colour 255 (its transparent one), sets
  entry 1 to white and draws its text white with the DC out; matched against the old
  colours, white became 255 and its letters lost strokes. Such a DC now follows changes to
  the palette shown, as GDI on a palettized display did (a surface's own palette still
  doesn't change its DC: Wine's `test_palette_gdi`).
- **"play ... window" stays in the game's window.** Richman 4 plays its movies with
  `play vfw window from 0 notify` after giving the player its window: the old player played
  there all the same, Windows 11's opened a player window of its own with its controls, the
  movie at its size in the corner. For a device drawing in the game's window, the word is
  left out (logged).
- **Movies through DirectShow's Video Mixing Renderer 7 show.** Romance of the Three
  Kingdoms VII plays its intro (WMV in an AVI) with Windows' default video renderer in
  windowless mode: the screen stayed black while the decoder ran through every frame and
  the game waited out the movie's length. dd2d9 described YV12 surfaces without their bits
  per pixel (`dwYUVBitCount` 0); the renderer sizes its frames by that and showed none. YUV
  pixel formats now carry it (16 for YUY2 and UYVY, 12 for YV12, I420 and IYUV), as drivers
  report it. Reproduced with XP's `quartz.dll` under Wine: no frame drawn before, every
  frame after.
- **A window shared by two DirectDraw objects shows the newer frame.** Objects in normal mode
  each have a primary of their own in dd2d9, where on Windows they share the one screen: the
  video renderer's beside the game's, which stays alive under the movie. Each re-presents
  its last frame four times a second, so the game's last screen flashed over the video. A
  window now shows the object that last drew a changed frame into it; the others' unchanged
  frames aren't presented over it (until it draws nothing any more).
- **A game minimized on Alt+Tab can be brought back.** dd2d9 minimizes the exclusive window
  on focus loss, as `ddraw.dll` does. Pal95's window (Visual Basic 4), unowned and of no
  special style, had no taskbar button: Windows minimized it onto the desktop, a small title
  bar in the corner behind the other windows, with no way back by the taskbar or Alt+Tab.
  A window Windows minimizes onto the desktop (where it puts one it has no button for; with
  a button, off the screen) is now hidden and shown minimized again with
  `WS_EX_APPWINDOW`, which gives it the button; so is a tool window (`WS_EX_TOOLWINDOW`,
  `WS_EX_NOACTIVATE`). Its own style comes back with the focus. A window with an owner
  (whose button is the owner's) has the owner minimized instead, hidden with it and shown
  again with it; with an owner that isn't visible it is left as it is.
- **`DirectDrawEnumerateEx` lists each display.** With `DDENUM_ATTACHEDSECONDARYDEVICES`,
  `ddraw.dll` lists every display on the desktop after the primary display driver, by GUID,
  `\\.\DISPLAYn` name and monitor, even with one monitor (Wine does too); dd2d9 listed
  the primary display driver alone. The video renderer builds its list of monitors from it.
  The GUIDs are the ones `GetDeviceIdentifier` reports, and `DirectDrawCreate` takes them.
- Known, not changed: a game that stays in normal mode and calls `SetDisplayMode`,
  expecting its window to fill the screen, shows in the corner with borderless (no game is
  known to do it; `ddtest audit` records it).
- Tested: ddtest 360/360 (new checks for each fix, each failing before it), `ddtest audit`
  10/10 with `display=borderless` and with `display=window`, d3d7test 40/40 (16 and 32
  bpp; default settings, windowed, borderless, antialiasing, render32), Wine's 46
  display-mode, cooperative-level and window-style sub-tests in all interface versions with
  no regressions. In the game: Hydro Thunder filling the screen confirmed by the owner (with
  the first fix alone; its first two seconds and the rest not yet). Road Rash's movies and
  menus, Romance of the Three Kingdoms VII's intro (no flicker) and Pal95's Alt+Tab
  confirmed by players. The Rockman X5, Sango 4, KOF 99, Daytona USA, Pal95 (text) and
  Richman 4 fixes come from players' logs (and the Daytona USA, Pal95 and Richman 4
  executables), each reproduced with a ddtest check of the same calls; not yet confirmed in
  the games.

## 1.2.2
- **Movies in the video player's window control (MCIWnd) are scaled with the game too.**
  Rockman X4 plays its movies in an MCIWnd window (msvfw32's `MCIWndCreate`), a child of
  its own window that it moves and sizes (`MoveWindow`, `SetWindowPos`) and places the
  picture in (`MCIWNDM_PUT_DEST`) in its mode's coordinates: the movie played at its size
  in the corner of the screen, as Street Fighter Zero's did before 1.2.1. Those calls are
  now translated for an MCIWnd window in the window the game is shown in, scaled as the
  image is, and the game's unchanged frame isn't presented over one while it shows. New
  ddtest checks.
- **`display=borderless` covers the primary monitor**, as `display=switch` (and
  `ddraw.dll`'s fullscreen) does, instead of the one Windows had put the game's window on:
  Rockman X4 came up on either monitor from one start to the next. `display=window` stays
  where the window is.
- Tested: ddtest 321/321, d3d7test 40/40, Wine's ddraw tests with no regressions; Rockman
  X4's movies and monitor confirmed in the game by the user who reported them.

## 1.2.1
- **Videos that Windows' player draws into the game's window are scaled with the game.**
  Games that play their movies through MCI (`mciSendString`: "window ... handle", "put ...
  destination at x y w h") place the picture in their window's coordinates as they see
  them, their mode's; in a mode shown scaled (`display=borderless` or `window`,
  `mouse=scale`) the player drew it at those pixels of the real window: Street Fighter
  Zero's opening movie played at its size in the corner of the screen. The game's imports
  of `mciSendStringA/W` now put the destination where the image is, scaled and moved as
  the image is; a video shown in that window without a destination gets its natural size
  there, scaled too; and while one plays, an unchanged frame isn't presented again over
  it. (`mciSendCommand`'s `MCI_PUT` isn't translated.) New ddtest checks (a small AVI
  played into the window).
- The Direct3D design notes said cube maps were still to do; they have been in since 1.1.0.
- Tested: ddtest 319/319, d3d7test 40/40, Wine's ddraw tests with no regressions; Street
  Fighter Zero's movie confirmed in the game by the user who reported it.

## 1.2.0
The optional items left after 1.1.0:
- **Overlays in windowed programs.** 1.1.0 offered overlays to fullscreen programs only: a
  windowed one paints the destination colour key into its window with GDI, on the desktop
  dd2d9's primary doesn't hold, so its overlay stayed hidden (the DirectX 7 SDK's DDOverlay
  sample showed nothing). Now the presenter also looks for the key in what the window draws
  (`WM_PRINT`, ten times a second at most, waiting 50 ms at most for an answer), and a key
  that shows nowhere under the overlay doesn't hide it. DDOverlay shows its picture; Wine's
  `test_overlay_rect` (where its DWM check lets it run), `test_offscreen_overlay` and
  `test_yv12_overlay` run and pass. Only the part of an overlay over the program's own
  window can be seen (the SDK's Mosquito puts its overlay beside its window). New ddtest
  check: a window half key colour, half black.
- **YV12 surfaces** (and I420/IYUV): planar 4:2:0 video, offscreen or overlay, in
  `GetFourCCCodes`. Locked a luma byte per pixel (pitch 256-byte aligned, as overlay
  hardware had it), blitted to and from any surface through 32-bit colour, copied plane by
  plane between two of one format, laid over the screen as overlays. Not colour-filled, not
  textures. New ddtest checks.
- **Render-target textures copied on the GPU when mipmapped or colour-keyed** (until now
  read back through the CPU, which waits for the GPU): the levels Direct3D drew into copied
  there, the others uploaded from their CPU copies; a colour key made transparent black by a
  pixel shader, as the CPU path does it. Palettized ones still go through the CPU. New
  d3d7test checks.
- **The `DDSCL_CREATEDEVICEWINDOW` window** covers the screen in exclusive mode and shows the
  picture, as `ddraw.dll`'s did (it was kept hidden, the picture in the focus window, so a
  game that left that window small or hidden showed nothing). Its mouse messages go on to the
  focus window (double-clicks as plain clicks where that window takes no double-clicks). It
  hides while the game is in the background. New ddtest check (with `DDSCL_NOWINDOWCHANGES`).
- **Direct3D 7's `GetClipStatus`** takes vertex blending and the user clip planes into
  account (`D3DCLIP_GEN0`-`5`); **Direct3D 3–6's `Pick`** finds a triangle the clip volume
  cuts in its visible part. New d3d7test and ddtest checks.
- **Bump maps** (`DDPF_BUMPDUDV`: U8V8, U5V5L6, U8V8L8), found missing by the DirectX 7 SDK's
  BumpEarth: the caps said the GPU does environment-mapped bump mapping (D3D9's
  `D3DTEXOPCAPS_BUMPENVMAP`), but no bump format was listed or could be made, so the sample
  stopped with an error. Now `EnumTextureFormats` lists those the GPU has (Direct3D 6 and 7
  hardware devices), they are made and uploaded as D3D9's V8U8, L6V5U5 and X8L8V8U8 (U8V8L8
  widened), and BumpEarth draws as with Wine's own ddraw. Blits between a bump map and a
  colour surface are refused (they only copy as they are). New d3d7test check.
- **Overlays laid over the picture on the GPU** (a pixel shader with the destination and
  source colour keys) instead of on the CPU: until now a visible overlay made the presenter
  convert the whole primary every frame and read Direct3D's drawing back from the GPU to lay
  the overlay over it. Now only an overlay that changed is decoded and uploaded; the rest
  stays on the GPU. The CPU path stays for a GPU without pixel shaders.
- **Blits from a render target on the GPU**: a plain copy (`Blt` without effects, colour
  keys or a raster operation other than `SRCCOPY`) from a surface Direct3D drew into, to the
  primary, a back buffer or another surface on the GPU of one format, is made there with
  `StretchRect` (stretched too, when unclipped) instead of reading the drawing back to the
  CPU first. Windowed Direct3D games that blit their back buffer to the primary (the SDK's
  framework does) no longer wait for the GPU every frame. New d3d7test check.
- **SSE2** for YUY2/UYVY and YV12/I420 decoding (eight pixels at a time, the same results as
  before to the bit) and a faster U8V8L8 bump-map upload.
- **A review of the whole code** for speed and mistakes found:
  - *Faster software blits:* a blit from or to a DXT/YUV surface decodes only the rows it
    uses into memory kept from blit to blit (it decoded the whole surface into a newly
    allocated one, and for a source without alpha copied and compared all of it after: a
    32x32 sprite out of a 1024x1024 DXT atlas decoded a million pixels); blits between
    formats convert whole rows with SSE2 and pack through tables (they went through two
    out-of-line conversions per pixel); a stretched blit copies a repeated row instead of
    making it again; a plain copy of any depth, overlapping scrolls included, goes row by
    row with `memmove` (24-bit ones went pixel by pixel, overlapping ones through a copy of
    the whole source); 24-bit primaries and textures convert four pixels at a time.
    `ddbench blit` (new cases for these) against 1.1's blitter: a YUY2 frame stretched to
    640x480 2.8-7.5x faster, 16/32-bit conversion 3.4-3.7x, a scroll 2.3-4.5x, a 2x stretch
    1.2-1.4x, a sprite out of a DXT atlas about 40x; the rest as fast as before or faster.
  - *Fewer Direct3D 9 calls:* Direct3D 3-6 send stage 0 and 1's texture operations and
    the alpha test only when they change (up to 17 calls at every draw before), Direct3D 7
    the alpha test only when it changes (at every stage-0 texture change before); strided
    vertices are gathered into memory kept from draw to draw.
  - *Fixed:* a windowed overlay with a destination colour key didn't show under a gamma
    ramp (the key was compared with the frame after the ramp); the GPU overlay path could
    use a released texture when the device was reset while an overlay appeared; an 8-bit
    overlay's palette changes didn't reach the GPU path; a windowed render target blitted
    to the primary while the window was partly off the screen was read back every frame
    (it now goes through the CPU as before); no multisampled copy of the primary is made
    for it under `antialiasing`; a YUY2/YV12 blit between a surface and its
    `DuplicateSurface` copy that overlapped copied rows in the wrong order; a duplicate
    counted its memory twice; a palette released too often left a surface that outlived
    its flip chain pointing at it; Direct3D 7's `ProcessVertices` without clipping left
    the clip codes of an earlier clipping one in the destination (those vertices then
    drawn as clip-space ones); `SetViewport` with sizes that wrap around 4 GB was taken;
    indexed draws with indices past the vertices given read past them (now vertex 0, as
    the vertex buffer path did); a huge vertex count could end the game with an uncaught
    out-of-memory exception; Direct3D 3-6's textured viewport background turned texture
    wrapping off; `SetExecuteData` vertex counts and offsets near 4 GB weren't refused;
    matrix handles weren't freed with their device; a vertex buffer made with
    `D3DDP_DONOTCLIP` was still clipped when drawn; with `desktopbpp`, a game putting back
    the "current" mode it read switched the real desktop to that depth (or failed); an ini
    value that is only a comment (`logfile= ; ...`) was taken as the value.
  - *Startup:* the compatibility hooks walk each module's imports once (they were walked up
    to five times at every DirectDraw object a game created).
- **Overlay and capture tools** (RivaTuner Statistics Server, MSI Afterburner) show and
  record dd2d9's frames: they went out through an additional swap chain on the game's
  window, which those tools don't hook. Now the device's own implicit back buffer (as large
  as the largest monitor) holds the frame, `IDirect3DDevice9::Present` shows it in the
  game's window, the device is made on that window, and every frame has a draw call (one
  that draws nothing): RivaTuner draws only into frames that had one, and a 2D game's are
  copied into the back buffer. A window larger than every monitor still gets a swap chain.
- **Found in games after the review** (with users' logs):
  - *3DMark 2000:* the benchmark looked like 30 frames a second however fast it ran, and its
    logo's fade showed few steps: with `cpus=1` the presenter thread shared the one processor
    with a game drawing without waiting for the vertical blank, at equal priority, and only
    ran at the end of its time slices. It now runs above the game's threads. The splash
    showed nothing: it calls `LockWindowUpdate` on its full-screen window (which hid all of
    dd2d9's picture in it, where `ddraw.dll` drew on the screen itself); that lock is no
    longer done on the window dd2d9 shows the primary in.
  - *FlyingSB:* grey instead of its colours: it sets up its palette only when the screen is
    a palette device, so in an 8-bit mode of the game's own `GetDeviceCaps` now answers as XP
    did after the switch (8 bits, `RC_PALETTE`, 256 entries, 20 reserved), and in any mode of
    its own that mode's depth and size; and the display's palette is kept from one primary
    to the next (a primary made without a palette shows the colours the last one left, as
    the hardware palette did). Back from Alt+Tab nothing showed: it goes exclusive again
    while still minimized, and the window region was made from the icon's rectangle.
  - *Need for Speed: High Stakes* quit after Alt+Tab: it never restores its flip chain, and
    every Flip failed. With `display=borderless` or `window`, surfaces are no longer lost
    once the game is back in front (the display never left its mode).
  - *Two monitors with `display=switch`:* the game's window went to the monitor it was on
    and the first one changed mode; now both are the primary monitor's, as with
    `ddraw.dll`.
  - *Xuan-Yuan Sword 3 EX* crashed: it writes a few rows past the end of its back buffer
    (surfaces now have slack around their memory); back from Alt+Tab it was told its window
    was the monitor's size by `WM_SIZE` and made its buffers that size (its window's size and
    move messages now carry the mode's size, as `GetClientRect` already did); and it draws its
    cursor where `GetCursorPos` says, unclipped, which went past 640x480 while its window was
    exactly the mode's size (the cursor is now kept on the game's screen then too; with
    `mouse=raw` the game still crashes once the cursor leaves that area).
- Tested against the DirectX 7 SDK's samples (Direct3D and DirectDraw), each compared with
  the same sample on Wine's own ddraw: all draw what the reference draws (the fullscreen
  DirectDraw ones, whose mode switch Wine's ddraw can't make on the test display, draw with
  dd2d9 only).

## 1.1.0
The release after 1.0.1: 1.0.2, 1.0.3 and 1.0.4 (below), and the fixes that testing them in
games brought.

Since 1.0.1, in short:
- **Wine's ddraw test suite:** failed checks down from 463 to 162; 386 of its 423 sub-tests
  clean. The rest are deliberate differences or the test machine (tests/wine/README.md).
- **Direct3D 7 as Windows draws it:** ProcessVertices lighting (normals, vertex blending,
  specular power), fog with equal start and end and exponential vertex fog, `Load` of mip
  levels.
- **Z buffers DirectDraw can lock and blit**, where the GPU has the INTZ depth format and
  antialiasing is off (new `zaccess=off` turns it off); a small lock reads back only its
  rectangle.
- **FourCC surfaces:** DXT1–5 in the game's memory, and blits to and from DXT, YUY2 and
  UYVY surfaces. `GetFourCCCodes` lists them.
- **`DDSCL_CREATEDEVICEWINDOW`** makes `ddraw.dll`'s device window.
- **Direct3D 3–6** caches its list of active lights; `DDCREATE_EMULATIONONLY` objects make
  system-memory surfaces.

New in 1.1.0:
- **antialias2d=off: 2D draws that write depth are antialiased with the rest.** That path
  draws 2D without antialiasing into a surface with no Z buffer, so a draw there wrote no
  depth. NFS Porsche Unleashed draws its menu labels with the depth test passing always
  but writing depth, then translucent button panels behind them: the panels covered the
  labels, which came out dim. The published 1.0.1 already kept such draws out of that path
  (a fix made in review); the game showed why, and a draw with no Z buffer attached now
  takes the 2D path too. New d3d7test check.
- **In the background after Alt+Tab, back buffers can't be locked**, as with `ddraw.dll`
  (the primary and offscreen surfaces still can: Captain Claw, 0.7.19). NFS Porsche
  Unleashed stopped with "D3D pageflip called in while locked" when Alt+Tabbed or the
  Windows key was pressed. New ddtest check.
- **`zaccess=off`** (new option): Z buffers on the GPU only, as before 1.0.3, in case a
  driver misbehaves with the INTZ depth textures that let DirectDraw read and write them.
- `drawtrace=on` also shows the fog states, marks the draws taken as 2D without
  antialiasing (`FLAT2D`), and logs the copies between the antialiased and resolved images.
- New d3d7test check: a back buffer locked after every flip finds the frame drawn into it.

The last of what the README listed as not implemented:
- **Overlays in exclusive mode.** Overlay surfaces (video memory; RGB, YUY2 or UYVY) are
  made, and the presenter lays the visible ones over the primary: stretched or shrunk,
  mirrored, behind a destination colour key and with a source key, in their z-order.
  Windowed programs still get no overlay hardware: they paint the destination key into
  their window with GDI, which dd2d9's primary doesn't see (the DirectX 7 SDK's DDOverlay
  sample showed nothing), and overlays outside their window couldn't be shown. `UpdateOverlay`,
  `Get`/`SetOverlayPosition`, `UpdateOverlayZOrder`, `EnumOverlayZOrders`, overlay keys in
  `SetColorKey` and overlay flip chains (`DDFLIP_EVEN`/`ODD`) follow `ddraw.dll`'s checks;
  the caps report the overlay hardware. New `videooverlay=off` reports none, as before.
  Wine's overlay tests run windowed, so they still skip. New `ddtest overlay` (exclusive,
  read back from the screen).
- **Cube maps** in Direct3D 7: D3D9 cube textures, each face with its mip levels, faces
  Direct3D drew into read back first; `Load` copies the faces it is told to. Caps say
  `D3DPTEXTURECAPS_CUBEMAP` where the GPU has it. Wine's `test_device_load` passes all its
  checks, and the DirectX 7 SDK's EnvCube sample (a cube map drawn into every frame) looks
  as with Wine's own ddraw. New d3d7test check.
- **Blits that convert to an 8-bit surface** pick the nearest palette entry (they wrote
  index 0).
- **Colour keys on FourCC surfaces** (YUY2, UYVY, DXT) work in blits: compared as the
  colour they decode to.
- **Direct3D 3–6 `Pick`** finds the triangles under the point, with their offset and
  depth, as XP's driver path did (`GetPickRecords`); **`D3DOP_SPAN`** is drawn (as points)
  where XP drew it: unclipped execution, or after a `PROCESSVERTICES` copy.
- **Ramp devices light in the mono model**: the lights' brightness shades the material's
  colours, as XP's Ramp rasterizer did.
- **Direct3D 7 `GetClipStatus`** follows what is drawn: the clip codes of untransformed
  vertices, and their extents with `D3DRENDERSTATE_EXTENTS` (from the first call on, as it
  costs a transform of each vertex). New d3d7test check.
- New ddtest checks for the 8-bit conversion, FourCC keys, Pick and spans.

## 1.0.4
- **Small Z buffer locks read back only their rectangle.** 1.0.3 copied the whole Z buffer
  back for every lock: an 8x8 lock each frame (a lens flare's occlusion test) cost as
  much as a whole one. A lock of at most 64x64 pixels now copies just that part through a
  small target, the rest of the CPU copy staying behind until something reads it (what
  DirectDraw wrote into a part goes up to the GPU first). New `ddbench zlock`: under Wine
  (software rendering) an 8x8 lock went from about 1.7 to 0.6 ms a frame at 640x480 and
  from 4 to 0.6 ms at 1024x768; the rest is waiting for the GPU, which a read needs. New
  d3d7test check: Z read and written in small and whole locks, and drawn against.
- **YUY2 and UYVY surfaces blit** to and from any surface (a video player decoding into
  an offscreen surface and blitting it to the screen, stretched or not): converted
  through 32-bit colour with BT.601 video levels, the chroma of a written pixel pair
  averaged. A plain copy between two surfaces of one YUV format is copied as it is. A
  colour fill of a YUV surface writes the value as it is (Wine's `test_color_fill`).
  `GetFourCCCodes` now lists YUY2, UYVY and DXT1–5 and the caps say `DDCAPS_BLTFOURCC`, as
  a driver that blits them does (until now none were listed, so a game that checks first
  never used them). New ddtest checks. Wine's `test_rt_caps` now runs its YUY2 flip-chain
  case, which meets the known difference in SetRenderTarget's codes (XP's
  `DDERR_INVALIDPARAMS`, newer Windows' `DDERR_INVALIDCAPS`): 2 more expected failures,
  162 in all.

## 1.0.3
The three missing features behind Wine test failures (failed checks down from 367 to 160;
37 of 423 sub-tests left, each a difference on purpose, a feature still missing or the test
machine: tests/wine/README.md):
- **DXT1–5 surfaces in game memory** (`DDSD_LPSURFACE`): taken by their linear size, which
  must hold every block (a pitch is refused; IDirectDraw4 takes a pitch flag alone), and
  `GetSurfaceDesc` reports a linear size of ~0 for them, as `ddraw.dll`
  (`test_block_formats_creation`, ddraw4/7).
- **Blits from, to and between DXT surfaces:** each compressed side is decoded to A8R8G8B8
  for the blit (stretching, keys and mirroring as any blit), and the destination's blocks
  that the rectangle touches are encoded again (new `src/dxt.cpp`). As Windows' drivers
  encode: the two colours farthest apart are a block's endpoints (two colours come back
  exactly), and a source without alpha is opaque. Fills into them still fail with
  `DDERR_UNSUPPORTED` (`test_compressed_surface_stretch`,
  `test_surface_format_conversion_alpha`'s DXT cases, which used to be skipped).
- **Z buffers readable and writable by DirectDraw:** where the GPU samples `INTZ` depth
  textures (nearly all, DXVK too) and antialiasing is off, a Z buffer is made as one. Lock
  and Blt read it back by drawing it into an R32F target, and what DirectDraw writes goes
  up by drawing it with a shader writing depth, before the next draw or clear that needs
  it (a whole-target clear skips the upload). Depth fills stay on the GPU as before
  (`test_depth_blit`, `test_depth_readback`, ddraw2/4/7).
- **`DDSCL_CREATEDEVICEWINDOW`** makes `ddraw.dll`'s "DirectDrawDeviceWnd" window, owned
  by the focus window, handing it the keyboard focus. It stays hidden (the picture still
  goes to the focus window) and is destroyed by the next call with another window or the
  release. SetCooperativeLevel's rules about the focus window follow `ddraw.dll`'s
  window-and-focus bookkeeping: `SetCooperativeLevel(NULL, DDSCL_NORMAL)` forgets the
  focus window for `DDERR_NOFOCUSWINDOW`, and `DDSCL_SETFOCUSWINDOW` with
  `DDSCL_CREATEDEVICEWINDOW` and no window makes the device window, then fails with
  `DDERR_NOHWND` (`test_coop_level_create_device_window`, all four interfaces).

## 1.0.2
Wine's test suite down from 463 failed checks to 367 (50 of 423 sub-tests left, all either
differences on purpose, missing features or the test machine: tests/wine/README.md):
- **DirectDraw objects made with `DDCREATE_EMULATIONONLY`** put their surfaces in system
  memory (unless video memory is asked for) and refuse 24-bit Z buffers, as the emulation
  layer does (`test_caps`, all four interfaces).
- **Direct3D 7 ProcessVertices lighting, as XP's:**
  - normals go through the transpose of the inverse of the whole 4x4 world x view: a
    non-affine world turns them to zero, a singular one (no inverse) too (`test_lighting`);
  - normals are normalized only with `NORMALIZENORMALS`, so a world or view that shrinks
    lights stronger and saturates (`test_fog_process_vertices`: 57% green became full);
  - a material with specular power 0 keeps the last power above 0
    (`test_specular_lighting`, 47 checks);
  - vertex blending reads `VERTEXBLEND`'s count of weights after the position, whatever
    the FVF declares (this undoes 0.9.17's cap at the FVF's weights, which Wine's test on
    Windows contradicts), blends the matrices' whole 4-component results (a world with a w
    column), and blends each matrix's normal by the weights without normalizing the sum
    (`test_process_vertices`, `test_lighting`).
- **Direct3D 7 fog, as Windows draws it:** exponential vertex fog with lighting off on
  vertices that bring a specular colour takes the fog factor from its alpha
  (`test_fog_interpolation`); FOGSTART equal to FOGEND makes table fog a step at that depth
  and linear vertex fog fog everything (`test_fog_special`). Sent per draw (`SendFog`).
- **`IDirect3DDevice7::Load` of mip levels:** each smaller level's source rectangle rounds
  outward and the destination point halves, also over the source levels skipped to reach
  the destination's size (`test_device_load`: the levels were off by a pixel, and a load
  into a smaller texture failed).
- **Direct3D 3-6 keeps its list of active lights between draws** while no light, viewport
  light list, current viewport or view matrix changes (`g_lightGen`).
- The Wine test baseline is updated, and its README lists the newly triaged sub-tests.

## 1.0.1
- **Transformed vertices are drawn without Direct3D 9's clipping**, so depth past [0, 1]
  is clamped per pixel as the hardware of the era did, not cut off. NFS Porsche
  Unleashed's sky haze (depth up to 11, and the sky dome from -100 to 700) was cut in
  wedges on Intel's driver; Wine's own Direct3D clamps, so it never showed there.
  Untransformed vertices keep the game's CLIPPING state. d3d7test checks a triangle from
  depth 0.5 to 3 draws whole.
- **`antialias2d=` (off by default): with antialiasing, 2D drawn through Direct3D stays
  sharp.** A draw of transformed vertices at one depth and one rhw (screen-aligned: no
  perspective) with the depth test off, or ALWAYS without writing depth, is drawn into
  the resolved image after the 3D so far; the next 3D draw copies the part drawn into the
  multisampled image, as for the CPU's drawing. HUDs' edges (NFS Porsche Unleashed's dials) came out soft. `on`
  antialiases them as before. d3d7test's slanted-edge check is now depth tested (3D), and
  a 2D triangle is checked beside it.

## 1.0.0
First public release. Since 0.9.21:
- **`desktopbpp=` also answers GDI and EnumDisplaySettings** (`GetDeviceCaps` BITSPIXEL and
  NUMCOLORS on display and memory device contexts, the current and registry settings of
  the primary display), for games that check the desktop there rather than through
  DirectDraw. While the game has a display mode of its own, its depth is reported, as on
  XP; an 8-bit one is left real (GDI would have to act as a palette device). The imports
  are patched only when `desktopbpp` isn't 32.
- **`lockwhole=on`**: a Lock of a rectangle counts as a change to the whole surface (the
  GPU copy and the screen). For a game that writes outside the rectangle it locked, as
  XP's memory let it; off by default (since 0.9.18 only the rectangle reaches the GPU).
- **log=2 logs Direct3D 3 execute buffers** (one line per Execute: vertices and the
  instructions it holds) **and GetDisplayMode** (when its answer changes). Both of Moto
  Racer's problems were invisible at log=2 before.
- The Wine test baseline is brought up to date.

## 0.9.21
- Moto Racer's menu, continued: rhw that are tiny but not 0 (denormals, which the GPU takes
  as 0) count as bad too, and an untextured draw whose rhw differ over 10000 times gets
  one rhw (the smallest weigh nothing in Direct3D 9's interpolation). drawtrace prints rhw
  in full (%g), not to six decimals (tiny values read 0.000000).

## 0.9.20
- **Transformed vertices with rhw 0 beside valid ones in one draw** are drawn coloured.
  Hardware of the era interpolated colour across the screen without rhw; Direct3D 9
  divides every attribute by it, and a triangle whose rhw are all 0 came out black on
  Intel's driver (Moto Racer's menu letters flickered black: untextured, part of their
  vertices at rhw 0). Such a draw now gets rhw 1 on every vertex when untextured, else the
  draw's smallest valid rhw on the bad ones. (A draw with rhw 0 everywhere was already
  handled.) d3d7test checks it, though Wine's Direct3D draws it right either way.

## 0.9.19
- **`desktopbpp=` (16, 24 or 32; default 32): the desktop colour depth DirectDraw
  reports** before the game sets its own mode (GetDisplayMode, the windowed primary
  surface). Moto Racer calls GetDisplayMode at startup and stops with "16 bit screen mode
  required !" unless it says 16, even though it then switches to its own mode;
  `desktopbpp=16` in its section lets it start (as Windows' "Reduced color mode" did).

## 0.9.18 (optimizations)
No behaviour changes intended; each one checked with the full test set.
- **Presenter: the frame is converted before taking the GPU lock**, which now covers only
  copying the converted rows into the upload surface (the game's Direct3D draws on the
  same device no longer wait for the conversion).
- **Presenter's gamma and sharp-bilinear passes put back only the device state they
  change** (`SavedDeviceState`, src/devstate.h) instead of the game's Direct3D context
  sending all of its state again (some 450 calls) after every present.
- **antialiasing: CPU changes reach the GPU by dirty rectangle.** A lock or blit marks the
  rectangle it covers (`Buffer::gpuDirty`); only that part is uploaded and drawn into the
  multisampled surface, which also uses `SavedDeviceState` instead of a D3D9 state block.
  A HUD blit no longer copies the whole frame twice.
- **Direct3D 3-6 indexed draws light only the vertices the indices use.** The others get
  their position and clip code only (the clip status and extents stay as XP's transform
  of all of them made them), or are skipped where neither is kept.
- **Direct3D 3-6 pipeline:** clipping uses fixed arrays instead of vectors per triangle;
  the output vertex buffer grows geometrically instead of once per vertex; index and light
  lists reuse their storage; the eye vector is computed only for specular; the normal's
  length takes one square root instead of two; extents accumulate in locals.
- **Mouse: the game's image on the screen is a snapshot** (`GameImage`), so cursor
  functions and mouse messages neither wait for the DirectDraw lock nor ask Windows for
  the window's geometry each call. It's made again after a display-state change or a
  move of the game's window (and every 250 ms in case one went unseen); the window
  procedure never waits for it.

## 0.9.17 (review of 0.9.0-0.9.16)
Fixes from a line-by-line review (four reviewers, each finding checked against the code):
- **Direct3D 7: vertex buffers processed with `D3DVOP_CLIP` are drawn clipped.**
  ProcessVertices keeps the vertices outside the frustum in clip space, as XP does, and the
  draw sent them to the GPU as screen coordinates: triangles crossing a screen edge
  stretched to the corner. Now each primitive is clipped against the near plane in clip
  space and projected (`ExpandClipped`); d3d7test compares it with the direct draw.
- **Direct3D 3-6: a device outliving its DirectDraw object** no longer uses it after it is
  freed (the device keeps it, as XP's do through internal surfaces).
- **Textures given a new buffer by `SetSurfaceDesc`** (another size or format) are made
  again instead of being written past their end; a new buffer can't be taken for a freed
  one at the same address (its change count comes from one global counter).
- Direct3D 3-6: `CreateViewport`/`CreateMaterial` with a NULL pointer return
  `DDERR_INVALIDPARAMS` (one leaked a viewport, the other crashed); `SetRenderTarget` to a
  Z buffer doesn't leave the GPU context pointing at the old target; a surface outliving
  its DirectDraw object no longer makes a new Direct3D object on it; draws and execute
  buffers with absurd sizes fail instead of overflowing or throwing; an execute buffer's
  branch past its end stops instead of looping.
- Direct3D 7: `LightEnable` caps the index as `SetLight` does (a huge one threw);
  ProcessVertices blends min(the FVF's weights, VERTEXBLEND), draws send VERTEXBLEND no
  higher than the vertices' weights (D3D9 blended with weight 0), normals are lit through
  the inverse-transpose (non-uniform scale), and the destination format is checked as XP
  does (`D3DERR_INVALIDVERTEXFORMAT`).
- Z buffers: several depth fills before a draw all apply (a split screen's two halves);
  a new GPU depth buffer starts from the surface's own memory (zero-filled by DirectDraw, or
  the game's earlier fill), not as the driver leaves it; the
  emulated W buffer's scale starts again with each whole depth clear.
- GPU: before a Reset nothing of dd2d9's stays bound (native D3D9 refused the Reset, so
  every loss made a new device); a failed surface creation isn't retried (and logged) at
  every draw; CPU changes are marked sent only once they are; the slope-scaled overlay bias
  is sent again after a device change; palettized targets read back through their current
  palette.
- Windows: the topmost timer acts only while the game is in front; a deferred activation
  takes the real state when it runs (a stale "deactivated" could minimize a game in front)
  and is retried at most ~500 times; waiting for the display lock handles messages sent to
  the thread (a game with a render thread could deadlock); `WM_DISPLAYCHANGE` uses the
  DirectDraw object only under the lock; re-covering the screen gives up for real after 5
  tries in 2 s.
- `fpslimit`: windowed games are paced too (blits covering half the window), only after
  blits that succeeded, and never with the game's DirectDraw lock held.
- Compatibility: `cpus=N` takes one logical processor per physical core, core 0 last;
  `ram=`/`cpu=` also answer games that look the functions up with GetProcAddress, and keys
  opened with RegCreateKey(Ex) or through a copy of HKLM; a reused registry handle replaces
  the old entry. Import patching restores only slots still holding dd2d9's function, in
  loaded modules, writes them atomically, checks the import table lies in the image, and
  retries the module snapshot; `C:\WindowsGames\...` is no longer taken for Windows' folder.
- ini: a `[game.exe]` section overrides the general ones whichever file they are in (two
  passes); lines without `=` are logged; a comment may follow a tab.
- The log file is closed when the DLL is unloaded early (a reload truncated it under the
  open handle). MapWindowPoints' hook clears the last error when it returns 0.

Optimizations:
- Indexed draws send the GPU only the vertices their indices use (a few triangles from a
  big vertex buffer copied all of it, every draw).
- Waits (fpslimit, vblank pacing) use a high-resolution waitable timer, spinning only the
  last half millisecond instead of the last 2 ms on Sleep(0) (with `cpus=1` that took the
  game's own core).
- Direct3D 3-6: unchanged render states, stage states and textures return before the
  device check; Direct3D 7 re-sends the colour key's alpha test only when it changes; no
  heap allocation per bound texture per draw.

## 0.9.16 (0.9.15 had the timer on the window)
- **The exclusive window is made topmost again every 1.5 s** if something took that away,
  as `ddraw.dll`'s timer does (a hidden window stays hidden; not while the game is in the
  background, or with `DDSCL_NOWINDOWCHANGES`). A thread-pool timer, not one on the window:
  its ticks would go through the game's message loop. Wine's `test_window_style`
  "Expected WS_EX_TOPMOST" passes now (ddraw1/2/4/7).
- **A game resizing its own window in a scaled (borderless) mode, to a size that covers
  its screen, covers the real one again**: Jazz Jackrabbit 2 Plus sizes its window to
  661x544 now and then in its 640x480 mode (fullscreen on a real 640x480 display) and sat
  in a window until Alt+Enter or its next mode change. A smaller size stays a window.

## 0.9.14
- **`overlaybias` finds NFS III's light maps**: its trace showed every world draw alpha
  blended with depth writes on, so "blends" took in everything and the bias moved it all
  alike. An overlay is now a pass that doesn't write depth, or that adds or multiplies
  (DESTBLEND ONE or a colour factor, SRCBLEND ZERO or DESTCOLOR); ordinary alpha blending
  doesn't count. Overlays don't write depth while drawn: two lights laid on the same spot
  fought each other's depth as they fought the road's.
- `d3d7test`'s overlay check is NFS III's pattern: all blended, a road and two additive
  lights cut differently, all writing depth.

## 0.9.13
- `drawtrace=on` (diagnostic): every 600 scenes (about 10 s), one whole frame's draws are
  logged with their depth, blend and alpha-test states, texture, the depth bias dd2d9 put on
  them, and the range of z and rhw in their vertices.

## 0.9.12
- `overlaybias`: one 16-bit depth step whatever the game's Z buffer (NFS3+ asks for a 32-bit
  one, which got 16 steps of a 24-bit one: too little), plus a slope-scaled bias of one
  pixel's depth change (the mismatch grows with the depth gradient: roads at a grazing
  angle). `overlaybias=N` gives N steps, for games whose overlays need more.
- `filter=sharp`'s rewritten shader is taken by Windows' Direct3D 9 (Intel UHD 620).

## 0.9.11
- **No more hatched z-fighting of light maps and decals** (`overlaybias=on`, default): a
  pass laid on a surface already drawn (it doesn't write depth, or it blends) is pulled
  forward by one step of the depth precision the game asked for (a 16-bit step for a
  16-bit Z buffer, else 16 steps of a 24-bit one). The two differ only by rounding, which
  the 16-bit depth buffers of the era rounded the same way; 24-bit ones (`depth24`, most
  wrappers) show it as a fine hatch. NFS III's headlights and light maps on the road.
  (NFS III draws with Z buffering, not W: `wbuffer=force` hid the hatch but broke its depth
  order, the car drawn black.)
- `wbuffer=on` no longer makes 16-bit Z buffers 24-bit (only `force` does, and `depth24`).
- `filter=sharp`'s shader rewritten to stay clear of whatever Windows' ps_2_0 validator may
  refuse (Wine checks nothing): registers set whole before they are read, no negated
  constants, one constant per instruction. If it is refused still, the log says whether
  `frc` alone is.
- `d3d7test` checks an overlay on a floor cut differently, with Z buffering.

## 0.9.10
- The log names the Direct3D 9 adapter, its driver and pixel shader version when the
  device is made; when `filter=sharp`'s shader is refused, whether the device takes a
  pass-through shader at all. (On one Windows laptop the shader is refused with
  D3DERR_INVALIDCALL; the point-then-linear fallback is used there meanwhile.)
- The "game uses ZBIAS" line no longer fires for ZBIAS 0.

## 0.9.9
- **W buffering** (`wbuffer=on`, default): Direct3D 3-7 devices report
  `D3DPRASTERCAPS_WBUFFER` again, as most hardware of the era did, and `D3DZB_USEW` is
  emulated where the GPU lacks it (every current one): the depth of transformed vertices
  becomes 1 - k/w, linear across the screen over any flat surface, as a Z buffer
  interpolates it. Coplanar polygons cut differently then agree, where the game's own z
  (linear in w) fought: NFS III's headlights and light maps on the road, which every
  wrapper showed hatched. A pass that doesn't write depth (light maps, decals) gets a
  tolerance of 16 steps of a 24-bit depth buffer, which a 16-bit W buffer's coarser rounding
  gave for free. 16-bit Z buffers are made 24-bit with it. `wbuffer=force` treats Z buffering
  of transformed vertices the same way; `wbuffer=off` is the old behaviour. Untransformed
  vertices drawn by Direct3D 7 with W buffering still use the Z buffer.
- The log says when a game first draws with W buffering, and first uses ZBIAS.
- `d3d7test` checks a floor drawn twice, cut two ways (z-fighting or not).

## 0.9.8
- **`filter=sharp` on Windows**: Windows' Direct3D 9 refused its pixel shader (the texture
  read's coordinate register had components never written, which Wine doesn't check), and
  the frame was shown with plain linear filtering. Fixed; the log gives the error if a
  shader is refused again. Without pixel shaders, sharp now enlarges with point sampling by
  the whole factor at or above the scale, then linear down (no longer plain linear below 2x).
- **`antialiasing`: no lines at the edges of cut-out sprites** (NFS III's trees and corn):
  multisampling takes the texture coordinate at the pixel's centre, past the sprite's edge
  where it covers only part of the pixel, and wrapping read the other side of the texture
  there. A cut-out texture (colour-keyed or with alpha) drawn within its own coordinates
  (0 to 1, not tiled) is addressed clamped for that draw; tiled ones keep the game's.
- `d3d7test` checks a cut-out sprite's edge.

## 0.9.7
- **`fpslimit=N`**: frames paced to N per second at Flip and at blits covering half the
  primary or more; a frame later than a whole period starts the count again (no burst).
- **`render32=on`**: 16-bit render targets drawn in 32 bits on the GPU, converted when the
  CPU reads them. Read-back conversion to any RGB layout now goes through a table (the
  same rounding as before).
- **`depth24=on`**: 16-bit Z buffers made 24-bit on the GPU.
- **`antialiasing=2|4|8`**: multisampled render targets and Z buffers (32-bit and 24-bit,
  the formats every GPU multisamples; the largest sample count the GPU has, up to N).
  Direct3D draws into the multisampled surface, resolved before anything reads it (the
  CPU, the presenter, a texture copy). 2D the CPU writes between 3D draws is drawn back in
  as a quad, the game's device state kept around it in a state block; a full clear skips it.
- **`texfilter=linear`**: point-filtered textures drawn bilinear, cut-outs excepted.
- `d3d7test` checks faint additive passes (16- vs 32-bit precision), a slanted triangle
  edge (antialiased or not) and 2D written between 3D draws (kept).

## 0.9.6
- `mipmaps=on` leaves textures with an alpha channel alone too, as it did colour-keyed
  ones: their transparent texels blurred into the smaller levels and drew lines through
  NFS III's corn field in the distance.

## 0.9.5
- `anisotropy` leaves cut-out textures alone (colour-keyed, or with an alpha channel):
  its longer footprint reached past their edges and wrapped round, drawing thin vertical
  lines through NFS III's trees (as with other wrappers). Sprites are seen face-on, where
  anisotropic filtering changes nothing anyway.

## 0.9.4
- **`anisotropy=2|4|8|16`**: anisotropic filtering on every texture stage the game filters
  smoothly (linear or anisotropic minification), at most what the GPU can; point-filtered
  stages are left as they are. Off by default. The log says what the GPU allows.
- `d3d7test` checks one-pixel stripes squashed 8x vertically (blurred without anisotropic
  filtering, kept with it).

## 0.9.3
- **`filter=sharp` works at every scale**: it was plain bilinear below 2x (640x480 on a
  1366x768 or 1080p screen) and a two-pass approximation above. Now one pixel shader
  (sharp bilinear): each game pixel a flat block, blended with its neighbour only over the
  one screen pixel at its edge.
- **`mipmaps=on`**: textures the game gave no mipmaps get them from the GPU
  (`D3DUSAGE_AUTOGENMIPMAP`), and the mip filter is linear where the game asked for none:
  no shimmer in the distance. Colour-keyed, block-compressed and render-target textures are
  left alone. Off by default.
- **`alternatepixelcenter=on`**: Direct3D drawn half a pixel up and left, as old ATI and
  Nvidia drivers offered, for games with lines at the edges of textures. Transformed
  vertices are moved; the projection of untransformed ones is. Off by default.
- `d3d7test` checks a texture drawn 8x smaller (mipmapped or not) and 1:1 with bilinear
  filtering (where the pixel centres fall).
- After drawing through its shaders (gamma, sharp), the presenter sets the device's own
  back buffer as its render target again, so the device holds no surface of the presenter's.

## 0.9.2
- **Compatibility options** (`src/compat.cpp`), what the game is told about the machine:
  - `ram=1024` (default): `GlobalMemoryStatus(Ex)` reports 1 GB of memory (3/4 of it free
    at most) and a 1.5 GB commit limit. Windows tells programs that aren't
    large-address-aware 2 GB and a 4 GB page file, which old games overflow and then
    claim there isn't enough memory. `ram=0` gives the real values.
  - `vram=512` (default, as before): the video memory DirectDraw reports.
  - `cpu=2000` (default): the registry's `~MHz` for every processor reads 2 GHz
    (`RDTSC` can't be faked from a DLL; `cpu=0` gives the real clock).
  - `cpus=1` (default): the game runs on one processor (its affinity) and is told so
    (`GetSystemInfo`'s count and mask), as on the PCs of its era; `cpus=0` gives all.
  The imports are patched from `DllMain`, before the game's code runs, and again for each
  new DirectDraw object. The import patching is shared with the mouse translation
  (`src/hooks.cpp`).

## 0.9.1
- **The gamma ramp applies to Direct3D frames presented from the GPU** (0.8.3 showed them
  without it): the frame goes through a small pixel shader that looks each channel up in
  the ramp. Games with a brightness slider using `SetGammaRamp` look right again in 3D.
- **Render targets used as textures are copied on the GPU** while Direct3D holds their
  newest pixels, instead of being read back through system memory and uploaded again at
  every use (reflections, render-to-texture menus). Mipmapped or colour-keyed ones still go
  through the CPU copy.
- **D7VK support removed**: dd2d9's own Direct3D 3–7 replaces it. Gone: the Direct3D
  objects for a wrapper (`d3dproxy.cpp`), the presenter stepping aside for a wrapper's
  device, `install-dd2d9.sh --d7vk`. `d3d=on` now means the same as `auto`.
- With D7VK gone, two workarounds for references it kept go too, back to `ddraw.dll`'s
  rules (re/spec/ddraw-object.md): a second DirectDraw object gets
  `DDERR_EXCLUSIVEMODEALREADYSET` while the first holds exclusive mode in front (it takes
  over once the first lost activation, or its window is gone or hidden), and a new primary
  surface is refused with `DDERR_PRIMARYSURFACEALREADYEXISTS` while the old one exists, lost
  or not.
- **Faster texture and render-target uploads**: rows go to A8R8G8B8 through a converter
  built once per surface (`src/convert.cpp`): SSE2 for 565/555/1555/4444 and 32-bit RGB,
  colour key included, and a table for palettized ones. It is bit-exact with the old
  per-pixel path and about 7x faster (a 1024x768 565 texture: ~0.55 ms instead of ~4 ms).
- Built with `-msse2 -mfpmath=sse`: the Direct3D 3–6 transform, lighting and clipping
  code no longer runs on the x87 stack. (The presenter's 16-bit conversion stays a
  table: measured, SSE2 wasn't faster there.)
- `d3d7test` checks render to texture; `gamma` sets an inverting ramp.

## 0.9.0
**Direct3D 3–6 in dd2d9 itself** (`src/d3d6*.cpp`), matched to XP SP3's `d3dim.dll`
(`re/spec/d3d6-*.md`): DirectDraw objects made by `DirectDrawCreate` now offer
`IDirect3D`, `IDirect3D2` and `IDirect3D3` (and report 3D support), and their surfaces
`IDirect3DTexture`/`2` and Direct3D 1 devices.
- EnumDevices and FindDevice list Ramp, RGB and HAL (IDirect3D3: RGB and HAL) with XP's
  names and descriptors; the HAL's caps come from the Direct3D 9 adapter, the software
  devices' are XP's fixed tables (the DX3/5 RGB one without alpha blending).
- Devices v1 (made by a surface's QueryInterface, sharing its reference count), v2 and v3,
  with XP's creation checks, references, render-state defaults (version-dependent
  TEXTUREPERSPECTIVE, SPECULARENABLE, COLORKEYENABLE), light states with their override
  bits, texture stages, transforms, v1 matrix handles, viewports (XP's list order and
  reference rules), scenes (BeginScene restores lost surfaces for the game, as d3dim does),
  SetRenderTarget, GetCaps for all three descriptor sizes, EnumTextureFormats with XP's
  inclusion list, GetStats, clip status, Begin/Vertex/End and all the draws.
- The legacy render states reach Direct3D 9 the way Microsoft's own DX6 rasterizer took
  them: TEXTUREHANDLE (stage 0 only while set), TEXTUREMAPBLEND's table, TEXTUREMAG/MIN,
  TEXTUREADDRESS, WRAPU/V and the rest onto stage 0, last writer wins; Get still answers
  what the game set.
- Viewports with D3DVIEWPORT and D3DVIEWPORT2 semantics (the clip volume), TransformVertices,
  Clear and Clear2 (backgrounds from material handles, textured backgrounds), lights with
  both the D3DLIGHT (DX3/5) and D3DLIGHT2 (DX6) models, materials and their per-device
  handles, texture handles (one per surface, as XP's DP2 handles) with SwapTextureHandles
  and Load.
- Execute buffers and the whole instruction stream: PROCESSVERTICES (transform, light, copy),
  points, lines, triangles, state instructions with override bits, matrix load/multiply
  with live bindings, texture loads, branches, SETSTATUS, and the status written back.
- A CPU front end does every untransformed vertex, as XP's did (no hardware T&L for DX6):
  world, view, projection and the viewport's clip matrix, lighting, vertex fog, clip codes
  and status, clipping where primitives leave a guard band or cross the near or far plane.
  Direct3D 9 draws the result as transformed vertices.
- Deliberately safer than `d3dim.dll` where it dereferences blindly: unknown handles, no
  current viewport, bad vertex types, offsets past an execute buffer and NULL pointers give
  errors instead of crashes.
- The viewport's Clear is held to the viewport on version 1 devices too (Wine's test_clear:
  XP's drivers did it, AMD's didn't); `Clear(n, NULL)` clears nothing and succeeds (Wine's
  test_clear_rect_count). A hardware device draws untextured from a surface that isn't a
  texture (Wine's test_texture_wrong_caps). Matrix handles work on every device (Wine's
  test_multiple_devices).
- Surface QueryInterface: IDirectDrawColorControl is `E_NOINTERFACE`; other IIDs it doesn't
  know are `E_INVALIDARG` on DirectDraw 1-4 surfaces, as `d3dim.dll` answers them.
- Direct3D 9 is shared by several Direct3D devices: one taking it over sends all of its
  state again.
- **8-bit palettized render targets** for the software devices (Direct3D 3-6's RGB, Ramp; d3dim700's RGB never had them):
  drawn in true colour on the GPU, mapped back to palette indices when read (exact entry,
  else nearest). Direct3D 7's sysmem palettized target fails with E_FAIL as on XP.
  Palettized textures are listed by EnumTextureFormats on every device
  (converted on upload, as before), so they no longer land in video memory unlisted (Wine's
  test_unsupported_formats, test_rt_caps).
- Lit vertices get a specular alpha of 0, fog aside (Wine's test_lighting on Windows);
  colour keys act on the texel alone when the game's alpha isn't used (test_colorkey_precision).
- DX6 ComputeSphereVisibility answers in D3DVIS_* flags; legacy texture Load keeps a colour
  key only the destination has and refuses a palette into a palette-less destination.

## 0.8.3
- **Direct3D frames are presented straight from the GPU**: a flipped-in render target the
  GPU holds the newest copy of is stretched into the window as it is, instead of being
  read back into system memory at every Flip and converted again. Frames with DirectDraw
  or GDI drawing on top (the CPU copy is then the newest) still go through system memory.
  The gamma ramp isn't applied to frames presented this way yet.
- `d3d7test ... 3d` draws without the 2D on top (frames presented from the GPU).

## 0.8.2
- **Entering exclusive mode brings the game's window to the foreground** past Windows'
  foreground lock, as `ddraw.dll` does (FUN_7375d557: the lock timeout zeroed for the
  call, and a do-nothing mouse event so the process counts as the last to get input).
  Nerf ArenaBlast, started from Explorer, stayed behind it after its splash window
  closed: no keyboard or mouse input until Alt+Tab, and Windows' cursor showing in the
  middle of the screen.

## 0.8.1
- **The taskbar no longer shows over Unreal engine games in fullscreen** (Nerf
  ArenaBlast). After restyling its window, the game sized it back to its own idea of
  fullscreen every time dd2d9 covered the screen again, until dd2d9 gave up with part of
  the screen uncovered. Those re-covers are now hidden from the game (its window procedure
  doesn't see the move). The game's own moves and sizes still stand, as in `ddraw.dll`
  (Wine's test_window_position).

## 0.8.0
**Direct3D 7 in dd2d9 itself**, no D7VK needed: `IDirect3D7`, `IDirect3DDevice7` and
`IDirect3DVertexBuffer7` (`src/d3d7.cpp`), matched to XP SP3's `d3dim700.dll`
(`re/spec/d3d7-*.md`), drawing through the Direct3D 9 device dd2d9 presents with
(`src/gpures.cpp`), so on Windows' own Direct3D 9 and on DXVK alike.
- EnumDevices lists RGB, HAL and T&L HAL with XP's names; the hardware devices' caps come
  from the Direct3D 9 adapter, RGB's are XP's fixed table. CreateDevice checks the render
  target and its attached Z buffer as XP does, and sets the x87 FPU to single precision
  unless DDSCL_FPUPRESERVE.
- Render states, texture stages (and the sampler states D3D9 moved them to, with
  MAGFILTER/MIPFILTER's changed values), transforms, lights, material, viewport, clip planes,
  clip status, state blocks (XP's own, handles from 1), Clear, scenes, every Draw*
  call, Load, ComputeSphereVisibility, and ProcessVertices on the CPU (transform, clip
  codes, lighting, vertex fog, vertex blending).
- Textures, render targets and Z buffers get a GPU copy (P8 and colour-keyed textures
  converted, the key becoming alpha while COLORKEYENABLE is on); what Direct3D drew is read
  back whenever DirectDraw touches the pixels, so 2D over a 3D frame works.
- Quirks for D3D9: D3DFVF_RESERVED1 (D3D9's PSIZE) is skipped; transformed vertices with
  ZENABLE off have their z clamped (D3D9 always clips it); rhw 0 on every vertex becomes 1
  (DXVK); lit vertices without a normal get a zero one.
- Deliberate differences: `ValidateDevice` reports one pass (XP returns E_NOTIMPL without
  a driver callback); render targets can't be palettized (D3D9 can't render to P8).
- DirectDraw objects made by DirectDrawCreateEx report 3D support (DDCAPS_3D and
  friends); Direct3D 1-6 aren't implemented yet, so DirectDrawCreate objects don't.
- DirectDraw side: Z buffers with an 8-bit stencil (24+8) can be created; DirectDraw 7
  managed textures take the system-memory textures' lock fast path; CreateSurface with an
  unwritable out pointer returns DDERR_INVALIDPARAMS (XP faults inside its handler).

## 0.7.19
- **In the background after Alt+Tab, the primary surface (and the other surfaces lost by
  the focus loss alone) can still be locked**, though it reports lost as in `ddraw.dll`
  (a deliberate difference: dd2d9 keeps the pixels). Captain Claw locks its primary while
  in the background (taking a picture of the screen for its pause menu), ignores the
  failure and writes through the null pointer: it crashed when Alt+Tabbed during play.
- Groundwork for Direct3D, not used yet: GPU copies of render targets, Z buffers and
  textures (`src/gpures.cpp`), read back before DirectDraw touches the pixels.

## 0.7.18
- **Back from Alt+Tab, the game's surfaces other than the primary and its back buffers
  can still be drawn with** (Blt, BltFast, Lock, GetDC), though they report lost as in
  `ddraw.dll` (IsLost, Restore): dd2d9 keeps every surface's pixels, where `ddraw.dll`
  frees their video memory. Captain Claw never restored some of its sprites after Alt+Tab:
  its text was gone, and the failing blits slowed it to 2 fps. After a display mode
  change nothing is drawable until restored, as before.
- **The Direct3D 9 device's hidden window has a thread of its own** that only handles its
  messages. 0.7.17 made it on the presenter thread, which can be busy or waiting on one
  of dd2d9's locks while Windows sends every window the news of a display mode change and
  waits for the answers: the likely cause of Nerf Arena Blast freezing behind D7VK with
  `display=switch` (fine in 0.7.16).
- **The device goes when a wrapper (D7VK) takes the presentation over**, as the
  presenter's own device did before 0.7.17, rather than stay beside the wrapper's.

## 0.7.17
Groundwork for Direct3D (dd2d9 implementing Direct3D 3-7 itself on Direct3D 9; design in
`re/d3d-design.md`):
- **One Direct3D 9 device per DirectDraw object** (`src/gpu.cpp`), created once on a hidden
  window of its own and presenting into the game's window through an additional swap
  chain. A window resize recreates only the swap chain, never resets the device, so the
  GPU resources Direct3D will keep there (render targets, textures) survive it. A lost
  device is reset once Windows gives it back.
- **`present=exclusive` is gone** (a Direct3D 9 exclusive-fullscreen device can't share
  the window with the swap chain). The key is still accepted, and ignored.

## 0.7.16
More of XP SP3's `ddraw.dll` (new specs: `re/spec/legacy-interfaces.md`,
`gamma-colorcontrol.md`, `window-procedure.md`):
- **The old interfaces:** IDirectDrawSurface2/3's GetAttachedSurface and
  EnumAttachedSurfaces hand out IDirectDrawSurface (v1) pointers; an attachment counts on
  the pointer the game passed, whatever interface it was called through (attach through
  one version, detach through another); EnumAttachedSurfaces (v1-3) and EnumSurfaces
  (IDirectDraw/2) describe a Z buffer by its pixel format, as `ddraw.dll`'s cut-down
  structure does; EnumSurfaces(CANBECREATED) through IDirectDraw2 tries IDirectDraw2's
  creation rules; GetAttachedSurface checks for a lost surface first and leaves the out
  pointer alone on failure; IDirectDraw's cooperative-level behaviour is kept even when
  the window can't be taken.
- **The window procedure:** the monitor may power down in exclusive mode (only the
  screensaver is blocked); a game that asked for DDSCL_NOWINDOWCHANGES isn't minimized or
  made non-topmost on focus loss; the cursor is kept on the screen after another
  program's mode change; no "Not Responding" ghost window replaces a busy game's; the hook
  goes with the window (WM_NCDESTROY).
- **The cursor imports are left alone with a real mode switch**: they're pointed at
  dd2d9's only while a mode is shown scaled or in a window.
- Gamma: no change needed (XP's DDERR_UNSUPPORTED when Windows has no ramp is replaced
  by dd2d9's own ramp, deliberately).

## 0.7.15
- `log=2` traces the reference counts of the primary surface's flip chain and of the
  DirectDraw objects (AddRef/Release), and each GetAttachedSurface, EnumAttachedSurfaces
  and surface QueryInterface call, to settle how Jazz Jackrabbit 2 and Type Wild get
  past `ddraw.dll`'s refusals (a second primary, a second exclusive object) on XP, and so
  replace those two workarounds with what XP really does.

## 0.7.14
- **3D games behind D7VK: use `display=switch`** to keep 4:3 modes 4:3. D7VK presents
  through its own swap chain, which fills the game's window, so dd2d9's `stretch` and
  `filter` don't apply to its frames (only to what dd2d9 presents: 2D games and screens).
  Fitting the window to the image behind the game's back worked, but needed black
  windows beside it, hiding the moves from the game (Unreal sizes its window back) and
  the taskbar: too much working around the wrapper, so it was left out.
- **A desktop display change no longer loses the surfaces of a game whose mode dd2d9
  emulates** (`display=borderless` or `window`, or a mode the monitor lacks). Nerf
  ArenaBlast quit with "Failed blt" while loading a level: a `WM_DISPLAYCHANGE` that left
  the desktop at the same resolution (a laptop changing its refresh rate on its own) lost
  every surface, and Unreal treats a failed texture upload as fatal. The game's mode
  isn't the desktop's there, and dd2d9's surfaces need no video memory freed for it. A
  real mode switch (`display=switch`) still loses them on a change, as `ddraw.dll` does.
- **The log names C++ exceptions too**, with the modules on the stack, as it does crashes.
  Unreal engine games (Nerf ArenaBlast) throw one for a "critical error" and then show
  their own error box, which left nothing in the log.
- A posted window check that finds the display busy (another thread setting a device up)
  is retried instead of dropped.

## 0.7.13
- **`filter=sharp`** (also `bilinear-sharp`): the image is scaled up by the largest whole
  multiple with sharp pixels, then smoothly the rest of the way. Pixels stay crisp and
  even, where `point` makes some rows and columns wider than others at non-integer
  scales, and `linear` blurs everything.
- **Other programs can't take the focus from an exclusive-mode game** (Captain Claw lost
  it a few seconds after starting, to a window of another program; the game then wrote
  through its failed Lock of the lost primary and crashed). While the game is in front,
  other programs can't bring their windows to the front on their own
  (`LockSetForegroundWindow`); Alt+Tab and clicks still can. When the game does lose the
  focus, the log names the program that took it.

## 0.7.12
- **The mouse in scaled and windowed display modes** (`display=borderless` or `window`, or
  a mode the monitor lacks): the game sees the cursor in its own mode's coordinates, its
  window as its screen at 0,0, as after a real mode switch. dd2d9 points the game's
  imports of the cursor functions at its own (`GetCursorPos`, `SetCursorPos`,
  `ClipCursor`, `GetClipCursor`, `GetMessagePos`, `GetSystemMetrics` for the screen size,
  `GetClientRect`/`GetWindowRect`/`ScreenToClient`/`ClientToScreen`/`MapWindowPoints`
  for its window) and translates its window's mouse messages. Clicks used to land where
  the unscaled coordinates pointed. `mouse=raw` turns it off.

## 0.7.11
More of XP SP3's `ddraw.dll`:
- **CreateSurface** checks descriptions as `ddraw.dll` does, in its order: its caps rules
  (e.g. PALETTE or VISIBLE asked for, invalid combinations), Z buffers (by depth through
  IDirectDraw/2, by pixel format through 4/7), no size or format for a primary, power-of-two
  mipmaps with no more levels than the size allows (they used to be cut down), color keys
  (no overlay keys or ranges), client memory (checked first; IDirectDraw4 takes an
  unaligned pointer), cube maps through IDirectDraw7 only; CLASS_E_NOAGGREGATION comes
  later through IDirectDraw/2.
- **EnumSurfaces** validates its flags and filter as `ddraw.dll` does and matches only the
  fields it compares (caps exactly, the size, keys, counts, and a pixel format only for
  surfaces created with one), newest surface first; CANBECREATED ignores the callback's answer.
- **SetCooperativeLevel**: exclusive mode stays on its window (another window gets
  `DDERR_HWNDALREADYSET` until the game goes back to normal); entering it loses every
  DirectDraw object's surfaces; `DDSCL_SETDEVICEWINDOW`'s checks; a window with a dialog
  frame but no border isn't changed.
- A DirectDraw object made by CoCreateInstance answers `DDERR_NOTINITIALIZED` until
  Initialize. A released palette's AddRef/Release return 0 and SetPalette refuses it.

## 0.7.10
- **In exclusive mode the image fills the screen whatever size the window is.** JJ2+
  sizes its window past the screen while starting up (661x544 at 640x480); the image was
  centered in the whole window, which put a black bar at the top and cut off the bottom
  until the next mode change. As with `ddraw.dll`, the screen shows the game now.

## 0.7.9
- **A game changing its window's frame in exclusive mode** gets its window fitted to the
  screen again (the frame just off it).
- `log=2` also traces style changes, with the window and client rectangles.

## 0.7.8
- **Leaving exclusive mode loses the surfaces when the desktop's display mode comes
  back**, as in `ddraw.dll` (the game's mode is gone). The old fullscreen primary is then
  no longer shown in the window, which still covered the screen for a moment until the
  game resized it: a black flash after Alt+Enter. (JJ2+ switching to windowed.)

## 0.7.7
Matched to Windows XP SP3's `ddraw.dll` call for call (from its decompiled code; the
specifications are in `re/spec/`), method by method for the surfaces, palettes, clippers,
the exports and much of the DirectDraw object.
- **Blt and BltFast report lost surfaces** (`DDERR_SURFACELOST`), so a game that blits
  to the primary restores it after a mode change or Alt+Tab instead of drawing to a lost
  one. They check in `ddraw.dll`'s order: locked surfaces (or a DC out) are busy, a
  `DDBLTFX` needs its size, raster operations other than SRCCOPY, BLACKNESS and
  WHITENESS fail with `DDERR_NORASTEROPHW` (they used to succeed and draw nothing; GetCaps
  now lists those three), rotation and DDROPS are refused, fills check their surfaces.
- **Flip chains put together with AddAttachedSurface** work through IDirectDrawSurface
  1-4 for any two surfaces of a kind, as in `ddraw.dll`; a surface attached to the front
  becomes the back buffer. AddAttachedSurface follows its size, one-per-kind and memory
  rules (a Z buffer on any surface, not only primaries and 3D devices);
  DeleteAttachedSurface(NULL) detaches everything; attachments are listed newest first.
- **GetSurfaceDesc** builds its flags as `ddraw.dll` does; after a mode change the primary
  reports the display's pixel format. SetSurfaceDesc, GetPixelFormat, the uniqueness
  value, private data (`DDSPD_VOLATILE`), priority/LOD (managed textures only) and the
  overlay methods return what `ddraw.dll` returns.
- **Gamma ramps** belong to the primary and show only in exclusive mode;
  `SetGammaRamp(NULL)` resets.
- **Palettes and clippers**: CreatePalette's flag rules, entry ranges and flags,
  GetCaps; a window clipper's GetClipList is its visible region on the screen,
  IsClipListChanged says TRUE, SetClipList/SetHWnd validate, clippers made through
  DirectDraw 4/7 reference it.
- **DirectDraw object**: RestoreAllSurfaces returns the first failure;
  TestCooperativeLevel out of exclusive mode reports another object's exclusive mode or a
  changed display mode; Set/RestoreDisplayMode refuse while a video-memory surface is
  locked; a primary can't be made while another object is exclusive, and the exclusive
  object's new primary makes the others' lost; leaving exclusive mode flips back to the
  GDI surface; the mouse stays on the game's monitor in exclusive mode; checks for
  WaitForVerticalBlank, GetScanLine, GetAvailableVidMem, Compact, GetCaps,
  GetDeviceIdentifier, EnumDisplayModes, DuplicateSurface and GetSurfaceFromDC.
- Lock fills the DirectX 7 fields only for a whole DDSURFACEDESC2.
- Looking for display changes made by others costs less per call (once per call, timed
  with the processor's counter).

## 0.7.6
- **The window keeps its style in fullscreen.** Instead of removing the frame, dd2d9 grows
  the window until its client area covers the monitor, the frame just off the screen (a
  window region keeps it off neighbouring monitors; `WM_GETMINMAXINFO` lets the window be
  that large). Games reading their style see Windows' value, and D7VK, which always draws
  into the client area, still fills the screen. Leaving exclusive mode shrinks the window
  back to the screen. (Wine `test_window_style`.)
- **A hidden window stays hidden** when exclusive mode starts while another window is in
  front, and isn't minimized on focus loss, as with `ddraw.dll`.
- **DirectDraw 1 leaving exclusive mode drops the window's topmost state** (the display mode
  stays, as before).

## 0.7.5
- **Leaving exclusive mode after a focus loss loses the surfaces**, as in `ddraw.dll`
  (not with DirectDraw 1, or when the game never went to the background).
  (Wine `test_lost_device`.)
- **Interface tables (vtables) are writable**, as `ddraw.dll`'s: overlays and fan
  patches hook DirectDraw by rewriting them, which crashed on read-only memory.
  (Wine `test_vtbl_protection`.)
- **Attaching a surface again through another interface version** returns
  `DDERR_CANNOTATTACHSURFACE` (the same version still gives `DDERR_SURFACEALREADYATTACHED`).
- **GetDC**: none for 32-bit surfaces with red in the low byte (A8B8G8R8, X8B8G8R8), as
  on Vista and later; A2R10G10B10 gets a D3DKMT DC; 32-bit fallback DIBs are `BI_RGB`.
- Tests: `tests/wine/` builds and runs Wine's ddraw tests one sub-test per process and
  compares runs with a baseline (`tests/wine/README.md`).

## 0.7.4
- **`d3d9=system` (default): the 2D picture goes through Windows' own `d3d9.dll`**, even
  when the game folder holds DXVK's (which D7VK needs). Presenting through DXVK's Vulkan in
  a window covering the screen let the driver take the display over and later switch it
  back to the desktop resolution, which dropped Jazz Jackrabbit 2 (JJ2+) out of fullscreen.
  `d3d9=game` restores the old behaviour. The log names the `d3d9.dll` in use.

## 0.7.3
- **A display-change notice only loses the surfaces if the display really left the
  game's mode.** Windows sends `WM_DISPLAYCHANGE` twice, the second time late, and between
  two game modes one notice may name the desktop's; JJ2+ took the resulting lost surfaces
  as a reason to go windowed. (JJ2+ switching 640×480 → 800×600.)

## 0.7.2
- `log=2` also traces the game window's focus, activation, size and display-change
  messages, and every DirectDraw call that fails, with its error.

## 0.7.1
- **A new primary surface can replace one a display-mode change made lost.** (Jazz
  Jackrabbit 2 sets its mode again without releasing the old primary: crash.) `ddraw.dll`
  itself refuses a second primary; this is a dd2d9 workaround.
- **A second DirectDraw object takes exclusive mode over from the first** instead of
  getting `DDERR_EXCLUSIVEMODEALREADYSET`, as in Wine (`ddraw.dll` refuses it while the
  first is active); the old object drops to normal mode and hands over the saved window
  state. (Type Wild recreates its DirectDraw object without releasing the old one: hang.)
- **Only one DirectDraw object presents**: one whose primary is lost, or while another
  object is exclusive, stays off the window. (Hydro Thunder around its videos, Nerf Arena
  Blast after Alt+Tab: two presenters on one window flickered.)
- `ddtest` covers both game patterns.

## 0.7.0
- Surfaces in plain memory with `ddraw.dll`'s pitch; `GetDC` through
  `D3DKMTCreateDCFromMemory`, with a DIB copy as fallback.
- Per-interface reference counts (surfaces and DirectDraw), as `ddraw.dll` counts them.
- Lost surfaces: display-mode changes, entering exclusive mode and losing focus in it;
  `IsLost`, `Restore`, `RestoreAllSurfaces`, `TestCooperativeLevel`.
- Exclusive mode: the window covers the screen and is activated, topmost while in front;
  focus loss restores the desktop mode and minimizes.
- DirectDraw 1 flip chains put together with `AddAttachedSurface`.
- Overlays and colour-key ranges refused, as on hardware without them.
- Many return codes and validation orders matched to Windows using Wine's tests:
  1643 → 237 failed checks, 262 → 364 clean sub-tests.

Earlier versions aren't recorded here.
