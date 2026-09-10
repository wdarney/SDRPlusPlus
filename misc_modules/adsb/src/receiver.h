#pragma once
#include "http_server.h"
#include "history.h"
#include <rtl-sdr.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>
#include <json.hpp>

namespace adsb {
struct Device { std::string serial, label; unsigned index; bool unique; };
struct Settings { std::string serial; int gain=400, ppm=0, historyHours=6; double lat=0, lon=0; bool location=false, agc=false; };
std::vector<Device> devices();
uint64_t nowMillis();
class Receiver {
public:
    ~Receiver() { stop(); }
    void start(const Settings& settings);
    void stop();
    bool running() const { return active_; }
    std::string status();
    unsigned aircraftCount() const { return aircraftCount_; }
    uint64_t dropped() const { return dropped_; }
    uint64_t messages() const { return messages_; }
    Response data(const std::string& path);
private:
    struct Block { std::vector<uint8_t> iq; uint64_t now; bool gap; };
    static void callback(unsigned char* data, uint32_t length, void* context);
    void capture();
    void decode();
    void publish();
    void setStatus(std::string value);
    Settings settings_;
    std::atomic<bool> stop_{true}, active_{false}, readerDone_{true};
    std::atomic<uint64_t> dropped_{0}, messages_{0};
    std::atomic<unsigned> aircraftCount_{0};
    std::mutex deviceMutex_, queueMutex_, stateMutex_;
    rtlsdr_dev_t* device_=nullptr;
    std::condition_variable ready_;
    std::deque<Block> queue_;
    bool gap_=false;
    std::thread reader_, decoder_;
    std::string status_="Stopped", aircraft_;
    SessionHistory history_;
};
}
