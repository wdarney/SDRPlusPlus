#pragma once
#ifdef __APPLE__
#include <string>

namespace encoding {
    // Encode a WAV file to M4A (AAC) using AudioToolbox.
    // If transcript is non-empty, embeds it as an iTunes lyrics tag (©lyr).
    // If avgSnrDb != 0, embeds "SNR: X.X dB avg" as an iTunes comment tag (©cmt).
    // Both tags are written via an AVAssetExportSession passthrough — no re-encode, no quality loss.
    // Deletes the source WAV on success.
    // Returns the new .m4a path, or empty string on failure.
    std::string wavToM4A(const std::string& wavPath, const std::string& transcript = {}, float avgSnrDb = 0.0f);
}
#endif
