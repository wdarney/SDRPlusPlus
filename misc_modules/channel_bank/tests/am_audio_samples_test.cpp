#include "../src/am_audio_demod.h"
#include <dsp/channel/rx_vfo.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {
constexpr int rate = 48000;
constexpr double pi = 3.14159265358979323846;
int failures = 0;
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

std::vector<dsp::complex_t> tone(float carrier, float depth) {
    std::vector<dsp::complex_t> input(rate);
    for (int i = 0; i < rate; ++i) {
        const float amplitude = carrier * (1.0f + depth * std::sin(2.0 * pi * 1000.0 * i / rate));
        input[i] = {amplitude, 0.0f};
    }
    return input;
}

std::vector<dsp::stereo_t> demodulate(const std::vector<dsp::complex_t>& input,
                                   const std::vector<int>& blocks) {
    channel_bank_audio::AmDemod demod;
    dsp::stream<dsp::complex_t> stream;
    demod.init(&stream, 10000.0, rate);
    std::vector<dsp::stereo_t> output(input.size());
    int at = 0;
    size_t block = 0;
    while (at < static_cast<int>(input.size())) {
        const int count = std::min(blocks[block++ % blocks.size()], static_cast<int>(input.size()) - at);
        demod.process(count, input.data() + at, output.data() + at);
        at += count;
    }
    return output;
}

double rms(const std::vector<dsp::stereo_t>& samples) {
    double power = 0.0;
    for (int i = rate / 2; i < rate; ++i) power += samples[i].l * samples[i].l;
    return std::sqrt(power / (rate / 2));
}

void widebandContinuity() {
    dsp::stream<dsp::complex_t> input;
    dsp::channel::RxVFO vfo(&input, 32000000.0, rate, 10000.0, 100000.0);
    channel_bank_audio::AmDemod demod;
    demod.init(&vfo.out, 10000.0, rate);
    std::vector<dsp::complex_t> iq(65536), translated(65536);
    std::vector<dsp::stereo_t> audio(65536);
    constexpr int total = 8000000; // 250 ms of continuous 32 MS/s IQ.
    int produced = 0;
    float previous = 0, maxStep = 0, maxPeak = 0;
    for (int at = 0, block = 0; at < total; ++block) {
        const int count = std::min(block % 3 == 0 ? 65536 : 32768, total - at);
        for (int i = 0; i < count; ++i) {
            const double t = static_cast<double>(at + i) / 32000000.0;
            const double amplitude = 0.02 * (1.0 + 0.4 * std::sin(2 * pi * 1000 * t));
            iq[i] = {static_cast<float>(amplitude * std::cos(2 * pi * 100000 * t)),
                     static_cast<float>(amplitude * std::sin(2 * pi * 100000 * t))};
        }
        const int countOut = vfo.process(count, iq.data(), translated.data());
        demod.process(countOut, translated.data(), audio.data());
        for (int i = 0; i < countOut; ++i, ++produced) {
            const float sample = audio[i].l;
            if (produced > rate / 5) {
                maxStep = std::max(maxStep, std::abs(sample - previous));
                maxPeak = std::max(maxPeak, std::abs(sample));
            }
            previous = sample;
        }
        at += count;
    }
    check(std::abs(produced - rate / 4) <= 2, "32 MS/s conversion must preserve sample duration");
    check(maxPeak > 0.15f && maxPeak < 0.4f, "wideband AM output must preserve modulation");
    check(maxStep < 0.055f, "continuous wideband IQ must not create boundary impulses");
    std::printf("32 MS/s pipeline: %d audio samples, peak %.6f, max step %.6f\n", produced, maxPeak, maxStep);
}
}

int main() {
    const auto input = tone(0.02f, 0.4f);
    const auto whole = demodulate(input, {rate});
    // 32 MS/s RX888 blocks produce very short 48 kHz blocks. Include those,
    // irregular edges and blocks longer than the entire recording fade.
    const auto split = demodulate(input, {49, 50, 1, 480, 7, 4096, 2399, 2401});
    float maxDifference = 0.0f;
    for (int i = 0; i < rate; ++i) {
        maxDifference = std::max(maxDifference, std::abs(whole[i].l - split[i].l));
        check(split[i].l == split[i].r, "mono expansion must preserve both channels");
    }
    check(maxDifference < 2e-6f, "AM output must not change when callbacks are split");
    std::printf("callback partition max difference: %.9g\n", maxDifference);

    const auto strong = demodulate(tone(2.0f, 0.4f), {32768, 49});
    check(std::abs(rms(strong) / rms(whole) - 1.0) < 0.002,
          "100x carrier amplitude must preserve modulation loudness");
    check(rms(whole) > 0.18 && rms(whole) < 0.23, "40 percent modulation must remain audible");

    const auto carrier = demodulate(tone(0.5f, 0.0f), {49, 50});
    check(rms(carrier) < 1e-7, "steady carrier must not generate periodic audio impulses");
    channel_bank_audio::AmSamples safe;
    safe.configure(rate);
    check(safe.finish(safe.envelope(0, 0)) == 0, "zero IQ must stay silent");
    safe.envelope(std::numeric_limits<float>::infinity(), 0);
    check(std::isfinite(safe.finish(safe.envelope(0.5f, 0))), "invalid IQ must not poison future samples");

    channel_bank_audio::AmRecordingEnvelope fade;
    fade.reset();
    float previous = 1.0f;
    float maxStep = 0.0f;
    // Reverse an in-progress fade, then let it close and reopen completely.
    for (int i = 0; i < 12000; ++i) {
        const bool present = (i >= 1173 && i < 3500) || i >= 8000;
        const float gain = fade.next(present);
        maxStep = std::max(maxStep, std::abs(gain - previous));
        check(gain >= 0 && gain <= 1, "fade must stay bounded");
        if (i == 7500) check(fade.silent() && gain == 0, "fade must reach true silence");
        previous = gain;
    }
    check(maxStep < 0.00066f, "AM fade/recovery must not jump at a buffer boundary");
    check(previous == 1.0f && !fade.silent(), "recovery must reach full gain");

    // Simulate the actual pre-roll ring and opening callback. Every sample
    // after the oldest retained history must occur once, even for huge blocks.
    for (const int currentCount : {1, 49, 480, 19200, 25000}) {
        constexpr int capacity = 19200;
        constexpr int prior = 15000;
        const int head = (prior + currentCount) % capacity;
        const int available = std::min(capacity, prior + currentCount);
        std::vector<int> ring(capacity);
        for (int i = 0; i < prior + currentCount; ++i) ring[i % capacity] = i;
        const int history = channel_bank_audio::preRollHistoryCount(available, currentCount);
        const int start = (head - available + capacity) % capacity;
        std::vector<int> written;
        for (int i = 0; i < history; ++i) written.push_back(ring[(start + i) % capacity]);
        for (int i = prior; i < prior + currentCount; ++i) written.push_back(i);
        for (size_t i = 1; i < written.size(); ++i)
            check(written[i] == written[i - 1] + 1, "pre-roll/current block must be continuous without duplicates");
        check(written.back() == prior + currentCount - 1, "opening callback must retain its last sample");
    }
    std::printf("AM fade max sample step: %.9g\n", maxStep);
    widebandContinuity();
    return failures ? 1 : 0;
}
