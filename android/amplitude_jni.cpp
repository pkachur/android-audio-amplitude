// amplitude_jni.cpp — JNI-обёртка: аудиофайл -> int[] значений амплитуды.
//
// Собирается в libamplitude.so, вызывается из Kotlin/Java (см. Amplitude.kt).
// Вся логика декодирования и свёртки — общая с CLI (src/), здесь только мост.
//
// ВАЖНО: имена функций содержат имя Java-пакета. Если ваш пакет не
// com.example.amplitude — замените во всех именах ниже точки на подчёркивания:
//   com.foo.bar.Amplitude -> Java_com_foo_bar_Amplitude_nativeDecodeFile
// (подчёркивание внутри самого имени пакета экранируется как _1).

#include <jni.h>

#include <new>
#include <vector>

#include "decoder.h"
#include "envelope.h"

namespace {

struct VecSink {
    std::vector<jint> &out;
    void operator()(const int32_t *v, int n) { out.insert(out.end(), v, v + n); }
};

amp::Backend toBackend(jint v)
{
    switch (v) {
        case 1:  return amp::BACKEND_MINIMP3;
        case 2:  return amp::BACKEND_MEDIANDK;
        default: return amp::BACKEND_AUTO;
    }
}

amp::Params toParams(jint channel, jint reduce, jint block, jint intervalMs,
                     jboolean absolute, jint limitPoints)
{
    amp::Params p;
    p.chan       = (int)channel;
    p.reduce     = (int)reduce;
    p.block      = block > 0 ? (int)block : 1;
    p.intervalMs = intervalMs > 0 ? (int)intervalMs : 0;
    p.absolute   = (absolute == JNI_TRUE);
    p.limit      = limitPoints > 0 ? (long long)limitPoints : -1;
    return p;
}

/// Прогоняет декодер и собирает результат. Декодер удаляется здесь же.
jintArray finish(JNIEnv *env, amp::Decoder *dec, const amp::Params &p, jintArray info)
{
    if (!dec) return nullptr;

    const int hz    = dec->sampleRate();
    const int nch   = dec->channels();
    const int outch = amp::valuesPerPoint(p, nch);
    const int block = amp::resolveBlock(p, hz);

    std::vector<jint> values;
    try {
        if (dec->totalFrames() > 0) {          // разумный резерв
            long long points = (dec->totalFrames() + block - 1) / block;
            if (p.limit > 0 && points > p.limit) points = p.limit;
            if (points > 0) values.reserve((size_t)points * (size_t)outch);
        }
        VecSink sink{values};
        amp::run(*dec, p, sink);
    } catch (const std::bad_alloc &) {
        delete dec;
        if (jclass oom = env->FindClass("java/lang/OutOfMemoryError"))
            env->ThrowNew(oom, "amplitude: не хватило памяти под массив значений");
        return nullptr;
    }

    if (info) {
        jint hdr[5] = { (jint)hz, (jint)nch, (jint)outch,
                        (jint)dec->totalFrames(), (jint)block };
        const jsize n = env->GetArrayLength(info);
        env->SetIntArrayRegion(info, 0, n < 5 ? n : 5, hdr);
    }
    delete dec;

    jintArray arr = env->NewIntArray((jsize)values.size());
    if (!arr) return nullptr;
    if (!values.empty())
        env->SetIntArrayRegion(arr, 0, (jsize)values.size(), values.data());
    return arr;
}

} // namespace

extern "C" {

JNIEXPORT jintArray JNICALL
Java_com_example_amplitude_Amplitude_nativeDecodeFile(
        JNIEnv *env, jclass, jstring jpath,
        jint channel, jint reduce, jint block, jint intervalMs,
        jboolean absolute, jint limitPoints, jint backend, jintArray info)
{
    const char *path = env->GetStringUTFChars(jpath, nullptr);
    if (!path) return nullptr;
    amp::Decoder *dec = amp::openFile(path, toBackend(backend));
    env->ReleaseStringUTFChars(jpath, path);
    return finish(env, dec, toParams(channel, reduce, block, intervalMs,
                                     absolute, limitPoints), info);
}

// fd НЕ закрывается и НЕ дублируется — владельцем остаётся вызывающая сторона.
// size <= 0 — «до конца файла».
JNIEXPORT jintArray JNICALL
Java_com_example_amplitude_Amplitude_nativeDecodeFd(
        JNIEnv *env, jclass, jint fd, jlong offset, jlong size,
        jint channel, jint reduce, jint block, jint intervalMs,
        jboolean absolute, jint limitPoints, jint backend, jintArray info)
{
    amp::Decoder *dec = amp::openFd((int)fd, (long long)offset, (long long)size,
                                    toBackend(backend));
    return finish(env, dec, toParams(channel, reduce, block, intervalMs,
                                     absolute, limitPoints), info);
}

JNIEXPORT jintArray JNICALL
Java_com_example_amplitude_Amplitude_nativeDecodeBytes(
        JNIEnv *env, jclass, jbyteArray jdata, jint dataOffset, jint dataLength,
        jint channel, jint reduce, jint block, jint intervalMs,
        jboolean absolute, jint limitPoints, jint backend, jintArray info)
{
    if (!jdata || dataLength <= 0) return nullptr;
    jbyte *raw = env->GetByteArrayElements(jdata, nullptr);
    if (!raw) return nullptr;

    amp::Decoder *dec = amp::openMemory((const uint8_t *)raw + dataOffset,
                                        (size_t)dataLength, toBackend(backend));
    jintArray res = finish(env, dec, toParams(channel, reduce, block, intervalMs,
                                              absolute, limitPoints), info);
    env->ReleaseByteArrayElements(jdata, raw, JNI_ABORT);
    return res;
}

JNIEXPORT jstring JNICALL
Java_com_example_amplitude_Amplitude_nativeLastError(JNIEnv *env, jclass)
{
    return env->NewStringUTF(amp::lastError());
}

} // extern "C"
