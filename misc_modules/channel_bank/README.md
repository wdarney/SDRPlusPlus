# Channel Bank Module for SDR++

A multi-channel auto-demodulator and scanner module for SDR++. Automatically detects signals across the SDR bandwidth, spawns VFOs, demodulates, records transmissions to WAV, and plays them back through a monitor queue.

## Features

### Signal Detection & Demodulation
- **FFT-based auto-detection** — continuously monitors the full SDR bandwidth for signals above a configurable SNR threshold
- **Multi-channel** — spawns multiple simultaneous VFOs for parallel demodulation and recording
- **AM, USB, LSB, NFM, WFM demodulation** — matched to SDR++ radio module quality
- **BFO trim** — per-channel fine-tuning offset for SSB signals (DragInt for precise 1 Hz control)
- **Configurable channel spacing** — 5, 6.25, 8.33, 12.5, 25, or 50 kHz

### Operating Modes
- **Auto Detect** — FFT squelch automatically finds and tracks signals across the current SDR bandwidth
- **Manual Channels** — specify exact frequencies for VFO placement, with import from the Frequency Manager
- **Scan Mode** — define frequency ranges (start/stop MHz), automatically sweeps through them using auto-detection at each stop

### Scan Mode
- Define multiple scan ranges with automatic stop computation based on sample rate
- **Quiet Timeout** — configurable delay after a transmission ends before advancing (1-30s)
- **No Signal Skip** — faster skip when no transmission is detected at a stop (0.1-5s)
- Automatic wrap-around through all scan stops

### Recording & Playback
- **Automatic WAV recording** — captures transmissions to timestamped files
- **Monitor queue** — plays back recordings through the audio output after capture
- **Configurable recording gain, minimum TX duration, and tail length**
- **Silence-based splitting** — separate recordings per transmission

### Frequency History & Blocking
- **Frequency log** — tracks every detected frequency with hit count and last-seen time
- **50 MHz band grouping** — collapsible sections keep the history organized (e.g., "100-150 MHz (12)")
- **Block/unblock frequencies** — from the history list or directly from the active channel view
- **Blocked frequencies are immediately torn down** — stops recording as soon as you block
- **Clear options** — "Clear All" or "Clear (keep blocked)" to preserve your blocklist

### Visualization
- **Mini spectrum display** — shows power levels, threshold line, and detected signal markers in the module panel

## Building

The module builds as part of SDR++. Add it to the CMake build:

```
option(OPT_BUILD_CHANNEL_BANK "Build the channel bank auto-demodulator" ON)
```

The compiled module is a `.dylib` (macOS), `.dll` (Windows), or `.so` (Linux) placed in the SDR++ plugins directory. Register it in `config.json` under the `"modules"` section.
