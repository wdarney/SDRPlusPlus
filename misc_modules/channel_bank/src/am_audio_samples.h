#pragma once

#include <algorithm>
#include <cmath>

namespace channel_bank_audio {

// Sample-clocked AM envelope normalization. See AUDIO_SAMPLE_PROCESSING.md
// for the recovered behavior and the deliberately different startup guard.
class AmSamples {
public:
    void configure(double sampleRate) {
        envelopeKeep = std::exp(-1.0 / (sampleRate * 0.3));
        highPassKeep = std::exp(-2.0 * 3.14159265358979323846 * 300.0 / sampleRate);
        reset();
    }

    void reset() {
        carrier = 0.0;
        weight = 0.0;
        previousInput = previousOutput = 0.0;
    }

    float envelope(float real, float imag) {
        const double magnitude = std::hypot(static_cast<double>(real), static_cast<double>(imag));
        if (!std::isfinite(magnitude)) {
            reset();
            return 0.0f;
        }
        // Correct the initial EMA bias, including a channelizer's gradual
        // filter startup. Seeding from its first tiny nonzero sample would
        // amplify the next few hundred milliseconds. Initial zero IQ carries
        // no observation weight; normal carrier loss keeps existing history.
        if (weight == 0.0 && magnitude <= 1e-5) return 0.0f;
        carrier += (1.0 - envelopeKeep) * (magnitude - carrier);
        weight += (1.0 - envelopeKeep) * (1.0 - weight);
        const double estimate = carrier / weight;
        return static_cast<float>(0.25 * (magnitude - estimate) / std::max(estimate, 1e-5));
    }

    // Called after the existing bandwidth filter, once for every sample.
    float finish(float input) {
        if (!std::isfinite(input)) {
            previousInput = previousOutput = 0.0;
            return 0.0f;
        }
        const double output = highPassKeep * (previousOutput + input - previousInput);
        previousInput = input;
        previousOutput = output;
        return static_cast<float>(std::clamp(3.0 * output, -1.0, 1.0));
    }

private:
    double envelopeKeep = 0.0;
    double highPassKeep = 0.0;
    double carrier = 0.0;
    double weight = 0.0;
    double previousInput = 0.0;
    double previousOutput = 0.0;
};

// The RF decision still belongs to Channel Bank. Only the transition between
// its requested levels is sample-clocked, including recovery during a fade.
class AmRecordingEnvelope {
public:
    void reset(float value = 1.0f) {
        level = start = target = value;
        position = fadeSamples;
    }

    float next(bool signalPresent) {
        const float requested = signalPresent ? 1.0f : 0.0f;
        if (requested != target) {
            start = level;
            target = requested;
            position = 0;
        }
        if (position < fadeSamples) {
            const double progress = static_cast<double>(++position) / fadeSamples;
            const double ramp = 0.5 - 0.5 * std::cos(3.14159265358979323846 * progress);
            level = static_cast<float>(start + (target - start) * ramp);
        }
        return level;
    }

    bool silent() const { return target == 0.0f && position == fadeSamples; }

private:
    static constexpr int fadeSamples = 2400; // Existing 50 ms fade at 48 kHz.
    float level = 1.0f;
    float start = 1.0f;
    float target = 1.0f;
    int position = fadeSamples;
};

// The current callback has already entered the pre-roll ring. Flush only its
// predecessor samples; the normal writer below will consume the current block.
inline int preRollHistoryCount(int available, int currentCount) {
    return std::max(0, available - currentCount);
}

} // namespace channel_bank_audio
