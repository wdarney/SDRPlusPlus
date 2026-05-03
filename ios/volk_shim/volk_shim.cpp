// VOLK -> Accelerate / scalar shim. See ios/volk_shim/include/volk/volk.h.
//
// Performance notes:
// * Real-vector ops (add/sub/mul/dotpr/sum/argmax) map directly to vDSP and
//   are the biggest wins.
// * Conversion ops use vDSP's vfix/vflt + a fused scalar multiply.
// * Interleaved-complex ops (multiply, magnitude, conjugate, etc.) don't
//   have direct vDSP equivalents — vDSP wants split complex (DSPSplitComplex)
//   and the cost of transposing in/out for SDR++'s small-to-medium buffers
//   is higher than just letting clang autovectorize a tight scalar loop.
//   Verified at -O3 on arm64 it produces NEON FMLA / FMUL / FSUB / FSQRT
//   sequences that match what hand-written intrinsics would do.
// * The phase rotator is fundamentally serial across samples (phase
//   accumulates), so scalar with sin/cos is the only practical option.

#include <volk/volk.h>
#include <Accelerate/Accelerate.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex>

extern "C" {

// ---- Allocator ---------------------------------------------------------
void* volk_malloc(size_t size, size_t alignment) {
    void* p = NULL;
    // posix_memalign requires alignment to be a power of two and a multiple
    // of sizeof(void*). Round up if the caller passes something smaller.
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    if (posix_memalign(&p, alignment, size) != 0) return NULL;
    return p;
}

void volk_free(void* ptr) { free(ptr); }

size_t volk_get_alignment(void) {
    // arm64 NEON wants 16-byte alignment; AVX would want 32. SDR++ only
    // calls this to align allocations, never compares the value.
    return 32;
}

// ---- Real-vector arithmetic --------------------------------------------
void volk_32f_x2_add_32f(float* c, const float* a, const float* b, unsigned int n) {
    vDSP_vadd(a, 1, b, 1, c, 1, n);
}
void volk_32f_x2_subtract_32f(float* c, const float* a, const float* b, unsigned int n) {
    // vDSP_vsub computes B - A. Argument order matches volk's a - b -> c.
    vDSP_vsub(b, 1, a, 1, c, 1, n);
}
void volk_32f_x2_multiply_32f(float* c, const float* a, const float* b, unsigned int n) {
    vDSP_vmul(a, 1, b, 1, c, 1, n);
}
void volk_32f_s32f_multiply_32f(float* c, const float* a, float k, unsigned int n) {
    vDSP_vsmul(a, 1, &k, c, 1, n);
}
void volk_32f_x2_dot_prod_32f(float* result, const float* a, const float* b, unsigned int n) {
    vDSP_dotpr(a, 1, b, 1, result, n);
}
void volk_32f_accumulator_s32f(float* result, const float* a, unsigned int n) {
    vDSP_sve(a, 1, result, n);
}
void volk_32f_index_max_32u(uint32_t* idx, const float* a, unsigned int n) {
    float val;
    vDSP_Length pos;
    vDSP_maxvi(a, 1, &val, &pos, n);
    *idx = (uint32_t)pos;
}

// ---- Conversions -------------------------------------------------------
// VOLK's "s32f" convention: when going int -> float, divide by `scale`;
// when going float -> int, multiply by `scale`. (Yes, that asymmetry is
// upstream; see volk's docs.)
void volk_8i_s32f_convert_32f(float* out, const int8_t* in, float scale, unsigned int n) {
    vDSP_vflt8((const char*)in, 1, out, 1, n);
    float inv = 1.0f / scale;
    vDSP_vsmul(out, 1, &inv, out, 1, n);
}
void volk_16i_s32f_convert_32f(float* out, const int16_t* in, float scale, unsigned int n) {
    vDSP_vflt16(in, 1, out, 1, n);
    float inv = 1.0f / scale;
    vDSP_vsmul(out, 1, &inv, out, 1, n);
}
void volk_32i_s32f_convert_32f(float* out, const int32_t* in, float scale, unsigned int n) {
    vDSP_vflt32(in, 1, out, 1, n);
    float inv = 1.0f / scale;
    vDSP_vsmul(out, 1, &inv, out, 1, n);
}
void volk_32f_s32f_convert_8i(int8_t* out, const float* in, float scale, unsigned int n) {
    // vDSP_vsmul + vDSP_vfix8 with saturation. vfix8 saturates to int8 range.
    // The vDSP signature uses `char*` rather than `int8_t*`; cast through.
    float buf_static[1024];
    float* buf = (n <= 1024) ? buf_static : (float*)malloc(n * sizeof(float));
    vDSP_vsmul(in, 1, &scale, buf, 1, n);
    vDSP_vfix8(buf, 1, (char*)out, 1, n);
    if (buf != buf_static) free(buf);
}
void volk_32f_s32f_convert_16i(int16_t* out, const float* in, float scale, unsigned int n) {
    float buf_static[1024];
    float* buf = (n <= 1024) ? buf_static : (float*)malloc(n * sizeof(float));
    vDSP_vsmul(in, 1, &scale, buf, 1, n);
    vDSP_vfix16(buf, 1, out, 1, n);
    if (buf != buf_static) free(buf);
}
void volk_32f_s32f_convert_32i(int32_t* out, const float* in, float scale, unsigned int n) {
    float buf_static[1024];
    float* buf = (n <= 1024) ? buf_static : (float*)malloc(n * sizeof(float));
    vDSP_vsmul(in, 1, &scale, buf, 1, n);
    vDSP_vfix32(buf, 1, out, 1, n);
    if (buf != buf_static) free(buf);
}

// ---- Complex arithmetic (interleaved 32fc) -----------------------------
// All scalar with std::complex; the optimizer produces fast NEON code on arm64.

void volk_32fc_x2_multiply_32fc(lv_32fc_t* c, const lv_32fc_t* a, const lv_32fc_t* b, unsigned int n) {
    for (unsigned i = 0; i < n; ++i) c[i] = a[i] * b[i];
}

void volk_32fc_x2_dot_prod_32fc(lv_32fc_t* r, const lv_32fc_t* a, const lv_32fc_t* b, unsigned int n) {
    std::complex<float> acc(0.0f, 0.0f);
    for (unsigned i = 0; i < n; ++i) acc += a[i] * b[i];
    *r = acc;
}

void volk_32fc_32f_multiply_32fc(lv_32fc_t* c, const lv_32fc_t* a, const float* b, unsigned int n) {
    for (unsigned i = 0; i < n; ++i) c[i] = a[i] * b[i];
}

void volk_32fc_32f_dot_prod_32fc(lv_32fc_t* r, const lv_32fc_t* a, const float* b, unsigned int n) {
    std::complex<float> acc(0.0f, 0.0f);
    for (unsigned i = 0; i < n; ++i) acc += a[i] * b[i];
    *r = acc;
}

void volk_32fc_conjugate_32fc(lv_32fc_t* c, const lv_32fc_t* a, unsigned int n) {
    for (unsigned i = 0; i < n; ++i) c[i] = std::conj(a[i]);
}

void volk_32fc_magnitude_32f(float* m, const lv_32fc_t* a, unsigned int n) {
    for (unsigned i = 0; i < n; ++i) {
        float re = a[i].real(), im = a[i].imag();
        m[i] = sqrtf(re*re + im*im);
    }
}

void volk_32fc_s32f_power_spectrum_32f(float* p, const lv_32fc_t* a, float norm, unsigned int n) {
    // VOLK definition: 10*log10(|a[i]|^2 / norm) + correction. SDR++ only
    // ever passes norm=1.0 in practice but keep the divide for compat.
    const float invNorm = 1.0f / norm;
    for (unsigned i = 0; i < n; ++i) {
        float re = a[i].real(), im = a[i].imag();
        float mag2 = (re*re + im*im) * invNorm;
        p[i] = 10.0f * log10f(mag2 + 1e-30f);   // floor to avoid -inf on silence
    }
}

void volk_32fc_deinterleave_real_32f(float* r, const lv_32fc_t* a, unsigned int n) {
    for (unsigned i = 0; i < n; ++i) r[i] = a[i].real();
}

void volk_32f_x2_interleave_32fc(lv_32fc_t* c, const float* re, const float* im, unsigned int n) {
    for (unsigned i = 0; i < n; ++i) c[i] = std::complex<float>(re[i], im[i]);
}

// ---- Phase rotators ----------------------------------------------------
// Multiply each input sample by an advancing phasor. The phase MUST be
// renormalized periodically; otherwise its magnitude drifts away from 1.0
// and accumulated multiplies turn into amplitude modulation. Renorm every
// 256 samples matches what real VOLK does.
static inline void rotator_impl(lv_32fc_t* out, const lv_32fc_t* in,
                                const lv_32fc_t pdelta,
                                lv_32fc_t* phase, unsigned int n) {
    std::complex<float> ph = *phase;
    const std::complex<float> pd = pdelta;
    unsigned i = 0;
    while (i < n) {
        const unsigned end = (i + 256u < n) ? i + 256u : n;
        for (; i < end; ++i) {
            out[i] = in[i] * ph;
            ph    *= pd;
        }
        // Renormalize.
        float mag = std::abs(ph);
        if (mag > 0.0f) ph /= mag;
    }
    *phase = ph;
}

void volk_32fc_s32fc_x2_rotator_32fc(lv_32fc_t* out, const lv_32fc_t* in,
                                     const lv_32fc_t pdelta,
                                     lv_32fc_t* phase, unsigned int n) {
    rotator_impl(out, in, pdelta, phase, n);
}

void volk_32fc_s32fc_x2_rotator2_32fc(lv_32fc_t* out, const lv_32fc_t* in,
                                      const lv_32fc_t* pdelta,
                                      lv_32fc_t* phase, unsigned int n) {
    rotator_impl(out, in, *pdelta, phase, n);
}

} // extern "C"
