#include "receiver.h"
#include "decoder.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>

namespace adsb {
using json=nlohmann::json;
uint64_t nowMillis() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::vector<Device> devices() {
    std::vector<Device> result;
    std::map<std::string,unsigned> counts;
    for(unsigned i=0;i<rtlsdr_get_device_count();++i) {
        char vendor[256]{},product[256]{},serial[256]{};
        rtlsdr_get_device_usb_strings(i,vendor,product,serial);
        result.push_back({serial,std::string(rtlsdr_get_device_name(i))+" ["+(serial[0]?serial:"no serial")+"]",i,false});
        ++counts[serial];
    }
    for(auto& d:result) { d.unique=!d.serial.empty() && counts[d.serial]==1; if(!d.unique)d.label+=" (unique serial required)"; }
    return result;
}
void Receiver::setStatus(std::string value) { std::lock_guard<std::mutex> lock(stateMutex_); status_=std::move(value); }
std::string Receiver::status() { std::lock_guard<std::mutex> lock(stateMutex_); return status_; }
void Receiver::start(const Settings& settings) {
    stop();
    if(settings.serial.empty()) { setStatus("Select a dongle with a unique serial number"); return; }
    if(settings.location && (!std::isfinite(settings.lat)||!std::isfinite(settings.lon)||std::abs(settings.lat)>90||std::abs(settings.lon)>180)) {
        setStatus("Receiver coordinates are outside the valid range"); return;
    }
    { std::lock_guard<std::mutex> lock(stateMutex_); settings_=settings; history_.reset(settings.historyHours); aircraft_.clear(); }
    dropped_=0; messages_=0; aircraftCount_=0; gap_=true;
    stop_=false; readerDone_=false; active_=true;
    setStatus("Opening dedicated RTL-SDR...");
    try {
        decoder_=std::thread([this] {
            try { decode(); }
            catch(const std::exception& e) {
                adsb_decoder_destroy();
                stop_=true;active_=false;
                setStatus(std::string("ADS-B decoder stopped: ")+e.what());
            }
        });
        reader_=std::thread([this] {
            try { capture(); }
            catch(const std::exception& e) {
                { std::lock_guard<std::mutex> lock(deviceMutex_); if(device_){rtlsdr_close(device_);device_=nullptr;} }
                setStatus(std::string("ADS-B receiver stopped: ")+e.what());
                readerDone_=true;ready_.notify_all();
            }
        });
    } catch(const std::exception& e) {
        stop_=true;readerDone_=true;active_=false;ready_.notify_all();
        if(decoder_.joinable())decoder_.join();
        setStatus(std::string("Cannot start ADS-B workers: ")+e.what());
    }
}
void Receiver::stop() {
    stop_=true;
    ready_.notify_all();
    // Repeat cancellation until read_async has returned, including the small
    // window between opening the device and entering libusb's event loop.
    while(!readerDone_) {
        { std::lock_guard<std::mutex> lock(deviceMutex_); if(device_)rtlsdr_cancel_async(device_); }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if(reader_.joinable())reader_.join();
    if(decoder_.joinable())decoder_.join();
    { std::lock_guard<std::mutex> lock(queueMutex_); queue_.clear(); }
    active_=false; aircraftCount_=0;
    setStatus("Stopped");
}
void Receiver::callback(unsigned char* data,uint32_t length,void* context) {
    auto& self=*static_cast<Receiver*>(context);
    if(self.stop_) {
        std::lock_guard<std::mutex> lock(self.deviceMutex_);
        if(self.device_)rtlsdr_cancel_async(self.device_);
        return;
    }
    std::lock_guard<std::mutex> lock(self.queueMutex_);
    if(length<640 || length>262144 || length%2 || self.queue_.size()>=8) {
        ++self.dropped_; self.gap_=true; return;
    }
    try {
        self.queue_.push_back({std::vector<uint8_t>(data,data+length),nowMillis(),self.gap_});
    } catch(const std::bad_alloc&) {
        // Never unwind a C++ allocation failure through librtlsdr's C callback.
        self.stop_=true;
        std::lock_guard<std::mutex> deviceLock(self.deviceMutex_);
        if(self.device_)rtlsdr_cancel_async(self.device_);
        return;
    }
    self.gap_=false;
    self.ready_.notify_one();
}
void Receiver::capture() {
    auto finish=[this](const std::string& error) {
        if(!error.empty())setStatus(error);
        { std::lock_guard<std::mutex> lock(deviceMutex_); if(device_){rtlsdr_close(device_);device_=nullptr;} }
        readerDone_=true; ready_.notify_all();
    };
    auto list=devices();
    auto it=std::find_if(list.begin(),list.end(),[this](const Device& d){return d.unique&&d.serial==settings_.serial;});
    if(it==list.end()){finish("Selected RTL-SDR missing or serial is ambiguous");return;}
    {
        std::lock_guard<std::mutex> lock(deviceMutex_);
        if(rtlsdr_open(&device_,it->index)!=0)device_=nullptr;
    }
    if(!device_){finish("Cannot open RTL-SDR: device busy, disconnected, or unavailable");return;}
    char actualSerial[256]{};
    if(rtlsdr_get_usb_strings(device_,nullptr,nullptr,actualSerial)!=0 || settings_.serial!=actualSerial) {
        finish("RTL-SDR identity changed during opening; refresh and select again");return;
    }
    int result=rtlsdr_set_sample_rate(device_,2400000);
    result|=rtlsdr_set_center_freq(device_,1090000000);
    int ppmResult=rtlsdr_set_freq_correction(device_,settings_.ppm);
    // librtlsdr returns -2 when the requested correction is already applied.
    if(ppmResult!=0 && ppmResult!=-2)result=ppmResult;
    result|=rtlsdr_set_tuner_gain_mode(device_,settings_.agc?0:1);
    result|=rtlsdr_set_agc_mode(device_,0);
    if(!settings_.agc) {
        int n=rtlsdr_get_tuner_gains(device_,nullptr);
        if(n>0 && n<1024) {
            std::vector<int> gains(n);rtlsdr_get_tuner_gains(device_,gains.data());
            auto nearest=std::min_element(gains.begin(),gains.end(),[this](int a,int b){return std::abs(a-settings_.gain)<std::abs(b-settings_.gain);});
            result|=rtlsdr_set_tuner_gain(device_,*nearest);
        } else result=-1;
    }
    result|=rtlsdr_reset_buffer(device_);
    if(result){finish("RTL-SDR configuration failed");return;}
    if(stop_){finish("");return;}
    setStatus("Receiving 1090 MHz at 2.4 MS/s");
    result=rtlsdr_read_async(device_,callback,this,12,262144);
    finish(stop_?"Stopped":"RTL-SDR stream ended; reconnect and press Start (code "+std::to_string(result)+")");
}
void Receiver::decode() {
    adsb_decoder_init(settings_.lat,settings_.lon,settings_.location);
    uint64_t lastPublish=0;
    while(!stop_) {
        Block block;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            ready_.wait_for(lock,std::chrono::milliseconds(100),[this]{return stop_||readerDone_||!queue_.empty();});
            if(stop_)break;
            if(!queue_.empty()) { block=std::move(queue_.front());queue_.pop_front(); }
            else if(readerDone_)break;
        }
        if(!block.iq.empty())adsb_decoder_iq(block.iq.data(),block.iq.size(),block.now,block.gap);
        uint64_t now=nowMillis();
        if(now-lastPublish>=1000) { publish();lastPublish=now; }
    }
    adsb_decoder_destroy();
    active_=false;
}
void Receiver::publish() {
    std::vector<adsb_aircraft> planes(4096);
    auto now=nowMillis();
    auto count=adsb_decoder_snapshot(planes.data(),planes.size(),now);
    json list=json::array();
    for(size_t i=0;i<count;++i) {
        auto& a=planes[i];char hex[10];snprintf(hex,sizeof(hex),"%s%06x",a.address&0x1000000?"~":"",a.address&0xffffff);
        json p={{"hex",hex},{"type",a.type},{"messages",a.messages},{"seen",a.seen},{"rssi",a.rssi}};
        if(a.valid&ADSB_FLIGHT)p["flight"]=a.flight;
        if(a.valid&ADSB_POSITION){p["lat"]=a.lat;p["lon"]=a.lon;p["seen_pos"]=a.seen_pos;}
        if(a.ground)p["alt_baro"]="ground";else if(a.valid&ADSB_ALTITUDE)p["alt_baro"]=a.altitude;
        if(a.valid&ADSB_SPEED)p["gs"]=a.speed;
        if(a.valid&ADSB_TRACK)p["track"]=a.track;
        if(a.valid&ADSB_RATE)p["baro_rate"]=a.vertical_rate;
        if(a.valid&ADSB_SQUAWK){char s[8];snprintf(s,sizeof(s),"%04x",a.squawk);p["squawk"]=s;}
        if(a.category){char s[8];snprintf(s,sizeof(s),"%02X",a.category);p["category"]=s;}
        list.push_back(std::move(p));
    }
    auto total=adsb_decoder_messages();messages_=total;aircraftCount_=count;
    auto body=json({{"now",now/1000.0},{"messages",total},{"aircraft",list}}).dump();
    std::lock_guard<std::mutex> lock(stateMutex_);
    aircraft_=std::move(body);
    history_.append(now,aircraft_);
}
Response Receiver::data(const std::string& path) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if(path=="/data/receiver.json") {
        json j={{"version","SDR++ ADS-B / dump1090-fa"},{"refresh",1000},{"history",history_.size()},{"zstd",false}};
        if(settings_.location){j["lat"]=settings_.lat;j["lon"]=settings_.lon;}
        return {200,"application/json",j.dump()};
    }
    if(path=="/data/aircraft.json") {
        if(!active_ || aircraft_.empty())return {200,"application/json",json({{"now",nowMillis()/1000.0},{"messages",messages_.load()},{"aircraft",json::array()}}).dump()};
        return {200,"application/json",aircraft_};
    }
    const std::string prefix="/data/history_";
    if(path.rfind(prefix,0)==0 && path.size()>prefix.size()+5 && path.substr(path.size()-5)==".json") {
        auto value=path.substr(prefix.size(),path.size()-prefix.size()-5);
        if(value.size()<=4 && value.find_first_not_of("0123456789")==std::string::npos) {
            auto index=std::stoul(value);if(index<history_.size())return {200,"application/json",history_[index]};
        }
    }
    return {404,"application/json","{}"};
}
}
