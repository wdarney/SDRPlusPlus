#!/usr/bin/env bash
# Configure the iOS Xcode project. Generates build-ios-{device,sim}/sdrpp.xcodeproj.
#
# Usage:
#   export SDRPP_IOS_DEPS_ROOT=/abs/path/to/ios-deps
#   ios/build.sh device   # arm64 device
#   ios/build.sh sim      # arm64 simulator
#
# SDRPP_IOS_DEPS_ROOT must point to a directory containing include/ and lib/
# with iOS-targeted builds of fftw3f, libzstd, and (optionally) volk. See
# ios/README.md.
set -euo pipefail

target="${1:-device}"
case "$target" in
    device) sdk="iphoneos";        archs="arm64" ;;
    sim)    sdk="iphonesimulator"; archs="arm64" ;;
    *) echo "usage: $0 {device|sim}"; exit 2 ;;
esac

if [[ -z "${SDRPP_IOS_DEPS_ROOT:-}" ]]; then
    echo "SDRPP_IOS_DEPS_ROOT is not set — see ios/README.md" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="$repo_root/build-ios-$target"

cmake -S "$repo_root" -B "$build_dir" -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$sdk" \
    -DCMAKE_OSX_ARCHITECTURES="$archs" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DSDRPP_IOS_DEPS_ROOT="$SDRPP_IOS_DEPS_ROOT"

echo
echo "Xcode project: $build_dir/sdrpp.xcodeproj"
echo "Open it, select the SDRPP_iOS scheme, set your signing team, and run."
