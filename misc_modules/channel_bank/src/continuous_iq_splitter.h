#pragma once

#include <dsp/sink.h>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace channel_bank {
// A membership change temporarily stops publication, but must not release an
// input block until every remaining consumer has received it. The core splitter
// flushes on a cancelled swap, losing the block for this and later consumers.
template<class T>
class ContinuousIQSplitter : public dsp::Sink<T> {
    using Base = dsp::Sink<T>;
public:
    explicit ContinuousIQSplitter(dsp::stream<T>* input) { Base::init(input); }
    ~ContinuousIQSplitter() {
        Base::stop();
        if (pending) Base::_in->flush();
    }

    void bindStream(dsp::stream<T>* stream) {
        std::lock_guard<std::recursive_mutex> lock(Base::ctrlMtx);
        if (std::find(streams.begin(), streams.end(), stream) != streams.end())
            throw std::runtime_error("[ChannelBank splitter] stream already bound");
        Base::tempStop();
        Base::registerOutput(stream);
        streams.push_back(stream);
        Base::tempStart();
    }

    void unbindStream(dsp::stream<T>* stream) {
        std::lock_guard<std::recursive_mutex> lock(Base::ctrlMtx);
        auto found = std::find(streams.begin(), streams.end(), stream);
        if (found == streams.end())
            throw std::runtime_error("[ChannelBank splitter] stream not bound");
        Base::tempStop();
        streams.erase(found);
        Base::unregisterOutput(stream);
        // Remove identity while stopped, before the caller can free/reuse it.
        delivered.erase(std::remove(delivered.begin(), delivered.end(), stream), delivered.end());
        Base::tempStart();
    }

    int run() override {
        if (!pending) {
            count = Base::_in->read();
            if (count < 0) return -1;
            pending = true;
            delivered.clear();
        }
        for (auto* stream : streams) {
            if (std::find(delivered.begin(), delivered.end(), stream) != delivered.end()) continue;
            std::memcpy(stream->writeBuf, Base::_in->readBuf, count * sizeof(T));
            if (!stream->swap(count)) return -1; // Retain input and publication progress.
            delivered.push_back(stream);
        }
        Base::_in->flush();
        pending = false;
        return count;
    }
private:
    std::vector<dsp::stream<T>*> streams;
    std::vector<dsp::stream<T>*> delivered;
    bool pending = false;
    int count = 0;
};

inline double channelGridFrequency(double frequency, double spacing) {
    return std::round(frequency / spacing) * spacing;
}
} // namespace channel_bank
