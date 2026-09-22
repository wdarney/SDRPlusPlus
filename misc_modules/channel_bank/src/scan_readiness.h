#pragma once
#include <chrono>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace channel_bank_scan {
// Caller serializes this state with FFT collection and scan snapshots.
class Readiness {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int settleMs = 250;
    static constexpr unsigned requiredFrames = 3;

    void reset() { *this = Readiness{}; }
    void request() {
        ++generation;
        waiting = true;
        settling = false;
        frames = 0;
        firstFrame = {};
    }
    void acknowledge(double sampleRate, Clock::time_point now) {
        if (!waiting) request();
        waiting = false;
        settling = true;
        settleUntil = now + std::chrono::milliseconds(settleMs);
        samplesLeft = static_cast<int64_t>(std::ceil(sampleRate * settleMs / 1000.0));
    }
    bool discard(int count, Clock::time_point now) {
        if (waiting) return true;
        if (!settling) return false;
        samplesLeft = std::max<int64_t>(0, samplesLeft - std::max(0, count));
        if (samplesLeft == 0 && now >= settleUntil) settling = false;
        // Always discard the whole block that crosses the settling boundary.
        return true;
    }
    void frame(Clock::time_point now) {
        if (waiting || settling) return;
        if (frames == 0) firstFrame = now;
        if (frames < requiredFrames) ++frames;
    }
    bool ready() const { return !waiting && !settling && frames >= requiredFrames; }
    bool awaitingTune() const { return waiting; }
    uint64_t generation = 0;
    Clock::time_point firstFrame{};
private:
    bool waiting = false;
    bool settling = false;
    unsigned frames = 0;
    int64_t samplesLeft = 0;
    Clock::time_point settleUntil{};
};

// Retuning discovery must not tear down independently hosted recordings.
template<class Channels, class Dispose>
void clearDiscoveryChannels(Channels& channels, Dispose dispose) {
    for (auto it = channels.begin(); it != channels.end();) {
        if (it->second->multiReceiver) { ++it; continue; }
        dispose(it->second);
        it = channels.erase(it);
    }
}
} // namespace channel_bank_scan
