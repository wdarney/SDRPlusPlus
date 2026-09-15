#include "vdl2_protocol.h"
#include <cmath>
#include <vector>
extern "C" {
#include <libacars/acars.h>
#include "../protocol/x25.h"
}

struct VDL2ProtocolDecoder::Impl {
    reasm_contexts contexts{la_reasm_ctx_new(), reasm_ctx_new()};
    ~Impl() {
        la_reasm_ctx_destroy(contexts.seqbased);
        reasm_ctx_destroy(contexts.offsetbased);
    }
};

VDL2ProtocolDecoder::VDL2ProtocolDecoder() : impl(new Impl) {}
VDL2ProtocolDecoder::~VDL2ProtocolDecoder() = default;
void VDL2ProtocolDecoder::reset() { impl.reset(new Impl); }

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
        la_vstring* text = la_proto_tree_format_text(nullptr, tree);
        if (text) { result.text = text->str; la_vstring_destroy(text, true); }
        la_vstring* json = la_proto_tree_format_json(nullptr, tree);
        if (json) { result.json = json->str; la_vstring_destroy(json, true); }
    }
    return result;
}
