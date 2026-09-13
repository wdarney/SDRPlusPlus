// Wrapper of the Core ML Whisper Encoder model
//
// Code is derived from the work of Github user @wangchou
// ref: https://github.com/wangchou/callCoreMLFromCpp

#include <stdint.h>
#include <stdbool.h>

#if __cplusplus
extern "C" {
#endif

struct whisper_coreml_context;

struct whisper_coreml_context * whisper_coreml_init(const char * path_model, int n_mels, int n_audio_ctx, int n_audio_state);
void whisper_coreml_free(struct whisper_coreml_context * ctx);

bool whisper_coreml_encode(
        const whisper_coreml_context * ctx,
                             int64_t   n_ctx,
                             int64_t   n_mel,
                               float * mel,
                               float * out,
                               int64_t   n_out);

#if __cplusplus
}
#endif
