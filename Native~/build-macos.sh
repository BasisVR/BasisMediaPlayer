#!/usr/bin/env bash
# Build basis_media_native (stub backend) for macOS (x86_64 + arm64).
# Must run on a macOS host with Xcode command-line tools installed.
# Output:
#   Plugins/macOS/x86_64/libbasis_media_native.dylib
#   Plugins/macOS/arm64/libbasis_media_native.dylib
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

build_arch() {
  local arch="$1" outdir="$2"
  local build="$HERE/build-macos-$arch"
  rm -rf "$build"
  cmake -S "$HERE" -B "$build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
  cmake --build "$build" --config Release -j
  mkdir -p "$outdir"
  cp -f "$build/libbasis_media_native.dylib" "$outdir/libbasis_media_native.dylib"
  echo "Built: $outdir/libbasis_media_native.dylib"
}

build_arch x86_64 "$HERE/../Plugins/macOS/x86_64"
build_arch arm64  "$HERE/../Plugins/macOS/arm64"
