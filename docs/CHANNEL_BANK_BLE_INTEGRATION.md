# Channel Bank BLE Integration Map

## Purpose

This document is the source-of-truth handoff for promoting Channel Bank BLE
work into the shared integration branch without duplicating historical patches.
It covers macOS peripheral support, the native iPhone client, and Android BLE
transport. Windows remains out of scope.

## Current State

### Consolidation Approved 2026-09-14

The user reported the combined system looking good and authorized merging.
The integration candidate combines these exact source trees:

- Shared baseline: `c07b15c6` on `integration/main`.
- Tested Mac build history: `06ffde2f` on `wd/macos-0eab6454`, merged with
  history. This already includes the proxy controls, Mac build fixes, Bluetooth
  adapter, SNR telemetry, queue fixes, and Bluetooth-only AAC conversion.
- Native iPhone client: the exact tracked `ios/ChannelBankBLEClient` tree from
  `a8721904`, imported as `82b96532`. The divergent Android branch itself is
  not merged; its server changes and physical validation remain separately owned.

All desktop source and build files are identical to `06ffde2f`. In particular,
local Channel Bank `main.cpp` and `encoding.mm` are unchanged relative to the
tested Mac build. The client tree is identical to `a8721904`. No generated
artifacts or dirty protocol/known-good documents from other worktrees are included.

The historical porting instructions below describe provenance, not steps to
repeat after this consolidation. Future Mac and iPhone work should start from
the resulting `integration/main`; do not reapply those historical patches.
The user has performed live testing; this merge adds no claim of exhaustive
hardware, audio-quality, or throughput validation. Existing Mac package and
signed iPhone installation evidence applies because their implementation trees
are preserved. Focused Swift and Mac codec/lease tests are rerun on the candidate.

### Latest AAC Build Handoff

- Mac adapter implementation: `7e241d9e` on
  `origin/wd/channel-bank-macos-telemetry`. Apply this after the existing
  telemetry/queue fixes, including `35cb1e43` (or its integrated equivalent).
- iPhone implementation: `a8721904` on
  `origin/codex/sdrpp-android-channel-bank-ble`, including UI threading fix
  `6deebec2` and download-continuity fix `3486ccad`. Installed on the iPhone.
- Rebuild the Mac test app with `7e241d9e`. It adds only a Bluetooth-private
  AAC copy, preparation polling and adapter tests/documentation. Preserve the
  local Mac Channel Bank pipeline: do not change `main.cpp`, `encoding.mm`,
  playback, recording, transcription, or the WebUI for this feature.
- Mac focused build and radio-free codec/lease tests passed. The ten-second
  fixture shrank from 960,044 to 51,434 bytes and decoded at full duration.
  All 32 Swift tests and the signed iPhone build/install passed. Physical
  Mac-to-iPhone compressed transfer and audible playback still need testing.

- Canonical shared target: `integration/main` at `c07b15c6`.
- The packaged macOS test app at local commit `a3ae628e` is an intentionally
  layered test artifact, not the canonical source history.
- The macOS candidate is `origin/wd/channel-bank-macos-telemetry`; latest
  implementation commit is `7e241d9e` (subsequent commits may update this map).
- The current Android/iPhone candidate is
  `origin/codex/sdrpp-android-channel-bank-ble` at `a8721904`.
- Do not modify the dirty `docs/RX888_MACOS_KNOWN_GOOD.md` in the existing
  integration checkout. Do not stage generated build directories or the dirty
  `misc_modules/channel_bank/BLE_GATT_PROTOCOL.md` from the Android checkout.

## Do Not Duplicate Mac BLE

`625f6345` is a complete macOS adapter snapshot created on the divergent
`codex/channel-bank-local-noise` baseline. It includes the effective content
of these older commits:

1. `e4a5fcd3` - macOS CoreBluetooth control adapter.
2. `fe6cd1b9` - leased completed-playback file transfer.

Do not cherry-pick all three commits into `integration/main`. That creates
overlapping adapter files and risks silently resolving away either playback or
telemetry behavior.

Instead, use `e4a5fcd3` and `fe6cd1b9` as the adapter base, then port only the
characteristic-0007 telemetry additions from `625f6345` into the resulting
integration candidate. Apply `7c4a962a` after that port, then the compact-frame
scheduler follow-up from this branch. Together they supply startup and queue
behavior needed for a usable Summary before the large full State transfer and
ensure current SNR telemetry can pass a paused full-State transfer.

The Mac telemetry port must retain all of the following:

- Characteristic `7d2f0007-8c4b-4d7a-9a61-8e3c4f2a1000`, Read + Notify.
- The schema-1 compact binary payload at 4 Hz, capped at 160 points.
- The `bleSnrTelemetryPayload()` fixed-grid producer in Channel Bank.
- Lossy coalescing for unsent telemetry frames.
- Completion of a started frame on each characteristic, while allowing Response,
  Summary, and current telemetry to pass a paused full-State transfer.
- Initial Summary before periodic full State, and full State at the reduced
  cadence from `7c4a962a`.

## iPhone Client Series

Bring the whole iPhone client series, in order, from
`codex/sdrpp-android-channel-bank-ble`; do not select only the last two
diagnostic commits. The range starts with `5fe3d1f2` and currently ends at
`a8721904`:

```text
5fe3d1f2 aee8082b 6c46f414 1191ce26 9f6dd25a 1936084a daf56435
4ed14785 b0a37542 653d5932 55737fee b93aa410 6c490b4a ab8a20d8
4367baef fe5da81c 23b176ec fe401569 402000a0 747df005 a6fbbe38
53aaeff5 40f79a58 893b2049
6deebec2 3486ccad a8721904
```

Commits `40f79a58` and `893b2049` preserve fresh SNR telemetry across delayed full State
snapshots, add bounded receipt diagnostics, and fall back to the compact State
Summary route rather than requesting another full State over BLE.
The following three fixes serialize UI/request state on the main actor, finish
leased audio before following a new clip, and support AAC preparation polling.

## Android Series

Android physical build/install remains owned by its separate session. Preserve
the existing Android BLE-server series from the same candidate branch and make
the following telemetry commits explicit in the integration history:

1. `fa104b32` - fixed-grid Channel Bank SNR telemetry producer.
2. `af661ba6` - Android characteristic-0007 GATT transport and queue policy.

Keep Android-only code guarded by `__ANDROID__`. Do not bring Android source,
JNI, Gradle, or package changes into Windows builds.

## Clean Promotion Procedure

1. Create a fresh worktree from `origin/integration/main`; do not reuse the
   currently dirty integration checkout.
2. Apply the already-tested proxy-control prerequisite `59c7399d` if it is not
   an ancestor of the selected target.
3. Apply the macOS adapter base (`e4a5fcd3`, then `fe6cd1b9`).
4. Port the telemetry-only delta from `625f6345`, resolve it against the
   current Channel Bank WebUI authority, then apply `7c4a962a` and the
   compact-frame scheduler follow-up from this branch.
5. Apply the ordered iPhone series and the Android BLE series, resolving only
   within their platform ownership boundaries.
6. Update `BLE_GATT_PROTOCOL.md` manually from the final source behavior;
   never add the currently dirty copy blindly.
7. Commit the resulting candidate once, with a merge manifest listing the
   resolved source commits and the final tree SHA.
8. Only merge that candidate into `integration/main` after the validation
   below passes.

## Required Validation

- macOS: `channel_bank` build, `bluetooth_macos_test`, portable-bundle/rpath/
  signature audit, app launch to `Ready`.
- iPhone: install the native client; confirm immediate State Summary, a live
  `RX high-rate SNR telemetry` receipt log, and SNR chart updates in fixed-grid
  Auto or Scan mode.
- macOS playback: validate leased current-playback pull after a completed
  recording exists.
- Android: perform its separate build/install/device validation.
- Windows: verify no files or build configuration changed.

Passing source/build validation does not prove discovery, throughput, audio,
or attached-radio behavior. Record physical-device results with the final
candidate SHA before promotion.
