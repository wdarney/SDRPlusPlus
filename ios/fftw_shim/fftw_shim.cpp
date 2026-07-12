// FFTW3 -> Accelerate (vDSP DFT) shim. See ios/fftw_shim/include/fftw3.h.
//
// SDR++ uses interleaved complex (fftwf_complex = float[2]); vDSP wants
// split complex (real and imaginary in separate float arrays). Each
// fftwf_execute therefore does:
//
//     ctoz(in)  -> internal split-complex buffer
//     vDSP_DFT_Execute(setup, ...)
//     ztoc(...) -> out (interleaved)
//
// The ctoz/ztoc passes are nearly free on arm64 (one NEON permute per pair),
// so the all-in cost is dominated by the DFT itself, which on Apple silicon
// goes through the optimized vDSP kernels.
//
// vDSP_DFT_zop_CreateSetup accepts lengths f * 2^n where f ∈ {1, 3, 5, 15}.
// SDR++'s FFT sizes are powers of two (typically 65536), well within range.

#include <fftw3.h>
#include <Accelerate/Accelerate.h>
#include <stdlib.h>

struct fftwf_plan_s {
    vDSP_DFT_Setup setup;     // owned; freed in destroy
    vDSP_Length    n;         // FFT length
    fftwf_complex* in;        // caller-owned source buffer
    fftwf_complex* out;       // caller-owned destination buffer
    float*         splitReIn; // owned: ctoz(in) lands here
    float*         splitImIn;
    float*         splitReOut;// owned: vDSP DFT output, fed to ztoc
    float*         splitImOut;
};

extern "C" {

void* fftwf_malloc(size_t bytes) {
    void* p = NULL;
    // 16-byte alignment matches FFTW's default and is sufficient for NEON.
    if (posix_memalign(&p, 16, bytes) != 0) return NULL;
    return p;
}

void fftwf_free(void* ptr) { free(ptr); }

fftwf_plan fftwf_plan_dft_1d(int n,
                             fftwf_complex* in,
                             fftwf_complex* out,
                             int sign,
                             unsigned /*flags*/) {
    if (n <= 0 || !in || !out) return NULL;

    auto* plan = (fftwf_plan_s*)calloc(1, sizeof(fftwf_plan_s));
    if (!plan) return NULL;
    plan->n   = (vDSP_Length)n;
    plan->in  = in;
    plan->out = out;

    vDSP_DFT_Direction dir =
        (sign == FFTW_FORWARD) ? vDSP_DFT_FORWARD : vDSP_DFT_INVERSE;
    plan->setup = vDSP_DFT_zop_CreateSetup(NULL, plan->n, dir);
    if (!plan->setup) { free(plan); return NULL; }

    // Internal split-complex scratch. We could share one input/output pair
    // and let vDSP do in-place, but the API is clearer (and slightly safer
    // against user misuse) with separate buffers.
    plan->splitReIn  = (float*)fftwf_malloc(n * sizeof(float));
    plan->splitImIn  = (float*)fftwf_malloc(n * sizeof(float));
    plan->splitReOut = (float*)fftwf_malloc(n * sizeof(float));
    plan->splitImOut = (float*)fftwf_malloc(n * sizeof(float));
    if (!plan->splitReIn || !plan->splitImIn ||
        !plan->splitReOut || !plan->splitImOut) {
        fftwf_destroy_plan(plan);
        return NULL;
    }
    return plan;
}

void fftwf_execute(const fftwf_plan plan) {
    if (!plan) return;

    DSPSplitComplex inSplit  = { plan->splitReIn,  plan->splitImIn  };
    DSPSplitComplex outSplit = { plan->splitReOut, plan->splitImOut };

    // Interleaved (DSPComplex*) -> split. Stride 2 in the source because we
    // give vDSP the float[2] array reinterpreted as floats; the function
    // itself reads pairs.
    vDSP_ctoz((const DSPComplex*)plan->in, 2, &inSplit, 1, plan->n);

    vDSP_DFT_Execute(plan->setup,
                     inSplit.realp, inSplit.imagp,
                     outSplit.realp, outSplit.imagp);

    vDSP_ztoc(&outSplit, 1, (DSPComplex*)plan->out, 2, plan->n);
}

void fftwf_destroy_plan(fftwf_plan plan) {
    if (!plan) return;
    if (plan->setup) vDSP_DFT_DestroySetup(plan->setup);
    fftwf_free(plan->splitReIn);
    fftwf_free(plan->splitImIn);
    fftwf_free(plan->splitReOut);
    fftwf_free(plan->splitImOut);
    free(plan);
}

} // extern "C"
