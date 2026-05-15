#pragma once
//
// iOS backend hooks. The host Xcode project owns the UIApplication lifecycle;
// the bridge below lets it drive the SDR++ render path without changing
// core's `namespace backend` interface.
//

#include <atomic>
#include <string>
#include <imgui.h> // ImTextureID

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

    // Called from sdrpp_main after gui::mainWindow.init() returns. Until this
    // is called, iosDrawFrame() clears the screen but skips the SDR++ UI
    // pass — prevents drawing an uninitialised mainWindow on the render thread.
    void iosSetMainWindowReady();

    // App-files dir injected by the host (NSApplicationSupportDirectory) so
    // sdrpp_main can be invoked with -r <path>.
    std::string iosAppFilesDir();
    void        iosSetAppFilesDir(const std::string& path);

    // Returns the id<MTLDevice> as void* so Metal-aware .mm files can obtain
    // it without the ios_backend.h header pulling in <Metal/Metal.h> (which
    // breaks inclusion from plain C++ TUs). Cast with __bridge id<MTLDevice>.
    // Returns nullptr before iosAttachView() is called.
    void* iosGetMetalDevicePtr();

    // Returns true when running on an iPad (any model / size class).
    // Callable from plain C++ — implemented in backend.mm (Obj-C++).
    bool iosIsIPad();

    // On-device speech recognition bridge (Speech.framework, iOS 13+).
    // iosTranscribeCreate returns an opaque handle; all other functions are no-ops if handle is null.
    bool        iosTranscribeIsAvailable();
    void        iosTranscribeRequestPermission();
    void*       iosTranscribeCreate(double sampleRate);
    void        iosTranscribeAppend(void* handle, const float* samples, int count);
    void        iosTranscribeEndAudio(void* handle);   // signal end of audio; final result follows
    void        iosTranscribeCancel(void* handle);
    std::string iosTranscribeGetText(void* handle);    // latest partial or final text
    bool        iosTranscribeIsFinal(void* handle);
    void        iosTranscribeDestroy(void* handle);

    // Play an audio file via AVAudioPlayer (bypasses the kAudioUnitSubType_RemoteIO
    // singleton limitation — only one CoreAudioSink AudioUnit can exist at a time,
    // so a second sink used for channel-bank monitor playback fails silently).
    // Blocks the calling thread until playback finishes or *stop_flag becomes false.
    // Safe to call from any background thread; no run loop required in the caller.
    void iosPlayRecordingFile(const char* path, const std::atomic<bool>* stop_flag);

    // Tells the touch classifier where the FFT/waterfall horizontal resize
    // divider is (Y in logical points). Vertical drags that start within ~20pt
    // of this line are treated as widget drags rather than scroll gestures, so
    // the ImGui resize handler receives mouse events. Call every draw frame from
    // the waterfall widget. Pass -1 to disable (e.g. when waterfall is hidden).
    void iosSetResizeDividerY(float y);

    // Registers the current width of the left-column controls panel (logical
    // pts). Call every frame from main_window.cpp so the touch classifier can
    // decide whether a gesture started inside the panel without reading ImGui
    // context from the touch thread (which would be a data race).
    void iosSetMenuWidth(float w);

    // Registers the left edge (logical pts) of the right WaterfallControls
    // column so vertical drags there are classified as DRAG (not SCROLL),
    // allowing the Zoom/Min/Max VSliders to be dragged.  Pass FLT_MAX when
    // the column is hidden.
    void iosSetRightPanelX(float x);

    // Returns the accumulated vertical panel-scroll delta since the last call
    // and atomically resets it to zero. Call once per frame from inside
    // BeginChild("Left Column") and apply via ImGui::SetScrollY.
    // Positive value → scroll content downward (iOS natural: finger up →
    // content moves up → Scroll.y increases).
    float iosTakePanelScrollDelta();

    // Dynamic-texture management for widgets that upload pixel data every
    // frame (waterfall, image viewer, line_push_image, etc.).
    //
    // iosUpdateTexture() creates or replaces a GPU texture large enough for
    // w×h RGBA8 pixels, then uploads `rgba` (may be nullptr to skip upload).
    // The caller passes &textureId; if the stored handle is nullptr or the
    // dimensions changed, the old texture is released and a new one created.
    //
    // On non-iOS builds host_stub.cpp provides a no-op stub — those builds
    // use the existing glTexImage2D path which is guarded by TARGET_OS_IPHONE.
    void iosUpdateTexture(ImTextureID* texId, int w, int h, const void* rgba);
}
