#include "../src/sample_diagnostics.h"
#include <future>
#include <iostream>
#include <vector>
#include <stdexcept>

void require(bool ok) { if (!ok) throw std::runtime_error("diagnostic regression failed"); }
int main() {
    cbdiag::Stream<float> stream;
    const std::vector<int> blocks{1,49,480,32768,7};
    auto producer = std::async(std::launch::async, [&] {
        int sample = 0;
        for (int count : blocks) {
            for (int i=0;i<count;++i) stream.writeBuf[i] = float(sample++);
            require(stream.swap(count));
        }
    });
    int sample = 0;
    for (int count : blocks) {
        require(stream.read() == count);
        for (int i=0;i<count;++i) require(stream.readBuf[i] == float(sample++));
        stream.flush();
    }
    producer.get();
    require(stream.published == sample && stream.received == sample && stream.released == sample);
    require(stream.repeatedReads == 0 && stream.unpairedFlushes == 0);
    require(stream.publishedBlocks == blocks.size());
    // Duplicate reads are protocol misuse, even though base stream allows it.
    stream.writeBuf[0] = 1;
    require(stream.swap(1));
    require(stream.read() == 1 && stream.read() == 1);
    stream.flush();
    require(stream.repeatedReads == 1);
    stream.stopWriter();
    require(!stream.swap(4));
    require(stream.cancelledSamples == 4 && stream.cancelledBlocks == 1);
    // Instrumented run must produce exactly the same samples as the original
    // RxVFO process across multiple blocks, including its resampler history.
    cbdiag::Stream<dsp::complex_t> iq;
    cbdiag::VFO measured(&iq, 32000000.0, 48000.0, 10000.0, 100000.0);
    dsp::channel::RxVFO reference(&iq, 32000000.0, 48000.0, 10000.0, 100000.0);
    std::vector<dsp::complex_t> expected(65536);
    uint64_t inputs = 0, outputs = 0;
    for (int count : {32768, 65536, 49, 32768}) {
        for (int i=0;i<count;++i) iq.writeBuf[i] = {float((inputs+i)%997)/997.0f, 0.1f};
        const int want = reference.process(count, iq.writeBuf, expected.data());
        require(iq.swap(count));
        require(measured.run() == want);
        if (want) {
            require(measured.out.read() == want);
            for (int i=0;i<want;++i) {
                require(measured.out.readBuf[i].re == expected[i].re);
                require(measured.out.readBuf[i].im == expected[i].im);
            }
            measured.out.flush();
        }
        inputs += count;
        outputs += want;
    }
    require(measured.inputSamples == inputs && measured.outputSamples == outputs);
    require(measured.publishedSamples == outputs && iq.released == inputs);
    cbdiag::Handler handler;
    { cbdiag::CallbackScope scope(&handler, 17); handler.warmup += 17; }
    require(handler.input == 17 && handler.warmup == 17);
    { cbdiag::CallbackScope scope(nullptr, 17); }
    std::cout << "sample accounting, payload continuity, duplicate read and stop cancellation passed\n";
}
