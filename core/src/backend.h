#pragma once
#include <string>

namespace backend {
    int init(std::string resDir = "");
    void beginFrame();
    void render(bool vsync = true);
    void setFrameRateLimit(int fps);
    int getFrameRateLimit();
    void getMouseScreenPos(double& x, double& y);
    void setMouseScreenPos(double x, double y);
    int renderLoop();
    int end();
    // iOS renders on the MTKView delegate thread. Desktop and Android
    // backends always return true.
    bool isRenderThread();
}
