# Channel Bank BLE iOS Client Handoff

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
- Receives and reassembles live State indications without doing a startup State
  characteristic read or immediate startup `/api/state` command.
- Waits for the subscribed State stream first; if no complete State arrives
  after 8 seconds, it sends a quiet fallback `/api/state` request and keeps
  waiting on State indications if that fallback times out.
- Decodes live State into the Radio, Center, Channel Bank, active-channel,
  waterfall, history, playback, settings, and diagnostics panels.
- Decodes the broader WebUI/BLE State schema, including `sdrppServer`,
  `sourceOffset`, scan counters, RX888 telemetry/toggles, complete Channel Bank
  settings, recordings, and numeric Unix-second `history[].lastSeen` values.
- Sends Start/Stop Channel Bank, Start/Stop Radio, center tune, source, SDR++
  Server, source offset, source-control, Channel Bank settings, playback-lock,
  recording-session, Clear WAVs, and frequency block/unblock commands.
- Applies successful mutating command Response bodies immediately because they
  contain the updated full State.
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

## Useful Retest Markers

After installing both the Android APK and iOS app:

- iOS should discover a peripheral advertising the Channel Bank UUID.
- Connection diagnostics should show Response and State indications enabled.
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
