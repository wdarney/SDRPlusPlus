#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/smgui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <utils/optionlist.h>
#include <atomic>
#include <sddc.h>
#include <filesystem>
#include <algorithm>
#include <fftw3.h>

#ifdef __ANDROID__
#include <android_backend.h>
#endif

SDRPP_MOD_INFO{
    /* Name:            */ "sddc_source",
    /* Description:     */ "SDDC Source Module",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ -1
};

ConfigManager config;

#define CONCAT(a, b) ((std::string(a) + b).c_str())


class SDDCSourceModule : public ModuleManager::Instance {
public:
    SDDCSourceModule(std::string name) {
        this->name = name;

        // The Android asset copy places bundled resources under root/res.
        std::filesystem::path firmwarePath = (std::string)core::args["root"];
        firmwarePath /= "res/SDDC_FX3.img";
        sddc_set_firmware_path(firmwarePath.string().c_str());

        sampleRate = 128e6;

        // Initialize the DDC
        ddc.init(&ddcIn, 50e6, 50e6, 50e6, 0.0);

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.stream = &ddc.out;

        // Refresh devices
        refresh();

        // Select device from config
        config.acquire();
        std::string devSerial = config.conf["device"];
        config.release();
        select(devSerial);

        sigpath::sourceManager.registerSource("SDDC", &handler);
    }

    ~SDDCSourceModule() {
        // Nothing to do
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    enum Port {
        PORT_VHF,
        PORT_HF
    };

private:
    std::string getBandwdithScaled(double bw) {
        char buf[1024];
        if (bw >= 1000000.0) {
            sprintf(buf, "%.1lfMHz", bw / 1000000.0);
        }
        else if (bw >= 1000.0) {
            sprintf(buf, "%.1lfKHz", bw / 1000.0);
        }
        else {
            sprintf(buf, "%.1lfHz", bw);
        }
        return std::string(buf);
    }

    void refresh() {
        devices.clear();
        
        sddc_devinfo_t* devList = nullptr;
#ifdef __ANDROID__
        int vid = -1;
        int pid = -1;
        devFd = backend::getDeviceFD(vid, pid, backend::SDDC_VIDPIDS);
        if (devFd < 0) {
            flog::error("No Android USB fd available for SDDC device");
            return;
        }
        int count = sddc_get_device_list_fd(devFd, vid, pid, &devList);
#else
        int count = sddc_get_device_list(&devList);
#endif
        if (count < 0) {
            flog::error("Failed to list SDDC devices: {}", count);
            return;
        }

        for (int i = 0; i < count; i++) {
            std::string name = sddc_model_to_string(devList[i].model);
            name += '[';
            name += devList[i].serial;
            name += ']';
            devices.define(devList[i].serial, name, devList[i].serial);
        }

        if (devList) {
            sddc_free_device_list(devList);
        }
    }

    void select(const std::string& serial) {
        // If there are no devices, give up
        if (devices.empty()) {
            selectedSerial.clear();
            return;
        }

        // If the serial was not found, select the first available serial
        if (!devices.keyExists(serial)) {
            select(devices.key(0));
            return;
        }

        // Get the ID in the list
        int id = devices.keyId(serial);

        // Open the device
        sddc_dev_t* dev;
#ifdef __ANDROID__
        int err = sddc_open_fd(devFd, serial.c_str(), &dev);
#else
        int err = sddc_open(serial.c_str(), &dev);
#endif
        if (err) {
            flog::error("Failed to open device: {}", err);
            return;
        }

        // Generate samplerate list
        samplerates.clear();
        samplerates.define(4e6, "4 MHz", 4e6);
#ifndef __ANDROID__
        samplerates.define(8e6, "8 MHz", 8e6);
        samplerates.define(16e6, "16 MHz", 16e6);
        samplerates.define(32e6, "32 MHz", 32e6);
        samplerates.define(64e6, "64 MHz", 64e6);
#else
        samplerates.define(8e6, "8 MHz", 8e6);
#endif

        ports.clear();
        ports.define("vhf", "VHF", PORT_VHF);
        ports.define("hf", "HF", PORT_HF);

        // Close the device
        sddc_close(dev);

        // Save serial number
        selectedSerial = serial;
        devId = id;

        // Load default options
        sampleRate = 4e6;
        srId = samplerates.valueId(sampleRate);
        port = PORT_VHF;
        portId = ports.valueId(port);
        rfAtt = 15;
        ifGain = 9;
        vhfAtt = 0;
        dithering = false;
        randomizer = false;

        // Load config
        config.acquire();
        if (config.conf["devices"][selectedSerial].contains("samplerate")) {
            int desiredSr = config.conf["devices"][selectedSerial]["samplerate"];
            if (samplerates.keyExists(desiredSr)) {
                srId = samplerates.keyId(desiredSr);
                sampleRate = samplerates[srId];
            }
        }
        if (config.conf["devices"][selectedSerial].contains("port")) {
            std::string desiredPort = config.conf["devices"][selectedSerial]["port"];
            if (desiredPort == "hf1" || desiredPort == "hf2") {
                desiredPort = "hf";
            }
            if (ports.keyExists(desiredPort)) {
                portId = ports.keyId(desiredPort);
                port = ports[portId];
            }
        }
        if (config.conf["devices"][selectedSerial].contains("rfAtt")) {
            rfAtt = std::clamp<int>(config.conf["devices"][selectedSerial]["rfAtt"], 0, 63);
        }
        else if (config.conf["devices"][selectedSerial].contains("r82xxAtt")) {
            rfAtt = std::clamp<int>(config.conf["devices"][selectedSerial]["r82xxAtt"], 0, 63);
        }
        if (config.conf["devices"][selectedSerial].contains("ifGain")) {
            ifGain = std::clamp<int>(config.conf["devices"][selectedSerial]["ifGain"], 0, 126);
        }
        else if (config.conf["devices"][selectedSerial].contains("r83xxVga")) {
            ifGain = std::clamp<int>(config.conf["devices"][selectedSerial]["r83xxVga"], 0, 126);
        }
        if (config.conf["devices"][selectedSerial].contains("vhfAtt")) {
            vhfAtt = std::clamp<int>(config.conf["devices"][selectedSerial]["vhfAtt"], 0, 31);
        }
        if (config.conf["devices"][selectedSerial].contains("dithering")) {
            dithering = config.conf["devices"][selectedSerial]["dithering"];
        }
        if (config.conf["devices"][selectedSerial].contains("randomizer")) {
            randomizer = config.conf["devices"][selectedSerial]["randomizer"];
        }
        config.release();

        // Update the samplerate
        core::setInputSampleRate(sampleRate);
    }

    void applySettings() {
        if (!openDev) { return; }
        sddc_set_port(openDev, (sddc_port_t)port);
        sddc_set_rf_attenuator(openDev, rfAtt);
        sddc_set_if_gain(openDev, ifGain);
        sddc_set_vhf_attenuator(openDev, vhfAtt);
        sddc_set_dithering(openDev, dithering);
        sddc_set_randomizer(openDev, randomizer);
    }

    void saveDeviceSettings() {
        if (selectedSerial.empty()) { return; }
        config.acquire();
        config.conf["devices"][selectedSerial]["port"] = ports.key(portId);
        config.conf["devices"][selectedSerial]["rfAtt"] = rfAtt;
        config.conf["devices"][selectedSerial]["ifGain"] = ifGain;
        config.conf["devices"][selectedSerial]["vhfAtt"] = vhfAtt;
        config.conf["devices"][selectedSerial]["dithering"] = dithering;
        config.conf["devices"][selectedSerial]["randomizer"] = randomizer;
        config.release(true);
    }

    static void menuSelected(void* ctx) {
        SDDCSourceModule* _this = (SDDCSourceModule*)ctx;
        core::setInputSampleRate(_this->sampleRate);
        flog::info("SDDCSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        SDDCSourceModule* _this = (SDDCSourceModule*)ctx;
        flog::info("SDDCSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        SDDCSourceModule* _this = (SDDCSourceModule*)ctx;
        if (_this->running) { return; }

        // Open the device
#ifdef __ANDROID__
        sddc_error_t err = sddc_open_fd(_this->devFd, _this->selectedSerial.c_str(), &_this->openDev);
#else
        sddc_error_t err = sddc_open(_this->selectedSerial.c_str(), &_this->openDev);
#endif
        if (err) {
            flog::error("Failed to open device: {}", (int)err);
            return;
        }

        _this->port = _this->ports[_this->portId];

        // Configure the device
        sddc_set_samplerate(_this->openDev, _this->sampleRate * 2);
        _this->applySettings();

        // // Configure the DDC
        // if (_this->port == PORT_RF && _this->sampleRate >= 50e6) {
        //     // Set the frequency
        //     fobos_rx_set_frequency(_this->openDev, _this->freq, &actualFreq);
        // }
        // else if (_this->port == PORT_RF) {
        //     // Set the frequency
        //     fobos_rx_set_frequency(_this->openDev, _this->freq, &actualFreq);

        //     // Configure and start the DDC for decimation only
        //     _this->ddc.setInSamplerate(actualSr);
        //     _this->ddc.setOutSamplerate(_this->sampleRate, _this->sampleRate);
        //     _this->ddc.setOffset(0.0);
        //     _this->ddc.start();
        // }
        // else {
            // Configure and start the DDC
            _this->ddc.setInSamplerate(_this->sampleRate * 2);
            _this->ddc.setOutSamplerate(_this->sampleRate, _this->sampleRate);
            _this->ddc.setOffset(_this->freq);
            _this->ddc.start();
        // }

        // Compute buffer size (Lower than usual, but it's a workaround for their API having broken streaming)
        _this->bufferSize = (_this->sampleRate * 2.0) / 100.0;

        // Start streaming
        err = sddc_start(_this->openDev);
        if (err) {
            flog::error("Failed to start stream: {}", (int)err);
            return;
        }

        // Start worker
        _this->run = true;
        _this->workerThread = std::thread(&SDDCSourceModule::worker, _this);
        
        _this->running = true;
        flog::info("SDDCSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        SDDCSourceModule* _this = (SDDCSourceModule*)ctx;
        if (!_this->running) { return; }
        _this->running = false;

        // Stop worker
        _this->run = false;
        if (false) {
            _this->ddc.out.stopWriter();
            if (_this->workerThread.joinable()) { _this->workerThread.join(); }
            _this->ddc.out.clearWriteStop();
        }
        else {
            _this->ddcIn.stopWriter();
            if (_this->workerThread.joinable()) { _this->workerThread.join(); }
            _this->ddcIn.clearWriteStop();
        }

        // Stop streaming
        sddc_stop(_this->openDev);

        // Stop the DDC
        _this->ddc.stop();

        // Close the device
        sddc_close(_this->openDev);

        flog::info("SDDCSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        SDDCSourceModule* _this = (SDDCSourceModule*)ctx;
        if (_this->running) {
            // if (_this->port == PORT_RF) {
            //     double actual; // Dummy, don't care
            //     //fobos_rx_set_frequency(_this->openDev, freq, &actual);
            // }
            // else {
                _this->ddc.setOffset(freq);
            // }
        }
        _this->freq = freq;
        flog::info("SDDCSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        SDDCSourceModule* _this = (SDDCSourceModule*)ctx;
        
        if (_this->running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_sddc_dev_sel_", _this->name), &_this->devId, _this->devices.txt)) {
            _this->select(_this->devices.key(_this->devId));
            core::setInputSampleRate(_this->sampleRate);
            config.acquire();
            config.conf["device"] = _this->selectedSerial;
            config.release(true);
        }

        if (SmGui::Combo(CONCAT("##_sddc_sr_sel_", _this->name), &_this->srId, _this->samplerates.txt)) {
            _this->sampleRate = _this->samplerates.value(_this->srId);
            core::setInputSampleRate(_this->sampleRate);
            if (!_this->selectedSerial.empty()) {
                config.acquire();
                config.conf["devices"][_this->selectedSerial]["samplerate"] = _this->samplerates.key(_this->srId);
                config.release(true);
            }
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_sddc_refr_", _this->name))) {
            _this->refresh();
            _this->select(_this->selectedSerial);
            core::setInputSampleRate(_this->sampleRate);
        }

        SmGui::LeftLabel("Input");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_sddc_port_", _this->name), &_this->portId, _this->ports.txt)) {
            _this->port = _this->ports[_this->portId];
            if (_this->running) { _this->applySettings(); }
            _this->saveDeviceSettings();
        }

        if (_this->running) { SmGui::EndDisabled(); }

        int rfAttMax = (_this->port == PORT_HF) ? 63 : 28;
        if (_this->rfAtt > rfAttMax) { _this->rfAtt = rfAttMax; }
        SmGui::LeftLabel("RF Att");
        SmGui::FillWidth();
        if (SmGui::SliderInt(CONCAT("##_sddc_rf_att_", _this->name), &_this->rfAtt, 0, rfAttMax)) {
            if (_this->running) { _this->applySettings(); }
            _this->saveDeviceSettings();
        }

        int ifGainMax = (_this->port == PORT_HF) ? 126 : 15;
        if (_this->ifGain > ifGainMax) { _this->ifGain = ifGainMax; }
        SmGui::LeftLabel("IF Gain");
        SmGui::FillWidth();
        if (SmGui::SliderInt(CONCAT("##_sddc_if_gain_", _this->name), &_this->ifGain, 0, ifGainMax)) {
            if (_this->running) { _this->applySettings(); }
            _this->saveDeviceSettings();
        }

        SmGui::LeftLabel("VHF Att");
        SmGui::FillWidth();
        if (SmGui::SliderInt(CONCAT("##_sddc_vhf_att_", _this->name), &_this->vhfAtt, 0, 31)) {
            if (_this->running) { _this->applySettings(); }
            _this->saveDeviceSettings();
        }

        if (SmGui::Checkbox(CONCAT("Dithering##_sddc_dither_", _this->name), &_this->dithering)) {
            if (_this->running) { _this->applySettings(); }
            _this->saveDeviceSettings();
        }

        if (SmGui::Checkbox(CONCAT("Randomizer##_sddc_rand_", _this->name), &_this->randomizer)) {
            if (_this->running) { _this->applySettings(); }
            _this->saveDeviceSettings();
        }
    }

    void worker() {
        int16_t* buffer = dsp::buffer::alloc<int16_t>(bufferSize);
        dsp::complex_t* fftIn = (dsp::complex_t*)fftwf_malloc(sizeof(fftwf_complex) * bufferSize);
        dsp::complex_t* fftOut = (dsp::complex_t*)fftwf_malloc(sizeof(fftwf_complex) * bufferSize);
        fftwf_plan forwardPlan = fftwf_plan_dft_1d(bufferSize, (fftwf_complex*)fftIn, (fftwf_complex*)fftOut, FFTW_FORWARD, FFTW_ESTIMATE);
        fftwf_plan inversePlan = fftwf_plan_dft_1d(bufferSize, (fftwf_complex*)fftOut, (fftwf_complex*)fftIn, FFTW_BACKWARD, FFTW_ESTIMATE);
        float scale = 1.0f / (float)bufferSize;

        while (run) {
            int err = sddc_rx(openDev, buffer, bufferSize);
            if (err) { break; }

            for (int i = 0; i < bufferSize; i++) {
                fftIn[i].re = (float)buffer[i] / 32768.0f;
                fftIn[i].im = 0.0f;
            }
            fftwf_execute(forwardPlan);
            int half = bufferSize / 2;
            for (int i = 1; i < half; i++) {
                fftOut[i].re *= 2.0f;
                fftOut[i].im *= 2.0f;
            }
            for (int i = half + 1; i < bufferSize; i++) {
                fftOut[i].re = 0.0f;
                fftOut[i].im = 0.0f;
            }
            fftwf_execute(inversePlan);
            for (int i = 0; i < bufferSize; i++) {
                ddcIn.writeBuf[i].re = fftIn[i].re * scale;
                ddcIn.writeBuf[i].im = fftIn[i].im * scale;
            }

            if (!ddcIn.swap(bufferSize)) { break; }
        }

        fftwf_destroy_plan(forwardPlan);
        fftwf_destroy_plan(inversePlan);
        fftwf_free(fftIn);
        fftwf_free(fftOut);
        dsp::buffer::free(buffer);
    }

    std::string name;
    bool enabled = true;
    double sampleRate;
    SourceManager::SourceHandler handler;
    bool running = false;
    double freq;

    OptionList<std::string, std::string> devices;
    OptionList<int, int> samplerates;
    OptionList<std::string, Port> ports;
    int devId = 0;
    int srId = 0;
    int portId = 0;
    Port port = PORT_VHF;
    int rfAtt = 15;
    int ifGain = 9;
    int vhfAtt = 0;
    bool dithering = false;
    bool randomizer = false;
    std::string selectedSerial;

    sddc_dev_t* openDev;
#ifdef __ANDROID__
    int devFd = -1;
#endif

    int bufferSize;
    std::thread workerThread;
    std::atomic<bool> run = false;

    dsp::stream<dsp::complex_t> ddcIn;
    dsp::channel::RxVFO ddc;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["devices"] = json({});
    def["device"] = "";
    config.setPath(core::args["root"].s() + "/sddc_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SDDCSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (SDDCSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
