#!/bin/sh
# Starts an X server for the tests on :${1:-78}: Xvnc at 1024x768 with the low-resolution
# modes the mode-switch tests ask for (xrandr). Xvfb can't switch modes, so those tests
# fail there.
N=${1:-78}
P=$(pgrep -f "[X]tigervnc :$N "); if [ -n "$P" ]; then kill $P; sleep 0.5; fi
Xtigervnc :$N -geometry 1024x768 -depth 24 -SecurityTypes None -nolisten tcp >/dev/null 2>&1 &
sleep 2
export DISPLAY=:$N
add() { xrandr --newmode "$1" $2 $3 $4 $5 $6 $7 $8 $9 ${10} 2>/dev/null; xrandr --addmode VNC-0 "$1" 2>/dev/null; }
add 320x240 6.29 320 328 376 400 240 245 246 262
add 320x200 5.38 320 328 376 400 200 205 206 224
add 512x384 14.40 512 520 568 600 384 385 388 400
add 640x400 20.16 640 656 752 800 400 412 414 420
