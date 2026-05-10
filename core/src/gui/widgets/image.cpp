#include <gui/widgets/image.h>
#include <ios_backend.h>

namespace ImGui {
    ImageDisplay::ImageDisplay(int width, int height) {
        _width = width;
        _height = height;
        buffer = malloc(_width * _height * 4);
        activeBuffer = malloc(_width * _height * 4);
        memset(buffer, 0, _width * _height * 4);
        memset(activeBuffer, 0, _width * _height * 4);

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        textureId = nullptr; // created lazily in updateTexture()
#else
        GLuint glId;
        glGenTextures(1, &glId);
        textureId = (ImTextureID)(intptr_t)glId;
#endif
    }

    ImageDisplay::~ImageDisplay() {
        free(buffer);
        free(activeBuffer);
    }

    void ImageDisplay::draw(const ImVec2& size_arg) {
        std::lock_guard<std::mutex> lck(bufferMtx);

        ImGuiWindow* window = GetCurrentWindow();
        ImGuiStyle& style = GetStyle();
        ImVec2 min = window->DC.CursorPos;

        // Calculate scale
        float width = CalcItemWidth();
        float height = roundf((width / (float)_width) * (float)_height);

        ImVec2 size = CalcItemSize(size_arg, CalcItemWidth(), height);
        ImRect bb(min, ImVec2(min.x + size.x, min.y + size.y));

        ItemSize(size, style.FramePadding.y);
        if (!ItemAdd(bb, 0)) {
            return;
        }

        if (newData) {
            newData = false;
            updateTexture();
        }

        window->DrawList->AddImage(textureId, min, ImVec2(min.x + width, min.y + height));
    }

    void ImageDisplay::swap() {
        std::lock_guard<std::mutex> lck(bufferMtx);
        void* tmp = activeBuffer;
        activeBuffer = buffer;
        buffer = tmp;
        newData = true;
        memset(buffer, 0, _width * _height * 4);
    }

    void ImageDisplay::updateTexture() {
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
        backend::iosUpdateTexture(&textureId, _width, _height, activeBuffer);
#else
        GLuint glId = (GLuint)(intptr_t)textureId;
        glBindTexture(GL_TEXTURE_2D, glId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, _width, _height, 0, GL_RGBA, GL_UNSIGNED_BYTE, activeBuffer);
#endif
    }

}