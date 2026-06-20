// Cross-platform whisper.cpp transcription backend (Windows + future Linux).
// macOS uses transcription_whisper.mm (Obj-C++ for NSLog/NSFileManager/Metal).
// This file is only compiled on non-Apple platforms.
#ifndef __APPLE__
#include "transcription_whisper.h"
#include "whisper.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>
#endif

#include <utils/flog.h>

namespace transcription_whisper {

// ── Model registry ──────────────────────────────────────────────────────────
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
#ifdef _WIN32
    char appData[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appData) != S_OK) return {};
    std::string dir = std::string(appData) + "\\SDR++\\channel_bank\\models";
    CreateDirectoryA((std::string(appData) + "\\SDR++").c_str(), NULL);
    CreateDirectoryA((std::string(appData) + "\\SDR++\\channel_bank").c_str(), NULL);
    CreateDirectoryA(dir.c_str(), NULL);
    return dir;
#else
    const char* home = getenv("HOME");
    if (!home) home = getpwuid(getuid())->pw_dir;
    std::string dir = std::string(home) + "/.local/share/SDR++/channel_bank/models";
    mkdir((std::string(home) + "/.local/share/SDR++").c_str(), 0755);
    mkdir((std::string(home) + "/.local/share/SDR++/channel_bank").c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    return dir;
#endif
}

std::string modelPath(Model m) {
    std::string d = modelsDir();
    if (d.empty()) return {};
#ifdef _WIN32
    return d + "\\" + modelFilename(m);
#else
    return d + "/" + modelFilename(m);
#endif
}

bool isModelInstalled(Model m) {
    std::string p = modelPath(m);
    if (p.empty()) return false;
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && st.st_size > 0;
#endif
}

uint64_t modelSize(Model m) {
    std::string p = modelPath(m);
    if (p.empty()) return 0;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
#else
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) return 0;
    return (uint64_t)st.st_size;
#endif
}

// ── WAV reader ──────────────────────────────────────────────────────────────
static bool readWavToFloat(const std::string& path, std::vector<float>& out, int& srOut) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
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

static std::vector<float> resampleTo16k(const std::vector<float>& in, int srIn) {
    constexpr int SR_OUT = 16000;
    if (srIn == SR_OUT) return in;
    std::vector<float> out;
    if (srIn == 48000) {
        out.reserve(in.size() / 3 + 1);
        for (size_t i = 0; i + 2 < in.size(); i += 3) {
            out.push_back((in[i] + in[i+1] + in[i+2]) * (1.0f / 3.0f));
        }
    } else {
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
static const char* kAtcPrompt =
    "Air traffic control. Tower, ground, approach, departure, center, "
    "clearance. Cleared, taxi, hold short, line up, takeoff, landing, "
    "go around, missed approach, contact, monitor, squawk, ident. "
    "Runway, heading, altitude, flight level, knots, niner, tree, fife. "
    "United, Delta, American, JetBlue, Southwest, FedEx, UPS.";

// ── Persistent model context cache ──────────────────────────────────────────
static std::mutex                              g_cacheMtx;
static std::map<Model, whisper_context*>       g_ctxCache;
static std::timed_mutex                        g_inferMtx;
static constexpr int                           kInferTimeoutSec = 120;

static whisper_context* getOrLoadCtx(Model m) {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    auto it = g_ctxCache.find(m);
    if (it != g_ctxCache.end()) return it->second;

    std::string p = modelPath(m);
    if (p.empty() || !isModelInstalled(m)) return nullptr;

    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu     = false;      // CPU-only on Windows (no Metal)
    cparams.flash_attn  = false;
    whisper_context* ctx =
        whisper_init_from_file_with_params(p.c_str(), cparams);
    if (ctx) {
        g_ctxCache[m] = ctx;
        flog::info("[CBWhisper] loaded + cached {0}", modelFilename(m));
    } else {
        flog::error("[CBWhisper] whisper_init_from_file failed: {0}", p);
    }
    return ctx;
}

void shutdown() {
    if (g_inferMtx.try_lock_for(std::chrono::seconds(5))) {
        std::lock_guard<std::timed_mutex> ilk(g_inferMtx, std::adopt_lock);
        std::lock_guard<std::mutex> clk(g_cacheMtx);
        for (auto& [m, ctx] : g_ctxCache) {
            if (ctx) whisper_free(ctx);
        }
        g_ctxCache.clear();
    } else {
        flog::warn("[CBWhisper] shutdown: inference mutex held — skipping whisper_free");
    }
}

// ── Session object ──────────────────────────────────────────────────────────
static constexpr int kInferWallClockSec = 90;

struct Session {
    std::atomic<bool>      finalFlag { false };
    std::atomic<bool>      cancelled { false };
    std::mutex             textMtx;
    std::string            text;
    std::vector<Segment>   segments;
    std::thread            thread;
    std::chrono::steady_clock::time_point inferStart;
};

static bool abortCb(void* userData) {
    Session* s = (Session*)userData;
    if (s->cancelled.load()) return true;
    auto elapsed = std::chrono::steady_clock::now() - s->inferStart;
    if (elapsed > std::chrono::seconds(kInferWallClockSec)) {
        flog::warn("[CBWhisper] inference exceeded {0}s wall clock — aborting", kInferWallClockSec);
        return true;
    }
    return false;
}

static void runInference(Session* s, std::string wavPath, Model model) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

    if (s->cancelled.load()) { s->finalFlag.store(true); return; }

    whisper_context* ctx = getOrLoadCtx(model);
    if (!ctx) {
        s->finalFlag.store(true);
        return;
    }
    if (s->cancelled.load()) { s->finalFlag.store(true); return; }

    std::vector<float> pcm;
    int srIn = 0;
    if (!readWavToFloat(wavPath, pcm, srIn)) {
        flog::error("[CBWhisper] could not read WAV: {0}", wavPath);
        s->finalFlag.store(true);
        return;
    }
    std::vector<float> pcm16k = resampleTo16k(pcm, srIn);
    if (pcm16k.empty()) {
        s->finalFlag.store(true);
        return;
    }

    whisper_full_params wparams =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.language         = "en";
    wparams.detect_language  = false;
    wparams.translate        = false;
    wparams.no_context       = true;
    wparams.single_segment   = false;
    wparams.print_progress   = false;
    wparams.print_realtime   = false;
    wparams.print_timestamps = false;
    wparams.print_special    = false;
    wparams.suppress_blank   = true;
    wparams.initial_prompt   = kAtcPrompt;
    wparams.no_speech_thold  = 0.6f;
    // CPU-only: use more threads than Metal path since there's no GPU to feed
    wparams.n_threads        = 4;
    wparams.abort_callback           = abortCb;
    wparams.abort_callback_user_data = s;

    std::string          out;
    std::vector<Segment> segs;
    int                  nseg = 0;
    {
        if (!g_inferMtx.try_lock_for(std::chrono::seconds(kInferTimeoutSec))) {
            flog::warn("[CBWhisper] inference mutex held for >{0}s — skipping {1}",
                       kInferTimeoutSec, wavPath);
            s->finalFlag.store(true);
            return;
        }
        std::lock_guard<std::timed_mutex> lk(g_inferMtx, std::adopt_lock);
        if (s->cancelled.load()) { s->finalFlag.store(true); return; }
        s->inferStart = std::chrono::steady_clock::now();
        int rc = whisper_full(ctx, wparams, pcm16k.data(), (int)pcm16k.size());
        if (rc != 0) {
            flog::error("[CBWhisper] whisper_full failed rc={0} for {1}", rc, wavPath);
            s->finalFlag.store(true);
            return;
        }
        nseg = whisper_full_n_segments(ctx);
        out.reserve(256);
        segs.reserve(nseg);
        for (int i = 0; i < nseg; i++) {
            const char* seg = whisper_full_get_segment_text(ctx, i);
            if (!seg) continue;
            int64_t t0cs = whisper_full_get_segment_t0(ctx, i);
            int64_t t1cs = whisper_full_get_segment_t1(ctx, i);
            while (*seg == ' ') seg++;
            if (*seg == '\0') continue;
            segs.push_back({ (int)(t0cs * 10), (int)(t1cs * 10), std::string(seg) });
            if (!out.empty()) out += ' ';
            out += seg;
        }
    }

    {
        std::lock_guard<std::mutex> lk(s->textMtx);
        s->text     = out;
        s->segments = std::move(segs);
    }

    if (!s->cancelled.load()) {
        flog::info("[CBWhisper] {0} -> {1} ({2} segments)", wavPath, out, nseg);
    }
    s->finalFlag.store(true);
}

// ── Public API ──────────────────────────────────────────────────────────────
void* transcribeFile(const char* path, Model m) {
    if (!path || !*path) return nullptr;
    if (!isModelInstalled(m)) {
        flog::error("[CBWhisper] model not installed: {0}", modelFilename(m));
        return nullptr;
    }
    Session* s = new Session();
    std::string wavPath(path);
    s->thread = std::thread(runInference, s, wavPath, m);
    return s;
}

void cancel(void* handle) {
    if (!handle) return;
    ((Session*)handle)->cancelled.store(true);
}

std::string getText(void* handle) {
    if (!handle) return {};
    Session* s = (Session*)handle;
    std::lock_guard<std::mutex> lk(s->textMtx);
    return s->text;
}

std::vector<Segment> getSegments(void* handle) {
    if (!handle) return {};
    Session* s = (Session*)handle;
    std::lock_guard<std::mutex> lk(s->textMtx);
    return s->segments;
}

std::string formatLrc(const std::vector<Segment>& segs) {
    std::string out;
    out.reserve(segs.size() * 32);
    char buf[16];
    for (auto& s : segs) {
        int ms  = s.t0Ms;
        int mm  = ms / 60000;
        int ss  = (ms / 1000) % 60;
        int cs  = (ms / 10)   % 100;
        snprintf(buf, sizeof(buf), "[%02d:%02d.%02d]", mm, ss, cs);
        out += buf;
        out += s.text;
        out += '\n';
    }
    return out;
}

bool isFinal(void* handle) {
    if (!handle) return false;
    return ((Session*)handle)->finalFlag.load();
}

void destroy(void* handle) {
    if (!handle) return;
    Session* s = (Session*)handle;
    s->cancelled.store(true);
    if (!s->thread.joinable()) { delete s; return; }
    if (s->finalFlag.load()) {
        s->thread.join();
        delete s;
        return;
    }
    flog::warn("[CBWhisper] destroy: thread not finished — detaching to avoid deadlock");
    s->thread.detach();
}

} // namespace transcription_whisper
#endif // !__APPLE__
