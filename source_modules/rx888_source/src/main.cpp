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
#include <atomic>
#include <filesystem>
#include <memory>
#include <stdexcept>
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
        shuttingDown.store(true);
        restartAfterCleanup.store(false);
        stop(this);
        if (cleanupThread.joinable()) {
            cleanupThread.join();
        }
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
#ifdef _WIN32
        HMODULE selfModule = NULL;
        if (!GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&_INFO_),
                &selfModule)) {
            return;
        }

        std::vector<char> modulePath(32768);
        DWORD pathLength = GetModuleFileNameA(
            selfModule,
            modulePath.data(),
            static_cast<DWORD>(modulePath.size())
        );
        if (pathLength == 0 || pathLength >= modulePath.size()) { return; }

        // rx888_source.dll is at modules/rx888_source.dll.
        // SoapySDDC is at SoapySDR/modules0.8/SDDCSupport.dll.
        std::filesystem::path sddc =
            std::filesystem::path(modulePath.data()).parent_path().parent_path() /
            "SoapySDR" / "modules0.8" / "SDDCSupport.dll";

        std::string err = SoapySDR::loadModule(sddc.string());
        if (!err.empty())
            flog::warn("RX888: SoapySDDC load: {}", err);
        else
            flog::info("RX888: Loaded SoapySDDC from {}", sddc.string());
#elif defined(__APPLE__)
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

    // Strip non-printable and non-ASCII bytes from a device name.
    // The FX3 USB chip can return garbage in its descriptor strings when
    // firmware is stale (after close/reopen without power-cycle).
    // Feeding those bytes to nlohmann::json causes dump_escaped() to throw
    // on invalid UTF-8, crashing the auto-save worker.
    static std::string sanitizeLabel(const std::string& raw) {
        std::string out;
        out.reserve(raw.size());
        for (unsigned char c : raw) {
            if (c >= 0x20 && c < 0x7F)  // printable ASCII only
                out += (char)c;
        }
        // Trim trailing whitespace
        while (!out.empty() && out.back() == ' ')
            out.pop_back();
        return out.empty() ? "RX888" : out;
    }

    // Returns a human-readable label for a device entry
    static std::string deviceLabel(const SoapySDR::Kwargs& args) {
        if (args.count("label") && !args.at("label").empty())
            return sanitizeLabel(args.at("label"));
        if (args.count("hardware") && !args.at("hardware").empty())
            return sanitizeLabel(args.at("hardware"));
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
            gainRanges.push_back(defaultGainRange(g, queryMode));
            // Default to 0 dB clamped to the valid range, not minimum (which is too low)
            double def = std::max(gainRanges.back().minimum(),
                         std::min(0.0, gainRanges.back().maximum()));
            uiGains.push_back((float)def);
        }

        auto antennas = dev->listAntennas(SOAPY_SDR_RX, 0);
        hasHF  = std::find(antennas.begin(), antennas.end(), "HF")  != antennas.end();
        hasVHF = std::find(antennas.begin(), antennas.end(), "VHF") != antennas.end();

        refreshSettingCapabilities(dev);
        refreshSampleRates(dev);
    }

    void refreshSettingCapabilities(SoapySDR::Device* dev) {
        // Detect which settings keys this driver version supports without
        // replacing the saved gain values.
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
    }

    void setDefaultCapabilities() {
        hasHF  = true;
        hasVHF = true;

        gainList = { "RF", "IF" };
        gainRanges.clear();
        uiGains.clear();
        for (const auto& g : gainList) {
            gainRanges.push_back(defaultGainRange(g, mode));
            double def = std::max(gainRanges.back().minimum(),
                         std::min(0.0, gainRanges.back().maximum()));
            uiGains.push_back((float)def);
        }

        supportsNewBiasTee = true;
        supportsAdcFreq    = true;
        supportsBiasTee    = false;
        supportsDithering  = false;
        refreshDefaultSampleRates();
    }

    static SoapySDR::Range defaultGainRange(const std::string& name, const std::string& rangeMode) {
        if (name == "RF")
            return rangeMode == "VHF" ? SoapySDR::Range(0.0, 49.6) : SoapySDR::Range(-31.5, 0.0);
        if (name == "IF")
            return rangeMode == "VHF" ? SoapySDR::Range(-4.7, 40.8) : SoapySDR::Range(-24.6, 33.1);
        return SoapySDR::Range();
    }

    void refreshSampleRates(SoapySDR::Device* dev) {
        sampleRates = dev->listSampleRates(SOAPY_SDR_RX, 0);
        buildSrText();
    }

    void refreshDefaultSampleRates() {
        sampleRates.clear();

        int numRates = adcFreq > 80000000.0 ? 6 : 5;
        double bwmin = adcFreq / 64.0;
        if (adcFreq > 80000000.0) bwmin /= 2.0;

        for (int idx = 0; idx < numRates; idx++) {
            double rate = bwmin * (double)(1 << idx) * 2.0;
            if ((rate / adcFreq) * 2.0 <= 1.1)
                sampleRates.push_back(rate);
        }

        buildSrText();
    }

    bool clampAdcToDriverRange(SoapySDR::Device* dev) {
        if (!supportsAdcFreq || !dev) return false;

        try {
            auto settingInfo = dev->getSettingInfo();
            for (const auto& s : settingInfo) {
                if (s.key != "adc_frequency") continue;

                double minFreq = s.range.minimum();
                double maxFreq = s.range.maximum();
                if (maxFreq <= minFreq) return false;

                double clamped = std::max(minFreq, std::min(adcFreq, maxFreq));
                if (clamped == adcFreq) return false;

                flog::warn("RX888: Saved ADC clock {} MHz is outside driver range {}-{} MHz; using {} MHz",
                    adcFreq / 1e6, minFreq / 1e6, maxFreq / 1e6, clamped / 1e6);
                adcFreq = clamped;
                refreshDefaultSampleRates();
                selectSampleRate(sampleRate);
                saveConfig();
                return true;
            }
        }
        catch (const std::exception& e) {
            flog::warn("RX888: Could not read driver ADC range: {}", e.what());
        }
        catch (...) {
            flog::warn("RX888: Could not read driver ADC range");
        }

        return false;
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
        auto newList   = gainList.empty() ? std::vector<std::string>{ "RF", "IF" } : gainList;
        std::vector<SoapySDR::Range> newRanges;
        for (const auto& g : newList)
            newRanges.push_back(defaultGainRange(g, mode));

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
        if (devList.empty() || devId < 0 || running.load()) return;
        auto m    = mode;
        auto newList = std::vector<std::string>{ "RF", "IF" };
        std::vector<SoapySDR::Range> newRanges;
        for (const auto& g : newList)
            newRanges.push_back(defaultGainRange(g, m));

        gainList   = newList;
        gainRanges = newRanges;
        uiGains.resize(gainList.size(), 0.0f);
        for (int i = 0; i < (int)gainList.size(); i++) {
            uiGains[i] = std::max((float)gainRanges[i].minimum(),
                         std::min(uiGains[i], (float)gainRanges[i].maximum()));
        }
    }

    void applyModeRateCap() {
        bool changed = false;
        if (mode == "VHF" && sampleRate > VHF_MAX_SR) {
            selectSampleRate(VHF_MAX_SR);
            changed = true;
        }
        buildSrText();
        srVisibleId = realToVisibleIdx(srId);
        if (changed) saveConfig();
    }

    void selectDevice(const std::string& label) {
        if (devList.empty()) { devId = -1; return; }

        int found = 0;
        for (int i = 0; i < (int)devList.size(); i++) {
            if (deviceLabel(devList[i]) == label) { found = i; break; }
        }
        devId = found;
        std::string selectedLabel = deviceLabel(devList[devId]);

        json savedDevice;
        bool hasSavedDevice = false;
        config.acquire();
        auto& dc = config.conf["devices"];
        if (dc.contains(selectedLabel) || dc.contains(label)) {
            savedDevice = dc.contains(selectedLabel) ? dc[selectedLabel] : dc[label];
            hasSavedDevice = true;
            if (savedDevice.contains("mode"))       mode       = savedDevice["mode"].get<std::string>();
            if (savedDevice.contains("adcFreq"))    adcFreq    = savedDevice["adcFreq"].get<double>();
            if (savedDevice.contains("biasTeeHF"))  biasTeeHF  = savedDevice["biasTeeHF"].get<bool>();
            if (savedDevice.contains("biasTeeVHF")) biasTeeVHF = savedDevice["biasTeeVHF"].get<bool>();
            if (savedDevice.contains("dithering"))  dithering  = savedDevice["dithering"].get<bool>();
        }
        config.release();

        setDefaultCapabilities();

        // Load saved config for this device
        if (hasSavedDevice) {
            for (int i = 0; i < (int)gainList.size(); i++) {
                if (savedDevice.contains("gains") && savedDevice["gains"].contains(gainList[i]))
                    uiGains[i] = savedDevice["gains"][gainList[i]].get<float>();
            }
            if (!sampleRates.empty()) {
                double savedSr = savedDevice.contains("sampleRate") ? savedDevice["sampleRate"].get<double>() : sampleRates[0];
                selectSampleRate(savedSr);
            }
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

    static void cleanupFailedStart(RX888SourceModule* _this) {
        _this->running.store(false);
        _this->stream.stopWriter();

        auto* dev = _this->dev;
        auto* devStream = _this->devStream;
        _this->dev = nullptr;
        _this->devStream = nullptr;

        try {
            if (dev && devStream) dev->deactivateStream(devStream);
        }
        catch (const std::exception& e) {
            flog::warn("RX888: Failed-start deactivate failed: {}", e.what());
        }
        catch (...) {
            flog::warn("RX888: Failed-start deactivate failed");
        }

        if (_this->workerThread.joinable()) {
            _this->workerThread.join();
        }

        try {
            if (dev && devStream) dev->closeStream(devStream);
        }
        catch (const std::exception& e) {
            flog::warn("RX888: Failed-start closeStream failed: {}", e.what());
        }
        catch (...) {
            flog::warn("RX888: Failed-start closeStream failed");
        }

        try {
            if (dev) SoapySDR::Device::unmake(dev);
        }
        catch (const std::exception& e) {
            flog::warn("RX888: Failed-start unmake failed: {}", e.what());
        }
        catch (...) {
            flog::warn("RX888: Failed-start unmake failed");
        }

        _this->stream.clearWriteStop();
    }

    static void menuSelected(void* ctx) {
        RX888SourceModule* _this = (RX888SourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
    }

    static void menuDeselected(void* ctx) {}

    static void start(void* ctx) {
        RX888SourceModule* _this = (RX888SourceModule*)ctx;
        if (_this->shuttingDown.load()) return;
        if (_this->running.load()) return;
        if (_this->driverCleanupBusy->load()) {
            _this->restartAfterCleanup.store(true);
            flog::warn("RX888: Previous driver shutdown is still in progress; start queued");
            return;
        }
        if (_this->devId < 0) { flog::error("RX888: No device selected"); return; }

        try {
            _this->dev = SoapySDR::Device::make(_this->devList[_this->devId]);
            if (!_this->dev) throw std::runtime_error("SoapySDR returned no device");

            // Replace the pre-open fallback list with the rates and settings
            // actually supported by this driver instance.
            _this->refreshSettingCapabilities(_this->dev);
            _this->clampAdcToDriverRange(_this->dev);
            if (_this->supportsAdcFreq)
                _this->applySetting("adc_frequency", std::to_string((int64_t)_this->adcFreq));

            double requestedSampleRate = _this->sampleRate;
            _this->refreshSampleRates(_this->dev);
            if (_this->sampleRates.empty())
                throw std::runtime_error("Driver reported no supported sample rates");
            _this->selectSampleRate(requestedSampleRate);
            _this->applyModeRateCap();

            // Program the ADC while the freshly opened MkII is still in its
            // initialized HF state. Initializing the VHF tuner first can make
            // the following STARTADC control transfer fail. Then select the
            // requested antenna explicitly and preserve it while tuning.
            _this->dev->setSampleRate(SOAPY_SDR_RX, 0, _this->sampleRate);
            _this->dev->setAntenna(SOAPY_SDR_RX, 0, _this->mode);
            _this->updateGainRanges();
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

            _this->stream.clearWriteStop();
            _this->running.store(true);
            _this->workerThread = std::thread(_worker, _this);
            flog::info("RX888: Started ({}, {} MHz, ADC {} MHz)", _this->mode,
                       _this->sampleRate / 1e6, _this->adcFreq / 1e6);
        }
        catch (const std::exception& e) {
            flog::error("RX888: Start failed: {}", e.what());
            cleanupFailedStart(_this);
        }
        catch (...) {
            flog::error("RX888: Start failed with an unknown driver error");
            cleanupFailedStart(_this);
        }
    }

    static void stop(void* ctx) {
        RX888SourceModule* _this = (RX888SourceModule*)ctx;
        if (!_this->running.exchange(false)) return;

        auto* dev = _this->dev;
        auto* devStream = _this->devStream;
        std::thread worker;
        if (_this->workerThread.joinable()) {
            worker = std::move(_this->workerThread);
        }

        _this->dev = nullptr;
        _this->devStream = nullptr;

        auto cleanupBusy = _this->driverCleanupBusy;
        cleanupBusy->store(true);

        // A completed cleanup remains joinable until it is collected. Joining
        // it here is immediate and lets this member safely own the next cleanup.
        if (_this->cleanupThread.joinable()) {
            _this->cleanupThread.join();
        }

        // Unblock any pending stream.swap() immediately, then let the slow
        // driver/USB shutdown continue off the SDR++ UI thread.
        _this->stream.stopWriter();

        _this->cleanupThread = std::thread([_this, dev, devStream, cleanupBusy, worker = std::move(worker)]() mutable {
            // Tell the hardware stream to stop before joining the worker.  The RX888
            // driver can otherwise keep readStream() alive long enough to leave the
            // FX3/USB side in a half-stopped state, which makes the next Start fail
            // until the app or device is power-cycled.
            try {
                if (dev && devStream) { dev->deactivateStream(devStream); }
            }
            catch (const std::exception& e) {
                flog::warn("RX888: Driver deactivate failed during stop: {}", e.what());
            }
            catch (...) {
                flog::warn("RX888: Driver deactivate failed during stop");
            }

            if (worker.joinable()) {
                worker.join();
            }
            _this->stream.clearWriteStop();

            try {
                if (dev && devStream) { dev->closeStream(devStream); }
            }
            catch (const std::exception& e) {
                flog::warn("RX888: Driver closeStream failed during stop: {}", e.what());
            }
            catch (...) {
                flog::warn("RX888: Driver closeStream failed during stop");
            }

            try {
                if (dev) { SoapySDR::Device::unmake(dev); }
            }
            catch (const std::exception& e) {
                flog::warn("RX888: Driver unmake failed during stop: {}", e.what());
            }
            catch (...) {
                flog::warn("RX888: Driver unmake failed during stop");
            }

            cleanupBusy->store(false);
            flog::info("RX888: Driver cleanup finished");
            if (!_this->shuttingDown.load() && _this->restartAfterCleanup.exchange(false)) {
                flog::info("RX888: Running queued start after driver cleanup");
                start(_this);
            }
        });
        flog::info("RX888: Stop requested; driver cleanup is running in background");
    }

    static void tune(double freq, void* ctx) {
        RX888SourceModule* _this = (RX888SourceModule*)ctx;
        _this->freq = freq;
        if (_this->running.load())
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
                _this->saveConfig();
            }
            return;
        }

        if (_this->running.load()) SmGui::BeginDisabled();

        // Device selector
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##rx888_dev_", _this->name), &_this->devId, _this->txtDevList.c_str())) {
            _this->selectDevice(deviceLabel(_this->devList[_this->devId]));
            _this->saveConfig();
        }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##rx888_refr_", _this->name))) {
            _this->refresh();
            config.acquire();
            std::string label = config.conf["device"];
            config.release();
            _this->selectDevice(label);
            _this->saveConfig();
        }

        // Sample rate
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##rx888_sr_", _this->name), &_this->srVisibleId, _this->txtSrList.c_str())) {
            _this->srId = _this->visibleToRealIdx(_this->srVisibleId);
            _this->sampleRate = _this->sampleRates[_this->srId];
            core::setInputSampleRate(_this->sampleRate);
            _this->saveConfig();
        }

        // ADC frequency (only shown if driver supports it)
        if (_this->supportsAdcFreq) {
            SmGui::LeftLabel("ADC Clock");
            SmGui::FillWidth();
            float adcMHz = (float)(_this->adcFreq / 1e6);
            if (SmGui::SliderFloat(CONCAT("##rx888_adc_", _this->name), &adcMHz, 16.0f, 140.0f, SmGui::FMT_STR_FLOAT_NO_DECIMAL)) {
                _this->adcFreq = adcMHz * 1e6;
                _this->applyModeRateCap();
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

        if (_this->running.load()) SmGui::EndDisabled();

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
                if (_this->running.load())
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

        while (_this->running.load()) {
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
    std::atomic<bool> running{false};
    std::atomic<bool> restartAfterCleanup{false};
    std::atomic<bool> shuttingDown{false};
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
    std::shared_ptr<std::atomic<bool>> driverCleanupBusy = std::make_shared<std::atomic<bool>>(false);

    // DSP
    dsp::stream<dsp::complex_t> stream;
    SourceManager::SourceHandler handler;
    std::thread workerThread;
    std::thread cleanupThread;
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
