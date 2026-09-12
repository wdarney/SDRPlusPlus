# macOS iPhone Control

The macOS Channel Bank module exposes the version 1 GATT service used by the
native ChannelBankBLEClient iPhone app on the Android BLE branch. Enable
**Bluetooth iPhone control** in SDR++'s Channel Bank panel, grant Bluetooth
permission, then scan from the iPhone app for **SDR++ Channel Bank**.

This is native app control; the HTML page itself is not transported over BLE.
Bluetooth defaults off and the preference is saved per module instance.
Nearby clients can control the radio when it is enabled; this initial protocol
does not require pairing or application authentication.

## Build and Test Handoff

Build branch `wd/channel-bank-macos-bluetooth`. It includes the proxy-control
fixes from `59c7399d`; those fixes are not yet on `integration/main`.

Build the `channel_bank` and `sdrpp` targets using a matching build directory,
then package the application with the repository's macOS bundling script.
The bundle needs `NSBluetoothAlwaysUsageDescription` and
`NSBluetoothPeripheralUsageDescription`; the bundle helper now emits both.
Copying only the plugin into an older app will not supply these keys. A missing
permission description is displayed in the module panel without initializing
CoreBluetooth.
The helper also includes local-network permission text and the
`NSAllowsLocalNetworking` ATS setting for the adapter's local HTTP connection.

Device testing and deployment are left to the separate testing session.
Check discovery, permission deny/allow, connection and reconnection, module
unload, Bluetooth off/on, start/stop, tuning, gain changes, SNR, blocking,
and playback locking. Check simultaneous web and iPhone use as well.

## Scope

Supported requests include radio and Channel Bank start/stop, source selection,
source controls and offsets, SDR++ Server target/connect/disconnect, tuning,
Channel Bank settings, frequency blocking, playback locking, recording lists,
session creation, and WAV cleanup. Existing server validation applies.

Bluetooth audio and paged recording downloads are not implemented. The Audio
characteristic remains discoverable because the current iPhone client requires
it during connection, but capabilities report `audio.available:false`.
Audio/download requests return an explicit error; no PCM is published.

Web Control starts automatically when Bluetooth is enabled and must remain
enabled while Bluetooth is in use. The adapter sends only allowlisted requests
to the module's configured local HTTP listener. Wildcard binding connects
through loopback; specific bindings use that configured address. The iPhone
does not need IP connectivity to the Mac. Disabling Bluetooth leaves Web
Control running.

## Transport and Lifecycle

The service UUID is `7d2f0000-8c4b-4d7a-9a61-8e3c4f2a1000`.
Characteristics 0001 through 0006 match Protocol, Command, Response, State,
Audio, and State Summary. Framing is version/flags/u16le ID/u32le offset,
followed by UTF-8 JSON. Commands use acknowledged writes; responses and state
use indications. Outbound fragments respect the central's maximum update
length, capped at 512 bytes, and resume on CoreBluetooth readiness.

Limits are four subscribed clients, one 64 KiB command assembly per client,
16 queued commands, and 32 outgoing messages (each at most 256 KiB).
Incomplete assemblies expire after ten seconds. Snapshots coalesce while an
earlier message is queued. The joined worker polls compact status about every
500 ms and full state about every five seconds, independent of panel visibility;
slow HTTP requests extend these intervals. Local HTTP requests time out after
seven seconds. Disabling/unloading may wait for an in-flight request to finish.

CoreBluetooth delegate work stays on a dedicated serial queue. HTTP requests
run on a separate joined worker and use the WebUI's existing SDR++ UI dispatch.
No radio/source logic is duplicated and no core ABI changes are required.

## Radio-Free Verification

From the repository root (with `build-proxy-test` already configured):

```sh
clang++ -std=c++17 -fobjc-arc -framework Foundation -framework CoreBluetooth misc_modules/channel_bank/tests/bluetooth_macos_test.mm -o build-proxy-test/bluetooth_macos_test
build-proxy-test/bluetooth_macos_test
```

The tests use delegate doubles without initializing a Bluetooth manager.
They cover command reassembly, invalid offsets, payload bounds, outgoing
backpressure/fragmentation, route rejection, and stable long-read snapshots.
They do not verify discovery or delivery on physical hardware.
