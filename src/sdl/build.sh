#!/bin/sh
# Build libiconsdl.a and the vendored SDL3/Blend2D/AsmJit static
# libraries. Verifies every tarball in ../../deps against deps.lock, then
# runs one CMake build. Never uses the network.
set -eu
cd "$(dirname "$0")"
DEPS=../../deps
LOCK=$DEPS/deps.lock
B=build
# The SHA-256 of file $1: GNU or BSD sha256sum, or Perl's shasum.
sha256() {
   { sha256sum "$1" 2>/dev/null || shasum -a 256 "$1"; } | awk '{print $1; exit}'
}

grep -v '^#' "$LOCK" | while read -r name ver sha url; do
   [ -n "$name" ] || continue
   f="$DEPS/$name-$ver.tar.gz"
   [ -f "$f" ] || { echo "src/sdl: missing $f" >&2; exit 1; }
   [ "$(sha256 "$f")" = "$sha" ] \
      || { echo "src/sdl: checksum mismatch for $f" >&2; exit 1; }
done

if ! cmp -s "$LOCK" "$B/src/.lock" 2>/dev/null; then
   rm -rf "$B"
   mkdir -p "$B/src"
   grep -v '^#' "$LOCK" | while read -r name ver sha url; do
      [ -n "$name" ] || continue
      mkdir -p "$B/src/$name"
      tar -xzf "$DEPS/$name-$ver.tar.gz" -C "$B/src/$name" --strip-components 1
   done
   cp "$LOCK" "$B/src/.lock"
fi

if [ ! -f "$B/cmake/CMakeCache.txt" ]; then
   echo "src/sdl: configuring SDL3, Blend2D and libiconsdl (log: src/sdl/$B/configure.log)"
   cmake -S . -B "$B/cmake" -DCMAKE_BUILD_TYPE=Release >"$B/configure.log" 2>&1 \
      || { cat "$B/configure.log" >&2; exit 1; }
fi
echo "src/sdl: building (log: src/sdl/$B/build.log)"
cmake --build "$B/cmake" -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" >"$B/build.log" 2>&1 \
   || { tail -40 "$B/build.log" >&2; exit 1; }
cp "$B/cmake/libiconsdl.a" libiconsdl.a
