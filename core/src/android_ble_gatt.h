#pragma once

#ifdef __ANDROID__

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace android_ble_gatt {

using RequestHandler = std::function<std::string(const std::string&)>;

void registerRequestHandler(RequestHandler handler);
void unregisterRequestHandler();
bool registerNativeMethods();

void start();
void stop();

bool hasStateSubscribers();
bool hasAudioSubscribers();
void notifyState(const std::string& json);
void publishAudio(const int16_t* samples, size_t count);

}

#endif
