#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <dsp/buffer/packer.h>
#include <utils/flog.h>
#include <config.h>
#include <utils/optionlist.h>
#include <aaudio/AAudio.h>
#include <core.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <chrono>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "audio_sink",
    /* Description:     */ "Android audio sink module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class AudioSink : SinkManager::Sink {
public:
    AudioSink(SinkManager::Stream* stream, std::string streamName) {
        _stream = stream;
        _streamName = streamName;
        directWriteMode = (_streamName.find("_monitor") != std::string::npos);

        packer.init(_stream->sinkOut, 512);

        // TODO: Add choice? I don't think anyone cares on android...
        sampleRate = 48000;
        _stream->setSampleRate(sampleRate);
    }

    ~AudioSink() {
    }

    void start() {
        if (running) {
            return;
        }
        doStart();
        running = true;
    }

    void stop() {
        if (!running) {
            return;
        }
        doStop();
        running = false;
    }

    void menuHandler() {
        updateOutputState();

        char line[128];
        if (stream) { xrunCount = AAudioStream_getXRunCount(stream); }
        snprintf(line, sizeof(line), "Audio %s %d Hz, buffer %d, xruns %d",
                 directWriteMode ? "direct" : "callback", actualSampleRate, bufferSize, xrunCount);
        ImGui::Text("%s", line);
    }

private:
    void doStart() {
        // Create stream builder
        AAudioStreamBuilder *builder;
        aaudio_result_t result = AAudio_createStreamBuilder(&builder);

        // Set stream options
        int requestedRate = (int)sampleRate;
        int deviceBufferFrames = round(sampleRate / 10.0);
        bufferSize = round(sampleRate / 100.0);
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
        AAudioStreamBuilder_setSampleRate(builder, requestedRate);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setBufferCapacityInFrames(builder, deviceBufferFrames * 2);
        if (!directWriteMode) {
            AAudioStreamBuilder_setDataCallback(builder, dataCallback, this);
        }
        AAudioStreamBuilder_setErrorCallback(builder, errorCallback, this);
        
        // Open the stream
        result = AAudioStreamBuilder_openStream(builder, &stream);
        if (result != AAUDIO_OK || stream == NULL) {
            flog::error("Android audio sink: failed to open AAudio stream ({})", (int)result);
            AAudioStreamBuilder_delete(builder);
            return;
        }

        actualSampleRate = AAudioStream_getSampleRate(stream);
        if (actualSampleRate <= 0) { actualSampleRate = requestedRate; }
        sampleRate = actualSampleRate;
        maxQueuedFrames = actualSampleRate / 2;
        _stream->setSampleRate(sampleRate);

        int framesPerBurst = AAudioStream_getFramesPerBurst(stream);
        if (framesPerBurst > 0) {
            deviceBufferFrames = std::max(deviceBufferFrames, framesPerBurst * 12);
            bufferSize = std::max(bufferSize, framesPerBurst);
        }
        AAudioStream_setBufferSizeInFrames(stream, deviceBufferFrames);
        deviceBufferSize = AAudioStream_getBufferSizeInFrames(stream);
        if (deviceBufferSize <= 0) {
            deviceBufferSize = deviceBufferFrames;
        }
        packer.setSampleCount(bufferSize);
        xrunCount = 0;
        callbackUnderruns = 0;
        queuedFrames = 0;
        workerFrames = 0;
        directWrittenFrames = 0;
        directShortWrites = 0;
        directWriteErrors = 0;
        {
            std::lock_guard<std::mutex> lock(queueMtx);
            audioQueue.clear();
        }
        flog::info("Android audio sink ({0}): {1}, requested {2} Hz, actual {3} Hz, packer {4} frames, device buffer {5} frames",
                   _streamName, directWriteMode ? "direct" : "callback",
                   requestedRate, actualSampleRate, bufferSize, deviceBufferSize);

        packer.start();
        workerThread = std::thread(&AudioSink::worker, this);
        if (!directWriteMode) {
            // Start SDR++ audio packing before starting the device callback.
            prebufferFrames = std::max(actualSampleRate / 4, bufferSize * 4);
            auto prebufferStart = std::chrono::steady_clock::now();
            while (queuedFrames.load() < prebufferFrames &&
                   std::chrono::steady_clock::now() - prebufferStart < std::chrono::milliseconds(500)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        outputStarted = false;
        updateOutputState();

        // We no longer need the builder
        AAudioStreamBuilder_delete(builder);
    }

    void doStop() {
        if (outputStarted) {
            AAudioStream_requestStop(stream);
            outputStarted = false;
        }
        packer.stop();
        packer.out.stopReader();
        if (workerThread.joinable()) { workerThread.join(); }
        AAudioStream_close(stream);
        stream = nullptr;
        packer.out.clearReadStop();
        std::lock_guard<std::mutex> lock(queueMtx);
        audioQueue.clear();
        queuedFrames = 0;
    }

    void worker() {
        while (true) {
            int count = packer.out.read();
            if (count < 0) { return; }
            workerFrames += count;

            if (directWriteMode) {
                aaudio_result_t written = AAudioStream_write(stream, packer.out.readBuf, count, 100000000);
                if (written < 0) {
                    directWriteErrors++;
                }
                else {
                    directWrittenFrames += written;
                    if (written != count) {
                        directShortWrites++;
                    }
                }
                packer.out.flush();
                continue;
            }

            if (_stream->getMuted()) {
                {
                    std::lock_guard<std::mutex> lock(queueMtx);
                    audioQueue.clear();
                    queuedFrames = 0;
                }
                packer.out.flush();
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(queueMtx);
                for (int i = 0; i < count; i++) {
                    audioQueue.push_back(packer.out.readBuf[i]);
                }

                while ((int)audioQueue.size() > maxQueuedFrames) {
                    audioQueue.pop_front();
                }
                queuedFrames = (int)audioQueue.size();
            }

            packer.out.flush();
        }
    }

    static aaudio_data_callback_result_t dataCallback(AAudioStream*, void* userData, void* audioData, int32_t numFrames) {
        auto* _this = (AudioSink*)userData;
        auto* out = (dsp::stereo_t*)audioData;
        int copied = 0;

        {
            std::lock_guard<std::mutex> lock(_this->queueMtx);
            while (copied < numFrames && !_this->audioQueue.empty()) {
                out[copied++] = _this->audioQueue.front();
                _this->audioQueue.pop_front();
            }
            _this->queuedFrames = (int)_this->audioQueue.size();
        }

        if (copied < numFrames) {
            std::memset(out + copied, 0, (numFrames - copied) * sizeof(dsp::stereo_t));
            _this->callbackUnderruns++;
        }

        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    static void errorCallback(AAudioStream *stream, void *userData, aaudio_result_t error){
        // detect an audio device detached and restart the stream
        if (error == AAUDIO_ERROR_DISCONNECTED){
            std::thread thr(&AudioSink::restart, (AudioSink*)userData);
            thr.detach();
        }
    }

    void restart() {
        if (running) { doStop(); }
        if (running) { doStart(); }
    }

    void updateOutputState() {
        if (!stream) { return; }
        if (directWriteMode) {
            if (!outputStarted) {
                AAudioStream_requestStart(stream);
                outputStarted = true;
            }
            mutedSleep = false;
            return;
        }

        bool shouldSleep = _stream->getMuted();
        mutedSleep = shouldSleep;
        if (shouldSleep) {
            if (outputStarted) {
                AAudioStream_requestStop(stream);
                outputStarted = false;
            }
            return;
        }

        if (!outputStarted) {
            AAudioStream_requestStart(stream);
            outputStarted = true;
        }
    }

    std::thread workerThread;

    AAudioStream *stream = NULL;
    SinkManager::Stream* _stream;
    dsp::buffer::Packer<dsp::stereo_t> packer;
    std::mutex queueMtx;
    std::deque<dsp::stereo_t> audioQueue;

    std::string _streamName;
    double sampleRate;
    int actualSampleRate = 48000;
    int bufferSize;
    int deviceBufferSize = 0;
    int xrunCount = 0;
    int maxQueuedFrames = 24000;
    int prebufferFrames = 12000;
    std::atomic<int> queuedFrames{0};
    std::atomic<int> callbackUnderruns{0};
    std::atomic<uint64_t> workerFrames{0};
    std::atomic<uint64_t> directWrittenFrames{0};
    std::atomic<int> directShortWrites{0};
    std::atomic<int> directWriteErrors{0};
    std::atomic<bool> mutedSleep{false};

    bool directWriteMode = false;
    bool outputStarted = false;
    bool running = false;
};

class AudioSinkModule : public ModuleManager::Instance {
public:
    AudioSinkModule(std::string name) {
        this->name = name;
        provider.create = create_sink;
        provider.ctx = this;

        sigpath::sinkManager.registerSinkProvider("Audio", provider);
    }

    ~AudioSinkModule() {
        // Unregister sink, this will automatically stop and delete all instances of the audio sink
        sigpath::sinkManager.unregisterSinkProvider("Audio");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static SinkManager::Sink* create_sink(SinkManager::Stream* stream, std::string streamName, void* ctx) {
        return (SinkManager::Sink*)(new AudioSink(stream, streamName));
    }

    std::string name;
    bool enabled = true;
    SinkManager::SinkProvider provider;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/audio_sink_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT void* _CREATE_INSTANCE_(std::string name) {
    AudioSinkModule* instance = new AudioSinkModule(name);
    return instance;
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (AudioSinkModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
