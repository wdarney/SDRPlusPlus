# Scanner Web UI

Branch: `wd/channel-bank-scanner-webui`, based on `integration/main` at
`b531c598`. This change is confined to desktop Channel Bank, not the native
iPhone controller or the separate iOS Channel Bank implementation.

## Controls

- Multi-Receiver Scan in the Channel Bank mode selector.
- Scanner section: discovery source, ordered transmission pool, editable MHz
  ranges, maximum monitor time (zero is unlimited), and receiver activity.
- Apply Scanner saves ranges and pool together. Changing discovery also selects
  the actual SDR++ source and removes it from the transmission pool.
- Discovery changes require the radio and Channel Bank stopped. Pool/range
  changes require Channel Bank stopped. Existing scan settling, quiet, and
  no-signal sliders remain available.
- Source Settings has a Control receiver selector. It does not change the
  discovery source. Gains, AGC/bias toggles and Airspy gain mode target that
  receiver explicitly, including independent transmission receivers.
- Device, sample rate, ADC and refresh controls require the receiver to be the
  selected SDR. This prevents adapters' global sample-rate side effects from
  disturbing another running SDR. Configure those before starting the scanner.

Gain adapters currently exist for RX888, RTL-SDR, Airspy and Airspy HF+.
Soapy sources can appear in the transmission pool but do not have a generic
web gain adapter; the page reports unavailable controls rather than inventing
hardware settings. Source instance availability and same-driver limits remain
those of the existing multi-receiver implementation.

## API

Full state adds `receiverControls`, keyed by registered receiver name.
`POST /api/source-controls` accepts an optional `receiver` string; omission
preserves the original selected-source behavior. Nonselected receivers accept
only gains, toggles, bias tees, dithering and Airspy mode. Requests are dispatched
through the existing UI action queue and source adapters.

## Validation and Build

Build the `channel_bank` target from this branch against the matching integrated
core. This patch does not add a core ABI change, but the underlying multi-receiver
feature already requires matching core/source modules. Do not install it into a
pre-multi-receiver application by replacing just the plugin.

Local validation: channel_bank compilation, allocator/readiness CTests, and the
mock-backed browser test in `tests/scanner_webui_test.cjs`. The browser test uses
Node with Playwright, optional `CHROME_PATH` for an installed Chromium browser,
and optional `SCREENSHOT_DIR`. It covers addressed gains through delayed replies,
draft preservation, scanner saves, stop guards, and desktop/mobile bounds.

These tests do not establish attached-hardware behavior or native Safari behavior.
Use the separate app build/testing session for discovery switching, independent
gain readback, pool start/stop, and live receiver activity. No installed app,
radio configuration, or remote deployment is modified by this source task.
