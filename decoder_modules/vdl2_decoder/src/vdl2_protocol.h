#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// One owner per receiver channel; reset on retune/restart. Not shared across DSP workers.
class VDL2ProtocolDecoder {
public:
    struct Result { std::string text; std::string json; };
    VDL2ProtocolDecoder();
    ~VDL2ProtocolDecoder();
    VDL2ProtocolDecoder(const VDL2ProtocolDecoder&) = delete;
    VDL2ProtocolDecoder& operator=(const VDL2ProtocolDecoder&) = delete;
    void reset();
    Result decode(const uint8_t* data, size_t length, uint32_t src, uint32_t dst,
                  bool fromGround, double timestamp, bool acars);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
