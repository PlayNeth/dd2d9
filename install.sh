#!/bin/sh
# Install dd2d9 into a Wine prefix.
#
#   ./install.sh                       global: every program in the prefix uses dd2d9
#   ./install.sh --app game.exe ...    only these executables use dd2d9 (others keep Wine's ddraw)
#   ./install.sh --uninstall
#
# Uses $WINEPREFIX (default ~/.wine) and the wine in PATH, which should be the Wine the prefix is used with (any
# wine command brings a prefix to its own version).  After a Wine upgrade run this script again: dd2d9's DLL stays
# in place, but the copy of Wine's own kept beside it (ddraw.dll.wine) is the old Wine's until it has.
set -e
PREFIX="${WINEPREFIX:-$HOME/.wine}"
case "$PREFIX" in /*) ;; *) PREFIX="$PWD/$PREFIX" ;; esac     # (the directory changes below)
cd "$(dirname "$0")"
DLL="build/ddraw.dll"
if [ -f ddraw.dll ]; then DLL="ddraw.dll"; fi       # (a release package: the DLL next to this script)
export WINEPREFIX="$PREFIX"
export WINEDEBUG=-all

[ -d "$PREFIX/drive_c/windows" ] || { echo "no Wine prefix at $PREFIX"; exit 1; }

# 32-bit system dir: syswow64 in a 64-bit prefix, system32 in a 32-bit one
if [ -d "$PREFIX/drive_c/windows/syswow64" ]; then SYS="$PREFIX/drive_c/windows/syswow64"
else SYS="$PREFIX/drive_c/windows/system32"; fi

# Wine's own DLL is kept beside dd2d9's as ddraw.dll.wine.  It has to be this Wine's: a Wine upgrade leaves dd2d9's
# DLL in place, and the copy kept would be the Wine before's.  So with dd2d9's in place already, it is removed first
# and Wine asked to put its own back (wineboot -u does that for a DLL that is missing); only if it does not is the
# copy kept before used again.  A copy kept that is a link (a prefix whose system DLLs are links into the Wine it
# uses) is this Wine's as it is, and goes back as the link it was.
is_dd2d9() {    # dd2d9's ddraw.dll (its log's first line, "dd2d9 <version>")
    [ -f "$1" ] && grep -q 'dd2d9 [0-9][0-9]*\.[0-9]' "$1"
}
there() { [ -e "$1" ] || [ -L "$1" ]; }
wine_back() {   # Wine's own ddraw.dll in place again
    f="$SYS/ddraw.dll"
    # (the copy as this script kept it up to dd2d9 1.9.2, and the DLL an older one put behind D7VK)
    if there "$f.wine-orig"; then
        if there "$f.wine"; then rm -f "$f.wine-orig"; else mv -f "$f.wine-orig" "$f.wine"; fi
    fi
    rm -f "$SYS/ddraw_.dll"
    if is_dd2d9 "$f" || { [ -L "$f" ] && [ ! -e "$f" ]; }; then rm -f "$f"; fi
    if [ -L "$f.wine" ] && [ ! -e "$f.wine" ]; then rm -f "$f.wine"; fi     # (to a Wine gone)
    if ! there "$f"; then
        if [ -L "$f.wine" ]; then mv -f "$f.wine" "$f"
        else
            wine wineboot -u >/dev/null 2>&1 || true
            wineserver -w 2>/dev/null || true
        fi
    fi
    if there "$f" || is_dd2d9 "$f.wine"; then rm -f "$f.wine"
    elif there "$f.wine"; then mv -f "$f.wine" "$f"; fi
}

case "$1" in
    ""|--uninstall) ;;
    --app) [ $# -gt 1 ] || { echo "--app: name the executables (game.exe ...)"; exit 1; } ;;
    *) sed -n '2,6p' "$(basename "$0")" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
command -v wine >/dev/null 2>&1 || { echo "no wine in PATH"; exit 1; }

if [ "$1" = "--uninstall" ]; then
    wine_back
    wine reg delete 'HKCU\Software\Wine\DllOverrides' /v ddraw /f >/dev/null 2>&1 || true
    echo "dd2d9 removed from $SYS (per-app overrides under AppDefaults are left; they now fall back to builtin;"
    echo "dd2d9.ini is kept)"
    exit 0
fi

[ -f "$DLL" ] || { echo "no $DLL: build dd2d9 first (see README.md), or run this from a release package"; exit 1; }
wine_back
# dd2d9's DLL in the system directory, Wine's moved aside (moved, and the place written anew: dd2d9 must not be
# written through a link; and written beside it first, so that the place is never empty for longer than a rename)
cp -f "$DLL" "$SYS/ddraw.dll.dd2d9"
if there "$SYS/ddraw.dll"; then mv -f "$SYS/ddraw.dll" "$SYS/ddraw.dll.wine"; fi
mv -f "$SYS/ddraw.dll.dd2d9" "$SYS/ddraw.dll"
echo "installed $DLL -> $SYS/ddraw.dll"

if [ ! -f "$PREFIX/drive_c/windows/dd2d9.ini" ] && [ -f dd2d9.ini.example ]; then
    cp dd2d9.ini.example "$PREFIX/drive_c/windows/dd2d9.ini"
    echo "config: $PREFIX/drive_c/windows/dd2d9.ini"
fi

if [ "$1" = "--app" ]; then
    shift
    for exe in "$@"; do
        wine reg add "HKCU\\Software\\Wine\\AppDefaults\\$exe\\DllOverrides" /v ddraw /d native,builtin /f >/dev/null
        echo "override: $exe -> ddraw=native,builtin"
    done
else
    wine reg add 'HKCU\Software\Wine\DllOverrides' /v ddraw /d native,builtin /f >/dev/null
    echo "override: global ddraw=native,builtin"
fi
