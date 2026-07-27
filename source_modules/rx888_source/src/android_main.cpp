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
#include "../sddc_core/arch/android/FX3handler_android.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
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
    }

    ~RX888SourceModule() {
        stop(this);
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
        if (c.contains("biasTeeHF")) { biasTeeHF = c["biasTeeHF"].get<bool>(); }
        if (c.contains("biasTeeVHF")) { biasTeeVHF = c["biasTeeVHF"].get<bool>(); }
        if (c.contains("dithering")) { dithering = c["dithering"].get<bool>(); }
        config.release();
    }

    void saveConfig() {
        config.acquire();
        auto& c = config.conf["devices"]["android-rx888"];
        c["mode"] = mode;
        c["sampleRate"] = sampleRate;
        c["adcFreq"] = adcFreq;
        c["rfGain"] = rfGain;
        c["ifGain"] = ifGain;
        c["biasTeeHF"] = biasTeeHF;
        c["biasTeeVHF"] = biasTeeVHF;
        c["dithering"] = dithering;
        config.release(true);
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
        flog::info("RX888: applying gains RF {:.1f} dB step {}, IF {:.1f} dB step {}",
                   rfGain, rfStep, ifGain, ifStep);
        radio.UpdateattRF(rfStep);
        radio.UpdateIFGain(ifStep);
        radio.UpdBiasT_HF(biasTeeHF);
        radio.UpdBiasT_VHF(biasTeeVHF);
        radio.UptDither(dithering);
        radio.UptRand(dithering);
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
            flog::error("RX888: dropping oversized DSP block: {} samples", count);
            return;
        }
        uint32_t cb = ++_this->callbackCount;
        if (cb <= 5 || (cb % 200) == 0) {
            float peak = 0.0f;
            uint32_t limit = std::min<uint32_t>(count, 4096);
            for (uint32_t i = 0; i < limit * 2; i++) {
                peak = std::max(peak, std::abs(data[i]));
            }
            flog::info("RX888: samples block {} count={} peak={:.6f}", cb, count, peak);
        }
        memcpy(_this->stream.writeBuf, data, count * sizeof(dsp::complex_t));
        if (!_this->stream.swap(count)) {
            _this->running = false;
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
        _this->callbackCount = 0;
        _this->activeSampleRate = _this->sampleRate;
        _this->activeSelector = _this->sampleRateIndex();
        _this->activeDecimation = _this->decimationIndex();
        flog::info("RX888: starting stream selector={} decimation={} expected sample rate {} MHz",
                   _this->activeSelector, _this->activeDecimation, _this->activeSampleRate / 1e6);
        _this->radio.Start(_this->activeSelector);
        flog::info("RX888: Started direct Android core ({}, {} MHz, ADC {} MHz)",
                   _this->mode, _this->sampleRate / 1e6, _this->adcFreq / 1e6);
    }

    static void stop(void* ctx) {
        auto* _this = (RX888SourceModule*)ctx;
        if (!_this->running && !_this->radioReady && !_this->fx3) { return; }
        _this->running = false;
        _this->stream.stopWriter();
        if (_this->radioReady) {
            _this->radio.Stop();
            _this->radio.Close();
            _this->radioReady = false;
        }
        _this->stream.clearWriteStop();
        delete _this->fx3;
        _this->fx3 = nullptr;
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
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##rx888_sr_", _this->name), &_this->srVisibleId, _this->txtSrList.c_str())) {
            _this->srId = _this->visibleToRealIdx(_this->srVisibleId);
            _this->sampleRate = SAMPLE_RATES[_this->srId];
            core::setInputSampleRate(_this->sampleRate);
            _this->saveConfig();
        }

        SmGui::LeftLabel("ADC Clock");
        SmGui::FillWidth();
        float adcMHz = (float)(_this->adcFreq / 1e6);
        if (SmGui::SliderFloat(CONCAT("##rx888_adc_", _this->name), &adcMHz, 50.0f, 140.0f, SmGui::FMT_STR_FLOAT_NO_DECIMAL)) {
            _this->adcFreq = adcMHz * 1e6;
            _this->saveConfig();
        }

        SmGui::LeftLabel("Mode");
        if (SmGui::RadioButton(CONCAT("HF##rx888_mode_", _this->name), _this->mode == "HF")) {
            _this->mode = "HF";
            _this->buildSrText();
            _this->srVisibleId = _this->realToVisibleIdx(_this->srId);
            _this->saveConfig();
        }
        SmGui::SameLine();
        if (SmGui::RadioButton(CONCAT("VHF##rx888_mode_", _this->name), _this->mode == "VHF")) {
            _this->mode = "VHF";
            if (_this->sampleRate > 8e6) { _this->selectSampleRate(8e6); }
            _this->buildSrText();
            _this->saveConfig();
        }

        if (_this->running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("RF Gain");
        SmGui::FillWidth();
        const float* rfSteps = nullptr;
        if (_this->radioReady) { _this->radio.GetRFAttSteps(&rfSteps); }
        float rfMinDb = (rfSteps && _this->rfGainCount > 0) ? rfSteps[0] : 0.0f;
        float rfMaxDb = (rfSteps && _this->rfGainCount > 0) ? rfSteps[_this->rfGainCount - 1] : 0.0f;
        if (SmGui::SliderFloat(CONCAT("##rx888_rf_gain_", _this->name), &_this->rfGain, rfMinDb, rfMaxDb, SmGui::FMT_STR_FLOAT_DB_ONE_DECIMAL)) {
            if (_this->running) { _this->radio.UpdateattRF(_this->nearestGainStep(true, _this->rfGain)); }
            _this->saveConfig();
        }

        SmGui::LeftLabel("IF Gain");
        SmGui::FillWidth();
        const float* ifSteps = nullptr;
        if (_this->radioReady) { _this->radio.GetIFGainSteps(&ifSteps); }
        float ifMinDb = (ifSteps && _this->ifGainCount > 0) ? ifSteps[0] : 0.0f;
        float ifMaxDb = (ifSteps && _this->ifGainCount > 0) ? ifSteps[_this->ifGainCount - 1] : 0.0f;
        if (SmGui::SliderFloat(CONCAT("##rx888_if_gain_", _this->name), &_this->ifGain, ifMinDb, ifMaxDb, SmGui::FMT_STR_FLOAT_DB_ONE_DECIMAL)) {
            if (_this->running) { _this->radio.UpdateIFGain(_this->nearestGainStep(false, _this->ifGain)); }
            _this->saveConfig();
        }

        if (SmGui::Checkbox(CONCAT("HF Bias Tee##rx888_bt_hf_", _this->name), &_this->biasTeeHF)) {
            if (_this->running) { _this->radio.UpdBiasT_HF(_this->biasTeeHF); }
            _this->saveConfig();
        }
        SmGui::SameLine();
        if (SmGui::Checkbox(CONCAT("VHF Bias Tee##rx888_bt_vhf_", _this->name), &_this->biasTeeVHF)) {
            if (_this->running) { _this->radio.UpdBiasT_VHF(_this->biasTeeVHF); }
            _this->saveConfig();
        }

        if (SmGui::Checkbox(CONCAT("Dithering##rx888_dith_", _this->name), &_this->dithering)) {
            if (_this->running) {
                _this->radio.UptDither(_this->dithering);
                _this->radio.UptRand(_this->dithering);
            }
            _this->saveConfig();
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
    bool biasTeeHF = false;
    bool biasTeeVHF = false;
    bool dithering = true;

    fx3class* fx3 = nullptr;
    RadioHandlerClass radio;
    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
    std::atomic<uint32_t> callbackCount{0};
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
