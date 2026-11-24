#!/bin/sh
# X11 reference outputs, made in the test VM (sdl/vm/vm.sh; must be running).
#
#   make-reference.sh           regenerate sdl/tests/ref/expected from
#                               unmodified upstream Icon (branch `master`)
#   make-reference.sh --check   build this working tree's X11 configuration
#                               instead, and compare its outputs byte for
#                               byte with expected/ (nothing is overwritten)
#
# Either way: Icon is built with X11 graphics (make X-Configure name=linux),
# every *.icn here runs under Xvfb (24-bit TrueColor), and each program's
# image (NAME.gif) and standard output (NAME.out) are collected. --check
# shows that changes to shared code leave the X11 backend unaffected.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
vm="$top/sdl/vm/vm.sh"
check=0
[ "${1:-}" = "--check" ] && check=1

"$vm" status | grep -q running || { echo "start the VM: sdl/vm/vm.sh start" >&2; exit 1; }

if [ $check = 1 ]; then
   rev="working tree of $(git -C "$top" rev-parse --abbrev-ref HEAD)"
   src() { cd "$top" && { git ls-files -c; git ls-files -o --exclude-standard; } |
      sort -u | while read -r f; do [ -e "$f" ] && echo "$f"; done | tar -cf - -T -; }
else
   rev=$(git -C "$top" rev-parse master)
   src() { git -C "$top" archive --format=tar master; }
fi

echo "building icont ($rev) with X11 graphics in the VM"
src | "$vm" ssh '
   set -e
   rm -rf icon-ref && mkdir icon-ref && tar -x -C icon-ref
   cd icon-ref
   make X-Configure name=linux >../ref-configure.log 2>&1
   make Icont >../ref-build.log 2>&1
   ls -l bin/icont bin/iconx >/dev/null'

echo "running reference programs under Xvfb"
(cd "$here" && tar -cf - *.icn) | "$vm" ssh '
   set -e
   rm -rf ref-run && mkdir ref-run && cd ref-run && tar -xf -
   Xvfb :99 -screen 0 1024x768x24 -nolisten tcp >../xvfb.log 2>&1 &
   xvfb=$!
   trap "kill $xvfb" EXIT
   sleep 1
   export DISPLAY=:99
   for f in *.icn; do
      n=${f%.icn}
      ../icon-ref/bin/icont -s -o "$n" "$f"
      ./"$n" "$n.gif" >"$n.out" 2>&1 || echo "$n: exit status $?" >>"$n.out"
   done'

# Every program must have produced its image.
"$vm" ssh 'cd ref-run && for f in *.icn; do [ -s "${f%.icn}.gif" ] || { echo "no image from $f:"; cat "${f%.icn}.out"; exit 1; }; done' \
   || { echo "reference run failed" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"$vm" ssh 'cd ref-run && tar -cf - *.gif *.out' > "$tmp/out.tar"
mkdir "$tmp/out"
tar -xf "$tmp/out.tar" -C "$tmp/out"

if [ $check = 1 ]; then
   bad=0
   for f in "$here"/expected/*.gif "$here"/expected/*.out; do
      n=$(basename "$f")
      if cmp -s "$f" "$tmp/out/$n"; then echo "  same     $n"
      else echo "  DIFFERS  $n"; bad=1; fi
   done
   [ $bad = 0 ] && echo "X11 output identical to the reference" && exit 0
   echo "X11 output differs from the reference" >&2
   exit 1
fi

mkdir -p "$here/expected"
rm -f "$here"/expected/*
cp "$tmp"/out/* "$here/expected/"
{
   echo "Reference outputs from unmodified upstream Icon, built with X11 graphics."
   echo "upstream commit: $rev"
   echo "X server: $("$vm" ssh 'dpkg-query -W -f "\${Package} \${Version}" xvfb')"
   echo "VM packages: sdl/vm/packages.lock"
} > "$here/expected/SOURCE"
ls "$here/expected"
