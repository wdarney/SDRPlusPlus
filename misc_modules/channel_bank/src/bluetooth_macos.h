#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace channel_bank_bluetooth {
// Returns an owned read-only descriptor for the current completed recording.
using OpenPlayback = std::function<int(std::string& name)>;
// Returns the optional compact SNR telemetry payload for characteristic 0007.
using SnrTelemetryPayload = std::function<std::vector<uint8_t>()>;
void* start(const std::string& host, int port, OpenPlayback openPlayback = {},
            SnrTelemetryPayload snrTelemetry = {});
void stop(void* handle);
std::string status(void* handle);
}
