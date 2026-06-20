#include "acars_dsp.h"

extern "C" {
#include <libacars/libacars.h>
#include <libacars/acars.h>
#include <libacars/vstring.h>
}

#include <cstdio>
#include <ctime>

// ============================================================================
// CRC-16-CCITT table (reflected / LSb-first, same as KERMIT)
// ============================================================================

const uint16_t ACARSChannel::crc_ccitt_table[256] = {
    0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf,
    0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
    0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e,
    0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
    0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd,
    0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
    0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c,
    0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
    0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb,
    0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
    0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a,
    0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
    0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9,
    0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
    0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738,
    0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
    0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7,
    0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
    0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036,
    0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
    0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5,
    0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
    0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134,
    0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
    0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3,
    0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
    0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232,
    0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
    0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1,
    0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
    0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330,
    0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78,
};

// ============================================================================
// ACARSChannel implementation
// ============================================================================

ACARSChannel::ACARSChannel() {
    // Precompute half-sine matched filter
    for (int i = 0; i < ACARS_MFLTLEN; i++) {
        mfilt[i] = sinf((float)M_PI * ACARS_FREQ_SPACE * (float)i
                        / (float)ACARS_INTRATE / (float)ACARS_MFLTOVER);
    }
}

void ACARSChannel::init(uint32_t _freq) {
    freq = _freq;
    reset();
}

void ACARSChannel::reset() {
    vcoPhase = 0;
    mskDf = 0;
    mskDphi = 0;
    mskClk = 0;
    memset(inbuf_re, 0, sizeof(inbuf_re));
    memset(inbuf_im, 0, sizeof(inbuf_im));
    bufIdx = 0;
    mskS = 0;
    mskMag = 0;
    mskPwr = 0;
    mskNF = 1e-6f;
    outbits = 0;
    nbits = 8;
    state = AcarsState::PREKEY;
    preCount = 0;
    syncIdx = 0;
    msgBuf.clear();
    crc = 0;
    txtLen = 0;
}

void ACARSChannel::processIQ(const float* iq, int num_samples) {
    samplesProcessed += num_samples;
    for (int i = 0; i < num_samples; i++) {
        // AM envelope detection: magnitude of complex sample
        float re = iq[i * 2];
        float im = iq[i * 2 + 1];
        float envelope = sqrtf(re * re + im * im);

        // Feed to MSK demodulator
        demodMSK(envelope);
    }
}

// ============================================================================
// MSK Demodulator
// ============================================================================

void ACARSChannel::demodMSK(float sample) {
    // VCO frequency: center freq + PLL correction
    float s = 2.f * (float)M_PI * ACARS_FREQ_CENTER / (float)ACARS_INTRATE + mskDphi;

    // Advance bit clock
    mskClk += s;

    // Mix real AM envelope with complex VCO to get complex baseband
    float cs = cosf(vcoPhase);
    float sn = sinf(vcoPhase);
    inbuf_re[bufIdx] = sample * cs;
    inbuf_im[bufIdx] = -sample * sn;
    bufIdx = (bufIdx + 1) % ACARS_BITLEN;

    // Advance VCO phase
    vcoPhase += s;
    if (vcoPhase >= 2.f * (float)M_PI) vcoPhase -= 2.f * (float)M_PI;

    // Bit decision at optimal sampling point
    if (mskClk < 3.f * (float)M_PI / 2.f) return;
    mskClk -= 3.f * (float)M_PI / 2.f;

    // Apply matched filter with sub-sample interpolation
    float o = (float)ACARS_MFLTOVER * (mskClk / s);
    if (o > (float)ACARS_MFLTOVER) o = (float)ACARS_MFLTOVER;

    float v_re = 0, v_im = 0;
    for (int j = 0; j < ACARS_BITLEN; j++) {
        int fi = (int)o;
        if (fi >= ACARS_MFLTLEN) fi = ACARS_MFLTLEN - 1;
        float h = mfilt[fi];
        int bi = (j + bufIdx) % ACARS_BITLEN;
        v_re += h * inbuf_re[bi];
        v_im += h * inbuf_im[bi];
        o += (float)ACARS_MFLTOVER;
    }

    // Normalize
    float lvl = sqrtf(v_re * v_re + v_im * v_im) + 1e-8f;
    v_re /= lvl;
    v_im /= lvl;

    // Update magnitude tracking
    mskMag += (lvl - mskMag) / 8.f;

    // I/Q alternating MSK decision
    float vo, dphi;
    if (mskS & 1) {
        // Q phase
        vo = v_im;
        dphi = (vo >= 0) ? -v_re : v_re;
    } else {
        // I phase
        vo = v_re;
        dphi = (vo >= 0) ? v_im : -v_im;
    }

    // Polarity correction (MSK sign pattern cycles with period 4)
    float bitVal = (mskS & 2) ? -vo : vo;

    // PLL update (only when tracking a signal)
    if (state != AcarsState::PREKEY || preCount > 0) {
        mskDf += ACARS_PLL_Ki * dphi;
        mskDphi = mskDf + ACARS_PLL_Kp * dphi;
    } else {
        mskDf = 0;
        mskDphi = 0;
    }

    mskS++;

    // Output bit
    putBit(bitVal);
}

// ============================================================================
// Bit assembly
// ============================================================================

void ACARSChannel::putBit(float v) {
    // LSb first: shift right, put new bit in MSb
    outbits >>= 1;
    if (v > 0) outbits |= 0x80;

    nbits--;
    if (nbits > 0) return;

    // Byte complete
    nbits = 8;
    decodeAcars(outbits);
}

// ============================================================================
// ACARS state machine
// ============================================================================

bool ACARSChannel::parityOK(uint8_t c) {
    // Odd parity check: popcount should be odd
    uint8_t p = c;
    p ^= p >> 4;
    p ^= p >> 2;
    p ^= p >> 1;
    return (p & 1) == 1;
}

uint16_t ACARSChannel::updateCRC16(uint16_t crc, uint8_t c) {
    return (crc >> 8) ^ crc_ccitt_table[(crc ^ c) & 0xFF];
}

void ACARSChannel::decodeAcars(uint8_t byte) {
    switch (state) {
    case AcarsState::PREKEY: {
        if (byte == 0xFF) {
            preCount++;
        } else if (byte == 0x00) {
            // Inverted polarity — flip MSK phase
            preCount--;
            if (preCount <= -10) {
                mskS ^= 2;     // Flip polarity
                preCount = 0;
            }
        } else {
            if (preCount >= ACARS_PREKEY_MIN) {
                // End of preamble — find bit alignment
                // The first non-FF byte should contain the start of '+' (0xAB)
                // Find position of first 0-bit
                int firstZero = 0;
                for (int i = 0; i < 8; i++) {
                    if (((byte >> i) & 1) == 0) {
                        firstZero = i + 1;
                        break;
                    }
                }

                if (firstZero <= 2) {
                    // Need to realign: read extra bits
                    nbits = 6 | (byte & 0x01);
                } else if (firstZero > 3) {
                    nbits = firstZero - 3;
                }
                // else firstZero == 3: perfect alignment

                state = AcarsState::SYNC;
                syncIdx = 0;
                syncCount++;
            } else {
                // Update noise floor during idle
                float mag2 = mskMag * mskMag;
                mskNF += (mag2 - mskNF) * 1e-4f;
            }
            preCount = 0;
        }
        break;
    }

    case AcarsState::SYNC: {
        // Expected sync sequence: 0xAB(+), 0x2A(*), 0x16(SYN), 0x16(SYN)
        static const uint8_t syncSeq[] = { 0xAB, 0x2A, 0x16, 0x16 };
        if (byte == syncSeq[syncIdx]) {
            syncIdx++;
            if (syncIdx >= 4) {
                state = AcarsState::SOH1;
            }
        } else {
            // Sync failed
            state = AcarsState::PREKEY;
            preCount = 0;
        }
        break;
    }

    case AcarsState::SOH1: {
        if (byte == ACARS_SOH) {
            state = AcarsState::TXT;
            msgBuf.clear();
            crc = 0;
            txtLen = 0;
            // Update power estimate
            mskPwr += (mskMag * mskMag - mskPwr) / 16.f;
        } else {
            state = AcarsState::PREKEY;
            preCount = 0;
        }
        break;
    }

    case AcarsState::TXT: {
        msgBuf.push_back(byte);
        crc = updateCRC16(crc, byte);
        txtLen++;

        // Check for end-of-text markers
        if (byte == ACARS_ETX || byte == ACARS_ETB) {
            state = AcarsState::CRC1;
        } else if (txtLen >= ACARS_TXTMAXLEN) {
            // Too long — abort
            state = AcarsState::PREKEY;
            preCount = 0;
        }
        break;
    }

    case AcarsState::CRC1: {
        crcBytes[0] = byte;
        crc = updateCRC16(crc, byte);
        state = AcarsState::CRC2;
        break;
    }

    case AcarsState::CRC2: {
        crcBytes[1] = byte;
        crc = updateCRC16(crc, byte);
        state = AcarsState::ENDX;
        break;
    }

    case AcarsState::ENDX: {
        // Expect DEL (0x7F)
        // Process the message regardless (some transmitters omit DEL)

        if (txtLen >= ACARS_TXTMINLEN && crc == 0) {
            // CRC OK — build message
            buildMessage();
        }

        state = AcarsState::PREKEY;
        preCount = 0;
        break;
    }
    }
}

// ============================================================================
// Build decoded message
// ============================================================================

void ACARSChannel::buildMessage() {
    if (msgBuf.size() < ACARS_TXTMINLEN) return;

    VDL2Message msg;
    msg.freq = freq;
    msg.is_acars = true;
    msg.num_fec_corrections = 0;
    msg.ppm_error = 0;

    // SNR estimate
    if (mskNF > 0 && mskPwr > 0) {
        msg.snr = 10.f * log10f(mskPwr / mskNF);
    } else {
        msg.snr = 0;
    }

    // Timestamp
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    msg.timestamp = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;

    // Strip parity bits from message bytes
    std::vector<uint8_t> clean(msgBuf.size());
    int parityErrors = 0;
    for (size_t i = 0; i < msgBuf.size(); i++) {
        if (!parityOK(msgBuf[i])) parityErrors++;
        clean[i] = msgBuf[i] & 0x7F;
    }

    // Parse fields: mode(1) addr(7) ack(1) label(2) bid(1) sot(1) [text...] suffix(1)
    char mode = clean[0];
    char addr[8] = {};
    memcpy(addr, &clean[1], 7);
    addr[7] = 0;
    char ack = clean[8];
    char label[3] = { (char)clean[9], (char)clean[10], 0 };
    if (label[1] == 0x7F) label[1] = 'd';
    char bid = clean[11];
    char sot = clean[12];

    // Strip leading dots from address
    char* addrClean = addr;
    while (*addrClean == '.') addrClean++;

    // Format header
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
        "ACARS %.3f MHz | SNR: %.1f dB | Mode: %c | Reg: %s\n"
        "  Label: %s | BID: %c | ACK: %c",
        (float)freq / 1e6f, msg.snr, mode, addrClean,
        label, bid, (ack == 0x15) ? '!' : ack);
    msg.formatted_text = hdr;

    // Extract text content (after sot, before suffix)
    int textStart = 13;
    int textEnd = (int)clean.size() - 1;  // exclude ETX/ETB suffix
    if (textEnd > textStart && sot == ACARS_STX) {
        // For downlinks (bid is '0'-'9'), first 4+6 bytes are msg number + flight ID
        std::string text((char*)&clean[textStart], textEnd - textStart);
        if (!text.empty()) {
            msg.formatted_text += "\n  ";
            msg.formatted_text += text;
        }
    }

    // Also try libacars for deeper parsing
    la_msg_dir dir = (bid >= '0' && bid <= '9') ? LA_MSG_DIR_AIR2GND : LA_MSG_DIR_GND2AIR;
    la_proto_node* node = la_acars_parse(msgBuf.data(), (int)msgBuf.size(), dir);
    if (node) {
        la_vstring* vstr = la_proto_tree_format_text(NULL, node);
        if (vstr && vstr->str && strlen(vstr->str) > 0) {
            msg.formatted_text += "\n";
            msg.formatted_text += vstr->str;
            la_vstring_destroy(vstr, true);
        }
        la_proto_tree_destroy(node);
    }

    messageCount++;
    if (msgCallback) {
        msgCallback(msg);
    }
}

float ACARSChannel::getSNR() const {
    if (mskNF > 0 && mskPwr > mskNF) {
        return 10.f * log10f(mskPwr / mskNF);
    }
    return 0.f;
}
