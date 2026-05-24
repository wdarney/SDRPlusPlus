#ifdef _WIN32
#define NOMINMAX
#define _USE_MATH_DEFINES
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
    dsp::channel::FrequencyXlator*                  preXlator    = nullptr;
    dsp::multirate::PowerDecimator<dsp::complex_t>* preDecimator = nullptr;
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
    std::atomic<bool>                          signalPresent  { false };
    bool                                       prevSignalPresent = false; // rising-edge detect for watch alert

#ifdef __APPLE__
    void*       transcribeHandle       = nullptr;
    std::string liveTranscript;
    std::string pendingTranscriptPath;
#endif

    // SNR accumulation — written by management thread (under channelsMtx),
    // read by audioHandler at file-close time (intentional benign race; both
    // are at most one 250ms sample apart and the values are used for metadata only).
    float snrSum   = 0.0f;  // sum of SNR dB samples taken while recording + signal present
    int   snrCount = 0;     // number of samples in snrSum

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
    static constexpr int SPAWN_VOTES = 3;    // FFT frames above threshold before spawning
    static constexpr int MAX_VOTES   = 8;    // vote cap (controls how fast channel drops out)

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
        if (config.conf[name].contains("recordingEnabled"))
            recordingEnabled = config.conf[name]["recordingEnabled"];
        if (config.conf[name].contains("transcriptionEnabled"))
            transcriptionEnabled = config.conf[name]["transcriptionEnabled"];
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

        // Precompute Hann window
        hannWindow.resize(FFT_SIZE);
        for (int i = 0; i < FFT_SIZE; i++) {
            hannWindow[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (FFT_SIZE - 1)));
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
        avgPower.clear();           // reset spectrum averaging on start
        globalNoiseFloor = 0.0f;

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

        // Bind spectrum monitor stream
        specStream = new dsp::stream<dsp::complex_t>();
        sigpath::iqFrontEnd.bindIQStream(specStream);
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
        sigpath::iqFrontEnd.unbindIQStream(specStream);
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

        // Find peak from the post-trim audio to set the normalization scale.
        int32_t peak = 0;
        for (int i = outStart; i < (int)samples.size(); i++)
            peak = std::max(peak, (int32_t)std::abs((int32_t)samples[i]));
        if (peak < 100) return;  // silent or too short — skip

        float scale = 23170.0f / (float)peak;  // target -3 dBFS
        if (scale > 8.0f) scale = 8.0f;

        // Apply scale to all samples.
        for (auto& smp : samples) {
            int32_t v = std::clamp((int32_t)std::round((float)smp * scale), -32768, 32767);
            smp = (int16_t)v;
        }

        int outCount = (int)samples.size() - outStart;
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
        slot.fileTrimSamples      = 9600; // skip first 200ms from WAV file
        slot.recFadeRemaining     = 4800;   // 100ms fade-in at 48kHz — suppresses PTT click and filter ring
        slot.snrSum               = 0.0f;
        slot.snrCount             = 0;
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

        for (int i = 0; i < count; i++) {
            _this->fftAccum[_this->fftBufPos] = data[i];
            if (++_this->fftBufPos >= FFT_SIZE) {
                _this->fftBufPos = 0;
                _this->analyzeSpectrum();
            }
        }
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
            globalNoiseFloor  = 0.0f;
            displayNoiseFloor = 0.0f;
            floorHistory.clear();
            slotVotes.clear();
            manualVotes.clear();
            retuneFlag.store(false);
            return;  // skip this frame — buffers are stale from old tuning
        }

        // Apply Hann window and copy to FFTW input
        for (int i = 0; i < FFT_SIZE; i++) {
            float w = hannWindow[i];
            fftIn[i][0] = fftAccum[i].re * w;
            fftIn[i][1] = fftAccum[i].im * w;
        }

        fftwf_execute(fftPlan);

        // Compute linear power per bin (FFT-shifted, normalised)
        float scale = 1.0f / (float)(FFT_SIZE * FFT_SIZE);

        // Exponential moving average across FFT frames.
        // alpha ~0.15 gives an effective averaging window of ~7 frames (~24ms at
        // 2.4MHz SR), which reduces noise variance by ~7x while still reacting
        // quickly to real signals.
        constexpr float alpha = 0.15f;
        bool firstFrame = avgPower.empty();
        if (firstFrame) avgPower.resize(FFT_SIZE);
        for (int i = 0; i < FFT_SIZE; i++) {
            int k = (i + FFT_SIZE / 2) % FFT_SIZE;
            float re = fftOut[k][0], im = fftOut[k][1];
            float inst = (re * re + im * im) * scale;
            avgPower[i] = firstFrame ? inst : (alpha * inst + (1.0f - alpha) * avgPower[i]);
        }
        // Use the averaged spectrum for all detection
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

        // snrLinear computed once per frame
        float snrLinear = powf(10.0f, snrThreshold / 10.0f);

        // Compute per-slot means (using detection window, not full slot width)
        std::vector<float> slotMeans(numSlots);
        std::map<int, double> newPeakOffsets;
        for (int s = 0; s < numSlots; s++) {
            double slotOffset = ((double)s - (double)(numSlots - 1) / 2.0) * channelSpacing;
            int centerBin = (int)std::round((slotOffset / lastKnownSr) * FFT_SIZE) + FFT_SIZE / 2;
            int lo = std::clamp(centerBin - halfBins, 0, FFT_SIZE - 1);
            int hi = std::clamp(centerBin + halfBins, 0, FFT_SIZE - 1);
            float sum = 0.0f;
            int   peakBin = lo;
            for (int b = lo; b <= hi; b++) {
                sum += power[b];
                if (power[b] > power[peakBin]) peakBin = b;
            }
            slotMeans[s] = sum / (float)(hi - lo + 1);
            // Spectral centroid: energy-weighted average bin frequency.
            // For SSB voice, this lands near the middle of the voice passband
            // (~1000–1500 Hz above/below carrier) rather than at the loudest
            // fundamental (~300–500 Hz), giving much better carrier tracking.
            double weightedSum = 0.0;
            for (int b = lo; b <= hi; b++)
                weightedSum += (double)b * (double)power[b];
            double centroidBin = (sum > 0.0f) ? (weightedSum / (double)sum) : (double)centerBin;
            newPeakOffsets[s] = ((centroidBin - FFT_SIZE / 2) / FFT_SIZE) * lastKnownSr;
        }

        // Global noise floor — 20th percentile of CENTER slot means (bwUsage
        // fraction).  Edge slots are excluded so filter rolloff doesn't inflate
        // the floor estimate, but those slots can still detect signals.
        // The spectrum EMA already smooths bin-level noise, so the rawFloor from
        // the 20th percentile is stable.  We use a simple symmetric EMA here
        // (fast enough to track real noise changes, slow enough to ignore brief
        // signal bursts that leak into the 20th percentile).
        {
            int edgeSkip = (int)std::round(numSlots * (1.0f - bwUsage) / 2.0f);
            std::vector<float> centerMeans;
            for (int s = edgeSkip; s < numSlots - edgeSkip; s++)
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

        // Manual mode: check configured frequencies instead of grid voting
        if (manualMode) {
            std::vector<double> localFreqs = getActiveManualFreqs();
            std::set<int>       newDetected;
            std::map<int,float> newManualSnr;
            for (int i = 0; i < (int)localFreqs.size(); i++) {
                double freqOffset = localFreqs[i] - lastKnownCenter;
                if (std::abs(freqOffset) >= lastKnownSr / 2.0) continue;
                int centerBin = (int)std::round((freqOffset / lastKnownSr) * FFT_SIZE) + FFT_SIZE / 2;
                int halfBins2 = std::max(1, (int)std::round(std::min(channelSpacing * 0.4, DETECT_BW_HZ / 2.0) / binHz));
                int lo = std::clamp(centerBin - halfBins2, 0, FFT_SIZE - 1);
                int hi = std::clamp(centerBin + halfBins2, 0, FFT_SIZE - 1);
                float sum = 0.0f;
                for (int b = lo; b <= hi; b++) sum += power[b];
                float mean = sum / (float)(hi - lo + 1);
                bool above = (mean > globalNoiseFloor * snrLinear);
                // Store per-freq SNR for M4A metadata
                if (globalNoiseFloor > 0.0f)
                    newManualSnr[i] = 10.0f * log10f(mean / globalNoiseFloor);
                int& v = manualVotes[i];
                v = above ? std::min(v + 1, MAX_VOTES) : std::max(v - 1, 0);
                if (v >= SPAWN_VOTES) newDetected.insert(i);
            }
            {
                std::lock_guard<std::mutex> lk(manualDetectedMtx);
                manualDetected = newDetected;
                manualSnrDb    = std::move(newManualSnr);
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
            mgmtCv.notify_one();
            return;
        }

        // Vote on each slot against the global floor
        for (int s = 0; s < numSlots; s++) {
            bool aboveThreshold = (slotMeans[s] > globalNoiseFloor * snrLinear);
            int& votes = slotVotes[s];
            if (aboveThreshold) { votes = std::min(votes + 1, MAX_VOTES); }
            else                { votes = std::max(votes - 1, 0); }
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
            slot->liveTranscript = transcription::getText(slot->transcribeHandle);
            if (!transcription::isFinal(slot->transcribeHandle)) continue;

            // Update live transcript display
            if (!slot->liveTranscript.empty()) {
                std::lock_guard<std::mutex> tlk(lastTranscriptMtx);
                lastTranscriptText = slot->liveTranscript;
                lastTranscriptName = displayName(slot->freqHz);
            }
            transcription::destroy(slot->transcribeHandle);
            slot->transcribeHandle = nullptr;

            // Transcription chain is done — check if encoding can now proceed.
            // Store the transcript text so the encode thread can embed it as ©lyr.
            if (m4aEnabled && recordingEnabled) {
                bool canEncode = false;
                std::string encodePath, encodeTranscript;
                float       encodeSnrDb = 0.0f;
                {
                    std::lock_guard<std::mutex> elk(pendingEncodesMtx);
                    auto it = pendingEncodes.find(slot->pendingTranscriptPath);
                    if (it != pendingEncodes.end()) {
                        it->second.transcriptionDone = true;
                        it->second.transcript        = slot->liveTranscript;
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
            std::map<int, double> localPeakOffsets;
            std::vector<float>    localSnrDb;
            {
                std::lock_guard<std::mutex> lck(detectedMtx);
                current          = detectedSlots;
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
                    if (isBlocked(slotFreq)) { blkSkip++; continue; }
                    flog::info("[ChannelBank] Spawning slot {0} at {1:.3f}MHz", idx, slotFreq / 1e6);
                    auto* slot = new ChannelSlot();
                    slot->lastDetected  = now;
                    slot->signalPresent = true;
                    initSlot(*slot, idx, numSlots, peakOffHz);
                    activeChannels[idx] = slot;
                }
                else {
                    it->second->lastDetected  = now;
                    it->second->signalPresent = true;
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

                bool detected = current.count(it->first) > 0;
                if (detected) slot->lastDetected = now;
                {
                    float holdElapsed = std::chrono::duration<float>(now - slot->lastDetected).count();
                    slot->signalPresent = detected || (holdElapsed * 1000.0f < (float)signalHoldMs);
                }
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

    static unsigned int computePreDecimRatio(double sr) {
        constexpr double MIN_INTERMEDIATE_SR = 4.0 * 48000.0;
        unsigned int maxRatio = dsp::multirate::PowerDecimator<dsp::complex_t>::getMaxRatio();
        unsigned int ratio = 1;
        while (ratio * 2 <= maxRatio && (sr / (ratio * 2)) >= MIN_INTERMEDIATE_SR) {
            ratio *= 2;
        }
        return ratio;
    }

    // exactOffsetHz: when not NaN, overrides the grid-based offset calculation and
    // disables spectral-centroid / BFO adjustment (used by manual mode).
    void initSlot(ChannelSlot& slot, int gridIdx, int numSlots, double peakOffsetHz, double exactOffsetHz = NAN) {
        slot.module  = this;
        slot.gridIdx = gridIdx;

        bool isManual = !std::isnan(exactOffsetHz);
        // Always use grid-aligned offset in auto mode. Signals are on standard channel
        // frequencies so there is no benefit to peak-bin tuning, and it causes the VFO
        // to lock onto a sideband rather than the carrier.
        // In manual mode, use the caller-supplied exact offset.
        double offset = isManual
            ? exactOffsetHz
            : ((double)gridIdx - (double)(numSlots - 1) / 2.0) * channelSpacing;
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
        const double vfoOff = isManual
            ? (demodMode == DEMOD_USB ? offset + ssbBw / 2.0
             : demodMode == DEMOD_LSB ? offset - ssbBw / 2.0
             : offset)
            : (demodMode == DEMOD_USB || demodMode == DEMOD_LSB)
                ? peakOffsetHz - (double)ssbBfoHz
                : offset;

        slot.iqIn = new dsp::stream<dsp::complex_t>();
        sigpath::iqFrontEnd.bindIQStream(slot.iqIn);

        unsigned int preDecimRatio = computePreDecimRatio(lastKnownSr);
        if (preDecimRatio > 1) {
            double preDecimSr = lastKnownSr / preDecimRatio;
            slot.preXlator    = new dsp::channel::FrequencyXlator(slot.iqIn, -vfoOff, lastKnownSr);
            slot.preDecimator = new dsp::multirate::PowerDecimator<dsp::complex_t>(&slot.preXlator->out, preDecimRatio);
            slot.vfo = new dsp::channel::RxVFO(&slot.preDecimator->out, preDecimSr, audioSr, bw, 0.0);
        } else {
            slot.vfo = new dsp::channel::RxVFO(slot.iqIn, lastKnownSr, audioSr, bw, vfoOff);
        }

        // AM demod bandwidth = full channel width (same as VFO), matching SDR++ radio module.
        // SSB/FM use narrower audio bandwidth.
        const double audioBw = bw / 2.0;
        if (demodMode == DEMOD_AM) {
            slot.amDemod = new dsp::demod::AM<dsp::stereo_t>();
            slot.amDemod->init(&slot.vfo->out,
                dsp::demod::AM<dsp::stereo_t>::AGCMode::CARRIER,
                bw, 1.0 / (audioSr * 1.0), 1.0 / (audioSr * 2.0), 100.0 / audioSr, audioSr);
        }
        else if (demodMode == DEMOD_USB || demodMode == DEMOD_LSB) {
            auto ssbMode = (demodMode == DEMOD_USB)
                ? dsp::demod::SSB<dsp::stereo_t>::Mode::USB
                : dsp::demod::SSB<dsp::stereo_t>::Mode::LSB;
            slot.ssbDemod = new dsp::demod::SSB<dsp::stereo_t>();
            slot.ssbDemod->init(&slot.vfo->out, ssbMode, ssbBw, audioSr, 1.0 / (audioSr * 1.0), 1.0 / (audioSr * 2.0));
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

        if (slot.preXlator)    slot.preXlator->start();
        if (slot.preDecimator) slot.preDecimator->start();
        slot.vfo->start();
        if (slot.amDemod)  slot.amDemod->start();
        if (slot.fmDemod)  slot.fmDemod->start();
        if (slot.ssbDemod) slot.ssbDemod->start();
        slot.splitter->start();
        slot.meter->start();
        slot.recSink->start();

    }

    void destroySlot(ChannelSlot& slot) {
        slot.recSink->stop();
        slot.meter->stop();
        slot.splitter->stop();
        if (slot.amDemod)  slot.amDemod->stop();
        if (slot.fmDemod)  slot.fmDemod->stop();
        if (slot.ssbDemod) slot.ssbDemod->stop();
        slot.vfo->stop();
        if (slot.preDecimator) { slot.preDecimator->stop(); delete slot.preDecimator; slot.preDecimator = nullptr; }
        if (slot.preXlator)    { slot.preXlator->stop();    delete slot.preXlator;    slot.preXlator    = nullptr; }

        sigpath::iqFrontEnd.unbindIQStream(slot.iqIn);

        if (slot.fileOpen) {
            slot.writer.close();
            slot.fileOpen = false;
            if (slot.module) {
                int64_t tailSamples   = (int64_t)slot.module->tailMs * 48000 / 1000;
                int64_t signalSamples = slot.audioSamplesWritten - tailSamples;
                int64_t signalMs      = signalSamples * 1000 / 48000;
                if (signalMs < (int64_t)slot.module->minTransmissionMs) {
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
            transcription::cancel(slot.transcribeHandle);
            transcription::destroy(slot.transcribeHandle);
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

        // Use FFT-based detection (signalPresent) rather than audio amplitude.
        // AM/FM demodulators always output noise, so amplitude-based silence
        // detection is unreliable — the FFT already knows if a signal is there.
        if (slot->signalPresent.load()) {
            slot->inSilence = false;
            if (!slot->fileOpen) { _this->openNewFile(*slot); }
        }
        else {
            // Signal gone — start 1-second grace before closing file
            if (!slot->inSilence) {
                slot->inSilence    = true;
                slot->silenceStart = std::chrono::steady_clock::now();
            }
            if (slot->fileOpen) {
                auto silenceElapsed = std::chrono::steady_clock::now() - slot->silenceStart;
                if (silenceElapsed >= std::chrono::milliseconds(_this->tailMs)) {
                    // Signal duration = total samples written minus tail samples.
                    // This is exact — no vote-accumulation or poll-delay error.
                    int64_t tailSamples = (int64_t)_this->tailMs * 48000 / 1000;
                    int64_t signalSamples = slot->audioSamplesWritten - tailSamples;
                    int64_t signalMs = signalSamples * 1000 / 48000;
                    slot->writer.close();
                    slot->fileOpen = false;
                    if (signalMs < (int64_t)_this->minTransmissionMs) {
                        flog::info("[ChannelBank] Discarding short recording ({0}ms < {1}ms threshold)", signalMs, _this->minTransmissionMs);
                        std::remove(slot->currentFilePath.c_str());
                    } else {
                        flog::info("[ChannelBank] Keeping recording ({0}ms >= {1}ms threshold) slot {2}", signalMs, _this->minTransmissionMs, slot->gridIdx);
                        normalizeWavFile(slot->currentFilePath);
#ifdef __APPLE__
                        if (_this->transcriptionEnabled) {
                            if (slot->transcribeHandle) {
                                transcription::cancel(slot->transcribeHandle);
                                transcription::destroy(slot->transcribeHandle);
                            }
                            slot->pendingTranscriptPath = slot->currentFilePath;
                            slot->liveTranscript.clear();
                            slot->transcribeHandle = transcription::transcribeFile(slot->currentFilePath.c_str());
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
                            es.transcriptionDone = (!_this->transcriptionEnabled || !slot->transcribeHandle);
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

        // Tail fade-out: raised-cosine from 1→0 over the tail period.
        // This kills the AGC noise ramp-up that happens when the signal drops —
        // the demodulator chases noise and gets louder, so we taper it to silence.
        // Compute once per callback (chunk is short, ~1024 samples — smooth enough).
        float tailFade = 1.0f;
        if (slot->inSilence && _this->tailMs > 0) {
            auto silenceElapsed = std::chrono::steady_clock::now() - slot->silenceStart;
            float frac = std::clamp(
                std::chrono::duration<float>(silenceElapsed).count() / (_this->tailMs * 0.001f),
                0.0f, 1.0f);
            tailFade = 0.5f * (1.0f + cosf(M_PI * frac));  // 1.0 → 0.0
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
                currentlyPlayingFreqKey.store(freqKey(playFreq));
                playbackWavFile(path);
                currentlyPlayingFreqKey.store(0);
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
        }
    }

    // ── Main-waterfall overlay ───────────────────────────────────────────────
    // Draws a marker on the SDR++ main waterfall for each active channel:
    //   • red filled dot  = currently recording (file open, signal present)
    //   • orange ring     = active but in cooldown / silence countdown
    // Plus a counter in the top-right of the FFT area showing
    // "Recording: N / Active: M".

    static void fftRedrawHandlerFunc(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        ChannelBankModule* _this = (ChannelBankModule*)ctx;
        if (!_this->enabled || !_this->running) { return; }

        // Snapshot active-channel state under lock; minimise work inside the lock.
        int64_t playingKey = _this->currentlyPlayingFreqKey.load();
        struct Mark { double freq; bool recording; bool playing; };
        std::vector<Mark> marks;
        int  recCount              = 0;
        bool playingKeyAccountedFor = false;
        {
            std::lock_guard<std::mutex> clck(_this->channelsMtx);
            marks.reserve(_this->activeChannels.size());
            for (auto& [idx, slot] : _this->activeChannels) {
                bool rec  = slot->fileOpen;
                if (rec) recCount++;
                bool play = (playingKey != 0 && _this->freqKey(slot->freqHz) == playingKey);
                if (play) playingKeyAccountedFor = true;
                marks.push_back({ slot->freqHz, rec, play });
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

            if (m.recording) {
                // Red filled dot — actively recording
                dl->AddCircleFilled(c, radius, IM_COL32(255, 60, 60, 255), 16);
                dl->AddCircle(c, radius + 1.0f, IM_COL32(255, 255, 255, 255), 16, 1.5f);
            } else {
                // Orange ring — active channel, not recording
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
        // Speech transcription toggle
        {
            auto txStatus = transcription::authStatus();
            if (txStatus == transcription::AuthStatus::NotConfigured) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.2f, 1.0f));
                ImGui::TextWrapped("Transcription: NSSpeechRecognitionUsageDescription missing from Info.plist");
                ImGui::PopStyleColor();
            } else if (txStatus == transcription::AuthStatus::Denied) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::TextUnformatted("Transcription: access denied");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::SmallButton(CONCAT("Open Settings##_cb_txset_", _this->name)))
                    transcription::openSystemSettings();
            } else if (txStatus == transcription::AuthStatus::NotDetermined) {
                bool dummy = false;
                if (ImGui::Checkbox(CONCAT("Transcribe (click to authorize)##_cb_txen_", _this->name), &dummy))
                    transcription::requestPermission();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Click to request speech recognition permission from macOS");
            } else {
                if (ImGui::Checkbox(CONCAT("Transcribe recordings##_cb_txen_", _this->name),
                                    &_this->transcriptionEnabled)) {
                    config.acquire();
                    config.conf[_this->name]["transcriptionEnabled"] = _this->transcriptionEnabled;
                    config.release(true);
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
                _this->transcriptionEnabled ? " + transcription" : "");
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
                if (_this->transcriptionEnabled) {
                    std::lock_guard<std::mutex> tlk(_this->lastTranscriptMtx);
                    txText = _this->lastTranscriptText;
                    txName = _this->lastTranscriptName;
                }
                float panelH = (!txText.empty()) ? 110.0f : 50.0f;
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
                if (!txText.empty()) {
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.6f, 1.0f, 1.0f));
                    if (!txName.empty())
                        ImGui::Text("[%s]", txName.c_str());
                    ImGui::PopStyleColor();
                    ImGui::TextWrapped("%s", txText.c_str());
                } else if (_this->transcriptionEnabled) {
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

            // Group entries by 50 MHz band
            std::map<int64_t, std::vector<std::pair<int64_t, FreqEntry*>>> bands;
            for (auto& [k, e] : _this->freqLog)
                bands[(int64_t)std::floor(e.freqHz / 50e6)].push_back({k, &e});

            for (auto& [bandIdx, entries] : bands) {
                // Sort entries within band by count desc
                std::sort(entries.begin(), entries.end(),
                    [](auto& a, auto& b){ return a.second->count > b.second->count; });

                char hdr[64];
                snprintf(hdr, sizeof(hdr), "%lld-%lld MHz (%d)##band_%lld",
                         (long long)(bandIdx * 50), (long long)(bandIdx * 50 + 50),
                         (int)entries.size(), (long long)bandIdx);
                if (ImGui::CollapsingHeader(hdr)) {
                    for (auto& [k, ep] : entries) {
                        FreqEntry& e = *ep;
                        std::string dn = _this->displayName(e.freqHz);
                        std::string label = "  " + dn;

                        if (e.blocked)
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));

                        std::string rel = relTime(e.lastSeen);
                        ImGui::Text("%-22s %5d  %-10s", label.c_str(), e.count, rel.c_str());

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
        std::map<int,float> localSnrDb;
        {
            std::lock_guard<std::mutex> lk(manualDetectedMtx);
            localDetected = manualDetected;
            localSnrDb    = manualSnrDb;
        }

        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> clck(channelsMtx);

        // All freqs in the current stop are within SDR bandwidth by construction;
        // the offset check guards against stale lastKnownCenter during retune.
        std::set<int> desired;
        for (int i = 0; i < (int)stop.freqsHz.size(); i++) {
            double offset = stop.freqsHz[i] - lastKnownCenter;
            if (std::abs(offset) < lastKnownSr / 2.0)
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
            bool present = localDetected.count(idx) > 0;
            if (present) it->second->lastDetected = now;
            {
                float holdElapsed = std::chrono::duration<float>(now - it->second->lastDetected).count();
                it->second->signalPresent = present || (holdElapsed * 1000.0f < (float)signalHoldMs);
            }
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
            double offset = stop.freqsHz[i] - lastKnownCenter;
            flog::info("[ChannelBank] BkScan: spawning slot {0} at {1:.3f}MHz", i, stop.freqsHz[i] / 1e6);
            auto* slot = new ChannelSlot();
            slot->lastDetected  = now;
            slot->signalPresent = localDetected.count(i) > 0;
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
        std::map<int,float> localSnrDb;
        {
            std::lock_guard<std::mutex> lk(manualDetectedMtx);
            localDetected = manualDetected;
            localSnrDb    = manualSnrDb;
        }
        std::vector<double> localFreqs = getActiveManualFreqs();

        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> clck(channelsMtx);

        // Build desired set (indices currently within SDR bandwidth)
        std::set<int> desired;
        for (int i = 0; i < (int)localFreqs.size(); i++) {
            double offset = localFreqs[i] - lastKnownCenter;
            if (std::abs(offset) < lastKnownSr / 2.0)
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
            bool present = localDetected.count(idx) > 0;
            if (present) it->second->lastDetected = now;
            {
                float holdElapsed = std::chrono::duration<float>(now - it->second->lastDetected).count();
                it->second->signalPresent = present || (holdElapsed * 1000.0f < (float)signalHoldMs);
            }
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
            slot->lastDetected  = now;
            slot->signalPresent = localDetected.count(i) > 0;
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
    float        snrThreshold  = 4.0f;      // dB above noise floor
    float        cooldownSec   = 5.0f;      // seconds before destroying a quiet channel
    int          signalHoldMs          = 500;    // hold signalPresent true N ms after last detection (dropout hysteresis)
    bool         recordingEnabled      = true;   // global recording on/off toggle
    bool         transcriptionEnabled  = false;  // Apple Speech transcription
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
    std::set<int>       manualDetected;   // written by DSP, read by mgmt thread
    std::map<int,float> manualSnrDb;      // per-freq SNR dB; manual/bookmark scan mode
    std::mutex          manualDetectedMtx;
    std::set<int64_t> watchedFreqs;       // watched freq keys; protected by manualFreqMtx
    std::atomic<int64_t> watchAlert{0};   // non-zero = freqKey of watched freq that just fired

    // Description editor (UI thread only)
    int64_t descEditKey = 0;
    char    descEditBuf[256] = {};
    bool    descEditRequest = false;

    // FM list cache (populated by loadFMConfig, UI thread only)
    std::map<std::string, std::vector<double>> fmLists;
    std::mutex bookmarkNamesMtx;
    std::map<int64_t, std::string> bookmarkNames;   // freqKey -> "Tower KLAX"

    // FFT spectrum monitor
    dsp::stream<dsp::complex_t>*            specStream     = nullptr;
    dsp::sink::Handler<dsp::complex_t>*     specSink       = nullptr;
    fftwf_complex*                          fftIn          = nullptr;
    fftwf_complex*                          fftOut         = nullptr;
    fftwf_plan                              fftPlan;
    std::vector<float>                      hannWindow;
    std::vector<dsp::complex_t>             fftAccum;
    int                                     fftBufPos      = 0;
    std::atomic<int>                        debugDetectedCount { 0 };
    std::atomic<int>                        debugBlockedSkips  { 0 };
    std::atomic<int>                        debugCapSkips      { 0 };
    std::vector<float>                      avgPower;           // EMA-smoothed power spectrum
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

    // Active demod/record channels (managed by mgmt thread)
    std::mutex                      channelsMtx;
    std::map<int, ChannelSlot*>     activeChannels;

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

    void logRecording(double hz) {
        std::lock_guard<std::mutex> lk(freqLogMtx);
        auto  key  = freqKey(hz);
        auto& e    = freqLog[key];
        e.freqHz   = hz;
        e.count++;
        e.lastSeen = (int64_t)std::time(nullptr);
    }

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
    struct PlaybackEntry { std::string path; double freqHz; bool deleteAfter; };
    std::deque<PlaybackEntry> playbackQueue;
#ifdef __APPLE__
    std::mutex  lastTranscriptMtx;
    std::string lastTranscriptText;  // most recently completed transcript
    std::string lastTranscriptName;  // displayName of the transcribed freq
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
