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

Build the `channel_bank` and `sdrpp` targets from the checkout that contains
this module integration, then package the application with the repository's
macOS bundling script.
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

Completed-transmission audio uses the Android-compatible leased playback route
described below. The existing iPhone client's paged pull and local playback code
can use it without changes. Live PCM notifications and arbitrary recording-library
downloads remain unsupported. The Audio characteristic remains discoverable for
client compatibility; `audio.available:false` refers to live PCM, while
`playbackTransfer.available:true` advertises the completed-file path.

## Buffered Audio

### Bluetooth-Only AAC Copy

Updated iPhone clients request `body.encoding: "aac"` on the first
`GET /api/audio/current-playback` page. The Bluetooth adapter converts its leased
source descriptor to a private M4A containing 24 kHz mono AAC at 32 kbps.
Conversion runs on a separate thread, so command handling and telemetry can
continue. The temporary output is unlinked after opening it for paged reading.
An already-M4A source is passed through. Requests without `encoding` retain
the original file format for older clients.

While conversion runs, Response returns status 202 with `ok:true` and a body
such as:

```json
{"transferId":"...","preparing":true,"retryAfterMs":250,"offset":0,"name":"voice.m4a","contentType":"audio/mp4"}
```

The client retains that lease and repeats offset zero until a normal 200 data
page arrives. No new lease is created for a preparation poll. The output size
and offsets then describe the compressed file. Cancel, idle expiry, disconnect,
and shutdown cancel conversion and release its private descriptors. Conversion
checks cancellation and a 15-second deadline between PCM blocks; failures return
502 instead of silently transferring a large WAV. iOS bounds preparation waits
and displays preparation/download progress.

This feature changes only `bluetooth_macos.mm` and its private
`bluetooth_audio_macos.h` helper. Local `main.cpp`, `encoding.mm`, playback,
recordings, transcription, and WebUI behavior are unchanged. Compression works
with recording-disabled temporary WAVs and does not wait for the local M4A
recording encoder.

Radio-free validation used a ten-second 48 kHz mono WAV: 960,044 bytes became
51,434 M4A bytes, with 240,000 decoded frames at 24 kHz and nonzero audio energy.
The test verifies original source bytes, conversion after source unlink,
multi-page download, EOF cleanup, invalid-input failure, and cancellation.
Build the test with `-framework AudioToolbox` in addition to Foundation and
CoreBluetooth. A rebuilt Mac app and updated iPhone client are needed for a
physical BLE playback test; no new Mac package is produced by this source change.

Send `GET /api/audio/current-playback` in a BLE command with `body.offset` (default
0) and `body.limit` (default 4096, clamped to 1..16384). The first page leases the
current completed playback file and returns `transferId`, `offset`, `nextOffset`,
`size`, `eof`, `name`, `contentType`, and `dataBase64`. Echo the same `transferId`
on subsequent pages. This matches the Android client's `collectLeased` contract.
Send `body: {"transferId":"...", "cancel":true}` to cancel.

The lease holds an open read-only file descriptor, not a whole-file RAM copy.
On macOS the descriptor remains readable after normal playback cleanup unlinks
the recording. It closes at EOF, cancellation, disconnect, module shutdown, or
60 seconds of idle time. Transfers are scoped to the originating Bluetooth
central, limited to eight total and 64 MiB per file. Missing playback or expired
leases return 404; missing IDs after offset zero return 400; past-EOF offsets
return 416. Oversized recordings return 413; exhausted lease capacity returns 503.

The existing playback queue and frequency lock select the file. Recording-disabled
temporary WAVs work too. This does not change the web audio path or broadcast live
VFO audio. Command pages no longer wait a fixed 500 ms between requests; state
snapshots retain their own cadence. Responses precede unsent full-state snapshots.

Device testing should include recording disabled, changing playback while a file
is transferring, frequency lock, Monitor stop/restart, reconnect, and background
playback. BLE throughput and the iPhone client's behavior when a newer transmission
appears during a download still need physical testing; server-side leases alone
do not guarantee gapless playback or delivery of every transmission.

Web Control starts automatically when Bluetooth is enabled and must remain
enabled while Bluetooth is in use. The adapter sends only allowlisted requests
to the module's configured local HTTP listener. Wildcard binding connects
through loopback; specific bindings use that configured address. The iPhone
does not need IP connectivity to the Mac. Disabling Bluetooth leaves Web
Control running.

## Transport and Lifecycle

The service UUID is `7d2f0000-8c4b-4d7a-9a61-8e3c4f2a1000`.
Characteristics 0001 through 0006 match Protocol, Command, Response, State,
Audio, and State Summary. Characteristic 0007 is optional low-latency SNR
telemetry. Its notification payload is little-endian binary: schema `u8` (1),
sequence `u32`, first frequency `f64`, spacing `f64`, point count `u16`, then
up to 160 `(snrTenthsDb:i16, flags:u8)` points. Flag bits are detected (0), raw
detected (1), and blocked (2). It is produced at 4 Hz only for fixed-grid auto
or scan operation while a client is subscribed; manual and bookmark scans use
the reliable full State snapshot instead.

All notifications and indications use the common version/flags/u16le ID/u32le
offset framing before their payload. Commands use acknowledged writes; Response,
State, and Summary use indications. SNR telemetry uses lossy notifications:
an in-flight frame is completed, while an unsent stale sample is replaced with
the newest one. Response indications have first priority, State Summary
indications next, telemetry notifications third, and full-State indications
last. A compact frame may pause a full-State transfer between fragments; clients
resume it using independent assemblers for each characteristic. Outbound
fragments respect the central's maximum update length, capped at 512 bytes, and
resume on CoreBluetooth readiness.

Limits are four subscribed clients, one 64 KiB command assembly per client,
16 queued commands, and 32 outgoing messages (each at most 256 KiB).
Incomplete assemblies expire after ten seconds. Snapshots coalesce while an
earlier message is queued. The joined worker polls compact status about every
500 ms and full state about every ten seconds, independent of panel visibility;
slow HTTP requests extend these intervals. A separate lightweight worker obtains
the compact SNR payload every 250 ms, so local HTTP polling does not stall the
overview. Local HTTP requests time out after seven seconds. Disabling/unloading
may wait for an in-flight request to finish.

CoreBluetooth delegate work stays on a dedicated serial queue. HTTP requests
run on a separate joined worker and use the WebUI's existing SDR++ UI dispatch.
Audio pages use a narrow module callback to open the selected playback file;
page reads and lease cleanup run on that same worker.
No radio/source logic is duplicated and no core ABI changes are required.

## Radio-Free Verification

From the repository root (with `build-proxy-test` already configured):

```sh
clang++ -std=c++17 -fobjc-arc -framework Foundation -framework CoreBluetooth misc_modules/channel_bank/tests/bluetooth_macos_test.mm -o build-proxy-test/bluetooth_macos_test
build-proxy-test/bluetooth_macos_test
```

The tests use delegate doubles without initializing a Bluetooth manager.
They cover command reassembly, invalid offsets, payload bounds, outgoing
backpressure/fragmentation, route rejection, stable long-read snapshots, audio
page bounds, owner isolation, byte-exact paging after unlink, and descriptor
release at EOF/cancellation/expiry.
They do not verify discovery or delivery on physical hardware.
