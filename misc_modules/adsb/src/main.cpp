#include <module.h>
#include <core.h>
#include <config.h>
#include <gui/gui.h>
#include <utils/flog.h>
#include <filesystem>
#include <algorithm>
#include <dlfcn.h>
#include "receiver.h"
#include "map_window.h"

SDRPP_MOD_INFO { "adsb", "Dedicated RTL-SDR ADS-B receiver with tar1090", "wdarney", 0, 1, 0, 1 };
static ConfigManager config;

static std::string assetsPath() {
    Dl_info info{};
    if(dladdr(reinterpret_cast<void*>(&assetsPath),&info) && info.dli_fname) {
        auto directory=std::filesystem::path(info.dli_fname).parent_path();
        for(auto path : {directory/"adsb-web",directory/"../Resources/adsb",directory/"../../../share/sdrpp/res/adsb"})
            if(std::filesystem::is_regular_file(path/"index.html"))return path.string();
    }
    return "";
}
class ADSBModule : public ModuleManager::Instance {
public:
    explicit ADSBModule(std::string name):name_(std::move(name)) {
        config.acquire();
        settings_.serial=config.conf.value("serial",std::string());
        settings_.gain=config.conf.value("gain",400);
        settings_.ppm=config.conf.value("ppm",0);
        settings_.historyHours=std::clamp(config.conf.value("history_hours",6),1,24);
        settings_.lat=config.conf.value("latitude",0.0);
        settings_.lon=config.conf.value("longitude",0.0);
        settings_.location=config.conf.value("location_valid",false);
        settings_.agc=config.conf.value("tuner_agc",false);
        openOnLoad_=config.conf.value("open_map_on_load",true);
        config.release();
        refresh();
        gui::menu.registerEntry(name_,draw,this,nullptr);
        startServer();
    }
    ~ADSBModule() override {
        gui::menu.removeEntry(name_);
        window_.close();server_.stop();receiver_.stop();
    }
    void postInit() override { if(enabled_ && openOnLoad_ && server_.port())window_.show(server_.url()); }
    void enable() override { enabled_=true;startServer();if(openOnLoad_ && server_.port())window_.show(server_.url()); }
    void disable() override { enabled_=false;window_.close();server_.stop();receiver_.stop(); }
    bool isEnabled() override {return enabled_;}
private:
    void startServer() {
        if(server_.port())return;
        error_.clear();
        if(!server_.start(assetsPath(),[this](const std::string& p){return receiver_.data(p);},error_))
            flog::error("ADS-B: {0}",error_);
    }
    void refresh() {
        devices_=adsb::devices();selected_=-1;
        for(size_t i=0;i<devices_.size();++i)if(devices_[i].serial==settings_.serial && devices_[i].unique)selected_=i;
    }
    void save() {
        config.acquire();
        config.conf={{"serial",settings_.serial},{"gain",settings_.gain},{"ppm",settings_.ppm},
            {"latitude",settings_.lat},{"longitude",settings_.lon},{"location_valid",settings_.location},{"tuner_agc",settings_.agc},
            {"open_map_on_load",openOnLoad_},{"history_hours",settings_.historyHours}};
        config.release(true);
    }
    static void draw(void* context) {
        auto& s=*static_cast<ADSBModule*>(context);
        if(!s.enabled_){ImGui::TextUnformatted("ADS-B disabled");return;}
        ImGui::TextUnformatted("Dedicated receiver: 1090 MHz");
        bool running=s.receiver_.running();
        ImGui::BeginDisabled(running);
        const char* preview=s.selected_>=0?s.devices_[s.selected_].label.c_str():"Select RTL-SDR";
        if(ImGui::BeginCombo("Device##adsb",preview)) {
            for(size_t i=0;i<s.devices_.size();++i) {
                ImGui::PushID(static_cast<int>(i));
                if(ImGui::Selectable(s.devices_[i].label.c_str(),s.selected_==static_cast<int>(i))){s.selected_=i;s.settings_.serial=s.devices_[i].serial;s.save();}
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if(ImGui::Button("Refresh devices##adsb"))s.refresh();
        bool changed=ImGui::Checkbox("Tuner auto gain##adsb",&s.settings_.agc);
        float gain=s.settings_.gain/10.0f;
        if(ImGui::SliderFloat("Gain (dB)##adsb",&gain,0,50,"%.1f")){s.settings_.gain=static_cast<int>(gain*10);changed=true;}
        changed|=ImGui::InputInt("Correction (ppm)##adsb",&s.settings_.ppm);
        s.settings_.ppm=std::clamp(s.settings_.ppm,-200,200);
        changed|=ImGui::Checkbox("Receiver location set##adsb",&s.settings_.location);
        changed|=ImGui::InputDouble("Latitude##adsb",&s.settings_.lat,0,0,"%.6f");
        changed|=ImGui::InputDouble("Longitude##adsb",&s.settings_.lon,0,0,"%.6f");
        changed|=ImGui::SliderInt("History (hours)##adsb",&s.settings_.historyHours,1,24);
        if(changed)s.save();
        ImGui::EndDisabled();
        if(running){if(ImGui::Button("Stop ADS-B"))s.receiver_.stop();}
        else if(ImGui::Button("Start ADS-B")) {
            s.receiver_.start(s.settings_);
            if(s.receiver_.running() && s.server_.port())s.window_.show(s.server_.url());
        }
        ImGui::SameLine();
        if(ImGui::Button("Open tar1090")){s.startServer();if(s.server_.port())s.window_.show(s.server_.url());}
        if(ImGui::Checkbox("Open map on module load",&s.openOnLoad_))s.save();
        ImGui::TextWrapped("%s",s.receiver_.status().c_str());
        ImGui::Text("Aircraft: %u  Messages: %llu",s.receiver_.aircraftCount(),static_cast<unsigned long long>(s.receiver_.messages()));
        ImGui::Text("Dropped buffers: %llu",static_cast<unsigned long long>(s.receiver_.dropped()));
        if(!s.error_.empty())ImGui::TextWrapped("%s",s.error_.c_str());
        if(s.server_.port())ImGui::TextWrapped("%s",s.server_.url().c_str());
        ImGui::TextWrapped("History stays in memory until the next Start or app exit (512 MB maximum). Basic offline map is bundled; detailed map layers need internet. Start/Stop is independent of the main receiver.");
    }
    std::string name_,error_;
    bool enabled_=true;
    bool openOnLoad_=true;
    int selected_=-1;
    adsb::Settings settings_;
    std::vector<adsb::Device> devices_;
    adsb::Receiver receiver_;
    adsb::HttpServer server_;
    adsb::MapWindow window_;
};
MOD_EXPORT void _INIT_() {
    config.setPath(core::args["root"].s()+"/adsb_config.json");
    config.load(nlohmann::json::object());config.enableAutoSave();
}
MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {return new ADSBModule(std::move(name));}
MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {delete static_cast<ADSBModule*>(instance);}
MOD_EXPORT void _END_() {config.disableAutoSave();config.save();}
