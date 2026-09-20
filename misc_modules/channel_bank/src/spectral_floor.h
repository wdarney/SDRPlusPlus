#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace channel_bank_detector {

// Slide the existing energy window through an Auto slot rather than inspecting
// only its center. A sharper FFT must not leave narrow signals in the gaps
// between fixed windows. Prefix sums keep the full-slot search linear in the
// number of spectrum bins, without introducing a carrier/peak classifier.
class AutoEnergyWindows {
public:
    explicit AutoEnergyWindows(const std::vector<float>& power) : prefix_(power.size() + 1, 0.0) {
        for (std::size_t i = 0; i < power.size(); ++i)
            prefix_[i + 1] = prefix_[i] + power[i];
    }

    float mean(int center, int halfWidth) const {
        if (prefix_.size() < 2) return 0.0f;
        const int last = static_cast<int>(prefix_.size()) - 2;
        center = std::clamp(center, 0, last);
        halfWidth = std::max(0, halfWidth);
        const int lo = std::max(0, center - halfWidth);
        const int hi = std::min(last, center + halfWidth);
        return static_cast<float>((prefix_[hi + 1] - prefix_[lo]) / (hi - lo + 1));
    }

    int strongestCenter(int nominalCenter, int halfSlot, int halfWidth) const {
        if (prefix_.size() < 2) return 0;
        const int last = static_cast<int>(prefix_.size()) - 2;
        int best = std::clamp(nominalCenter, 0, last);
        float bestMean = mean(best, halfWidth);
        for (int center = std::max(0, nominalCenter - halfSlot);
             center <= std::min(last, nominalCenter + halfSlot); ++center) {
            float candidate = mean(center, halfWidth);
            if (candidate > bestMean) { best = center; bestMean = candidate; }
        }
        return best;
    }

private:
    std::vector<double> prefix_;
};

// Bound CPU/memory use while retaining ~250 Hz bins through 64 MS/s.
inline int fftSizeForRate(double sampleRate) {
    int size = 8192;
    while (size < 262144 && sampleRate / size > 250.0) size *= 2;
    return size;
}

// Collect real consecutive samples, even when a transform spans several source
// buffers. The remaining samples in each 50 ms period are skipped. At low rates
// use a shorter real window and let the caller zero-pad the transform.
class FrameCollector {
public:
    void reset() { filled_ = 0; skip_ = 0; }

    template <typename Sample, typename Callback>
    void feed(const Sample* data, int count, Sample* frame, int size,
              double sampleRate, Callback ready) {
        if (size <= 0 || count <= 0 || !std::isfinite(sampleRate) || sampleRate <= 0.0) return;
        const int64_t period = std::max<int64_t>(1, std::llround(sampleRate / 20.0));
        const int target = static_cast<int>(std::min<int64_t>(size, period));
        while (count > 0) {
            if (skip_ > 0) {
                const int n = static_cast<int>(std::min<int64_t>(count, skip_));
                data += n; count -= n; skip_ -= n;
                continue;
            }
            const int n = std::min(count, target - filled_);
            std::copy_n(data, n, frame + filled_);
            filled_ += n; data += n; count -= n;
            if (filled_ == target) {
                std::fill(frame + target, frame + size, Sample{});
                ready(target);
                filled_ = 0;
                skip_ = period - target;
            }
        }
    }

private:
    int filled_ = 0;
    int64_t skip_ = 0;
};

// A lower-quantile estimate in overlapping 128 kHz neighborhoods rejects sparse
// carriers without depending on a bookmark list. Anchors every 32 kHz are
// interpolated in dB, then averaged over each detector's actual signal window.
// Use instantaneous FFT powers: Gaussian IQ noise has exponential bin power,
// whose 20th percentile is -ln(0.8) times its mean. Correcting that bias keeps
// noise-only channel SNR near zero, independently of spectrum EMA warm-up.
class SpectralFloor {
public:
    void reset() { anchorsDb_.clear(); prefix_.clear(); curve_.clear(); }

    void update(const std::vector<float>& power, double binHz) {
        if (power.size() < 2 || !std::isfinite(binHz) || !(binHz > 0.0)) { reset(); return; }
        const int n = static_cast<int>(power.size());
        const int stride = std::max(1, static_cast<int>(std::lround(32000.0 / binHz)));
        const int radius = std::max(32, static_cast<int>(std::lround(64000.0 / binHz)));
        const int count = (n - 1 + stride - 1) / stride + 1;
        const bool fresh = anchorsDb_.size() != static_cast<std::size_t>(count);
        if (fresh) anchorsDb_.assign(count, 0.0f);
        std::vector<float> samples;
        samples.reserve(std::min(n, radius * 2 + 1));
        for (int a = 0; a < count; ++a) {
            const int center = std::min(a * stride, n - 1);
            samples.clear();
            for (int b = std::max(0, center - radius); b <= std::min(n - 1, center + radius); ++b) {
                if (std::isfinite(power[b]) && power[b] > 0.0f) samples.push_back(power[b]);
            }
            float estimate = 1e-30f;
            if (!samples.empty()) {
                const std::size_t q = static_cast<std::size_t>(0.20 * (samples.size() - 1));
                std::nth_element(samples.begin(), samples.begin() + q, samples.end());
                estimate = std::max(1e-30f, samples[q] / 0.2231435513f);
            }
            const float db = 10.0f * std::log10(estimate);
            // Fixed 20 Hz analysis: a one-second time constant. This tracks the
            // background, never the per-channel open/close or vote state.
            anchorsDb_[a] = fresh ? db : anchorsDb_[a] + 0.0487705755f * (db - anchorsDb_[a]);
        }
        curve_.resize(n);
        prefix_.assign(n + 1, 0.0);
        for (int b = 0; b < n; ++b) {
            const int a = std::min(b / stride, count - 2);
            const int left = a * stride;
            const int right = std::min((a + 1) * stride, n - 1);
            const float t = right > left ? float(b - left) / float(right - left) : 0.0f;
            curve_[b] = std::pow(10.0f, (anchorsDb_[a] + t * (anchorsDb_[a + 1] - anchorsDb_[a])) / 10.0f);
            prefix_[b + 1] = prefix_[b] + curve_[b];
        }
    }

    float mean(int lo, int hi) const {
        if (curve_.empty()) return 1e-30f;
        lo = std::clamp(lo, 0, static_cast<int>(curve_.size()) - 1);
        hi = std::clamp(hi, lo, static_cast<int>(curve_.size()) - 1);
        return std::max(1e-30f, static_cast<float>((prefix_[hi + 1] - prefix_[lo]) / (hi - lo + 1)));
    }
    const std::vector<float>& curve() const { return curve_; }

private:
    std::vector<float> anchorsDb_;
    std::vector<float> curve_;
    std::vector<double> prefix_;
};

} // namespace channel_bank_detector
