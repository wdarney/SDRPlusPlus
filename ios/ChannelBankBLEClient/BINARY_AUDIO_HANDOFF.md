# Binary Audio File Transfer

## Build Scope

Branch: `wd/ble-binary-audio`. This extends the existing leased AAC/M4A path
in the macOS Bluetooth adapter and iPhone client. Rebuild both endpoints.
Do not modify `main.cpp`, `encoding.mm`, local Mac recording/playback,
transcription or WebUI. Android implementation/build is out of scope.
The preceding iPhone UI commit is `97cab59b`.

## Protocol

- Optional Audio File characteristic:
  `7d2f0008-8c4b-4d7a-9a61-8e3c4f2a1000`, Notify (Read also advertised).
  Subscribe before requesting a window. This is not live PCM characteristic 0005.
- Create/poll `GET /api/audio/current-playback` with `body.transport:"binary-v1"`,
  `body.encoding:"aac"`, `body.offset:0`. Echo the returned `transferId` on polls.
  Existing 202 preparation responses remain unchanged.
- Ready 200 body contains `transferId`, nonzero uint32 `streamId`, `size`,
  `offset:0`, `name`, and `contentType`, with no Base64 data.
- Request `GET /api/audio/window` with body `transferId`, `offset`, and a fresh
  nonzero uint32 `windowId`. Advancing offset acknowledges the previous window.
- The ordinary Response descriptor contains `transferId`, `streamId`, `windowId`,
  `offset`, `nextOffset`, `size`, `chunkBytes`, and unsigned IEEE `crc32` for the
  complete raw window. Data may arrive before the descriptor is processed.
- Each window contains at most 16 raw notifications. Their 16-byte little-endian
  header is: version u8 (1), flags u8 (0), reserved u16 (0), streamId u32,
  windowId u32, absolute file offset u32. Remaining bytes are raw file content.
  These packets do NOT use the normal eight-byte JSON fragment header.
- Complete values are at most `min(512, maximumUpdateValueLength)` bytes.
  Thus payloads are at most 496 bytes, and windows at most 7,936 bytes.
- The client validates identity, exact offsets/lengths and CRC before appending.
  It retries a failed window at the same offset with a fresh windowId, at most
  three attempts. Late packets from prior windows are ignored. Each descriptor
  request has a four-second timeout; data wait after a descriptor is two seconds.
- Only one binary window per client is queued; replacements discard stale queued
  windows. Queued windows expire after five seconds. CoreBluetooth backpressure
  pauses at the exact unsent packet. Priority is Response, Summary, SNR telemetry,
  binary audio, full State. Already accepted CoreBluetooth packets cannot be recalled.
- EOF retains the lease for final-window retries. After validating the whole file,
  the client releases it using the existing best-effort cancel request. Idle leases
  expire after 60 seconds; disconnect cleanup remains in place.
- If binary collection fails, the client retries through existing Base64 pagination
  at offset zero on the SAME AAC lease. Paged access discards queued binary packets.
  Servers without 0008 use the original paged path automatically.

## Validation And Device Check

The 38-test Swift suite passes, including loss of first/middle/last packets,
out-of-order/duplicate packets, corrupt data, stale IDs, bounded retries and
cancellation. Focused macOS tests pass for subscription/ownership, MTUs 20/185/512,
backpressure, command priority, window replacement and EOF release. Existing AAC,
framing and paged lease tests also pass. These are synthetic, not live BLE proof.

The signed Debug iPhone build and strict signature verification passed, and the
app was installed on the paired iPhone. No full Mac app or Android build was made
in this task. The Mac build task should integrate this branch's binary transfer
commit into its current integration tree, preserve its other features, and produce
a separate test bundle. Do not replace a known-good installed Mac app.

On the rebuilt Mac server and iPhone, look for `Binary Audio File notifications
enabled`, then `Binary audio start` and `Binary audio complete`. Completion reports
bytes, seconds, KB/s and retry count; preparation time is reported separately.
Compare with the reported roughly 5 KB/s paged behavior. Verify audible playback,
responsive controls/SNR, disconnect cancellation, and an older server fallback.
No measured speed improvement is claimed until that physical-device comparison.
