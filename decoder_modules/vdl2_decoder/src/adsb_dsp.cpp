#include "adsb_dsp.h"
#include <cstdio>
#include <ctime>
#include <algorithm>

// ============================================================================
// CRC-24 table (Mode S polynomial 0xFFF409)
// ============================================================================

uint32_t ADSBChannel::crc_table[256];
bool ADSBChannel::crc_table_ready = false;

void ADSBChannel::buildCRCTable() {
    for (int i = 0; i < 256; i++) {
        uint32_t c = (uint32_t)i << 16;
        for (int j = 0; j < 8; j++) {
            if (c & 0x800000)
                c = (c << 1) ^ ADSB_CRC24_POLY;
            else
                c <<= 1;
        }
        crc_table[i] = c & 0xFFFFFF;
    }
    crc_table_ready = true;
}

// ============================================================================
// CPR NL lookup table (from dump1090)
// ============================================================================

int ADSBChannel::cprNL(double lat) {
    lat = fabs(lat);
    if (lat < 10.47047130) return 59;
    if (lat < 14.82817437) return 58;
    if (lat < 18.18626357) return 57;
    if (lat < 21.02939493) return 56;
    if (lat < 23.54504487) return 55;
    if (lat < 25.82924707) return 54;
    if (lat < 27.93898710) return 53;
    if (lat < 29.91135686) return 52;
    if (lat < 31.77209708) return 51;
    if (lat < 33.53993436) return 50;
    if (lat < 35.22899598) return 49;
    if (lat < 36.85025108) return 48;
    if (lat < 38.41241892) return 47;
    if (lat < 39.92256684) return 46;
    if (lat < 41.38651832) return 45;
    if (lat < 42.80914012) return 44;
    if (lat < 44.19454951) return 43;
    if (lat < 45.54626723) return 42;
    if (lat < 46.86733252) return 41;
    if (lat < 48.16039128) return 40;
    if (lat < 49.42776439) return 39;
    if (lat < 50.67150166) return 38;
    if (lat < 51.89342469) return 37;
    if (lat < 53.09516153) return 36;
    if (lat < 54.27817472) return 35;
    if (lat < 55.44378444) return 34;
    if (lat < 56.59318756) return 33;
    if (lat < 57.72747354) return 32;
    if (lat < 58.84763776) return 31;
    if (lat < 59.95459277) return 30;
    if (lat < 61.04917774) return 29;
    if (lat < 62.13216659) return 28;
    if (lat < 63.20427479) return 27;
    if (lat < 64.26616523) return 26;
    if (lat < 65.31845310) return 25;
    if (lat < 66.36171008) return 24;
    if (lat < 67.39646774) return 23;
    if (lat < 68.42322022) return 22;
    if (lat < 69.44242631) return 21;
    if (lat < 70.45451075) return 20;
    if (lat < 71.45986473) return 19;
    if (lat < 72.45884545) return 18;
    if (lat < 73.45177442) return 17;
    if (lat < 74.43893416) return 16;
    if (lat < 75.42056257) return 15;
    if (lat < 76.39684391) return 14;
    if (lat < 77.36789461) return 13;
    if (lat < 78.33374083) return 12;
    if (lat < 79.29428225) return 11;
    if (lat < 80.24923213) return 10;
    if (lat < 81.19801349) return 9;
    if (lat < 82.13956981) return 8;
    if (lat < 83.07199445) return 7;
    if (lat < 83.99173563) return 6;
    if (lat < 84.89166191) return 5;
    if (lat < 85.75541621) return 4;
    if (lat < 86.53536998) return 3;
    if (lat < 87.00000000) return 2;
    return 1;
}

int ADSBChannel::cprN(int nl, int oddFlag) {
    int n = nl - oddFlag;
    return (n < 1) ? 1 : n;
}

int ADSBChannel::cprMod(int a, int b) {
    int r = a % b;
    return (r < 0) ? r + b : r;
}

// ============================================================================
// Channel lifecycle
// ============================================================================

ADSBChannel::ADSBChannel() {
    if (!crc_table_ready) buildCRCTable();
}

void ADSBChannel::init(uint32_t f) {
    freq = f;
    reset();
}

void ADSBChannel::reset() {
    magBuf.clear();
    aircraft.clear();
    messageCount = syncCount = crcFailCount = 0;
    samplesProcessed = 0;
}

// ============================================================================
// IQ processing — magnitude envelope + sliding preamble detector
// ============================================================================

void ADSBChannel::processIQ(const float* iq, int numSamples) {
    samplesProcessed += numSamples;

    // Convert IQ → magnitude and append to buffer
    size_t prevLen = magBuf.size();
    magBuf.resize(prevLen + numSamples);
    for (int i = 0; i < numSamples; i++) {
        float re = iq[i * 2];
        float im = iq[i * 2 + 1];
        magBuf[prevLen + i] = sqrtf(re * re + im * im);
    }

    // Scan for preambles
    int total = (int)magBuf.size();
    int scanEnd = total - ADSB_FULL_LEN;
    int pos = 0;

    while (pos <= scanEnd) {
        if (detectPreamble(pos)) {
            syncCount++;

            // Try 112-bit (long / DF17) message first
            uint8_t msg[14];
            decodeBits(pos + ADSB_PREAMBLE_SAMPLES, msg, ADSB_LONG_MSG_BITS);
            uint32_t crc = computeCRC24(msg, 14);

            if (crc == 0) {
                parseMessage(msg, 14);
                pos += ADSB_FULL_LEN;
                continue;
            }

            // 1-bit error correction on long message (skip DF field bits 0-4)
            bool corrected = false;
            for (int b = 5; b < ADSB_LONG_MSG_BITS; b++) {
                msg[b / 8] ^= (1 << (7 - (b & 7)));
                if (computeCRC24(msg, 14) == 0) {
                    parseMessage(msg, 14);
                    pos += ADSB_FULL_LEN;
                    corrected = true;
                    break;
                }
                msg[b / 8] ^= (1 << (7 - (b & 7)));  // flip back
            }
            if (corrected) continue;

            // Try 56-bit (short) message
            uint8_t shortMsg[7];
            decodeBits(pos + ADSB_PREAMBLE_SAMPLES, shortMsg, ADSB_SHORT_MSG_BITS);
            crc = computeCRC24(shortMsg, 7);
            if (crc == 0) {
                parseMessage(shortMsg, 7);
                pos += ADSB_PREAMBLE_SAMPLES + ADSB_SHORT_MSG_BITS * 2;
                continue;
            }

            crcFailCount++;
        }
        pos++;
    }

    // Keep unscanned tail as residual for next call
    int keep = total - pos;
    if (keep < 0) keep = 0;
    if (keep > 0 && pos > 0) {
        memmove(magBuf.data(), magBuf.data() + pos, keep * sizeof(float));
    }
    magBuf.resize(keep);

    // Prune aircraft not seen in 5 minutes
    time_t now = time(nullptr);
    for (auto it = aircraft.begin(); it != aircraft.end(); ) {
        if (now - it->second.lastSeen > 300)
            it = aircraft.erase(it);
        else
            ++it;
    }
}

// ============================================================================
// Preamble detection (dump1090 algorithm)
// ============================================================================
//
// ADS-B preamble at 2 Msps (2 samples per microsecond):
//   Time (us):  0  0.5  1  1.5  2  2.5  3  3.5  4  4.5  5  5.5  6  6.5  7  7.5
//   Sample:     0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
//   Level:      H   L   H   L   L   L   L   H   L   H   L   L   L   L   L   L
//
// High samples: 0, 2, 7, 9
// Low samples:  1, 3, 4, 5, 6, 8, 10-15

bool ADSBChannel::detectPreamble(int idx) {
    const float* m = magBuf.data() + idx;

    float high0 = m[0], high1 = m[2], high2 = m[7], high3 = m[9];
    float low0 = m[1], low1 = m[3], low2 = m[4], low3 = m[5], low4 = m[6], low5 = m[8];

    // Each high must exceed its adjacent lows
    if (high0 <= low0 || high0 <= low1) return false;
    if (high1 <= low0 || high1 <= low1) return false;
    if (high2 <= low4 || high2 <= low5) return false;
    if (high3 <= low5) return false;

    // Average highs must be well above average lows
    float highAvg = (high0 + high1 + high2 + high3) * 0.25f;
    float lowAvg = (low0 + low1 + low2 + low3 + low4 + low5) / 6.0f;

    if (highAvg < 2.0f * (lowAvg + 1e-8f)) return false;

    // Post-preamble gap (samples 10-15) should also be low
    float postAvg = (m[10] + m[11] + m[12] + m[13] + m[14] + m[15]) / 6.0f;
    if (highAvg < 1.5f * (postAvg + 1e-8f)) return false;

    return true;
}

// ============================================================================
// PPM bit extraction
// ============================================================================
//
// Each bit occupies 2 samples (1 us at 2 Msps):
//   Bit '1': first sample high, second low   (10)
//   Bit '0': first sample low, second high   (01)

void ADSBChannel::decodeBits(int startIdx, uint8_t* msg, int bits) {
    int bytes = (bits + 7) / 8;
    memset(msg, 0, bytes);

    const float* m = magBuf.data();
    int prevBit = 0;
    for (int i = 0; i < bits; i++) {
        int si = startIdx + i * 2;
        float s0 = m[si], s1 = m[si + 1];
        float delta = fabsf(s0 - s1);
        float avg = (s0 + s1) * 0.5f + 1e-8f;

        int bit;
        if (delta / avg < 0.15f) {
            // Weak transition — inherit previous bit (continuity)
            bit = prevBit;
        } else {
            bit = (s0 > s1) ? 1 : 0;
        }

        if (bit) {
            msg[i / 8] |= (1 << (7 - (i & 7)));  // MSB first
        }
        prevBit = bit;
    }
}

// ============================================================================
// CRC-24
// ============================================================================

uint32_t ADSBChannel::computeCRC24(const uint8_t* msg, int bytes) {
    uint32_t crc = 0;
    for (int i = 0; i < bytes; i++) {
        crc ^= ((uint32_t)msg[i]) << 16;
        crc = ((crc << 8) & 0xFFFFFF) ^ crc_table[(crc >> 16) & 0xFF];
    }
    return crc & 0xFFFFFF;
}

// ============================================================================
// Message parsing
// ============================================================================

void ADSBChannel::parseMessage(const uint8_t* msg, int len) {
    int df = (msg[0] >> 3) & 0x1F;
    uint32_t icao = 0;
    if (len >= 4)
        icao = ((uint32_t)msg[1] << 16) | ((uint32_t)msg[2] << 8) | msg[3];

    if ((df == 17 || df == 18) && len == 14) {
        auto& ac = aircraft[icao];
        ac.icao = icao;
        ac.lastSeen = time(nullptr);
        parseDF17(msg, icao);
    }
    else {
        messageCount++;
        VDL2Message vmsg;
        vmsg.timestamp = (double)time(nullptr);
        vmsg.freq = freq;
        vmsg.snr = 0;
        vmsg.num_fec_corrections = 0;
        vmsg.ppm_error = 0;
        vmsg.is_acars = false;
        vmsg.formatted_text = formatMessage(msg, len, df, icao);
        if (msgCallback) msgCallback(vmsg);
    }
}

void ADSBChannel::parseDF17(const uint8_t* msg, uint32_t icao) {
    auto& ac = aircraft[icao];
    const uint8_t* me = msg + 4;  // ME field starts at byte 4
    int tc = (me[0] >> 3) & 0x1F;

    if (tc >= 1 && tc <= 4)
        decodeCallsign(me, ac);
    else if (tc >= 9 && tc <= 18)
        decodePosition(me, ac);
    else if (tc == 19)
        decodeVelocity(me, ac);

    messageCount++;
    VDL2Message vmsg;
    vmsg.timestamp = (double)time(nullptr);
    vmsg.freq = freq;
    vmsg.snr = 0;
    vmsg.num_fec_corrections = 0;
    vmsg.ppm_error = 0;
    vmsg.is_acars = false;
    vmsg.formatted_text = formatMessage(msg, 14, (msg[0] >> 3) & 0x1F, icao);
    if (msgCallback) msgCallback(vmsg);
}

// ============================================================================
// ADS-B field decoders
// ============================================================================

// ADS-B callsign character set (6-bit encoding)
static const char adsb_charset[] =
    "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";

void ADSBChannel::decodeCallsign(const uint8_t* me, Aircraft& ac) {
    // 8 characters × 6 bits each, packed into ME bytes 1-6
    uint64_t bits = 0;
    for (int i = 1; i <= 6; i++)
        bits = (bits << 8) | me[i];

    for (int i = 0; i < 8; i++) {
        int idx = (bits >> (42 - 6 * i)) & 0x3F;
        ac.callsign[i] = adsb_charset[idx];
    }
    ac.callsign[8] = '\0';

    // Trim trailing spaces
    for (int i = 7; i >= 0 && ac.callsign[i] == ' '; i--)
        ac.callsign[i] = '\0';
}

void ADSBChannel::decodePosition(const uint8_t* me, Aircraft& ac) {
    // Altitude: 12-bit field with Q-bit
    int altBits = ((me[1] & 0xFF) << 4) | ((me[2] >> 4) & 0x0F);
    int qBit = (altBits >> 4) & 1;
    if (qBit) {
        // 25 ft resolution
        int n = ((altBits & 0xFF0) >> 1) | (altBits & 0x0F);
        ac.altitude = n * 25 - 1000;
    }

    // CPR frame
    int oddFlag = (me[2] >> 2) & 1;
    int cprLat = ((me[2] & 3) << 15) | (me[3] << 7) | ((me[4] >> 1) & 0x7F);
    int cprLon = ((me[4] & 1) << 16) | (me[5] << 8) | me[6];

    double now = (double)time(nullptr);
    if (oddFlag) {
        ac.cprOddLat = cprLat;
        ac.cprOddLon = cprLon;
        ac.cprOddTime = now;
        ac.hasOdd = true;
    }
    else {
        ac.cprEvenLat = cprLat;
        ac.cprEvenLon = cprLon;
        ac.cprEvenTime = now;
        ac.hasEven = true;
    }

    // Decode position when both even + odd frames available within 10s
    if (ac.hasEven && ac.hasOdd && fabs(ac.cprEvenTime - ac.cprOddTime) < 10.0)
        decodeCPR(ac);
}

void ADSBChannel::decodeCPR(Aircraft& ac) {
    // CPR global decoding algorithm
    const double dlat0 = 360.0 / 60.0;  // even frame
    const double dlat1 = 360.0 / 59.0;  // odd frame

    double lat0 = ac.cprEvenLat / 131072.0;
    double lat1 = ac.cprOddLat / 131072.0;

    int j = (int)floor(59.0 * lat0 - 60.0 * lat1 + 0.5);
    double rlat0 = dlat0 * (cprMod(j, 60) + lat0);
    double rlat1 = dlat1 * (cprMod(j, 59) + lat1);

    if (rlat0 >= 270.0) rlat0 -= 360.0;
    if (rlat1 >= 270.0) rlat1 -= 360.0;

    // Check latitude zone consistency
    if (cprNL(rlat0) != cprNL(rlat1)) return;

    double rlat;
    int nl;
    if (ac.cprEvenTime >= ac.cprOddTime) {
        rlat = rlat0;
        nl = cprNL(rlat0);
    }
    else {
        rlat = rlat1;
        nl = cprNL(rlat1);
    }

    // Longitude
    double lon0 = ac.cprEvenLon / 131072.0;
    double lon1 = ac.cprOddLon / 131072.0;

    int ni = cprN(nl, 0);
    double dlon = (ni > 0) ? 360.0 / ni : 360.0;
    int m = (int)floor(lon0 * (nl - 1) - lon1 * nl + 0.5);

    double rlon;
    if (ac.cprEvenTime >= ac.cprOddTime) {
        rlon = dlon * (cprMod(m, ni) + lon0);
    }
    else {
        ni = cprN(nl, 1);
        dlon = (ni > 0) ? 360.0 / ni : 360.0;
        rlon = dlon * (cprMod(m, ni) + lon1);
    }

    if (rlon > 180.0) rlon -= 360.0;

    ac.lat = rlat;
    ac.lon = rlon;
    ac.posValid = true;
}

void ADSBChannel::decodeVelocity(const uint8_t* me, Aircraft& ac) {
    int subtype = me[0] & 0x07;
    if (subtype != 1 && subtype != 2) return;

    int ewDir = (me[1] >> 2) & 1;
    int ewVel = ((me[1] & 3) << 8) | me[2];
    int nsDir = (me[3] >> 7) & 1;
    int nsVel = ((me[3] & 0x7F) << 3) | ((me[4] >> 5) & 7);

    if (!ewVel || !nsVel) return;

    ewVel--; nsVel--;
    if (subtype == 2) { ewVel *= 4; nsVel *= 4; }  // supersonic

    float ew = ewDir ? -(float)ewVel : (float)ewVel;
    float ns = nsDir ? -(float)nsVel : (float)nsVel;

    ac.speed = (int)sqrtf(ew * ew + ns * ns);
    ac.heading = (int)(atan2f(ew, ns) * 180.0f / (float)M_PI);
    if (ac.heading < 0) ac.heading += 360;
    ac.velValid = true;

    // Vertical rate
    int vrSign = (me[4] >> 3) & 1;
    int vrVal = ((me[4] & 7) << 6) | ((me[5] >> 2) & 0x3F);
    if (vrVal) {
        ac.vertRate = (vrVal - 1) * 64;
        if (vrSign) ac.vertRate = -ac.vertRate;
    }
}

// ============================================================================
// Message formatting
// ============================================================================

std::string ADSBChannel::formatMessage(const uint8_t* msg, int len, int df, uint32_t icao) {
    char buf[512];
    int pos = 0;

    if (df == 17 || df == 18) {
        int tc = (msg[4] >> 3) & 0x1F;
        auto it = aircraft.find(icao);

        pos += snprintf(buf + pos, sizeof(buf) - pos, "ADS-B | %06X", icao);

        if (it != aircraft.end()) {
            auto& ac = it->second;
            if (ac.callsign[0])
                pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", ac.callsign);

            if (tc >= 1 && tc <= 4) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, " | Ident: %s", ac.callsign);
            }
            else if (tc >= 9 && tc <= 18) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, " | Alt %dft", ac.altitude);
                if (ac.posValid)
                    pos += snprintf(buf + pos, sizeof(buf) - pos, " @ %.4f, %.4f", ac.lat, ac.lon);
            }
            else if (tc == 19 && ac.velValid) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, " | %dkt hdg %d\xC2\xB0", ac.speed, ac.heading);
                if (ac.vertRate)
                    pos += snprintf(buf + pos, sizeof(buf) - pos, " vr %+dft/m", ac.vertRate);
            }
            else {
                pos += snprintf(buf + pos, sizeof(buf) - pos, " | TC%d", tc);
            }
        }
    }
    else if (df == 11) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Mode S | %06X | All-call (DF11)", icao);
    }
    else {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Mode S | DF%d", df);
        if (len >= 4)
            pos += snprintf(buf + pos, sizeof(buf) - pos, " | %06X", icao);
    }

    return std::string(buf, pos);
}
