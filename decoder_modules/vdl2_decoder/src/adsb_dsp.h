#pragma once
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <functional>
#include <ctime>

#include "vdl2_dsp.h"  // VDL2Message struct

// ============================================================================
// ADS-B / Mode S Constants
// ============================================================================

#define ADSB_RATE               2000000     // 2 Msps processing rate
#define ADSB_PREAMBLE_SAMPLES   16          // 8 us at 2 Msps
#define ADSB_LONG_MSG_BITS      112         // Extended Squitter (DF17/18)
#define ADSB_SHORT_MSG_BITS     56          // Short messages
#define ADSB_LONG_MSG_SAMPLES   (ADSB_LONG_MSG_BITS * 2)   // 224 (PPM = 2 samples/bit)
#define ADSB_FULL_LEN           (ADSB_PREAMBLE_SAMPLES + ADSB_LONG_MSG_SAMPLES)  // 240
#define ADSB_CRC24_POLY         0xFFF409

// ============================================================================
// ADS-B Channel Demodulator + Decoder
// ============================================================================

class ADSBChannel {
public:
    ADSBChannel();
    ~ADSBChannel() = default;

    void init(uint32_t freq);
    void reset();

    // Process complex IQ samples (interleaved float I,Q pairs at 2 Msps)
    void processIQ(const float* iq, int numSamples);

    void setMessageCallback(std::function<void(const VDL2Message&)> cb) { msgCallback = cb; }

    int getMessageCount() const { return messageCount; }
    int getSyncCount() const { return syncCount; }
    int getCrcFailCount() const { return crcFailCount; }
    long long getSamplesProcessed() const { return samplesProcessed; }
    int getAircraftCount() const { return (int)aircraft.size(); }

private:
    // Aircraft state for CPR position decoding + display
    struct Aircraft {
        uint32_t icao = 0;
        char callsign[9] = {};
        double lat = 0, lon = 0;
        bool posValid = false;
        int altitude = 0;
        int speed = 0;          // knots
        int heading = 0;        // degrees
        int vertRate = 0;       // ft/min
        bool velValid = false;
        // CPR (Compact Position Reporting) state
        int cprEvenLat = 0, cprEvenLon = 0;
        int cprOddLat = 0, cprOddLon = 0;
        double cprEvenTime = 0, cprOddTime = 0;
        bool hasEven = false, hasOdd = false;
        time_t lastSeen = 0;
    };

    std::map<uint32_t, Aircraft> aircraft;

    // Magnitude buffer for envelope detection
    std::vector<float> magBuf;

    // Detection & decoding
    bool detectPreamble(int idx);
    void decodeBits(int startIdx, uint8_t* msg, int bits);
    uint32_t computeCRC24(const uint8_t* msg, int bytes);
    void parseMessage(const uint8_t* msg, int len);
    void parseDF17(const uint8_t* msg, uint32_t icao);
    void decodeCallsign(const uint8_t* me, Aircraft& ac);
    void decodePosition(const uint8_t* me, Aircraft& ac);
    void decodeVelocity(const uint8_t* me, Aircraft& ac);
    void decodeCPR(Aircraft& ac);
    std::string formatMessage(const uint8_t* msg, int len, int df, uint32_t icao);

    // CPR helpers
    static int cprNL(double lat);
    static int cprN(int nl, int oddFlag);
    static int cprMod(int a, int b);

    // Callback & config
    std::function<void(const VDL2Message&)> msgCallback;
    uint32_t freq = 0;

    // Stats
    int messageCount = 0;
    int syncCount = 0;
    int crcFailCount = 0;
    long long samplesProcessed = 0;

    // CRC-24 lookup table
    static uint32_t crc_table[256];
    static bool crc_table_ready;
    static void buildCRCTable();
};
