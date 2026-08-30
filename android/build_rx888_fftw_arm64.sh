#!/usr/bin/env bash
set -euo pipefail

FFTW_VERSION=3.3.10
FFTW_ARCHIVE_SHA256=56c932549852cddcfafdab3820b0200c7742675be92179e59e6215b340e26467
NDK_VERSION=25.1.8937393
ANDROID_API=28

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
OUTPUT_DIR="$REPO_ROOT/source_modules/rx888_source/android-prebuilt/arm64-v8a"

if [[ -n "${ANDROID_NDK_HOME:-}" ]]; then
    NDK_ROOT="$ANDROID_NDK_HOME"
elif [[ -n "${ANDROID_SDK_ROOT:-}" ]]; then
    NDK_ROOT="$ANDROID_SDK_ROOT/ndk/$NDK_VERSION"
else
    NDK_ROOT="$HOME/Library/Android/sdk/ndk/$NDK_VERSION"
fi

case "$(uname -s)" in
    Darwin) HOST_TAG=darwin-x86_64 ;;
    Linux) HOST_TAG=linux-x86_64 ;;
    *) echo "Unsupported build host: $(uname -s)" >&2; exit 1 ;;
esac

TOOLCHAIN="$NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG/bin"
CC="$TOOLCHAIN/aarch64-linux-android${ANDROID_API}-clang"
if [[ ! -x "$CC" ]]; then
    echo "Android NDK compiler not found: $CC" >&2
    exit 1
fi

BUILD_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/rx888-fftw.XXXXXX")
trap 'rm -rf -- "$BUILD_ROOT"' EXIT
ARCHIVE="$BUILD_ROOT/fftw-${FFTW_VERSION}.tar.gz"

curl -fsSL "https://fftw.org/fftw-${FFTW_VERSION}.tar.gz" -o "$ARCHIVE"
ACTUAL_SHA256=$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')
if [[ "$ACTUAL_SHA256" != "$FFTW_ARCHIVE_SHA256" ]]; then
    echo "FFTW source checksum mismatch: $ACTUAL_SHA256" >&2
    exit 1
fi

tar -xzf "$ARCHIVE" -C "$BUILD_ROOT"
cd "$BUILD_ROOT/fftw-${FFTW_VERSION}"

CC="$CC" \
AR="$TOOLCHAIN/llvm-ar" \
RANLIB="$TOOLCHAIN/llvm-ranlib" \
STRIP="$TOOLCHAIN/llvm-strip" \
CFLAGS='-O3 -DNDEBUG -ffast-math -fomit-frame-pointer' \
./configure \
    --host=aarch64-linux-android \
    --enable-single \
    --enable-shared \
    --disable-static \
    --disable-fortran \
    --enable-neon \
    --enable-threads \
    --with-combined-threads

make -j"${JOBS:-8}"
mkdir -p "$OUTPUT_DIR"
cp .libs/libfftw3f.so "$OUTPUT_DIR/libfftw3f.so"
"$TOOLCHAIN/llvm-strip" "$OUTPUT_DIR/libfftw3f.so"

echo "Built $OUTPUT_DIR/libfftw3f.so"
shasum -a 256 "$OUTPUT_DIR/libfftw3f.so"
