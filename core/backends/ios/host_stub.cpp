// Host-build stub. Provides empty implementations of the backend interface so
// the rest of the codebase compiles when building on macOS / Linux without an
// iOS SDK. Real iOS builds use backend.mm — see core/CMakeLists.txt.

#include <backend.h>
#include "ios_backend.h"

namespace backend {
    int  init(std::string)       { return 0; }
    bool isRenderThread()        { return true; }
    void setFrameRateLimit(int)  {}
    int  getFrameRateLimit()     { return 0; }
    void beginFrame()            {}
    void render(bool)            {}
    void getMouseScreenPos(double& x, double& y) { x = 0; y = 0; }
    void setMouseScreenPos(double, double)       {}
    int  renderLoop()            { return 0; }
    int  end()                   { return 0; }

    void iosAttachView(void*)                 {}
    void iosDrawFrame()                       {}
    void iosResize(double, double, double)    {}
    void iosTouchBegan(double, double)        {}
    void iosTouchMoved(double, double)        {}
    void iosTouchEnded(double, double)        {}
    bool iosWantsKeyboard()                   { return false; }
    void iosTypeChar(unsigned)                {}
    void iosTypeBackspace()                   {}
    void iosWheel(double, double)             {}
    void iosRightClickAt(double, double)      {}
    void iosPanBegan(double, double)          {}
    void iosPanMoved(double, double)          {}
    void iosPanEnded()                        {}

    void iosSetMainWindowReady()               {}

    static std::string g_appFilesDir;
    std::string iosAppFilesDir()              { return g_appFilesDir; }
    void        iosSetAppFilesDir(const std::string& p) { g_appFilesDir = p; }
    void*       iosGetMetalDevicePtr()        { return nullptr; }

    // iosUpdateTexture / iosSetResizeDividerY are only called from
    // #if TARGET_OS_IPHONE blocks, so these stubs are never reached on non-iOS
    // builds — they exist solely so the symbols resolve at link time.
    void iosUpdateTexture(ImTextureID*, int, int, const void*) {}
    void iosSetResizeDividerY(float) {}
    void iosSetMenuWidth(float) {}
    void iosSetRightPanelX(float) {}
    float iosTakePanelScrollDelta() { return 0.0f; }

    bool iosIsIPad() { return false; }
    void iosPlayRecordingFile(const char*, const std::atomic<bool>*) {}
}
