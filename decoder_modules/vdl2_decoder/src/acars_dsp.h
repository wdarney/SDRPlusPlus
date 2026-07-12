#pragma once
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <functional>

// Reuse the message struct from vdl2_dsp.h
#include "vdl2_dsp.h"

// ============================================================================
// ACARS Constants (from acarsdec / ARINC 618)
// ============================================================================

#define ACARS_INTRATE       12000       // Internal processing sample rate
#define ACARS_BAUD          2400        // Baud rate
#define ACARS_BITLEN        10          // Samples per bit at 12 kHz (12000/1200)
#define ACARS_MFLTOVER      240         // Matched filter oversampling
#define ACARS_MFLTLEN       2401        // BITLEN * MFLTOVER + 1

#define ACARS_FREQ_MARK     2400.f      // Mark tone (Hz)
#define ACARS_FREQ_SPACE    1200.f      // Space tone (Hz)
#define ACARS_FREQ_CENTER   1800.f      // Center frequency (Hz)

#define ACARS_PLL_Ki        (71e-7f / ACARS_BITLEN)
#define ACARS_PLL_Kp        (60e-3f / ACARS_BITLEN)

// ACARS special characters
#define ACARS_SYN           0x16
#define ACARS_SOH           0x01
#define ACARS_STX           0x02
#define ACARS_ETX           0x83        // 0x03 | 0x80 (with parity)
#define ACARS_ETB           0x97        // 0x17 | 0x80 (with parity)
#define ACARS_DEL           0x7F
#define ACARS_PLUS_PARITY   0xAB        // '+' | 0x80

#define ACARS_PREKEY_MIN    12          // Minimum preamble 0xFF bytes
#define ACARS_TXTMAXLEN     240         // Max message body bytes
#define ACARS_TXTMINLEN     13          // Min message body (mode+addr7+ack+label2+bid+sot)

// ============================================================================
// ACARS Channel Demodulator + Decoder
// ============================================================================

enum class AcarsState {
    PREKEY,     // Hunting for preamble
    SYNC,       // Verifying sync sequence
    SOH1,       // Waiting for SOH
    TXT,        // Collecting message text
    CRC1,       // Reading CRC byte 1
    CRC2,       // Reading CRC byte 2
    ENDX        // Expecting DEL
};

class ACARSChannel {
public:
    ACARSChannel();
    ~ACARSChannel() = default;

    void init(uint32_t freq);
    void reset();

    // Process complex IQ samples (interleaved float I,Q pairs at 12 kHz)
    void processIQ(const float* iq, int num_samples);

    void setMessageCallback(std::function<void(const VDL2Message&)> cb) { msgCallback = cb; }

    int getMessageCount() const { return messageCount; }
    float getSNR() const;
    int getSyncCount() const { return syncCount; }
    long long getSamplesProcessed() const { return samplesProcessed; }

private:
    // MSK demodulator
    void demodMSK(float sample);
    void putBit(float v);
    void decodeAcars(uint8_t byte);

    // Message output
    void buildMessage();

    // CRC
    static uint16_t updateCRC16(uint16_t crc, uint8_t c);

    // Parity check (returns true if odd parity OK)
    static bool parityOK(uint8_t c);

    // Matched filter table (half-sine)
    float mfilt[ACARS_MFLTLEN];

    // VCO / PLL state
    float vcoPhase = 0;         // VCO phase accumulator
    float mskDf = 0;            // PLL integrator (frequency correction)
    float mskDphi = 0;          // PLL total correction (prop + integral)
    float mskClk = 0;           // Bit clock accumulator

    // Circular buffer for matched filter
    float inbuf_re[ACARS_BITLEN] = {};
    float inbuf_im[ACARS_BITLEN] = {};
    int bufIdx = 0;

    // Bit counter / phase
    int mskS = 0;               // MSK phase counter (0-3 cycle)

    // Signal level tracking
    float mskMag = 0;           // Magnitude moving average
    float mskPwr = 0;           // Power moving average
    float mskNF = 1e-6f;        // Noise floor

    // Bit assembly
    uint8_t outbits = 0;
    int nbits = 8;

    // ACARS state machine
    AcarsState state = AcarsState::PREKEY;
    int preCount = 0;           // Preamble 0xFF counter
    int syncIdx = 0;            // Position in sync sequence
    std::vector<uint8_t> msgBuf;
    uint16_t crc = 0;
    uint8_t crcBytes[2] = {};
    int txtLen = 0;

    // Channel info
    uint32_t freq = 0;

    // Callback
    std::function<void(const VDL2Message&)> msgCallback;
    int messageCount = 0;
    int syncCount = 0;
    long long samplesProcessed = 0;

    // CRC table
    static const uint16_t crc_ccitt_table[256];
};
