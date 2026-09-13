# Optional macOS Core ML encoder proof of concept

The baseline is `origin/integration/main` at `c07b15c6`. The vendored marker is
`23ee03506a91ac3d3f0071b40e66a430eebdfa1d`, whisper.cpp 1.8.6. This is a copied
source tree, not a Git submodule. The local patches below deliberately leave its
provenance marker intact.

## What the vendor does

`WHISPER_COREML=ON` builds `whisper.coreml`, defines `WHISPER_USE_COREML` for
whisper, and links Apple's Foundation and CoreML frameworks.
`WHISPER_COREML_ALLOW_FALLBACK=ON` is essential: the upstream default is OFF,
which makes a missing/broken encoder fail context creation.

The runtime removes `.bin` and a trailing five-character quantization suffix
such as `-q5_0`, then appends `-encoder.mlmodelc`. Thus these two items must be
siblings in the existing SDR++ **root/channel_bank/models** directory:

```
ggml-whisper-large-v3-atc-q5_0.bin
ggml-whisper-large-v3-atc-encoder.mlmodelc/
```

Core ML consumes float32 `logmel_data` with shape `[1,128,3000]` and returns
float32 `output` with shape `[1,1500,1280]` for large-v3. It executes convolution
and transformer encoder layers. Whisper's mel preprocessing, cross-attention
preparation, token decoder, sampling, and session management remain in the
existing implementation. Keep `GGML_METAL=ON`, flash attention and Accelerate;
Core ML does not replace the decoder. The Q5 GGML file is still required and
still loaded in full; adding an FP16 Core ML encoder increases resident memory.

The snapshot has **no models/conversion scripts**. At the exact upstream commit,
`models/generate-coreml-model.sh`, `convert-h5-to-coreml.py`, and
`convert-whisper-to-coreml.py` support conversion. The shell's HF route does not
request the ANE rewrite by default. The upstream HF converter loads a full HF
checkpoint, maps to OpenAI Whisper names, and writes an intermediate full `.pt`.
Our tool instead directly maps the requested checkpoint's encoder tensors,
strictly loads them, uses an encoder-only adaptation of the upstream ANE classes,
and checks HF/reference/rewritten/Core ML output agreement. It still loads the
full HF checkpoint initially, so allow substantial RAM and disk space for large-v3.
It never calls `whisper.load_model` or downloads stock OpenAI encoder weights.

## Changes and boundaries

- Module CMake enables optional Core ML only in the macOS branch; iOS returns
  before it, Android and Windows retain their existing branches. Explicit
  `CHANNEL_BANK_COREML=OFF` restores a build without Core ML. A macOS-scoped
  CMake policy makes the existing static-library override deterministic.
- A macOS-only `CB_WHISPER_COREML` definition guards a vendored context selection
  field and active-state query. It is propagated to the module and whisper
  together. Other platform builds keep their original context structure/API.
- The existing macOS loader requests Core ML only for ATC Large, only when the
  expected directory exists, and only when not disabled by environment.
  Medium/Turbo stay on ggml/Metal. No public Channel Bank transcription API,
  queue, detached worker, cancellation, or context-cache redesign is involved.
- The Core ML wrapper validates input/output dimensions and types on load;
  failed/incompatible loads use the existing fallback. Prediction failures or
  unexpected output layouts return an inference error cleanly. They do **not**
  retry mid-inference with a differently allocated encoder graph.
- Core ML uses `MLComputeUnitsAll`: ANE is eligible, but the operating system
  decides placement. The log says Core ML active only after successful loading;
  this is not proof of ANE execution. Compiled support (`COREML = 1`) alone is
  also not an activity indicator.
- Restart the app after installing/removing an encoder or changing the
  environment: existing contexts remain cached for the process/session lifetime.
- The model filename and tensor shapes cannot prove fine-tuning identity. Use
  only this tool's output from the specified ATC checkpoint, retain its JSON
  provenance, and never rename a stock large-v3 encoder into the ATC slot.

## Build

Run from this feature checkout (substitute its location on another machine):

```sh
cd /Users/willdarney/Documents/SDR++/SDRPlusPlus-channel-bank-coreml
cmake -S . -B build-channel-bank-coreml-check \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DUSE_BUNDLE_DEFAULTS=ON -DOPT_BUILD_CHANNEL_BANK=ON \
  -DCHANNEL_BANK_COREML=ON -DOPT_BUILD_ADSB=OFF
cmake --build build-channel-bank-coreml-check --target channel_bank sdrpp -j8
```

ADS-B is disabled only for this focused build: its current local OBJCXX language
setup causes an unrelated fresh desktop configure problem. No ADS-B source or
default was changed. Core ML's `.m`/`.mm` sources follow the embedding desktop
build's existing C/C++ compiler convention; Clang recognizes the extensions.

For the compile-time baseline, use a separate directory and the same flags,
changing `-DCHANNEL_BANK_COREML=OFF`; then build `channel_bank`.

Whisper, ggml and the Core ML bridge are statically linked into the module.
`otool -L build-channel-bank-coreml-check/misc_modules/channel_bank/channel_bank.dylib`
should show **system CoreML, Foundation, Accelerate, Metal** dependencies.
Do not copy system frameworks into an app. Keep the existing SDR++ bundle
packaging/signing workflow for other dependencies. Install the entire compiled
`.mlmodelc` directory as external model data, not its individual files or the
`.mlpackage`. No Core ML compiler/Python packages are needed on the runtime Mac.
The converter targets macOS 13 or newer. No app deployment is performed by these
commands; deployment still follows the module's private deployment guide.

## Convert the exact ATC model

Use Python 3.11 on an Apple Silicon Mac with Xcode's `coremlc` available:

```sh
python3.11 -m venv /tmp/cb-coreml-venv
/tmp/cb-coreml-venv/bin/pip install 'setuptools<81' wheel
/tmp/cb-coreml-venv/bin/pip install --no-build-isolation \
  -r misc_modules/channel_bank/tools/coreml-requirements.txt
/tmp/cb-coreml-venv/bin/pip install --no-deps 'ane-transformers==0.1.3'
/tmp/cb-coreml-venv/bin/python \
  misc_modules/channel_bank/tools/convert_hf_coreml_encoder.py \
  --model jacktol/whisper-large-v3-finetuned-for-ATC \
  --ggml-name ggml-whisper-large-v3-atc-q5_0.bin
```

The separate `--no-deps` install is intentional: ANE transformers' old package
metadata pins Torch <=1.11; this converter uses its LayerNorm helper with the
verified Torch 2.5/Core ML tools environment. Avoid upgrading it implicitly.

Remote model IDs are resolved to an immutable HF commit before loading; pass
`--revision <HF-commit>` to reproduce a particular snapshot. `--model` also
accepts a local saved HF checkpoint/snapshot directory. Use the same fine-tuned
checkpoint revision used to produce your Q5 model when its provenance is known.
The existing GGML filename alone cannot recover that historical HF revision.

Outputs are the compiled encoder, an intermediate `.mlpackage`, and a `.json`
manifest containing source/revision, original encoder tensor SHA-256, dimensions,
whisper revision, tool versions, and measured conversion error. The tool refuses
to overwrite existing outputs. The manifest hash identifies source weights;
it is not a claim that quantized GGML bytes have the same hash. Conversion uses
FP16 internal arithmetic with float32 I/O, independently of GGML Q5 quantization.

By default, outputs go directly to
`~/Library/Application Support/sdrpp/channel_bank/models` (resolved with
`Path.home()`), so no copy step is needed for the default local profile. Existing
outputs are never overwritten. Restart the test app after successful conversion.

For a different profile or a staging-only conversion, pass `--output-dir`, for
example `--output-dir /tmp/cb-atc-coreml`. If you use that staging directory, copy
only the `.mlmodelc` directory (and retain the manifest for provenance) beside the
existing GGML model. Example for the default local profile, **only if this is the
root used by your test app**:

```sh
cb_models="$HOME/Library/Application Support/sdrpp/channel_bank/models"
test ! -e "$cb_models/ggml-whisper-large-v3-atc-encoder.mlmodelc" && \
  ditto /tmp/cb-atc-coreml/ggml-whisper-large-v3-atc-encoder.mlmodelc \
    "$cb_models/ggml-whisper-large-v3-atc-encoder.mlmodelc"
```

## Checks and profiling

Fast conversion contract tests and the synthetic end-to-end conversion:

```sh
/tmp/cb-coreml-venv/bin/python \
  misc_modules/channel_bank/tools/test_coreml_conversion.py --integration
```

Build the isolated runtime smoke helper against the same static whisper build:

```sh
cb_vendor="$PWD/misc_modules/channel_bank/external/whisper.cpp"
cb_build="$PWD/build-channel-bank-coreml-check/misc_modules/channel_bank/external/whisper.cpp"
c++ -std=c++17 -DCB_WHISPER_COREML \
  -I "$cb_vendor/include" -I "$cb_vendor/ggml/include" -I "$cb_vendor/src" \
  misc_modules/channel_bank/tools/test_whisper_coreml.cpp \
  "$cb_build/src/libwhisper.a" "$cb_build/src/libwhisper.coreml.a" \
  "$cb_build/ggml/src/libggml.a" "$cb_build/ggml/src/libggml-cpu.a" \
  "$cb_build/ggml/src/ggml-blas/libggml-blas.a" \
  "$cb_build/ggml/src/ggml-metal/libggml-metal.a" "$cb_build/ggml/src/libggml-base.a" \
  -framework Accelerate -framework Foundation -framework CoreML -framework Metal \
  -o /tmp/cb-test-whisper-coreml
/tmp/cb-coreml-venv/bin/python \
  misc_modules/channel_bank/tools/test_coreml_conversion.py --integration \
  --runtime-test /tmp/cb-test-whisper-coreml \
  --ggml-model "$HOME/Library/Application Support/sdrpp/channel_bank/models/ggml-whisper-large-v3-atc-q5_0.bin"
```

These checks use temporary symlinks and never alter the installed model. They
exercise synthetic bridge prediction, disabled/missing/damaged/wrong-shape
encoder loads with the real GGML file. After installing the actual ATC encoder:

```sh
/tmp/cb-test-whisper-coreml \
  "$HOME/Library/Application Support/sdrpp/channel_bank/models/ggml-whisper-large-v3-atc-q5_0.bin" active
```

For live transcription, select ATC Large in a complete test app, submit the same
recordings, and inspect Console or the process log for `[CBWhisper]`:

- `encoder=Core ML active; decoder=whisper.cpp/Metal` means the context loaded
  Core ML. Confirm successful completed transcriptions as well.
- `encoder=ggml/Metal fallback` means the encoder remains on ggml/Metal.
- `Core ML load failed` or `incompatible Core ML encoder` explains a fallback.

Quit and relaunch the test executable directly with `CB_WHISPER_DISABLE_COREML=1`
for the baseline, preserving the same `--root` and arguments. Unset the variable
and restart for Core ML. An already-running app will not inherit a new shell's
environment. Warm both runs first, then compare repeated identical recordings,
transcript quality, steady-state latency, and memory; measure first-load time
separately because Core ML may compile/specialize on first use.

In Instruments, attach to the test SDR++ process, use the Core ML template and
add Neural Engine and GPU/Metal tracks. Correlate **Core ML encoder predictions**
with Neural Engine activity. Decoder GPU activity should remain. Core ML events
without corresponding ANE activity do not demonstrate ANE acceleration; GPU
activity by itself cannot distinguish Core ML GPU work from the ggml decoder.
See Apple's [Core ML profiling walkthrough](https://developer.apple.com/videos/play/wwdc2022/10027/)
and [compute-unit semantics](https://developer.apple.com/documentation/coreml/mlcomputeunits).

## Validation recorded for this proof of concept

- macOS arm64 `channel_bank` and `sdrpp` build with Core ML enabled.
- Separate macOS `channel_bank` build with `CHANNEL_BANK_COREML=OFF` succeeds.
- Synthetic HF -> reference -> ANE rewrite -> Core ML prediction -> compiled
  encoder succeeds; prediction also succeeds through the vendored native bridge.
- Actual installed ATC Q5 file loads with Core ML disabled and falls back for
  missing, malformed, and wrong-dimension Core ML encoders.
- Full fine-tuned ATC conversion, live Channel Bank transcription, ANE placement,
  performance/accuracy A/B and deployment are **not yet validated**. Synthetic
  checks are deliberately not presented as ATC-model or hardware proof.
- Windows/Android/iOS were not built; their branches and runtime implementations
  were not changed.
