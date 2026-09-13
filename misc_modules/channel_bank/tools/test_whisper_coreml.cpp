// Load-only smoke check using the exact vendored runtime and existing GGML file.
// Usage: test_whisper_coreml MODEL.bin [disabled|fallback|active]
#include "whisper.h"
#include "coreml/whisper-encoder.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc, char ** argv) {
    if (argc != 3) return 2;
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
    params.use_coreml = std::strcmp(argv[2], "disabled") != 0;
    auto * ctx = whisper_init_from_file_with_params(argv[1], params);
    if (!ctx) return 1;
    bool active = whisper_coreml_is_active(ctx);
    std::printf("Core ML active: %s\n", active ? "yes" : "no");
    bool expected = std::strcmp(argv[2], "active") == 0;
    whisper_free(ctx);
    return active == expected ? 0 : 1;
}
