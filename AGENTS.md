# SDR++ Unified Repository Guide

## Authoritative branch

`integration/main` is the unified development baseline for this fork. It is
based on the official SDR++ upstream repository and contains the macOS,
iOS, RX888, VDL2, Channel Bank, SELCAL, packaging, and core fixes maintained
in this fork.

- `origin` is `wdarney/SDRPlusPlus`, the working fork.
- `upstream` is `AlexandreRouma/SDRPlusPlus`, the official project.
- Never develop directly on `integration/main`. Start a focused branch named
  `codex/<short-task-name>` from the latest `origin/integration/main`.
- Keep a task limited to the named module or failure mode unless the user
  explicitly broadens its scope.
- Do not delete or replace another platform's implementation to make one
  platform build. Use platform-specific CMake files and sources.

The pre-consolidation feature tips are protected by remote tags under
`pre-integration/2026-07-12/*`. Preserve those tags as recovery points.

## Feature locations

| Feature | Location |
| --- | --- |
| Channel Bank | `misc_modules/channel_bank/` |
| VDL2 decoder and libacars | `decoder_modules/vdl2_decoder/`, `core/libacars/` |
| RX888 source | `source_modules/rx888_source/` |
| SELCAL decoder | `decoder_modules/selcal_decoder/` |
| Brown DSD decoder | `decoder_modules/ch_extravhf_decoder/`, `decoder_modules/radio/src/radio_module_interface.h` |
| iOS application and build support | `ios/`, `core/backends/ios/`, `sink_modules/coreaudio_sink/` |
| macOS packaging/deployment | `make_macos_bundle.sh`, `deploy.sh` |

Channel Bank deliberately has separate entry points: desktop/macOS uses
`src/main.cpp`; iOS uses `src/main_ios.cpp`. Reconcile shared behavior
carefully instead of overwriting either implementation.

## Starting a new task

```sh
git fetch origin upstream --tags
git switch integration/main
git pull --ff-only origin integration/main
git switch -c codex/<short-task-name>
```

Before editing, inspect `git status`, read this file and the module's CMake
file, and confirm the requested runtime target. Commit and push the focused
branch after validation; merge it into `integration/main` only after it is
working.

## Validation

Configure the combined macOS build from the repository root:

```sh
cmake -S . -B build-integration-macos \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOPT_BUILD_RX888_SOURCE=ON \
  -DOPT_BUILD_CHANNEL_BANK=ON \
  -DOPT_BUILD_VDL2_DECODER=ON \
  -DOPT_BUILD_SELCAL_DECODER=ON
cmake --build build-integration-macos --target sdrpp channel_bank rx888_source vdl2_decoder selcal_decoder -j8
```

For Brown DSD work in `SDRPlusPlus-brown-dsd`, keep the DSD module enabled
explicitly. Fresh configures default it on, and the named preset forces it on
even when an old CMake cache is reused:

```sh
cmake --preset browndsd-macos
cmake --build --preset browndsd-macos
python3 tools/enable_browndsd_original_config.py
```

The BrownDSD macOS test app is
`/Applications/SDR++MODULETESTING-BrownDSD.app`. Its launcher intentionally
uses the original SDR++ profile at
`~/Library/Application Support/sdrpp`, so do not create a second hidden profile
unless the user asks for isolation.

For macOS runtime work, validate the installed test bundle at
`/Applications/SDR++MODULETESTING.app`, including codesign verification and a
launch/quit smoke test. A library build alone is not complete validation.

For iOS simulator work:

```sh
./ios/build_deps.sh sim
./ios/build.sh sim
```

macOS File Provider metadata under `Documents` can make the final simulator
codesign step fail even when compilation is correct. If that happens,
configure/build the iOS project under `/private/tmp`, then verify the produced
`.app` with `codesign --verify --deep --strict --verbose=2`.

Do not commit generated build directories, dependency outputs, app bundles,
recordings, or local configuration.
