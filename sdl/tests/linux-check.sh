#!/bin/sh
# Build this working tree's linux-sdl configuration in the test VM
# (sdl/vm/vm.sh; must be running) and run every test there: Icon's test
# suite, the X11 reference comparison, and the window tests under both
# X11 (Xvfb) and Wayland (headless Weston).
set -eu
top=$(cd "$(dirname "$0")/../.." && pwd)
vm="$top/sdl/vm/vm.sh"
"$vm" status | grep -q running || { echo "start the VM: sdl/vm/vm.sh start" >&2; exit 1; }

cd "$top"
{ git ls-files -c; git ls-files -o --exclude-standard; } | sort -u |
   while read -r f; do [ -e "$f" ] && echo "$f"; done | tar -cf - -T - |
   "$vm" ssh '
   set -e
   rm -rf icon-sdl && mkdir icon-sdl && tar -x -C icon-sdl && cd icon-sdl
   echo "== build (linux-sdl)"
   (make SDL-Configure name=linux-sdl && make Icont && make Ilib) >../linux-build.log 2>&1 \
      || { tail -30 ../linux-build.log; exit 1; }
   echo "== Icon test suite"
   make Test >../linux-test.log 2>&1 || { grep -i failed ../linux-test.log; exit 1; }
   echo "ok"
   echo "== reference comparison"
   st=0
   sdl/tests/ref/run-sdl.sh . /tmp/linux-ref-out >/tmp/linux-ref.txt 2>&1 || st=$?
   grep -v "^outputs" /tmp/linux-ref.txt
   [ $st = 0 ] || exit 1
   echo "== window tests, X11 (Xvfb)"
   Xvfb :99 -screen 0 1280x800x24 -nolisten tcp >/tmp/xvfb.log 2>&1 &
   xv=$!
   sleep 1
   DISPLAY=:99 SDL_VIDEO_DRIVER=x11 sdl/tests/win/run.sh . || { kill $xv; exit 1; }
   kill $xv
   echo "== window tests, Wayland (Weston, Cairo decorations)"
   export XDG_RUNTIME_DIR=/tmp/xdg-icon
   mkdir -p -m 700 $XDG_RUNTIME_DIR /tmp/ldp
   ln -sf /usr/lib/*/libdecor/plugins-1/libdecor-cairo.so /tmp/ldp/
   weston --backend=headless --socket=wl-icon --width=1280 --height=800 >/tmp/weston.log 2>&1 &
   wp=$!
   sleep 2
   LIBDECOR_PLUGIN_DIR=/tmp/ldp WAYLAND_DISPLAY=wl-icon SDL_VIDEO_DRIVER=wayland \
      sdl/tests/win/run.sh . || { kill $wp; exit 1; }
   kill $wp
   echo "== all Linux checks passed"'
