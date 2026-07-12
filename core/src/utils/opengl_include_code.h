#pragma once

// iOS fork: we render via Metal (see core/backends/ios/), not OpenGL. But
// several widgets (icons, image, waterfall, line_push_image) include this
// header to get GL types like GLuint that they store as texture handles.
// On iOS we point those at OpenGLES — the headers exist in the iOS SDK and
// give us GLuint/GLint typedefs without dragging in any GL.framework. The
// Metal backend ignores the texture-handle path and renders ImGui's textures
// through MTLTexture instead.

#if defined(__APPLE__)
  #include <TargetConditionals.h>
  #if TARGET_OS_IPHONE
    #include <OpenGLES/ES3/gl.h>
  #else
    #include <OpenGL/gl.h>
  #endif
#elif defined(_WIN32)
  #include <windows.h>
  #include <GL/gl.h>
#elif defined(__ANDROID__)
  #include <EGL/egl.h>
  #include <GLES3/gl3.h>
#else
  #include <GL/gl.h>
#endif
