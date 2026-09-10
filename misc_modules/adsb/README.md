# ADS-B with tar1090 (macOS testing)

An SDR++ module owning a dedicated RTL-SDR, a 1090 MHz decoder, and the real
tar1090 interface in an SDR++-owned macOS WebKit window. The decoder and HTTP
server run in-process. WebKit uses macOS's normal web-content processes.
There is no dump1090 executable, Linux service, external web server, or feeder
account to install. Reception is independent of the main SDR++ source.

## Use the test app

The local candidate is installed as `/Applications/SDR++MODULETESTING.app`,
display name **SDR++ ADS-B Test**. It uses
`~/Library/Application Support/sdrpp-adsb-test` and leaves the normal SDR++
profile untouched. This focused test package includes ADS-B, RTL-SDR source,
Radio, and Audio Sink; it is not the all-features integration package.

1. Connect a dedicated RTL-SDR and a suitable 1090 MHz antenna.
2. Open the test app. The ADS-B section and tar1090 window open automatically.
3. Refresh the device list and select the dedicated dongle. It must have a
   nonempty, unique serial number. Missing/duplicate serials are not silently
   replaced with another dongle. A dongle already held by the main receiver
   or another application cannot be opened simultaneously.
4. Set receiver latitude/longitude and enable **Receiver location set** for
   range rings and surface-position decoding. Settings apply on the next Start.
5. Press **Start ADS-B**. Gain is rounded to the nearest supported tuner step.
   **Stop ADS-B** releases the dongle. **Open tar1090** opens/reloads the map.

The map can be closed while reception continues. Disable/unload of the module
or closing SDR++ stops capture, workers, and the server. Automatic map opening
can be disabled using **Open map on module load**. Capture never auto-starts.

## Implementation and current limits

- Pinned dump1090-fa demodulation, CRC, CPR, and tracking sources, compiled
  into the module with hidden C symbols. Strict CRC validation; error
  correction is disabled. Only one module instance is allowed.
- 2.4 MS/s unsigned 8-bit IQ capture, an eight-buffer queue, explicit gap
  handling, and 320-sample overlap for messages spanning USB buffers.
- ICAO address, callsign, barometric altitude, position, ground speed, track,
  barometric vertical rate, squawk, category, signal strength, and timestamps.
  Fields are emitted only while valid. Contacts without a position remain in
  the aircraft table. Live contacts expire after 60 seconds without messages.
- Up to 4096 retained tracks after periodic cleanup; in-memory history snapshots every
  15 seconds, default six hours (1440 snapshots), adjustable from 1–24 hours
  while stopped. A 512 MB cap may shorten retention in busy airspace.
  History resets on Start or app exit; Stop alone preserves it.
  Reopening/reloading tar1090 reconstructs trails from available snapshots.
- Read-only HTTP server on an OS-assigned **127.0.0.1** port. The URL appears
  in the module controls and can also be opened in a local browser. No LAN
  listening, outbound aircraft feed, or device-control HTTP endpoints.
- Web assets and licenses are bundled. The default **Basic offline map** includes
  worldwide country outlines, country labels, and city labels from Natural Earth (2.5 MB bundled).
  It requires no tile cache, download, or internet connection. This is a
  low-detail map, without streets or terrain. Detailed online layers remain
  selectable using tar1090’s layer selector and need internet. Aircraft photos
  are disabled by default to avoid an optional external dependency.
- Aircraft metadata databases, registration/type enrichment, long-term trace
  archives, replay, MLAT, and 978 MHz UAT reception are not implemented.
  tar1090's standard legend includes other data sources even though this
  module receives only 1090 MHz. Optional upstream online features depend
  on their own services. Map preferences currently last for the window's
  nonpersistent WebKit session; receiver settings persist in adsb_config.json.
- macOS only. Other platform implementations are not changed. A future
  Windows/Linux version needs native web-window and socket adaptations.

## Build and software verification

From this checkout, with the usual SDR++ macOS build dependencies installed:

```sh
python3 misc_modules/adsb/tests/build_test_app.py
ctest --test-dir /private/tmp/sdrpp-adsb-build/misc_modules/adsb --output-on-failure
```

The helper configures a focused build outside Documents to avoid File Provider
metadata/copy conflicts, runs the tests, packages and ad-hoc signs a fresh app
under `/private/tmp`, and prints its path. It initializes the dedicated test
profile only when absent. It does not replace an installed app, push, or merge.
For a full integration build, enable `OPT_BUILD_ADSB=ON`; normal module
installation and `make_macos_bundle.sh` include the web assets.

Three test suites cover known frames, CRC rejection, CPR/altitude, stale
contacts, repeat decoder lifecycles, synthetic IQ demodulation across buffer
boundaries and gaps; missing-device startup/shutdown; and HTTP data/assets,
fragmented headers, path/host rejection, and bounded shutdown with a partial
request. The optional `adsb_http_fixture ... --fixture` is a test-only server
with a clearly identified sample aircraft; it is not shipped in the app.

## Validation status

Software tests pass. The installed test bundle passes strict deep signature
verification. SDR++ launches with the ADS-B controls, and the native WebKit
window renders tar1090 with an online map layer. The separate browser fixture
renders and selects the sample aircraft. No RTL-SDR was attached during these
checks: live reception sensitivity, USB unplug/replug, sustained capture, and
simultaneous operation with the main receiver remain for hardware testing.

See [THIRD_PARTY.md](THIRD_PARTY.md) for exact bundled revisions and licenses.
