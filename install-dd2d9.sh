#!/usr/bin/env bash
# Installs dd2d9's ddraw.dll into a Wine prefix's 32-bit system directory.
#
#   install-dd2d9.sh install <dd2d9 ddraw.dll>                     dd2d9 replaces Wine's ddraw
#   install-dd2d9.sh uninstall                                    restores Wine's ddraw
#
# Also creates C:\windows\dd2d9.ini in the prefix from dd2d9.ini.example if there is none
# (uninstall keeps it).
#
# The prefix is taken from $WINEPREFIX (default: ~/.wine). Nothing is downloaded.
set -euo pipefail

prefix="${WINEPREFIX:-$HOME/.wine}"
backup_name="ddraw.dll.wine-orig"

die() { echo "error: $*" >&2; exit 1; }

usage() {
    sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

# Prints "<machine> <characteristics>" from the PE header (machine 014c = 32-bit x86).
pe_info() {
    local f="$1" off
    [ "$(head -c 2 "$f")" = "MZ" ] || { echo "none 0"; return; }
    off=$(od -An -t u4 -j 60 -N 4 "$f" | tr -d ' ')
    [ "$(od -An -c -j "$off" -N 2 "$f" | tr -d ' ')" = "PE" ] || { echo "none 0"; return; }
    echo "$(od -An -t x2 -j $((off + 4)) -N 2 "$f" | tr -d ' ') $(od -An -t u2 -j $((off + 22)) -N 2 "$f" | tr -d ' ')"
}

require_x86_dll() {
    local f="$1" what="$2" machine flags
    [ -f "$f" ] || die "$what not found: $f"
    read -r machine flags <<<"$(pe_info "$f")"
    [ "$machine" = "014c" ] || die "$what is not a 32-bit Windows binary (PE machine: $machine): $f"
    (( flags & 0x2000 )) || die "$what is not a DLL: $f"
}

# 32-bit DLLs live in syswow64 in a 64-bit prefix and in system32 in a 32-bit one.
sysdir() {
    [ -d "$prefix/drive_c/windows" ] || die "no Wine prefix at $prefix (set WINEPREFIX)"
    if [ -d "$prefix/drive_c/windows/syswow64" ]; then
        echo "$prefix/drive_c/windows/syswow64"
    else
        echo "$prefix/drive_c/windows/system32"
    fi
}

set_override() {
    WINEPREFIX="$prefix" wine reg add 'HKCU\Software\Wine\DllOverrides' /v ddraw /t REG_SZ /d native,builtin /f >/dev/null
}

remove_override() {
    WINEPREFIX="$prefix" wine reg delete 'HKCU\Software\Wine\DllOverrides' /v ddraw /f >/dev/null 2>&1 || true
}

backup_wine_ddraw() {
    local dir="$1"
    if [ ! -f "$dir/$backup_name" ]; then
        [ -f "$dir/ddraw.dll" ] || die "Wine's ddraw.dll is missing from $dir"
        mv "$dir/ddraw.dll" "$dir/$backup_name"
        echo "backed up Wine's ddraw.dll -> $backup_name"
    fi
}

cmd_install() {
    local dd2d9=""
    [ $# -eq 1 ] || usage
    dd2d9="$1"
    require_x86_dll "$dd2d9" "dd2d9 ddraw.dll"

    local dir
    dir=$(sysdir)
    echo "prefix: $prefix"
    echo "system directory: $dir"
    backup_wine_ddraw "$dir"

    cp -f "$dd2d9" "$dir/ddraw.dll"
    rm -f "$dir/ddraw_.dll"  # left by older versions' D7VK setup
    echo "installed dd2d9 as ddraw.dll"
    set_override
    echo "set DLL override: ddraw = native,builtin"
    local ini="$prefix/drive_c/windows/dd2d9.ini" example
    example="$(dirname "$(readlink -f "$0")")/dd2d9.ini.example"
    if [ ! -f "$ini" ] && [ -f "$example" ]; then
        cp "$example" "$ini"
        echo "config: $ini"
    elif [ -f "$ini" ]; then
        echo "config: $ini (kept)"
    fi
    echo "note: a Wine update (wineboot -u) may restore Wine's ddraw.dll; run this again afterwards"
}

cmd_uninstall() {
    local dir
    dir=$(sysdir)
    if [ -f "$dir/$backup_name" ]; then
        mv -f "$dir/$backup_name" "$dir/ddraw.dll"
        echo "restored Wine's ddraw.dll"
    else
        echo "no backup found in $dir; leaving ddraw.dll as it is"
    fi
    rm -f "$dir/ddraw_.dll"
    remove_override
    echo "removed the ddraw DLL override"
}

command -v wine >/dev/null || die "wine not found in PATH"
case "${1:-}" in
    install) shift; cmd_install "$@" ;;
    uninstall) shift; cmd_uninstall ;;
    *) usage ;;
esac
