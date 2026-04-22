#include <imgui.h>
#include <module.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <dsp/channel/rx_vfo.h>
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
    std::atomic<bool>                          signalPresent  { false };
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
        if (config.conf[name].contains("manualFrequencies"))
            for (auto& j : config.conf[name]["manualFrequencies"])
                manualFrequencies.push_back(j.get<double>());
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
        if (running) { stop(); }
        fftwf_destroy_plan(fftPlan);
        fftwf_free(fftIn);
        fftwf_free(fftOut);
    }

    void postInit() {}
    void enable()  { enabled = true; }
    void disable() { enabled = false; }
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

        // Bind spectrum monitor stream
        specStream = new dsp::stream<dsp::complex_t>();
        sigpath::iqFrontEnd.bindIQStream(specStream);
        specSink = new dsp::sink::Handler<dsp::complex_t>(specStream, spectrumHandler, this);
        specSink->start();

        // Start management thread
        mgmtRunning = true;
        mgmtThread = std::thread(&ChannelBankModule::managementThreadFunc, this);

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

        // Stop management thread
        mgmtRunning = false;
        mgmtCv.notify_all();
        if (mgmtThread.joinable()) { mgmtThread.join(); }

        // Stop spectrum monitor
        specSink->stop();
        sigpath::iqFrontEnd.unbindIQStream(specStream);
        delete specSink;  specSink  = nullptr;
        delete specStream; specStream = nullptr;

        // Teardown all active channels
        std::lock_guard<std::mutex> clck(channelsMtx);
        for (auto& [idx, slot] : activeChannels) {
            destroySlot(*slot);
            delete slot;
        }
        activeChannels.clear();
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

        int       outCount  = (int)samples.size() - outStart;
        if (outCount <= 0) return;

        uint32_t newDataSize = (uint32_t)(outCount * 2);
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
        fwrite(samples.data() + outStart, sizeof(int16_t), outCount, fw);
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
        char buf[256];
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
            if (globalNoiseFloor <= 0.0f) {
                globalNoiseFloor  = rawFloor;
                displayNoiseFloor = rawFloor;
            } else {
                // Symmetric EMA — tracks both up and down at the same rate.
                // ~0.05 gives ~20 frame time constant (~70ms at 2.4MHz SR).
                constexpr float nfAlpha = 0.05f;
                globalNoiseFloor = nfAlpha * rawFloor + (1.0f - nfAlpha) * globalNoiseFloor;
            }
            displayNoiseFloor = 0.985f * displayNoiseFloor + 0.015f * globalNoiseFloor;
        }

        // Manual mode: check configured frequencies instead of grid voting
        if (manualMode) {
            std::vector<double> localFreqs;
            { std::lock_guard<std::mutex> lk(manualFreqMtx); localFreqs = manualFrequencies; }
            std::set<int> newDetected;
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
                int& v = manualVotes[i];
                v = above ? std::min(v + 1, MAX_VOTES) : std::max(v - 1, 0);
                if (v >= SPAWN_VOTES) newDetected.insert(i);
            }
            {
                std::lock_guard<std::mutex> lk(manualDetectedMtx);
                manualDetected = newDetected;
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

        // Pass 2: non-maximum suppression — when adjacent slots both qualify,
        // only keep the one with higher power (prevents one signal spawning two chains).
        // Exception: if a channel is already active on a slot, always keep it detected
        // so it doesn't lose lock due to NMS oscillation between adjacent slots.
        std::set<int> detected;
        {
            std::lock_guard<std::mutex> clck(channelsMtx);
            for (int s = 0; s < numSlots; s++) {
                if (slotVotes[s] < SPAWN_VOTES) { continue; }
                // Already-active channels are exempt from NMS — prevents oscillation
                // when a signal falls between two slots at wider spacings.
                bool active = (activeChannels.find(s) != activeChannels.end());
                if (!active) {
                    bool leftStronger  = (s > 0            && slotVotes[s-1] >= SPAWN_VOTES && slotMeans[s-1] > slotMeans[s]);
                    bool rightStronger = (s < numSlots - 1 && slotVotes[s+1] >= SPAWN_VOTES && slotMeans[s+1] > slotMeans[s]);
                    if (leftStronger || rightStronger) { continue; }
                }
                detected.insert(s);
            }
        }

        {
            std::lock_guard<std::mutex> lck(detectedMtx);
            detectedSlots   = detected;
            slotPeakOffsets = std::move(newPeakOffsets);
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

    void managementThreadFunc() {
        while (mgmtRunning) {
            std::unique_lock<std::mutex> ulck(mgmtWaitMtx);
            mgmtCv.wait_for(ulck, std::chrono::milliseconds(250));
            if (!mgmtRunning) { break; }

            if (manualMode) { manageManualChannels(); continue; }

            std::set<int>         current;
            std::map<int, double> localPeakOffsets;
            {
                std::lock_guard<std::mutex> lck(detectedMtx);
                current          = detectedSlots;
                localPeakOffsets = slotPeakOffsets;
            }

            auto now = std::chrono::steady_clock::now();
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
                if (!detected) {
                    slot->signalPresent = false;
                    float elapsed = std::chrono::duration<float>(
                        now - slot->lastDetected).count();
                    bool isPlaying = (currentlyPlayingFreqKey.load() == freqKey(slot->freqHz));
                    bool isQueued  = false;
                    {
                        std::lock_guard<std::mutex> plk(playbackMtx);
                        int64_t fk = freqKey(slot->freqHz);
                        for (auto& entry : playbackQueue)
                            if (freqKey(entry.second) == fk) { isQueued = true; break; }
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
        slot.vfo  = new dsp::channel::RxVFO(slot.iqIn, lastKnownSr, audioSr, bw, vfoOff);
        sigpath::iqFrontEnd.bindIQStream(slot.iqIn);

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

        sigpath::iqFrontEnd.unbindIQStream(slot.iqIn);

        if (slot.fileOpen) {
            slot.writer.close();
            slot.fileOpen = false;
            if (slot.module) {
                int64_t tailSamples   = (int64_t)slot.module->tailMs * 48000 / 1000;
                int64_t signalSamples = slot.audioSamplesWritten - tailSamples;
                int64_t signalMs      = signalSamples * 1000 / 48000;
                if (signalMs < (int64_t)slot.module->minTransmissionMs)
                    std::remove(slot.currentFilePath.c_str());
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
                        // Log the frequency and queue for playback
                        _this->logRecording(slot->freqHz);
                        _this->saveFreqLog();
                        if (!slot->currentFilePath.empty()) {
                            std::lock_guard<std::mutex> lk(_this->playbackMtx);
                            _this->playbackQueue.push_back({slot->currentFilePath, slot->freqHz});
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

        // Apply recording gain + fade-in, then mix stereo down to mono in-place.
        // AM output is identical on L and R so averaging is lossless; it also
        // halves the file size with no audible difference.
        const int totalFade = 4800;
        float* mono = (float*)data;  // safe: mono[i] written before data[i] is needed
        for (int i = 0; i < count; i++) {
            float gain = _this->recGain;
            if (slot->recFadeRemaining > 0) {
                // Raised-cosine taper: zero slope at both ends — prevents onset pop
                float progress = 1.0f - (float)slot->recFadeRemaining / totalFade;
                gain *= 0.5f * (1.0f - cosf(M_PI * progress));
                slot->recFadeRemaining--;
            }
            mono[i] = std::clamp((data[i].l + data[i].r) * 0.5f * gain, -1.0f, 1.0f);
        }
        slot->writer.write(mono, count);
        slot->audioSamplesWritten += count;
    }

    // ── Playback monitor ─────────────────────────────────────────────────────

    void playbackThreadFunc() {
        const int CHUNK = 1024;
        std::vector<dsp::stereo_t> silence(CHUNK);
        memset(silence.data(), 0, CHUNK * sizeof(dsp::stereo_t));

        while (playbackRunning) {
            std::string path;
            double      playFreq = 0.0;
            {
                std::lock_guard<std::mutex> lk(playbackMtx);
                if (!playbackQueue.empty()) {
                    path     = playbackQueue.front().first;
                    playFreq = playbackQueue.front().second;
                    playbackQueue.pop_front();
                }
            }

            if (!path.empty()) {
                currentlyPlayingFreqKey.store(freqKey(playFreq));
                playbackWavFile(path);
                currentlyPlayingFreqKey.store(0);
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

    // ── Retune handler ───────────────────────────────────────────────────────

    static void retuneHandlerFunc(double /*freq*/, void* ctx) {
        ChannelBankModule* _this = (ChannelBankModule*)ctx;
        if (!_this->running) { return; }

        double newSr     = sigpath::iqFrontEnd.getSampleRate();
        double newCenter = gui::waterfall.getCenterFrequency();
        if (newSr == _this->lastKnownSr && newCenter == _this->lastKnownCenter) { return; }

        // Teardown all active channels and reset — new spectrum, new grid
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

        _this->lastKnownSr       = newSr;
        _this->lastKnownCenter   = newCenter;
        _this->fftBufPos         = 0;
        _this->globalNoiseFloor  = 0.0f;
        _this->displayNoiseFloor = 0.0f;
        _this->slotVotes.clear();
        _this->manualVotes.clear();
        {
            std::lock_guard<std::mutex> lk(_this->manualDetectedMtx);
            _this->manualDetected.clear();
        }
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

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Detection mode (disabled while running)
        bool isAuto   = !_this->manualMode && !_this->scanMode;
        bool isManual = _this->manualMode;
        bool isScan   = _this->scanMode;
        if (ImGui::RadioButton(CONCAT("Auto##_cb_auto_", _this->name), isAuto)) {
            _this->manualMode = false; _this->scanMode = false;
            _this->saveManualConfig(); _this->saveScanConfig();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(CONCAT("Manual##_cb_manual_", _this->name), isManual)) {
            _this->manualMode = true; _this->scanMode = false;
            _this->saveManualConfig(); _this->saveScanConfig();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(CONCAT("Scan##_cb_scan_", _this->name), isScan)) {
            _this->manualMode = false; _this->scanMode = true;
            _this->saveManualConfig(); _this->saveScanConfig();
        }

        if (_this->running) { style::endDisabled(); }

        // Manual frequency list — editable while running
        if (_this->manualMode) {
            ImGui::Spacing();
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

            ImGui::BeginChild(CONCAT("##_cb_manlist_", _this->name), ImVec2(menuWidth, 100), true);
            int toRemove = -1;
            for (int i = 0; i < (int)_this->manualFrequencies.size(); i++) {
                char lbl[64];
                snprintf(lbl, sizeof(lbl), "%.4f MHz", _this->manualFrequencies[i] / 1e6);
                ImGui::Text("%s", lbl);
                ImGui::SameLine(menuWidth - 38);
                char btn[32];
                snprintf(btn, sizeof(btn), "X##_cb_rm_%d", i);
                if (ImGui::SmallButton(btn)) toRemove = i;
            }
            ImGui::EndChild();

            if (toRemove >= 0) {
                { std::lock_guard<std::mutex> lk(_this->manualFreqMtx); _this->manualFrequencies.erase(_this->manualFrequencies.begin() + toRemove); }
                _this->saveManualConfig();
            }

            if (ImGui::Button(CONCAT("Import from Frequency Manager##_cb_importfm_", _this->name), ImVec2(menuWidth, 0))) {
                _this->loadFMConfig();
                _this->fmImportOpen = true;
            }
        }

        if (_this->fmImportOpen) {
            ImGui::OpenPopup(CONCAT("FM Import##_cb_fmpopup_", _this->name));
            _this->fmImportOpen = false;
        }
        if (ImGui::BeginPopupModal(CONCAT("FM Import##_cb_fmpopup_", _this->name), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Select a list to import:");
            ImGui::Spacing();
            if (_this->fmLists.empty()) {
                ImGui::TextDisabled("No lists found in frequency_manager_config.json");
            } else {
                for (auto& [listName, freqs] : _this->fmLists) {
                    char lbl[256];
                    snprintf(lbl, sizeof(lbl), "%s  (%d entries)", listName.c_str(), (int)freqs.size());
                    if (ImGui::Button(lbl)) {
                        { std::lock_guard<std::mutex> lk(_this->manualFreqMtx); for (double f : freqs) _this->manualFrequencies.push_back(f); }
                        _this->saveManualConfig();
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            ImGui::Spacing();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
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
        if (ImGui::SliderInt(CONCAT("##_cb_mintx_", _this->name),
                             &_this->minTransmissionMs, 0, 1000, "%d ms")) {
            config.acquire();
            config.conf[_this->name]["minTransmissionMs"] = _this->minTransmissionMs;
            config.release(true);
        }

        // Tail length — how long to keep recording after signal gone
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
            // Shows the description of the currently monitored frequency.
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
                ImGui::BeginChild(CONCAT("##_cb_nowplaying_", _this->name),
                                  ImVec2(menuWidth, 50), true);
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
        config.conf[name]["manualMode"] = manualMode;
        auto& arr = config.conf[name]["manualFrequencies"];
        arr = nlohmann::json::array();
        for (double f : manualFrequencies) arr.push_back(f);
        config.release(true);
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
        std::set<int> localDetected;
        {
            std::lock_guard<std::mutex> lk(manualDetectedMtx);
            localDetected = manualDetected;
        }
        std::vector<double> localFreqs;
        {
            std::lock_guard<std::mutex> lk(manualFreqMtx);
            localFreqs = manualFrequencies;
        }

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
            it->second->signalPresent = present;
            if (present) it->second->lastDetected = now;
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
    float        recGain       = 0.25f;     // linear gain applied before WAV write (~-12dB)
    int          minTransmissionMs = 300;   // discard recordings shorter than this
    int          tailMs            = 500;   // ms to keep recording after signal gone
    int          maxChannels   = 16;
    double       channelSpacing = 25000.0;
    float        bwUsage       = 0.8f;      // fraction of SDR bandwidth to use (avoids filter rolloff edges)

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

    // Manual mode
    bool manualMode = false;
    std::mutex manualFreqMtx;
    std::vector<double> manualFrequencies;
    char manualFreqInputBuf[64] = {};
    std::map<int, int> manualVotes;       // list-index → vote count (DSP thread only)
    std::set<int> manualDetected;         // written by DSP, read by mgmt thread
    std::mutex manualDetectedMtx;

    // Description editor (UI thread only)
    int64_t descEditKey = 0;
    char    descEditBuf[256] = {};
    bool    descEditRequest = false;

    // FM import UI state (UI thread only)
    bool fmImportOpen = false;
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
    float                                   globalNoiseFloor  = 0.0f; // 20th-pct noise floor (linear)
    float                                   displayNoiseFloor = 0.0f; // smoothed copy for display only
    std::map<int, int>                      slotVotes;

    // Detected signals (written by DSP thread, read by mgmt thread)
    std::mutex              detectedMtx;
    std::set<int>           detectedSlots;
    std::map<int, double>   slotPeakOffsets;  // Hz from SDR center of peak energy bin per slot

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

    // Playback queue + monitor output
    std::deque<std::pair<std::string,double>> playbackQueue;
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
