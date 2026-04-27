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

    // App-files dir injected by the host (NSApplicationSupportDirectory) so
    // sdrpp_main can be invoked with -r <path>.
    std::string iosAppFilesDir();
    void        iosSetAppFilesDir(const std::string& path);
}
