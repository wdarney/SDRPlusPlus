# VDL2 shutdown ownership and regression tests

## Failure identified

The previous `ChannelSlot` retained one Handler across activations. Each
`startChannel()` called `Handler::init()` again. `Sink::init()` calls
`registerInput()`, which appends to `block::inputs`; it does not replace the old
registration. Stop deleted the VFO and its output stream, leaving that pointer in
the retained Handler. On restart, `block::doStop()` visited both the old freed
stream and the current stream. Locking a mutex in the freed stream can explain
the reported macOS `std::system_error`/abort.

A standalone reproduction using the actual core Handler and streams registered
two inputs after one restart. AddressSanitizer reported heap-use-after-free in
`dsp::block::doStop()` at the second Stop. This proves the ownership defect; it
does not prove that the supplied live crash had a prior restart. If a fresh
process still crashes on its first activation with this fix, capture that stack
and activation history separately.

The slots are a fixed array, so iteration does not invalidate them. Each channel
owns its own Handler and VFO output, rather than sharing those objects. Repeated
core VFO `stop()` calls are guarded by the block's running flag while the object
is alive; the stale Handler input registration was not protected by that guard.

## Ownership and shutdown order

`VDL2ChannelInput` constructs a fresh Handler for each activation and initializes
it exactly once. Its borrowed input stream belongs to the VFO and must remain
alive until `join()` returns. No DSP, FEC, HDLC, CRC, or protocol algorithms change.

Module-wide Stop uses these phases:

1. Reject new work on every channel; close the message callback gate and drain
   any already admitted output callback.
2. Stop writes on each private VFO output to wake a blocked producer.
3. Stop/join all Handler workers while their input streams and decoder objects
   are alive, then destroy the Handlers.
4. Delete VFOs. Existing VFO destruction stops/joins producers and unregisters
   their frontend connections before freeing their streams.
5. Reset decoders and protocol/reassembly state only after consumers have joined.
6. Close message outputs after all workers have finished.

Single-channel Stop uses the same input stop/join/release order. Repeated stops
see empty ownership. Cleanup does not depend on the module `running` or channel
`active` flags: partially constructed activations are also released. Startup
exceptions trigger cleanup and propagate; shutdown errors are not caught or
ignored. Module destruction calls Stop unconditionally.

The lifecycle mutex serializes control/UI operations. Workers never acquire it.
The callback mutex serializes output configuration and callbacks, and is released
before joining workers. Decoder mutexes protect UI stats reads against processing;
no stats lock is held across Stop. Channel callback captures remain valid until
all workers join. Shared UI counters use atomics.

## Validation (macOS arm64)

- Module `main.cpp` C++17 syntax check passed.
- Existing protocol/application/reassembly/JSONL tests passed.
- Lifecycle tests passed normally, under AddressSanitizer + UBSan, and separately
  under ThreadSanitizer, with no lifecycle sanitizer findings.
- All 29 configured channel slots run concurrently in the lifecycle test. It
  performs 12 full Start/Stop cycles, repeated Stop, individual channel restarts,
  idle-source Stop, partial startup failure at every channel position, a paused
  callback during shutdown, and destruction while running.
- Protocol tests produced no ASan findings. UBSan reports remain in vendored
  libacars: signed overflow in `hash.c:33`, signed shifts in
  `asn1/per_support.c:89` and `asn1/INTEGER.c:819`. This is not a clean UBSan run;
  these arithmetic implementations were not changed for this lifecycle fix.

The lifecycle harness uses production shutdown helpers, real core Handler/stream
objects and real producer/consumer threads. It substitutes RF producers and
protocol work; it does not instantiate the GUI VFO manager or run the complete
application under TSan. Long-duration live RF reception followed by repeated
UI Start/Stop remains a required check on the rebuilt app.

### Reproduce the automated checks

From the repository root (use separate build directories):

```sh
cmake -S decoder_modules/vdl2_decoder/tests -B /tmp/vdl2-tests -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build /tmp/vdl2-tests -j8
ctest --test-dir /tmp/vdl2-tests --output-on-failure

cmake -S decoder_modules/vdl2_decoder/tests -B /tmp/vdl2-asan -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DVDL2_SANITIZERS=ON
cmake --build /tmp/vdl2-asan -j8
ctest --test-dir /tmp/vdl2-asan --output-on-failure

cmake -S decoder_modules/vdl2_decoder/tests -B /tmp/vdl2-tsan -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DVDL2_THREAD_SANITIZER=ON
cmake --build /tmp/vdl2-tsan --target lifecycle_tests -j8
ctest --test-dir /tmp/vdl2-tsan -R vdl2_lifecycle --output-on-failure
```
