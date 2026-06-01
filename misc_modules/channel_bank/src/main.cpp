#include <imgui.h>
#include <module.h>
#include <frequency_manager_interface.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <dsp/channel/rx_vfo.h>
#include <dsp/channel/frequency_xlator.h>
#include <dsp/multirate/power_decimator.h>
#include <dsp/demod/am.h>
#include <dsp/demod/fm.h>
#include <dsp/demod/ssb.h>
#include <dsp/routing/splitter.h>
#include <dsp/bench/peak_level_meter.h>
#include <dsp/sink/handler_sink.h>
#include <signal_path/signal_path.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/widgets/folder_select.h>
#include <config.h>
#include <core.h>
#include <utils/wav.h>
#include <fftw3.h>
#include <rnnoise.h>
#include <chrono>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <regex>
#include <queue>
#include <deque>
#include <fstream>
#ifdef __APPLE__
#include "transcription.h"
#include "transcription_whisper.h"
#include "encoding.h"
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:        */ "channel_bank",
    /* Description: */ "SNR-triggered multi-channel auto-demodulator and recorder",
    /* Author:      */ "SDR++ Contributors",
    /* Version:     */ 0, 2, 0,
    /* Max instances*/ -1
};

ConfigManager config;

class ChannelBankModule;

struct ChannelSlot {
    int    gridIdx = 0;
    double freqHz  = 0.0;
    std::string streamName;

    ChannelBankModule* module = nullptr;

    // DSP chain
    dsp::stream<dsp::complex_t>*               iqIn           = nullptr;
    dsp::channel::RxVFO*                       vfo            = nullptr;
    dsp::demod::AM<dsp::stereo_t>*             amDemod        = nullptr;
    dsp::demod::FM<dsp::stereo_t>*             fmDemod        = nullptr;
    dsp::demod::SSB<dsp::stereo_t>*            ssbDemod       = nullptr;
    dsp::routing::Splitter<dsp::stereo_t>*     splitter       = nullptr;

    // Meter branch
    dsp::stream<dsp::stereo_t>                 meterStream;
    dsp::bench::PeakLevelMeter<dsp::stereo_t>* meter          = nullptr;

    // Recorder branch
    dsp::stream<dsp::stereo_t>*                recFeedStream  = nullptr;
    dsp::sink::Handler<dsp::stereo_t>*         recSink        = nullptr;
    wav::Writer                                writer;
    std::string                                currentFilePath;
    bool    fileOpen        = false;
    bool    inSilence       = false;
    std::chrono::steady_clock::time_point      silenceStart;
    std::chrono::steady_clock::time_point      fileOpenTime;
    int64_t audioSamplesWritten = 0;  // exact sample count — used for min TX duration check
    int     recFadeRemaining = 0;   // samples left in recording fade-in
    int     warmupSamples      = 9600; // discard first 200ms so AGC settles before recording
    bool    warmupSignalLost   = false; // if signal drops during warmup, restart the clock
    int     fileTrimSamples    = 0;    // discard first 200ms after file open before writing

    // Lifecycle tracking
    std::chrono::steady_clock::time_point      lastDetected;  // last time FFT saw signal here
    std::atomic<bool>                          signalPresent    { false };
    std::atomic<bool>                          rawSignalPresent { false }; // above SNR threshold RIGHT NOW (no vote smoothing, no hold)
    std::atomic<int>                           rawConsecutiveHits { 0 };  // consecutive FFT frames rawSignalPresent was true; gated file-open requires ≥2
    bool                                       prevSignalPresent = false;  // rising-edge detect for watch alert

    // Sample-accurate fade-out driven by rawSignalPresent (DSP thread only — no locking needed)
    int fadeOutRemaining  = 2400; // counts down from 50ms-worth of samples; reset to max while signal present
    int audioHoldRemaining = 0;   // short independent audio-hold (200ms) before fade starts;
                                  // decoupled from signalHoldMs so AM AGC ramp doesn't bleed into recording

    // Pre-roll circular buffer — always running once warmup is done (DSP thread only).
    // When a file opens we flush the last PREROLL_SAMPLES of audio first so we
    // capture the start of the transmission that occurred before detection fired.
    static constexpr int PREROLL_SAMPLES = 19200;  // 400ms @ 48kHz
    std::vector<float> preRollBuf  = std::vector<float>(PREROLL_SAMPLES, 0.0f);
    std::vector<float> preRollTmp  = std::vector<float>(PREROLL_SAMPLES, 0.0f);  // scratch for flush
    int  preRollHead  = 0;   // next write position (wraps mod PREROLL_SAMPLES)
    int  preRollCount = 0;   // valid samples currently in buffer (0..PREROLL_SAMPLES)

#ifdef __APPLE__
    void*       transcribeHandle       = nullptr;
    int         transcribeBackend      = 0;  // mirrors TranscriptionBackend; tracks which lib owns transcribeHandle
    std::string liveTranscript;
    // Time-aligned segments from the last completed transcription (Whisper only;
    // empty for Apple Speech).  Drives the synced display during playback and
    // the LRC ©lyr tag embedded in the M4A.  Lives on the slot until playback
    // captures it for display.
    std::vector<transcription_whisper::Segment> liveSegments;
    std::string pendingTranscriptPath;
#endif

    // SNR accumulation — written by management thread (under channelsMtx),
    // read by audioHandler at file-close time (intentional benign race; both
    // are at most one 250ms sample apart and the values are used for metadata only).
    float snrSum   = 0.0f;  // sum of SNR dB samples taken while recording + signal present
    int   snrCount = 0;     // number of samples in snrSum

    // Static-vs-voice gate — spectral-flatness tally. Written by analyzeSpectrum
    // (DSP spectrum thread, under channelsMtx) on every above-threshold frame while
    // the file is open; read by audioHandler at file-close time. Atomic so the
    // close-time read is clean across threads. A recording whose channel spectrum
    // is predominantly *flat* (broadband static, no carrier) instead of *peaky*
    // (carrier + voice sidebands) is discarded before it queues or logs.
    std::atomic<int> gateFramesAbove { 0 };  // frames above threshold while recording (auto only — static/drift gate)
    std::atomic<int> gateFramesVoice { 0 };  // ...of those, frames with a carrier (low flatness)

    // On-air time tally — frames the carrier was actually present (above the hold threshold)
    // while recording, counted in BOTH auto and manual modes. Min TX is checked against this
    // cumulative on-air time rather than the open→last-seen span, so intermittent data bursts
    // (ACARS, etc.) that get bridged into one long span are still discarded for having little
    // real airtime. At SPEC_ANALYSIS_HZ (20 Hz) each frame is 50 ms.
    std::atomic<int> onAirFrames { 0 };

    // Drift gate — running stats of the carrier centroid (relative to slot center, Hz)
    // over the above-threshold frames of this recording. A healthy transmitter holds a
    // fixed carrier (small stddev); a faulty/drifting emitter sweeps in frequency (large
    // stddev → diagonal streaks in the waterfall). Plain doubles, written by analyzeSpectrum
    // under channelsMtx, read once at close (benign one-frame race, like snrSum).
    double driftSum   = 0.0;  // Σ centroidHz
    double driftSumSq = 0.0;  // Σ centroidHz²

    // RNNoise state (per-slot noise reduction)
    DenoiseState*  nrState    = nullptr;
    float          nrInBuf[480] = {};     // RNNoise processes 480 samples (10ms at 48kHz)
    int            nrInPos    = 0;
};

class ChannelBankModule : public ModuleManager::Instance {
public:
    enum DemodMode { DEMOD_AM = 0, DEMOD_NFM = 1, DEMOD_WFM = 2, DEMOD_USB = 3, DEMOD_LSB = 4 };

    static constexpr double SPACINGS[] = {
        8333.0, 12500.0, 25000.0, 50000.0, 100000.0, 200000.0
    };
    static constexpr int FFT_SIZE    = 8192;
    static constexpr int    SPAWN_VOTES      = 3;    // FFT frames above threshold before spawning
    static constexpr int    MAX_VOTES        = 8;    // vote cap (controls how fast channel drops out)
    static constexpr double SPEC_ANALYSIS_HZ = 20.0; // target spectrum analysis rate (Hz)

    ChannelBankModule(std::string name) : folderSelect("%ROOT%/recordings") {
        this->name = name;
        root = (std::string)core::args["root"];

        config.acquire();
        if (config.conf[name].contains("spacingId"))
            spacingId = config.conf[name]["spacingId"];
        if (config.conf[name].contains("demodMode"))
            demodMode = config.conf[name]["demodMode"];
        if (config.conf[name].contains("ssbBfoHz"))
            ssbBfoHz = config.conf[name]["ssbBfoHz"];
        if (config.conf[name].contains("snrThreshold"))
            snrThreshold = config.conf[name]["snrThreshold"];
        if (config.conf[name].contains("holdHysteresisDb"))
            holdHysteresisDb = config.conf[name]["holdHysteresisDb"];
        if (config.conf[name].contains("maxRecordingSec"))
            maxRecordingSec = config.conf[name]["maxRecordingSec"];
        if (config.conf[name].contains("staticGateEnabled"))
            staticGateEnabled = config.conf[name]["staticGateEnabled"];
        if (config.conf[name].contains("staticGateFlatness"))
            staticGateFlatness = config.conf[name]["staticGateFlatness"];
        if (config.conf[name].contains("staticGateVoiceFrac"))
            staticGateVoiceFrac = config.conf[name]["staticGateVoiceFrac"];
        if (config.conf[name].contains("driftGateEnabled"))
            driftGateEnabled = config.conf[name]["driftGateEnabled"];
        if (config.conf[name].contains("driftMaxStdHz"))
            driftMaxStdHz = config.conf[name]["driftMaxStdHz"];
        if (config.conf[name].contains("cooldownSec"))
            cooldownSec = config.conf[name]["cooldownSec"];
        if (config.conf[name].contains("recGain"))
            recGain = config.conf[name]["recGain"];
        if (config.conf[name].contains("minTransmissionMs"))
            minTransmissionMs = config.conf[name]["minTransmissionMs"];
        if (config.conf[name].contains("tailMs"))
            tailMs = config.conf[name]["tailMs"];
        if (config.conf[name].contains("maxChannels"))
            maxChannels = config.conf[name]["maxChannels"];
        if (config.conf[name].contains("bwUsage"))
            bwUsage = config.conf[name]["bwUsage"];
        if (config.conf[name].contains("noiseReduction"))
            noiseReduction = config.conf[name]["noiseReduction"];
        if (config.conf[name].contains("nrMix"))
            nrMix = config.conf[name]["nrMix"];
        if (config.conf[name].contains("recPath"))
            folderSelect.setPath(config.conf[name]["recPath"]);
        if (config.conf[name].contains("freqLog")) {
            for (auto& j : config.conf[name]["freqLog"]) {
                double hz  = j.value("freq", 0.0);
                FreqEntry e;
                e.freqHz  = hz;
                e.count       = j.value("count", 0);
                e.blocked     = j.value("blocked", false);
                e.lastSeen    = j.value("lastSeen", (int64_t)0);
                e.description = j.value("description", std::string());
                // One-time cleanup of legacy touchFreqLog() junk: drop entries that never
                // produced a kept recording (count==0), aren't blocked, and have no
                // description. These are leftover open-time entries from interference;
                // real history (count>0), blocks, and named entries are preserved.
                if (e.count == 0 && !e.blocked && e.description.empty()) continue;
                freqLog[freqKey(hz)] = e;
            }
        }
        if (config.conf[name].contains("manualMode"))
            manualMode = config.conf[name]["manualMode"];
        if (config.conf[name].contains("bookmarkScanMode"))
            bookmarkScanMode = config.conf[name]["bookmarkScanMode"];
        if (config.conf[name].contains("manualFrequencies"))
            for (auto& j : config.conf[name]["manualFrequencies"])
                manualFrequencies.push_back(j.get<double>());
        if (config.conf[name].contains("boundBookmarkLists")) {
            for (auto& j : config.conf[name]["boundBookmarkLists"])
                boundBookmarkLists.insert(j.get<std::string>());
        }
        else if (config.conf[name].contains("boundBookmarkList")) {
            // Migrate legacy single-list config
            std::string s = config.conf[name]["boundBookmarkList"].get<std::string>();
            if (!s.empty()) boundBookmarkLists.insert(s);
        }
        if (config.conf[name].contains("watchedFreqs"))
            for (auto& j : config.conf[name]["watchedFreqs"])
                watchedFreqs.insert(j.get<int64_t>());
        if (config.conf[name].contains("signalHoldMs"))
            signalHoldMs = config.conf[name]["signalHoldMs"];
        if (config.conf[name].contains("leftTrimFrac"))
            leftTrimFrac  = config.conf[name]["leftTrimFrac"];
        if (config.conf[name].contains("rightTrimFrac"))
            rightTrimFrac = config.conf[name]["rightTrimFrac"];
        if (config.conf[name].contains("recordingEnabled"))
            recordingEnabled = config.conf[name]["recordingEnabled"];
        // Transcription backend: prefer the new enum key, fall back to the legacy
        // boolean so existing configs keep working (true → Apple Speech, false → Off).
        if (config.conf[name].contains("transcriptionBackend")) {
            transcriptionBackend = config.conf[name]["transcriptionBackend"].get<int>();
        }
        else if (config.conf[name].contains("transcriptionEnabled")) {
            transcriptionBackend = config.conf[name]["transcriptionEnabled"].get<bool>()
                                   ? TB_APPLE_SPEECH : TB_OFF;
        }
        if (config.conf[name].contains("m4aEnabled"))
            m4aEnabled = config.conf[name]["m4aEnabled"];
        if (config.conf[name].contains("scanMode"))
            scanMode = config.conf[name]["scanMode"];
        if (config.conf[name].contains("scanQuietSec"))
            scanQuietSec = config.conf[name]["scanQuietSec"];
        if (config.conf[name].contains("scanNoSignalSec"))
            scanNoSignalSec = config.conf[name]["scanNoSignalSec"];
        if (config.conf[name].contains("scanRanges"))
            for (auto& j : config.conf[name]["scanRanges"])
                scanRanges.push_back({ j.value("start", 0.0), j.value("stop", 0.0) });
        config.release();

        channelSpacing = SPACINGS[std::clamp(spacingId, 0, 5)];

        // Load FM bookmarks so displayName() can show names
        loadFMConfig();

        // Populate cached freqs from all bound lists
        rebuildBoundFreqs();

        // Allocate FFTW buffers
        fftIn  = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * FFT_SIZE);
        fftOut = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * FFT_SIZE);
        fftPlan = fftwf_plan_dft_1d(FFT_SIZE, fftIn, fftOut, FFTW_FORWARD, FFTW_ESTIMATE);

        // Precompute 4-term Blackman-Harris window.
        // Sidelobes: -92 dB vs Hann's -31.5 dB.  For a 40 dB strong signal,
        // Hann sidelobes sit at 8.5 dB above noise (above any detection threshold);
        // Blackman-Harris sidelobes sit at -52 dB — completely invisible.
        // This prevents strong SELCAL / HFDL carriers from leaking into adjacent
        // bookmark detection windows and showing wide spikes in the mini-spectrum.
        hannWindow.resize(FFT_SIZE);
        for (int i = 0; i < FFT_SIZE; i++) {
            float phi = 2.0f * M_PI * i / (float)(FFT_SIZE - 1);
            hannWindow[i] = 0.35875f
                          - 0.48829f * cosf(phi)
                          + 0.14128f * cosf(2.0f * phi)
                          - 0.01168f * cosf(3.0f * phi);
        }
        fftAccum.resize(FFT_SIZE);
        fftBufPos = 0;

        retuneHandler.ctx     = this;
        retuneHandler.handler = retuneHandlerFunc;
        sigpath::sourceManager.onRetune.bindHandler(&retuneHandler);

        gui::menu.registerEntry(name, menuHandler, this);
    }

    ~ChannelBankModule() {
        gui::menu.removeEntry(name);
        sigpath::sourceManager.onRetune.unbindHandler(&retuneHandler);
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        restoreWaterfallVisibility();  // always restore on unload, safe no-op if not saved
        if (running) { stop(); }
#ifdef __APPLE__
        // Drain any cached Whisper model contexts BEFORE static destructors run.
        // ggml-metal's residency-set teardown asserts that all Metal buffers
        // have been freed (rsets->data count == 0) and waits for its async
        // keep-alive worker to stop.  whisper_free() does both correctly; the
        // static-destructor path (which fires from libc's __cxa_finalize_ranges
        // at process exit) does not, and crashes with SIGABRT in ggml_abort.
        // Calling shutdown() here gives whisper.cpp a clean teardown window.
        transcription_whisper::shutdown();
#endif
        fftwf_destroy_plan(fftPlan);
        fftwf_free(fftIn);
        fftwf_free(fftOut);
    }

    void postInit() {
        // Hook the main waterfall's FFT-redraw event so we can draw markers
        // at each currently-active channel's frequency.
        fftRedrawHandler.handler = fftRedrawHandlerFunc;
        fftRedrawHandler.ctx     = this;
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
    }
    void enable()  { enabled = true; }
    void disable() { enabled = false; restoreWaterfallVisibility(); }
    bool isEnabled() { return enabled; }

    void start() {
        std::lock_guard<std::mutex> lck(runMtx);
        if (running) { return; }

        lastKnownSr     = sigpath::iqFrontEnd.getSampleRate();
        lastKnownCenter = gui::waterfall.getCenterFrequency();
        fftBufPos = 0;
        specSamplesUntilFFT = 0;    // trigger first spectrum analysis immediately
        avgPower.clear();           // reset spectrum averaging on start
        instPower.clear();
        rawSlotMisses.clear();
        rawManualMisses.clear();
        globalNoiseFloor = 0.0f;
        { std::lock_guard<std::mutex> lck(channelsMtx); recentChannels.clear(); }

        if (scanMode) {
            computeScanStops();
            scanStopIdx        = 0;
            scanStopHadSignal  = false;
            lastSignalTime     = std::chrono::steady_clock::now();
            if (!scanStops.empty()) {
                gui::waterfall.setCenterFrequency(scanStops[0]);
                gui::waterfall.centerFreqMoved = true;
            }
        }

        if (bookmarkScanMode) {
            computeBookmarkScanStops();
            bookmarkScanStopIdx   = 0;
            bookmarkScanHadSignal = false;
            lastSignalTime        = std::chrono::steady_clock::now();
            if (!bookmarkScanStops.empty()) {
                gui::waterfall.setCenterFrequency(bookmarkScanStops[0].centerHz);
                gui::waterfall.centerFreqMoved = true;
            }
        }

        // Create one shared IQ binding — all consumers fan out from this splitter
        // so the main signal-path thread only copies one buffer.
        sharedIqIn = new dsp::stream<dsp::complex_t>();
        sigpath::iqFrontEnd.bindIQStream(sharedIqIn);
        iqSplitter = new dsp::routing::Splitter<dsp::complex_t>(sharedIqIn);
        iqSplitter->start();

        // Bind spectrum monitor stream to our splitter (not directly to the frontend)
        specStream = new dsp::stream<dsp::complex_t>();
        iqSplitter->bindStream(specStream);
        specSink = new dsp::sink::Handler<dsp::complex_t>(specStream, spectrumHandler, this);
        specSink->start();

        // Start management thread
        mgmtRunning = true;
        mgmtThread = std::thread(&ChannelBankModule::managementThreadFunc, this);

        // Start encode thread (WAV → M4A after playback+transcription complete)
        encodeThreadRunning = true;
        encodeThread = std::thread(&ChannelBankModule::encodeThreadFunc, this);

        // Register monitor audio output (single stream for sequential playback)
        monitorSrHandler.ctx     = this;
        monitorSrHandler.handler = [](float, void*) {};
        monitorSinkStream = new SinkManager::Stream();
        monitorSinkStream->init(&monitorStream, &monitorSrHandler, 48000.0f);
        sigpath::sinkManager.registerStream(name + "_monitor", monitorSinkStream);
        monitorSinkStream->start();

        // Start playback thread
        playbackRunning = true;
        playbackThread  = std::thread(&ChannelBankModule::playbackThreadFunc, this);

        running = true;
    }

    void stop() {
        std::lock_guard<std::mutex> lck(runMtx);
        if (!running) { return; }
        running = false;

        // Restore FM waterfall visibility before tearing down
        if (manualMode) restoreWaterfallVisibility();

        // Stop playback thread — stopWriter() unblocks any pending swap() call
        playbackRunning = false;
        monitorStream.stopWriter();
        if (playbackThread.joinable()) { playbackThread.join(); }
        monitorStream.clearWriteStop();
        { std::lock_guard<std::mutex> lk(playbackMtx); playbackQueue.clear(); }
#ifdef __APPLE__
        // Drop any orphaned synced-segment entries (would otherwise leak if the
        // WAV they belonged to got discarded before playback dequeued them).
        { std::lock_guard<std::mutex> sk(pendingPlaybackSegmentsMtx); pendingPlaybackSegments.clear(); }
        playbackPosMs.store(-1);
        { std::lock_guard<std::mutex> tlk(lastTranscriptMtx); playingSegments.clear(); }
#endif

        // Tear down monitor stream
        monitorSinkStream->stop();
        sigpath::sinkManager.unregisterStream(name + "_monitor");
        delete monitorSinkStream;
        monitorSinkStream = nullptr;

        // Stop management thread and spectrum monitor so no new channels are spawned
        // or modified while we tear down.
        mgmtRunning = false;
        mgmtCv.notify_all();
        if (mgmtThread.joinable()) { mgmtThread.join(); }

        specSink->stop();
        iqSplitter->unbindStream(specStream);
        delete specSink;  specSink  = nullptr;
        delete specStream; specStream = nullptr;

        // Teardown all active channels BEFORE stopping the encode thread.
        // destroySlot() may queue open recordings for direct M4A encoding; the
        // encode thread must still be alive to accept those items.
        {
            std::lock_guard<std::mutex> clck(channelsMtx);
            for (auto& [idx, slot] : activeChannels) {
                destroySlot(*slot);
                delete slot;
            }
            activeChannels.clear();
        }

        // Now drain + stop the encode thread (picks up anything destroySlot queued above).
        encodeThreadRunning = false;
        encodeQueueCv.notify_all();
        if (encodeThread.joinable()) { encodeThread.join(); }
        { std::lock_guard<std::mutex> lk(encodeQueueMtx);   encodeQueue.clear(); }
        { std::lock_guard<std::mutex> lk(pendingEncodesMtx); pendingEncodes.clear(); }

        // Tear down shared IQ splitter — all slot streams have been unbound by destroySlot above.
        iqSplitter->stop();
        sigpath::iqFrontEnd.unbindIQStream(sharedIqIn);
        delete iqSplitter; iqSplitter = nullptr;
        delete sharedIqIn; sharedIqIn = nullptr;
    }

    // Normalize a closed INT16 mono WAV to -3 dBFS in-place.
    static void normalizeWavFile(const std::string& path) {
        flog::info("[ChannelBank] normalizeWavFile: {0}", path);
        // Read phase — separate ifstream avoids stream-state issues on write-back.
        std::vector<int16_t> samples;
        {
            std::ifstream f(path, std::ios::binary);
            if (!f) { flog::error("[ChannelBank] normalizeWavFile: failed to open for read"); return; }
            f.seekg(44);  // skip standard 44-byte PCM WAV header
            int16_t s;
            while (f.read((char*)&s, 2)) samples.push_back(s);
        }
        flog::info("[ChannelBank] normalizeWavFile: read {0} samples", (int)samples.size());
        if (samples.empty()) return;

        // Trim the first 250ms — removes residual AGC/filter transient at recording start.
        const int trimLen  = 12000;  // 250ms @ 48kHz
        int       outStart = std::min(trimLen, (int)samples.size());

        // Normalization: use the 99th-percentile absolute value as the reference
        // instead of the absolute peak.  A single static crash or PTT click sets
        // the absolute peak and anchors the scale at a low value, making the rest
        // of the recording sound quiet.  The 99th-percentile ignores the top 1% of
        // samples, so brief spikes don't drag down the overall level.
        //
        // Target: -6 dBFS at the 99th percentile.  That leaves 6 dB of headroom
        // for the top 1% of samples before they clip, which is appropriate for
        // speech recordings where occasional noise bursts exceed normal speech level.
        int postLen = (int)samples.size() - outStart;
        if (postLen <= 0) return;
        std::vector<int32_t> absVals;
        absVals.reserve(postLen);
        for (int i = outStart; i < (int)samples.size(); i++)
            absVals.push_back(std::abs((int32_t)samples[i]));
        std::sort(absVals.begin(), absVals.end());
        int p99idx = std::max(0, (int)(absVals.size() * 0.99f) - 1);
        int32_t ref99 = absVals[p99idx];
        if (ref99 < 50) return;  // silent or too short — skip

        // -6 dBFS = 32767 * 10^(-6/20) ≈ 16423
        const float target6dBFS = 16423.0f;
        float scale = target6dBFS / (float)ref99;
        // Allow up to 20x gain for very weak signals; cap hard to avoid
        // turning pure noise into a wall of sound.
        if (scale > 20.0f) scale = 20.0f;

        // Apply scale to all samples.
        for (auto& smp : samples) {
            int32_t v = std::clamp((int32_t)std::round((float)smp * scale), -32768, 32767);
            smp = (int16_t)v;
        }

        // Block-based dynamic range compression.
        // On HF a ground station can be 30-40 dB louder than an aircraft reply
        // within the same recording, so a single scale factor leaves the quiet
        // parts inaudible.  We divide the content into 500 ms blocks, estimate
        // the noise floor from the quietest blocks, and apply a smoothed gain
        // envelope that brings each active block toward a consistent RMS target.
        // A noise gate prevents amplifying silence between transmissions.
        {
            const int   BLOCK_SAMPS = 24000;    // 500 ms @ 48 kHz
            const float TARGET_RMS  = 0.12f;    // ≈ -18 dBFS — comfortable speech
            const float MAX_GAIN    = 4.0f;     // cap boost at +12 dB
            const float MIN_GAIN    = 0.33f;    // cap cut  at -10 dB
            const float GATE_RATIO  = 3.0f;     // skip blocks below 3× noise floor

            int totalOut = (int)samples.size() - outStart;
            if (totalOut > 0) {
                int numBlocks = (int)std::ceil((float)totalOut / BLOCK_SAMPS);

                // Per-block RMS from the post-trim content
                std::vector<float> blockRms(numBlocks, 0.0f);
                for (int b = 0; b < numBlocks; b++) {
                    int s0 = outStart + b * BLOCK_SAMPS;
                    int s1 = std::min(s0 + BLOCK_SAMPS, (int)samples.size());
                    float sum2 = 0.0f;
                    for (int i = s0; i < s1; i++) {
                        float f = samples[i] / 32767.0f;
                        sum2 += f * f;
                    }
                    blockRms[b] = sqrtf(sum2 / (float)(s1 - s0));
                }

                // Noise floor: 15th-percentile block RMS
                std::vector<float> sortedRms = blockRms;
                std::sort(sortedRms.begin(), sortedRms.end());
                float noiseRms   = sortedRms[std::max(0, (int)(sortedRms.size() * 0.15f) - 1)];
                float gateThresh = noiseRms * GATE_RATIO;

                // Target gain per block; gated (quiet/silence) blocks hold at 1.0
                std::vector<float> blockGain(numBlocks, 1.0f);
                for (int b = 0; b < numBlocks; b++) {
                    if (blockRms[b] > gateThresh && blockRms[b] > 1e-6f) {
                        float g = TARGET_RMS / blockRms[b];
                        blockGain[b] = std::clamp(g, MIN_GAIN, MAX_GAIN);
                    }
                }

                // Forward-lookahead smoothing: cap each block's gain to at most
                // FWD_RATIO × the following block's gain.
                //
                // Without this, a quiet pre-voice carrier window (T=250-500 ms)
                // has low RMS and receives a large boost.  The compressor then
                // applies that boosted gain right as the operator keys up (PTT
                // click / carrier onset noise), which the user hears as a brief
                // loud static burst before the gain drops to the voice level.
                // Limiting the gain increase relative to the *next* block ensures
                // the onset never gets amplified beyond what the following content
                // warrants — the transition is at most ~3.5 dB (factor 1.5) per
                // 500 ms window, so ≈ 7 dB/s maximum gain decay rate.
                {
                    const float FWD_RATIO = 1.5f;
                    for (int b = 0; b < numBlocks - 1; b++) {
                        if (blockGain[b] > blockGain[b + 1] * FWD_RATIO)
                            blockGain[b] = blockGain[b + 1] * FWD_RATIO;
                    }
                }

                // Apply with linear interpolation between block boundaries
                // to avoid audible gain steps at every 500 ms boundary.
                for (int b = 0; b < numBlocks; b++) {
                    int s0 = outStart + b * BLOCK_SAMPS;
                    int s1 = std::min(s0 + BLOCK_SAMPS, (int)samples.size());
                    float g0 = blockGain[b];
                    float g1 = (b + 1 < numBlocks) ? blockGain[b + 1] : g0;
                    for (int i = s0; i < s1; i++) {
                        float t = (float)(i - s0) / (float)(s1 - s0);
                        float g = g0 + (g1 - g0) * t;
                        int32_t v = std::clamp((int32_t)std::round(samples[i] * g), -32768, 32767);
                        samples[i] = (int16_t)v;
                    }
                }
            }
        }

        // Dynamic onset / offset trim.
        // After the scalar normalisation and block RMS compressor have run (both
        // in-place), scan the result in 50 ms windows to find where audio actually
        // starts and where it last contains content.  This removes:
        //   • leading noise: pre-voice carrier hiss that precedes the first word
        //   • trailing silence: digital zeros written by the fade-out path
        //
        // Uses the 15th-percentile window-RMS as the post-compression noise floor
        // (same estimator as the block compressor, but computed on the final samples
        // so it's invariant to the gain curve applied above).
        //
        // Only applied when the processable region is ≥ 1 s (48000 samples) — short
        // clips don't have enough windows for a stable percentile.
        int outEnd = (int)samples.size();
        {
            const int   SCAN_WIN    = 2400;   // 50 ms @ 48 kHz
            const float ONSET_MULT  = 2.0f;   // onset:  first window > 2× noise floor (gentle — keeps quiet word onsets)
            const float OFFSET_MULT = 2.0f;   // offset: last  window > 2× noise floor
            const int   MIN_CONTENT = 48000;  // require ≥ 1 s of post-trim content

            int avail = (int)samples.size() - outStart;
            if (avail >= MIN_CONTENT) {
                // Build per-window RMS vector for noise-floor percentile
                std::vector<float> wRms;
                wRms.reserve(avail / SCAN_WIN + 1);
                for (int i = outStart; i + SCAN_WIN <= (int)samples.size(); i += SCAN_WIN) {
                    float s2 = 0.0f;
                    for (int j = i; j < i + SCAN_WIN; j++) {
                        float f = samples[j] / 32767.0f;
                        s2 += f * f;
                    }
                    wRms.push_back(sqrtf(s2 / (float)SCAN_WIN));
                }

                if (!wRms.empty()) {
                    std::vector<float> sortedW = wRms;
                    std::sort(sortedW.begin(), sortedW.end());
                    float scanNoise = sortedW[std::max(0, (int)(sortedW.size() * 0.15f) - 1)];

                    if (scanNoise > 1e-6f) {
                        float onsetThr  = scanNoise * ONSET_MULT;
                        float offsetThr = scanNoise * OFFSET_MULT;

                        // Onset: advance outStart to the first window above the onset
                        // threshold, but (a) back up 4 windows (200 ms) so a quiet first-word
                        // attack is preserved, and (b) cap the advance at 6 windows (300 ms)
                        // past the fixed trim so a mis-fire can never eat into speech. With the
                        // 200 ms lead-in this usually doesn't advance at all when the signal
                        // begins right after the fixed trim — it only bites on real dead air.
                        const int onsetBase    = outStart;
                        const int ONSET_LEADIN = 4 * SCAN_WIN;   // 200 ms attack lead-in
                        const int ONSET_CAP    = 6 * SCAN_WIN;   // ≤ 300 ms total onset trim
                        for (int i = outStart; i + SCAN_WIN <= (int)samples.size(); i += SCAN_WIN) {
                            float s2 = 0.0f;
                            for (int j = i; j < i + SCAN_WIN; j++) {
                                float f = samples[j] / 32767.0f;
                                s2 += f * f;
                            }
                            if (sqrtf(s2 / (float)SCAN_WIN) >= onsetThr) {
                                int cand = std::min(i - ONSET_LEADIN, onsetBase + ONSET_CAP);
                                outStart = std::max(onsetBase, cand);
                                break;
                            }
                        }

                        // Offset: retract outEnd to just past last window above offset threshold.
                        // Add two windows of tail so the final word isn't clipped.
                        for (int i = (int)samples.size() - SCAN_WIN;
                             i > outStart + SCAN_WIN; i -= SCAN_WIN) {
                            float s2 = 0.0f;
                            for (int j = i; j < i + SCAN_WIN; j++) {
                                float f = samples[j] / 32767.0f;
                                s2 += f * f;
                            }
                            if (sqrtf(s2 / (float)SCAN_WIN) >= offsetThr) {
                                outEnd = std::min((int)samples.size(), i + 2 * SCAN_WIN);
                                break;
                            }
                        }
                    }
                }
            }
        }

        int outCount = outEnd - outStart;
        if (outCount <= 0) return;

        // 500ms silence on each side — prevents Apple Speech error 1110 ("no speech")
        // on short clips by ensuring total duration >= ~1.5s. Inaudible during playback.
        const int silencePad = 24000; // 500ms @ 48kHz
        int outputTotal = silencePad + outCount + silencePad;

        uint32_t newDataSize = (uint32_t)(outputTotal * 2);
        uint32_t newRiffSize = 36 + newDataSize;

        flog::info("[ChannelBank] normalizeWavFile: trimming {0} samples, writing {1}", outStart, outCount);

        // Write to a temp file then rename — most reliable, no in-place patching issues.
        std::string tmp = path + ".tmp";
        FILE* fw = fopen(tmp.c_str(), "wb");
        if (!fw) { flog::error("[ChannelBank] normalizeWavFile: failed to open tmp for write"); return; }

        // Write a standard 44-byte PCM WAV header for mono INT16 @ 48 kHz.
        const uint32_t sampleRate  = 48000;
        const uint16_t channels    = 1;
        const uint16_t bitsPerSamp = 16;
        const uint16_t blockAlign  = channels * bitsPerSamp / 8;
        const uint32_t byteRate    = sampleRate * blockAlign;
        const uint16_t audioFmt    = 1; // PCM
        fwrite("RIFF", 1, 4, fw);
        fwrite(&newRiffSize,  4, 1, fw);
        fwrite("WAVE", 1, 4, fw);
        fwrite("fmt ", 1, 4, fw);
        uint32_t fmtSize = 16; fwrite(&fmtSize,     4, 1, fw);
        fwrite(&audioFmt,    2, 1, fw);
        fwrite(&channels,    2, 1, fw);
        fwrite(&sampleRate,  4, 1, fw);
        fwrite(&byteRate,    4, 1, fw);
        fwrite(&blockAlign,  2, 1, fw);
        fwrite(&bitsPerSamp, 2, 1, fw);
        fwrite("data", 1, 4, fw);
        fwrite(&newDataSize, 4, 1, fw);
        std::vector<int16_t> zeroPad(silencePad, 0);
        fwrite(zeroPad.data(),           sizeof(int16_t), silencePad, fw);
        fwrite(samples.data() + outStart, sizeof(int16_t), outCount,   fw);
        fwrite(zeroPad.data(),           sizeof(int16_t), silencePad, fw);
        fclose(fw);

        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) flog::error("[ChannelBank] normalizeWavFile: rename failed: {0}", ec.message());
    }

    bool openNewFile(ChannelSlot& slot) {
        const double audioSr = 48000.0;
        slot.writer.setFormat(wav::FORMAT_WAV);
        slot.writer.setChannels(1);
        slot.writer.setSampleType(wav::SAMP_TYPE_INT16);
        slot.writer.setSamplerate((uint64_t)audioSr);

        time_t now = time(0);
        tm* ltm = localtime(&now);
        char buf[320];
        std::string bmName = bookmarkNameForFilename(slot.freqHz);
        if (!bmName.empty())
            snprintf(buf, sizeof(buf), "%s_%s_%.4fMHz_%02d-%02d-%02d_%02d-%02d-%04d.wav",
                bmName.c_str(), name.c_str(), slot.freqHz / 1e6,
                ltm->tm_hour, ltm->tm_min, ltm->tm_sec,
                ltm->tm_mday, ltm->tm_mon + 1, ltm->tm_year + 1900);
        else
            snprintf(buf, sizeof(buf), "%s_%.4fMHz_%02d-%02d-%02d_%02d-%02d-%04d.wav",
                name.c_str(), slot.freqHz / 1e6,
                ltm->tm_hour, ltm->tm_min, ltm->tm_sec,
                ltm->tm_mday, ltm->tm_mon + 1, ltm->tm_year + 1900);
        std::string path = expandString(folderSelect.path + "/" + buf);
        slot.currentFilePath = path;
        flog::info("[ChannelBank] Opening file: {0}", path);
        if (!slot.writer.open(path)) {
            flog::error("[ChannelBank] Failed to open: {0}", path);
            return false;
        }
        slot.fileOpen             = true;
        slot.fileOpenTime         = std::chrono::steady_clock::now();
        slot.audioSamplesWritten  = 0;
        slot.fileTrimSamples      = 0;    // pre-roll provides seamless audio, no post-open discard needed
        slot.recFadeRemaining     = 0;    // pre-roll audio is continuous, no fade-in click to suppress
        slot.snrSum               = 0.0f;
        slot.snrCount             = 0;
        slot.gateFramesAbove.store(0);   // fresh static-gate tally per recording
        slot.gateFramesVoice.store(0);
        slot.onAirFrames.store(0);       // fresh on-air (min-TX) tally per recording
        slot.driftSum             = 0.0; // fresh drift-gate stats per recording
        slot.driftSumSq           = 0.0;
        slot.fadeOutRemaining     = 2400; // 50ms at 48kHz — signal is present at file open
        // NOTE: deliberately do NOT register the frequency in permanent history here.
        // Doing so created an entry for every file-open — including the flood of opens
        // from broadband/drifting interference whose recordings are then discarded by the
        // static/drift gates — so the history filled with count-0 junk that per-frequency
        // blocking couldn't keep up with. The frequency is still blockable while recording
        // (active list) and for 30 s after teardown (recent list); a permanent history
        // entry is created only when a recording is actually KEPT (logRecording()).
        return true;
    }

private:
    std::string expandString(std::string input) {
        input = std::regex_replace(input, std::regex("%ROOT%"), root);
        return std::regex_replace(input, std::regex("//"), "/");
    }

    // ── Spectrum analysis ────────────────────────────────────────────────────

    static void spectrumHandler(dsp::complex_t* data, int count, void* ctx) {
        ChannelBankModule* _this = (ChannelBankModule*)ctx;

        // Rate-limit spectrum analysis to SPEC_ANALYSIS_HZ regardless of sample rate.
        // At 64 MHz SR the naive approach (FFT every 8192 samples) runs ~7,800 FFTs/sec,
        // saturates a CPU core, and creates backpressure on the DSP Splitter chain that
        // starves the waterfall's own FFT thread — causing visual choppiness.
        // Returning early here still lets the Handler sink flush the buffer immediately,
        // so the Splitter never blocks.
        _this->specSamplesUntilFFT -= count;
        if (_this->specSamplesUntilFFT > 0) return;

        double sr = _this->lastKnownSr;
        _this->specSamplesUntilFFT = (sr > 0.0)
            ? (int64_t)(sr / SPEC_ANALYSIS_HZ)
            : (int64_t)(FFT_SIZE);

        // Fill fftAccum from this block; zero-pad if the block is smaller than FFT_SIZE
        // (only possible at very low sample rates — typical HF/SDR blocks are much larger).
        int fill = std::min(count, FFT_SIZE);
        std::copy(data, data + fill, _this->fftAccum.data());
        if (fill < FFT_SIZE) {
            std::fill(_this->fftAccum.begin() + fill, _this->fftAccum.end(),
                      dsp::complex_t{0.0f, 0.0f});
        }
        _this->fftBufPos = 0;
        _this->analyzeSpectrum();
    }

    void analyzeSpectrum() {
        // Refresh center frequency every frame — free (single double load) and ensures
        // PPM changes or other mid-session corrections are picked up without a full retune.
        lastKnownCenter = gui::waterfall.getCenterFrequency();

        // Check for deferred retune — safely reset DSP-owned state on the DSP thread
        if (retuneFlag.load()) {
            lastKnownSr     = pendingRetuneSr;
            lastKnownCenter = pendingRetuneCenter;
            fftBufPos        = 0;
            avgPower.clear();
            instPower.clear();
            rawSlotMisses.clear();
            rawManualMisses.clear();
            globalNoiseFloor  = 0.0f;
            displayNoiseFloor = 0.0f;
            floorHistory.clear();
            slotVotes.clear();
            manualVotes.clear();
            retuneFlag.store(false);
            return;  // skip this frame — buffers are stale from old tuning
        }

        // Determine how many real samples are in fftAccum this frame.
        // At low sample rates (< FFT_SIZE * SPEC_ANALYSIS_HZ ≈ 164 kHz) the DSP block
        // is shorter than FFT_SIZE, so the remainder was zero-padded in the DSP callback.
        // Applying the full FFT_SIZE BH window to this truncated data leaves it at ~0.14
        // at the cut point — far from zero — which reintroduces sidelobes almost as bad
        // as Hann.  The fix: build a BH window sized to the actual fill, zero-padded to
        // FFT_SIZE, and cache it so we only recompute on sample-rate changes.
        int fill = (lastKnownSr > 0.0)
            ? std::min((int)(lastKnownSr / SPEC_ANALYSIS_HZ), FFT_SIZE)
            : FFT_SIZE;
        if (fill != fftWindowFill) {
            fftWindow.resize(FFT_SIZE, 0.0f);
            int n = fill > 1 ? fill : 1;
            for (int i = 0; i < fill; i++) {
                float phi = 2.0f * M_PI * i / (float)(n - 1);
                fftWindow[i] = 0.35875f
                             - 0.48829f * cosf(phi)
                             + 0.14128f * cosf(2.0f * phi)
                             - 0.01168f * cosf(3.0f * phi);
            }
            for (int i = fill; i < FFT_SIZE; i++) fftWindow[i] = 0.0f;
            fftWindowFill = fill;
        }

        // Apply window and copy to FFTW input
        for (int i = 0; i < FFT_SIZE; i++) {
            fftIn[i][0] = fftAccum[i].re * fftWindow[i];
            fftIn[i][1] = fftAccum[i].im * fftWindow[i];
        }

        fftwf_execute(fftPlan);

        // Compute linear power per bin (FFT-shifted, normalised)
        float scale = 1.0f / (float)(FFT_SIZE * FFT_SIZE);

        // Two power arrays:
        //   avgPower  (alpha=0.15) — slow EMA: voting/spawning/NMS/noise floor
        //   instPower              — instantaneous (no EMA): rawSignalPresent / fade trigger only
        //
        // Why not use an EMA for rawSignalPresent?  A charged EMA (even alpha=0.50) takes
        // 5–7 frames (250–350 ms) to decay below the SNR threshold after a strong carrier
        // drops — because signal power during a long transmission fully charges the accumulator.
        // Instantaneous power drops to noise level in ONE frame (50 ms), so with a
        // 2-consecutive-miss guard the fade starts within ≤100 ms regardless of how long
        // or strong the transmission was.
        constexpr float alpha = 0.15f;
        bool firstFrame = avgPower.empty();
        if (firstFrame) {
            avgPower.resize(FFT_SIZE);
            instPower.resize(FFT_SIZE);
        }
        for (int i = 0; i < FFT_SIZE; i++) {
            int k = (i + FFT_SIZE / 2) % FFT_SIZE;
            float re = fftOut[k][0], im = fftOut[k][1];
            float inst = (re * re + im * im) * scale;
            avgPower[i]  = firstFrame ? inst : (alpha * inst + (1.0f - alpha) * avgPower[i]);
            instPower[i] = inst;  // no smoothing — raw per-frame power for fade trigger
        }
        // Slow EMA for all detection/voting; instantaneous for raw fade trigger
        std::vector<float>& power = avgPower;

        double binHz  = lastKnownSr / FFT_SIZE;
        // numSlots covers the FULL bandwidth so we can detect signals anywhere.
        // bwUsage only controls which slots contribute to the noise floor estimate
        // (avoids filter rolloff edges inflating it).
        int numSlots  = (int)std::floor(lastKnownSr / channelSpacing);
        // Detection window: fixed ~8 kHz bandwidth (matching AM signal width)
        // regardless of channel spacing, so wider spacings don't dilute the SNR.
        // Capped to not exceed the slot width.
        constexpr double DETECT_BW_HZ = 8000.0;
        int halfBins  = std::max(1, (int)std::round(std::min(channelSpacing * 0.4, DETECT_BW_HZ / 2.0) / binHz));

        // snrLinear: threshold required to SPAWN a new channel.
        // holdSnrLinear: lower threshold used while a recording file is open — lets the
        //   channel ride through HF QSB dips without losing votes or rawSignalPresent.
        //   Derived from snrThreshold - holdHysteresisDb.
        float snrLinear     = powf(10.0f, snrThreshold / 10.0f);
        float holdSnrLinear = snrLinear * powf(10.0f, -holdHysteresisDb / 10.0f);

        // Pre-compute which active-channel indices currently have an open recording
        // file — used in both manual and auto modes below to select the right threshold.
        std::set<int> openSlotIndices;
        {
            std::lock_guard<std::mutex> clck(channelsMtx);
            for (auto& [idx, slot] : activeChannels)
                if (slot->fileOpen) openSlotIndices.insert(idx);
        }

        // Compute per-slot means (using detection window, not full slot width).
        // Two variants: slotMeans (slow EMA) for voting; instSlotMeans (instantaneous) for fade trigger.
        std::vector<float> slotMeans(numSlots);
        std::vector<float> instSlotMeans(numSlots);
        std::vector<float> slotFlatness(numSlots, 1.0f);  // spectral flatness per slot: ~1 flat/noise, ~0 peaky/carrier
        std::vector<float> slotCentroidHz(numSlots, 0.0f);// carrier centroid relative to slot center (Hz) — for drift gate
        std::map<int, double> newPeakOffsets;
        for (int s = 0; s < numSlots; s++) {
            double slotOffset = ((double)s - (double)(numSlots - 1) / 2.0) * channelSpacing;
            int centerBin = (int)std::round((slotOffset / lastKnownSr) * FFT_SIZE) + FFT_SIZE / 2;
            int lo = std::clamp(centerBin - halfBins, 0, FFT_SIZE - 1);
            int hi = std::clamp(centerBin + halfBins, 0, FFT_SIZE - 1);
            float  sum = 0.0f, instSum = 0.0f;
            double logSum = 0.0;  // Σ ln(power) for geometric mean → spectral flatness
            int   peakBin = lo;
            for (int b = lo; b <= hi; b++) {
                sum     += power[b];
                instSum += instPower[b];
                logSum  += logf(std::max(power[b], 1e-20f));
                if (power[b] > power[peakBin]) peakBin = b;
            }
            int nBins = hi - lo + 1;
            slotMeans[s]     = sum     / (float)nBins;
            instSlotMeans[s] = instSum / (float)nBins;
            // Spectral flatness measure (Wiener entropy): geometric mean / arithmetic mean
            // of the channel's power bins. → 1.0 for a flat (broadband static) spectrum,
            // → 0 when one bin (the carrier) dominates. This is what separates a real
            // AM/voice signal (carrier + sidebands = peaky) from interference (flat),
            // regardless of when the bursts occur.
            double arithMean = (double)slotMeans[s];
            double geoMean   = exp(logSum / (double)nBins);
            slotFlatness[s]  = (arithMean > 1e-30) ? (float)(geoMean / arithMean) : 1.0f;
            // Spectral centroid: energy-weighted average bin frequency.
            // For SSB voice, this lands near the middle of the voice passband
            // (~1000–1500 Hz above/below carrier) rather than at the loudest
            // fundamental (~300–500 Hz), giving much better carrier tracking.
            double weightedSum = 0.0;
            for (int b = lo; b <= hi; b++)
                weightedSum += (double)b * (double)power[b];
            double centroidBin = (sum > 0.0f) ? (weightedSum / (double)sum) : (double)centerBin;
            newPeakOffsets[s] = ((centroidBin - FFT_SIZE / 2) / FFT_SIZE) * lastKnownSr;
            // Centroid relative to this slot's center — small (±detection window), so the
            // drift-gate variance math stays well-conditioned. Subtracting a constant
            // doesn't change the stddev we ultimately test.
            slotCentroidHz[s] = (float)(newPeakOffsets[s] - slotOffset);
        }

        // Global noise floor — 20th percentile of CENTER slot means (bwUsage
        // fraction).  Edge slots are excluded so filter rolloff doesn't inflate
        // the floor estimate, but those slots can still detect signals.
        // The spectrum EMA already smooths bin-level noise, so the rawFloor from
        // the 20th percentile is stable.  We use a simple symmetric EMA here
        // (fast enough to track real noise changes, slow enough to ignore brief
        // signal bursts that leak into the 20th percentile).
        {
            int baseEdgeSkip = (int)std::round(numSlots * (1.0f - bwUsage) / 2.0f);
            int leftSkip  = std::max(baseEdgeSkip, (int)std::round(numSlots * leftTrimFrac));
            int rightSkip = std::max(baseEdgeSkip, (int)std::round(numSlots * rightTrimFrac));
            std::vector<float> centerMeans;
            for (int s = leftSkip; s < numSlots - rightSkip; s++)
                centerMeans.push_back(slotMeans[s]);
            if (centerMeans.empty()) centerMeans = {slotMeans[numSlots / 2]};
            std::sort(centerMeans.begin(), centerMeans.end());
            float rawFloor = centerMeans[std::max(0, (int)(centerMeans.size() * 0.20f) - 1)];
            rawFloor = std::max(rawFloor, 1e-30f);

            // Median-over-N-frames: rejects transient noise pulses (RFI) entirely
            // unless they last for more than half the buffer. EMA smoothers
            // partially track every pulse, raising the threshold momentarily and
            // making detection vulnerable to brief whole-band noise events.
            // ~30 frames is ~100ms at 2.4MHz SR (FFT_SIZE=8192, ~290 Hz frame
            // rate); short enough to track real band-condition changes within
            // half a second, long enough that 50/60/100/120 Hz mains-related
            // RFI pulses are completely ignored.
            constexpr size_t FLOOR_HISTORY_LEN = 30;
            floorHistory.push_back(rawFloor);
            if (floorHistory.size() > FLOOR_HISTORY_LEN) floorHistory.pop_front();

            if (globalNoiseFloor <= 0.0f) {
                globalNoiseFloor  = rawFloor;
                displayNoiseFloor = rawFloor;
            } else {
                std::vector<float> sorted(floorHistory.begin(), floorHistory.end());
                std::nth_element(sorted.begin(),
                                 sorted.begin() + sorted.size() / 2,
                                 sorted.end());
                globalNoiseFloor = sorted[sorted.size() / 2];
            }
            displayNoiseFloor = 0.985f * displayNoiseFloor + 0.015f * globalNoiseFloor;
        }

        // Wideband noise event detection — lightning, power-line QRM, solar events, etc.
        //
        // Real signals raise 1–3 adjacent slots. Broadband impulses raise ALL of them.
        // If >40% of center slots simultaneously exceed the SNR threshold (measured on
        // instantaneous power so the flag rises in one FFT frame, ~50 ms), we freeze all
        // slot votes for that frame: no channels spawn, no channels lose votes.
        // Active channels that were genuinely signalling remain unaffected — their
        // rawSignalPresent state is left unchanged until normal processing resumes.
        //
        // Threshold rationale:
        //   Real signals: a busy HF band (e.g. 80m SSB, 500 kHz) at 8 MHz SR
        //     occupies 500/8000 = 6% of slots — even wall-to-wall.
        //     At 2 MHz SR focused on 80m, worst-case occupancy is ~25%.
        //   Lightning: raises 80–100% of all slots simultaneously.
        //   40% gives a comfortable margin in all realistic cases and protects
        //   narrower-bandwidth HF setups where a packed 40m/80m band could
        //   approach the old 20% line.
        {
            int wbBaseEdge  = (int)std::round(numSlots * (1.0f - bwUsage) / 2.0f);
            int wbLeftEdge  = std::max(wbBaseEdge, (int)std::round(numSlots * leftTrimFrac));
            int wbRightEdge = std::max(wbBaseEdge, (int)std::round(numSlots * rightTrimFrac));
            int wbCenter    = std::max(1, numSlots - wbLeftEdge - wbRightEdge);
            int wbAbove     = 0;
            for (int s = wbLeftEdge; s < numSlots - wbRightEdge; s++)
                if (instSlotMeans[s] > globalNoiseFloor * snrLinear) wbAbove++;
            widebandEvent = (wbAbove > wbCenter * 2 / 5);  // >40%
        }

        // Manual mode: check configured frequencies instead of grid voting
        if (manualMode) {
            std::vector<double> localFreqs = getActiveManualFreqs();
            std::set<int>       newDetected;
            std::set<int>       newRawDetected;
            std::map<int,float> newManualSnr;
            for (int i = 0; i < (int)localFreqs.size(); i++) {
                double freqOffset = localFreqs[i] - lastKnownCenter;
                if (std::abs(freqOffset) >= lastKnownSr / 2.0) continue;
                int centerBin = (int)std::round((freqOffset / lastKnownSr) * FFT_SIZE) + FFT_SIZE / 2;
                int halfBins2 = std::max(1, (int)std::round(std::min(channelSpacing * 0.4, DETECT_BW_HZ / 2.0) / binHz));
                int lo, hi;
                // Two guard bands per SSB mode (-1 = unused):
                //   guard1: outer guard, same side as the voice passband
                //           (above carrier for USB, below for LSB) — catches interferers
                //           whose lower/upper sideband overlaps the detection window
                //   guard2: inner guard, opposite side of the carrier from the voice
                //           (below carrier for USB, above for LSB) — catches interferers
                //           whose upper/lower sideband leaks across the carrier
                // USB voice has zero energy below the carrier; LSB has zero above.
                // Any elevation there means an adjacent signal is leaking through.
                int guard1Lo = -1, guard1Hi = -1;
                int guard2Lo = -1, guard2Hi = -1;
                if (demodMode == DEMOD_USB) {
                    // USB energy lives in 300-2800 Hz above the carrier only.
                    // Using halfBins2*2 (≈6.6 kHz for 8.33 kHz channel spacing)
                    // is far too wide and catches adjacent HFDL signals 6 kHz up.
                    // Clamp the window to the actual SSB passband width (2800 Hz).
                    int ssbBins = std::max(1, (int)std::round(2800.0 / binHz));
                    lo = centerBin;
                    hi = std::clamp(centerBin + ssbBins, 0, FFT_SIZE - 1);
                    // guard1: above the voice passband (catches HFDL above the bookmark)
                    guard1Lo = hi + 1;
                    guard1Hi = std::clamp(hi + ssbBins, 0, FFT_SIZE - 1);
                    // guard2: below the carrier (catches HFDL below the bookmark)
                    // USB voice never has energy here, so any elevation is interference.
                    guard2Lo = std::clamp(centerBin - ssbBins, 0, FFT_SIZE - 1);
                    guard2Hi = std::max(0, centerBin - 1);
                } else if (demodMode == DEMOD_LSB) {
                    // Mirror: LSB energy lives in 300-2800 Hz below the carrier.
                    int ssbBins = std::max(1, (int)std::round(2800.0 / binHz));
                    lo = std::clamp(centerBin - ssbBins, 0, FFT_SIZE - 1);
                    hi = centerBin;
                    // guard1: below the voice passband (catches HFDL below the bookmark)
                    guard1Lo = std::clamp(lo - ssbBins, 0, FFT_SIZE - 1);
                    guard1Hi = lo - 1;
                    // guard2: above the carrier (catches HFDL above the bookmark)
                    guard2Lo = hi + 1;
                    guard2Hi = std::clamp(hi + ssbBins, 0, FFT_SIZE - 1);
                } else {
                    // AM / NFM / WFM: symmetric window around carrier
                    lo = std::clamp(centerBin - halfBins2, 0, FFT_SIZE - 1);
                    hi = std::clamp(centerBin + halfBins2, 0, FFT_SIZE - 1);
                }
                float sum = 0.0f, instSum = 0.0f;
                for (int b = lo; b <= hi; b++) {
                    sum     += power[b];
                    instSum += instPower[b];
                }
                int nBins2 = hi - lo + 1;
                float mean     = sum     / (float)nBins2;
                float instMean = instSum / (float)nBins2;
                // Two separate thresholds — same split as auto mode:
                //   aboveVote: slow EMA (mean) — stable, low-noise, drives vote accumulation
                //   aboveRaw:  instantaneous (instMean) — fast, drives rawSignalPresent / fade only
                // Using instMean for voting caused strong-signal modulation spikes to
                // accumulate votes on adjacent channels, triggering false detections.
                //
                // Hysteresis: once a file is open on this index, use holdSnrLinear
                // (= snrThreshold - holdHysteresisDb) so brief fades don't drain votes.
                float effSnr   = openSlotIndices.count(i) ? holdSnrLinear : snrLinear;
                bool aboveVote = (mean     > globalNoiseFloor * effSnr);
                bool aboveRaw  = (instMean > globalNoiseFloor * effSnr);

                // Guard-band suppression for USB/LSB.
                //
                // Two complementary guards cover both directions:
                //   guard1 (outer): same side as the voice passband.
                //                  Elevated when HFDL/interferer is above USB
                //                  (or below LSB) and its sideband bleeds in.
                //   guard2 (inner): opposite side of the carrier from the voice.
                //                  USB voice has ZERO energy below the carrier;
                //                  LSB voice has ZERO above.  Any elevation there
                //                  means a signal below (USB) or above (LSB) the
                //                  bookmark is leaking across the carrier.
                //
                // If EITHER guard is significantly elevated AND the detection window
                // is not much stronger than that guard, the signal is rejected as
                // likely interference rather than genuine voice.
                //
                // The "guard elevated" pre-check prevents penalising weak voice
                // transmissions in a quiet environment — guard check only activates
                // when there is actually something to suppress.
                auto checkGuard = [&](int gLo, int gHi) {
                    if (gLo < 0 || gHi < gLo) return;
                    float gSum = 0.0f, gInstSum = 0.0f;
                    for (int b = gLo; b <= gHi; b++) {
                        gSum     += power[b];
                        gInstSum += instPower[b];
                    }
                    int   nGuard    = gHi - gLo + 1;
                    float gMean     = gSum     / (float)nGuard;
                    float gInstMean = gInstSum / (float)nGuard;
                    // Activation gate: slow EMA (stable, ignores single-frame noise spikes).
                    if (gMean > globalNoiseFloor * (snrLinear * 0.5f)) {
                        // Ratio uses instantaneous power for both numerator and denominator.
                        // This eliminates EMA charge-up lag at transmission start:
                        //   • New voice tx: instMean jumps immediately → ratio is high → passes.
                        //   • Persistent HFDL with no voice: instMean ≈ gInstMean → ratio ≈ 1 → suppressed.
                        // Using the EMA 'mean' here caused 1-3 extra frames of false suppression
                        // at the start of each transmission (~50-150 ms of missing audio).
                        float ratio = (gInstMean > 1e-30f) ? (instMean / gInstMean) : 100.0f;
                        if (ratio < 1.5f) {
                            aboveVote = false;
                            aboveRaw  = false;
                        }
                    }
                };
                checkGuard(guard1Lo, guard1Hi);
                checkGuard(guard2Lo, guard2Hi);

                // Ambient leakage suppression for AM / NFM / WFM.
                // USB/LSB are already protected by the two-guard system above.
                //
                // Problem: on HF, a strong signal (e.g. HFDL at 8.925 MHz) whose
                // spectral edge falls inside the ±4 kHz detection window of a nearby
                // bookmark (e.g. 8.918 or 8.933 MHz, each ~7 kHz away) causes a false
                // detection.  The main lobe of the interferer sits in the adjacent
                // frequency window outside the detection bins.
                //
                // Fix: compute the instantaneous mean power in the two adjacent windows
                // (each 12 kHz wide, just outside the detection window).  If either
                // exceeds the detection-window power by more than 2.5× (≈ +4 dB), the
                // detection is classified as spillover from a dominant adjacent carrier,
                // not a genuine signal, and both flags are suppressed.
                //
                // When a real signal IS present at the bookmark frequency alongside the
                // adjacent carrier, the detection-window power rises above just the
                // spillover component, pulling the ratio back below 2.5 and letting the
                // real signal through.
                if ((aboveRaw || aboveVote) && demodMode != DEMOD_USB && demodMode != DEMOD_LSB) {
                    const double AMBIENT_BW_HZ = 12000.0;
                    int ambW = std::max(1, (int)std::round(AMBIENT_BW_HZ / binHz));
                    int leftLo  = std::clamp(lo - ambW, 0, FFT_SIZE - 1);
                    int leftHi  = std::clamp(lo - 1,   0, FFT_SIZE - 1);
                    int rightLo = std::clamp(hi + 1,    0, FFT_SIZE - 1);
                    int rightHi = std::clamp(hi + ambW, 0, FFT_SIZE - 1);

                    // Instantaneous ambient mean (for aboveRaw gate)
                    auto instAmbMean = [&](int aLo, int aHi) -> float {
                        if (aLo > aHi) return 0.0f;
                        float s = 0.0f;
                        for (int b = aLo; b <= aHi; b++) s += instPower[b];
                        return s / (float)(aHi - aLo + 1);
                    };
                    // EMA ambient mean (for aboveVote gate — matches slow-EMA mean)
                    auto emaAmbMean = [&](int aLo, int aHi) -> float {
                        if (aLo > aHi) return 0.0f;
                        float s = 0.0f;
                        for (int b = aLo; b <= aHi; b++) s += power[b];
                        return s / (float)(aHi - aLo + 1);
                    };

                    float instMaxAmb = std::max(instAmbMean(leftLo, leftHi),
                                                instAmbMean(rightLo, rightHi));
                    float emaMaxAmb  = std::max(emaAmbMean(leftLo, leftHi),
                                                emaAmbMean(rightLo, rightHi));

                    if (instMean > 1e-30f && instMaxAmb > instMean * 2.5f) aboveRaw  = false;
                    if (mean     > 1e-30f && emaMaxAmb  > mean     * 2.5f) aboveVote = false;
                }

                if (aboveRaw && !widebandEvent) newRawDetected.insert(i);  // raw, un-voted
                // Store per-freq SNR for M4A metadata
                if (globalNoiseFloor > 0.0f)
                    newManualSnr[i] = 10.0f * log10f(mean / globalNoiseFloor);
                // Votes frozen during wideband events (lightning protection)
                if (!widebandEvent) {
                    int& v = manualVotes[i];
                    v = aboveVote ? std::min(v + 1, MAX_VOTES) : std::max(v - 1, 0);
                    if (v >= SPAWN_VOTES) newDetected.insert(i);
                }
            }
            {
                std::lock_guard<std::mutex> lk(manualDetectedMtx);
                manualDetected    = newDetected;
                rawManualDetected = newRawDetected;   // copy — keep newRawDetected usable below
                manualSnrDb       = std::move(newManualSnr);
            }
            {
                std::lock_guard<std::mutex> dlck(displayMtx);
                displaySnap.power.resize(FFT_SIZE);
                for (int i = 0; i < FFT_SIZE; i++)
                    displaySnap.power[i] = 10.0f * log10f(power[i] + 1e-30f);
                displaySnap.threshDb  = 10.0f * log10f(displayNoiseFloor * snrLinear + 1e-30f);
                displaySnap.slotCenterBin.clear();
                displaySnap.detected.clear();
                displaySnap.numSlots  = 0;
                displaySnap.manualCenterBins.clear();
                displaySnap.manualActiveFlags.clear();
                for (int i = 0; i < (int)localFreqs.size(); i++) {
                    double freqOffset = localFreqs[i] - lastKnownCenter;
                    if (std::abs(freqOffset) >= lastKnownSr / 2.0) continue;
                    int bin = (int)std::round((freqOffset / lastKnownSr) * FFT_SIZE) + FFT_SIZE / 2;
                    displaySnap.manualCenterBins.push_back(bin);
                    displaySnap.manualActiveFlags.push_back(newDetected.count(i) > 0);
                }
                std::vector<float> sorted = displaySnap.power;
                std::sort(sorted.begin(), sorted.end());
                displaySnap.dBmin = sorted[(int)(FFT_SIZE * 0.05f)] - 5.0f;
                displaySnap.dBmax = sorted[(int)(FFT_SIZE * 0.95f)] + 15.0f;
            }
            // Immediately propagate raw detection to active slots — same FFT frame,
            // no management-thread hop. Uses 2-consecutive-miss guard, same as auto mode.
            // Skipped during wideband events so lightning doesn't interfere with state.
            if (!widebandEvent) {
                std::lock_guard<std::mutex> clck(channelsMtx);
                for (auto& [idx, slot] : activeChannels) {
                    bool above = (newRawDetected.count(idx) > 0);
                    if (above) {
                        rawManualMisses[idx] = 0;
                        slot->rawSignalPresent.store(true);
                        int hits = slot->rawConsecutiveHits.load() + 1;
                        slot->rawConsecutiveHits.store(hits > 4 ? 4 : hits); // cap to avoid int creep
                    } else if (++rawManualMisses[idx] >= 2) {
                        slot->rawSignalPresent.store(false);
                        slot->rawConsecutiveHits.store(0);
                    }
                    // On-air tally for the Min TX check (manual mode). Mirrors the auto path.
                    if (above && slot->fileOpen) slot->onAirFrames.fetch_add(1);
                }
            }
            mgmtCv.notify_one();
            return;
        }

        // Vote on each slot against the global floor.
        // Frozen during wideband noise events (lightning, etc.) so broadband
        // impulses can't accumulate the votes needed to spawn new channels.
        // Existing votes don't decay either — real active signals are protected.
        //
        // Hysteresis: slots with an open recording file use holdSnrLinear
        // (snrThreshold - holdHysteresisDb) so brief fades don't drain votes
        // and prematurely end recordings of weak/QSB signals.
        if (!widebandEvent) {
            for (int s = 0; s < numSlots; s++) {
                float effSnr        = openSlotIndices.count(s) ? holdSnrLinear : snrLinear;
                bool aboveThreshold = (slotMeans[s] > globalNoiseFloor * effSnr);
                int& votes = slotVotes[s];
                if (aboveThreshold) { votes = std::min(votes + 1, MAX_VOTES); }
                else                { votes = std::max(votes - 1, 0); }
            }
        }

        // Pass 2: non-maximum suppression. We need to prevent two adjacent slots
        // from being detected for the same physical signal — common at wide
        // bandwidths (e.g. 10 MHz, ~1.2 kHz bin width) where an AM carrier
        // straddling two 12.5 kHz slots has its peak energy drift across the
        // boundary as it modulates.
        //
        // Approach: greedy strongest-first selection, respecting both already-
        // active channels and slots accepted earlier *within this same frame*.
        // The frame-local accept tracking is critical because activeChannels is
        // updated by the mgmt thread asynchronously — so checking it alone
        // doesn't prevent two newly-qualifying adjacent slots from both winning
        // the same frame.
        std::set<int> detected;
        {
            std::lock_guard<std::mutex> clck(channelsMtx);

            // Always include already-active slots, exempt from NMS, with a much
            // looser vote threshold than spawning (SPAWN_VOTES). Once a slot is
            // active we trust it: as long as ANY votes remain, keep it in
            // `detected` so it (a) keeps its channel alive and (b) blocks
            // adjacent newcomers via the greedy NMS pass below.
            //
            // Why this matters: a signal sitting on a slot boundary can have its
            // peak energy drift between adjacent slots over time. Without this
            // looser threshold, slot N's votes briefly dip below SPAWN_VOTES,
            // its channel "loses lock" to the silence countdown, slot N+1
            // spawns a fresh channel, and we end up with two overlapping
            // recordings of the same transmission. Vote decay is symmetric
            // (one per frame), so a real signal-gone case still tears the
            // channel down within a few frames once the signal genuinely
            // disappears.
            std::vector<int> candidates;
            for (int s = 0; s < numSlots; s++) {
                bool active = (activeChannels.find(s) != activeChannels.end());
                if (active) {
                    if (slotVotes[s] > 0) detected.insert(s);
                    // votes==0 → let the channel die naturally; don't insert
                } else {
                    if (slotVotes[s] < SPAWN_VOTES) { continue; }
                    candidates.push_back(s);
                }
            }

            // Sort remaining candidates by power desc — strongest signals win
            // the right to spawn first.
            std::sort(candidates.begin(), candidates.end(),
                      [&](int a, int b) { return slotMeans[a] > slotMeans[b]; });

            // Greedy accept: skip if any neighbor is already active OR already
            // accepted into `detected` this frame.
            for (int s : candidates) {
                bool leftBlocked  = (s > 0            && detected.count(s - 1) > 0);
                bool rightBlocked = (s < numSlots - 1 && detected.count(s + 1) > 0);
                if (leftBlocked || rightBlocked) { continue; }
                detected.insert(s);
            }

            // Immediately propagate raw (un-voted) detection to active slots.
            // Uses instantaneous power with a 2-consecutive-miss guard:
            //   • above threshold → reset miss counter, hold rawSignalPresent true
            //   • 1st miss        → hold (single-frame noise dip protection)
            //   • 2nd+ miss       → set rawSignalPresent false → cosine fade begins
            // Worst-case tail: 2 frames (100 ms) + 50 ms fade = ~150 ms.
            //
            // During wideband events (lightning, etc.) the update is skipped entirely:
            // active real signals keep their current state; quiet channels don't get
            // their miss counter reset by broadband noise.
            if (!widebandEvent) {
                for (auto& [idx, slot] : activeChannels) {
                    float effSnrRaw = slot->fileOpen ? holdSnrLinear : snrLinear;
                    bool above = (idx < numSlots && instSlotMeans[idx] > globalNoiseFloor * effSnrRaw);
                    if (above) {
                        rawSlotMisses[idx] = 0;
                        slot->rawSignalPresent.store(true);
                        int hits = slot->rawConsecutiveHits.load() + 1;
                        slot->rawConsecutiveHits.store(hits > 4 ? 4 : hits);
                    } else if (++rawSlotMisses[idx] >= 2) {
                        slot->rawSignalPresent.store(false);
                        slot->rawConsecutiveHits.store(0);
                    }
                    // 1st miss: leave rawSignalPresent unchanged — protects against
                    // a single noisy frame setting off the fade prematurely.

                    // Static gate tally: on every above-threshold frame while recording,
                    // count whether this channel's spectrum has a carrier (peaky, low
                    // flatness = voice-like) or is flat (broadband static). The fraction
                    // over the whole recording decides keep-vs-discard at file close.
                    if (above && slot->fileOpen && idx < numSlots) {
                        slot->gateFramesAbove.fetch_add(1);
                        slot->onAirFrames.fetch_add(1);
                        if (slotFlatness[idx] < staticGateFlatness)
                            slot->gateFramesVoice.fetch_add(1);
                        // Drift gate: accumulate carrier-centroid stats. A stable carrier
                        // barely moves; a drifting/faulty emitter spreads these out.
                        double c = (double)slotCentroidHz[idx];
                        slot->driftSum   += c;
                        slot->driftSumSq += c * c;
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lck(detectedMtx);
            detectedSlots   = detected;
            slotPeakOffsets = std::move(newPeakOffsets);
            // Per-slot SNR for M4A metadata: signal power relative to noise floor (dB)
            slotSnrDb.resize(numSlots);
            for (int s = 0; s < numSlots; s++)
                slotSnrDb[s] = (globalNoiseFloor > 0.0f)
                    ? 10.0f * log10f(slotMeans[s] / globalNoiseFloor)
                    : 0.0f;
            // Raw (un-voted) detection — uses instSlotMeans (instantaneous, no EMA).
            // Not updated during wideband events to prevent management-thread reads
            // from seeing lightning-inflated slot sets.
            if (!widebandEvent) {
                rawDetectedSlots.clear();
                for (int s = 0; s < numSlots; s++)
                    if (instSlotMeans[s] > globalNoiseFloor * snrLinear)
                        rawDetectedSlots.insert(s);
            }
        }

        // Update display snapshot (UI reads under displayMtx)
        {
            std::lock_guard<std::mutex> dlck(displayMtx);
            displaySnap.power.resize(FFT_SIZE);
            for (int i = 0; i < FFT_SIZE; i++)
                displaySnap.power[i] = 10.0f * log10f(power[i] + 1e-30f);
            displaySnap.slotCenterBin.resize(numSlots);
            for (int s = 0; s < numSlots; s++) {
                double slotOffset = ((double)s - (double)(numSlots - 1) / 2.0) * channelSpacing;
                displaySnap.slotCenterBin[s] = (int)std::round((slotOffset / lastKnownSr) * FFT_SIZE) + FFT_SIZE / 2;
            }
            displaySnap.threshDb  = 10.0f * log10f(displayNoiseFloor * snrLinear + 1e-30f);
            displaySnap.detected  = detected;
            displaySnap.numSlots  = numSlots;
            // Auto-range: 5th/95th percentile for a clean y-axis
            std::vector<float> sorted = displaySnap.power;
            std::sort(sorted.begin(), sorted.end());
            displaySnap.dBmin = sorted[(int)(FFT_SIZE * 0.05f)] - 5.0f;
            displaySnap.dBmax = sorted[(int)(FFT_SIZE * 0.95f)] + 15.0f;
        }

        mgmtCv.notify_one();
    }

    // ── Channel lifecycle ────────────────────────────────────────────────────

#ifdef __APPLE__
    void pollTranscriptions() {
        std::lock_guard<std::mutex> clck(channelsMtx);
        for (auto& [idx, slot] : activeChannels) {
            if (!slot->transcribeHandle) continue;
            slot->liveTranscript = txGetText(slot->transcribeBackend, slot->transcribeHandle);
            if (!txIsFinal(slot->transcribeBackend, slot->transcribeHandle)) continue;

            // Pull time-aligned segments too (Whisper only — Apple Speech returns []).
            if (slot->transcribeBackend >= TB_WHISPER_ATC_LARGE) {
                slot->liveSegments = transcription_whisper::getSegments(slot->transcribeHandle);
                // Stash by path so playbackThreadFunc can install them when this
                // WAV's turn comes up in the queue.  pendingTranscriptPath is the
                // path the WAV was registered under at file-close time — same
                // string the playback queue uses.
                if (!slot->liveSegments.empty() && !slot->pendingTranscriptPath.empty()) {
                    std::lock_guard<std::mutex> sk(pendingPlaybackSegmentsMtx);
                    pendingPlaybackSegments[slot->pendingTranscriptPath] = slot->liveSegments;
                }
            }

            // Update live transcript display
            if (!slot->liveTranscript.empty()) {
                std::lock_guard<std::mutex> tlk(lastTranscriptMtx);
                lastTranscriptText = slot->liveTranscript;
                lastTranscriptName = displayName(slot->freqHz);
            }
            txDestroy(slot->transcribeBackend, slot->transcribeHandle);
            slot->transcribeHandle = nullptr;

            // Transcription chain is done — check if encoding can now proceed.
            // Store the transcript text so the encode thread can embed it as ©lyr.
            if (m4aEnabled && recordingEnabled) {
                bool canEncode = false;
                std::string encodePath, encodeTranscript;
                float       encodeSnrDb = 0.0f;
                // Prefer LRC (synced captions) when segments exist — external players
                // like VLC/QuickTime/IINA render them as time-aligned subtitles when
                // embedded in ©lyr.  Apple Speech has no segments → falls back to
                // the joined transcript text just like before.
                std::string transcriptForM4A = !slot->liveSegments.empty()
                    ? transcription_whisper::formatLrc(slot->liveSegments)
                    : slot->liveTranscript;
                {
                    std::lock_guard<std::mutex> elk(pendingEncodesMtx);
                    auto it = pendingEncodes.find(slot->pendingTranscriptPath);
                    if (it != pendingEncodes.end()) {
                        it->second.transcriptionDone = true;
                        it->second.transcript        = transcriptForM4A;
                        if (it->second.playbackDone) {
                            canEncode        = true;
                            encodePath       = it->first;
                            encodeTranscript = it->second.transcript;
                            encodeSnrDb      = it->second.avgSnrDb;
                            pendingEncodes.erase(it);
                        }
                    }
                }
                if (canEncode) triggerEncode(encodePath, encodeTranscript, encodeSnrDb);
            }
        }
    }
#endif

    void managementThreadFunc() {
        while (mgmtRunning) {
            std::unique_lock<std::mutex> ulck(mgmtWaitMtx);
            mgmtCv.wait_for(ulck, std::chrono::milliseconds(250));
            if (!mgmtRunning) { break; }

            auto now = std::chrono::steady_clock::now();

#ifdef __APPLE__
            pollTranscriptions();
#endif

            if (bookmarkScanMode) {
                bool anySignal = manageBookmarkScanChannels();
                if (!bookmarkScanStops.empty()) {
                    if (anySignal) {
                        bookmarkScanHadSignal = true;
                        lastSignalTime = now;
                    } else {
                        float elapsed = std::chrono::duration<float>(now - lastSignalTime).count();
                        float timeout = bookmarkScanHadSignal ? scanQuietSec : scanNoSignalSec;
                        if (elapsed >= timeout) {
                            // Destroy all current slots before retuning
                            {
                                std::lock_guard<std::mutex> clck(channelsMtx);
                                for (auto& [idx, slot] : activeChannels) {
                                    destroySlot(*slot);
                                    delete slot;
                                }
                                activeChannels.clear();
                            }
                            bookmarkScanStopIdx = (bookmarkScanStopIdx + 1) % (int)bookmarkScanStops.size();
                            bookmarkScanHadSignal = false;
                            double nextCenter = bookmarkScanStops[bookmarkScanStopIdx].centerHz;
                            flog::info("[ChannelBank] BkScan: advancing to stop {0} at {1:.3f}MHz",
                                       bookmarkScanStopIdx, nextCenter / 1e6);
                            gui::waterfall.setCenterFrequency(nextCenter);
                            gui::waterfall.centerFreqMoved = true;
                            lastSignalTime = now;
                        }
                    }
                }
                continue;
            }

            if (manualMode) { manageManualChannels(); continue; }

            std::set<int>         current;
            std::set<int>         localRawDetected;
            std::map<int, double> localPeakOffsets;
            std::vector<float>    localSnrDb;
            {
                std::lock_guard<std::mutex> lck(detectedMtx);
                current          = detectedSlots;
                localRawDetected = rawDetectedSlots;
                localPeakOffsets = slotPeakOffsets;
                localSnrDb       = slotSnrDb;
            }

            std::lock_guard<std::mutex> clck(channelsMtx);

            // Create or refresh channels for detected slots
            debugDetectedCount.store((int)current.size());
            int blkSkip = 0, capSkip = 0;
            for (int idx : current) {
                auto it = activeChannels.find(idx);
                if (it == activeChannels.end()) {
                    // New signal — spawn channel (if under cap and not blocked)
                    if ((int)activeChannels.size() >= maxChannels) { capSkip++; continue; }
                    int    numSlots   = (int)std::floor(lastKnownSr / channelSpacing);
                    double slotOffset = ((double)idx - (double)(numSlots - 1) / 2.0) * channelSpacing;
                    auto   pit        = localPeakOffsets.find(idx);
                    double peakOffHz  = (pit != localPeakOffsets.end()) ? pit->second : slotOffset;
                    double slotFreq   = lastKnownCenter + slotOffset;
                    if (!isInActiveSpan(slotFreq)) continue;
                    if (isBlocked(slotFreq)) { blkSkip++; continue; }
                    flog::info("[ChannelBank] Spawning slot {0} at {1:.3f}MHz", idx, slotFreq / 1e6);
                    auto* slot = new ChannelSlot();
                    slot->lastDetected      = now;
                    slot->signalPresent     = true;
                    slot->rawSignalPresent  = true;
                    initSlot(*slot, idx, numSlots, peakOffHz);
                    activeChannels[idx] = slot;
                }
                else {
                    it->second->lastDetected  = now;
                    it->second->signalPresent = true;
                    // rawSignalPresent is maintained exclusively by the DSP thread
                    // (analyzeSpectrum NMS pass) with 2-consecutive-miss logic.
                    // Accumulate SNR while the slot is actively recording
                    if (it->second->fileOpen && idx < (int)localSnrDb.size()) {
                        it->second->snrSum   += localSnrDb[idx];
                        it->second->snrCount += 1;
                    }
                }
            }
            debugBlockedSkips.store(blkSkip);
            debugCapSkips.store(capSkip);

            // Mark channels no longer detected; destroy once file is closed
            for (auto it = activeChannels.begin(); it != activeChannels.end(); ) {
                auto* slot = it->second;

                // Immediately tear down blocked channels
                if (isBlocked(slot->freqHz)) {
                    flog::info("[ChannelBank] Destroying blocked slot {0}", it->first);
                    destroySlot(*slot);
                    delete slot;
                    it = activeChannels.erase(it);
                    continue;
                }

                // Tear down channels outside the active span trim
                if (!isInActiveSpan(slot->freqHz)) {
                    flog::info("[ChannelBank] Destroying out-of-span slot {0}", it->first);
                    destroySlot(*slot);
                    delete slot;
                    it = activeChannels.erase(it);
                    continue;
                }

                bool detected   = current.count(it->first) > 0;
                bool rawPresent = slot->rawSignalPresent.load();
                // Update lastDetected from rawSignalPresent too (see manual/bkscan comment).
                if (detected || rawPresent) slot->lastDetected = now;
                {
                    float holdElapsed = std::chrono::duration<float>(now - slot->lastDetected).count();
                    // Boost hold for long active recordings — once a transmission has been going
                    // for >2s we know it's real, so protect against momentary SNR dropouts by
                    // using at least 2 s of hold regardless of signalHoldMs setting.
                    float recDurSec = slot->fileOpen
                        ? std::chrono::duration<float>(now - slot->fileOpenTime).count() : 0.0f;
                    int effectiveHoldMs = (recDurSec > 2.0f)
                        ? std::max(signalHoldMs, 2000) : signalHoldMs;
                    slot->signalPresent = detected || rawPresent || (holdElapsed * 1000.0f < (float)effectiveHoldMs);
                }
                // rawSignalPresent maintained exclusively by the DSP thread (NMS pass).
                if (!detected) {
                    float elapsed = std::chrono::duration<float>(
                        now - slot->lastDetected).count();
                    bool isPlaying = (currentlyPlayingFreqKey.load() == freqKey(slot->freqHz));
                    bool isQueued  = false;
                    {
                        std::lock_guard<std::mutex> plk(playbackMtx);
                        int64_t fk = freqKey(slot->freqHz);
                        for (auto& entry : playbackQueue)
                            if (freqKey(entry.freqHz) == fk) { isQueued = true; break; }
                    }
                    if (elapsed > cooldownSec && !slot->fileOpen && !isPlaying && !isQueued) {
                        flog::info("[ChannelBank] Destroying slot {0}", it->first);
                        destroySlot(*slot);
                        delete slot;
                        it = activeChannels.erase(it);
                        continue;
                    }
                }
                ++it;
            }

            // Scan mode: advance to next stop once the band has been quiet long enough
            if (scanMode && !scanStops.empty()) {
                bool anyActive = false;
                for (auto& [idx, slot] : activeChannels)
                    if (slot->signalPresent || slot->fileOpen) { anyActive = true; break; }
                if (anyActive) {
                    scanStopHadSignal = true;
                    lastSignalTime = now;
                } else {
                    float elapsed = std::chrono::duration<float>(now - lastSignalTime).count();
                    float timeout = scanStopHadSignal ? scanQuietSec : scanNoSignalSec;
                    if (elapsed >= timeout) {
                        scanStopIdx = (scanStopIdx + 1) % (int)scanStops.size();
                        scanStopHadSignal = false;
                        flog::info("[ChannelBank] Scan: advancing to stop {0} at {1:.3f}MHz",
                                   scanStopIdx, scanStops[scanStopIdx] / 1e6);
                        gui::waterfall.setCenterFrequency(scanStops[scanStopIdx]);
                        gui::waterfall.centerFreqMoved = true;
                        lastSignalTime = now;
                    }
                }
            }
        }
    }

    // exactOffsetHz: when not NaN, overrides the grid-based offset calculation and
    // disables spectral-centroid / BFO adjustment (used by manual mode).
    void initSlot(ChannelSlot& slot, int gridIdx, int numSlots, double peakOffsetHz, double exactOffsetHz = NAN) {
        slot.module  = this;
        slot.gridIdx = gridIdx;

        bool isManual = !std::isnan(exactOffsetHz);
        // Auto mode: use the spectral centroid (peakOffsetHz) as the channel frequency.
        // The centroid tracks the actual carrier position, which is typically NOT exactly
        // at the grid slot center — the SDR tuning offset, transmitter frequency error, or
        // a mismatch between the grid spacing and the actual channel plan (e.g. airband
        // 8.33 kHz channels relative to an SDR tuned to a non-grid-aligned frequency) all
        // shift the carrier away from the computed slot center.  Using the grid center for
        // slot.freqHz causes the waterfall dot and sidebar frequency label to show the wrong
        // position relative to the visible carrier peak, making it appear the module grabbed
        // the "wrong" channel.  Using the centroid here keeps dot, label, blocking check,
        // and VFO all anchored to the actual carrier.
        //
        // Clamp to ±channelSpacing/2 of the grid center so a noisy centroid (e.g. during
        // a very brief burst) cannot drift the channel into an adjacent slot's territory.
        // In manual mode, use the caller-supplied exact offset.
        const double gridOffset = ((double)gridIdx - (double)(numSlots - 1) / 2.0) * channelSpacing;
        double offset = isManual
            ? exactOffsetHz
            : std::clamp(peakOffsetHz, gridOffset - channelSpacing * 0.5, gridOffset + channelSpacing * 0.5);
        slot.freqHz   = lastKnownCenter + offset;

        char freqBuf[64];
        snprintf(freqBuf, sizeof(freqBuf), "%.3fMHz", slot.freqHz / 1e6);
        slot.streamName = name + "_" + freqBuf;

        const double audioSr = 48000.0;
        // Full channel width — ensures both sidebands are captured even if the
        // carrier is slightly off-grid (SDR frequency offset or off-freq transmitter)
        const double bw      = channelSpacing;

        // For SSB in auto mode: use the spectral centroid with BFO trim applied.
        // In manual mode: compensate for the SSB demodulator's internal ±ssbBw/2 xlator
        // (getTranslation() returns +bw/2 for USB, -bw/2 for LSB).  The VFO must be
        // placed at carrier ∓ ssbBw/2 so after the xlator the carrier lands at DC.
        const double ssbBw  = 2800.0;  // standard SSB voice bandwidth
        // VFO placement (consistent with slot.freqHz above — both centroid-based in auto mode):
        //   Manual mode: caller supplies the exact offset (already correct for SSB sideband shift).
        //   Auto, SSB: centroid with user BFO trim applied.
        //   Auto, AM/NFM/WFM: centroid directly (symmetric energy → centroid ≈ carrier).
        const double vfoOff = isManual
            ? (demodMode == DEMOD_USB ? offset + ssbBw / 2.0
             : demodMode == DEMOD_LSB ? offset - ssbBw / 2.0
             : offset)
            : (demodMode == DEMOD_USB || demodMode == DEMOD_LSB)
                ? peakOffsetHz - (double)ssbBfoHz
                : peakOffsetHz;  // centroid: centers VFO on actual carrier, not grid slot

        slot.iqIn = new dsp::stream<dsp::complex_t>();
        iqSplitter->bindStream(slot.iqIn);
        slot.vfo  = new dsp::channel::RxVFO(slot.iqIn, lastKnownSr, audioSr, bw, vfoOff);

        // AM demod bandwidth = full channel width (same as VFO), matching SDR++ radio module.
        // SSB/FM use narrower audio bandwidth.
        const double audioBw = bw / 2.0;
        if (demodMode == DEMOD_AM) {
            slot.amDemod = new dsp::demod::AM<dsp::stereo_t>();
            slot.amDemod->init(&slot.vfo->out,
                dsp::demod::AM<dsp::stereo_t>::AGCMode::CARRIER,
                bw, 50.0 / audioSr, 5.0 / audioSr, 100.0 / audioSr, audioSr);
        }
        else if (demodMode == DEMOD_USB || demodMode == DEMOD_LSB) {
            auto ssbMode = (demodMode == DEMOD_USB)
                ? dsp::demod::SSB<dsp::stereo_t>::Mode::USB
                : dsp::demod::SSB<dsp::stereo_t>::Mode::LSB;
            slot.ssbDemod = new dsp::demod::SSB<dsp::stereo_t>();
            slot.ssbDemod->init(&slot.vfo->out, ssbMode, ssbBw, audioSr, 0.001, 0.00001);
        }
        else {
            double demodBw = (demodMode == DEMOD_WFM) ? 150000.0 : audioBw;
            slot.fmDemod = new dsp::demod::FM<dsp::stereo_t>();
            slot.fmDemod->init(&slot.vfo->out, audioSr, demodBw, true, false);
        }
        dsp::stream<dsp::stereo_t>* demodOut =
            slot.amDemod  ? &slot.amDemod->out  :
            slot.ssbDemod ? &slot.ssbDemod->out :
                            &slot.fmDemod->out;

        slot.splitter = new dsp::routing::Splitter<dsp::stereo_t>(demodOut);
        slot.splitter->bindStream(&slot.meterStream);
        slot.recFeedStream = new dsp::stream<dsp::stereo_t>();
        slot.splitter->bindStream(slot.recFeedStream);

        slot.meter = new dsp::bench::PeakLevelMeter<dsp::stereo_t>(&slot.meterStream);

        slot.recSink = new dsp::sink::Handler<dsp::stereo_t>(
            slot.recFeedStream, audioHandler, &slot);
        slot.writer.setFormat(wav::FORMAT_WAV);
        slot.writer.setChannels(1);
        slot.writer.setSampleType(wav::SAMP_TYPE_INT16);
        slot.writer.setSamplerate((uint64_t)audioSr);

        // RNNoise noise reduction (per-slot)
        if (noiseReduction) {
            slot.nrState = rnnoise_create(nullptr);
            slot.nrInPos = 0;
        }

        slot.vfo->start();
        if (slot.amDemod)  slot.amDemod->start();
        if (slot.fmDemod)  slot.fmDemod->start();
        if (slot.ssbDemod) slot.ssbDemod->start();
        slot.splitter->start();
        slot.meter->start();
        slot.recSink->start();

    }

    void destroySlot(ChannelSlot& slot) {
        // Register in the sticky-recent list (caller always holds channelsMtx).
        recentChannels.push_back({slot.freqHz, std::chrono::steady_clock::now()});

        slot.recSink->stop();
        slot.meter->stop();
        slot.splitter->stop();
        if (slot.amDemod)  slot.amDemod->stop();
        if (slot.fmDemod)  slot.fmDemod->stop();
        if (slot.ssbDemod) slot.ssbDemod->stop();
        slot.vfo->stop();

        iqSplitter->unbindStream(slot.iqIn);

        if (slot.fileOpen) {
            slot.writer.close();
            slot.fileOpen = false;
            if (slot.module) {
                // Same time-based signal duration as the normal close path — immune to
                // signalHoldMs inflating the sample count (see comment there for details).
                int64_t signalMs = std::max(int64_t(0),
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        slot.lastDetected - slot.fileOpenTime).count());
                // Min TX on cumulative on-air time (see normal close path) — discards
                // bursty data (ACARS) even when bridged into a long span.
                int64_t onAirMs = (int64_t)(slot.onAirFrames.load() * (1000.0 / SPEC_ANALYSIS_HZ));
                // Same static gate as the normal close path — don't keep a flat-spectrum
                // (carrier-less) recording just because it was torn down mid-flight.
                int   gAbove    = slot.gateFramesAbove.load();
                int   gVoice    = slot.gateFramesVoice.load();
                float voiceFrac = (gAbove > 0) ? (float)gVoice / (float)gAbove : 1.0f;
                bool  staticReject = slot.module->staticGateEnabled
                                     && gAbove >= slot.module->staticGateMinFrames
                                     && voiceFrac < slot.module->staticGateVoiceFrac;
                // Same drift gate as the normal close path — a torn-down recording of a
                // drifting/faulty carrier shouldn't be kept either.
                double dMean    = (gAbove > 0) ? slot.driftSum / (double)gAbove : 0.0;
                double dVar     = (gAbove > 0) ? std::max(0.0, slot.driftSumSq / (double)gAbove - dMean * dMean) : 0.0;
                float  driftStd = (float)sqrt(dVar);
                bool   driftReject = slot.module->driftGateEnabled
                                     && gAbove >= slot.module->staticGateMinFrames
                                     && driftStd > slot.module->driftMaxStdHz;
                if (staticReject || driftReject || onAirMs < (int64_t)slot.module->minTransmissionMs) {
                    std::remove(slot.currentFilePath.c_str());
                } else {
                    // Recording meets the minimum duration — process it even though
                    // it was cut short by a stop/scan-advance/block rather than by
                    // the normal silence-tail path.
                    normalizeWavFile(slot.currentFilePath);
                    if (slot.module->m4aEnabled && slot.module->recordingEnabled
                            && slot.module->encodeThreadRunning.load()) {
                        // Queue for direct encoding (no playback, no transcription).
                        float avgSnrDb = (slot.snrCount > 0)
                            ? slot.snrSum / (float)slot.snrCount : 0.0f;
                        slot.module->triggerEncode(slot.currentFilePath, {}, avgSnrDb);
                    }
                    // If M4A encoding is off the normalized WAV stays on disk as-is.
                }
            }
        }

        slot.splitter->unbindStream(&slot.meterStream);
        slot.splitter->unbindStream(slot.recFeedStream);

        delete slot.recSink;
        delete slot.meter;
        delete slot.splitter;
        if (slot.amDemod)  { delete slot.amDemod;  slot.amDemod  = nullptr; }
        if (slot.fmDemod)  { delete slot.fmDemod;  slot.fmDemod  = nullptr; }
        if (slot.ssbDemod) { delete slot.ssbDemod; slot.ssbDemod = nullptr; }
        delete slot.vfo;
        delete slot.recFeedStream;
        delete slot.iqIn;
        if (slot.nrState) { rnnoise_destroy(slot.nrState); slot.nrState = nullptr; }
#ifdef __APPLE__
        if (slot.transcribeHandle) {
            txCancel(slot.transcribeBackend, slot.transcribeHandle);
            txDestroy(slot.transcribeBackend, slot.transcribeHandle);
            slot.transcribeHandle = nullptr;
        }
#endif
    }

    // ── Audio handler (split-on-silence recording) ───────────────────────────

    static void audioHandler(dsp::stereo_t* data, int count, void* ctx) {
        ChannelSlot* slot = (ChannelSlot*)ctx;
        ChannelBankModule* _this = slot->module;

        // Discard audio until the AGC has had 200ms of *continuous* signal to
        // settle on.  If the signal drops during warmup the AGC ramps back up,
        // so we reset the clock each time it comes back — no pop on recording start.
        if (slot->warmupSamples > 0) {
            if (!slot->signalPresent.load()) {
                slot->warmupSignalLost = true;
            } else if (slot->warmupSignalLost) {
                slot->warmupSamples    = 9600;   // restart full 200ms on signal return
                slot->warmupSignalLost = false;
            }
            slot->warmupSamples = std::max(0, slot->warmupSamples - count);
            return;
        }

        // Pre-roll: mix stereo→mono and push into the circular buffer.
        // This runs continuously so we always have the last PREROLL_SAMPLES
        // of audio ready to prepend when a file opens.  Runs before the
        // file-open check so the samples that arrived just before detection
        // fired are captured — that's where the call sign lives.
        for (int i = 0; i < count; i++) {
            slot->preRollBuf[slot->preRollHead] = (data[i].l + data[i].r) * 0.5f;
            slot->preRollHead = (slot->preRollHead + 1) % ChannelSlot::PREROLL_SAMPLES;
            if (slot->preRollCount < ChannelSlot::PREROLL_SAMPLES) slot->preRollCount++;
        }

        // Use FFT-based detection rather than audio amplitude.
        // AM/FM demodulators always output noise, so amplitude-based silence
        // detection is unreliable — the FFT already knows if a signal is there.
        //
        // Two flags drive this:
        //   rawSignalPresent – updated in the DSP thread every FFT frame (50 ms).
        //                      Goes TRUE on the very first frame above the SNR
        //                      threshold; goes FALSE only after 2 consecutive misses.
        //                      Fast but has no vote smoothing.
        //   signalPresent    – updated by the management thread every 250 ms.
        //                      Requires SPAWN_VOTES consecutive detections and
        //                      holds TRUE for signalHoldMs after the last detection.
        //                      Slow but stable; provides dropout hysteresis.
        //
        // Using signalPresent alone for file-open caused up to ~400 ms of missed
        // audio per transmission (150 ms vote accumulation + up to 250 ms mgmt-thread
        // polling lag). The call sign is spoken in the first ~500 ms, so this
        // consistently clipped it.
        //
        // rawSignalPresent gives fast detection (first frame above SNR = 50 ms), but
        // a single-frame spike from lightning or a nearby HFDL burst is enough to set
        // it and open a file on adjacent bookmarks.  Requiring ≥ 2 consecutive raw
        // frames (rawConsecutiveHits ≥ 2, i.e. 100 ms of continuous detection) keeps
        // the file-open latency at ~100 ms while filtering out single-frame spikes.
        // The pre-roll buffer recovers this 100ms (and the former 200ms fileTrimSamples
        // discard) by prepending buffered audio when the file opens, so the net latency
        // heard in the recording is ~0ms even though detection takes 100ms to fire.
        // signalPresent still controls the hold/silence-countdown (unchanged).
        bool rawOpen     = (slot->rawSignalPresent.load() && slot->rawConsecutiveHits.load() >= 2);
        bool activeSignal = slot->signalPresent.load() || rawOpen;

        // Hard duration cap: a recording open longer than maxRecordingSec is force-closed
        // regardless of whether the signal is still "present". Persistent come-and-go
        // interference keeps activeSignal latched forever (each burst re-arms it), so this
        // is the only thing that breaks the loop. The closed segment is then judged by the
        // static gate below — static segments are discarded, a genuinely long transmission
        // is kept and simply continues into a fresh file on the next frame.
        bool forceClose = false;
        if (slot->fileOpen && _this->maxRecordingSec > 0.0f) {
            float openSec = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - slot->fileOpenTime).count();
            forceClose = (openSec >= _this->maxRecordingSec);
        }

        if (activeSignal && !forceClose) {
            slot->inSilence = false;
            if (!slot->fileOpen) {
                _this->openNewFile(*slot);
                // Flush pre-roll: write the last PREROLL_SAMPLES of audio that
                // arrived before detection fired.  This recovers the ~100-300ms
                // of transmission that happened before the file opened.
                // Gain is applied; the buffer already contains mixed mono.
                if (slot->preRollCount > 0) {
                    int avail = slot->preRollCount;
                    int startIdx = (slot->preRollHead - avail + ChannelSlot::PREROLL_SAMPLES)
                                   % ChannelSlot::PREROLL_SAMPLES;
                    for (int i = 0; i < avail; i++) {
                        int idx = (startIdx + i) % ChannelSlot::PREROLL_SAMPLES;
                        slot->preRollTmp[i] = std::clamp(
                            slot->preRollBuf[idx] * _this->recGain, -1.0f, 1.0f);
                    }
                    slot->writer.write(slot->preRollTmp.data(), avail);
                    slot->audioSamplesWritten += avail;
                    slot->preRollCount = 0;   // consumed; don't re-write on next call
                }
            }
        }
        else {
            // Signal gone — start 1-second grace before closing file
            if (!slot->inSilence) {
                slot->inSilence    = true;
                slot->silenceStart = std::chrono::steady_clock::now();
            }
            if (slot->fileOpen) {
                auto silenceElapsed = std::chrono::steady_clock::now() - slot->silenceStart;
                if (silenceElapsed >= std::chrono::milliseconds(_this->tailMs) || forceClose) {
                    // Signal duration = time from file-open to the last genuine FFT detection.
                    // Using (audioSamplesWritten - tailSamples) was wrong: the signalHoldMs
                    // grace period kept signalPresent=true and the audio handler writing,
                    // so even a 50ms noise burst appeared as (holdMs) worth of "signal time"
                    // and passed the min-TX check.  lastDetected is only updated when the FFT
                    // actually sees power above the SNR threshold, so it's hold-period-immune.
                    int64_t signalMs = std::max(int64_t(0),
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            slot->lastDetected - slot->fileOpenTime).count());
                    // Min TX is judged on cumulative ON-AIR time (frames the carrier was
                    // actually present), not the open→last-seen span. Intermittent data
                    // bursts (ACARS) get bridged into a long span by the signal-hold, but
                    // their real airtime stays small — so this discards them while keeping
                    // continuous voice. 50 ms per frame @ SPEC_ANALYSIS_HZ.
                    int64_t onAirMs = (int64_t)(slot->onAirFrames.load() * (1000.0 / SPEC_ANALYSIS_HZ));
                    slot->writer.close();
                    slot->fileOpen = false;

                    // Static gate: did this recording's channel show a carrier (voice)
                    // or was it predominantly flat-spectrum (broadband interference)?
                    int   gAbove    = slot->gateFramesAbove.load();
                    int   gVoice    = slot->gateFramesVoice.load();
                    float voiceFrac = (gAbove > 0) ? (float)gVoice / (float)gAbove : 1.0f;
                    bool  staticReject = _this->staticGateEnabled
                                         && gAbove >= _this->staticGateMinFrames
                                         && voiceFrac < _this->staticGateVoiceFrac;

                    // Drift gate: stddev of the carrier centroid over the recording.
                    // Stable carrier → small; drifting/faulty emitter → large.
                    double dMean    = (gAbove > 0) ? slot->driftSum / (double)gAbove : 0.0;
                    double dVar     = (gAbove > 0) ? std::max(0.0, slot->driftSumSq / (double)gAbove - dMean * dMean) : 0.0;
                    float  driftStd = (float)sqrt(dVar);
                    bool   driftReject = _this->driftGateEnabled
                                         && gAbove >= _this->staticGateMinFrames
                                         && driftStd > _this->driftMaxStdHz;

                    if (staticReject) {
                        flog::info("[ChannelBank] Discarding static recording (carrier in {0}/{1} frames = {2:.0f}%% < {3:.0f}%%) slot {4}{5}",
                                   gVoice, gAbove, voiceFrac * 100.0f,
                                   _this->staticGateVoiceFrac * 100.0f, slot->gridIdx,
                                   forceClose ? " [dur-cap]" : "");
                        std::remove(slot->currentFilePath.c_str());
                    } else if (driftReject) {
                        flog::info("[ChannelBank] Discarding drifting recording (carrier drift {0:.0f}Hz > {1:.0f}Hz over {2} frames) slot {3}{4}",
                                   driftStd, _this->driftMaxStdHz, gAbove, slot->gridIdx,
                                   forceClose ? " [dur-cap]" : "");
                        std::remove(slot->currentFilePath.c_str());
                    } else if (onAirMs < (int64_t)_this->minTransmissionMs) {
                        flog::info("[ChannelBank] Discarding short recording (on-air {0}ms < {1}ms threshold; span was {2}ms)", onAirMs, _this->minTransmissionMs, signalMs);
                        std::remove(slot->currentFilePath.c_str());
                    } else {
                        flog::info("[ChannelBank] Keeping recording (on-air {0}ms / span {1}ms, carrier {2}/{3} = {4:.0f}%%, drift {5:.0f}Hz) slot {6}",
                                   onAirMs, signalMs, gVoice, gAbove, voiceFrac * 100.0f, driftStd, slot->gridIdx);
                        normalizeWavFile(slot->currentFilePath);
#ifdef __APPLE__
                        if (_this->transcriptionOn()) {
                            if (slot->transcribeHandle) {
                                _this->txCancel(slot->transcribeBackend, slot->transcribeHandle);
                                _this->txDestroy(slot->transcribeBackend, slot->transcribeHandle);
                            }
                            slot->pendingTranscriptPath = slot->currentFilePath;
                            slot->liveTranscript.clear();
                            slot->transcribeBackend = _this->transcriptionBackend;
                            slot->transcribeHandle  = _this->txTranscribeFile(
                                slot->transcribeBackend,
                                slot->currentFilePath.c_str());
                        }
                        // Register for M4A encoding after playback+transcription both complete.
                        // transcriptionDone=true when transcription is off or failed to start,
                        // so encoding fires immediately after playback in those cases.
                        if (_this->m4aEnabled && _this->recordingEnabled && !slot->currentFilePath.empty()) {
                            float avgSnrDb = (slot->snrCount > 0)
                                ? slot->snrSum / (float)slot->snrCount : 0.0f;
                            std::lock_guard<std::mutex> elk(_this->pendingEncodesMtx);
                            EncodeState& es = _this->pendingEncodes[slot->currentFilePath];
                            es.playbackDone      = false;
                            es.transcriptionDone = (!_this->transcriptionOn() || !slot->transcribeHandle);
                            es.avgSnrDb          = avgSnrDb;
                        }
#endif
                        // Log the frequency and queue for playback
                        _this->logRecording(slot->freqHz);
                        _this->saveFreqLog();
                        if (!slot->currentFilePath.empty()) {
                            std::lock_guard<std::mutex> lk(_this->playbackMtx);
                            // deleteAfter=true when recording is disabled — play it back but don't keep the file
                            _this->playbackQueue.push_back({slot->currentFilePath, slot->freqHz, !_this->recordingEnabled});
                            _this->playbackCv.notify_one();
                        }
                    }
                }
            }
            if (!slot->fileOpen) { return; }
        }
        if (!slot->fileOpen) { return; }

        // Discard the first 200ms after file open — by that point the AGC has
        // settled on the carrier so there's no spike written into the file.
        if (slot->fileTrimSamples > 0) {
            slot->fileTrimSamples = std::max(0, slot->fileTrimSamples - count);
            return;
        }

        // Apply recording gain + fade-in + fade-out, then mix stereo down to mono in-place.
        // AM output is identical on L and R so averaging is lossless; it also
        // halves the file size with no audible difference.
        const int totalFade = 4800;

        // Fade-out: raised-cosine from 1→0 driven by signal absence.
        //
        // The fade only begins when BOTH the instantaneous power (rawSignalPresent,
        // 2-frame miss counter) AND the vote accumulator (signalPresent) agree the
        // signal is gone.  Using rawSignalPresent alone caused mid-transmission
        // squelch: USB inter-word pauses (50-300ms) are long enough for the 2-miss
        // counter to fire and begin a 50ms fade, producing audible dropouts.
        //
        // signalPresent (vote-based) holds true through brief pauses — MAX_VOTES=8
        // takes 6 frames (300ms) to drain below SPAWN_VOTES — so it acts as a hold
        // that keeps the audio at full gain while speech is momentarily quiet.
        // The fade only starts once the slot is genuinely idle (votes exhausted AND
        // instantaneous power below threshold).
        //
        // This runs entirely on the DSP thread (no mutex needed) — both flags are
        // atomic and fadeOutRemaining is DSP-thread-only.
        float tailFade = 1.0f;
        // Audio fade is driven by rawSignalPresent plus a short independent hold
        // (200 ms, ~9600 samples).  This hold is separate from signalHoldMs so that
        // AM mode doesn't write AGC ramp-up noise for the entire hold period before
        // the fade kicks in.  200 ms is long enough to bridge SSB inter-word pauses
        // without creating audible dropouts.
        //
        // The file stays open for the full signalHoldMs+tailMs period (driven by
        // signalPresent + the silenceElapsed counter above), but any audio written
        // after the fade completes is digital silence (tailFade = 0.0f).
        const int AUDIO_HOLD_SAMPLES = 9600;  // 200 ms @ 48 kHz
        bool rawAlive = slot->rawSignalPresent.load();
        if (rawAlive) {
            slot->audioHoldRemaining = AUDIO_HOLD_SAMPLES;
        } else if (slot->audioHoldRemaining > 0) {
            slot->audioHoldRemaining = std::max(0, slot->audioHoldRemaining - count);
        }
        bool signalAlive = rawAlive || (slot->audioHoldRemaining > 0);
        if (signalAlive) {
            slot->fadeOutRemaining = 2400;   // hold at max while signal (or hold) is present
        } else {
            if (slot->fadeOutRemaining > 0) {
                // progress: 1.0 (just started fading) → 0.0 (fully faded)
                float progress = (float)slot->fadeOutRemaining / 2400.0f;
                tailFade = 0.5f * (1.0f + cosf(M_PI * (1.0f - progress)));  // 1.0 → 0.0
                slot->fadeOutRemaining = std::max(0, slot->fadeOutRemaining - count);
            } else {
                tailFade = 0.0f;  // fully faded — write silence until file closes
            }
        }

        float* mono = (float*)data;  // safe: mono[i] written before data[i] is needed
        for (int i = 0; i < count; i++) {
            float gain = _this->recGain * tailFade;
            if (slot->recFadeRemaining > 0) {
                // Raised-cosine taper: zero slope at both ends — prevents onset pop
                float progress = 1.0f - (float)slot->recFadeRemaining / totalFade;
                gain *= 0.5f * (1.0f - cosf(M_PI * progress));
                slot->recFadeRemaining--;
            }
            mono[i] = std::clamp((data[i].l + data[i].r) * 0.5f * gain, -1.0f, 1.0f);
        }

        // RNNoise processing — accumulate into 480-sample frames, process,
        // and write each completed frame to WAV individually.
        // RNNoise expects/returns samples in int16 range (-32768..32767).
        if (slot->nrState) {
            int pos = 0;
            while (pos < count) {
                int room = 480 - slot->nrInPos;
                int take = std::min(room, count - pos);
                for (int i = 0; i < take; i++)
                    slot->nrInBuf[slot->nrInPos + i] = mono[pos + i] * 32768.0f;
                slot->nrInPos += take;
                pos += take;

                if (slot->nrInPos >= 480) {
                    float outBuf[480];
                    rnnoise_process_frame(slot->nrState, outBuf, slot->nrInBuf);
                    // Blend original (dry) and denoised (wet) based on nrMix
                    float mix = _this->nrMix;
                    float nrMono[480];
                    for (int i = 0; i < 480; i++) {
                        float dry = slot->nrInBuf[i] / 32768.0f;
                        float wet = outBuf[i] / 32768.0f;
                        nrMono[i] = std::clamp(dry * (1.0f - mix) + wet * mix, -1.0f, 1.0f);
                    }
                    slot->writer.write(nrMono, 480);
                    slot->audioSamplesWritten += 480;
                    slot->nrInPos = 0;
                }
            }
        } else {
            slot->writer.write(mono, count);
            slot->audioSamplesWritten += count;
        }
    }

    // ── Playback monitor ─────────────────────────────────────────────────────

    void triggerEncode(const std::string& wavPath, const std::string& transcript = {}, float avgSnrDb = 0.0f) {
        std::lock_guard<std::mutex> lk(encodeQueueMtx);
        encodeQueue.push_back({wavPath, transcript, avgSnrDb});
        encodeQueueCv.notify_one();
    }

    void encodeThreadFunc() {
        while (true) {
            EncodeTask task;
            {
                std::unique_lock<std::mutex> lk(encodeQueueMtx);
                encodeQueueCv.wait(lk, [this] {
                    return !encodeQueue.empty() || !encodeThreadRunning.load();
                });
                if (!encodeThreadRunning.load() && encodeQueue.empty()) break;
                if (!encodeQueue.empty()) {
                    task = encodeQueue.front();
                    encodeQueue.pop_front();
                }
            }
            if (!task.wavPath.empty()) {
#ifdef __APPLE__
                auto result = encoding::wavToM4A(task.wavPath, task.transcript, task.avgSnrDb);
                if (result.empty())
                    flog::error("[ChannelBank] M4A encoding failed: {0}", task.wavPath);
                else
                    flog::info("[ChannelBank] Encoded: {0}", result);
#endif
            }
        }
    }

    void playbackThreadFunc() {
        const int CHUNK = 1024;
        std::vector<dsp::stereo_t> silence(CHUNK);
        memset(silence.data(), 0, CHUNK * sizeof(dsp::stereo_t));

        while (playbackRunning) {
            std::string path;
            double      playFreq    = 0.0;
            bool        deleteAfter = false;
            {
                std::lock_guard<std::mutex> lk(playbackMtx);
                if (!playbackQueue.empty()) {
                    path        = playbackQueue.front().path;
                    playFreq    = playbackQueue.front().freqHz;
                    deleteAfter = playbackQueue.front().deleteAfter;
                    playbackQueue.pop_front();
                }
            }

            if (!path.empty()) {
#ifdef __APPLE__
                // Install synced-playback state for THIS file before playback
                // starts, so the UI thread can highlight whichever segment the
                // playback cursor is in.  If no segments were stashed (Apple
                // Speech, transcription off, or transcription not yet finished
                // by the time playback reaches the front of the queue), the
                // overlay just doesn't render — graceful degradation.
                {
                    std::vector<transcription_whisper::Segment> segs;
                    {
                        std::lock_guard<std::mutex> sk(pendingPlaybackSegmentsMtx);
                        auto it = pendingPlaybackSegments.find(path);
                        if (it != pendingPlaybackSegments.end()) {
                            segs = std::move(it->second);
                            pendingPlaybackSegments.erase(it);
                        }
                    }
                    std::lock_guard<std::mutex> tlk(lastTranscriptMtx);
                    playingSegments = std::move(segs);
                }
                playbackPosMs.store(0);
#endif
                currentlyPlayingFreqKey.store(freqKey(playFreq));
                playbackWavFile(path);
                currentlyPlayingFreqKey.store(0);
#ifdef __APPLE__
                playbackPosMs.store(-1);
                // Leave playingSegments intact for a moment so the user can read
                // the final transcript — it's cleared on the next playback start.
#endif
                if (deleteAfter) {
                    std::remove(path.c_str());
                }
#ifdef __APPLE__
                else if (m4aEnabled) {
                    // Playback done — check if transcription is also complete
                    bool        canEncode = false;
                    std::string encodeTranscript;
                    float       encodeSnrDb = 0.0f;
                    {
                        std::lock_guard<std::mutex> lk(pendingEncodesMtx);
                        auto it = pendingEncodes.find(path);
                        if (it != pendingEncodes.end()) {
                            it->second.playbackDone = true;
                            if (it->second.transcriptionDone) {
                                canEncode        = true;
                                encodeTranscript = it->second.transcript;
                                encodeSnrDb      = it->second.avgSnrDb;
                                pendingEncodes.erase(it);
                            }
                        }
                    }
                    if (canEncode) triggerEncode(path, encodeTranscript, encodeSnrDb);
                }
#endif
            } else {
                // Write silence to keep monitorStream continuously flowing.
                // swap() naturally throttles to the consumer's 48 kHz read rate.
                memcpy(monitorStream.writeBuf, silence.data(), CHUNK * sizeof(dsp::stereo_t));
                if (!monitorStream.swap(CHUNK)) { return; }
            }
        }
    }

    void playbackWavFile(const std::string& path) {
        // Write one silence chunk before opening the file so the file I/O
        // happens while the consumer processes audio — prevents underrun pop.
        const int PREBUF = 1024;
        memset(monitorStream.writeBuf, 0, PREBUF * sizeof(dsp::stereo_t));
        if (!monitorStream.swap(PREBUF)) { return; }

        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) { return; }

        // Parse minimal WAV header to find data start
        char riff[4]; f.read(riff, 4);
        if (std::string(riff, 4) != "RIFF") { return; }
        uint32_t fileSize; f.read((char*)&fileSize, 4);
        char wave[4]; f.read(wave, 4);
        if (std::string(wave, 4) != "WAVE") { return; }

        // Walk chunks — parse fmt for channel count, find data
        uint16_t fmtChannels = 2;
        uint32_t dataOffset = 0, dataSize = 0;
        while (!f.eof()) {
            char id[4]; f.read(id, 4);
            uint32_t chunkSize; f.read((char*)&chunkSize, 4);
            if (f.fail()) { break; }
            if (std::string(id, 4) == "fmt ") {
                uint16_t codec, numCh; f.read((char*)&codec, 2); f.read((char*)&numCh, 2);
                fmtChannels = numCh;
                if (chunkSize > 4) f.seekg(chunkSize - 4, std::ios::cur);
            } else if (std::string(id, 4) == "data") {
                dataOffset = (uint32_t)f.tellg();
                dataSize   = chunkSize;
                break;
            } else {
                f.seekg(chunkSize, std::ios::cur);
            }
        }
        if (dataSize == 0) { return; }

        // Read int16 (mono or stereo) → float stereo_t, write to monitorStream in chunks
        const int CHUNK       = 1024;
        const int FADE_SAMPS  = 240;   // ~5ms fade at 48 kHz
        const int bytesPerFrame = fmtChannels * (int)sizeof(int16_t);
        const int totalSamps  = (int)(dataSize / bytesPerFrame);
        std::vector<int16_t>       pcm(CHUNK * fmtChannels);
        std::vector<dsp::stereo_t> buf(CHUNK);

        uint32_t remaining = dataSize;
        int      samplesRead = 0;
        while (remaining > 0 && playbackRunning) {
            uint32_t toRead = std::min(remaining, (uint32_t)(CHUNK * bytesPerFrame));
            f.read((char*)pcm.data(), toRead);
            int bytesRead = (int)f.gcount();
            if (bytesRead <= 0) { break; }
            int samples = bytesRead / bytesPerFrame;
            for (int i = 0; i < samples; i++) {
                float gain = 1.0f;
                int absIdx  = samplesRead + i;
                int fromEnd = totalSamps - absIdx;
                if (absIdx  < FADE_SAMPS) gain = (float)absIdx  / FADE_SAMPS;
                if (fromEnd < FADE_SAMPS) gain = std::min(gain, (float)fromEnd / FADE_SAMPS);
                if (fmtChannels == 1) {
                    float s = (pcm[i] / 32768.0f) * gain;
                    buf[i].l = buf[i].r = s;
                } else {
                    buf[i].l = (pcm[i * 2]     / 32768.0f) * gain;
                    buf[i].r = (pcm[i * 2 + 1] / 32768.0f) * gain;
                }
            }
            memcpy(monitorStream.writeBuf, buf.data(), samples * sizeof(dsp::stereo_t));
            if (!monitorStream.swap(samples)) { break; }
            remaining    -= bytesRead;
            samplesRead  += samples;
#ifdef __APPLE__
            // Publish the playback cursor for the synced-transcript overlay.
            // The WAV is 48 kHz so ms = samples * 1000 / 48000.  Atomic store —
            // UI thread reads without locking.  Updated once per CHUNK (~21ms
            // @ 48 kHz), plenty fast enough for UI sync.
            playbackPosMs.store(int((int64_t)samplesRead * 1000 / 48000));
#endif
        }
    }

    // ── Main-waterfall overlay ───────────────────────────────────────────────
    // Draws a marker on the SDR++ main waterfall for each active channel:
    //   • red filled dot    = signal on air RIGHT NOW (rawSignalPresent); turns
    //                         off within ~100ms of signal end
    //   • orange filled dot = file open but signal gone (hold/tail recording);
    //                         disappears when tailMs elapses and the file closes
    //   • orange ring       = monitoring channel, idle
    // "Recording: N" in the overlay counts channels with a live signal.

    static void fftRedrawHandlerFunc(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        ChannelBankModule* _this = (ChannelBankModule*)ctx;
        if (!_this->enabled || !_this->running) { return; }

        // Snapshot active-channel state under lock; minimise work inside the lock.
        int64_t playingKey = _this->currentlyPlayingFreqKey.load();
        struct Mark { double freq; bool liveSignal; bool recording; bool playing; };
        std::vector<Mark> marks;
        int  recCount              = 0;
        bool playingKeyAccountedFor = false;
        {
            std::lock_guard<std::mutex> clck(_this->channelsMtx);
            marks.reserve(_this->activeChannels.size());
            for (auto& [idx, slot] : _this->activeChannels) {
                bool live = slot->rawSignalPresent.load();
                bool rec  = slot->fileOpen;
                if (live) recCount++;
                bool play = (playingKey != 0 && _this->freqKey(slot->freqHz) == playingKey);
                if (play) playingKeyAccountedFor = true;
                marks.push_back({ slot->freqHz, live, rec, play });
            }
        }

        ImDrawList* dl = args.window->DrawList;
        const float radius       = 5.0f;
        const float diamondR     = 7.0f;
        const float diamondYOff  = radius + diamondR + 4.0f; // stack diamond above circle
        float y = args.max.y - 12.0f;

        // Helper: draw a green diamond (playback indicator)
        auto drawPlayDiamond = [&](float cx, float cy) {
            ImVec2 top  (cx,            cy - diamondR);
            ImVec2 right(cx + diamondR, cy);
            ImVec2 bot  (cx,            cy + diamondR);
            ImVec2 left (cx - diamondR, cy);
            dl->AddQuadFilled(top, right, bot, left, IM_COL32(40, 210, 80, 255));
            dl->AddQuad      (top, right, bot, left, IM_COL32(255, 255, 255, 255), 1.5f);
        };

        for (auto& m : marks) {
            if (m.freq < args.lowFreq || m.freq > args.highFreq) continue;
            double x = args.min.x + (m.freq - args.lowFreq) * args.freqToPixelRatio;
            ImVec2 c((float)x, y);

            if (m.liveSignal) {
                // Bright red filled dot — signal on air RIGHT NOW (rawSignalPresent).
                // Turns off within ~100ms of signal end.
                dl->AddCircleFilled(c, radius, IM_COL32(255, 60, 60, 255), 16);
                dl->AddCircle(c, radius + 1.0f, IM_COL32(255, 255, 255, 255), 16, 1.5f);
            } else if (m.recording) {
                // Orange filled dot — file still open (signal hold / tail recording).
                // Signal is gone but we're still writing the post-signal audio.
                // Goes away when tailMs elapses and the file closes.
                dl->AddCircleFilled(c, radius, IM_COL32(255, 140, 30, 255), 16);
                dl->AddCircle(c, radius + 1.0f, IM_COL32(255, 255, 255, 255), 16, 1.5f);
            } else {
                // Orange ring — monitoring channel, idle (no active signal or recording)
                dl->AddCircle(c, radius, IM_COL32(255, 165, 0, 255), 16, 2.0f);
            }

            // Green diamond stacked above the circle when this channel is playing back
            if (m.playing) {
                drawPlayDiamond((float)x, y - diamondYOff);
            }
        }

        // Standalone diamond for playback at a freq whose channel was already torn down
        if (playingKey != 0 && !playingKeyAccountedFor) {
            double playFreq = (double)playingKey * 1000.0;
            if (playFreq >= args.lowFreq && playFreq <= args.highFreq) {
                double x = args.min.x + (playFreq - args.lowFreq) * args.freqToPixelRatio;
                drawPlayDiamond((float)x, y);
            }
        }

        // Counter badge in top-right of the FFT area.
        char buf[64];
        snprintf(buf, sizeof(buf), "Rec %d / Act %d", recCount, (int)marks.size());
        ImVec2 textSize = ImGui::CalcTextSize(buf);
        ImVec2 padMin(args.max.x - textSize.x - 12.0f, args.min.y + 4.0f);
        ImVec2 padMax(args.max.x - 4.0f,               args.min.y + textSize.y + 8.0f);
        dl->AddRectFilled(padMin, padMax, IM_COL32(0, 0, 0, 160), 3.0f);
        dl->AddText(ImVec2(padMin.x + 4.0f, padMin.y + 2.0f),
                    IM_COL32(255, 255, 255, 255), buf);

        // SNR threshold line — horizontal line at the detection threshold dB level.
        // Only drawn once the noise floor is calibrated (displayNoiseFloor > 0).
        float nf = _this->displayNoiseFloor;
        if (nf > 0.0f && _this->running) {
            float snrLinear = powf(10.0f, _this->snrThreshold / 10.0f);
            float threshDb  = 10.0f * log10f(nf * snrLinear);
            float fftMin    = gui::waterfall.getFFTMin();
            float fftMax    = gui::waterfall.getFFTMax();
            if (fftMax > fftMin && threshDb >= fftMin && threshDb <= fftMax) {
                float fftH    = args.max.y - args.min.y;
                float threshY = args.min.y + (fftMax - threshDb) / (fftMax - fftMin) * fftH;

                // Dashed line: alternate 8px drawn / 6px gap across full width
                const float dashLen = 8.0f, gapLen = 6.0f;
                float x = args.min.x;
                bool  draw = true;
                while (x < args.max.x) {
                    float segEnd = std::min(x + (draw ? dashLen : gapLen), args.max.x);
                    if (draw)
                        dl->AddLine(ImVec2(x, threshY), ImVec2(segEnd, threshY),
                                    IM_COL32(255, 200, 50, 180), 1.0f);
                    x = segEnd;
                    draw = !draw;
                }

                // Label at the left edge
                char threshBuf[32];
                snprintf(threshBuf, sizeof(threshBuf), "%.0f dB", threshDb);
                ImVec2 lblSize = ImGui::CalcTextSize(threshBuf);
                float  lblX    = args.min.x + 4.0f;
                float  lblY    = threshY - lblSize.y - 2.0f;
                dl->AddRectFilled(ImVec2(lblX - 2, lblY - 1),
                                  ImVec2(lblX + lblSize.x + 2, lblY + lblSize.y + 1),
                                  IM_COL32(0, 0, 0, 140), 2.0f);
                dl->AddText(ImVec2(lblX, lblY), IM_COL32(255, 200, 50, 255), threshBuf);
            }
        }

        // Active span trim bars
        {
            float wfWidth  = (float)(args.max.x - args.min.x);
            float wfHeight = (float)(args.max.y - args.min.y);

            // Convert trim fractions to screen X positions
            float leftBarX  = (float)args.min.x + wfWidth * _this->leftTrimFrac;
            float rightBarX = (float)args.max.x - wfWidth * _this->rightTrimFrac;

            // Grey overlay on excluded zones
            if (_this->leftTrimFrac > 0.001f)
                dl->AddRectFilled(ImVec2((float)args.min.x, (float)args.min.y),
                                  ImVec2(leftBarX, (float)args.max.y),
                                  IM_COL32(0, 0, 0, 90));
            if (_this->rightTrimFrac > 0.001f)
                dl->AddRectFilled(ImVec2(rightBarX, (float)args.min.y),
                                  ImVec2((float)args.max.x, (float)args.max.y),
                                  IM_COL32(0, 0, 0, 90));

            // Bar lines — amber colored
            ImU32 barCol = IM_COL32(255, 180, 0, 220);
            if (_this->leftTrimFrac > 0.001f)
                dl->AddLine(ImVec2(leftBarX, (float)args.min.y),
                            ImVec2(leftBarX, (float)args.max.y), barCol, 2.0f);
            if (_this->rightTrimFrac > 0.001f)
                dl->AddLine(ImVec2(rightBarX, (float)args.min.y),
                            ImVec2(rightBarX, (float)args.max.y), barCol, 2.0f);

            // Drag handles (small triangles at bottom of bars)
            if (_this->leftTrimFrac > 0.001f) {
                float tx = leftBarX, ty = (float)args.max.y - 6.0f;
                dl->AddTriangleFilled(ImVec2(tx, ty), ImVec2(tx-6, ty+6), ImVec2(tx+6, ty+6), barCol);
            }
            if (_this->rightTrimFrac > 0.001f) {
                float tx = rightBarX, ty = (float)args.max.y - 6.0f;
                dl->AddTriangleFilled(ImVec2(tx, ty), ImVec2(tx-6, ty+6), ImVec2(tx+6, ty+6), barCol);
            }

            // Mouse interaction — drag bars
            ImVec2 mp = ImGui::GetMousePos();
            bool inWF = mp.x >= args.min.x && mp.x <= args.max.x &&
                        mp.y >= args.min.y && mp.y <= args.max.y;
            bool lmbDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

            // Release
            if (!lmbDown) {
                if (_this->draggingLeft || _this->draggingRight) {
                    // Save config on release
                    config.acquire();
                    config.conf[_this->name]["leftTrimFrac"]  = _this->leftTrimFrac;
                    config.conf[_this->name]["rightTrimFrac"] = _this->rightTrimFrac;
                    config.release(true);
                }
                _this->draggingLeft  = false;
                _this->draggingRight = false;
            }

            // Update drag
            if (_this->draggingLeft && lmbDown) {
                float newFrac = (mp.x - (float)args.min.x) / wfWidth;
                _this->leftTrimFrac = std::clamp(newFrac, 0.0f, 0.48f);
            }
            if (_this->draggingRight && lmbDown) {
                float newFrac = ((float)args.max.x - mp.x) / wfWidth;
                _this->rightTrimFrac = std::clamp(newFrac, 0.0f, 0.48f);
            }

            // Start drag — only if not already dragging, mouse is in waterfall, and near a bar
            if (!_this->draggingLeft && !_this->draggingRight &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) && inWF) {
                const float hitDist = 10.0f;
                float dLeft  = std::abs(mp.x - leftBarX);
                float dRight = std::abs(mp.x - rightBarX);
                if (dLeft <= hitDist && dLeft < dRight)
                    _this->draggingLeft = true;
                else if (dRight <= hitDist)
                    _this->draggingRight = true;
            }
        }

        // Hover-frequency crosshair — cyan vertical line + label when the user
        // mouses over any row in the active-channel list, recent-channel list,
        // or frequency-history panel.  hoveredFreqHz is reset to 0.0 each frame
        // at the start of menuHandler so it never lingers.
        {
            double hf = _this->hoveredFreqHz;
            if (hf != 0.0 && hf >= args.lowFreq && hf <= args.highFreq) {
                float hx = (float)(args.min.x +
                            (hf - args.lowFreq) * args.freqToPixelRatio);
                dl->AddLine(ImVec2(hx, (float)args.min.y),
                            ImVec2(hx, (float)args.max.y),
                            IM_COL32(0, 220, 255, 180), 1.5f);

                // Small frequency label near the top of the line
                char hfBuf[32];
                snprintf(hfBuf, sizeof(hfBuf), "%.4f MHz", hf / 1e6);
                ImVec2 lblSz = ImGui::CalcTextSize(hfBuf);
                float  lblX  = hx + 4.0f;
                // Flip label to the left if it would overflow the right edge
                if (lblX + lblSz.x > (float)args.max.x - 4.0f)
                    lblX = hx - lblSz.x - 4.0f;
                float lblY = (float)args.min.y + 4.0f;
                dl->AddRectFilled(ImVec2(lblX - 2.0f, lblY - 1.0f),
                                  ImVec2(lblX + lblSz.x + 2.0f, lblY + lblSz.y + 1.0f),
                                  IM_COL32(0, 0, 0, 160), 2.0f);
                dl->AddText(ImVec2(lblX, lblY),
                            IM_COL32(0, 220, 255, 230), hfBuf);
            }
        }
    }

    // ── Retune handler ───────────────────────────────────────────────────────

    static void retuneHandlerFunc(double /*freq*/, void* ctx) {
        ChannelBankModule* _this = (ChannelBankModule*)ctx;
        if (!_this->running) { return; }

        double newSr     = sigpath::iqFrontEnd.getSampleRate();
        double newCenter = gui::waterfall.getCenterFrequency();
        if (newSr == _this->lastKnownSr && newCenter == _this->lastKnownCenter) { return; }

        // Teardown all active channels — safe because destroySlot stops the
        // DSP sinks before freeing, so no audio callback will fire on freed data.
        {
            std::lock_guard<std::mutex> lck(_this->channelsMtx);
            for (auto& [idx, slot] : _this->activeChannels) {
                _this->destroySlot(*slot);
                delete slot;
            }
            _this->activeChannels.clear();
        }
        {
            std::lock_guard<std::mutex> lck(_this->detectedMtx);
            _this->detectedSlots.clear();
        }
        {
            std::lock_guard<std::mutex> lk(_this->manualDetectedMtx);
            _this->manualDetected.clear();
        }

        // Store new params but DON'T touch DSP-thread-owned state (slotVotes,
        // avgPower, fftBufPos, etc.) — set a flag so the DSP thread resets
        // them safely at the start of its next frame.
        _this->pendingRetuneSr     = newSr;
        _this->pendingRetuneCenter = newCenter;
        _this->retuneFlag.store(true);

        // Wake mgmt thread to re-spawn manual channels at new center immediately
        _this->mgmtCv.notify_one();
    }

    // ── UI ───────────────────────────────────────────────────────────────────

    static void menuHandler(void* ctx) {
        ChannelBankModule* _this = (ChannelBankModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // Reset each frame; set below whenever a channel/history row is hovered.
        // Read by fftRedrawHandlerFunc to draw the cyan crosshair.
        _this->hoveredFreqHz = 0.0;

        if (_this->running) { style::beginDisabled(); }

        ImGui::LeftLabel("Channel Spacing");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##_cb_spacing_", _this->name), &_this->spacingId,
                         "8.33kHz\0" "12.5kHz\0" "25kHz\0" "50kHz\0" "100kHz\0" "200kHz\0\0")) {
            _this->channelSpacing = SPACINGS[_this->spacingId];
            config.acquire();
            config.conf[_this->name]["spacingId"] = _this->spacingId;
            config.release(true);
        }

        ImGui::LeftLabel("Demod Mode");
        ImGui::FillWidth();
        if (ImGui::Combo(CONCAT("##_cb_demod_", _this->name), &_this->demodMode,
                         "AM\0" "NFM\0" "WFM\0" "USB\0" "LSB\0\0")) {
            config.acquire();
            config.conf[_this->name]["demodMode"] = _this->demodMode;
            config.release(true);
        }

        if (_this->demodMode == DEMOD_USB || _this->demodMode == DEMOD_LSB) {
            ImGui::LeftLabel("BFO Trim");
            ImGui::FillWidth();
            if (ImGui::DragInt(CONCAT("##_cb_bfo_", _this->name),
                               &_this->ssbBfoHz, 1.0f, -1500, 1500, "%d Hz")) {
                _this->ssbBfoHz = std::clamp(_this->ssbBfoHz, -1500, 1500);
                config.acquire();
                config.conf[_this->name]["ssbBfoHz"] = _this->ssbBfoHz;
                config.release(true);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag left/right for 1 Hz steps");
        }

        ImGui::LeftLabel("Max Channels");
        ImGui::FillWidth();
        if (ImGui::InputInt(CONCAT("##_cb_maxch_", _this->name), &_this->maxChannels)) {
            _this->maxChannels = std::clamp(_this->maxChannels, 1, 256);
            config.acquire();
            config.conf[_this->name]["maxChannels"] = _this->maxChannels;
            config.release(true);
        }

        ImGui::LeftLabel("BW Usage");
        ImGui::FillWidth();
        {
            int bwPct = (int)std::round(_this->bwUsage * 100.0f);
            if (ImGui::SliderInt(CONCAT("##_cb_bwusage_", _this->name),
                                 &bwPct, 50, 100, "%d%%")) {
                bwPct = std::clamp(bwPct, 50, 100);
                _this->bwUsage = bwPct / 100.0f;
                config.acquire();
                config.conf[_this->name]["bwUsage"] = _this->bwUsage;
                config.release(true);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fraction of SDR bandwidth to use.\n"
                              "Lower = avoids filter rolloff at edges.\n"
                              "RTL-SDR: 0.9, SDRPlay/Airspy: 0.7-0.8");

        if (ImGui::Checkbox(CONCAT("Noise Reduction##_cb_nr_", _this->name), &_this->noiseReduction)) {
            config.acquire();
            config.conf[_this->name]["noiseReduction"] = _this->noiseReduction;
            config.release(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("RNNoise neural network noise suppression.\n"
                              "Removes static/hiss from recordings while\n"
                              "preserving voice clarity. Takes effect on\n"
                              "new channels (restart to apply to all).");

        if (_this->noiseReduction) {
            ImGui::LeftLabel("NR Strength");
            ImGui::FillWidth();
            {
                int nrPct = (int)std::round(_this->nrMix * 100.0f);
                if (ImGui::SliderInt(CONCAT("##_cb_nrmix_", _this->name),
                                     &nrPct, 0, 100, "%d%%")) {
                    nrPct = std::clamp(nrPct, 0, 100);
                    _this->nrMix = nrPct / 100.0f;
                    config.acquire();
                    config.conf[_this->name]["nrMix"] = _this->nrMix;
                    config.release(true);
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("0%% = original audio (no NR)\n"
                                  "100%% = full noise reduction\n"
                                  "50-70%% = good balance for weak signals");
        }

        // Active span trim
        {
            ImGui::Text("Active Span Trim");
            ImGui::SameLine();
            ImGui::TextDisabled("(drag bars on waterfall)");
            float leftPct  = _this->leftTrimFrac  * 100.0f;
            float rightPct = _this->rightTrimFrac * 100.0f;
            ImGui::SetNextItemWidth(menuWidth * 0.45f);
            if (ImGui::SliderFloat(CONCAT("L##_cb_triml_", _this->name), &leftPct, 0.0f, 48.0f, "%.0f%%")) {
                _this->leftTrimFrac = leftPct / 100.0f;
                config.acquire();
                config.conf[_this->name]["leftTrimFrac"] = _this->leftTrimFrac;
                config.release(true);
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(menuWidth * 0.45f);
            if (ImGui::SliderFloat(CONCAT("R##_cb_trimr_", _this->name), &rightPct, 0.0f, 48.0f, "%.0f%%")) {
                _this->rightTrimFrac = rightPct / 100.0f;
                config.acquire();
                config.conf[_this->name]["rightTrimFrac"] = _this->rightTrimFrac;
                config.release(true);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Exclude this fraction of the bandwidth\n"
                                  "from left/right edge (Airspy filter rolloff).");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Detection mode (disabled while running)
        bool isAuto   = !_this->manualMode && !_this->scanMode && !_this->bookmarkScanMode;
        bool isManual = _this->manualMode;
        bool isScan   = _this->scanMode;
        bool isBkScan = _this->bookmarkScanMode;
        if (ImGui::RadioButton(CONCAT("Auto##_cb_auto_", _this->name), isAuto)) {
            if (_this->manualMode || _this->bookmarkScanMode) _this->restoreWaterfallVisibility();
            _this->manualMode = false; _this->scanMode = false; _this->bookmarkScanMode = false;
            _this->saveManualConfig(); _this->saveScanConfig();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(CONCAT("Manual##_cb_manual_", _this->name), isManual)) {
            _this->manualMode = true; _this->scanMode = false; _this->bookmarkScanMode = false;
            if (!_this->boundBookmarkLists.empty())
                _this->applyWaterfallVisibility();
            _this->saveManualConfig(); _this->saveScanConfig();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(CONCAT("Scan##_cb_scan_", _this->name), isScan)) {
            if (_this->manualMode || _this->bookmarkScanMode) _this->restoreWaterfallVisibility();
            _this->manualMode = false; _this->scanMode = true; _this->bookmarkScanMode = false;
            _this->saveManualConfig(); _this->saveScanConfig();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(CONCAT("Bk Scan##_cb_bkscan_", _this->name), isBkScan)) {
            if (_this->manualMode) _this->restoreWaterfallVisibility();
            _this->manualMode = false; _this->scanMode = false; _this->bookmarkScanMode = true;
            if (!_this->boundBookmarkLists.empty())
                _this->applyWaterfallVisibility();
            _this->saveManualConfig(); _this->saveScanConfig();
        }

        if (_this->running) { style::endDisabled(); }

        // Manual frequency list — editable while running
        if (_this->manualMode) {
            // Refresh bookmark JSON once per second so newly-added entries
            // in the Frequency Manager appear here automatically.
            auto now = std::chrono::steady_clock::now();
            if (now - _this->lastFmConfigRefresh > std::chrono::seconds(1)) {
                _this->loadFMConfig();
                _this->lastFmConfigRefresh = now;
                std::lock_guard<std::mutex> lk(_this->manualFreqMtx);
                _this->rebuildBoundFreqs();
            }

            ImGui::Spacing();

            // Bookmark source dropdown — multi-select: tick any number of FM
            // lists; their frequencies are scanned alongside manual entries.
            // Ticking a list also hides all other FM bookmark groups on the
            // main waterfall so only the selected lists are visible.
            ImGui::LeftLabel("Bookmark lists");
            ImGui::FillWidth();
            {
                char preview[160];
                int  nSelected = (int)_this->boundBookmarkLists.size();
                int  nFreqs    = (int)_this->boundFreqs.size();
                if (nSelected == 0)
                    snprintf(preview, sizeof(preview), "(none)");
                else if (nSelected == 1)
                    snprintf(preview, sizeof(preview), "%s  (%d)",
                             _this->boundBookmarkLists.begin()->c_str(), nFreqs);
                else
                    snprintf(preview, sizeof(preview), "%d lists  (%d freqs)",
                             nSelected, nFreqs);

                if (ImGui::BeginCombo(CONCAT("##_cb_bmsrc_", _this->name), preview)) {
                    if (_this->fmLists.empty()) {
                        ImGui::TextDisabled("No FM lists found");
                    }
                    else {
                        bool anyChange = false;
                        for (auto& [listName, freqs] : _this->fmLists) {
                            bool checked = (_this->boundBookmarkLists.count(listName) > 0);
                            char lbl[256];
                            snprintf(lbl, sizeof(lbl), "%s  (%d)",
                                     listName.c_str(), (int)freqs.size());
                            // DontClosePopups: keep the dropdown open for multi-select
                            if (ImGui::Selectable(lbl, checked,
                                                  ImGuiSelectableFlags_DontClosePopups)) {
                                std::lock_guard<std::mutex> lk(_this->manualFreqMtx);
                                if (checked)
                                    _this->boundBookmarkLists.erase(listName);
                                else
                                    _this->boundBookmarkLists.insert(listName);
                                _this->rebuildBoundFreqs();
                                anyChange = true;
                            }
                        }
                        if (anyChange) {
                            // Apply or restore waterfall visibility
                            if (_this->boundBookmarkLists.empty())
                                _this->restoreWaterfallVisibility();
                            else
                                _this->applyWaterfallVisibility();
                            _this->saveManualConfig();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::Spacing();

            // Watch alert banner — shown when a watched frequency gets activity
            {
                int64_t alertKey = _this->watchAlert.load();
                if (alertKey != 0) {
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.55f, 0.25f, 0.0f, 0.85f));
                    ImGui::BeginChild(CONCAT("##_cb_alert_", _this->name), ImVec2(menuWidth, 26), false);
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3);
                    // Look up display name without holding a lock while drawing
                    std::string alertDisp = _this->displayName((double)alertKey * 1000.0);
                    char alertTxt[160];
                    snprintf(alertTxt, sizeof(alertTxt), "[W] Activity: %s", alertDisp.c_str());
                    ImGui::TextUnformatted(alertTxt);
                    ImGui::SameLine(menuWidth - 55);
                    if (ImGui::SmallButton(CONCAT("Clear##_cb_alertclr_", _this->name)))
                        _this->watchAlert.store(0);
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                    ImGui::Spacing();
                }
            }

            // Custom frequency input (add new custom freq)
            ImGui::Text("Custom frequencies (MHz):");
            ImGui::SetNextItemWidth(menuWidth - ImGui::CalcTextSize("Add").x - ImGui::GetStyle().ItemSpacing.x * 2 - 8);
            ImGui::InputText(CONCAT("##_cb_freqin_", _this->name), _this->manualFreqInputBuf, sizeof(_this->manualFreqInputBuf));
            ImGui::SameLine();
            if (ImGui::Button(CONCAT("Add##_cb_addfreq_", _this->name))) {
                double mhz = atof(_this->manualFreqInputBuf);
                if (mhz > 0.0) {
                    { std::lock_guard<std::mutex> lk(_this->manualFreqMtx); _this->manualFrequencies.push_back(mhz * 1e6); }
                    _this->saveManualConfig();
                    memset(_this->manualFreqInputBuf, 0, sizeof(_this->manualFreqInputBuf));
                }
            }

            // Frequency list: all active manual freqs with ★ watch toggles.
            // Custom entries get an X remove button; bound bookmark entries show dimmed.
            {
                // Snapshot both lists while holding the lock
                std::vector<double> customFreqs, bndFreqs;
                {
                    std::lock_guard<std::mutex> lk(_this->manualFreqMtx);
                    customFreqs = _this->manualFrequencies;
                    bndFreqs    = _this->boundFreqs;
                }
                // Build bound-only list (deduplicated against custom)
                std::vector<double> bndOnly;
                for (double bf : bndFreqs) {
                    bool dup = false;
                    for (double mf : customFreqs)
                        if (std::abs(mf - bf) < 100.0) { dup = true; break; }
                    if (!dup) bndOnly.push_back(bf);
                }

                int toRemove = -1;
                ImGui::BeginChild(CONCAT("##_cb_manlist_", _this->name),
                                  ImVec2(menuWidth, 120), true);

                // Helper lambda: draw a watch toggle button.
                // Orange [W] = watched, default [ ] = not watched.
                auto watchToggle = [&](const char* idSuffix, double hz) {
                    int64_t k = _this->freqKey(hz);
                    bool watched;
                    { std::lock_guard<std::mutex> lk(_this->manualFreqMtx);
                      watched = _this->watchedFreqs.count(k) > 0; }
                    if (watched) {
                        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.7f, 0.35f, 0.0f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.45f, 0.0f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.5f, 0.25f, 0.0f, 1.0f));
                    }
                    char btn[48];
                    snprintf(btn, sizeof(btn), "%s##%s", watched ? "W" : " ", idSuffix);
                    if (ImGui::SmallButton(btn)) {
                        { std::lock_guard<std::mutex> lk(_this->manualFreqMtx);
                          if (watched) _this->watchedFreqs.erase(k);
                          else         _this->watchedFreqs.insert(k); }
                        _this->saveManualConfig();
                    }
                    if (watched) ImGui::PopStyleColor(3);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(watched ? "Watching — click to stop" : "Click to watch");
                };

                // — Custom entries —
                for (int i = 0; i < (int)customFreqs.size(); i++) {
                    double hz = customFreqs[i];
                    char idBuf[32]; snprintf(idBuf, sizeof(idBuf), "_cb_wc_%d", i);
                    watchToggle(idBuf, hz);
                    ImGui::SameLine();
                    std::string dname = _this->displayName(hz);
                    ImGui::Text("%s", dname.c_str());
                    ImGui::SameLine(menuWidth - 38);
                    char rmBtn[32]; snprintf(rmBtn, sizeof(rmBtn), "X##_cb_rm_%d", i);
                    if (ImGui::SmallButton(rmBtn)) toRemove = i;
                }

                // — Bound bookmark list entries (read-only; can watch, cannot remove) —
                for (int i = 0; i < (int)bndOnly.size(); i++) {
                    double hz = bndOnly[i];
                    char idBuf[32]; snprintf(idBuf, sizeof(idBuf), "_cb_wb_%d", i);
                    watchToggle(idBuf, hz);
                    ImGui::SameLine();
                    std::string dname = _this->displayName(hz);
                    ImGui::TextDisabled("%s", dname.c_str());
                }

                ImGui::EndChild();

                if (toRemove >= 0) {
                    { std::lock_guard<std::mutex> lk(_this->manualFreqMtx);
                      _this->manualFrequencies.erase(_this->manualFrequencies.begin() + toRemove); }
                    _this->saveManualConfig();
                }
            }
        }

        // ── Scan range list (shown when scan mode selected) ───────────────────
        if (_this->scanMode) {
            ImGui::Spacing();

            // Add a range: [start] → [stop]  [Add]
            float addBtnW = ImGui::CalcTextSize("Add").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
            float arrowW  = ImGui::CalcTextSize(" -> ").x;
            float inputW  = (menuWidth - addBtnW - arrowW - ImGui::GetStyle().ItemSpacing.x * 2) / 2.0f;
            ImGui::SetNextItemWidth(inputW);
            ImGui::InputText(CONCAT("##_cb_scstart_", _this->name), _this->scanStartBuf, sizeof(_this->scanStartBuf));
            ImGui::SameLine(); ImGui::Text(" -> ");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(inputW);
            ImGui::InputText(CONCAT("##_cb_scstop_", _this->name), _this->scanStopBuf, sizeof(_this->scanStopBuf));
            ImGui::SameLine();
            if (ImGui::Button(CONCAT("Add##_cb_scadd_", _this->name))) {
                double startMHz = atof(_this->scanStartBuf);
                double stopMHz  = atof(_this->scanStopBuf);
                if (stopMHz > startMHz && startMHz > 0.0) {
                    _this->scanRanges.push_back({ startMHz * 1e6, stopMHz * 1e6 });
                    _this->saveScanConfig();
                    memset(_this->scanStartBuf, 0, sizeof(_this->scanStartBuf));
                    memset(_this->scanStopBuf,  0, sizeof(_this->scanStopBuf));
                }
            }

            // Range list
            ImGui::BeginChild(CONCAT("##_cb_sclist_", _this->name), ImVec2(menuWidth, 100), true);
            int toRemoveScan = -1;
            double stepMHz = (_this->lastKnownSr > 0.0 ? _this->lastKnownSr * _this->bwUsage : 2400000.0) / 1e6;
            for (int i = 0; i < (int)_this->scanRanges.size(); i++) {
                auto& r = _this->scanRanges[i];
                double spanMHz = (r.stopHz - r.startHz) / 1e6;
                int stops = std::max(1, (int)std::ceil(spanMHz / stepMHz));
                char lbl[128];
                snprintf(lbl, sizeof(lbl), "%.3f -> %.3f  [%d x %.2fMHz]",
                         r.startHz / 1e6, r.stopHz / 1e6, stops, stepMHz);
                ImGui::Text("%s", lbl);
                ImGui::SameLine(menuWidth - 38);
                char btn[32]; snprintf(btn, sizeof(btn), "X##_cb_scrm_%d", i);
                if (ImGui::SmallButton(btn)) toRemoveScan = i;
            }
            ImGui::EndChild();
            if (toRemoveScan >= 0) {
                _this->scanRanges.erase(_this->scanRanges.begin() + toRemoveScan);
                _this->saveScanConfig();
            }

            // Quiet timeout (after a transmission ends)
            ImGui::LeftLabel("Quiet Timeout");
            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_cb_scquiet_", _this->name),
                                   &_this->scanQuietSec, 1.0f, 30.0f, "%.1f s")) {
                _this->saveScanConfig();
            }

            // No-signal timeout (no transmission ever detected at this stop)
            ImGui::LeftLabel("No Signal Skip");
            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_cb_scnosig_", _this->name),
                                   &_this->scanNoSignalSec, 0.1f, 5.0f, "%.1f s")) {
                _this->saveScanConfig();
            }
        }

        // ── Bookmark scan mode settings ───────────────────────────────────────
        if (_this->bookmarkScanMode) {
            // Refresh FM lists
            auto bkNow = std::chrono::steady_clock::now();
            if (bkNow - _this->lastFmConfigRefresh > std::chrono::seconds(1)) {
                _this->loadFMConfig();
                _this->lastFmConfigRefresh = bkNow;
                std::lock_guard<std::mutex> lk(_this->manualFreqMtx);
                _this->rebuildBoundFreqs();
            }

            ImGui::Spacing();

            // Bookmark list selector (same as manual mode)
            ImGui::LeftLabel("Bookmark lists");
            ImGui::FillWidth();
            {
                char preview[160];
                int nSelected = (int)_this->boundBookmarkLists.size();
                int nFreqs    = (int)_this->boundFreqs.size();
                if (nSelected == 0)
                    snprintf(preview, sizeof(preview), "(none)");
                else if (nSelected == 1)
                    snprintf(preview, sizeof(preview), "%s  (%d)",
                             _this->boundBookmarkLists.begin()->c_str(), nFreqs);
                else
                    snprintf(preview, sizeof(preview), "%d lists  (%d freqs)", nSelected, nFreqs);

                if (ImGui::BeginCombo(CONCAT("##_cb_bksrc_", _this->name), preview)) {
                    if (_this->fmLists.empty()) {
                        ImGui::TextDisabled("No FM lists found");
                    } else {
                        bool anyChange = false;
                        for (auto& [listName, freqs] : _this->fmLists) {
                            bool checked = (_this->boundBookmarkLists.count(listName) > 0);
                            char lbl[256];
                            snprintf(lbl, sizeof(lbl), "%s  (%d)", listName.c_str(), (int)freqs.size());
                            if (ImGui::Selectable(lbl, checked, ImGuiSelectableFlags_DontClosePopups)) {
                                std::lock_guard<std::mutex> lk(_this->manualFreqMtx);
                                if (checked) _this->boundBookmarkLists.erase(listName);
                                else         _this->boundBookmarkLists.insert(listName);
                                _this->rebuildBoundFreqs();
                                anyChange = true;
                            }
                        }
                        if (anyChange) {
                            if (_this->boundBookmarkLists.empty())
                                _this->restoreWaterfallVisibility();
                            else
                                _this->applyWaterfallVisibility();
                            _this->saveManualConfig();
                        }
                    }
                    ImGui::EndCombo();
                }
            }

            // Cluster preview
            if (!_this->boundFreqs.empty() || !_this->manualFrequencies.empty()) {
                double sr = _this->lastKnownSr > 0.0 ? _this->lastKnownSr : 2400000.0;
                double usableBw = sr * _this->bwUsage;
                std::vector<double> freqs = _this->getActiveManualFreqs();
                std::sort(freqs.begin(), freqs.end());
                int nStops = 0, maxPerStop = 0;
                int i = 0;
                while (i < (int)freqs.size()) {
                    int j = i + 1, cnt = 1;
                    while (j < (int)freqs.size() && freqs[j] - freqs[i] <= usableBw) { j++; cnt++; }
                    nStops++;
                    maxPerStop = std::max(maxPerStop, cnt);
                    i = j;
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.8f, 0.5f, 1.0f));
                ImGui::Text("%d stop%s, up to %d freq%s per stop  (%.1f MHz window)",
                    nStops, nStops == 1 ? "" : "s",
                    maxPerStop, maxPerStop == 1 ? "" : "s",
                    usableBw / 1e6);
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                ImGui::TextUnformatted("Select bookmark lists above to scan");
                ImGui::PopStyleColor();
            }

            // Timers (shared with scan mode)
            ImGui::LeftLabel("Quiet Timeout");
            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_cb_bkquiet_", _this->name),
                                   &_this->scanQuietSec, 1.0f, 30.0f, "%.1f s"))
                _this->saveScanConfig();
            ImGui::LeftLabel("No Signal Skip");
            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_cb_bknosig_", _this->name),
                                   &_this->scanNoSignalSec, 0.1f, 5.0f, "%.1f s"))
                _this->saveScanConfig();
        }

        // ── Mini spectrum display ─────────────────────────────────────────────
        {
            const float H = 100.0f;
            ImVec2 pos  = ImGui::GetCursorScreenPos();
            float  W    = ImGui::GetContentRegionAvail().x;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            // Background
            dl->AddRectFilled(pos, ImVec2(pos.x + W, pos.y + H), IM_COL32(15, 15, 15, 255));
            dl->AddRect      (pos, ImVec2(pos.x + W, pos.y + H), IM_COL32(60, 60, 60, 255));

            DisplaySnapshot snap;
            {
                std::lock_guard<std::mutex> dlck(_this->displayMtx);
                snap = _this->displaySnap;
            }

            if (!snap.power.empty()) {
                int   N       = (int)snap.power.size();
                float dBrange = snap.dBmax - snap.dBmin;
                if (dBrange < 1.0f) dBrange = 1.0f;

                auto binToX = [&](int bin) -> float {
                    return pos.x + (float)bin / N * W;
                };
                auto dBtoY = [&](float db) -> float {
                    float y = pos.y + H * (1.0f - (db - snap.dBmin) / dBrange);
                    return std::clamp(y, pos.y, pos.y + H);
                };

                // Auto mode: active slot background highlights
                for (int s = 0; s < snap.numSlots && s < (int)snap.slotCenterBin.size(); s++) {
                    bool active   = snap.detected.count(s) > 0;
                    if (!active) continue;
                    int   cb  = snap.slotCenterBin[s];
                    int   hw  = std::max(1, (int)std::round(W / N * 2));
                    float x0  = binToX(cb) - hw;
                    float x1  = binToX(cb) + hw;
                    dl->AddRectFilled(ImVec2(x0, pos.y), ImVec2(x1, pos.y + H),
                                      IM_COL32(255, 200, 0, 40));
                }

                // Manual mode: draw a vertical line per configured frequency
                for (int m = 0; m < (int)snap.manualCenterBins.size(); m++) {
                    float x = binToX(snap.manualCenterBins[m]);
                    ImU32 col = snap.manualActiveFlags[m]
                        ? IM_COL32(80, 220, 255, 220)   // cyan = signal present
                        : IM_COL32(80, 140, 200, 120);  // dim blue = quiet
                    dl->AddLine(ImVec2(x, pos.y), ImVec2(x, pos.y + H), col, 1.5f);
                }

                // FFT power curve (green)
                for (int i = 0; i < N - 1; i++) {
                    float x0 = binToX(i),     y0 = dBtoY(snap.power[i]);
                    float x1 = binToX(i + 1), y1 = dBtoY(snap.power[i + 1]);
                    dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 210, 0, 200));
                }

                // Global threshold line (orange horizontal line across full width)
                float ty = dBtoY(snap.threshDb);
                dl->AddLine(ImVec2(pos.x, ty), ImVec2(pos.x + W, ty),
                            IM_COL32(255, 120, 0, 200), 1.5f);

                // Legend
                dl->AddText(ImVec2(pos.x + 4, pos.y + 2),  IM_COL32(0, 210, 0, 200),   "Power");
                dl->AddText(ImVec2(pos.x + 4, pos.y + 14), IM_COL32(255, 120, 0, 200),  "Threshold");
                dl->AddText(ImVec2(pos.x + 4, pos.y + 26), IM_COL32(255, 200, 0, 200),  "Detected");

                // dB range labels
                char buf[32];
                snprintf(buf, sizeof(buf), "%.0f dB", snap.dBmax);
                dl->AddText(ImVec2(pos.x + W - 48, pos.y + 2),      IM_COL32(160,160,160,200), buf);
                snprintf(buf, sizeof(buf), "%.0f dB", snap.dBmin);
                dl->AddText(ImVec2(pos.x + W - 48, pos.y + H - 14), IM_COL32(160,160,160,200), buf);
            }

            ImGui::Dummy(ImVec2(W, H));
            ImGui::Spacing();
        }

        // SNR threshold (live)
        ImGui::LeftLabel("SNR Threshold");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##_cb_snr_", _this->name),
                               &_this->snrThreshold, 1.0f, 30.0f, "%.1f dB")) {
            config.acquire();
            config.conf[_this->name]["snrThreshold"] = _this->snrThreshold;
            config.release(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Minimum SNR above noise floor required to START\n"
                              "a new recording. Raising this reduces false triggers\n"
                              "from noise; lowering it catches weaker signals.");

        // Hold hysteresis — lower threshold while a recording is already open
        ImGui::LeftLabel("Hold Hysteresis");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##_cb_hyst_", _this->name),
                               &_this->holdHysteresisDb, 0.0f, 8.0f, "%.1f dB")) {
            config.acquire();
            config.conf[_this->name]["holdHysteresisDb"] = _this->holdHysteresisDb;
            config.release(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("dB below SNR Threshold that keeps an open\n"
                              "recording alive. Signal can fade this far below\n"
                              "the threshold without losing votes or triggering\n"
                              "the dropout timer.\n"
                              "4 dB covers typical HF QSB fading.\n"
                              "0 = disabled (symmetric open/hold threshold).");

        // Max recording length — hard duration cap (live)
        ImGui::LeftLabel("Max Length");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##_cb_maxlen_", _this->name),
                               &_this->maxRecordingSec, 0.0f, 300.0f, "%.0f s")) {
            config.acquire();
            config.conf[_this->name]["maxRecordingSec"] = _this->maxRecordingSec;
            config.release(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Force-close any recording open longer than this.\n"
                              "Persistent come-and-go interference keeps a channel\n"
                              "latched open forever; this breaks the loop so the\n"
                              "static gate can judge and discard each segment.\n"
                              "A genuinely long transmission simply continues into\n"
                              "a new file.  0 = no cap.");

        // Static gate — discard carrier-less (broadband interference) recordings
        if (ImGui::Checkbox(CONCAT("Static Gate##_cb_statgate_", _this->name),
                            &_this->staticGateEnabled)) {
            config.acquire();
            config.conf[_this->name]["staticGateEnabled"] = _this->staticGateEnabled;
            config.release(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Discard a finished recording whose channel spectrum\n"
                              "was flat (broadband static, no carrier) rather than\n"
                              "peaky (carrier + voice sidebands).  Judges the whole\n"
                              "recording's structure, so it catches interference that\n"
                              "comes and goes.  Discarded files never queue or log.");
        if (_this->staticGateEnabled) {
            ImGui::LeftLabel("  Min Voice %");
            ImGui::FillWidth();
            float pct = _this->staticGateVoiceFrac * 100.0f;
            if (ImGui::SliderFloat(CONCAT("##_cb_statfrac_", _this->name),
                                   &pct, 5.0f, 75.0f, "%.0f %%")) {
                _this->staticGateVoiceFrac = pct / 100.0f;
                config.acquire();
                config.conf[_this->name]["staticGateVoiceFrac"] = _this->staticGateVoiceFrac;
                config.release(true);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Minimum %% of above-threshold frames that must show\n"
                                  "a carrier for the recording to be kept.  Higher =\n"
                                  "stricter (discards more borderline recordings).\n"
                                  "If real transmissions are being discarded, lower it.");
        }

        // Drift gate — discard drifting/faulty (frequency-wandering) carriers
        if (ImGui::Checkbox(CONCAT("Drift Gate##_cb_driftgate_", _this->name),
                            &_this->driftGateEnabled)) {
            config.acquire();
            config.conf[_this->name]["driftGateEnabled"] = _this->driftGateEnabled;
            config.release(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Discard a recording whose carrier wandered in frequency\n"
                              "(a faulty/drifting transmitter — diagonal streaks in the\n"
                              "waterfall) instead of holding a fixed carrier.  Catches the\n"
                              "spur/comb interference the static gate can't, since each\n"
                              "drifting spur still reads as a carrier.");
        if (_this->driftGateEnabled) {
            ImGui::LeftLabel("  Max Drift");
            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_cb_drift_", _this->name),
                                   &_this->driftMaxStdHz, 200.0f, 3000.0f, "%.0f Hz")) {
                config.acquire();
                config.conf[_this->name]["driftMaxStdHz"] = _this->driftMaxStdHz;
                config.release(true);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Carrier-frequency stddev (over the recording) above which\n"
                                  "the signal counts as drifting and is discarded.  A healthy\n"
                                  "transmitter holds within a few hundred Hz.  Lower = stricter.\n"
                                  "If real (slightly off-tuned) signals are discarded, raise it.");
        }

        // Cooldown (live)
        ImGui::LeftLabel("Cooldown");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##_cb_cd_", _this->name),
                               &_this->cooldownSec, 1.0f, 30.0f, "%.0f s")) {
            config.acquire();
            config.conf[_this->name]["cooldownSec"] = _this->cooldownSec;
            config.release(true);
        }

        // Global recording enable/disable toggle
        if (ImGui::Checkbox(CONCAT("Enable Recording##_cb_recen_", _this->name),
                            &_this->recordingEnabled)) {
            config.acquire();
            config.conf[_this->name]["recordingEnabled"] = _this->recordingEnabled;
            config.release(true);
        }

#ifdef __APPLE__
        // ── Transcription backend dropdown ──────────────────────────────────
        // Selecting a Whisper model that isn't installed shows an inline status
        // message and a Download button (download UI lands in a follow-up step;
        // for now users can place a .bin in the models dir manually).
        {
            ImGui::LeftLabel("Transcribe");
            ImGui::FillWidth();
            const char* labels[] = {
                "Off",
                "Apple Speech",
                "Whisper ATC Large (best)",
                "Whisper ATC Medium",
                "Whisper Turbo (fast)",
            };
            int  cur = _this->transcriptionBackend;
            if (cur < 0 || cur > TB_WHISPER_TURBO) cur = TB_OFF;
            if (ImGui::Combo(CONCAT("##_cb_txbe_", _this->name), &cur, labels, 5)) {
                _this->transcriptionBackend = cur;
                config.acquire();
                config.conf[_this->name]["transcriptionBackend"] = cur;
                config.release(true);
                // Trigger Apple Speech authorization the first time it's picked
                if (cur == TB_APPLE_SPEECH &&
                    transcription::authStatus() == transcription::AuthStatus::NotDetermined) {
                    transcription::requestPermission();
                }
            }

            // Surface backend-specific status / actions on the next line.
            if (_this->transcriptionBackend == TB_APPLE_SPEECH) {
                auto txStatus = transcription::authStatus();
                if (txStatus == transcription::AuthStatus::NotConfigured) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.2f, 1.0f));
                    ImGui::TextWrapped("Info.plist missing NSSpeechRecognitionUsageDescription");
                    ImGui::PopStyleColor();
                } else if (txStatus == transcription::AuthStatus::Denied) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                    ImGui::TextUnformatted("Speech access denied");
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    if (ImGui::SmallButton(CONCAT("Open Settings##_cb_txset_", _this->name)))
                        transcription::openSystemSettings();
                } else if (txStatus == transcription::AuthStatus::NotDetermined) {
                    if (ImGui::SmallButton(CONCAT("Authorize##_cb_txauth_", _this->name)))
                        transcription::requestPermission();
                }
            } else if (_this->transcriptionBackend >= TB_WHISPER_ATC_LARGE) {
                using transcription_whisper::Model;
                Model whichModel = Model::ATCLarge;
                if (_this->transcriptionBackend == TB_WHISPER_ATC_MEDIUM) whichModel = Model::ATCMedium;
                else if (_this->transcriptionBackend == TB_WHISPER_TURBO)   whichModel = Model::Turbo;
                bool installed = transcription_whisper::isModelInstalled(whichModel);
                if (installed) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
                    ImGui::TextWrapped("Model: ready (%.1f MB)",
                        transcription_whisper::modelSize(whichModel) / (1024.0 * 1024.0));
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.3f, 1.0f));
                    ImGui::TextWrapped(
                        "Model not installed. Place %s in:\n%s",
                        transcription_whisper::modelFilename(whichModel).c_str(),
                        transcription_whisper::modelsDir().c_str());
                    ImGui::PopStyleColor();
                }
            }
        }

        // M4A encoding toggle (macOS only — uses AudioToolbox)
        if (ImGui::Checkbox(CONCAT("Encode to M4A##_cb_m4a_", _this->name),
                            &_this->m4aEnabled)) {
            config.acquire();
            config.conf[_this->name]["m4aEnabled"] = _this->m4aEnabled;
            config.release(true);
        }
        if (_this->m4aEnabled) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.8f, 0.5f, 1.0f));
            ImGui::TextUnformatted("32 kbps AAC");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
            ImGui::TextWrapped("WAV converted after first playback%s. WAV deleted on success.",
                _this->transcriptionOn() ? " + transcription" : "");
            ImGui::PopStyleColor();
        }
#endif

        // Record gain (live)
        ImGui::LeftLabel("Rec Gain");
        ImGui::FillWidth();
        if (ImGui::SliderFloat(CONCAT("##_cb_gain_", _this->name),
                               &_this->recGain, 0.01f, 1.0f, "%.2f")) {
            config.acquire();
            config.conf[_this->name]["recGain"] = _this->recGain;
            config.release(true);
        }

        // Minimum transmission duration — discard recordings shorter than this
        ImGui::LeftLabel("Min TX Duration");
        ImGui::FillWidth();
        {
            char mintxFmt[32];
            if (_this->minTransmissionMs >= 1000)
                snprintf(mintxFmt, sizeof(mintxFmt), "%.1f s", _this->minTransmissionMs / 1000.0f);
            else
                snprintf(mintxFmt, sizeof(mintxFmt), "%d ms", _this->minTransmissionMs);
            if (ImGui::SliderInt(CONCAT("##_cb_mintx_", _this->name),
                                 &_this->minTransmissionMs, 0, 10000, mintxFmt)) {
                config.acquire();
                config.conf[_this->name]["minTransmissionMs"] = _this->minTransmissionMs;
                config.release(true);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Discard recordings with less than this much actual ON-AIR\n"
                                  "time (sum of the moments the carrier is truly present),\n"
                                  "not elapsed wall-clock. Bursty data like ACARS gets bridged\n"
                                  "into a long recording by the signal-hold, but its real\n"
                                  "airtime stays small — so this discards it while keeping\n"
                                  "continuous voice of the same wall-clock length.");
        }

        // Tail length — how long to keep recording after signal gone
        ImGui::LeftLabel("Signal Hold");
        ImGui::FillWidth();
        {
            char holdFmt[32];
            if (_this->signalHoldMs >= 1000)
                snprintf(holdFmt, sizeof(holdFmt), "%.1f s", _this->signalHoldMs / 1000.0f);
            else
                snprintf(holdFmt, sizeof(holdFmt), "%d ms", _this->signalHoldMs);
            if (ImGui::SliderInt(CONCAT("##_cb_hold_", _this->name),
                                 &_this->signalHoldMs, 0, 5000, holdFmt)) {
                config.acquire();
                config.conf[_this->name]["signalHoldMs"] = _this->signalHoldMs;
                config.release(true);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Keep recording through dropouts up to this long.\nSet higher to bridge gaps from aircraft banking or multipath.");

        ImGui::LeftLabel("TX Tail");
        ImGui::FillWidth();
        if (ImGui::SliderInt(CONCAT("##_cb_tail_", _this->name),
                             &_this->tailMs, 100, 2000, "%d ms")) {
            config.acquire();
            config.conf[_this->name]["tailMs"] = _this->tailMs;
            config.release(true);
        }

        // Recording path
        if (_this->folderSelect.render("##_cb_fold_" + _this->name)) {
            if (_this->folderSelect.pathIsValid()) {
                config.acquire();
                config.conf[_this->name]["recPath"] = _this->folderSelect.path;
                config.release(true);
            }
        }


        // Start / Stop
        if (!_this->running) {
            if (!_this->folderSelect.pathIsValid()) { style::beginDisabled(); }
            if (ImGui::Button(CONCAT("Start##_cb_start_", _this->name), ImVec2(menuWidth, 0))) {
                _this->start();
            }
            if (!_this->folderSelect.pathIsValid()) { style::endDisabled(); }
        }
        else {
            if (ImGui::Button(CONCAT("Stop##_cb_stop_", _this->name), ImVec2(menuWidth, 0))) {
                _this->stop();
            }

            int total = 0, recording = 0;
            {
                std::lock_guard<std::mutex> lck(_this->channelsMtx);
                total = (int)_this->activeChannels.size();
                for (auto& [idx, slot] : _this->activeChannels) {
                    if (slot->fileOpen) recording++;
                }
            }
            int queued = 0;
            {
                std::lock_guard<std::mutex> lk(_this->playbackMtx);
                queued = (int)_this->playbackQueue.size();
            }
            ImGui::Text("Active: %d  Recording: %d", total, recording);
            ImGui::Text("Monitor queue: %d pending", queued);

            // Diagnostic: show noise floor + threshold in dB
            {
                float nf = _this->globalNoiseFloor;
                float th = nf * powf(10.0f, _this->snrThreshold / 10.0f);
                float nfDb = (nf > 0.0f) ? 10.0f * log10f(nf) : -999.0f;
                float thDb = (th > 0.0f) ? 10.0f * log10f(th) : -999.0f;
                int det = _this->debugDetectedCount.load();
                int blk = _this->debugBlockedSkips.load();
                int cap = _this->debugCapSkips.load();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
                ImGui::Text("Floor: %.1f dB  Thresh: %.1f dB", nfDb, thDb);
                ImGui::Text("Det: %d  BlkSkip: %d  CapSkip: %d", det, blk, cap);
                ImGui::PopStyleColor();
            }

            // Now-playing description panel (above the active-channel list).
            // Shows the description and (when transcription is on) the transcript.
            {
                int64_t playingKey = _this->currentlyPlayingFreqKey.load();
                std::string playingName, playingDesc;
                if (playingKey != 0) {
                    std::lock_guard<std::mutex> lk(_this->freqLogMtx);
                    auto it = _this->freqLog.find(playingKey);
                    if (it != _this->freqLog.end()) {
                        playingName = _this->displayName(it->second.freqHz);
                        playingDesc = it->second.description;
                    }
                }
#ifdef __APPLE__
                std::string txText, txName;
                std::vector<transcription_whisper::Segment> txSegs;
                int txPosMs = _this->playbackPosMs.load();
                if (_this->transcriptionOn()) {
                    std::lock_guard<std::mutex> tlk(_this->lastTranscriptMtx);
                    txText = _this->lastTranscriptText;
                    txName = _this->lastTranscriptName;
                    txSegs = _this->playingSegments;
                }
                // Pick panel height: synced display is taller (line per segment).
                float panelH = 50.0f;
                if (!txSegs.empty())     panelH = std::min(220.0f, 60.0f + 22.0f * (float)txSegs.size());
                else if (!txText.empty()) panelH = 110.0f;
#else
                float panelH = 50.0f;
#endif
                ImGui::BeginChild(CONCAT("##_cb_nowplaying_", _this->name),
                                  ImVec2(menuWidth, panelH), true);
                if (playingKey != 0 && !playingName.empty()) {
                    ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), "Playing: %s", playingName.c_str());
                    if (!playingDesc.empty()) {
                        ImGui::TextWrapped("%s", playingDesc.c_str());
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                        ImGui::TextWrapped("(no description — click D in history to add one)");
                        ImGui::PopStyleColor();
                    }
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    ImGui::Text("(nothing playing)");
                    ImGui::PopStyleColor();
                }
#ifdef __APPLE__
                if (!txSegs.empty()) {
                    // Synced segment display: stack each segment with [mm:ss],
                    // bright color on the active one (cursor lies in [t0,t1)),
                    // dim everything else.  Past segments stay readable;
                    // future ones get the same dim color so the user sees
                    // what's coming.  Once playback ends (txPosMs == -1)
                    // everything dims uniformly — still readable, no highlight.
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.6f, 1.0f, 1.0f));
                    if (!txName.empty()) ImGui::Text("[%s]", txName.c_str());
                    ImGui::PopStyleColor();
                    for (auto& seg : txSegs) {
                        bool active = (txPosMs >= 0 && txPosMs >= seg.t0Ms && txPosMs < seg.t1Ms);
                        ImVec4 col = active
                            ? ImVec4(1.00f, 0.95f, 0.45f, 1.0f)   // active = warm yellow
                            : ImVec4(0.55f, 0.55f, 0.62f, 1.0f);  // dim
                        ImGui::PushStyleColor(ImGuiCol_Text, col);
                        // [mm:ss]
                        int mm = seg.t0Ms / 60000;
                        int ss = (seg.t0Ms / 1000) % 60;
                        ImGui::Text("[%02d:%02d]", mm, ss);
                        ImGui::SameLine();
                        ImGui::TextWrapped("%s", seg.text.c_str());
                        ImGui::PopStyleColor();
                    }
                } else if (!txText.empty()) {
                    // No segments (Apple Speech or transcription pre-Whisper) —
                    // fall back to the original flat display.
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.6f, 1.0f, 1.0f));
                    if (!txName.empty())
                        ImGui::Text("[%s]", txName.c_str());
                    ImGui::PopStyleColor();
                    ImGui::TextWrapped("%s", txText.c_str());
                } else if (_this->transcriptionOn()) {
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                    ImGui::Text("(transcript will appear here)");
                    ImGui::PopStyleColor();
                }
#endif
                ImGui::EndChild();
            }

            // Active channel list
            ImGui::Separator();
            bool needSaveFreqLog = false;
            ImGui::BeginChild(CONCAT("##_cb_ch_", _this->name),
                              ImVec2(menuWidth, 150), false);
            {
                std::lock_guard<std::mutex> lck(_this->channelsMtx);
                for (auto& [idx, slot] : _this->activeChannels) {
                    std::string label = _this->displayName(slot->freqHz);
                    ImGui::Text("%s", label.c_str());
                    if (ImGui::IsItemHovered()) _this->hoveredFreqHz = slot->freqHz;
                    ImGui::SameLine();
                    bool playing = (_this->currentlyPlayingFreqKey.load() == _this->freqKey(slot->freqHz));
                    if (playing) {
                        ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), "[PLAY]");
                    }
                    else if (slot->fileOpen) {
                        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "[REC]");
                    }
                    else if (slot->inSilence) {
                        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "[---]");
                    }
                    else {
                        ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.35f, 1.0f), "[ ? ]");
                    }
                    ImGui::SameLine();
                    bool blocked = _this->isBlocked(slot->freqHz);
                    char blkId[48];
                    snprintf(blkId, sizeof(blkId), "Blk##_cb_ablk_%d", idx);
                    if (ImGui::Checkbox(blkId, &blocked)) {
                        std::lock_guard<std::mutex> lk(_this->freqLogMtx);
                        auto& entry = _this->freqLog[_this->freqKey(slot->freqHz)];
                        if (entry.freqHz == 0.0) entry.freqHz = slot->freqHz;
                        entry.blocked = blocked;
                        needSaveFreqLog = true;
                    }
                }
            }
            ImGui::EndChild();

            // ── Recent channels (sticky 30s after teardown) ───────────────────
            // These linger so the user can confirm a frequency was just noise and
            // hit Block before the row vanishes.  Alpha fades from 1→0 over 30s.
            {
                auto now = std::chrono::steady_clock::now();

                // Snapshot + prune under channelsMtx
                std::vector<std::pair<double, float>> recent;  // {freqHz, alpha}
                {
                    std::lock_guard<std::mutex> lck(_this->channelsMtx);
                    for (auto it = _this->recentChannels.begin();
                         it != _this->recentChannels.end(); ) {
                        long ageMs = std::chrono::duration_cast<
                            std::chrono::milliseconds>(now - it->destroyedAt).count();
                        if (ageMs >= 30000) {
                            it = _this->recentChannels.erase(it);
                        } else {
                            float alpha = std::max(0.0f, 1.0f - (float)ageMs / 30000.0f);
                            recent.push_back({it->freqHz, alpha});
                            ++it;
                        }
                    }
                }

                // Render without lock; collect any block-toggle action
                double toBlockFreq = 0.0;
                bool   toBlockVal  = false;
                for (auto& [hz, alpha] : recent) {
                    std::string dn = _this->displayName(hz);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.75f, 0.35f, alpha));
                    ImGui::Text("  [recent] %s", dn.c_str());
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) _this->hoveredFreqHz = hz;
                    ImGui::SameLine();
                    bool blocked = _this->isBlocked(hz);
                    char blkId[64];
                    snprintf(blkId, sizeof(blkId), "Blk##_cb_rblk_%lld",
                             (long long)_this->freqKey(hz));
                    if (ImGui::Checkbox(blkId, &blocked)) {
                        toBlockFreq = hz;
                        toBlockVal  = blocked;
                    }
                }

                // Apply block toggle outside both locks (freqLogMtx → saveFreqLog)
                if (toBlockFreq != 0.0) {
                    {
                        std::lock_guard<std::mutex> lk(_this->freqLogMtx);
                        auto& entry = _this->freqLog[_this->freqKey(toBlockFreq)];
                        if (entry.freqHz == 0.0) entry.freqHz = toBlockFreq;
                        entry.blocked = toBlockVal;
                    }
                    _this->saveFreqLog();
                }
            }

            if (needSaveFreqLog) _this->saveFreqLog();
        }

        // ── Frequency history + blocklist ─────────────────────────────────────
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Frequency History");
        ImGui::SameLine();
        bool clearAll  = ImGui::SmallButton("Clear All##_cb_clrlog");
        ImGui::SameLine();
        bool clearKeep = ImGui::SmallButton("Clear (keep blocked)##_cb_clrlogkeep");
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload Bookmarks##_cb_reloadbm")) {
            _this->loadFMConfig();
        }

        // Filter row: text search + blocked-only toggle
        ImGui::Checkbox("Blocked only##_cb_histblk", &_this->freqHistBlockedOnly);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX() - 26);
        ImGui::InputText("##_cb_histflt", _this->freqHistFilter, sizeof(_this->freqHistFilter));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Filter by name or frequency (MHz)");
        ImGui::SameLine();
        if (ImGui::SmallButton("x##_cb_clrflt"))
            _this->freqHistFilter[0] = '\0';

        bool needSave = false;

        ImGui::BeginChild(CONCAT("##_cb_freqlog_", _this->name),
                          ImVec2(0, 200), true);
        {
            std::lock_guard<std::mutex> lk(_this->freqLogMtx);

            if (clearAll)  { _this->freqLog.clear(); needSave = true; }
            if (clearKeep) {
                for (auto it = _this->freqLog.begin(); it != _this->freqLog.end(); ) {
                    if (!it->second.blocked) it = _this->freqLog.erase(it);
                    else ++it;
                }
                needSave = true;
            }

            // Collect entries, apply filters, sort: named first (alpha by name),
            // then unnamed sorted by frequency ascending.
            // Named = display name starts with a letter (bookmark name).
            // Unnamed = display name is "X.XXX MHz" format (starts with a digit).
            std::string filterStr = _this->freqHistFilter;
            std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);

            std::vector<std::pair<int64_t, FreqEntry*>> allEntries;
            allEntries.reserve(_this->freqLog.size());
            for (auto& [k, e] : _this->freqLog) {
                // Blocked-only filter
                if (_this->freqHistBlockedOnly && !e.blocked) continue;
                // Text filter: match against display name or frequency string
                if (!filterStr.empty()) {
                    std::string dn = _this->displayName(e.freqHz);
                    char freqBuf[32];
                    snprintf(freqBuf, sizeof(freqBuf), "%.4f", e.freqHz / 1e6);
                    std::string dnL = dn, fqL = freqBuf;
                    std::transform(dnL.begin(), dnL.end(), dnL.begin(), ::tolower);
                    std::transform(fqL.begin(), fqL.end(), fqL.begin(), ::tolower);
                    if (dnL.find(filterStr) == std::string::npos &&
                        fqL.find(filterStr) == std::string::npos) continue;
                }
                allEntries.push_back({k, &e});
            }

            // Sort: named bookmarks (letter-leading) first, alphabetically;
            //        unnamed (digit-leading) last, by frequency ascending.
            std::sort(allEntries.begin(), allEntries.end(), [&](auto& aa, auto& bb) {
                std::string na = _this->displayName(aa.second->freqHz);
                std::string nb = _this->displayName(bb.second->freqHz);
                bool aIsNamed = !na.empty() && std::isalpha((unsigned char)na[0]);
                bool bIsNamed = !nb.empty() && std::isalpha((unsigned char)nb[0]);
                if (aIsNamed != bIsNamed) return aIsNamed > bIsNamed; // named first
                std::string naL = na, nbL = nb;
                std::transform(naL.begin(), naL.end(), naL.begin(), ::tolower);
                std::transform(nbL.begin(), nbL.end(), nbL.begin(), ::tolower);
                if (naL != nbL) return naL < nbL;
                return aa.second->freqHz < bb.second->freqHz;
            });

            // Render flat list
            for (auto& [k, ep] : allEntries) {
                FreqEntry& e = *ep;
                std::string dn = _this->displayName(e.freqHz);

                if (e.blocked)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));

                std::string rel = relTime(e.lastSeen);
                ImGui::Text("%-22s %5d  %-10s", dn.c_str(), e.count, rel.c_str());
                if (ImGui::IsItemHovered()) _this->hoveredFreqHz = e.freqHz;

                if (e.blocked)
                    ImGui::PopStyleColor();

                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
                char editBtn[32];
                snprintf(editBtn, sizeof(editBtn), "D##edt_%lld", (long long)k);
                if (ImGui::SmallButton(editBtn)) {
                    _this->descEditKey   = k;
                    strncpy(_this->descEditBuf, e.description.c_str(), sizeof(_this->descEditBuf) - 1);
                    _this->descEditBuf[sizeof(_this->descEditBuf) - 1] = '\0';
                    _this->descEditRequest = true;
                }
                ImGui::SameLine();
                char chk[32];
                snprintf(chk, sizeof(chk), "##blk_%lld", (long long)k);
                if (ImGui::Checkbox(chk, &e.blocked))
                    needSave = true;

                // Show description line under the entry if set
                if (!e.description.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.65f, 1.0f));
                    ImGui::TextWrapped("      %s", e.description.c_str());
                    ImGui::PopStyleColor();
                }
            }
        }
        ImGui::EndChild();

        // Description edit popup (opened at parent scope so it actually renders)
        if (_this->descEditRequest) {
            ImGui::OpenPopup("##cb_desc_edit_popup");
            _this->descEditRequest = false;
        }
        if (ImGui::BeginPopup("##cb_desc_edit_popup")) {
            ImGui::Text("Edit description");
            ImGui::SetNextItemWidth(320.0f);
            ImGui::InputText("##cb_desc_edit_txt", _this->descEditBuf, sizeof(_this->descEditBuf));
            if (ImGui::Button("Save##cb_desc_save")) {
                std::lock_guard<std::mutex> lk(_this->freqLogMtx);
                auto it = _this->freqLog.find(_this->descEditKey);
                if (it != _this->freqLog.end()) {
                    it->second.description = _this->descEditBuf;
                    needSave = true;
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel##cb_desc_cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // Save outside the lock to avoid deadlock
        if (needSave) _this->saveFreqLog();
    }

    // ── Scan mode helpers ────────────────────────────────────────────────────

    void computeScanStops() {
        scanStops.clear();
        if (lastKnownSr <= 0.0) return;
        double step = lastKnownSr * bwUsage;
        for (auto& r : scanRanges) {
            if (r.stopHz <= r.startHz) continue;
            int n = std::max(1, (int)std::ceil((r.stopHz - r.startHz) / step));
            double s = (r.stopHz - r.startHz) / (double)n;
            for (int i = 0; i < n; i++)
                scanStops.push_back(r.startHz + s / 2.0 + i * s);
        }
    }

    void computeBookmarkScanStops() {
        bookmarkScanStops.clear();
        if (lastKnownSr <= 0.0) return;
        double usableBw = lastKnownSr * bwUsage;

        std::vector<double> freqs = getActiveManualFreqs();
        std::sort(freqs.begin(), freqs.end());
        if (freqs.empty()) return;

        // Greedy clustering: keep adding while the span fits within one SDR bandwidth
        int i = 0;
        while (i < (int)freqs.size()) {
            BookmarkScanStop stop;
            stop.freqsHz.push_back(freqs[i]);
            int j = i + 1;
            while (j < (int)freqs.size() && freqs[j] - freqs[i] <= usableBw) {
                stop.freqsHz.push_back(freqs[j]);
                j++;
            }
            stop.centerHz = (freqs[i] + freqs[j - 1]) / 2.0;
            bookmarkScanStops.push_back(std::move(stop));
            i = j;
        }
        flog::info("[ChannelBank] BkScan: {0} stops from {1} bookmarks",
                   (int)bookmarkScanStops.size(), (int)freqs.size());
    }

    // Manages slots for the current bookmark scan stop.
    // Returns true if any slot currently has signal or an open file.
    bool manageBookmarkScanChannels() {
        if (bookmarkScanStops.empty()) return false;
        auto& stop = bookmarkScanStops[bookmarkScanStopIdx];

        std::set<int>       localDetected;
        std::set<int>       localRawDetected;
        std::map<int,float> localSnrDb;
        {
            std::lock_guard<std::mutex> lk(manualDetectedMtx);
            localDetected    = manualDetected;
            localRawDetected = rawManualDetected;
            localSnrDb       = manualSnrDb;
        }

        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> clck(channelsMtx);

        // All freqs in the current stop are within SDR bandwidth by construction;
        // the offset check guards against stale lastKnownCenter during retune.
        std::set<int> desired;
        for (int i = 0; i < (int)stop.freqsHz.size(); i++) {
            double offset = stop.freqsHz[i] - lastKnownCenter;
            if (std::abs(offset) < lastKnownSr / 2.0 && isInActiveSpan(stop.freqsHz[i]))
                desired.insert(i);
        }

        // Destroy stale slots
        for (auto it = activeChannels.begin(); it != activeChannels.end(); ) {
            int idx = it->first;
            bool freqMismatch = desired.count(idx) > 0 && idx < (int)stop.freqsHz.size() &&
                                std::abs(it->second->freqHz - stop.freqsHz[idx]) > 500.0;
            if (desired.count(idx) == 0 || freqMismatch) {
                destroySlot(*it->second);
                delete it->second;
                it = activeChannels.erase(it);
                continue;
            }
            // Immediately tear down blocked channels (mirrors auto-mode behaviour)
            if (isBlocked(it->second->freqHz)) {
                flog::info("[ChannelBank] BkScan: removing blocked slot {0}", idx);
                destroySlot(*it->second);
                delete it->second;
                it = activeChannels.erase(it);
                continue;
            }
            // Tear down channels outside the active span trim
            if (!isInActiveSpan(it->second->freqHz)) {
                flog::info("[ChannelBank] BkScan: removing out-of-span slot {0}", idx);
                destroySlot(*it->second);
                delete it->second;
                it = activeChannels.erase(it);
                continue;
            }
            bool present    = localDetected.count(idx) > 0;
            bool rawPresent = it->second->rawSignalPresent.load();
            // Update lastDetected from rawSignalPresent as well as voted detection.
            // The management thread runs every 250ms, so using voted detection alone
            // means lastDetected can be up to 250ms stale when a signal starts to
            // dip — the hold timer effectively begins 250ms early, causing the file
            // to close prematurely and then immediately reopen on signal recovery.
            // rawSignalPresent is updated in the DSP thread every FFT frame (50ms),
            // so pinning lastDetected to it keeps the hold countdown accurate to
            // within ~100ms of actual signal loss.
            if (present || rawPresent) it->second->lastDetected = now;
            {
                float holdElapsed = std::chrono::duration<float>(now - it->second->lastDetected).count();
                float recDurSec = it->second->fileOpen
                    ? std::chrono::duration<float>(now - it->second->fileOpenTime).count() : 0.0f;
                int effectiveHoldMs = (recDurSec > 2.0f)
                    ? std::max(signalHoldMs, 2000) : signalHoldMs;
                it->second->signalPresent = present || rawPresent || (holdElapsed * 1000.0f < (float)effectiveHoldMs);
            }
            // rawSignalPresent maintained exclusively by the DSP thread (analyzeSpectrum).
            // Accumulate SNR while signal present and recording
            if (present && it->second->fileOpen) {
                auto sit = localSnrDb.find(idx);
                if (sit != localSnrDb.end()) {
                    it->second->snrSum   += sit->second;
                    it->second->snrCount += 1;
                }
            }
            ++it;
        }

        // Spawn slots for all desired freqs not yet active
        for (int i : desired) {
            if (activeChannels.find(i) != activeChannels.end()) continue;
            if ((int)activeChannels.size() >= maxChannels) continue;
            if (isBlocked(stop.freqsHz[i])) continue;
            double offset = stop.freqsHz[i] - lastKnownCenter;
            flog::info("[ChannelBank] BkScan: spawning slot {0} at {1:.3f}MHz", i, stop.freqsHz[i] / 1e6);
            auto* slot = new ChannelSlot();
            slot->lastDetected     = now;
            slot->signalPresent    = localDetected.count(i) > 0;
            slot->rawSignalPresent = localRawDetected.count(i) > 0;
            initSlot(*slot, i, 1, 0.0, offset);
            activeChannels[i] = slot;
        }

        // Return whether any slot is active with signal or recording
        bool anyActive = false;
        for (auto& [idx, slot] : activeChannels)
            if (slot->signalPresent || slot->fileOpen) { anyActive = true; break; }
        return anyActive;
    }

    void saveScanConfig() {
        config.acquire();
        config.conf[name]["scanMode"]     = scanMode;
        config.conf[name]["scanQuietSec"]    = scanQuietSec;
        config.conf[name]["scanNoSignalSec"] = scanNoSignalSec;
        auto& arr = config.conf[name]["scanRanges"];
        arr = nlohmann::json::array();
        for (auto& r : scanRanges)
            arr.push_back({ {"start", r.startHz}, {"stop", r.stopHz} });
        config.release(true);
    }

    // ── Manual mode helpers ──────────────────────────────────────────────────

    void saveManualConfig() {
        config.acquire();
        config.conf[name]["manualMode"]       = manualMode;
        config.conf[name]["bookmarkScanMode"] = bookmarkScanMode;
        auto& arr = config.conf[name]["manualFrequencies"];
        arr = nlohmann::json::array();
        for (double f : manualFrequencies) arr.push_back(f);
        auto& arr2 = config.conf[name]["boundBookmarkLists"];
        arr2 = nlohmann::json::array();
        for (auto& s : boundBookmarkLists) arr2.push_back(s);
        auto& arr3 = config.conf[name]["watchedFreqs"];
        arr3 = nlohmann::json::array();
        for (int64_t k : watchedFreqs) arr3.push_back(k);
        config.release(true);
    }

    // Rebuild boundFreqs as the deduplicated union of all boundBookmarkLists.
    // Caller must hold manualFreqMtx (or call from constructor before threads start).
    void rebuildBoundFreqs() {
        boundFreqs.clear();
        for (auto& listName : boundBookmarkLists) {
            auto it = fmLists.find(listName);
            if (it == fmLists.end()) continue;
            for (double f : it->second) {
                bool dup = false;
                for (double bf : boundFreqs)
                    if (std::abs(bf - f) < 100.0) { dup = true; break; }
                if (!dup) boundFreqs.push_back(f);
            }
        }
    }

    // Hide all FM waterfall bookmark groups except the selected ones.
    // Saves original visibility states the first time it's called.
    // Safe no-op if FM is not loaded (fm_iface calls return false).
    void applyWaterfallVisibility() {
        if (!waterfallStateSaved) {
            savedShowOnWaterfall.clear();
            for (auto& n : fm_iface::getListNames())
                savedShowOnWaterfall[n] = fm_iface::isListShownOnWaterfall(n);
            waterfallStateSaved = true;
        }
        for (auto& n : fm_iface::getListNames())
            fm_iface::setListShownOnWaterfall(n, boundBookmarkLists.count(n) > 0);
    }

    // Restore FM waterfall visibility to the states saved by applyWaterfallVisibility().
    void restoreWaterfallVisibility() {
        if (!waterfallStateSaved) return;
        for (auto& [n, v] : savedShowOnWaterfall)
            fm_iface::setListShownOnWaterfall(n, v);
        savedShowOnWaterfall.clear();
        waterfallStateSaved = false;
    }

    // Build the manual-mode frequency list as the union of user-entered
    // frequencies and the bound bookmark lists (deduped within ±100 Hz).
    // BOTH the DSP thread (analyzeSpectrum) and the mgmt thread
    // (manageManualChannels) must use this same view, otherwise the indices
    // in `manualDetected` don't map to anything and channels never spawn.
    std::vector<double> getActiveManualFreqs() {
        std::lock_guard<std::mutex> lk(manualFreqMtx);
        std::vector<double> out = manualFrequencies;
        for (double bf : boundFreqs) {
            bool dup = false;
            for (double mf : out) {
                if (std::abs(mf - bf) < 100.0) { dup = true; break; }
            }
            if (!dup) out.push_back(bf);
        }
        return out;
    }

    void loadFMConfig() {
        fmLists.clear();
        std::map<int64_t, std::string> newNames;
        std::string path = root + "/frequency_manager_config.json";
        try {
            std::ifstream f(path);
            if (!f.is_open()) {
                std::lock_guard<std::mutex> lk(bookmarkNamesMtx);
                bookmarkNames.clear();
                return;
            }
            nlohmann::json j;
            f >> j;
            if (!j.contains("lists")) return;
            for (auto& [listName, listData] : j["lists"].items()) {
                std::vector<double> freqs;
                if (listData.contains("bookmarks"))
                    for (auto& [bmName, bm] : listData["bookmarks"].items())
                        if (bm.contains("frequency")) {
                            double hz = bm["frequency"].get<double>();
                            freqs.push_back(hz);
                            int64_t k = (int64_t)std::round(hz / 1000.0);
                            // First one wins if duplicates across lists
                            if (newNames.find(k) == newNames.end())
                                newNames[k] = bmName;
                        }
                if (!freqs.empty()) fmLists[listName] = freqs;
            }
            {
                std::lock_guard<std::mutex> lk(bookmarkNamesMtx);
                bookmarkNames = std::move(newNames);
            }
        } catch (...) {}
    }

    void manageManualChannels() {
        std::set<int>       localDetected;
        std::set<int>       localRawDetected;
        std::map<int,float> localSnrDb;
        {
            std::lock_guard<std::mutex> lk(manualDetectedMtx);
            localDetected    = manualDetected;
            localRawDetected = rawManualDetected;
            localSnrDb       = manualSnrDb;
        }
        std::vector<double> localFreqs = getActiveManualFreqs();

        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> clck(channelsMtx);

        // Build desired set (indices currently within SDR bandwidth and active span)
        std::set<int> desired;
        for (int i = 0; i < (int)localFreqs.size(); i++) {
            double offset = localFreqs[i] - lastKnownCenter;
            if (std::abs(offset) < lastKnownSr / 2.0 && isInActiveSpan(localFreqs[i]))
                desired.insert(i);
        }

        // Destroy slots that are stale: out of desired set, or frequency at their index
        // changed (happens when the user inserts/removes a mid-list entry while running).
        for (auto it = activeChannels.begin(); it != activeChannels.end(); ) {
            int idx = it->first;
            bool freqMismatch = desired.count(idx) > 0 && idx < (int)localFreqs.size() &&
                                std::abs(it->second->freqHz - localFreqs[idx]) > 500.0;
            if (desired.count(idx) == 0 || freqMismatch) {
                flog::info("[ChannelBank] Manual: removing slot {0}", idx);
                destroySlot(*it->second);
                delete it->second;
                it = activeChannels.erase(it);
                continue;
            }
            // Immediately tear down blocked channels (mirrors auto-mode behaviour).
            // Without this, blocking a freq left the slot alive forever in manual mode,
            // so unblocking had no effect (nothing to respawn since slot never left).
            if (isBlocked(it->second->freqHz)) {
                flog::info("[ChannelBank] Manual: removing blocked slot {0}", idx);
                destroySlot(*it->second);
                delete it->second;
                it = activeChannels.erase(it);
                continue;
            }
            // Tear down channels outside the active span trim
            if (!isInActiveSpan(it->second->freqHz)) {
                flog::info("[ChannelBank] Manual: removing out-of-span slot {0}", idx);
                destroySlot(*it->second);
                delete it->second;
                it = activeChannels.erase(it);
                continue;
            }
            bool present    = localDetected.count(idx) > 0;
            bool rawPresent = it->second->rawSignalPresent.load();
            // Update lastDetected from rawSignalPresent as well as voted detection.
            // The management thread runs every 250ms, so using voted detection alone
            // means lastDetected can be up to 250ms stale when a signal starts to
            // dip — the hold timer effectively begins 250ms early, causing the file
            // to close prematurely and then immediately reopen on signal recovery.
            // rawSignalPresent is updated in the DSP thread every FFT frame (50ms),
            // so pinning lastDetected to it keeps the hold countdown accurate to
            // within ~100ms of actual signal loss.
            if (present || rawPresent) it->second->lastDetected = now;
            {
                float holdElapsed = std::chrono::duration<float>(now - it->second->lastDetected).count();
                float recDurSec = it->second->fileOpen
                    ? std::chrono::duration<float>(now - it->second->fileOpenTime).count() : 0.0f;
                int effectiveHoldMs = (recDurSec > 2.0f)
                    ? std::max(signalHoldMs, 2000) : signalHoldMs;
                it->second->signalPresent = present || rawPresent || (holdElapsed * 1000.0f < (float)effectiveHoldMs);
            }
            // rawSignalPresent maintained exclusively by the DSP thread (analyzeSpectrum).
            // Accumulate SNR while signal present and recording
            if (present && it->second->fileOpen) {
                auto sit = localSnrDb.find(idx);
                if (sit != localSnrDb.end()) {
                    it->second->snrSum   += sit->second;
                    it->second->snrCount += 1;
                }
            }
            // Watch alert: fire on rising edge of signal presence
            if (present && !it->second->prevSignalPresent) {
                int64_t k = freqKey(localFreqs[idx]);
                std::lock_guard<std::mutex> wlk(manualFreqMtx);
                if (watchedFreqs.count(k) > 0) watchAlert.store(k);
            }
            it->second->prevSignalPresent = present;
            ++it;
        }

        // Spawn a slot for each desired freq not yet active
        for (int i : desired) {
            if (activeChannels.find(i) != activeChannels.end()) continue;
            if ((int)activeChannels.size() >= maxChannels) continue;
            if (isBlocked(localFreqs[i])) continue;
            double offset = localFreqs[i] - lastKnownCenter;
            flog::info("[ChannelBank] Manual: spawning slot {0} at {1:.3f}MHz", i, localFreqs[i] / 1e6);
            auto* slot = new ChannelSlot();
            slot->lastDetected     = now;
            slot->signalPresent    = localDetected.count(i) > 0;
            slot->rawSignalPresent = localRawDetected.count(i) > 0;
            initSlot(*slot, i, 1, 0.0, offset);
            activeChannels[i] = slot;
        }
    }

    // ── Members ──────────────────────────────────────────────────────────────

    std::string  name;
    std::string  root;
    bool         enabled       = true;
    bool         running       = false;
    std::mutex   runMtx;

    FolderSelect folderSelect;
    int          spacingId     = 2;         // default → 25kHz
    int          demodMode     = DEMOD_AM;
    int          ssbBfoHz      = 0;         // BFO trim for USB/LSB; positive = pitch up
    float        snrThreshold  = 4.0f;      // dB above noise floor — required to START a recording
    float        holdHysteresisDb = 4.0f;  // dB below snrThreshold that keeps an open recording alive
    float        cooldownSec   = 5.0f;      // seconds before destroying a quiet channel
    // Hard cap: force-close any recording open longer than this many seconds. Real
    // voice transmissions are short; a multi-minute "transmission" is interference
    // holding the channel open. Force-closing lets the static gate (below) judge and
    // discard each segment, and bounds queue/encode backlog. 0 disables the cap.
    float        maxRecordingSec   = 90.0f;
    // Static gate: discard a finished recording whose channel spectrum was
    // predominantly flat (broadband static / no carrier) rather than peaky (carrier
    // + voice sidebands). Immune to come-and-go interference because it judges the
    // whole recording's structure, not its temporal envelope.
    bool         staticGateEnabled  = true;
    float        staticGateFlatness = 0.55f; // spectral flatness ≥ this on a frame ⇒ "flat/static" (no carrier)
    float        staticGateVoiceFrac= 0.30f; // need ≥ this fraction of active frames carrier-present, else discard
    int          staticGateMinFrames= 30;    // need ≥ this many above-threshold frames before judging (~1.5s)
    // Drift gate: discard a recording whose carrier wandered in frequency (a faulty /
    // drifting transmitter — diagonal streaks in the waterfall) rather than holding a
    // fixed carrier like a healthy emitter. Catches the comb/spur interference that the
    // static gate can't (each drifting spur is a carrier, so it reads as "voice-like").
    bool         driftGateEnabled   = true;
    float        driftMaxStdHz      = 700.0f; // carrier-centroid stddev above this (Hz) ⇒ drifting ⇒ discard
    float        leftTrimFrac   = 0.0f;  // fraction of bandwidth to exclude from left edge
    float        rightTrimFrac  = 0.0f;  // fraction of bandwidth to exclude from right edge
    bool         draggingLeft   = false;
    bool         draggingRight  = false;
    int          signalHoldMs          = 500;    // hold signalPresent true N ms after last detection (dropout hysteresis)
    bool         recordingEnabled      = true;   // global recording on/off toggle
    // Transcription backend selector.  ChannelSlot::transcribeBackend stores the
    // raw int form of this enum so it can live in the slot struct (which is
    // declared above ChannelBankModule).  Order is config-stable — DO NOT
    // renumber without writing migration logic.
    enum TranscriptionBackend {
        TB_OFF              = 0,
        TB_APPLE_SPEECH     = 1,
        TB_WHISPER_ATC_LARGE  = 2,
        TB_WHISPER_ATC_MEDIUM = 3,
        TB_WHISPER_TURBO    = 4,
    };
    int          transcriptionBackend  = TB_OFF;
    // True if any non-Off backend is selected — most call sites just want to
    // know "is transcription on?", regardless of which backend.
    bool         transcriptionOn() const { return transcriptionBackend != TB_OFF; }
    bool         m4aEnabled            = false;  // encode to M4A after playback+transcription
    float        recGain       = 0.25f;     // linear gain applied before WAV write (~-12dB)
    int          minTransmissionMs = 300;   // discard recordings shorter than this
    int          tailMs            = 500;   // ms to keep recording after signal gone
    int          maxChannels   = 16;
    double       channelSpacing = 25000.0;
    float        bwUsage       = 0.8f;      // fraction of SDR bandwidth to use (avoids filter rolloff edges)
    bool         noiseReduction = false;    // RNNoise neural noise suppression on recordings
    float        nrMix          = 0.7f;    // 0=dry (original), 1=full NR

    // Scan mode
    struct ScanRange { double startHz, stopHz; };
    bool scanMode = false;
    std::vector<ScanRange> scanRanges;
    float scanQuietSec    = 3.0f;
    float scanNoSignalSec = 1.0f;
    // Runtime scan state (not persisted)
    std::vector<double> scanStops;
    int   scanStopIdx       = 0;
    bool  scanStopHadSignal = false;
    std::chrono::steady_clock::time_point lastSignalTime;
    char  scanStartBuf[32] = {};
    char  scanStopBuf[32]  = {};

    // Bookmark scan mode — clusters bookmarks by SDR bandwidth and hops between them
    bool bookmarkScanMode = false;
    struct BookmarkScanStop {
        double              centerHz;   // where to tune the SDR
        std::vector<double> freqsHz;    // bookmark freqs in this cluster
    };
    std::vector<BookmarkScanStop> bookmarkScanStops;
    int  bookmarkScanStopIdx   = 0;
    bool bookmarkScanHadSignal = false;

    // Manual mode
    bool manualMode = false;
    std::mutex manualFreqMtx;
    std::vector<double>   manualFrequencies;    // user-entered frequencies (custom additions)
    std::set<std::string> boundBookmarkLists;   // names of bound FM bookmark lists
    std::vector<double>   boundFreqs;           // union of boundBookmarkLists (refreshed periodically)
    // Waterfall visibility save/restore (Option A)
    std::map<std::string, bool> savedShowOnWaterfall; // saved FM visibility states before Manual mode
    bool                        waterfallStateSaved = false;
    std::chrono::steady_clock::time_point lastFmConfigRefresh{};
    char manualFreqInputBuf[64] = {};
    std::map<int, int> manualVotes;       // list-index → vote count (DSP thread only)
    std::set<int>       manualDetected;      // written by DSP, read by mgmt thread
    std::set<int>       rawManualDetected;   // un-voted; instant fade-out (under manualDetectedMtx)
    std::map<int,float> manualSnrDb;         // per-freq SNR dB; manual/bookmark scan mode
    std::mutex          manualDetectedMtx;
    std::set<int64_t> watchedFreqs;       // watched freq keys; protected by manualFreqMtx
    std::atomic<int64_t> watchAlert{0};   // non-zero = freqKey of watched freq that just fired

    // Description editor (UI thread only)
    int64_t descEditKey = 0;
    char    descEditBuf[256] = {};
    bool    descEditRequest = false;

    // Frequency history filter state (UI thread only)
    bool freqHistBlockedOnly = false;
    char freqHistFilter[64]  = {};

    // FM list cache (populated by loadFMConfig, UI thread only)
    std::map<std::string, std::vector<double>> fmLists;
    std::mutex bookmarkNamesMtx;
    std::map<int64_t, std::string> bookmarkNames;   // freqKey -> "Tower KLAX"

    // Shared IQ bus — one frontend binding fans out to all consumers via iqSplitter,
    // keeping the main signal-path thread's memcpy cost at O(1) regardless of slot count.
    dsp::stream<dsp::complex_t>*            sharedIqIn  = nullptr;
    dsp::routing::Splitter<dsp::complex_t>* iqSplitter  = nullptr;

    // FFT spectrum monitor
    dsp::stream<dsp::complex_t>*            specStream     = nullptr;
    dsp::sink::Handler<dsp::complex_t>*     specSink       = nullptr;
    fftwf_complex*                          fftIn          = nullptr;
    fftwf_complex*                          fftOut         = nullptr;
    fftwf_plan                              fftPlan;
    std::vector<float>                      hannWindow;
    // At very low sample rates (< FFT_SIZE * SPEC_ANALYSIS_HZ ≈ 164 kHz) the
    // DSP block has fewer than FFT_SIZE samples, so fftAccum is zero-padded.
    // Applying the full-size BH window to truncated data leaves it non-zero at
    // the cut point, causing sidelobes nearly as bad as Hann.  We cache a BH
    // window sized to the actual fill length (zero-padded to FFT_SIZE) and
    // recompute only when the sample rate changes.
    std::vector<float>                      fftWindow;     // active window (length FFT_SIZE, correct for current SR)
    int                                     fftWindowFill  = 0; // fill length fftWindow was built for
    std::vector<dsp::complex_t>             fftAccum;
    int                                     fftBufPos      = 0;
    int64_t                                 specSamplesUntilFFT = 0;  // countdown; analysis fires when ≤ 0
    std::atomic<int>                        debugDetectedCount { 0 };
    std::atomic<int>                        debugBlockedSkips  { 0 };
    std::atomic<int>                        debugCapSkips      { 0 };
    std::vector<float>                      avgPower;           // slow EMA power spectrum (alpha=0.15) — voting
    std::vector<float>                      instPower;          // instantaneous per-frame power (no EMA) — fade trigger
    std::map<int, int>                      rawSlotMisses;      // DSP-thread-only: consecutive below-threshold frames (auto mode)
    std::map<int, int>                      rawManualMisses;    // DSP-thread-only: consecutive below-threshold frames (manual mode)
    bool                                    widebandEvent = false; // DSP-thread-only: >40% of slots above threshold this frame
    float                                   globalNoiseFloor  = 0.0f; // median-smoothed floor used for detection (linear)
    float                                   displayNoiseFloor = 0.0f; // EMA-smoothed copy for display only
    std::deque<float>                       floorHistory;             // ring buffer of recent rawFloor values for median filter
    std::map<int, int>                      slotVotes;

    // Deferred retune — set by waterfall thread, consumed by DSP thread
    std::atomic<bool>                       retuneFlag { false };
    double                                  pendingRetuneSr     = 0.0;
    double                                  pendingRetuneCenter = 0.0;

    // Detected signals (written by DSP thread, read by mgmt thread)
    std::mutex              detectedMtx;
    std::set<int>           detectedSlots;
    std::map<int, double>   slotPeakOffsets;  // Hz from SDR center of peak energy bin per slot
    std::vector<float>      slotSnrDb;        // per-slot SNR dB (signal / noise floor); auto mode
    std::set<int>           rawDetectedSlots; // un-voted; instant fade-out (under detectedMtx)

    // Active demod/record channels (managed by mgmt thread)
    std::mutex                      channelsMtx;
    std::map<int, ChannelSlot*>     activeChannels;

    // Recently-destroyed channels — kept alive in the UI for 30 s so the user
    // can correlate to the waterfall and block after the channel is gone.
    // Protected by channelsMtx (destroySlot always called under that lock).
    struct RecentChannel {
        double freqHz;
        std::chrono::steady_clock::time_point destroyedAt;
    };
    std::vector<RecentChannel> recentChannels;

    // Frequency being hovered in any list panel — read by fftRedrawHandlerFunc
    // to draw a crosshair line on the main waterfall.  UI thread only.
    double hoveredFreqHz = 0.0;

    // Management thread
    std::thread             mgmtThread;
    std::atomic<bool>       mgmtRunning { false };
    std::condition_variable mgmtCv;
    std::mutex              mgmtWaitMtx;

    // Frequency history + blocklist
    struct FreqEntry {
        double       freqHz      = 0.0;
        int          count       = 0;
        bool         blocked     = false;
        int64_t      lastSeen    = 0;     // unix timestamp of most recent recording
        std::string  description;         // user-editable; lives in channel_bank_config.json
    };
    // keyed by round(freqHz / 1000) — kHz-level uniqueness
    std::map<int64_t, FreqEntry> freqLog;
    std::mutex                   freqLogMtx;

    int64_t freqKey(double hz) { return (int64_t)std::round(hz / 1000.0); }

    // Returns bookmark name if one exists within ±tolerance kHz, else "%.3f MHz"
    std::string displayName(double hz) {
        // Tolerance = half the channel spacing (in kHz), so grid-aligned detections
        // always match the nearest real-channel bookmark regardless of SDR center freq.
        const int64_t tolerance = std::max((int64_t)5, (int64_t)std::round(channelSpacing / 1000.0 / 2.0));
        int64_t target = freqKey(hz);
        {
            std::lock_guard<std::mutex> lk(bookmarkNamesMtx);
            // Exact hit first
            auto it = bookmarkNames.find(target);
            if (it != bookmarkNames.end()) return it->second;
            // Nearby scan: find closest within tolerance
            auto lo = bookmarkNames.lower_bound(target - tolerance);
            auto hi = bookmarkNames.upper_bound(target + tolerance);
            int64_t bestDist = tolerance + 1;
            std::string bestName;
            for (auto jt = lo; jt != hi; ++jt) {
                int64_t d = std::abs(jt->first - target);
                if (d < bestDist) { bestDist = d; bestName = jt->second; }
            }
            if (!bestName.empty()) return bestName;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%.3f MHz", hz / 1e6);
        return std::string(buf);
    }

    // Returns a filesystem-safe bookmark name for use in recording filenames,
    // or an empty string if no bookmark is found within ±channelSpacing/2.
    std::string bookmarkNameForFilename(double hz) {
        const int64_t tolerance = std::max((int64_t)5, (int64_t)std::round(channelSpacing / 1000.0 / 2.0));
        int64_t target = freqKey(hz);
        std::string found;
        {
            std::lock_guard<std::mutex> lk(bookmarkNamesMtx);
            auto it = bookmarkNames.find(target);
            if (it != bookmarkNames.end()) found = it->second;
            else {
                auto lo = bookmarkNames.lower_bound(target - tolerance);
                auto hi = bookmarkNames.upper_bound(target + tolerance);
                int64_t bestDist = tolerance + 1;
                for (auto jt = lo; jt != hi; ++jt) {
                    int64_t d = std::abs(jt->first - target);
                    if (d < bestDist) { bestDist = d; found = jt->second; }
                }
            }
        }
        if (found.empty()) return "";
        // Sanitize: replace characters illegal in filenames with underscores
        std::string out;
        for (char c : found) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<'  || c == '>' || c == '|' ||
                c == '\n' || c == '\r' || c == '\t')
                out += '_';
            else if (c == ' ')
                out += '_';
            else
                out += c;
        }
        // Strip trailing underscores and limit length
        while (!out.empty() && out.back() == '_') out.pop_back();
        if (out.size() > 32) out = out.substr(0, 32);
        return out;
    }

    static std::string relTime(int64_t ts) {
        if (ts == 0) return "never";
        int64_t secs = (int64_t)std::time(nullptr) - ts;
        if (secs <  60)  return "just now";
        if (secs <  3600) { char b[16]; snprintf(b, sizeof(b), "%lldm ago", (long long)(secs/60));   return b; }
        if (secs <  86400){ char b[16]; snprintf(b, sizeof(b), "%lldh ago", (long long)(secs/3600));  return b; }
                           { char b[16]; snprintf(b, sizeof(b), "%lldd ago", (long long)(secs/86400)); return b; }
    }

    bool isBlocked(double hz) {
        std::lock_guard<std::mutex> lk(freqLogMtx);
        auto it = freqLog.find(freqKey(hz));
        return it != freqLog.end() && it->second.blocked;
    }

    bool isInActiveSpan(double freqHz) const {
        if (lastKnownSr <= 0.0) return true;
        double lo = lastKnownCenter - lastKnownSr * 0.5;
        double hi = lastKnownCenter + lastKnownSr * 0.5;
        double leftCut  = lo + (hi - lo) * leftTrimFrac;
        double rightCut = hi - (hi - lo) * rightTrimFrac;
        return freqHz >= leftCut && freqHz <= rightCut;
    }

    // ── Transcription backend dispatch ──────────────────────────────────────
    // Each slot stamps its handle with `transcribeBackend` at create time so
    // later poll/cancel/destroy calls route to the same implementation that
    // produced the handle — the user could switch backends mid-stream without
    // breaking in-flight transcriptions.
#ifdef __APPLE__
    void* txTranscribeFile(int backend, const char* path) {
        switch (backend) {
            case TB_APPLE_SPEECH: return transcription::transcribeFile(path);
            case TB_WHISPER_ATC_LARGE:
                return transcription_whisper::transcribeFile(path,
                    transcription_whisper::Model::ATCLarge);
            case TB_WHISPER_ATC_MEDIUM:
                return transcription_whisper::transcribeFile(path,
                    transcription_whisper::Model::ATCMedium);
            case TB_WHISPER_TURBO:
                return transcription_whisper::transcribeFile(path,
                    transcription_whisper::Model::Turbo);
            default: return nullptr;
        }
    }
    void txCancel(int backend, void* h) {
        if (!h) return;
        if (backend == TB_APPLE_SPEECH) transcription::cancel(h);
        else if (backend >= TB_WHISPER_ATC_LARGE) transcription_whisper::cancel(h);
    }
    void txDestroy(int backend, void* h) {
        if (!h) return;
        if (backend == TB_APPLE_SPEECH) transcription::destroy(h);
        else if (backend >= TB_WHISPER_ATC_LARGE) transcription_whisper::destroy(h);
    }
    std::string txGetText(int backend, void* h) {
        if (!h) return {};
        if (backend == TB_APPLE_SPEECH) return transcription::getText(h);
        if (backend >= TB_WHISPER_ATC_LARGE) return transcription_whisper::getText(h);
        return {};
    }
    bool txIsFinal(int backend, void* h) {
        if (!h) return true;
        if (backend == TB_APPLE_SPEECH) return transcription::isFinal(h);
        if (backend >= TB_WHISPER_ATC_LARGE) return transcription_whisper::isFinal(h);
        return true;
    }
#endif

    void logRecording(double hz) {
        std::lock_guard<std::mutex> lk(freqLogMtx);
        auto  key  = freqKey(hz);
        auto& e    = freqLog[key];
        e.freqHz   = hz;
        e.count++;
        e.lastSeen = (int64_t)std::time(nullptr);
    }

    // (touchFreqLog removed) — permanent history is now populated only by logRecording()
    // when a recording is KEPT, so discarded interference no longer floods the list.
    // Frequencies stay blockable live (active list) and for 30 s after teardown (recent list).

    void saveFreqLog() {
        config.acquire();
        auto& arr = config.conf[name]["freqLog"];
        arr = nlohmann::json::array();
        std::lock_guard<std::mutex> lk(freqLogMtx);
        for (auto& [k, e] : freqLog) {
            arr.push_back({
                {"freq", e.freqHz},
                {"count", e.count},
                {"blocked", e.blocked},
                {"lastSeen", e.lastSeen},
                {"description", e.description},
            });
        }
        config.release(true);
    }

    // Display snapshot (written by DSP thread, read by UI thread)
    struct DisplaySnapshot {
        std::vector<float>  power;          // per-bin dB  (FFT_SIZE)
        float               threshDb = -120.0f; // global threshold line in dB
        std::vector<int>    slotCenterBin;  // pixel-mapping helper (auto mode)
        std::set<int>       detected;
        int                 numSlots = 0;
        float               dBmin = -120.0f;
        float               dBmax = -40.0f;
        // Manual mode display
        std::vector<int>    manualCenterBins;
        std::vector<bool>   manualActiveFlags;
    };
    std::mutex       displayMtx;
    DisplaySnapshot  displaySnap;

    // WAV → M4A encode queue (processed after playback + transcription both complete)
    struct EncodeState {
        bool        playbackDone      = false;
        bool        transcriptionDone = true;   // true = don't wait for transcription
        std::string transcript;                 // filled in when transcription finalises
        float       avgSnrDb          = 0.0f;  // average SNR over the recording
    };
    struct EncodeTask {
        std::string wavPath;
        std::string transcript;
        float       avgSnrDb = 0.0f;
    };
    std::map<std::string, EncodeState> pendingEncodes;
    std::mutex                         pendingEncodesMtx;
    std::deque<EncodeTask>             encodeQueue;
    std::mutex                         encodeQueueMtx;
    std::condition_variable            encodeQueueCv;
    std::thread                        encodeThread;
    std::atomic<bool>                  encodeThreadRunning { false };

    // Playback queue + monitor output
    // PlaybackEntry now carries the segments alongside the WAV path so the
    // playback thread can install them as `playingSegments` for the synced
    // display.  Empty segments = no sync overlay (Apple Speech / transcription off).
    struct PlaybackEntry {
        std::string path;
        double      freqHz;
        bool        deleteAfter;
#ifdef __APPLE__
        std::vector<transcription_whisper::Segment> segments;
#endif
    };
    std::deque<PlaybackEntry> playbackQueue;
#ifdef __APPLE__
    std::mutex  lastTranscriptMtx;
    std::string lastTranscriptText;  // most recently completed transcript
    std::string lastTranscriptName;  // displayName of the transcribed freq
    // Synced-playback state: the segments captured at playback start, plus an
    // atomic playback position (ms from the start of the WAV).  The UI thread
    // reads playbackPosMs without locking and walks playingSegments to find
    // which segment to highlight.  lastTranscriptMtx guards playingSegments.
    std::vector<transcription_whisper::Segment> playingSegments;
    std::atomic<int> playbackPosMs { -1 };  // -1 = not playing
    // Segments waiting for their WAV to start playback.  Populated by the
    // transcription poll loop (under pendingPlaybackSegmentsMtx) and consumed
    // by playbackThreadFunc before each WAV plays.  Path-keyed because
    // playbacks queue independently of when transcriptions finish.
    std::mutex pendingPlaybackSegmentsMtx;
    std::map<std::string, std::vector<transcription_whisper::Segment>> pendingPlaybackSegments;
#endif
    std::atomic<int64_t>            currentlyPlayingFreqKey { 0 };
    std::mutex                      playbackMtx;
    std::condition_variable         playbackCv;
    std::thread                     playbackThread;
    std::atomic<bool>               playbackRunning { false };
    dsp::stream<dsp::stereo_t>      monitorStream;
    SinkManager::Stream*            monitorSinkStream = nullptr;
    EventHandler<float>             monitorSrHandler;

    // SDR state
    double lastKnownSr     = 0.0;
    double lastKnownCenter = 0.0;

    EventHandler<double> retuneHandler;

    // Main waterfall draw hook — adds per-channel markers to gui::waterfall
    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;
};

MOD_EXPORT void _INIT_() {
    std::string root = (std::string)core::args["root"];
    if (!std::filesystem::exists(root + "/recordings")) {
        std::filesystem::create_directory(root + "/recordings");
    }
    json def = json({});
    config.setPath(root + "/channel_bank_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new ChannelBankModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* inst) {
    delete (ChannelBankModule*)inst;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
