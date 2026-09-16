# Focused dumpvdl2 protocol subset, sharing SDR++'s libacars/asn1c runtime.
include(TestBigEndian)
test_big_endian(IS_BIG_ENDIAN)
configure_file(config.h.in vdl2_protocol_config.h)
file(GLOB ATN_ASN_SOURCES CONFIGURE_DEPENDS "asn1/*.c")
add_library(vdl2_protocol STATIC
    x25.c clnp.c cotp.c esis.c atn.c reassembly.c tlv.c util.c
    icao.c asn1-util.c asn1-format-icao-text.c asn1-format-icao-json.c
    ${ATN_ASN_SOURCES})
set_target_properties(vdl2_protocol PROPERTIES
    C_STANDARD 11 POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
target_include_directories(vdl2_protocol PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/asn1"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../core/libacars/libacars/asn1")
target_link_libraries(vdl2_protocol PUBLIC acars_static)
