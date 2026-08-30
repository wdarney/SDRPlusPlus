#include <android_ble_gatt.h>

#include <android_native_app_glue.h>
#include <jni.h>
#include <atomic>
#include <mutex>
#include <utility>

namespace backend {
    extern struct android_app* app;
}

namespace {

std::mutex handlerMtx;
android_ble_gatt::RequestHandler requestHandler;
std::atomic<bool> stateSubscribers{false};
std::atomic<bool> audioSubscribers{false};

template <typename Fn>
void withActivity(Fn&& fn) {
    if (!backend::app || !backend::app->activity || !backend::app->activity->vm) return;
    JavaVM* vm = backend::app->activity->vm;
    JNIEnv* env = nullptr;
    bool attached = false;
    jint status = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    else if (status != JNI_OK) {
        return;
    }
    fn(env, backend::app->activity->clazz);
    if (attached) vm->DetachCurrentThread();
}

void callVoid(const char* name) {
    withActivity([name](JNIEnv* env, jobject activity) {
        jclass cls = env->GetObjectClass(activity);
        jmethodID method = cls ? env->GetMethodID(cls, name, "()V") : nullptr;
        if (method) env->CallVoidMethod(activity, method);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (cls) env->DeleteLocalRef(cls);
    });
}

}

namespace android_ble_gatt {

void registerRequestHandler(RequestHandler handler) {
    std::lock_guard<std::mutex> lock(handlerMtx);
    requestHandler = std::move(handler);
}

void unregisterRequestHandler() {
    std::lock_guard<std::mutex> lock(handlerMtx);
    requestHandler = nullptr;
    stateSubscribers.store(false);
    audioSubscribers.store(false);
}

void start() { callVoid("startChannelBankGatt"); }
void stop() { callVoid("stopChannelBankGatt"); }

bool hasStateSubscribers() { return stateSubscribers.load(); }
bool hasAudioSubscribers() { return audioSubscribers.load(); }

void notifyState(const std::string& json) {
    if (!hasStateSubscribers()) return;
    withActivity([&json](JNIEnv* env, jobject activity) {
        jclass cls = env->GetObjectClass(activity);
        jmethodID method = cls ? env->GetMethodID(
            cls, "notifyChannelBankGattState", "(Ljava/lang/String;)V") : nullptr;
        if (method) {
            jstring value = env->NewStringUTF(json.c_str());
            env->CallVoidMethod(activity, method, value);
            env->DeleteLocalRef(value);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (cls) env->DeleteLocalRef(cls);
    });
}

void publishAudio(const int16_t* samples, size_t count) {
    if (!samples || count == 0 || !hasAudioSubscribers()) return;
    withActivity([samples, count](JNIEnv* env, jobject activity) {
        jclass cls = env->GetObjectClass(activity);
        jmethodID method = cls ? env->GetMethodID(
            cls, "publishChannelBankGattAudio", "([B)V") : nullptr;
        if (method) {
            size_t byteCount = count * sizeof(int16_t);
            jbyteArray value = env->NewByteArray(static_cast<jsize>(byteCount));
            env->SetByteArrayRegion(value, 0, static_cast<jsize>(byteCount),
                                    reinterpret_cast<const jbyte*>(samples));
            env->CallVoidMethod(activity, method, value);
            env->DeleteLocalRef(value);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (cls) env->DeleteLocalRef(cls);
    });
}

}

extern "C" JNIEXPORT jstring JNICALL
Java_org_sdrpp_sdrpp_MainActivity_nativeChannelBankGattRequest(
    JNIEnv* env, jobject, jstring request) {
    const char* chars = request ? env->GetStringUTFChars(request, nullptr) : nullptr;
    std::string input = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(request, chars);

    android_ble_gatt::RequestHandler handler;
    {
        std::lock_guard<std::mutex> lock(handlerMtx);
        handler = requestHandler;
    }
    std::string output = handler
        ? handler(input)
        : R"({"v":1,"id":0,"ok":false,"status":503,"error":{"code":"unavailable","message":"Channel Bank is not loaded"}})";
    return env->NewStringUTF(output.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_org_sdrpp_sdrpp_MainActivity_nativeChannelBankGattSubscriptionChanged(
    JNIEnv*, jobject, jboolean state, jboolean audio) {
    stateSubscribers.store(state == JNI_TRUE);
    audioSubscribers.store(audio == JNI_TRUE);
}

namespace android_ble_gatt {

bool registerNativeMethods() {
    bool registered = false;
    withActivity([&registered](JNIEnv* env, jobject activity) {
        jclass cls = env->GetObjectClass(activity);
        if (!cls) return;

        JNINativeMethod methods[] = {
            {
                const_cast<char*>("nativeChannelBankGattRequest"),
                const_cast<char*>("(Ljava/lang/String;)Ljava/lang/String;"),
                reinterpret_cast<void*>(
                    Java_org_sdrpp_sdrpp_MainActivity_nativeChannelBankGattRequest)
            },
            {
                const_cast<char*>("nativeChannelBankGattSubscriptionChanged"),
                const_cast<char*>("(ZZ)V"),
                reinterpret_cast<void*>(
                    Java_org_sdrpp_sdrpp_MainActivity_nativeChannelBankGattSubscriptionChanged)
            }
        };
        registered = env->RegisterNatives(cls, methods, 2) == JNI_OK;
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(cls);
    });
    return registered;
}

}
