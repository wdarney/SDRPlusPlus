#pragma once

#include <dsp/stream.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace cbdiag {
using Clock = std::chrono::steady_clock;
inline bool enabled() {
    const char* value = std::getenv("SDRPP_CB_SAMPLE_DIAGNOSTICS");
    return value && std::strcmp(value, "1") == 0;
}
inline double milliseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

// Single producer / single consumer, like dsp::stream. Producer and consumer
// own separate fields. Inspect only after BOTH ends have stopped; live reads
// of these counters would race. No sample contents or handoff rules change.
template<class T>
class Stream : public dsp::stream<T> {
public:
    uint64_t published = 0, received = 0, released = 0;
    uint64_t publishedBlocks = 0, receivedBlocks = 0;
    uint64_t cancelledSamples = 0, cancelledBlocks = 0;
    uint64_t repeatedReads = 0, unpairedFlushes = 0;
    double maxPublishMs = 0, maxHoldMs = 0;

    bool swap(int count) override {
        const auto begin = Clock::now();
        const bool ok = dsp::stream<T>::swap(count);
        maxPublishMs = std::max(maxPublishMs, milliseconds(Clock::now() - begin));
        if (ok) { published += count; ++publishedBlocks; }
        else { cancelledSamples += count; ++cancelledBlocks; }
        return ok;
    }
    int read() override {
        const int count = dsp::stream<T>::read();
        if (count >= 0) {
            if (pending) ++repeatedReads;
            received += count;
            ++receivedBlocks;
            pending = true;
            pendingCount = count;
            heldSince = Clock::now();
        }
        return count;
    }
    void flush() override {
        if (pending) {
            released += pendingCount;
            maxHoldMs = std::max(maxHoldMs, milliseconds(Clock::now() - heldSince));
            pending = false;
        } else ++unpairedFlushes;
        dsp::stream<T>::flush();
    }
private:
    bool pending = false;
    int pendingCount = 0;
    Clock::time_point heldSince;
};

struct Handler {
    uint64_t input = 0, warmup = 0, noFile = 0, trim = 0;
    uint64_t eligible = 0, preroll = 0, writeRequested = 0;
    double maxCallbackMs = 0;
};
class CallbackScope {
public:
    CallbackScope(Handler* stats, int count) : stats(stats) {
        if (stats) { begin = Clock::now(); stats->input += count; }
    }
    ~CallbackScope() {
        if (stats) stats->maxCallbackMs = std::max(stats->maxCallbackMs,
            milliseconds(Clock::now() - begin));
    }
private:
    Handler* stats;
    Clock::time_point begin;
};
} // namespace cbdiag

#include <dsp/channel/rx_vfo.h>
namespace cbdiag {
// Same run sequence as RxVFO, with observations around process and publication.
class VFO : public dsp::channel::RxVFO {
public:
    using dsp::channel::RxVFO::RxVFO;
    uint64_t inputSamples = 0, outputSamples = 0, publishedSamples = 0;
    double maxProcessMs = 0, maxOutputWaitMs = 0;
    int run() override {
        const int count = _in->read();
        if (count < 0) return -1;
        const auto begin = Clock::now();
        const int outCount = process(count, _in->readBuf, out.writeBuf);
        maxProcessMs = std::max(maxProcessMs, milliseconds(Clock::now() - begin));
        inputSamples += count;
        outputSamples += outCount;
        _in->flush();
        if (outCount) {
            const auto publishBegin = Clock::now();
            const bool ok = out.swap(outCount);
            maxOutputWaitMs = std::max(maxOutputWaitMs, milliseconds(Clock::now() - publishBegin));
            if (!ok) return -1;
            publishedSamples += outCount;
        }
        return outCount;
    }
};
} // namespace cbdiag
