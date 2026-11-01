#!/bin/bash
# run.sh: the DirectX 7 SDK samples, each run 8 s with dd2d9 and with Wine's own ddraw, a
# screenshot of each (compare.py then compares them). Needs Xvfb, ImageMagick's import, Wine.
#   D3DIM_DIR  the SDK's samples/Multimedia/D3DIM (its bin/ and media/), e.g. from
#              github.com/AmiSimon/orbiter2016 (Extern/mssdk_dx7)
#   DDRAW_DIR  the DirectDraw samples' executables (ddex1..5, donuts, ...), e.g.
#              github.com/BieHDC/mingw_dx7_sdk_samples (ddraw/bin_orig)
#   DLL        dd2d9's ddraw.dll (default: ../../build/ddraw.dll)
#   OUT        where to work (default: ./work); WINE, WINEPREFIX as usual
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
DLL=${DLL:-$HERE/../../build/ddraw.dll}; OUT=${OUT:-$HERE/work}; WINE=${WINE:-wine}
rm -rf "$OUT"; mkdir -p "$OUT/dd" "$OUT/ref" "$OUT/shots"
for d in dd ref; do cp "$D3DIM_DIR"/bin/*.exe "$D3DIM_DIR"/media/* "$DDRAW_DIR"/*.exe "$OUT/$d/" 2>/dev/null; done
cp "$DLL" "$OUT/dd/ddraw.dll"; printf '[dd2d9]\nlog=1\n' > "$OUT/dd/dd2d9.ini"
export WINEDEBUG=-all
for exe in $(cd "$OUT/dd" && ls *.exe); do
  n=${exe%.exe}
  for side in dd ref; do
    ov=$([ $side = dd ] && echo n || echo b)
    Xvfb :95 -screen 0 1024x768x24 >/dev/null 2>&1 & XP=$!; sleep 1
    export DISPLAY=:95; wineserver -k 2>/dev/null
    (cd "$OUT/$side"; rm -f dd2d9.log; WINEDLLOVERRIDES="ddraw=$ov;mscoree=;mshtml=;winedbg.exe=d" timeout 10 "$WINE" $exe >/dev/null 2>&1; echo $? > $n.rc) &
    P=$!; sleep 8; import -window root "$OUT/shots/$n-$side.png" 2>/dev/null; wait $P
    [ $side = dd ] && cp "$OUT/dd/dd2d9.log" "$OUT/shots/$n.log" 2>/dev/null
    echo "$n $side rc=$(cat "$OUT/$side/$n.rc")"   # 124: ran until stopped
    wineserver -k 2>/dev/null; kill $XP 2>/dev/null; wait $XP 2>/dev/null
  done
done
