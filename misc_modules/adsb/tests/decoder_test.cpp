#include "decoder.h"
#include <array>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstdio>
#include <cmath>
#include <chrono>
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
static std::array<uint8_t,14> frame(const std::string& hex){std::array<uint8_t,14> a{};for(unsigned i=0;i<14;++i)a[i]=std::stoul(hex.substr(i*2,2),nullptr,16);return a;}
static uint64_t now(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
int main(){
    auto even=frame("8D40621D58C382D690C8AC2863A7");
    auto odd=frame("8D40621D58C386435CC412692AD6");
    uint64_t t=now()-1000;
    adsb_aircraft aircraft[16];
    for(int cycle=0;cycle<3;++cycle){
        adsb_decoder_init(0,0,0);
        check(adsb_decoder_message(odd.data(),14,t),"valid odd frame rejected");
        check(adsb_decoder_message(even.data(),14,t+1),"valid even frame rejected");
        check(adsb_decoder_message(even.data(),14,t+2),"valid repeated frame rejected");
        size_t n=adsb_decoder_snapshot(aircraft,16,t+3);
        check(n==1,"expected one reliable aircraft");
        check(aircraft[0].address==0x40621d,"wrong ICAO address");
        check(aircraft[0].valid&ADSB_POSITION,"CPR position missing");
        check(std::abs(aircraft[0].lat-52.257202)<0.001 && std::abs(aircraft[0].lon-3.919373)<0.001,"CPR position incorrect");
        check(aircraft[0].altitude==38000,"altitude incorrect");
        auto bad=even;bad[5]^=1;
        check(!adsb_decoder_message(bad.data(),14,t+4),"bad CRC accepted");
        check(!adsb_decoder_message(even.data(),7,t+4),"truncated frame accepted");
        check(adsb_decoder_messages()==3,"invalid frame changed message count");
        check(adsb_decoder_snapshot(aircraft,16,t+120003)==0,"stale aircraft remained live");
        adsb_decoder_destroy();
    }
    // Generate rectangular ADS-B pulses, integrated over the sample aperture.
    // The second message straddles a USB block boundary.
    adsb_decoder_init(0,0,0);
    std::vector<uint8_t> iq(262144*2,127);
    auto pulse=[&](const std::array<uint8_t,14>& msg,size_t start){
        for(size_t sample=0;sample<300;++sample){
            double high=0;
            for(int sub=0;sub<20;++sub){
                double us=(sample+(sub+0.5)/20.0)/2.4;
                bool on=(us<0.5)||(us>=1&&us<1.5)||(us>=3.5&&us<4)||(us>=4.5&&us<5);
                if(us>=8&&us<120){int bit=static_cast<int>(us-8);bool one=(msg[bit/8]>>(7-bit%8))&1;on=one?us-8-bit<0.5:us-8-bit>=0.5;}
                high+=on?1:0;
            }
            iq[(start+sample)*2]=static_cast<uint8_t>(127+110*high/20);
        }
    };
    pulse(odd,1000);pulse(even,131000);pulse(even,140000);
    adsb_decoder_iq(iq.data(),262144,t+100,1);
    adsb_decoder_iq(iq.data()+262144,262144,t+155,0);
    size_t n=adsb_decoder_snapshot(aircraft,16,t+200);
    check(adsb_decoder_messages()==3,"IQ demodulation lost or duplicated a frame across buffer boundary");
    check(n==1 && (aircraft[0].valid&ADSB_POSITION),"IQ pipeline did not produce positioned aircraft");
    // A discontinuity must not join samples from unrelated USB buffers.
    adsb_decoder_init(0,0,0);
    adsb_decoder_iq(iq.data(),262144,t+100,1);
    adsb_decoder_iq(iq.data()+262144,262144,t+155,1);
    check(adsb_decoder_messages()==2,"discontinuous buffers reconstructed an invalid crossing frame");
    adsb_decoder_destroy();
    puts("PASS: CRC, CPR, altitude, stale expiry, repeated lifecycle, IQ decoding, buffer overlap and discontinuity");
}
