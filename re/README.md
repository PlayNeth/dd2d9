# dd2d9 – reverse-engineering notes: ddraw.dll (XP SP3)

Target: `ddraw.dll` 5.3.2600.5512 (xpsp.080413-0845), Swedish XP SP3, x86, image base 0x73730000
SHA-256: 3006f1c887a799091c31d1d50129108c36c20dbffd6a2c2ab9a8ff8689994425
PDB: ddraw.pdb, key DF483A833685447F9F96D93012393C342
<https://msdl.microsoft.com/download/symbols/ddraw.pdb/DF483A833685447F9F96D93012393C342/ddraw.pdb>

## Findings
- This build has no static vtables. `InitInterfaceVtables` (0x73731cd2) fills 25 tables
  in .data (0x7376f020–0x7376f99c) at load time. They were recovered by emulating that routine.
- Every slot was checked against the ddraw.h prototypes using the callee's `ret N` stack size:
  494 slots checked, 4 mismatches. All 4 are in the "uninitialized object" stub tables or in
  IDDVideoPortContainer::CreateVideoPort, none of which dd2d9 needs.
- The code is BBT-optimized: function bodies are split into hot and cold blocks scattered through
  .text. Expect odd control flow in the decompiler.
- Older interfaces share implementations. For example, Surface/2/3/4/7 Blt are a single function,
  but Surface/2/3 Unlock (LPVOID) and Surface4/7 Unlock (LPRECT) are separate.

## Where the notes in `spec/` are behind dd2d9
The notes compare `ddraw.dll` with dd2d9 as it was when each was written; their line numbers
and their lists of differences are of that time. Since then (the CHANGELOG has each):

- dd2d9 has colour-key ranges, overlays, the video port container, `IDirectDrawColorControl`,
  the gamma calibrator, `DDSCAPS_OWNDC`, the PrintScreen copy, GDI's palette of the screen,
  and exclusive mode's system settings, semaphore and activation as `ddraw.dll`'s (1.9.0).
  Where a note says dd2d9 lacks one of these, or lists it as "not copied", it is out of date.
- Deliberately not as XP since 1.9.1, and not in the notes' lists:
  - an override key (`DDBLT_KEYSRCOVERRIDE`, `DDBLT_KEYDESTOVERRIDE`) is its low value alone
    (XP with the Voodoo3's driver compared the range in video memory, and the emulation layer
    refused a destination override's range);
  - a `DDPCAPS_ALLOW256` palette keeps its first and last entries on the primary (`ddraw.dll`
    forces them by the surface's flags, FUN_73751e14: see `spec/palette-clipper-exports.md`);
  - the system settings exclusive mode changes are put back when the game exits without
    leaving it (`ddraw.dll`'s unloading, FUN_7373149b, only frees its record of the process).

## Files
- `ddraw_xpsp3_symbols.json`: 178 named functions (addresses, aliases, stack bytes) and 25 vtables.
- `ghidra_name_ddraw.py`: run it from Ghidra's Script Manager after auto-analysis.
- `ida_name_ddraw.py`: File → Script file in IDA.

Load the PDB as well when you can. It names the internal helpers that these scripts don't cover.
