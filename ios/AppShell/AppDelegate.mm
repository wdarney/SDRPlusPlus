#import "AppDelegate.h"
#import "ViewController.h"

#include <gui/gui.h>
#include <gui/main_window.h>

@implementation AppDelegate

- (BOOL)application:(UIApplication*)application
        didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    self.window.rootViewController = [ViewController new];
    [self.window makeKeyAndVisible];
    return YES;
}

// With UIBackgroundModes:audio (declared in Info.plist) iOS grants the app
// full background execution: network I/O, DSP threads, and the CoreAudio
// render callback all continue running. We intentionally do NOT stop the
// source here — the user expects audio to continue after pressing home.
//
// applicationWillResignActive fires for transient interruptions too (phone
// calls, Siri, Control Center), so stopping there would be overly aggressive.
// AVAudioSession interruption handling in ViewController re-activates the
// session when focus returns.
- (void)applicationWillResignActive:(UIApplication*)application {
    // no-op: keep DSP + audio running
}

- (void)applicationDidEnterBackground:(UIApplication*)application {
    // no-op: UIBackgroundModes:audio keeps us alive
}

@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
