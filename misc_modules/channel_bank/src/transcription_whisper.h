#pragma once
#ifdef __APPLE__
#include <string>
#include <cstdint>
#include <vector>

// Whisper.cpp transcription backend.  Drop-in alternative to transcription::
// (Apple Speech) — same handle/poll API so call sites only need to dispatch
// by backend choice; downstream code (polling, embedding text in M4A tags,
// teardown) is identical.
//
// Model files live in ~/Library/Application Support/SDR++/channel_bank/models/.
// transcribeFile() returns nullptr if the requested model file isn't present —
// the caller (main.cpp) is responsible for the download UX before calling.

namespace transcription_whisper {

    // Identifies which Whisper model variant a recording should be transcribed with.
    // Each maps to a single ggml .bin filename in the models dir.
    enum class Model {
        ATCLarge,   // jacktol/whisper-large-v3-finetuned-for-ATC  → ~1.1 GB
        ATCMedium,  // jacktol/whisper-medium.en-fine-tuned-for-ATC → ~540 MB
        Turbo,      // openai/whisper-large-v3-turbo                → ~570 MB
    };

    // Canonical ggml filename for a model (lives inside modelsDir()).
    std::string modelFilename(Model m);

    // Human-readable model label for UI.
    std::string modelLabel(Model m);

    // The directory under ~/Library/Application Support/ where models live.
    // Created on first call if it doesn't exist.
    std::string modelsDir();

    // Full path to the model file (whether or not it exists on disk).
    std::string modelPath(Model m);

    // True iff the .bin file for that model is present on disk.
    bool isModelInstalled(Model m);

    // Disk size in bytes of the on-disk model file (0 if missing).
    uint64_t modelSize(Model m);

    // ── Transcription API — mirrors transcription:: shape ───────────────────
    // Begin transcribing a finished WAV file.  Returns an opaque handle that
    // can be polled with getText()/isFinal() and freed with destroy().
    // Returns nullptr if: model not installed, file unreadable, or load failed.
    void*       transcribeFile(const char* path, Model m);

    // Cancel an in-flight transcription (safe to call repeatedly).
    void        cancel(void* handle);

    // Most recent best-effort transcript.  May be empty until inference completes
    // (whisper.cpp doesn't stream incremental partials the way Apple Speech does;
    //  the text becomes available all at once when isFinal() flips to true).
    std::string getText(void* handle);

    // True once whisper_full() has returned and the segments are joined.
    bool        isFinal(void* handle);

    // ── Time-aligned segments ───────────────────────────────────────────────
    // Whisper produces per-segment timestamps natively (centiseconds of the
    // input audio).  Available after isFinal() flips true; empty before.
    // Times are in milliseconds, relative to the START of the WAV that was
    // transcribed (i.e. directly comparable to playback elapsed time).
    struct Segment {
        int         t0Ms;
        int         t1Ms;
        std::string text;
    };
    std::vector<Segment> getSegments(void* handle);

    // Format a segment list as standard LRC ("[mm:ss.xx]text" per line).
    // Embeddable in the M4A ©lyr metadata atom so external players (VLC,
    // QuickTime, IINA, iTunes) render synced captions automatically.
    std::string  formatLrc(const std::vector<Segment>& segs);

    // Release the handle (also cancels if still in flight).
    void        destroy(void* handle);

    // Free any cached model contexts.  Safe to call when no transcription is in
    // flight.  Otherwise (rarely needed) this blocks until the active inference
    // releases the context lock.  Not called automatically — leak-on-exit is
    // fine because the OS reaps everything on process teardown.  Provided so
    // the module can release the ~1.5 GB resident in the loaded model on stop().
    void        shutdown();
}
#endif
