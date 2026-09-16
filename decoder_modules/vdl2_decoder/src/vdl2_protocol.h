#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// One owner per receiver channel; reset on retune/restart. Not shared across DSP workers.
class VDL2ProtocolDecoder {
public:
    struct Result {
        std::string text, json, protocol = "VDL2", transport, tail, flight;
        bool acars = false, fansCpdlc = false;
        bool x25 = false, clnp = false, cotp = false;
        bool atnCpdlc = false, atnAdsc = false, atnCm = false;
    };
    struct Counters {
        uint64_t acars, fansCpdlc, atnX25, atnClnp, atnCpdlc, atnAdsc, atnCm;
    };
    // Thread-safe lifetime counters; reset() clears only reassembly state.
    Counters counters() const;
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
