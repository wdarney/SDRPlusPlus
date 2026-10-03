#include "../src/continuous_iq_splitter.h"
#include <future>
#include <iostream>
#include <stdexcept>

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
struct PausedStream : dsp::stream<float> {
    std::promise<void> secondAttempt;
    bool notified = false;
    bool swap(int count) override {
        if (writeBuf[0] == 2 && !notified) {
            notified = true;
            secondAttempt.set_value();
        }
        return dsp::stream<float>::swap(count);
    }
};
void consume(dsp::stream<float>& stream, float expected) {
    require(stream.read() == 1, "wrong block size");
    require(stream.readBuf[0] == expected, "missing or repeated sample during membership change");
    stream.flush();
}
void membershipChange(bool removePaused) {
    dsp::stream<float> input, early, late, added;
    PausedStream paused;
    channel_bank::ContinuousIQSplitter<float> splitter(&input);
    splitter.bindStream(&early);
    splitter.bindStream(&paused);
    splitter.bindStream(&late);
    splitter.start();
    input.writeBuf[0] = 1;
    require(input.swap(1), "first publish");
    consume(early, 1);
    consume(late, 1); // Paused consumer still owns block 1.
    input.writeBuf[0] = 2;
    require(input.swap(1), "second publish");
    consume(early, 2); // This consumer must NOT receive block 2 again on restart.
    paused.secondAttempt.get_future().wait();
    if (removePaused) splitter.unbindStream(&paused);
    else splitter.bindStream(&added);
    consume(paused, 1);
    if (!removePaused) consume(paused, 2);
    consume(late, 2); // Core splitter previously lost this block.
    if (!removePaused) consume(added, 2);
    input.writeBuf[0] = 3;
    require(input.swap(1), "third publish");
    consume(early, 3);
    if (!removePaused) consume(paused, 3);
    consume(late, 3);
    if (!removePaused) consume(added, 3);
    splitter.stop();
}
int main() {
    for (int i=0;i<20;++i) {
        membershipChange(false);
        membershipChange(true);
    }
    // Exact off-grid frequency from the affected RadioMac run: admission and
    // slot identity must map to the same key used by the block list.
    require(channel_bank::channelGridFrequency(118003055.0, 8333.0) == 118003613.0,
            "blocked channel grid identity mismatch");
    require(channel_bank::channelGridFrequency(127994322.0, 8333.0) == 127994880.0,
            "second blocked channel grid identity mismatch");
    std::cout << "40 live add/remove cases preserved every survivor sample without duplication\n";
}
