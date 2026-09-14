# Channel Bank BLE iOS Client Handoff

## Blocked SNR Display (2026-09-14)

The iPhone SNR chart now combines blocked history frequencies with telemetry
block flags using the server's rounded-kHz identity. It draws red frequency
markers even when a sampled SNR bucket is absent or near zero, and matching
bars remain red with a three-pixel minimum height. This is an iOS-only display
change; no Mac or Android server rebuild is needed for it. All 40 core tests
pass, including blocked history with absent/unflagged telemetry and low-SNR
telemetry-only blocks. Live chart appearance still needs iPhone confirmation.

## Binary Audio Trial (2026-09-14)

The optional macOS/iPhone binary notification transfer is on `wd/ble-binary-audio`.
It sends bounded, checksum-validated windows instead of Base64 Response pages,
with retries and same-lease paged fallback. Both endpoints need rebuilding;
an old Mac server continues using paged audio. See
[BINARY_AUDIO_HANDOFF.md](BINARY_AUDIO_HANDOFF.md) for the exact protocol, build
boundary, tests and physical-device throughput checklist. Local Mac Channel Bank
playback/recording and Android are unchanged. Live speed is not yet validated.

## Bluetooth AAC/M4A Transfer (2026-09-13)

The iPhone now requests `body.encoding: "aac"` when creating a current-playback
lease. The updated Mac Bluetooth adapter prepares a private AAC/M4A copy on a
background thread and can return 202 with `preparing:true`, `transferId`,
`offset:0`, and `retryAfterMs`. The client preserves the lease, shows preparation
status, and retries the same offset until normal Base64 audio pages arrive.
Preparation sleeps are cancelable and waits are bounded. WAV responses from
older servers remain accepted. Existing M4A playback uses AVFoundation.

The Mac implementation is commit `7e241d9e` on `wd/channel-bank-macos-telemetry`, in
`bluetooth_macos.mm` and `bluetooth_audio_macos.h`. It leaves the local Mac
Channel Bank, recording encoder, playback, transcription and WebUI unchanged.
Its synthetic ten-second audio test reduced 960,044 WAV bytes to 51,434 M4A
bytes and decoded all 240,000 output frames at 24 kHz. Actual BLE latency and
audible playback still require a rebuilt Mac server and the updated iPhone app.

The iOS core suite now passes 32 tests, covering the AAC request, 202 decoding,
stable lease and offset through preparation, lease mismatch, and cancellation.
The signed iPhone build and strict signature verification passed, and the app
was installed on the paired iPhone. The Mac build session must integrate
`7e241d9e` on top of its existing Bluetooth/telemetry adapter and rebuild the
test app before compressed transfer is available. Do not modify `main.cpp` or
the local recording encoder when integrating this change.

## Audio Stuck Pulling Follow-Up (2026-09-13)

Reported symptom: Monitor remains at `Pulling...` with a filename and never
reaches local playback. Source inspection found that a changed State playback
identity canceled the current leased download. If clips advance faster than
Bluetooth can download them, the monitor repeatedly restarts before EOF.

The client now finishes its current download and local playback before following
the latest State playback identity. It does not accumulate a queue of old clips.
The server's existing transfer lease preserves the open file across playback
changes/deletion. Pressing Monitor again while busy no longer restarts the pull.
Disconnect still cancels the transfer and releases its lease.

Each returned page updates visible downloaded/total bytes. The original pull
failure is retained in diagnostics before attempting the recordings fallback.
AVFoundation preparation/start failures now report failure instead of claiming
`Playing`, and its completion delegate clears the playback status and resumes
monitoring. A Mac server still needs its existing leased-audio implementation;
no server changes are part of this client fix.

Physical acceptance: while the Mac advances through several transmissions,
confirm one download's byte count keeps advancing to EOF, that clip is audible,
and monitoring follows the latest clip after playback finishes. The initial
page still needs to arrive before a total file size can be displayed. Large WAV
files can take a long time over the reliable BLE response channel.

Validation: the existing 28 core tests pass; signed iPhone build and strict
signature verification passed; the update installed on the paired iPhone.
Audible playback with advancing Mac transmissions remains a device test.

## iPhone UI Hang Follow-Up (2026-09-13)

The user reports that tapping a control such as SNR + or - can freeze the
iPhone interface itself, including scrolling. This is distinct from a chart
that pauses while a large Bluetooth State or Response message is transferring.

Source inspection found that command tasks called `writeCommandFrame` and its
`@Published` diagnostics from a background executor while CoreBluetooth and
SwiftUI were publishing on the main thread. Request registration, response
delivery, and timeouts also accessed the same pending-request dictionary from
different executors. This provides a plausible UI deadlock/data-race cause;
a device hang stack was not captured.

The iOS fix:

- Isolates BLE UI state, command transport, and request tracking to the main
  actor, matching CoreBluetooth's explicitly configured main delegate queue.
- Completes each request once and cancels its writer and timeout task on
  response, cancellation, timeout, or disconnect.
- Routes responses using only their ID before decoding the typed body, avoiding
  a redundant decode of the entire State into generic JSON.
- Keeps assembled audio file writes off the main actor.

The core suite passes 28 tests, including immediate replies, 24 concurrent
commands with out-of-order replies, cancellation without a server response,
timeout during a fragmented write, and disconnect followed by a late reply.
The signed Debug iOS build succeeded and installed on the paired iPhone.
Strict signature verification passed on a metadata-free copy under `/private/tmp`;
Finder kept reattaching disallowed extended attributes to the Documents build.
Physical acceptance remains: connect to the Mac server, repeatedly adjust SNR,
and verify scrolling and other controls remain responsive during replies.
Mac/Android server rebuilds are not required for this iOS threading fix.

## Summary

This branch now contains an Android Channel Bank BLE GATT server and a native
SwiftUI/CoreBluetooth iOS client. The iOS app can discover the SDR++ Channel
Bank service, connect to the Android phone, decode live state snapshots, and
send basic Channel Bank control commands.

## Android BLE status

The Android side has been live-tested on a Samsung phone with RX888 and Channel
Bank initialized. The installed Release APK validated:

- GATT service startup.
- BLE advertising with the Channel Bank service UUID.
- ATT value capping at 512 bytes.
- 504-byte payload fragments at MTU 517 after the 8-byte Channel Bank frame
  header.
- Reliable State indications.
- Atomic complete-message queueing.
- Coalesced pending State snapshots.
- Successful reassembly of complete live `/api/state` JSON.

The protocol document at `misc_modules/channel_bank/BLE_GATT_PROTOCOL.md`
captures the corrected 512-byte ATT cap and State indication behavior.

## iOS client status

The iOS app lives in `ios/ChannelBankBLEClient/` and includes:

- `ChannelBankCore`: request/response models, framing, client transport,
  recording pagination, and activity-waterfall state model.
- `ChannelBankBLEClientApp`: SwiftUI/CoreBluetooth app UI and BLE central.
- CMake/Xcode wrapper for physical iPhone builds.
- Unit tests for framing, request handling, state decoding, pagination, and
  waterfall behavior.

The app currently:

- Scans by the Channel Bank service UUID, with a broad nearby-BLE fallback for
  diagnostics.
- Connects to the five Channel Bank characteristics.
- Reads the Protocol characteristic.
- Enables Response and State indications.
- Optionally discovers State Summary characteristic
  `7d2f0006-8c4b-4d7a-9a61-8e3c4f2a1000` and enables indications when present.
- Accepts State Summary as raw JSON or as the normal response envelope from
  `GET /api/state/summary`.
- Merges State Summary fields into the local State model without clearing
  fields that are absent from the summary.
- Uses `seq` when present to ignore stale Summary/State updates.
- Receives and reassembles live State indications without doing a startup State
  characteristic read or immediate startup `/api/state` command.
- Waits for the subscribed State Summary stream first when available, otherwise
  the full State stream; if no snapshot arrives after 8 seconds, it sends a
  quiet fallback `/api/state` request and keeps waiting on indications if that
  fallback times out.
- Decodes live State into the Radio, Center, Channel Bank, active-channel,
  waterfall, history, playback, settings, and diagnostics panels.
- Decodes the broader WebUI/BLE State schema, including `sdrppServer`,
  `sourceOffset`, scan counters, RX888 telemetry/toggles, complete Channel Bank
  settings, recordings, and numeric Unix-second `history[].lastSeen` values.
- Sends Start/Stop Channel Bank, Start/Stop Radio, center tune, source, SDR++
  Server, source offset, source-control, Channel Bank settings, playback-lock,
  recording-session, Clear WAVs, and frequency block/unblock commands.
- Applies successful mutating command Response bodies immediately. Full State
  responses replace the local model, while compact Summary-shaped responses are
  merged without clearing omitted detail fields.
- Watches State playback identity, pulls paged `/api/audio/current-playback`
  data, validates page offsets, assembles Base64 WAV/M4A data into a temporary
  file, and plays it locally with AVFoundation.
- Retries brief current-playback `404` races with bounded backoff and falls back
  to `/api/recordings` plus `/api/recordings/download` when the completed
  recording is available there.
- Tolerates dropped/partial State and Response fragments without surfacing them
  as primary UI errors.
- Accepts numeric or legacy string `history[].lastSeen` values and exposes them
  to the UI as Unix seconds.

## Validation So Far

Local iOS validation:

```sh
swift test
```

Current result after pull/download audio implementation: 18 tests passing.

Physical iPhone validation:

- App builds and signs with Apple development team `7WP947RA97`.
- App installs successfully on the connected iPhone.
- Pull/download audio build installs successfully on the connected iPhone.
- iOS discovers/connects to Android SDR++ Channel Bank BLE.
- The native interface populates from live Android State.
- Basic start/stop controls reach Android and change SDR++ behavior.

## Known Follow-Up

- Some UI fields can still feel out of sync during live operation. The basic BLE
  framing, discovery, and State decode gates are now through; remaining work is
  likely field-specific state merge/timing rather than raw transport failure.
- Command response ordering should be watched during rapid user actions,
  especially if pressing Start/Stop pauses the State first/complete cycle.
- Recording listing, recording sessions, guarded Clear WAVs, current-playback
  pull, and recording-download fallback are exposed. A polished recording
  browser with manual tap-to-download/play controls remains follow-up.
- Live Audio characteristic discovery exists and `/api/audio/live.pcm` can be
  requested, but continuous PCM notifications are intentionally not the default
  path. Keep that mode optional because it competes with State and command
  traffic.
- Protocol v1 is unauthenticated and should only be used on trusted nearby
  development devices.
- Best experience follow-up: Android should publish a small State Summary every
  250-500 ms and immediately after user actions, prioritize Response then State
  Summary then full State, and let full State hydrate detail/history less often.

## Useful Retest Markers

After installing both the Android APK and iOS app:

- iOS should discover a peripheral advertising the Channel Bank UUID.
- Connection diagnostics should show Response and State indications enabled.
- On Android builds with State Summary, diagnostics should also show
  `State Summary indications enabled` followed by `RX State Summary complete`.
- The first visible UI state should populate without an immediate
  `Request timed out id=1` startup error.
- If fragment loss occurs, it should be diagnostic-only, not the primary red
  error.
- Tapping Start/Stop Radio or Start/Stop Bank should produce a matching State
  update in the app.
- When Android reports a new playback file in State, iOS should show
  `Monitor: Pulling ...`, then play the assembled temporary WAV/M4A locally.
- If current playback is not ready yet, iOS should retry briefly, then use the
  recordings-list/download fallback if the finished file is listed.
- If a command times out and State indications stop, compare Android logs with
  iOS diagnostics:
  `TX Command first`, `RX Response first`, `RX Response complete`,
  `RX State first afterResponseMs=...`, and `RX State complete`.
