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
    static bool                   g_imguiInitialized  = false;
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
        io.DisplayFramebufferScale = ImVec2((float)[UIScreen mainScreen].nativeScale,
                                            (float)[UIScreen mainScreen].nativeScale);

        ImGui_ImplMetal_Init(g_device);
        g_imguiInitialized = true;
        return 0;
    }

    void beginFrame() {
        if (!g_mtkView || !g_renderPassDesc) return;
        ImGui_ImplMetal_NewFrame(g_renderPassDesc);

        ImGuiIO& io = ImGui::GetIO();
        CGSize sz = g_mtkView.drawableSize;
        io.DisplaySize = ImVec2((float)sz.width / io.DisplayFramebufferScale.x,
                                (float)sz.height / io.DisplayFramebufferScale.y);
        io.DeltaTime   = 1.0f / 60.0f; // TODO: real delta from timestamps

        io.AddMousePosEvent((float)g_mouseX, (float)g_mouseY);
        io.AddMouseButtonEvent(0, g_mouseDown);

        ImGui::NewFrame();
    }

    void render(bool /*vsync*/) {
        ImGui::Render();

        id<CAMetalDrawable> drawable = g_mtkView.currentDrawable;
        if (!drawable) return;

        id<MTLCommandBuffer> cb = [g_commandQueue commandBuffer];
        g_renderPassDesc.colorAttachments[0].clearColor =
            MTLClearColorMake(gui::themeManager.clearColor.x,
                              gui::themeManager.clearColor.y,
                              gui::themeManager.clearColor.z,
                              gui::themeManager.clearColor.w);
        g_renderPassDesc.colorAttachments[0].texture     = drawable.texture;
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
        // The UIApplicationMain runloop drives drawing via MTKViewDelegate ->
        // iosDrawFrame(). Block here until the app is requested to exit; iOS
        // apps don't really exit, so this just parks the calling thread.
        flog::info("iOS renderLoop parked (UIApplication drives draws)");
        [[NSRunLoop currentRunLoop] run];
        return 0;
    }

    int end() {
        if (g_imguiInitialized) {
            ImGui_ImplMetal_Shutdown();
            ImGui::DestroyContext();
            g_imguiInitialized = false;
        }
        return 0;
    }

    void iosAttachView(void* mtkViewPtr) {
        g_mtkView        = (__bridge MTKView*)mtkViewPtr;
        g_device         = g_mtkView.device;
        g_commandQueue   = [g_device newCommandQueue];
        g_renderPassDesc = [MTLRenderPassDescriptor new];
    }

    void iosDrawFrame() {
        if (!g_imguiInitialized) return;
        beginFrame();
        ImGuiIO& io = ImGui::GetIO();
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
}
