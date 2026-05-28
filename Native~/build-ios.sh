#!/usr/bin/env bash
# Build basis_media_native (stub backend) for iOS arm64 (device).
# Must run on a macOS host with Xcode installed.
# Output:   Plugins/iOS/arm64/libbasis_media_native.a   (static archive)
#
# Unity requires iOS plugins to be static libraries (.a) — the engine relinks
# them into the final app binary, so a .dylib is rejected by the importer.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$HERE/build-ios-arm64"
OUT="$HERE/../Plugins/iOS/arm64"

rm -rf "$BUILD"
cmake -S "$HERE" -B "$BUILD" -G Xcode \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
      -DCMAKE_XCODE_ATTRIBUTE_ENABLE_BITCODE=NO \
      -DCMAKE_IOS_INSTALL_COMBINED=NO
cmake --build "$BUILD" --config Release -- -sdk iphoneos

mkdir -p "$OUT"
# Static-lib output lives under Release-iphoneos/ when using the Xcode generator.
cp -f "$BUILD/Release-iphoneos/libbasis_media_native.a" "$OUT/libbasis_media_native.a"
echo
echo "Built: $OUT/libbasis_media_native.a"
