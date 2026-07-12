//
// CoreAudio sink (iOS/macOS). Drives the default system output via AudioUnit
// RemoteIO (iOS) / DefaultOutput (macOS).
//
// Key design:
//   • AVAudioSession configured for .playback before the audio unit starts
//     (required on iOS for audio to route to speaker/headphones).
//   • Lock-free SPSC ring buffer between the DSP chain and the real-time
//     CoreAudio render callback. The render callback NEVER blocks: if the
//     ring is empty it outputs silence. A producer thread reads from the
//     SDR++ audio stream and fills the ring.
//

#import <Foundation/Foundation.h>
#if TARGET_OS_IPHONE
#import <AVFoundation/AVFoundation.h>
#endif

#include <AudioToolbox/AudioToolbox.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstring>

#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <utils/flog.h>
#include <config.h>
#include <core.h>

SDRPP_MOD_INFO{
    /* Name:            */ "coreaudio_sink",
    /* Description:     */ "CoreAudio sink (iOS/macOS)",
    /* Author:          */ "SDR++",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

static ConfigManager config;

static const unsigned int CA_SAMPLE_RATES[] = { 22050, 32000, 44100, 48000 };
static const int          CA_SR_COUNT       = (int)(sizeof(CA_SAMPLE_RATES) / sizeof(CA_SAMPLE_RATES[0]));

// ---------------------------------------------------------------------------
// Lock-free single-producer single-consumer ring buffer.
// Head is written by the producer thread; tail by the render callback.
// Buffer size is a power of 2 so modulo is a mask operation.
// ---------------------------------------------------------------------------
static const int RING_LOG2 = 15;                    // 32 768 stereo frames
static const int RING_SIZE = 1 << RING_LOG2;        // ≈ 680 ms at 48 kHz
static const int RING_MASK = RING_SIZE - 1;

class CoreAudioSink : SinkManager::Sink {
public:
    CoreAudioSink(SinkManager::Stream* stream, std::string streamName)
        : _stream(stream), _streamName(streamName)
    {
        _ring.resize(RING_SIZE);

        bool created = false;
        config.acquire();
        if (!config.conf.contains(_streamName)) {
            created = true;
            config.conf[_streamName]["sampleRate"] = 48000;
        }
        _sampleRate = config.conf[_streamName]["sampleRate"];
        config.release(created);

        _srIdx = CA_SR_COUNT - 1;
        for (int i = 0; i < CA_SR_COUNT; i++) {
            if (CA_SAMPLE_RATES[i] == _sampleRate) { _srIdx = i; break; }
        }
        for (int i = 0; i < CA_SR_COUNT; i++) {
            _srTxt += std::to_string(CA_SAMPLE_RATES[i]);
            _srTxt += '\0';
        }

        _stream->setSampleRate(_sampleRate);
    }

    ~CoreAudioSink() { stop(); }

    void start()       override { if (!_running) { _running = doStart(); } }
    void stop()        override { if (_running)  { doStop(); _running = false; } }
    void menuHandler() override {
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo(("##coreaudio_sr_" + _streamName).c_str(), &_srIdx, _srTxt.c_str())) {
            _sampleRate = CA_SAMPLE_RATES[_srIdx];
            _stream->setSampleRate(_sampleRate);
            if (_running) { doStop(); doStart(); }
            config.acquire();
            config.conf[_streamName]["sampleRate"] = _sampleRate;
            config.release(true);
        }
    }

private:
    // -----------------------------------------------------------------------
    // Ring buffer — written by producer thread, read by render callback.
    // -----------------------------------------------------------------------
    std::vector<dsp::stereo_t> _ring;
    std::atomic<uint32_t>      _ringHead{0};   // producer writes here
    std::atomic<uint32_t>      _ringTail{0};   // consumer reads here

    // Push up to `count` frames from `data` into the ring.
    // Drops the earliest data if the ring is full (shouldn't happen with a
    // 680 ms buffer but is safe to do rather than block).
    void ringPush(const dsp::stereo_t* data, int count) {
        uint32_t head = _ringHead.load(std::memory_order_relaxed);
        uint32_t tail = _ringTail.load(std::memory_order_acquire);
        int free = RING_SIZE - 1 - (int)((head - tail) & RING_MASK);
        if (count > free) count = free;
        for (int i = 0; i < count; i++)
            _ring[(head + i) & RING_MASK] = data[i];
        _ringHead.store(head + (uint32_t)count, std::memory_order_release);
    }

    // Pop exactly `count` frames into `out`. Returns false if the ring
    // doesn't have enough data; in that case `out` is zero-filled.
    bool ringPop(float* out, int count) {
        uint32_t head = _ringHead.load(std::memory_order_acquire);
        uint32_t tail = _ringTail.load(std::memory_order_relaxed);
        int avail = (int)((head - tail) & RING_MASK);
        if (avail < count) return false;
        for (int i = 0; i < count; i++) {
            const dsp::stereo_t& s = _ring[(tail + i) & RING_MASK];
            out[i * 2]     = s.l;
            out[i * 2 + 1] = s.r;
        }
        _ringTail.store(tail + (uint32_t)count, std::memory_order_release);
        return true;
    }

    // -----------------------------------------------------------------------
    // Producer thread — reads from the DSP stream (blocking is fine here)
    // and pushes samples to the ring buffer.
    // -----------------------------------------------------------------------
    std::thread           _producer;
    std::atomic<bool>     _producerRunning{false};

    void producerLoop() {
        while (_producerRunning.load(std::memory_order_relaxed)) {
            int got = _stream->sinkOut->read();
            if (got <= 0) break;
            ringPush(_stream->sinkOut->readBuf, got);
            _stream->sinkOut->flush();
        }
    }

    // -----------------------------------------------------------------------
    // CoreAudio render callback — must never block.
    // -----------------------------------------------------------------------
    static OSStatus renderCB(void*                       userData,
                              AudioUnitRenderActionFlags* /*flags*/,
                              const AudioTimeStamp*       /*ts*/,
                              UInt32                      /*bus*/,
                              UInt32                      nFrames,
                              AudioBufferList*            ioData) {
        auto* self = (CoreAudioSink*)userData;

        // Expect interleaved stereo float in buffer 0.
        auto* out = (float*)ioData->mBuffers[0].mData;
        UInt32 outBytes = ioData->mBuffers[0].mDataByteSize;

        if (!self->_producerRunning || !self->ringPop(out, (int)nFrames)) {
            memset(out, 0, outBytes);
        }
        return noErr;
    }

    // -----------------------------------------------------------------------
    // Start / stop
    // -----------------------------------------------------------------------
    bool doStart() {
#if TARGET_OS_IPHONE
        // AVAudioSessionCategoryPlayback is already set by ViewController at
        // launch time (main thread). Here we just ensure the session is active
        // and adjust the preferred sample rate. setActive:YES must be called on
        // the main thread for background-audio eligibility to be recognised by
        // iOS; dispatch_sync guarantees that even if doStart() is called from a
        // background DSP thread.
        double sr = (double)_sampleRate;
        dispatch_block_t activateBlock = ^{
            NSError* e = nil;
            AVAudioSession* sess = [AVAudioSession sharedInstance];
            if (![sess setPreferredSampleRate:sr error:&e]) {
                NSLog(@"[SDR++ coreaudio_sink] setPreferredSampleRate failed: %@",
                      e.localizedDescription);
            }
            if (![sess setActive:YES error:&e]) {
                NSLog(@"[SDR++ coreaudio_sink] setActive failed: %@",
                      e.localizedDescription);
            }
            NSLog(@"[SDR++ coreaudio_sink] AVAudioSession active, SR=%.0f Hz",
                  sess.sampleRate);
        };
        if ([NSThread isMainThread]) {
            activateBlock();
        } else {
            dispatch_sync(dispatch_get_main_queue(), activateBlock);
        }
#endif

        AudioComponentDescription desc = {};
        desc.componentType         = kAudioUnitType_Output;
#if TARGET_OS_IPHONE
        desc.componentSubType      = kAudioUnitSubType_RemoteIO;
#else
        desc.componentSubType      = kAudioUnitSubType_DefaultOutput;
#endif
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;

        AudioComponent comp = AudioComponentFindNext(NULL, &desc);
        if (!comp) { flog::error("coreaudio_sink: AudioComponentFindNext failed"); return false; }
        if (AudioComponentInstanceNew(comp, &_au) != noErr) {
            flog::error("coreaudio_sink: instantiate failed"); return false;
        }

#if TARGET_OS_IPHONE
        // Explicitly enable output element (bus 0 output scope).
        UInt32 flag = 1;
        AudioUnitSetProperty(_au, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Output, 0, &flag, sizeof(flag));
        // Disable input element (bus 1) — we don't record.
        flag = 0;
        AudioUnitSetProperty(_au, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Input, 1, &flag, sizeof(flag));
#endif

        // Stereo interleaved 32-bit float PCM.
        AudioStreamBasicDescription asbd = {};
        asbd.mSampleRate       = _sampleRate;
        asbd.mFormatID         = kAudioFormatLinearPCM;
        asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        asbd.mChannelsPerFrame = 2;
        asbd.mBitsPerChannel   = 32;
        asbd.mFramesPerPacket  = 1;
        asbd.mBytesPerFrame    = 8;
        asbd.mBytesPerPacket   = 8;

        OSStatus st = AudioUnitSetProperty(_au, kAudioUnitProperty_StreamFormat,
                                           kAudioUnitScope_Input, 0, &asbd, sizeof(asbd));
        if (st != noErr) {
            flog::error("coreaudio_sink: SetProperty StreamFormat failed: {}", (int)st);
            AudioComponentInstanceDispose(_au); _au = NULL;
            return false;
        }

        AURenderCallbackStruct cb = { &CoreAudioSink::renderCB, this };
        AudioUnitSetProperty(_au, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0, &cb, sizeof(cb));

        if (AudioUnitInitialize(_au) != noErr) {
            flog::error("coreaudio_sink: AudioUnitInitialize failed");
            AudioComponentInstanceDispose(_au); _au = NULL;
            return false;
        }

        // Start the producer thread before the audio unit so the first
        // render callback already has something in the ring buffer.
        _ringHead.store(0, std::memory_order_relaxed);
        _ringTail.store(0, std::memory_order_relaxed);
        _producerRunning.store(true, std::memory_order_release);
        _producer = std::thread(&CoreAudioSink::producerLoop, this);

        if (AudioOutputUnitStart(_au) != noErr) {
            flog::error("coreaudio_sink: AudioOutputUnitStart failed");
            _producerRunning.store(false, std::memory_order_release);
            _stream->sinkOut->stopReader();
            _producer.join();
            _stream->sinkOut->clearReadStop();
            AudioUnitUninitialize(_au);
            AudioComponentInstanceDispose(_au); _au = NULL;
            return false;
        }

        flog::info("coreaudio_sink: started @ {} Hz", (int)_sampleRate);
        return true;
    }

    void doStop() {
        if (!_au) return;

        AudioOutputUnitStop(_au);

        // Unblock the producer thread's blocking read and join it.
        _producerRunning.store(false, std::memory_order_release);
        _stream->sinkOut->stopReader();
        if (_producer.joinable()) _producer.join();
        _stream->sinkOut->clearReadStop();

        AudioUnitUninitialize(_au);
        AudioComponentInstanceDispose(_au);
        _au = NULL;

        flog::info("coreaudio_sink: stopped");
    }

    SinkManager::Stream* _stream;
    std::string          _streamName;
    AudioUnit            _au       = NULL;
    bool                 _running  = false;
    unsigned             _sampleRate = 48000;
    int                  _srIdx    = CA_SR_COUNT - 1;
    std::string          _srTxt;
};

// ---------------------------------------------------------------------------

class CoreAudioSinkModule : public ModuleManager::Instance {
public:
    CoreAudioSinkModule(std::string name) : name(name) {
        provider.create = create_sink;
        provider.ctx    = this;
        sigpath::sinkManager.registerSinkProvider("Audio", provider);
    }

    ~CoreAudioSinkModule() { sigpath::sinkManager.unregisterSinkProvider("Audio"); }

    void postInit() override {}
    void enable()   override { enabled = true; }
    void disable()  override { enabled = false; }
    bool isEnabled() override { return enabled; }

private:
    static SinkManager::Sink* create_sink(SinkManager::Stream* stream,
                                          std::string streamName, void* /*ctx*/) {
        return (SinkManager::Sink*)(new CoreAudioSink(stream, streamName));
    }

    std::string               name;
    bool                      enabled = true;
    SinkManager::SinkProvider provider;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/coreaudio_sink_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new CoreAudioSinkModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (CoreAudioSinkModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
