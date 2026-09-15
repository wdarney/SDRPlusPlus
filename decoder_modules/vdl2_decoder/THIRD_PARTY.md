# VDL2 upper-layer licensing and provenance

## Decision

The requested `wdarney/SDRPlusPlus` baseline, `codex/vdl2-performance` at
`4260caac1e624e1d09a5485c42465bc22b3a1b15`, contains the GNU GPL version 3 in
its root `license` file. No separate license or exception was found in
`decoder_modules/vdl2_decoder`. Consequently, incorporating a focused subset
of dumpvdl2's GPL-3.0-or-later protocol implementation is compatible with this
GPLv3 repository; this does not convert a permissively licensed SDR++ module
into a GPL module. The licensing decision was reported before incorporation.
Retain the upstream notices and provide corresponding source when distributing
binaries under the applicable GPL obligations. This decision does not license
unrelated third-party components or change their licenses.

## Source snapshot

- Upstream: https://github.com/szpajder/dumpvdl2
- Commit: `686e87f2294ef6275855ba7364bd4bb9a0f8f56a` (master inspected 2026-09-15 UTC).
- Protocol implementation: Copyright 2017–2026 Tomasz Lemiech, GPL-3.0-or-later.
- License copy: `protocol/COPYING`.
- libacars: existing `core/libacars`, version 2.2.1, MIT (`LICENSE.md`).
- Generated ASN.1 files retain their generator/source notices. The additional
  asn1c support types retain their original BSD-style notices where present.
  The repository-level GPL distribution terms also apply to the incorporated
  subset; generated types are not represented as an independently permissive
  replacement implementation.

The AVLC, X.25, CLNP, COTP, ES-IS, ATN, offset reassembly, and TLV implementations
and headers were inspected before selecting the dependency set.

## Included dependency set

`protocol/` contains X.25/SNDCF, normal and compressed CLNP, ES-IS, COTP, ATN
security labels, sequential-reassembly integration, offset reassembly, TLVs,
ICAO session/presentation/ACSE dispatch, ATN CPDLC B1/B2, ADS-C v2 and CM types,
and text/JSON formatters. Application schemas are interdependent: the generated
ATN type set is required by the application decoder and formatter tables.

The existing libacars supplies sequential reassembly (`la_reasm_ctx`), protocol
trees, JSON/text primitives, common ASN.1 formatters and the shared asn1c runtime.
Its CPDLC/ADS-C APIs decode the FANS/ACARS variants, not the ATN OSI variants;
calling those APIs on raw COTP payloads would not implement ATN decoding.
The 29 duplicate ASN.1 runtime `.c` files and shared headers are not imported.
A transitive include/source dependency check also excludes the unused
`AircraftIdentificationO` type.

Not imported: dumpvdl2's demodulation, FEC, CRC, HDLC, AVLC, receiver drivers,
CLI, queues, output servers, station databases, StatsD, or IDRP decoder.
IDRP remains explicitly opaque (`unknown_proto`). No GLib dependency is added;
the two temporary TSAP formatting buffers use libacars vstrings.

## Local adaptations

- A narrow `dumpvdl2.h` adapter replaces upstream application globals with
  immutable parser policy and helper declarations. Partial fragments are not
  decoded as complete applications.
- CMake builds only the protocol subset, with hidden symbols and PIC, and
  links the existing `acars_static` target. No second ASN.1 runtime is linked.
- Failed sequential reassembly (including missing/out-of-sequence fragments)
  is not passed on as a complete higher-layer packet.
- X.25 keys retain the directional AVLC address pair and additionally include
  the logical channel, avoiding interference between simultaneous circuits.
- Normal CLNP passes the assembled buffer/length to its child decoder. The
  upstream snapshot retrieved it but then passed the original fragment.
- Compressed CLNP creates its reassembly table with the same identifier used
  for lookup; upstream created it with the normal-CLNP identifier.
- The hex formatter uses a size_t iterator for reassembled buffers.
- Nested error-report decoding is bounded to 16 layers.
- Normal CLNP validates header and segment boundaries before parsing addresses,
  options, or payload. X.25 validates SNDCF availability before reading version.

Changes are limited to upper-layer decoding and its integration. Existing
D8PSK, FEC, HDLC, CRC and AVLC address extraction remain in SDR++.
