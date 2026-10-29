#!/bin/sh
# Fetches Wine's ddraw tests (and the two headers they need) at a pinned commit into
# work/, and prepares them for one sub-test per process (patch_tests.py).
# usage: fetch.sh [wine commit]     (needs git and network access to GitHub)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
COMMIT=${1:-66caeb46aea4aa8fe6f87b7a209e7867651c09dd}  # the Wine tree the baseline was made with
W=$HERE/work/wine
if [ ! -d "$W/.git" ]; then
    git clone --filter=blob:none --no-checkout https://github.com/wine-mirror/wine.git "$W"
    git -C "$W" sparse-checkout set dlls/ddraw/tests include/wine
fi
git -C "$W" fetch --filter=blob:none origin "$COMMIT" 2>/dev/null || true
git -C "$W" checkout -q "$COMMIT"
mkdir -p "$HERE/work/inc/wine"
cp "$W/include/wine/test.h" "$W/include/wine/debug.h" "$HERE/work/inc/wine/"
python3 "$HERE/patch_tests.py" "$W/dlls/ddraw/tests" "$HERE/work"
echo "Wine tests at $COMMIT prepared in $HERE/work"
