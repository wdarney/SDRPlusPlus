#if !__has_feature(objc_arc)
#error This file must be compiled with automatic reference counting enabled (-fobjc-arc)
#endif

#import "whisper-encoder.h"
#import "whisper-encoder-impl.h"

#import <CoreML/CoreML.h>

#include <stdlib.h>

#if __cplusplus
extern "C" {
#endif

struct whisper_coreml_context {
    const void * data;
};

struct whisper_coreml_context * whisper_coreml_init(const char * path_model, int n_mels, int n_audio_ctx, int n_audio_state) {
    NSString * path_model_str = [[NSString alloc] initWithUTF8String:path_model];

    NSURL * url_model = [NSURL fileURLWithPath: path_model_str];

    // select which device to run the Core ML model on
    MLModelConfiguration *config = [[MLModelConfiguration alloc] init];
    // config.computeUnits = MLComputeUnitsCPUAndGPU;
    //config.computeUnits = MLComputeUnitsCPUAndNeuralEngine;
    config.computeUnits = MLComputeUnitsAll;

    NSError * error = nil;
    whisper_encoder_impl * model = [[whisper_encoder_impl alloc] initWithContentsOfURL:url_model configuration:config error:&error];

    if (model == nil) {
        NSLog(@"[CBWhisper] Core ML load failed: %@", error);
        return NULL;
    }

    MLMultiArrayConstraint * input = model.model.modelDescription.inputDescriptionsByName[@"logmel_data"].multiArrayConstraint;
    MLMultiArrayConstraint * output = model.model.modelDescription.outputDescriptionsByName[@"output"].multiArrayConstraint;
    if (input.dataType != MLMultiArrayDataTypeFloat32 || output.dataType != MLMultiArrayDataTypeFloat32 ||
        ![input.shape isEqualToArray:@[@1, @(n_mels), @(2*n_audio_ctx)]] ||
        ![output.shape isEqualToArray:@[@1, @(n_audio_ctx), @(n_audio_state)]]) {
        NSLog(@"[CBWhisper] incompatible Core ML encoder input/output; falling back to ggml/Metal");
        return NULL;
    }
    NSLog(@"[CBWhisper] Core ML loaded with computeUnits=All (CPU/GPU/ANE eligible)");
    const void * data = CFBridgingRetain(model);
    whisper_coreml_context * ctx = new whisper_coreml_context;

    ctx->data = data;

    return ctx;
}

void whisper_coreml_free(struct whisper_coreml_context * ctx) {
    CFRelease(ctx->data);
    delete ctx;
}

bool whisper_coreml_encode(
        const whisper_coreml_context * ctx,
                             int64_t   n_ctx,
                             int64_t   n_mel,
                               float * mel,
                               float * out,
                               int64_t   n_out) {
    MLMultiArray * inMultiArray = [
        [MLMultiArray alloc] initWithDataPointer: mel
                                           shape: @[@1, @(n_mel), @(n_ctx)]
                                        dataType: MLMultiArrayDataTypeFloat32
                                         strides: @[@(n_ctx*n_mel), @(n_ctx), @1]
                                     deallocator: nil
                                           error: nil
    ];

    if (!inMultiArray) return false;

    @autoreleasepool {
        NSError * error = nil;
        whisper_encoder_implOutput * outCoreML = [(__bridge id) ctx->data predictionFromLogmel_data:inMultiArray error:&error];

        MLMultiArray * output = outCoreML.output;
        if (!output || output.dataType != MLMultiArrayDataTypeFloat32 || output.count != n_out) {
            NSLog(@"[CBWhisper] Core ML prediction failed or incompatible output: %@", error);
            return false;
        }
        // The converter emits a contiguous [1, n_audio_ctx, n_audio_state] tensor.
        NSInteger stride = 1;
        for (NSInteger i = output.shape.count - 1; i >= 0; --i) {
            if (output.strides[i].integerValue != stride) {
                NSLog(@"[CBWhisper] Core ML output is not contiguous");
                return false;
            }
            stride *= output.shape[i].integerValue;
        }
        memcpy(out, output.dataPointer, n_out * sizeof(float));
        return true;
    }
}

#if __cplusplus
}
#endif
