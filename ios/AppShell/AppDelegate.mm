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

// Stop streaming when the app loses focus. iOS will throttle / suspend audio
// IO and CPU shortly after, so leaving a network source active will at best
// hammer the radio and at worst hang the receive thread on a socket read
// that never returns. This mirrors what desktop SDR++ users do manually.
- (void)applicationWillResignActive:(UIApplication*)application {
    gui::mainWindow.setPlayState(false);
}

- (void)applicationDidEnterBackground:(UIApplication*)application {
    gui::mainWindow.setPlayState(false);
}

@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
