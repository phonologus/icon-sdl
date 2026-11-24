#!/bin/sh
# Window tests: these open real windows (briefly), so they need a
# display. Each NAME.icn runs with ICONSDL_TEST_EVENTS set from
# NAME.script (if any); its standard output and error must match
# NAME.std.
#
# NAME.env, if present, holds VAR=value lines added to its environment.
#
#   run.sh ICONDIR
set -eu
here=$(cd "$(dirname "$0")" && pwd)
icont=$(cd "$1" && pwd)/bin/icont
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
bad=0
cd "$tmp"
for f in "$here"/*.icn; do
   n=$(basename "$f" .icn)
   cp "$f" "$n.icn"			# so messages name just the file
   "$icont" -s -o "$n" "$n.icn"
   script=""
   [ -f "$here/$n.script" ] && script=$(cat "$here/$n.script")
   envs=""
   [ -f "$here/$n.env" ] && envs=$(cat "$here/$n.env")
   env $envs ICONSDL_TEST_EVENTS="$script" perl -e 'alarm 30; exec @ARGV' ./"$n" \
      >"$n.out" 2>&1 || echo "exit status $?" >>"$n.out"
   if diff "$here/$n.std" "$n.out" >"$n.diff"; then
      echo "$n: ok"
   else
      echo "$n: FAILED"; cat "$n.diff"; bad=1
   fi
done
exit $bad
