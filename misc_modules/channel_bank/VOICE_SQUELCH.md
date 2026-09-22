# Experimental Voice Squelch

Source-only prototype based on integration/main `7ad6434c`. No builds,
packaging, deployment, automated tests, or live RF testing were performed in
this task, at the user's request. This branch is not ready to merge until the
separate build session and user validate it.

## User control

One **Voice Squelch** checkbox, default off, beside the existing noise/voice
controls. It can change while running and is saved in configuration and
profiles. Older profiles default to off. Off bypasses voice admission and scan
hold decisions immediately; generation-tagged state prevents a rapid off/on
toggle from reusing old voice decisions or retry suppression.

This is distinct from **RNNoise Voice Gate**, which rejects recordings after
opening. Neither that setting nor Noise Reduction is enabled or changed by
Voice Squelch. For an isolated comparison, leave the older Voice Gate off.
Existing queued recordings/playback are not removed when this toggle changes.
An already-open recording is allowed to finish under its normal RF rules.

## Signal path

- Existing RF detection, threshold, adaptive floor, trimming, and NMS identify
  candidates exactly as before.
- A separate per-slot RNNoise state inspects 48 kHz dry demodulated audio,
  before recording gain, fades, and noise reduction. Its denoised output is
  discarded. There is no new dependency or model download.
- After the existing 200 ms slot warmup, examine 10 ms frames. Initial
  confirmation requires at least 200 ms of audio and speech probability
  >= 0.60 in at least 3 of the latest 10 frames. These are provisional constants,
  not validated radio-specific thresholds or exposed settings.
- Unconfirmed audio cannot open a recording or enter its playback queue.
  With the feature enabled, a one-second pre-roll replaces the normal 400 ms
  buffer. It preserves available audio during confirmation, not audio lost
  before a slot existed or during the existing warmup. Buffered RF-present
  airtime contributes to the unchanged Min TX duration check.
- Confirmation latches for the RF episode/open recording. Normal hold/tail
  rules remain in charge; speech pauses do not close the recording. This is
  not continuous silence squelch and does not close a confirmed call just
  because its carrier subsequently becomes unmodulated.
- At 600 ms without confirmation, a candidate is rejected. Fixed Manual and
  Bookmark Scan slots continue listening and can confirm later speech.
  Auto/Scan slots are released, with a separate one-second retry suppression
  so rejected carriers cannot monopolize the demodulator limit. That may miss
  a brief call during the retry interval; it never changes the user's block
  list, RF votes, cooldown, or existing RNNoise quarantine.
- Probing can delay scanning briefly but never sets the scan's “had signal”
  latch or refreshes its quiet timer. A 1.2-second wall-clock limit bounds
  probe extensions from the last actual activity/scan-stop entry, including
  stalled audio and successive candidates. Configured scan dwell/quiet time
  can be longer; it is not rewritten.

## Platforms and integration

Implemented in desktop/Android `main.cpp`; the separate iOS implementation is
unchanged. Builds defining `CB_NO_RNNOISE` (currently MSVC) leave the feature off
and do not show its checkbox. No alternate classifier is silently substituted.

Settings API: `voiceSquelchEnabled` (boolean, live mutable) and
`supportsVoiceSquelch` (read-only). An enable request is rejected on unsupported
builds. Each active-channel state has `voiceSquelchState`: `off`, `idle`,
`probing`, `confirmed`, or `rejected`. RF telemetry remains RF telemetry;
voice qualification is separate. Native waterfall live markers require voice
acceptance when enabled. Native channel rows show `[VOICE?]` / `[NO VOICE]`.

## Pending user validation

Use the separate build session; do not replace the known-good installation.
Compare Off/On on the same source, RF threshold, and noise settings:

1. Unmodulated carriers, weak edge noise, hiss, and data bursts: no unwanted
   recorded playback or indefinite scan hold with On.
2. Strong, weak/noisy, short, and different speakers' calls: check admission,
   first words, natural pauses, and ordinary RF close-out.
3. Toggle off while probing/rejected and quickly off/on: legacy admission
   resumes without lingering voice rejection. Toggle during a recording:
   current call is not cut up.
4. Auto, Manual, Scan, and Bookmark Scan; multiple simultaneous candidates,
   max-channel capacity, retunes, stop/start, saved profiles, and API toggles.
5. CPU load on each intended platform/radio bandwidth. Voice work happens at
   fixed audio rate but scales with candidate count. A noise-reduction-enabled
   slot may run both independent RNNoise states.

Weak/noisy speech rejection and false positives on speech-like interference
remain real risks. This is not claimed to reproduce a verified Mykola audio
classifier; the earlier binary analysis established its RF detection path.
