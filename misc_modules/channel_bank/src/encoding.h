#pragma once
#if defined(__APPLE__) || defined(_WIN32)
#include <string>

namespace encoding {
    // Encode a WAV file to M4A (AAC).
    // macOS: AudioToolbox + AVAssetExportSession (metadata tags, no re-encode).
    // Windows: ffmpeg subprocess via CreateProcess (no console window).
    // If transcript is non-empty, embeds it as lyrics metadata.
    // If avgSnrDb != 0, embeds "SNR: X.X dB avg" as a comment tag.
    // Deletes the source WAV on success.
    // Returns the new .m4a path, or empty string on failure.
    std::string wavToM4A(const std::string& wavPath, const std::string& transcript = {}, float avgSnrDb = 0.0f);

#ifdef _WIN32
    // Set or query the path to ffmpeg.exe. If not set, searches PATH.
    void setFfmpegPath(const std::string& path);
    std::string getFfmpegPath();
#endif
}
#endif
