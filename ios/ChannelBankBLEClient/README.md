# SDR++ Channel Bank BLE Client

Native SwiftUI/CoreBluetooth client for the Channel Bank BLE GATT server on the
`codex/sdrpp-android-channel-bank-ble` SDR++ branch.

The server contract is `misc_modules/channel_bank/BLE_GATT_PROTOCOL.md` on the
same branch. The iOS app is a BLE central only; it is not another SDR++ host and
it does not expose a GATT server.

## Milestone 1

- Scan by primary service UUID `7d2f0000-8c4b-4d7a-9a61-8e3c4f2a1000`.
- Display discovered peripherals and require explicit user selection.
- Connect, discover all five characteristics, and read the Protocol
  characteristic.
- Enable Response indications and State notifications.
- Optionally enable State Summary indications on
  `7d2f0006-8c4b-4d7a-9a61-8e3c4f2a1000` when Android exposes the summary
  characteristic.
- Use State Summary to populate the immediately visible interface quickly, while
  full State remains the detail/history compatibility path.
- Send `GET /api/state`.
- Decode and display connection status, selected source, radio state, Channel
  Bank state, center frequency, sample rate, RX888 controls, active channels,
  recording/playback state, SNR overview, activity waterfall, history, settings,
  and diagnostics.
- Implement Start/Stop Channel Bank, Start/Stop Radio, center tune, and
  frequency block/unblock from the activity waterfall.
- Display structured server errors.

The app deliberately does not auto-start recordings, delete files, or change
source settings after connection.

## Waterfall

This client reproduces the WebUI activity waterfall from state snapshots. It
does not transport or draw the real SDR++ FFT waterfall. Rows advance only when
a new State notification is received, and the circular buffer retains 72 rows.
State Summary updates can update counters and running status before the detailed
active-channel arrays arrive.

## Security note

Protocol version 1 is unauthenticated. Any nearby BLE central that can discover
the service may control SDR++ Channel Bank or retrieve listed recordings. Use
this build as a development client on trusted radios and nearby devices.

## Destructive operations

`POST /api/recordings/clear-wavs` is intentionally not wired into the first UI
milestone. When added, the app must require explicit confirmation and state that
the operation deletes WAV files, preserves M4A files, and skips active or queued
files.

## Build and tests

The pure protocol/model code is packaged separately as `ChannelBankCore` so it
can be validated without a BLE peripheral:

```sh
swift test
```

Generate an installable Xcode iOS app wrapper with:

```sh
cmake -S XcodeApp -B build-xcode-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
xcodebuild -project build-xcode-ios/ChannelBankBLEClient.xcodeproj -scheme ChannelBankBLEClient -destination generic/platform=iOS CODE_SIGNING_ALLOWED=NO build
```

For a physical iPhone, open
`build-xcode-ios/ChannelBankBLEClient.xcodeproj`, select the
`ChannelBankBLEClient` scheme, choose your signing team, then run it on the
paired device. The app Info template includes `NSBluetoothAlwaysUsageDescription`. Do
not add `bluetooth-central` background mode unless background BLE behavior is
intentionally implemented and tested.
