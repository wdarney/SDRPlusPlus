# Android State Stream Handoff

## Observed iOS behavior

- Compact State Summary control fields arrive promptly.
- Channel Bank controls are responsive after moving automatic refreshes off the full State response lane.
- SNR Overview and Activity Span can take tens of seconds to about a minute to update, despite a healthy BLE connection.

## Confirmed cause

`ChannelBankGattServer.kt` has three outgoing indication queues:

1. Response
2. State Summary
3. State and Audio stream

`sendNext()` currently chooses State Summary before the State stream after every acknowledged indication. The Android publisher emits a Summary every 500 ms. A complete State envelope is roughly 20 KB, or about 40 fragments at MTU 517. Therefore a Summary can interrupt every State fragment, stretching a full snapshot far beyond its intended five-second cadence.

The iOS client now accepts a delayed complete State and merges its telemetry fields without rolling back newer Summary control fields. It is not discarding the chart data once the complete State actually arrives.

## Required queue behavior

Use this priority at each indication boundary:

1. Response fragments
2. Remaining fragments of an already-started full State message
3. State Summary fragments
4. New full State or Audio stream fragments

While a full State message is in progress:

- Keep sending its remaining State fragments after each successful `onNotificationSent` callback.
- Permit a Response to interrupt at a fragment boundary.
- Do not let periodic State Summaries interrupt it.
- Coalesce periodic Summaries to the newest complete Summary; send that newest one after the State completes.
- Keep the existing rule that only one complete State snapshot may be queued per client.

Use a `stateMessageInProgress` flag analogous to the existing summary-progress tracking. Set it when sending a State `FIRST` fragment and clear it after its `LAST` fragment or a delivery failure. The flag must be per active message/device if multi-client operation is intended.

## Do not change

- Keep State Summary cadence at 500 ms and immediate Summary publication after successful control mutations.
- Keep full State cadence at five seconds.
- Keep reliable indications and one in-flight indication.
- Keep Response above all other traffic.
- Do not put the full `snrOverview` array into State Summary; that would recreate the large-message contention under a different characteristic.

## Smooth SNR telemetry characteristic

For smooth SNR Overview rendering, add an optional characteristic:

- UUID: `7d2f0007-8c4b-4d7a-9a61-8e3c4f2a1000`
- Properties: Read, Notify. Notifications are intentionally lossy; the client renders only the newest complete frame.
- Cadence: begin at 4 Hz while subscribed. Drop a frame rather than queue it behind a previous telemetry frame, Response, Summary, or State traffic.
- Priority: below Response, State Summary, and reliable full State. It must never delay controls.
- Framing: use the existing eight-byte GATT frame header and a monotonically increasing message ID/sequence.

The reassembled binary payload is little-endian:

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | `u8` | Telemetry schema version, `1` |
| 1 | `u32` | Telemetry sequence |
| 5 | `f64` | Frequency of point 0 in Hz |
| 13 | `f64` | Spacing between points in Hz |
| 21 | `u16` | Point count |
| 23 | repeated `i16`,`u8` | SNR in tenths of dB and flags |

Flags: bit 0 `detected`, bit 1 `rawDetected`, bit 2 `blocked`. In automatic fixed-grid modes this represents the complete `snrOverview` without repeating each frequency as JSON. 160 points occupy 503 bytes before outer GATT framing. For manual/nonuniform frequencies, do not publish this version; retain the reliable full State chart until a versioned nonuniform encoding is added.

The iOS client already discovers, subscribes to, reassembles, validates, and renders this optional characteristic. Older Android builds remain compatible because the characteristic is optional.

## Acceptance test

1. Connect iOS and enable Response, State Summary, and State indications.
2. Confirm State Summary continues at roughly 500 ms.
3. Confirm a 20 KB full State completes in a few seconds, not a minute.
4. While that full State is draining, press a Channel Bank control. Its Response must arrive promptly, and the control UI must update immediately.
5. Confirm SNR Overview and Activity Span refresh after each completed full State.
6. Confirm no partial-frame errors or assembler resets occur when Response interrupts State.

## Relevant files

- `android/app/src/main/java/ChannelBankGattServer.kt`
- `misc_modules/channel_bank/src/main.cpp` (`bleStatePublisherFunc`, 500 ms Summary / five-second full State cadence)
- `ios/ChannelBankBLEClient/Sources/ChannelBankBLEClientApp/BLECentralManager.swift` (delayed full-State telemetry merge)
