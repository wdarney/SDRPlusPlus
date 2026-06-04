#!/bin/zsh
# Build + deploy channel_bank.dylib into the local testing app, and re-sign.
#
# Signing strategy (macOS 26.x / Tahoe — IMPORTANT):
# `codesign --deep` is deprecated and on Tahoe it silently skips re-signing
# nested plugins that already carry a linker-signed signature.  Result: the
# bundle reseals, the plugin keeps its stale signature, the kernel's runtime
# page-hash check fails, and the app dies on launch with SIGKILL (Code
# Signature Invalid).  Sign the plugin explicitly FIRST, then the bundle.
set -e

APP="/Applications/SDR++MODULETESTING.app"
DYLIB="build/misc_modules/channel_bank/channel_bank.dylib"
PLUGIN_DEST="$APP/Contents/Plugins/channel_bank.dylib"

cd "$(dirname "$0")"

SDKROOT=$(xcrun --show-sdk-path) cmake --build build --target channel_bank
cp "$DYLIB" "$PLUGIN_DEST"

# Clear any xattrs Spotlight/quarantine/Time Machine may have left on the
# fresh file.  Some xattrs cause codesign to behave oddly.
xattr -c "$PLUGIN_DEST" 2>/dev/null || true

# Re-sign the plugin EXPLICITLY (no --deep — see header).
codesign --force --sign - "$PLUGIN_DEST"

# Now re-seal the bundle so its outer signature covers the new plugin hash.
codesign --force --sign - "$APP"

echo "Done — deployed and re-signed."
