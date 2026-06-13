#!/usr/bin/env bash
# Build librist as a static library for basis_media_native's RIST transport
# (-DBASIS_WITH_RIST=ON) and stage it into third_party/<rid>/.
#
# librist vendors its own mbedTLS and links it into the archive, so a single
# library per platform is produced; the consumer links one lib (rist).
#
# Usage: build-librist.sh <target>
#   android-arm64   NDK arm64-v8a / android-29 static (needs ANDROID_NDK_ROOT)
#
# Requires: git, meson, ninja. librist is cloned from upstream at the tag
# basis_rist.c targets (override with LIBRIST_REF).
#
# Output: third_party/<rid>/librist.a, third_party/include/librist/*.h
set -euo pipefail

TARGET="${1:-}"
LIBRIST_REF="${LIBRIST_REF:-v0.2.11}"
LIBRIST_REPO="${LIBRIST_REPO:-https://code.videolan.org/rist/librist.git}"

HERE="$(cd "$(dirname "$0")" && pwd)"
TP="$HERE/third_party"
WORK="$HERE/build-librist/$TARGET"

if [ -z "$TARGET" ]; then
    echo "usage: build-librist.sh <target>   (supported: android-arm64)" >&2
    exit 2
fi
for t in git meson ninja; do
    command -v "$t" >/dev/null 2>&1 || { echo "error: $t not on PATH" >&2; exit 1; }
done

rm -rf "$WORK"; mkdir -p "$WORK"
SRC="$WORK/librist"
git clone --depth 1 -b "$LIBRIST_REF" "$LIBRIST_REPO" "$SRC"

case "$TARGET" in
    android-arm64)
        : "${ANDROID_NDK_ROOT:?set ANDROID_NDK_ROOT to your NDK path}"
        TC="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/bin"
        CROSS="$WORK/android-arm64.ini"
        cat > "$CROSS" <<EOF
[binaries]
c = '$TC/aarch64-linux-android29-clang'
cpp = '$TC/aarch64-linux-android29-clang++'
ar = '$TC/llvm-ar'
strip = '$TC/llvm-strip'
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF
        meson setup "$SRC/build" "$SRC" --cross-file "$CROSS" \
            --default-library=static --buildtype=release
        ;;
    *)
        echo "error: unknown target '$TARGET' (supported: android-arm64)" >&2
        exit 2
        ;;
esac

ninja -C "$SRC/build" librist.a   # only the static we link; skip librist's CLI tools/tests

LIB="$SRC/build/librist.a"
if [ ! -f "$LIB" ] || [ "$(wc -c < "$LIB")" -lt 102400 ]; then
    echo "error: librist static missing or implausibly small at $LIB" >&2
    exit 1
fi

mkdir -p "$TP/$TARGET" "$TP/include/librist"
cp -f "$LIB" "$TP/$TARGET/librist.a"
cp -f "$SRC"/include/librist/*.h "$TP/include/librist/"
[ -d "$SRC/build/include/librist" ] && cp -f "$SRC"/build/include/librist/*.h "$TP/include/librist/" || true

echo "Staged: third_party/$TARGET/librist.a + third_party/include/librist/"
