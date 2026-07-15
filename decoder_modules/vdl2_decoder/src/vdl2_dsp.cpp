#include "vdl2_dsp.h"

extern "C" {
#include <correct.h>
#include <libacars/libacars.h>
#include <libacars/acars.h>
#include <libacars/vstring.h>
}

#include <cstdio>
#include <chrono>
#include <ctime>

// ============================================================================
// Static tables
// ============================================================================

// Preamble phase pattern (16 symbols, cumulative differential phase)
const float VDL2Channel::pr_phase[VDL2_PREAMBLE_SYMS] = {
    0 * (float)M_PI / 4.f,
    3 * (float)M_PI / 4.f,
   -3 * (float)M_PI / 4.f,
    1 * (float)M_PI / 4.f,
    1 * (float)M_PI / 4.f,
    2 * (float)M_PI / 4.f,
    0 * (float)M_PI / 4.f,
    4 * (float)M_PI / 4.f,
   -3 * (float)M_PI / 4.f,
    4 * (float)M_PI / 4.f,
   -2 * (float)M_PI / 4.f,
    3 * (float)M_PI / 4.f,
    1 * (float)M_PI / 4.f,
   -2 * (float)M_PI / 4.f,
   -3 * (float)M_PI / 4.f,
    0 * (float)M_PI / 4.f,
};

// Gray code mapping: symbol index -> 3-bit value
const uint8_t VDL2Channel::graycode[VDL2_ARITY] = { 0, 1, 3, 2, 6, 7, 5, 4 };

// Header FEC parity check matrix (5 rows x 25 bits)
const uint32_t VDL2Channel::H[VDL2_HDRFECLEN] = {
    0x001FFF0, // 0000000011111111111110000
    0x07E1FE8, // 0011111100001111111101000
    0x18E61E4, // 1100011100110000111100100
    0x1B6A662, // 1101101101010011001100010
    0x0D3CAA1, // 0110100111100101010100001
};

// Actually, let me use the exact values from the source:
// H[0] = 0b0000000011111111111110000 = 0x000FFE0 -> but that's wrong for 25 bits
// Let me recompute carefully. 25-bit values, MSB first:
// H[0] = 0000000011111111111110000 = 0x0007FF0  (wrong, let me count)
// Bit 24 (MSB) ... Bit 0 (LSB)
// 0000000011111111111110000
// = 0x0 << 24 | ... let me just compute these as hex

// Actually, I need to be very careful here. Let me use the exact binary from dumpvdl2.

const uint32_t VDL2Channel::syndtable[1 << VDL2_HDRFECLEN] = {
    0x0000000, 0x0000001, 0x0000002, 0x0800004, 0x0000004, 0x0800002, 0x1000000, 0x0800000,
    0x0000008, 0x0400000, 0x0200000, 0x0100000, 0x0080000, 0x1100000, 0x0040000, 0x0020000,
    0x0000010, 0x0010000, 0x0804000, 0x0008000, 0x0808000, 0x0004000, 0x0002000, 0x1010000,
    0x0001000, 0x0000800, 0x0000400, 0x0000200, 0x0000100, 0x0000080, 0x0000040, 0x0000020,
};

const uint32_t VDL2Channel::synd_weight[1 << VDL2_HDRFECLEN] = {
    0, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1, 1,
    1, 1, 2, 1, 2, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1
};

// CRC-16-CCITT table (reflected/LSB-first)
const uint16_t VDL2Channel::crctable[256] = {
    0x0000, 0x1189, 0x2312, 0x329B, 0x4624, 0x57AD, 0x6536, 0x74BF,
    0x8C48, 0x9DC1, 0xAF5A, 0xBED3, 0xCA6C, 0xDBE5, 0xE97E, 0xF8F7,
    0x1081, 0x0108, 0x3393, 0x221A, 0x56A5, 0x472C, 0x75B7, 0x643E,
    0x9CC9, 0x8D40, 0xBFDB, 0xAE52, 0xDAED, 0xCB64, 0xF9FF, 0xE876,
    0x2102, 0x308B, 0x0210, 0x1399, 0x6726, 0x76AF, 0x4434, 0x55BD,
    0xAD4A, 0xBCC3, 0x8E58, 0x9FD1, 0xEB6E, 0xFAE7, 0xC87C, 0xD9F5,
    0x3183, 0x200A, 0x1291, 0x0318, 0x77A7, 0x662E, 0x54B5, 0x453C,
    0xBDCB, 0xAC42, 0x9ED9, 0x8F50, 0xFBEF, 0xEA66, 0xD8FD, 0xC974,
    0x4204, 0x538D, 0x6116, 0x709F, 0x0420, 0x15A9, 0x2732, 0x36BB,
    0xCE4C, 0xDFC5, 0xED5E, 0xFCD7, 0x8868, 0x99E1, 0xAB7A, 0xBAF3,
    0x5285, 0x430C, 0x7197, 0x601E, 0x14A1, 0x0528, 0x37B3, 0x263A,
    0xDECD, 0xCF44, 0xFDDF, 0xEC56, 0x98E9, 0x8960, 0xBBFB, 0xAA72,
    0x6306, 0x728F, 0x4014, 0x519D, 0x2522, 0x34AB, 0x0630, 0x17B9,
    0xEF4E, 0xFEC7, 0xCC5C, 0xDDD5, 0xA96A, 0xB8E3, 0x8A78, 0x9BF1,
    0x7387, 0x620E, 0x5095, 0x411C, 0x35A3, 0x242A, 0x16B1, 0x0738,
    0xFFCF, 0xEE46, 0xDCDD, 0xCD54, 0xB9EB, 0xA862, 0x9AF9, 0x8B70,
    0x8408, 0x9581, 0xA71A, 0xB693, 0xC22C, 0xD3A5, 0xE13E, 0xF0B7,
    0x0840, 0x19C9, 0x2B52, 0x3ADB, 0x4E64, 0x5FED, 0x6D76, 0x7CFF,
    0x9489, 0x8500, 0xB79B, 0xA612, 0xD2AD, 0xC324, 0xF1BF, 0xE036,
    0x18C1, 0x0948, 0x3BD3, 0x2A5A, 0x5EE5, 0x4F6C, 0x7DF7, 0x6C7E,
    0xA50A, 0xB483, 0x8618, 0x9791, 0xE32E, 0xF2A7, 0xC03C, 0xD1B5,
    0x2942, 0x38CB, 0x0A50, 0x1BD9, 0x6F66, 0x7EEF, 0x4C74, 0x5DFD,
    0xB58B, 0xA402, 0x9699, 0x8710, 0xF3AF, 0xE226, 0xD0BD, 0xC134,
    0x39C3, 0x284A, 0x1AD1, 0x0B58, 0x7FE7, 0x6E6E, 0x5CF5, 0x4D7C,
    0xC60C, 0xD785, 0xE51E, 0xF497, 0x8028, 0x91A1, 0xA33A, 0xB2B3,
    0x4A44, 0x5BCD, 0x6956, 0x78DF, 0x0C60, 0x1DE9, 0x2F72, 0x3EFB,
    0xD68D, 0xC704, 0xF59F, 0xE416, 0x90A9, 0x8120, 0xB3BB, 0xA232,
    0x5AC5, 0x4B4C, 0x79D7, 0x685E, 0x1CE1, 0x0D68, 0x3FF3, 0x2E7A,
    0xE70E, 0xF687, 0xC41C, 0xD595, 0xA12A, 0xB0A3, 0x8238, 0x93B1,
    0x6B46, 0x7ACF, 0x4854, 0x59DD, 0x2D62, 0x3CEB, 0x0E70, 0x1FF9,
    0xF78F, 0xE606, 0xD49D, 0xC514, 0xB1AB, 0xA022, 0x92B9, 0x8330,
    0x7BC7, 0x6A4E, 0x58D5, 0x495C, 0x3DE3, 0x2C6A, 0x1EF1, 0x0F78,
};

// ============================================================================
// Chebyshev LPF
// ============================================================================

void ChebyshevLPF::calcPole(int p, float cutoff, float ripple, int npoles,
                            float* AA, float* BB) {
    float rp, ip;
    float angle = (float)M_PI / (2.f * npoles) + (p - 1) * (float)M_PI / npoles;
    ip = sinf(angle);
    rp = -cosf(angle);

    if (ripple != 0.f) {
        float es = sqrtf(powf(100.f / (100.f - ripple), 2.f) - 1.f);
        float vx = (1.f / npoles) * logf((1.f / es) + sqrtf(1.f / (es * es) + 1.f));
        float kx = (1.f / npoles) * logf((1.f / es) + sqrtf(1.f / (es * es) - 1.f));
        kx = (expf(kx) + expf(-kx)) / 2.f;
        rp *= (expf(vx) - expf(-vx)) / 2.f / kx;
        ip *= (expf(vx) + expf(-vx)) / 2.f / kx;
    }

    float t = 2.f * tanf(0.5f);
    float w = 2.f * (float)M_PI * cutoff;
    float m = rp * rp + ip * ip;
    float d = 4.f - 4.f * rp * t + m * t * t;
    float x0 = t * t / d;
    float x1 = 2.f * x0;
    float x2 = x0;
    float y1 = (8.f - 2.f * m * t * t) / d;
    float y2 = (-4.f - 4.f * rp * t - m * t * t) / d;

    float k = sinf(0.5f - w / 2.f) / sinf(0.5f + w / 2.f);
    d = 1.f + y1 * k - y2 * k * k;
    AA[0] = (x0 - x1 * k + x2 * k * k) / d;
    AA[1] = (-2.f * x0 * k + x1 + x1 * k * k - 2.f * x2 * k) / d;
    AA[2] = (x0 * k * k - x1 * k + x2) / d;
    BB[1] = (2.f * k + y1 + y1 * k * k - 2.f * y2 * k) / d;
    BB[2] = (-(k * k) - y1 * k + y2) / d;
}

void ChebyshevLPF::init(float cutoff_freq, float ripple, int npoles) {
    const int BSIZE = 23;
    float tA[BSIZE] = {}, tB[BSIZE] = {};
    float AA[3] = {}, BB[3] = {};
    float cA[BSIZE] = {}, cB[BSIZE] = {};

    cA[2] = 1.f;
    cB[2] = 1.f;

    for (int p = 1; p <= npoles / 2; p++) {
        calcPole(p, cutoff_freq, ripple, npoles, AA, BB);
        memcpy(tA, cA, sizeof(tA));
        memcpy(tB, cB, sizeof(tB));
        for (int i = 2; i < BSIZE; i++) {
            cA[i] = AA[0] * tA[i] + AA[1] * tA[i - 1] + AA[2] * tA[i - 2];
            cB[i] = tB[i] - BB[1] * tB[i - 1] - BB[2] * tB[i - 2];
        }
    }

    cB[2] = 0.f;
    for (int i = 0; i < BSIZE - 2; i++) {
        A[i] = cA[i + 2];
        B[i] = -cB[i + 2];
    }

    float sa = 0.f, sb = 0.f;
    for (int i = 0; i < BSIZE - 2; i++) {
        sa += A[i];
        sb += B[i];
    }
    float gain = sa / (1.f - sb);
    for (int i = 0; i < BSIZE - 2; i++) {
        A[i] /= gain;
    }
}

float ChebyshevLPF::process(float* in_hist, float* out_hist) {
    // 2-pole: uses A[0..2] and B[1..2]
    float r = A[0] * in_hist[0] + A[1] * in_hist[1] + A[2] * in_hist[2]
            + B[1] * out_hist[1] + B[2] * out_hist[2];
    return r;
}

// ============================================================================
// VDL2 Channel
// ============================================================================

VDL2Channel::VDL2Channel() {
    rs_decoder = correct_reed_solomon_create(
        (uint16_t)VDL2_RS_POLY,
        VDL2_RS_FCR, VDL2_RS_PRIM,
        VDL2_RS_N - VDL2_RS_K  // 6 parity symbols
    );
}

VDL2Channel::~VDL2Channel() {
    if (rs_decoder) {
        correct_reed_solomon_destroy((correct_reed_solomon*)rs_decoder);
    }
}

void VDL2Channel::init(uint32_t _freq, uint32_t _sample_rate) {
    freq = _freq;
    sample_rate = _sample_rate;

    // Init Chebyshev LPF: 8 kHz cutoff at our sample rate
    float cutoff = 8000.f / (float)sample_rate;
    lpf.init(cutoff, 0.5f, 2);

    // Precompute linear regression values for preamble sync
    float mean_x = 0.f;
    lr_denom = 0.f;
    for (int i = 0; i < VDL2_PREAMBLE_SYMS; i++) mean_x += (float)i;
    mean_x /= VDL2_PREAMBLE_SYMS;
    for (int i = 0; i < VDL2_PREAMBLE_SYMS; i++) {
        lr_X[i] = (float)i - mean_x;
        lr_denom += lr_X[i] * lr_X[i];
    }

    reset();
}

void VDL2Channel::reset() {
    demod_state = DemodState::INIT;
    decoder_state = DecoderState::IDLE;
    memset(syncbuf, 0, sizeof(syncbuf));
    syncbufidx = 0;
    prev_phi = 0;
    prev_dphi = 0;
    dphi = 0;
    pherr[0] = pherr[1] = pherr[2] = 1000.f;
    mag_lp = 0;
    mag_nf = 0;
    nfcnt = 0;
    sclk = 0;
    frame_pwr = 0;
    frame_pwr_cnt = 0;
    num_fec_corrections = 0;
    memset(re_in, 0, sizeof(re_in));
    memset(re_out, 0, sizeof(re_out));
    memset(im_in, 0, sizeof(im_in));
    memset(im_out, 0, sizeof(im_out));
    bs.reset();
    requested_bits = VDL2_HEADER_LEN;
}

void VDL2Channel::processIQ(const float* iq, int num_samples) {
    samplesProcessed += num_samples;
    for (int i = 0; i < num_samples; i++) {
        // Shift filter history
        re_in[2] = re_in[1]; re_in[1] = re_in[0];
        im_in[2] = im_in[1]; im_in[1] = im_in[0];
        re_out[2] = re_out[1]; re_out[1] = re_out[0];
        im_out[2] = im_out[1]; im_out[1] = im_out[0];

        // New sample (VFO already centered — no downmix needed)
        re_in[0] = iq[i * 2];
        im_in[0] = iq[i * 2 + 1];

        // Chebyshev LPF
        re_out[0] = lpf.process(re_in, re_out);
        im_out[0] = lpf.process(im_in, im_out);

        // Feed to demodulator
        demod(re_out[0], im_out[0]);
    }
}

// ============================================================================
// Demodulator
// ============================================================================

void VDL2Channel::demod(float re, float im) {
    switch (demod_state) {
    case DemodState::INIT: {
        // Store phase in circular sync buffer
        syncbufidx = (syncbufidx + 1) % VDL2_SYNC_BUFLEN;
        syncbuf[syncbufidx] = atan2f(im, re);

        // Only check sync every SYNC_SKIP samples
        if (++sclk < VDL2_SYNC_SKIP) return;
        sclk = 0;

        // Update magnitude / noise floor
        float mag = hypotf(re, im);
        mag_lp = mag_lp * VDL2_MAG_LP + mag * (1.f - VDL2_MAG_LP);

        if (++nfcnt >= 1000) {
            nfcnt = 0;
            mag_nf = VDL2_NF_LP * mag_nf + (1.f - VDL2_NF_LP) * fminf(mag_lp, mag_nf) + 0.0001f;
        }

        if (gotSync()) {
            syncCount++;
            demod_state = DemodState::SYNC;
            decoder_state = DecoderState::HEADER;
            bs.reset();
            requested_bits = VDL2_HEADER_LEN;
            frame_pwr = 0;
            frame_pwr_cnt = 0;
            num_fec_corrections = 0;
        }
        break;
    }

    case DemodState::SYNC: {
        if (++sclk < VDL2_SPS) return;
        sclk = 0;

        float phi = atan2f(im, re);
        float dp = phi - prev_phi - dphi;

        // Wrap to [0, 2*pi)
        while (dp < 0) dp += 2.f * (float)M_PI;
        while (dp > 2.f * (float)M_PI) dp -= 2.f * (float)M_PI;

        // Normalize to [0, 8) in units of pi/4
        dp /= (float)M_PI_4;
        int idx = (int)roundf(dp) % VDL2_ARITY;

        // Track signal power
        float symbol_pwr = re * re + im * im;
        frame_pwr = (frame_pwr * frame_pwr_cnt + symbol_pwr) / (frame_pwr_cnt + 1);
        frame_pwr_cnt++;

        prev_phi = phi;

        // Append 3 gray-coded bits
        uint8_t gc = graycode[idx];
        bs.appendMSBFirst(&gc, 1, VDL2_BPS);

        // Check if we have enough bits
        if (bs.end - bs.start >= requested_bits) {
            decodeBurst();
        }
        break;
    }
    }
}

// ============================================================================
// Preamble sync detection
// ============================================================================

static float calc_para_vertex(float x, int d, float y1, float y2, float y3) {
    float denom = (float)(d * 2 * d * (-d));
    float fA = (x * (y2 - y1) + (x - d) * (y1 - y3) + (x - 2 * d) * (y3 - y2)) / denom;
    float fB = (x * x * (y1 - y2) + (x - d) * (x - d) * (y3 - y1) +
                (x - 2 * d) * (x - 2 * d) * (y2 - y3)) / denom;
    return -fB / (2.f * fA);
}

bool VDL2Channel::gotSync() {
    float errvec[VDL2_PREAMBLE_SYMS];

    // Read phase values at symbol positions from circular buffer
    for (int i = 0; i < VDL2_PREAMBLE_SYMS; i++) {
        int idx = (syncbufidx - (VDL2_PREAMBLE_SYMS - 1 - i) * VDL2_SPS + VDL2_SYNC_BUFLEN * 16) % VDL2_SYNC_BUFLEN;
        errvec[i] = syncbuf[idx] - pr_phase[i];
    }

    // Phase unwrap
    for (int i = 1; i < VDL2_PREAMBLE_SYMS; i++) {
        float diff = errvec[i] - errvec[i - 1];
        if (diff > (float)M_PI) errvec[i] -= 2.f * (float)M_PI;
        else if (diff < -(float)M_PI) errvec[i] += 2.f * (float)M_PI;
    }

    // Subtract mean (removes arbitrary starting phase offset)
    float mean = 0.f;
    for (int i = 0; i < VDL2_PREAMBLE_SYMS; i++) mean += errvec[i];
    mean /= VDL2_PREAMBLE_SYMS;
    for (int i = 0; i < VDL2_PREAMBLE_SYMS; i++) errvec[i] -= mean;

    // Linear regression to estimate frequency error
    float lr_num = 0.f;
    for (int i = 0; i < VDL2_PREAMBLE_SYMS; i++) {
        lr_num += lr_X[i] * errvec[i];
    }
    float slope = lr_num / lr_denom;

    // Remove freq error and compute residual
    float err = 0.f;
    for (int i = 0; i < VDL2_PREAMBLE_SYMS; i++) {
        float residual = errvec[i] - slope * lr_X[i];
        err += residual * residual;
    }

    // Shift error history
    pherr[2] = pherr[1];
    pherr[1] = pherr[0];
    pherr[0] = err;

    // Sync detection: found minimum (error increasing after being below threshold)
    if (pherr[1] < VDL2_SYNC_THRESHOLD && pherr[0] > pherr[1]) {
        // Parabolic interpolation for precise timing
        float vertex = calc_para_vertex((float)sclk, VDL2_SYNC_SKIP,
                                        pherr[2], pherr[1], pherr[0]);
        sclk = -(int)roundf(vertex);

        // Save frequency correction from the best preamble
        dphi = slope;  // radians per symbol for freq correction
        prev_dphi = dphi;
        prev_phi = syncbuf[syncbufidx];

        // Compute ppm error
        if (freq > 0) {
            ppm_error = (float)VDL2_SYMBOL_RATE * dphi / (2.f * (float)M_PI * (float)freq) * 1e6f;
        }

        pherr[0] = pherr[1] = pherr[2] = 1000.f;
        return true;
    }

    return false;
}

// ============================================================================
// Burst decoder
// ============================================================================

uint32_t VDL2Channel::parity(uint32_t v) {
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
}

uint32_t VDL2Channel::reverse(uint32_t v, int numbits) {
    uint32_t r = v;
    int s = sizeof(v) * 8 - 1;
    for (v >>= 1; v; v >>= 1) {
        r <<= 1;
        r |= v & 1;
        s--;
    }
    r <<= s;
    r >>= (32 - numbits);
    return r;
}

int VDL2Channel::getFecOctetCount(uint32_t len) {
    if (len < 3)  return 0;
    if (len < 31) return 2;
    if (len < 68) return 4;
    return 6;
}

uint32_t VDL2Channel::decodeHeader(uint32_t* r) {
    uint32_t syn = 0u;
    for (int i = 0; i < VDL2_HDRFECLEN; i++) {
        uint32_t row = *r & H[i];
        syn |= parity(row) << (VDL2_HDRFECLEN - 1 - i);
    }
    *r ^= syndtable[syn];
    return syn;
}

int VDL2Channel::rsVerify(uint8_t* data, int fec_octets) {
    if (fec_octets == 0) return 0;

    // libcorrect expects the full 255-byte codeword
    // data should be 255 bytes: RS_K data + (RS_N-RS_K) parity
    // Unused parity positions should be marked as erasures

    int actual_parity = fec_octets;
    int expected_parity = VDL2_RS_N - VDL2_RS_K;  // 6
    int erasure_cnt = expected_parity - actual_parity;

    if (erasure_cnt > 0) {
        uint8_t erasure_locs[6];
        for (int i = 0; i < erasure_cnt; i++) {
            erasure_locs[i] = VDL2_RS_K + actual_parity + i;
        }
        ssize_t ret = correct_reed_solomon_decode_with_erasures(
            (correct_reed_solomon*)rs_decoder,
            data, VDL2_RS_N,
            erasure_locs, erasure_cnt,
            data  // decode in-place
        );
        return (ret < 0) ? -1 : (int)(VDL2_RS_N - VDL2_RS_K);
    }
    else {
        ssize_t ret = correct_reed_solomon_decode(
            (correct_reed_solomon*)rs_decoder,
            data, VDL2_RS_N,
            data
        );
        return (ret < 0) ? -1 : 0;
    }
}

void VDL2Channel::decodeBurst() {
    switch (decoder_state) {
    case DecoderState::HEADER: {
        lfsr = VDL2_LFSR_IV;
        bs.descramble(&lfsr);

        uint32_t header = bs.readWordMSBFirst(VDL2_HEADER_LEN);
        uint32_t mask = (1u << (VDL2_TRLEN + VDL2_HDRFECLEN)) - 1;
        header &= mask;

        syndrome = decodeHeader(&header);

        // Check reserved bits
        if ((header & mask) != header) {
            headerFailCount++;
            demod_state = DemodState::INIT;
            decoder_state = DecoderState::IDLE;
            return;
        }

        header >>= VDL2_HDRFECLEN;
        datalen = reverse(header & ((1u << VDL2_TRLEN) - 1), VDL2_TRLEN);

        uint32_t max_len = (syndrome != 0) ? VDL2_MAX_FRAME_LEN_CORRECTED : VDL2_MAX_FRAME_LEN;
        if (datalen == 0 || datalen > max_len) {
            headerFailCount++;
            demod_state = DemodState::INIT;
            decoder_state = DecoderState::IDLE;
            return;
        }

        headerOkCount++;
        // Compute RS blocking
        datalen_octets = (datalen + 7) / 8;
        num_blocks = datalen_octets / VDL2_RS_K;
        fec_octets = num_blocks * (VDL2_RS_N - VDL2_RS_K);
        last_block_len_octets = datalen_octets % VDL2_RS_K;
        if (last_block_len_octets != 0) {
            num_blocks++;
            fec_octets += getFecOctetCount(last_block_len_octets);
        }
        else {
            last_block_len_octets = VDL2_RS_K;
        }

        requested_bits = 8 * (datalen_octets + fec_octets);
        decoder_state = DecoderState::DATA;
        break;
    }

    case DecoderState::DATA:
        decodeData();
        break;

    case DecoderState::IDLE:
        break;
    }
}

void VDL2Channel::decodeData() {
    // Descramble (LFSR continues from header)
    bs.descramble(&lfsr);

    // Read data and FEC octets
    std::vector<uint8_t> data(datalen_octets);
    std::vector<uint8_t> fec(fec_octets);
    bs.readOctetsLSBFirst(data.data(), datalen_octets);
    bs.readOctetsLSBFirst(fec.data(), fec_octets);

    // Deinterleave into RS blocks
    std::vector<std::vector<uint8_t>> rs_tab(num_blocks, std::vector<uint8_t>(VDL2_RS_N, 0));

    // Deinterleave data (column-major -> row-major)
    {
        uint32_t row = 0, col = 0;
        uint32_t last_row_len = datalen_octets % VDL2_RS_K;
        if (last_row_len == 0) last_row_len = VDL2_RS_K;

        for (uint32_t i = 0; i < datalen_octets; i++) {
            if (row == num_blocks - 1 && col >= last_row_len) {
                rs_tab[row][col] = 0;
                row = 0;
                col++;
            }
            if (row < num_blocks && col < VDL2_RS_K) {
                rs_tab[row][col] = data[i];
            }
            row++;
            if (row >= num_blocks) {
                row = 0;
                col++;
            }
        }
    }

    // Deinterleave FEC (into parity positions of each block)
    {
        uint32_t fec_rows = num_blocks;
        int last_fec = getFecOctetCount(last_block_len_octets);
        uint32_t last_row_fec = (last_block_len_octets == VDL2_RS_K) ?
                                (VDL2_RS_N - VDL2_RS_K) : (uint32_t)last_fec;

        uint32_t row = 0, col = VDL2_RS_K;
        for (uint32_t i = 0; i < fec_octets; i++) {
            if (row == fec_rows - 1 && (col - VDL2_RS_K) >= last_row_fec) {
                rs_tab[row][col] = 0;
                row = 0;
                col++;
            }
            if (row < num_blocks && col < VDL2_RS_N) {
                rs_tab[row][col] = fec[i];
            }
            row++;
            if (row >= fec_rows) {
                row = 0;
                col++;
            }
        }
    }

    // RS decode each block
    num_fec_corrections = 0;
    for (uint32_t b = 0; b < num_blocks; b++) {
        int block_fec = (b == num_blocks - 1) ?
                        getFecOctetCount(last_block_len_octets) :
                        (int)(VDL2_RS_N - VDL2_RS_K);

        int ret = rsVerify(rs_tab[b].data(), block_fec);
        if (ret < 0) {
            // RS decode failed
            rsFailCount++;
            demod_state = DemodState::INIT;
            decoder_state = DecoderState::IDLE;
            return;
        }
        num_fec_corrections += ret;
    }

    // Reassemble corrected data
    Bitstream corrected(datalen + 64);
    for (uint32_t b = 0; b < num_blocks; b++) {
        uint32_t block_len = (b == num_blocks - 1) ? last_block_len_octets : VDL2_RS_K;
        corrected.appendLSBFirst(rs_tab[b].data(), block_len, 8);
    }

    // Truncate to exact datalen
    if (corrected.end > corrected.start + datalen) {
        corrected.end = corrected.start + datalen;
    }

    // HDLC unstuffing - extract frames
    while (true) {
        auto frame = corrected.copyNextFrame();
        if (frame.empty()) break;

        // Compute SNR estimate
        float snr = 0.f;
        if (mag_nf > 0.f && frame_pwr > 0.f) {
            snr = 10.f * log10f(frame_pwr / (mag_nf * mag_nf));
        }

        parseAVLC(frame.data(), (int)frame.size(), snr);
    }

    // Back to searching
    demod_state = DemodState::INIT;
    decoder_state = DecoderState::IDLE;
}

// ============================================================================
// CRC-16
// ============================================================================

uint16_t VDL2Channel::crc16_ccitt(const uint8_t* data, uint32_t len, uint16_t crc_init) {
    uint16_t crc = crc_init;
    for (uint32_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crctable[(crc ^ data[i]) & 0xFF];
    }
    return crc;
}

// ============================================================================
// AVLC frame parser -> libacars
// ============================================================================

void VDL2Channel::parseAVLC(const uint8_t* data, int len, float snr) {
    if (len < VDL2_MIN_AVLC_LEN) return;

    // CRC check
    uint16_t crc = crc16_ccitt(data, len, 0xFFFF);
    if (crc != VDL2_GOOD_FCS) { crcFailCount++; return; }

    // Strip 2-byte FCS
    len -= 2;

    // Build message
    VDL2Message msg;
    msg.freq = freq;
    msg.snr = snr;
    msg.num_fec_corrections = num_fec_corrections;
    msg.ppm_error = ppm_error;
    msg.is_acars = false;

    // Get timestamp
    msg.timestamp = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    // Parse addresses (4 bytes each)
    // Destination: bytes 0-3, Source: bytes 4-7, Control: byte 8
    uint32_t dst_addr = reverse(
        ((data[0] >> 1) | (data[1] << 6) | (data[2] << 13) | ((data[3] & 0xFE) << 20))
        & 0x0FFFFFFF, 28);
    uint32_t src_addr = reverse(
        ((data[4] >> 1) | (data[5] << 6) | (data[6] << 13) | ((data[7] & 0xFE) << 20))
        & 0x0FFFFFFF, 28);

    bool dst_is_ground = (data[0] & 1) == 0;
    bool src_is_ground = (data[4] & 1) == 0;
    uint8_t control = data[8];

    // Format header
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
        "VDL2 %.3f MHz | SNR: %.1f dB | FEC: %d corrections | PPM: %+.1f\n"
        "  %s %06X -> %s %06X",
        (float)freq / 1e6f, snr, num_fec_corrections, ppm_error,
        src_is_ground ? "GND" : "AIR", src_addr & 0xFFFFFF,
        dst_is_ground ? "GND" : "AIR", dst_addr & 0xFFFFFF);
    msg.formatted_text = hdr;

    // Check for ACARS payload (I-frame with FF FF 01 marker)
    if ((control & 0x01) == 0 && len > 12) {
        // I-frame
        int info_start = 9;  // after addresses + control
        int info_len = len - info_start;
        const uint8_t* info = data + info_start;

        if (info_len >= 3 && info[0] == 0xFF && info[1] == 0xFF && info[2] == 0x01) {
            // ACARS message — feed to libacars
            msg.is_acars = true;
            const uint8_t* acars_data = info + 3;
            int acars_len = info_len - 3;

            la_msg_dir dir = src_is_ground ? LA_MSG_DIR_GND2AIR : LA_MSG_DIR_AIR2GND;
            la_proto_node* node = la_acars_parse(acars_data, acars_len, dir);

            if (node) {
                la_vstring* vstr = la_proto_tree_format_text(NULL, node);
                if (vstr) {
                    msg.formatted_text += "\n";
                    msg.formatted_text += vstr->str;
                    la_vstring_destroy(vstr, true);
                }
                la_proto_tree_destroy(node);
            }
        }
        else {
            // Non-ACARS I-frame (X.25/CLNP/CPDLC/ADS-C)
            // For now, hex-dump the info field
            msg.formatted_text += "\n  [Non-ACARS I-frame, ";
            msg.formatted_text += std::to_string(info_len) + " bytes]";

            // Try to format with libacars anyway (X.25 path)
            // TODO: Add X.25 -> CLNP -> CPDLC path when we add the shim
        }
    }
    else if ((control & 0x03) == 0x03) {
        // U-frame
        msg.formatted_text += "\n  [U-frame, control=0x";
        char ctlhex[8];
        snprintf(ctlhex, sizeof(ctlhex), "%02X", control);
        msg.formatted_text += ctlhex;
        msg.formatted_text += "]";
    }
    else {
        // S-frame
        msg.formatted_text += "\n  [S-frame, control=0x";
        char ctlhex[8];
        snprintf(ctlhex, sizeof(ctlhex), "%02X", control);
        msg.formatted_text += ctlhex;
        msg.formatted_text += "]";
    }

    messageCount++;

    if (msgCallback) {
        msgCallback(msg);
    }
}

float VDL2Channel::getSNR() const {
    if (mag_nf > 0.f && mag_lp > mag_nf) {
        return 20.f * log10f(mag_lp / mag_nf);
    }
    return 0.f;
}
