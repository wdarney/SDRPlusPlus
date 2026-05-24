#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#include "encoding.h"
#include <vector>
#include <cstdio>

namespace encoding {

std::string wavToM4A(const std::string& wavPath, const std::string& transcript, float avgSnrDb) {
    if (wavPath.size() < 4 || wavPath.substr(wavPath.size() - 4) != ".wav") return {};

    std::string m4aPath = wavPath.substr(0, wavPath.size() - 4) + ".m4a";

    NSURL* srcURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:wavPath.c_str()]];
    NSURL* dstURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:m4aPath.c_str()]];

    // ── Open source WAV ──────────────────────────────────────────────────────
    ExtAudioFileRef srcFile = nullptr;
    OSStatus err = ExtAudioFileOpenURL((__bridge CFURLRef)srcURL, &srcFile);
    if (err != noErr) {
        NSLog(@"[CBEncoding] Cannot open WAV (err=%d): %@", (int)err, srcURL.lastPathComponent);
        return {};
    }

    // Get source ASBD
    AudioStreamBasicDescription srcFmt = {};
    UInt32 propSize = sizeof(srcFmt);
    err = ExtAudioFileGetProperty(srcFile, kExtAudioFileProperty_FileDataFormat, &propSize, &srcFmt);
    if (err != noErr) {
        ExtAudioFileDispose(srcFile);
        return {};
    }

    // ── Create destination M4A ───────────────────────────────────────────────
    AudioStreamBasicDescription dstFmt = {};
    dstFmt.mSampleRate       = srcFmt.mSampleRate;
    dstFmt.mFormatID         = kAudioFormatMPEG4AAC;
    dstFmt.mChannelsPerFrame = srcFmt.mChannelsPerFrame;
    // All other fields left 0 — the codec fills them in

    ExtAudioFileRef dstFile = nullptr;
    err = ExtAudioFileCreateWithURL(
        (__bridge CFURLRef)dstURL,
        kAudioFileM4AType,
        &dstFmt,
        nullptr,                    // channel layout
        kAudioFileFlags_EraseFile,
        &dstFile
    );
    if (err != noErr) {
        NSLog(@"[CBEncoding] Cannot create M4A (err=%d): %@", (int)err, dstURL.lastPathComponent);
        ExtAudioFileDispose(srcFile);
        return {};
    }

    // ── Client format: INT16 PCM (matches the WAV samples we read) ──────────
    AudioStreamBasicDescription clientFmt = {};
    clientFmt.mSampleRate       = srcFmt.mSampleRate;
    clientFmt.mFormatID         = kAudioFormatLinearPCM;
    clientFmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    clientFmt.mChannelsPerFrame = srcFmt.mChannelsPerFrame;
    clientFmt.mBitsPerChannel   = 16;
    clientFmt.mBytesPerFrame    = 2 * srcFmt.mChannelsPerFrame;
    clientFmt.mBytesPerPacket   = clientFmt.mBytesPerFrame;
    clientFmt.mFramesPerPacket  = 1;

    ExtAudioFileSetProperty(srcFile, kExtAudioFileProperty_ClientDataFormat, sizeof(clientFmt), &clientFmt);
    ExtAudioFileSetProperty(dstFile, kExtAudioFileProperty_ClientDataFormat, sizeof(clientFmt), &clientFmt);

    // ── Target bitrate: 32 kbps mono / 64 kbps stereo ───────────────────────
    {
        AudioConverterRef converter = nullptr;
        UInt32 sz = sizeof(converter);
        if (ExtAudioFileGetProperty(dstFile, kExtAudioFileProperty_AudioConverter, &sz, &converter) == noErr
            && converter)
        {
            UInt32 bitrate = (srcFmt.mChannelsPerFrame == 1) ? 32000 : 64000;
            AudioConverterSetProperty(converter, kAudioConverterEncodeBitRate, sizeof(bitrate), &bitrate);
            // Poke ExtAudioFile so it picks up the updated converter configuration
            CFArrayRef cf = nullptr;
            ExtAudioFileSetProperty(dstFile, kExtAudioFileProperty_ConverterConfig, sizeof(cf), &cf);
        }
    }

    // ── Read / write loop ────────────────────────────────────────────────────
    const UInt32 CHUNK = 4096;
    std::vector<int16_t> buf(CHUNK * srcFmt.mChannelsPerFrame);
    bool ok = true;

    while (true) {
        AudioBufferList abl;
        abl.mNumberBuffers              = 1;
        abl.mBuffers[0].mNumberChannels = srcFmt.mChannelsPerFrame;
        abl.mBuffers[0].mDataByteSize   = (UInt32)(buf.size() * sizeof(int16_t));
        abl.mBuffers[0].mData           = buf.data();

        UInt32 frames = CHUNK;
        err = ExtAudioFileRead(srcFile, &frames, &abl);
        if (err != noErr || frames == 0) break;

        abl.mBuffers[0].mDataByteSize = frames * clientFmt.mBytesPerFrame;
        err = ExtAudioFileWrite(dstFile, frames, &abl);
        if (err != noErr) { ok = false; break; }
    }

    ExtAudioFileDispose(srcFile);
    ExtAudioFileDispose(dstFile);

    if (!ok) {
        std::remove(m4aPath.c_str());  // remove partial output
        NSLog(@"[CBEncoding] ✗ encode failed (err=%d): %@", (int)err, srcURL.lastPathComponent);
        return {};
    }

    // ── Embed iTunes tags via AVAssetExportSession passthrough ──────────────────
    // Copies AAC frames verbatim, only rewrites the container atoms.
    // ©lyr = transcript (lyrics),  ©cmt = SNR stat.  No re-encode, no quality loss.
    bool hasTranscript = !transcript.empty();
    bool hasSnr        = (avgSnrDb != 0.0f);
    char snrBuf[32]    = {};
    if (hasSnr) snprintf(snrBuf, sizeof(snrBuf), "SNR: %.1f dB avg", avgSnrDb);

    if (hasTranscript || hasSnr) {
        NSString* tmpPath = [dstURL.path stringByAppendingString:@".tmp"];
        NSURL*    tmpURL  = [NSURL fileURLWithPath:tmpPath];

        AVURLAsset* asset = [AVURLAsset assetWithURL:dstURL];

        NSMutableArray<AVMutableMetadataItem*>* metaItems = [NSMutableArray array];

        if (hasTranscript) {
            NSString* transStr = [NSString stringWithUTF8String:transcript.c_str()];
            AVMutableMetadataItem* lyricsItem = [AVMutableMetadataItem metadataItem];
            lyricsItem.keySpace = AVMetadataKeySpaceiTunes;
            lyricsItem.key      = @"©lyr";
            lyricsItem.value    = transStr;
            [metaItems addObject:lyricsItem];
        }

        if (hasSnr) {
            AVMutableMetadataItem* commentItem = [AVMutableMetadataItem metadataItem];
            commentItem.keySpace = AVMetadataKeySpaceiTunes;
            commentItem.key      = @"©cmt";
            commentItem.value    = [NSString stringWithUTF8String:snrBuf];
            [metaItems addObject:commentItem];
        }

        AVAssetExportSession* session = [AVAssetExportSession
            exportSessionWithAsset:asset
            presetName:AVAssetExportPresetPassthrough];
        session.outputURL      = tmpURL;
        session.outputFileType = AVFileTypeAppleM4A;
        session.metadata       = metaItems;

        // Block until export finishes — we're already on the encode background thread
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [session exportAsynchronouslyWithCompletionHandler:^{
            dispatch_semaphore_signal(sem);
        }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

        if (session.status == AVAssetExportSessionStatusCompleted) {
            std::remove(m4aPath.c_str());
            std::rename(tmpPath.UTF8String, m4aPath.c_str());
            NSLog(@"[CBEncoding] ✓ %@ (tags:%s%s)", dstURL.lastPathComponent,
                  hasTranscript ? " transcript" : "", hasSnr ? snrBuf : "");
        } else {
            NSLog(@"[CBEncoding] Tag embed failed (%@) — keeping M4A without tags",
                  session.error.localizedDescription);
            std::remove(tmpPath.UTF8String);  // clean up temp; untagged M4A is still valid
        }
    } else {
        NSLog(@"[CBEncoding] ✓ %@", dstURL.lastPathComponent);
    }

    std::remove(wavPath.c_str());
    return m4aPath;
}

} // namespace encoding
