#import <Speech/Speech.h>
#import <AppKit/AppKit.h>
#include "transcription.h"

// ── Objective-C session object ───────────────────────────────────────────────

@interface CBTranscriptionSession : NSObject
- (instancetype)initWithFileURL:(NSURL*)url;
- (void)cancel;
@property (atomic, copy, readonly) NSString* latestText;
@property (atomic, readonly)       BOOL      isFinal;
@end

@implementation CBTranscriptionSession {
    SFSpeechRecognizer*            _recognizer;
    SFSpeechURLRecognitionRequest* _request;
    SFSpeechRecognitionTask*       _task;
    NSString*                      _latestText;
    BOOL                           _isFinal;
    BOOL                           _cancelled;
}

- (instancetype)initWithFileURL:(NSURL*)url {
    self = [super init];
    if (!self) return nil;
    _latestText = @"";
    _isFinal    = NO;
    _cancelled  = NO;

    _recognizer = [[SFSpeechRecognizer alloc] initWithLocale:[NSLocale localeWithLocaleIdentifier:@"en-US"]];
    if (!_recognizer || !_recognizer.isAvailable) {
        NSLog(@"[CBTranscription] recognizer unavailable");
        return nil;
    }

    _request = [[SFSpeechURLRecognitionRequest alloc] initWithURL:url];
    _request.shouldReportPartialResults = YES;
    if (@available(macOS 12, *)) {
        _request.requiresOnDeviceRecognition = YES;
        if (!_recognizer.supportsOnDeviceRecognition)
            NSLog(@"[CBTranscription] WARNING: on-device model not available — "
                  "enable Dictation in System Settings → Keyboard → Dictation to download it");
    }
    if (@available(macOS 13, *)) {
        _request.addsPunctuation = YES;
    }

    __weak CBTranscriptionSession* ws = self;
    _task = [_recognizer recognitionTaskWithRequest:_request
                                     resultHandler:^(SFSpeechRecognitionResult* result, NSError* err) {
        CBTranscriptionSession* ss = ws;
        if (!ss || ss->_cancelled) return;
        @synchronized(ss) {
            if (result) {
                NSString* text = result.bestTranscription.formattedString;
                if (text.length > 0) ss->_latestText = [text copy];
                if (result.isFinal) ss->_isFinal = YES;
            }
            if (err && !ss->_isFinal) {
                // 1110 = kAFSpeechRecognitionErrorCodeNoSpeech — silence or too-short clip
                if (err.code != 1110)
                    NSLog(@"[CBTranscription] error %ld for %@: %@",
                          (long)err.code, url.lastPathComponent, err.localizedDescription);
                ss->_isFinal = YES;
            }
        }
    }];

    if (!_task) {
        NSLog(@"[CBTranscription] failed to start task for %@", url.lastPathComponent);
        return nil;
    }
    NSLog(@"[CBTranscription] started: %@", url.lastPathComponent);
    return self;
}

- (void)cancel {
    _cancelled = YES;
    [_task cancel];
}

- (NSString*)latestText { @synchronized(self) { return _latestText; } }
- (BOOL)isFinal         { @synchronized(self) { return _isFinal; }   }
@end

// ── C++ API ──────────────────────────────────────────────────────────────────

namespace transcription {

static bool plistKeyPresent() {
    return [[NSBundle mainBundle]
            objectForInfoDictionaryKey:@"NSSpeechRecognitionUsageDescription"] != nil;
}

AuthStatus authStatus() {
    if (!plistKeyPresent()) return AuthStatus::NotConfigured;
    @try {
        switch ([SFSpeechRecognizer authorizationStatus]) {
            case SFSpeechRecognizerAuthorizationStatusAuthorized:    return AuthStatus::Authorized;
            case SFSpeechRecognizerAuthorizationStatusDenied:        return AuthStatus::Denied;
            case SFSpeechRecognizerAuthorizationStatusRestricted:    return AuthStatus::Denied;
            case SFSpeechRecognizerAuthorizationStatusNotDetermined: return AuthStatus::NotDetermined;
            default: return AuthStatus::NotDetermined;
        }
    } @catch (NSException*) { return AuthStatus::NotConfigured; }
}

bool isAvailable() { return authStatus() == AuthStatus::Authorized; }

void requestPermission() {
    if (!plistKeyPresent()) return;
    // Call directly — GLFW's event loop doesn't drain the GCD main queue,
    // so dispatch_async(main_queue) would silently never fire.
    @try {
        [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus status) {
            NSLog(@"[CBTranscription] authorization result: %ld", (long)status);
        }];
    } @catch (NSException* e) {
        NSLog(@"[CBTranscription] requestAuthorization exception: %@", e.reason);
    }
}

void openSystemSettings() {
    NSURL* url = [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_SpeechRecognition"];
    [[NSWorkspace sharedWorkspace] openURL:url];
}

void* transcribeFile(const char* path) {
    if (!plistKeyPresent()) return nullptr;
    auto status = [SFSpeechRecognizer authorizationStatus];
    if (status == SFSpeechRecognizerAuthorizationStatusNotDetermined) {
        requestPermission();
        return nullptr;
    }
    if (status != SFSpeechRecognizerAuthorizationStatusAuthorized) return nullptr;

    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    CBTranscriptionSession* s = [[CBTranscriptionSession alloc] initWithFileURL:url];
    if (!s) return nullptr;
    return (__bridge_retained void*)s;
}

void cancel(void* handle) {
    if (!handle) return;
    [(__bridge CBTranscriptionSession*)handle cancel];
}

std::string getText(void* handle) {
    if (!handle) return {};
    NSString* t = [(__bridge CBTranscriptionSession*)handle latestText];
    return t ? std::string(t.UTF8String) : std::string();
}

bool isFinal(void* handle) {
    if (!handle) return false;
    return [(__bridge CBTranscriptionSession*)handle isFinal] == YES;
}

void destroy(void* handle) {
    if (!handle) return;
    CBTranscriptionSession* s = (__bridge_transfer CBTranscriptionSession*)handle;
    [s cancel];
    (void)s; // ARC releases
}

} // namespace transcription
