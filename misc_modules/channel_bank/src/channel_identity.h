#pragma once

#include <cmath>

namespace channel_bank {

// Automatic detection uses a receiver-independent channel-grid identity so
// centroid motion and SDR retunes do not create duplicate history/block keys.
inline double automaticGridIdentityHz(double gridCenterHz, double channelSpacingHz) {
    if (!std::isfinite(gridCenterHz) || !std::isfinite(channelSpacingHz) || channelSpacingHz <= 0.0) {
        return gridCenterHz;
    }
    return std::round(gridCenterHz / channelSpacingHz) * channelSpacingHz;
}

// Manual and bookmark lanes are identified by their exact requested target.
// Only automatic lanes fall back to their grid identity.
inline double slotIdentityHz(double targetFreqHz, double gridFreqHz) {
    return std::isfinite(targetFreqHz) ? targetFreqHz : gridFreqHz;
}

} // namespace channel_bank
