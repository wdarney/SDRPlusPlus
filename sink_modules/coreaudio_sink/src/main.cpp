//
// CoreAudio sink (iOS/macOS). Drives the default system output via AudioUnit
// RemoteIO. iOS exposes no per-device picker — the OS routes audio to whatever
// the user picked (speaker, headphones, AirPlay, Bluetooth). The UI is
// therefore much simpler than the desktop audio_sink: just a sample-rate combo.
//

#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <dsp/buffer/packer.h>
#include <dsp/convert/stereo_to_mono.h>
#include <utils/flog.h>
#include <config.h>
#include <core.h>

#include <AudioToolbox/AudioToolbox.h>

SDRPP_MOD_INFO{
    /* Name:            */ "coreaudio_sink",
    /* Description:     */ "CoreAudio sink (iOS/macOS)",
    /* Author:          */ "SDR++",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

static ConfigManager config;

static const unsigned int CA_SAMPLE_RATES[] = { 22050, 32000, 44100, 48000 };
static const int          CA_SR_COUNT       = sizeof(CA_SAMPLE_RATES) / sizeof(CA_SAMPLE_RATES[0]);

class CoreAudioSink : SinkManager::Sink {
public:
    CoreAudioSink(SinkManager::Stream* stream, std::string streamName)
        : _stream(stream), _streamName(streamName) {
        s2m.init(_stream->sinkOut);
        stereoPacker.init(_stream->sinkOut, 512);

        bool created = false;
        config.acquire();
        if (!config.conf.contains(_streamName)) {
            created = true;
            config.conf[_streamName]["sampleRate"] = 48000;
        }
        sampleRate = config.conf[_streamName]["sampleRate"];
        config.release(created);

        srIdx = 3;
        for (int i = 0; i < CA_SR_COUNT; i++) {
            if (CA_SAMPLE_RATES[i] == sampleRate) { srIdx = i; break; }
        }
        for (int i = 0; i < CA_SR_COUNT; i++) {
            srTxt += std::to_string(CA_SAMPLE_RATES[i]);
            srTxt += '\0';
        }

        _stream->setSampleRate(sampleRate);
    }

    ~CoreAudioSink() { stop(); }

    void start()       override { if (!running) { running = doStart(); } }
    void stop()        override { if (running)  { doStop(); running = false; } }
    void menuHandler() override {
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(w);
        if (ImGui::Combo(("##coreaudio_sr_" + _streamName).c_str(), &srIdx, srTxt.c_str())) {
            sampleRate = CA_SAMPLE_RATES[srIdx];
            _stream->setSampleRate(sampleRate);
            if (running) { doStop(); doStart(); }
            config.acquire();
            config.conf[_streamName]["sampleRate"] = sampleRate;
            config.release(true);
        }
    }

private:
    bool doStart() {
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
        if (AudioComponentInstanceNew(comp, &au) != noErr) { flog::error("coreaudio_sink: instantiate failed"); return false; }

        AudioStreamBasicDescription asbd = {};
        asbd.mSampleRate       = sampleRate;
        asbd.mFormatID         = kAudioFormatLinearPCM;
        asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        asbd.mChannelsPerFrame = 2;
        asbd.mBitsPerChannel   = 32;
        asbd.mFramesPerPacket  = 1;
        asbd.mBytesPerFrame    = 8;
        asbd.mBytesPerPacket   = 8;

        if (AudioUnitSetProperty(au, kAudioUnitProperty_StreamFormat,
                                 kAudioUnitScope_Input, 0, &asbd, sizeof(asbd)) != noErr) {
            flog::error("coreaudio_sink: SetProperty StreamFormat failed");
            return false;
        }

        AURenderCallbackStruct cb = { &CoreAudioSink::renderCB, this };
        AudioUnitSetProperty(au, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0, &cb, sizeof(cb));

        stereoPacker.setSampleCount(sampleRate / 60);
        stereoPacker.start();

        if (AudioUnitInitialize(au) != noErr) { flog::error("coreaudio_sink: init failed"); return false; }
        if (AudioOutputUnitStart(au)  != noErr) { flog::error("coreaudio_sink: start failed"); return false; }

        flog::info("coreaudio_sink: stream open @ {}Hz", (int)sampleRate);
        return true;
    }

    void doStop() {
        if (!au) return;
        AudioOutputUnitStop(au);
        AudioUnitUninitialize(au);
        AudioComponentInstanceDispose(au);
        au = NULL;
        s2m.stop();
        stereoPacker.stop();
        stereoPacker.out.stopReader();
        stereoPacker.out.clearReadStop();
    }

    static OSStatus renderCB(void* userData,
                             AudioUnitRenderActionFlags* /*flags*/,
                             const AudioTimeStamp* /*ts*/,
                             UInt32 /*busNumber*/,
                             UInt32 nFrames,
                             AudioBufferList* ioData) {
        auto* self = (CoreAudioSink*)userData;
        int got = self->stereoPacker.out.read();
        if (got <= 0) {
            for (UInt32 b = 0; b < ioData->mNumberBuffers; b++) {
                memset(ioData->mBuffers[b].mData, 0, ioData->mBuffers[b].mDataByteSize);
            }
            return noErr;
        }
        size_t bytes = nFrames * sizeof(dsp::stereo_t);
        if (bytes > ioData->mBuffers[0].mDataByteSize) bytes = ioData->mBuffers[0].mDataByteSize;
        memcpy(ioData->mBuffers[0].mData, self->stereoPacker.out.readBuf, bytes);
        self->stereoPacker.out.flush();
        return noErr;
    }

    SinkManager::Stream*               _stream;
    dsp::convert::StereoToMono         s2m;
    dsp::buffer::Packer<dsp::stereo_t> stereoPacker;
    std::string _streamName;

    AudioUnit au = NULL;
    bool      running    = false;
    unsigned  sampleRate = 48000;
    int       srIdx      = 3;
    std::string srTxt;
};

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
    static SinkManager::Sink* create_sink(SinkManager::Stream* stream, std::string streamName, void* /*ctx*/) {
        return (SinkManager::Sink*)(new CoreAudioSink(stream, streamName));
    }

    std::string                 name;
    bool                        enabled = true;
    SinkManager::SinkProvider   provider;
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
