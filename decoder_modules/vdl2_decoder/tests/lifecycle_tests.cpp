#include "channel_input.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (0)
using namespace std::chrono_literals;

// Real SDR++ streams/Handler/block::doStop. Only RF/VFO production and protocol
// work are replaced here; the input owner and stop phases are production code.
struct Channel {
    std::unique_ptr<dsp::stream<dsp::complex_t>> stream;
    VDL2ChannelInput input;
    std::thread producer;
    std::atomic<unsigned> calls{0}, inFlight{0};
    std::atomic<bool> decoderAlive{true}, badLifetime{false};
    bool hold = false, entered = false, released = false;
    std::mutex holdMutex;
    std::condition_variable holdCV;

    ~Channel() { stop(); decoderAlive = false; }
    static void callback(dsp::complex_t*, int, void* context) {
        auto& channel = *static_cast<Channel*>(context);
        if (!channel.input.accepting()) return;
        ++channel.inFlight;
        {
            std::unique_lock<std::mutex> lock(channel.holdMutex);
            channel.entered = true;
            channel.holdCV.notify_all();
            if (channel.hold) channel.holdCV.wait(lock, [&] { return channel.released; });
        }
        if (!channel.decoderAlive) channel.badLifetime = true;
        if (channel.input.accepting()) ++channel.calls;
        --channel.inFlight;
    }
    void prepare() {
        CHECK(!stream);
        stream = std::make_unique<dsp::stream<dsp::complex_t>>();
        stream->setBufferSize(64);
        decoderAlive = true; entered = false; released = false;
    }
    void start(bool withProducer = true) {
        prepare();
        input.start(stream.get(), callback, this);
        if (withProducer) producer = std::thread([this] {
            while (true) {
                for (int i = 0; i < 64; ++i) stream->writeBuf[i] = {0, 0};
                if (!stream->swap(64)) break;
            }
        });
    }
    void release() {
        CHECK(inFlight == 0);
        if (producer.joinable()) producer.join();
        stream.reset(); // the Handler and its input registrations are gone
        decoderAlive = false; // models protocol/reassembly reset/destruction
        CHECK(!badLifetime);
    }
    void stop() { input.stop(); release(); }
    void waitForCallback() {
        std::unique_lock<std::mutex> lock(holdMutex);
        CHECK(holdCV.wait_for(lock, 5s, [&] { return entered; }));
    }
};

template<size_t N> void stopAll(std::array<Channel, N>& channels) {
    stopVDL2Channels(channels, [] {}, [&](Channel& channel) {
        for (auto& other : channels) CHECK(other.inFlight == 0);
        channel.release();
    });
}

int main() try {
    // This count is generated from the production channel table by CMake.
    std::array<Channel, VDL2_CONFIGURED_CHANNELS> channels;
    for (int cycle = 0; cycle < 12; ++cycle) {
        for (auto& channel : channels) channel.start();
        for (auto& channel : channels) channel.waitForCallback();
        // Independently disable/re-enable the first channel while others run.
        channels.front().stop(); channels.front().stop(); channels.front().start();
        channels.front().waitForCallback();
        stopAll(channels); stopAll(channels); // Stop is idempotent
    }
    // A worker blocked waiting for IQ must join even with no source running.
    channels.front().start(false); stopAll(channels); stopAll(channels);
    // Failure during partial startup: previous workers and the newly created
    // producer stream exist, but the module-wide 'running' flag is still false.
    for (size_t failAt = 0; failAt < channels.size(); ++failAt) {
        try {
            for (size_t i = 0; i < channels.size(); ++i) {
                if (i == failAt) {
                    channels[i].prepare();
                    throw std::runtime_error("injected startup failure");
                }
                channels[i].start();
            }
        } catch (const std::runtime_error&) { stopAll(channels); }
        stopAll(channels);
    }
    // Pause a callback: shutdown must wait, keep its decoder alive, and reject
    // publication after the stopping gate closes.
    {
        Channel held; held.hold = true; held.start(); held.waitForCallback();
        held.input.rejectWork();
        auto stopped = std::async(std::launch::async, [&] { held.stop(); });
        CHECK(stopped.wait_for(20ms) == std::future_status::timeout);
        CHECK(held.decoderAlive && held.inFlight == 1);
        {
            std::lock_guard<std::mutex> lock(held.holdMutex); held.released = true;
        }
        held.holdCV.notify_all();
        CHECK(stopped.wait_for(5s) == std::future_status::ready); stopped.get();
        CHECK(held.calls == 0 && !held.decoderAlive);
    }
    // Destroy a running channel: destructor joins before destroying its state.
    { Channel running; running.start(); running.waitForCallback(); }
    // start() cannot silently initialize the same block/input twice.
    {
        Channel channel; channel.start(false);
        bool rejected = false;
        try { channel.input.start(channel.stream.get(), Channel::callback, &channel); }
        catch (const std::logic_error&) { rejected = true; }
        CHECK(rejected); channel.stop();
    }
    std::cout << "Lifecycle checks passed: " << channels.size()
              << " simultaneous channels, 12 full restarts, per-channel restarts, partial startup, callback drain, destruction\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
