# Multi-Receiver Scan: macOS build handoff

Implementation branch: `codex/channel-bank-multi-receiver-scan`

Initial feature commit: `2932708f995e076ea1eaab3f759d052117f7decf`. The current branch tip for the features below is `6aa89dbb` (receiver and VFO activity display); do not build only the initial commit.

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

The desktop UI adds the mode, Discovery Receiver selection, ordered Transmission Receiver Pool, Maximum Monitor Time, and a live Transmission Receiver Activity list showing each receiver's assigned channel frequencies. An open receiver with no assigned channel shows its commanded SDR center and is marked parked. Active channel rows also name the assigned receiver. The Web UI's Active Channels table has a Receiver column. Channel Bank settings expose `mode: "multi_receiver_scan"`, `discoveryReceiver`, `transmissionReceiverPool`, `maximumMonitorSec`, and `supportsMultiReceiverScan`. The existing state response includes a separate `multiReceiverScan` object with receiver assignments, dispatch states, and failure counts; each active channel also includes `receiverId`. Structural settings still require Channel Bank to be stopped.

The desktop Transmission Receiver section also shows configured gain stages and permits manual live gain changes for RX888, RTL-SDR, Airspy, and Airspy HF+ through their existing source-control interfaces. These are configured values, not hardware readback. SoapySDR currently has no matching source-control interface and displays gain controls unavailable; its gain is still set in the source module's own settings. Blocking a frequency now interrupts its current Channel Bank monitor playback, removes matching queued playback without deleting kept recordings, and clears matching buffered browser audio. Already-delivered audio in an output device or browser cannot be recalled.

Multi-receiver dispatch now carries the discovery detector's NMS exclusion across FFT frames: a pending or active channel prevents a neighboring grid hit within the configured NMS radius from opening another VFO. This addresses duplicate off-frequency recordings from one transmission on adjacent 8.33 kHz channels; it does not alter the normal discovery detector or intentional allocations farther apart.

The discovery grid frequency remains the stable assignment/blocking identity, but the dispatched receiver and VFO now tune to the detector's measured carrier centroid (bounded to one channel spacing from that grid). The receiver-side RF observer uses the same measured center. This prevents a single accepted grid cell from recording several kilohertz beside its visible carrier. The ACTIVE log reports both grid and carrier frequencies so a macOS RF test can verify centering.

The desktop Transmission Receiver Activity panel now distinguishes each SDR's last commanded center from each assigned VFO frequency and grid identity. Per-VFO labels show `SIGNAL`, `HOLD`, `QUIET / ASSIGNED`, `RELEASING`, and `REC` where applicable; HOLD/QUIET entries are still allocated but do not imply a fresh detection. An open receiver without channels is marked parked. The `multiReceiverScan.receivers` status preserves its existing `centerHz`, `sampleRate`, and numeric `channels` fields and adds `tunedCenterHz` plus `channelDetails` with the same live state flags. These are commanded tuning values, not hardware frequency readback.

## Scan settling and combined build history

As of 2026-09-23, `integration/main` already contains the scan-settling work through `0181f692`: Scan settling is configurable from 250 to 2,000 ms, with a 250 ms default. The setting applies at the next acknowledged retune; both discarded IQ and wall time must satisfy it, followed by three fresh FFT frames. See `SCAN_SETTLING_BUILD_HANDOFF.md` on the integration branch for the setting and validation details. The multi-receiver feature branch alone does not contain those scan-settling commits; build the combined integration baseline for both features.

The earlier macOS test combination used fixed 350 ms settling (`28da0509`) and receiver-status commit `6aa89dbb`, cherry-picked locally as `e959d144`. That historical build does not establish validation of the later configurable setting. Its one-line allocator fixture correction is now included in the feature source: `nms.failPending(124570);` in `tests/multi_receiver_allocator_test.cpp`. It removes stale fixture state before the next assertion. The correction changes only the test source and its test executable, **not** the Channel Bank or SDR++ app binaries. The scan-settling and receiver-status commits change production Channel Bank code and its app binary.

## Receiver release and RF acceptance

An assigned channel is meant to remain on its transmission receiver as discovery moves on. Once the receiver-side observer reports no signal beyond Signal Hold, Channel Bank requests release and removes the assignment after any open recording closes. Normal release removes its dispatch key, allowing rediscovery. Maximum Monitor Time instead suppresses the key until discovery later observes the carrier cleared at that scan stop. Pending or active keys also exclude nearby grid hits through the NMS radius. Thus an assignment that never releases can appear to prevent nearby respawns.

The current receiver observer uses `max(1 dB, SNR threshold - hold hysteresis)` for presence. For example, 4 dB SNR and 4 dB hysteresis yield a 1 dB presence test. Persistent noise classification is a **hypothesis**, not a confirmed cause of a stuck channel. During RF testing, record the frequency, receiver, elapsed time after the transmission ends, and whether its VFO stays at `SIGNAL`, `HOLD`, `QUIET / ASSIGNED`, or `RELEASING`; keep the nearby Channel Bank log. `SIGNAL` persisting in silence points toward receiver-side presence detection; prolonged `QUIET / ASSIGNED` or `RELEASING` points toward a different release/recording path. Do not treat the new labels as a release fix.

Receiver identifiers currently come from registered SDR++ source instances. The adapters listed above register one instance per driver type in the current upstream source modules. Running several dongles of the *same* driver type concurrently will need those modules to register distinct source instances; this feature does not create them automatically.

The user will perform application and SDR hardware testing. Useful checks are startup with all configured transmission receivers visibly ready, immediate discovery advancement after detection, uninterrupted reception/playback as discovery advances, multiple VFOs on one receiver's IQ span, idle-receiver allocation, no-capacity behavior, shared blocking, maximum-monitor suppression and carrier-clear reset, receiver disconnect, and clean stop/restart. For audible playback, select a real audio output for the Channel Bank monitor sink; a `None` sink will still permit recording/queueing but cannot play sound. Do not deploy or alter an installed SDR++ app solely from this handoff.
