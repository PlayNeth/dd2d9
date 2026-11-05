#!/bin/sh
# Builds the Wine ddraw tests prepared by fetch.sh into work/*.exe (32-bit, mingw-w64).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
W=$HERE/work
CC=${CC:-i686-w64-mingw32-gcc}
cd "$W"
$CC -O1 -c "$HERE/shim.c" -o shim.o
$CC -O1 -c "$HERE/iid3.c" -o iid3.o
fail=0
for t in ddraw1 ddraw2 ddraw4 ddraw7 ddrawmodes dsurface refcount visual d3d; do
    extra=iid3.o; [ $t = d3d ] && extra=  # d3d.c defines IID_IDirectDraw3 itself
    # (Wine's tests rely on implicit declarations and int/pointer conversions, errors since GCC 14.)
    $CC -O1 -std=gnu11 -Wno-error=implicit-function-declaration -Wno-error=int-conversion -DSTANDALONE -DCOBJMACROS -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DDECLSPEC_EXPORT= \
        -include windows.h -I"$W/inc" -I"$W/wine/dlls/ddraw/tests" "src/$t.c" shim.o $extra -o $t.exe \
        -static -lddraw -luser32 -lgdi32 -lole32 -ldxguid -luuid -lm -w || { echo "$t: build failed"; fail=1; }
done
ls *.exe
exit $fail
