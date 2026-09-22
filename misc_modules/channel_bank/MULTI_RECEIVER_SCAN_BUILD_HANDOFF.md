# Multi-Receiver Scan: macOS build handoff

Implementation branch: `codex/channel-bank-multi-receiver-scan`

Feature commit: `2932708f995e076ea1eaab3f759d052117f7decf` (initial implementation; build the current branch tip for playback and warm-receiver fixes)

Base: `origin/integration/main` at `7ad6434ce6019b2933ba997a906e25528c8d8c41`

This feature adds **Multi-Receiver Scan** to desktop Channel Bank. One user-selected SDR++ source scans the configured ranges for activity. At Channel Bank Start, the configured transmission receivers are opened and kept ready. A separate dispatch thread assigns each detected frequency to a transmission source, reusing an active receiver when its current IQ window covers the frequency and otherwise tuning an open, idle receiver. Each assignment uses the existing Channel Bank channel, demodulation, recording, playback, transcription, and blocking paths. Once assigned, the transmission receiver monitors its own RF presence; discovery moving to another scan stop does not end its channel.

## Core and source changes

The feature changes two SDR++ core files:

- `core/src/signal_path/source.h` adds optional source sample-rate/running callbacks and an independent-source lease API.
- `core/src/signal_path/source.cpp` implements source discovery, ownership, start/stop/tune, IQ stream access, and synchronization. An independently leased source cannot become SDR++'s globally selected source while leased.

The following source modules supply the optional callbacks and can be selected for the transmission pool: `rtl_sdr_source`, `airspy_source`, `airspyhf_source`, `soapy_source`, and `rx888_source`. The `rx888_source` adapter also guards global sample-rate updates when started as an independent transmission source, so it cannot overwrite the discovery receiver's rate. The desktop Channel Bank changes are in `misc_modules/channel_bank/src/main.cpp`; allocator logic and its unit test are under that module. The iOS Channel Bank implementation was not changed.

Because the shared source interface changed, a macOS build needs a matching **`sdrpp_core` and `sdrpp`**, plus `channel_bank` and whichever of those source modules the build will load. Replacing only `channel_bank.dylib` in an older application is insufficient.

## Build and static checks

From a clean checkout of this branch, use a fresh build directory. A focused macOS configuration is:

```sh
cmake -S . -B build-multi-receiver-scan \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBUILD_TESTING=ON \
  -DOPT_BUILD_CHANNEL_BANK=ON \
  -DOPT_BUILD_RX888_SOURCE=ON \
  -DOPT_BUILD_SOAPY_SOURCE=ON

cmake --build build-multi-receiver-scan --target \
  sdrpp_core sdrpp channel_bank \
  rtl_sdr_source airspy_source airspyhf_source soapy_source rx888_source \
  channel_bank_multi_receiver_allocator_test -j8

ctest --test-dir build-multi-receiver-scan/misc_modules/channel_bank --output-on-failure
```

The feature branch was configured and built on macOS with those targets. The allocator test passed (1/1), and `git diff --check` was clean. These results establish source/build/logic validation; they do not establish application launch, RF reception, or attached-SDR behavior.

The separate legacy `sddc_source` target could not be configured in this checkout because its vendored `libsddc.pc.in` is absent. The `rx888_source` target backed by SoapySDDC built successfully. That legacy target is not needed for the focused command above.

## Interface and current limits

The desktop UI adds the mode, Discovery Receiver selection, ordered Transmission Receiver Pool, Maximum Monitor Time, and a live Transmission Receiver Activity list showing each receiver's assigned channel frequencies. A receiver with no channel reads `READY (open)` while scanning. Active channel rows also name the assigned receiver. The Web UI's Active Channels table has a Receiver column. Channel Bank settings expose `mode: "multi_receiver_scan"`, `discoveryReceiver`, `transmissionReceiverPool`, `maximumMonitorSec`, and `supportsMultiReceiverScan`. The existing state response includes a separate `multiReceiverScan` object with receiver assignments, dispatch states, and failure counts; each active channel also includes `receiverId`. Structural settings still require Channel Bank to be stopped.

The desktop Transmission Receiver section also shows configured gain stages and permits manual live gain changes for RX888, RTL-SDR, Airspy, and Airspy HF+ through their existing source-control interfaces. These are configured values, not hardware readback. SoapySDR currently has no matching source-control interface and displays gain controls unavailable; its gain is still set in the source module's own settings. Blocking a frequency now interrupts its current Channel Bank monitor playback, removes matching queued playback without deleting kept recordings, and clears matching buffered browser audio. Already-delivered audio in an output device or browser cannot be recalled.

Receiver identifiers currently come from registered SDR++ source instances. The adapters listed above register one instance per driver type in the current upstream source modules. Running several dongles of the *same* driver type concurrently will need those modules to register distinct source instances; this feature does not create them automatically.

The user will perform application and SDR hardware testing. Useful checks are startup with all configured transmission receivers visibly ready, immediate discovery advancement after detection, uninterrupted reception/playback as discovery advances, multiple VFOs on one receiver's IQ span, idle-receiver allocation, no-capacity behavior, shared blocking, maximum-monitor suppression and carrier-clear reset, receiver disconnect, and clean stop/restart. For audible playback, select a real audio output for the Channel Bank monitor sink; a `None` sink will still permit recording/queueing but cannot play sound. Do not deploy or alter an installed SDR++ app solely from this handoff.
