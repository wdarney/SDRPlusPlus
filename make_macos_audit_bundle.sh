#!/bin/sh
set -e

BUILD_DIR=${1:-./build}
BUNDLE=${2:-./SDR++-audit.app}

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"

export SDRPP_APP_NAME="sdrpp-audit"
export SDRPP_DISPLAY_NAME="SDR++ Audit"
export SDRPP_BUNDLE_ID="org.sdrpp.sdrpp.audit"
export SDRPP_APP_SIGNATURE="sdra"
export SDRPP_EXECUTABLE_NAME="sdrpp"
export SDRPP_ICON_NAME="sdrpp"

exec sh "$SCRIPT_DIR/make_macos_bundle.sh" "$BUILD_DIR" "$BUNDLE"
