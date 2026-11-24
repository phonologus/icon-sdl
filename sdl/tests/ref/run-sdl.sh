#!/bin/sh
# Run the reference programs with an Icon build and compare its outputs
# with the X11 references in expected/.
#
#   run-sdl.sh ICONDIR [OUTDIR]
#
# ICONDIR is a built Icon tree (ICONDIR/bin/icont). Outputs go to OUTDIR
# (default: a temporary directory), with NAME.diff.png for each image
# that differs. Standard output must match exactly; images are reported
# as the number of differing pixels. Exit status 0 if everything matches.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
icont=$(cd "$1" && pwd)/bin/icont
out=${2:-$(mktemp -d)}
mkdir -p "$out"
out=$(cd "$out" && pwd)
[ -x "$icont" ] || { echo "run-sdl.sh: no $icont" >&2; exit 2; }

# tolerance NAME FIELD: the accepted difference from the tolerance file
tolerance() {
   awk -v n="$1" -v f="$2" '$1 == n { print $f; found = 1 }
      END { if (!found) print (f == 2 ? 0 : "same") }' "$here/tolerance"
}

bad=0
cd "$out"
for f in "$here"/*.icn; do
   n=$(basename "$f" .icn)
   maxpix=$(tolerance "$n" 2)
   outrule=$(tolerance "$n" 3)
   "$icont" -s -o "$n" "$f"
   ./"$n" "$n.gif" >"$n.out" 2>&1 || echo "$n: exit status $?" >>"$n.out"
   if cmp -s "$here/expected/$n.out" "$n.out"; then o="out same"
   elif [ "$outrule" = differs ]; then o="out differs (ok)"
   else o="out DIFFERS"; bad=1; fi
   if [ -f "$n.gif" ]; then
      g=$(python3 "$here/compare.py" "$here/expected/$n.gif" "$n.gif" "$n.diff.png") || true
      npix=${g%% *}
      case "$npix" in *[!0-9]*|"") verdict="ERROR"; bad=1 ;;
      *) if [ "$maxpix" -lt 0 ] || [ "$npix" -le "$maxpix" ]; then verdict="ok"
         else verdict="FAIL (allowed $maxpix)"; bad=1; fi ;;
      esac
   else
      g="no image"; verdict="FAIL"; bad=1
   fi
   printf '%-10s %-17s %-40s %s\n' "$n" "$o" "$g" "$verdict"
done
echo "outputs in $out"
exit $bad
