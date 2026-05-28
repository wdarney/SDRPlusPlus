#!/bin/bash
# Rebuilds rx888_source + channel_bank and deploys them to SDR++MODULETESTING.app.
# Also rebuilds the SoapySDDC driver (against MacPorts SoapySDR) if needed.
# Run from the repo root: ./deploy_macos_testing.sh

set -e

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKTREE="$REPO_DIR/.claude/worktrees/laughing-carson"
BUILD_DIR="$WORKTREE/build"
APP="/Applications/SDR++MODULETESTING.app"
EXTIO_DIR="$HOME/src/ExtIO_sddc"
EXTIO_BUILD="$EXTIO_DIR/build_macports"
SIGN_ID="Apple Development: wdarney@outlook.com (BPB26A8GR2)"
JOBS=$(sysctl -n hw.logicalcpu)

echo "=== Building channel_bank + rx888_source ==="
cmake --build "$BUILD_DIR" --target channel_bank rx888_source -- -j$JOBS

echo "=== Copying plugins ==="
cp "$BUILD_DIR/misc_modules/channel_bank/channel_bank.dylib"           "$APP/Contents/Plugins/channel_bank.dylib"
cp "$BUILD_DIR/source_modules/rx888_source/rx888_source.dylib"         "$APP/Contents/Plugins/rx888_source.dylib"

# Rebuild SoapySDDC if source is newer than installed module
SDDC_SRC="$EXTIO_DIR/SoapySDDC/Settings.cpp"
SDDC_OUT="$EXTIO_BUILD/SoapySDDC/libSDDCSupport.so"
if [ "$SDDC_SRC" -nt "$SDDC_OUT" ] || [ ! -f "$SDDC_OUT" ]; then
    echo "=== Rebuilding SoapySDDC (MacPorts) ==="
    mkdir -p "$EXTIO_BUILD"
    cmake -S "$EXTIO_DIR" -B "$EXTIO_BUILD" \
        -DCMAKE_PREFIX_PATH=/opt/local \
        -DSoapySDR_DIR=/opt/local/share/cmake/SoapySDR \
        -DCMAKE_BUILD_TYPE=Release -Wno-dev 2>&1 | grep -v "^--"
    cmake --build "$EXTIO_BUILD" --target SDDCSupport -j$JOBS
    cp "$SDDC_OUT" "$APP/Contents/SoapySDR/modules0.8/libSDDCSupport.so"
    echo "  SoapySDDC deployed"
else
    echo "=== SoapySDDC up to date, skipping rebuild ==="
fi

echo "=== Signing ==="
codesign --force --sign "$SIGN_ID" "$APP/Contents/Plugins/channel_bank.dylib"
codesign --force --sign "$SIGN_ID" "$APP/Contents/Plugins/rx888_source.dylib"
codesign --force --sign "$SIGN_ID" "$APP/Contents/SoapySDR/modules0.8/libSDDCSupport.so"
codesign --force --deep --sign "$SIGN_ID" "$APP"
codesign --verify --deep --strict "$APP" && echo "Bundle valid ✓"

echo ""
echo "Done. Quit and relaunch SDR++MODULETESTING.app from Finder."
echo "Power-cycle the RX888 if the firmware version changed."
