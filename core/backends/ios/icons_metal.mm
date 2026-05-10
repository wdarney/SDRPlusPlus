// iOS / Metal icon loader — replaces core/src/gui/icons.cpp in iOS builds.
//
// On iOS we render via the ImGui Metal backend which expects ImTextureID to
// be an id<MTLTexture> wrapped via __bridge. The desktop icons.cpp uses
// glGenTextures / glTexImage2D (OpenGL), which are undefined without an
// active GL context. This file loads PNG icons directly into MTLTextures so
// ImageButton / Image calls work correctly on Metal.
//
// CMakeLists.txt excludes icons.cpp from iOS builds; this .mm is swept in
// by the "backends/ios/*.mm" glob. The STB_IMAGE_IMPLEMENTATION define
// lives here (not in icons.cpp) for iOS builds.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <gui/icons.h>       // declares icons::LOGO, PLAY, …, icons::load()
#include "ios_backend.h"     // backend::iosGetMetalDevicePtr()
#include <utils/flog.h>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#include <imgui/stb_image.h>

// Strong references so ARC keeps the textures alive for the app lifetime.
// (We __bridge-cast to void* / ImTextureID; the retain-side lives here.)
static NSMutableArray<id<MTLTexture>>* g_iconTextures = nil;

namespace icons {
    // Variable definitions (declared extern in icons.h)
    ImTextureID LOGO;
    ImTextureID PLAY;
    ImTextureID STOP;
    ImTextureID MENU;
    ImTextureID MUTED;
    ImTextureID UNMUTED;
    ImTextureID NORMAL_TUNING;
    ImTextureID CENTER_TUNING;

    // Stub kept for any code that calls loadTexture() directly.
    // On iOS, always use loadTextureMetal() / icons::load().
    GLuint loadTexture(std::string /*path*/) { return 0; }

    static ImTextureID loadTextureMetal(id<MTLDevice> device,
                                        const std::string& path) {
        int w = 0, h = 0, channels = 0;
        // Force RGBA so we always get 4 bytes/pixel for MTLPixelFormatRGBA8Unorm.
        stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
        if (!pixels) {
            flog::error("icons_metal: stbi_load failed for {}", path);
            return nullptr;
        }

        MTLTextureDescriptor* desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:(NSUInteger)w
                                                              height:(NSUInteger)h
                                                           mipmapped:NO];
        desc.usage       = MTLTextureUsageShaderRead;
        desc.storageMode = MTLStorageModeShared; // writable from CPU, readable by GPU

        id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
        if (!tex) {
            flog::error("icons_metal: newTextureWithDescriptor failed for {}", path);
            stbi_image_free(pixels);
            return nullptr;
        }

        [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
               mipmapLevel:0
                 withBytes:pixels
               bytesPerRow:(NSUInteger)(w * 4)];
        stbi_image_free(pixels);

        // Retain in g_iconTextures so ARC doesn't release the object while
        // we hold a __bridge (non-retained) ImTextureID pointer to it.
        [g_iconTextures addObject:tex];

        // ImGui Metal backend does: (__bridge id<MTLTexture>)(tex_id)
        // so we need a __bridge (non-owning) cast here — ownership is in
        // g_iconTextures above.
        return (__bridge void*)tex;
    }

    bool load(std::string resDir) {
        if (!std::filesystem::is_directory(resDir)) {
            flog::error("icons_metal: invalid resource directory: {}", resDir);
            return false;
        }

        id<MTLDevice> device =
            (__bridge id<MTLDevice>)backend::iosGetMetalDevicePtr();
        if (!device) {
            flog::error("icons_metal: Metal device not available (call iosAttachView first)");
            return false;
        }

        g_iconTextures = [NSMutableArray new];

        LOGO          = loadTextureMetal(device, resDir + "/icons/sdrpp.png");
        PLAY          = loadTextureMetal(device, resDir + "/icons/play.png");
        STOP          = loadTextureMetal(device, resDir + "/icons/stop.png");
        MENU          = loadTextureMetal(device, resDir + "/icons/menu.png");
        MUTED         = loadTextureMetal(device, resDir + "/icons/muted.png");
        UNMUTED       = loadTextureMetal(device, resDir + "/icons/unmuted.png");
        NORMAL_TUNING = loadTextureMetal(device, resDir + "/icons/normal_tuning.png");
        CENTER_TUNING = loadTextureMetal(device, resDir + "/icons/center_tuning.png");

        flog::info("icons_metal: loaded {} icon textures", (int)[g_iconTextures count]);
        return true;
    }
}
