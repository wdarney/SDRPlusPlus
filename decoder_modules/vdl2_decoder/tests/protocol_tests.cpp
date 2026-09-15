#include "vdl2_dsp.h"
#include "acars_dsp.h"
#include "adsb_dsp.h"
#include "vdl2_message_json.h"
#include "aviation_jsonl.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <iostream>
#include <stdexcept>
#include <random>
extern "C" {
#include "asn1/Fully-encoded-data.h"
#include "asn1/ACSE-apdu.h"
#include "asn1/CMGroundMessage.h"
#include "asn1/CMAircraftMessage.h"
#include "asn1/ProtectedGroundPDUs.h"
#include "asn1/ATCUplinkMessage.h"
#include "asn1/ATCDownlinkMessage.h"
#include "asn1/ProtectedAircraftPDUs.h"
#include "asn1/ADSGroundPDUs.h"
#include <libacars/asn1/per_encoder.h>
#include <libacars/asn1/FANSATCDownlinkMessage.h>
#include <libacars/crc.h>
}
using Bytes = std::vector<uint8_t>;
using Json = nlohmann::json;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string("line ")+std::to_string(__LINE__)+": " #x); } while(0)
static Bytes join(Bytes a, const Bytes& b) { a.insert(a.end(),b.begin(),b.end()); return a; }
static Bytes part(const Bytes& b,size_t a,size_t z) { return Bytes(b.begin()+a,b.begin()+z); }
static Bytes x25(Bytes p,int seq=0,bool more=false,int channel=1) {
    return join({0x10,uint8_t(channel),uint8_t((seq<<1)|(more?0x10:0))},p);
}
static Bytes cotp(Bytes p,int seq=0,bool more=false) {
    return join({4,0xf0,0x12,0x34,uint8_t(seq|(more?0:0x80))},p);
}
static Bytes compressed(Bytes p) { return join({0,0x20,0,1},p); }
static Bytes fragment(Bytes p,int offset,int total,bool more,bool normal=false,int id=42) {
    if(normal) {
        int size=19+p.size();
        return join({0x81,19,1,32,uint8_t(0x80|0x1c|(more?0x40:0)),uint8_t(size>>8),uint8_t(size),0,0,
                     1,0x47,1,0x48,0,uint8_t(id),uint8_t(offset>>8),uint8_t(offset),uint8_t((total+19)>>8),uint8_t(total+19)},p);
    }
    return join({uint8_t(more?0x70:0x60),32,0,1,0,uint8_t(id),uint8_t(offset>>8),uint8_t(offset),uint8_t(total>>8),uint8_t(total)},p);
}
static Json decode(VDL2ProtocolDecoder& d,Bytes b,double t=1000,uint32_t src=0x1000001,int dst=0x4000002,bool ground=false) {
    auto r=d.decode(b.data(),b.size(),src,dst,ground,t,false);
    CHECK(!r.text.empty()); return Json::parse(r.json);
}
static bool contains(const Json& j,const std::string& key) {
    if(j.is_object() && j.contains(key)) return true;
    if(j.is_structured()) for(const auto& v:j) if(contains(v,key)) return true;
    return false;
}
static Bytes encode(asn_TYPE_descriptor_t& td,void* obj) {
    void* buf=nullptr;
    auto len=uper_encode_to_new_buffer(&td,nullptr,obj,&buf);
    if(len<0) throw std::runtime_error(std::string("PER encode: ")+td.name);
    Bytes bytes((uint8_t*)buf,(uint8_t*)buf+len); free(buf); return bytes;
}
// Explicit application selector in ACSE prevents ambiguous application probing.
static Bytes application(Bytes payload,int qualifier) {
    ACSE_apdu_t a{}; a.present=ACSE_apdu_PR_aarq;
    AE_qualifier_t q{}; q.present=AE_qualifier_PR_ae_qualifier_form2; q.choice.ae_qualifier_form2=qualifier;
    a.choice.aarq.calling_AE_qualifier=&q;
    unsigned oid[]={1,3,27,8};
    CHECK(OBJECT_IDENTIFIER_set_arcs(&a.choice.aarq.application_context_name,oid,sizeof(unsigned),4)==0);
    Association_information_t info{};
    info.data.encoding.present=EXTERNALt__encoding_PR_arbitrary;
    info.data.encoding.choice.arbitrary.buf=payload.data();
    info.data.encoding.choice.arbitrary.size=payload.size();
    a.choice.aarq.user_information=&info;
    auto acse=encode(asn_DEF_ACSE_apdu,&a);
    free(a.choice.aarq.application_context_name.buf);
    Fully_encoded_data_t fed{};
    fed.data.presentation_context_identifier=Presentation_context_identifier_acse_apdu;
    fed.data.presentation_data_values.present=PDV_list__presentation_data_values_PR_arbitrary;
    fed.data.presentation_data_values.choice.arbitrary.buf=acse.data();
    fed.data.presentation_data_values.choice.arbitrary.size=acse.size();
    return encode(asn_DEF_Fully_encoded_data,&fed);
}
static Bytes cm(bool ground) {
    if(ground) {
        CMGroundMessage_t m{}; m.present=CMGroundMessage_PR_cmAbortReason; m.choice.cmAbortReason=0;
        return application(encode(asn_DEF_CMGroundMessage,&m),1);
    }
    CMAircraftMessage_t m{}; m.present=CMAircraftMessage_PR_cmAbortReason; m.choice.cmAbortReason=0;
    return application(encode(asn_DEF_CMAircraftMessage,&m),1);
}
static Bytes cmLogon() {
    CMAircraftMessage_t m{}; m.present=CMAircraftMessage_PR_cmLogonRequest;
    auto& logon=m.choice.cmLogonRequest;
    uint8_t flight[]={'U','A','0','8','8','4'};
    uint8_t rdp[5]={0x47,0,0,0,0}, local[10]={0};
    logon.aircraftFlightIdentification.buf=flight; logon.aircraftFlightIdentification.size=sizeof(flight);
    logon.cMLongTSAP.rDP.buf=rdp; logon.cMLongTSAP.rDP.size=sizeof(rdp);
    logon.cMLongTSAP.shortTsap.locSysNselTsel.buf=local;
    logon.cMLongTSAP.shortTsap.locSysNselTsel.size=sizeof(local);
    return application(encode(asn_DEF_CMAircraftMessage,&m),1);
}
static Bytes cpdlc() {
    ATCUplinkMessage_t m{};
    m.header.messageIdNumber=7;
    m.header.dateTime.date.year=2026; m.header.dateTime.date.month=9; m.header.dateTime.date.day=14;
    ATCUplinkMsgElementId_t e{}; e.present=ATCUplinkMsgElementId_PR_uM0NULL;
    ASN_SEQUENCE_ADD(&m.messageData.elementIds.list,&e);
    auto raw=encode(asn_DEF_ATCUplinkMessage,&m);
    free(m.messageData.elementIds.list.array);
    ProtectedGroundPDUs_t p{}; p.present=ProtectedGroundPDUs_PR_send;
    CPDLCMessage_t message{}; message.buf=raw.data(); message.size=raw.size();
    p.choice.send.protectedMessage=&message;
    uint8_t checksum[2]={0,0}; p.choice.send.integrityCheck.buf=checksum; p.choice.send.integrityCheck.size=2;
    return application(encode(asn_DEF_ProtectedGroundPDUs,&p),22);
}
static Bytes cpdlcDown() {
    ATCDownlinkMessage_t m{};
    m.header.messageIdNumber=7;
    m.header.dateTime.date.year=2026; m.header.dateTime.date.month=9; m.header.dateTime.date.day=14;
    ATCDownlinkMsgElementId_t e{}; e.present=ATCDownlinkMsgElementId_PR_dM0NULL;
    ASN_SEQUENCE_ADD(&m.messageData.elementIds.list,&e);
    auto raw=encode(asn_DEF_ATCDownlinkMessage,&m);
    free(m.messageData.elementIds.list.array);
    ProtectedAircraftPDUs_t p{}; p.present=ProtectedAircraftPDUs_PR_send;
    CPDLCMessage_t message{}; message.buf=raw.data(); message.size=raw.size();
    p.choice.send.protectedMessage=&message;
    uint8_t checksum[2]={0,0}; p.choice.send.integrityCheck.buf=checksum; p.choice.send.integrityCheck.size=2;
    return application(encode(asn_DEF_ProtectedAircraftPDUs,&p),22);
}
static Bytes adsc() {
    ADSGroundPDUs_t m{}; m.timestamp.date.year=2026; m.timestamp.date.month=9; m.timestamp.date.day=14;
    m.adsGroundPdu.present=ADSGroundPDU_PR_aDS_cancel_all_contracts_PDU;
    return application(encode(asn_DEF_ADSGroundPDUs,&m),0);
}
struct VDL2ChannelTestAccess {
    static void metadata(VDL2Channel& c) { c.freq=136975000; c.num_fec_corrections=2; c.ppm_error=-0.75f; }
    static void parse(VDL2Channel& c,Bytes b) { c.parseAVLC(b.data(),b.size(),15.5f); }
};
static Bytes avlc(Bytes payload,bool ground=true,uint8_t control=0) {
    // Encode address bits independently, including type and EA extension bit.
    auto address=[](uint32_t v) {
        uint32_t reversed=0; for(int n=0;n<28;n++) reversed=(reversed<<1)|((v>>n)&1);
        return Bytes{uint8_t((reversed&127)<<1),uint8_t(((reversed>>7)&127)<<1),
                     uint8_t(((reversed>>14)&127)<<1),uint8_t((((reversed>>21)&127)<<1)|1)};
    };
    Bytes b=join(join(address(ground?0x1000001:0x4000002),address(ground?0x4000002:0x1000001)),{control}); b=join(b,payload);
    uint16_t crc=0xffff; for(auto byte:b) { crc^=byte; for(int k=0;k<8;k++) crc=(crc>>1)^((crc&1)?0x8408:0); }
    crc^=0xffff; b.push_back(crc&255); b.push_back(crc>>8); return b;
}
static Bytes acarsWire(const std::string& text, const std::string& label="Q0") {
    std::string header="2.N795UA"; header+=char(0x15); header+=label; header+='0'; header+=char(2);
    header+="001AUA0884"; header+=text; header+=char(3);
    Bytes b(header.begin(),header.end());
    for(auto& v:b) { unsigned ones=0; for(int k=0;k<7;k++) ones+=(v>>k)&1; if(!(ones&1)) v|=0x80; }
    uint16_t crc=la_crc16_ccitt(b.data(),b.size(),0);
    b.push_back(crc&255); b.push_back(crc>>8);
    CHECK(la_crc16_ccitt(b.data(),b.size(),0)==0);
    b.push_back(0x7f); return b;
}
static Bytes fansWire() {
    FANSATCDownlinkMessage_t m{};
    m.aTCMessageheader.msgIdentificationNumber=7;
    m.aTCDownlinkmsgelementid.present=FANSATCDownlinkMsgElementId_PR_dM0NULL;
    auto per=encode(asn_DEF_FANSATCDownlinkMessage,&m);
    std::string prefix="AT1.N795UA";
    auto crcInput=join(Bytes(prefix.begin(),prefix.end()),per);
    uint16_t crc=la_crc16_arinc(crcInput.data(),crcInput.size(),0xffff)^0xffff;
    per.push_back(crc>>8); per.push_back(crc&255);
    auto verified=join(Bytes(prefix.begin(),prefix.end()),per);
    CHECK(la_crc16_arinc(verified.data(),verified.size(),0xffff)==0x1d0f);
    std::string body="/KUSA."+prefix;
    for(auto b:per) { char hex[3]; snprintf(hex,sizeof(hex),"%02X",b); body+=hex; }
    return acarsWire(body,"BA");
}
struct ACARSChannelTestAccess {
    static void parse(ACARSChannel& c,const Bytes& wire) {
        c.msgBuf=part(wire,0,wire.size()-3);
        c.crcBytes[0]=wire[wire.size()-3]; c.crcBytes[1]=wire[wire.size()-2];
        c.buildMessage();
    }
};
struct ADSBChannelTestAccess {
    static void parse(ADSBChannel& c,const Bytes& b) { c.parseMessage(b.data(),b.size()); }
};
static void structuredTests() {
    VDL2Channel channel;
    VDL2ChannelTestAccess::metadata(channel);
    std::vector<VDL2Message> emitted;
    channel.setMessageCallback([&](const VDL2Message& m) { emitted.push_back(m); });
    auto send=[&](Bytes p,bool ground=true,uint8_t control=0) {
        VDL2ChannelTestAccess::parse(channel,avlc(p,ground,control));
        CHECK(!emitted.empty());
        auto& m=emitted.back(); auto j=Json::parse(m.json_text);
        CHECK(j["timestamp"]==m.timestamp); CHECK(j["freq"]==136975000);
        CHECK(j["snr"]==15.5); CHECK(j["fec"]==2); CHECK(j["ppm"]==-0.75);
        CHECK(j["text"]==m.formatted_text); CHECK(j["decoded"].is_object());
        CHECK(j["direction"]==(ground?"GND2AIR":"AIR2GND"));
        CHECK(j["src"]["type"]==(ground?"GND":"AIR"));
        CHECK(j["dst"]["type"]==(ground?"AIR":"GND"));
        CHECK(j["src"]["address"]==(ground?"000002":"000001"));
        return j;
    };
    for(uint8_t control:{uint8_t(0),uint8_t(1),uint8_t(3)}) {
        auto j=send({},true,control); CHECK(j["protocol"]=="VDL2"); CHECK(!j.contains("transport"));
        CHECK(j["frame_type"]==(control==0?"I":control==1?"S":"U"));
        CHECK(!j.contains("tail")); CHECK(!j.contains("flight"));
    }
    auto j=send(join({255,255,1},acarsWire("STATUS CPDLC ADS-C CM\n\"quoted\"")),false);
    CHECK(j["protocol"]=="ACARS"); CHECK(j["transport"]=="ACARS");
    CHECK(j["tail"]=="N795UA"); CHECK(j["flight"]=="UA0884");
    CHECK(j["decoded"]["acars"]["crc_ok"]==true);
    CHECK(j["decode_path"]["atn_x25"]==false);
    auto fans=fansWire(); j=send(join({255,255,1},fans),false);
    CHECK(j["protocol"]=="CPDLC"); CHECK(j["transport"]=="ACARS");
    CHECK(contains(j["decoded"],"cpdlc")); CHECK(j["decode_path"]["fans_cpdlc"]==true);
    CHECK(j["decode_path"]["atn_cpdlc"]==false);
    for(auto app:{std::make_pair(cpdlc(),"CPDLC"),std::make_pair(adsc(),"ADS-C"),std::make_pair(cm(true),"CM")}) {
        j=send(x25(compressed(cotp(app.first))));
        CHECK(j["protocol"]==app.second); CHECK(j["transport"]=="ATN");
        CHECK(j["decode_path"]["atn_x25"]==true); CHECK(j["decode_path"]["atn_clnp"]==true);
        CHECK(j["decode_path"]["atn_cotp"]==true); CHECK(j["decode_path"]["fans_cpdlc"]==false);
        CHECK(!j.contains("tail"));
    }
    // Classification/counters cannot treat a partial packet as an application.
    auto network=compressed(cotp(cpdlc())); auto cut=network.size()/2;
    j=send(x25(part(network,0,cut),0,true)); CHECK(j["protocol"]=="X.25");
    double firstTime=emitted.back().timestamp;
    j=send(x25(part(network,cut,network.size()),1));
    CHECK(j["protocol"]=="CPDLC"); CHECK(j["transport"]=="ATN");
    CHECK(j["timestamp"].get<double>()>=firstTime); CHECK(j["frame_type"]=="I");
    auto counts=channel.getProtocolCounters();
    CHECK(counts.acars==2); CHECK(counts.fansCpdlc==1); CHECK(counts.atnX25==5);
    CHECK(counts.atnClnp==4); CHECK(counts.atnCpdlc==2); CHECK(counts.atnAdsc==1); CHECK(counts.atnCm==1);
    j=send(x25(compressed(cotp(cmLogon()))),false);
    CHECK(j["protocol"]=="CM"); CHECK(j["flight"]=="UA0884"); CHECK(!j.contains("tail"));
    CHECK(contains(j["decoded"],"flight_id"));
    auto countBeforeError=channel.getProtocolCounters();
    j=send(x25(join({0xe0,0,0},compressed(cotp(cpdlcDown())))));
    CHECK(j["protocol"]=="SNDCF"); CHECK(j["decode_path"]["atn_cpdlc"]==false);
    CHECK(channel.getProtocolCounters().atnCpdlc==countBeforeError.atnCpdlc);
    // Embedded application is still preserved for inspection in the decoded tree.
    CHECK(contains(j["decoded"],"cpdlc"));
    // Native ACARS restores BCS/DEL for libacars, with no invented AVLC address.
    ACARSChannel native;
    native.setMessageCallback([&](const VDL2Message& m) { emitted.push_back(m); });
    ACARSChannelTestAccess::parse(native, fans);
    j=Json::parse(emitted.back().json_text);
    CHECK(j["protocol"]=="CPDLC"); CHECK(j["transport"]=="ACARS"); CHECK(j["direction"]=="AIR2GND");
    CHECK(j["tail"]=="N795UA"); CHECK(j["flight"]=="UA0884"); CHECK(!j.contains("src")); CHECK(!j.contains("frame_type"));
    ADSBChannel adsb;
    adsb.setMessageCallback([&](const VDL2Message& m) { emitted.push_back(m); });
    ADSBChannelTestAccess::parse(adsb,{0x8d,0xaa,0xcc,0x5b,0x08,0,0,0,0,0,0,0,0,0});
    j=Json::parse(emitted.back().json_text); CHECK(j["protocol"]=="ADS-B"); CHECK(j["transport"]=="1090ES");
    CHECK(j["decoded"]["adsb"]["address"]==0xaacc5b); CHECK(!j.contains("avlc"));
    ADSBChannelTestAccess::parse(adsb,{0x20,0xaa,0xcc,0x5b,0,0,0});
    j=Json::parse(emitted.back().json_text); CHECK(j["protocol"]=="Mode S"); CHECK(j["decoded"]["mode_s"]["df"]==4);
    // Display wording is deliberately replaced: classification/tree cannot depend on it.
    auto sample=emitted[emitted.size()-3]; auto before=Json::parse(sample.json_text);
    CHECK(before["decoded"]["acars"].is_object());
    // Unknown AVLC endpoint types must not be called ground stations.
    sample.src_addr=0x7000001; sample.dst_addr=0; sample.formatted_text="ADS-C CPDLC N12345";
    VDL2ProtocolDecoder::Result unknown;
    populateMessageJSON(sample,unknown,true);
    j=Json::parse(sample.json_text); CHECK(!j.contains("direction")); CHECK(!j["src"].contains("type"));
    CHECK(!j.contains("tail")); CHECK(j["protocol"]=="VDL2");
    // Large/escaped records and simultaneous channel writers with a live tail reader.
    auto large=Json::parse(emitted.front().json_text); large["text"]=std::string(20000,'X')+"\n\"\\\t";
    std::vector<std::string> records;
    for(const auto& m:emitted) records.push_back(m.json_text);
    records.push_back(large.dump());
    std::string path=(std::filesystem::temp_directory_path()/
        ("vdl2-jsonl-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".jsonl")).string();
    { std::ofstream create(path); }
    std::atomic<bool> done{false}, failed{false}; std::atomic<int> readCount{0};
    std::thread reader([&] {
        std::ifstream file(path,std::ios::binary); std::string pending; char buf[2048]; std::streamoff offset=0;
        while(true) {
            bool finishing=done.load();
            file.clear(); file.seekg(offset);
            file.read(buf,sizeof(buf)); auto n=file.gcount(); offset+=n; pending.append(buf,n); file.clear();
            size_t end;
            while((end=pending.find('\n'))!=std::string::npos) {
                auto parsed=Json::parse(pending.substr(0,end),nullptr,false);
                if(!parsed.is_object()) failed=true;
                pending.erase(0,end+1); ++readCount;
            }
            if(finishing && n==0) { if(!pending.empty()) failed=true; break; }
            if(n==0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    std::vector<std::thread> writers;
    for(int i=0;i<4;i++) writers.emplace_back([&,i] {
        for(int k=0;k<40;k++) if(!appendAviationJSONL(path,records[(k+i)%records.size()])) failed=true;
    });
    for(auto& t:writers) t.join(); done=true; reader.join();
    CHECK(!failed); CHECK(readCount==160);
    CHECK(!appendAviationJSONL(path,"not json")); CHECK(!appendAviationJSONL(path,"[]"));
    CHECK(!appendAviationJSONL(path+"/missing",records[0]));
    std::ifstream complete(path); std::string line; int lines=0;
    while(std::getline(complete,line)) { CHECK(Json::parse(line).is_object()); ++lines; }
    CHECK(lines==160); std::filesystem::remove(path);
}

int main() try {
    structuredTests();
    VDL2ProtocolDecoder d;
    Bytes transport=cotp({0xff,0,0x55});
    Bytes network=compressed(transport);
    auto j=decode(d,x25(network)); CHECK(contains(j,"cotp"));
    // Sequential X.25: duplicate, wrap, interleaved aircraft and circuits.
    d.reset(); CHECK(!contains(decode(d,x25(part(network,0,5),7,true)),"cotp"));
    CHECK(!contains(decode(d,x25(part(network,0,5),7,true)),"cotp"));
    CHECK(contains(decode(d,x25(part(network,5,network.size()),0)),"cotp"));
    d.reset();
    for(int src: {0x1000001,0x1000002}) for(int chan:{1,2})
        decode(d,x25(part(network,0,5),0,true,chan),1000,src);
    for(int src: {0x1000001,0x1000002}) for(int chan:{1,2})
        CHECK(contains(decode(d,x25(part(network,5,network.size()),1,false,chan),1001,src),"cotp"));
    d.reset(); decode(d,x25(part(network,0,5),0,true));
    CHECK(!contains(decode(d,x25(part(network,5,network.size()),4)),"cotp"));
    d.reset(); decode(d,x25(part(network,0,5),0,true));
    CHECK(!contains(decode(d,x25(part(network,5,network.size()),1),1010),"cotp"));
    d.reset(); decode(d,x25(part(network,0,5),0,true)); d.reset();
    CHECK(!contains(decode(d,x25(part(network,5,network.size()),1)),"cotp"));
    // Both offset engines: reversed arrival order and duplicate first fragment.
    for(bool normal:{false,true}) for(bool reversed:{false,true}) {
        d.reset();
        auto first=fragment(part(transport,0,3),0,transport.size(),true,normal);
        auto last=fragment(part(transport,3,transport.size()),3,transport.size(),false,normal);
        CHECK(!contains(decode(d,x25(reversed?last:first)),"cotp"));
        if(!reversed) CHECK(!contains(decode(d,x25(first)),"cotp"));
        CHECK(contains(decode(d,x25(reversed?first:last)),"cotp"));
    }
    // Normal and compressed CLNP with the same PDU ID cannot share a table.
    d.reset();
    decode(d,x25(fragment(part(transport,0,3),0,transport.size(),true,false)));
    decode(d,x25(fragment(part(transport,0,3),0,transport.size(),true,true)));
    CHECK(contains(decode(d,x25(fragment(part(transport,3,transport.size()),3,transport.size(),false,false))),"cotp"));
    CHECK(contains(decode(d,x25(fragment(part(transport,3,transport.size()),3,transport.size(),false,true))),"cotp"));
    // Nested error reports are bounded, even for a large valid-link payload.
    Bytes nested;
    for(int n=0;n<1000;n++) nested=join(nested,{0xe0,0,0});
    decode(d,x25(nested));
    // ES-IS and X.25 Fast Select/SNDCF.
    CHECK(contains(decode(d,x25({0x82,11,1,0,4,0,30,0,0,1,0x47})),"esis"));
    CHECK(contains(decode(d,join({0x10,1,0x0b,0,0,0xc1,4,1,0,0,0},network)),"cotp"));
    for(bool ground:{false,true}) {
        d.reset(); auto app=cm(ground);
        CHECK(contains(decode(d,x25(compressed(cotp(app))),1000,1,2,ground),"context_mgmt"));
    }
    d.reset(); CHECK(contains(decode(d,x25(compressed(cotp(cpdlcDown())))),"cpdlc"));
    for(auto appKey:{std::make_pair(cpdlc(),"cpdlc"),std::make_pair(adsc(),"adsc_v2")}) {
        d.reset(); auto app=appKey.first;
        auto r=d.decode(x25(compressed(cotp(app))).data(),x25(compressed(cotp(app))).size(),2,1,true,1000,false);
        CHECK(contains(Json::parse(r.json),appKey.second)); CHECK(!r.text.empty());
        // COTP reassembly must withhold the application until EOT.
        d.reset(); size_t cut=app.size()/2;
        CHECK(!contains(decode(d,x25(compressed(cotp(part(app,0,cut),0,true))),1000,2,1,true),appKey.second));
        CHECK(contains(decode(d,x25(compressed(cotp(part(app,cut,app.size()),1))),1001,2,1,true),appKey.second));
    }
    // Every prefix and deterministic noise must serialize and release cleanly.
    auto valid=x25(compressed(cotp(cpdlc())));
    for(size_t n=1;n<valid.size();n++) { d.reset(); decode(d,part(valid,0,n)); }
    std::mt19937 rng(12345);
    for(int n=0;n<2000;n++) {
        Bytes b(1+rng()%128); for(auto& v:b) v=rng();
        d.reset(); decode(d,b);
        d.reset(); decode(d,x25(b));
        d.reset(); decode(d,x25(compressed(cotp(b))),1000,2,1,true);
    }
    VDL2Channel channel; int calls=0;
    channel.setMessageCallback([&](const VDL2Message& m) {
        calls++; auto out=Json::parse(m.json_text);
        CHECK(out["avlc"]["src"]==0x4000002); CHECK(out["avlc"]["dst"]==0x1000001);
        CHECK(out["timestamp"]==m.timestamp); CHECK(m.timestamp>0);
        CHECK(contains(out,"context_mgmt")); CHECK(!m.formatted_text.empty());
    });
    auto frame=avlc(x25(compressed(cotp(cm(true)))));
    VDL2ChannelTestAccess::parse(channel,frame); CHECK(calls==1);
    frame.back()^=1; VDL2ChannelTestAccess::parse(channel,frame); CHECK(calls==1);
    channel.setMessageCallback([&](const VDL2Message& m) {
        calls++; auto out=Json::parse(m.json_text);
        CHECK(out["avlc"]["frame_type"]=="I"); CHECK(out["is_acars"]==m.is_acars);
    });
    // Short X.25 control and malformed ACARS still get valid structured output.
    VDL2ChannelTestAccess::parse(channel,avlc({0x10,1,1}));
    VDL2ChannelTestAccess::parse(channel,avlc({0xff,0xff,1,0})); CHECK(calls==3);
    std::cout<<"VDL2 protocol, application, reassembly, malformed-input and callback checks passed\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
