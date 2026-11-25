#!/bin/sh
# Window tests: these open real windows (briefly), so they need a
# display. Each NAME.icn runs with ICONSDL_TEST_EVENTS set from
# NAME.script (if any); its standard output and error must match
# NAME.std.
#
# NAME.env, if present, holds VAR=value lines added to its environment.
# ICONSDL_TEST_IGNORE, if set, is an extended regular expression for
# output lines to ignore: messages from the environment rather than the
# program (e.g. Mesa's complaints in a VM without a GPU).
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
   if [ -n "${ICONSDL_TEST_IGNORE:-}" ]; then
      grep -v -E "$ICONSDL_TEST_IGNORE" "$n.out" >"$n.kept" || true
      mv "$n.kept" "$n.out"
   fi
   if diff "$here/$n.std" "$n.out" >"$n.diff"; then
      echo "$n: ok"
   else
      echo "$n: FAILED"; cat "$n.diff"; bad=1
   fi
done
exit $bad
