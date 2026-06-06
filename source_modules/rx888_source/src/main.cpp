#include <SoapySDR/Device.hpp>
#include <SoapySDR/Modules.hpp>
#include <SoapySDR/Logger.hpp>
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/smgui.h>
#include <gui/widgets/stepped_slider.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <dlfcn.h>
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "rx888_source",
    /* Description:     */ "RX888 MkII Source Module (via SoapySDDC)",
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

        loadSoapySDDC();
        refresh();

        config.acquire();
        std::string devLabel = config.conf["device"];
        config.release();
        selectDevice(devLabel);

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
    // Explicitly load the SoapySDDC module bundled alongside this plugin.
    // Resolves the path relative to this dylib's own location so it works
    // regardless of where the app bundle lives or how it was launched.
    // Falls back silently if the module is already loaded or not found.
    static void loadSoapySDDC() {
#ifdef __APPLE__
        Dl_info info;
        if (!dladdr((void*)&loadSoapySDDC, &info) || !info.dli_fname) return;

        // rx888_source.dylib is at Contents/Plugins/rx888_source.dylib
        // SoapySDDC is at  Contents/SoapySDR/modules0.8/libSDDCSupport.so
        std::string self = info.dli_fname;
        auto pluginsPos = self.rfind("/Plugins/");
        if (pluginsPos == std::string::npos) return;

        std::string sddc = self.substr(0, pluginsPos) +
                           "/SoapySDR/modules0.8/libSDDCSupport.so";

        std::string err = SoapySDR::loadModule(sddc);
        if (!err.empty())
            flog::warn("RX888: SoapySDDC load: {}", err);
        else
            flog::info("RX888: Loaded SoapySDDC from {}", sddc);
#endif
    }

    // Returns a human-readable label for a device entry
    static std::string deviceLabel(const SoapySDR::Kwargs& args) {
        if (args.count("label") && !args.at("label").empty())
            return args.at("label");
        if (args.count("hardware") && !args.at("hardware").empty())
            return args.at("hardware");
        return "RX888";
    }

    void refresh() {
        devList.clear();
        txtDevList = "";
        try {
            devList = SoapySDR::Device::enumerate("driver=SDDC");
        }
        catch (const std::exception& e) {
            flog::error("RX888: enumerate failed: {}", e.what());
            return;
        }
        for (auto& d : devList)
            txtDevList += deviceLabel(d) + '\0';
    }

    void queryCapabilities(SoapySDR::Device* dev, const std::string& queryMode = "HF") {
        gainList = dev->listGains(SOAPY_SDR_RX, 0);
        gainRanges.clear();
        uiGains.clear();
        for (const auto& g : gainList) {
            gainRanges.push_back(dev->getGainRange(SOAPY_SDR_RX, 0, g));
            // Default to 0 dB clamped to the valid range, not minimum (which is too low)
            double def = std::max(gainRanges.back().minimum(),
                         std::min(0.0, gainRanges.back().maximum()));
            uiGains.push_back((float)def);
        }

        auto antennas = dev->listAntennas(SOAPY_SDR_RX, 0);
        hasHF  = std::find(antennas.begin(), antennas.end(), "HF")  != antennas.end();
        hasVHF = std::find(antennas.begin(), antennas.end(), "VHF") != antennas.end();

        // Detect which settings keys this driver version supports
        auto settingInfo = dev->getSettingInfo();
        supportsNewBiasTee = false;
        supportsAdcFreq    = false;
        supportsBiasTee    = false;
        supportsDithering  = false;
        for (const auto& s : settingInfo) {
            if (s.key == "UpdBiasT_HF" || s.key == "UpdBiasT_VHF") supportsNewBiasTee = true;
            if (s.key == "adc_frequency") supportsAdcFreq = true;
            if (s.key == "biastee")       supportsBiasTee = true;
            if (s.key == "dithering")     supportsDithering = true;
        }

        refreshSampleRates(dev);
    }

    void refreshSampleRates(SoapySDR::Device* dev) {
        sampleRates = dev->listSampleRates(SOAPY_SDR_RX, 0);
        buildSrText();
    }

    // Max useful sample rate for VHF mode — R820T2 IF bandwidth is ~8-10 MHz
    static constexpr double VHF_MAX_SR = 8e6;

    void buildSrText() {
        txtSrList = "";
        for (double sr : sampleRates) {
            if (mode == "VHF" && sr > VHF_MAX_SR) continue;
            char buf[32];
            if (sr >= 1e6)
                snprintf(buf, sizeof(buf), "%.0f MHz", sr / 1e6);
            else
                snprintf(buf, sizeof(buf), "%.0f kHz", sr / 1e3);
            txtSrList += std::string(buf) + '\0';
        }
    }

    // Returns the index into sampleRates[] for a given visible combo index,
    // accounting for VHF filtering.
    int visibleToRealIdx(int visibleIdx) {
        int count = 0;
        for (int i = 0; i < (int)sampleRates.size(); i++) {
            if (mode == "VHF" && sampleRates[i] > VHF_MAX_SR) continue;
            if (count == visibleIdx) return i;
            count++;
        }
        return 0;
    }

    int realToVisibleIdx(int realIdx) {
        int count = 0;
        for (int i = 0; i < (int)sampleRates.size(); i++) {
            if (mode == "VHF" && sampleRates[i] > VHF_MAX_SR) continue;
            if (i == realIdx) return count;
            count++;
        }
        return 0;
    }

    // Called in start() after setAntenna() — re-queries gain ranges for the active mode
    // and clamps existing slider values into the new valid range.
    void updateGainRanges() {
        auto newList   = dev->listGains(SOAPY_SDR_RX, 0);
        std::vector<SoapySDR::Range> newRanges;
        for (const auto& g : newList)
            newRanges.push_back(dev->getGainRange(SOAPY_SDR_RX, 0, g));

        if (newList.size() != gainList.size()) {
            gainList   = newList;
            gainRanges = newRanges;
            uiGains.resize(gainList.size(), 0.0f);
        } else {
            gainList   = newList;
            gainRanges = newRanges;
        }

        for (int i = 0; i < (int)gainList.size(); i++) {
            uiGains[i] = std::max((float)gainRanges[i].minimum(),
                         std::min(uiGains[i], (float)gainRanges[i].maximum()));
        }
    }

    // Called when mode radio button changes while device is stopped —
    // spawns a background thread so the firmware-upload open doesn't block the UI.
    void reQueryGainsForMode() {
        if (devList.empty() || devId < 0 || running) return;
        auto args = devList[devId];
        auto m    = mode;
        std::thread([this, args, m]() {
            SoapySDR::Device* d = nullptr;
            try { d = SoapySDR::Device::make(args); }
            catch (const std::exception& e) {
                flog::warn("RX888: reQueryGainsForMode open failed: {}", e.what());
                return;
            }
            try { d->setAntenna(SOAPY_SDR_RX, 0, m); } catch (...) {}
            auto newList = d->listGains(SOAPY_SDR_RX, 0);
            std::vector<SoapySDR::Range> newRanges;
            for (const auto& g : newList)
                newRanges.push_back(d->getGainRange(SOAPY_SDR_RX, 0, g));
            SoapySDR::Device::unmake(d);
            // Only update if mode hasn't changed since we started
            if (m == mode) {
                gainList   = newList;
                gainRanges = newRanges;
                uiGains.resize(gainList.size(), 0.0f);
                for (int i = 0; i < (int)gainList.size(); i++) {
                    uiGains[i] = std::max((float)gainRanges[i].minimum(),
                                 std::min(uiGains[i], (float)gainRanges[i].maximum()));
                }
            }
        }).detach();
    }

    void applyModeRateCap() {
        if (mode == "VHF" && sampleRate > VHF_MAX_SR) {
            selectSampleRate(VHF_MAX_SR);
            saveConfig();
        }
        buildSrText();
        srVisibleId = realToVisibleIdx(srId);
    }

    void selectDevice(const std::string& label) {
        if (devList.empty()) { devId = -1; return; }

        int found = 0;
        for (int i = 0; i < (int)devList.size(); i++) {
            if (deviceLabel(devList[i]) == label) { found = i; break; }
        }
        devId = found;

        SoapySDR::Device* dev = nullptr;
        try {
            dev = SoapySDR::Device::make(devList[devId]);
        }
        catch (const std::exception& e) {
            flog::error("RX888: open failed during select: {}", e.what());
            devId = -1;
            return;
        }

        queryCapabilities(dev, mode);
        SoapySDR::Device::unmake(dev);

        // Load saved config for this device
        config.acquire();
        auto& dc = config.conf["devices"];
        if (dc.contains(label)) {
            auto& c = dc[label];
            if (c.contains("mode"))       mode       = c["mode"].get<std::string>();
            if (c.contains("adcFreq"))    adcFreq    = c["adcFreq"].get<double>();
            if (c.contains("biasTeeHF"))  biasTeeHF  = c["biasTeeHF"].get<bool>();
            if (c.contains("biasTeeVHF")) biasTeeVHF = c["biasTeeVHF"].get<bool>();
            if (c.contains("dithering"))  dithering  = c["dithering"].get<bool>();
            for (int i = 0; i < (int)gainList.size(); i++) {
                if (c.contains("gains") && c["gains"].contains(gainList[i]))
                    uiGains[i] = c["gains"][gainList[i]].get<float>();
            }
            double savedSr = c.contains("sampleRate") ? c["sampleRate"].get<double>() : sampleRates[0];
            selectSampleRate(savedSr);
        }
        else {
            // Defaults
            mode       = hasHF ? "HF" : (hasVHF ? "VHF" : "HF");
            adcFreq    = 128e6;
            biasTeeHF  = false;
            biasTeeVHF = false;
            dithering  = true;
            if (!sampleRates.empty()) selectSampleRate(sampleRates[0]);
        }
        config.release();

        // Sync visible combo index after mode and rate are set
        applyModeRateCap();
    }

    void selectSampleRate(double sr) {
        if (sampleRates.empty()) return;
        int best = 0;
        double bestDiff = std::abs(sampleRates[0] - sr);
        for (int i = 1; i < (int)sampleRates.size(); i++) {
            double diff = std::abs(sampleRates[i] - sr);
            if (diff < bestDiff) { bestDiff = diff; best = i; }
        }
        srId       = best;
        sampleRate = sampleRates[srId];
        core::setInputSampleRate(sampleRate);
    }

    void saveConfig() {
        if (devId < 0 || devList.empty()) return;
        std::string label = deviceLabel(devList[devId]);
        json c;
        c["mode"]       = mode;
        c["sampleRate"] = sampleRate;
        c["adcFreq"]    = adcFreq;
        c["biasTeeHF"]  = biasTeeHF;
        c["biasTeeVHF"] = biasTeeVHF;
        c["dithering"]  = dithering;
        for (int i = 0; i < (int)gainList.size(); i++)
            c["gains"][gainList[i]] = uiGains[i];
        config.acquire();
        config.conf["device"]           = label;
        config.conf["devices"][label]   = c;
        config.release(true);
    }

    void applySetting(const std::string& key, const std::string& value) {
        if (!dev) return;
        try { dev->writeSetting(key, value); }
        catch (const std::exception& e) {
            flog::warn("RX888: writeSetting({}) failed: {}", key, e.what());
        }
    }

    static void menuSelected(void* ctx) {
        RX888SourceModule* _this = (RX888SourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
    }

    static void menuDeselected(void* ctx) {}

    static void start(void* ctx) {
        RX888SourceModule* _this = (RX888SourceModule*)ctx;
        if (_this->running) return;
        if (_this->devId < 0) { flog::error("RX888: No device selected"); return; }

        try {
            _this->dev = SoapySDR::Device::make(_this->devList[_this->devId]);
        }
        catch (const std::exception& e) {
            flog::error("RX888: Failed to open device: {}", e.what());
            return;
        }

        // ADC frequency — set first as it determines valid sample rates
        if (_this->supportsAdcFreq)
            _this->applySetting("adc_frequency", std::to_string((int64_t)_this->adcFreq));

        // Re-query sample rates (they depend on ADC freq) and reselect
        _this->refreshSampleRates(_this->dev);
        _this->selectSampleRate(_this->sampleRate);

        // Antenna / mode — re-query gains after switching so ranges are correct for this mode
        _this->dev->setAntenna(SOAPY_SDR_RX, 0, _this->mode);
        _this->updateGainRanges();
        _this->dev->setSampleRate(SOAPY_SDR_RX, 0, _this->sampleRate);
        _this->dev->setFrequency(SOAPY_SDR_RX, 0, _this->freq);

        // Gains
        for (int i = 0; i < (int)_this->gainList.size(); i++)
            _this->dev->setGain(SOAPY_SDR_RX, 0, _this->gainList[i], _this->uiGains[i]);

        // Bias tees (driver version–aware)
        if (_this->supportsNewBiasTee) {
            _this->applySetting("UpdBiasT_HF",  _this->biasTeeHF  ? "true" : "false");
            _this->applySetting("UpdBiasT_VHF", _this->biasTeeVHF ? "true" : "false");
        }
        else if (_this->supportsBiasTee) {
            bool biasOn = (_this->mode == "HF") ? _this->biasTeeHF : _this->biasTeeVHF;
            _this->applySetting("biastee", biasOn ? "true" : "false");
        }

        // Dithering / randomization (old driver)
        if (_this->supportsDithering) {
            _this->applySetting("dithering",     _this->dithering ? "true" : "false");
            _this->applySetting("randomization", _this->dithering ? "true" : "false");
        }

        _this->devStream = _this->dev->setupStream(SOAPY_SDR_RX, "CF32");
        _this->dev->activateStream(_this->devStream);

        _this->running = true;
        _this->workerThread = std::thread(_worker, _this);
        flog::info("RX888: Started ({}, {} MHz, ADC {} MHz)", _this->mode,
                   _this->sampleRate / 1e6, _this->adcFreq / 1e6);
    }

    static void stop(void* ctx) {
        RX888SourceModule* _this = (RX888SourceModule*)ctx;
        if (!_this->running) return;
        _this->running = false;

        // Deactivate stream first — unblocks readStream in worker
        _this->dev->deactivateStream(_this->devStream);
        // Unblock any pending stream.swap()
        _this->stream.stopWriter();
        _this->workerThread.join();
        _this->stream.clearWriteStop();

        _this->dev->closeStream(_this->devStream);
        SoapySDR::Device::unmake(_this->dev);
        _this->dev = nullptr;
        flog::info("RX888: Stopped");
    }

    static void tune(double freq, void* ctx) {
        RX888SourceModule* _this = (RX888SourceModule*)ctx;
        _this->freq = freq;
        if (_this->running)
            _this->dev->setFrequency(SOAPY_SDR_RX, 0, freq);
    }

    static void menuHandler(void* ctx) {
        RX888SourceModule* _this = (RX888SourceModule*)ctx;

        if (_this->devId < 0) {
            SmGui::FillWidth();
            SmGui::ForceSync();
            if (SmGui::Button(CONCAT("Refresh##rx888_refr_", _this->name))) {
                _this->refresh();
                config.acquire();
                std::string label = config.conf["device"];
                config.release();
                _this->selectDevice(label);
            }
            return;
        }

        if (_this->running) SmGui::BeginDisabled();

        // Device selector
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##rx888_dev_", _this->name), &_this->devId, _this->txtDevList.c_str())) {
            _this->selectDevice(deviceLabel(_this->devList[_this->devId]));
            _this->saveConfig();
        }

        // Sample rate + Refresh on same line
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##rx888_sr_", _this->name), &_this->srVisibleId, _this->txtSrList.c_str())) {
            _this->srId = _this->visibleToRealIdx(_this->srVisibleId);
            _this->sampleRate = _this->sampleRates[_this->srId];
            core::setInputSampleRate(_this->sampleRate);
            _this->saveConfig();
        }
        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##rx888_refr_", _this->name))) {
            _this->refresh();
            config.acquire();
            std::string label = config.conf["device"];
            config.release();
            _this->selectDevice(label);
        }

        // ADC frequency (only shown if driver supports it)
        if (_this->supportsAdcFreq) {
            SmGui::LeftLabel("ADC Clock");
            SmGui::FillWidth();
            float adcMHz = (float)(_this->adcFreq / 1e6);
            if (SmGui::SliderFloat(CONCAT("##rx888_adc_", _this->name), &adcMHz, 16.0f, 140.0f, SmGui::FMT_STR_FLOAT_NO_DECIMAL)) {
                _this->adcFreq = adcMHz * 1e6;
                _this->saveConfig();
            }
        }

        // Mode: HF / VHF radio buttons
        if (_this->hasHF || _this->hasVHF) {
            SmGui::LeftLabel("Mode");
            SmGui::ForceSyncForNext();
            if (_this->hasHF) {
                if (SmGui::RadioButton(CONCAT("HF##rx888_mode_", _this->name), _this->mode == "HF")) {
                    _this->mode = "HF";
                    _this->reQueryGainsForMode();
                    _this->applyModeRateCap();
                    _this->saveConfig();
                }
            }
            if (_this->hasHF && _this->hasVHF) SmGui::SameLine();
            if (_this->hasVHF) {
                if (SmGui::RadioButton(CONCAT("VHF##rx888_mode_", _this->name), _this->mode == "VHF")) {
                    _this->mode = "VHF";
                    _this->reQueryGainsForMode();
                    _this->applyModeRateCap();
                    _this->saveConfig();
                }
            }
        }

        if (_this->running) SmGui::EndDisabled();

        // --- Controls that work live ---

        // Gains — skip any stage whose range is zero (e.g. RF gain in HF mode = [0,0])
        for (int i = 0; i < (int)_this->gainList.size(); i++) {
            float gmin = (float)_this->gainRanges[i].minimum();
            float gmax = (float)_this->gainRanges[i].maximum();
            if (gmin == gmax) continue;  // nothing to control

            // RF gain: 0 = minimum gain, higher values = more gain/sensitivity
            std::string glabel = _this->gainList[i] + " Gain";
            SmGui::LeftLabel(glabel.c_str());
            SmGui::FillWidth();
            float step = (float)_this->gainRanges[i].step();
            bool changed;
            std::string id = std::string("##rx888_gain_") + _this->name + "_" + _this->gainList[i];
            if (step > 0.0f) {
                changed = SmGui::SliderFloatWithSteps(id.c_str(), &_this->uiGains[i], gmin, gmax, step);
            }
            else {
                changed = SmGui::SliderFloat(id.c_str(), &_this->uiGains[i], gmin, gmax);
            }
            if (changed) {
                if (_this->running)
                    _this->dev->setGain(SOAPY_SDR_RX, 0, _this->gainList[i], _this->uiGains[i]);
                _this->saveConfig();
            }
        }

        // Bias Tee — new driver has independent HF/VHF controls
        if (_this->supportsNewBiasTee) {
            if (_this->hasHF) {
                if (SmGui::Checkbox(CONCAT("HF Bias Tee##rx888_bt_hf_", _this->name), &_this->biasTeeHF)) {
                    _this->applySetting("UpdBiasT_HF", _this->biasTeeHF ? "true" : "false");
                    _this->saveConfig();
                }
            }
            if (_this->hasHF && _this->hasVHF) SmGui::SameLine();
            if (_this->hasVHF) {
                if (SmGui::Checkbox(CONCAT("VHF Bias Tee##rx888_bt_vhf_", _this->name), &_this->biasTeeVHF)) {
                    _this->applySetting("UpdBiasT_VHF", _this->biasTeeVHF ? "true" : "false");
                    _this->saveConfig();
                }
            }
        }
        else if (_this->supportsBiasTee) {
            // Old driver: single bias tee, apply based on active mode
            bool& bt = (_this->mode == "HF") ? _this->biasTeeHF : _this->biasTeeVHF;
            if (SmGui::Checkbox(CONCAT("Bias Tee##rx888_bt_", _this->name), &bt)) {
                _this->applySetting("biastee", bt ? "true" : "false");
                _this->saveConfig();
            }
        }

        // Dithering (old driver only)
        if (_this->supportsDithering) {
            if (SmGui::Checkbox(CONCAT("Dithering##rx888_dith_", _this->name), &_this->dithering)) {
                _this->applySetting("dithering",     _this->dithering ? "true" : "false");
                _this->applySetting("randomization", _this->dithering ? "true" : "false");
                _this->saveConfig();
            }
        }
    }

    static void _worker(RX888SourceModule* _this) {
        // Use at least the driver's natural MTU (32768) but no smaller than sampleRate/200
        // to keep CPU overhead reasonable at low sample rates.
        int blockSize = std::max(32768, (int)(_this->sampleRate / 200.0));
        int flags     = 0;
        long long timeNs = 0;

        while (_this->running) {
            int ret = _this->dev->readStream(_this->devStream,
                (void**)&_this->stream.writeBuf, blockSize, flags, timeNs, 100000 /*us*/);
            if (ret < 0) {
                if (ret == SOAPY_SDR_OVERFLOW)
                    flog::warn("RX888: stream overflow");
                continue;
            }
            if (!_this->stream.swap(ret)) return;
        }
    }

    // Identity
    std::string name;
    bool enabled = true;

    // State
    bool running = false;
    double freq  = 100e6;

    // Device list
    SoapySDR::KwargsList devList;
    std::string txtDevList;
    int devId = -1;

    // Capabilities (queried on select)
    std::vector<std::string>      gainList;
    std::vector<SoapySDR::Range>  gainRanges;
    std::vector<float>            uiGains;
    bool hasHF  = true;
    bool hasVHF = true;
    bool supportsNewBiasTee = false;
    bool supportsAdcFreq    = false;
    bool supportsBiasTee    = false;
    bool supportsDithering  = false;

    // Settings
    std::string mode      = "HF";
    double sampleRate     = 32e6;
    double adcFreq        = 128e6;
    bool   biasTeeHF      = false;
    bool   biasTeeVHF     = false;
    bool   dithering      = true;

    // Sample rates
    std::vector<double> sampleRates;
    std::string txtSrList;
    int srId = 0;          // index into sampleRates[]
    int srVisibleId = 0;   // index into filtered combo list

    // SoapySDR handles
    SoapySDR::Device* dev       = nullptr;
    SoapySDR::Stream* devStream = nullptr;

    // DSP
    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
    std::thread workerThread;
};

MOD_EXPORT void _INIT_() {
    json def;
    def["device"]  = "";
    def["devices"] = json::object();
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
