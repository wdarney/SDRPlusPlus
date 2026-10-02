# AM sample processing

This change replaces only the desktop Channel Bank AM sample processor and
repairs sample continuity at the recording boundary. Detection thresholds,
receiver allocation, recording lifetime, user settings, queued playback,
transcription and encoding retain their existing paths. FM/SSB demodulation
is unchanged; the pre-roll duplication correction also benefits those modes.

## Reference behavior and limits

Static inspection of MykolaSDR FREE 1.6.89 ARM64 located the AM magnitude path
in the buffered audio worker at `0x1001e665c`. The decisive instructions are
at `0x1001e8eb4–0x1001e8f40`: calculate IQ magnitude, update a persistent
exponential envelope estimate, subtract it, and divide by that estimate with
a `1e-5` denominator floor and a 0.25 scale. The initializer stores the
`exp(-1/(sampleRate*0.3))` coefficient into state offset `0x73c`.

After its intervening filter stage, `0x1001e968c` onward applies the persistent
high-pass recurrence `y = a*(previousY + x - previousX)`, where the initializer
sets `a = exp(-2*pi*300/sampleRate)` at state offset `0x74c`. The result is
scaled by 3 and bounded to [-1, 1]. These arithmetic/state transitions were
checked against ARM64 instructions because the decompiler omitted the
denominator floor from its pseudocode. This is an independently written
implementation of the observed equations, not imported vendor source.

Channel Bank keeps its existing 48 kHz rate and bandwidth filter. It corrects
the envelope estimator's startup bias and contains nonfinite input;
these are deliberate startup/numerical safeguards, not claimed vendor parity.
Mykola's broader channelizer, ring scheduling, squelch and playback policy are
not ported. Audio tone and level can differ because the sample processor has
changed; the existing Rec Gain remains a subsequent multiplier.

## Recording continuity

The AM recording envelope now moves per sample through the existing 50 ms
raised-cosine fade, including a smooth reversal when RF detection recovers.
The two-hit qualification after complete silence and the RF/file-close rules
are retained. FM/SSB retain their existing hold and fade implementation.

The pre-roll ring already receives the current callback before a recording
opens. Only older retained samples are now prepended; the normal recording
path writes the current callback once. Previously the current callback was
written both as pre-roll and again as live audio. The ring wrap and blocks
larger than the ring are covered by sequence-continuity tests.

## Validation

Build `channel_bank` and `channel_bank_am_audio_samples_test`, then run
`ctest --test-dir <build>/misc_modules/channel_bank --output-on-failure`.
The audio regression checks actual demodulator/FIR output across arbitrary
callback partitions, carrier-level invariance, steady-carrier silence,
nonfinite input recovery, bounded fade/recovery steps, and pre-roll continuity.
It also runs a continuous synthetic AM signal through the actual RxVFO at
32 MS/s to 48 kHz using alternating source block sizes.

These checks do not establish that the reported receiver recordings are
fixed. Their repeated sub-millisecond impulses have not been reproduced from
the original IQ. Compare a fresh AM recording with the manual VFO on the same
transmission; retain a WAV before optional normalization/encoding when possible.
