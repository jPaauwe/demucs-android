// JNI binding for android/kotlin/NativeEngine.kt. The methods are registered
// on SDENGINE_JNI_CLASS (set it in CMake to your own class name).
#include "engine.hpp"

#include <android/log.h>
#include <jni.h>

#include <ctime>
#include <limits>
#include <memory>
#include <string>

#ifndef SDENGINE_JNI_CLASS
#define SDENGINE_JNI_CLASS "io/github/jpaauwe/demucsengine/NativeEngine"
#endif

namespace
{
std::string str(JNIEnv *env, jstring s)
{
    if (!s)
        return {};
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string out(c ? c : "");
    if (c)
        env->ReleaseStringUTFChars(s, c);
    return out;
}

void throwNew(JNIEnv *env, const char *cls, const std::string &msg)
{
    if (jclass c = env->FindClass(cls))
        env->ThrowNew(c, msg.c_str());
}

// Wraps the Kotlin ProgressListener; calls are throttled to ~10 per second.
sdengine::ProgressFn progress(JNIEnv *env, jobject listener)
{
    if (!listener)
        return [](float, const char *) { return true; };
    jclass cls = env->GetObjectClass(listener);
    jmethodID mid = env->GetMethodID(cls, "onProgress", "(FLjava/lang/String;)Z");
    auto last = std::make_shared<long long>(0);
    return [env, listener, mid, last](float frac, const char *stage) -> bool {
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const long long now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
        if (now - *last < 100 && frac < 1.0f)
            return true;
        *last = now;
        jstring jstage = env->NewStringUTF(stage);
        const jboolean keep = env->CallBooleanMethod(listener, mid, frac, jstage);
        env->DeleteLocalRef(jstage);
        if (env->ExceptionCheck())
        {
            env->ExceptionClear();
            return false;
        }
        return keep == JNI_TRUE;
    };
}

jstring nLoadModel(JNIEnv *env, jclass, jstring path)
{
    return env->NewStringUTF(sdengine::load_model(str(env, path)).c_str());
}

jstring nPrepareInput(JNIEnv *env, jclass, jstring raw, jint rate, jint channels, jstring out, jobject listener)
{
    const std::string err =
        sdengine::prepare_input(str(env, raw), rate, channels, str(env, out), progress(env, listener));
    if (err == "cancelled")
    {
        throwNew(env, "java/util/concurrent/CancellationException", "cancelled");
        return nullptr;
    }
    return env->NewStringUTF(err.c_str());
}

jdoubleArray nAnalyze(JNIEnv *env, jclass, jstring path)
{
    const sdengine::InputStats st = sdengine::analyze_input(str(env, path));
    const double nan = std::numeric_limits<double>::quiet_NaN();
    jdouble vals[3] = {st.durationSec, st.hasPeak ? st.peakDb : nan, st.hasLufs ? st.lufs : nan};
    jdoubleArray arr = env->NewDoubleArray(3);
    env->SetDoubleArrayRegion(arr, 0, 3, vals);
    return arr;
}

jdoubleArray nSeparate(JNIEnv *env, jclass, jstring prepared, jstring outDir, jbooleanArray write, jint threads,
                       jint computeThreads, jboolean gpu, jobject listener)
{
    sdengine::SeparateOptions opts;
    opts.threads = threads;
    opts.computeThreads = computeThreads;
    opts.gpu = gpu == JNI_TRUE;
    if (write && env->GetArrayLength(write) == sdengine::kNumSources)
    {
        jboolean w[sdengine::kNumSources];
        env->GetBooleanArrayRegion(write, 0, sdengine::kNumSources, w);
        for (int i = 0; i < sdengine::kNumSources; ++i)
            opts.write[i] = w[i] == JNI_TRUE;
    }
    const sdengine::SeparateResult r =
        sdengine::separate(str(env, prepared), str(env, outDir), opts, progress(env, listener));
    if (r.cancelled)
    {
        throwNew(env, "java/util/concurrent/CancellationException", "cancelled");
        return nullptr;
    }
    if (!r.ok)
    {
        throwNew(env, "java/lang/RuntimeException", r.error.empty() ? "Separation failed." : r.error);
        return nullptr;
    }
    jdoubleArray arr = env->NewDoubleArray(sdengine::kNumSources);
    env->SetDoubleArrayRegion(arr, 0, sdengine::kNumSources, r.rms.data());
    return arr;
}

jstring nComputeInfo(JNIEnv *env, jclass, jint computeThreads, jboolean gpu)
{
    sdengine::SeparateOptions opts;
    opts.computeThreads = computeThreads;
    opts.gpu = gpu == JNI_TRUE;
    return env->NewStringUTF(sdengine::compute_info(opts).c_str());
}

void nCancel(JNIEnv *, jclass) { sdengine::request_cancel(); }

const JNINativeMethod kMethods[] = {
    {"nativeLoadModel", "(Ljava/lang/String;)Ljava/lang/String;", (void *)nLoadModel},
    {"nativePrepareInput",
     "(Ljava/lang/String;IILjava/lang/String;L" SDENGINE_JNI_CLASS "$ProgressListener;)Ljava/lang/String;",
     (void *)nPrepareInput},
    {"nativeAnalyze", "(Ljava/lang/String;)[D", (void *)nAnalyze},
    {"nativeSeparate",
     "(Ljava/lang/String;Ljava/lang/String;[ZIIZL" SDENGINE_JNI_CLASS "$ProgressListener;)[D",
     (void *)nSeparate},
    {"nativeComputeInfo", "(IZ)Ljava/lang/String;", (void *)nComputeInfo},
    {"nativeCancel", "()V", (void *)nCancel},
};
} // namespace

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *)
{
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;
    jclass cls = env->FindClass(SDENGINE_JNI_CLASS);
    if (!cls || env->RegisterNatives(cls, kMethods, sizeof(kMethods) / sizeof(kMethods[0])) != 0)
    {
        __android_log_print(ANDROID_LOG_ERROR, "sdengine", "could not register natives on %s", SDENGINE_JNI_CLASS);
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}
