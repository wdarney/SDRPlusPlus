# RX888 MkII ADC control and gain-state integration

Integrated on 2026-09-08 into SDR++ `integration/main` with the paired SDDC
driver integrated into `wdarney/SDDC_Driver` `master`.

## Exact source provenance

SDR++ feature branch `codex/rx888-adc-control` ended at
`478fe726927db911635fb12b33e71bc35c16441b` and contains:

- `10d7a2d9` — honor the ADC clock actually applied by the driver;
- `59311e83` — preserve the active RX888 profile when the same device changes
  its USB label between `WestBridge` and `RX888mk2`;
- `9f8c5170` — keep the user's preferred IQ bandwidth separate from the
  nearest temporary rate selected while changing the ADC clock; and
- `478fe726` — replace the misleading continuous desktop ADC slider with
  stable 16, 32, 64, and 128 MHz choices. An arbitrary value supplied by an
  older configuration or the control API remains visible as a custom clock
  whose bandwidth can vary.

The paired SDDC feature branch `wd/rx888-adc-control` ended at
`30a7fd9d65aacd0a677c05538c22d1dc3e9d221b`. It was merged into SDDC
`master` as `a73dce78ba820c0e7ea8e4571535be5e805fa564` and contains:

- `10ea62d` — independent 16–130 MHz MkII ADC-clock control, exact
  power-of-two decimation planning, ADC-derived IQ-rate enumeration, applied
  clock reporting, and an 8 MHz VHF IQ-rate limit;
- `95a5efc` — initialize host gain caches as unknown, invalidate and restore
  VHF gains after tuner reset, and propagate failed gain writes instead of
  recording them as successful;
- `e82b98b` — work around the separate legacy FX3 IF-VGA cache after
  `TUNERINIT`; and
- `30a7fd9` — extend that workaround to Refresh, where a new host object can
  reconnect to firmware retaining stale gain state.

## Why the IF workaround is necessary

The legacy FX3 firmware's tuner initialization resets the physical R828D IF
VGA register to step 11, approximately +26.5 dB, but does not reset its
`m_vgagain_index` cache. A later request for the same cached step can therefore
be suppressed even though the hardware register no longer matches it. SDR++
then displays the saved IF gain while the tuner remains at the initialization
gain; touching the IF slider forces a different step and makes the waterfall
change abruptly.

After every `TUNERINIT`, the SDDC host now writes a different valid IF step and
then the intended step. If no host value exists yet, it finishes at the safe
-4.7 dB minimum until the application applies its saved value. Two distinct
writes guarantee that at least one differs from any stale firmware cache and
that the final physical register matches the host. The bundled FX3 firmware
image and its USB protocol are unchanged.

## ADC clock and IQ bandwidth behavior

The MkII real-to-IQ converter supports power-of-two decimation:

`IQ rate = ADC clock / (2 * 2^decimation)`

An arbitrary clock such as 78 MHz therefore cannot produce exactly 8 MHz; its
nearby rates include 4.875 and 9.75 MHz. Feeding each temporary nearest rate
back into a continuous slider drag caused the selected waterfall bandwidth to
drift. SDR++ now retains the explicit user preference while recalculating the
currently compatible rate. The standard 16/32/64/128 MHz desktop choices keep
the familiar 1/2/4/8 MHz family available where allowed by the driver.

Configuration continues to use the normal shared SDR++ data root. Per-device
state now records both `sampleRate` (the compatible rate in use) and
`preferredSampleRate` (the user's explicit target). No version-specific data
root was introduced.

## Validation completed before integration

The paired macOS test bundle was built from SDR++ `478fe726` and SDDC
`30a7fd9d`, with ARM64, bundle defaults, Apple Accelerate, portable runtime
paths, and strict recursive ad-hoc signature checks. All 15 SDDC tests passed,
including sample-rate planning, VHF gain reinitialization, R2IQ processing,
and R2IQ tone purity. The packaged module loaded in a smoke test.

The merged SDDC `master` result was rebuilt on 2026-09-08. The complete Soapy
module linked and all 15 tests passed again. These checks establish source,
DSP, and packaging integrity; they do not replace attached-hardware testing of
the final stale-firmware-cache workaround.

## Attached-hardware acceptance test

1. Select VHF mode, an 8 MHz IQ rate, and one of the standard ADC clocks.
2. Set RF and IF gains and note the noise floor, desired signal, and spurs.
3. Stop, press Refresh, and start without touching either gain control.
4. Confirm the waterfall and spur level remain consistent.
5. Repeat after HF to VHF switching and at 32, 64, and 128 MHz ADC clocks.
6. Confirm that changing among standard ADC clocks retains the explicit 8 MHz
   bandwidth and that no power cycle is required.

If the waterfall changes only after moving a gain slider, capture the saved
gain values and runtime log before making further changes; that would indicate
another hardware-state mismatch rather than an R2IQ mirror.
