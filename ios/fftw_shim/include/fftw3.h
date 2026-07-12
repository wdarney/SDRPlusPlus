// Single-precision FFTW3 shim for iOS — exports just the slice of the FFTW3
// API that SDR++ actually calls. Implementations live in fftw_shim.cpp and
// use Apple's Accelerate framework (vDSP DFT) under the hood.
//
// Real FFTW3 ships hundreds of API entry points (multi-dim, real, MPI,
// threading, wisdom, ...). SDR++ uses a 1D complex single-precision DFT
// and an aligned allocator. Anything else will fail at link time, which is
// the desired behavior — adding a new FFTW3 dep should be deliberate.

#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// FFTW's interleaved-complex type: [real, imag] floats.
typedef float fftwf_complex[2];

// Opaque plan handle — definition lives in the shim's .cpp.
typedef struct fftwf_plan_s* fftwf_plan;

// Direction flags. FFTW uses -1 / +1 sign conventions.
#define FFTW_FORWARD  (-1)
#define FFTW_BACKWARD (+1)

// Planning flags. SDR++ only ever passes FFTW_ESTIMATE; the others are
// defined as no-op aliases so future code that uses them still compiles.
#define FFTW_ESTIMATE   (1U << 6)
#define FFTW_MEASURE    (0U)
#define FFTW_PATIENT    (1U << 5)
#define FFTW_EXHAUSTIVE (1U << 3)

void* fftwf_malloc(size_t bytes);
void  fftwf_free(void* ptr);

fftwf_plan fftwf_plan_dft_1d(int n,
                             fftwf_complex* in,
                             fftwf_complex* out,
                             int sign,
                             unsigned flags);
void fftwf_execute(const fftwf_plan plan);
void fftwf_destroy_plan(fftwf_plan plan);

#ifdef __cplusplus
}
#endif
