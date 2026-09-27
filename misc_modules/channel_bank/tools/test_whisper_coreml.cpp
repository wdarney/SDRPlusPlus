// Load-only smoke check using the exact vendored runtime and existing GGML file.
// Usage: test_whisper_coreml MODEL.bin [disabled|fallback|active|infer|metal-infer]
#include "whisper.h"
#include "coreml/whisper-encoder.h"
#include <vector>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc, char ** argv) {
    if (argc < 3 || argc > 4) return 2;
    if (std::strcmp(argv[2], "bridge") == 0) {
        auto * encoder = whisper_coreml_init(argv[1], 80, 1500, 32);
        if (!encoder) return 1;
        std::vector<float> mel(80 * 3000, 0.1f), out(1500 * 32);
        bool ok = whisper_coreml_encode(encoder, 3000, 80, mel.data(), out.data(), out.size());
        whisper_coreml_free(encoder);
        std::printf("Core ML bridge prediction: %s\n", ok ? "passed" : "failed");
        return ok ? 0 : 1;
    }
    setenv("GGML_METAL_NO_RESIDENCY", "1", 1);
    auto params = whisper_context_default_params();
    params.use_gpu = true;
    params.flash_attn = true;
    params.use_coreml = std::strcmp(argv[2], "disabled") != 0 && std::strcmp(argv[2], "metal-infer") != 0;
    auto * ctx = whisper_init_from_file_with_params(argv[1], params);
    if (!ctx) return 1;
    bool active = whisper_coreml_is_active(ctx);
    std::printf("Core ML active: %s\n", active ? "yes" : "no");
    bool expected = std::strcmp(argv[2], "active") == 0 || std::strcmp(argv[2], "infer") == 0;
    bool ok = active == expected;
    if (ok && (std::strcmp(argv[2], "infer") == 0 || std::strcmp(argv[2], "metal-infer") == 0)) {
        auto full = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        full.n_threads = 4;
        full.language = "en";
        full.no_context = true;
        full.no_timestamps = true;
        full.print_progress = false;
        full.print_realtime = false;
        full.print_timestamps = false;
        full.temperature_inc = 0;
        std::vector<float> samples(WHISPER_SAMPLE_RATE * 2, 0.0f);
        if (argc == 4) {
            std::ifstream input(argv[3], std::ios::binary);
            samples.clear();
            float sample;
            while (input.read(reinterpret_cast<char*>(&sample), sizeof(sample))) samples.push_back(sample);
            if (!input.eof() || samples.empty()) { whisper_free(ctx); return 2; }
        }
        ok = whisper_full(ctx, full, samples.data(), samples.size()) == 0;
        if (ok) {
            for (int i = 0; i < whisper_full_n_segments(ctx); ++i)
                std::printf("Transcript: %s\n", whisper_full_get_segment_text(ctx, i));
        }
        whisper_print_timings(ctx);
        std::printf("Full inference smoke: %s\n", ok ? "passed" : "failed");
    }
    whisper_free(ctx);
    return ok ? 0 : 1;
}
