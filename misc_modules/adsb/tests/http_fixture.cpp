#include "http_server.h"
#include "receiver.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <stdexcept>
int main(int argc,char** argv){
    if(argc<2)return 2;
    adsb::Receiver receiver;
    if(argc>2 && std::string(argv[2])=="--receiver-check") {
        adsb::Settings settings;
        receiver.start(settings);
        if(receiver.running())throw std::runtime_error("empty device selection started capture");
        settings.serial="SDRPP-TEST-NONEXISTENT-DEVICE";
        settings.location=true;settings.lat=100;
        receiver.start(settings);
        if(receiver.running())throw std::runtime_error("invalid latitude started capture");
        settings.location=false;
        for(int i=0;i<3;++i) {
            receiver.start(settings);
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(receiver.running() && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if(receiver.running())throw std::runtime_error("missing device left worker running");
            if(receiver.status().find("missing")==std::string::npos)throw std::runtime_error("missing device error not surfaced");
            receiver.stop();
        }
        std::cout<<"PASS: empty selection, invalid location, repeated missing-device startup/shutdown"<<std::endl;
        return 0;
    }
    adsb::HttpServer server;
    std::string error;
    bool fixture=argc>2&&std::string(argv[2])=="--fixture";
    if(!server.start(argv[1],[&](const std::string& path){
        if(fixture&&path=="/data/receiver.json")return adsb::Response{200,"application/json",R"({"version":"TEST FIXTURE - no live receiver","refresh":1000,"history":0,"lat":52.25,"lon":3.9,"zstd":false})"};
        if(fixture&&path=="/data/aircraft.json")return adsb::Response{200,"application/json",nlohmann::json({{"now",adsb::nowMillis()/1000.0},{"messages",100},{"aircraft",nlohmann::json::array({{{"hex","40621d"},{"flight","TEST123 "},{"type","adsb_icao"},{"lat",52.2572},{"lon",3.91937},{"alt_baro",38000},{"gs",420},{"track",85},{"messages",100},{"seen",0},{"seen_pos",0},{"rssi",-20}}})}}).dump()};
        return receiver.data(path);
    },error)){std::cerr<<error<<std::endl;return 1;}
    std::cout<<server.url()<<std::endl;
    std::string line;std::getline(std::cin,line);
}
