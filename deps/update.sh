#!/bin/sh
# Pin a vendored dependency to a new version.
#
#   deps/update.sh NAME VERSION            re-pin an existing dependency
#   deps/update.sh NAME VERSION URL        add a new one (URL uses @V@ for VERSION)
#
# Downloads the source tarball, records its SHA-256 in deps.lock, stores it
# as deps/NAME-VERSION.tar.gz and removes the previous one. Commit the
# tarball, deps.lock and any code changes the upgrade needs together.
# This is the only script that uses the network; build.sh never does.
set -eu
cd "$(dirname "$0")"

die() { echo "update.sh: $*" >&2; exit 1; }
[ $# -eq 2 ] || [ $# -eq 3 ] || die "usage: update.sh NAME VERSION [URL-TEMPLATE]"
name=$1 ver=$2

line=$(awk -v n="$name" '$1 == n' deps.lock)
if [ -n "$line" ]; then
   old=$(echo "$line" | awk '{print $2}')
   tmpl=${3:-$(echo "$line" | awk '{print $4}')}
else
   [ $# -eq 3 ] || die "$name is not in deps.lock; give a URL template to add it"
   old= tmpl=$3
fi

url=$(printf '%s' "$tmpl" | sed "s|@V@|$ver|g")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "fetching $url"
curl -fsSL -o "$tmp/src.tar.gz" "$url" || die "download failed"
tar -tzf "$tmp/src.tar.gz" >/dev/null 2>&1 || die "not a gzipped tar: $url"
sha=$(shasum -a 256 "$tmp/src.tar.gz" | awk '{print $1}')

mv "$tmp/src.tar.gz" "$name-$ver.tar.gz"
if [ -n "$old" ] && [ "$old" != "$ver" ]; then
   rm -f "$name-$old.tar.gz"
fi

awk -v n="$name" -v v="$ver" -v s="$sha" -v t="$tmpl" '
   BEGIN { done = 0 }
   $1 == n { printf "%-8s %-40s %s %s\n", n, v, s, t; done = 1; next }
   { print }
   END { if (!done) printf "%-8s %-40s %s %s\n", n, v, s, t }
' deps.lock > "$tmp/lock"
mv "$tmp/lock" deps.lock
echo "$name: ${old:-(new)} -> $ver  sha256 $sha"
