#import "ViewController.h"
#import <Metal/Metal.h>

#include <core.h>
#include <backend.h>
#include "ios_backend.h"
#include <thread>
#include <vector>
#include <string>

// Forward decl from src/main.cpp -> core/src/core.cpp.
int sdrpp_main(int argc, char* argv[]);

@interface ViewController ()
@property (nonatomic, strong) MTKView*                  mtkView;
@property (nonatomic, strong) UITextField*              inputShim;
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

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    self.mtkView = [[MTKView alloc] initWithFrame:self.view.bounds device:device];
    self.mtkView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.mtkView.colorPixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
    self.mtkView.delegate         = self;
    self.mtkView.preferredFramesPerSecond = 60;
    [self.view addSubview:self.mtkView];

    // Hidden text field captures soft-keyboard input. Sized 1×1 and offscreen
    // so it never paints; iOS still routes characters to its delegate.
    self.inputShim = [[UITextField alloc] initWithFrame:CGRectMake(-100, -100, 1, 1)];
    self.inputShim.delegate              = self;
    self.inputShim.autocorrectionType    = UITextAutocorrectionTypeNo;
    self.inputShim.autocapitalizationType= UITextAutocapitalizationTypeNone;
    [self.view addSubview:self.inputShim];

    // Pinch -> mouse wheel scroll, used by ImGui's waterfall zoom logic.
    self.pinch = [[UIPinchGestureRecognizer alloc]
                    initWithTarget:self action:@selector(onPinch:)];
    [self.mtkView addGestureRecognizer:self.pinch];

    // Long-press -> right-click. minimumPressDuration matches iOS's own
    // context-menu delay so the gesture feels native.
    self.longPress = [[UILongPressGestureRecognizer alloc]
                        initWithTarget:self action:@selector(onLongPress:)];
    self.longPress.minimumPressDuration = 0.5;
    [self.mtkView addGestureRecognizer:self.longPress];

    // Two-finger pan. Coexists with the single-touch path (which routes to
    // ImGui as the mouse cursor) so a one-finger drag still means "drag the
    // ImGui widget" while two fingers means "pan the waterfall".
    self.twoFingerPan = [[UIPanGestureRecognizer alloc]
                            initWithTarget:self action:@selector(onTwoFingerPan:)];
    self.twoFingerPan.minimumNumberOfTouches = 2;
    self.twoFingerPan.maximumNumberOfTouches = 2;
    [self.mtkView addGestureRecognizer:self.twoFingerPan];

    backend::iosAttachView((__bridge void*)self.mtkView);

    // Resource and config dirs both live under <appSupport>. The bundle's /res
    // tree is copied here on first launch by copyBundleResources.
    NSString* docs = [NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES) firstObject];
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

    // Sync soft keyboard visibility with ImGui's want-text-input flag every
    // frame. Cheap on iOS — becomeFirstResponder when already first responder
    // is a no-op.
    BOOL want = backend::iosWantsKeyboard();
    if (want && !self.keyboardVisible) {
        [self.inputShim becomeFirstResponder];
        self.keyboardVisible = YES;
    } else if (!want && self.keyboardVisible) {
        [self.inputShim resignFirstResponder];
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
    // Forward pinch as a mouse-wheel event at the gesture centroid. ImGui's
    // waterfall reads MouseWheel under the cursor, so warp the cursor first.
    // velocity is "scale / second"; at 60 fps that's roughly the wheel delta
    // we want, with a small damping factor for control.
    if (g.state == UIGestureRecognizerStateBegan ||
        g.state == UIGestureRecognizerStateChanged) {
        CGPoint c = [g locationInView:self.mtkView];
        backend::iosTouchMoved(c.x, c.y);
        backend::iosWheel(0.0, g.velocity * (1.0 / 60.0) * 0.5);
    }
}

- (void)onLongPress:(UILongPressGestureRecognizer*)g {
    // Fire one synthetic right-click on UIGestureRecognizerStateBegan. iOS
    // continues to send State{Changed,Ended} as the user holds — ignore them
    // so we don't open the context menu repeatedly.
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

#pragma mark - UITextFieldDelegate

- (BOOL)textField:(UITextField*)tf
        shouldChangeCharactersInRange:(NSRange)range
        replacementString:(NSString*)string {
    if (string.length == 0) {
        backend::iosTypeBackspace();
        return NO;
    }
    [string enumerateSubstringsInRange:NSMakeRange(0, string.length)
                               options:NSStringEnumerationByComposedCharacterSequences
                            usingBlock:^(NSString* s, NSRange r, NSRange er, BOOL* stop) {
        const char* utf8 = [s UTF8String];
        if (!utf8) return;
        // Decode UTF-8 to a single codepoint (handles ASCII trivially).
        unsigned cp = 0;
        unsigned char c = (unsigned char)utf8[0];
        if (c < 0x80)        cp = c;
        else if (c < 0xE0)   cp = ((c & 0x1F) << 6) | (utf8[1] & 0x3F);
        else if (c < 0xF0)   cp = ((c & 0x0F) << 12) | ((utf8[1] & 0x3F) << 6) | (utf8[2] & 0x3F);
        else                 cp = ((c & 0x07) << 18) | ((utf8[1] & 0x3F) << 12) | ((utf8[2] & 0x3F) << 6) | (utf8[3] & 0x3F);
        backend::iosTypeChar(cp);
    }];
    return NO;
}

@end
