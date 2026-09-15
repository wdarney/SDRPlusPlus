// SDR++ protocol-only adapter for dumpvdl2. See THIRD_PARTY.md.
#pragma once
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <libacars/libacars.h>
#include <libacars/vstring.h>
#include <libacars/dict.h>
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#define nop() do {} while(0)
#define ASSERT(x) assert(x)
#define debug_print(...) nop()
#define debug_print_buf_hex(...) nop()
#define statsd_increment_per_msgdir(...) nop()
#define statsd_increment(...) nop()
#define statsd_set(...) nop()
#define XCALLOC(n,s) xcalloc((n),(s),__FILE__,__LINE__,__func__)
#define XREALLOC(p,s) xrealloc((p),(s),__FILE__,__LINE__,__func__)
#define XFREE(p) do { free(p); (p)=NULL; } while(0)
#define NEW(t,p) t *p = XCALLOC(1,sizeof(t))
#define UNUSED(x) (void)(x)
#define EOL(v) la_vstring_append_sprintf((v), "%s", "\n")
#define ONES(x) ~(~0u << (x))
#define SAFE_JSON_APPEND_STRING(v,n,val) do { if(val) la_json_append_string(v,n,val); } while(0)
// Immutable parser policy. Never decode an incomplete fragment as an application PDU.
static const struct { bool decode_fragments; bool dump_asn1; unsigned debug_filter; }
    Config = { false, false, 0 };
#define D_PROTO_DETAIL 0
// message filters
#define MSGFLT_ALL                  (~0)
#define MSGFLT_NONE                 (0)
#define MSGFLT_SRC_GND              (1 <<  0)
#define MSGFLT_SRC_AIR              (1 <<  1)
#define MSGFLT_AVLC_S               (1 <<  2)
#define MSGFLT_AVLC_U               (1 <<  3)
#define MSGFLT_AVLC_I               (1 <<  4)
#define MSGFLT_ACARS_NODATA         (1 <<  5)
#define MSGFLT_ACARS_DATA           (1 <<  6)
#define MSGFLT_XID_NO_GSIF          (1 <<  7)
#define MSGFLT_XID_GSIF             (1 <<  8)
#define MSGFLT_X25_CONTROL          (1 <<  9)
#define MSGFLT_X25_DATA             (1 << 10)
#define MSGFLT_IDRP_NO_KEEPALIVE    (1 << 11)
#define MSGFLT_IDRP_KEEPALIVE       (1 << 12)
#define MSGFLT_ESIS                 (1 << 13)
#define MSGFLT_CM                   (1 << 14)
#define MSGFLT_CPDLC                (1 << 15)
#define MSGFLT_ADSC                 (1 << 16)

// util.c
typedef struct {
	uint8_t *buf;
	size_t len;
} octet_string_t;

extern la_type_descriptor const proto_DEF_unknown;
void *xcalloc(size_t nmemb, size_t size, char const *file, int line, char const *func);
void *xrealloc(void *ptr, size_t size, char const *file, int line, char const *func);
uint16_t extract_uint16_msbfirst(uint8_t const *data);
uint32_t extract_uint32_msbfirst(uint8_t const *data);
void bitfield_format_text(la_vstring *vstr, uint8_t const *buf, size_t len, la_dict const *d);
void bitfield_format_json(la_vstring *vstr, uint8_t const *buf, size_t len, la_dict const *d, char const *key);

octet_string_t *octet_string_new(void *buf, size_t len);
octet_string_t *octet_string_copy(octet_string_t const *ostring);
int octet_string_parse(uint8_t *buf, size_t len, octet_string_t *result);
void octet_string_format_text(la_vstring *vstr, octet_string_t const *ostring, int indent);
void octet_string_as_ascii_format_text(la_vstring *vstr, octet_string_t const *ostring, int indent);
void octet_string_as_ascii_format_json(la_vstring *vstr, char const *key,
		octet_string_t const *ostring);
void octet_string_with_ascii_format_text(la_vstring *vstr, octet_string_t const *ostring, int indent);
void octet_string_destroy(octet_string_t *ostring);

char *hexdump(uint8_t *data, size_t len);
void append_hexdump_with_indent(la_vstring *vstr, uint8_t *data, size_t len, int indent);
la_proto_node *unknown_proto_pdu_new(void *buf, size_t len);

