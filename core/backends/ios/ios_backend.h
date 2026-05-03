#pragma once
//
// iOS backend hooks. The host Xcode project owns the UIApplication lifecycle;
// the bridge below lets it drive the SDR++ render path without changing
// core's `namespace backend` interface.
//

#include <string>

namespace backend {
    // Called by the host UIViewController once the CAMetalLayer-backed view is
    // ready. Stores the view (typed as void* to keep this header includable
    // from non-Obj-C++ TUs) and triggers Metal/ImGui setup.
    void iosAttachView(void* mtkView);

    // Called from the host's MTKViewDelegate every vsync. beginFrame() + draw
    // the main window + render() in one step.
    void iosDrawFrame();

    // Pixel size changed (rotation, split-view, etc).
    void iosResize(double widthPx, double heightPx, double scale);

    // Touch routing — the host translates UITouch events and calls these.
    void iosTouchBegan(double x, double y);
    void iosTouchMoved(double x, double y);
    void iosTouchEnded(double x, double y);

    // Software keyboard show/hide hint — host wires this to becomeFirstResponder
    // / resignFirstResponder on its input shim view.
    bool iosWantsKeyboard();

    // Text input from the host's hidden UITextField. UTF-32 codepoint per
    // call; the host decodes UITextField's `replacementString:` for us.
    void iosTypeChar(unsigned codepoint);
    void iosTypeBackspace();

    // Pinch gesture -> mouse wheel ticks. dy > 0 for zoom-in (matches the
    // desktop scroll convention used by the waterfall zoom handler).
    void iosWheel(double dx, double dy);

    // Long-press translation. iOS has no native right click; we emit a
    // synthetic right-mouse-button down/up pair when a UILongPressGesture
    // recognizer fires, so ImGui's standard right-click context menus work.
    void iosRightClickAt(double x, double y);

    // Two-finger pan. Forwarded as a delta in screen pixels — the host
    // tracks gesture state and only sends incremental movement. SDR++'s
    // waterfall doesn't read this directly today; the hook exists so a
    // dedicated handler in main_window.cpp can be wired up later.
    void iosPanBegan(double x, double y);
    void iosPanMoved(double dx, double dy);
    void iosPanEnded();

    // App-files dir injected by the host (NSApplicationSupportDirectory) so
    // sdrpp_main can be invoked with -r <path>.
    std::string iosAppFilesDir();
    void        iosSetAppFilesDir(const std::string& path);
}
