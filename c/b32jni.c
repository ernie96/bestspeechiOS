/* b32jni -- JNI bridge exposing b32emu to an Android TextToSpeechService.
 *
 * The Java side owns threading: TextToSpeechService serializes calls to
 * onSynthesizeText, so one native engine per service instance is enough. Each
 * captured buffer is handed straight back to Java, which forwards it to
 * SynthesisCallback.audioAvailable() -- no full-utterance buffering, so speech
 * starts playing while the rest is still being synthesized.
 *
 * Public domain.
 */
#include "b32emu.h"

#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include <android/log.h>
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "b32tts", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "b32tts", __VA_ARGS__)

#define CLASS "com/bestspeech/tts/B32Native"

/* Per-synthesis context for the streaming callback. */
typedef struct {
    JNIEnv    *env;
    jobject    sink;        /* the Java object with onAudio([B)Z */
    jmethodID  on_audio;
    jbyteArray scratch;
    jsize      scratch_len;
    int        failed;
} sink_ctx;

static int emit(const int16_t *pcm, size_t bytes, void *user) {
    sink_ctx *c = (sink_ctx *)user;
    if (c->failed || bytes == 0) return !c->failed;

    if (c->scratch == NULL || c->scratch_len < (jsize)bytes) {
        if (c->scratch) (*c->env)->DeleteLocalRef(c->env, c->scratch);
        c->scratch = (*c->env)->NewByteArray(c->env, (jsize)bytes);
        if (!c->scratch) { c->failed = 1; return 0; }
        c->scratch_len = (jsize)bytes;
    }
    (*c->env)->SetByteArrayRegion(c->env, c->scratch, 0, (jsize)bytes,
                                  (const jbyte *)pcm);
    jboolean keep = (*c->env)->CallBooleanMethod(c->env, c->sink, c->on_audio,
                                                 c->scratch, (jint)bytes);
    if ((*c->env)->ExceptionCheck(c->env)) {
        (*c->env)->ExceptionClear(c->env);
        c->failed = 1;
        return 0;
    }
    return keep == JNI_TRUE;
}

static jlong JNICALL n_open(JNIEnv *env, jclass cls, jbyteArray dll) {
    (void)cls;
    jsize n = (*env)->GetArrayLength(env, dll);
    jbyte *bytes = (*env)->GetByteArrayElements(env, dll, NULL);
    if (!bytes) return 0;

    char err[256] = {0};
    b32_emu *e = b32_open(bytes, (size_t)n, err, sizeof err);
    (*env)->ReleaseByteArrayElements(env, dll, bytes, JNI_ABORT);
    if (!e) {
        LOGE("b32_open failed: %s", err);
        jclass rt = (*env)->FindClass(env, "java/lang/RuntimeException");
        if (rt) (*env)->ThrowNew(env, rt, err);
        return 0;
    }
    int nvoices = 0;
    b32_voices(&nvoices);
    LOGI("engine ready (%d voices)", nvoices);
    return (jlong)(intptr_t)e;
}

static void JNICALL n_close(JNIEnv *env, jclass cls, jlong h) {
    (void)env; (void)cls;
    b32_close((b32_emu *)(intptr_t)h);
}

static void JNICALL n_set_rate(JNIEnv *env, jclass cls, jlong h, jint rate) {
    (void)env; (void)cls;
    if (h) b32_set_rate((b32_emu *)(intptr_t)h, (int)rate);
}

static void JNICALL n_set_gain(JNIEnv *env, jclass cls, jlong h, jint gain) {
    (void)env; (void)cls;
    if (h) b32_set_gain((b32_emu *)(intptr_t)h, (int)gain);
}

static void JNICALL n_set_speed(JNIEnv *env, jclass cls, jlong h, jfloat speed) {
    (void)env; (void)cls;
    if (h) b32_set_speed((b32_emu *)(intptr_t)h, (float)speed);
}

static jint JNICALL n_voice_count(JNIEnv *env, jclass cls) {
    (void)env; (void)cls;
    int n = 0;
    b32_voices(&n);
    return n;
}

static jstring JNICALL n_voice_name(JNIEnv *env, jclass cls, jint i) {
    (void)cls;
    int n = 0;
    const b32_voice *v = b32_voices(&n);
    if (i < 0 || i >= n) return NULL;
    return (*env)->NewStringUTF(env, v[i].name);
}

/* The voice's ~f baseline, so Java can scale it for Android's pitch control. */
static jint JNICALL n_voice_base_hz(JNIEnv *env, jclass cls, jint i) {
    (void)env; (void)cls;
    int n = 0;
    const b32_voice *v = b32_voices(&n);
    if (i < 0 || i >= n) return 0;
    return v[i].base_hz;
}

/* Streams PCM to sink.onAudio(byte[], int) -> boolean. Returns 0 on success. */
static jint JNICALL n_speak(JNIEnv *env, jclass cls, jlong h,
                            jstring jtext, jstring jvoice, jobject sink) {
    (void)cls;
    b32_emu *e = (b32_emu *)(intptr_t)h;
    if (!e || !jtext || !sink) return -1;

    const char *text = (*env)->GetStringUTFChars(env, jtext, NULL);
    if (!text) return -1;

    const b32_voice *voice = NULL;
    const char *vname = NULL;
    if (jvoice) {
        vname = (*env)->GetStringUTFChars(env, jvoice, NULL);
        if (vname) voice = b32_voice_by_name(vname);
    }

    jclass sk = (*env)->GetObjectClass(env, sink);
    sink_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.env = env;
    ctx.sink = sink;
    ctx.on_audio = (*env)->GetMethodID(env, sk, "onAudio", "([BI)Z");

    jint rc = -1;
    if (ctx.on_audio) {
        rc = voice ? b32_speak_voice(e, voice, text, emit, &ctx)
                   : b32_speak(e, text, emit, &ctx);
        if (rc != 0) LOGE("synthesis failed: %s", b32_error(e));
    } else {
        LOGE("sink is missing onAudio([BI)Z");
    }

    if (ctx.scratch) (*env)->DeleteLocalRef(env, ctx.scratch);
    if (vname) (*env)->ReleaseStringUTFChars(env, jvoice, vname);
    (*env)->ReleaseStringUTFChars(env, jtext, text);
    return ctx.failed ? -1 : rc;
}

static const JNINativeMethod METHODS[] = {
    { "open",       "([B)J",                    (void *)n_open },
    { "close",      "(J)V",                     (void *)n_close },
    { "setRate",    "(JI)V",                    (void *)n_set_rate },
    { "setGain",    "(JI)V",                    (void *)n_set_gain },
    { "setSpeed",   "(JF)V",                    (void *)n_set_speed },
    { "voiceCount", "()I",                      (void *)n_voice_count },
    { "voiceName",  "(I)Ljava/lang/String;",    (void *)n_voice_name },
    { "voiceBaseHz", "(I)I",                    (void *)n_voice_base_hz },
    { "speak",      "(JLjava/lang/String;Ljava/lang/String;L" CLASS "$Sink;)I",
                                                (void *)n_speak },
};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return -1;
    jclass cls = (*env)->FindClass(env, CLASS);
    if (!cls) { LOGE("cannot find " CLASS); return -1; }
    if ((*env)->RegisterNatives(env, cls, METHODS,
                                (jint)(sizeof METHODS / sizeof METHODS[0])) != 0) {
        LOGE("RegisterNatives failed");
        return -1;
    }
    return JNI_VERSION_1_6;
}
