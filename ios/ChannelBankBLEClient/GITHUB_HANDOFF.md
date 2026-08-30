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
  characteristic read.
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
- Tolerates dropped/partial State and Response fragments without surfacing them
  as primary UI errors.
- Accepts numeric or legacy string `history[].lastSeen` values and exposes them
  to the UI as Unix seconds.

## Validation So Far

Local iOS validation:

```sh
swift test
```

Current result after WebUI parity schema/client expansion: 17 tests passing.

Physical iPhone validation:

- App builds and signs with Apple development team `7WP947RA97`.
- App installs successfully on the connected iPhone.
- iOS discovers/connects to Android SDR++ Channel Bank BLE.
- The native interface populates from live Android State.
- Basic start/stop controls reach Android and change SDR++ behavior.

## Known Follow-Up

- Some UI fields can still feel out of sync during live operation. The basic BLE
  framing, discovery, and State decode gates are now through; remaining work is
  likely field-specific state merge/timing rather than raw transport failure.
- Command response ordering should be watched during rapid user actions,
  especially if pressing Start/Stop pauses the State first/complete cycle.
- Recording listing, recording sessions, and guarded Clear WAVs are exposed.
  Paged recording download/playback still needs a polished iOS workflow.
- Live Audio characteristic discovery exists and `/api/audio/live.pcm` can be
  requested, but the app does not yet subscribe to or play the PCM Audio stream.
- Protocol v1 is unauthenticated and should only be used on trusted nearby
  development devices.

## Useful Retest Markers

After installing both the Android APK and iOS app:

- iOS should discover a peripheral advertising the Channel Bank UUID.
- Connection diagnostics should show Response and State indications enabled.
- The first visible UI state should populate without a persistent
  `Request timed out` error.
- If fragment loss occurs, it should be diagnostic-only, not the primary red
  error.
- Tapping Start/Stop Radio or Start/Stop Bank should produce a matching State
  update in the app.
- If a command times out and State indications stop, compare Android logs with
  iOS diagnostics:
  `TX Command first`, `RX Response first`, `RX Response complete`,
  `RX State first afterResponseMs=...`, and `RX State complete`.
