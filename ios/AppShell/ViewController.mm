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
@property (nonatomic, strong) MTKView* mtkView;
@property (nonatomic, assign) BOOL     coreStarted;
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
    // SDR++ expects bandplans/colormaps/fonts/icons/themes laid out under
    // resourcesDirectory. The Xcode bundle ships /res with that structure;
    // mirror it so the existing config defaults work unchanged.
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

@end
