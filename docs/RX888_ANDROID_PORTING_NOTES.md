# RX888 Android Porting Notes

Last updated: 2026-07-27

This document tracks the current SDR++ Android RX888 MkII porting work, the
runtime behavior observed on real phones, and the open questions still being
debugged. It is intended as a practical handoff, not a polished upstream design
document.

## Checkout And Target

- Working tree: `/Users/willdarney/Documents/SDR++/SDRPlusPlus-rx888-android`
- Android package: `org.sdrpp.sdrpp`
- Primary test device: Samsung `SM-G781V`, ADB serial `RFCN90XR6AP`
- Wireless debugging IP currently reported by phone: `192.168.1.122`
- RX hardware: RX888 MkII
- Phone USB test state:
  - Older phone: USB 2.0, usable at low rates but slow.
  - Samsung: USB 3.0 path confirmed by high reported USB bandwidth.

## Build And Install

Build from the Android directory:

```zsh
cd /Users/willdarney/Documents/SDR++/SDRPlusPlus-rx888-android/android
JAVA_HOME="/opt/homebrew/Cellar/openjdk@11/11.0.30/libexec/openjdk.jdk/Contents/Home" \
ANDROID_SDK_ROOT="$HOME/Library/Android/sdk" \
ANDROID_HOME="$HOME/Library/Android/sdk" \
/Users/willdarney/.gradle-versions/gradle-6.8.3/bin/gradle assembleRelease
```

Install over USB:

```zsh
$HOME/Library/Android/sdk/platform-tools/adb -s RFCN90XR6AP install -r app/build/outputs/apk/release/app-release.apk
```

Wireless debugging note:

- `adb connect 192.168.1.122:5555` failed with connection refused.
- This phone is likely using Android's modern wireless pairing flow.
- Use `Developer options -> Wireless debugging -> Pair device with pairing code`.
- Pair with the pairing IP/port and code, then connect to the separate wireless
  debugging IP/port shown on that screen.

## Android Build Configuration

`android/app/build.gradle` now enables the Android RX888 module and Channel Bank:

- `OPT_BUILD_RX888_SOURCE=ON`
- `OPT_BUILD_SDDC_SOURCE=OFF`
- `OPT_BUILD_ANDROID_AUDIO_SINK=ON`
- `OPT_BUILD_AUDIO_SINK=OFF`
- `OPT_BUILD_CHANNEL_BANK=ON`
- `OPT_BUILD_VDL2_DECODER=OFF`

Release builds currently use the debug signing config so the APK can be
installed directly during testing.

## RX888 Source Module Work

The RX888 module has an Android-specific entrypoint:

- Desktop: `source_modules/rx888_source/src/main.cpp`
- Android: `source_modules/rx888_source/src/android_main.cpp`

The Android build now compiles the imported SDDC/RX888 core as a static helper
library, links against Android `libusb1.0.so` and `libfftw3f.so` from
`/sdr-kit/${ANDROID_ABI}`, and excludes AVX code paths. NEON is enabled for ARM
ABIs where appropriate.

Implemented Android-facing module behavior includes:

- Android USB permission flow for the Cypress/WestBridge device.
- Firmware loading path for the RX888.
- HF/VHF mode controls.
- Gain controls.
- Sample rate selection.
- ADC clock reporting.
- The RX888 source module UI is now back to normal controls only; the temporary
  runtime stats panel was removed after it started adding UI churn during tests.

Observed behavior:

- Firmware permission prompt appears as expected.
- After accepting permission, RX888 can stream and produce a live waterfall.
- HF appeared quiet at one point because the SDR++ offset was entered as
  `-110.000` instead of `-110000000.000000`.
- VHF mode gain controls were confirmed to affect the waterfall.
- USB errors/drops have generally remained at `0`.

## RX888 Sample Rates And Throughput

For the current 128 MHz ADC clock mapping:

- 2 MHz uses decimation 5, selector 0.
- 4 MHz uses decimation 4, selector 1.
- 8 MHz uses decimation 3, selector 2.
- 16 MHz uses decimation 2, selector 3.
- 32 MHz uses decimation 1, selector 4.

Observed stats during testing:

- At 8 MHz, stream rate initially reported around `7.16` to `7.20 MS/s` before
  the later USB async changes.
- At 16 MHz on the Samsung USB 3.0 phone, the waterfall was surprisingly usable.
- Reported USB bandwidth exceeded `200 MB/s` in some configurations and was
  around `165 MB/s` at 16 MHz in one later test.
- USB drops/errors remained at `0`.
- Process CPU was observed around `188%` and later around `254%`.

## Android FX3 USB Streaming

The Android FX3 handler was changed from a single synchronous
`libusb_bulk_transfer` loop to queued asynchronous libusb transfers.

Current async design:

- Uses multiple outstanding transfer slots.
- Each slot owns its own transfer buffer.
- Transfer callback copies completed chunks into the SDR++ ring buffer.
- Tracks USB byte count, transfer count, and error count.
- Cancels and drains pending transfers on stop.

Reason for the change:

- The waterfall showed horizontal lines and electronic noise-like artifacts.
- USB errors stayed at `0`, so the suspected issue was not device-level USB
  failure but possible synchronous transfer boundary starvation.

Open question:

- The async path improved the architecture, but the horizontal-line artifact has
  not been isolated conclusively from the later audio/Channel Bank work.

## RX888 Runtime Telemetry

The RX888 module UI no longer shows runtime telemetry.

Notes:

- CPU frequency and thermal polling were tried but removed from the hot UI stats
  path because they caused UI skipping/jumping.
- The later stream/USB/peak/CPU/error stats panel was also removed from the
  source module UI after user testing showed the normal Channel Bank
  `Max Channels` setting was the useful load-control knob.
- UI responsiveness generally stayed good even when audio skipped.

## Android Audio Sink Work

The Android audio sink has been converted from blocking `AAudioStream_write` to
an AAudio callback model.

Current behavior:

- The AAudio device callback drains an internal queue.
- A worker thread reads SDR++ audio from `packer.out` and fills that queue.
- The callback zero-fills when the queue runs dry.
- The sink prebuffers before starting the AAudio stream.
- The UI now shows only a compact audio status line with mode, sample rate,
  buffer size, and AAudio xruns. The earlier worker-frame, queue-depth,
  direct-write, and callback-underrun counters were useful diagnostics, but
  were removed from the visible menu to reduce UI churn during RX888 testing.

Important test observations:

- Initial callback versions showed rapidly increasing callback underruns and
  pulsed audio.
- Switching from `try_lock` to blocking lock in the callback reduced false
  underruns.
- Prebuffering and a larger queue improved playback.
- At 8 MHz, audio became mostly usable with occasional skips.
- At 16 MHz, audio skipped a little, and skips tended to line up with UI stats
  hitches.
- AAudio xruns can be `0` while callback underruns still reveal starvation.

## Channel Bank Android Enablement

Channel Bank is now included in the Android build.

Android-specific build changes:

- `misc_modules/channel_bank/CMakeLists.txt` excludes
  `src/transcription_whisper.cpp` on Android.
- Android builds define `CB_NO_WHISPER`.
- Minor integer type fixes were applied for Android compilation.

Feature scope on Android right now:

- Channel detection and temporary WAV capture are expected to work.
- Whisper transcription is disabled.
- M4A encode paths remain desktop/macOS/Windows guarded.

## Frequency Manager Config Injection

The desktop frequency manager config was injected into the phone from:

```text
/Users/willdarney/Library/Application Support/sdrpp/frequency_manager_config.json
```

Destination used on Android:

```text
/data/user/0/org.sdrpp.sdrpp/files/frequency_manager_config.json
```

The config was copied with a temporarily debuggable build via `run-as`, then a
non-debuggable release was reinstalled. The config appeared to persist.

## Channel Bank Playback Issue

Current user-visible issue:

- Channel Bank catches a transmission.
- The playback icon flashes.
- Recordings are off.
- Expected behavior: scratch WAV should still be played back, then deleted.
- Actual behavior: playback appears to happen internally, but no audio is heard.

Important debugging result:

- `Playback: start` climbs.
- `openFail`, `bad`, `noData`, and `swapFail` stay at `0`.
- `samples` climbs, for example above `1,287,196`.

Meaning:

- The scratch WAV exists.
- Channel Bank can open and parse the WAV.
- The WAV contains data.
- `monitorStream.swap()` succeeds.
- The failure is downstream of WAV file reading.

Critical discovery:

- `monitorStream.swap()` succeeding does not prove audio is reaching the Android
  speaker.
- SDR++ `NullSink` also reads from streams and discards samples.
- Therefore the existing counters can all look good while playback is being
  consumed by `None`.

Current fix attempt:

- Added `SinkManager::getStreamSink()`.
- Channel Bank now displays:
  - `Monitor sink: ...`
  - `Playback: start openFail bad noData swapFail samples`
- On Android, Channel Bank tries to force its monitor stream to `Audio`:
  - When the monitor stream is registered.
  - When the `Audio` sink provider is registered.
  - Immediately before each WAV playback.
- Restoring continuous silence on the Channel Bank monitor stream did not fix
  playback by itself.
- A diagnostic Android audio sink path was added: streams whose name contains
  `_monitor` use direct `AAudioStream_write`, while the main VFO stream remains
  callback-buffered.
- With direct monitor mode installed, Channel Bank preview audio became audible.
- Direct monitor playback was reported clear, without skips, and not sped up.
- After the phone heated up, the direct monitor playback began to sound like
  thermal/CPU throttling again. This points to remaining system load/headroom,
  not the original silent playback bug.
- Clarification from testing: the electronic noise/horizontal-line artifact is
  present in the Channel Bank recordings themselves. That means the direct
  playback path can be working while the WAV content is already corrupted or
  discontinuous upstream.
- A follow-up lower-load test build was prepared: Android Channel Bank monitor
  playback stays direct-write, but the monitor thread no longer writes
  continuous idle silence between queued WAVs.
- Another diagnostic build was installed after noticing the muted main VFO
  audio sink still had many callback underruns. SDR++ mute only zeroes volume;
  it does not stop the audio sink. The Android callback sink now stops its
  AAudio callback while muted and drops queued audio instead of accumulating
  callback underruns.

Conclusion:

- Channel Bank detection, scratch WAV creation, WAV parsing, and monitor stream
  writes are working.
- The silent playback bug is specific to using the callback-buffered Android
  audio sink for the secondary Channel Bank monitor stream.
- The earlier sped-up/skipping preview symptom is also tied to the callback
  queue path, not the WAV content or Channel Bank sample timing.
- Multiple independent AAudio outputs may still be expensive under RX888 load;
  direct monitor mode proves the path but may not be the final architecture.
- Continuous idle silence on the monitor path is unnecessary for direct-write
  mode and may add heat/load, so Android direct monitor playback should remain
  burst-only unless a callback-mode test specifically needs silence prefeed.
- Muted Android callback sinks should not keep running their AAudio callback.
  This is especially important when the main VFO is muted while Channel Bank is
  the only audio the user wants to hear.
- Because the artifact is baked into the recording, the next diagnostics should
  look at RX888 USB/sample continuity and the per-slot Channel Bank DSP chain,
  not just Android audio output.
- A temporary RX888 instrumentation build added:
  - `USB errors ..., short ...`
  - `USB write waits ..., max ...us`
- `USB write waits` counted times the libusb completion callback had to wait
  more than 1 ms for the RX888 input ringbuffer write slot. When this climbed
  during electronic-noise/horizontal-line artifacts, it showed the R2IQ and
  downstream consumer side was falling behind USB input.
- Test result: `USB write waits` reached about `32k` and was rising rapidly
  while the audio returned to the old electronic/stuttered sound. This confirms
  downstream RX888/R2IQ processing was not keeping up with USB input.
- A follow-up build changed the Android RX888 callback to use a non-blocking
  ringbuffer write probe. If the input ring is full, the transfer is counted as
  `USB ring drops` and immediately resubmitted instead of blocking the libusb
  callback thread. This should prevent catastrophic USB resubmission stalls, but
  rising ring drops still mean the selected rate/load is too high for the phone.
- Test result: non-blocking callback kept write waits low (`6` observed), but
  `USB ring drops` rose rapidly and the recording/audio artifact sounded similar.
  This showed the phone was still overloaded; the build was explicitly dropping
  RX888 ADC blocks instead of stalling transfer resubmission.
- Final cleanup removed the non-blocking ring-drop path and the temporary
  wait/drop/short-transfer UI counters. The RX888 callback is back to the
  blocking ringbuffer write path, and the source module stats panel is hidden.
- A short-lived Android-specific Channel Bank cap was also removed. Lowering the
  normal Channel Bank `Max Channels` fixed the audio issue and stopped the wait
  numbers in testing, so `Max Channels` should remain the single visible load
  control for now.

Next test:

- Reproduce a Channel Bank playback event.
- Confirm whether main VFO audio worsens while Channel Bank monitor is using
  direct AAudio writes.
- If Channel Bank monitor playback becomes silent again, temporarily re-enable
  the hidden direct-write/worker counters in the Android audio sink.
- Decide whether to keep direct mode for monitor streams or replace both paths
  with a shared Android mixer/single AAudio output.

## Current Open Questions

1. Does Channel Bank monitor playback remain on `Audio` during actual playback?
2. If monitor sink is `Audio`, does the Android audio sink worker receive queued
   frames from the Channel Bank monitor stream?
3. Are there multiple Android AAudio sink instances, and does the OS/device allow
   them to play simultaneously in this SDR++ module arrangement?
4. Are RX888 horizontal waterfall artifacts still present after async USB, or
   were the later symptoms mostly audio/Channel Bank related?
5. At high RX888 bandwidths, are remaining audio skips caused by CPU saturation,
   audio callback starvation, or a specific DSP path?

## Suggested Next Experiments

Channel Bank playback:

1. Measure main VFO underruns while Channel Bank direct monitor playback is
   active.
2. Confirm muting the main VFO no longer worsens Channel Bank monitor playback.
3. Test the lower-load direct monitor build that does not write idle silence.
4. On Android, use the normal `Max Channels` setting to find the phone's stable
   per-band/channel-count limit without adding a hidden Android-only cap.
5. If main VFO audio suffers, avoid multiple independent AAudio output streams.
6. Consider a shared Android mixer sink or route Channel Bank preview through
   the existing main audio path instead of creating a second output device
   stream.
7. Keep direct monitor mode as a known-good diagnostic/reference path.

RX888 performance:

1. Keep source-module runtime telemetry disabled unless a specific diagnostic
   build needs it.
2. Avoid polling CPU frequency/thermal data in the UI loop.
3. Compare 8 MHz and 16 MHz with Channel Bank disabled and enabled.
4. Keep watching USB errors/drops; so far they have not implicated USB loss.
5. If waterfall stripes or electronic recording artifacts persist, add cheap
   block continuity/latency counters at the FX3 callback boundary, after R2IQ
   conversion, and inside Channel Bank per-slot audio handlers.

## Files Touched In This Work Area

Primary Android RX888 files:

- `source_modules/rx888_source/CMakeLists.txt`
- `source_modules/rx888_source/src/android_main.cpp`
- `source_modules/rx888_source/sddc_core/arch/android/FX3handler_android.cpp`
- `source_modules/rx888_source/sddc_core/arch/android/FX3handler_android.h`
- `source_modules/rx888_source/sddc_core/dsp/ringbuffer.h`

Android app/build files:

- `android/app/build.gradle`
- `android/app/src/main/java/MainActivity.kt`
- `core/backends/android/backend.cpp`
- `core/backends/android/android_backend.h`

Audio and sink routing:

- `sink_modules/android_audio_sink/src/main.cpp`
- `core/src/signal_path/sink.cpp`
- `core/src/signal_path/sink.h`

Channel Bank:

- `misc_modules/channel_bank/CMakeLists.txt`
- `misc_modules/channel_bank/src/main.cpp`

Related source/module files also have local changes in this worktree. Review
`git status --short` before staging or splitting commits.
