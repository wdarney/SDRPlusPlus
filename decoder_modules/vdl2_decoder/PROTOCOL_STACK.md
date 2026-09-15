# VDL2 upper-layer decoding

`VDL2Channel::parseAVLC()` still checks the FCS and invokes the existing message
callback. For I-frames it now calls a channel-owned `VDL2ProtocolDecoder`:

```
AVLC I-frame
  FF FF 01 -> libacars ACARS (with sequential reassembly)
  otherwise -> X.25 / SNDCF -> CLNP or compressed CLNP / ES-IS
                           -> COTP -> session / presentation / ACSE
                                   -> ATN CPDLC / ADS-C v2 / CM
```

Each channel owns both sequential (`la_reasm_ctx`) and offset (`reasm_ctx`)
contexts. They survive individual packets and are destroyed on channel teardown
or reset/retune. The original receive timestamp and directional AVLC address
pair are passed through every reassembling layer. X.25 also keys on its logical
channel; CLNP uses the PDU ID and COTP the destination reference, as in the
reference implementation. Different channels never share reassembly state.
The callback for the completing fragment contains the assembled application
message and the completing fragment's receive metadata. Earlier fragments retain
protocol/reassembly information rather than being presented as full applications.

The input and assembled buffers remain alive until text/JSON formatting and
protocol-tree destruction have completed. The callback receives owned strings,
not pointers into parser memory. The caller must serialize calls on one channel,
as with its existing DSP state.

AVLC address extraction is unchanged. Direction now uses the decoded address
type in bits 24..26 (aircraft type 1), rather than the wire EA extension bit,
so the correct uplink/downlink ASN.1 schema is selected. Short X.25 control
I-frames are no longer excluded by the old ACARS-oriented minimum length gate.

## JSON version 1

Every callback, including S/U frames, has a JSON object in `json_text`:

```json
{
  "schema_version": 1,
  "timestamp": 1789423200.125,
  "frequency_hz": 136975000,
  "snr_db": 15.5,
  "fec_corrections": 0,
  "ppm_error": 0.0,
  "is_acars": false,
  "avlc": {
    "src": 16777217,
    "dst": 67108866,
    "src_type": 1,
    "dst_type": 4,
    "control": 0,
    "frame_type": "I"
  },
  "protocols": {}
}
```

`timestamp` is Unix seconds with a fractional part. `src`/`dst` are the full
28-bit decoded AVLC values; the low 24 bits are the address and bits 24..26
are the address type. `VDL2Message::src_addr` and `dst_addr` preserve the same
values for callback consumers.

`protocols` contains the nested libacars/dumpvdl2 tree, not a JSON-encoded string.
Typical keys are `x25`, `clnp`, `cotp`, `x225_spdu`, `x227_apdu`, `cpdlc`,
`adsc_v2`, `context_mgmt`, and `acars`. COTP concatenation uses `pdu_list`.
`err`, `reasm_status`, and `unknown_proto` preserve diagnostic/unsupported
content. Empty S/U payload trees are `{}`. The protocol formatter's field names
are retained; consumers should ignore unknown keys and tolerate missing layers.
`formatted_text` retains the existing radio/AVLC header and appends the decoded
tree. No dashboard or external transport is added.

## Focused verification

From the repository root:

```sh
cmake -S decoder_modules/vdl2_decoder/tests -B /tmp/vdl2-tests \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build /tmp/vdl2-tests --target protocol_tests -j8
ctest --test-dir /tmp/vdl2-tests --output-on-failure
```

The test project builds the baseline's vendored libacars directly using available
system optional dependencies, bypassing its incomplete vendored-zlib packaging.
The production module continues to use `acars_static` from the normal SDR++ build.
Tests exercise the real `vdl2_dsp.cpp` callback path as well as the protocol stack.
Application fixtures are generated PER payloads with an explicit ACSE application
selector, wrapped in COTP, CLNP and X.25. They validate decoding/formatting, not
live RF reception or application checksum authentication. As in dumpvdl2, an
ATN integrity field is decoded but is not claimed to be cryptographically verified.

Coverage includes normal/compressed CLNP out-of-order and duplicate fragments,
X.25 sequence wrap, flow isolation, duplicates, expiration/reset, COTP application
fragmentation, CPDLC uplink/downlink, CM uplink/downlink, ADS-C cancellation, ES-IS,
SNDCF Fast Select, malformed/truncated input, and AVLC callback/CRC behavior.
This is source-level protocol validation; hardware, Windows and packaged-app
runtime validation are separate.

## Validation results (2026-09-15 UTC)

The standalone Release build and CTest passed on macOS arm64. The same cases
passed under AddressSanitizer without memory errors, including 6,000 deterministic
malformed-input probes plus all prefixes of a CPDLC packet. Combined ASan/UBSan
reported two existing signed-shift issues in the unchanged libacars ASN.1 runtime:
`per_support.c:89` and `INTEGER.c:819`. Thus this is not a clean UBSan result.
No full SDR++ application build, packaged deployment, Windows run or live radio
validation is claimed. The historical baseline's vendored-zlib configure failures
(missing `zconf.h` and `test/`) remain outside this module change.
