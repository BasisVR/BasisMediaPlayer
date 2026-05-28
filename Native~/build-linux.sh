#!/usr/bin/env bash
# Build basis_media_native (stub backend) for Linux x86_64.
# Requires: cmake >= 3.18, clang or gcc, pthreads.
# Output:   Plugins/Linux/x86_64/libbasis_media_native.so
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build-linux-x64"
OUT="$HERE/../Plugins/Linux/x86_64"

rm -rf "$BUILD"
cmake -S "$HERE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" --config Release -j

mkdir -p "$OUT"
cp -f "$BUILD/libbasis_media_native.so" "$OUT/libbasis_media_native.so"
echo
echo "Built: $OUT/libbasis_media_native.so"
