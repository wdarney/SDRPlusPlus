#include "license.txt"  
/*
The ADC input real stream of 16 bit samples (at Fs = 64 Msps in the example) is converted to:
- 32 Msps float Fs/2 complex stream, or
- 16 Msps float Fs/2 complex stream, or
-  8 Msps float Fs/2 complex stream, or
-  4 Msps float Fs/2 complex stream, or
-  2 Msps float Fs/2 complex stream.
The decimation factor is selectable from HDSDR GUI sampling rate selector

The name r2iq as Real 2 I+Q stream

*/

#include "fft_mt_r2iq.h"
#include "config.h"
#include "fftw3.h"
#include "RadioHandler.h"
#include "thread_names.h"

#include "fir.h"

#include <assert.h>
#include <chrono>
#include <cstdio>
#include <utility>

#if defined(__ANDROID__)
#define RX888_FFTW_PLAN_FLAGS FFTW_ESTIMATE
#else
#define RX888_FFTW_PLAN_FLAGS FFTW_MEASURE
#endif


r2iqControlClass::r2iqControlClass()
{
	r2iqOn = false;
	randADC = false;
	sideband = false;
	mdecimation = 0;
	mratio[0] = 1;  // 1,2,4,8,16
	for (int i = 1; i < NDECIDX; i++)
	{
		mratio[i] = mratio[i - 1] * 2;
	}
}

fft_mt_r2iq::fft_mt_r2iq() :
	r2iqControlClass(),
	filterHw(nullptr),
	processor_count(0),
	allocated_thread_count(0)
#if defined(__ANDROID__)
	,
	requested_android_worker_count(3),
	android_worker_count(1),
	android_next_k(0),
	android_work_seq(0),
	android_completed_seq(0),
	android_stop_workers(false),
	android_adc_in_time(nullptr),
	android_pout(nullptr),
	android_mfft(0),
	android_mtunebin(0),
	android_decimate(0),
	android_filter(nullptr),
	android_filter2(nullptr),
	android_lsb(false),
	timing_chunks(0),
	timing_forward_ns(0),
	timing_shift_ns(0),
	timing_inverse_ns(0),
	timing_copy_ns(0),
	timing_sync_ns(0)
#endif
{
	mtunebin = halfFft / 4;
	mfftdim[0] = halfFft;
	for (int i = 1; i < NDECIDX; i++)
	{
		mfftdim[i] = mfftdim[i - 1] / 2;
	}
	GainScale = 0.0f;

#ifndef NDEBUG
	int mratio = 1;  // 1,2,4,8,16,..
	const float Astop = 120.0f;
	const float relPass = 0.85f;  // 85% of Nyquist should be usable
	const float relStop = 1.1f;   // 'some' alias back into transition band is OK
	printf("\n***************************************************************************\n");
	printf("Filter tap estimation, Astop = %.1f dB, relPass = %.2f, relStop = %.2f\n", Astop, relPass, relStop);
	for (int d = 0; d < NDECIDX; d++)
	{
		float Bw = 64.0f / mratio;
		int ntaps = KaiserWindow(0, Astop, relPass * Bw / 128.0f, relStop * Bw / 128.0f, nullptr);
		printf("decimation %2d: KaiserWindow(Astop = %.1f dB, Fpass = %.3f,Fstop = %.3f, Bw %.3f @ %f ) => %d taps\n",
			d, Astop, relPass * Bw, relStop * Bw, Bw, 128.0f, ntaps);
		mratio = mratio * 2;
	}
	printf("***************************************************************************\n");
#endif

}

fft_mt_r2iq::~fft_mt_r2iq()
{
	if (filterHw == nullptr)
		return;

	fftwf_export_wisdom_to_filename("wisdom");

	for (int d = 0; d < NDECIDX; d++)
	{
		fftwf_free(filterHw[d]);     // 4096
	}
	fftwf_free(filterHw);

	fftwf_destroy_plan(plan_t2f_r2c);
	for (int d = 0; d < NDECIDX; d++)
	{
		fftwf_destroy_plan(plans_f2t_c2c[d]);
	}

	for (unsigned t = 0; t < allocated_thread_count; t++) {
		auto th = threadArgs[t];
		fftwf_destroy_plan(th->plan_t2f_r2c);
		for (int d = 0; d < NDECIDX; d++)
		{
			fftwf_destroy_plan(th->plans_f2t_c2c[d]);
		}
		fftwf_free(th->ADCinTime);
		fftwf_free(th->ADCinFreq);
		fftwf_free(th->inFreqTmp);

		delete threadArgs[t];
	}
}


float fft_mt_r2iq::setFreqOffset(float offset)
{
	// align to 1/4 of halfft
	this->mtunebin = int(offset * halfFft / 4) * 4;  // mtunebin step 4 bin  ?
	float delta = ((float)this->mtunebin  / halfFft) - offset;
	float ret = delta * getRatio(); // ret increases with higher decimation
	DbgPrintf("offset %f mtunebin %d delta %f (%f)\n", offset, this->mtunebin, delta, ret);
	return ret;
}

void fft_mt_r2iq::TurnOn() {
	this->r2iqOn = true;
	this->bufIdx = 0;
	this->lastThread = threadArgs[0];

	inputbuffer->Start();
	outputbuffer->Start();

	for (unsigned t = 0; t < processor_count; t++) {
		r2iq_thread[t] = std::thread(
			[this, t] (void* arg) {
				char name[16];
				std::snprintf(name, sizeof(name), "rx888-r2iq-%u", t);
				rx888_set_thread_name(name);
				return this->r2iqThreadf((r2iqThreadArg*)arg);
			}, (void*)threadArgs[t]);
	}
}

void fft_mt_r2iq::TurnOff(void) {
	this->r2iqOn = false;

	inputbuffer->Stop();
	outputbuffer->Stop();
	for (unsigned t = 0; t < processor_count; t++) {
		r2iq_thread[t].join();
	}
}

bool fft_mt_r2iq::IsOn(void) { return(this->r2iqOn); }

void fft_mt_r2iq::setWorkerCount(int workers)
{
#if defined(__ANDROID__)
	if (workers < 1)
		workers = 1;
	if (workers > N_MAX_R2IQ_THREADS)
		workers = N_MAX_R2IQ_THREADS;
	requested_android_worker_count = (uint32_t)workers;
#else
	(void)workers;
#endif
}

#if defined(__ANDROID__)
void fft_mt_r2iq::processFftChunk(r2iqThreadArg* th, const float* adcInTime, int k, int mfft, int mtunebin, const fftwf_complex* filter, const fftwf_complex* filter2, bool lsb, fftwf_complex* pout, int decimate)
{
	auto t0 = std::chrono::steady_clock::now();
	fftwf_execute_dft_r2c(th->plan_t2f_r2c, const_cast<float*>(adcInTime) + (3 * halfFft / 2) * k, th->ADCinFreq);
	auto t1 = std::chrono::steady_clock::now();

	const auto count = std::min(mfft / 2, halfFft - mtunebin);
	const auto source = &th->ADCinFreq[mtunebin];
	const auto start = std::max(0, mfft / 2 - mtunebin);
	const auto source2 = &th->ADCinFreq[mtunebin - mfft / 2];
	const auto dest = &th->inFreqTmp[mfft / 2];

	shift_freq(th->inFreqTmp, source, filter, 0, count);
	if (mfft / 2 != count)
		memset(th->inFreqTmp[count], 0, sizeof(float) * 2 * (mfft / 2 - count));

	shift_freq(dest, source2, filter2, start, mfft / 2);
	if (start != 0)
		memset(th->inFreqTmp[mfft / 2], 0, sizeof(float) * 2 * start);
	auto t2 = std::chrono::steady_clock::now();

	fftwf_execute_dft(th->plans_f2t_c2c[decimate], th->inFreqTmp, th->inFreqTmp);
	auto t3 = std::chrono::steady_clock::now();

	if (lsb)
	{
		if (k == 0)
			copy<true>(pout, &th->inFreqTmp[mfft / 4], mfft / 2);
		else
			copy<true>(pout + mfft / 2 + (3 * mfft / 4) * (k - 1), &th->inFreqTmp[0], (3 * mfft / 4));
	}
	else
	{
		if (k == 0)
			copy<false>(pout, &th->inFreqTmp[mfft / 4], mfft / 2);
		else
			copy<false>(pout + mfft / 2 + (3 * mfft / 4) * (k - 1), &th->inFreqTmp[0], (3 * mfft / 4));
	}
	auto t4 = std::chrono::steady_clock::now();
	timing_forward_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count(), std::memory_order_relaxed);
	timing_shift_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count(), std::memory_order_relaxed);
	timing_inverse_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count(), std::memory_order_relaxed);
	timing_copy_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(t4 - t3).count(), std::memory_order_relaxed);
	timing_chunks.fetch_add(1, std::memory_order_relaxed);
}

void fft_mt_r2iq::androidWorkerLoop(unsigned workerIdx)
{
	char name[16];
	std::snprintf(name, sizeof(name), "rx888-r2iq-%u", workerIdx);
	rx888_set_thread_name(name);

	uint64_t seenSeq = 0;
	while (true)
	{
		{
			std::unique_lock<std::mutex> lk(android_work_mutex);
			android_work_cv.wait(lk, [this, seenSeq] {
				return android_stop_workers || android_work_seq != seenSeq;
			});
			if (android_stop_workers)
				return;
			seenSeq = android_work_seq;
		}

		while (true)
		{
			int k = android_next_k.fetch_add(1);
			if (k >= fftPerBuf)
				break;
			processFftChunk(threadArgs[workerIdx], android_adc_in_time, k, android_mfft, android_mtunebin, android_filter, android_filter2, android_lsb, android_pout, android_decimate);
		}

		{
			std::lock_guard<std::mutex> lk(android_work_mutex);
			android_completed_seq++;
			android_done_cv.notify_one();
		}
	}
}

void fft_mt_r2iq::androidRunBlock(fftwf_complex* pout, int mfft, int mtunebin, const fftwf_complex* filter, const fftwf_complex* filter2, bool lsb, int decimate)
{
	if (android_worker_count <= 1)
	{
		for (int k = 0; k < fftPerBuf; k++)
			processFftChunk(threadArgs[0], threadArgs[0]->ADCinTime, k, mfft, mtunebin, filter, filter2, lsb, pout, decimate);
		return;
	}

	{
		std::lock_guard<std::mutex> lk(android_work_mutex);
		android_adc_in_time = threadArgs[0]->ADCinTime;
		android_pout = pout;
		android_mfft = mfft;
		android_mtunebin = mtunebin;
		android_filter = filter;
		android_filter2 = filter2;
		android_lsb = lsb;
		android_decimate = decimate;
		android_next_k = 0;
		android_completed_seq = 0;
		android_work_seq++;
	}
	android_work_cv.notify_all();

	while (true)
	{
		int k = android_next_k.fetch_add(1);
		if (k >= fftPerBuf)
			break;
		processFftChunk(threadArgs[0], threadArgs[0]->ADCinTime, k, mfft, mtunebin, filter, filter2, lsb, pout, decimate);
	}

	auto waitStart = std::chrono::steady_clock::now();
	std::unique_lock<std::mutex> lk(android_work_mutex);
	android_done_cv.wait(lk, [this] {
		return android_completed_seq >= android_worker_count - 1;
	});
	auto waitEnd = std::chrono::steady_clock::now();
	timing_sync_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(waitEnd - waitStart).count(), std::memory_order_relaxed);
}

R2iqTimingSnapshot fft_mt_r2iq::getTimingSnapshot() const
{
	return {
		timing_chunks.load(std::memory_order_relaxed),
		timing_forward_ns.load(std::memory_order_relaxed),
		timing_shift_ns.load(std::memory_order_relaxed),
		timing_inverse_ns.load(std::memory_order_relaxed),
		timing_copy_ns.load(std::memory_order_relaxed),
		timing_sync_ns.load(std::memory_order_relaxed)
	};
}

void fft_mt_r2iq::resetTiming()
{
	timing_chunks.store(0, std::memory_order_relaxed);
	timing_forward_ns.store(0, std::memory_order_relaxed);
	timing_shift_ns.store(0, std::memory_order_relaxed);
	timing_inverse_ns.store(0, std::memory_order_relaxed);
	timing_copy_ns.store(0, std::memory_order_relaxed);
	timing_sync_ns.store(0, std::memory_order_relaxed);
}

void* fft_mt_r2iq::r2iqThreadf_android(r2iqThreadArg* th)
{
	rx888_set_thread_name("rx888-r2iq-0");

	android_stop_workers = false;
	android_workers.clear();
	for (unsigned t = 1; t < android_worker_count; t++)
	{
		android_workers.emplace_back([this, t] {
			androidWorkerLoop(t);
		});
	}

	const int decimate = this->mdecimation;
	const int mfft = this->mfftdim[decimate];
	const fftwf_complex* filter = filterHw[decimate];
	const bool lsb = this->getSideband();
	const auto filter2 = &filter[halfFft - mfft / 2];

	fftwf_complex* pout = nullptr;
	int decimate_count = 0;

	while (r2iqOn)
	{
		const int16_t* dataADC = inputbuffer->getReadPtr();
		if (!r2iqOn)
			break;

		this->bufIdx = (this->bufIdx + 1) % QUEUE_SIZE;
		const int16_t* endloop = inputbuffer->peekReadPtr(-1) + transferSamples - halfFft;
		const int mtunebin = this->mtunebin;

		auto inloop = th->ADCinTime;
		if (!this->getRand())
		{
			convert_float<false>(endloop, inloop, halfFft);
			convert_float<false>(dataADC, inloop + halfFft, transferSamples);
		}
		else
		{
			convert_float<true>(endloop, inloop, halfFft);
			convert_float<true>(dataADC, inloop + halfFft, transferSamples);
		}
		inputbuffer->ReadDone();

		if (decimate_count == 0)
			pout = (fftwf_complex*)outputbuffer->getWritePtr();

		decimate_count = (decimate_count + 1) & ((1 << decimate) - 1);
		androidRunBlock(pout, mfft, mtunebin, filter, filter2, lsb, decimate);

		if (decimate_count == 0)
		{
			outputbuffer->WriteDone();
			pout = nullptr;
		}
		else
		{
			pout += mfft / 2 + (3 * mfft / 4) * (fftPerBuf - 1);
		}
	}

	{
		std::lock_guard<std::mutex> lk(android_work_mutex);
		android_stop_workers = true;
		android_work_seq++;
	}
	android_work_cv.notify_all();
	for (auto& worker : android_workers)
	{
		if (worker.joinable())
			worker.join();
	}
	android_workers.clear();
	return 0;
}
#endif

void fft_mt_r2iq::Init(float gain, ringbuffer<int16_t> *input, ringbuffer<float>* obuffers)
{
	this->inputbuffer = input;    // set to the global exported by main_loop
	this->outputbuffer = obuffers;  // set to the global exported by main_loop

	this->GainScale = gain;

	fftwf_import_wisdom_from_filename("wisdom");

	// Get the processor count
	processor_count = std::thread::hardware_concurrency() - 1;
	if (processor_count == 0)
		processor_count = 1;
	if (processor_count > N_MAX_R2IQ_THREADS)
		processor_count = N_MAX_R2IQ_THREADS;
	allocated_thread_count = processor_count;
#if defined(__ANDROID__)
	unsigned androidMaxWorkers = std::thread::hardware_concurrency();
	if (androidMaxWorkers > 2)
		androidMaxWorkers -= 2;
	else
		androidMaxWorkers = 1;
	if (androidMaxWorkers > 3)
		androidMaxWorkers = 3;
	android_worker_count = std::min<uint32_t>((uint32_t)requested_android_worker_count, androidMaxWorkers);
	if (android_worker_count < 1)
		android_worker_count = 1;
	processor_count = 1;
	allocated_thread_count = android_worker_count;
#endif

	{
		fftwf_plan filterplan_t2f_c2c; // time to frequency fft

		DbgPrintf("r2iqCntrl initialization\n");


		DbgPrintf("RandTable generated\n");

		   // filters
		fftwf_complex *pfilterht;       // time filter ht
		pfilterht = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex)*halfFft);     // halfFft
		filterHw = (fftwf_complex**)fftwf_malloc(sizeof(fftwf_complex*)*NDECIDX);
		for (int d = 0; d < NDECIDX; d++)
		{
			filterHw[d] = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex)*halfFft);     // halfFft
		}

		filterplan_t2f_c2c = fftwf_plan_dft_1d(halfFft, pfilterht, filterHw[0], FFTW_FORWARD, RX888_FFTW_PLAN_FLAGS);
		float *pht = new float[halfFft / 4 + 1];
		const float Astop = 120.0f;
		const float relPass = 0.85f;  // 85% of Nyquist should be usable
		const float relStop = 1.1f;   // 'some' alias back into transition band is OK
		for (int d = 0; d < NDECIDX; d++)	// @todo when increasing NDECIDX
		{
			// @todo: have dynamic bandpass filter size - depending on decimation
			//   to allow same stopband-attenuation for all decimations
			float Bw = 64.0f / mratio[d];
			// Bw *= 0.8f;  // easily visualize Kaiser filter's response
			KaiserWindow(halfFft / 4 + 1, Astop, relPass * Bw / 128.0f, relStop * Bw / 128.0f, pht);

			float gainadj = gain * 2048.0f / (float)FFTN_R_ADC; // reference is FFTN_R_ADC == 2048

			for (int t = 0; t < halfFft; t++)
			{
				pfilterht[t][0] = pfilterht[t][1]= 0.0F;
			}
		
			for (int t = 0; t < (halfFft/4+1); t++)
			{
				pfilterht[halfFft-1-t][0] = gainadj * pht[t];
			}

			fftwf_execute_dft(filterplan_t2f_c2c, pfilterht, filterHw[d]);
		}
		delete[] pht;
		fftwf_destroy_plan(filterplan_t2f_c2c);
		fftwf_free(pfilterht);

		for (unsigned t = 0; t < allocated_thread_count; t++) {
			r2iqThreadArg *th = new r2iqThreadArg();
			threadArgs[t] = th;

			th->ADCinTime = (float*)fftwf_malloc(sizeof(float) * (halfFft + transferSize / 2));                 // 2048

			th->ADCinFreq = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex)*(halfFft + 1)); // 1024+1
			th->inFreqTmp = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex)*(halfFft));    // 1024
			th->plan_t2f_r2c = fftwf_plan_dft_r2c_1d(2 * halfFft, th->ADCinTime, th->ADCinFreq, RX888_FFTW_PLAN_FLAGS);
			for (int d = 0; d < NDECIDX; d++)
			{
				th->plans_f2t_c2c[d] = fftwf_plan_dft_1d(mfftdim[d], th->inFreqTmp, th->inFreqTmp, FFTW_BACKWARD, RX888_FFTW_PLAN_FLAGS);
			}
		}

		plan_t2f_r2c = fftwf_plan_dft_r2c_1d(2 * halfFft, threadArgs[0]->ADCinTime, threadArgs[0]->ADCinFreq, RX888_FFTW_PLAN_FLAGS);
		for (int d = 0; d < NDECIDX; d++)
		{
			plans_f2t_c2c[d] = fftwf_plan_dft_1d(mfftdim[d], threadArgs[0]->inFreqTmp, threadArgs[0]->inFreqTmp, FFTW_BACKWARD, RX888_FFTW_PLAN_FLAGS);
		}
	}
}

#ifndef NO_SIMD_OPTIM
#ifdef _WIN32
	//  Windows, assumed MSVC
	#include <intrin.h>
	#define cpuid(info, x)    __cpuidex(info, x, 0)
	#define DETECT_AVX
#elif defined(__x86_64__)
	//  GCC Intrinsics, x86 only
	#include <cpuid.h>
	#define cpuid(info, x)  __cpuid_count(x, 0, info[0], info[1], info[2], info[3])
	#define DETECT_AVX
#elif defined(__arm__) || defined(__aarch64__)
	#define DETECT_NEON
	#if defined(__linux__)
	#include <sys/auxv.h>
	#include <asm/hwcap.h>
	static bool detect_neon()
	{
#if defined(__aarch64__)
		return true;
#else
		unsigned long caps = getauxval(AT_HWCAP);
		return (caps & HWCAP_NEON);
#endif
	}
    #elif defined(__APPLE__)
        #include <sys/sysctl.h>
        static bool detect_neon()
        {
            int hasNeon = 0;
            size_t len = sizeof(hasNeon);
            sysctlbyname("hw.optional.neon", &hasNeon, &len, NULL, 0);
            return hasNeon;
        }
    #endif
#else
#error Compiler does not identify an x86 or ARM core..
#endif
#endif

void * fft_mt_r2iq::r2iqThreadf(r2iqThreadArg *th)
{
#if defined(__ANDROID__)
	return r2iqThreadf_android(th);
#endif
#ifdef NO_SIMD_OPTIM
	DbgPrintf("Hardware Capability: all SIMD features (AVX, AVX2, AVX512) deactivated\n");
	return r2iqThreadf_def(th);
#else
#if defined(DETECT_AVX)
	int info[4];
	bool HW_AVX = false;
	bool HW_AVX2 = false;
	bool HW_AVX512F = false;

	cpuid(info, 0);
	int nIds = info[0];

	if (nIds >= 0x00000001){
		cpuid(info,0x00000001);
		HW_AVX    = (info[2] & ((int)1 << 28)) != 0;
	}
	if (nIds >= 0x00000007){
		cpuid(info,0x00000007);
		HW_AVX2   = (info[1] & ((int)1 <<  5)) != 0;

		HW_AVX512F     = (info[1] & ((int)1 << 16)) != 0;
	}

	DbgPrintf("Hardware Capability: AVX:%d AVX2:%d AVX512:%d\n", HW_AVX, HW_AVX2, HW_AVX512F);

	if (HW_AVX512F)
		return r2iqThreadf_avx512(th);
	else if (HW_AVX2)
		return r2iqThreadf_avx2(th);
	else if (HW_AVX)
		return r2iqThreadf_avx(th);
	else
		return r2iqThreadf_def(th);
#elif defined(DETECT_NEON)
	bool NEON = detect_neon();
	DbgPrintf("Hardware Capability: NEON:%d\n", NEON);
	if (NEON)
		return r2iqThreadf_neon(th);
	else
		return r2iqThreadf_def(th);
#endif
#endif
}
