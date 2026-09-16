#pragma once
#include <dsp/sink/handler_sink.h>
#include <atomic>
#include <memory>
#include <stdexcept>

// One Handler per activation. Sink::init() appends input registrations, so an
// initialized Handler must never be reused after its VFO stream is destroyed.
// Control methods run on the module's serialized lifecycle thread. Worker code
// can read accepting() without taking that lifecycle lock.
class VDL2ChannelInput {
public:
    using Callback = void (*)(dsp::complex_t*, int, void*);
    VDL2ChannelInput() = default;
    VDL2ChannelInput(const VDL2ChannelInput&) = delete;
    VDL2ChannelInput& operator=(const VDL2ChannelInput&) = delete;
    ~VDL2ChannelInput() { stop(); }

    void start(dsp::stream<dsp::complex_t>* stream, Callback callback, void* context) {
        if (sink || input) throw std::logic_error("VDL2 input already attached");
        if (!stream || !callback) throw std::invalid_argument("VDL2 input requires a stream and callback");
        // Construct before publishing input, so allocation failure owns nothing.
        auto next = std::make_unique<dsp::sink::Handler<dsp::complex_t>>();
        next->init(stream, callback, context);
        input = stream;
        sink = std::move(next);
        acceptingWork.store(true);
        // If thread creation throws, the module's rollback still stops/releases
        // this initialized block while its input stream is alive.
        sink->start();
    }

    bool accepting() const { return acceptingWork.load(); }
    void rejectWork() { acceptingWork.store(false); }
    void disconnect() {
        rejectWork();
        // Wake a producer blocked on this private VFO output. Do not stop the
        // shared IQ splitter or another channel's stream.
        if (input) input->stopWriter();
    }
    void join() {
        if (sink) {
            sink->stop(); // doStop joins the Handler, including its callback.
            sink.reset(); // unregister references by destroying the block alive.
        }
        input = nullptr;
    }
    void stop() { disconnect(); join(); }
private:
    std::atomic<bool> acceptingWork{false};
    dsp::stream<dsp::complex_t>* input = nullptr; // VFO owns it until join returns.
    std::unique_ptr<dsp::sink::Handler<dsp::complex_t>> sink;
};

// Shared shutdown phases: no channel's VFO is deleted until every consumer has
// joined. closeCallbacks must release its lock before returning (join can wait
// for a callback). release removes producers/registrations and resets decoders.
template<class Channels, class CloseCallbacks, class Release>
void stopVDL2Channels(Channels& channels, CloseCallbacks closeCallbacks, Release release) {
    for (auto& channel : channels) channel.input.rejectWork();
    closeCallbacks();
    for (auto& channel : channels) channel.input.disconnect();
    for (auto& channel : channels) channel.input.join();
    for (auto& channel : channels) release(channel);
}
