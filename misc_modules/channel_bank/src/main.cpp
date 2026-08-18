#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define _USE_MATH_DEFINES
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif
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
#ifndef CB_NO_RNNOISE
#include <rnnoise.h>
#endif
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
#include <condition_variable>
#include <regex>
#include <queue>
#include <deque>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <memory>
#ifdef __APPLE__
#include "transcription.h"
#endif
#if defined(__APPLE__) || defined(_WIN32)
#include "transcription_whisper.h"
#include "encoding.h"
#endif
#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
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

#ifdef _WIN32
using WebSocket = SOCKET;
using WebSockLen = int;
static const WebSocket INVALID_WEB_SOCKET = INVALID_SOCKET;
#else
using WebSocket = int;
using WebSockLen = socklen_t;
static const WebSocket INVALID_WEB_SOCKET = -1;
#endif

#if defined(__APPLE__) || defined(_WIN32)
#ifdef _WIN32
static constexpr int kM4ATranscriptionMaxWaitSec = 60;
#else
static constexpr int kM4ATranscriptionMaxWaitSec = 180;
#endif
static constexpr int kM4AEncodeMaxAttempts = 6;
static constexpr int kM4AFsRetryAttempts = 5;
#endif

class ChannelBankModule;

#if defined(__APPLE__) || defined(_WIN32)
struct TranscriptionJob {
    std::string path;
    std::string name;
    int         backend = 0;
    void*       handle  = nullptr;
};
#endif

struct SDRPPServerSourceControlV1 {
    char host[1024];
    int port = 0;
    bool connected = false;
    bool running = false;
    bool ok = false;
};

struct ChannelSlot {
    int    gridIdx = 0;
    double freqHz  = 0.0;   // centroid-aligned (auto mode) — for VFO placement + display
    // Grid-aligned channel freq (lastKnownCenter + gridOffset).  Stable across the
    // centroid jitter, so it's the right key for blocking + freqLog history: one
    // grid slot ⇒ one history entry, blocks persist across re-spawns of the same
    // channel.  Same as freqHz in manual mode (no centroid).
    double gridFreqHz = 0.0;
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
    std::string                                currentFinalM4APath;
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
    std::atomic<float>                         sustainSnrDb { -120.0f }; // smoothed SNR for audio sustain only
    std::atomic<bool>                          sustainSnrValid { false };
    bool                                       prevSignalPresent = false;  // rising-edge detect for watch alert

    // Sample-accurate fade-out driven by rawSignalPresent (DSP thread only — no locking needed)
    int fadeOutRemaining   = 2400; // counts down from 50ms-worth of samples; reset to max while signal present
    int audioHoldRemaining = 0;    // independent post-detection hold before fade starts;
                                   // AM bypasses it to suppress carrier-AGC release noise

    // Pre-roll circular buffer — always running once warmup is done (DSP thread only).
    // When a file opens we flush the last PREROLL_SAMPLES of audio first so we
    // capture the start of the transmission that occurred before detection fired.
    static constexpr int PREROLL_SAMPLES = 19200;  // 400ms @ 48kHz
    std::vector<float> preRollBuf  = std::vector<float>(PREROLL_SAMPLES, 0.0f);
    std::vector<float> preRollTmp  = std::vector<float>(PREROLL_SAMPLES, 0.0f);  // scratch for flush
    int  preRollHead  = 0;   // next write position (wraps mod PREROLL_SAMPLES)
    int  preRollCount = 0;   // valid samples currently in buffer (0..PREROLL_SAMPLES)

#if defined(__APPLE__) || defined(_WIN32)
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

    // Stuck-noise guard — manual passband mode only.  This intentionally does not
    // try to classify "voice"; it looks for long recordings that stay continuously
    // active while the passband centroid wanders like local interference.
    std::atomic<int> noiseGuardFramesTotal  { 0 };
    std::atomic<int> noiseGuardFramesActive { 0 };
    double noiseGuardCentroidSum   = 0.0;
    double noiseGuardCentroidSumSq = 0.0;
    double noiseGuardWidthSum      = 0.0;

#ifndef CB_NO_RNNOISE
    DenoiseState*  nrState    = nullptr;
    float          nrInBuf[480] = {};
    int            nrInPos    = 0;
    int            rnVadFrames = 0;
    int            rnVadVoiceFrames = 0;
    float          rnVadSum = 0.0f;
#endif
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

    ChannelBankModule(std::string name) : folderSelect("%ROOT%/channel_bank/recordings") {
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
        if (config.conf[name].contains("stuckNoiseGuardEnabled"))
            stuckNoiseGuardEnabled = config.conf[name]["stuckNoiseGuardEnabled"];
        if (config.conf[name].contains("stuckNoiseGuardMinSec"))
            stuckNoiseGuardMinSec = config.conf[name]["stuckNoiseGuardMinSec"];
        if (config.conf[name].contains("stuckNoiseGuardActiveFrac"))
            stuckNoiseGuardActiveFrac = config.conf[name]["stuckNoiseGuardActiveFrac"];
        if (config.conf[name].contains("stuckNoiseGuardDriftHz"))
            stuckNoiseGuardDriftHz = config.conf[name]["stuckNoiseGuardDriftHz"];
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
        if (config.conf[name].contains("nmsRadiusSlots"))
            nmsRadiusSlots = config.conf[name]["nmsRadiusSlots"];
        if (config.conf[name].contains("playbackAutoFlushEnabled"))
            playbackAutoFlushEnabled = config.conf[name]["playbackAutoFlushEnabled"];
        if (config.conf[name].contains("playbackAutoFlushThreshold"))
            playbackAutoFlushThreshold = config.conf[name]["playbackAutoFlushThreshold"];
        if (config.conf[name].contains("playbackAutoFlushKeepLatest"))
            playbackAutoFlushKeepLatest = config.conf[name]["playbackAutoFlushKeepLatest"];
        if (config.conf[name].contains("bwUsage"))
            bwUsage = config.conf[name]["bwUsage"];
        if (config.conf[name].contains("noiseReduction"))
            noiseReduction = config.conf[name]["noiseReduction"];
        if (config.conf[name].contains("nrMix"))
            nrMix = config.conf[name]["nrMix"];
        if (config.conf[name].contains("rnVoiceGateEnabled"))
            rnVoiceGateEnabled = config.conf[name]["rnVoiceGateEnabled"];
        if (config.conf[name].contains("rnVoiceGateVoiceFrac"))
            rnVoiceGateVoiceFrac = config.conf[name]["rnVoiceGateVoiceFrac"];
        if (config.conf[name].contains("rnVoiceGateProbeSec"))
            rnVoiceGateProbeSec = config.conf[name]["rnVoiceGateProbeSec"];
        if (config.conf[name].contains("rnVoiceGateQuarantineSec"))
            rnVoiceGateQuarantineSec = config.conf[name]["rnVoiceGateQuarantineSec"];
        if (config.conf[name].contains("recPath"))
            folderSelect.setPath(config.conf[name]["recPath"]);
        if (config.conf[name].contains("freqLog")) {
            double sp = SPACINGS[std::clamp(spacingId, 0, 5)];
            for (auto& j : config.conf[name]["freqLog"]) {
                double hz  = j.value("freq", 0.0);
                FreqEntry e;
                // Snap stored freq to grid so blocks survive retuning
                e.freqHz  = std::round(hz / sp) * sp;
                e.count       = j.value("count", 0);
                e.blocked     = j.value("blocked", false);
                e.lastSeen    = j.value("lastSeen", (int64_t)0);
                e.description = j.value("description", std::string());
                if (e.count == 0 && !e.blocked && e.description.empty()) continue;
                int64_t key = freqKey(e.freqHz);
                auto& existing = freqLog[key];
                if (existing.freqHz == 0.0) {
                    existing = e;
                } else {
                    existing.count += e.count;
                    if (e.blocked) existing.blocked = true;
                    if (e.lastSeen > existing.lastSeen) existing.lastSeen = e.lastSeen;
                    if (existing.description.empty()) existing.description = e.description;
                }
            }
        }
        if (config.conf[name].contains("manualMode"))
            manualMode = config.conf[name]["manualMode"];
        if (config.conf[name].contains("bookmarkScanMode"))
            bookmarkScanMode = config.conf[name]["bookmarkScanMode"];
        if (config.conf[name].contains("manualPassbandLimit"))
            manualPassbandLimit = config.conf[name]["manualPassbandLimit"];
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
        if (config.conf[name].contains("portableRecordingGroup"))
            portableRecordingGroup = config.conf[name]["portableRecordingGroup"];
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
        if (config.conf[name].contains("normalizeRecordings"))
            normalizeRecordings = config.conf[name]["normalizeRecordings"];
        if (config.conf[name].contains("scanMode"))
            scanMode = config.conf[name]["scanMode"];
        if (config.conf[name].contains("scanQuietSec"))
            scanQuietSec = config.conf[name]["scanQuietSec"];
        if (config.conf[name].contains("scanNoSignalSec"))
            scanNoSignalSec = config.conf[name]["scanNoSignalSec"];
        if (config.conf[name].contains("scanRanges"))
            for (auto& j : config.conf[name]["scanRanges"])
                scanRanges.push_back({ j.value("start", 0.0), j.value("stop", 0.0) });
        if (config.conf[name].contains("autoStart"))
            autoStart = config.conf[name]["autoStart"];
        if (config.conf[name].contains("webControlEnabled"))
            webControlEnabled = config.conf[name]["webControlEnabled"];
        if (config.conf[name].contains("webControlPort"))
            webControlPort = config.conf[name]["webControlPort"];
        if (config.conf[name].contains("webControlBind"))
            webControlBind = config.conf[name]["webControlBind"].get<std::string>();
        strncpy(webControlBindBuf, webControlBind.c_str(), sizeof(webControlBindBuf) - 1);
        webControlBindBuf[sizeof(webControlBindBuf) - 1] = '\0';
        config.release();

        channelSpacing = SPACINGS[std::clamp(spacingId, 0, 5)];

        // Load FM bookmarks so displayName() can show names
        loadFMConfig();

        // Migrate to profile system and load active profile
        migrateToProfiles();

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
        stopWebServer();
        gui::menu.removeEntry(name);
        sigpath::sourceManager.onRetune.unbindHandler(&retuneHandler);
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        restoreWaterfallVisibility();  // always restore on unload, safe no-op if not saved
        if (running) { stop(); }
#if defined(__APPLE__) || defined(_WIN32)
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

        if (autoStart) {
            if (folderSelect.pathIsValid()) {
                start();
            }
            else {
                flog::warn("[ChannelBank] autoStart requested but recording path is invalid: {0}", folderSelect.path);
            }
        }
        if (webControlEnabled) startWebServer();
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
#if defined(__APPLE__) || defined(_WIN32)
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

#if defined(__APPLE__) || defined(_WIN32)
        cancelTranscriptionJobs();
#endif

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

#ifdef _WIN32
        if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            flog::error("[ChannelBank] normalizeWavFile: replace failed: {0}",
                std::to_string((unsigned long)GetLastError()));
            std::error_code cleanupEc;
            std::filesystem::remove(tmp, cleanupEc);
        }
#else
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) flog::error("[ChannelBank] normalizeWavFile: rename failed: {0}", ec.message());
#endif
    }

    void normalizeRecordingIfEnabled(const std::string& path) {
        if (normalizeRecordings) normalizeWavFile(path);
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
        auto bmInfo = bookmarkForFilename(slot.freqHz);
        std::string groupName = portableRecordingGroup ? "Portable-System" : bmInfo.listName;
        if (!groupName.empty()) groupName = sanitizeForFilename(groupName);
        if (!bmInfo.bmName.empty() && !groupName.empty())
            snprintf(buf, sizeof(buf), "%s_%s_%s_%.4fMHz_%02d-%02d-%02d_%02d-%02d-%04d.wav",
                groupName.c_str(), bmInfo.bmName.c_str(), name.c_str(), slot.freqHz / 1e6,
                ltm->tm_hour, ltm->tm_min, ltm->tm_sec,
                ltm->tm_mday, ltm->tm_mon + 1, ltm->tm_year + 1900);
        else if (!bmInfo.bmName.empty())
            snprintf(buf, sizeof(buf), "%s_%s_%.4fMHz_%02d-%02d-%02d_%02d-%02d-%04d.wav",
                bmInfo.bmName.c_str(), name.c_str(), slot.freqHz / 1e6,
                ltm->tm_hour, ltm->tm_min, ltm->tm_sec,
                ltm->tm_mday, ltm->tm_mon + 1, ltm->tm_year + 1900);
        else if (!groupName.empty())
            snprintf(buf, sizeof(buf), "%s_%s_%.4fMHz_%02d-%02d-%02d_%02d-%02d-%04d.wav",
                groupName.c_str(), name.c_str(), slot.freqHz / 1e6,
                ltm->tm_hour, ltm->tm_min, ltm->tm_sec,
                ltm->tm_mday, ltm->tm_mon + 1, ltm->tm_year + 1900);
        else
            snprintf(buf, sizeof(buf), "%s_%.4fMHz_%02d-%02d-%02d_%02d-%02d-%04d.wav",
                name.c_str(), slot.freqHz / 1e6,
                ltm->tm_hour, ltm->tm_min, ltm->tm_sec,
                ltm->tm_mday, ltm->tm_mon + 1, ltm->tm_year + 1900);
        std::filesystem::path requestedWav = expandString(folderSelect.path + "/" + buf);
        std::string path = requestedWav.string();
        slot.currentFinalM4APath.clear();

#if defined(__APPLE__) || defined(_WIN32)
        // Intermediate WAVs are scratch files when recording is disabled or
        // when the final output will be M4A. Keep those off network shares.
        if (!recordingEnabled || m4aEnabled) {
            std::error_code ec;
            std::filesystem::path scratchBase = std::filesystem::path(root) / "channel_bank" / "tmp" / "recordings";
            std::filesystem::create_directories(scratchBase, ec);
            if (ec) {
                ec.clear();
                scratchBase = std::filesystem::temp_directory_path(ec) / "sdrpp-channel-bank" / "recordings";
                if (!ec) std::filesystem::create_directories(scratchBase, ec);
            }
            if (!ec) {
                static std::atomic<uint64_t> scratchSeq { 0 };
                auto id = scratchSeq.fetch_add(1);
                path = (scratchBase / (std::to_string(id) + "-" + requestedWav.filename().string())).string();
            } else {
                flog::warn("[ChannelBank] Could not create local recording scratch directory; using recPath for intermediate WAV");
            }

            if (recordingEnabled && m4aEnabled) {
                std::filesystem::path finalM4A = requestedWav;
                finalM4A.replace_extension(".m4a");
                slot.currentFinalM4APath = finalM4A.string();
            }
        }
#endif

        slot.currentFilePath = path;
        flog::info("[ChannelBank] Opening file: {0}", path);
        if (!slot.currentFinalM4APath.empty()) {
            flog::info("[ChannelBank] Final M4A destination: {0}", slot.currentFinalM4APath);
        }
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
        slot.sustainSnrDb.store(-120.0f);
        slot.sustainSnrValid.store(false);
        slot.gateFramesAbove.store(0);   // fresh static-gate tally per recording
        slot.gateFramesVoice.store(0);
        slot.onAirFrames.store(0);       // fresh on-air (min-TX) tally per recording
#ifndef CB_NO_RNNOISE
        slot.rnVadFrames = 0;
        slot.rnVadVoiceFrames = 0;
        slot.rnVadSum = 0.0f;
#endif
        slot.driftSum             = 0.0; // fresh drift-gate stats per recording
        slot.driftSumSq           = 0.0;
        slot.noiseGuardFramesTotal.store(0);
        slot.noiseGuardFramesActive.store(0);
        slot.noiseGuardCentroidSum   = 0.0;
        slot.noiseGuardCentroidSumSq = 0.0;
        slot.noiseGuardWidthSum      = 0.0;
        slot.fadeOutRemaining     = 2400; // 50ms at 48kHz — signal is present at file open
        slot.audioHoldRemaining   = 0;    // never carry a previous recording's tail state forward
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
    struct WebUiAction {
        std::function<json()> fn;
        json result;
        std::string error;
        bool done = false;
        std::mutex mtx;
        std::condition_variable cv;
    };

    static void closeFd(WebSocket fd) {
        if (fd == INVALID_WEB_SOCKET) return;
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
    }

    static std::string socketErrorString(const char* what) {
#ifdef _WIN32
        return std::string(what) + " failed: WSA " + std::to_string(WSAGetLastError());
#else
        return std::string(what) + " failed: " + strerror(errno);
#endif
    }

    static bool isRetryableAcceptError() {
#ifdef _WIN32
        int err = WSAGetLastError();
        return err == WSAEINTR || err == WSAECONNABORTED;
#else
        return errno == EINTR || errno == ECONNABORTED || errno == EPROTO;
#endif
    }

    void processWebUiActions() {
        std::deque<std::shared_ptr<WebUiAction>> actions;
        {
            std::lock_guard<std::mutex> lk(webUiActionMtx);
            actions.swap(webUiActions);
        }
        for (auto& action : actions) {
            json result;
            std::string error;
            try {
                result = action->fn ? action->fn() : webStateSnapshot();
            }
            catch (const std::exception& e) {
                error = e.what();
            }
            catch (...) {
                error = "UI action failed";
            }
            {
                std::lock_guard<std::mutex> lk(action->mtx);
                action->result = result;
                action->error = error;
                action->done = true;
            }
            action->cv.notify_one();
        }
    }

    bool runOnUiThread(std::function<json()> fn, json& result, std::string& error) {
        auto action = std::make_shared<WebUiAction>();
        action->fn = std::move(fn);
        {
            std::lock_guard<std::mutex> lk(webUiActionMtx);
            webUiActions.push_back(action);
        }
        std::unique_lock<std::mutex> lk(action->mtx);
        if (!action->cv.wait_for(lk, std::chrono::seconds(5), [&] { return action->done; })) {
            error = "SDR++ UI thread did not respond";
            return false;
        }
        result = action->result;
        error = action->error;
        return error.empty();
    }
    static const char* demodModeName(int mode) {
        switch (mode) {
            case DEMOD_AM:  return "AM";
            case DEMOD_NFM: return "NFM";
            case DEMOD_WFM: return "WFM";
            case DEMOD_USB: return "USB";
            case DEMOD_LSB: return "LSB";
            default:        return "Unknown";
        }
    }

    int demodModeFromName(const std::string& mode) const {
        if (mode == "AM") return DEMOD_AM;
        if (mode == "NFM") return DEMOD_NFM;
        if (mode == "WFM") return DEMOD_WFM;
        if (mode == "USB") return DEMOD_USB;
        if (mode == "LSB") return DEMOD_LSB;
        return -1;
    }

    std::string detectionModeName() const {
        if (manualMode) return "manual";
        if (scanMode) return "scan";
        if (bookmarkScanMode) return "bookmark_scan";
        return "auto";
    }

    json channelBankSettingsJson() {
        return {
            {"mode", detectionModeName()},
            {"spacingId", spacingId},
            {"channelSpacingHz", channelSpacing},
            {"demodMode", demodModeName(demodMode)},
            {"snrThresholdDb", snrThreshold},
            {"maxChannels", maxChannels},
            {"bwUsage", bwUsage},
            {"recordingEnabled", recordingEnabled},
            {"minTransmissionMs", minTransmissionMs},
            {"signalHoldMs", signalHoldMs},
            {"tailMs", tailMs},
            {"scanQuietSec", scanQuietSec},
            {"scanNoSignalSec", scanNoSignalSec}
        };
    }

    bool applyChannelBankSettings(const json& body, std::string& error) {
        if (!body.is_object()) {
            error = "settings payload must be an object";
            return false;
        }
        bool structural = body.contains("mode") || body.contains("spacingId") || body.contains("demodMode");
        if (running && structural) {
            error = "stop Channel Bank before changing mode, spacing, or demod";
            return false;
        }

        if (body.contains("mode")) {
            if (!body["mode"].is_string()) {
                error = "mode must be a string";
                return false;
            }
            std::string mode = body["mode"].get<std::string>();
            if (mode == "auto") {
                if (manualMode || bookmarkScanMode) restoreWaterfallVisibility();
                manualMode = false;
                scanMode = false;
                bookmarkScanMode = false;
            }
            else if (mode == "manual") {
                manualMode = true;
                scanMode = false;
                bookmarkScanMode = false;
                if (!boundBookmarkLists.empty()) applyWaterfallVisibility();
            }
            else if (mode == "scan") {
                if (manualMode || bookmarkScanMode) restoreWaterfallVisibility();
                manualMode = false;
                scanMode = true;
                bookmarkScanMode = false;
            }
            else if (mode == "bookmark_scan") {
                if (manualMode) restoreWaterfallVisibility();
                manualMode = false;
                scanMode = false;
                bookmarkScanMode = true;
                if (!boundBookmarkLists.empty()) applyWaterfallVisibility();
            }
            else {
                error = "invalid Channel Bank mode";
                return false;
            }
            saveManualConfig();
            saveScanConfig();
        }

        if (body.contains("spacingId") && !body["spacingId"].is_number_integer()) {
            error = "channel spacing must be an integer preset";
            return false;
        }
        if (body.contains("demodMode") && !body["demodMode"].is_string()) {
            error = "demod mode must be a string";
            return false;
        }
        if (body.contains("snrThresholdDb") && !body["snrThresholdDb"].is_number()) {
            error = "snr threshold must be numeric";
            return false;
        }
        if (body.contains("maxChannels") && !body["maxChannels"].is_number_integer()) {
            error = "max channels must be an integer";
            return false;
        }
        if (body.contains("bwUsage") && !body["bwUsage"].is_number()) {
            error = "frequency span must be numeric";
            return false;
        }
        if (body.contains("recordingEnabled") && !body["recordingEnabled"].is_boolean()) {
            error = "recording must be true or false";
            return false;
        }
        if (body.contains("minTransmissionMs") && !body["minTransmissionMs"].is_number_integer()) {
            error = "min tx duration must be an integer";
            return false;
        }
        if (body.contains("signalHoldMs") && !body["signalHoldMs"].is_number_integer()) {
            error = "signal hold must be an integer";
            return false;
        }
        if (body.contains("tailMs") && !body["tailMs"].is_number_integer()) {
            error = "tail must be an integer";
            return false;
        }
        if (body.contains("scanQuietSec") && !body["scanQuietSec"].is_number()) {
            error = "scan quiet must be numeric";
            return false;
        }
        if (body.contains("scanNoSignalSec") && !body["scanNoSignalSec"].is_number()) {
            error = "no-signal skip must be numeric";
            return false;
        }

        config.acquire();
        if (body.contains("spacingId")) {
            spacingId = std::clamp(body["spacingId"].get<int>(), 0, 5);
            channelSpacing = SPACINGS[spacingId];
            config.conf[name]["spacingId"] = spacingId;
        }
        if (body.contains("demodMode")) {
            int next = demodModeFromName(body["demodMode"].get<std::string>());
            if (next < 0) {
                config.release();
                error = "invalid demod mode";
                return false;
            }
            demodMode = next;
            config.conf[name]["demodMode"] = demodMode;
        }
        if (body.contains("snrThresholdDb")) {
            snrThreshold = std::clamp(body["snrThresholdDb"].get<float>(), 1.0f, 30.0f);
            config.conf[name]["snrThreshold"] = snrThreshold;
        }
        if (body.contains("maxChannels")) {
            maxChannels = std::clamp(body["maxChannels"].get<int>(), 1, 256);
            config.conf[name]["maxChannels"] = maxChannels;
        }
        if (body.contains("bwUsage")) {
            bwUsage = std::clamp(body["bwUsage"].get<float>(), 0.5f, 1.0f);
            config.conf[name]["bwUsage"] = bwUsage;
        }
        if (body.contains("recordingEnabled")) {
            recordingEnabled = body["recordingEnabled"].get<bool>();
            config.conf[name]["recordingEnabled"] = recordingEnabled;
        }
        if (body.contains("minTransmissionMs")) {
            minTransmissionMs = std::clamp(body["minTransmissionMs"].get<int>(), 0, 10000);
            config.conf[name]["minTransmissionMs"] = minTransmissionMs;
        }
        if (body.contains("signalHoldMs")) {
            signalHoldMs = std::clamp(body["signalHoldMs"].get<int>(), 0, 5000);
            config.conf[name]["signalHoldMs"] = signalHoldMs;
        }
        if (body.contains("tailMs")) {
            tailMs = std::clamp(body["tailMs"].get<int>(), 100, 2000);
            config.conf[name]["tailMs"] = tailMs;
        }
        if (body.contains("scanQuietSec")) {
            scanQuietSec = std::clamp(body["scanQuietSec"].get<float>(), 1.0f, 30.0f);
            config.conf[name]["scanQuietSec"] = scanQuietSec;
        }
        if (body.contains("scanNoSignalSec")) {
            scanNoSignalSec = std::clamp(body["scanNoSignalSec"].get<float>(), 0.1f, 5.0f);
            config.conf[name]["scanNoSignalSec"] = scanNoSignalSec;
        }
        config.conf[name]["profiles"][activeProfileName] = snapshotProfile();
        config.conf[name]["activeProfile"] = activeProfileName;
        config.release(true);
        return true;
    }

    bool setFrequencyBlocked(double hz, bool blocked, std::string& error) {
        if (!std::isfinite(hz) || hz <= 0.0) {
            error = "hz must be positive";
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(freqLogMtx);
            auto& entry = freqLog[freqKey(hz)];
            if (entry.freqHz == 0.0) entry.freqHz = hz;
            entry.blocked = blocked;
        }
        saveFreqLog();
        mgmtCv.notify_one();
        return true;
    }

    std::string selectedSourceName() {
        std::string source;
        core::configManager.acquire();
        if (core::configManager.conf.contains("source")) {
            source = core::configManager.conf["source"].get<std::string>();
        }
        core::configManager.release();
        return source;
    }

    json sourceNamesJson() {
        json arr = json::array();
        for (auto& source : sigpath::sourceManager.getSourceNames()) {
            arr.push_back(source);
        }
        return arr;
    }

    enum ServerSourceControlCode {
        SERVER_SOURCE_CONTROL_GET = 1,
        SERVER_SOURCE_CONTROL_SET = 2,
        SERVER_SOURCE_CONTROL_CONNECT = 3,
        SERVER_SOURCE_CONTROL_DISCONNECT = 4
    };

    bool callServerSourceControl(int code, SDRPPServerSourceControlV1* inout) {
        if (!core::modComManager.interfaceExists("sdrpp_server_source.control.v1")) return false;
        return core::modComManager.callInterface(
            "sdrpp_server_source.control.v1", code, inout, inout);
    }

    json serverSourceStateJson() {
        json j;
        j["available"] = false;
        SDRPPServerSourceControlV1 state{};
        if (!callServerSourceControl(SERVER_SOURCE_CONTROL_GET, &state) || !state.ok) return j;

        j["available"] = true;
        j["host"] = state.host;
        j["port"] = state.port;
        j["connected"] = state.connected;
        j["sourceRunning"] = state.running;
        return j;
    }

    json webStateSnapshot() {
        json channels = json::array();
        json recent = json::array();
        {
            std::lock_guard<std::mutex> lk(channelsMtx);
            for (auto& [idx, slot] : activeChannels) {
                if (!slot) continue;
                channels.push_back({
                    {"slot", idx},
                    {"freqHz", slot->freqHz},
                    {"gridFreqHz", slot->gridFreqHz},
                    {"name", displayName(slot->freqHz)},
                    {"blocked", isBlocked(slot->gridFreqHz)},
                    {"recording", slot->fileOpen},
                    {"signalPresent", slot->signalPresent.load()},
                    {"rawSignalPresent", slot->rawSignalPresent.load()},
                    {"file", slot->currentFilePath}
                });
            }
            auto now = std::chrono::steady_clock::now();
            for (auto& ch : recentChannels) {
                auto ageMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - ch.destroyedAt).count();
                recent.push_back({
                    {"freqHz", ch.freqHz},
                    {"name", displayName(ch.freqHz)},
                    {"blocked", isBlocked(ch.freqHz)},
                    {"ageMs", ageMs}
                });
            }
        }

        int detectedCount = 0;
        {
            std::lock_guard<std::mutex> lk(detectedMtx);
            detectedCount = (int)detectedSlots.size();
        }

        int manualDetectedCount = 0;
        {
            std::lock_guard<std::mutex> lk(manualDetectedMtx);
            manualDetectedCount = (int)manualDetected.size();
        }

        int playbackQueued = 0;
        int64_t playingKey = currentlyPlayingFreqKey.load();
        {
            std::lock_guard<std::mutex> lk(playbackMtx);
            playbackQueued = (int)playbackQueue.size();
        }

        json history = json::array();
        {
            std::lock_guard<std::mutex> lk(freqLogMtx);
            int emitted = 0;
            for (auto it = freqLog.rbegin(); it != freqLog.rend() && emitted < 160; ++it, ++emitted) {
                const auto& e = it->second;
                history.push_back({
                    {"freqHz", e.freqHz},
                    {"name", displayName(e.freqHz)},
                    {"count", e.count},
                    {"blocked", e.blocked},
                    {"lastSeen", e.lastSeen},
                    {"description", e.description}
                });
            }
        }

        json j;
        j["module"] = name;
        j["enabled"] = enabled;
        j["running"] = running;
        j["mode"] = detectionModeName();
        j["demodMode"] = demodModeName(demodMode);
        j["centerHz"] = lastKnownCenter;
        j["waterfallCenterHz"] = gui::waterfall.getCenterFrequency();
        j["sampleRate"] = lastKnownSr;
        j["usableSpanHz"] = lastKnownSr > 0.0 ? lastKnownSr * bwUsage : 0.0;
        j["bwUsage"] = bwUsage;
        j["radioPlaying"] = gui::mainWindow.isPlaying();
        j["sdrppHeartbeat"] = webHeartbeat.fetch_add(1) + 1;
        j["serverTimeMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        j["selectedSource"] = selectedSourceName();
        j["sources"] = sourceNamesJson();
        j["sdrppServer"] = serverSourceStateJson();
        j["settings"] = channelBankSettingsJson();
        j["snrThresholdDb"] = snrThreshold;
        j["maxChannels"] = maxChannels;
        j["recordingEnabled"] = recordingEnabled;
        j["activeChannels"] = channels;
        j["recentChannels"] = recent;
        j["detectedSlots"] = detectedCount;
        j["manualDetected"] = manualDetectedCount;
        j["playbackQueued"] = playbackQueued;
        j["currentlyPlayingFreqKey"] = playingKey;
        j["history"] = history;
        if (scanMode) {
            j["scanStopIndex"] = scanStopIdx;
            j["scanStopCount"] = (int)scanStops.size();
        }
        if (bookmarkScanMode) {
            j["bookmarkScanStopIndex"] = bookmarkScanStopIdx;
            j["bookmarkScanStopCount"] = (int)bookmarkScanStops.size();
        }
#if defined(__APPLE__) || defined(_WIN32)
        {
            std::lock_guard<std::mutex> lk(lastTranscriptMtx);
            j["lastTranscriptName"] = lastTranscriptName;
            j["lastTranscriptText"] = lastTranscriptText;
        }
#endif
        return j;
    }

    std::string webIndexHtml() const {
        return R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Channel Bank Control</title>
<style>
:root { color-scheme: dark; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; background: #111; color: #f3f3f3; }
body { margin: 0; background: #111; }
main { max-width: 980px; margin: 0 auto; padding: 18px; }
header { display: flex; align-items: center; justify-content: space-between; gap: 12px; margin-bottom: 14px; }
h1 { font-size: 22px; margin: 0; font-weight: 650; }
button { border: 1px solid #4a4a4a; background: #202020; color: #fff; padding: 8px 12px; border-radius: 6px; font-size: 14px; }
button:hover { background: #2b2b2b; }
select, input { border: 1px solid #4a4a4a; background: #101010; color: #fff; padding: 8px 10px; border-radius: 6px; font-size: 14px; min-width: 0; }
.primary { background: #0d6efd; border-color: #2b7cff; }
.danger { background: #7c1d1d; border-color: #ad2f2f; }
.secondary { background: #26313d; border-color: #405166; }
.grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 8px; margin-bottom: 12px; }
.tile, section { background: #191919; border: 1px solid #303030; border-radius: 8px; padding: 12px; }
.controls { display: grid; grid-template-columns: 1.2fr 1fr auto auto; gap: 8px; align-items: end; }
.server-controls { display: none; grid-template-columns: 1.3fr .7fr auto auto; gap: 8px; align-items: end; margin-top: 10px; }
.settings-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 8px; align-items: end; }
.status-strip { display: grid; grid-template-columns: repeat(auto-fit, minmax(135px, 1fr)); gap: 8px; margin-top: 10px; }
.control { display: flex; flex-direction: column; gap: 5px; }
.slider-control { display: grid; grid-template-columns: 1fr auto; gap: 6px 10px; align-items: center; }
.slider-control .label { grid-column: 1 / 2; }
.slider-value { color: #ddd; font-size: 12px; min-width: 58px; text-align: right; font-variant-numeric: tabular-nums; }
input[type="range"] { grid-column: 1 / 3; padding: 0; width: 100%; accent-color: #2b7cff; }
.label { color: #aaa; font-size: 12px; }
.value { font-size: 20px; margin-top: 3px; }
.small-value { font-size: 15px; margin-top: 3px; }
section { margin-top: 12px; }
h2 { font-size: 15px; margin: 0 0 8px; color: #ddd; }
table { width: 100%; border-collapse: collapse; font-size: 13px; }
th, td { text-align: left; border-top: 1px solid #2d2d2d; padding: 7px 6px; }
th { color: #aaa; font-weight: 500; }
.muted { color: #999; }
.ok { color: #62d26f; }
.off { color: #ffb15c; }
.bad { color: #ff6b6b; }
pre { white-space: pre-wrap; margin: 0; color: #ddd; }
.heatmap { display: grid; grid-template-columns: repeat(auto-fill, minmax(132px, 1fr)); gap: 7px; }
.heat-cell { border: 1px solid #343434; border-radius: 7px; padding: 8px; background: #202020; cursor: pointer; min-height: 58px; display: flex; flex-direction: column; justify-content: space-between; gap: 5px; }
.heat-cell:hover { border-color: #7aa7ff; }
.heat-cell.blocked { border-color: #bc3d3d; background: #321818; }
.heat-cell.live { box-shadow: inset 0 0 0 1px #2b7cff; }
.heat-freq { font-size: 14px; font-variant-numeric: tabular-nums; }
.heat-name, .heat-meta { color: #aaa; font-size: 11px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.inline-action { padding: 4px 8px; font-size: 12px; }
.span-head { display: flex; justify-content: space-between; gap: 10px; align-items: baseline; margin-bottom: 8px; }
.span-waterfall { width: 100%; height: 180px; display: block; background: #090b0e; border: 1px solid #2b3440; border-radius: 7px; cursor: crosshair; }
@media (max-width: 720px) { .controls, .server-controls, .settings-grid, .status-strip { grid-template-columns: 1fr; } }
</style>
</head>
<body>
<main>
<header>
<h1>Channel Bank</h1>
</header>
<section>
<h2>SDR Source</h2>
<div class="controls">
<label class="control"><span class="label">Source</span><select id="source"></select></label>
<label class="control"><span class="label">Center MHz</span><input id="centerInput" inputmode="decimal" placeholder="157.06745"></label>
<button class="primary" id="radioPlay">Play</button>
<button class="danger" id="radioStop">Stop</button>
</div>
<div class="status-strip">
<div class="tile"><div class="label">SDR++</div><div class="small-value ok" id="sdrppStatus">Connected</div></div>
<div class="tile"><div class="label">Radio</div><div class="small-value" id="radioState">-</div></div>
<div class="tile"><div class="label">Sample Rate</div><div class="small-value" id="sampleRate">-</div><div class="small-value muted" id="freqSpan">Span -</div></div>
<div class="tile"><div class="label">Heartbeat</div><div class="small-value" id="heartbeat">-</div></div>
</div>
<div class="server-controls" id="serverControls">
<label class="control"><span class="label">Server host</span><input id="serverHost" autocomplete="off" placeholder="192.168.1.150"></label>
<label class="control"><span class="label">Port</span><input id="serverPort" inputmode="numeric" placeholder="8081"></label>
<button class="secondary" id="serverApply">Apply</button>
<button class="secondary" id="serverConnect">Connect</button>
</div>
</section>
<section>
<h2>Channel Bank</h2>
<div class="grid">
<div class="tile"><div class="label">State</div><div class="value" id="state">-</div></div>
<div class="tile"><div class="label">Mode</div><div class="value" id="mode">-</div></div>
<div class="tile"><div class="label">Center</div><div class="value" id="center">-</div></div>
<div class="tile"><div class="label">Active</div><div class="value" id="active">-</div></div>
</div>
<div class="controls">
<button class="primary" id="start">Start Channel Bank</button>
<button class="danger" id="stop">Stop Channel Bank</button>
<button class="secondary" id="monitorAudio">Monitor Audio</button>
<div class="muted" id="monitorState">Monitor stopped</div>
</div>
<div class="muted" id="settingsStatus" style="margin-top:8px">Settings ready</div>
<div class="settings-grid" style="margin-top:10px">
<label class="control"><span class="label">Mode</span><select id="cbMode"><option value="auto">Auto</option><option value="manual">Manual</option><option value="scan">Scan</option><option value="bookmark_scan">Bookmark Scan</option></select></label>
<label class="control slider-control"><span class="label">Channel spacing</span><span class="slider-value" id="cbSpacingValue">-</span><input id="cbSpacing" type="range" min="0" max="5" step="1"></label>
<label class="control"><span class="label">Demod</span><select id="cbDemod"><option>AM</option><option>NFM</option><option>WFM</option><option>USB</option><option>LSB</option></select></label>
<label class="control slider-control"><span class="label">SNR dB</span><span class="slider-value" id="cbSnrValue">-</span><input id="cbSnr" type="range" min="1" max="30" step="0.1"></label>
<label class="control slider-control"><span class="label">Max channels</span><span class="slider-value" id="cbMaxChannelsValue">-</span><input id="cbMaxChannels" type="range" min="1" max="256" step="1"></label>
<label class="control slider-control"><span class="label">Freq span</span><span class="slider-value" id="cbBwUsageValue">-</span><input id="cbBwUsage" type="range" min="50" max="100" step="1"></label>
<label class="control slider-control"><span class="label">Min TX ms</span><span class="slider-value" id="cbMinTxValue">-</span><input id="cbMinTx" type="range" min="0" max="10000" step="50"></label>
<label class="control slider-control"><span class="label">Signal hold ms</span><span class="slider-value" id="cbSignalHoldValue">-</span><input id="cbSignalHold" type="range" min="0" max="5000" step="50"></label>
<label class="control slider-control"><span class="label">TX tail ms</span><span class="slider-value" id="cbTailValue">-</span><input id="cbTail" type="range" min="100" max="2000" step="25"></label>
<label class="control slider-control"><span class="label">Scan quiet s</span><span class="slider-value" id="cbScanQuietValue">-</span><input id="cbScanQuiet" type="range" min="1" max="30" step="0.5"></label>
<label class="control slider-control"><span class="label">No-signal skip s</span><span class="slider-value" id="cbScanNoSignalValue">-</span><input id="cbScanNoSignal" type="range" min="0.1" max="5" step="0.1"></label>
<label class="control"><span class="label">Save recordings</span><button class="secondary" id="cbRecordingToggle" type="button">-</button></label>
</div>
</section>
<section>
<div class="span-head">
<h2>Activity Span</h2>
<div class="muted" id="spanRange">-</div>
</div>
<canvas class="span-waterfall" id="spanWaterfall"></canvas>
<div class="muted" id="spanStatus" style="margin-top:8px">Click lit activity to block or unblock it.</div>
</section>
<section>
<h2>Frequency Heat Map</h2>
<div class="heatmap" id="heatmap"></div>
<div class="muted" id="heatmapStatus" style="margin-top:8px">Click a frequency to block or unblock it.</div>
</section>
<section>
<h2>Active Channels</h2>
<table><thead><tr><th>Slot</th><th>Frequency</th><th>Name</th><th>Signal</th><th>Recording</th><th>Block</th></tr></thead><tbody id="channels"></tbody></table>
</section>
<section>
<h2>Playback / Transcript</h2>
<pre id="transcript" class="muted">No transcript yet.</pre>
</section>
<section>
<h2>History</h2>
<table><thead><tr><th>Frequency</th><th>Name</th><th>Count</th><th>Blocked</th></tr></thead><tbody id="history"></tbody></table>
</section>
</main>
<script>
const fmtMHz = hz => hz ? (hz / 1e6).toFixed(4) + " MHz" : "-";
const fmtRate = hz => hz > 0 ? (hz >= 1e6 ? (hz / 1e6).toFixed(3) + " MS/s" : Math.round(hz).toLocaleString() + " S/s") : "-";
const esc = value => String(value ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const spacingLabels = ["8.33 kHz", "12.5 kHz", "25 kHz", "50 kHz", "100 kHz", "200 kHz"];
let monitorCtx = null;
let monitorAbort = null;
let monitorNode = null;
let monitorRunning = false;
let monitorRunId = 0;
const monitorSampleRate = 48000;
const monitorBufferCapacity = monitorSampleRate * 2;
const monitorTargetSamples = Math.round(monitorSampleRate * 0.16);
const monitorMaxSamples = Math.round(monitorSampleRate * 0.42);
const monitorRing = new Float32Array(monitorBufferCapacity);
let monitorReadPos = 0;
let monitorWritePos = 0;
let monitorBufferedSamples = 0;
let monitorLastStatusMs = 0;
const pendingSettingTimers = new Map();
let currentRecordingEnabled = true;
const spanWaterfallFrames = [];
const spanWaterfallMaxFrames = 72;
let spanWaterfallKey = "";
let spanWaterfallClick = null;
const sliderFormatters = {
  cbSpacing: v => spacingLabels[Math.max(0, Math.min(5, Math.round(v)))] || "-",
  cbSnr: v => v.toFixed(1),
  cbMaxChannels: v => String(Math.round(v)),
  cbBwUsage: v => `${Math.round(v)}%`,
  cbMinTx: v => `${Math.round(v)} ms`,
  cbSignalHold: v => `${Math.round(v)} ms`,
  cbTail: v => `${Math.round(v)} ms`,
  cbScanQuiet: v => `${v.toFixed(1)} s`,
  cbScanNoSignal: v => `${v.toFixed(1)} s`
};
async function post(path, body) {
  const r = await fetch(path, {
    method: "POST",
    headers: body ? { "Content-Type": "application/json" } : undefined,
    body: body ? JSON.stringify(body) : undefined
  });
  if (!r.ok) {
    let msg = r.statusText;
    try { msg = (await r.json()).error || msg; } catch (_) {}
    throw new Error(msg);
  }
  await refresh();
}
function setControlValue(id, value) {
  const el = document.getElementById(id);
  if (document.activeElement !== el && value !== undefined && value !== null) el.value = value;
}
function setSliderValue(id, value, format) {
  const el = document.getElementById(id);
  if (!el || value === undefined || value === null) return;
  if (document.activeElement !== el) el.value = value;
  const out = document.getElementById(id + "Value");
  const formatter = format || sliderFormatters[id];
  if (out) out.textContent = formatter ? formatter(Number(el.value)) : el.value;
}
function updateSliderOutput(id) {
  const el = document.getElementById(id);
  const out = document.getElementById(id + "Value");
  if (!el || !out) return;
  const formatter = sliderFormatters[id];
  out.textContent = formatter ? formatter(Number(el.value)) : el.value;
}
async function postAndReport(path, body) {
  try {
    await post(path, body);
    return true;
  } catch (e) {
    console.warn(e);
    const settingsStatus = document.getElementById("settingsStatus");
    if (settingsStatus) settingsStatus.textContent = e?.message || "Save failed";
    await refresh();
    return false;
  }
}
async function saveSettingValue(key, value) {
  const settingsStatus = document.getElementById("settingsStatus");
  if (settingsStatus) settingsStatus.textContent = "Saving settings...";
  const ok = await postAndReport("/api/channel-bank/settings", { [key]: value });
  if (ok && settingsStatus) settingsStatus.textContent = "Settings saved";
}
async function blockFrequency(hz, blocked) {
  const status = document.getElementById("heatmapStatus");
  if (status) status.textContent = blocked ? "Blocking frequency..." : "Unblocking frequency...";
  const ok = await postAndReport("/api/frequency/block", { hz, blocked });
  if (status) status.textContent = ok ? (blocked ? "Frequency blocked" : "Frequency unblocked") : "Block action failed";
}
function renderHeatMap(s) {
  const rows = new Map();
  const put = (hz, data = {}) => {
    if (!hz || !Number.isFinite(Number(hz))) return;
    const key = Math.round(Number(hz) / 1000);
    const row = rows.get(key) || {
      freqHz: Number(hz),
      name: "",
      count: 0,
      blocked: false,
      live: false,
      recent: false,
      lastSeen: 0
    };
    row.freqHz = data.gridFreqHz || data.freqHz || row.freqHz;
    row.name = data.name || row.name;
    row.count = Math.max(row.count, data.count || 0);
    row.blocked = row.blocked || !!data.blocked;
    row.live = row.live || !!data.live;
    row.recent = row.recent || !!data.recent;
    row.lastSeen = Math.max(row.lastSeen, data.lastSeen || 0);
    rows.set(key, row);
  };
  (s.history || []).forEach(h => put(h.freqHz, h));
  (s.recentChannels || []).forEach(ch => put(ch.freqHz, { ...ch, recent: true, count: 1 }));
  (s.activeChannels || []).forEach(ch => put(ch.gridFreqHz || ch.freqHz, { ...ch, live: true, count: 2 }));
  const items = Array.from(rows.values()).sort((a, b) =>
    (Number(b.blocked) - Number(a.blocked)) ||
    (Number(b.live) - Number(a.live)) ||
    (b.count - a.count) ||
    (b.lastSeen - a.lastSeen) ||
    (a.freqHz - b.freqHz)
  ).slice(0, 72);
  const maxCount = Math.max(1, ...items.map(i => i.count || 0));
  const heatmap = document.getElementById("heatmap");
  if (!items.length) {
    heatmap.innerHTML = `<div class="muted">No active or logged frequencies yet.</div>`;
    return;
  }
  heatmap.innerHTML = items.map(item => {
    const heat = Math.max(0.18, Math.min(1, (item.count || 0) / maxCount));
    const bg = item.blocked
      ? `linear-gradient(135deg, rgba(124,29,29,${0.42 + heat * 0.35}), #201414)`
      : `linear-gradient(135deg, rgba(13,110,253,${0.16 + heat * 0.50}), rgba(35,49,61,0.75))`;
    const meta = [
      item.live ? "live" : "",
      item.recent ? "recent" : "",
      `${item.count || 0} hits`,
      item.blocked ? "blocked" : "open"
    ].filter(Boolean).join(" / ");
    return `<div class="heat-cell ${item.blocked ? "blocked" : ""} ${item.live ? "live" : ""}"
        style="background:${bg}"
        onclick="blockFrequency(${Number(item.freqHz).toFixed(0)}, ${item.blocked ? "false" : "true"})"
        title="${item.blocked ? "Unblock" : "Block"} ${esc(fmtMHz(item.freqHz))}">
        <div class="heat-freq">${esc(fmtMHz(item.freqHz))}</div>
        <div class="heat-name">${esc(item.name || "")}</div>
        <div class="heat-meta">${esc(meta)}</div>
      </div>`;
  }).join("");
}
function spanInfo(s) {
  const center = Number(s.centerHz || s.waterfallCenterHz || 0);
  const span = Number(s.usableSpanHz || ((s.sampleRate || 0) * (s.bwUsage || 0.8)));
  if (!Number.isFinite(center) || !Number.isFinite(span) || center <= 0 || span <= 0) return null;
  return { center, span, lo: center - span / 2, hi: center + span / 2 };
}
function spanX(freqHz, info, width) {
  return Math.round(((freqHz - info.lo) / info.span) * width);
}
function collectSpanPoints(s, info) {
  const points = [];
  const add = (freqHz, data = {}) => {
    const freq = Number(freqHz);
    if (!Number.isFinite(freq) || freq < info.lo || freq > info.hi) return;
    points.push({
      freqHz: freq,
      name: data.name || "",
      blocked: !!data.blocked,
      live: !!data.live,
      recent: !!data.recent,
      history: !!data.history,
      strength: Math.max(0.06, Math.min(1, Number(data.strength || 0.25)))
    });
  };
  (s.history || []).forEach(h => add(h.freqHz, { ...h, history: true, strength: Math.min(0.32, 0.08 + (h.count || 0) * 0.025) }));
  (s.recentChannels || []).forEach(ch => add(ch.freqHz, {
    ...ch,
    recent: true,
    strength: Math.max(0.18, 0.58 * (1 - Math.min(30000, ch.ageMs || 0) / 30000))
  }));
  (s.activeChannels || []).forEach(ch => add(ch.gridFreqHz || ch.freqHz, {
    ...ch,
    live: true,
    strength: ch.signalPresent ? 1 : 0.65
  }));
  return points;
}
function drawSpanWaterfall(s) {
  const canvas = document.getElementById("spanWaterfall");
  const status = document.getElementById("spanStatus");
  const range = document.getElementById("spanRange");
  const info = spanInfo(s);
  if (!canvas || !info) {
    if (range) range.textContent = "-";
    return;
  }
  range.textContent = `${fmtMHz(info.lo)} to ${fmtMHz(info.hi)}`;
  const cssWidth = Math.max(320, canvas.clientWidth || 800);
  const cssHeight = Math.max(140, canvas.clientHeight || 180);
  const dpr = window.devicePixelRatio || 1;
  if (canvas.width !== Math.round(cssWidth * dpr) || canvas.height !== Math.round(cssHeight * dpr)) {
    canvas.width = Math.round(cssWidth * dpr);
    canvas.height = Math.round(cssHeight * dpr);
  }
  const ctx = canvas.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const key = `${Math.round(info.center / 1000)}:${Math.round(info.span / 1000)}`;
  if (key !== spanWaterfallKey) {
    spanWaterfallKey = key;
    spanWaterfallFrames.length = 0;
  }
  const points = collectSpanPoints(s, info);
  spanWaterfallFrames.push(points.filter(p => p.live || p.recent));
  while (spanWaterfallFrames.length > spanWaterfallMaxFrames) spanWaterfallFrames.shift();
  spanWaterfallClick = { info, points, channelSpacingHz: Number(s.settings?.channelSpacingHz || s.channelSpacingHz || 0) };

  const width = cssWidth;
  const height = cssHeight;
  ctx.fillStyle = "#080a0d";
  ctx.fillRect(0, 0, width, height);
  ctx.fillStyle = "#111820";
  for (let i = 0; i <= 8; i++) {
    const x = Math.round((i / 8) * width);
    ctx.fillRect(x, 0, 1, height);
  }
  (s.history || []).forEach(h => {
    if (!h.freqHz || h.freqHz < info.lo || h.freqHz > info.hi) return;
    const x = spanX(h.freqHz, info, width);
    const alpha = h.blocked ? 0.42 : Math.min(0.24, 0.06 + (h.count || 0) * 0.015);
    ctx.fillStyle = h.blocked ? `rgba(210,70,70,${alpha})` : `rgba(70,130,210,${alpha})`;
    ctx.fillRect(x - 1, 0, 2, height);
  });
  const rowH = height / spanWaterfallMaxFrames;
  spanWaterfallFrames.forEach((frame, i) => {
    const y = height - (spanWaterfallFrames.length - i) * rowH;
    frame.forEach(p => {
      const x = spanX(p.freqHz, info, width);
      const alpha = p.blocked ? 0.95 : p.strength;
      ctx.fillStyle = p.blocked ? `rgba(255,74,74,${alpha})` : p.live ? `rgba(80,210,115,${alpha})` : `rgba(255,177,92,${alpha})`;
      ctx.fillRect(x - 3, y, 6, Math.max(2, rowH + 1));
    });
  });
  points.filter(p => p.live).slice(0, 20).forEach(p => {
    const x = spanX(p.freqHz, info, width);
    ctx.strokeStyle = p.blocked ? "#ff6b6b" : "#62d26f";
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, height);
    ctx.stroke();
    ctx.fillStyle = p.blocked ? "#ff9b9b" : "#d8ffe0";
    ctx.font = "11px -apple-system, BlinkMacSystemFont, Segoe UI, sans-serif";
    const label = (p.name || fmtMHz(p.freqHz)).slice(0, 18);
    ctx.fillText(label, Math.min(width - 96, Math.max(4, x + 5)), 14);
  });
  if (status && points.some(p => p.live || p.recent)) status.textContent = "Click lit activity to block or unblock it.";
}
function nearestSpanPoint(clientX) {
  const canvas = document.getElementById("spanWaterfall");
  if (!canvas || !spanWaterfallClick) return null;
  const rect = canvas.getBoundingClientRect();
  const x = clientX - rect.left;
  const info = spanWaterfallClick.info;
  const freq = info.lo + (x / Math.max(1, rect.width)) * info.span;
  const tolerance = Math.max((spanWaterfallClick.channelSpacingHz || 0) / 2, info.span / 160);
  let best = null;
  let bestDist = tolerance;
  spanWaterfallClick.points.filter(p => p.live || p.recent || p.blocked).forEach(p => {
    const dist = Math.abs(p.freqHz - freq);
    if (dist <= bestDist) {
      best = p;
      bestDist = dist;
    }
  });
  return best;
}
function coerceSettingValue(raw, read) {
  const value = read ? read(raw) : raw;
  if (value === null || value === undefined) return undefined;
  if (typeof value === "number" && !Number.isFinite(value)) return undefined;
  return value;
}
function setMonitorUi(active, text) {
  document.getElementById("monitorAudio").textContent = active ? "Stop Monitor" : "Monitor Audio";
  document.getElementById("monitorState").textContent = text;
}
function resetMonitorBuffer() {
  monitorReadPos = 0;
  monitorWritePos = 0;
  monitorBufferedSamples = 0;
}
function dropMonitorSamples(count) {
  const drop = Math.min(count, monitorBufferedSamples);
  monitorReadPos = (monitorReadPos + drop) % monitorBufferCapacity;
  monitorBufferedSamples -= drop;
}
function writeMonitorSample(sample) {
  if (monitorBufferedSamples >= monitorBufferCapacity) dropMonitorSamples(monitorBufferedSamples - monitorTargetSamples);
  monitorRing[monitorWritePos] = sample;
  monitorWritePos = (monitorWritePos + 1) % monitorBufferCapacity;
  monitorBufferedSamples++;
}
function updateMonitorBufferStatus() {
  const now = performance.now();
  if (now - monitorLastStatusMs < 400) return;
  monitorLastStatusMs = now;
  setMonitorUi(true, `Monitor live  ${Math.round(monitorBufferedSamples * 1000 / monitorSampleRate)} ms`);
}
function enqueuePcm16(bytes) {
  if (!monitorCtx || bytes.length < 2) return;
  const samples = Math.floor(bytes.length / 2);
  const view = new DataView(bytes.buffer, bytes.byteOffset, samples * 2);
  if (monitorBufferedSamples > monitorMaxSamples) dropMonitorSamples(monitorBufferedSamples - monitorTargetSamples);
  for (let i = 0; i < samples; i++) writeMonitorSample(view.getInt16(i * 2, true) / 32768);
  if (monitorBufferedSamples > monitorMaxSamples) dropMonitorSamples(monitorBufferedSamples - monitorTargetSamples);
  updateMonitorBufferStatus();
}
function startMonitorOutput() {
  resetMonitorBuffer();
  monitorNode = monitorCtx.createScriptProcessor(1024, 0, 1);
  monitorNode.onaudioprocess = e => {
    const out = e.outputBuffer.getChannelData(0);
    for (let i = 0; i < out.length; i++) {
      if (monitorBufferedSamples > 0) {
        out[i] = monitorRing[monitorReadPos];
        monitorReadPos = (monitorReadPos + 1) % monitorBufferCapacity;
        monitorBufferedSamples--;
      } else {
        out[i] = 0;
      }
    }
  };
  monitorNode.connect(monitorCtx.destination);
}
function stopMonitorOutput() {
  if (monitorNode) {
    monitorNode.disconnect();
    monitorNode.onaudioprocess = null;
  }
  monitorNode = null;
  resetMonitorBuffer();
}
async function startMonitorAudio() {
  if (monitorRunning) return;
  const runId = ++monitorRunId;
  monitorRunning = true;
  setMonitorUi(true, "Monitor connecting...");
  const AudioCtor = window.AudioContext || window.webkitAudioContext;
  let carry = new Uint8Array(0);
  try {
    if (!monitorCtx) {
      try { monitorCtx = new AudioCtor({ sampleRate: monitorSampleRate }); }
      catch (_) { monitorCtx = new AudioCtor(); }
    }
    await monitorCtx.resume();
    if (!monitorRunning || runId !== monitorRunId) return;
    monitorAbort = new AbortController();
    startMonitorOutput();
    const r = await fetch("/api/audio/live.pcm", { cache: "no-store", signal: monitorAbort.signal });
    if (!r.ok || !r.body) throw new Error("monitor stream unavailable");
    setMonitorUi(true, "Monitor live");
    const reader = r.body.getReader();
    while (true) {
      if (!monitorRunning || runId !== monitorRunId) break;
      const { value, done } = await reader.read();
      if (done) break;
      let chunk = value;
      if (carry.length) {
        const joined = new Uint8Array(carry.length + chunk.length);
        joined.set(carry, 0);
        joined.set(chunk, carry.length);
        chunk = joined;
        carry = new Uint8Array(0);
      }
      if (chunk.length & 1) {
        carry = chunk.slice(chunk.length - 1);
        chunk = chunk.slice(0, chunk.length - 1);
      }
      enqueuePcm16(chunk);
    }
  } catch (e) {
    if (monitorRunning && runId === monitorRunId && !monitorAbort?.signal.aborted) console.warn(e);
  } finally {
    if (runId === monitorRunId) {
      monitorRunning = false;
      monitorAbort = null;
      stopMonitorOutput();
      setMonitorUi(false, "Monitor stopped");
    }
  }
}
function stopMonitorAudio() {
  monitorRunning = false;
  monitorRunId++;
  if (monitorAbort) monitorAbort.abort();
  monitorAbort = null;
  stopMonitorOutput();
  if (monitorCtx && monitorCtx.state === "running") monitorCtx.suspend().catch(() => {});
  setMonitorUi(false, "Monitor stopped");
}
async function refresh() {
  try {
    const r = await fetch("/api/state", { cache: "no-store" });
    const s = await r.json();
    const now = s.serverTimeMs ? new Date(s.serverTimeMs).toLocaleTimeString() : new Date().toLocaleTimeString();
    document.getElementById("sdrppStatus").textContent = "Connected";
    document.getElementById("sdrppStatus").className = "small-value ok";
    document.getElementById("radioState").textContent = s.radioPlaying ? "Playing" : "Stopped";
    document.getElementById("radioState").className = "small-value " + (s.radioPlaying ? "ok" : "off");
    document.getElementById("sampleRate").textContent = fmtRate(s.sampleRate);
    document.getElementById("freqSpan").textContent = `Span ${fmtRate(s.usableSpanHz)}`;
    document.getElementById("heartbeat").textContent = `#${s.sdrppHeartbeat || 0}  ${now}`;
    document.getElementById("state").textContent = s.running ? "Running" : "Stopped";
    document.getElementById("state").className = "value " + (s.running ? "ok" : "off");
    document.getElementById("mode").textContent = s.mode + " / " + s.demodMode;
    document.getElementById("center").textContent = fmtMHz(s.centerHz);
    document.getElementById("active").textContent = (s.activeChannels || []).length + " / " + s.maxChannels;
    const source = document.getElementById("source");
    const prev = source.value;
    source.innerHTML = (s.sources || []).map(name =>
      `<option value="${esc(name)}">${esc(name)}</option>`
    ).join("");
    source.value = s.selectedSource || prev;
    source.disabled = !!s.radioPlaying;
    document.getElementById("radioPlay").disabled = !!s.radioPlaying;
    document.getElementById("radioStop").disabled = !s.radioPlaying;
    document.getElementById("centerInput").placeholder = s.waterfallCenterHz ? (s.waterfallCenterHz / 1e6).toFixed(6) : "157.06745";
    const server = s.sdrppServer || {};
    const serverControls = document.getElementById("serverControls");
    const showServer = s.selectedSource === "SDR++ Server" && server.available;
    serverControls.style.display = showServer ? "grid" : "none";
    if (showServer) {
      const host = document.getElementById("serverHost");
      const port = document.getElementById("serverPort");
      if (document.activeElement !== host) host.value = server.host || "";
      if (document.activeElement !== port) port.value = server.port || "";
      host.disabled = !!s.radioPlaying || !!server.connected;
      port.disabled = !!s.radioPlaying || !!server.connected;
      document.getElementById("serverApply").disabled = !!s.radioPlaying || !!server.connected;
      document.getElementById("serverConnect").textContent = server.connected ? "Disconnect" : "Connect";
      document.getElementById("serverConnect").disabled = !!s.radioPlaying;
    }
    const settings = s.settings || {};
    setControlValue("cbMode", settings.mode || s.mode || "auto");
    setSliderValue("cbSpacing", settings.spacingId);
    setControlValue("cbDemod", settings.demodMode || s.demodMode || "AM");
    setSliderValue("cbSnr", settings.snrThresholdDb, v => v.toFixed(1));
    setSliderValue("cbMaxChannels", settings.maxChannels, v => String(Math.round(v)));
    setSliderValue("cbBwUsage", Math.round((settings.bwUsage ?? s.bwUsage ?? 0.8) * 100));
    setSliderValue("cbMinTx", settings.minTransmissionMs, v => `${Math.round(v)} ms`);
    setSliderValue("cbSignalHold", settings.signalHoldMs, v => `${Math.round(v)} ms`);
    setSliderValue("cbTail", settings.tailMs, v => `${Math.round(v)} ms`);
    setSliderValue("cbScanQuiet", settings.scanQuietSec, v => `${v.toFixed(1)} s`);
    setSliderValue("cbScanNoSignal", settings.scanNoSignalSec, v => `${v.toFixed(1)} s`);
    currentRecordingEnabled = settings.recordingEnabled !== false;
    const recToggle = document.getElementById("cbRecordingToggle");
    recToggle.textContent = currentRecordingEnabled ? "On - keeping files" : "Off - monitor only";
    recToggle.className = currentRecordingEnabled ? "primary" : "danger";
    document.getElementById("cbMode").disabled = !!s.running;
    document.getElementById("cbSpacing").disabled = !!s.running;
    document.getElementById("cbDemod").disabled = !!s.running;
    drawSpanWaterfall(s);
    renderHeatMap(s);
    document.getElementById("channels").innerHTML = (s.activeChannels || []).map(ch =>
      `<tr><td>${ch.slot}</td><td>${esc(fmtMHz(ch.freqHz))}</td><td>${esc(ch.name || "")}</td><td>${ch.signalPresent ? "yes" : "no"}</td><td>${ch.recording ? "yes" : "no"}</td><td><button class="inline-action ${ch.blocked ? "danger" : "secondary"}" onclick="blockFrequency(${Number(ch.gridFreqHz || ch.freqHz).toFixed(0)}, ${ch.blocked ? "false" : "true"})">${ch.blocked ? "Unblock" : "Block"}</button></td></tr>`
    ).join("") || `<tr><td colspan="6" class="muted">No active channels</td></tr>`;
    document.getElementById("history").innerHTML = (s.history || []).map(h =>
      `<tr><td>${esc(fmtMHz(h.freqHz))}</td><td>${esc(h.name || "")}</td><td>${h.count}</td><td><button class="inline-action ${h.blocked ? "danger" : "secondary"}" onclick="blockFrequency(${Number(h.freqHz).toFixed(0)}, ${h.blocked ? "false" : "true"})">${h.blocked ? "Unblock" : "Block"}</button></td></tr>`
    ).join("") || `<tr><td colspan="4" class="muted">No history yet</td></tr>`;
    const tx = (s.lastTranscriptText || "").trim();
    document.getElementById("transcript").textContent = tx ? `${s.lastTranscriptName || "Last"}\n\n${tx}` : "No transcript yet.";
  } catch (e) {
    document.getElementById("state").textContent = "Disconnected";
    document.getElementById("state").className = "value off";
    document.getElementById("sdrppStatus").textContent = "Disconnected";
    document.getElementById("sdrppStatus").className = "small-value bad";
    document.getElementById("radioState").textContent = "-";
    document.getElementById("radioState").className = "small-value off";
  }
}
document.getElementById("start").onclick = () => postAndReport("/api/start");
document.getElementById("stop").onclick = () => postAndReport("/api/stop");
document.getElementById("source").onchange = e => postAndReport("/api/source", { name: e.target.value });
document.getElementById("radioPlay").onclick = () => postAndReport("/api/play");
document.getElementById("radioStop").onclick = () => postAndReport("/api/stop-radio");
document.getElementById("monitorAudio").onclick = () => {
  if (monitorRunning) stopMonitorAudio();
  else startMonitorAudio();
};
document.getElementById("cbRecordingToggle").onclick = () => {
  saveSettingValue("recordingEnabled", !currentRecordingEnabled);
};
document.getElementById("serverApply").onclick = () => {
  const host = document.getElementById("serverHost").value.trim();
  const port = Number(document.getElementById("serverPort").value);
  if (host && Number.isInteger(port)) postAndReport("/api/sdrpp-server", { host, port });
};
document.getElementById("serverConnect").onclick = async e => {
  const connecting = e.target.textContent !== "Disconnect";
  await postAndReport(connecting ? "/api/sdrpp-server/connect" : "/api/sdrpp-server/disconnect");
};
function saveSetting(id, key, read) {
  const el = document.getElementById(id);
  const queueSave = () => {
    const value = coerceSettingValue(el.value, read);
    if (value !== undefined) saveSettingValue(key, value);
  };
  if (el.tagName === "INPUT") {
    el.oninput = () => {
      if (el.type === "range") updateSliderOutput(id);
      clearTimeout(pendingSettingTimers.get(id));
      pendingSettingTimers.set(id, setTimeout(queueSave, 350));
    };
    el.onkeydown = e => {
      if (e.key === "Enter") {
        clearTimeout(pendingSettingTimers.get(id));
        queueSave();
        el.blur();
      }
    };
    el.onblur = () => {
      clearTimeout(pendingSettingTimers.get(id));
      queueSave();
    };
  } else {
    el.onchange = queueSave;
  }
}
saveSetting("cbMode", "mode");
saveSetting("cbSpacing", "spacingId", v => Number.parseInt(v, 10));
saveSetting("cbDemod", "demodMode");
saveSetting("cbSnr", "snrThresholdDb", Number);
saveSetting("cbMaxChannels", "maxChannels", v => Number.parseInt(v, 10));
saveSetting("cbBwUsage", "bwUsage", v => Number.parseInt(v, 10) / 100);
saveSetting("cbMinTx", "minTransmissionMs", v => Number.parseInt(v, 10));
saveSetting("cbSignalHold", "signalHoldMs", v => Number.parseInt(v, 10));
saveSetting("cbTail", "tailMs", v => Number.parseInt(v, 10));
saveSetting("cbScanQuiet", "scanQuietSec", Number);
saveSetting("cbScanNoSignal", "scanNoSignalSec", Number);
document.getElementById("centerInput").onchange = e => {
  const mhz = Number(e.target.value);
  if (Number.isFinite(mhz) && mhz > 0) postAndReport("/api/center", { hz: mhz * 1e6 });
};
document.getElementById("spanWaterfall").onclick = e => {
  const point = nearestSpanPoint(e.clientX);
  const status = document.getElementById("spanStatus");
  if (!point) {
    if (status) status.textContent = "Click closer to a lit frequency.";
    return;
  }
  blockFrequency(point.freqHz, !point.blocked);
};
refresh();
setInterval(refresh, 500);
</script>
</body>
</html>)HTML";
    }

    bool sendAll(WebSocket fd, const void* data, size_t size) {
        const char* p = (const char*)data;
        size_t left = size;
        while (left > 0) {
            int toSend = (int)std::min<size_t>(left, 64 * 1024);
            int n = send(fd, p, toSend, 0);
            if (n <= 0) return false;
            p += n;
            left -= (size_t)n;
        }
        return true;
    }

    void sendHttpResponse(WebSocket fd, const std::string& status, const std::string& contentType,
                          const std::string& body) {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << status << "\r\n"
            << "Content-Type: " << contentType << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Cache-Control: no-store\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            << "Access-Control-Allow-Headers: Content-Type\r\n"
            << "Connection: close\r\n\r\n"
            << body;
        std::string out = oss.str();
        sendAll(fd, out.data(), out.size());
    }

    static uint64_t steadyMs() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void publishLiveAudio(ChannelSlot& slot, const float* mono, int count) {
        if (liveAudioClients.load() <= 0 || !mono || count <= 0) return;

        int64_t slotKey = freqKey(slot.freqHz);
        uint64_t nowMs = steadyMs();
        int64_t selectedKey = liveAudioSelectedFreqKey.load();
        if (selectedKey != slotKey) {
            uint64_t lastMs = liveAudioSelectedMs.load();
            if (selectedKey != 0 && nowMs <= lastMs + 500) return;
            int64_t expected = selectedKey;
            if (!liveAudioSelectedFreqKey.compare_exchange_strong(expected, slotKey) &&
                expected != slotKey) {
                return;
            }
        }
        liveAudioSelectedMs.store(nowMs);

        std::vector<int16_t> chunk;
        chunk.resize((size_t)count);
        for (int i = 0; i < count; i++) {
            float s = std::clamp(mono[i], -1.0f, 1.0f);
            chunk[(size_t)i] = (int16_t)std::lround(s * 32767.0f);
        }

        std::unique_lock<std::mutex> lk(liveAudioMtx, std::try_to_lock);
        if (!lk.owns_lock()) {
            liveAudioDroppedChunks.fetch_add(1);
            return;
        }

        liveAudioQueuedSamples += chunk.size();
        liveAudioChunks.push_back(std::move(chunk));
        while (liveAudioQueuedSamples > LIVE_AUDIO_MAX_SAMPLES && !liveAudioChunks.empty()) {
            liveAudioQueuedSamples -= liveAudioChunks.front().size();
            liveAudioChunks.pop_front();
            liveAudioDroppedChunks.fetch_add(1);
        }
        lk.unlock();
        liveAudioCv.notify_one();
    }

    void handleLiveAudioStream(WebSocket fd) {
        int expected = 0;
        if (!liveAudioClients.compare_exchange_strong(expected, 1)) {
            sendHttpResponse(fd, "409 Conflict", "application/json",
                json({{"ok", false}, {"error", "live monitor already connected"}}).dump());
            return;
        }

        {
            std::lock_guard<std::mutex> lk(liveAudioMtx);
            liveAudioChunks.clear();
            liveAudioQueuedSamples = 0;
            liveAudioSelectedFreqKey.store(0);
            liveAudioSelectedMs.store(0);
        }

        std::ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: application/octet-stream\r\n"
            << "Cache-Control: no-store\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Connection: close\r\n"
            << "X-Audio-Format: pcm_s16le; rate=48000; channels=1\r\n\r\n";

        std::string header = hdr.str();
        bool ok = sendAll(fd, header.data(), header.size());
        auto nextAudioSend = std::chrono::steady_clock::now();
        while (ok && webServerRunning.load()) {
            std::vector<int16_t> chunk;
            {
                std::unique_lock<std::mutex> lk(liveAudioMtx);
                liveAudioCv.wait_for(lk, std::chrono::milliseconds(100), [&] {
                    return !liveAudioChunks.empty() || !webServerRunning.load();
                });
                if (!webServerRunning.load()) break;
                if (!liveAudioChunks.empty()) {
                    chunk = std::move(liveAudioChunks.front());
                    liveAudioQueuedSamples -= chunk.size();
                    liveAudioChunks.pop_front();
                }
            }
            size_t sendSamples = chunk.empty() ? 4800 : chunk.size();
            auto now = std::chrono::steady_clock::now();
            if (nextAudioSend < now - std::chrono::milliseconds(250)) {
                nextAudioSend = now;
            }
            if (nextAudioSend > now) {
                std::this_thread::sleep_until(nextAudioSend);
            }
            if (chunk.empty()) {
                static const int16_t silence[4800] = {};
                ok = sendAll(fd, silence, sizeof(silence));
            } else {
                ok = sendAll(fd, chunk.data(), chunk.size() * sizeof(int16_t));
            }
            nextAudioSend += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>((double)sendSamples / 48000.0));
        }

        liveAudioClients.store(0);
        std::lock_guard<std::mutex> lk(liveAudioMtx);
        liveAudioChunks.clear();
        liveAudioQueuedSamples = 0;
        liveAudioSelectedFreqKey.store(0);
        liveAudioSelectedMs.store(0);
    }

    json requestJsonBody(const std::string& reqText) {
        size_t pos = reqText.find("\r\n\r\n");
        if (pos == std::string::npos) return json::object();
        std::string body = reqText.substr(pos + 4);
        if (body.empty()) return json::object();
        try {
            return json::parse(body);
        }
        catch (...) {
            return json::object();
        }
    }

    bool sourceExists(const std::string& name) {
        auto sources = sigpath::sourceManager.getSourceNames();
        return std::find(sources.begin(), sources.end(), name) != sources.end();
    }

    void sendUiActionResponse(WebSocket fd, std::function<json()> fn) {
        json result;
        std::string error;
        if (!runOnUiThread(std::move(fn), result, error)) {
            sendHttpResponse(fd, "503 Service Unavailable", "application/json",
                json({{"ok", false}, {"error", error.empty() ? "UI action failed" : error}}).dump());
            return;
        }
        std::string status = result.value("_httpStatus", std::string("200 OK"));
        result.erase("_httpStatus");
        sendHttpResponse(fd, status, "application/json", result.dump());
    }

    void handleWebClient(WebSocket fd) {
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        char buf[4096];
        int n = recv(fd, buf, (int)sizeof(buf) - 1, 0);
        if (n <= 0) return;
        buf[n] = '\0';
        std::string reqText(buf, (size_t)n);

        std::istringstream req(reqText);
        std::string method, path, version;
        req >> method >> path >> version;

        if (method == "OPTIONS") {
            sendHttpResponse(fd, "204 No Content", "text/plain", "");
            return;
        }
        if (method == "GET" && (path == "/" || path == "/index.html")) {
            sendHttpResponse(fd, "200 OK", "text/html; charset=utf-8", webIndexHtml());
            return;
        }
        if (method == "GET" && (path == "/api/state" || path == "/state")) {
            sendHttpResponse(fd, "200 OK", "application/json", webStateSnapshot().dump());
            return;
        }
        if (method == "GET" && path == "/api/sources") {
            sendHttpResponse(fd, "200 OK", "application/json", json({
                {"selected", selectedSourceName()},
                {"sources", sourceNamesJson()}
            }).dump());
            return;
        }
        if (method == "GET" && path == "/api/sdrpp-server") {
            sendHttpResponse(fd, "200 OK", "application/json", serverSourceStateJson().dump());
            return;
        }
        if (method == "GET" && path == "/api/channel-bank/settings") {
            sendHttpResponse(fd, "200 OK", "application/json", channelBankSettingsJson().dump());
            return;
        }
        if (method == "GET" && path == "/api/audio/live.pcm") {
            handleLiveAudioStream(fd);
            return;
        }
        if (method == "POST" && path == "/api/start") {
            sendUiActionResponse(fd, [this] {
                if (!folderSelect.pathIsValid()) {
                    return json({{"ok", false}, {"error", "recording path is invalid"}, {"_httpStatus", "409 Conflict"}});
                }
                start();
                return webStateSnapshot();
            });
            return;
        }
        if (method == "POST" && path == "/api/stop") {
            sendUiActionResponse(fd, [this] {
                stop();
                return webStateSnapshot();
            });
            return;
        }
        if (method == "POST" && path == "/api/channel-bank/settings") {
            json body = requestJsonBody(reqText);
            sendUiActionResponse(fd, [this, body] {
                std::string error;
                if (!applyChannelBankSettings(body, error)) {
                    return json({
                        {"ok", false},
                        {"error", error},
                        {"_httpStatus", running ? "409 Conflict" : "400 Bad Request"}
                    });
                }
                return webStateSnapshot();
            });
            return;
        }
        if (method == "POST" && path == "/api/frequency/block") {
            json body = requestJsonBody(reqText);
            double hz = body.value("hz", 0.0);
            bool blocked = body.value("blocked", true);
            sendUiActionResponse(fd, [this, hz, blocked] {
                std::string error;
                if (!setFrequencyBlocked(hz, blocked, error)) {
                    return json({{"ok", false}, {"error", error}, {"_httpStatus", "400 Bad Request"}});
                }
                return webStateSnapshot();
            });
            return;
        }
        if (method == "POST" && path == "/api/source") {
            json body = requestJsonBody(reqText);
            std::string source = body.value("name", std::string());
            sendUiActionResponse(fd, [this, source] {
                if (gui::mainWindow.isPlaying()) {
                    return json({{"ok", false}, {"error", "stop SDR before changing source"}, {"_httpStatus", "409 Conflict"}});
                }
                if (source.empty() || !sourceExists(source)) {
                    return json({{"ok", false}, {"error", "source not found"}, {"_httpStatus", "404 Not Found"}});
                }
                sigpath::sourceManager.selectSource(source);
                core::configManager.acquire();
                core::configManager.conf["source"] = source;
                core::configManager.release(true);
                return webStateSnapshot();
            });
            return;
        }
        if (method == "POST" && path == "/api/sdrpp-server") {
            json body = requestJsonBody(reqText);
            std::string host = body.value("host", std::string());
            int port = body.value("port", 0);
            sendUiActionResponse(fd, [this, host, port] {
                if (gui::mainWindow.isPlaying()) {
                    return json({{"ok", false}, {"error", "stop SDR before changing server target"}, {"_httpStatus", "409 Conflict"}});
                }
                if (!core::modComManager.interfaceExists("sdrpp_server_source.control.v1")) {
                    return json({{"ok", false}, {"error", "SDR++ Server source control unavailable"}, {"_httpStatus", "404 Not Found"}});
                }
                if (host.empty() || port <= 0 || port > 65535) {
                    return json({{"ok", false}, {"error", "host and valid port required"}, {"_httpStatus", "400 Bad Request"}});
                }
                SDRPPServerSourceControlV1 req{};
                strncpy(req.host, host.c_str(), sizeof(req.host) - 1);
                req.host[sizeof(req.host) - 1] = '\0';
                req.port = port;
                if (!callServerSourceControl(SERVER_SOURCE_CONTROL_SET, &req) || !req.ok) {
                    return json({{"ok", false}, {"error", "server target is busy or invalid"}, {"_httpStatus", "409 Conflict"}});
                }
                return webStateSnapshot();
            });
            return;
        }
        if (method == "POST" && path == "/api/sdrpp-server/connect") {
            sendUiActionResponse(fd, [this] {
                if (gui::mainWindow.isPlaying()) {
                    return json({{"ok", false}, {"error", "stop SDR before connecting server source"}, {"_httpStatus", "409 Conflict"}});
                }
                if (!core::modComManager.interfaceExists("sdrpp_server_source.control.v1")) {
                    return json({{"ok", false}, {"error", "SDR++ Server source control unavailable"}, {"_httpStatus", "404 Not Found"}});
                }
                SDRPPServerSourceControlV1 state{};
                bool ok = callServerSourceControl(SERVER_SOURCE_CONTROL_CONNECT, &state) && state.connected;
                json snapshot = webStateSnapshot();
                if (!ok) snapshot["_httpStatus"] = "502 Bad Gateway";
                return snapshot;
            });
            return;
        }
        if (method == "POST" && path == "/api/sdrpp-server/disconnect") {
            sendUiActionResponse(fd, [this] {
                if (gui::mainWindow.isPlaying()) {
                    return json({{"ok", false}, {"error", "stop SDR before disconnecting server source"}, {"_httpStatus", "409 Conflict"}});
                }
                if (!core::modComManager.interfaceExists("sdrpp_server_source.control.v1")) {
                    return json({{"ok", false}, {"error", "SDR++ Server source control unavailable"}, {"_httpStatus", "404 Not Found"}});
                }
                SDRPPServerSourceControlV1 state{};
                callServerSourceControl(SERVER_SOURCE_CONTROL_DISCONNECT, &state);
                return webStateSnapshot();
            });
            return;
        }
        if (method == "POST" && path == "/api/play") {
            sendUiActionResponse(fd, [this] {
                gui::mainWindow.setPlayState(true);
                sigpath::sourceManager.tune(gui::waterfall.getCenterFrequency());
                return webStateSnapshot();
            });
            return;
        }
        if (method == "POST" && (path == "/api/stop-radio" || path == "/api/radio/stop")) {
            sendUiActionResponse(fd, [this] {
                gui::mainWindow.setPlayState(false);
                return webStateSnapshot();
            });
            return;
        }
        if (method == "POST" && path == "/api/center") {
            json body = requestJsonBody(reqText);
            double hz = body.value("hz", 0.0);
            if (!std::isfinite(hz) || hz <= 0.0) {
                sendHttpResponse(fd, "400 Bad Request", "application/json",
                    json({{"ok", false}, {"error", "hz must be positive"}}).dump());
                return;
            }
            sendUiActionResponse(fd, [this, hz] {
                gui::waterfall.setCenterFrequency(hz);
                gui::waterfall.centerFreqMoved = true;
                sigpath::sourceManager.tune(hz);
                lastKnownCenter = hz;
                return webStateSnapshot();
            });
            return;
        }

        sendHttpResponse(fd, "404 Not Found", "application/json",
            json({{"ok", false}, {"error", "not found"}}).dump());
    }

    void webServerLoop() {
        while (webServerRunning.load()) {
            sockaddr_in clientAddr{};
            WebSockLen clientLen = sizeof(clientAddr);
            WebSocket clientFd = accept(webServerFd, (sockaddr*)&clientAddr, &clientLen);
            if (clientFd == INVALID_WEB_SOCKET) {
                if (webServerRunning.load() && isRetryableAcceptError()) {
                    continue;
                }
                break;
            }
            std::lock_guard<std::mutex> lk(webClientThreadsMtx);
            webClientThreads.emplace_back(&ChannelBankModule::webClientThreadMain, this, clientFd);
        }
    }

    void webClientThreadMain(WebSocket fd) {
        handleWebClient(fd);
        closeFd(fd);
    }

    bool parseWebBindAddress(in_addr& out) {
        if (webControlBind.empty() || webControlBind == "localhost") {
            out.s_addr = htonl(INADDR_LOOPBACK);
            return true;
        }
        if (webControlBind == "*" || webControlBind == "0.0.0.0") {
            out.s_addr = htonl(INADDR_ANY);
            return true;
        }
#ifdef _WIN32
        return InetPtonA(AF_INET, webControlBind.c_str(), &out) == 1;
#else
        return inet_pton(AF_INET, webControlBind.c_str(), &out) == 1;
#endif
    }

    std::string webDisplayBindAddress() const {
        if (webControlBind.empty() || webControlBind == "localhost") return "127.0.0.1";
        if (webControlBind == "*") return "0.0.0.0";
        return webControlBind;
    }

    void startWebServer() {
        std::lock_guard<std::mutex> lk(webServerMtx);
        if (webServerRunning.load()) return;
        webControlError.clear();

#ifdef _WIN32
        if (!webSocketsStarted) {
            WSADATA wsa{};
            int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsa);
            if (wsaResult != 0) {
                webControlError = "WSAStartup failed: " + std::to_string(wsaResult);
                return;
            }
            webSocketsStarted = true;
        }
#endif

        webServerFd = socket(AF_INET, SOCK_STREAM, 0);
        if (webServerFd == INVALID_WEB_SOCKET) {
            webControlError = socketErrorString("socket");
            return;
        }

        int one = 1;
        setsockopt(webServerFd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)webControlPort);
        if (!parseWebBindAddress(addr.sin_addr)) {
            webControlError = "invalid bind address: " + webControlBind;
            closeFd(webServerFd);
            webServerFd = INVALID_WEB_SOCKET;
            return;
        }

        if (bind(webServerFd, (sockaddr*)&addr, sizeof(addr)) != 0) {
            webControlError = socketErrorString("bind");
            closeFd(webServerFd);
            webServerFd = INVALID_WEB_SOCKET;
            return;
        }
        if (listen(webServerFd, 8) != 0) {
            webControlError = socketErrorString("listen");
            closeFd(webServerFd);
            webServerFd = INVALID_WEB_SOCKET;
            return;
        }

        webServerRunning.store(true);
        webServerThread = std::thread(&ChannelBankModule::webServerLoop, this);
        flog::info("[ChannelBank] Web control listening at http://{0}:{1}/", webDisplayBindAddress(), webControlPort);
    }

    void stopWebServer() {
        std::thread threadToJoin;
        WebSocket fdToClose = INVALID_WEB_SOCKET;
        {
            std::lock_guard<std::mutex> lk(webServerMtx);
            if (!webServerRunning.load() && !webServerThread.joinable()) return;
            webServerRunning.store(false);
            fdToClose = webServerFd;
            webServerFd = INVALID_WEB_SOCKET;
            if (webServerThread.joinable()) threadToJoin = std::move(webServerThread);
        }
        if (fdToClose != INVALID_WEB_SOCKET) {
#ifdef _WIN32
            shutdown(fdToClose, SD_BOTH);
#else
            shutdown(fdToClose, SHUT_RDWR);
#endif
            closeFd(fdToClose);
        }
        if (threadToJoin.joinable()) threadToJoin.join();
        liveAudioCv.notify_all();

        std::vector<std::thread> clientThreads;
        {
            std::lock_guard<std::mutex> lk(webClientThreadsMtx);
            clientThreads.swap(webClientThreads);
        }
        for (auto& t : clientThreads) {
            if (t.joinable()) t.join();
        }
    }

    void updateSustainSnr(ChannelSlot& slot, float instantSnrDb) {
        if (!std::isfinite(instantSnrDb)) return;

        constexpr float FRAME_SEC   = 1.0f / (float)SPEC_ANALYSIS_HZ;
        constexpr float ATTACK_SEC  = 0.12f;
        constexpr float RELEASE_SEC = 0.45f;

        if (!slot.sustainSnrValid.load()) {
            slot.sustainSnrDb.store(instantSnrDb);
            slot.sustainSnrValid.store(true);
            return;
        }

        float current = slot.sustainSnrDb.load();
        float tau = (instantSnrDb > current) ? ATTACK_SEC : RELEASE_SEC;
        float alpha = 1.0f - expf(-FRAME_SEC / tau);
        slot.sustainSnrDb.store(current + alpha * (instantSnrDb - current));
    }

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

        if (detectorFloorResetRequested.exchange(false)) {
            globalNoiseFloor  = 0.0f;
            displayNoiseFloor = 0.0f;
            floorHistory.clear();
            avgPower.clear();
            instPower.clear();
            slotVotes.clear();
            manualVotes.clear();
            rawSlotMisses.clear();
            rawManualMisses.clear();
            {
                std::lock_guard<std::mutex> lk(detectedMtx);
                detectedSlots.clear();
                rawDetectedSlots.clear();
            }
            {
                std::lock_guard<std::mutex> lk(manualDetectedMtx);
                manualDetected.clear();
                rawManualDetected.clear();
                manualSnrDb.clear();
            }
#ifndef CB_NO_RNNOISE
            {
                std::lock_guard<std::mutex> lk(rnVoiceQuarantineMtx);
                rnVoiceQuarantineUntil.clear();
            }
#endif
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
        // Instantaneous power drops to noise level in ONE frame (50 ms), so carrier
        // modes can begin fading after the normal 2-miss guard (≤100 ms) regardless
        // of how long or strong the transmission was. SSB gets a longer guard below
        // because normal inter-word pauses contain no carrier.
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
        std::vector<double> manualPassbandFreqs;
        if (manualMode && manualPassbandLimit)
            manualPassbandFreqs = getActiveManualFreqs();

        std::vector<uint8_t> manualPassbandMask;
        if (!manualPassbandFreqs.empty()) {
            manualPassbandMask.assign(FFT_SIZE, 0);
            for (double f : manualPassbandFreqs) {
                int pbLo = 0, pbHi = -1;
                if (!manualPassbandBinsForFreq(f, binHz, pbLo, pbHi)) continue;
                for (int b = pbLo; b <= pbHi; b++) manualPassbandMask[b] = 1;
            }
        }

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
            // Single pass over bins: accumulate mean, instantaneous mean, log-sum
            // (for spectral flatness), peak bin, AND energy-weighted centroid sum.
            // Previously the centroid was a separate second loop — merging saves
            // numSlots × (2×halfBins) redundant memory reads per frame.
            float  sum = 0.0f, instSum = 0.0f;
            double logSum = 0.0;       // Σ ln(power) for geometric mean → spectral flatness
            double weightedSum = 0.0;  // Σ (bin × power) for spectral centroid
            int   peakBin = lo;
            for (int b = lo; b <= hi; b++) {
                float p = power[b];
                sum     += p;
                instSum += instPower[b];
                logSum  += logf(std::max(p, 1e-20f));
                weightedSum += (double)b * (double)p;
                if (p > power[peakBin]) peakBin = b;
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
            if (!manualPassbandMask.empty()) {
                centerMeans.reserve(FFT_SIZE);
                for (int b = 0; b < FFT_SIZE; b++)
                    if (manualPassbandMask[b]) centerMeans.push_back(power[b]);
            } else {
                for (int s = leftSkip; s < numSlots - rightSkip; s++)
                    centerMeans.push_back(slotMeans[s]);
            }
            if (centerMeans.empty()) centerMeans = {slotMeans[numSlots / 2]};
            std::sort(centerMeans.begin(), centerMeans.end());
            float floorPct = manualPassbandMask.empty() ? 0.20f : 0.25f;
            float rawFloor = centerMeans[std::max(0, (int)(centerMeans.size() * floorPct) - 1)];
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
            if (!manualPassbandMask.empty()) {
                wbCenter = 0;
                for (int b = 0; b < FFT_SIZE; b++) {
                    if (!manualPassbandMask[b]) continue;
                    wbCenter++;
                    if (instPower[b] > globalNoiseFloor * snrLinear) wbAbove++;
                }
                widebandEvent = (wbCenter > 0 && wbAbove > wbCenter * 4 / 5);  // >80% of allowed passbands
            } else {
                for (int s = wbLeftEdge; s < numSlots - wbRightEdge; s++)
                    if (instSlotMeans[s] > globalNoiseFloor * snrLinear) wbAbove++;
                widebandEvent = (wbAbove > wbCenter * 2 / 5);  // >40%
            }
        }

        // Manual mode: check configured frequencies instead of grid voting
        if (manualMode) {
            std::vector<double> localFreqs = getActiveManualFreqs();
            std::set<int>       newDetected;
            std::set<int>       newRawDetected;
            std::map<int,float> newManualSnr;
            std::map<int,float> newManualInstantSnr;
            std::map<int,float> newManualCentroidHz;
            std::map<int,float> newManualWidthHz;
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
                if (globalNoiseFloor > 0.0f && instMean > 1e-30f)
                    newManualInstantSnr[i] = 10.0f * log10f(instMean / globalNoiseFloor);
                double centroidWeighted = 0.0;
                double centroidPower = 0.0;
                for (int b = lo; b <= hi; b++) {
                    double p = (double)power[b];
                    centroidWeighted += ((double)b - (double)centerBin) * binHz * p;
                    centroidPower += p;
                }
                float centroidHz = (centroidPower > 1e-30) ? (float)(centroidWeighted / centroidPower) : 0.0f;
                newManualCentroidHz[i] = centroidHz;
                double widthPower = 0.0;
                double widthWeighted = 0.0;
                for (int b = lo; b <= hi; b++) {
                    double p = (double)power[b];
                    double hz = ((double)b - (double)centerBin) * binHz;
                    double d = hz - (double)centroidHz;
                    widthWeighted += d * d * p;
                    widthPower += p;
                }
                newManualWidthHz[i] = (widthPower > 1e-30) ? (float)std::sqrt(widthWeighted / widthPower) : 0.0f;
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
                int lo5  = (int)(FFT_SIZE * 0.05f);
                int hi95 = (int)(FFT_SIZE * 0.95f);
                std::nth_element(sorted.begin(), sorted.begin() + lo5, sorted.end());
                displaySnap.dBmin = sorted[lo5] - 5.0f;
                std::nth_element(sorted.begin(), sorted.begin() + hi95, sorted.end());
                displaySnap.dBmax = sorted[hi95] + 15.0f;
            }
            // Immediately propagate raw detection to active slots — same FFT frame,
            // no management-thread hop. Use the normal 2-frame miss guard before
            // file-open and for AM. Other modes retain 4 frames (~200 ms) after
            // file-open so this AM-tail fix does not change their dropout behavior.
            if (!widebandEvent) {
                std::lock_guard<std::mutex> clck(channelsMtx);
                for (auto& [idx, slot] : activeChannels) {
                    const int missLimit = (slot->fileOpen && !slot->amDemod) ? 4 : 2;
                    bool above = (newRawDetected.count(idx) > 0);
                    auto ssit = newManualInstantSnr.find(idx);
                    if (slot->fileOpen && ssit != newManualInstantSnr.end())
                        updateSustainSnr(*slot, ssit->second);
                    if (slot->fileOpen && manualPassbandLimit && stuckNoiseGuardEnabled) {
                        slot->noiseGuardFramesTotal.fetch_add(1);
                        if (above) {
                            slot->noiseGuardFramesActive.fetch_add(1);
                            double c = 0.0;
                            auto cit = newManualCentroidHz.find(idx);
                            if (cit != newManualCentroidHz.end()) c = (double)cit->second;
                            slot->noiseGuardCentroidSum   += c;
                            slot->noiseGuardCentroidSumSq += c * c;
                            auto wit = newManualWidthHz.find(idx);
                            if (wit != newManualWidthHz.end())
                                slot->noiseGuardWidthSum += (double)wit->second;
                        }
                    }
                    if (above) {
                        rawManualMisses[idx] = 0;
                        slot->rawSignalPresent.store(true);
                        int hits = slot->rawConsecutiveHits.load() + 1;
                        slot->rawConsecutiveHits.store(hits > 4 ? 4 : hits); // cap to avoid int creep
                    } else if (++rawManualMisses[idx] >= missLimit) {
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

            // Greedy accept with configurable NMS radius.  Skip if ANY accepted
            // slot is within ±nmsRadius — this catches the case where a wide signal
            // (e.g. AM voice carrier + sidebands ~10–16 kHz total) spills detectable
            // energy into slots beyond ±1.  With radius=1, slot N+2 wasn't blocked
            // by N (only N+1 was), and since N+1 was suppressed-not-accepted, N+2
            // passed and we'd open a spurious recording on the sideband.
            // Radius 2 covers typical AM voice at 8.33–25 kHz channel spacing.
            // Sorted-by-power order ensures the carrier center wins first; sidebands
            // are then suppressed.
            const int nmsR = std::max(1, nmsRadiusSlots);
            for (int s : candidates) {
                bool blocked = false;
                for (int r = 1; r <= nmsR && !blocked; r++) {
                    if (s - r >= 0          && detected.count(s - r) > 0) blocked = true;
                    if (s + r < numSlots    && detected.count(s + r) > 0) blocked = true;
                }
                if (blocked) continue;
                detected.insert(s);
            }

            // Immediately propagate raw (un-voted) detection to active slots.
            // Uses instantaneous power with a miss debounce:
            //   • above threshold → reset miss counter, hold rawSignalPresent true
            //   • before file-open and for AM: 2 misses (fast stop)
            //   • other modes after file-open: retain 4 misses (~200 ms)
            //
            // During wideband events (lightning, etc.) the update is skipped entirely:
            // active real signals keep their current state; quiet channels don't get
            // their miss counter reset by broadband noise.
            if (!widebandEvent) {
                for (auto& [idx, slot] : activeChannels) {
                    const int missLimit = (slot->fileOpen && !slot->amDemod) ? 4 : 2;
                    float effSnrRaw = slot->fileOpen ? holdSnrLinear : snrLinear;
                    bool above = (idx < numSlots && instSlotMeans[idx] > globalNoiseFloor * effSnrRaw);
                    if (slot->fileOpen && idx < numSlots && globalNoiseFloor > 0.0f && instSlotMeans[idx] > 1e-30f) {
                        float instantSnrDb = 10.0f * log10f(instSlotMeans[idx] / globalNoiseFloor);
                        updateSustainSnr(*slot, instantSnrDb);
                    }
                    if (above) {
                        rawSlotMisses[idx] = 0;
                        slot->rawSignalPresent.store(true);
                        int hits = slot->rawConsecutiveHits.load() + 1;
                        slot->rawConsecutiveHits.store(hits > 4 ? 4 : hits);
                    } else if (++rawSlotMisses[idx] >= missLimit) {
                        slot->rawSignalPresent.store(false);
                        slot->rawConsecutiveHits.store(0);
                    }
                    // Until missLimit is reached, leave rawSignalPresent unchanged.

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
            // Auto-range: 5th/95th percentile for a clean y-axis.
            // nth_element is O(n) vs std::sort's O(n log n) — saves ~50% of
            // this block's CPU on 8192-element arrays at 20 Hz.
            std::vector<float> sorted = displaySnap.power;
            int lo5  = (int)(FFT_SIZE * 0.05f);
            int hi95 = (int)(FFT_SIZE * 0.95f);
            std::nth_element(sorted.begin(), sorted.begin() + lo5, sorted.end());
            displaySnap.dBmin = sorted[lo5] - 5.0f;
            std::nth_element(sorted.begin(), sorted.begin() + hi95, sorted.end());
            displaySnap.dBmax = sorted[hi95] + 15.0f;
        }

        mgmtCv.notify_one();
    }

    // ── Channel lifecycle ────────────────────────────────────────────────────

#if defined(__APPLE__) || defined(_WIN32)
    void pollTranscriptions() {
        struct CompletedTranscript {
            std::string path;
            std::string name;
            std::string text;
            std::vector<transcription_whisper::Segment> segments;
        };

        std::vector<CompletedTranscript> completed;
        {
            std::lock_guard<std::mutex> jlk(transcriptionJobsMtx);
            for (auto it = transcriptionJobs.begin(); it != transcriptionJobs.end();) {
                TranscriptionJob& job = it->second;
                if (!job.handle) {
                    it = transcriptionJobs.erase(it);
                    continue;
                }

                std::string text = txGetText(job.backend, job.handle);
                if (!txIsFinal(job.backend, job.handle)) {
                    ++it;
                    continue;
                }

                CompletedTranscript done;
                done.path = job.path;
                done.name = job.name;
                done.text = text;
                if (job.backend >= TB_WHISPER_ATC_LARGE) {
                    done.segments = transcription_whisper::getSegments(job.handle);
                }

                txDestroy(job.backend, job.handle);
                it = transcriptionJobs.erase(it);
                completed.push_back(std::move(done));
            }
        }

        for (auto& done : completed) {
            if (!done.segments.empty()) {
                std::lock_guard<std::mutex> sk(pendingPlaybackSegmentsMtx);
                pendingPlaybackSegments[done.path] = done.segments;
            }

            if (!done.text.empty()) {
                std::lock_guard<std::mutex> tlk(lastTranscriptMtx);
                lastTranscriptText = done.text;
                lastTranscriptName = done.name;
            }

            if (m4aEnabled && recordingEnabled) {
                bool canEncode = false;
                std::string encodePath, encodeFinalM4APath, encodeTranscript;
                float       encodeSnrDb = 0.0f;
                std::string transcriptForM4A = !done.segments.empty()
                    ? transcription_whisper::formatLrc(done.segments)
                    : done.text;
                {
                    std::lock_guard<std::mutex> elk(pendingEncodesMtx);
                    auto it = pendingEncodes.find(done.path);
                    if (it != pendingEncodes.end()) {
                        it->second.transcriptionDone = true;
                        it->second.transcript        = transcriptForM4A;
                        if (it->second.playbackDone) {
                            canEncode        = true;
                            encodePath       = it->first;
                            encodeFinalM4APath = it->second.finalM4APath;
                            encodeTranscript = it->second.transcript;
                            encodeSnrDb      = it->second.avgSnrDb;
                            pendingEncodes.erase(it);
                        }
                    }
                }
                if (canEncode) triggerEncode(encodePath, encodeFinalM4APath, encodeTranscript, encodeSnrDb);
            }
        }
    }

    void expirePendingEncodeWaits() {
        if (!m4aEnabled || !recordingEnabled || !transcriptionOn()) return;

        struct ExpiredEncode {
            std::string wavPath;
            std::string finalM4APath;
            float       avgSnrDb = 0.0f;
        };
        std::vector<ExpiredEncode> expired;
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(pendingEncodesMtx);
            for (auto it = pendingEncodes.begin(); it != pendingEncodes.end();) {
                EncodeState& es = it->second;
                if (!es.transcriptionDone) {
                    auto queuedAt = es.queuedAt;
                    if (queuedAt == std::chrono::steady_clock::time_point{}) {
                        queuedAt = (es.playbackDoneAt == std::chrono::steady_clock::time_point{})
                            ? now : es.playbackDoneAt;
                    }
                    auto waitSec = std::chrono::duration_cast<std::chrono::seconds>(now - queuedAt).count();
                    if (waitSec >= kM4ATranscriptionMaxWaitSec) {
                        if (!es.playbackDone && isCurrentlyPlaying(it->first)) {
                            ++it;
                            continue;
                        }
                        if (!es.playbackDone) {
                            removeFromPlaybackQueue(it->first);
                            clearPendingPlaybackSegments(it->first);
                        }
                        flog::warn("[ChannelBank] M4A encode waited {0}s after close for transcript; encoding without lyrics: {1}",
                                   kM4ATranscriptionMaxWaitSec, it->first);
                        expired.push_back({it->first, es.finalM4APath, es.avgSnrDb});
                        it = pendingEncodes.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }

        for (auto& task : expired) {
            triggerEncode(task.wavPath, task.finalM4APath, {}, task.avgSnrDb);
        }
    }

    bool isCurrentlyPlaying(const std::string& path) {
        std::lock_guard<std::mutex> lk(currentPlaybackPathMtx);
        return currentPlaybackPath == path;
    }

    bool removeFromPlaybackQueue(const std::string& path) {
        std::lock_guard<std::mutex> lk(playbackMtx);
        auto it = std::find_if(playbackQueue.begin(), playbackQueue.end(),
            [&](const auto& entry) { return entry.path == path; });
        if (it == playbackQueue.end()) return false;
        playbackQueue.erase(it);
        return true;
    }

    void clearPendingPlaybackSegments(const std::string& path) {
        std::lock_guard<std::mutex> lk(pendingPlaybackSegmentsMtx);
        pendingPlaybackSegments.erase(path);
    }
#endif

    void managementThreadFunc() {
        while (mgmtRunning) {
            std::unique_lock<std::mutex> ulck(mgmtWaitMtx);
            mgmtCv.wait_for(ulck, std::chrono::milliseconds(250));
            if (!mgmtRunning) { break; }

            auto now = std::chrono::steady_clock::now();

#if defined(__APPLE__) || defined(_WIN32)
            pollTranscriptions();
            expirePendingEncodeWaits();
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

            // Snapshot queued freq keys ONCE per management cycle instead of
            // scanning the entire playback queue per non-detected channel — O(Q)
            // up front vs O(C × Q) inside the teardown loop.
            std::set<int64_t> queuedFreqKeys;
            {
                std::lock_guard<std::mutex> plk(playbackMtx);
                for (auto& entry : playbackQueue)
                    queuedFreqKeys.insert(freqKey(entry.freqHz));
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
                    if (isBlocked(slotFreq) || isRnVoiceQuarantined(slotFreq)) { blkSkip++; continue; }
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
                    // (analyzeSpectrum NMS pass) with mode-aware miss debounce.
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

                // Immediately tear down blocked channels.  Key on gridFreqHz so this
                // matches the block-check at spawn (which used slotFreq = grid) — using
                // slot->freqHz here would be the centroid key, which doesn't match what
                // the UI's Block toggle wrote, and the slot would never get destroyed
                // (or it would, depending on which side of the kHz boundary the centroid
                // jittered to).  gridFreqHz makes the whole loop coherent.
                if (isBlocked(slot->gridFreqHz)) {
                    flog::info("[ChannelBank] Destroying blocked slot {0}", it->first);
                    destroySlot(*slot);
                    delete slot;
                    it = activeChannels.erase(it);
                    continue;
                }
                if (isRnVoiceQuarantined(slot->gridFreqHz)) {
                    flog::info("[ChannelBank] Destroying RNNoise-quarantined slot {0}", it->first);
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
                    bool isQueued  = (queuedFreqKeys.count(freqKey(slot->freqHz)) > 0);
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
        slot.freqHz     = lastKnownCenter + offset;
        // gridFreqHz: deterministic identity for blocking + freqLog keying.
        // Snap to nearest multiple of channelSpacing so the key is independent
        // of the SDR center frequency — retuning won't invalidate blocks.
        double rawGrid = isManual ? slot.freqHz : (lastKnownCenter + gridOffset);
        slot.gridFreqHz = std::round(rawGrid / channelSpacing) * channelSpacing;

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
            slot.fmDemod->init(&slot.vfo->out, audioSr, demodBw, true);
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
#ifndef CB_NO_RNNOISE
        if (noiseReduction || rnVoiceGateEnabled) {
            slot.nrState = rnnoise_create(nullptr);
            slot.nrInPos = 0;
        }
#endif

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
        // Push the GRID freq, not the centroid — so the recent-list block button
        // writes the same freqLog key the spawn/destroy checks use.
        // Deduplicate: if this freq is already recent, just refresh its timestamp.
        {
            int64_t k = freqKey(slot.gridFreqHz);
            bool found = false;
            for (auto& rc : recentChannels) {
                if (freqKey(rc.freqHz) == k) {
                    rc.destroyedAt = std::chrono::steady_clock::now();
                    found = true;
                    break;
                }
            }
            if (!found)
                recentChannels.push_back({slot.gridFreqHz, std::chrono::steady_clock::now()});
        }

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
                    static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                        slot.lastDetected - slot.fileOpenTime).count()));
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
                std::string stuckNoiseReason;
                bool stuckNoiseReject = slot.module->shouldRejectStuckNoise(slot, stuckNoiseReason);
                float rnVoiceFrac = 1.0f;
                bool rnVoiceReject = false;
#ifndef CB_NO_RNNOISE
                rnVoiceFrac = (slot.rnVadFrames > 0)
                    ? (float)slot.rnVadVoiceFrames / (float)slot.rnVadFrames : 1.0f;
                rnVoiceReject = slot.module->rnVoiceGateEnabled
                                && slot.rnVadFrames >= slot.module->rnVoiceGateMinFrames
                                && rnVoiceFrac < slot.module->rnVoiceGateVoiceFrac;
#endif
                if (stuckNoiseReject) {
                    flog::info("[ChannelBank] Discarding stuck-noise recording ({0}) slot {1}",
                               stuckNoiseReason, slot.gridIdx);
                }
                if (rnVoiceReject) {
                    flog::info("[ChannelBank] Discarding RNNoise non-voice recording (voice in {0}/{1} frames = {2:.0f}% < {3:.0f}%) slot {4}",
                               slot.rnVadVoiceFrames, slot.rnVadFrames, rnVoiceFrac * 100.0f,
                               slot.module->rnVoiceGateVoiceFrac * 100.0f, slot.gridIdx);
                    slot.module->quarantineRnVoice(slot.gridFreqHz);
                }
                if (staticReject || driftReject || stuckNoiseReject || rnVoiceReject || onAirMs < (int64_t)slot.module->minTransmissionMs) {
                    std::remove(slot.currentFilePath.c_str());
                } else {
                    // Recording meets the minimum duration — process it even though
                    // it was cut short by a stop/scan-advance/block rather than by
                    // the normal silence-tail path.
                    slot.module->normalizeRecordingIfEnabled(slot.currentFilePath);
                    if (slot.module->m4aEnabled && slot.module->recordingEnabled
                            && slot.module->encodeThreadRunning.load()) {
                        // Queue for direct encoding (no playback, no transcription).
                        float avgSnrDb = (slot.snrCount > 0)
                            ? slot.snrSum / (float)slot.snrCount : 0.0f;
                        slot.module->triggerEncode(slot.currentFilePath, slot.currentFinalM4APath, {}, avgSnrDb);
                    }
                    // If M4A encoding is off the WAV stays on disk as-is.
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
#ifndef CB_NO_RNNOISE
        if (slot.nrState) { rnnoise_destroy(slot.nrState); slot.nrState = nullptr; }
#endif
#if defined(__APPLE__) || defined(_WIN32)
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
        //                      threshold; goes FALSE after 2 consecutive misses in
        //                      AM, or 4 for other open recording modes.
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
        bool quarantined = _this->isRnVoiceQuarantined(slot->gridFreqHz);
        bool activeSignal = !quarantined && (slot->signalPresent.load() || rawOpen);

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
#ifndef CB_NO_RNNOISE
        if (slot->fileOpen && !forceClose && _this->rnVoiceGateEnabled && _this->rnVoiceGateProbeSec > 0.0f) {
            float rnVoiceFrac = (slot->rnVadFrames > 0)
                ? (float)slot->rnVadVoiceFrames / (float)slot->rnVadFrames : 1.0f;
            int probeFrames = std::max(_this->rnVoiceGateMinFrames,
                (int)std::ceil(_this->rnVoiceGateProbeSec * 100.0f));
            forceClose = slot->rnVadFrames >= probeFrames
                         && rnVoiceFrac < _this->rnVoiceGateVoiceFrac;
        }
#endif

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
                        static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                            slot->lastDetected - slot->fileOpenTime).count()));
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
                    std::string stuckNoiseReason;
                    bool stuckNoiseReject = _this->shouldRejectStuckNoise(*slot, stuckNoiseReason);
                    float rnVoiceFrac = 1.0f;
                    bool rnVoiceReject = false;
#ifndef CB_NO_RNNOISE
                    rnVoiceFrac = (slot->rnVadFrames > 0)
                        ? (float)slot->rnVadVoiceFrames / (float)slot->rnVadFrames : 1.0f;
                    rnVoiceReject = _this->rnVoiceGateEnabled
                                    && slot->rnVadFrames >= _this->rnVoiceGateMinFrames
                                    && rnVoiceFrac < _this->rnVoiceGateVoiceFrac;
#endif

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
                    } else if (stuckNoiseReject) {
                        flog::info("[ChannelBank] Discarding stuck-noise recording ({0}) slot {1}{2}",
                                   stuckNoiseReason, slot->gridIdx,
                                   forceClose ? " [dur-cap]" : "");
                        std::remove(slot->currentFilePath.c_str());
                    } else if (rnVoiceReject) {
                        flog::info("[ChannelBank] Discarding RNNoise non-voice recording (voice in {0}/{1} frames = {2:.0f}%% < {3:.0f}%%) slot {4}{5}",
                                   slot->rnVadVoiceFrames, slot->rnVadFrames, rnVoiceFrac * 100.0f,
                                   _this->rnVoiceGateVoiceFrac * 100.0f, slot->gridIdx,
                                   forceClose ? " [dur-cap]" : "");
                        _this->quarantineRnVoice(slot->gridFreqHz);
                        std::remove(slot->currentFilePath.c_str());
                    } else if (onAirMs < (int64_t)_this->minTransmissionMs) {
                        flog::info("[ChannelBank] Discarding short recording (on-air {0}ms < {1}ms threshold; span was {2}ms)", onAirMs, _this->minTransmissionMs, signalMs);
                        std::remove(slot->currentFilePath.c_str());
                    } else {
#ifndef CB_NO_RNNOISE
                        flog::info("[ChannelBank] Keeping recording (on-air {0}ms / span {1}ms, carrier {2}/{3} = {4:.0f}%%, RNNoise voice {5}/{6} = {7:.0f}%%, drift {8:.0f}Hz) slot {9}",
                                   onAirMs, signalMs, gVoice, gAbove, voiceFrac * 100.0f,
                                   slot->rnVadVoiceFrames, slot->rnVadFrames, rnVoiceFrac * 100.0f,
                                   driftStd, slot->gridIdx);
#else
                        flog::info("[ChannelBank] Keeping recording (on-air {0}ms / span {1}ms, carrier {2}/{3} = {4:.0f}%%, drift {5:.0f}Hz) slot {6}",
                                   onAirMs, signalMs, gVoice, gAbove, voiceFrac * 100.0f, driftStd, slot->gridIdx);
#endif
#if defined(__APPLE__) || defined(_WIN32)
                        bool transcriptionStarted = false;
                        if (_this->transcriptionOn()) {
                            if (slot->transcribeHandle) {
                                _this->txCancel(slot->transcribeBackend, slot->transcribeHandle);
                                _this->txDestroy(slot->transcribeBackend, slot->transcribeHandle);
                                slot->transcribeHandle = nullptr;
                            }
                            slot->pendingTranscriptPath = slot->currentFilePath;
                            slot->liveTranscript.clear();
                            slot->liveSegments.clear();
                            transcriptionStarted = _this->startTranscriptionJob(
                                slot->currentFilePath,
                                _this->displayName(slot->freqHz));
                        }
                        // Register for M4A encoding after playback+transcription,
                        // bounded by kM4ATranscriptionMaxWaitSec from file close.
                        // transcriptionDone=true when transcription is off or failed to start,
                        // so encoding fires immediately after playback in those cases.
                        if (_this->m4aEnabled && _this->recordingEnabled && !slot->currentFilePath.empty()) {
                            float avgSnrDb = (slot->snrCount > 0)
                                ? slot->snrSum / (float)slot->snrCount : 0.0f;
                            std::lock_guard<std::mutex> elk(_this->pendingEncodesMtx);
                            EncodeState& es = _this->pendingEncodes[slot->currentFilePath];
                            es.playbackDone      = false;
                            es.transcriptionDone = (!_this->transcriptionOn() || !transcriptionStarted);
                            es.finalM4APath      = slot->currentFinalM4APath;
                            es.avgSnrDb          = avgSnrDb;
                            es.queuedAt          = std::chrono::steady_clock::now();
                        }
#endif
                        // Log the frequency and queue for playback
                        // Log under the GRID freq — one history row per channel, no
                        // per-recording centroid-jitter duplicates.
                        _this->logRecording(slot->gridFreqHz);
                        _this->saveFreqLog();
                        if (!slot->currentFilePath.empty()) {
                            size_t qSize = 0;
                            {
                                std::lock_guard<std::mutex> lk(_this->playbackMtx);
                                // deleteAfter=true when recording is disabled — play it back but don't keep the file
                                _this->playbackQueue.push_back({slot->currentFilePath, slot->freqHz, slot->currentFinalM4APath, !_this->recordingEnabled});
                                qSize = _this->playbackQueue.size();
                                _this->playbackCv.notify_one();
                            }
                            // Auto-flush: if the queue is running away (live preview
                            // gets hours behind otherwise), drain the oldest entries
                            // straight to M4A so the latest few stay listenable.
                            if (_this->playbackAutoFlushEnabled &&
                                (int)qSize > _this->playbackAutoFlushThreshold) {
                                _this->flushPlaybackQueue(_this->playbackAutoFlushKeepLatest);
                            }
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

        // Fade-out: raised-cosine from 1→0 driven by raw RF signal absence. The
        // AM-specific detector debounce and audio-hold bypass below remove carrier
        // AGC release noise without changing the other demodulators' tail behavior.
        // This runs entirely on the DSP thread (no mutex needed).
        float tailFade = 1.0f;
        // Audio fade is driven by rawSignalPresent plus a short independent hold.
        // AM bypasses that hold because its keyed carrier remains present through
        // speech pauses; after unkey, holding full gain records the carrier AGC's
        // recovery as a rising burst of static. Other modes retain the established
        // hold unchanged so this fix remains strictly AM-specific.
        //
        // The file stays open for the full signalHoldMs+tailMs period (driven by
        // signalPresent + the silenceElapsed counter above), but any audio written
        // after the fade completes is digital silence (tailFade = 0.0f).
        const int AUDIO_HOLD_SAMPLES = slot->amDemod ? 0 : 16800; // 350 ms @ 48 kHz outside AM
        const int POST_FADE_REOPEN_HITS = 2;  // same 100 ms qualifier used for file-open
        bool rawAlive = slot->rawSignalPresent.load();
        bool fullyFaded = (slot->fadeOutRemaining <= 0 && slot->audioHoldRemaining <= 0);
        // Once audio is already at digital silence, still require a qualified raw
        // detection before unmuting again. Keep this at the same 2-frame threshold
        // as file-open; stricter post-fade gating clipped resumed voice after
        // normal HF speech gaps.
        bool rawAudioQualified = rawAlive && (!fullyFaded || slot->rawConsecutiveHits.load() >= POST_FADE_REOPEN_HITS);
        if (rawAudioQualified) {
            slot->audioHoldRemaining = AUDIO_HOLD_SAMPLES;
        } else if (slot->audioHoldRemaining > 0) {
            slot->audioHoldRemaining = std::max(0, slot->audioHoldRemaining - count);
        }
        // Keep the audio fade stricter than the recording/file lifetime.  The
        // smoothed SNR/vote path can keep the WAV open through weak HF pauses,
        // but once raw detection falls away we should let the written audio fade
        // down instead of holding full-volume static until close.
        bool signalAlive = rawAudioQualified || (slot->audioHoldRemaining > 0);
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
#ifndef CB_NO_RNNOISE
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
                    float vadProb = rnnoise_process_frame(slot->nrState, outBuf, slot->nrInBuf);
                    slot->rnVadFrames++;
                    slot->rnVadSum += vadProb;
                    if (vadProb >= _this->rnVoiceGateFrameThreshold)
                        slot->rnVadVoiceFrames++;
                    float mix = _this->nrMix;
                    float nrMono[480];
                    for (int i = 0; i < 480; i++) {
                        float dry = slot->nrInBuf[i] / 32768.0f;
                        float wet = outBuf[i] / 32768.0f;
                        nrMono[i] = _this->noiseReduction
                            ? std::clamp(dry * (1.0f - mix) + wet * mix, -1.0f, 1.0f)
                            : std::clamp(dry, -1.0f, 1.0f);
                    }
                    slot->writer.write(nrMono, 480);
                    slot->audioSamplesWritten += 480;
                    _this->publishLiveAudio(*slot, nrMono, 480);
                    slot->nrInPos = 0;
                }
            }
        } else
#endif
        {
            slot->writer.write(mono, count);
            slot->audioSamplesWritten += count;
            _this->publishLiveAudio(*slot, mono, count);
        }
    }

    // ── Playback monitor ─────────────────────────────────────────────────────

    void triggerEncode(const std::string& wavPath, const std::string& finalM4APath = {}, const std::string& transcript = {}, float avgSnrDb = 0.0f, int attempt = 0) {
        std::lock_guard<std::mutex> lk(encodeQueueMtx);
        encodeQueue.push_back({wavPath, finalM4APath, transcript, avgSnrDb, attempt});
        encodeQueueCv.notify_one();
    }

#if defined(__APPLE__) || defined(_WIN32)
    static bool m4aLooksComplete(const std::filesystem::path& path) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) return false;
        if (!std::filesystem::is_regular_file(path, ec) || ec) return false;
        auto size = std::filesystem::file_size(path, ec);
        return !ec && size > 512;
    }

    static bool retryDelay(int attempt) {
        if (attempt <= 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(150 * attempt));
        return true;
    }

    static bool copyFileWithRetries(const std::filesystem::path& from,
                                    const std::filesystem::path& to,
                                    const char* desc,
                                    std::error_code& lastEc) {
        for (int attempt = 1; attempt <= kM4AFsRetryAttempts; attempt++) {
            lastEc.clear();
            std::filesystem::copy_file(from, to,
                                       std::filesystem::copy_options::overwrite_existing,
                                       lastEc);
            if (!lastEc) return true;
            retryDelay(attempt);
        }
        flog::error("[ChannelBank] M4A staging failed: {0} ({1}): {2}",
                    desc, lastEc.message(), to.string());
        return false;
    }

    static bool removeFileWithRetries(const std::filesystem::path& path,
                                      const char* desc,
                                      std::error_code& lastEc,
                                      bool missingIsOk = true) {
        for (int attempt = 1; attempt <= kM4AFsRetryAttempts; attempt++) {
            lastEc.clear();
            if (missingIsOk && !std::filesystem::exists(path, lastEc)) {
                if (!lastEc) return true;
                lastEc.clear();
            }
            std::filesystem::remove(path, lastEc);
            if (!lastEc) return true;
            retryDelay(attempt);
        }
        flog::warn("[ChannelBank] M4A staging warning: {0} ({1}): {2}",
                   desc, lastEc.message(), path.string());
        return false;
    }

    static bool renameFileWithRetries(const std::filesystem::path& from,
                                      const std::filesystem::path& to,
                                      const char* desc,
                                      std::error_code& lastEc) {
        for (int attempt = 1; attempt <= kM4AFsRetryAttempts; attempt++) {
            lastEc.clear();
            std::filesystem::rename(from, to, lastEc);
            if (!lastEc) return true;
            retryDelay(attempt);
        }
        flog::error("[ChannelBank] M4A staging failed: {0} ({1}): {2}",
                    desc, lastEc.message(), to.string());
        return false;
    }

    std::filesystem::path makeEncodeStagingDir() {
        static std::atomic<uint64_t> seq { 0 };
        uint64_t id = seq.fetch_add(1);
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();

        std::error_code ec;
        std::filesystem::path base = std::filesystem::path(root) / "channel_bank" / "tmp" / "encode";
        std::filesystem::create_directories(base, ec);
        if (ec) {
            ec.clear();
            base = std::filesystem::temp_directory_path(ec) / "sdrpp-channel-bank" / "encode";
            if (ec) return {};
            std::filesystem::create_directories(base, ec);
            if (ec) return {};
        }

        std::filesystem::path dir = base / ("job-" + std::to_string(now) + "-" + std::to_string(id));
        std::filesystem::create_directories(dir, ec);
        if (ec) return {};
        return dir;
    }

    std::string encodeWavToM4AStaged(const std::string& wavPath, const std::string& finalM4APath, const std::string& transcript, float avgSnrDb) {
        std::filesystem::path originalWav(wavPath);
        std::filesystem::path finalM4A = finalM4APath.empty() ? originalWav : std::filesystem::path(finalM4APath);
        if (finalM4APath.empty()) finalM4A.replace_extension(".m4a");

        std::filesystem::path stagingDir = makeEncodeStagingDir();
        if (stagingDir.empty()) {
            flog::error("[ChannelBank] M4A staging failed: could not create local temp directory");
            return {};
        }

        auto cleanupStaging = [&]() {
            std::error_code ec;
            std::filesystem::remove_all(stagingDir, ec);
        };

        std::filesystem::path stagedWav = stagingDir / originalWav.filename();
        std::filesystem::path finalPart(finalM4A.string() + ".part-" +
                                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        std::error_code ec;
        if (!copyFileWithRetries(originalWav, stagedWav, "could not copy WAV locally", ec)) {
            cleanupStaging();
            return {};
        }

        std::string stagedM4A = encoding::wavToM4A(stagedWav.string(), transcript, avgSnrDb);
        if (stagedM4A.empty() || !m4aLooksComplete(stagedM4A)) {
            flog::error("[ChannelBank] M4A staging failed: local encode did not produce a complete file: {0}",
                        wavPath);
            cleanupStaging();
            return {};
        }

        std::filesystem::create_directories(finalM4A.parent_path(), ec);
        ec.clear();
        removeFileWithRetries(finalPart, "could not remove stale .part file", ec);
        ec.clear();
        if (!copyFileWithRetries(stagedM4A, finalPart, "could not copy final M4A to destination", ec)) {
            removeFileWithRetries(finalPart, "could not remove failed .part file", ec);
            cleanupStaging();
            return {};
        }

        ec.clear();
        auto stagedTime = std::filesystem::last_write_time(stagedM4A, ec);
        if (!ec) {
            std::error_code timeEc;
            std::filesystem::last_write_time(finalPart, stagedTime, timeEc);
        }

        ec.clear();
        removeFileWithRetries(finalM4A, "could not remove previous M4A", ec);
        ec.clear();
        if (!renameFileWithRetries(finalPart, finalM4A, "could not publish final M4A", ec)) {
            removeFileWithRetries(finalPart, "could not remove unpublished .part file", ec);
            cleanupStaging();
            return {};
        }

        if (!m4aLooksComplete(finalM4A)) {
            flog::error("[ChannelBank] M4A staging failed: published M4A failed size check: {0}",
                        finalM4A.string());
            cleanupStaging();
            return {};
        }

        ec.clear();
        removeFileWithRetries(originalWav, "could not remove original WAV after encode", ec, false);
        if (ec) {
            flog::warn("[ChannelBank] M4A encoded but original WAV could not be removed ({0}): {1}",
                       ec.message(), wavPath);
        }

        cleanupStaging();
        return finalM4A.string();
    }
#endif

    // Drain the oldest entries from the playback queue WITHOUT playing them.
    // Each drained entry has its M4A pipeline kicked off (or its WAV deleted if
    // recordingEnabled was off at record-time).  Used by the manual "Flush" UI
    // button and the auto-flush threshold.
    //
    // keepLatest: how many entries to LEAVE in the queue (most recent kept).
    //   keepLatest=0  ⇒ flush everything
    //   keepLatest=N  ⇒ keep the last N for live preview, flush the rest
    void flushPlaybackQueue(int keepLatest = 0) {
        std::vector<PlaybackEntry> toFlush;
        {
            std::lock_guard<std::mutex> lk(playbackMtx);
            int drain = (int)playbackQueue.size() - keepLatest;
            for (int i = 0; i < drain && !playbackQueue.empty(); i++) {
                toFlush.push_back(std::move(playbackQueue.front()));
                playbackQueue.pop_front();
            }
        }
        for (auto& entry : toFlush) {
            if (entry.deleteAfter) {
                // recordingEnabled was false at record-time — discard the WAV
                // (matches the deleteAfter contract: the user never wanted to keep it).
                std::remove(entry.path.c_str());
#if defined(__APPLE__) || defined(_WIN32)
                std::lock_guard<std::mutex> elk(pendingEncodesMtx);
                pendingEncodes.erase(entry.path);
#endif
            } else {
                normalizeRecordingIfEnabled(entry.path);
#if defined(__APPLE__) || defined(_WIN32)
                // Mark playbackDone in pendingEncodes; if transcription is also
                // done, fire the M4A encode now.  Otherwise leave the entry
                // alone — pollTranscriptions will fire the encode once Whisper
                // finishes, with the transcript baked in.
                if (m4aEnabled && recordingEnabled) {
                    bool        canEncode = false;
                    std::string encodeFinalM4APath = entry.finalM4APath;
                    std::string encodeTranscript;
                    float       encodeSnrDb = 0.0f;
                    {
                        std::lock_guard<std::mutex> elk(pendingEncodesMtx);
                        auto it = pendingEncodes.find(entry.path);
                        if (it != pendingEncodes.end()) {
                            it->second.playbackDone = true;
                            it->second.playbackDoneAt = std::chrono::steady_clock::now();
                            if (it->second.transcriptionDone) {
                                canEncode        = true;
                                encodeFinalM4APath = it->second.finalM4APath;
                                encodeTranscript = it->second.transcript;
                                encodeSnrDb      = it->second.avgSnrDb;
                                pendingEncodes.erase(it);
                            }
                        } else {
                            canEncode = true;
                        }
                    }
                    if (canEncode) triggerEncode(entry.path, encodeFinalM4APath, encodeTranscript, encodeSnrDb);
                }
                // If m4a is disabled the WAV stays as-is on disk — same outcome
                // as if playback had run to completion with m4aEnabled=false.
#endif
            }
        }
        if (!toFlush.empty()) {
            flog::info("[ChannelBank] Flushed {0} queued recording(s) to M4A", (int)toFlush.size());
        }
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
#if defined(__APPLE__) || defined(_WIN32)
                auto result = encodeWavToM4AStaged(task.wavPath, task.finalM4APath, task.transcript, task.avgSnrDb);
                if (result.empty()) {
                    std::error_code ec;
                    bool canRetry = task.attempt + 1 < kM4AEncodeMaxAttempts
                                    && std::filesystem::exists(task.wavPath, ec) && !ec;
                    if (canRetry) {
                        int nextAttempt = task.attempt + 1;
                        flog::warn("[ChannelBank] M4A encoding failed; retrying attempt {0}/{1}: {2}",
                                   nextAttempt + 1, kM4AEncodeMaxAttempts, task.wavPath);
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        triggerEncode(task.wavPath, task.finalM4APath, task.transcript, task.avgSnrDb, nextAttempt);
                    } else {
                        flog::error("[ChannelBank] M4A encoding failed permanently: {0}", task.wavPath);
                    }
                } else {
                    flog::info("[ChannelBank] Encoded: {0}", result);
                }
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
            std::string finalM4APath;
            bool        deleteAfter = false;
            {
                std::lock_guard<std::mutex> lk(playbackMtx);
                if (!playbackQueue.empty()) {
                    path        = playbackQueue.front().path;
                    playFreq    = playbackQueue.front().freqHz;
                    finalM4APath = playbackQueue.front().finalM4APath;
                    deleteAfter = playbackQueue.front().deleteAfter;
                    playbackQueue.pop_front();
#if defined(__APPLE__) || defined(_WIN32)
                    {
                        std::lock_guard<std::mutex> cpk(currentPlaybackPathMtx);
                        currentPlaybackPath = path;
                    }
#endif
                }
            }

            if (!path.empty()) {
#if defined(__APPLE__) || defined(_WIN32)
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
                normalizeRecordingIfEnabled(path);
                currentlyPlayingFreqKey.store(freqKey(playFreq));
                playbackWavFile(path);
                currentlyPlayingFreqKey.store(0);
#if defined(__APPLE__) || defined(_WIN32)
                playbackPosMs.store(-1);
                // Leave playingSegments intact for a moment so the user can read
                // the final transcript — it's cleared on the next playback start.
#endif
                if (deleteAfter) {
                    std::remove(path.c_str());
                }
#if defined(__APPLE__) || defined(_WIN32)
                else if (m4aEnabled) {
                    // Playback done — check if transcription is also complete
                    bool        canEncode = false;
                    std::string encodeFinalM4APath = finalM4APath;
                    std::string encodeTranscript;
                    float       encodeSnrDb = 0.0f;
                    {
                        std::lock_guard<std::mutex> lk(pendingEncodesMtx);
                        auto it = pendingEncodes.find(path);
                        if (it != pendingEncodes.end()) {
                            it->second.playbackDone = true;
                            it->second.playbackDoneAt = std::chrono::steady_clock::now();
                            if (it->second.transcriptionDone) {
                                canEncode        = true;
                                encodeFinalM4APath = it->second.finalM4APath;
                                encodeTranscript = it->second.transcript;
                                encodeSnrDb      = it->second.avgSnrDb;
                                pendingEncodes.erase(it);
                            }
                        } else {
                            canEncode = true;
                        }
                    }
                    if (canEncode) triggerEncode(path, encodeFinalM4APath, encodeTranscript, encodeSnrDb);
                }
#endif
#if defined(__APPLE__) || defined(_WIN32)
                {
                    std::lock_guard<std::mutex> cpk(currentPlaybackPathMtx);
                    if (currentPlaybackPath == path) currentPlaybackPath.clear();
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
#if defined(__APPLE__) || defined(_WIN32)
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
        _this->processWebUiActions();
        float menuWidth = ImGui::GetContentRegionAvail().x;

        // Reset each frame; set below whenever a channel/history row is hovered.
        // Read by fftRedrawHandlerFunc to draw the cyan crosshair.
        _this->hoveredFreqHz = 0.0;

        // ── Config Profiles ──────────────────────────────────────────────
        if (_this->running) { style::beginDisabled(); }
        ImGui::LeftLabel("Profile");
        float comboW = menuWidth - ImGui::GetCursorPosX() - 75;
        ImGui::SetNextItemWidth(comboW > 80 ? comboW : 80);
        if (ImGui::BeginCombo(CONCAT("##_cb_profile_", _this->name),
                              _this->activeProfileName.c_str())) {
            for (int i = 0; i < (int)_this->profileNames.size(); i++) {
                bool sel = (i == _this->profileComboIdx);
                if (ImGui::Selectable(_this->profileNames[i].c_str(), sel)) {
                    if (_this->profileNames[i] != _this->activeProfileName) {
                        _this->saveCurrentProfile();
                        _this->loadProfile(_this->profileNames[i]);
                    }
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("Save##_cb_profsave_", _this->name))) {
            _this->saveCurrentProfile();
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("+##_cb_profnew_", _this->name))) {
            memset(_this->newProfileNameBuf, 0, sizeof(_this->newProfileNameBuf));
            _this->showNewProfilePopup = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(CONCAT("x##_cb_profdel_", _this->name))) {
            if (_this->profileNames.size() > 1) {
                _this->showDeleteConfirm = true;
            }
        }
        if (_this->running) { style::endDisabled(); }

        if (_this->showNewProfilePopup) {
            ImGui::OpenPopup(CONCAT("New Profile##_cb_newprof_", _this->name));
            _this->showNewProfilePopup = false;
        }
        if (ImGui::BeginPopup(CONCAT("New Profile##_cb_newprof_", _this->name))) {
            ImGui::Text("Profile name:");
            ImGui::InputText(CONCAT("##_cb_profname_", _this->name),
                             _this->newProfileNameBuf, sizeof(_this->newProfileNameBuf));
            if (ImGui::Button("Create") && _this->newProfileNameBuf[0]) {
                std::string newName(_this->newProfileNameBuf);
                _this->saveCurrentProfile();
                _this->activeProfileName = newName;
                _this->saveCurrentProfile();
                _this->refreshProfileNames();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (_this->showDeleteConfirm) {
            ImGui::OpenPopup(CONCAT("Delete Profile?##_cb_delprof_", _this->name));
            _this->showDeleteConfirm = false;
        }
        if (ImGui::BeginPopup(CONCAT("Delete Profile?##_cb_delprof_", _this->name))) {
            ImGui::Text("Delete \"%s\"?", _this->activeProfileName.c_str());
            if (ImGui::Button("Delete")) {
                config.acquire();
                config.conf[_this->name]["profiles"].erase(_this->activeProfileName);
                config.release(true);
                _this->refreshProfileNames();
                _this->loadProfile(_this->profileNames[0]);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::Separator();

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

#ifndef CB_NO_RNNOISE
        if (ImGui::Checkbox(CONCAT("RNNoise Voice Gate##_cb_rn_vad_", _this->name),
                            &_this->rnVoiceGateEnabled)) {
            config.acquire();
            config.conf[_this->name]["rnVoiceGateEnabled"] = _this->rnVoiceGateEnabled;
            config.release(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Test feature: use RNNoise voice probability to\n"
                              "discard finished recordings that look non-voice.\n"
                              "Independent of Noise Reduction; saved audio can\n"
                              "remain completely dry.");
        if (_this->rnVoiceGateEnabled) {
            ImGui::LeftLabel("  RN Voice %");
            ImGui::FillWidth();
            float pct = _this->rnVoiceGateVoiceFrac * 100.0f;
            if (ImGui::SliderFloat(CONCAT("##_cb_rn_vad_frac_", _this->name),
                                   &pct, 5.0f, 75.0f, "%.0f %%")) {
                _this->rnVoiceGateVoiceFrac = std::clamp(pct / 100.0f, 0.05f, 0.75f);
                config.acquire();
                config.conf[_this->name]["rnVoiceGateVoiceFrac"] = _this->rnVoiceGateVoiceFrac;
                config.release(true);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Minimum fraction of RNNoise frames that must\n"
                                  "look speech-like for the recording to be kept.\n"
                                  "Lower is safer while testing weak radio audio.");
            ImGui::LeftLabel("  Probe Time");
            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_cb_rn_vad_probe_", _this->name),
                                   &_this->rnVoiceGateProbeSec, 3.0f, 30.0f, "%.0f s")) {
                _this->rnVoiceGateProbeSec = std::clamp(_this->rnVoiceGateProbeSec, 3.0f, 30.0f);
                config.acquire();
                config.conf[_this->name]["rnVoiceGateProbeSec"] = _this->rnVoiceGateProbeSec;
                config.release(true);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("After this many seconds of a still-open recording,\n"
                                  "RNNoise can force a close if the accumulated audio\n"
                                  "still looks non-voice. The normal close-time gate\n"
                                  "then decides whether to delete the file.");
            ImGui::LeftLabel("  Quarantine");
            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_cb_rn_vad_quarantine_", _this->name),
                                   &_this->rnVoiceGateQuarantineSec, 0.0f, 300.0f, "%.0f s")) {
                _this->rnVoiceGateQuarantineSec = std::clamp(_this->rnVoiceGateQuarantineSec, 0.0f, 300.0f);
                config.acquire();
                config.conf[_this->name]["rnVoiceGateQuarantineSec"] = _this->rnVoiceGateQuarantineSec;
                config.release(true);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("After RNNoise rejects a non-voice recording,\n"
                                  "temporarily suppress that frequency before\n"
                                  "probing it again. 0 disables quarantine.");
        }
#endif

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

        bool canLimitPassbands = _this->manualMode;
        if (!canLimitPassbands) style::beginDisabled();
        if (ImGui::Checkbox(CONCAT("Limit to manual passbands##_cb_manpb_", _this->name),
                            &_this->manualPassbandLimit)) {
            config.acquire();
            config.conf[_this->name]["manualPassbandLimit"] = _this->manualPassbandLimit;
            config.release(true);
            _this->resetDetectorFloor();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Manual mode only.\n"
                              "USB watches bookmark to +channel spacing.\n"
                              "LSB watches -channel spacing to bookmark.\n"
                              "AM/NFM/WFM watch a centered channel-width passband.\n"
                              "When off, manual detection uses the current full-span floor.");
        }
        if (!canLimitPassbands) style::endDisabled();

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
            _this->resetDetectorFloor();
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
            _this->resetDetectorFloor();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("dB below SNR Threshold that keeps an open\n"
                              "recording alive. Signal can fade this far below\n"
                              "the threshold without losing votes or triggering\n"
                              "the dropout timer.\n"
                              "4 dB covers typical HF QSB fading.\n"
                              "0 = disabled (symmetric open/hold threshold).");

        if (ImGui::SmallButton(CONCAT("Reset detector##_cb_reset_detector_", _this->name))) {
            _this->resetDetectorFloor();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clear detector averaging, votes, raw hits, and\n"
                              "temporary RNNoise quarantine without restarting.");

        // NMS radius — how far around a detected center to suppress neighbors
        ImGui::LeftLabel("NMS Radius");
        ImGui::FillWidth();
        if (ImGui::SliderInt(CONCAT("##_cb_nmsr_", _this->name),
                             &_this->nmsRadiusSlots, 1, 4, "±%d slots")) {
            config.acquire();
            config.conf[_this->name]["nmsRadiusSlots"] = _this->nmsRadiusSlots;
            config.release(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When a signal is detected at slot N, also block\n"
                              "spawn on the neighboring ±N slots.  Prevents the\n"
                              "sidebands of a wide AM/FM transmission from being\n"
                              "treated as separate signals.\n"
                              "  1 = old behavior (only direct neighbors)\n"
                              "  2 = default — covers AM voice on 8.33/12.5/25 kHz grids\n"
                              "  3-4 = wider signals or fine grid spacing");

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

        if (ImGui::Checkbox(CONCAT("Stuck Noise Guard##_cb_stucknoise_", _this->name),
                            &_this->stuckNoiseGuardEnabled)) {
            config.acquire();
            config.conf[_this->name]["stuckNoiseGuardEnabled"] = _this->stuckNoiseGuardEnabled;
            config.release(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Manual passband mode only. Currently debug-only;\n"
                              "shows whether long recordings stay continuously\n"
                              "active while the passband centroid wanders like\n"
                              "drifting local interference. Does not discard.");
        if (_this->stuckNoiseGuardEnabled) {
            ImGui::LeftLabel("  Guard After");
            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_cb_stucksec_", _this->name),
                                   &_this->stuckNoiseGuardMinSec, 20.0f, 180.0f, "%.0f s")) {
                config.acquire();
                config.conf[_this->name]["stuckNoiseGuardMinSec"] = _this->stuckNoiseGuardMinSec;
                config.release(true);
            }
            ImGui::LeftLabel("  Active Min");
            ImGui::FillWidth();
            float activePct = _this->stuckNoiseGuardActiveFrac * 100.0f;
            if (ImGui::SliderFloat(CONCAT("##_cb_stuckactive_", _this->name),
                                   &activePct, 70.0f, 98.0f, "%.0f %%")) {
                _this->stuckNoiseGuardActiveFrac = activePct / 100.0f;
                config.acquire();
                config.conf[_this->name]["stuckNoiseGuardActiveFrac"] = _this->stuckNoiseGuardActiveFrac;
                config.release(true);
            }
            ImGui::LeftLabel("  Drift Min");
            ImGui::FillWidth();
            if (ImGui::SliderFloat(CONCAT("##_cb_stuckdrift_", _this->name),
                                   &_this->stuckNoiseGuardDriftHz, 300.0f, 3000.0f, "%.0f Hz")) {
                config.acquire();
                config.conf[_this->name]["stuckNoiseGuardDriftHz"] = _this->stuckNoiseGuardDriftHz;
                config.release(true);
            }
            std::string dbg = _this->getStuckNoiseDebug();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.55f, 1.0f));
            ImGui::TextWrapped("Noise guard: %s%s",
                               (_this->manualMode && _this->manualPassbandLimit) ? "" : "inactive; ",
                               dbg.empty() ? "waiting for close" : dbg.c_str());
            ImGui::PopStyleColor();
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

        // Auto-flush playback queue when it gets too long
        if (ImGui::Checkbox(CONCAT("Auto-flush queue##_cb_autoflush_", _this->name),
                            &_this->playbackAutoFlushEnabled)) {
            config.acquire();
            config.conf[_this->name]["playbackAutoFlushEnabled"] = _this->playbackAutoFlushEnabled;
            config.release(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When the playback queue exceeds the threshold below,\n"
                              "drain the OLDEST entries straight to M4A (no playback)\n"
                              "so the live preview stays current.  Skipped recordings\n"
                              "are still encoded with full transcripts + backdated\n"
                              "timestamps — you just don't listen to them in real time.");
        if (_this->playbackAutoFlushEnabled) {
            ImGui::LeftLabel("  Flush at");
            ImGui::FillWidth();
            if (ImGui::SliderInt(CONCAT("##_cb_aflush_th_", _this->name),
                                 &_this->playbackAutoFlushThreshold, 5, 200, "%d in queue")) {
                config.acquire();
                config.conf[_this->name]["playbackAutoFlushThreshold"] = _this->playbackAutoFlushThreshold;
                config.release(true);
            }
            ImGui::LeftLabel("  Keep latest");
            ImGui::FillWidth();
            if (ImGui::SliderInt(CONCAT("##_cb_aflush_keep_", _this->name),
                                 &_this->playbackAutoFlushKeepLatest, 0, 30, "%d to play")) {
                config.acquire();
                config.conf[_this->name]["playbackAutoFlushKeepLatest"] = _this->playbackAutoFlushKeepLatest;
                config.release(true);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("After a flush, how many of the NEWEST recordings\n"
                                  "to leave in the queue for live preview.\n"
                                  "0 = drain completely; 5 = keep the last 5 to listen to.");
        }

#if defined(__APPLE__) || defined(_WIN32)
        // ── Transcription backend dropdown ──────────────────────────────────
        // Selecting a Whisper model that isn't installed shows an inline status
        // message and a Download button (download UI lands in a follow-up step;
        // for now users can place a .bin in the models dir manually).
        {
            ImGui::LeftLabel("Transcribe");
            ImGui::FillWidth();
#ifdef __APPLE__
            const char* labels[] = {
                "Off",
                "Apple Speech",
                "Whisper ATC Large (best)",
                "Whisper ATC Medium",
                "Whisper Turbo (fast)",
            };
            int labelCount = 5;
#else
            const char* labels[] = {
                "Off",
                "Whisper ATC Large (best)",
                "Whisper ATC Medium",
                "Whisper Turbo (fast)",
            };
            int labelCount = 4;
#endif
            int  cur = _this->transcriptionBackend;
            if (cur < 0 || cur > TB_WHISPER_TURBO) cur = TB_OFF;
#ifndef __APPLE__
            int comboVal = (cur >= TB_WHISPER_ATC_LARGE) ? (cur - TB_WHISPER_ATC_LARGE + 1) : 0;
#else
            int comboVal = cur;
#endif
            if (ImGui::Combo(CONCAT("##_cb_txbe_", _this->name), &comboVal, labels, labelCount)) {
#ifndef __APPLE__
                cur = (comboVal >= 1) ? (comboVal - 1 + TB_WHISPER_ATC_LARGE) : TB_OFF;
#else
                cur = comboVal;
#endif
                _this->transcriptionBackend = cur;
                config.acquire();
                config.conf[_this->name]["transcriptionBackend"] = cur;
                config.release(true);
                if (cur == TB_OFF) {
                    std::lock_guard<std::mutex> tlk(_this->lastTranscriptMtx);
                    _this->lastTranscriptText.clear();
                    _this->lastTranscriptName.clear();
                    _this->playingSegments.clear();
                }
#ifdef __APPLE__
                if (cur == TB_APPLE_SPEECH &&
                    transcription::authStatus() == transcription::AuthStatus::NotDetermined) {
                    transcription::requestPermission();
                }
#endif
            }

            // Surface backend-specific status / actions on the next line.
#ifdef __APPLE__
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
            } else
#endif
            if (_this->transcriptionBackend >= TB_WHISPER_ATC_LARGE) {
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

        // M4A encoding toggle (AudioToolbox on macOS, ffmpeg on Windows)
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
            if (_this->transcriptionOn()) {
                ImGui::TextWrapped("WAV converted after playback or %ds transcript wait. WAV deleted on success.",
                    kM4ATranscriptionMaxWaitSec);
            } else {
                ImGui::TextWrapped("WAV converted after first playback. WAV deleted on success.");
            }
            ImGui::PopStyleColor();
        }
#endif

        if (ImGui::Checkbox(CONCAT("Normalize recordings##_cb_normrec_", _this->name),
                            &_this->normalizeRecordings)) {
            config.acquire();
            config.conf[_this->name]["normalizeRecordings"] = _this->normalizeRecordings;
            config.release(true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("When enabled, completed WAVs are rewritten to trim silence\n"
                              "and add a small silence pad before playback/M4A encoding.\n"
                              "Turn off to preserve the raw recorded file.");
        }

        if (ImGui::Checkbox(CONCAT("Portable recording group##_cb_portable_group_", _this->name),
                            &_this->portableRecordingGroup)) {
            config.acquire();
            config.conf[_this->name]["portableRecordingGroup"] = _this->portableRecordingGroup;
            config.release(true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("When enabled, new recording filenames use Portable-System\n"
                              "as the group prefix while keeping the bookmark or\n"
                              "frequency name intact for Web Audio Monitor filters.");
        }

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

        if (ImGui::Checkbox(CONCAT("Start on module load##_cb_autostart_", _this->name),
                            &_this->autoStart)) {
            config.acquire();
            config.conf[_this->name]["autoStart"] = _this->autoStart;
            config.release(true);
        }

        if (ImGui::Checkbox(CONCAT("Web Control##_cb_webctl_", _this->name),
                            &_this->webControlEnabled)) {
            if (_this->webControlEnabled) _this->startWebServer();
            else {
                _this->stopWebServer();
                _this->webControlError.clear();
            }
            config.acquire();
            config.conf[_this->name]["webControlEnabled"] = _this->webControlEnabled;
            config.release(true);
        }

        bool webControlListening = _this->webServerRunning.load();
        if (webControlListening) style::beginDisabled();
        ImGui::LeftLabel("Web Bind");
        ImGui::FillWidth();
        if (ImGui::InputText(CONCAT("##_cb_webbind_", _this->name),
                             _this->webControlBindBuf,
                             sizeof(_this->webControlBindBuf))) {
            _this->webControlBind = _this->webControlBindBuf;
            config.acquire();
            config.conf[_this->name]["webControlBind"] = _this->webControlBind;
            config.release(true);
        }
        ImGui::LeftLabel("Web Port");
        ImGui::FillWidth();
        if (ImGui::InputInt(CONCAT("##_cb_webport_", _this->name), &_this->webControlPort)) {
            _this->webControlPort = std::clamp(_this->webControlPort, 1024, 65535);
            config.acquire();
            config.conf[_this->name]["webControlPort"] = _this->webControlPort;
            config.release(true);
        }
        if (webControlListening) style::endDisabled();

        if (webControlListening) {
            ImGui::Text("http://%s:%d/", _this->webDisplayBindAddress().c_str(), _this->webControlPort);
        }
        else if (_this->webControlEnabled && !_this->webControlError.empty()) {
            ImGui::TextWrapped("Web control error: %s", _this->webControlError.c_str());
        }
        else {
            ImGui::TextDisabled("Web control stopped");
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
            // Inline "Flush" button — only useful when the queue actually has items
            if (queued > 0) {
                ImGui::SameLine();
                if (ImGui::SmallButton(CONCAT("Flush##_cb_flushq_", _this->name))) {
                    _this->flushPlaybackQueue(0);   // drain everything
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Skip playback for ALL queued recordings.\n"
                                      "Each is sent straight to M4A encoding instead.\n"
                                      "WAVs include transcripts when ready; otherwise\n"
                                      "encoding proceeds after the transcription wait\n"
                                      "limit so the recording folder keeps moving.");
            }

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
#if defined(__APPLE__) || defined(_WIN32)
                std::string txText, txName;
                std::vector<transcription_whisper::Segment> txSegs;
                int txPosMs = _this->playbackPosMs.load();
                if (_this->transcriptionOn()) {
                    std::lock_guard<std::mutex> tlk(_this->lastTranscriptMtx);
                    txText = _this->lastTranscriptText;
                    txName = _this->lastTranscriptName;
                    txSegs = _this->playingSegments;
                }
                // Fixed height — scrolls internally when transcript is long.
                // Previously this was dynamic (50–220 px depending on content),
                // which caused every module below channel_bank in the sidebar to
                // jump position whenever playback started/stopped or a transcript
                // arrived.
                float panelH = 130.0f;
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
#if defined(__APPLE__) || defined(_WIN32)
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

            // Active + Recent channels — fixed-height scrollable region so the
            // panel below (Frequency History, settings, etc.) stays put even as
            // channels spawn/expire.
            ImGui::Separator();
            bool needSaveFreqLog = false;
            ImGui::BeginChild(CONCAT("##_cb_ch_", _this->name),
                              ImVec2(menuWidth, 150), false);
            {
                // ── Active channels ──────────────────────────────────────────
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
                        bool blocked = _this->isBlocked(slot->gridFreqHz);
                        char blkId[48];
                        snprintf(blkId, sizeof(blkId), "Blk##_cb_ablk_%d", idx);
                        if (ImGui::Checkbox(blkId, &blocked)) {
                            std::lock_guard<std::mutex> lk(_this->freqLogMtx);
                            auto& entry = _this->freqLog[_this->freqKey(slot->gridFreqHz)];
                            if (entry.freqHz == 0.0) entry.freqHz = slot->gridFreqHz;
                            entry.blocked = blocked;
                            needSaveFreqLog = true;
                        }
                    }
                }

                // ── Recent channels (sticky 30s after teardown) ──────────────
                // These linger so the user can confirm a frequency was just noise
                // and hit Block before the row vanishes.  Alpha fades 1→0 over 30s.
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
            }
            ImGui::EndChild();

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

            // Pre-compute display names ONCE per entry instead of inside the sort
            // comparator — the old code called displayName() (which locks
            // bookmarkNamesMtx) on both entries for every comparison, yielding
            // O(n log n) mutex lock/unlock pairs for N history entries.
            struct HistEntry {
                int64_t    key;
                FreqEntry* ep;
                std::string name;    // cached displayName
                std::string nameLow; // lower-cased for sort
            };
            std::vector<HistEntry> allEntries;
            allEntries.reserve(_this->freqLog.size());
            for (auto& [k, e] : _this->freqLog) {
                // Blocked-only filter
                if (_this->freqHistBlockedOnly && !e.blocked) continue;
                std::string dn = _this->displayName(e.freqHz);
                // Text filter: match against display name or frequency string
                if (!filterStr.empty()) {
                    char freqBuf[32];
                    snprintf(freqBuf, sizeof(freqBuf), "%.4f", e.freqHz / 1e6);
                    std::string dnL = dn, fqL = freqBuf;
                    std::transform(dnL.begin(), dnL.end(), dnL.begin(), ::tolower);
                    std::transform(fqL.begin(), fqL.end(), fqL.begin(), ::tolower);
                    if (dnL.find(filterStr) == std::string::npos &&
                        fqL.find(filterStr) == std::string::npos) continue;
                }
                std::string dnLow = dn;
                std::transform(dnLow.begin(), dnLow.end(), dnLow.begin(), ::tolower);
                allEntries.push_back({k, &e, std::move(dn), std::move(dnLow)});
            }

            // Sort: named bookmarks (letter-leading) first, alphabetically;
            //        unnamed (digit-leading) last, by frequency ascending.
            // Comparator uses pre-computed names — no mutex acquisitions.
            std::sort(allEntries.begin(), allEntries.end(), [](const HistEntry& aa, const HistEntry& bb) {
                bool aIsNamed = !aa.name.empty() && std::isalpha((unsigned char)aa.name[0]);
                bool bIsNamed = !bb.name.empty() && std::isalpha((unsigned char)bb.name[0]);
                if (aIsNamed != bIsNamed) return aIsNamed > bIsNamed; // named first
                if (aa.nameLow != bb.nameLow) return aa.nameLow < bb.nameLow;
                return aa.ep->freqHz < bb.ep->freqHz;
            });

            // Render flat list — reuse pre-computed display names from HistEntry
            for (auto& he : allEntries) {
                FreqEntry& e = *he.ep;
                const std::string& dn = he.name;

                if (e.blocked)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));

                std::string rel = relTime(e.lastSeen);
                ImGui::Text("%-22s %5d  %-10s", dn.c_str(), e.count, rel.c_str());
                if (ImGui::IsItemHovered()) _this->hoveredFreqHz = e.freqHz;

                if (e.blocked)
                    ImGui::PopStyleColor();

                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
                char editBtn[32];
                snprintf(editBtn, sizeof(editBtn), "D##edt_%lld", (long long)he.key);
                if (ImGui::SmallButton(editBtn)) {
                    _this->descEditKey   = he.key;
                    strncpy(_this->descEditBuf, e.description.c_str(), sizeof(_this->descEditBuf) - 1);
                    _this->descEditBuf[sizeof(_this->descEditBuf) - 1] = '\0';
                    _this->descEditRequest = true;
                }
                ImGui::SameLine();
                char chk[32];
                snprintf(chk, sizeof(chk), "##blk_%lld", (long long)he.key);
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
        if (retuneFlag.load()) return false;
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
            if (isBlocked(it->second->gridFreqHz)) {
                flog::info("[ChannelBank] BkScan: removing blocked slot {0}", idx);
                destroySlot(*it->second);
                delete it->second;
                it = activeChannels.erase(it);
                continue;
            }
            if (isRnVoiceQuarantined(it->second->gridFreqHz)) {
                flog::info("[ChannelBank] BkScan: removing RNNoise-quarantined slot {0}", idx);
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
            if (isBlocked(stop.freqsHz[i]) || isRnVoiceQuarantined(stop.freqsHz[i])) continue;
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
        config.conf[name]["manualPassbandLimit"] = manualPassbandLimit;
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

    bool manualPassbandBinsForFreq(double freqHz, double binHz, int& lo, int& hi) const {
        if (lastKnownSr <= 0.0 || binHz <= 0.0) return false;
        double freqOffset = freqHz - lastKnownCenter;
        if (std::abs(freqOffset) >= lastKnownSr / 2.0) return false;

        int centerBin = (int)std::round((freqOffset / lastKnownSr) * FFT_SIZE) + FFT_SIZE / 2;
        int widthBins = std::max(1, (int)std::round(channelSpacing / binHz));

        if (demodMode == DEMOD_USB) {
            lo = centerBin;
            hi = centerBin + widthBins;
        } else if (demodMode == DEMOD_LSB) {
            lo = centerBin - widthBins;
            hi = centerBin;
        } else {
            int half = std::max(1, widthBins / 2);
            lo = centerBin - half;
            hi = centerBin + half;
        }

        lo = std::clamp(lo, 0, FFT_SIZE - 1);
        hi = std::clamp(hi, 0, FFT_SIZE - 1);
        return lo <= hi;
    }

    void resetDetectorFloor() {
        {
            std::lock_guard<std::mutex> lk(channelsMtx);
            for (auto it = activeChannels.begin(); it != activeChannels.end(); ) {
                ChannelSlot* slot = it->second;
                if (slot->fileOpen) {
                    ++it;
                    continue;
                }
                destroySlot(*slot);
                delete slot;
                it = activeChannels.erase(it);
            }
        }
        detectorFloorResetRequested.store(true);
        mgmtCv.notify_one();
    }

    bool shouldRejectStuckNoise(ChannelSlot& slot, std::string& reason) {
        int totalFrames  = slot.noiseGuardFramesTotal.load();
        int activeFrames = slot.noiseGuardFramesActive.load();
        float activeFrac = (totalFrames > 0) ? (float)activeFrames / (float)totalFrames : 0.0f;
        float openSec = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - slot.fileOpenTime).count();

        double mean = (activeFrames > 0) ? slot.noiseGuardCentroidSum / (double)activeFrames : 0.0;
        double var = (activeFrames > 0)
            ? std::max(0.0, slot.noiseGuardCentroidSumSq / (double)activeFrames - mean * mean)
            : 0.0;
        float driftHz = (float)std::sqrt(var);
        float widthHz = (activeFrames > 0)
            ? (float)(slot.noiseGuardWidthSum / (double)activeFrames)
            : 0.0f;
        constexpr float STUCK_NOISE_MAX_WIDTH_HZ = 650.0f;

        char buf[192];
        snprintf(buf, sizeof(buf), "sec=%.0f active=%.0f%% drift=%.0fHz width=%.0fHz frames=%d",
                 openSec, activeFrac * 100.0f, driftHz, widthHz, totalFrames);
        reason = buf;

        {
            std::lock_guard<std::mutex> lk(stuckNoiseDebugMtx);
            lastStuckNoiseDebug = reason;
        }

        if (!stuckNoiseGuardEnabled || !manualMode || !manualPassbandLimit) return false;
        // Observation-only for now. Field testing showed this guard can suppress
        // copyable HF traffic before we have enough shape data to trust it.
        // Keep the debug line above live, but never discard recordings here.
        bool allowStuckNoiseDiscard = false;
        if (!allowStuckNoiseDiscard) return false;
        if (openSec < stuckNoiseGuardMinSec) return false;
        if (totalFrames < (int)(stuckNoiseGuardMinSec * SPEC_ANALYSIS_HZ * 0.5f)) return false;
        if (activeFrac < stuckNoiseGuardActiveFrac) return false;
        if (driftHz < stuckNoiseGuardDriftHz) return false;
        // Real SSB voice can be long, continuously active, and centroid-drifty as
        // syllables move energy around the passband. Only discard when the energy
        // also stays narrow, which is the more useful signature of a wandering
        // local-noise line.
        if (widthHz > STUCK_NOISE_MAX_WIDTH_HZ) return false;
        return true;
    }

    std::string getStuckNoiseDebug() {
        std::lock_guard<std::mutex> lk(stuckNoiseDebugMtx);
        return lastStuckNoiseDebug;
    }

    void loadFMConfig() {
        fmLists.clear();
        std::map<int64_t, BookmarkEntry> newNames;
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
                            if (newNames.find(k) == newNames.end())
                                newNames[k] = {bmName, listName};
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
        if (retuneFlag.load()) return;

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
            if (isBlocked(it->second->gridFreqHz)) {
                flog::info("[ChannelBank] Manual: removing blocked slot {0}", idx);
                destroySlot(*it->second);
                delete it->second;
                it = activeChannels.erase(it);
                continue;
            }
            if (isRnVoiceQuarantined(it->second->gridFreqHz)) {
                flog::info("[ChannelBank] Manual: removing RNNoise-quarantined slot {0}", idx);
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
            if (isBlocked(localFreqs[i]) || isRnVoiceQuarantined(localFreqs[i])) continue;
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
    bool         autoStart     = false;
    std::mutex   runMtx;
    bool         webControlEnabled = false;
    int          webControlPort    = 18080;
    std::string  webControlBind    = "127.0.0.1";
    char         webControlBindBuf[64] = "127.0.0.1";
    std::atomic<uint64_t> webHeartbeat { 0 };
    std::atomic<bool> webServerRunning { false };
    std::thread  webServerThread;
    std::mutex   webServerMtx;
    std::string  webControlError;
#ifdef _WIN32
    bool         webSocketsStarted = false;
#endif
    WebSocket    webServerFd = INVALID_WEB_SOCKET;
    std::mutex   webClientThreadsMtx;
    std::vector<std::thread> webClientThreads;
    std::mutex webUiActionMtx;
    std::deque<std::shared_ptr<WebUiAction>> webUiActions;
    static constexpr size_t LIVE_AUDIO_MAX_SAMPLES = 48000;
    std::atomic<int> liveAudioClients { 0 };
    std::mutex liveAudioMtx;
    std::condition_variable liveAudioCv;
    std::deque<std::vector<int16_t>> liveAudioChunks;
    size_t liveAudioQueuedSamples = 0;
    std::atomic<int64_t> liveAudioSelectedFreqKey { 0 };
    std::atomic<uint64_t> liveAudioSelectedMs { 0 };
    std::atomic<uint64_t> liveAudioDroppedChunks { 0 };

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
    bool         stuckNoiseGuardEnabled = false;
    float        stuckNoiseGuardMinSec = 45.0f;
    float        stuckNoiseGuardActiveFrac = 0.85f;
    float        stuckNoiseGuardDriftHz = 900.0f;
    std::mutex   stuckNoiseDebugMtx;
    std::string  lastStuckNoiseDebug;
    float        leftTrimFrac   = 0.0f;  // fraction of bandwidth to exclude from left edge
    float        rightTrimFrac  = 0.0f;  // fraction of bandwidth to exclude from right edge
    bool         draggingLeft   = false;
    bool         draggingRight  = false;
    int          signalHoldMs          = 500;    // hold signalPresent true N ms after last detection (dropout hysteresis)
    bool         recordingEnabled      = true;   // global recording on/off toggle
    bool         portableRecordingGroup = false; // save new recordings under the Portable group name
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
    bool         m4aEnabled            = false;  // encode to M4A after playback plus bounded transcript wait
    bool         normalizeRecordings   = true;   // trim/pad completed WAVs before playback or M4A
    float        recGain       = 0.25f;     // linear gain applied before WAV write (~-12dB)
    int          minTransmissionMs = 300;   // discard recordings shorter than this
    int          tailMs            = 500;   // ms to keep recording after signal gone
    int          maxChannels   = 16;
    // Playback-queue auto-flush.  When playback can't keep up with recording
    // (common at busy times — many channels, many short transmissions, real-
    // time-only playback), the queue snowballs and the live preview falls
    // hours behind.  Auto-flush drains the OLDEST entries from the queue
    // straight to M4A (no playback) when the queue size exceeds the threshold,
    // keeping the latest few for live preview.
    bool         playbackAutoFlushEnabled    = true;
    int          playbackAutoFlushThreshold  = 30;  // queue size above which auto-flush kicks in
    int          playbackAutoFlushKeepLatest = 5;   // how many to keep playable after a flush
    // Non-max-suppression radius in slots — when detecting a signal at slot N,
    // also suppress neighbors up to ±nmsRadiusSlots from joining `detected`.
    // Default 2: covers AM voice (carrier + ±5–8 kHz sidebands) at 8.33–25 kHz
    // channel spacing.  Drop to 1 if your real channels are packed tighter than
    // their signal widths (rare).  Bump higher (3–4) only for very wide signals
    // like FM broadcast at narrow grid spacing.
    int          nmsRadiusSlots = 2;
    double       channelSpacing = 25000.0;
    float        bwUsage       = 0.8f;      // fraction of SDR bandwidth to use (avoids filter rolloff edges)
    bool         noiseReduction = false;    // RNNoise neural noise suppression on recordings
    float        nrMix          = 0.7f;    // 0=dry (original), 1=full NR
    bool         rnVoiceGateEnabled = false; // RNNoise VAD test gate, independent of noise reduction
    float        rnVoiceGateFrameThreshold = 0.50f;
    float        rnVoiceGateVoiceFrac = 0.20f;
    int          rnVoiceGateMinFrames = 30;
    float        rnVoiceGateProbeSec = 10.0f;
    float        rnVoiceGateQuarantineSec = 60.0f;

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
    bool manualPassbandLimit = false;
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
    struct BookmarkEntry { std::string name; std::string listName; };
    std::map<int64_t, BookmarkEntry> bookmarkNames;  // freqKey -> {name, listName}

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
    std::atomic<bool>                       detectorFloorResetRequested { false };
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
    std::map<int64_t, std::chrono::steady_clock::time_point> rnVoiceQuarantineUntil;
    std::mutex                   rnVoiceQuarantineMtx;

    int64_t freqKey(double hz) { return (int64_t)std::round(hz / 1000.0); }

    // Returns bookmark name if one exists within ±tolerance kHz, else "%.3f MHz"
    std::string displayName(double hz) {
        const int64_t tolerance = std::max((int64_t)5, (int64_t)std::round(channelSpacing / 1000.0 / 2.0));
        int64_t target = freqKey(hz);
        {
            std::lock_guard<std::mutex> lk(bookmarkNamesMtx);
            auto it = bookmarkNames.find(target);
            if (it != bookmarkNames.end()) return it->second.name;
            auto lo = bookmarkNames.lower_bound(target - tolerance);
            auto hi = bookmarkNames.upper_bound(target + tolerance);
            int64_t bestDist = tolerance + 1;
            std::string bestName;
            for (auto jt = lo; jt != hi; ++jt) {
                int64_t d = std::abs(jt->first - target);
                if (d < bestDist) { bestDist = d; bestName = jt->second.name; }
            }
            if (!bestName.empty()) return bestName;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%.3f MHz", hz / 1e6);
        return std::string(buf);
    }

    static std::string sanitizeForFilename(const std::string& s, size_t maxLen = 32) {
        std::string out;
        for (char c : s) {
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
                c == '"' || c == '<'  || c == '>' || c == '|' ||
                c == '\n' || c == '\r' || c == '\t')
                out += '-';
            else if (c == ' ')
                out += '-';
            else
                out += c;
        }
        while (!out.empty() && out.back() == '-') out.pop_back();
        if (out.size() > maxLen) out = out.substr(0, maxLen);
        return out;
    }

    struct BookmarkFilenameInfo { std::string bmName; std::string listName; };

    BookmarkFilenameInfo bookmarkForFilename(double hz) {
        const int64_t tolerance = std::max((int64_t)5, (int64_t)std::round(channelSpacing / 1000.0 / 2.0));
        int64_t target = freqKey(hz);
        BookmarkEntry found;
        {
            std::lock_guard<std::mutex> lk(bookmarkNamesMtx);
            auto it = bookmarkNames.find(target);
            if (it != bookmarkNames.end()) { found = it->second; }
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
        if (found.name.empty()) return {};
        return { sanitizeForFilename(found.name), sanitizeForFilename(found.listName) };
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

    void quarantineRnVoice(double hz) {
#ifndef CB_NO_RNNOISE
        if (!rnVoiceGateEnabled || rnVoiceGateQuarantineSec <= 0.0f) return;
        auto until = std::chrono::steady_clock::now() +
            std::chrono::milliseconds((int)std::round(rnVoiceGateQuarantineSec * 1000.0f));
        {
            std::lock_guard<std::mutex> lk(rnVoiceQuarantineMtx);
            rnVoiceQuarantineUntil[freqKey(hz)] = until;
        }
        flog::info("[ChannelBank] RNNoise quarantined {0:.3f}MHz for {1:.0f}s",
                   hz / 1e6, rnVoiceGateQuarantineSec);
#endif
    }

    bool isRnVoiceQuarantined(double hz) {
#ifndef CB_NO_RNNOISE
        if (!rnVoiceGateEnabled || rnVoiceGateQuarantineSec <= 0.0f) return false;
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(rnVoiceQuarantineMtx);
        auto it = rnVoiceQuarantineUntil.find(freqKey(hz));
        if (it == rnVoiceQuarantineUntil.end()) return false;
        if (now >= it->second) {
            rnVoiceQuarantineUntil.erase(it);
            return false;
        }
        return true;
#else
        return false;
#endif
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
#if defined(__APPLE__) || defined(_WIN32)
    void* txTranscribeFile(int backend, const char* path) {
        switch (backend) {
#ifdef __APPLE__
            case TB_APPLE_SPEECH: return transcription::transcribeFile(path);
#endif
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
#ifdef __APPLE__
        if (backend == TB_APPLE_SPEECH) transcription::cancel(h);
        else
#endif
        if (backend >= TB_WHISPER_ATC_LARGE) transcription_whisper::cancel(h);
    }
    void txDestroy(int backend, void* h) {
        if (!h) return;
#ifdef __APPLE__
        if (backend == TB_APPLE_SPEECH) transcription::destroy(h);
        else
#endif
        if (backend >= TB_WHISPER_ATC_LARGE) transcription_whisper::destroy(h);
    }
    std::string txGetText(int backend, void* h) {
        if (!h) return {};
#ifdef __APPLE__
        if (backend == TB_APPLE_SPEECH) return transcription::getText(h);
#endif
        if (backend >= TB_WHISPER_ATC_LARGE) return transcription_whisper::getText(h);
        return {};
    }
    bool txIsFinal(int backend, void* h) {
        if (!h) return true;
#ifdef __APPLE__
        if (backend == TB_APPLE_SPEECH) return transcription::isFinal(h);
#endif
        if (backend >= TB_WHISPER_ATC_LARGE) return transcription_whisper::isFinal(h);
        return true;
    }

    bool startTranscriptionJob(const std::string& path, const std::string& name) {
        if (path.empty() || !transcriptionOn()) return false;

        int backend = transcriptionBackend;
        void* handle = txTranscribeFile(backend, path.c_str());
        if (!handle) return false;

        std::lock_guard<std::mutex> jlk(transcriptionJobsMtx);
        auto it = transcriptionJobs.find(path);
        if (it != transcriptionJobs.end() && it->second.handle) {
            txCancel(it->second.backend, it->second.handle);
            txDestroy(it->second.backend, it->second.handle);
        }
        transcriptionJobs[path] = TranscriptionJob{path, name, backend, handle};
        return true;
    }

    void cancelTranscriptionJobs() {
        std::vector<TranscriptionJob> jobs;
        {
            std::lock_guard<std::mutex> jlk(transcriptionJobsMtx);
            for (auto& [path, job] : transcriptionJobs) {
                if (job.handle) jobs.push_back(job);
            }
            transcriptionJobs.clear();
        }

        for (auto& job : jobs) {
            txCancel(job.backend, job.handle);
            txDestroy(job.backend, job.handle);
        }
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

    // WAV → M4A encode queue (processed after playback + transcription, bounded by timeout)
    struct EncodeState {
        bool        playbackDone      = false;
        bool        transcriptionDone = true;   // true = don't wait for transcription
        std::string finalM4APath;               // empty = derive beside wavPath
        std::string transcript;                 // filled in when transcription finalises
        float       avgSnrDb          = 0.0f;  // average SNR over the recording
        std::chrono::steady_clock::time_point queuedAt;
        std::chrono::steady_clock::time_point playbackDoneAt;
    };
    struct EncodeTask {
        std::string wavPath;
        std::string finalM4APath;
        std::string transcript;
        float       avgSnrDb = 0.0f;
        int         attempt = 0;
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
        std::string finalM4APath;
        bool        deleteAfter;
#if defined(__APPLE__) || defined(_WIN32)
        std::vector<transcription_whisper::Segment> segments;
#endif
    };
    std::deque<PlaybackEntry> playbackQueue;
#if defined(__APPLE__) || defined(_WIN32)
    std::mutex transcriptionJobsMtx;
    std::map<std::string, TranscriptionJob> transcriptionJobs;
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
#if defined(__APPLE__) || defined(_WIN32)
    std::string                     currentPlaybackPath;
    std::mutex                      currentPlaybackPathMtx;
#endif
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

    // ── Config profiles ─────────────────────────────────────────────────────
    std::string              activeProfileName = "Default";
    std::vector<std::string> profileNames;
    int                      profileComboIdx   = 0;
    char                     newProfileNameBuf[64] = {};
    bool                     showNewProfilePopup   = false;
    bool                     showDeleteConfirm     = false;

    json snapshotProfile() {
        json p;
        p["spacingId"]        = spacingId;
        p["demodMode"]        = demodMode;
        p["ssbBfoHz"]         = ssbBfoHz;
        p["snrThreshold"]     = snrThreshold;
        p["holdHysteresisDb"] = holdHysteresisDb;
        p["maxRecordingSec"]  = maxRecordingSec;
        p["cooldownSec"]      = cooldownSec;
        p["recGain"]          = recGain;
        p["minTransmissionMs"]= minTransmissionMs;
        p["tailMs"]           = tailMs;
        p["maxChannels"]      = maxChannels;
        p["bwUsage"]          = bwUsage;
        p["noiseReduction"]   = noiseReduction;
        p["nrMix"]            = nrMix;
        p["rnVoiceGateEnabled"] = rnVoiceGateEnabled;
        p["rnVoiceGateVoiceFrac"] = rnVoiceGateVoiceFrac;
        p["rnVoiceGateProbeSec"] = rnVoiceGateProbeSec;
        p["rnVoiceGateQuarantineSec"] = rnVoiceGateQuarantineSec;
        p["nmsRadiusSlots"]   = nmsRadiusSlots;
        p["recPath"]          = folderSelect.path;
        p["staticGateEnabled"]  = staticGateEnabled;
        p["staticGateFlatness"] = staticGateFlatness;
        p["staticGateVoiceFrac"]= staticGateVoiceFrac;
        p["driftGateEnabled"]   = driftGateEnabled;
        p["driftMaxStdHz"]      = driftMaxStdHz;
        p["stuckNoiseGuardEnabled"] = stuckNoiseGuardEnabled;
        p["stuckNoiseGuardMinSec"] = stuckNoiseGuardMinSec;
        p["stuckNoiseGuardActiveFrac"] = stuckNoiseGuardActiveFrac;
        p["stuckNoiseGuardDriftHz"] = stuckNoiseGuardDriftHz;
        p["leftTrimFrac"]       = leftTrimFrac;
        p["rightTrimFrac"]      = rightTrimFrac;
        p["signalHoldMs"]       = signalHoldMs;
        p["recordingEnabled"]   = recordingEnabled;
        p["portableRecordingGroup"] = portableRecordingGroup;
        p["transcriptionBackend"] = transcriptionBackend;
        p["m4aEnabled"]         = m4aEnabled;
        p["normalizeRecordings"]= normalizeRecordings;
        p["manualMode"]         = manualMode;
        p["bookmarkScanMode"]   = bookmarkScanMode;
        p["manualPassbandLimit"]= manualPassbandLimit;
        p["scanMode"]           = scanMode;
        p["scanQuietSec"]       = scanQuietSec;
        p["scanNoSignalSec"]    = scanNoSignalSec;
        p["playbackAutoFlushEnabled"]    = playbackAutoFlushEnabled;
        p["playbackAutoFlushThreshold"]  = playbackAutoFlushThreshold;
        p["playbackAutoFlushKeepLatest"] = playbackAutoFlushKeepLatest;
        p["manualFrequencies"]  = json::array();
        for (auto f : manualFrequencies) p["manualFrequencies"].push_back(f);
        p["boundBookmarkLists"] = json::array();
        for (auto& s : boundBookmarkLists) p["boundBookmarkLists"].push_back(s);
        p["watchedFreqs"]       = json::array();
        for (auto k : watchedFreqs) p["watchedFreqs"].push_back(k);
        p["scanRanges"]         = json::array();
        for (auto& r : scanRanges) p["scanRanges"].push_back({{"start", r.startHz}, {"stop", r.stopHz}});
        return p;
    }

    void applyProfile(const json& p) {
        spacingId         = p.value("spacingId", 2);
        demodMode         = p.value("demodMode", (int)DEMOD_AM);
        ssbBfoHz          = p.value("ssbBfoHz", 0);
        snrThreshold      = p.value("snrThreshold", 4.0f);
        holdHysteresisDb  = p.value("holdHysteresisDb", 4.0f);
        maxRecordingSec   = p.value("maxRecordingSec", 90.0f);
        cooldownSec       = p.value("cooldownSec", 5.0f);
        recGain           = p.value("recGain", 0.25f);
        minTransmissionMs = p.value("minTransmissionMs", 300);
        tailMs            = p.value("tailMs", 500);
        maxChannels       = p.value("maxChannels", 16);
        bwUsage           = p.value("bwUsage", 0.8f);
        noiseReduction    = p.value("noiseReduction", false);
        nrMix             = p.value("nrMix", 0.7f);
        rnVoiceGateEnabled = p.value("rnVoiceGateEnabled", false);
        rnVoiceGateVoiceFrac = p.value("rnVoiceGateVoiceFrac", 0.20f);
        rnVoiceGateProbeSec = p.value("rnVoiceGateProbeSec", 10.0f);
        rnVoiceGateQuarantineSec = p.value("rnVoiceGateQuarantineSec", 60.0f);
        nmsRadiusSlots    = p.value("nmsRadiusSlots", 2);
        if (p.contains("recPath")) folderSelect.setPath(p["recPath"].get<std::string>());
        staticGateEnabled  = p.value("staticGateEnabled", true);
        staticGateFlatness = p.value("staticGateFlatness", 0.55f);
        staticGateVoiceFrac= p.value("staticGateVoiceFrac", 0.30f);
        driftGateEnabled   = p.value("driftGateEnabled", true);
        driftMaxStdHz      = p.value("driftMaxStdHz", 700.0f);
        stuckNoiseGuardEnabled = p.value("stuckNoiseGuardEnabled", false);
        stuckNoiseGuardMinSec = p.value("stuckNoiseGuardMinSec", 45.0f);
        stuckNoiseGuardActiveFrac = p.value("stuckNoiseGuardActiveFrac", 0.85f);
        stuckNoiseGuardDriftHz = p.value("stuckNoiseGuardDriftHz", 900.0f);
        leftTrimFrac       = p.value("leftTrimFrac", 0.0f);
        rightTrimFrac      = p.value("rightTrimFrac", 0.0f);
        signalHoldMs       = p.value("signalHoldMs", 500);
        recordingEnabled   = p.value("recordingEnabled", true);
        portableRecordingGroup = p.value("portableRecordingGroup", false);
        transcriptionBackend = p.value("transcriptionBackend", (int)TB_OFF);
        m4aEnabled         = p.value("m4aEnabled", false);
        normalizeRecordings= p.value("normalizeRecordings", true);
        manualMode         = p.value("manualMode", false);
        bookmarkScanMode   = p.value("bookmarkScanMode", false);
        manualPassbandLimit= p.value("manualPassbandLimit", false);
        scanMode           = p.value("scanMode", false);
        scanQuietSec       = p.value("scanQuietSec", 3.0f);
        scanNoSignalSec    = p.value("scanNoSignalSec", 1.0f);
        playbackAutoFlushEnabled    = p.value("playbackAutoFlushEnabled", true);
        playbackAutoFlushThreshold  = p.value("playbackAutoFlushThreshold", 30);
        playbackAutoFlushKeepLatest = p.value("playbackAutoFlushKeepLatest", 5);

        manualFrequencies.clear();
        if (p.contains("manualFrequencies"))
            for (auto& j : p["manualFrequencies"]) manualFrequencies.push_back(j.get<double>());
        boundBookmarkLists.clear();
        if (p.contains("boundBookmarkLists"))
            for (auto& j : p["boundBookmarkLists"]) boundBookmarkLists.insert(j.get<std::string>());
        watchedFreqs.clear();
        if (p.contains("watchedFreqs"))
            for (auto& j : p["watchedFreqs"]) watchedFreqs.insert(j.get<int64_t>());
        scanRanges.clear();
        if (p.contains("scanRanges"))
            for (auto& j : p["scanRanges"]) scanRanges.push_back({ j.value("start", 0.0), j.value("stop", 0.0) });

        channelSpacing = SPACINGS[std::clamp(spacingId, 0, 5)];
        rebuildBoundFreqs();
    }

    void saveCurrentProfile() {
        config.acquire();
        config.conf[name]["profiles"][activeProfileName] = snapshotProfile();
        config.conf[name]["activeProfile"] = activeProfileName;
        config.release(true);
    }

    void loadProfile(const std::string& profileName) {
        config.acquire();
        if (config.conf[name].contains("profiles") &&
            config.conf[name]["profiles"].contains(profileName)) {
            applyProfile(config.conf[name]["profiles"][profileName]);
            activeProfileName = profileName;
            config.conf[name]["activeProfile"] = activeProfileName;
            // Write loaded settings to top-level keys for backward compat
            json snap = snapshotProfile();
            for (auto& [k, v] : snap.items()) {
                config.conf[name][k] = v;
            }
        }
        config.release(true);
        refreshProfileNames();
    }

    void refreshProfileNames() {
        profileNames.clear();
        profileComboIdx = 0;
        config.acquire();
        if (config.conf[name].contains("profiles")) {
            for (auto& [k, v] : config.conf[name]["profiles"].items()) {
                profileNames.push_back(k);
            }
        }
        config.release();
        std::sort(profileNames.begin(), profileNames.end());
        for (int i = 0; i < (int)profileNames.size(); i++) {
            if (profileNames[i] == activeProfileName) { profileComboIdx = i; break; }
        }
        if (profileNames.empty()) {
            profileNames.push_back("Default");
            activeProfileName = "Default";
            profileComboIdx = 0;
        }
    }

    void migrateToProfiles() {
        config.acquire();
        if (!config.conf[name].contains("profiles")) {
            config.conf[name]["profiles"]["Default"] = snapshotProfile();
            config.conf[name]["activeProfile"] = "Default";
            config.release(true);
        } else {
            if (config.conf[name].contains("activeProfile"))
                activeProfileName = config.conf[name]["activeProfile"].get<std::string>();
            config.release();
        }
        refreshProfileNames();
    }
};

MOD_EXPORT void _INIT_() {
    std::string root = (std::string)core::args["root"];
    std::filesystem::create_directories(root + "/channel_bank/recordings");
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
