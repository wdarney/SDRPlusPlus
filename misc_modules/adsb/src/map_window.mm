#include "map_window.h"
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

@interface SDRPPADSBWindow : NSObject <WKNavigationDelegate>
@property(nonatomic,strong) NSWindow* window;
@property(nonatomic,strong) WKWebView* web;
@property(nonatomic,strong) NSURL* origin;
@end
@implementation SDRPPADSBWindow
- (void)webView:(WKWebView*)webView decidePolicyForNavigationAction:(WKNavigationAction*)action decisionHandler:(void (^)(WKNavigationActionPolicy))handler {
    NSURL* url=action.request.URL;
    BOOL local=[url.scheme isEqualToString:@"http"] && [url.host isEqualToString:self.origin.host] && [url.port isEqualToNumber:self.origin.port];
    handler(local ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}
@end

namespace adsb {
MapWindow::~MapWindow() { close(); }
void MapWindow::show(const std::string& url) {
    NSCAssert([NSThread isMainThread], @"ADS-B window must run on main thread");
    SDRPPADSBWindow* controller=(__bridge SDRPPADSBWindow*)controller_;
    if(!controller) {
        controller=[SDRPPADSBWindow new];
        controller_= (__bridge_retained void*)controller;
        controller.window=[[NSWindow alloc] initWithContentRect:NSMakeRect(120,120,1200,800)
            styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable|NSWindowStyleMaskMiniaturizable
            backing:NSBackingStoreBuffered defer:NO];
        controller.window.title=@"SDR++ ADS-B — tar1090";
        controller.window.releasedWhenClosed=NO;
        controller.window.minSize=NSMakeSize(640,400);
        WKWebViewConfiguration* config=[WKWebViewConfiguration new];
        config.websiteDataStore=[WKWebsiteDataStore nonPersistentDataStore];
        controller.web=[[WKWebView alloc] initWithFrame:controller.window.contentView.bounds configuration:config];
        controller.web.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
        controller.web.navigationDelegate=controller;
        [controller.window.contentView addSubview:controller.web];
    }
    NSURL* target=[NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    if(![target isEqual:controller.origin]) {
        controller.origin=target;
        [controller.web loadRequest:[NSURLRequest requestWithURL:target]];
    } else {
        [controller.web reload];
    }
    [controller.window makeKeyAndOrderFront:nil];
}
void MapWindow::close() {
    if(!controller_)return;
    NSCAssert([NSThread isMainThread], @"ADS-B window must close on main thread");
    SDRPPADSBWindow* controller=(__bridge_transfer SDRPPADSBWindow*)controller_;
    controller_=nullptr;
    [controller.web stopLoading];
    controller.web.navigationDelegate=nil;
    [controller.web removeFromSuperview];
    [controller.window close];
}
}
