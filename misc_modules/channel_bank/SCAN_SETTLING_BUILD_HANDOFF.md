# Channel Bank scan settling

## Integration history

- The user confirmed the initial 250 ms fix worked. It was integrated into
  `integration/main` at `4c1c8507`, including the multi-receiver scanner at
  `51617820`.
- The fixed 350 ms follow-up was approved and integrated at `28da0509`.
- Follow-up `0181f692`, now on `integration/main`, makes settling
  configurable and returns the default and minimum to 250 ms. This is the
  lowest setting the user confirmed working; lower values have not been
  established as reliable.

The user uses a separate session for application builds and hardware testing.
The integration baseline includes the configurable setting and the approved
receiver-status display. Build from `integration/main` to include both.
No core, source-driver, or native iPhone UI changes are included in this follow-up.

## Setting

The desktop range Scan, Multi-Receiver Scan, and bookmark scan panels expose
**Scan settling (ms)**. The range is **250–2,000 ms**, default **250 ms**. It is
also available in the built-in web controls and settings API as `scanSettleMs`
(integer milliseconds). API values are clamped to the same bounds and
non-integer values are rejected before settings are applied.

The setting is saved in the module configuration and named profiles. Existing
configurations/profiles without this field use 250 ms. A change while running
applies to the next acknowledged retune; it does not shorten or extend a
settling interval already in progress. Logs report the interval actually used
for that tune, even if the setting has subsequently changed.

This is the settling delay before detection, not the total dwell at a scan
stop. Three fresh FFT frames are still required after settling. **No Signal
Skip** continues to control the empty-stop detection dwell, starting at the
first post-settling frame. **Quiet Timeout** retains its existing meaning.

## Retune and multi-receiver behavior

- A requested scan hop invalidates discovery results and pauses discovery
  analysis until the source retune callback acknowledges tuning.
- The detector waits for both the configured wall-time interval and the
  corresponding number of discarded IQ samples. It discards the complete
  boundary block, then collects at least three new FFT frames before making
  scan decisions. Without arriving IQ, it stays at the unmeasured stop.
- Multi-Receiver Scan retains advancement after a fresh detection. Receiver
  observations, release timers and recording management continue throughout
  discovery settling.
- Discovery retunes preserve independently hosted VFOs, open files, detector
  observations and allocator assignments.
- FFT collection and tune invalidation are serialized. The detector center
  follows the acknowledged tune rather than the requested GUI center. Local
  sink teardown occurs outside the analysis lock so IQ can drain.
- Stationary Auto/Manual modes do not receive the scan settling delay.

The interval is a practical guard, not a hardware timestamp or a measurement
of a particular driver's buffered latency. Reliability remains receiver- and
buffering-dependent; increase the setting if hardware testing calls for it.

## Validation and build handoff

The configurable-setting follow-up originally used a source-only handoff.
Integration validation on 2026-09-23 subsequently built `sdrpp`, `channel_bank`,
and both Channel Bank test targets in a fresh ARM64 Release build; allocator
and scan-readiness CTests passed (2/2). Packaging and RF checks remain with the
separate build session. The regression source covers the
250 ms default, 350 ms selection, longer settling, and lower/upper bounds, in
addition to the original fresh-frame and independent-slot preservation cases.
The original standalone readiness test passed during the initial investigation;
the user subsequently reported the initial fix working on hardware.

Follow `MULTI_RECEIVER_SCAN_BUILD_HANDOFF.md` for the full app build with
`BUILD_TESTING=ON`. Include `channel_bank_scan_readiness_test` and run CTest in
the Channel Bank build directory. Use a fresh build directory: the earlier
`build-scan-settling` configure was incomplete after Brown DSD dependency
bootstrap encountered generated dependency directories in its filename glob.

Check desktop changes and restart/profile persistence at 250, 350, and 1,000 ms.
Confirm a live change applies at the next hop and API bounds match desktop
bounds. Compare stationary Auto with range Scan on known active frequencies;
compare empty-stop No Signal Skip at 0.1 s and 2 s, allowing for settling and
fresh-frame time. In Multi-Receiver Scan, verify discovery continues hopping
while other receivers' recordings remain open and audible. Preserve and check
carrier centering, adjacent-dispatch suppression, multiple channels on one
receiver, no-capacity behavior, blocking/suppression, maximum-monitor release,
and stop/restart. Keep the test app separate from known-good apps until accepted.
