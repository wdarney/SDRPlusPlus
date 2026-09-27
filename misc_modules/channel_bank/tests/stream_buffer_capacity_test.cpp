// Exercise the actual RxVFO with guard space so the old undersized allocation
// fails deterministically without performing an out-of-bounds write in the test.
#ifdef _WIN32
#define NOMINMAX
#define _USE_MATH_DEFINES
#endif
#include "../src/stream_buffer_sizes.h"
#include <dsp/channel/rx_vfo.h>
#include <algorithm>
#include <cstdio>

int main() {
    bool ok = CB_RF_STREAM_BUFFER_SAMPLES >= STREAM_BUFFER_SIZE;
    for (int count : {32768, 65536, 262144, STREAM_BUFFER_SIZE}) {
        dsp::stream<dsp::complex_t> input;
        dsp::channel::RxVFO vfo(&input, 2400000, 48000, 12500, 100000);
        const int capacity = CB_AUDIO_STREAM_BUFFER_SAMPLES;
        const int allocation = std::max(count, capacity) + 64;
        auto* in = dsp::buffer::alloc<dsp::complex_t>(count);
        auto* out = dsp::buffer::alloc<dsp::complex_t>(allocation);
        for (int i = 0; i < count; ++i) in[i] = {0.01f, 0.02f};
        for (int i = 0; i < allocation; ++i) out[i] = {12345.0f, 12345.0f};
        const int produced = vfo.process(count, in, out);
        int overwritten = 0;
        for (int i = capacity; i < allocation; ++i) {
            if (out[i].re != 12345.0f || out[i].im != 12345.0f) ++overwritten;
        }
        std::printf("input=%d output=%d capacity=%d guard writes=%d\n",
                    count, produced, capacity, overwritten);
        ok = ok && produced > 0 && produced <= capacity && overwritten == 0;
        dsp::buffer::free(in);
        dsp::buffer::free(out);
    }
    return ok ? 0 : 1;
}
