# The window hook: what ddraw.dll does to the game's window procedure

Reference: ddraw.dll 5.3.2600.5512 (XP SP3). Decompilation in `$S/decomp-annotated/` (`$S` = the session
scratchpad). Line numbers are those of the plain-named file (`FUN_xxxxxxxx_xxxxxxxx.c`,
`IDirectDraw7_X.c`). Return values the decompiler lost (it shows `extraout_EDX`) were read from the
disassembly (`$S/xp.dis`); those cite an address.
dd2d9 references are to `src/ddraw.cpp` at the time of writing.

Related: ddraw-object.md (SetCooperativeLevel, Release, TestCooperativeLevel; differences 6 and 9 there
cover ClipCursor and window placement on entering exclusive mode and aren't repeated here).

## Data

**Hook node** (0x24 bytes, `FUN_7375e236`:72-93), one per process in a global list `DAT_73773228`
(next at +4). `FUN_7375d7c7(hwnd)` finds the node by window (+0xc).

| Offset | Field |
|---|---|
| +0x00 | magic `0x42954295`. The hook procedure checks it and calls `DefWindowProcA` if it's missing (FUN_7375dcdc:38-41) |
| +0x04 | next node |
| +0x08 | DSoundHelp callback (node flag 2) |
| +0x0c | hooked window |
| +0x10 | previous window procedure |
| +0x14 | owner key: the process id (DirectDraw) or DSoundHelp's key |
| +0x18 | **node flags** (below) |
| +0x1c | the DirectDraw LCL (node flag 1) |
| +0x20 | the cooperative-level flags of the call that hooked (node flag 1) |

**Node flags** (+0x18):

| Bit | Meaning | Set / cleared |
|---|---|---|
| 0x01 | DirectDraw exclusive hook (+0x1c and +0x20 valid) | e236:90 / e236:30 |
| 0x02 | DSoundHelp hook (+0x08 valid) | e236:86 / e236:27 |
| 0x08 | "free the node on unhook". Set when the unhook is deferred or impossible | e236:38, 44 |
| 0x10 | "unhook at the next message" | dcdc:44 (WM_NCDESTROY), e236:44 (cross-thread) |
| 0x20 | Alt was held when the game was activated (protects the Tab key-up of the Alt+Tab that brought it back) | dcdc:200 / dcdc:134, 203 |
| 0x40 | **ddraw's own window operation in progress** (its ShowWindow/SetWindowPos, forwarding WM_DISPLAYCHANGE) | db9a:32-34, d5aa:61-63, d5aa:90-104, dcdc:105/115 |
| 0x80 | **inside WM_ACTIVATEAPP** | dcdc:72 / dcdc:256 |

**LCL fields used** (names from the DDK's `ddrawi.h`, offsets matching it): +0x04 lpGbl, +0x0c dwLocalFlags,
+0x20 hWnd (device window), +0x28 dwErrorMode, +0x2c lpPrimary, +0x34 dwPreferredMode, +0x48 dwAppHackFlags,
+0x4c hFocusWnd, +0x50 dwHotTracking, +0x54 dwIMEState, +0x58 hWndPopup, +0x70 next LCL (list `DAT_7377008c`).
LCL flags: HASEXCLUSIVEMODE 1, ISFULLSCREEN 2, ACTIVEYES 8, ACTIVENO 0x10, HOOKEDHWND 0x20, V1SCLBEHAVIOUR 0x80,
MODEHASBEENCHANGED 0x100, CURSORCLIPPED 0x1000, EXPLICITMONITOR 0x2000.
GBL flags (+4): DISPLAYDRV 0x20, FULLSCREEN 0x40, FLIPPEDTOGDI 0x20000. Exclusive owner: gbl+0x3f8.

Cooperative-level bit **0x2000** (node +0x20) is not a public DDSCL flag. SetCooperativeLevel accepts it
(validation masks 0xFFFFC000). Who passes it is uncertain (perhaps Direct3D 8's internal call). DirectX 1-7 games
don't pass it, so the two 0x2000 rules below don't apply to them.

## Installing and removing — `FUN_7375e236` (called by `FUN_7375e3ef` and `DSoundHelp`)

1. **Install** (SetCooperativeLevel exclusive, `IDirectDraw7_SetCooperativeLevel.c`:224-225, unless flag 0x20).
   `FUN_7375e3ef` (lcl, hwnd, flags) first places the window (`FUN_7375d40c`, e3ef:23-26; see "Placing the window").
   It hooks **the focus window** (lcl+0x4c, e3ef:48), not the device window, with key = process id (e3ef:27, 50).
   - A node with this key exists: on another window → DDERR_HWNDALREADYSET (e236:57-58). On the same window, only the
     flags/LCL are updated (e236:85-93).
   - The window already has a node (another key) → DDERR_INVALIDPARAMS (e236:68-70).
   - Otherwise a node is allocated and linked, and `SetWindowLongA(hwnd, GWL_WNDPROC, FUN_7375dcdc)`.
     The old procedure is saved (e236:72-83). The ANSI call is used even for a Unicode window.
   - Node flag 1, LCL and cooperative flags are stored (e236:90-92).
   - Another LCL of the process with HOOKEDHWND on the same focus window (multi-monitor) isn't hooked again
     (e3ef:28-35).
2. **Remove** (`FUN_7375e3ef(lcl, NULL, ...)`): from SetCooperativeLevel when leaving exclusive mode
   (SetCooperativeLevel.c:195-196) and from Release (`IDirectDraw7_Release.c`:88-89). Another LCL on the same focus
   window takes the node over instead (`FUN_7375e3b1`, e3ef:36-37). Otherwise, in e236:25-55:
   - flag 1 is cleared. If flag 2 remains, the hook stays;
   - **window gone** → node freed;
   - **the window's procedure is neither the hook nor the saved one** (something subclassed it after ddraw) → node
     flag 8, the node stays and **the hook stays in the chain**, forwarding only. It is removed on WM_NCDESTROY
     (dcdc:43-53) (e236:36-39);
   - **called from another thread than the window's** → node flags 0x18 and `PostMessageA(hwnd, WM_NULL)`. The hook
     unhooks itself on that thread at the next message (dcdc:47-53) (e236:41-46);
   - otherwise `KillTimer(NULL, 0x4242)`. **The window handle is NULL, so the window's 0x4242 timer isn't killed**
     (XP bug: if it is still pending, it later reaches the game's procedure). Then the old procedure is restored with
     `SetWindowLongA` and the node is freed (e236:48-52).
3. **WM_NCDESTROY (0x82)**: node flag 0x10 (dcdc:43-45). Then, as for any message with flag 0x10 (dcdc:47-54): the node
   is freed if flag 8 is set, `KillTimer(hwnd, 0x4242)`, the old procedure is restored, and the message is forwarded.
   Without flag 8 the node stays in the list (stale).

## The hook procedure — `FUN_7375dcdc`

Order for every message:

1. No node, or bad magic → `DefWindowProcA` (38-41).
2. Unhook pending (WM_NCDESTROY or cross-thread unhook) → unhook, forward (43-54).
3. **Pre-processing for any node** (DSoundHelp-only nodes too, and also when the game isn't exclusive):
   - **WM_SIZE (5)**, wParam SIZE_RESTORED or SIZE_MAXIMIZED, not inside WM_ACTIVATEAPP (flag 0x80), not ddraw's own
     operation (flag 0x40), window in the foreground → **wait on the cross-process `__DDrawExclMode__` mutex**
     (`DAT_7376f3ec`, FUN_7373149b:85) **with INFINITE timeout** and release it at once. This blocks while another
     process holds exclusive mode. Then `PostMessageA(hwnd, WM_ACTIVATEAPP, TRUE, GetCurrentThreadId())` (55-66).
     The WM_SIZE itself is forwarded.
   - **WM_ACTIVATEAPP (0x1c)**: **window iconic and wParam TRUE → swallowed, returns 0** (68-71; 0x7375ddbe →
     0x7375df53 `xor eax,eax`). Neither ddraw nor the game sees it. Otherwise node flag 0x80 (72).
   - **WM_POWER (0x48)**, wParam PWR_SUSPENDREQUEST (1) → `SendMessageA(hwnd, WM_ACTIVATEAPP, FALSE, tid)` (75-80).
     Then forwarded. WM_POWER is a 16-bit-compatibility message. Whether NT ever sends it to a Win32 window is
     uncertain.
   - **WM_KEYUP (0x101) VK_SNAPSHOT** with the registry value `HKLM\Software\Microsoft\DirectDraw\EnablePrintScreen`
     (`DAT_7376f3a0 & 8`, FUN_7373ad42:73-77) → the primary surface is copied to the clipboard (`FUN_7375dc1e` →
     `FUN_7375d82c`) (82-85). Then forwarded.
4. DSoundHelp callback (flag 2) is called with the message (86-89).
5. **No exclusive hook (flag 1 clear) → forwarded**, `CallWindowProcA` (91-97). This is the state after a
   "subclassed after us" unhook.
6. Exclusive hook, by message (98-170):
   - **WM_ACTIVATEAPP** → "Activation" below. Always forwarded, after ddraw's work, possibly with wParam changed (252).
     Result = the game's (0x7375e199-0x7375e1a9).
   - **WM_DISPLAYCHANGE (0x7e)** (100-116):
     - cooperative bit 0x2000 and the device not DDRAWI_FULLSCREEN → swallowed, returns 0 (0x7375df83 → 0x7375df53);
     - otherwise node flag 0x40. With DDSCL_FULLSCREEN and without NOWINDOWCHANGES, `FUN_7375d4d2(lcl, hwnd)`
       (below). Then forwarded to the game. Flag 0x40 is cleared afterwards. Result = the game's (0x7375dfb7, e1a9).
     - **The hook doesn't lose surfaces here.** Mode changes are noticed lazily, by each method's `GdiEntry13`
       uniqueness check.
   - **WM_SYSKEYUP (0x105)** (118-142):
     - "switch-away key" = **VK_TAB with lParam bit 29 (Alt down) and node flag 0x20 clear**, or **VK_ESCAPE**
       (any lParam);
     - node flag 0x20 is cleared on any WM_SYSKEYUP (134);
     - switch-away key and the LCL HASEXCLUSIVEMODE → `PostMessageA(hwnd, WM_ACTIVATEAPP, FALSE, tid)` (135-141).
       The key and lParam go to `DAT_7377320c`/`DAT_73773210`; those are never read (xp.dis has only the
       writes, 0x7375df2a/df32, and a clear, 0x7375d7b9);
     - then forwarded (unless the 0x2000 rule below applies).
   - **WM_SYSCOMMAND (0x112)**, LCL HASEXCLUSIVEMODE only (143-155):
     - **SC_RESTORE (0xF120)**: a saved popup (lcl+0x58 ≠ 0) → `ShowWindow(hwnd, SW_RESTORE)` (146-149). Then forwarded;
     - **SC_SCREENSAVE (0xF140)** → **swallowed, returns 0** (151-153; 0x7375df53). Exact compare: wParam isn't masked
       with 0xFFF0;
     - **SC_MONITORPOWER and every other command are forwarded.** HASEXCLUSIVEMODE stays set in the background, so
       SC_SCREENSAVE is swallowed there too.
   - **WM_TIMER (0x113) id 0x4242** (156-163): if the window is in the foreground and not iconic →
     `FUN_7375db9a` (re-assert topmost, below). **`KillTimer(hwnd, 0x4242)` in all cases** (one-shot). Swallowed,
     returns 0 (0x7375dec3 → df53).
   - **Any message while node flag 0x40 is set, with cooperative bit 0x2000 → swallowed, returns 0** (164-166).
     Without 0x2000 (all DX1-7 games), messages caused by ddraw's own ShowWindow/SetWindowPos reach the game.
   - Everything else → forwarded with `CallWindowProcA`, game's result (167-169).
   - **Not handled**: WM_SIZE (except the pre-processing above), WM_WINDOWPOSCHANGED, WM_ACTIVATE, WM_SETFOCUS,
     WM_KILLFOCUS, WM_GETMINMAXINFO, WM_SYSKEYDOWN, mouse messages, WM_POWERBROADCAST. All go to the game unchanged.

### Activation — WM_ACTIVATEAPP with an exclusive hook (dcdc:171-258)

Nothing happens unless the LCL has HASEXCLUSIVEMODE (171). That flag stays set in the background; only the exclusive
*owner* (gbl+0x3f8) changes.

1. **active** = wParam ≠ 0 **and** the window isn't iconic **and** `GetForegroundWindow() == hwnd` (172-183).
   Otherwise active = FALSE, and **a TRUE wParam is rewritten to FALSE for the game** (174-177). Example: the
   activation went to another window of the process, such as a popup.
2. `EnterCriticalSection` (ddraw's global, re-entrant lock) and the display-uniqueness refresh `FUN_7373cc76`
   (184-190).
3. **Same state as already recorded → nothing** (191-195): deactivation with ACTIVENO already set, or activation with
   ACTIVEYES already set. The game still gets the message. On activation, the popup focus step (7) still runs.
4. Activation only: node flag 0x20 = Alt (VK_MENU) currently down, by `GetAsyncKeyState` (196-204).
5. ACTIVEYES/ACTIVENO set to the new state **before** the work (206-213). The ClipCursor gate in `FUN_7375d3c8`
   depends on this order.
6. `FUN_7375d5aa(lcl, node, active, 1)` (214). Then the same for **every other LCL** that has a different GBL, the same
   focus window, HASEXCLUSIVEMODE, and DISPLAYDRV on both devices (multi-monitor exclusive), with param_4 = 0 (215-232).
7. Activation: a saved popup (lcl+0x58) that still exists gets `SetFocus`, then lcl+0x58 = 0 (234-240).
8. `LeaveCriticalSection` (242-243). Then a pending IME-UI change is applied outside the lock:
   `SystemParametersInfoA(SPI_SETSHOWIMEUI (0x6f), DAT_7376f9a0 - 1)` (244-247). `FUN_7375ac27`:40 sets it to
   "hide" (value 1 → 0). `FUN_7375aa94`:24 sets it to "the saved state" (lcl+0x54 + 1).
9. Deactivation: `PostMessageA(hwnd, 0x1634, 0xFFBADADD, 0xFFADDBAD)` if `DAT_73773208 ≠ 0` (248-250). Nothing
   writes `DAT_73773208` (only the read at 0x7375e162 in xp.dis), so this never happens.
10. **Forward WM_ACTIVATEAPP to the game** with the possibly rewritten wParam (252). Node flag 0x80 cleared (254-257).

### `FUN_7375d5aa(lcl, node, active, isMain)` — the work

Names: *fs-window* = the hooking call had DDSCL_FULLSCREEN, not DDSCL_NOWINDOWCHANGES, and `IsWindowVisible(hwnd)`
(d5aa:52-53, 88-89). *owner* = this LCL is the exclusive owner, read before any change (`FUN_73734625`, 34).

**Focus lost** (active = 0):
1. Owner with a primary → **FlipToGDISurface** (`FUN_73734deb`, 36-39). Owner → **every surface of the device is
   lost** (`FUN_73760648`, 40-42; system-memory surfaces aside, see ddraw-object.md). Not owner and a primary
   exists → steps 1-2 are skipped (37).
2. Nobody owns exclusive mode, or this LCL does → **the desktop display mode is restored**: `FUN_73734f22(lcl, 1)`
   (66-67) → `ChangeDisplaySettingsExA(dev, NULL, NULL, CDS_FULLSCREEN)`. It acts only if this LCL changed the mode
   (MODEHASBEENCHANGED; f22:13, 32-34). This happens **with DirectDraw 1 (V1SCLBEHAVIOUR) too**. That mode change
   sends WM_DISPLAYCHANGE synchronously. Its handler moves the window to the desktop-size device rect; no topmost
   because the game isn't in front, no clip because ACTIVENO is already set (`FUN_7375d4d2` → `FUN_7375d40c`).
   - Mode restore failed (DDERR_UNSUPPORTED) and the input desktop isn't the thread's desktop (secure desktop:
     Ctrl+Alt+Del, a locked workstation) → **the window won't be minimized** (68-77).
3. Owner → `FUN_7375aa94` (80-83):
   - fullscreen display driver only: the error mode is restored, the "DDrawCheckFullscreenSemaphore" is released
     (`FUN_7375aa67`), SPI_SETSCREENSAVERRUNNING FALSE (unless dwAppHackFlags & 0x10), and mouse sonar, hot tracking
     and IME UI are restored (aa94:16-25);
   - DDRAWI_FULLSCREEN and FLIPPEDTOGDI are cleared (26);
   - a palettized device: `SetSystemPaletteUse(SYSPAL_STATIC)`, then `WM_PALETTECHANGED` is broadcast (27-44);
   - the mode is restored again unless V1SCLBEHAVIOUR. This is a no-op after step 2 succeeded (45-47);
   - gamma restored (`FUN_737343fb`, 48-51), the driver's SetExclusiveMode(FALSE) (52-62);
   - **the owner is cleared** (gbl+0x3f8 = 0, 63), the `__DDrawExclMode__` mutex is released (64-66);
   - **`ClipCursor(NULL)` if this LCL clipped it** (CURSORCLIPPED; `FUN_7375d3c8(lcl, NULL)`, 67; d3c8:8-14, 28).
4. fs-window and not the "secure desktop" case: node flag 0x40, the last active popup is saved in lcl+0x58 (if it
   isn't the window itself), and **`ShowWindow(hwnd, SW_SHOWMINNOACTIVE)` (7)** (88-100).
   - **No `SetWindowPos(HWND_NOTOPMOST)`**: the window is only minimized and keeps WS_EX_TOPMOST.
   - **No minimize with DDSCL_NOWINDOWCHANGES**, without DDSCL_FULLSCREEN, or for a hidden window.
5. isMain → `DAT_7377320c = 0` (106-108; dead data, see WM_SYSKEYUP).

**Focus regained** (active = 1):
1. ISFULLSCREEN → device DDRAWI_FULLSCREEN (45-47).
2. `FUN_7375ac27(lcl)` (48) — **taking exclusive mode back**:
   - `DisableProcessWindowsGhosting()` (ac27:19-26), every time (it is permanent for the process anyway);
   - **owner = this LCL** (27);
   - fullscreen display driver only (28-41): "DDrawCheckFullscreenSemaphore" created, `SetErrorMode(SEM_FAILCRITICALERRORS |
     SEM_NOOPENFILEERRORBOX)` (old mode saved), **SPI_SETSCREENSAVERRUNNING TRUE** (unless dwAppHackFlags & 0x10; that bit's
     name is uncertain), mouse sonar saved and turned off, hot tracking saved and turned off, the IME UI state saved
     (lcl+0x54). Hiding it is applied by the hook afterwards (step 8 above);
   - **every surface of the device is lost again** (`FUN_73760648`, 43). Surfaces "restored" in the background
     are lost once more;
   - gamma reapplied (`FUN_737642d1`, 44-47), the driver's SetExclusiveMode(TRUE) (48-58).
3. fs-window → node flag 0x40, **`ShowWindow(hwnd, SW_SHOWNOACTIVATE)` (4)** (60-64). On a minimized window this
   restores it without activating.
4. **The game's display mode is set again, always**: `FUN_73761aa2(lcl, lcl->dwPreferredMode, 1, 1)` (56). It uses the
   LCL's expected mode (lcl+0x60 w, +0x62 h, +0x64 bpp, +0x66 refresh; FUN_73761aa2:33-52), skips the busy check
   (param 3) and keeps the stored refresh rate (param 4). It sets MODEHASBEENCHANGED (aa2:13). This doesn't
   depend on fs-window. Its WM_DISPLAYCHANGE runs the hook's handler (placement, clip, topmost, below).
5. The primary's palette is set again, so it is realized (`FUN_73766da1(primary, palette)`, 85-87).
6. fs-window → node flag 0x40 and **`FUN_7375d40c(lcl, lcl->hWnd)`** (101-103), "Placing the window".
7. **No SetForegroundWindow on reactivation.** `FUN_7375d557` (SetForegroundWindow with the foreground lock timeout
   zeroed and a fake mouse event) is only called by SetCooperativeLevel (SetCooperativeLevel.c:238).

### Placing the window — `FUN_7375d40c(lcl, hwnd)`

Called from SetCooperativeLevel's hook install (e3ef:23-26), reactivation (d5aa:102), and WM_DISPLAYCHANGE through
`FUN_7375d4d2` (d4d2:8, plus 17 for other LCLs on the same focus window and other devices).
1. `SetWindowPos(hwnd, NULL, device rect, SWP_NOZORDER|SWP_NOACTIVATE)` (d40c:10-13). The device rect is the monitor
   rect for a secondary device, otherwise `0,0,SM_CXSCREEN,SM_CYSCREEN` (`FUN_7375d369`). Frame included; the style
   is untouched.
2. **`ClipCursor(device rect)`** via `FUN_7375d3c8` (14). It only clips when the LCL has HASEXCLUSIVEMODE and
   ACTIVEYES and not EXPLICITMONITOR, and then sets CURSORCLIPPED (d3c8:15-28). So there is no clip in the
   background or during deactivation.
3. Focus window in the foreground (16-27): hidden → `ShowWindow(SW_SHOW)`.
   **`SetWindowPos(hwnd, HWND_TOPMOST, SWP_NOSIZE|SWP_NOMOVE|SWP_NOACTIVATE)`**. Maximized →
   `ShowWindow(SW_RESTORE)`.
4. The window goes in a 15-entry list (`DAT_73775680`, 28-31). **`SetTimer(focus window, 0x4242, 1500 ms)`** (32).
5. 1.5 s later (WM_TIMER above), if the window is still in front and not iconic, `FUN_7375db9a` gives every listed
   window (each once) **`SetWindowPos(HWND_TOPMOST, SWP_NOSIZE|SWP_NOMOVE|SWP_NOACTIVATE)` again**, under node flag
   0x40, and empties the list (db9a:13-49). This re-asserts topmost against anything that went topmost meanwhile.

`FUN_7375d4d2` (WM_DISPLAYCHANGE) empties the list first (d4d2:7). It also places other LCLs' device windows that
share this focus window, unless `FUN_73731bb1` (display-device enumeration; its return value was lost by the
decompiler) returns non-zero while this LCL is ACTIVENO (d4d2:9-21). Exact meaning uncertain.

### Re-entrancy

- ddraw's global critical section is re-entrant. The activation work runs inside it (184-243), so a game calling
  DirectDraw from a message sent during that work runs normally on the same thread. Another thread blocks.
- Node flag 0x80 (inside WM_ACTIVATEAPP) and 0x40 (own window operation) stop the WM_SIZE rule from posting another
  WM_ACTIVATEAPP: for ddraw's own `ShowWindow(SW_SHOWNOACTIVATE)` (d5aa:61-63), its minimize, and its
  topmost/placement calls. **They don't stop the messages from reaching the game** (only bit 0x2000 does).
- ACTIVEYES/ACTIVENO make a repeated WM_ACTIVATEAPP in the same direction a no-op (dcdc:191-195).
- Messages ddraw generates itself go through the same hook synchronously: WM_DISPLAYCHANGE from its
  ChangeDisplaySettings, and WM_SIZE/WM_WINDOWPOSCHANGED from its ShowWindow and SetWindowPos.
- Posted, not sent: the synthetic WM_ACTIVATEAPP from WM_SIZE (63) and WM_SYSKEYUP (139). Sent: from WM_POWER (78).
  All carry the game's own thread id in lParam.

### Summary: focus loss and regain in exclusive mode (one exclusive object, DDSCL_FULLSCREEN, window visible)

| | Focus lost | Focus regained |
|---|---|---|
| Trigger | the system's WM_ACTIVATEAPP(FALSE); **ddraw's own posted FALSE on Alt+Tab key-up / Alt+Esc key-up**; a TRUE that isn't really for this window (rewritten) | the system's WM_ACTIVATEAPP(TRUE) while not iconic and in front; **ddraw's own posted TRUE on WM_SIZE restored/maximized while in front** |
| Surfaces | FlipToGDISurface, then all lost (owner) | all lost again; primary palette re-set |
| Display mode | desktop mode restored (DD1 too) | game's mode set again (always, with stored rate) |
| Window | **minimized (SW_SHOWMINNOACTIVE)**, popup remembered; topmost untouched | SW_SHOWNOACTIVATE, moved over device rect, **HWND_TOPMOST** (in front only), un-maximized, re-asserted after 1.5 s; popup gets SetFocus |
| Cursor | ClipCursor(NULL) if ddraw clipped | ClipCursor(device rect) |
| Exclusive owner | cleared (mutex released) | taken back |
| System settings | screensaver flag, sonar, hot tracking, IME UI, error mode restored | saved and turned off; ghosting disabled |
| Game sees | WM_ACTIVATEAPP(FALSE) **after** all of the above | WM_ACTIVATEAPP(TRUE) **after** all of the above; **nothing at all while iconic** |
| NOWINDOWCHANGES | no minimize | no show, no placement, no topmost, no clip; mode still set |

## dd2d9's equivalent

- `HookProc` (ddraw.cpp:403-488), installed by `HookWindow` (496-535) in exclusive mode
  (`SetCooperativeLevel`, 1049) and removed by `UnhookWindow` (537-548) when leaving exclusive mode (1019) and in
  `TearDown` (124). The same "something subclassed after us: stay, forwarding only" rule applies (543). It uses
  W/A per window, chained to the original procedure.
- WM_ACTIVATEAPP → `OnActivate(wp != 0, lp)` (426-435, 550-593), before forwarding. If `g_displayLock` is busy, it is
  retried later with a registered `DD2D9_DeferredActivate` message. It is ignored while `applying_` (551).
  - Lost (554-577): ClipCursor off, then in switch/borderless: drop the D3D9 device, restore the desktop mode if
    switched, **`SetWindowPos(HWND_NOTOPMOST)`** (568), surfaces `kDevLost`, `ShowWindow(SW_SHOWMINNOACTIVE)` if
    visible (575).
  - Regained (579-592): surfaces lost (kDevNotRestored). If it was suspended: `ShowWindow(SW_RESTORE)` if iconic, then
    `ApplyWindow` (mode switch, cover, topmost, `ConfineCursor`). Otherwise HWND_TOPMOST and `ConfineCursor`.
- WM_DISPLAYCHANGE (436-451): surfaces lost unless in dd2d9's own mode, then `OnDisplayChange` (597-602):
  `CoverMonitor(HWND_TOP)` unless iconic/NOWINDOWCHANGES/window mode. Then forwarded.
- WM_SYSCOMMAND SC_SCREENSAVE (masked with 0xFFF0) swallowed while `Exclusive()` (474-478). SC_MONITORPOWER goes
  through: **matches XP** in the current source.
- dd2d9's own messages: WM_GETMINMAXINFO (frame-off-screen cover), mouse coordinate translation, `DD2D9_CheckCover`
  (window covering the monitor again after the game restyles it, `CheckCover` 697). These are deliberate, for
  display=borderless/window and the cover technique.
- `ConfineCursor` (670-689) also calls **`LockSetForegroundWindow(LSFW_LOCK)` while in front: deliberate, not XP
  behaviour** (XP never calls it).
- display=window: no minimize and no lost surfaces on focus loss (dd2d9's addition; XP has no equivalent).

## Differences from dd2d9

Ranked game-visible first. "display=switch" is the mode to compare directly. For borderless/window only XP's intent
applies (go out of the way on focus loss, come back covering the screen).

1. **DDSCL_NOWINDOWCHANGES games are minimized and made non-topmost on focus loss** (game-visible).
   - XP: minimizes only a "fs-window": FULLSCREEN, no NOWINDOWCHANGES, visible (FUN_7375d5aa:88-100). Mode restore and
     surface loss happen regardless.
   - dd2d9: `cover` (ddraw.cpp:558) ignores `coop_ & DDSCL_NOWINDOWCHANGES`, so `SetWindowPos(HWND_NOTOPMOST)` (568) and
     `ShowWindow(SW_SHOWMINNOACTIVE)` (575) touch a window the game asked to be left alone. On regain,
     `ShowWindow(SW_RESTORE)` (582) restores it too.
   - Fix: `const bool windowChanges = !(coop_ & DDSCL_NOWINDOWCHANGES);` Do the NOTOPMOST, minimize and SW_RESTORE
     only when `windowChanges`. Keep the device drop and mode restore unconditional (as XP's f22 call, d5aa:66-67).
2. **WM_ACTIVATEAPP(TRUE) while minimized is swallowed; activation comes from WM_SIZE instead** (game-visible:
   message order and count).
   - XP: an iconic window's WM_ACTIVATEAPP(TRUE) never reaches the game, and ddraw ignores it (dcdc:68-71, returns 0).
     When the window is then restored or maximized while in front, ddraw waits for the cross-process exclusive mutex
     and **posts** WM_ACTIVATEAPP(TRUE). That message does the reactivation, and the game gets it after the mode is
     back (dcdc:55-66).
   - dd2d9: forwards the iconic TRUE. `OnActivate(true)` restores the window itself (582) and reactivates. There is
     no WM_SIZE rule: a game that restores its own minimized window while in front, without a new WM_ACTIVATEAPP,
     isn't reactivated until the next one.
   - Fix: in `HookProc`, `if (msg == WM_ACTIVATEAPP && wp && IsIconic(h) && owner && owner->Exclusive()) return 0;`.
     On `WM_SIZE` with SIZE_RESTORED/SIZE_MAXIMIZED, not during dd2d9's own operations (`applying_`, and a new flag set
     around OnActivate's ShowWindow calls), window in the foreground and exclusive →
     `PostMessage(h, WM_ACTIVATEAPP, TRUE, GetCurrentThreadId())`. The mutex wait is cross-process, so it can be
     skipped. Keep the `IsIconic → SW_RESTORE` in OnActivate as a fallback.
3. **WM_ACTIVATEAPP(TRUE) when the window isn't the foreground window is a deactivation, and the game is told FALSE**
   (game-visible, uncommon: activation through another window of the process, such as a dialog).
   - XP: active = wParam && !iconic && foreground == hwnd; otherwise wParam is rewritten to 0 for the game (dcdc:172-183).
     The game's last active popup is remembered on deactivation and gets `SetFocus` on reactivation (d5aa:93-97,
     dcdc:234-240). SC_RESTORE shows the window again if such a popup exists (dcdc:145-149).
   - dd2d9: `OnActivate(wp != 0)` (430) activates (mode switch, topmost over the dialog) and forwards TRUE.
   - Fix: compute `active = wp && !IsIconic(h) && GetForegroundWindow() == h` for an exclusive owner. Pass it to
     OnActivate and forward `WPARAM(active)` when `wp` was TRUE. Popup handling: optional.
4. **Alt+Tab / Alt+Esc deactivate immediately** (game-visible on key handling; may matter where the system's own
   switch is slow or doesn't happen).
   - XP: WM_SYSKEYUP of VK_ESCAPE, or of VK_TAB with Alt down (bit 29), unless Alt was already held when the game was
     activated (node flag 0x20, dcdc:196-204), → posts WM_ACTIVATEAPP(FALSE) to itself (dcdc:118-142). The game
     deactivates (mode back, minimized) even if the system didn't switch: e.g. Alt+Esc with no other window, or a
     quick Alt+Tab back to the same game. The key-up is still forwarded.
   - dd2d9: no WM_SYSKEYUP handling. It deactivates only on the system's WM_ACTIVATEAPP(FALSE).
   - Fix: copy the rule in display=switch/borderless: an `altAtActivate_` flag set in OnActivate(true) from
     `GetAsyncKeyState(VK_MENU) < 0` and cleared on every WM_SYSKEYUP. Post `WM_ACTIVATEAPP(FALSE, GetCurrentThreadId())`
     for the two keys. Because of `LockSetForegroundWindow` (deliberate), check that a real Alt+Tab still reaches the
     switched-to window.
5. **Topmost is not re-asserted after 1.5 s, and XP's minimize keeps WS_EX_TOPMOST** (edge case, visible with
   overlays/notifications).
   - XP: every placement sets a one-shot 1.5 s timer 0x4242. If still in front then, every placed window is made
     HWND_TOPMOST again, and the WM_TIMER is swallowed (FUN_7375d40c:32, dcdc:156-163, FUN_7375db9a). On deactivation
     XP never calls HWND_NOTOPMOST (d5aa has no SetWindowPos); only leaving exclusive mode does
     (SetCooperativeLevel.c:211).
   - dd2d9: topmost once on regain (590 or via ApplyWindow), and HWND_NOTOPMOST on loss (568).
   - Fix: optional. After `ApplyWindow`/topmost in OnActivate, `SetTimer(hwnd_, someId, 1500, nullptr)`. In HookProc,
     swallow that WM_TIMER, re-apply HWND_TOPMOST if the window is still in front and not iconic, and kill the timer.
     Keeping the NOTOPMOST on loss is harmless for a minimized window (needed for borderless, where nothing else
     un-tops it); not copying XP here is acceptable.
6. **Reactivation ShowWindow value** (edge case).
   - XP: `ShowWindow(SW_SHOWNOACTIVATE)` (d5aa:62), then the mode switch, then placement + topmost (d5aa:56, 102).
     A maximized window is un-maximized (`SW_RESTORE`, d40c:23-26).
   - dd2d9: `ShowWindow(SW_RESTORE)` (582) only if iconic, which activates the window. In practice it is already the
     foreground window.
   - Fix: use `SW_SHOWNOACTIVATE` to match. No game impact is known.
7. **No FlipToGDISurface on focus loss** (edge case).
   - XP: the exclusive owner flips its primary chain back to the GDI surface before losing surfaces (d5aa:36-39). After
     reactivation and Restore, the chain is in its original order.
   - dd2d9: OnActivate(false) doesn't flip. The chain order after regain is whatever it was.
   - Fix: in OnActivate(false), for the exclusive owner with a primary flip chain, call `FlipToGDISurface()` before
     `kDevLost`. Check the lock order: OnActivate holds `g_displayLock`.
8. **System settings while exclusive and in front** (edge case; one item may be game-visible).
   - XP, on every activation of a fullscreen display driver (FUN_7375ac27:19-41), undone on loss (FUN_7375aa94:16-25):
     - **`DisableProcessWindowsGhosting()`**: a game that stops pumping messages (loading) isn't replaced by a
       "Not Responding" ghost window;
     - SPI_SETSCREENSAVERRUNNING TRUE;
     - mouse sonar and hot tracking off;
     - IME UI hidden (SPI_SETSHOWIMEUI via `DAT_7376f9a0`, dcdc:244-247);
     - `SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX)`.
   - dd2d9: none of these.
   - Fix: call `DisableProcessWindowsGhosting()` once, on the first exclusive activation in display=switch/borderless.
     On modern Windows a ghost window would cover a busy fullscreen game and break the topmost/cover logic. The
     SPI changes are system-wide and risky if the process crashes; skip them. Optionally hide the IME UI (CJK users)
     and restore it on loss.
9. **Activation ignored during dd2d9's own window operations; no same-state check** (edge case).
   - XP: processes every WM_ACTIVATEAPP. Only the WM_SIZE rule is suppressed during its own operations (flags
     0x40/0x80). A repeat in the same direction is a no-op via ACTIVEYES/ACTIVENO (dcdc:191-195).
   - dd2d9: `OnActivate` returns at once while `applying_` (551). A real focus loss during `ApplyWindow` (e.g. the
     user Alt+Tabs while the mode switches) is dropped and the game stays "in front". A repeated TRUE re-runs
     topmost + ConfineCursor.
   - Fix: when `applying_`, post the deferred-activate message instead of dropping it (as the busy-lock path does,
     430-434). Re-check `GetForegroundWindow()` when it is handled, so the activations dd2d9 causes itself are
     still discarded.
10. **WM_DISPLAYCHANGE placement details** (edge case).
    - XP: always moves the window over the new device rect, z-order kept. Topmost and ClipCursor only if in front and
      ACTIVEYES (FUN_7375d4d2 → FUN_7375d40c). It doesn't lose surfaces in the hook (that happens lazily by display
      uniqueness).
    - dd2d9: `CoverMonitor(HWND_TOP, SWP_NOACTIVATE)` unless iconic (597-602), with no re-clip. A mode change made by
      another program while the game is in front leaves the clip rect at the old size. dd2d9's own switches re-clip in
      ApplyWindow (784).
    - Fix: after `CoverMonitor` in OnDisplayChange, `if (GetForegroundWindow() == hwnd_) ConfineCursor(true);`.
11. **Unhooking details** (edge case).
    - XP: unhooks on WM_NCDESTROY (dcdc:43-53). From another thread it defers to the window's thread with a posted
      WM_NULL (e236:41-46). Its `KillTimer(NULL, 0x4242)` misses the window's timer (e236:48), an XP bug not to copy.
    - dd2d9: no WM_NCDESTROY handling. `g_hook.hwnd` can name a dead window until the next `HookWindow`, which checks
      `IsWindow` (502). It unhooks from any thread (`SetWindowLongPtr` works across threads of one process).
    - Fix: in HookProc, on `WM_NCDESTROY` for `g_hook.hwnd`, restore `g_hook.orig` and clear `g_hook.hwnd/owner`
      before forwarding.
12. **SC_SCREENSAVE matching** (edge case). XP compares wParam with 0xF140 exactly (dcdc:151). dd2d9 masks with 0xFFF0
    (474). XP also swallows only for the LCL that owns the hook, in the background too; dd2d9's `Exclusive()` is the
    same condition. Fix: none needed. **SC_MONITORPOWER is forwarded by both (confirmed: dcdc:143-155 handles only
    0xF120 and 0xF140).**
13. **Deliberate, not XP**: `LockSetForegroundWindow(LSFW_LOCK)` while in front (ConfineCursor, 677/685). Also the
    frame-off-screen cover with WM_GETMINMAXINFO, CheckCover, mouse translation, the display=window/borderless
    behaviour, and minimize/NOTOPMOST for borderless.

Not copied, with no game impact expected: the EnablePrintScreen clipboard copy (registry-gated, off by default);
WM_POWER suspend → deactivation (dcdc:75-81); the 0x1634 broadcast (unreachable); the cooperative-bit-0x2000 rules;
DSoundHelp nodes; multi-monitor propagation to other LCLs sharing a focus window (dcdc:215-232; dd2d9 has one
exclusive object).

## Confirmed in passing

- XP swallows SC_SCREENSAVE for an exclusive LCL and forwards SC_MONITORPOWER (dcdc:143-155, return 0 at 0x7375df53).
  The current dd2d9 source matches (474-478).
- XP restores the desktop mode on focus loss and sets the game's mode again on regain, losing all surfaces both times
  (d5aa:41, 66-67, 56; ac27:43).
- XP's ClipCursor is on the device rect and only while ACTIVEYES. It is undone on loss (d3c8, aa94:67).
- The game always gets WM_ACTIVATEAPP after ddraw has done its work (dcdc:252), as in dd2d9 (OnActivate before
  forwarding).

## Not determined

- Who sets cooperative bit 0x2000, and whether any DX1-7 path does (affects the "swallow during own operations" and
  WM_DISPLAYCHANGE rules).
- The name of dwAppHackFlags bit 0x10 (skips SPI_SETSCREENSAVERRUNNING); which registry/app-compat entry sets it.
- `FUN_73731bb1`'s return value (lost by the decompiler) in the WM_DISPLAYCHANGE multi-monitor branch (d4d2:9-10).
- Whether XP ever delivers WM_POWER (0x48) to a Win32 window, which makes the suspend → deactivate rule reachable.
- The exact message order Windows XP produces for "click the taskbar button of a minimized exclusive game" and for
  Alt+Tab into it. The analysis above assumes WM_ACTIVATEAPP(TRUE) arrives while still iconic, which is why the
  WM_SIZE rule exists. Worth confirming with a message trace on XP before implementing difference 2.
- What `SetWindowPos` to the device rect does to a minimized window during the deactivation-time WM_DISPLAYCHANGE
  (it probably changes only the restored placement).
- The return value of the hook for messages it handles before forwarding is always the game's. For swallowed ones it is
  0. For the "no node" `DefWindowProcA` path the decompile shows no return value (it is presumably DefWindowProcA's
  result).
