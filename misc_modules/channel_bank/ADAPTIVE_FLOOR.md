# Adaptive spectrum floor and detector resolution

Source change based on integration/main `6e7572e2`. Desktop entry point only;
the independent iOS DSP implementation is unchanged.

## Behavior

The existing **Local SNR floors** switch is now **Adaptive noise floor** and
applies to Manual, Auto, range scanning, and bookmark scanning. The persisted
profile/API key remains `manualLocalSnrEnabled`, preserving existing clients
and saved preferences. If the old switch was off, enable it once. Switching it
off selects the legacy shared floor; the improved FFT resolution remains active.

The detector builds a frequency-dependent background estimate from overlapping
128 kHz neighborhoods, with anchors every 32 kHz. A lower (20th) percentile
reduces contamination by narrow signals. The estimate uses instantaneous FFT
power with the exponential-noise quantile correction `1 / -ln(0.8)`; it then
smooths in dB with a one-second time constant and interpolates between anchors.
This aims to put noise-only channel measurements near 0 dB. It is an estimate,
not a calibrated RF power measurement; densely occupied neighborhoods and
non-Gaussian interference can bias it. Previously selected SNR margins may
need an initial adjustment because the former estimator had different bias.

Each channel averages the floor over the same bins as its signal measurement.
Voting, instantaneous presence, sustained SNR, telemetry, and recording SNR
use that reference. The existing vote counts, hold hysteresis, miss limits,
AM/SSB interference gates, audio hold/tail, and recording logic are retained.
There is no new AM carrier classifier or additional signal trigger.

The existing wideband-event test uses the corresponding local floor in adaptive
mode; its occupancy thresholds are unchanged. Manual storm detection retains
its existing shoulder estimator and decisions separately from the new curve.

The source review also found that bookmark scan consumed manual detector
indices while the DSP ran the Auto grid. Bookmark scan now runs the existing
manual detector with a synchronized copy of the current stop's frequency list.
Its SNR telemetry carries the matching frequency mapping.

## FFT and sample collection

The FFT is the smallest power of two providing at most 250 Hz/bin, bounded
between 8,192 and 262,144 points:

| Source rate | FFT points | Bin spacing |
| --- | ---: | ---: |
| 2 MHz | 8,192 | 244.14 Hz |
| 8 MHz | 32,768 | 244.14 Hz |
| 16 MHz | 65,536 | 244.14 Hz |
| 64 MHz | 262,144 | 244.14 Hz |

The collector assembles consecutive real samples across input callbacks and
skips the rest of each 50 ms analysis period. At low sample rates, it windows
the actual collected sample count before zero-padding. Retunes discard partial
frames and reset the spectral estimates. The intended analysis rate remains
20 Hz; demodulation and recording sample rates are unchanged.

The Channel Bank spectrum shows a cyan background curve and orange start
threshold curve, plus individual override ticks. The main waterfall uses a
separate FFT/window calibration, so its old absolute horizontal threshold is
replaced by an adaptive-margin label when enabled. The small spectrum renders
one peak per display pixel to bound drawing cost as FFT size increases.

## Handoff and validation boundary

Only source review has been performed. At the user's request, this session
does not run tests, compile modules, package an app, deploy, or perform live RF
testing. A CMake configuration was attempted before that instruction and failed
while obtaining unrelated Brown DSD dependencies; no module or app was built.

The build session should take this focused branch/commit on a compatible
integration baseline. Both `src/main.cpp` and `src/spectral_floor.h` are required.
No new library or CMake dependency is introduced. The intended runtime target
is a separate local macOS test app. Reception accuracy, behavior under gain
changes and interference, and CPU cost await the user's testing.
