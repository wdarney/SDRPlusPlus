# SDR++ Custom Build

Fork of [AlexandreRouma/SDRPlusPlus](https://github.com/AlexandreRouma/SDRPlusPlus) with custom modules for wideband aviation monitoring, multi-protocol data link decoding, and direct-sampling SDR support via the RX888 MkII.

## What is SDR++?

SDR++ is an open-source, cross-platform SDR (Software Defined Radio) receiver application written in C++. It provides a real-time spectrum/waterfall display, modular source and decoder architecture, and an ImGui-based UI. You plug in SDR hardware via USB, and SDR++ lets you tune, visualize, and decode radio signals.

---

## Repository layout

Custom work spans two branches:

| Branch | Contains |
|--------|----------|
| `master` | vdl2_decoder, hfdl_decoder, selcal_decoder, vendored liquid-dsp |
| `claude/laughing-carson` | channel_bank, rx888_source, deploy scripts, Windows packaging |

```
decoder_modules/vdl2_decoder/   VDL2 + ACARS + ADS-B/Mode S decoder (master)
decoder_modules/hfdl_decoder/   HF Data Link decoder (master)
decoder_modules/selcal_decoder/ SELCAL tone decoder (master)
misc_modules/channel_bank/      Wideband channel scanner/recorder (laughing-carson)
source_modules/rx888_source/    RX888 MkII source via SoapySDDC (laughing-carson)
core/liquid-dsp/                Vendored DSP library for HFDL (master)
core/libacars/                  Vendored ACARS/VDL2 protocol parser
core/libcorrect/                Vendored Reed-Solomon + convolutional FEC
deploy_macos_testing.sh         One-command macOS build+deploy+sign (laughing-carson)
```

SDR++ upstream is at `core/`, `sink_modules/`, etc. We also have a defensive fix in `core/src/config.cpp` (catches invalid UTF-8 in `json::dump()`).

---

## Module: vdl2_decoder

**Location:** `decoder_modules/vdl2_decoder/src/` (~3,150 lines across 7 files)

### What it does
Multi-protocol aviation data link decoder. Decodes three protocols from raw IQ samples in a single module:

**VDL2** (VHF Digital Link Mode 2) — D8PSK at 31,500 baud on 136 MHz band. Carries ACARS, CPDLC (Controller-Pilot Data Link Communications), and ADS-C. DSP chain: AGC → frequency offset correction → matched filter → Gardner timing recovery → constellation demodulation → frame sync → Reed-Solomon FEC → libacars protocol parsing.

**ACARS** (Aircraft Communications Addressing and Reporting System) — AM-MSK at 2,400 baud on 129/130/131 MHz. Legacy text-based aircraft messaging. DSP: envelope detection → bit-clock recovery → NRZI decoding → frame CRC → libacars parsing.

**ADS-B/Mode S** — PPM at 1 Mbps on 1090 MHz. Aircraft position/velocity/identification broadcasts. DSP: magnitude computation → preamble correlation (pulse pattern at samples 0,2,7,9 at 2 Msps) → PPM bit extraction with bit continuity → CRC-24 validation → 1-bit error correction → DF17 parsing (callsign, CPR position, velocity).

### Source files
| File | Purpose |
|------|---------|
| `main.cpp` | Channel management, VFO routing, ImGui UI, SQLite message storage |
| `vdl2_dsp.h` / `vdl2_dsp.cpp` | VDL2 D8PSK demodulator + frame decoder |
| `acars_dsp.h` / `acars_dsp.cpp` | ACARS AM-MSK demodulator |
| `adsb_dsp.h` / `adsb_dsp.cpp` | ADS-B/Mode S PPM decoder with CPR position decoding |

### Key features
- Multi-channel: monitors 10+ frequencies simultaneously via SDR++ VFO system
- Per-channel VFO bandwidth/sample rate overrides (e.g. 50 kHz BW for narrow signals, 2 MHz for ADS-B)
- Debug stats panel with per-mode decode counters (syncs, CRC pass/fail, messages, aircraft)
- SQLite message database with configurable retention and search/filter
- Color-coded channel list and message display by protocol (blue=VDL2, green=ACARS, cyan=ADS-B)

### Channel list
Configured channels with per-channel mode and optional BW/SR overrides:
- 129.125 / 130.025 / 130.450 / 131.550 MHz — ACARS
- 136.650 / 136.700 / 136.725 / 136.775 / 136.800 / 136.825 / 136.875 / 136.900 / 136.975 / 136.300 MHz — VDL2
- 1090.000 MHz — ADS-B (2 MHz BW)
- 133.334 MHz — ADS-B test (50 kHz BW override, 2 Msps internal rate)

### ADS-B implementation details
- Preamble detection at 2 Msps checking high samples at indices 0,2,7,9 vs low samples
- PPM bit extraction with bit continuity (weak transitions < 15% delta inherit previous bit)
- 1-bit error correction loop (flips bits 5-111, re-checks CRC-24)
- CRC-24 table-driven computation (Mode S polynomial 0xFFF409)
- CPR global decode with NL lookup table (from dump1090)
- DF17 parsing: callsign (TC 1-4), position (TC 9-18), velocity (TC 19)
- Aircraft pruning (remove after 300s not seen)

---

## Module: hfdl_decoder

**File:** `decoder_modules/hfdl_decoder/src/main.cpp` (~810 lines)

### What it does
HF Data Link decoder. Currently wraps `dumphfdl` as a subprocess (IQ pipe + JSON UDP output). Being rebuilt with native DSP using vendored liquid-dsp.

### Planned native DSP chain (in progress)
AGC → RRC filter → Gardner symbol sync → Costas loop → LMS equalizer → PSK demod → deinterleaver → Viterbi K=7 rate 1/2 → protocol parser (SPDU/MPDU/LPDU/HFNPDU)

**Status:** liquid-dsp vendored and building (Phase 1 complete). Phase 2 (DSP chain) is next.

Has complete HFDL frequency table (105 entries) with station names, ring buffer for IQ piping, JSON parsing, and ImGui UI.

---

## Module: channel_bank (on `claude/laughing-carson`)

**File:** `misc_modules/channel_bank/src/main.cpp` (~3,420 lines)
**Headers:** `encoding.h` (macOS M4A), `transcription.h` (macOS Speech)

### What it does
SNR-triggered multi-channel auto-demodulator, recorder, and scanner. Monitors the entire SDR bandwidth via FFT, detects signals above a configurable SNR threshold, and for each detection:
1. Spawns a per-channel DSP chain (VFO + demod + level meter + recorder)
2. Records audio to WAV (mono 48 kHz INT16)
3. Normalizes the recording (-3 dBFS, trims AGC transient, adds silence padding)
4. Optionally plays back through a monitor audio output
5. Optionally transcribes via Apple Speech Recognition (macOS)
6. Optionally encodes to M4A with embedded metadata (SNR tag + transcript lyrics)

### Threading model
| Thread | Role |
|--------|------|
| **DSP/spectrum** | `spectrumHandler` callback — rate-limited to `SPEC_ANALYSIS_HZ` (20 Hz). Runs FFT, computes per-slot SNR, updates vote counts, writes `detectedSlots` / `manualDetected`. |
| **Management** | `managementThreadFunc` — wakes every 250ms. Reads detected slots, spawns/destroys channel DSP chains, manages scan advancement, polls transcriptions. |
| **Audio handlers** | Per-slot `audioHandler` callbacks — write samples to WAV, apply gain + noise reduction + fade-in, manage silence detection and file open/close. |
| **Playback** | `playbackThreadFunc` — sequential playback queue, reads completed WAVs back through the monitor sink. |
| **Encode** | `encodeThreadFunc` — background WAV-to-M4A conversion after playback + transcription complete. Uses `AudioToolbox` on macOS, `CreateProcess` + ffmpeg on Windows. |

### Signal detection pipeline
1. **Hann-windowed 8192-pt FFT** on the full IQ bandwidth
2. **EMA smoothing** (alpha=0.15, ~7-frame window) per bin
3. **Per-slot mean power** computed over a fixed ~8 kHz detection window centered on each channel grid position
4. **Global noise floor** — 20th percentile of center-band slot means, median-filtered over 30 frames (~100ms) to reject transient RFI
5. **Vote system** — signal must exceed `snrThreshold` dB above noise floor for `SPAWN_VOTES` (3) consecutive frames before a channel is spawned. Votes decay by 1 per frame when below threshold, capped at `MAX_VOTES` (8).

### Operating modes

**Auto mode** (default) — divides the SDR bandwidth into a grid of channels at the configured spacing (8.33 / 12.5 / 25 / 50 / 100 / 200 kHz). Detects and records any signal on any grid slot.

**Manual mode** — monitors a specific list of frequencies (user-entered + bound bookmark lists from SDR++'s frequency manager). Only spawns channels for those frequencies. Supports "watched" frequencies that trigger a visual alert on signal detection.

**Scan mode** — hops the SDR center frequency across user-defined frequency ranges, dwelling on each stop until quiet for `scanQuietSec` seconds (or `scanNoSignalSec` if no signal was ever detected).

**Bookmark scan mode** — clusters frequencies from bound bookmark lists by SDR bandwidth, generates optimal center-frequency stops, and hops between them. Like scan mode but automatically computed from bookmarks.

### Per-channel DSP chain
```
sharedIqIn → iqSplitter ─┬→ specStream (spectrum analysis)
                          ├→ slot.iqIn → [FreqXlator → PowerDecimator →] RxVFO → Demod → Splitter ─┬→ meter
                          ├→ slot.iqIn → ...                                                         └→ recSink → WAV
                          └→ ...
```
A single `iqSplitter` fans out the full-bandwidth IQ from the frontend. Each channel slot gets its own frequency translation, decimation, VFO, and demodulator. At high sample rates (e.g. 64 MHz), a `PowerDecimator` reduces the per-slot bandwidth before the VFO to keep CPU load manageable.

### Demodulation modes
- **AM** — carrier-lock AGC, full channel bandwidth
- **NFM** — narrow FM, audio bandwidth = half channel spacing
- **WFM** — wide FM (150 kHz deviation)
- **USB / LSB** — 2.8 kHz SSB with spectral-centroid carrier tracking and configurable BFO trim

### Recording pipeline
1. **Warmup** — discards first 200ms (9600 samples) so AGC settles
2. **Fade-in** — 100ms cosine ramp suppresses PTT click and filter ring
3. **Gain + RNNoise** — applies `recGain` (default -12 dB), optional RNNoise neural noise suppression with wet/dry mix
4. **Silence detection** — closes file after `tailMs` (500ms) of silence
5. **Minimum duration check** — discards recordings shorter than `minTransmissionMs` (300ms)
6. **Normalization** — rescales to -3 dBFS, trims first 250ms, adds 500ms silence padding on each side (prevents Apple Speech error 1110 on short clips)

### M4A encoding
- **macOS:** Native `AudioToolbox` (AAC) via `encoding::wavToM4A()`. Embeds average SNR as iTunes comment tag (`©cmt`) and transcript as lyrics tag (`©lyr`).
- **Windows:** Spawns ffmpeg via `CreateProcess` with `CREATE_NO_WINDOW` (no console flash). Supports network paths (UNC). Falls back gracefully if ffmpeg is not found.

### Frequency log
Persistent per-frequency history stored in `channel_bank_config.json`. Tracks detection count, last-seen timestamp, user descriptions, and block status. Blocked frequencies are skipped during channel spawning.

### Settings reference
| Setting | Default | Notes |
|---------|---------|-------|
| Channel Spacing | 25 kHz | 8.33 / 12.5 / 25 / 50 / 100 / 200 kHz |
| SNR Threshold | 4 dB | Above median noise floor |
| Cooldown | 5 sec | Time before destroying a quiet channel's DSP chain |
| Signal Hold | 500 ms | Dropout hysteresis — keeps `signalPresent` true briefly after loss |
| Recording Gain | 0.25 | Linear gain before WAV write (~-12 dB) |
| Min TX Duration | 300 ms | Discard recordings shorter than this |
| Tail | 500 ms | Continue recording after signal drops |
| Max Channels | 16 | Concurrent demod/record channels |
| BW Usage | 80% | Fraction of SDR bandwidth for noise floor estimation (avoids filter rolloff) |
| Noise Reduction | off | RNNoise neural NR on recordings |
| NR Mix | 0.7 | 0 = dry, 1 = full noise reduction |
| Scan Quiet | 3 sec | Dwell time after last signal before advancing |
| Scan No Signal | 1 sec | Dwell time when no signal was ever detected on this stop |

### Config file
`channel_bank_config.json` in the SDR++ root directory.

---

## Module: rx888_source (on `claude/laughing-carson`)

**File:** `source_modules/rx888_source/src/main.cpp` (~690 lines)

### What it does
SoapySDR-based source module for the RX888 MkII direct-sampling SDR. Wraps the SoapySDDC driver with a UI for mode selection (HF/VHF), ADC clock frequency, sample rate, gain controls, bias tee, and dithering.

### Key architecture
- `loadSoapySDDC()` — on macOS, uses `dladdr()` to resolve the bundled SoapySDDC driver relative to the plugin's own dylib path and calls `SoapySDR::loadModule()`. This makes the app bundle portable across Macs without relying on `LSEnvironment` (which only works from Finder launches).
- `sanitizeLabel()` — strips non-printable and non-ASCII bytes from USB device descriptor strings before they reach the JSON config. The FX3 USB chip returns garbage in its descriptors when firmware is stale (close/reopen without power-cycle), and those invalid UTF-8 bytes would crash `nlohmann::json::dump_escaped()` in the auto-save thread.
- `deviceLabel()` / `selectDevice()` / `saveConfig()` — device selection lifecycle. Config is per-device, keyed by sanitized label.
- `queryCapabilities()` — probes the SoapySDDC driver for supported settings (`adc_frequency`, `biastee`, `UpdBiasT_HF`/`VHF`, `dithering`) and adjusts the UI accordingly. Different driver versions expose different settings.
- `reQueryGainsForMode()` — when the user switches HF/VHF mode while stopped, spawns a background thread to open the device, switch antenna, and re-query gain ranges (avoids blocking the UI during firmware upload).
- Pre-decimation: `initSlot()` calculates a power-of-2 decimation ratio for high sample rates to reduce per-channel CPU load.

### Settings
| Setting | Default | Notes |
|---------|---------|-------|
| ADC Clock | 128 MHz | Slider range 16–140 MHz. Use clean ratios with sample rate (e.g. 32 MHz ADC / 8 MHz SR = 4:1) |
| RF Gain | 0 | **0 = minimum gain** (not maximum). In HF mode this is purely an attenuator (only goes negative). |
| Mode | HF | HF = direct-sampling LTC2208. VHF = R820T2 tuner. |

### Config file
`rx888_source_config.json` in the SDR++ root directory. Stores per-device settings keyed by device label.

---

## Core patch: config.cpp

`core/src/config.cpp` — `ConfigManager::save()` wraps `conf.dump(4)` in a try/catch. If `dump_escaped()` throws on invalid UTF-8 (e.g. garbage device names from stale USB firmware), it retries with `error_handler_t::replace` instead of crashing the app. This is a defense-in-depth fix; `rx888_source` also sanitizes labels at the source.

---

## Vendored libraries

| Library | Location | Purpose |
|---------|----------|---------|
| **libcorrect** | `core/libcorrect/` | Reed-Solomon and convolutional FEC |
| **libacars** | `core/libacars/` | ACARS/VDL2/CPDLC/ADS-C protocol parsing |
| **liquid-dsp** | `core/liquid-dsp/` | DSP primitives for native HFDL decoder (AGC, filters, symbol sync, Costas loop, Viterbi). Built as static lib with NEON acceleration, no FFTW/threads/examples. |

---

## Build system

### macOS

**Prerequisites:** Homebrew with glfw, fftw, volk, zstd, pkg-config, cmake. MacPorts (`/opt/local/`) with SoapySDR for rx888_source.

**Build:**
```bash
# Configure (from repo root)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Build specific module
cmake --build build --target vdl2_decoder -- -j$(sysctl -n hw.logicalcpu)

# Build everything
cmake --build build -- -j$(sysctl -n hw.logicalcpu)
```

**For channel_bank + rx888_source** (on laughing-carson branch):
```bash
cmake -S . -B build -DOPT_BUILD_RX888_SOURCE=ON -DOPT_BUILD_CHANNEL_BANK=ON
cmake --build build --target rx888_source channel_bank -- -j$(sysctl -n hw.logicalcpu)
```

**Deploy to testing app:**
```bash
./deploy_macos_testing.sh
```
This script:
1. Builds `channel_bank` + `rx888_source` + `vdl2_decoder`
2. Copies plugins to `/Applications/SDR++MODULETESTING.app/Contents/Plugins/`
3. **Fixes dylib paths** — rewrites `/opt/local/` to `/usr/lib/` (system), `/opt/homebrew/` to `@rpath` (bundled Frameworks), cleans rpaths
4. Rebuilds SoapySDDC from `~/src/ExtIO_sddc` if source changed
5. Code-signs all dylibs and the app bundle

**Code signing identity:** `Apple Development: wdarney@outlook.com (BPB26A8GR2)`

### Portability

The deploy script rewrites all plugin dylib paths so the app bundle is self-contained:
- sqlite3, libxml2 → `/usr/lib/` (macOS system dyld cache — present on all Macs)
- glfw, fftw3f, volk, zstd → `@rpath` (bundled in `Contents/Frameworks/`)
- Build-dir and Homebrew rpaths stripped, replaced with `@loader_path/../Frameworks`

To install on another Mac: copy the `.app` bundle + config folder (`~/Library/Application Support/sdrpp-brown/`). Ensure `config.json` has all modules listed in `moduleInstances`. Run `xattr -cr /Applications/SDR++MODULETESTING.app` to strip quarantine. **No Homebrew or MacPorts needed.**

### Windows (VM)

**SSH:** `sshpass -p 'Nvidia!234' ssh willdarney@10.211.55.42`
**Source:** `C:\sdrplusplus` (tracks `claude/laughing-carson`)
**Build dir:** `C:\SDRPlusPlus\build`

```powershell
cd C:\sdrplusplus && git pull origin claude/laughing-carson

# Build one module:
powershell -ExecutionPolicy Bypass -Command "cd C:\SDRPlusPlus\build; cmake --build . --target channel_bank --config Release"

# Package:
Remove-Item -Force -ErrorAction SilentlyContinue C:\SDRPlusPlus\sdrpp_windows_x64.zip
Remove-Item -Force -Recurse -ErrorAction SilentlyContinue C:\SDRPlusPlus\sdrpp_windows_x64
powershell -ExecutionPolicy Bypass -Command "cd C:\SDRPlusPlus; & .\make_windows_package.ps1 'C:\SDRPlusPlus\build' 'C:\SDRPlusPlus\root'"
```

**Important:** Always use `-ExecutionPolicy Bypass` — script execution is disabled by default on the VM.

---

## macOS app bundle structure

```
/Applications/SDR++MODULETESTING.app/
  Contents/
    MacOS/sdrpp                              Main executable
    Plugins/
      vdl2_decoder.dylib                     Aviation data link decoder
      hfdl_decoder.dylib                     HF Data Link decoder
      channel_bank.dylib                     Channel scanner module
      rx888_source.dylib                     RX888 source module
      ...                                    Other SDR++ modules
    SoapySDR/modules0.8/
      libSDDCSupport.so                      SoapySDDC driver (firmware compiled in)
    Frameworks/
      libSoapySDR.0.8.dylib                 SoapySDR runtime
      libfftw3f.3.dylib                      FFT library
      libusb-1.0.0.dylib                     USB library
      ...
    Info.plist                               LSEnvironment sets SOAPY_SDR_PLUGIN_PATH
```

**Launch:** Always launch from **Finder or Dock**, not Terminal. `LSEnvironment` only takes effect via LaunchServices.

---

## RX888 hardware notes

### FX3 USB chip + firmware
The Cypress FX3 chip holds firmware in RAM. SoapySDDC has the firmware compiled in as a C array (`firmware.h`) and uploads it on device open. **After any driver change, power-cycle the RX888** (unplug/replug USB). Without this, the chip runs stale firmware and exhibits:
- Wrong sample rate caps (e.g. 6 MHz max instead of 64 MHz)
- Missing device settings (`adc_frequency` absent)
- USB descriptor corruption (garbage appended to device name)
- Device shows as "westbridge" in USB device list (bootloader mode)

### Gain structure

**HF mode (direct sampling, LTC2208 ADC):**
- RF Gain is purely an **attenuator** — 0 dB = full signal, negative values = attenuation
- IF Gain is **digital gain** — does not improve SNR, only scales the numbers post-ADC
- The ADC has ~77 dB SFDR; all signals from DC to Nyquist compete for that dynamic range
- There is no bandpass filtering — the ADC sees everything the antenna picks up

**VHF mode (R820T2 tuner):**
- RF Gain = LNA gain (amplifies before internal filtering)
- IF Gain = VGA gain (amplifies after internal filtering — "safer" with strong interference)
- Strategy: keep RF gain moderate (5-15), use IF gain for the rest

### Recommended airband setup
Signal chain: Multi-antenna (3 dB gain) → MW filter → FM filter → LNA → airband downconverter → RX888 HF input.

| Parameter | Value | Why |
|-----------|-------|-----|
| ADC Clock | 32 MHz | Clean 4:1 ratio with 8 MHz sample rate |
| Sample Rate | 8 MHz | Covers 118–126 or 126–134 MHz etc. |
| RF Gain | ~0 to -3 dB | With filters + LNA upstream, signal is strong. Back off to reduce intermod/ghost signals on strong transmissions. |
| Mode | HF | Downconverter shifts airband to HF range |

**Ghost signals on strong transmissions** are typically ADC intermod products or downconverter images. Reducing RF gain (adding attenuation) is the most effective fix — 3rd-order products drop 3:1 (i.e. -6 dB input = -18 dB on ghosts).

### External LNA
The RX888 is **ADC-dynamic-range-limited**, not noise-figure-limited. An LNA amplifies everything including interference, stealing dynamic range with net-zero SNR improvement — **unless** FM/MW bandpass filters are placed before the LNA to remove the dominant interferers first.

---

## ExtIO_sddc (SoapySDDC driver source)

**Location:** `~/src/ExtIO_sddc` (fork of cozycactus/ExtIO_sddc)
**MacPorts build dir:** `~/src/ExtIO_sddc/build_macports`

Must be on or past commit `331b35c` ("Add ADC frequency control #240") for the `adc_frequency` setting to appear in rx888_source. If the ADC clock slider is missing:
```bash
cd ~/src/ExtIO_sddc
git pull --rebase origin master
# Skip .github/ conflicts: git rebase --skip
# Settings.cpp conflict: keep HEAD side
```

Then rebuild via `deploy_macos_testing.sh` (it auto-rebuilds SoapySDDC if source is newer).

---

## Spectrum analysis rate limiter

`spectrumHandler` runs at 20 Hz regardless of sample rate (constant `SPEC_ANALYSIS_HZ`). At 64 MHz sample rate, the old code ran ~7,800 FFTs/sec which saturated a CPU core and created backpressure on the DSP Splitter chain, starving the waterfall's FFT thread and causing visual choppiness. The rate limiter returns early from most callbacks, letting the Handler sink flush the buffer immediately so the Splitter never blocks.

---

## Known issues and fixes applied

| Issue | Root cause | Fix |
|-------|-----------|-----|
| Crash on reopen without power-cycle | Stale FX3 firmware returns garbage USB descriptors → invalid UTF-8 in config → `dump_escaped()` throws in auto-save thread | `sanitizeLabel()` strips non-printable/non-ASCII; `config.cpp` catches dump exceptions |
| Waterfall choppiness at high SR | Spectrum analysis running at full FFT rate (7800/sec at 64 MHz) saturated CPU | Rate-limited to 20 Hz via `SPEC_ANALYSIS_HZ` |
| App broken after plugin copy | macOS code signature invalidated | `deploy_macos_testing.sh` re-signs individual dylibs + deep-signs the bundle |
| Module missing on other Mac | Plugins had hardcoded `/opt/local/` and `/opt/homebrew/` dylib paths | Deploy script rewrites to `/usr/lib/` (system) and `@rpath` (bundled) |
| App doesn't work on new Mac | Missing quarantine strip | `xattr -cr` strips quarantine |
| SoapySDDC not found | `SOAPY_SDR_PLUGIN_PATH` only works from Finder | `loadSoapySDDC()` uses `dladdr()` + `SoapySDR::loadModule()` for explicit loading |
| Module not in SDR++ menu | dylib present but not in config.json | Must add to `moduleInstances` in `config.json` |
| Windows M4A console flash | `system()` call opens visible cmd.exe | Switched to `CreateProcess` with `CREATE_NO_WINDOW` |
| RF Gain label wrong | UI said "0=max" but 0 is minimum gain | Changed label to just "{name} Gain" |
| ADC clock slider too restrictive | Minimum was 50 MHz, user needed 32 MHz | Lowered minimum to 16 MHz |
| False ADS-B decodes on 133.334 | 1-bit error correction creates ~26M CRC attempts → ~1.6 expected false positives | Statistical artifact, not real signals |

---

## In progress

- **HFDL native decoder** — liquid-dsp vendored and building (Phase 1 done). Next: DSP chain (AGC → RRC → Gardner symbol sync → Costas loop → LMS equalizer → PSK demod), deinterleaver, Viterbi K=7 rate 1/2, protocol parser (SPDU/MPDU/LPDU/HFNPDU), integrate with libacars.
- **Watchlist feature** — keyword-based filtered message lists in vdl2_decoder.
- **Deploy script sync** — worktree on `claude/laughing-carson` is behind master; builds currently done from main build dir.
- **133.334 MHz mystery signal** — unidentified digital signal in VHF airband near southern CT/NY TRACON. Not VDL2 (0% header decode), not ADS-B (only statistical false positives from error correction). Sounds like ADS-B audio but remains unidentified.
