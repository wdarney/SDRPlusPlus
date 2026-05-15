#import "ViewController.h"
#import <Metal/Metal.h>
#import <AVFoundation/AVFoundation.h>

#include <core.h>
#include <backend.h>
#include "ios_backend.h"
#include <thread>
#include <vector>
#include <string>

// Forward decl from src/main.cpp -> core/src/core.cpp.
int sdrpp_main(int argc, char* argv[]);

// ---------------------------------------------------------------------------
// SDRMTKView — MTKView that conforms to UIKeyInput so the view itself can
// become first responder and receive soft-keyboard text without a hidden
// UITextField shim. UIKeyInput is the standard iOS API for in-app keyboard
// capture (used by game engines, terminal emulators, etc.).
// ---------------------------------------------------------------------------
@interface SDRMTKView : MTKView <UIKeyInput>
@end

@implementation SDRMTKView

- (BOOL)canBecomeFirstResponder { return YES; }

// UIKeyInput — called for every character typed on the soft keyboard.
- (BOOL)hasText { return YES; } // always YES so Delete key is always enabled

- (void)insertText:(NSString*)text {
    if (!text.length) return;
    if ([text isEqualToString:@"\n"]) {
        // Return/Done key — inject Enter then let ImGui clear focus.
        backend::iosTypeChar('\n');
        return;
    }
    [text enumerateSubstringsInRange:NSMakeRange(0, text.length)
                             options:NSStringEnumerationByComposedCharacterSequences
                          usingBlock:^(NSString* s, NSRange r, NSRange er, BOOL* stop) {
        const char* utf8 = [s UTF8String];
        if (!utf8) return;
        // Decode first Unicode codepoint from UTF-8.
        unsigned cp = 0;
        unsigned char c = (unsigned char)utf8[0];
        if      (c < 0x80) cp = c;
        else if (c < 0xE0) cp = ((c & 0x1F) << 6)  | (utf8[1] & 0x3F);
        else if (c < 0xF0) cp = ((c & 0x0F) << 12) | ((utf8[1] & 0x3F) << 6) | (utf8[2] & 0x3F);
        else               cp = ((c & 0x07) << 18) | ((utf8[1] & 0x3F) << 12) | ((utf8[2] & 0x3F) << 6) | (utf8[3] & 0x3F);
        backend::iosTypeChar(cp);
    }];
}

- (void)deleteBackward {
    backend::iosTypeBackspace();
}

@end

// ---------------------------------------------------------------------------

@interface ViewController ()
@property (nonatomic, strong) SDRMTKView*               mtkView;
@property (nonatomic, strong) UIPinchGestureRecognizer* pinch;
@property (nonatomic, strong) UILongPressGestureRecognizer* longPress;
@property (nonatomic, strong) UIPanGestureRecognizer*   twoFingerPan;
@property (nonatomic, assign) BOOL                      coreStarted;
@property (nonatomic, assign) BOOL                      keyboardVisible;
@property (nonatomic, assign) CGPoint                   lastPanPoint;
@end

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];

    // Configure AVAudioSession for background playback here — on the main
    // thread, before sdrpp_main starts. iOS requires the session category to
    // be set on the main thread for UIBackgroundModes:audio to work reliably.
    // The coreaudio_sink module also calls setActive:YES when it starts audio;
    // this early call ensures the category is correct even if the sink hasn't
    // started yet when the user backgrounds the app.
    {
        NSError* audioErr = nil;
        AVAudioSession* sess = [AVAudioSession sharedInstance];
        if (![sess setCategory:AVAudioSessionCategoryPlayback error:&audioErr]) {
            NSLog(@"[SDR++] AVAudioSession setCategory failed: %@", audioErr.localizedDescription);
        }
        if (![sess setActive:YES error:&audioErr]) {
            NSLog(@"[SDR++] AVAudioSession setActive failed: %@", audioErr.localizedDescription);
        }

        // Observe interruptions (phone calls, other apps taking audio focus).
        // On interruption-end, reactivate the session so the DSP/audio chain
        // resumes automatically without user interaction.
        [[NSNotificationCenter defaultCenter]
            addObserver:self
               selector:@selector(handleAudioInterruption:)
                   name:AVAudioSessionInterruptionNotification
                 object:sess];
    }

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    CGRect initialFrame = [UIScreen mainScreen].bounds;
    self.mtkView = [[SDRMTKView alloc] initWithFrame:initialFrame device:device];
    self.mtkView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.mtkView.colorPixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
    self.mtkView.delegate         = self;
    self.mtkView.preferredFramesPerSecond = 60;
    [self.view addSubview:self.mtkView];

    // Pinch -> mouse wheel scroll (waterfall zoom).
    self.pinch = [[UIPinchGestureRecognizer alloc]
                    initWithTarget:self action:@selector(onPinch:)];
    [self.mtkView addGestureRecognizer:self.pinch];

    // Long-press -> right-click context menu.
    self.longPress = [[UILongPressGestureRecognizer alloc]
                        initWithTarget:self action:@selector(onLongPress:)];
    self.longPress.minimumPressDuration = 0.5;
    [self.mtkView addGestureRecognizer:self.longPress];

    // Two-finger pan (waterfall scroll).
    self.twoFingerPan = [[UIPanGestureRecognizer alloc]
                            initWithTarget:self action:@selector(onTwoFingerPan:)];
    self.twoFingerPan.minimumNumberOfTouches = 2;
    self.twoFingerPan.maximumNumberOfTouches = 2;
    [self.mtkView addGestureRecognizer:self.twoFingerPan];

    backend::iosAttachView((__bridge void*)self.mtkView);

    // Use the Documents directory as the SDR++ root so that config files and
    // recordings are visible in Files → On My iPhone → SDR++ Client.
    // (UIFileSharingEnabled in Info.plist exposes Documents, not Application
    // Support, so this is required for file access without a Mac.)
    NSString* docs = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
    [[NSFileManager defaultManager] createDirectoryAtPath:docs withIntermediateDirectories:YES attributes:nil error:nil];
    backend::iosSetAppFilesDir(std::string([docs UTF8String]));

    [self copyBundleResources:docs];

    self.coreStarted = NO;
    [NSThread detachNewThreadWithBlock:^{
        char arg0[]    = "sdrpp";
        char argFlag[] = "-r";
        std::string root = backend::iosAppFilesDir();
        std::vector<char> rootBuf(root.begin(), root.end());
        rootBuf.push_back('\0');
        char* argv[] = { arg0, argFlag, rootBuf.data() };
        sdrpp_main(3, argv);
    }];
    self.coreStarted = YES;
}

- (void)copyBundleResources:(NSString*)dest {
    NSString* src = [[NSBundle mainBundle] pathForResource:@"res" ofType:nil];
    if (!src) { NSLog(@"SDR++: bundled /res not found"); return; }
    NSString* dst = [dest stringByAppendingPathComponent:@"res"];
    NSFileManager* fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:dst]) {
        NSError* err = nil;
        [fm copyItemAtPath:src toPath:dst error:&err];
        if (err) NSLog(@"SDR++: failed to copy res: %@", err);
    }
}

#pragma mark - MTKViewDelegate

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
    backend::iosResize(size.width, size.height, view.contentScaleFactor);
}

- (void)drawInMTKView:(MTKView*)view {
    if (!self.coreStarted) return;
    backend::iosDrawFrame();

    // Sync soft-keyboard visibility with ImGui's WantTextInput flag.
    // SDRMTKView conforms to UIKeyInput so becoming first responder shows the
    // system keyboard and routes insertText:/deleteBackward: directly to us.
    BOOL want = backend::iosWantsKeyboard();
    if (want && !self.keyboardVisible) {
        [self.mtkView becomeFirstResponder];
        self.keyboardVisible = YES;
    } else if (!want && self.keyboardVisible) {
        [self.mtkView resignFirstResponder];
        self.keyboardVisible = NO;
    }
}

#pragma mark - Touches

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* t = touches.anyObject;
    CGPoint p  = [t locationInView:self.mtkView];
    backend::iosTouchBegan(p.x, p.y);
}
- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* t = touches.anyObject;
    CGPoint p  = [t locationInView:self.mtkView];
    backend::iosTouchMoved(p.x, p.y);
}
- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* t = touches.anyObject;
    CGPoint p  = [t locationInView:self.mtkView];
    backend::iosTouchEnded(p.x, p.y);
}
- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    UITouch* t = touches.anyObject;
    CGPoint p  = [t locationInView:self.mtkView];
    backend::iosTouchEnded(p.x, p.y);
}

#pragma mark - Gestures

- (void)onPinch:(UIPinchGestureRecognizer*)g {
    if (g.state == UIGestureRecognizerStateBegan ||
        g.state == UIGestureRecognizerStateChanged) {
        CGPoint c = [g locationInView:self.mtkView];
        backend::iosTouchMoved(c.x, c.y);
        backend::iosWheel(0.0, g.velocity * (1.0 / 60.0) * 0.5);
    }
}

- (void)onLongPress:(UILongPressGestureRecognizer*)g {
    if (g.state == UIGestureRecognizerStateBegan) {
        CGPoint p = [g locationInView:self.mtkView];
        backend::iosRightClickAt(p.x, p.y);
    }
}

- (void)onTwoFingerPan:(UIPanGestureRecognizer*)g {
    CGPoint p = [g locationInView:self.mtkView];
    switch (g.state) {
    case UIGestureRecognizerStateBegan:
        self.lastPanPoint = p;
        backend::iosPanBegan(p.x, p.y);
        break;
    case UIGestureRecognizerStateChanged: {
        double dx = p.x - self.lastPanPoint.x;
        double dy = p.y - self.lastPanPoint.y;
        self.lastPanPoint = p;
        backend::iosPanMoved(dx, dy);
        break;
    }
    case UIGestureRecognizerStateEnded:
    case UIGestureRecognizerStateCancelled:
        backend::iosPanEnded();
        break;
    default: break;
    }
}

#pragma mark - Audio session interruption

- (void)handleAudioInterruption:(NSNotification*)notification {
    NSInteger type = [notification.userInfo[AVAudioSessionInterruptionTypeKey] integerValue];
    if (type == AVAudioSessionInterruptionTypeEnded) {
        // Phone call ended, Siri dismissed, etc. — reactivate the session so
        // the CoreAudio render callback (and therefore background DSP) resumes.
        NSError* err = nil;
        if (![[AVAudioSession sharedInstance] setActive:YES error:&err]) {
            NSLog(@"[SDR++] AVAudioSession re-activate failed: %@", err.localizedDescription);
        }
    }
}

@end
