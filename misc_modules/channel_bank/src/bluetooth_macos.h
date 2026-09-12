#pragma once
#include <string>

namespace channel_bank_bluetooth {
void* start(const std::string& host, int port);
void stop(void* handle);
std::string status(void* handle);
}
