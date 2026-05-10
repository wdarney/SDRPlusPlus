#pragma once
#include <string>

namespace backend {
    int init(std::string resDir = "");
    void beginFrame();
    void render(bool vsync = true);
    void getMouseScreenPos(double& x, double& y);
    void setMouseScreenPos(double x, double y);
    int renderLoop();
    int end();

    // Returns true when it is safe to call beginFrame()/render() to produce a
    // visible frame. On iOS this is the main thread (MTKView delegate thread);
    // on all other platforms it is always true.
    bool isRenderThread();
}