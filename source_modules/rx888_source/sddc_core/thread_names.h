#pragma once

#if defined(__ANDROID__)
#include <pthread.h>

inline void rx888_set_thread_name(const char* name) {
    if (name) {
        pthread_setname_np(pthread_self(), name);
    }
}
#else
inline void rx888_set_thread_name(const char*) {}
#endif
