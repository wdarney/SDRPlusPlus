#include "vdl2_dsp.h"
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
    static void parse(VDL2Channel& c,Bytes b) { c.parseAVLC(b.data(),b.size(),15.5f); }
};
static Bytes avlc(Bytes payload) {
    // Encode address bits independently, including type and EA extension bit.
    auto address=[](uint32_t v) {
        uint32_t reversed=0; for(int n=0;n<28;n++) reversed=(reversed<<1)|((v>>n)&1);
        return Bytes{uint8_t((reversed&127)<<1),uint8_t(((reversed>>7)&127)<<1),
                     uint8_t(((reversed>>14)&127)<<1),uint8_t((((reversed>>21)&127)<<1)|1)};
    };
    Bytes b=join(join(address(0x1000001),address(0x4000002)),{0}); b=join(b,payload);
    uint16_t crc=0xffff; for(auto byte:b) { crc^=byte; for(int k=0;k<8;k++) crc=(crc>>1)^((crc&1)?0x8408:0); }
    crc^=0xffff; b.push_back(crc&255); b.push_back(crc>>8); return b;
}
int main() try {
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
