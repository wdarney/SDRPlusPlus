# Brown DSD SDR++ Test Integration Notes

Date: 2026-08-09

## Scope

This work was done in the isolated BrownDSD test checkout and app:

- Source checkout: `/Users/willdarney/Documents/SDR++/SDRPlusPlus-brown-dsd`
- Test app: `/Applications/SDR++MODULETESTING-BrownDSD.app`
- Test root/profile: `/Users/willdarney/Library/Application Support/sdrpp-browndsd-test`

The main module-testing app was kept separate:

- Main app: `/Applications/SDR++MODULETESTING.app`

## Current Result

Brown DSD now produces decoded audio for:

- P25 Phase 1
- DMR

The DMR fix was confirmed live after the DSD panel showed DMR voice syncs but no MBE frames, then after patching the stage-1 progress handling, DMR audio became audible.

## Integrated Pieces

Imported Brown SDR++ module:

- `decoder_modules/ch_extravhf_decoder`

Added Radio module interface support:

- `decoder_modules/radio/src/radio_module_interface.h`
- `core/src/module.h`
- `decoder_modules/radio/src/radio_module.h`

The Brown DSD module uses the Radio module interface to inject DSD demodulator support into Radio instances.

## Important Runtime Notes

The BrownDSD app uses the same known-good core as the main module-testing app:

- `/Applications/SDR++MODULETESTING-BrownDSD.app/Contents/Frameworks/libsdrpp_core.dylib`
- `/Applications/SDR++MODULETESTING.app/Contents/Frameworks/libsdrpp_core.dylib`

The deployed core hashes were verified equal after plugin updates.

The BrownDSD plugin was installed here:

- `/Applications/SDR++MODULETESTING-BrownDSD.app/Contents/Plugins/ch_extravhf_decoder.dylib`

After each plugin deployment, the plugin and app were ad-hoc signed and the app bundle verified.

The BrownDSD test profile was refreshed from the main SDR++ profile on 2026-08-09:

- Source profile: `/Users/willdarney/Library/Application Support/sdrpp`
- BrownDSD test profile: `/Users/willdarney/Library/Application Support/sdrpp-browndsd-test`
- Backup made before refresh: `/Users/willdarney/Library/Application Support/sdrpp-browndsd-test.before-main-config-20260809-143223`

The refresh copied the main app's module/source/sink settings into the BrownDSD test profile, then preserved the Brown DSD module registration:

- `Brown DSD`: `ch_extravhf_decoder`, enabled
- `ch_extravhf_decoder_config.json`

This keeps the original SDR++ profile protected while making the BrownDSD app start with the same practical configuration as the main app.

Later on 2026-08-09, the BrownDSD launcher was changed to use the original SDR++ profile directly:

- Shared profile now used by BrownDSD: `/Users/willdarney/Library/Application Support/sdrpp`
- Launcher source: `tools/browndsd_launcher.c`
- BrownDSD app launcher: `/Applications/SDR++MODULETESTING-BrownDSD.app/Contents/MacOS/sdrpp`
- Old launcher backup moved outside the app bundle: `/Users/willdarney/Documents/SDR++/PluginBackups/SDR++MODULETESTING-BrownDSD`
- Original config backup: `/Users/willdarney/Library/Application Support/sdrpp/config.json.codex-bak-before-browndsd-shared-20260809-151549`

The original config was updated to enable:

- `Brown DSD`: `ch_extravhf_decoder`

The Brown DSD module config was also copied into the original profile:

- `/Users/willdarney/Library/Application Support/sdrpp/ch_extravhf_decoder_config.json`

## Repeatable Build/Config Setup

BrownDSD is now hard to accidentally omit in future sessions:

- `OPT_BUILD_CH_EXTRAVHF_DECODER` defaults to `ON` in this branch.
- `CMakePresets.json` includes a `browndsd-macos` preset that forces the Brown DSD module on for fresh or reused build folders.
- `tools/enable_browndsd_original_config.py` enables `Brown DSD` in the original SDR++ profile and creates a config backup before writing.

Useful commands:

```sh
cmake --preset browndsd-macos
cmake --build --preset browndsd-macos
python3 tools/enable_browndsd_original_config.py
```

## Fixed Issues

### Module Not Showing In Radio Menu

The Brown DSD module required config/module registration and Radio interface wiring before `DSD` appeared in the Radio demodulator list.

### Startup/Radio Menu Crash

Radio could crash when a saved demodulator selection no longer matched the current demodulator map. The Radio module was patched to guard invalid selected demod IDs and fall back safely.

### `oldDSD` Crash/Lock

`oldDSD` was unstable in this integration. It is hidden/mapped away for now:

- `RADIO_DEMOD_OLDDSD` maps to `RADIO_DEMOD_DSD`
- `oldDSD` is not exposed in the Radio mode list

### DSD Lockup With Multiple VFOs

The DSD decoder could lock the SDR++ stream when it entered a no-progress loop. A stall guard was added in the new DSD decoder path so a repeated no-progress state resets sync and consumes one dibit instead of hanging.

### DMR No Audio

Symptoms:

- DMR mode detected
- `VOICE Header` displayed
- `DMR voice syncs` counted upward
- `DMR MBE frames` stayed at `0`
- No DMR audio
- P25 audio worked

Diagnostic counters showed:

- `DMR stage hits: 37 37 0 0 ...`
- DMR voice state machine reached stage 1, then reset before stage 2

Root cause:

The stall guard only looked at live input consumption. DMR stage 1 advances by reading already-buffered dibits from the pre-sync buffer, so it made real internal progress without consuming new live input. The guard treated that as no progress and reset the decoder before AMBE frames were assembled.

Fix:

- Added `internalProgress` tracking in `dsp::NewDSD`
- Incremented it when DMR voice stages consume buffered dibits
- Updated the stall guard to allow either live input progress, state progress, or internal buffered progress

Also fixed DMR sync buffer indexing in stage 5:

- Changed per-chunk indexing from `dmrv_sync[i]` to `dmrv_sync[dmrv_ctr]`
- Filled `dmrv_syncdata[dmrv_ctr]` consistently

After this, DMR audio became audible.

## Temporary Diagnostics Added

The DSD menu currently includes:

- `Audio probe tone`
- `DMR voice syncs`
- `DMR MBE frames`
- `DMR muted slots`
- `DMR voice stage`
- `DMR stage hits`

The audio probe tone proves the DSD demodulator output is reaching the SDR++ audio sink. It should be removed or hidden before a clean production-style build.

The DMR counters are useful while testing but can also be removed or placed behind a debug option later.

## Test Checklist

1. Launch `/Applications/SDR++MODULETESTING-BrownDSD.app`.
2. Select the RX888 source and start streaming.
3. Select `DSD` under the Radio module.
4. Confirm `Audio probe tone` produces a tone when checked.
5. Tune a known P25 Phase 1 signal and confirm decoded audio.
6. Tune a known DMR signal.
7. Confirm `DMR voice syncs` increases.
8. Confirm `DMR MBE frames` increases during voice activity.
9. Confirm DMR audio is audible.
10. Test one VFO and two VFOs to make sure SDR++ does not lock.

## Follow-Up Cleanup

Before promoting this beyond the test app:

- Decide whether to keep the Radio module interface changes as the long-term integration path.
- Remove or hide the `Audio probe tone` UI.
- Remove or gate DMR diagnostic counters.
- Re-check the source-level `core/src/signal_path/sink.cpp` experiment before merging anything core-related.
- Keep `oldDSD` hidden unless separately repaired.
- Run a clean build and deploy only the intended plugin/app artifacts.
