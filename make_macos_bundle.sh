#!/bin/sh
set -e

# ========================= Boilerplate =========================
BUILD_DIR=$1
BUNDLE=$2
APP_NAME=${SDRPP_APP_NAME:-sdrpp}
DISPLAY_NAME=${SDRPP_DISPLAY_NAME:-SDR++}
BUNDLE_ID=${SDRPP_BUNDLE_ID:-org.sdrpp.sdrpp}
VERSION=${SDRPP_APP_VERSION:-1.2.1}
SIGNATURE=${SDRPP_APP_SIGNATURE:-sdrp}
EXECUTABLE=${SDRPP_EXECUTABLE_NAME:-sdrpp}
ICON_NAME=${SDRPP_ICON_NAME:-sdrpp}

source macos/bundle_utils.sh

# ========================= Prepare dotapp structure =========================

# Clear .app
rm -rf $BUNDLE

# Create .app structure
bundle_create_struct $BUNDLE

# Add resources
cp -R root/res/* $BUNDLE/Contents/Resources/

# Create the icon file
bundle_create_icns root/res/icons/sdrpp.macos.png $BUNDLE/Contents/Resources/$ICON_NAME

# Create the property list
bundle_create_plist "$APP_NAME" "$DISPLAY_NAME" "$BUNDLE_ID" "$VERSION" "$SIGNATURE" "$EXECUTABLE" "$ICON_NAME" "$BUNDLE/Contents/Info.plist"

# ========================= Install binaries =========================

# Core
bundle_install_binary $BUNDLE $BUNDLE/Contents/MacOS $BUILD_DIR/sdrpp

# Finder-launched apps do not inherit a useful working directory, and the raw
# SDR++ executable defaults its root relative to that directory. Dedicated
# bundles can opt into the same native launcher used by the known-good macOS
# deployment: keep the real executable beside it and explicitly select the
# user's existing SDR++ configuration directory.
if [ "${SDRPP_USE_APP_LAUNCHER:-0}" = "1" ]; then
    mv "$BUNDLE/Contents/MacOS/sdrpp" "$BUNDLE/Contents/MacOS/sdrpp-bin"
    LAUNCHER_BUILD_DIR="$BUILD_DIR/macos-app-launcher"
    LAUNCHER_BINARY="$LAUNCHER_BUILD_DIR/$EXECUTABLE"
    mkdir -p "$LAUNCHER_BUILD_DIR"
    "${CC:-cc}" -Os macos/sdrpp_app_launcher.c -o "$LAUNCHER_BINARY"
    # The launcher only uses libSystem, so it needs no dependency rewriting.
    cp "$LAUNCHER_BINARY" "$BUNDLE/Contents/MacOS/$EXECUTABLE"
fi

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

# Optional SoapySDDC module for the dedicated RX888 bundle flow. It is not a
# linked dependency of rx888_source, so ordinary dependency traversal cannot
# discover it. Keep its module-relative rpath distinct from normal Plugins/.
if [ -n "${SDDC_SUPPORT_LIBRARY:-}" ]; then
    if [ ! -f "$SDDC_SUPPORT_LIBRARY" ]; then
        echo "ERROR: SDDC_SUPPORT_LIBRARY not found: $SDDC_SUPPORT_LIBRARY" >&2
        exit 1
    fi

    SDDC_DEST_DIR="$BUNDLE/Contents/SoapySDR/modules0.8"
    SDDC_DEST="$SDDC_DEST_DIR/libSDDCSupport.so"
    mkdir -p "$SDDC_DEST_DIR"
    cp "$SDDC_SUPPORT_LIBRARY" "$SDDC_DEST"

    SDDC_RPATHS=$(bundle_get_exec_rpaths "$SDDC_DEST")
    if [ -n "$SDDC_RPATHS" ]; then
        echo "$SDDC_RPATHS" | while IFS= read -r SDDC_RPATH; do
            install_name_tool -delete_rpath "$SDDC_RPATH" "$SDDC_DEST"
        done
    fi
    install_name_tool -add_rpath @loader_path/../../Frameworks "$SDDC_DEST"

    SDDC_DEPS=$(bundle_get_exec_deps "$SDDC_DEST")
    echo "$SDDC_DEPS" | while IFS= read -r SDDC_DEP; do
        case "$SDDC_DEP" in
            /System/Library/*|/usr/lib/*) continue ;;
        esac
        SDDC_DEP_NAME=$(basename "$SDDC_DEP")
        if [ ! -f "$BUNDLE/Contents/Frameworks/$SDDC_DEP_NAME" ]; then
            echo "ERROR: SDDC dependency missing from bundle: $SDDC_DEP_NAME" >&2
            exit 1
        fi
    done
fi

# ========================= Finalize =========================

# Sign the app
bundle_sign $BUNDLE
