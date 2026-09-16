#include "vdl2_protocol.h"
#include <cmath>
#include <vector>
#include <atomic>
extern "C" {
#include <libacars/acars.h>
#include "../protocol/x25.h"
#include "../protocol/clnp.h"
#include "../protocol/cotp.h"
#include "../protocol/esis.h"
#include "../protocol/asn1-util.h"
#include "../protocol/asn1/CMAircraftMessage.h"
#include <libacars/cpdlc.h>
#include <libacars/adsc.h>
extern la_type_descriptor const proto_DEF_X25_pkt, proto_DEF_clnp_pdu;
extern la_type_descriptor const proto_DEF_clnp_compressed_data_pdu, proto_DEF_cotp_concatenated_pdu;
extern la_type_descriptor const proto_DEF_X25_SNDCF_error_report;
extern la_type_descriptor const proto_DEF_cpdlc, proto_DEF_adsc_v2, proto_DEF_cm, proto_DEF_esis_pdu;
}

struct VDL2ProtocolDecoder::Impl {
    std::atomic<uint64_t> acars{0}, fansCpdlc{0}, x25{0}, clnp{0}, cpdlc{0}, adsc{0}, cm{0};
    reasm_contexts contexts{la_reasm_ctx_new(), reasm_ctx_new()};
    ~Impl() {
        la_reasm_ctx_destroy(contexts.seqbased);
        reasm_ctx_destroy(contexts.offsetbased);
    }
};

VDL2ProtocolDecoder::VDL2ProtocolDecoder() : impl(new Impl) {}
VDL2ProtocolDecoder::~VDL2ProtocolDecoder() = default;
void VDL2ProtocolDecoder::reset() {
    la_reasm_ctx_destroy(impl->contexts.seqbased);
    reasm_ctx_destroy(impl->contexts.offsetbased);
    impl->contexts = {la_reasm_ctx_new(), reasm_ctx_new(), 0};
}
VDL2ProtocolDecoder::Counters VDL2ProtocolDecoder::counters() const {
    return {impl->acars.load(), impl->fansCpdlc.load(), impl->x25.load(), impl->clnp.load(),
            impl->cpdlc.load(), impl->adsc.load(), impl->cm.load()};
}

// Registration padding is part of the decoded ACARS field, not display text.
static std::string identifier(const char* data, size_t size, bool registration=false) {
    std::string s(data, size);
    auto end = s.find('\0'); if(end != std::string::npos) s.resize(end);
    while(!s.empty() && s.back() == ' ') s.pop_back();
    auto first = s.find_first_not_of(registration ? ". " : " ");
    return first == std::string::npos ? std::string{} : s.substr(first);
}

static void classify(la_proto_node* tree, bool acars, VDL2ProtocolDecoder::Result& r) {
    r.transport = acars ? "ACARS" : "";
    // Walk the actual decoder nodes in order. Application classification requires
    // the matching successful parent path, not a label or a formatted string.
    for(auto* n=tree; n; n=n->next) {
        if(!n->data) continue;
        if(acars) {
            if(n->td == &la_DEF_acars_message) {
                auto* a=static_cast<la_acars_msg*>(n->data);
                if(!a->err) {
                    r.acars=true; r.protocol="ACARS";
                    r.tail=identifier(a->reg,sizeof(a->reg),true);
                    r.flight=identifier(a->flight_id,sizeof(a->flight_id));
                }
            } else if(r.acars && n->td == &la_DEF_cpdlc_message &&
                      !static_cast<la_cpdlc_msg*>(n->data)->err) {
                r.protocol="CPDLC"; r.fansCpdlc=true;
            } else if(r.acars && n->td == &la_DEF_adsc_message &&
                      !static_cast<la_adsc_msg_t*>(n->data)->err) r.protocol="ADS-C";
            continue;
        }
        // Embedded erroneous packets are diagnostic evidence, not a new message.
        if(n->td == &proto_DEF_X25_SNDCF_error_report) { r.protocol="SNDCF"; break; }
        if(n->td == &proto_DEF_clnp_pdu && !static_cast<clnp_pdu_t*>(n->data)->err &&
           static_cast<clnp_pdu_t*>(n->data)->hdr->type == CLNP_NDPU_ER) {
            if(r.x25) { r.clnp=true; r.protocol="CLNP"; }
            break;
        }
        if(n->td == &proto_DEF_X25_pkt && !static_cast<x25_pkt_t*>(n->data)->err) {
            r.x25=true; r.transport="ATN"; r.protocol="X.25";
        } else if(r.x25 && ((n->td == &proto_DEF_clnp_pdu && !static_cast<clnp_pdu_t*>(n->data)->err) ||
                  (n->td == &proto_DEF_clnp_compressed_data_pdu && !static_cast<clnp_compressed_data_pdu_t*>(n->data)->err))) {
            r.clnp=true; r.protocol="CLNP";
        } else if(r.x25 && n->td == &proto_DEF_esis_pdu && !static_cast<esis_pdu_t*>(n->data)->err) {
            r.protocol="ES-IS";
        } else if(r.clnp && n->td == &proto_DEF_cotp_concatenated_pdu) {
            auto* last=static_cast<la_list*>(n->data);
            while(last->next) last=last->next;
            r.cotp=last->data && !static_cast<cotp_pdu_t*>(last->data)->err;
            if(r.cotp) r.protocol="COTP";
        } else if(r.x25 && r.clnp && r.cotp &&
                  (n->td == &proto_DEF_cpdlc || n->td == &proto_DEF_adsc_v2 || n->td == &proto_DEF_cm)) {
            auto* a=static_cast<asn1_pdu_t*>(n->data);
            if(!a->type || !a->data) continue;
            r.atnCpdlc=n->td == &proto_DEF_cpdlc;
            r.atnAdsc=n->td == &proto_DEF_adsc_v2;
            r.atnCm=n->td == &proto_DEF_cm;
            r.protocol=r.atnCpdlc ? "CPDLC" : r.atnAdsc ? "ADS-C" : "CM";
            if(r.atnCm && a->type == &asn_DEF_CMAircraftMessage) {
                auto* cm=static_cast<CMAircraftMessage_t*>(a->data);
                if(cm->present == CMAircraftMessage_PR_cmLogonRequest) {
                    auto& id=cm->choice.cmLogonRequest.aircraftFlightIdentification;
                    if(id.buf && id.size>0) r.flight=identifier(reinterpret_cast<char*>(id.buf),id.size);
                }
            }
        }
    }
}

VDL2ProtocolDecoder::Result VDL2ProtocolDecoder::decode(
    const uint8_t* data, size_t length, uint32_t src, uint32_t dst,
    bool fromGround, double timestamp, bool acars) {
    Result result;
    if (!data || length == 0 || length > 65535 || !std::isfinite(timestamp) || timestamp < 0)
        return result;
    // Parsers retain views into this buffer; serialize and destroy the tree before it dies.
    std::vector<uint8_t> bytes(data, data + length);
    timeval time{};
    time.tv_sec = static_cast<decltype(time.tv_sec)>(timestamp);
    time.tv_usec = static_cast<decltype(time.tv_usec)>((timestamp - time.tv_sec) * 1e6);
    uint32_t type = fromGround ? MSGFLT_SRC_GND : MSGFLT_SRC_AIR;
    la_proto_node* tree = acars
        ? la_acars_parse_and_reassemble(bytes.data(), static_cast<int>(length),
            fromGround ? LA_MSG_DIR_GND2AIR : LA_MSG_DIR_AIR2GND, impl->contexts.seqbased, time)
        : x25_parse(bytes.data(), static_cast<uint32_t>(length), &type,
            &impl->contexts, time, src, dst);
    std::unique_ptr<la_proto_node, decltype(&la_proto_tree_destroy)> owner(tree, la_proto_tree_destroy);
    if (tree) {
        classify(tree, acars, result);
        if(result.acars) ++impl->acars;
        if(result.fansCpdlc) ++impl->fansCpdlc;
        if(result.x25) ++impl->x25;
        if(result.clnp) ++impl->clnp;
        if(result.atnCpdlc) ++impl->cpdlc;
        if(result.atnAdsc) ++impl->adsc;
        if(result.atnCm) ++impl->cm;
        la_vstring* text = la_proto_tree_format_text(nullptr, tree);
        if (text) { result.text = text->str; la_vstring_destroy(text, true); }
        la_vstring* json = la_proto_tree_format_json(nullptr, tree);
        if (json) { result.json = json->str; la_vstring_destroy(json, true); }
    }
    return result;
}
