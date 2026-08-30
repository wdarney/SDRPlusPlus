# SDR++ Channel Bank Bluetooth LE GATT protocol

Status: version 1, Android-first implementation. The protocol is transport-neutral at the Channel Bank boundary so CoreBluetooth on macOS and WinRT on Windows can expose the same service and envelopes later.

## Scope and source baseline

This contract mirrors the complete Channel Bank WebUI in the authoritative Android baseline `SDRpp-Android` at `0f9858d6`, plus the focused BLE integration commit built on that baseline. It does not add general SDR++ module control. The baseline includes the RX888 Android source, recording sessions, protected WAV cleanup, current-playback audio, telemetry-speed control, and the optimized RX888 FFTW packaging path.

## Service and characteristics

Primary service: `7d2f0000-8c4b-4d7a-9a61-8e3c4f2a1000`

| Characteristic | UUID | Properties | Contract |
|---|---|---|---|
| Protocol | `7d2f0001-8c4b-4d7a-9a61-8e3c4f2a1000` | Read | Raw UTF-8 JSON capability document; long reads use ATT offsets. |
| Command | `7d2f0002-8c4b-4d7a-9a61-8e3c4f2a1000` | Write, Write Without Response | Framed UTF-8 JSON request. Maximum reassembled request is 1 MiB. |
| Response | `7d2f0003-8c4b-4d7a-9a61-8e3c4f2a1000` | Read, Indicate | Framed response indications. A raw UTF-8 copy of the most recent response can be recovered with a long read. Enable indications before sending commands. |
| State | `7d2f0004-8c4b-4d7a-9a61-8e3c4f2a1000` | Read, Notify | A GET-state response envelope. Notifications are emitted at most every 500 ms while subscribed. Long read returns a fresh raw envelope. |
| Audio | `7d2f0005-8c4b-4d7a-9a61-8e3c4f2a1000` | Read, Notify | Framed PCM audio notifications: signed 16-bit little-endian, mono, 48 kHz. |

The standard Client Characteristic Configuration descriptor UUID `00002902-0000-1000-8000-00805f9b34fb` controls indications/notifications.

## Framing

Command writes and Response, State, and Audio notifications use the same 8-byte little-endian header:

| Offset | Type | Meaning |
|---:|---|---|
| 0 | `u8` | Protocol version, currently `1`. |
| 1 | `u8` | Flags: bit 0 `FIRST`, bit 1 `LAST`; both set for a one-frame message. |
| 2 | `u16le` | Message ID. The response echoes the request ID. State uses `0`. Audio uses a wrapping sequence number. |
| 4 | `u32le` | Byte offset of this payload in the reassembled message. |
| 8 | bytes | Payload, up to negotiated ATT MTU minus 11 bytes. |

Clients should request the largest MTU their platform supports, assemble by `(characteristic, messageId)`, require contiguous offsets, discard incomplete messages on disconnect or a new `FIRST`, and reject unknown versions. Prepared writes are not used; large commands are split into application frames. Response indications are reliable. State and Audio notifications are intentionally lossy and may be dropped under backpressure.

## Request and response envelopes

Request:

```json
{
  "v": 1,
  "id": 42,
  "method": "POST",
  "path": "/api/center",
  "query": {},
  "body": { "hz": 157067450 }
}
```

`query` and `body` may be omitted. `id` is a client-selected signed 64-bit integer; use a monotonically increasing positive value.

Success:

```json
{ "v": 1, "id": 42, "ok": true, "status": 200, "body": {} }
```

Failure:

```json
{
  "v": 1,
  "id": 42,
  "ok": false,
  "status": 409,
  "error": { "code": "request_failed", "message": "stop SDR before changing source" }
}
```

Status values preserve WebUI HTTP semantics: `200` success, `400` invalid payload/value, `404` unavailable source/file/route, `409` state conflict, `416` recording offset past EOF, `502` remote SDR++ Server connection failure, and `503` Channel Bank/UI unavailable or timed out. A client must use `ok`, not merely receipt of a BLE write response, to decide whether an action succeeded.

## WebUI-to-BLE mapping

The BLE request `method` and `path` are identical to the WebUI route. A successful mutating route returns the full state snapshot in `body`.

| WebUI surface | BLE request or characteristic | Request data / behavior |
|---|---|---|
| HTML `/` and `/index.html` | Protocol characteristic | No HTML is transported. A client renders its own UI from this contract and State. |
| `OPTIONS` / CORS | Not applicable | BLE has no browser CORS preflight. |
| `GET /api/state`, `GET /state` | State read/notify, or Command GET | Complete state envelope. |
| `GET /api/sources` | Command GET | Returns `selected` and `sources[]`. |
| `GET /api/sdrpp-server` | Command GET | Returns server-source availability/target/connection state. |
| `GET /api/source-controls` | Command GET | Returns RX888 controls when RX888 is selected; otherwise `available:false`. |
| `GET /api/source-offset` | Command GET | Returns selected/effective/manual offsets and mode names. |
| `GET /api/channel-bank/settings` | Command GET | Returns all WebUI-editable Channel Bank settings. |
| `GET /api/recordings` | Command GET | Returns up to 300 newest WAV/M4A files. |
| `GET /api/recordings/download?file=...` | Command GET with `query.file`, `body.offset`, `body.limit` | Paged download. `limit` is clamped to 1..16384 bytes, default 4096. Response has `dataBase64`, `offset`, `nextOffset`, `size`, `eof`, `name`, and `contentType`. Repeat until `eof:true`. Absolute/out-of-root paths are rejected. |
| `GET /api/audio/current-playback` | Command GET with `body.offset`, `body.limit` | Paged copy of the exact file currently being played. The response has the same paging fields as a recording download, without exposing an absolute path. Returns `404` when nothing is playing and `416` past EOF. |
| `GET /api/audio/live.pcm` | Command GET plus Audio CCCD | Response describes the Audio characteristic. Enable Audio notifications for PCM. |
| `GET /api/audio/live.wav` | Command GET plus Audio CCCD | Same PCM transport. A client wanting WAV constructs the standard 44-byte PCM WAV header locally. |
| `POST /api/recordings/session` | Command POST | `{ "name": string }`. Sanitizes the name, creates/selects the session subfolder, and returns the recordings listing including `activeSession`. |
| `POST /api/recordings/clear-wavs` | Command POST | Empty body. Recursively deletes WAV files only, preserves M4A, and skips active recordings, current playback, and queued playback files. Returns `deleted` and `skipped`. This is destructive and clients should require explicit user confirmation. |
| `POST /api/start` | Command POST | Empty body. Fails `409` if recording path is invalid. |
| `POST /api/stop` | Command POST | Empty body. Stops Channel Bank, not the SDR source. |
| `POST /api/play` | Command POST | Empty body. Starts the SDR source and tunes current waterfall center. |
| `POST /api/stop-radio`, `POST /api/radio/stop` | Command POST | Empty body. Stops the SDR source. |
| `POST /api/center` | Command POST | `{ "hz": positiveNumber }`. Sets waterfall/source center. |
| `POST /api/source` | Command POST | `{ "name": string }`; SDR must be stopped and name must exist. |
| `POST /api/sdrpp-server` | Command POST | `{ "host": string, "port": 1..65535 }`; SDR must be stopped. |
| `POST /api/sdrpp-server/connect` | Command POST | Empty body; SDR must be stopped. |
| `POST /api/sdrpp-server/disconnect` | Command POST | Empty body; SDR must be stopped. |
| `POST /api/source-offset` | Command POST | `{ "selected": "None"|"Manual"|customName, "manualOffsetHz"?: number }`; manual value is limited to +/-1 GHz. |
| `POST /api/source-controls` | Command POST | RX888 partial update described below. Device/rate/ADC/mode/R2IQ-worker/refresh require stopped SDR; telemetry speed, gains, bias tees, and dithering are live. |
| `POST /api/channel-bank/settings` | Command POST | Partial settings update described below. Mode/spacing/demod require stopped Channel Bank. |
| `POST /api/frequency/block` | Command POST | `{ "hz": positiveNumber, "blocked": boolean }`. |
| `POST /api/playback-lock` | Command POST | `{ "hz": number }`; `hz <= 0` clears the lock. |

## Channel Bank settings payload

All fields are optional in a partial update.

| Field | Type / values | Server normalization |
|---|---|---|
| `mode` | `auto`, `manual`, `scan`, `bookmark_scan` | Structural; Channel Bank must be stopped. |
| `spacingId` | integer 0..5 | Maps to 8,333; 12,500; 25,000; 50,000; 100,000; 200,000 Hz. Structural. |
| `demodMode` | `AM`, `NFM`, `WFM`, `USB`, `LSB` | Structural. |
| `snrThresholdDb` | number | Clamped 1..30 dB. |
| `maxChannels` | integer | Clamped 1..64. |
| `bwUsage` | number | Clamped 0.5..1.0. |
| `recordingEnabled` | boolean | Live. |
| `minTransmissionMs` | integer | Clamped 0..10000. |
| `signalHoldMs` | integer | Clamped 0..5000. |
| `tailMs` | integer | Clamped 100..2000. |
| `scanQuietSec` | number | Clamped 1..30. |
| `scanNoSignalSec` | number | Clamped 0.1..5.0. |
| `transcriptionBackend` | integer 0..4 | 0 Off, 1 Apple Speech, 2 Whisper ATC Large, 3 Whisper ATC Medium, 4 Whisper Turbo. Android coerces Apple Speech to Off; this branch excludes Android Whisper, so clients should normally use 0 on Android. |

The settings object also returns `channelSpacingHz` and `transcriptionBackendName` as read-only derived fields.

## RX888 source-controls payload and state

Partial write keys are `refresh:true`, `deviceLabel:string` or `deviceId:int`, `mode:"HF"|"VHF"`, `adcClockMHz:number`, `sampleRate:number` or `sampleRateId:int`, `r2iqWorkers:1..4`, `telemetryIntervalSec:1|5`, `gains:{stageName:number}`, `biasTeeHF:boolean`, `biasTeeVHF:boolean`, and `dithering:boolean`. Device/rate/ADC/mode/R2IQ-worker/refresh changes require a stopped source; telemetry speed, gains, bias tees, and dithering are live-mutable.

Returned state fields are:

- `available`, `source`, `running`, `cleanupBusy`
- `deviceId`; `devices[]` entries `{id,label}`
- `sampleRate`; `sampleRates[]` entries `{id,value,label,selected}`
- `supportsAdcFreq`, `adcClockMHz`, `adcMinMHz`, `adcMaxMHz`
- `mode`; `modes[]`
- `gains[]` entries `{name,label,value,min,max,step,available,liveMutable}`
- `supportsNewBiasTee`, `supportsBiasTee`, `biasTeeHF`, `biasTeeVHF`, `biasTeeLiveMutable`
- `supportsDithering`, `dithering`, `ditheringLiveMutable`
- `telemetryIntervalSec`; `telemetrySpeeds[]` entries `{label,intervalSec,selected}`; `telemetryLiveMutable`

## State schema

The `body` of a state response contains:

- Module/radio: `module`, `enabled`, `running`, `mode`, `demodMode`, `centerHz`, `waterfallCenterHz`, `sampleRate`, `usableSpanHz`, `bwUsage`, `radioPlaying`, `sdrppHeartbeat`, `serverTimeMs`.
- Sources: `selectedSource`, `sources[]`, `sdrppServer`, `sourceControls`, `sourceOffset`.
- Settings: `settings`, plus convenience copies `snrThresholdDb`, `maxChannels`, `recordingEnabled`.
- `activeChannels[]`: `{slot,freqHz,gridFreqHz,name,blocked,recording,signalPresent,rawSignalPresent,file}`.
- `recentChannels[]`: `{freqHz,name,blocked,ageMs}`.
- Detection: `detectedSlots`, `manualDetected`, `snrOverview[]`, whose entries are `{freqHz,snrDb,detected,rawDetected,blocked}`.
- Playback: `playbackQueued`; `playback` has `{active,freqKey,positionMs,queued}` plus `{freqHz,name,file,fileName}` while active (Android position is `-1`); `playbackLock` is `{active:false}` or `{active:true,freqHz,name}`; `currentlyPlayingFreqKey`.
- `history[]`: up to 160 entries `{freqHz,name,count,blocked,lastSeen,description}`.
- `diagnostics`: `{rssBytes,cpuPercent,activeChannels,maxChannels,playbackQueued,webClientThreads,transcriptionJobs,pendingEncodes,liveAudioClients,liveAudioQueuedSamples}`. `rssBytes` and transcription/encode counts are zero on Android in this branch; the first CPU sample is `-1`.
- Scan-only conditional fields: `scanStopIndex`, `scanStopCount`, `bookmarkScanStopIndex`, `bookmarkScanStopCount`.
- `lastTranscriptName` and `lastTranscriptText` exist only on Apple/Windows builds, not Android.

Clients must ignore unknown fields and tolerate conditional/missing fields for forward compatibility.

## Audio behavior and limits

Audio messages contain one contiguous block of raw PCM after frame reassembly. The existing Channel Bank selection rule is retained: one current channel is chosen, with a 500 ms hold before another channel can take over. Audio is best effort. 48 kHz mono PCM is roughly 768 kbit/s before BLE overhead and will not be sustainable on every Android radio, connection interval, PHY, or negotiated MTU. Clients should request a large MTU and 2M PHY when available, tolerate dropped message sequence numbers, and treat this as an experimental parity path. Control, state, and recording download do not depend on continuous audio success.

## Android permissions and lifecycle

- Minimum SDK remains 28. Android 11 and earlier use manifest `BLUETOOTH` and `BLUETOOTH_ADMIN` permissions.
- Android 12+ declares and requests runtime `android.permission.BLUETOOTH_ADVERTISE` and `android.permission.BLUETOOTH_CONNECT`.
- BLE hardware is optional in the manifest; lack of adapter, disabled Bluetooth, lack of advertiser support, or denied permission leaves the service unavailable without breaking SDR++.
- The server is requested when the Channel Bank module finishes initialization, advertises the service UUID while the activity/module is alive, and closes advertising/GATT on module unload or activity destruction. Permission grant retries startup.
- Version 1 deliberately matches the unauthenticated WebUI control model and does not require pairing or characteristic encryption. Any nearby BLE central may control SDR++ or download listed recordings. A production deployment should add an explicit opt-in and authentication policy without changing the v1 payload envelopes.

## Implementation ownership and portability

- `misc_modules/channel_bank/src/main.cpp`: authoritative request dispatcher, validation, state generation, recording paging, state cadence, and PCM publication.
- `core/src/android_ble_gatt.h` and `core/backends/android/ble_gatt_bridge.cpp`: narrow native registration/JNI bridge. No Channel Bank business rules live here.
- `android/app/src/main/java/ChannelBankGattServer.kt`: Android GATT database, advertising, frame assembly, per-device reads, notification backpressure, and subscriptions.
- `android/app/src/main/java/MainActivity.kt`: Android permission and activity lifecycle glue.
- `android/app/src/main/AndroidManifest.xml`: BLE declarations.
- `android/app/build.gradle`: preserves the `SDRpp-Android` RX888/Channel Bank Release configuration, optimized ARM64 FFTW verification, and disabled incompatible Android VDL2/sqlite target.

For macOS or Windows, implement the same five characteristics and byte framing with CoreBluetooth or WinRT, register the same Channel Bank request handler, and leave endpoint logic and schemas unchanged.
