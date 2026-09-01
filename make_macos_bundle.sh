#!/bin/sh
set -e

# ========================= Boilerplate =========================
BUILD_DIR=$1
BUNDLE=$2

source macos/bundle_utils.sh

# ========================= Prepare dotapp structure =========================

# Clear .app
rm -rf $BUNDLE

# Create .app structure
bundle_create_struct $BUNDLE

# Add resources
cp -R root/res/* $BUNDLE/Contents/Resources/

# Create the icon file
bundle_create_icns root/res/icons/sdrpp.macos.png $BUNDLE/Contents/Resources/sdrpp

# Create the property list
bundle_create_plist sdrpp SDR++ org.sdrpp.sdrpp 1.2.1 sdrp sdrpp sdrpp $BUNDLE/Contents/Info.plist

# ========================= Install binaries =========================

# Core
bundle_install_binary $BUNDLE $BUNDLE/Contents/MacOS $BUILD_DIR/sdrpp 
bundle_install_binary $BUNDLE $BUNDLE/Contents/Frameworks $BUILD_DIR/core/libsdrpp_core.dylib

# Channel Bank's post-build portability step gives its bundled Whisper/ggml
# dependencies @rpath names. Install those support libraries before the module
# so bundle_install_binary can resolve the already-portable references.
CHANNEL_BANK_SUPPORT="$BUILD_DIR/misc_modules/channel_bank/external/whisper.cpp"
if [ -d "$CHANNEL_BANK_SUPPORT" ]; then
    find -L "$CHANNEL_BANK_SUPPORT" -type f \( -name 'libwhisper.[0-9].dylib' -o -name 'libggml*.0.dylib' \) -print | while IFS= read -r SUPPORT_LIBRARY; do
        bundle_install_binary "$BUNDLE" "$BUNDLE/Contents/Frameworks" "$SUPPORT_LIBRARY"
    done
fi

# Modules
# Package exactly the modules produced by this build. Keeping this list dynamic
# prevents a newly built core from being bundled with stale plug-ins from a
# different revision and automatically includes optional modules when enabled.
for MODULE_GROUP in source_modules sink_modules decoder_modules misc_modules; do
    MODULE_ROOT="$BUILD_DIR/$MODULE_GROUP"
    if [ ! -d "$MODULE_ROOT" ]; then
        continue
    fi

    find "$MODULE_ROOT" -mindepth 2 -maxdepth 2 -type f -name '*.dylib' -print | while IFS= read -r MODULE; do
        bundle_install_binary "$BUNDLE" "$BUNDLE/Contents/Plugins" "$MODULE"
    done
done

# ========================= Finalize =========================

# Sign the app
bundle_sign $BUNDLE
