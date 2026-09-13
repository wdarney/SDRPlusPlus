#pragma once

#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>

// Owns only Bluetooth-private descriptors. Neither the source WAV nor the
// existing recording encoder is changed by this conversion.
class CBMacAudioCopy {
public:
    std::atomic<bool> canceled{false};
    std::atomic<bool> done{false};
    int outputFD = -1;
    OSStatus error = noErr;

    explicit CBMacAudioCopy(int sourceFD) : inputFD(sourceFD), worker([this] {
        @autoreleasepool { error = convert(); }
        done.store(true);
    }) {}

    ~CBMacAudioCopy() {
        canceled.store(true);
        if (worker.joinable()) worker.join();
        if (inputFD >= 0) close(inputFD);
        if (outputFD >= 0) close(outputFD);
    }

private:
    int inputFD;
    std::thread worker;

    static OSStatus readAudio(void* context, SInt64 position, UInt32 count, void* buffer, UInt32* actual) {
        auto* self = static_cast<CBMacAudioCopy*>(context);
        ssize_t n;
        do { n = pread(self->inputFD, buffer, count, position); } while (n < 0 && errno == EINTR);
        *actual = n < 0 ? 0 : static_cast<UInt32>(n);
        return n < 0 ? kAudioFileUnspecifiedError : noErr;
    }

    static SInt64 audioSize(void* context) {
        struct stat info{};
        return fstat(static_cast<CBMacAudioCopy*>(context)->inputFD, &info) == 0 ? info.st_size : 0;
    }

    OSStatus convert() {
        struct Resources {
            AudioFileID file = nullptr;
            ExtAudioFileRef input = nullptr;
            ExtAudioFileRef output = nullptr;
            std::string path;
            ~Resources() {
                if (output) ExtAudioFileDispose(output);
                if (input) ExtAudioFileDispose(input);
                if (file) AudioFileClose(file);
                if (!path.empty()) unlink(path.c_str());
            }
        } resources;
        if (inputFD < 0 || canceled.load()) return kAudioFileUnspecifiedError;
        OSStatus result = AudioFileOpenWithCallbacks(this, readAudio, nullptr, audioSize, nullptr, 0, &resources.file);
        if (result != noErr) return result;
        result = ExtAudioFileWrapAudioFileID(resources.file, false, &resources.input);
        if (result != noErr) return result;

        NSString* pattern = [NSTemporaryDirectory() stringByAppendingPathComponent:@"sdrpp-ble-audio-XXXXXX"];
        std::string name = pattern.fileSystemRepresentation;
        std::vector<char> temporary(name.begin(), name.end());
        temporary.push_back('\0');
        int temporaryFD = mkstemp(temporary.data());
        if (temporaryFD < 0) return kAudioFileUnspecifiedError;
        close(temporaryFD);
        resources.path = temporary.data();
        NSURL* url = [NSURL fileURLWithFileSystemRepresentation:temporary.data() isDirectory:NO relativeToURL:nil];

        AudioStreamBasicDescription encoded{};
        encoded.mSampleRate = 24000;
        encoded.mFormatID = kAudioFormatMPEG4AAC;
        encoded.mChannelsPerFrame = 1;
        result = ExtAudioFileCreateWithURL((__bridge CFURLRef)url, kAudioFileM4AType,
            &encoded, nullptr, kAudioFileFlags_EraseFile, &resources.output);
        if (result != noErr) return result;

        AudioStreamBasicDescription pcm{};
        pcm.mSampleRate = 24000;
        pcm.mFormatID = kAudioFormatLinearPCM;
        pcm.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
        pcm.mChannelsPerFrame = 1;
        pcm.mBitsPerChannel = 32;
        pcm.mBytesPerFrame = pcm.mBytesPerPacket = sizeof(float);
        pcm.mFramesPerPacket = 1;
        result = ExtAudioFileSetProperty(resources.input, kExtAudioFileProperty_ClientDataFormat, sizeof(pcm), &pcm);
        if (result != noErr) return result;
        result = ExtAudioFileSetProperty(resources.output, kExtAudioFileProperty_ClientDataFormat, sizeof(pcm), &pcm);
        if (result != noErr) return result;
        AudioConverterRef converter = nullptr;
        UInt32 size = sizeof(converter);
        result = ExtAudioFileGetProperty(resources.output, kExtAudioFileProperty_AudioConverter, &size, &converter);
        if (result != noErr || !converter) return result != noErr ? result : kAudioFileUnspecifiedError;
        UInt32 bitrate = 32000;
        result = AudioConverterSetProperty(converter, kAudioConverterEncodeBitRate, sizeof(bitrate), &bitrate);
        if (result != noErr) return result;
        CFArrayRef config = nullptr;
        result = ExtAudioFileSetProperty(resources.output, kExtAudioFileProperty_ConverterConfig, sizeof(config), &config);
        if (result != noErr) return result;

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        float samples[4096];
        while (!canceled.load() && std::chrono::steady_clock::now() < deadline) {
            UInt32 frames = 4096;
            AudioBufferList buffer{1, {{1, sizeof(samples), samples}}};
            result = ExtAudioFileRead(resources.input, &frames, &buffer);
            if (result != noErr) return result;
            if (frames == 0) {
                result = ExtAudioFileDispose(resources.output);
                resources.output = nullptr;
                if (result != noErr) return result;
                outputFD = open(resources.path.c_str(), O_RDONLY);
                return outputFD >= 0 ? noErr : kAudioFileUnspecifiedError;
            }
            result = ExtAudioFileWrite(resources.output, frames, &buffer);
            if (result != noErr) return result;
        }
        return kAudioFileUnspecifiedError;
    }
};
