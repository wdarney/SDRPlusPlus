#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#include "encoding.h"
#include <vector>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/attr.h>
#include <sys/time.h>
#include <unistd.h>

namespace encoding {

// Recover the capture time from the recording filename. openNewFile() formats the name
// (in LOCAL time) as:  ..._HH-MM-SS_DD-MM-YYYY.wav  — the last two '_'-delimited groups.
// Parsing from the back is robust to underscores/dots in bookmark or module names and in
// the frequency field. Returns 0 (caller skips) if the trailing pattern doesn't match.
static time_t captureTimeFromWavPath(const std::string& wavPath) {
    std::string base = wavPath;
    size_t slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    if (base.size() >= 4 && base.substr(base.size() - 4) == ".wav")
        base = base.substr(0, base.size() - 4);

    size_t p1 = base.rfind('_');                 // before DD-MM-YYYY
    if (p1 == std::string::npos || p1 == 0) return 0;
    size_t p2 = base.rfind('_', p1 - 1);         // before HH-MM-SS
    if (p2 == std::string::npos) return 0;

    std::string timePart = base.substr(p2 + 1, p1 - (p2 + 1));  // HH-MM-SS
    std::string datePart = base.substr(p1 + 1);                 // DD-MM-YYYY

    int hh, mn, ss, dd, mo, yyyy;
    if (sscanf(timePart.c_str(), "%d-%d-%d", &hh, &mn, &ss) != 3) return 0;
    if (sscanf(datePart.c_str(), "%d-%d-%d", &dd, &mo, &yyyy) != 3) return 0;
    if (yyyy < 1970 || mo < 1 || mo > 12 || dd < 1 || dd > 31 ||
        hh < 0 || hh > 23 || mn < 0 || mn > 59 || ss < 0 || ss > 61) return 0;

    struct tm tmv = {};
    tmv.tm_hour = hh; tmv.tm_min = mn; tmv.tm_sec = ss;
    tmv.tm_mday = dd; tmv.tm_mon = mo - 1; tmv.tm_year = yyyy - 1900;
    tmv.tm_isdst = -1;                            // resolve DST for that local date
    return mktime(&tmv);
}

// Stamp a file's modification/access time AND its macOS creation (birth) time so Finder
// "Date Created/Modified" and any date-sorted browsing reflect the real transmission time
// rather than whenever the (possibly hours-behind) encoder produced the file.
static void setFileCaptureTime(const std::string& path, time_t t) {
    if (t == 0) return;
    struct timeval tv[2];
    tv[0].tv_sec = t; tv[0].tv_usec = 0;  // access
    tv[1].tv_sec = t; tv[1].tv_usec = 0;  // modification
    utimes(path.c_str(), tv);

    struct attrlist al;
    memset(&al, 0, sizeof(al));
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.commonattr  = ATTR_CMN_CRTIME;
    struct timespec crt;
    crt.tv_sec = t; crt.tv_nsec = 0;
    setattrlist(path.c_str(), &al, &crt, sizeof(crt), 0);
}

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

    // Backdate the M4A to the actual capture time (parsed from the source filename), so
    // the file's timestamps match when the transmission happened — independent of how
    // far behind the encode queue is.
    setFileCaptureTime(m4aPath, captureTimeFromWavPath(wavPath));

    std::remove(wavPath.c_str());
    return m4aPath;
}

} // namespace encoding
