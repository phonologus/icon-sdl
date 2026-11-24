#!/bin/sh
# Regenerate the X11 reference outputs in sdl/tests/ref/expected.
#
# Builds unmodified upstream Icon (this repository's `master` branch) with
# X11 graphics inside the test VM (sdl/vm/vm.sh), runs every *.icn here
# under Xvfb (24-bit TrueColor), and copies back each program's image
# (NAME.gif) and standard output (NAME.out). The VM must be running.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
vm="$top/sdl/vm/vm.sh"
rev=$(git -C "$top" rev-parse master)

"$vm" status | grep -q running || { echo "start the VM: sdl/vm/vm.sh start" >&2; exit 1; }

echo "building upstream icont $rev with X11 graphics in the VM"
git -C "$top" archive --format=tar master | "$vm" ssh '
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

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
"$vm" ssh 'cd ref-run && tar -cf - *.gif *.out' > "$tmp"
mkdir -p "$here/expected"
rm -f "$here"/expected/*
tar -xf "$tmp" -C "$here/expected"
{
   echo "Reference outputs from unmodified upstream Icon, built with X11 graphics."
   echo "upstream commit: $rev"
   echo "X server: $("$vm" ssh 'dpkg-query -W -f "\${Package} \${Version}" xvfb')"
   echo "VM packages: sdl/vm/packages.lock"
} > "$here/expected/SOURCE"
ls "$here/expected"
