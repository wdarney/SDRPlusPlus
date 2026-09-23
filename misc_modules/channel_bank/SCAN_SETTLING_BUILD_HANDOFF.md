# Scan settling test candidate

This source candidate is on `codex/channel-bank-scan-settling`, based on
`51617820` (`codex/channel-bank-multi-receiver-scan`). That baseline already
contains `integration/main` at `7ad6434c`. The two remote tips were fetched and
verified on 2026-09-22. No core or source-driver changes are part of this fix.

The user will have a separate session build the app and will perform runtime
and hardware testing. The user authorized pushing this candidate branch so the
build session can fetch it. **Do not integrate it into `integration/main` until
the user explicitly approves the fix after testing.**

## Behavior

- A requested scan hop immediately invalidates discovery results and pauses
  discovery analysis until the source's retune callback acknowledges tuning.
- The detector discards at least 350 ms of IQ samples and waits at least 350 ms
  of wall time after acknowledgement. It discards the complete boundary block,
  then collects at least three new FFT frames before scan decisions resume.
- No Signal Skip starts at the first post-settling FFT frame. Without arriving
  IQ, the scanner remains at the unmeasured stop rather than skipping it.
- Multi-Receiver Scan retains immediate advancement after a fresh detection;
  No Signal Skip remains an empty-stop dwell, not a mandatory delay after a hit.
  Receiver observations, release timers and recording management continue
  throughout discovery settling.
- Retune cleanup only destroys discovery-owned slots. Independent receivers
  keep their VFOs, open files, observations and allocator assignments.
- The detector center is updated by the acknowledged retune, not by reading the
  requested GUI center during analysis. FFT collection and invalidation are
  serialized; local sink teardown occurs outside that lock so IQ can drain.
- Ordinary range Scan and bookmark scan share this readiness guard. Stationary
  Auto/Manual modes do not receive the scan settling delay.

The 350 ms interval adds 100 ms to the user-tested initial 250 ms setting; it is not a hardware timestamp
or a measurement of a particular driver's buffered latency. It still needs RF
acceptance on the user's receiver setup. The desktop range-scan panel shows the
settling interval and fresh-frame requirement. Logs identify when a fresh FFT
window becomes ready, and why multi-receiver discovery advanced.

## Validation so far

The standalone `scan_readiness_test.cpp` passed at the original 250 ms setting before build/test work was
stopped at the user's request. It covers tune acknowledgement, both wall-time
and sample-count settling, three-frame readiness through the real FFT frame
collector, stop/restart, and independent recording-slot preservation using
the teardown helper called by production code. The later source-only cleanup
coordination has not been compiled or run.

The full app configure was attempted but did not complete: Brown DSD dependency
bootstrap encountered generated dependency directories in its filename glob.
No app build, packaging, launch, deployment, or RF validation was completed.
Use a fresh build directory in the build session. The existing allocator test
now explicitly retains assertions in Release builds.

## Build-session checks

Follow `MULTI_RECEIVER_SCAN_BUILD_HANDOFF.md` for the full app build, with
`BUILD_TESTING=ON`. Also build `channel_bank_scan_readiness_test`, then run CTest
in the Channel Bank build directory. Preserve the existing multi-receiver
source adapters, carrier centering, adjacent-dispatch suppression, gain
controls, and RX888 driver provenance when packaging.

For hardware acceptance, compare stationary Auto with range Scan on known
active frequencies. Compare empty-stop dwell at No Signal Skip 0.1 s and 2 s;
allow the additional settling and fresh-frame time. With Multi-Receiver Scan,
verify discovery continues hopping while recordings on other receivers remain
open and audible. Check several channels sharing a receiver, no free receiver,
blocked/suppressed candidates, maximum-monitor release, and stop/restart.
Keep this candidate separate from known-good apps until accepted.

## 350 ms follow-up

The user confirmed the initial fix worked and it was integrated at `4c1c8507`.
They subsequently requested a 350 ms settling interval. This candidate changes
both the wall-time and discarded-IQ requirements through the same constant;
the three-frame requirement and independent-receiver handling are unchanged.
The desktop interval display follows the constant automatically. Test boundary
fixtures were updated, but no build or tests were run for this follow-up, per
the source-only handoff workflow. Push the candidate for the build session;
wait for user acceptance before integrating this follow-up into main.
