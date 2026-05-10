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

    // Single-touch -> ImGui mouse cursor, matching the Android approach.
    static bool   g_mouseDown = false;
    static double g_mouseX    = 0.0;
    static double g_mouseY    = 0.0;

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
        io.AddMouseButtonEvent(0, g_mouseDown);

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
        if (io.DisplaySize.x > 0 && io.DisplaySize.y > 0) {
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            gui::mainWindow.draw();
        }
        render();
    }

    void iosResize(double w, double h, double /*scale*/) {
        if (g_mtkView) g_mtkView.drawableSize = CGSizeMake(w, h);
    }

    void iosTouchBegan(double x, double y) { g_mouseX = x; g_mouseY = y; g_mouseDown = true; }
    void iosTouchMoved(double x, double y) { g_mouseX = x; g_mouseY = y; }
    void iosTouchEnded(double x, double y) { g_mouseX = x; g_mouseY = y; g_mouseDown = false; }

    bool iosWantsKeyboard() { return g_imguiInitialized && ImGui::GetIO().WantTextInput; }

    // Pump a unicode codepoint typed into the host's UITextField shim into
    // ImGui's input stream. Called from UITextFieldDelegate methods.
    void iosTypeChar(unsigned codepoint) {
        if (!g_imguiInitialized) return;
        ImGui::GetIO().AddInputCharacter(codepoint);
    }

    void iosTypeBackspace() {
        if (!g_imguiInitialized) return;
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, true);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Backspace, false);
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
}
