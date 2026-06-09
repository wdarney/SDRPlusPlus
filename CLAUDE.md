# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

A fork of [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) (cross-platform SDR receiver) with the **Channel Bank** module — a multi-channel auto-demodulator, recorder, and transcription plugin. Primary development target is macOS (Apple Silicon). The channel_bank module is the main focus of active development; the rest of the SDR++ codebase is upstream.

## Build & Deploy Commands

```bash
# Build just the channel_bank module (fastest iteration cycle):
SDKROOT=$(xcrun --show-sdk-path) cmake --build build --target channel_bank

# Build + deploy to the local testing app + re-sign (one command):
./deploy.sh

# Full clean rebuild (rarely needed):
SDKROOT=$(xcrun --show-sdk-path) cmake -B build . && cmake --build build --target channel_bank
```

The testing app lives at `/Applications/SDR++MODULETESTING.app`. The deploy script copies the dylib to `Contents/Plugins/`, then signs the plugin explicitly before the bundle (macOS Tahoe broke `codesign --deep` — see `deploy.sh` header for details).

There are no automated tests. Verification is manual: launch the app, tune to a busy frequency range, confirm channels spawn/record/play/transcode correctly.

## Project Structure

SDR++ is a modular plugin architecture. The core binary loads `.dylib` plugins at runtime:

```
source_modules/    — SDR hardware drivers (RTL-SDR, Airspy, HackRF, etc.)
decoder_modules/   — Signal decoders (radio AM/FM/SSB, pager, meteor, etc.)
sink_modules/      — Audio output (CoreAudio, PortAudio, network)
misc_modules/      — Everything else — channel_bank lives here
core/src/          — Core app: DSP framework, GUI (ImGui), signal path, config
sdrpp_module.cmake — Shared cmake include that all plugins use
```

All plugins link against `sdrpp_core` and use its DSP blocks (`dsp::*`), config system (`ConfigManager`), GUI widgets, and signal path (`sigpath::*`).

## Channel Bank Architecture

The module is in `misc_modules/channel_bank/`. All source files:

| File | Purpose |
|------|---------|
| `src/main.cpp` (~5000 lines) | The entire module: DSP analysis, channel lifecycle, recording, playback, UI, config |
| `src/transcription_whisper.mm` | Whisper.cpp backend — Metal-accelerated, ATC-fine-tuned model inference |
| `src/transcription_whisper.h` | Public API for Whisper backend |
| `src/transcription.mm` | Apple Speech framework backend (legacy alternative) |
| `src/transcription.h` | Public API for Apple Speech backend |
| `src/encoding.mm` | WAV→M4A encoding via AudioToolbox, file timestamp backdating |
| `src/encoding.h` | Public API for M4A encoding |
| `CMakeLists.txt` | Build config — whisper.cpp vendoring, Metal, Accelerate, framework linking |
| `external/whisper.cpp/` | Vendored whisper.cpp (statically linked, Metal shaders embedded) |
| `src/rnnoise/` | Vendored Mozilla RNNoise for neural noise suppression |
| `fix_rpath.cmake` | Post-build rpath fixup for macOS bundle portability |

### Thread Model

The module runs four concurrent threads plus the UI thread:

1. **DSP/Spectrum thread** (`spectrumHandler` → `analyzeSpectrum`) — called by SDR++ sink at the sample rate. Rate-limited to 20 Hz internally. Runs the 8192-point FFT, computes per-slot power/flatness/centroid, votes on signal presence, performs NMS, updates `rawSignalPresent` atomics on active channels.

2. **Management thread** (`mgmtThreadFunc`) — wakes every 250ms or on CV notify from DSP. Reads `detectedSlots`, spawns/destroys `ChannelSlot` objects, manages signal hold timers, file open/close lifecycle, recording gates.

3. **Playback thread** (`playbackThreadFunc`) — sequential queue consumer. Plays WAV files through the monitor audio output, publishes `playbackPosMs` for synced transcript display, triggers M4A encode after playback.

4. **Encode thread** (`encodeThreadFunc`) — background WAV→M4A encoding queue. Runs transcription, embeds transcript in M4A lyrics tag, backdates file timestamps.

Per-channel DSP (VFO → demod → recorder) runs on SDR++'s own DSP worker threads via the `dsp::routing::Splitter` fanout.

### Key Data Flow

```
SDR IQ stream → sharedIqIn → iqSplitter ─┬─→ specStream → FFT analysis (20 Hz)
                                          ├─→ slot[0].iqIn → VFO → demod → splitter → recSink (WAV write)
                                          ├─→ slot[1].iqIn → ...                    → meterStream
                                          └─→ slot[N].iqIn → ...
```

### Two Frequency Identities Per Channel

Every `ChannelSlot` carries two freq values — this distinction matters everywhere:

- **`freqHz`** (centroid) — energy-weighted spectral centroid. Used for VFO placement, waterfall dot, sidebar display. Jitters slightly frame-to-frame.
- **`gridFreqHz`** (grid-aligned) — deterministic `lastKnownCenter + gridOffset`. Used for blocking, frequency history keying, `freqKey()` lookups. Stable across respawns. In manual mode, both are identical.

Using centroid for blocking/history causes spawn→destroy churn and history flooding. Using grid for VFO placement causes visible misalignment with the carrier. Don't mix them up.

### Signal Detection Pipeline (Auto Mode)

Each FFT frame (50ms): per-slot power mean → EMA smoothing (α=0.15) → vote accumulation → NMS (configurable radius) → `detectedSlots`. Spawn requires `SPAWN_VOTES` (3) consecutive frames. Active channels get a looser threshold (hold hysteresis). Instantaneous (non-EMA) power drives `rawSignalPresent` for sub-100ms fade-out.

### Recording Quality Gates

Recordings pass through three gates at file-close before being kept:

1. **Min TX Duration** — cumulative on-air frames × 50ms (not span). Filters ACARS data bursts.
2. **Spectral Flatness Gate** — geometric/arithmetic mean ratio of channel power bins. Flat (~1.0) = broadband static → discard. Peaky (~0) = carrier+voice → keep.
3. **Drift Gate** — stddev of carrier centroid over the recording. High stddev (>700 Hz) = faulty/drifting emitter → discard.

### Whisper Integration

whisper.cpp is vendored under `external/whisper.cpp/` and statically linked (`BUILD_SHARED_LIBS=OFF`). Metal shaders are embedded in the dylib (`GGML_METAL_EMBED_LIBRARY=ON`). Model contexts are cached in `g_ctxCache` (loaded once, reused across transcriptions). Inference runs on a detached thread at `QOS_CLASS_UTILITY` with `n_threads=2` to avoid starving the audio pipeline.

Models live at `~/Library/Application Support/SDR++/channel_bank/models/`. Install the ATC Medium model:
```bash
/usr/bin/curl -L -o ~/Library/Application\ Support/SDR++/channel_bank/models/ggml-whisper-medium.en-atc-q5_0.bin \
  'https://huggingface.co/borisdiakur/whisper-finetuned-for-ATC-ggml/resolve/main/whisper-medium-v3-finetuned-for-ATC-ggml.bin'
```

**Critical shutdown ordering:** `transcription_whisper::shutdown()` must be called in `~ChannelBankModule()` before static destructors run. Otherwise ggml-metal's residency set assert fires (`SIGABRT`).

## Important Constraints

- **Filename format must not change.** A web server at `/Users/willdarney/audio-monitor/` parses recording filenames. The format is `BookmarkName_FreqMHz_HH-MM-SS_DD-MM-YYYY.wav` (and `.m4a`).
- **M4A file timestamps are backdated** to actual transmission time (parsed from the filename) using `utimes()` + `setattrlist(ATTR_CMN_CRTIME)`, so they correlate with FlightRadar24.
- **macOS Tahoe codesign:** `--deep` is broken for ad-hoc signed bundles. Sign the plugin dylib explicitly first, then the outer `.app` bundle. See `deploy.sh`.
- **8 GB RAM target.** The ATC Medium Whisper model (~540 MB resident) is the practical ceiling. Large-v3 F16 (3.1 GB) causes memory pressure.

## UI Layout

The module panel uses fixed-height `ImGui::BeginChild` regions so that modules below channel_bank in the sidebar don't shift when channels spawn/expire or transcripts appear. If adding new UI sections, wrap them in fixed-height children or the sidebar jitters.

## Config Persistence

All settings persist via SDR++'s `ConfigManager` (`config.conf[name]["key"]`). Frequency history (`freqLog`) is a map of `freqKey(hz)` → `FreqEntry` structs, saved to the JSON config. `freqKey()` rounds to nearest kHz (`int64_t`).
