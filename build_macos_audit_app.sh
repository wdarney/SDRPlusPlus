#!/bin/sh
set -e

BUILD_DIR=${1:-./build}
BUNDLE=${2:-/Applications/SDR++-audit.app}
JOBS=$(sysctl -n hw.logicalcpu)

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"

cmake --build "$BUILD_DIR" -- -j"$JOBS"
sh "$SCRIPT_DIR/make_macos_audit_bundle.sh" "$BUILD_DIR" "$BUNDLE"

echo "Built audit app at $BUNDLE"
