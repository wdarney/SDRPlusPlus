#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace channel_bank_multi_receiver {

enum class DispatchState { Pending, Active, Suppressed };

struct ReceiverState {
    std::string id;
    double sampleRate = 0.0;
    double centerHz = 0.0;
    bool available = true;
    std::vector<int64_t> channels;

    bool idle() const { return channels.empty(); }
};

struct Allocation {
    std::string receiverId;
    double centerHz = 0.0;
    bool reusedCoverage = false;
};

// Pure deterministic placement/state logic. Physical source operations live in
// Channel Bank; this class only answers where a normalized logical channel
// belongs and tracks PENDING/ACTIVE/SUPPRESSED identity.
class ReceiverAllocator {
public:
    void setReceivers(std::vector<ReceiverState> ordered) { receivers_ = std::move(ordered); }
    const std::vector<ReceiverState>& receivers() const { return receivers_; }

    bool markPending(int64_t key) {
        if (dispatch_.count(key)) return false;
        dispatch_[key] = DispatchState::Pending;
        return true;
    }

    // Discovery NMS sees only the current FFT frame. Retain that exclusion
    // across later frames, after a channel has entered dispatch. Keys are
    // rounded to kHz, so allow 1 kHz for two rounding errors.
    bool markPendingDistinct(int64_t key, double spacingHz, int radiusSlots) {
        if (dispatch_.count(key)) return false;
        const double radiusHz = std::max(1, radiusSlots) * spacingHz + 1000.0;
        for (const auto& [existingKey, state] : dispatch_) {
            if (state == DispatchState::Suppressed) continue;
            if (std::abs((double)(key - existingKey) * 1000.0) <= radiusHz) return false;
        }
        dispatch_[key] = DispatchState::Pending;
        return true;
    }

    bool activate(int64_t key, const Allocation& allocation) {
        auto state = dispatch_.find(key);
        if (state == dispatch_.end() || state->second != DispatchState::Pending) return false;
        auto receiver = findReceiver(allocation.receiverId);
        if (!receiver || !receiver->available) return false;
        receiver->centerHz = allocation.centerHz;
        if (std::find(receiver->channels.begin(), receiver->channels.end(), key) == receiver->channels.end())
            receiver->channels.push_back(key);
        assignments_[key] = allocation.receiverId;
        state->second = DispatchState::Active;
        return true;
    }

    void failPending(int64_t key) {
        auto it = dispatch_.find(key);
        if (it != dispatch_.end() && it->second == DispatchState::Pending) dispatch_.erase(it);
    }

    void release(int64_t key, bool suppress) {
        auto assigned = assignments_.find(key);
        if (assigned != assignments_.end()) {
            if (auto* receiver = findReceiver(assigned->second)) {
                receiver->channels.erase(std::remove(receiver->channels.begin(), receiver->channels.end(), key),
                                         receiver->channels.end());
                if (receiver->channels.empty()) receiver->centerHz = 0.0;
            }
            assignments_.erase(assigned);
        }
        if (suppress) dispatch_[key] = DispatchState::Suppressed;
        else dispatch_.erase(key);
    }

    bool clearSuppressed(int64_t key) {
        auto it = dispatch_.find(key);
        if (it == dispatch_.end() || it->second != DispatchState::Suppressed) return false;
        dispatch_.erase(it);
        return true;
    }

    void setAvailable(const std::string& id, bool available) {
        if (auto* receiver = findReceiver(id)) receiver->available = available;
    }

    std::vector<int64_t> disconnect(const std::string& id) {
        std::vector<int64_t> released;
        auto* receiver = findReceiver(id);
        if (!receiver) return released;
        receiver->available = false;
        released = receiver->channels;
        for (int64_t key : released) {
            assignments_.erase(key);
            dispatch_.erase(key);
        }
        receiver->channels.clear();
        receiver->centerHz = 0.0;
        return released;
    }

    std::optional<Allocation> choose(double frequencyHz, double channelBandwidthHz,
                                     double usableFraction = 0.90) const {
        if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0) return std::nullopt;
        for (const auto& receiver : receivers_) {
            if (!receiver.available || receiver.idle() || receiver.sampleRate <= 0.0) continue;
            double halfUsable = receiver.sampleRate * usableFraction * 0.5 - channelBandwidthHz * 0.5;
            if (halfUsable >= 0.0 && std::abs(frequencyHz - receiver.centerHz) <= halfUsable)
                return Allocation{receiver.id, receiver.centerHz, true};
        }
        for (const auto& receiver : receivers_) {
            if (receiver.available && receiver.idle() && receiver.sampleRate > 0.0)
                return Allocation{receiver.id, frequencyHz, false};
        }
        return std::nullopt;
    }

    std::optional<DispatchState> state(int64_t key) const {
        auto it = dispatch_.find(key);
        if (it == dispatch_.end()) return std::nullopt;
        return it->second;
    }

    std::string assignment(int64_t key) const {
        auto it = assignments_.find(key);
        return it == assignments_.end() ? std::string() : it->second;
    }

    std::vector<std::pair<int64_t, DispatchState>> dispatches() const {
        return {dispatch_.begin(), dispatch_.end()};
    }

    void clear() {
        dispatch_.clear();
        assignments_.clear();
        for (auto& receiver : receivers_) {
            receiver.channels.clear();
            receiver.centerHz = 0.0;
        }
    }

private:
    ReceiverState* findReceiver(const std::string& id) {
        auto it = std::find_if(receivers_.begin(), receivers_.end(), [&](const auto& r) { return r.id == id; });
        return it == receivers_.end() ? nullptr : &*it;
    }
    const ReceiverState* findReceiver(const std::string& id) const {
        auto it = std::find_if(receivers_.begin(), receivers_.end(), [&](const auto& r) { return r.id == id; });
        return it == receivers_.end() ? nullptr : &*it;
    }

    std::vector<ReceiverState> receivers_;
    std::map<int64_t, DispatchState> dispatch_;
    std::map<int64_t, std::string> assignments_;
};

} // namespace channel_bank_multi_receiver
