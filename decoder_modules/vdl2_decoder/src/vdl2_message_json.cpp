#include "vdl2_message_json.h"
#include <json.hpp>
#include <cstdio>

static const char* addressType(uint32_t a) {
    switch((a >> 24) & 7) {
        case 1: return "AIR";
        case 4: case 5: return "GND";
        default: return nullptr;
    }
}
static nlohmann::json address(uint32_t a) {
    char hex[7]; snprintf(hex,sizeof(hex),"%06X",a & 0xffffff);
    nlohmann::json j={{"address",hex}};
    if(auto type=addressType(a)) j["type"]=type;
    return j;
}
void populateMessageJSON(VDL2Message& msg, const VDL2ProtocolDecoder::Result& decoded,
                         bool hasAVLC, const std::string& direction) {
    nlohmann::json j={
        {"schema_version",2}, {"timestamp",msg.timestamp}, {"freq",msg.freq},
        {"protocol",decoded.protocol}, {"snr",msg.snr}, {"fec",msg.num_fec_corrections},
        {"ppm",msg.ppm_error}, {"text",msg.formatted_text}, {"decoded",nlohmann::json::object()}
    };
    if(!decoded.transport.empty()) j["transport"]=decoded.transport;
    if(!decoded.tail.empty()) j["tail"]=decoded.tail;
    if(!decoded.flight.empty()) j["flight"]=decoded.flight;
    if(!direction.empty()) j["direction"]=direction;
    if(hasAVLC) {
        j["src"]=address(msg.src_addr); j["dst"]=address(msg.dst_addr);
        j["frame_type"]= (msg.avlc_control & 1)==0 ? "I" : (msg.avlc_control & 3)==3 ? "U" : "S";
        const char* src=addressType(msg.src_addr); const char* dst=addressType(msg.dst_addr);
        if(src && dst && std::string(src)!=dst) j["direction"]=std::string(src)+"2"+dst;
        // Retain full address bits and control for protocol inspection.
        j["avlc"]={{"src",msg.src_addr},{"dst",msg.dst_addr},
                   {"src_type",(msg.src_addr>>24)&7},{"dst_type",(msg.dst_addr>>24)&7},
                   {"control",msg.avlc_control},{"frame_type",j["frame_type"]}};
    }
    j["is_acars"]=msg.is_acars;
    j["decode_path"]={{"acars",decoded.acars},{"fans_cpdlc",decoded.fansCpdlc},
        {"atn_x25",decoded.x25},{"atn_clnp",decoded.clnp},{"atn_cotp",decoded.cotp},
        {"atn_cpdlc",decoded.atnCpdlc},{"atn_adsc",decoded.atnAdsc},{"atn_cm",decoded.atnCm}};
    if(!decoded.json.empty()) {
        auto tree=nlohmann::json::parse(decoded.json,nullptr,false);
        if(tree.is_object()) j["decoded"]=std::move(tree);
        else j["serialization_error"]="Invalid protocol JSON";
    }
    msg.json_text=j.dump(-1,' ',false,nlohmann::json::error_handler_t::replace);
}
