# Sample accounting and IQ continuity test branch

Base: tested buffer-safety revision cc5e9755. This branch deliberately excludes
the experimental AM processor. The IQ continuity fix preserves pending blocks
across splitter membership changes and aligns blocked-channel admission with
the stored grid identity. The demodulation, gain, fades, pre-roll,
recording lifecycle, normalization, and encoding code paths remain unchanged.

Build the desktop app normally. Build-only handoff: do not deploy or replace
production automatically. Keep the RX888 driver identical to the comparison
app and record its commit/hash. Source-module and bundled-driver versions are
separate; an app commit alone does not identify the latter.

## Collect a short observation on RadioMac

Launch the diagnostic app's actual executable from its Contents/MacOS directory
with SDRPP_CB_SAMPLE_DIAGNOSTICS=1 in its environment, the intended --root, and
stdout/stderr redirected to a log. Use the same receiver and Channel Bank
settings as the failing run. Merely opening an app via Finder does not forward
a Terminal environment variable. The separate build session should supply the
exact launch command for its final bundle/profile; don't guess a production path.

Record a short affected transmission and note its frequency and clock time.
Then stop Channel Bank normally so its slots are torn down and summaries are
emitted. No per-block log or additional audio/IQ file is written. Each slot
emits a start line and four '[CB samples]' summary lines. Keep the whole log,
including any 'RX888: stream overflow' entries, and the matching recording.
If no '[CB samples] start' appears, instrumentation was not enabled.

The default (environment unset) uses the original stream/VFO implementations.
Enabled measurements add clocks and integer counters per block. They do not
insert a queue, change thread priority, or alter samples. Timings are diagnostic
observations, not zero-overhead production profiling.

## Interpret conservatively

- IQ and recording-feed published/received/released samples and blocks audit
  the local synchronous stream handoffs. Repeated reads and unpaired flushes
  flag protocol misuse. All counters are read only after both ends stop.
- Shutdown may leave a published block unread or cancel an in-progress publish.
  These values are reported separately and are NOT labeled runtime drops.
  In the continuity-fixed splitter, a cancelled IQ publish is retried for
  retained consumers; cancellation counts alone no longer mean lost samples.
- VFO input and generated output can be compared with input * 48000 / input_rate.
  Resampler phase/rounding can leave a small fractional/block-edge difference.
  Published VFO output can exceed received recording audio at shutdown because
  intermediate stages may still contain blocks. Do not call that a live loss.
- Handler input = warmup + no_file + trim + eligible. Those first three paths
  are intentional omissions. write_requested counts samples submitted to the
  existing WAV writer, including pre-roll, NOT confirmed disk writes.
- With no RNNoise state, write_requested = eligible + preroll. When RNNoise or
  voice gating creates an RNNoise state, its 480-sample framing and discarded
  partial frames at file boundaries must be considered. Counters span the slot,
  potentially multiple files. Existing pre-roll may repeat part of the opening
  block: this diagnostic branch observes it without repairing it.
- max_publish_ms is producer handoff waiting; max_hold_ms is consumer ownership
  duration. Long waits or callback times can expose backpressure but do not
  themselves establish missing samples. No deadline threshold is assumed.
- The existing RX888 overflow message means its driver reported an overflow.
  Absence of that message cannot prove the driver/USB path lost nothing.
  These counters cannot detect missing IQ before this slot, same-length content
  corruption/repetition, or prove that a waveform click is a missing sample.
  Those outcomes need driver sequence/timestamp evidence or paired IQ/audio
  captures as a follow-up.

## Validation

Build channel_bank and channel_bank_sample_diagnostics_test. Run the latter
through CTest. It exercises real threaded stream handoffs with changing block
sizes, checks exact payload order and accounting, deliberately repeats a read,
and verifies cancelled writes are separate from successful publications.
Hardware evidence remains necessary; passing this test does not establish that
RadioMac is or is not dropping samples.


## Build-session handoff: wd/channel-bank-iq-continuity

Build the tip of this branch, which includes corrected diagnostic log formatting.
The fix is desktop Channel Bank only. It does not modify the core splitter,
RX888 source/driver, demodulation, NR, normalization, or encoding. The module-local
IQ splitter retains both the pending input and which outputs already received
it when membership changes interrupt publication. Removed consumers need not
receive remaining data; new consumers may join the retained current block.
An upstream block is flushed only after all current outputs have received it.
Normal synchronous backpressure is retained; there is no dropping queue.

Use a separate named test bundle and record the source commit and bundled SDDC
driver hash in your build notes. Keep the same dependencies/driver as the previous
diagnostic test build so this comparison isolates the source fix. Preserve
SDR++_CURRENT.app as the known-good fallback. The user performs RadioMac testing;
do not deploy, launch, or change the production profile automatically.

Enable SDRPP_CB_SAMPLE_DIAGNOSTICS=1 and capture the app's actual log. If the
launcher sets its own log path or profile, document that path and the effective
--root; a shell redirection may otherwise produce an empty Desktop log. Do not
ask the user to add a second --root to a launcher without verifying its behavior.

Validation already performed: channel_bank builds, all five Channel Bank CTests
pass. The new threaded regression forces 40 add/remove membership changes during
a blocked publication, verifies pending block delivery to surviving consumers,
and verifies consumers already served do not receive duplicates. It also tests
the two off-grid frequencies involved in the observed blocked-channel loop.
This is source/unit proof, not a claim that RadioMac's clicks are fixed.

For RadioMac acceptance: use the same channel/receiver settings, listen to fresh
recordings, stop Channel Bank normally, and retain the log. The repeated blocked
spawn/destroy loop should cease. Check sample accounting and whether clicks
improve. Cancelled IQ attempts can remain nonzero because the fixed splitter now
retries them; inspect delivered totals rather than treating attempts as drops.
