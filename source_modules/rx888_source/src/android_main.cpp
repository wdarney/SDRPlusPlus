#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/smgui.h>
#include <gui/widgets/stepped_slider.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <utils/flog.h>
#include <android_backend.h>
#include "../sddc_core/RadioHandler.h"
#include "../sddc_core/thread_names.h"
#include "../sddc_core/arch/android/FX3handler_android.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "rx888_source",
    /* Description:     */ "RX888 MkII Source Module",
    /* Author:          */ "SDR++ Community",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

struct RX888SourceControlV1 {
    char request[4096];
    char response[32768];
    bool ok = false;
};

class RX888SourceModule : public ModuleManager::Instance {
public:
    RX888SourceModule(std::string name) {
        this->name = name;

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &stream;

        refresh();
        loadConfig();
        sigpath::sourceManager.registerSource("RX888", &handler);
        core::modComManager.registerInterface(
            "rx888_source", "rx888_source.control.v1", controlHandler, this);
    }

    ~RX888SourceModule() {
        stop(this);
        core::modComManager.unregisterInterface("rx888_source.control.v1");
        sigpath::sourceManager.unregisterSource("RX888");
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    static constexpr double SAMPLE_RATES[] = { 32e6, 16e6, 8e6, 4e6, 2e6 };
    static constexpr int SAMPLE_RATE_COUNT = sizeof(SAMPLE_RATES) / sizeof(SAMPLE_RATES[0]);

    void refresh() {
        int vid = -1;
        int pid = -1;
        devFd = backend::getDeviceFD(vid, pid, backend::SDDC_VIDPIDS);
        devVid = vid;
        devPid = pid;
        rx888_android_set_usb_fd(devFd, devVid, devPid);

        txtDevList.clear();
        if (devFd >= 0 && devPid == 0x00f1) {
            txtDevList = "RX888 Android\0";
            devId = 0;
        }
        else if (devFd >= 0 && devPid == 0x00f3) {
            txtDevList = "RX888 Bootloader\0";
            devId = -1;
        }
        else {
            devId = -1;
        }
    }

    void loadConfig() {
        buildSrText();
        config.acquire();
        auto& c = config.conf["devices"]["android-rx888"];
        if (c.contains("mode")) { mode = c["mode"].get<std::string>(); }
        if (c.contains("sampleRate")) { selectSampleRate(c["sampleRate"].get<double>()); }
        else { selectSampleRate(4e6); }
        if (c.contains("adcFreq")) { adcFreq = c["adcFreq"].get<double>(); }
        if (c.contains("rfGain")) { rfGain = c["rfGain"].get<float>(); }
        if (c.contains("ifGain")) { ifGain = c["ifGain"].get<float>(); }
        if (c.contains("r2iqWorkers")) { r2iqWorkers = std::clamp(c["r2iqWorkers"].get<int>(), 1, 4); }
        if (c.contains("telemetryIntervalSec")) { telemetryIntervalSec = std::clamp(c["telemetryIntervalSec"].get<int>(), 1, 5); }
        if (c.contains("biasTeeHF")) { biasTeeHF = c["biasTeeHF"].get<bool>(); }
        if (c.contains("biasTeeVHF")) { biasTeeVHF = c["biasTeeVHF"].get<bool>(); }
        if (c.contains("dithering")) { dithering = c["dithering"].get<bool>(); }
        config.release();
    }

    void saveConfig(const char* reason = "unspecified") {
        config.acquire();
        auto& c = config.conf["devices"]["android-rx888"];
        c["mode"] = mode;
        c["sampleRate"] = sampleRate;
        c["adcFreq"] = adcFreq;
        c["rfGain"] = rfGain;
        c["ifGain"] = ifGain;
        c["r2iqWorkers"] = r2iqWorkers;
        c["telemetryIntervalSec"] = telemetryIntervalSec.load();
        c["biasTeeHF"] = biasTeeHF;
        c["biasTeeVHF"] = biasTeeVHF;
        c["dithering"] = dithering;
        config.release(true);
        flog::info("RX888 config save [{}]: mode={} sampleRate={} MHz srId={} visible={} adc={} MHz",
                   reason, mode, sampleRate / 1e6, srId, srVisibleId, adcFreq / 1e6);
    }

    void buildSrText() {
        txtSrList.clear();
        for (double sr : SAMPLE_RATES) {
            if (mode == "VHF" && sr > 8e6) { continue; }
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0f MHz", sr / 1e6);
            txtSrList += std::string(buf) + '\0';
        }
    }

    int visibleToRealIdx(int visibleIdx) {
        int visible = 0;
        for (int i = 0; i < SAMPLE_RATE_COUNT; i++) {
            if (mode == "VHF" && SAMPLE_RATES[i] > 8e6) { continue; }
            if (visible == visibleIdx) { return i; }
            visible++;
        }
        return SAMPLE_RATE_COUNT - 1;
    }

    int realToVisibleIdx(int realIdx) {
        int visible = 0;
        for (int i = 0; i < SAMPLE_RATE_COUNT; i++) {
            if (mode == "VHF" && SAMPLE_RATES[i] > 8e6) { continue; }
            if (i == realIdx) { return visible; }
            visible++;
        }
        return 0;
    }

    void selectSampleRate(double sr) {
        int best = 0;
        double bestDiff = std::abs(SAMPLE_RATES[0] - sr);
        for (int i = 1; i < SAMPLE_RATE_COUNT; i++) {
            double diff = std::abs(SAMPLE_RATES[i] - sr);
            if (diff < bestDiff) {
                bestDiff = diff;
                best = i;
            }
        }
        srId = best;
        srVisibleId = realToVisibleIdx(srId);
        sampleRate = SAMPLE_RATES[srId];
        core::setInputSampleRate(sampleRate);
        flog::info("RX888 sample rate select: requested={} MHz selected={} MHz srId={} visible={} mode={}",
                   sr / 1e6, sampleRate / 1e6, srId, srVisibleId, mode);
    }

    int sampleRateIndex() const {
        double ratio = adcFreq / sampleRate;
        int decimation = (int)std::round(std::log2(std::max(2.0, ratio))) - 1;
        decimation = std::clamp(decimation, 0, 5);
        int selectorBase = (adcFreq > N2_BANDSWITCH) ? 5 : 4;
        return std::clamp(selectorBase - decimation, 0, 5);
    }

    int decimationIndex() const {
        double ratio = adcFreq / sampleRate;
        int decimation = (int)std::round(std::log2(std::max(2.0, ratio))) - 1;
        return std::clamp(decimation, 0, 5);
    }

    void updateGainRanges() {
        if (!radioReady) { return; }
        const float* rfSteps = nullptr;
        const float* ifSteps = nullptr;
        rfGainCount = radio.GetRFAttSteps(&rfSteps);
        ifGainCount = radio.GetIFGainSteps(&ifSteps);
        if (rfSteps && rfGainCount > 0) {
            rfGain = std::clamp(rfGain, rfSteps[0], rfSteps[rfGainCount - 1]);
        }
        if (ifSteps && ifGainCount > 0) {
            ifGain = std::clamp(ifGain, ifSteps[0], ifSteps[ifGainCount - 1]);
        }
        flog::info("RX888: gain ranges RF {}..{} dB ({} steps), IF {}..{} dB ({} steps)",
                   rfSteps && rfGainCount > 0 ? rfSteps[0] : 0.0f,
                   rfSteps && rfGainCount > 0 ? rfSteps[rfGainCount - 1] : 0.0f,
                   rfGainCount,
                   ifSteps && ifGainCount > 0 ? ifSteps[0] : 0.0f,
                   ifSteps && ifGainCount > 0 ? ifSteps[ifGainCount - 1] : 0.0f,
                   ifGainCount);
    }

    int nearestGainStep(bool rf, float gainDb) {
        if (!radioReady) { return 0; }
        const float* steps = nullptr;
        int count = rf ? radio.GetRFAttSteps(&steps) : radio.GetIFGainSteps(&steps);
        if (!steps || count <= 0) { return 0; }
        int best = 0;
        float bestDiff = std::abs(steps[0] - gainDb);
        for (int i = 1; i < count; i++) {
            float diff = std::abs(steps[i] - gainDb);
            if (diff < bestDiff) {
                bestDiff = diff;
                best = i;
            }
        }
        return best;
    }

    void applyControls() {
        if (!radioReady) { return; }
        radio.UpdatemodeRF(mode == "VHF" ? VHFMODE : HFMODE);
        updateGainRanges();
        int rfStep = nearestGainStep(true, rfGain);
        int ifStep = nearestGainStep(false, ifGain);
        rfStep = radio.UpdateattRF(rfStep);
        ifStep = radio.UpdateIFGain(ifStep);
        const float* rfSteps = nullptr;
        const float* ifSteps = nullptr;
        int rfCount = radio.GetRFAttSteps(&rfSteps);
        int ifCount = radio.GetIFGainSteps(&ifSteps);
        if (rfSteps && rfCount > 0) { rfGain = rfSteps[std::clamp(rfStep, 0, rfCount - 1)]; }
        if (ifSteps && ifCount > 0) { ifGain = ifSteps[std::clamp(ifStep, 0, ifCount - 1)]; }
        flog::info("RX888: applied gains RF {:.1f} dB step {}, IF {:.1f} dB step {}",
                   rfGain, rfStep, ifGain, ifStep);
        radio.UpdBiasT_HF(biasTeeHF);
        radio.UpdBiasT_VHF(biasTeeVHF);
        radio.UptDither(dithering);
        radio.UptRand(dithering);
    }

    static void writeControlResponse(RX888SourceControlV1* msg, const json& body, bool ok = true) {
        if (!msg) { return; }
        std::string text = body.dump();
        strncpy(msg->response, text.c_str(), sizeof(msg->response) - 1);
        msg->response[sizeof(msg->response) - 1] = '\0';
        msg->ok = ok;
    }

    json gainStateJson(const char* name, const char* label, float value, bool rf) {
        const float* steps = nullptr;
        int count = radioReady ? (rf ? radio.GetRFAttSteps(&steps) : radio.GetIFGainSteps(&steps)) : 0;
        double min = (steps && count > 0) ? steps[0] : value;
        double max = (steps && count > 0) ? steps[count - 1] : value;
        double step = (steps && count > 1) ? std::abs(steps[1] - steps[0]) : 0.1;
        return json({
            {"name", name},
            {"label", label},
            {"value", value},
            {"min", min},
            {"max", max},
            {"step", step > 0.0 ? step : 0.1},
            {"available", steps && count > 0 && min != max},
            {"liveMutable", true}
        });
    }

    json controlStateJson() {
        json devices = json::array();
        if (devFd >= 0) {
            devices.push_back({
                {"id", 0},
                {"label", devPid == 0x00f3 ? "RX888 Bootloader" : "RX888 Android"}
            });
        }

        json rates = json::array();
        int visible = 0;
        for (int i = 0; i < SAMPLE_RATE_COUNT; i++) {
            double sr = SAMPLE_RATES[i];
            if (mode == "VHF" && sr > 8e6) { continue; }
            char label[32];
            snprintf(label, sizeof(label), "%.0f MHz", sr / 1e6);
            rates.push_back({
                {"id", visible++},
                {"value", sr},
                {"label", label},
                {"selected", i == srId}
            });
        }

        json gains = json::array();
        gains.push_back(gainStateJson("RF", "RF Gain", rfGain, true));
        gains.push_back(gainStateJson("IF", "IF Gain", ifGain, false));

        return json({
            {"available", true},
            {"source", "RX888"},
            {"running", running},
            {"cleanupBusy", false},
            {"deviceId", devId},
            {"devices", devices},
            {"sampleRate", sampleRate},
            {"sampleRates", rates},
            {"supportsAdcFreq", true},
            {"adcClockMHz", adcFreq / 1e6},
            {"adcMinMHz", 50.0},
            {"adcMaxMHz", 140.0},
            {"mode", mode},
            {"modes", json::array({"HF", "VHF"})},
            {"gains", gains},
            {"supportsNewBiasTee", true},
            {"supportsBiasTee", true},
            {"biasTeeHF", biasTeeHF},
            {"biasTeeVHF", biasTeeVHF},
            {"biasTeeLiveMutable", true},
            {"supportsDithering", true},
            {"dithering", dithering},
            {"ditheringLiveMutable", true},
            {"telemetryIntervalSec", telemetryIntervalSec.load()},
            {"telemetrySpeeds", json::array({
                json({{"label", "Fast"}, {"intervalSec", 1}, {"selected", telemetryIntervalSec.load() == 1}}),
                json({{"label", "Slow"}, {"intervalSec", 5}, {"selected", telemetryIntervalSec.load() != 1}})
            })},
            {"telemetryLiveMutable", true}
        });
    }

    bool applyControlJson(const json& req, std::string& error) {
        bool stoppedOnlyChanged = req.contains("deviceLabel") || req.contains("deviceId") ||
                                  req.contains("sampleRate") || req.contains("sampleRateId") ||
                                  req.contains("adcClockMHz") || req.contains("mode") ||
                                  req.contains("r2iqWorkers") ||
                                  req.value("refresh", false);
        if (running && stoppedOnlyChanged) {
            error = "stop SDR before changing device, sample rate, ADC clock, mode, or refresh";
            return false;
        }

        if (req.value("refresh", false)) {
            refresh();
        }

        if (req.contains("deviceId")) {
            int id = req["deviceId"].get<int>();
            if (id != 0 || devFd < 0) {
                error = "device not found";
                return false;
            }
            devId = devPid == 0x00f1 ? 0 : -1;
        }

        if (req.contains("mode")) {
            std::string newMode = req["mode"].get<std::string>();
            if (newMode != "HF" && newMode != "VHF") {
                error = "mode not available";
                return false;
            }
            mode = newMode;
            if (mode == "VHF" && sampleRate > 8e6) { selectSampleRate(8e6); }
            buildSrText();
            srVisibleId = realToVisibleIdx(srId);
        }

        if (req.contains("adcClockMHz")) {
            double mhz = req["adcClockMHz"].get<double>();
            if (!std::isfinite(mhz) || mhz < 50.0 || mhz > 140.0) {
                error = "ADC clock out of range";
                return false;
            }
            adcFreq = mhz * 1e6;
        }

        if (req.contains("sampleRateId")) {
            int visibleId = req["sampleRateId"].get<int>();
            int realId = visibleToRealIdx(visibleId);
            if (visibleId < 0 || realId < 0 || realId >= SAMPLE_RATE_COUNT || realToVisibleIdx(realId) != visibleId) {
                error = "sample rate not found";
                return false;
            }
            srVisibleId = visibleId;
            srId = realId;
            sampleRate = SAMPLE_RATES[srId];
            core::setInputSampleRate(sampleRate);
        }
        else if (req.contains("sampleRate")) {
            selectSampleRate(req["sampleRate"].get<double>());
        }

        if (req.contains("r2iqWorkers")) {
            int workers = req["r2iqWorkers"].get<int>();
            if (workers < 1 || workers > 4) {
                error = "R2IQ worker count out of range";
                return false;
            }
            r2iqWorkers = workers;
        }

        if (req.contains("telemetryIntervalSec")) {
            int intervalSec = req["telemetryIntervalSec"].get<int>();
            if (intervalSec != 1 && intervalSec != 5) {
                error = "telemetry interval must be 1 or 5 seconds";
                return false;
            }
            telemetryIntervalSec = intervalSec;
        }

        if (req.contains("gains")) {
            for (auto& [key, val] : req["gains"].items()) {
                float gain = val.get<float>();
                if (key == "RF") {
                    rfGain = gain;
                    if (radioReady) {
                        int step = nearestGainStep(true, rfGain);
                        const float* steps = nullptr;
                        int count = radio.GetRFAttSteps(&steps);
                        if (steps && count > 0) { rfGain = steps[step]; }
                        if (running) { radio.UpdateattRF(step); }
                    }
                }
                else if (key == "IF") {
                    ifGain = gain;
                    if (radioReady) {
                        int step = nearestGainStep(false, ifGain);
                        const float* steps = nullptr;
                        int count = radio.GetIFGainSteps(&steps);
                        if (steps && count > 0) { ifGain = steps[step]; }
                        if (running) { radio.UpdateIFGain(step); }
                    }
                }
                else {
                    error = "gain not found: " + key;
                    return false;
                }
            }
        }

        if (req.contains("biasTeeHF")) {
            biasTeeHF = req["biasTeeHF"].get<bool>();
            if (radioReady) { radio.UpdBiasT_HF(biasTeeHF); }
        }
        if (req.contains("biasTeeVHF")) {
            biasTeeVHF = req["biasTeeVHF"].get<bool>();
            if (radioReady) { radio.UpdBiasT_VHF(biasTeeVHF); }
        }
        if (req.contains("dithering")) {
            dithering = req["dithering"].get<bool>();
            if (radioReady) {
                radio.UptDither(dithering);
                radio.UptRand(dithering);
            }
        }

        saveConfig("web-control");
        return true;
    }

    enum ControlCode {
        CONTROL_GET = 1,
        CONTROL_SET = 2
    };

    static void controlHandler(int code, void* in, void* out, void* ctx) {
        auto* _this = (RX888SourceModule*)ctx;
        auto* inMsg = (RX888SourceControlV1*)in;
        auto* outMsg = (RX888SourceControlV1*)(out ? out : in);
        if (!_this || !outMsg) { return; }

        try {
            if (code == CONTROL_SET) {
                json req = json::object();
                if (inMsg && inMsg->request[0]) { req = json::parse(inMsg->request); }
                std::string error;
                if (!_this->applyControlJson(req, error)) {
                    writeControlResponse(outMsg, json({{"ok", false}, {"error", error}}), false);
                    return;
                }
            }
            json state = _this->controlStateJson();
            state["ok"] = true;
            writeControlResponse(outMsg, state, true);
        }
        catch (const std::exception& e) {
            writeControlResponse(outMsg, json({{"ok", false}, {"error", e.what()}}), false);
        }
        catch (...) {
            writeControlResponse(outMsg, json({{"ok", false}, {"error", "RX888 control failed"}}), false);
        }
    }

    bool tuneHardware(double requestedFreq) {
        if (!radioReady) { return false; }
        if (!std::isfinite(requestedFreq) || requestedFreq < 10000.0 || requestedFreq > 1750e6) {
            flog::error("RX888: refusing invalid hardware tune frequency {} Hz; check SDR++ source offset", requestedFreq);
            return false;
        }
        rf_mode prepared = radio.PrepareLo((uint64_t)requestedFreq);
        if (prepared == NOMODE) {
            flog::error("RX888: frequency {} Hz is outside the RX888 hardware range", requestedFreq);
            return false;
        }
        rf_mode selected = mode == "VHF" ? VHFMODE : HFMODE;
        if (prepared != selected) {
            flog::error("RX888: refusing {} Hz because it needs {} mode but {} mode is selected; check SDR++ source offset",
                        requestedFreq, modeName(prepared), modeName(selected));
            return false;
        }
        radio.TuneLO((uint64_t)requestedFreq);
        return true;
    }

    static const char* modeName(rf_mode mode) {
        switch (mode) {
        case HFMODE: return "HF";
        case VHFMODE: return "VHF";
        default: return "unknown";
        }
    }

    static void onSamples(void* ctx, const float* data, uint32_t count) {
        auto* _this = (RX888SourceModule*)ctx;
        if (!_this->running) { return; }
        if (!data) { return; }
        if (count > STREAM_BUFFER_SIZE) {
            _this->oversizedDrops++;
            flog::error("RX888: dropping oversized DSP block: {} samples", count);
            return;
        }
        _this->callbackBlocks++;
        _this->callbackSamples += count;
        memcpy(_this->stream.writeBuf, data, count * sizeof(dsp::complex_t));
        auto swapStart = std::chrono::steady_clock::now();
        if (!_this->stream.swap(count)) {
            _this->streamSwapStops++;
            _this->running = false;
        }
        auto swapEnd = std::chrono::steady_clock::now();
        _this->streamSwapWaitNs += std::chrono::duration_cast<std::chrono::nanoseconds>(swapEnd - swapStart).count();
    }

    void diagnosticsLoop() {
        rx888_set_thread_name("rx888-diag");

        using namespace std::chrono_literals;

        uint64_t lastUsbBytes = rx888_android_get_usb_bytes();
        uint64_t lastUsbTransfers = rx888_android_get_usb_transfers();
        uint64_t lastUsbErrors = rx888_android_get_usb_errors();
        uint64_t lastBlocks = callbackBlocks.load();
        uint64_t lastSamples = callbackSamples.load();
        uint64_t lastSwapWaitNs = streamSwapWaitNs.load();
        int lastInputFull = radio.getInputFullCount();
        int lastInputEmpty = radio.getInputEmptyCount();
        int lastOutputFull = radio.getOutputFullCount();
        int lastOutputEmpty = radio.getOutputEmptyCount();
        R2iqTimingSnapshot lastTiming = radio.getR2iqTimingSnapshot();

        while (running) {
            int intervalSec = telemetryIntervalSec.load();
            std::this_thread::sleep_for(std::chrono::seconds(intervalSec));
            if (!running) { break; }
            double intervalScale = 1.0 / (double)intervalSec;

            uint64_t usbBytes = rx888_android_get_usb_bytes();
            uint64_t usbTransfers = rx888_android_get_usb_transfers();
            uint64_t usbErrors = rx888_android_get_usb_errors();
            uint64_t blocks = callbackBlocks.load();
            uint64_t samples = callbackSamples.load();
            uint64_t swapWaitNs = streamSwapWaitNs.load();
            int inputFull = radio.getInputFullCount();
            int inputEmpty = radio.getInputEmptyCount();
            int outputFull = radio.getOutputFullCount();
            int outputEmpty = radio.getOutputEmptyCount();
            R2iqTimingSnapshot timing = radio.getR2iqTimingSnapshot();

            double usbMBps = (double)(usbBytes - lastUsbBytes) / (1024.0 * 1024.0) * intervalScale;
            double dspMSps = (double)(samples - lastSamples) / 1000000.0 * intervalScale;
            double swapWaitMs = (double)(swapWaitNs - lastSwapWaitNs) / 1000000.0 * intervalScale;
            uint64_t timingChunks = timing.chunks - lastTiming.chunks;
            uint64_t forwardNs = timing.forwardNs - lastTiming.forwardNs;
            uint64_t shiftNs = timing.shiftNs - lastTiming.shiftNs;
            uint64_t inverseNs = timing.inverseNs - lastTiming.inverseNs;
            uint64_t copyNs = timing.copyNs - lastTiming.copyNs;
            uint64_t syncNs = timing.syncNs - lastTiming.syncNs;
            uint64_t totalNs = forwardNs + shiftNs + inverseNs + copyNs + syncNs;
            auto pct = [totalNs](uint64_t ns) -> double {
                return totalNs ? (double)ns * 100.0 / (double)totalNs : 0.0;
            };
            auto ms = [](uint64_t ns) -> double {
                return (double)ns / 1000000.0;
            };

            uint64_t deltaUsbTransfers = usbTransfers - lastUsbTransfers;
            uint64_t deltaBlocks = blocks - lastBlocks;
            uint64_t deltaUsbErrors = usbErrors - lastUsbErrors;
            int deltaInputFull = inputFull - lastInputFull;
            int deltaInputEmpty = inputEmpty - lastInputEmpty;
            int deltaOutputFull = outputFull - lastOutputFull;
            int deltaOutputEmpty = outputEmpty - lastOutputEmpty;
            {
                uint64_t usbTransfersPerSec = (uint64_t)((double)deltaUsbTransfers * intervalScale);
                uint64_t blocksPerSec = (uint64_t)((double)deltaBlocks * intervalScale);
                uint64_t timingChunksPerSec = (uint64_t)((double)timingChunks * intervalScale);
                char buf[512];
                snprintf(buf, sizeof(buf),
                         "Diag USB %.1f MiB/s  DSP %.2f MS/s\nXfer/s %llu  blocks/s %llu  swap %.0f ms/s\nR2IQ chunks/s %llu  fwd %.0fms %.0f%%  inv %.0fms %.0f%%\nshift %.0fms %.0f%%  copy %.0fms %.0f%%  sync %.0fms %.0f%%\nUSB err %llu (+%llu)  oversize %llu\nIn full %d (+%d)  empty %d (+%d)\nOut full %d (+%d)  empty %d (+%d)  stops %llu",
                         usbMBps, dspMSps,
                         (unsigned long long)usbTransfersPerSec,
                         (unsigned long long)blocksPerSec,
                         swapWaitMs,
                         (unsigned long long)timingChunksPerSec,
                         ms(forwardNs) * intervalScale, pct(forwardNs),
                         ms(inverseNs) * intervalScale, pct(inverseNs),
                         ms(shiftNs) * intervalScale, pct(shiftNs),
                         ms(copyNs) * intervalScale, pct(copyNs),
                         ms(syncNs) * intervalScale, pct(syncNs),
                         (unsigned long long)usbErrors,
                         (unsigned long long)deltaUsbErrors,
                         (unsigned long long)oversizedDrops.load(),
                         inputFull, deltaInputFull,
                         inputEmpty, deltaInputEmpty,
                         outputFull, deltaOutputFull,
                         outputEmpty, deltaOutputEmpty,
                         (unsigned long long)streamSwapStops.load());
                std::lock_guard<std::mutex> lck(diagTextMtx);
                diagText = buf;
            }
            char logBuf[640];
            uint64_t usbTransfersPerSec = (uint64_t)((double)deltaUsbTransfers * intervalScale);
            uint64_t blocksPerSec = (uint64_t)((double)deltaBlocks * intervalScale);
            uint64_t timingChunksPerSec = (uint64_t)((double)timingChunks * intervalScale);
            snprintf(logBuf, sizeof(logBuf),
                     "usb=%.1fMiB/s dsp=%.2fMS/s xfers/s=%llu blocks/s=%llu swap=%.0fms/s r2iqChunks/s=%llu fwd=%.0fms %.0f%% inv=%.0fms %.0f%% shift=%.0fms %.0f%% copy=%.0fms %.0f%% sync=%.0fms %.0f%% usbErr=%llu (+%llu) oversize=%llu inFull=%d (+%d) inEmpty=%d (+%d) outFull=%d (+%d) outEmpty=%d (+%d) stops=%llu",
                     usbMBps, dspMSps,
                     (unsigned long long)usbTransfersPerSec,
                     (unsigned long long)blocksPerSec,
                     swapWaitMs,
                     (unsigned long long)timingChunksPerSec,
                     ms(forwardNs) * intervalScale, pct(forwardNs),
                     ms(inverseNs) * intervalScale, pct(inverseNs),
                     ms(shiftNs) * intervalScale, pct(shiftNs),
                     ms(copyNs) * intervalScale, pct(copyNs),
                     ms(syncNs) * intervalScale, pct(syncNs),
                     (unsigned long long)usbErrors,
                     (unsigned long long)deltaUsbErrors,
                     (unsigned long long)oversizedDrops.load(),
                     inputFull, deltaInputFull,
                     inputEmpty, deltaInputEmpty,
                     outputFull, deltaOutputFull,
                     outputEmpty, deltaOutputEmpty,
                     (unsigned long long)streamSwapStops.load());
            flog::info("RX888 diag: {}", logBuf);

            lastUsbBytes = usbBytes;
            lastUsbTransfers = usbTransfers;
            lastUsbErrors = usbErrors;
            lastBlocks = blocks;
            lastSamples = samples;
            lastSwapWaitNs = swapWaitNs;
            lastInputFull = inputFull;
            lastInputEmpty = inputEmpty;
            lastOutputFull = outputFull;
            lastOutputEmpty = outputEmpty;
            lastTiming = timing;
        }
    }

    static void menuSelected(void* ctx) {
        auto* _this = (RX888SourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
    }

    static void menuDeselected(void*) {}

    static void start(void* ctx) {
        auto* _this = (RX888SourceModule*)ctx;
        if (_this->running) { return; }
        _this->refresh();
        if (_this->devFd >= 0 && _this->devPid == 0x00f3) {
            flog::info("RX888: bootloader device found; uploading FX3 firmware");
            rx888_android_set_usb_fd(_this->devFd, _this->devVid, _this->devPid);
            if (!rx888_android_upload_firmware()) {
                flog::error("RX888: firmware upload failed");
                return;
            }
            flog::info("RX888: firmware uploaded; accept the new Android USB permission prompt, then refresh/start again");
            return;
        }
        if (_this->devFd < 0 || _this->devPid != 0x00f1) {
            flog::error("RX888: runtime device is not available; refresh after Android grants USB permission");
            return;
        }

        flog::info("RX888: opening runtime USB device pid=0x{:04x} fd={}", _this->devPid, _this->devFd);
        rx888_android_set_usb_fd(_this->devFd, _this->devVid, _this->devPid);
        _this->fx3 = CreateUsbHandler();
        if (!_this->fx3 || !_this->fx3->Open()) {
            flog::error("RX888: failed to open Android USB device");
            delete _this->fx3;
            _this->fx3 = nullptr;
            return;
        }

        flog::info("RX888: initializing RadioHandler");
        adcnominalfreq = (uint32_t)_this->adcFreq;
        _this->radio.SetR2iqWorkerCount(_this->r2iqWorkers);
        _this->radioReady = _this->radio.Init(_this->fx3, onSamples, nullptr, _this);
        if (!_this->radioReady) {
            flog::error("RX888: RadioHandler init failed");
            delete _this->fx3;
            _this->fx3 = nullptr;
            return;
        }

        flog::info("RX888: applying controls and tuning");
        _this->applyControls();
        if (!_this->tuneHardware(_this->freq)) {
            _this->radio.Close();
            _this->radioReady = false;
            delete _this->fx3;
            _this->fx3 = nullptr;
            return;
        }

        _this->running = true;
        _this->callbackBlocks = 0;
        _this->callbackSamples = 0;
        _this->oversizedDrops = 0;
        _this->streamSwapStops = 0;
        _this->streamSwapWaitNs = 0;
        _this->radio.resetR2iqTiming();
        _this->activeSampleRate = _this->sampleRate;
        _this->activeSelector = _this->sampleRateIndex();
        _this->activeDecimation = _this->decimationIndex();
        flog::info("RX888: starting stream selector={} decimation={} expected sample rate {} MHz r2iqWorkers={}",
                   _this->activeSelector, _this->activeDecimation, _this->activeSampleRate / 1e6, _this->r2iqWorkers);
        _this->radio.Start(_this->activeSelector);
        _this->diagnosticThread = std::thread(&RX888SourceModule::diagnosticsLoop, _this);
        flog::info("RX888: Started direct Android core ({}, {} MHz, ADC {} MHz)",
                   _this->mode, _this->sampleRate / 1e6, _this->adcFreq / 1e6);
    }

    static void stop(void* ctx) {
        auto* _this = (RX888SourceModule*)ctx;
        if (!_this->running && !_this->radioReady && !_this->fx3) { return; }
        _this->running = false;
        _this->stream.stopWriter();
        if (_this->diagnosticThread.joinable()) {
            _this->diagnosticThread.join();
        }
        if (_this->radioReady) {
            _this->radio.Stop();
            _this->radio.Close();
            _this->radioReady = false;
        }
        _this->stream.clearWriteStop();
        delete _this->fx3;
        _this->fx3 = nullptr;
        {
            std::lock_guard<std::mutex> lck(_this->diagTextMtx);
            _this->diagText = "Diag idle";
        }
        flog::info("RX888: Stopped");
    }

    static void tune(double freq, void* ctx) {
        auto* _this = (RX888SourceModule*)ctx;
        _this->freq = freq;
        if (_this->running && _this->radioReady) {
            _this->tuneHardware(freq);
        }
    }

    static void menuHandler(void* ctx) {
        auto* _this = (RX888SourceModule*)ctx;

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (_this->devId >= 0) {
            SmGui::Combo(CONCAT("##rx888_dev_", _this->name), &_this->devId, _this->txtDevList.c_str());
        }
        if (SmGui::Button(CONCAT("Refresh##rx888_refr_", _this->name))) {
            _this->refresh();
        }

        if (_this->running) { SmGui::BeginDisabled(); }

        SmGui::LeftLabel("Sample Rate");
        if (_this->mode == "VHF") {
            ImGui::Text("%.0f MHz", _this->sampleRate / 1e6);
            if (ImGui::Button(CONCAT("8 MHz##rx888_sr_btn8_", _this->name))) {
                _this->selectSampleRate(8e6);
                core::setInputSampleRate(_this->sampleRate);
                _this->saveConfig("quick-8mhz");
            }
            ImGui::SameLine();
            if (ImGui::Button(CONCAT("4 MHz##rx888_sr_btn4_", _this->name))) {
                _this->selectSampleRate(4e6);
                core::setInputSampleRate(_this->sampleRate);
                _this->saveConfig("quick-4mhz");
            }
            ImGui::SameLine();
            if (ImGui::Button(CONCAT("2 MHz##rx888_sr_btn2_", _this->name))) {
                _this->selectSampleRate(2e6);
                core::setInputSampleRate(_this->sampleRate);
                _this->saveConfig("quick-2mhz");
            }
        }
        else {
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT("##rx888_sr_", _this->name), &_this->srVisibleId, _this->txtSrList.c_str())) {
                _this->srId = _this->visibleToRealIdx(_this->srVisibleId);
                _this->sampleRate = SAMPLE_RATES[_this->srId];
                core::setInputSampleRate(_this->sampleRate);
                _this->saveConfig("sample-rate-combo");
            }
        }

        SmGui::LeftLabel("ADC Clock");
        SmGui::FillWidth();
        float adcMHz = (float)(_this->adcFreq / 1e6);
        if (SmGui::SliderFloat(CONCAT("##rx888_adc_", _this->name), &adcMHz, 50.0f, 140.0f, SmGui::FMT_STR_FLOAT_NO_DECIMAL)) {
            _this->adcFreq = adcMHz * 1e6;
            _this->saveConfig("adc-clock");
        }

        SmGui::LeftLabel("Mode");
        if (SmGui::RadioButton(CONCAT("HF##rx888_mode_", _this->name), _this->mode == "HF")) {
            _this->mode = "HF";
            _this->buildSrText();
            _this->srVisibleId = _this->realToVisibleIdx(_this->srId);
            _this->saveConfig("mode-hf");
        }
        SmGui::SameLine();
        if (SmGui::RadioButton(CONCAT("VHF##rx888_mode_", _this->name), _this->mode == "VHF")) {
            _this->mode = "VHF";
            if (_this->sampleRate > 8e6) { _this->selectSampleRate(8e6); }
            _this->buildSrText();
            _this->saveConfig("mode-vhf");
        }

        if (_this->running) { SmGui::EndDisabled(); }

        if (_this->running) { SmGui::BeginDisabled(); }
        SmGui::LeftLabel("R2IQ Workers");
        int workers = _this->r2iqWorkers;
        SmGui::FillWidth();
        if (ImGui::SliderInt(CONCAT("##rx888_r2iq_workers_", _this->name), &workers, 1, 4)) {
            _this->r2iqWorkers = std::clamp(workers, 1, 4);
            _this->saveConfig("r2iq-workers");
        }
        if (_this->running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("Telemetry");
        int telemetryIdx = _this->telemetryIntervalSec.load() == 1 ? 0 : 1;
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##rx888_telemetry_", _this->name), &telemetryIdx, "Fast\0Slow\0")) {
            _this->telemetryIntervalSec = telemetryIdx == 0 ? 1 : 5;
            _this->saveConfig("telemetry");
        }

        SmGui::LeftLabel("RF Gain");
        SmGui::FillWidth();
        const float* rfSteps = nullptr;
        if (_this->radioReady) { _this->radio.GetRFAttSteps(&rfSteps); }
        float rfMinDb = (rfSteps && _this->rfGainCount > 0) ? rfSteps[0] : 0.0f;
        float rfMaxDb = (rfSteps && _this->rfGainCount > 0) ? rfSteps[_this->rfGainCount - 1] : 0.0f;
        if (SmGui::SliderFloat(CONCAT("##rx888_rf_gain_", _this->name), &_this->rfGain, rfMinDb, rfMaxDb, SmGui::FMT_STR_FLOAT_DB_ONE_DECIMAL)) {
            if (_this->running) { _this->radio.UpdateattRF(_this->nearestGainStep(true, _this->rfGain)); }
            _this->saveConfig("rf-gain");
        }

        SmGui::LeftLabel("IF Gain");
        SmGui::FillWidth();
        const float* ifSteps = nullptr;
        if (_this->radioReady) { _this->radio.GetIFGainSteps(&ifSteps); }
        float ifMinDb = (ifSteps && _this->ifGainCount > 0) ? ifSteps[0] : 0.0f;
        float ifMaxDb = (ifSteps && _this->ifGainCount > 0) ? ifSteps[_this->ifGainCount - 1] : 0.0f;
        if (SmGui::SliderFloat(CONCAT("##rx888_if_gain_", _this->name), &_this->ifGain, ifMinDb, ifMaxDb, SmGui::FMT_STR_FLOAT_DB_ONE_DECIMAL)) {
            if (_this->running) { _this->radio.UpdateIFGain(_this->nearestGainStep(false, _this->ifGain)); }
            _this->saveConfig("if-gain");
        }

        if (SmGui::Checkbox(CONCAT("HF Bias Tee##rx888_bt_hf_", _this->name), &_this->biasTeeHF)) {
            if (_this->running) { _this->radio.UpdBiasT_HF(_this->biasTeeHF); }
            _this->saveConfig("hf-biastee");
        }
        SmGui::SameLine();
        if (SmGui::Checkbox(CONCAT("VHF Bias Tee##rx888_bt_vhf_", _this->name), &_this->biasTeeVHF)) {
            if (_this->running) { _this->radio.UpdBiasT_VHF(_this->biasTeeVHF); }
            _this->saveConfig("vhf-biastee");
        }

        if (SmGui::Checkbox(CONCAT("Dithering##rx888_dith_", _this->name), &_this->dithering)) {
            if (_this->running) {
                _this->radio.UptDither(_this->dithering);
                _this->radio.UptRand(_this->dithering);
            }
            _this->saveConfig("dithering");
        }

        {
            std::lock_guard<std::mutex> lck(_this->diagTextMtx);
            ImGui::TextUnformatted(_this->diagText.c_str());
        }
        if (_this->running) {
            ImGui::Text("Active %.1f MHz  selector %d  decim %d  workers %d",
                        _this->activeSampleRate / 1e6,
                        _this->activeSelector,
                        _this->activeDecimation,
                        _this->r2iqWorkers);
        }
        else {
            ImGui::TextUnformatted("Active stopped");
        }
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    bool radioReady = false;
    double freq = 100e6;

    int devFd = -1;
    int devVid = -1;
    int devPid = -1;
    int devId = -1;
    std::string txtDevList;

    std::string mode = "HF";
    double sampleRate = 4e6;
    double adcFreq = 128e6;
    int srId = 3;
    int srVisibleId = 0;
    std::string txtSrList;

    float rfGain = 0.0f;
    float ifGain = 0.0f;
    int rfGainCount = 64;
    int ifGainCount = 127;
    int r2iqWorkers = 3;
    std::atomic<int> telemetryIntervalSec{5};
    bool biasTeeHF = false;
    bool biasTeeVHF = false;
    bool dithering = true;

    fx3class* fx3 = nullptr;
    RadioHandlerClass radio;
    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
    std::thread diagnosticThread;
    std::mutex diagTextMtx;
    std::string diagText = "Diag idle";
    std::atomic<uint64_t> callbackBlocks{0};
    std::atomic<uint64_t> callbackSamples{0};
    std::atomic<uint64_t> oversizedDrops{0};
    std::atomic<uint64_t> streamSwapStops{0};
    std::atomic<uint64_t> streamSwapWaitNs{0};
    double activeSampleRate = 0.0;
    int activeSelector = -1;
    int activeDecimation = -1;
};

MOD_EXPORT void _INIT_() {
    json def;
    def["devices"] = json::object();
    rx888_android_set_firmware_path((core::args["root"].s() + "/res/SDDC_FX3.img").c_str());
    config.setPath(core::args["root"].s() + "/rx888_source_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new RX888SourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (RX888SourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
