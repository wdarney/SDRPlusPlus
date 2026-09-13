#pragma once
#include <string>
#include <functional>

namespace channel_bank_bluetooth {
// Returns an owned read-only descriptor for the current completed recording.
using OpenPlayback = std::function<int(std::string& name)>;
void* start(const std::string& host, int port, OpenPlayback openPlayback = {});
void stop(void* handle);
std::string status(void* handle);
}
