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

## Structured JSON version 2

Every emitted aviation message owns its `json_text` before leaving the decoder
through the existing callback. `formatted_text` remains the display string.
The output callback writes `json_text`; it does not rebuild or truncate it.

```json
{
  "schema_version": 2,
  "timestamp": 1789423200.125,
  "freq": 136975000,
  "protocol": "CPDLC",
  "transport": "ATN",
  "direction": "AIR2GND",
  "src": { "type": "AIR", "address": "AACC5B" },
  "dst": { "type": "GND", "address": "102C3A" },
  "snr": 28.0,
  "fec": 0,
  "ppm": -0.7,
  "frame_type": "I",
  "decoded": {},
  "text": "Human-readable decoded message"
}
```

This example abbreviates `decoded`. Real packets retain the **complete** nested
libacars/dumpvdl2 JSON tree there: `x25`, `clnp`, `cotp`, `x225_spdu`, `x227_apdu`,
`cpdlc`, `adsc_v2`, `context_mgmt`, `acars`, `arinc622`, etc. COTP uses `pdu_list`.
Unknown bytes, error flags, integrity-check results where available, and
reassembly statuses remain in the tree. The schema does not imply that an
application integrity field was validated merely because decoding succeeded.

`timestamp` is Unix seconds with a fractional part; `freq` is integer Hz,
`snr` is dB, `fec` is the correction count, and `ppm` is frequency error in ppm.
`src.address`/`dst.address` are uppercase six-digit hexadecimal addresses. AIR
means AVLC type 1; GND means type 4 or 5. Other address types have no invented
classification or direction. Known opposite endpoints produce `AIR2GND` or
`GND2AIR`. Full 28-bit addresses, numeric address types and control are retained
in the additional `avlc` object and `VDL2Message` fields. S/U frames have
`protocol: VDL2`, their actual frame type and an empty decoded tree.

Classification uses type-descriptor identity and the decoded C structures,
never display text or regexes:

| Decoded path | protocol | transport |
| --- | --- | --- |
| Ordinary ACARS | ACARS | ACARS |
| ACARS / ARINC 622 / FANS CPDLC | CPDLC | ACARS |
| ACARS ADS-C | ADS-C | ACARS |
| X.25 / CLNP / COTP / ATN CPDLC | CPDLC | ATN |
| X.25 / CLNP / COTP / ATN ADS-C v2 | ADS-C | ATN |
| X.25 / CLNP / COTP / Context Management | CM | ATN |
| Network/control packet without application | X.25, CLNP, COTP, ES-IS, or SNDCF | ATN |
| Generic AVLC or unrecognized payload | VDL2 | omitted |

Only successfully decoded application nodes qualify. Embedded erroneous packets
in SNDCF/CLNP error reports remain in `decoded` but are not counted or classified
as new applications. Incomplete fragments retain their last decoded network
layer; the completing fragment carries the application and its own receive
metadata. Do not filter network/control frames as application messages.

`tail` comes from the decoded ACARS registration field (padding dots removed).
`flight` comes from ACARS `flight_id`, or an ATN CM logon request's typed
`aircraftFlightIdentification`. Neither is inferred from addresses, cached across
unrelated messages, or extracted from text. If absent in this message, it is
omitted. Native VHF ACARS restores the separately stored BCS and consumed DEL
before libacars parsing and uses its block ID for direction; AVLC fields do not
apply there. Existing ADS-B/Mode S callback producers also populate the envelope
from their decoded state so sharing the writer does not discard their records.

Version 2 replaces version 1's `frequency_hz`, `snr_db`, `fec_corrections`,
`ppm_error`, and `protocols` keys with `freq`, `snr`, `fec`, `ppm`, and `decoded`.
It also replaces the old file-only timestamp string/MHz/text-only object.
Consumers should use `schema_version`, accept missing optional fields and ignore
unknown keys. No parsing of `text` is needed for classification or filtering.

### Path instrumentation

Each envelope has a `decode_path` object containing booleans `acars`,
`fans_cpdlc`, `atn_x25`, `atn_clnp`, `atn_cotp`, `atn_cpdlc`, `atn_adsc`, `atn_cm`.
The last three require successful, ordered X.25 → CLNP → COTP → application
nodes from the non-ACARS entry point. A FANS message cannot increment ATN counters.

`VDL2Channel::getProtocolCounters()` exposes atomic lifetime counters for ACARS,
FANS CPDLC, ATN X.25, CLNP, CPDLC, ADS-C and CM. They are displayed only in the
existing collapsed Debug Stats panel. Reassembly reset does not erase lifetime
counts. Counters measure successful decodes, not unique aircraft or deduplicated
network retransmissions. Native ACARS uses the same classifier internally.

### JSONL output

The existing configurable file output retains `/tmp/aviation_messages.jsonl`
as its default. Enable **JSON File** in the module's existing output controls.
Each record is one compact JSON object plus a newline, including records larger
than 4 KB and text containing quotes/newlines. No enclosing array is used.

The writer validates objects, serializes writers across channels and module
instances, and appends without truncating existing content. On POSIX it uses
`O_APPEND`, a cooperative `flock`, retries interrupted/partial writes and attempts
to roll back a failed partial append while holding the lock. Successful writes
are immediately readable without waiting on a buffered stream. Windows uses a
binary append stream flushed per record and a process-wide mutex. File settings
are synchronized with the callback; failures increment a diagnostic counter.

Readers must buffer until newline before parsing: reads can split any record.
This is a streaming file, not a transactional store: abrupt termination/storage
failure can leave an incomplete final line. No crash-durability/fsync guarantee is
made, and unrelated writers must respect the same locking convention. Existing
UDP/SQLite controls are untouched apart from consuming the structured string;
no new network/database/dashboard functionality is implemented.

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

## Structured-output validation (2026-09-15)

The expanded tests exercise actual callback envelopes for generic I/S/U frames,
ordinary ACARS, valid-CRC FANS CPDLC, ATN CPDLC/ADS-C/CM, CM logon flight identity,
X.25 fragmentation with completing-frame metadata, and native VHF ACARS/FANS.
They assert path counters, absence of guessed identities, and separation of
embedded error-report applications. Four concurrent writers append 160 records
while a reader tails incrementally, including escaped strings and >20 KB records;
every complete line is independently parsed. Invalid objects and write failures
are also checked. The original protocol/reassembly/malformed-input tests remain.

Final result: Release CTest passed; the expanded suite passed under ASan with no
memory diagnostics. Combined UBSan again reported only the two previously noted
signed-shift findings in unchanged libacars ASN.1 runtime code. `main.cpp` also
passed a C++17 syntax check against the checkout's core headers on macOS arm64
(existing core-header warnings remain). No installed-app or live-radio run was
performed for this structured-output update.
