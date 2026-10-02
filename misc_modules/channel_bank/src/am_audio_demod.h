#pragma once

#include "am_audio_samples.h"
#include <dsp/processor.h>
#include <dsp/filter/fir.h>
#include <dsp/taps/low_pass.h>

namespace channel_bank_audio {

// Module-local AM path. The VFO, bandwidth, output rate and stereo interface
// remain the same as the other Channel Bank demodulators.
class AmDemod : public dsp::Processor<dsp::complex_t, dsp::stereo_t> {
    using Base = dsp::Processor<dsp::complex_t, dsp::stereo_t>;
public:
    ~AmDemod() {
        if (!Base::_block_init) return;
        Base::stop();
        dsp::taps::free(taps);
    }

    void init(dsp::stream<dsp::complex_t>* input, double bandwidth, double sampleRate) {
        samples.configure(sampleRate);
        taps = dsp::taps::lowPass(bandwidth / 2.0, bandwidth * 0.05, sampleRate);
        filter.init(nullptr, taps);
        filter.out.free(); // The filter is processed inline into our output.
        Base::init(input);
    }

    int process(int count, const dsp::complex_t* input, dsp::stereo_t* output) {
        // The stereo allocation has room for this mono intermediate. Expand it
        // backwards only after the stateful filters have run in sample order.
        float* mono = reinterpret_cast<float*>(output);
        for (int i = 0; i < count; ++i)
            mono[i] = samples.envelope(input[i].re, input[i].im);
        filter.process(count, mono, mono);
        for (int i = 0; i < count; ++i) mono[i] = samples.finish(mono[i]);
        for (int i = count; i-- > 0;) output[i] = {mono[i], mono[i]};
        return count;
    }

    int run() {
        const int count = Base::_in->read();
        if (count < 0) return -1;
        process(count, Base::_in->readBuf, Base::out.writeBuf);
        Base::_in->flush();
        return Base::out.swap(count) ? count : -1;
    }

private:
    AmSamples samples;
    dsp::tap<float> taps{};
    dsp::filter::FIR<float, float> filter;
};

} // namespace channel_bank_audio
