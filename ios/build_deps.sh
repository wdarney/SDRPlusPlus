#!/usr/bin/env bash
# Cross-compile libzstd for iOS / iOS Simulator. Outputs a tree laid out as
#
#   $OUT/include/zstd.h
#   $OUT/lib/libzstd.a
#
# Set SDRPP_IOS_DEPS_ROOT to that $OUT path before configuring the iOS build.
#
# Usage:
#   ios/build_deps.sh device       # arm64 iPhoneOS
#   ios/build_deps.sh sim          # arm64 iPhoneSimulator
#
# VOLK and FFTW3 are not built — both are replaced at compile time by shims
# under ios/volk_shim/ and ios/fftw_shim/, which implement the slices of
# those APIs that SDR++ uses on top of Apple's Accelerate framework.
#
set -euo pipefail

target="${1:-device}"
case "$target" in
    device) sdk="iphoneos";        host_triple="arm64-apple-darwin"; arch="arm64" ;;
    sim)    sdk="iphonesimulator"; host_triple="arm64-apple-darwin"; arch="arm64" ;;
    *) echo "usage: $0 {device|sim}"; exit 2 ;;
esac

# Versions pinned. Bump when needed; unrelated to SDR++ core.
ZSTD_VER="1.5.6"

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
out="${SDRPP_IOS_DEPS_ROOT:-$repo_root/build-ios-deps-$target}"
work="${TMPDIR:-/tmp}/sdrpp-ios-deps-$target"

mkdir -p "$out/include" "$out/lib" "$work"

sdk_path="$(xcrun --sdk "$sdk" --show-sdk-path)"
deployment="-mios-version-min=14.0"
if [[ "$sdk" == "iphonesimulator" ]]; then
    deployment="-mios-simulator-version-min=14.0"
fi

cflags="-arch $arch -isysroot $sdk_path $deployment -O2 -fembed-bitcode-marker"
ldflags="-arch $arch -isysroot $sdk_path $deployment"

cc="$(xcrun --sdk "$sdk" --find clang)"
ar="$(xcrun --sdk "$sdk" --find ar)"
ranlib="$(xcrun --sdk "$sdk" --find ranlib)"

echo "==> Building deps for $sdk ($arch) into $out"

# ---- libzstd -------------------------------------------------------------
zstd_dir="$work/zstd-$ZSTD_VER"
if [[ ! -d "$zstd_dir" ]]; then
    curl -sSfL "https://github.com/facebook/zstd/releases/download/v$ZSTD_VER/zstd-$ZSTD_VER.tar.gz" | tar -xz -C "$work"
fi
(
    cd "$zstd_dir/lib"
    make clean >/dev/null 2>&1 || true
    CC="$cc" AR="$ar" RANLIB="$ranlib" \
    CFLAGS="$cflags" LDFLAGS="$ldflags" \
    make libzstd.a -j"$(sysctl -n hw.ncpu)" >/dev/null
    cp libzstd.a "$out/lib/"
    cp zstd.h zstd_errors.h zdict.h "$out/include/" 2>/dev/null || true
    # zstd ships its public headers at lib/zstd.h directly in newer versions.
    [[ -f common/zstd_errors.h ]] && cp common/zstd_errors.h "$out/include/" || true
)
echo "    libzstd -> $out/lib/libzstd.a"

echo
echo "Done. Configure with:"
echo "    export SDRPP_IOS_DEPS_ROOT=$out"
echo "    ./ios/build.sh $target"
