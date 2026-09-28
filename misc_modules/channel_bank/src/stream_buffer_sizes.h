#pragma once
#include <dsp/stream.h>

// Splitter copies the complete upstream block without checking destination
// capacity. RxVFO also uses its output as scratch for the entire input block
// before resampling, even when the final audio-rate output is much smaller.
// Retain the core stream capacity throughout the chain until producers can
// enforce smaller block limits. This deliberately trades memory for safety.
static constexpr int CB_RF_STREAM_BUFFER_SAMPLES = STREAM_BUFFER_SIZE;
static constexpr int CB_AUDIO_STREAM_BUFFER_SAMPLES = STREAM_BUFFER_SIZE;
