#include <server.h>
#include "imgui.h"
#include <stdio.h>
#include <gui/main_window.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <gui/icons.h>
#include <version.h>
#include <utils/flog.h>
#include <gui/widgets/bandplan.h>
#include <stb_image.h>
#include <config.h>
#include <core.h>
#include <filesystem>
#include <gui/menus/theme.h>
#include <backend.h>
#include <static_modules.h>
#include "ios_backend.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>


namespace core {
    ConfigManager configManager;
    ModuleManager moduleManager;
    ModuleComManager modComManager;
    CommandArgsParser args;

    void setInputSampleRate(double samplerate) {
        // Forward this to the server
        if (args["server"].b()) { server::setInputSampleRate(samplerate); return; }
        
        // Update IQ frontend input samplerate and get effective samplerate
        sigpath::iqFrontEnd.setSampleRate(samplerate);
        double effectiveSr  = sigpath::iqFrontEnd.getEffectiveSamplerate();
        
        // Reset zoom
        gui::waterfall.setBandwidth(effectiveSr);
        gui::waterfall.setViewOffset(0);
        gui::waterfall.setViewBandwidth(effectiveSr);
        gui::mainWindow.setViewBandwidthSlider(1.0);

        // Debug logs
        flog::info("New DSP samplerate: {0} (source samplerate is {1})", effectiveSr, samplerate);
    }
};

// main
int sdrpp_main(int argc, char* argv[]) {
    flog::info("SDR++ v" VERSION_STR);

    // Define command line options and parse arguments
    core::args.defineAll();
    if (core::args.parse(argc, argv) < 0) { return -1; } 

    // Show help and exit if requested
    if (core::args["help"].b()) {
        core::args.showHelp();
        return 0;
    }

    bool serverMode = (bool)core::args["server"];

    // Check root directory
    std::string root = (std::string)core::args["root"];
    if (!std::filesystem::exists(root)) {
        flog::warn("Root directory {0} does not exist, creating it", root);
        if (!std::filesystem::create_directories(root)) {
            flog::error("Could not create root directory {0}", root);
            return -1;
        }
    }

    // Check that the path actually is a directory
    if (!std::filesystem::is_directory(root)) {
        flog::error("{0} is not a directory", root);
        return -1;
    }

    // ======== DEFAULT CONFIG ========
    json defConfig;
    defConfig["bandColors"]["amateur"] = "#FF0000FF";
    defConfig["bandColors"]["aviation"] = "#00FF00FF";
    defConfig["bandColors"]["broadcast"] = "#0000FFFF";
    defConfig["bandColors"]["marine"] = "#00FFFFFF";
    defConfig["bandColors"]["military"] = "#FFFF00FF";
    defConfig["bandPlan"] = "General";
    defConfig["bandPlanEnabled"] = true;
    defConfig["bandPlanPos"] = 0;
    defConfig["centerTuning"] = false;
    defConfig["colorMap"] = "Classic";
    defConfig["fftHold"] = false;
    defConfig["fftHoldSpeed"] = 60;
    defConfig["fftSmoothing"] = false;
    defConfig["fftSmoothingSpeed"] = 100;
    defConfig["snrSmoothing"] = false;
    defConfig["snrSmoothingSpeed"] = 20;
    defConfig["fastFFT"] = false;
    defConfig["fftHeight"] = 300;
    defConfig["fftRate"] = 20;
    defConfig["fftSize"] = 65536;
    defConfig["fftWindow"] = 2;
    defConfig["frequency"] = 100000000.0;
    defConfig["fullWaterfallUpdate"] = false;
    defConfig["max"] = 0.0;
    defConfig["maximized"] = false;
    defConfig["fullscreen"] = false;

    // Menu
    defConfig["menuElements"] = json::array();

    defConfig["menuElements"][0]["name"] = "Source";
    defConfig["menuElements"][0]["open"] = true;

    defConfig["menuElements"][1]["name"] = "Radio";
    defConfig["menuElements"][1]["open"] = true;

    defConfig["menuElements"][2]["name"] = "Recorder";
    defConfig["menuElements"][2]["open"] = true;

    defConfig["menuElements"][3]["name"] = "Sinks";
    defConfig["menuElements"][3]["open"] = true;

    defConfig["menuElements"][4]["name"] = "Frequency Manager";
    defConfig["menuElements"][4]["open"] = true;

    defConfig["menuElements"][5]["name"] = "VFO Color";
    defConfig["menuElements"][5]["open"] = true;

    defConfig["menuElements"][6]["name"] = "Band Plan";
    defConfig["menuElements"][6]["open"] = true;

    defConfig["menuElements"][7]["name"] = "Display";
    defConfig["menuElements"][7]["open"] = true;

    defConfig["menuWidth"] = 300;
    defConfig["min"] = -120.0;

    // Module instances — iOS client-only set.
    defConfig["moduleInstances"]["File Source"]["module"]          = "file_source";
    defConfig["moduleInstances"]["File Source"]["enabled"]         = true;
    defConfig["moduleInstances"]["Network Source"]["module"]       = "network_source";
    defConfig["moduleInstances"]["Network Source"]["enabled"]      = true;
    defConfig["moduleInstances"]["RTL-TCP Source"]["module"]       = "rtl_tcp_source";
    defConfig["moduleInstances"]["RTL-TCP Source"]["enabled"]      = true;
    defConfig["moduleInstances"]["SDR++ Server Source"]["module"]  = "sdrpp_server_source";
    defConfig["moduleInstances"]["SDR++ Server Source"]["enabled"] = true;
    defConfig["moduleInstances"]["Spectran HTTP Source"]["module"] = "spectran_http_source";
    defConfig["moduleInstances"]["Spectran HTTP Source"]["enabled"]= true;
    defConfig["moduleInstances"]["SpyServer Source"]["module"]     = "spyserver_source";
    defConfig["moduleInstances"]["SpyServer Source"]["enabled"]    = true;

    defConfig["moduleInstances"]["Audio Sink"]   = "coreaudio_sink";
    defConfig["moduleInstances"]["Network Sink"] = "network_sink";

    defConfig["moduleInstances"]["Radio"]             = "radio";
    defConfig["moduleInstances"]["Frequency Manager"] = "frequency_manager";
    defConfig["moduleInstances"]["Recorder"]          = "recorder";


    // Themes
    defConfig["theme"] = "Dark";
    // Touch UI on iPhone/iPad — bigger hit targets.
    defConfig["uiScale"] = 2.0f;

    defConfig["modules"] = json::array();

    defConfig["offsets"]["SpyVerter"] = 120000000.0;
    defConfig["offsets"]["Ham-It-Up"] = 125000000.0;
    defConfig["offsets"]["MMDS S-band (1998MHz)"] = -1998000000.0;
    defConfig["offsets"]["DK5AV X-Band"] = -6800000000.0;
    defConfig["offsets"]["Ku LNB (9750MHz)"] = -9750000000.0;
    defConfig["offsets"]["Ku LNB (10700MHz)"] = -10700000000.0;

    defConfig["selectedOffset"] = "None";
    defConfig["manualOffset"] = 0.0;
    defConfig["showMenu"] = true;
    defConfig["showWaterfall"] = true;
    defConfig["source"] = "";
    defConfig["decimation"] = 1;
    defConfig["iqCorrection"] = false;
    defConfig["invertIQ"] = false;

    defConfig["streams"]["Radio"]["muted"] = false;
    defConfig["streams"]["Radio"]["sink"] = "Audio";
    defConfig["streams"]["Radio"]["volume"] = 1.0f;

    defConfig["windowSize"]["h"] = 720;
    defConfig["windowSize"]["w"] = 1280;

    defConfig["vfoOffsets"] = json::object();

    defConfig["vfoColors"]["Radio"] = "#FFFFFF";

    defConfig["lockMenuOrder"] = true;

    // iOS sandbox: everything lives under <appSupport>. modulesDirectory is
    // unused (modules are statically linked) but the loader still needs a
    // valid path to scan harmlessly.
    defConfig["modulesDirectory"]   = root + "/modules";
    defConfig["resourcesDirectory"] = root + "/res";

    // Load config
    flog::info("Loading config");
    core::configManager.setPath(root + "/config.json");
    core::configManager.load(defConfig);
    core::configManager.enableAutoSave();
    core::configManager.acquire();

    // iOS: rewrite the absolute resource/modules paths every launch. The
    // app's data container UUID changes on reinstall (and on simulator
    // restore), so a path baked into a persisted config.json will be stale.
    // root itself is the *current* container's <Library/Application Support>.
    core::configManager.conf["resourcesDirectory"] = defConfig["resourcesDirectory"];
    core::configManager.conf["modulesDirectory"]   = defConfig["modulesDirectory"];

    // Static-link mode: register every module compiled into the binary, then
    // seed the "modules" config list so main_window's loader resolves them by
    // name. loadModule() short-circuits to the registry — see module.cpp.
    registerStaticModules();
    core::configManager.conf["modules"] = json::array();
    int modCount = 0;
    for (auto const& [name, _mod] : core::moduleManager.modules) {
        core::configManager.conf["modules"][modCount++] = name + std::string(SDRPP_MOD_EXTENTSION);
    }

    // Fix missing elements in config
    for (auto const& item : defConfig.items()) {
        if (!core::configManager.conf.contains(item.key())) {
            flog::info("Missing key in config {0}, repairing", item.key());
            core::configManager.conf[item.key()] = defConfig[item.key()];
        }
    }

    // Remove unused elements
    auto items = core::configManager.conf.items();
    auto newConf = core::configManager.conf;
    bool configCorrected = false;
    for (auto const& item : items) {
        if (!defConfig.contains(item.key())) {
            flog::info("Unused key in config {0}, repairing", item.key());
            newConf.erase(item.key());
            configCorrected = true;
        }
    }
    if (configCorrected) {
        core::configManager.conf = newConf;
    }

    // Update to new module representation in config if needed
    for (auto [_name, inst] : core::configManager.conf["moduleInstances"].items()) {
        if (!inst.is_string()) { continue; }
        std::string mod = inst;
        json newMod;
        newMod["module"] = mod;
        newMod["enabled"] = true;
        core::configManager.conf["moduleInstances"][_name] = newMod;
    }

    // Load UI scaling
    style::uiScale = core::configManager.conf["uiScale"];

    core::configManager.release(true);

    if (serverMode) { return server::main(); }

    core::configManager.acquire();
    std::string resDir = core::configManager.conf["resourcesDirectory"];
    json bandColors = core::configManager.conf["bandColors"];
    core::configManager.release();

    // Assert that the resource directory is absolute and check existence
    resDir = std::filesystem::absolute(resDir).string();
    if (!std::filesystem::is_directory(resDir)) {
        flog::error("Resource directory doesn't exist! Please make sure that you've configured it correctly in config.json (check readme for details)");
        return 1;
    }

    // Initialize backend
    int biRes = backend::init(resDir);
    if (biRes < 0) { return biRes; }

    // Initialize SmGui in normal mode
    SmGui::init(false);

    if (!style::loadFonts(resDir)) { return -1; }
    thememenu::init(resDir);
    LoadingScreen::init();

    LoadingScreen::show("Loading icons");
    flog::info("Loading icons");
    if (!icons::load(resDir)) { return -1; }

    LoadingScreen::show("Loading band plans");
    flog::info("Loading band plans");
    bandplan::loadFromDir(resDir + "/bandplans");

    LoadingScreen::show("Loading band plan colors");
    flog::info("Loading band plans color table");
    bandplan::loadColorTable(bandColors);

    gui::mainWindow.init();

    // Signal the Metal draw loop that it can now call gui::mainWindow.draw().
    // This must come after init() — the draw delegate runs at 60 Hz on the
    // main thread and would crash on uninitialised state without this gate.
    backend::iosSetMainWindowReady();

    flog::info("Ready.");

    // Run render loop. On iOS this parks on UIApplication's runloop and never
    // returns under normal use; the shutdown path below exists only as a
    // theoretical clean-exit. iOS apps are killed by the OS, not via main().
    backend::renderLoop();

    for (auto& [name, mod] : core::moduleManager.modules) {
        mod.end();
    }

    backend::end();

    sigpath::iqFrontEnd.stop();

    core::configManager.disableAutoSave();
    core::configManager.save();

    flog::info("Exiting successfully");
    return 0;
}
