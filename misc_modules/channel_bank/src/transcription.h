#pragma once
#ifdef __APPLE__
#include <string>

namespace transcription {
    enum class AuthStatus { NotConfigured, NotDetermined, Denied, Authorized };
    AuthStatus  authStatus();
    bool        isAvailable();   // true only when Authorized
    void        requestPermission();
    void        openSystemSettings(); // open Privacy > Speech Recognition in System Settings
    void*       transcribeFile(const char* path); // returns opaque handle; nullptr = failed
    void        cancel(void* handle);
    std::string getText(void* handle);            // latest partial or final text
    bool        isFinal(void* handle);
    void        destroy(void* handle);
}
#endif
