# Channel Bank buffer-safety test candidate

Baseline: `049fb805` (the reported crashing build). The user-tested prior
baseline is `afd744bb`. Both contain the undersized stream buffers; this change
fixes a reproduced overwrite, but does not yet establish why only the newer
application crashes in live use.

## Change

Restore the core `STREAM_BUFFER_SIZE` capacity for Channel Bank RF and audio
streams. `RxVFO::process` first translates the whole input into its output
buffer, then downsamples in place. Sizing that buffer for final audio output
alone is unsafe. Splitter also copies complete input blocks without checking
destination capacity, so RF input and downstream buffers retain the same
standard capacity.

This is a conservative memory-for-safety tradeoff: about 70 MiB more allocated
buffer capacity per active channel, plus about 15 MiB for the monitor stream.
Smaller buffers require separately enforced producer block-size limits.
No Whisper/Core ML behavior or iOS implementation is changed.

## Validation and build handoff

The regression calls the actual RxVFO with input blocks of 32,768, 65,536,
262,144 and 1,000,000 samples. Extra guard space makes the old-capacity test
safe: it detects writes beyond the configured capacity without corrupting
the test process. The old values fail for all three larger blocks; restored
capacities pass all four. The focused test was linked against the existing
matching baseline core. The desktop module also passed a syntax-only compile.

Full application compilation, packaging and live receiver testing are left to
the macOS build session. With `BUILD_TESTING=ON`, build the normal application
and module targets plus `channel_bank_stream_buffer_capacity_test`, then run:

```sh
ctest --test-dir <build>/misc_modules/channel_bank --output-on-failure
```

Use a separate test application and copies of the same profile for comparisons
with `afd744bb` and unmodified `049fb805`. First repeat the reception scenario
with transcription disabled, then with transcription enabled. Record input
source, sample rate, decimation, active channel count and whether it survives
the previously failing interval. Matching recorded IQ is preferable when
available. A passing test here is not proof that every reported crash is fixed.

If crashes remain, use a separate debug-symbol/AddressSanitizer build of the
core and module to catch the original invalid access. This branch does not
force sanitizer flags on ordinary builds.
