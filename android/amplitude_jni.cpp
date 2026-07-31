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

#include <cstdint>
#include <new>
#include <string>
#include <vector>

#include "decoder.h"
#include "envelope.h"

namespace {

struct VecSink {
    std::vector<jint> &out;
    void operator()(const int32_t *v, int n) { out.insert(out.end(), v, v + n); }
};

/// Удаляет декодер на любом пути выхода, включая раскрутку стека при bad_alloc.
class DecoderGuard {
public:
    explicit DecoderGuard(amp::Decoder *d) : d_(d) {}
    ~DecoderGuard() { delete d_; }
    amp::Decoder *get() const { return d_; }

private:
    DecoderGuard(const DecoderGuard &);
    DecoderGuard &operator=(const DecoderGuard &);
    amp::Decoder *d_;
};

/// Гарантирует ReleaseStringUTFChars ровно один раз на любом пути выхода.
class StringUtfGuard {
public:
    StringUtfGuard(JNIEnv *env, jstring s)
        : env_(env), s_(s), p_(s ? env->GetStringUTFChars(s, nullptr) : nullptr) {}
    ~StringUtfGuard() { if (p_) env_->ReleaseStringUTFChars(s_, p_); }
    const char *get() const { return p_; }

private:
    StringUtfGuard(const StringUtfGuard &);
    StringUtfGuard &operator=(const StringUtfGuard &);
    JNIEnv     *env_;
    jstring     s_;
    const char *p_;
};

/// Гарантирует ReleaseByteArrayElements даже при исключении.
class ByteArrayGuard {
public:
    ByteArrayGuard(JNIEnv *env, jbyteArray arr)
        : env_(env), arr_(arr), ptr_(env->GetByteArrayElements(arr, nullptr)) {}
    ~ByteArrayGuard() { if (ptr_) env_->ReleaseByteArrayElements(arr_, ptr_, JNI_ABORT); }
    jbyte *get() const { return ptr_; }

private:
    ByteArrayGuard(const ByteArrayGuard &);
    ByteArrayGuard &operator=(const ByteArrayGuard &);
    JNIEnv     *env_;
    jbyteArray  arr_;
    jbyte      *ptr_;
};

/// JNI принимает не обычный UTF-8, а Modified UTF-8: 4-байтовые последовательности
/// (символы вне BMP, например эмодзи в имени файла) в нём недопустимы, а строка,
/// обрезанная по границе буфера посреди символа, невалидна целиком. И то и другое
/// приводит к abort под CheckJNI, поэтому проблемные места заменяем на '?'.
std::string sanitizeMutf8(const char *s)
{
    std::string out;
    if (!s) return out;

    const unsigned char *p = reinterpret_cast<const unsigned char *>(s);
    while (*p) {
        const unsigned char c = *p;
        size_t len;
        if (c < 0x80)                len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else { out += '?'; ++p; continue; }

        bool ok = true;
        for (size_t i = 1; i < len; ++i) {
            if ((p[i] & 0xC0) != 0x80) { ok = false; break; }   // обрыв или мусор
        }
        if (!ok)      { out += '?'; ++p;      continue; }
        if (len == 4) { out += '?'; p += 4;   continue; }       // вне BMP

        out.append(reinterpret_cast<const char *>(p), len);
        p += len;
    }
    return out;
}

jstring newJavaString(JNIEnv *env, const char *s)
{
    return env->NewStringUTF(sanitizeMutf8(s).c_str());
}

jintArray throwOom(JNIEnv *env)
{
    if (jclass oom = env->FindClass("java/lang/OutOfMemoryError"))
        env->ThrowNew(oom, "amplitude: не хватило памяти при декодировании");
    return nullptr;
}

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

/// Прогоняет декодер и собирает результат. Декодер уничтожается здесь же.
/// Исключения наружу не выпускает — bad_alloc превращается в OutOfMemoryError.
jintArray finish(JNIEnv *env, amp::Decoder *decRaw, const amp::Params &p, jintArray info)
{
    DecoderGuard guard(decRaw);
    amp::Decoder *dec = guard.get();
    if (!dec) return nullptr;                 // причина уже в amp::lastError()

    const int hz    = dec->sampleRate();
    const int nch   = dec->channels();
    const int outch = amp::valuesPerPoint(p, nch);
    const int block = amp::resolveBlock(p, hz);

    std::vector<jint> values;
    if (dec->totalFrames() > 0) {             // разумный резерв
        long long points = (dec->totalFrames() + block - 1) / block;
        if (p.limit > 0 && points > p.limit) points = p.limit;
        if (points > 0) values.reserve((size_t)points * (size_t)outch);
    }
    VecSink sink{values};
    amp::run(*dec, p, sink);

    // Обрыв декодирования не должен выглядеть как успешный короткий результат.
    if (dec->failed()) return nullptr;

    if (values.size() > (size_t)INT32_MAX) {
        amp::setLastError("слишком много значений (%llu) — увеличьте окно или задайте limitPoints",
                          (unsigned long long)values.size());
        return nullptr;
    }

    if (info) {
        const long long tf = dec->totalFrames();
        jint hdr[5] = { (jint)hz, (jint)nch, (jint)outch,
                        (jint)(tf > INT32_MAX ? INT32_MAX : tf), (jint)block };
        const jsize n = env->GetArrayLength(info);
        env->SetIntArrayRegion(info, 0, n < 5 ? n : 5, hdr);
    }

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
    StringUtfGuard path(env, jpath);
    if (!path.get()) return nullptr;

    // Открытие тоже копирует данные в память, поэтому bad_alloc ловим и здесь,
    // иначе исключение улетело бы через границу JNI и уронило процесс.
    try {
        amp::Decoder *dec = amp::openFile(path.get(), toBackend(backend));
        return finish(env, dec, toParams(channel, reduce, block, intervalMs,
                                         absolute, limitPoints), info);
    } catch (const std::bad_alloc &) {
        return throwOom(env);
    }
}

// fd НЕ закрывается и НЕ дублируется — владельцем остаётся вызывающая сторона.
// size <= 0 — «до конца файла».
JNIEXPORT jintArray JNICALL
Java_com_example_amplitude_Amplitude_nativeDecodeFd(
        JNIEnv *env, jclass, jint fd, jlong offset, jlong size,
        jint channel, jint reduce, jint block, jint intervalMs,
        jboolean absolute, jint limitPoints, jint backend, jintArray info)
{
    try {
        amp::Decoder *dec = amp::openFd((int)fd, (long long)offset, (long long)size,
                                        toBackend(backend));
        return finish(env, dec, toParams(channel, reduce, block, intervalMs,
                                         absolute, limitPoints), info);
    } catch (const std::bad_alloc &) {
        return throwOom(env);
    }
}

JNIEXPORT jintArray JNICALL
Java_com_example_amplitude_Amplitude_nativeDecodeBytes(
        JNIEnv *env, jclass, jbyteArray jdata, jint dataOffset, jint dataLength,
        jint channel, jint reduce, jint block, jint intervalMs,
        jboolean absolute, jint limitPoints, jint backend, jintArray info)
{
    if (!jdata) { amp::setLastError("массив данных не задан"); return nullptr; }

    // Без этой проверки offset/length за пределами массива дают чтение чужой
    // памяти в куче ART: мусор в декодере или падение.
    const jsize arrLen = env->GetArrayLength(jdata);
    if (dataOffset < 0 || dataLength <= 0 || dataOffset > arrLen - dataLength) {
        amp::setLastError("некорректный диапазон: offset=%d length=%d при длине массива %d",
                          (int)dataOffset, (int)dataLength, (int)arrLen);
        return nullptr;
    }

    ByteArrayGuard data(env, jdata);
    if (!data.get()) return nullptr;

    try {
        amp::Decoder *dec = amp::openMemory(
                reinterpret_cast<const uint8_t *>(data.get()) + dataOffset,
                (size_t)dataLength, toBackend(backend));
        return finish(env, dec, toParams(channel, reduce, block, intervalMs,
                                         absolute, limitPoints), info);
    } catch (const std::bad_alloc &) {
        return throwOom(env);
    }
}

JNIEXPORT jstring JNICALL
Java_com_example_amplitude_Amplitude_nativeLastError(JNIEnv *env, jclass)
{
    return newJavaString(env, amp::lastError());
}

} // extern "C"
