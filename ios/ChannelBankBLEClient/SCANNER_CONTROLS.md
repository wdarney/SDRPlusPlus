# Native scanner controls

Targets the server contract in `wd/channel-bank-scanner-webui`, commit
`21fa3d71` (based on `integration/main` at `b531c598`). That server branch must
be built with its matching core/source modules. The iPhone change does not
merge the server branch or add multi-receiver support to older Android servers.

The Multi-Receiver Scanner disclosure appears in both interfaces when full
State contains `multiReceiverScan`. It edits discovery SDR, an ordered
transmission pool, ranges displayed in MHz, maximum monitor seconds (0 means
unlimited), and scan settling (250–2000 ms). Apply Scanner selects
`multi_receiver_scan` using `/api/channel-bank/settings`. Saving does not
start receivers or change recording, demodulation, or blocked frequencies.
The existing Play control starts the saved scanner configuration.

Edits stay in a local draft until applied; telemetry never overwrites them.
Reload saved settings discards the draft. Empty range arrays clear ranges.
Before saving, the client refreshes Summary/settings, validates the draft,
then sends the complete configuration. The server remains responsible for
hardware availability, generated stop count, and authoritative stop guards.
Discovery changes require radio and Channel Bank stopped; pool/range edits
require Channel Bank stopped. Discovery is omitted when it is unchanged, so
pool edits remain possible while only the radio is running.

Source Controls provides a separate Control receiver picker. Gain, toggle,
bias, dithering and Airspy mode requests include `receiver` captured at the
time of the action. Choosing a control receiver never selects the SDR++
discovery source. Device/rate/ADC/refresh remain disabled for nonselected
receivers. Unsupported adapters show unavailable controls. Live mutation
capabilities from the adapter are respected.

Receiver/channel activity displays discovery state/frequency, pool receiver
availability, tuning, sample rate, held/recording/releasing channels,
dispatch assignments and failure/no-capacity counts. It updates with full
State; compact Summary does not carry these details.

## BLE contract verification

At server `21fa3d71`, `src/bluetooth_macos.mm` permits both settings and
source-control routes, serializes the complete POST body, and returns the
full JSON response without filtering keys. Only `/api/state/summary` applies
a field whitelist. `src/main.cpp` includes both `receiverControls` and
`multiReceiverScan` in `webStateSnapshot()`. No new characteristic is needed.
The client decodes, encodes and merges these fields, including delayed full
snapshots overtaken by Summary. Fragmented-command tests exercise all scanner
fields and receiver-addressed gains through the existing command transport.

## RX888 Refresh

The prior native view nested Refresh under `sourceControls.available == true`
and placed it beside device/rate/mode menus in one horizontal row. It was also
inside the collapsed Source Controls section in Simple mode. Refresh is now
on its own row outside the availability gate, identified by the selected
receiver name, so recovery remains visible without an available device. It
still requires the selected RX888 and stopped radio/Channel Bank. This is a
source-level diagnosis; the installed phone binary and physical refresh
behavior have not been inspected or tested by this change.

## Validation

- 51 Swift tests passed, including fragmented scanner/targeted-gain commands,
  State round-trip and delayed-State merges, ordered pools, invalid ranges,
  stop guards, and existing audio-transfer tests.
- Release iPhone build passed with signing disabled. This is compilation
  evidence, not an installable signed release or hardware validation.
- The paired iPhone was reported unavailable by `devicectl`; installed-app
  inspection and on-device validation could not be performed.

Bluetooth audio encoding, lease/download logic and playback are unchanged.
Validate source selection, independent gains, pool allocation and Bluetooth
audio against physical hardware after installing a matching server/client.
