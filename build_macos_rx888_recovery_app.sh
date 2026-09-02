#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build_dir=${1:-"$script_dir/build-rx888-recovery-macos"}
bundle=${2:-/Applications/SDR++-RX888-Recovery.app}
jobs=${JOBS:-$(sysctl -n hw.logicalcpu)}

sddc_ref=2c89b681bb896ac43f4357f53db5ef92ca195121
sddc_short=$(printf '%s' "$sddc_ref" | cut -c1-12)
sddc_repo=${SDDC_DRIVER_REPOSITORY:-https://github.com/wdarney/SDDC_Driver.git}
default_sddc_source="$script_dir/../SDDC_Driver-rx888-macos-integration"

if [ -n "${SDDC_SOURCE_DIR:-}" ]; then
    sddc_source=$SDDC_SOURCE_DIR
elif [ -d "$default_sddc_source/.git" ] || [ -f "$default_sddc_source/.git" ]; then
    sddc_source=$default_sddc_source
else
    sddc_source="$build_dir/_deps/SDDC_Driver-$sddc_short"
    if [ ! -d "$sddc_source/.git" ]; then
        mkdir -p "$(dirname "$sddc_source")"
        git clone --filter=blob:none "$sddc_repo" "$sddc_source"
    fi
    git -C "$sddc_source" fetch origin "$sddc_ref"
    git -C "$sddc_source" checkout --detach "$sddc_ref"
fi

actual_sddc_ref=$(git -C "$sddc_source" rev-parse HEAD)
if [ "$actual_sddc_ref" != "$sddc_ref" ]; then
    echo "ERROR: expected SDDC driver $sddc_ref, found $actual_sddc_ref in $sddc_source" >&2
    exit 1
fi

cmake -S "$script_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DOPT_BUILD_RX888_SOURCE=ON \
    -DOPT_BUILD_CHANNEL_BANK=ON \
    -DOPT_BUILD_CH_EXTRAVHF_DECODER=ON \
    -DOPT_BUILD_VDL2_DECODER=ON \
    -DOPT_BUILD_SELCAL_DECODER=ON \
    -DOPT_BUILD_PLUTOSDR_SOURCE=OFF
cmake --build "$build_dir" --target \
    sdrpp \
    airspy_source airspyhf_source file_source network_source \
    rtl_sdr_source rtl_tcp_source sdrpp_server_source rx888_source \
    audio_sink network_sink radio ch_extravhf_decoder vdl2_decoder selcal_decoder \
    channel_bank frequency_manager recorder rigctl_client rigctl_server scanner \
    -- -j"$jobs"

export SDRPP_APP_NAME=sdrpp-rx888-recovery
export SDRPP_DISPLAY_NAME="SDR++ RX888 Recovery"
export SDRPP_BUNDLE_ID=org.sdrpp.sdrpp.rx888recovery
export SDRPP_APP_SIGNATURE=sdrr
export SDRPP_EXECUTABLE_NAME=sdrpp
export SDRPP_ICON_NAME=sdrpp
export SDRPP_USE_APP_LAUNCHER=1

# Create the app once so the driver links against the exact Frameworks that
# will be packaged, then recreate it with the resulting pinned Soapy module.
sh "$script_dir/make_macos_bundle.sh" "$build_dir" "$bundle"

sddc_build_dir="$build_dir/sddc-driver-build"
sddc_artifact_dir="$build_dir/sddc-driver-artifact"
SDDC_BUILD_DIR="$sddc_build_dir" \
SDDC_ARTIFACT_DIR="$sddc_artifact_dir" \
    "$sddc_source/scripts/build_macos_arm64_accelerate_sdrpp_bundle.sh" "$bundle"

export SDDC_SUPPORT_LIBRARY="$sddc_artifact_dir/lib/SoapySDR/modules0.8/libSDDCSupport.so"
sh "$script_dir/make_macos_bundle.sh" "$build_dir" "$bundle"

codesign --verify --deep --strict --verbose=2 "$bundle"
echo "Built RX888 recovery app at $bundle"
echo "Pinned SDDC driver: $sddc_ref"
