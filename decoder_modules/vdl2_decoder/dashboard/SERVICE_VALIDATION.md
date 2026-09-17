# Module-managed dashboard validation

2026-09-17, macOS ARM64, baseline `7762f631`.

- Combined targets `sdrpp`, `channel_bank`, `rx888_source`, `vdl2_decoder`,
  `selcal_decoder` build successfully (existing build has Brown DSD disabled).
- All 13 Python dashboard tests pass, including actual loopback HTTP startup,
  port collision, parent-pipe EOF and three restart cycles with temporary data.
- C++ launcher tests cover missing executable, invalid settings, immediate child
  failure, repeated start/stop, destructor cleanup and a child ignoring EOF.
  Assertions remain enabled in Release builds.
- Launcher and existing DSP lifecycle tests pass with AddressSanitizer and UBSan.
- Existing non-sanitized protocol tests pass.
- Full fresh sanitizer suite is **not clean**: the unchanged protocol test
  executable reports a heap-buffer-overflow in libacars ASN.1
  `per_get_few_bits`, called by `uper_open_type_get_simple` /
  `uper_open_type_skip`, through ACSE / COTP / compressed CLNP / X.25 parsing.
  It reads one byte immediately beyond an 18-byte allocation. Existing signed
  overflow/shift UBSan warnings also occur. The protocol executable does not
  link the launcher, UI, or Python changes; all its source inputs are unchanged
  from the baseline. This needs a separate decoder investigation, not a claim
  that the full protocol sanitizer run passed.

Reproduce the sanitizer finding with:

```sh
cmake -S decoder_modules/vdl2_decoder/tests -B /tmp/vdl2-service-asan -DVDL2_SANITIZERS=ON
cmake --build /tmp/vdl2-service-asan -j8
ctest --test-dir /tmp/vdl2-service-asan --output-on-failure
```

The installed SDR++ app has not been replaced, signed, or live-tested with these
controls. Packaging Python/dashboard dependencies and Windows process launching
remain outside this change. The module exposes configurable runtime paths.
