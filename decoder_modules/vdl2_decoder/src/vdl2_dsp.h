#pragma once
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <mutex>
#include <functional>

// ============================================================================
// VDL2 Constants (from ICAO Annex 10 Vol III / dumpvdl2)
// ============================================================================

#define VDL2_SYMBOL_RATE     10500       // symbols/sec
#define VDL2_SPS             10          // samples per symbol (post-decimation)
#define VDL2_SAMPLE_RATE     (VDL2_SYMBOL_RATE * VDL2_SPS)  // 105000 sps
#define VDL2_BPS             3           // bits per symbol (D8PSK)
#define VDL2_ARITY           8           // 2^BPS

#define VDL2_PREAMBLE_SYMS   16
#define VDL2_SYNC_BUFLEN     (VDL2_PREAMBLE_SYMS * VDL2_SPS)  // 160

#define VDL2_RS_K            249         // RS data bytes per block
#define VDL2_RS_N            255         // RS codeword length
#define VDL2_TRLEN           17          // transmission length field bits
#define VDL2_HDRFECLEN       5           // header FEC syndrome bits
#define VDL2_HEADER_LEN      (3 + VDL2_TRLEN + VDL2_HDRFECLEN)  // 25 bits

#define VDL2_LFSR_IV         0x6959u
#define VDL2_BSLEN           32768u

#define VDL2_SYNC_SKIP       3
#define VDL2_SYNC_THRESHOLD  4.0f
#define VDL2_MAG_LP          0.9f
#define VDL2_NF_LP           0.85f

#define VDL2_MAX_FRAME_LEN           0x3FFF
#define VDL2_MAX_FRAME_LEN_CORRECTED 0x1FFF

#define VDL2_MIN_AVLC_LEN   11
#define VDL2_GOOD_FCS        0xF0B8u

// RS parameters for libcorrect
#define VDL2_RS_POLY         0x187   // x^8 + x^7 + x^2 + x + 1
#define VDL2_RS_FCR          120     // first consecutive root
#define VDL2_RS_PRIM         1       // root spacing

// ============================================================================
// Decoded message structure
// ============================================================================

struct VDL2Message {
    double timestamp;           // time of reception
    uint32_t freq;              // channel frequency
    float snr;                  // signal-to-noise ratio estimate
    int num_fec_corrections;    // RS corrections applied
    float ppm_error;            // frequency error in ppm
    std::string formatted_text; // human-readable decoded message
    std::string json_text;      // JSON format (from libacars)
    bool is_acars;              // true if ACARS, false if other
};

// ============================================================================
// Bitstream
// ============================================================================

class Bitstream {
public:
    Bitstream(uint32_t len = VDL2_BSLEN) : buf(len, 0), start(0), end(0), descrambler_pos(0) {}

    void reset() { start = end = descrambler_pos = 0; }

    void appendMSBFirst(const uint8_t* data, int numbytes, int bps) {
        for (int i = 0; i < numbytes; i++) {
            for (int j = bps - 1; j >= 0; j--) {
                if (end < buf.size()) {
                    buf[end++] = (data[i] >> j) & 1;
                }
            }
        }
    }

    void appendLSBFirst(const uint8_t* data, int numbytes, int bps) {
        for (int i = 0; i < numbytes; i++) {
            for (int j = 0; j < bps; j++) {
                if (end < buf.size()) {
                    buf[end++] = (data[i] >> j) & 1;
                }
            }
        }
    }

    uint32_t readWordMSBFirst(uint32_t numbits) {
        uint32_t val = 0;
        for (uint32_t i = 0; i < numbits && start < end; i++) {
            val = (val << 1) | buf[start++];
        }
        return val;
    }

    void readOctetsLSBFirst(uint8_t* out, uint32_t num_octets) {
        for (uint32_t i = 0; i < num_octets; i++) {
            uint8_t val = 0;
            for (int j = 0; j < 8 && start < end; j++) {
                val |= buf[start++] << j;
            }
            out[i] = val;
        }
    }

    void descramble(uint16_t* lfsr) {
        if (descrambler_pos < start) descrambler_pos = start;
        for (uint32_t i = descrambler_pos; i < end; i++) {
            uint8_t bit = ((*lfsr >> 0) ^ (*lfsr >> 14)) & 1;
            *lfsr = (*lfsr >> 1) | (bit << 14);
            buf[i] ^= bit;
        }
        descrambler_pos = end;
    }

    // HDLC unstuffing — extract next frame from bitstream.
    // Flag 0x7E = 01111110.  Detected when a 0 arrives after 6 ones.
    // After 5 consecutive ones a stuffed 0 is removed.
    // 7+ consecutive ones = abort.
    //
    // All 1-bits are tentatively appended.  When the trailing 0 of a
    // flag arrives we rewind by 6 (the leading-0 + 5 ones that were
    // appended as if they were data).
    std::vector<uint8_t> copyNextFrame() {
        // Accumulate unstuffed bits, then convert to bytes at frame boundary
        std::vector<uint8_t> frameBits;
        int ones = 0;
        bool gotOpenFlag = false;

        while (start < end) {
            uint8_t bit = buf[start++];

            if (bit == 1) {
                ones++;
                if (ones <= 5 && gotOpenFlag) {
                    frameBits.push_back(1);
                }
                // ones == 6 or 7+: don't append yet — pending flag / abort
                continue;
            }

            // bit == 0
            if (ones >= 7) {
                // Abort — 7+ ones followed by 0
                frameBits.clear();
                gotOpenFlag = false;
                ones = 0;
                continue;
            }

            if (ones == 6) {
                // Flag detected (0 after 6 ones = trailing 0 of 01111110)
                if (gotOpenFlag && !frameBits.empty()) {
                    // Remove the 6 flag-prefix bits that were appended as
                    // data: the leading 0 + the first 5 ones of the flag.
                    // (The 6th one was held back; the trailing 0 is this bit.)
                    if (frameBits.size() >= 6) {
                        frameBits.resize(frameBits.size() - 6);
                    }

                    // Convert bits → bytes (LSB-first) and return frame
                    size_t nBytes = frameBits.size() / 8;
                    if (nBytes >= VDL2_MIN_AVLC_LEN) {
                        std::vector<uint8_t> frame(nBytes);
                        for (size_t i = 0; i < nBytes; i++) {
                            uint8_t byte = 0;
                            for (int j = 0; j < 8; j++) {
                                byte |= frameBits[i * 8 + j] << j;
                            }
                            frame[i] = byte;
                        }
                        ones = 0;
                        // This flag is also the opening flag for the next frame
                        gotOpenFlag = true;
                        frameBits.clear();
                        return frame;
                    }
                }
                // This flag becomes the opening flag
                gotOpenFlag = true;
                frameBits.clear();
                ones = 0;
                continue;
            }

            if (ones == 5) {
                // Stuffed bit — discard the 0, keep the 5 ones already appended
                ones = 0;
                continue;
            }

            // Normal 0 data bit (ones was 0-4)
            ones = 0;
            if (gotOpenFlag) {
                frameBits.push_back(0);
            }
        }

        return {};  // No complete frame found
    }

    std::vector<uint8_t> buf;
    uint32_t start, end, descrambler_pos;
};

// ============================================================================
// Chebyshev 2-pole LPF
// ============================================================================

class ChebyshevLPF {
public:
    void init(float cutoff_freq, float ripple, int npoles);

    // Process one sample (call for I and Q separately)
    float process(float* in_hist, float* out_hist);

    float A[5] = {};
    float B[5] = {};

private:
    void calcPole(int p, float cutoff, float ripple, int npoles, float* AA, float* BB);
};

// ============================================================================
// VDL2 Channel Demodulator + Decoder
// ============================================================================

enum class DemodState { INIT, SYNC };
enum class DecoderState { HEADER, DATA, IDLE };

class VDL2Channel {
public:
    VDL2Channel();
    ~VDL2Channel();

    void init(uint32_t freq, uint32_t sample_rate);
    void reset();

    // Process a block of complex IQ samples (interleaved float I,Q pairs)
    void processIQ(const float* iq, int num_samples);

    // Set callback for decoded messages
    void setMessageCallback(std::function<void(const VDL2Message&)> cb) { msgCallback = cb; }

    // Stats
    float getNoiseFloor() const { return mag_nf; }
    float getSNR() const;
    int getMessageCount() const { return messageCount; }
    int getSyncCount() const { return syncCount; }
    int getHeaderOkCount() const { return headerOkCount; }
    int getHeaderFailCount() const { return headerFailCount; }
    int getRsFailCount() const { return rsFailCount; }
    int getCrcFailCount() const { return crcFailCount; }
    long long getSamplesProcessed() const { return samplesProcessed; }

private:
    // Demodulator
    void demod(float re, float im);
    bool gotSync();

    // Burst decoder
    void decodeBurst();
    uint32_t decodeHeader(uint32_t* r);
    void decodeData();

    // AVLC
    void parseAVLC(const uint8_t* data, int len, float snr);

    // Helpers
    static uint32_t reverse(uint32_t v, int numbits);
    static int getFecOctetCount(uint32_t len);
    static uint32_t parity(uint32_t v);

    // RS decoder (using libcorrect)
    void* rs_decoder = nullptr;
    int rsVerify(uint8_t* data, int fec_octets);

    // CRC
    static uint16_t crc16_ccitt(const uint8_t* data, uint32_t len, uint16_t crc_init);

    // Filter
    ChebyshevLPF lpf;
    float re_in[3] = {}, re_out[3] = {};
    float im_in[3] = {}, im_out[3] = {};

    // Demod state
    DemodState demod_state = DemodState::INIT;
    DecoderState decoder_state = DecoderState::IDLE;

    float syncbuf[VDL2_SYNC_BUFLEN] = {};
    int syncbufidx = 0;
    float prev_phi = 0;
    float prev_dphi = 0, dphi = 0;
    float pherr[3] = { 1000.f, 1000.f, 1000.f };
    float ppm_error = 0;
    float mag_lp = 0;
    float mag_nf = 0;
    float frame_pwr = 0;
    int frame_pwr_cnt = 0;
    int nfcnt = 0;
    int sclk = 0;

    // Burst decoder state
    Bitstream bs;
    Bitstream frame_bs;
    uint16_t lfsr = VDL2_LFSR_IV;
    uint32_t requested_bits = VDL2_HEADER_LEN;
    uint32_t datalen = 0, datalen_octets = 0;
    uint32_t last_block_len_octets = 0, fec_octets = 0;
    uint32_t num_blocks = 0;
    uint32_t syndrome = 0;
    int num_fec_corrections = 0;

    // Channel info
    uint32_t freq = 0;
    uint32_t sample_rate = VDL2_SAMPLE_RATE;

    // Linear regression precomputed values for sync
    float lr_X[VDL2_PREAMBLE_SYMS] = {};
    float lr_denom = 0;

    // Message callback
    std::function<void(const VDL2Message&)> msgCallback;
    int messageCount = 0;

    // Debug counters
    int syncCount = 0;
    int headerOkCount = 0;
    int headerFailCount = 0;
    int rsFailCount = 0;
    int crcFailCount = 0;
    long long samplesProcessed = 0;

    // Preamble phase pattern
    static const float pr_phase[VDL2_PREAMBLE_SYMS];

    // Gray code
    static const uint8_t graycode[VDL2_ARITY];

    // Header FEC tables
    static const uint32_t H[VDL2_HDRFECLEN];
    static const uint32_t syndtable[1 << VDL2_HDRFECLEN];
    static const uint32_t synd_weight[1 << VDL2_HDRFECLEN];

    // CRC table
    static const uint16_t crctable[256];
};
