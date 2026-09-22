#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#ifndef CB_NO_RNNOISE
#include <rnnoise.h>
#endif

namespace channel_bank {

// Start-of-transmission confirmation only. Never modifies the supplied audio.
// process() is audio-thread owned; readers see a generation-tagged decision so
// a quick off/on toggle cannot resurrect a previous acceptance or rejection.
class VoiceSquelch {
public:
    enum Decision : uint64_t { Pending = 0, Confirmed = 1, Rejected = 2 };
#ifdef CB_NO_RNNOISE
    static constexpr bool available = false;
#else
    static constexpr bool available = true;
#endif
    ~VoiceSquelch() {
#ifndef CB_NO_RNNOISE
        if (state_) rnnoise_destroy(state_);
#endif
    }

    Decision decision(uint64_t policy) const {
        const auto value = published_.load();
        return (value >> 2) == policy ? Decision(value & 3) : Pending;
    }

    template<class Stereo>
    void process(const Stereo* audio, int count, uint64_t policy, bool rfPresent, bool fileOpen) {
#ifndef CB_NO_RNNOISE
        if (policy_ != policy) {
            policy_ = policy;
            reset();
            episode_ = false;
        }
        if (!(policy & 1)) return;
        if (!rfPresent && !fileOpen) {
            if (episode_) reset();
            episode_ = false;
            return;
        }
        episode_ = true;
        // Enabling mid-call must not cut up an already-open recording. Once
        // confirmed, ordinary RF hold/tail rules own the rest of the call.
        if (fileOpen || decision(policy) == Confirmed) {
            publish(Confirmed);
            return;
        }
        if (!state_) state_ = rnnoise_create(nullptr);
        if (!state_) { publish(Rejected); return; }
        for (int i = 0; i < count; ++i) {
            float sample = (audio[i].l + audio[i].r) * 0.5f;
            frame_[pos_++] = std::isfinite(sample)
                ? std::clamp(sample, -1.0f, 1.0f) * 32768.0f : 0.0f;
            if (pos_ != 480) continue;
            float discarded[480];
            const float probability = rnnoise_process_frame(state_, discarded, frame_);
            pos_ = 0;
            frames_ = std::min(frames_ + 1, 60);
            votes_ = ((votes_ << 1) | (probability >= 0.60f ? 1u : 0u)) & 0x3ffu;
            unsigned hits = 0;
            for (unsigned bits = votes_; bits; bits >>= 1) hits += bits & 1u;
            // Let the model settle for 200 ms, then require at least 3 of the
            // latest 10 frames to look like speech. 600 ms bounds scan probing.
            // Rejected fixed/manual slots keep listening for later speech.
            if (frames_ >= 20 && hits >= 3) { publish(Confirmed); return; }
            if (frames_ >= 60) publish(Rejected);
        }
#endif
    }

private:
    void publish(Decision decision) { published_.store((policy_ << 2) | decision); }
    void reset() {
#ifndef CB_NO_RNNOISE
        if (state_) rnnoise_init(state_, nullptr);
#endif
        pos_ = frames_ = 0;
        votes_ = 0;
        publish(Pending);
    }
    std::atomic<uint64_t> published_{0};
    uint64_t policy_ = 0;
    bool episode_ = false;
    int pos_ = 0, frames_ = 0;
    unsigned votes_ = 0;
#ifndef CB_NO_RNNOISE
    DenoiseState* state_ = nullptr;
    float frame_[480]{};
#endif
};

} // namespace channel_bank
