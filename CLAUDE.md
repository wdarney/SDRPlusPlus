# SDR++ Custom Build — Project Notes

## Active branch
`claude/laughing-carson` — all custom modules live here.

## Key modules
- `misc_modules/channel_bank` — wideband channel scanner with M4A encoding + SNR tagging
- `source_modules/rx888_source` — RX888 MkII source via SoapySDDC

---

## macOS testing app

**App:** `/Applications/SDR++MODULETESTING.app`

**Deploy script** (run from repo root):
```bash
./deploy_macos_testing.sh
```
This rebuilds channel_bank + rx888_source, redeploys SoapySDDC if source changed, signs everything, and re-signs the bundle.

### How the macOS RX888 stack works
1. `rx888_source.dylib` links against MacPorts SoapySDR (`/opt/local/lib/libSoapySDR.0.8.dylib`)
2. `Info.plist` sets `SOAPY_SDR_PLUGIN_PATH` → `Contents/SoapySDR/modules0.8/` via `LSEnvironment`
3. That directory contains `libSDDCSupport.so` — the SoapySDDC driver built from `~/src/ExtIO_sddc` against MacPorts SoapySDR
4. SoapySDDC has the RX888 firmware **compiled in** as a C array (no external `.img` file needed)
5. `libSoapySDR.0.8.dylib` is bundled in `Contents/Frameworks/`

**IMPORTANT:** Always launch from **Finder/Dock** (not terminal) so `LSEnvironment` takes effect.

**IMPORTANT:** After any SoapySDDC driver change, **power-cycle the RX888** (unplug/replug). The FX3 USB chip holds firmware in RAM — without a power cycle it keeps running the old firmware regardless of what driver loads.

### ExtIO_sddc source
Located at `~/src/ExtIO_sddc` (fork of cozycactus/ExtIO_sddc).
MacPorts build dir: `~/src/ExtIO_sddc/build_macports`

The local clone must be on or past commit `331b35c` ("Add ADC frequency control #240") for the ADC clock slider to appear in rx888_source. If the slider is missing after a rebuild, pull upstream:
```bash
cd ~/src/ExtIO_sddc
git pull --rebase origin master
# resolve any conflicts in .github/ by skipping (git rebase --skip)
# resolve SoapySDDC/Settings.cpp conflict by keeping HEAD side
```

---

## Windows build (VM)

**SSH:** `sshpass -p 'Nvidia!234' ssh willdarney@10.211.55.42`
**Source:** `C:\sdrplusplus` (tracks `claude/laughing-carson`)
**Build:** `C:\SDRPlusPlus\build`
**Root:** `C:\SDRPlusPlus\root`
**Package:** `C:\SDRPlusPlus\sdrpp_windows_x64.zip`

Rebuild + repackage workflow:
```powershell
# On Windows VM (via SSH):
cd C:\sdrplusplus && git pull origin claude/laughing-carson

# Build specific module:
powershell -ExecutionPolicy Bypass -Command "cd C:\SDRPlusPlus\build; cmake --build . --target channel_bank --config Release"

# Repackage:
Remove-Item -Force -ErrorAction SilentlyContinue C:\SDRPlusPlus\sdrpp_windows_x64.zip
Remove-Item -Force -Recurse -ErrorAction SilentlyContinue C:\SDRPlusPlus\sdrpp_windows_x64
powershell -ExecutionPolicy Bypass -Command "cd C:\SDRPlusPlus; & .\make_windows_package.ps1 'C:\SDRPlusPlus\build' 'C:\SDRPlusPlus\root'"
```

---

## channel_bank key settings (RX888 @ airband)
- **ADC Clock:** 32 MHz (clean 4:1 ratio with 8 MHz sample rate)
- **Sample Rate:** 8 MHz
- **RF Gain:** start low (~5–10), 0 = minimum gain (NOT maximum — label was wrong in old versions)
- **LNA:** don't use one; RX888 is ADC-dynamic-range-limited, not noise-figure-limited
- **Filter:** add a 118–137 MHz bandpass filter if possible — protects dynamic range from 5G/FM/etc.
- **Power-cycle RX888** after driver updates

## Spectrum analysis rate limiter
`spectrumHandler` runs at 20 Hz regardless of sample rate (constant `SPEC_ANALYSIS_HZ`).
At 64 MHz sample rate the old code ran ~7,800 FFTs/sec which saturated a CPU core and stalled the Splitter → waterfall choppiness. The rate limiter fixes this.
