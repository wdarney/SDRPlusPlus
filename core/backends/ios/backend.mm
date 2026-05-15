//
// iOS backend (Metal + UIKit). Mirrors the namespace `backend` interface
// declared in core/src/backend.h, but inverts control: instead of
// renderLoop() spinning, the host UIApplication drives draws via MTKView's
// delegate and we expose iosDrawFrame() / iosTouch*() entry points (see
// ios_backend.h).
//
// Still WIP — see ios/README.md for the punch list (vendored ImGui Metal
// backend, soft keyboard plumbing, multi-touch gestures, app lifecycle).
//

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <AVFoundation/AVFoundation.h>
#import <Speech/Speech.h>

#include <cmath>

#include <backend.h>
#include "ios_backend.h"

#include <core.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/icons.h>
#include <gui/menus/theme.h>
#include <utils/flog.h>

#include "imgui.h"
// Provide imgui_impl_metal.h alongside this file. See backends/ios/imgui/README.md
// for the vendored copy from the Dear ImGui distribution.
#include "imgui/imgui_impl_metal.h"

@interface CBTranscriptionSession : NSObject
- (instancetype)initWithSampleRate:(double)sr;
- (void)appendSamples:(const float*)samples count:(int)count;
- (void)endAudio;   // no more samples — triggers final recognition
- (void)cancel;
@property (atomic, copy, readonly) NSString* latestText;
@property (atomic, readonly) BOOL isFinal;
@end

@implementation CBTranscriptionSession {
    SFSpeechRecognizer*                    _recognizer;
    SFSpeechAudioBufferRecognitionRequest* _request;
    SFSpeechRecognitionTask*               _task;
    AVAudioFormat*                         _format;
    NSString*                              _latestText;
    BOOL                                   _isFinal;
    BOOL                                   _cancelled;
}

- (instancetype)initWithSampleRate:(double)sr {
    self = [super init];
    if (!self) return nil;
    _latestText = @"";
    _isFinal    = NO;
    _cancelled  = NO;
    _recognizer = [[SFSpeechRecognizer alloc] initWithLocale:[NSLocale localeWithLocaleIdentifier:@"en-US"]];
    if (!_recognizer || !_recognizer.isAvailable) return nil;

    _request = [[SFSpeechAudioBufferRecognitionRequest alloc] init];
    _request.requiresOnDeviceRecognition = YES;
    _request.shouldReportPartialResults  = YES;
    if (@available(iOS 16, *)) { _request.addsPunctuation = YES; }

    _format = [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                               sampleRate:sr
                                                 channels:1
                                              interleaved:NO];

    __weak CBTranscriptionSession* ws = self;
    _task = [_recognizer recognitionTaskWithRequest:_request
                                     resultHandler:^(SFSpeechRecognitionResult* result, NSError* err) {
        CBTranscriptionSession* ss = ws;
        if (!ss || ss->_cancelled) return;
        if (result) {
            @synchronized(ss) {
                ss->_latestText = [result.bestTranscription.formattedString copy] ?: @"";
                if (result.isFinal) ss->_isFinal = YES;
            }
        }
        (void)err; // cancellation errors are expected; log only unexpected ones
    }];
    return _task ? self : nil;
}

- (void)appendSamples:(const float*)samples count:(int)count {
    if (_cancelled || _isFinal || count <= 0) return;
    AVAudioPCMBuffer* buf = [[AVAudioPCMBuffer alloc] initWithPCMFormat:_format
                                                          frameCapacity:(AVAudioFrameCount)count];
    if (!buf) return;
    buf.frameLength = (AVAudioFrameCount)count;
    memcpy(buf.floatChannelData[0], samples, (size_t)count * sizeof(float));
    [_request appendAudioPCMBuffer:buf];
}

- (void)endAudio {
    if (!_cancelled) [_request endAudio];
}

- (void)cancel {
    _cancelled = YES;
    [_task cancel];
    [_request endAudio];
}

- (NSString*)latestText {
    @synchronized(self) { return _latestText; }
}
- (BOOL)isFinal {
    @synchronized(self) { return _isFinal; }
}
@end

namespace backend {
    static MTKView*               g_mtkView           = nil;
    static id<MTLDevice>          g_device            = nil;
    static id<MTLCommandQueue>    g_commandQueue      = nil;
    static MTLRenderPassDescriptor* g_renderPassDesc  = nil;
    // Both flags are written from the sdrpp_main background thread and read
    // from the Metal draw delegate on the main thread. Atomics ensure the
    // main thread sees a consistent view without data races.
    // g_imguiInitialized gates ALL ImGui calls (beginFrame, input hooks, etc.)
    // g_mainWindowReady is set at the same time; kept for future fine-grained use.
    static std::atomic<bool>      g_imguiInitialized{false};
    static std::atomic<bool>      g_mainWindowReady{false};
    // Set to true by beginFrame(), cleared by render(). Ensures render() never
    // calls ImGui::Render() without a matching prior ImGui::NewFrame(). Both
    // beginFrame() and render() are main-thread-only so no atomic needed.
    static bool                   g_frameStarted{false};
    // Drawable acquired in beginFrame() and consumed in render(). Stored here
    // so render() doesn't need to call currentDrawable a second time.
    static id<CAMetalDrawable>    g_currentDrawable   = nil;
    static std::string            g_appFilesDir;

    // Single-touch → ImGui mouse cursor.
    //
    // Problem: a quick tap (touchesBegan + touchesEnded both between two 60 Hz
    // frames) means the button is down AND up before the next NewFrame(). If we
    // inject both events immediately, NewFrame() processes DOWN then UP in the
    // same call and leaves MouseDown=false — MouseClicked is never set, so
    // InputText widgets never gain focus and WantTextInput stays false.
    //
    // Solution: track a "went down" peak flag. beginFrame() injects at most one
    // state transition per frame: DOWN first (even if the finger is already up),
    // then UP the following frame. This guarantees ImGui sees DOWN in frame N and
    // UP in frame N+1, making MouseClicked fire correctly for quick taps.
    static bool   g_mouseDown      = false; // current physical touch state
    static bool   g_mouseWentDown  = false; // went down since last beginFrame
    static bool   g_imguiMouseDown = false; // last state ImGui was told
    static double g_mouseX          = 0.0;
    static double g_mouseY          = 0.0;

    // Touch-gesture mode discrimination.
    //
    // A single finger can mean three things:
    //   NEUTRAL  — undecided; waiting for enough movement to classify.
    //   DRAG     — cursor moves with the finger (sliders, waterfall tuning,
    //               horizontal gestures).
    //   SCROLL   — vertical drag over a scrollable panel. The cursor is held
    //               fixed so the hovered window stays locked and wheel events
    //               drive the scroll position instead.
    //
    // Classification rule (after the finger has moved > kScrollThreshold pts):
    //   • primarily vertical  → SCROLL
    //   • primarily horizontal → DRAG
    //
    // KEY: g_mouseWentDown is NOT set in iosTouchBegan. It is only set when
    // DRAG mode is committed (so ImGui never sees a down event during a scroll
    // gesture) or when a quick tap ends in NEUTRAL (injecting a click). This
    // prevents accidental widget activation (slider drags, button presses)
    // that previously caused DSP reconfigurations and tanked the frame rate.
    enum class TouchMode { NEUTRAL, DRAG, SCROLL };
    static TouchMode g_touchMode       = TouchMode::NEUTRAL;
    static double    g_touchStartX     = 0.0;
    static double    g_touchStartY     = 0.0;
    // Last Y seen during a SCROLL gesture — updated every iosTouchMoved call
    // so we emit incremental deltas, not accumulated total displacement.
    static double    g_scrollLastY     = 0.0;

    // Points of finger movement required before committing to SCROLL vs DRAG.
    static constexpr double kScrollThreshold = 10.0;

    // Y coordinate (logical points) of a horizontal resize divider that should
    // be treated as DRAG rather than SCROLL when a vertical gesture begins near
    // it.  Updated every frame by the waterfall widget via iosSetResizeDividerY().
    // -1 means no active divider.
    static std::atomic<float> g_resizeDividerY{-1.0f};
    static constexpr float    kResizeDividerHalf = 20.0f; // grab radius in pts
    // Logical-point divisor mapping finger-δy → wheel ticks.
    // ImGui scrolls ≈ fontSize*3 pts per wheel tick (≈48 pts with default 16pt
    // font), so ÷50 gives close to 1:1 pixel tracking.
    static constexpr float kScrollDivisor = 50.0f;

    // Width of the left-column module-controls panel in logical points, set
    // every frame by main_window.cpp via iosSetMenuWidth().  Stored atomically
    // because it is written on the render thread and read on the main thread.
    static std::atomic<float> g_menuWidth{0.0f};

    // Left edge of the right WaterfallControls column (Zoom/Min/Max sliders).
    // Vertical gestures that start here must be DRAG (not SCROLL) so the
    // VSliderFloat widgets receive mouse events.  FLT_MAX when menu is hidden.
    static std::atomic<float> g_rightPanelX{FLT_MAX};

    // Touch starts in panel-scroll mode when the finger X is within the left
    // column.  A small dead zone handles exactly-on-boundary taps; the old
    // large dead zone (55 pt) was shrinking the scroll zone too aggressively
    // on narrow phone panels.  The "leaks to waterfall" problem is solved by
    // using SetScrollY (targets Left Column directly) rather than wheel events.
    static constexpr float kPanelScrollDeadZone = 8.0f; // logical pts

    // Set in iosTouchBegan via the menu-width dead-zone check (no ImGui-context
    // reads — those are racy from the touch thread).
    static bool g_scrollInPanel = false;

    // Accumulated vertical scroll for the left-column panel.  Written from the
    // touch thread (iosTouchMoved), read+reset on the render thread
    // (iosTakePanelScrollDelta called from main_window.cpp).  Using atomic
    // load/store of the raw bits avoids a mutex; the worst race is a single
    // lost frame of scroll, which is imperceptible.
    static std::atomic<float> g_panelScrollAccum{0.0f};

    // Keyboard backspace — same two-frame timing problem as mouse buttons.
    // Both AddKeyEvent(down) + AddKeyEvent(up) in the same NewFrame() leave
    // DownDuration=-1 so IsKeyPressed() never fires. Use a pending counter:
    // inject key-DOWN this frame, key-UP next frame.
    static int    g_pendingBackspaces = 0;
    static bool   g_backspaceDown    = false;

    int init(std::string resDir) {
        flog::info("iOS backend init (resDir={})", resDir);
        if (!g_device) {
            flog::warn("iOS backend init called before iosAttachView");
            return 0;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = NULL; // no settings file in the iOS sandbox
        // DisplaySize and DisplayFramebufferScale are set per-frame in beginFrame()
        // from g_mtkView.bounds/contentScaleFactor (main-thread UIKit calls).
        // Do NOT read nativeScale here — this runs on the sdrpp_main background
        // thread and nativeScale is UIKit which must be called on the main thread.

        ImGui_ImplMetal_Init(g_device);
        // Do NOT set g_imguiInitialized here. The draw loop must not call
        // ImGui::NewFrame() until after style::loadFonts() has finished
        // dirtying the atlas and gui::mainWindow.init() has completed.
        // iosSetMainWindowReady() sets the flag after all that is done.
        return 0;
    }

    bool isRenderThread() { return [[NSThread currentThread] isMainThread]; }

    void beginFrame() {
        // All Metal/ImGui frame work must happen on the main thread (MTKView
        // drawable ownership). LoadingScreen::show() calls us from the background
        // sdrpp_main thread during init — skip silently.
        if (!isRenderThread()) return;
        if (!g_mtkView || !g_renderPassDesc) return;

        // Acquire the drawable first so we can prime the render pass descriptor
        // with a real texture. ImGui_ImplMetal_NewFrame captures the framebuffer
        // descriptor (incl. sampleCount) from g_renderPassDesc — if the texture
        // is nil at that point, sampleCount=0 and pipeline state creation fails.
        g_currentDrawable = g_mtkView.currentDrawable;
        if (!g_currentDrawable) return;
        g_renderPassDesc.colorAttachments[0].texture = g_currentDrawable.texture;

        // On the first few frames log the actual drawable texture dimensions so
        // we can verify the Metal layer is sized correctly.
        static int s_texLog = 0;
        if (++s_texLog <= 2) {
            NSUInteger tw = g_currentDrawable.texture.width;
            NSUInteger th = g_currentDrawable.texture.height;
            CGRect b      = g_mtkView.bounds;
            CGFloat sc    = g_mtkView.contentScaleFactor;
            // Use NSLog for reliable float output — flog ignores printf format specs.
            NSLog(@"[SDR++] beginFrame: drawable=%dx%d, viewBounds=%.0fx%.0f, scale=%.1f, "
                  @"logicalSize=%.0fx%.0f",
                  (int)tw, (int)th,
                  b.size.width, b.size.height, (double)sc,
                  (double)tw / (double)sc, (double)th / (double)sc);
        }

        // ImGui_ImplMetal_Init builds the fonts texture once. If SDR++ adds
        // custom fonts later (style::loadFonts is called after backend::init),
        // the atlas is dirtied but the GPU texture is stale. NewFrame's sanity
        // check asserts the atlas is built, so we lazily rebuild here.
        ImGuiIO& io = ImGui::GetIO();
        if (!io.Fonts->IsBuilt()) {
            ImGui_ImplMetal_DestroyFontsTexture();
            ImGui_ImplMetal_CreateFontsTexture(g_device);
        }

        // Now the render pass has a valid texture → correct sampleCount.
        ImGui_ImplMetal_NewFrame(g_renderPassDesc);

        // DisplaySize = logical size in points. DisplayFramebufferScale = pixels/point.
        // Derive from the drawable texture's pixel dimensions (authoritative) and
        // the view's contentScaleFactor. This is more reliable than view.bounds,
        // which may lag if autoresizing hasn't completed or if drawableSize was
        // set explicitly via iosResize() independent of the view's bounds.
        NSUInteger texW  = g_currentDrawable.texture.width;
        NSUInteger texH  = g_currentDrawable.texture.height;
        CGFloat    scale = g_mtkView.contentScaleFactor;
        if (scale < 1.0f) scale = 1.0f; // safety: contentScaleFactor is never < 1
        io.DisplaySize             = ImVec2((float)texW / (float)scale,
                                            (float)texH / (float)scale);
        io.DisplayFramebufferScale = ImVec2((float)scale, (float)scale);
        io.DeltaTime               = 1.0f / 60.0f; // TODO: real delta from timestamps

        io.AddMousePosEvent((float)g_mouseX, (float)g_mouseY);

        // Inject at most one button-state transition per frame so ImGui always
        // sees DOWN in one NewFrame() and UP in the next — even for taps that
        // begin and end within a single 16 ms inter-frame interval.
        if (g_mouseWentDown && !g_imguiMouseDown) {
            io.AddMouseButtonEvent(0, true);
            g_imguiMouseDown = true;
            g_mouseWentDown  = false;
        } else if (!g_mouseDown && g_imguiMouseDown) {
            io.AddMouseButtonEvent(0, false);
            g_imguiMouseDown = false;
        }

        // Backspace: same two-frame pattern. inject DOWN this frame, UP next.
        if (g_pendingBackspaces > 0 && !g_backspaceDown) {
            io.AddKeyEvent(ImGuiKey_Backspace, true);
            g_backspaceDown = true;
            g_pendingBackspaces--;
        } else if (g_backspaceDown) {
            io.AddKeyEvent(ImGuiKey_Backspace, false);
            g_backspaceDown = false;
        }

        ImGui::NewFrame();
        g_frameStarted = true;
    }

    void render(bool /*vsync*/) {
        // Only render if beginFrame() actually ran on the main thread and
        // successfully acquired a drawable.
        if (!g_frameStarted) return;
        g_frameStarted = false;

        ImGui::Render();

        id<CAMetalDrawable> drawable = g_currentDrawable;
        g_currentDrawable = nil;
        if (!drawable) return;

        id<MTLCommandBuffer> cb = [g_commandQueue commandBuffer];
        g_renderPassDesc.colorAttachments[0].clearColor =
            MTLClearColorMake(gui::themeManager.clearColor.x,
                              gui::themeManager.clearColor.y,
                              gui::themeManager.clearColor.z,
                              gui::themeManager.clearColor.w);
        // texture is already set from beginFrame(); keep other fields consistent.
        g_renderPassDesc.colorAttachments[0].loadAction  = MTLLoadActionClear;
        g_renderPassDesc.colorAttachments[0].storeAction = MTLStoreActionStore;

        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:g_renderPassDesc];
        [enc pushDebugGroup:@"SDR++"];
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cb, enc);
        [enc popDebugGroup];
        [enc endEncoding];

        [cb presentDrawable:drawable];
        [cb commit];
    }

    void getMouseScreenPos(double& x, double& y) { x = g_mouseX; y = g_mouseY; }
    void setMouseScreenPos(double, double)       { /* no-op on iOS */ }

    int renderLoop() {
        // The UIApplicationMain runloop drives drawing via MTKViewDelegate →
        // iosDrawFrame(). We just need to park this background thread forever
        // so the cleanup code below renderLoop() never runs (iOS apps are killed
        // by the OS, not via main()).
        //
        // NOTE: [[NSRunLoop currentRunLoop] run] returns immediately on a
        // background thread that has no input sources — do NOT use it here.
        // Use a semaphore that is never signalled instead.
        flog::info("iOS renderLoop parked (UIApplication drives draws)");
        dispatch_semaphore_t park = dispatch_semaphore_create(0);
        dispatch_semaphore_wait(park, DISPATCH_TIME_FOREVER);
        return 0;
    }

    int end() {
        if (g_imguiInitialized.load()) {
            ImGui_ImplMetal_Shutdown();
            ImGui::DestroyContext();
            g_imguiInitialized.store(false);
        }
        return 0;
    }

    void iosAttachView(void* mtkViewPtr) {
        g_mtkView        = (__bridge MTKView*)mtkViewPtr;
        g_device         = g_mtkView.device;
        g_commandQueue   = [g_device newCommandQueue];
        g_renderPassDesc = [MTLRenderPassDescriptor new];

        // Diagnostic: log initial view geometry. Use NSLog (not flog) so the
        // numbers are guaranteed correct — flog ignores printf format specs like
        // {:.0f} and can misinterpret argument order.
        {
            CGRect b   = g_mtkView.bounds;
            CGFloat sc = g_mtkView.contentScaleFactor;
            CGSize ds  = g_mtkView.drawableSize;
            CGRect sb  = [UIScreen mainScreen].bounds;
            NSLog(@"[SDR++] iosAttachView: screen=%.0fx%.0f, "
                  @"viewBounds=%.0fx%.0f, scale=%.1f, drawableSize=%.0fx%.0f",
                  sb.size.width, sb.size.height,
                  b.size.width, b.size.height,
                  (double)sc,
                  ds.width, ds.height);
        }

        // Do NOT request speech recognition permission here — showing a system
        // dialog during view attachment can trigger watchdog kills if the main
        // run loop isn't fully set up yet.  iosTranscribeRequestPermission() is
        // called lazily by the channel bank module when it is first enabled.
    }

    void iosSetMainWindowReady() {
        // Called from sdrpp_main after gui::mainWindow.init() finishes.
        // style::loadFonts() has run and the atlas is dirty. Build the font
        // texture now (on whichever thread called us — still the sdrpp_main
        // background thread) so the very first iosDrawFrame() sees IsBuilt().
        ImGui_ImplMetal_CreateFontsTexture(g_device);
        g_mainWindowReady = true;
        g_imguiInitialized = true;   // unblock the draw loop
    }

    void iosDrawFrame() {
        if (!g_imguiInitialized) return;  // blocks until iosSetMainWindowReady

        // Log the first few frames so we can diagnose black-screen issues.
        static int s_framesSinceInit = 0;
        ++s_framesSinceInit;
        if (s_framesSinceInit <= 3) {
            flog::info("iosDrawFrame: frame {} after init", s_framesSinceInit);
        }

        beginFrame();

        // Only draw if beginFrame() actually acquired a drawable and called
        // ImGui::NewFrame(). Without a frame in flight, ImGui::Begin() asserts.
        if (!g_frameStarted) {
            if (s_framesSinceInit <= 3) {
                flog::warn("iosDrawFrame: beginFrame skipped (no drawable?), frame {}", s_framesSinceInit);
            }
            render(); // harmless no-op; keeps render() always paired with beginFrame() call
            return;
        }

        ImGuiIO& io = ImGui::GetIO();
        if (s_framesSinceInit <= 3) {
            NSLog(@"[SDR++] iosDrawFrame frame %d: DisplaySize=%.0fx%.0f",
                  s_framesSinceInit, (double)io.DisplaySize.x, (double)io.DisplaySize.y);
        }

        // Inset the SDR++ window from iOS safe areas (Dynamic Island, status bar,
        // home indicator). safeAreaInsets are UIKit points — the same logical
        // coordinate space as io.DisplaySize (pixels / contentScaleFactor).
        UIEdgeInsets safe  = g_mtkView.safeAreaInsets;
        float safeLeft     = (float)safe.left;
        float safeTop      = (float)safe.top;
        float safeRight    = (float)safe.right;
        float safeBottom   = (float)safe.bottom;
        // In landscape exactly one horizontal side has the Dynamic Island / notch
        // (large inset, ≥ 44 pt) and the other has only rounded corners or a side
        // button like Camera Control (smaller inset, no content is actually obscured).
        // Keep only the larger horizontal inset so the UI extends to the physical
        // edge on the non-DI side. In portrait safeLeft == safeRight == 0 so this
        // is a no-op.
        if (safeLeft >= safeRight) {
            safeRight = 0.0f;
        } else {
            safeLeft = 0.0f;
        }
        float winW = io.DisplaySize.x - safeLeft - safeRight;
        float winH = io.DisplaySize.y - safeTop  - safeBottom;

        if (winW > 0 && winH > 0) {
            ImGui::SetNextWindowPos(ImVec2(safeLeft, safeTop));
            ImGui::SetNextWindowSize(ImVec2(winW, winH));
            gui::mainWindow.draw();
        }
        render();
    }

    void iosResize(double /*w*/, double /*h*/, double /*scale*/) {
        // Do NOT set g_mtkView.drawableSize here. Setting drawableSize explicitly
        // disables MTKView's autoresizesDrawable, which means subsequent rotations
        // stop updating the Metal layer → size mismatch → GPU crash.
        //
        // drawableSizeWillChange: fires because MTKView is already autoresizing
        // the drawable; we don't need to do anything. beginFrame() reads the
        // live pixel dimensions from currentDrawable.texture every frame.
    }

    // Touch callbacks — translate UIKit touch events into ImGui mouse / wheel
    // input. beginFrame() injects at most one button-state transition per frame
    // (see g_mouseWentDown comment above); iosTouchMoved may emit wheel events
    // directly when in SCROLL mode.
    //
    // IMPORTANT: g_mouseWentDown is intentionally NOT set in iosTouchBegan.
    // It is only set when:
    //   • DRAG mode is committed in iosTouchMoved (cursor already at widget)
    //   • A quick tap ends in NEUTRAL in iosTouchEnded (classic click)
    // This prevents accidental widget activation during scroll classification
    // which previously caused sliders to move → DSP reconfigurations → CPU
    // spike → frame rate drop.
    void iosTouchBegan(double x, double y) {
        g_mouseX        = x;
        g_mouseY        = y;
        g_mouseDown     = true;
        g_mouseWentDown = false;   // delayed: set only when DRAG/tap is confirmed
        g_touchMode     = TouchMode::NEUTRAL;
        g_touchStartX   = x;
        g_touchStartY   = y;
        g_scrollLastY   = y;       // seed in case we go into SCROLL quickly

        // Panel-scroll detection: is this touch clearly inside the left-column
        // controls panel?  We deliberately avoid reading ImGui context here —
        // GImGui state is modified on the render thread and reading it from the
        // touch thread is a data race.  Instead, main_window.cpp registers the
        // panel width as an atomic every frame via iosSetMenuWidth(), and we do
        // a plain X-coordinate check.  A kPanelScrollDeadZone-pt strip at the
        // right edge is treated as "not panel" so touches near the
        // panel/waterfall boundary fall through to the waterfall gesture path.
        {
            float mw = g_menuWidth.load();
            g_scrollInPanel = (mw > 0.0f && (float)x < mw - kPanelScrollDeadZone);
        }
    }

    void iosTouchMoved(double x, double y) {
        // ---- Mode classification (once per gesture) -------------------------
        if (g_touchMode == TouchMode::NEUTRAL) {
            double totalDx = std::abs(x - g_touchStartX);
            double totalDy = std::abs(y - g_touchStartY);
            if (totalDx * totalDx + totalDy * totalDy >
                    kScrollThreshold * kScrollThreshold) {
                // In the left-column panel require a steeper angle before
                // committing to SCROLL (dy > 2·dx, ≈63°).  Controls there are
                // tapped or dragged horizontally; a loose dy>dx test (45°) was
                // hijacking small vertical slips on checkboxes and dropdowns.
                // Outside the panel the original dy>dx test is fine.
                bool clearlyVertical = g_scrollInPanel
                                       ? (totalDy > 2.0 * totalDx)
                                       : (totalDy > totalDx);
                if (clearlyVertical) {
                    // Primarily vertical drag.  Force DRAG mode for touches that
                    // started on a widget that needs it:
                    //   • near the FFT/waterfall horizontal resize divider
                    //   • inside the right WaterfallControls column (Zoom/Min/Max
                    //     vertical sliders cannot be adjusted in SCROLL mode)
                    float divY  = g_resizeDividerY.load();
                    float rpX   = g_rightPanelX.load();
                    bool nearDivider  = (divY >= 0.0f) &&
                                        (g_touchStartY >= divY - kResizeDividerHalf) &&
                                        (g_touchStartY <= divY + kResizeDividerHalf);
                    bool inRightPanel = (rpX < FLT_MAX / 2.0f) &&
                                        ((float)g_touchStartX >= rpX);
                    if (nearDivider || inRightPanel) {
                        g_touchMode     = TouchMode::DRAG;
                        g_mouseWentDown = true;
                    } else {
                        // Normal scroll: widget never sees a down event.
                        g_touchMode   = TouchMode::SCROLL;
                        g_scrollLastY = y;
                        g_mouseDown   = false;
                    }
                } else {
                    // Primarily horizontal drag → commit the delayed mouse-down
                    // now that we know this is a real drag (slider, waterfall...).
                    g_touchMode     = TouchMode::DRAG;
                    g_mouseWentDown = true;
                }
            }
        }

        // ---- Per-mode update ------------------------------------------------
        if (g_touchMode == TouchMode::SCROLL) {
            // Emit INCREMENTAL delta (y - lastY), not total displacement from
            // the gesture origin. Without this, each successive event scrolls
            // more than the last and the panel races away from the finger.
            // Keep the cursor X/Y fixed so the hovered panel stays locked.
            double dy = y - g_scrollLastY;
            g_scrollLastY = y;
            if (dy != 0.0) {
                if (g_scrollInPanel) {
                    // Panel scroll: accumulate raw δy for iosTakePanelScrollDelta().
                    // main_window.cpp applies this directly to the Left Column via
                    // SetScrollY — bypassing ImGui's wheel routing entirely so there
                    // is no ambiguity about which child window receives the event.
                    // Sign: -dy so that finger-up (dy < 0) produces a positive
                    // accumulator → Scroll.y increases → content scrolls down
                    // (iOS natural-scroll convention).
                    float cur = g_panelScrollAccum.load();
                    g_panelScrollAccum.store(cur + (float)(-dy) / kScrollDivisor);
                } else if (g_imguiInitialized) {
                    // Waterfall / right-panel: wheel event with the original sign.
                    // Waterfall frequency readers expect finger-up → positive wheel
                    // → higher frequency.
                    ImGui::GetIO().AddMouseWheelEvent(0.0f, -(float)dy / kScrollDivisor);
                }
            }
        } else {
            // NEUTRAL or DRAG: cursor tracks the finger.
            g_mouseX = x;
            g_mouseY = y;
        }
    }

    void iosTouchEnded(double x, double y) {
        if (g_touchMode == TouchMode::NEUTRAL) {
            // Quick tap — finger lifted before gesture threshold was reached.
            // Inject a click using the two-frame down→up pattern: set
            // g_mouseWentDown=true now (beginFrame injects DOWN next frame),
            // g_mouseDown=false (beginFrame injects UP the frame after that).
            g_mouseX        = x;
            g_mouseY        = y;
            g_mouseWentDown = true;
            g_mouseDown     = false;
        } else if (g_touchMode == TouchMode::DRAG) {
            // Release drag at the finger's final position.
            g_mouseX    = x;
            g_mouseY    = y;
            g_mouseDown = false;
        }
        // SCROLL: g_mouseDown was already cleared when scroll mode engaged;
        // g_mouseWentDown was never set so no click fires on lift.
        g_touchMode = TouchMode::NEUTRAL;
    }

    bool iosWantsKeyboard() { return g_imguiInitialized && ImGui::GetIO().WantTextInput; }

    // Called by the waterfall widget every frame to register the Y position of
    // its FFT/waterfall horizontal resize divider. Vertical touches that start
    // within kResizeDividerHalf pts of this line are classified as DRAG (not
    // SCROLL) so ImGui receives the mouse events needed to drive the resize.
    void iosSetResizeDividerY(float y) { g_resizeDividerY.store(y); }

    // Called every frame from main_window.cpp with the current left-column width
    // so iosTouchBegan can decide whether a touch is in the panel without reading
    // any ImGui context (which would be a cross-thread data race).
    void iosSetMenuWidth(float w) { g_menuWidth.store(w); }
    void iosSetRightPanelX(float x) { g_rightPanelX.store(x); }

    // Returns the accumulated panel-scroll delta since the last call and resets
    // the accumulator.  Called once per frame from main_window.cpp inside
    // BeginChild("Left Column") to apply the delta directly via SetScrollY.
    // Units: same as ImGui wheel ticks (positive = scroll content downward,
    // i.e. Scroll.y increases).
    float iosTakePanelScrollDelta() {
        return g_panelScrollAccum.exchange(0.0f);
    }

    // Pump a unicode codepoint typed into the host's UITextField shim into
    // ImGui's input stream. Called from UITextFieldDelegate methods.
    void iosTypeChar(unsigned codepoint) {
        if (!g_imguiInitialized) return;
        ImGui::GetIO().AddInputCharacter(codepoint);
    }

    void iosTypeBackspace() {
        if (!g_imguiInitialized) return;
        g_pendingBackspaces++;  // injected as DOWN+UP spread across two frames
    }

    void iosWheel(double dx, double dy) {
        if (!g_imguiInitialized) return;
        ImGui::GetIO().AddMouseWheelEvent((float)dx, (float)dy);
    }

    void iosRightClickAt(double x, double y) {
        if (!g_imguiInitialized) return;
        // Synthesise a right-button click at (x,y). One frame is enough — the
        // ImGui frame after this will see the down+up pair and open whatever
        // context menu the widget under the cursor registered.
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent((float)x, (float)y);
        io.AddMouseButtonEvent(1, true);
        io.AddMouseButtonEvent(1, false);
    }

    // Pan state lives here so the host doesn't have to track it. Two-finger
    // pan deltas accumulate into g_panX/g_panY, which a later patch can
    // wire into a waterfall-pan handler in main_window.cpp.
    // Dynamic textures used by waterfall / image / line_push_image widgets.
    // Strong ARC references prevent deallocation while the ImTextureID handle
    // (a non-owning __bridge void*) is still live in ImGui's draw lists.
    static NSMutableArray<id<MTLTexture>>* g_dynamicTextures = nil;

    void iosUpdateTexture(ImTextureID* texId, int w, int h, const void* rgba) {
        if (!g_device || w <= 0 || h <= 0) return;
        if (!g_dynamicTextures) g_dynamicTextures = [NSMutableArray new];

        // Check whether the existing texture can be reused (right size).
        if (*texId) {
            id<MTLTexture> existing = (__bridge id<MTLTexture>)*texId;
            if ((int)existing.width == w && (int)existing.height == h) {
                if (rgba) {
                    [existing replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
                                 mipmapLevel:0
                                   withBytes:rgba
                               bytesPerRow:(NSUInteger)(w * 4)];
                }
                return;
            }
            // Size changed — release old texture and fall through to create.
            [g_dynamicTextures removeObject:existing];
            *texId = nullptr;
        }

        // Create a new MTLTexture. MTLStorageModeShared allows CPU writes
        // (replaceRegion:) without a blit encoder — no extra synchronisation.
        MTLTextureDescriptor* desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:(NSUInteger)w
                                                              height:(NSUInteger)h
                                                           mipmapped:NO];
        desc.usage       = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared;

        id<MTLTexture> tex = [g_device newTextureWithDescriptor:desc];
        if (!tex) { flog::error("iosUpdateTexture: newTextureWithDescriptor failed ({}x{})", w, h); return; }

        if (rgba) {
            [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
                     mipmapLevel:0
                       withBytes:rgba
                     bytesPerRow:(NSUInteger)(w * 4)];
        }

        [g_dynamicTextures addObject:tex];   // ARC retention
        *texId = (__bridge void*)tex;        // non-owning ImTextureID
    }

    static double g_panX = 0.0, g_panY = 0.0;
    static bool   g_panActive = false;

    void iosPanBegan(double, double) { g_panX = 0.0; g_panY = 0.0; g_panActive = true; }
    void iosPanMoved(double dx, double dy) {
        if (!g_panActive) return;
        g_panX += dx;
        g_panY += dy;
        // No ImGui forwarding yet — see header comment.
    }
    void iosPanEnded() { g_panActive = false; }

    std::string iosAppFilesDir() { return g_appFilesDir; }
    void        iosSetAppFilesDir(const std::string& p) { g_appFilesDir = p; }

    // Expose the Metal device for Metal-aware code (icons_metal.mm, etc.)
    // that cannot include Metal.h from a plain C++ TU. Returns nil before
    // iosAttachView() has been called.
    void* iosGetMetalDevicePtr() { return (__bridge void*)g_device; }

    void iosPlayRecordingFile(const char* path, const std::atomic<bool>* stop_flag) {
        if (!path || !stop_flag) return;

        NSString* nsPath = [NSString stringWithUTF8String:path];
        NSURL* url = [NSURL fileURLWithPath:nsPath];
        NSError* err = nil;
        AVAudioPlayer* player = [[AVAudioPlayer alloc] initWithContentsOfURL:url error:&err];
        if (!player || err) {
            NSLog(@"[channel_bank] AVAudioPlayer init failed for %s: %@",
                  path, err.localizedDescription);
            return;
        }
        player.volume = 1.0f;
        if (![player play]) {
            NSLog(@"[channel_bank] AVAudioPlayer play returned NO for %s", path);
            return;
        }

        // Block the calling thread (playback thread) until the file finishes
        // playing or the module is being torn down (stop_flag cleared).
        // AVAudioPlayer drives its own AudioQueue worker thread; this loop
        // just waits without busying the CPU.
        while ([player isPlaying] && stop_flag->load(std::memory_order_relaxed)) {
            [NSThread sleepForTimeInterval:0.05]; // 50 ms poll
        }
        [player stop]; // idempotent if already finished
    }

    bool iosIsIPad() {
        return (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad);
    }

    bool iosTranscribeIsAvailable() {
        return ([SFSpeechRecognizer authorizationStatus] ==
                SFSpeechRecognizerAuthorizationStatusAuthorized);
    }

    void iosTranscribeRequestPermission() {
        // Must run on the main thread — dispatching ensures this is safe to call
        // from any thread (DSP thread, management thread, etc.).
        dispatch_async(dispatch_get_main_queue(), ^{
            [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus) {}];
        });
    }

    void* iosTranscribeCreate(double sampleRate) {
        SFSpeechRecognizerAuthorizationStatus status = [SFSpeechRecognizer authorizationStatus];
        if (status == SFSpeechRecognizerAuthorizationStatusNotDetermined) {
            // Trigger permission dialog (dispatched to main thread — non-blocking).
            iosTranscribeRequestPermission();
            return nullptr;  // Will succeed on next recording once user grants permission
        }
        if (status != SFSpeechRecognizerAuthorizationStatusAuthorized) return nullptr;
        CBTranscriptionSession* s = [[CBTranscriptionSession alloc] initWithSampleRate:sampleRate];
        if (!s) return nullptr;
        return (__bridge_retained void*)s;
    }

    void iosTranscribeAppend(void* handle, const float* samples, int count) {
        if (!handle) return;
        [(__bridge CBTranscriptionSession*)handle appendSamples:samples count:count];
    }

    void iosTranscribeEndAudio(void* handle) {
        if (!handle) return;
        [(__bridge CBTranscriptionSession*)handle endAudio];
    }

    void iosTranscribeCancel(void* handle) {
        if (!handle) return;
        [(__bridge CBTranscriptionSession*)handle cancel];
    }

    std::string iosTranscribeGetText(void* handle) {
        if (!handle) return {};
        NSString* t = [(__bridge CBTranscriptionSession*)handle latestText];
        return t ? std::string(t.UTF8String) : std::string();
    }

    bool iosTranscribeIsFinal(void* handle) {
        if (!handle) return false;
        return [(__bridge CBTranscriptionSession*)handle isFinal] == YES;
    }

    void iosTranscribeDestroy(void* handle) {
        if (!handle) return;
        CBTranscriptionSession* s = (__bridge_transfer CBTranscriptionSession*)handle;
        [s cancel];
        (void)s; // ARC releases
    }
}
