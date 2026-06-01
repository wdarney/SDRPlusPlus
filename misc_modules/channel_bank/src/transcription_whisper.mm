// Whisper.cpp transcription backend.  See transcription_whisper.h for the public
// API; this file implements it.  Inference runs on a detached background thread
// per recording — main thread just polls the handle for partial/final text.
#ifdef __APPLE__
#import <Foundation/Foundation.h>
#include "transcription_whisper.h"
#include "whisper.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <pthread/qos.h>   // pthread_set_qos_class_self_np
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace transcription_whisper {

// ── Model registry ──────────────────────────────────────────────────────────
//
// Filenames match the convention we'll publish them under (so a manual drop-in
// for testing uses the same names).  The "atc" suffix is meaningful — these are
// fine-tuned models, not vanilla Whisper; we don't want a vanilla large-v3.bin
// being mistaken for the ATC variant.
std::string modelFilename(Model m) {
    switch (m) {
        case Model::ATCLarge:  return "ggml-whisper-large-v3-atc-q5_0.bin";
        case Model::ATCMedium: return "ggml-whisper-medium.en-atc-q5_0.bin";
        case Model::Turbo:     return "ggml-whisper-large-v3-turbo-q5_0.bin";
    }
    return {};
}

std::string modelLabel(Model m) {
    switch (m) {
        case Model::ATCLarge:  return "Whisper ATC Large (best ~1.1 GB)";
        case Model::ATCMedium: return "Whisper ATC Medium (~540 MB)";
        case Model::Turbo:     return "Whisper Turbo (fast, generic ~570 MB)";
    }
    return {};
}

std::string modelsDir() {
    // ~/Library/Application Support/SDR++/channel_bank/models/
    NSArray<NSURL*>* urls = [[NSFileManager defaultManager]
        URLsForDirectory:NSApplicationSupportDirectory
               inDomains:NSUserDomainMask];
    if (urls.count == 0) return {};
    NSURL* base = [[urls.firstObject
        URLByAppendingPathComponent:@"SDR++" isDirectory:YES]
        URLByAppendingPathComponent:@"channel_bank" isDirectory:YES];
    NSURL* dir  = [base URLByAppendingPathComponent:@"models" isDirectory:YES];
    [[NSFileManager defaultManager]
        createDirectoryAtURL:dir withIntermediateDirectories:YES
                  attributes:nil error:nil];
    return std::string(dir.path.UTF8String);
}

std::string modelPath(Model m) {
    std::string d = modelsDir();
    if (d.empty()) return {};
    return d + "/" + modelFilename(m);
}

bool isModelInstalled(Model m) {
    struct stat st{};
    std::string p = modelPath(m);
    if (p.empty()) return false;
    return ::stat(p.c_str(), &st) == 0 && st.st_size > 0;
}

uint64_t modelSize(Model m) {
    struct stat st{};
    std::string p = modelPath(m);
    if (p.empty()) return 0;
    if (::stat(p.c_str(), &st) != 0) return 0;
    return (uint64_t)st.st_size;
}

// ── WAV reader (matches the int16 PCM mono 48 kHz the module writes) ────────
//
// Returns float samples in [-1,1] at the *original* sample rate.  The 44-byte
// header skip mirrors normalizeWavFile() — these files come from our own
// writer, so we know the layout.
static bool readWavToFloat(const std::string& path, std::vector<float>& out, int& srOut) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    // RIFF header is fixed 44 bytes in our writer's output.  Pull the sample
    // rate from the standard offset 24 just in case it ever changes.
    char hdr[44];
    f.read(hdr, sizeof(hdr));
    if (!f) return false;
    uint32_t sr = (uint8_t)hdr[24] | ((uint8_t)hdr[25] << 8)
                | ((uint8_t)hdr[26] << 16) | ((uint8_t)hdr[27] << 24);
    srOut = (int)sr;

    out.clear();
    int16_t s;
    while (f.read((char*)&s, 2)) out.push_back((float)s / 32768.0f);
    return !out.empty();
}

// Decimate from the source rate to 16 kHz (Whisper's native rate).  For
// 48 kHz → 16 kHz this is exactly factor-of-3 decimation; for any other
// rate we fall back to nearest-neighbor (rarely used — our recordings are
// always 48 kHz).  A simple 3-tap moving-average pre-filter knocks down the
// aliases above 8 kHz enough for speech transcription; whisper.cpp itself
// expects roughly this band-limit.  Voice content lives below 4 kHz so the
// gentle pre-filter is plenty.
static std::vector<float> resampleTo16k(const std::vector<float>& in, int srIn) {
    constexpr int SR_OUT = 16000;
    if (srIn == SR_OUT) return in;
    std::vector<float> out;
    if (srIn == 48000) {
        out.reserve(in.size() / 3 + 1);
        // 3-tap moving avg before every 3rd sample.
        for (size_t i = 0; i + 2 < in.size(); i += 3) {
            out.push_back((in[i] + in[i+1] + in[i+2]) * (1.0f / 3.0f));
        }
    } else {
        // Generic resample (good enough for any oddball rate).
        double ratio = (double)SR_OUT / (double)srIn;
        size_t n = (size_t)((double)in.size() * ratio);
        out.reserve(n);
        for (size_t i = 0; i < n; i++) {
            size_t j = (size_t)((double)i / ratio);
            if (j >= in.size()) break;
            out.push_back(in[j]);
        }
    }
    return out;
}

// ── ATC vocabulary prompt ───────────────────────────────────────────────────
//
// Whisper is heavily biased by `initial_prompt` text — it nudges token
// probabilities toward what's in the prompt.  Without this, generic Whisper
// transcribes "United 234 heavy" as "you and it two three four heavy" or
// similar.  Even the ATC-fine-tuned models benefit from a vocabulary hint,
// especially for less-frequent terms in the training data (e.g. specific
// airport names that may not have been in the fine-tune set).
//
// Kept short — long prompts eat the limited 224-token context Whisper
// allocates for the prompt, which hurts later transcription quality.
static const char* kAtcPrompt =
    "Air traffic control. Tower, ground, approach, departure, center, "
    "clearance. Cleared, taxi, hold short, line up, takeoff, landing, "
    "go around, missed approach, contact, monitor, squawk, ident. "
    "Runway, heading, altitude, flight level, knots, niner, tree, fife. "
    "United, Delta, American, JetBlue, Southwest, FedEx, UPS.";

// ── Persistent model context cache ──────────────────────────────────────────
//
// Loading a 1.5 GB ggml model takes ~500 ms–1 s and burns memory bandwidth.
// Doing that *per transcription* was the dominant source of "audio skips while
// transcribing" we saw on M-series machines — even with plenty of RAM, the
// transient load churn briefly stole bandwidth from the audio thread.
//
// Solution: cache the loaded whisper_context per model, indexed by Model enum.
// First call loads, subsequent calls reuse.  Memory cost: one model resident
// per backend the user has selected since startup (typically just one).
// On 8 GB Macs this is a meaningful budget; on 16 GB+ it's free.
//
// Threading model: we serialize ALL whisper_full calls behind a single mutex
// (g_inferMtx).  whisper.cpp's context state is mutated by whisper_full() and
// then read by whisper_full_get_segment_text(); a second concurrent inference
// would clobber the first's results.  ATC traffic isn't truly parallel, so
// serializing is a non-issue in practice.
static std::mutex                              g_cacheMtx;
static std::map<Model, whisper_context*>       g_ctxCache;
static std::mutex                              g_inferMtx;

static whisper_context* getOrLoadCtx(Model m) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    auto it = g_ctxCache.find(m);
    if (it != g_ctxCache.end()) return it->second;

    std::string p = modelPath(m);
    if (p.empty() || !isModelInstalled(m)) return nullptr;

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu     = true;       // Metal acceleration on Apple Silicon
    cparams.flash_attn  = true;       // ggml's fused attention kernel
    whisper_context* ctx =
        whisper_init_from_file_with_params(p.c_str(), cparams);
    if (ctx) {
        g_ctxCache[m] = ctx;
        NSLog(@"[CBWhisper] loaded + cached %s", modelFilename(m).c_str());
    } else {
        NSLog(@"[CBWhisper] whisper_init_from_file failed: %s", p.c_str());
    }
    return ctx;
}

void shutdown() {
    // Acquire the inference mutex first so we don't yank a context out from
    // under a running whisper_full().  Order matters: g_inferMtx → g_cacheMtx
    // (same as the worker's order — worker takes g_cacheMtx briefly, then
    // g_inferMtx for the inference itself; no overlap).
    std::lock_guard<std::mutex> ilk(g_inferMtx);
    std::lock_guard<std::mutex> clk(g_cacheMtx);
    for (auto& [m, ctx] : g_ctxCache) {
        if (ctx) whisper_free(ctx);
    }
    g_ctxCache.clear();
}

// ── Session object ──────────────────────────────────────────────────────────
struct Session {
    std::atomic<bool>     finalFlag { false };
    std::atomic<bool>     cancelled { false };
    std::mutex            textMtx;
    std::string           text;
    std::thread           thread;
    // ctx is owned by the worker thread — we don't touch it from the main
    // thread.  cancelled is atomic so the worker can check it cheaply.
};

// Set the latest transcript (thread-safe).
static void sessionSetText(Session* s, const std::string& t) {
    std::lock_guard<std::mutex> lk(s->textMtx);
    s->text = t;
}

// ── Worker thread: get cached context, run inference, publish text ──────────
//
// Critical: this thread is spawned from the recording-close path, which runs
// on the audio handler (QoS_CLASS_USER_INTERACTIVE).  std::thread inherits the
// parent's QoS, so without intervention the worker would run at audio priority
// and contend with the audio callback.  Demote it to UTILITY immediately so
// macOS keeps audio rendering ahead of us.
static void runInference(Session* s, std::string wavPath, Model model) {
    // Tell macOS this is background work — schedule on efficiency cores when
    // possible, don't preempt audio/UI threads.  Per-thread setting; only
    // affects this worker, not the rest of the module.
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);

    @autoreleasepool {
        if (s->cancelled.load()) { s->finalFlag.store(true); return; }

        // 1) Get a cached context (loads on first use for this model).
        whisper_context* ctx = getOrLoadCtx(model);
        if (!ctx) {
            s->finalFlag.store(true);
            return;
        }
        if (s->cancelled.load()) { s->finalFlag.store(true); return; }

        // 2) Read + resample the WAV.
        std::vector<float> pcm;
        int srIn = 0;
        if (!readWavToFloat(wavPath, pcm, srIn)) {
            NSLog(@"[CBWhisper] could not read WAV: %s", wavPath.c_str());
            s->finalFlag.store(true);
            return;
        }
        std::vector<float> pcm16k = resampleTo16k(pcm, srIn);
        if (pcm16k.empty()) {
            s->finalFlag.store(true);
            return;
        }

        // 3) Configure inference.  Greedy sampling — beam search costs latency
        //    we don't need for ATC.
        whisper_full_params wparams =
            whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.language         = "en";
        wparams.detect_language  = false;
        wparams.translate        = false;
        wparams.no_context       = true;     // each file is independent
        wparams.single_segment   = false;
        wparams.print_progress   = false;
        wparams.print_realtime   = false;
        wparams.print_timestamps = false;
        wparams.print_special    = false;
        wparams.suppress_blank   = true;
        wparams.initial_prompt   = kAtcPrompt;
        // Drop output when whisper internally rates the audio as silence —
        // these are the "Thank you for watching" hallucinations on near-empty
        // clips.  The gates already filter most of them upstream; this is
        // defense in depth.
        wparams.no_speech_thold  = 0.6f;
        // CPU thread count: 2 is the sweet spot for Metal-accelerated whisper.
        // The GPU does the heavy math; the CPU threads just feed it.  4+ just
        // creates scheduler contention with the audio thread for no speedup.
        wparams.n_threads        = 2;

        // 4) Run inference + segment readback under a single lock.  whisper_full
        //    mutates the context's segment buffer; whisper_full_get_segment_text
        //    reads it.  If a second worker started whisper_full() between our
        //    call and the readback, it would overwrite our results.  Serialize.
        std::string out;
        int nseg = 0;
        {
            std::lock_guard<std::mutex> lk(g_inferMtx);
            if (s->cancelled.load()) { s->finalFlag.store(true); return; }
            int rc = whisper_full(ctx, wparams, pcm16k.data(), (int)pcm16k.size());
            if (rc != 0) {
                NSLog(@"[CBWhisper] whisper_full failed rc=%d for %s",
                      rc, wavPath.c_str());
                s->finalFlag.store(true);
                return;
            }
            nseg = whisper_full_n_segments(ctx);
            out.reserve(256);
            for (int i = 0; i < nseg; i++) {
                const char* seg = whisper_full_get_segment_text(ctx, i);
                if (!seg) continue;
                // Trim leading whitespace whisper.cpp emits between segments.
                while (*seg == ' ') seg++;
                if (!out.empty()) out += ' ';
                out += seg;
            }
        }  // release g_inferMtx

        sessionSetText(s, out);
        s->finalFlag.store(true);

        if (!s->cancelled.load()) {
            NSLog(@"[CBWhisper] %s → %s (%d segments)",
                  [[[NSString stringWithUTF8String:wavPath.c_str()] lastPathComponent] UTF8String],
                  out.c_str(), nseg);
        }
    }
}

// ── Public API ──────────────────────────────────────────────────────────────
void* transcribeFile(const char* path, Model m) {
    if (!path || !*path) return nullptr;
    if (!isModelInstalled(m)) {
        NSLog(@"[CBWhisper] model not installed: %s",
              modelFilename(m).c_str());
        return nullptr;
    }

    Session* s = new Session();
    std::string wavPath(path);
    s->thread = std::thread(runInference, s, wavPath, m);
    s->thread.detach();
    return s;
}

void cancel(void* handle) {
    if (!handle) return;
    Session* s = (Session*)handle;
    s->cancelled.store(true);
}

std::string getText(void* handle) {
    if (!handle) return {};
    Session* s = (Session*)handle;
    std::lock_guard<std::mutex> lk(s->textMtx);
    return s->text;
}

bool isFinal(void* handle) {
    if (!handle) return false;
    Session* s = (Session*)handle;
    return s->finalFlag.load();
}

void destroy(void* handle) {
    if (!handle) return;
    Session* s = (Session*)handle;
    s->cancelled.store(true);
    // Worker thread is detached — it will finish whisper_free() in its own
    // time and exit on its own.  We just delete the session shell.  This is
    // safe because the worker only writes through atomics + the textMtx, and
    // once cancelled, it doesn't matter that we drop the read side.  In the
    // worst case the worker briefly holds the mutex on a deleted object —
    // which is why the worker treats `s` as alive for its whole lifetime
    // and we ONLY delete here after we know the user has stopped polling.
    //
    // For a fully clean design we'd refcount the session; for now we just
    // wait synchronously if the worker hasn't finished.  Cheap, deterministic.
    while (!s->finalFlag.load()) {
        [NSThread sleepForTimeInterval:0.005];
    }
    delete s;
}

} // namespace transcription_whisper
#endif // __APPLE__
