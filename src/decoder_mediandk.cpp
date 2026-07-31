// decoder_mediandk.cpp — бэкенд на системном декодере Android (NDK Media API).
//
// Умеет всё, что умеет устройство: m4a/AAC (в т.ч. HE-AAC v1/v2 с SBR+PS),
// mp3, flac, ogg/opus, wav. Демультиплексирование контейнера берёт на себя
// AMediaExtractor, декодирование — AMediaCodec.
//
// Требуется API 28+ для AMediaDataSource (у нас цель 31+), линковать -lmediandk.
// Вне Android файл собирается в заглушки.

#include "decoder_internal.h"

#ifdef __ANDROID__

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaDataSource.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace amp {
namespace {

const int64_t kInputTimeoutUs  = 2000;
const int64_t kOutputTimeoutUs = 5000;
const int     kMaxStallRounds  = 400;    // ~2 с ожидания без единого выходного буфера

// значения AudioFormat.ENCODING_* из android.media
enum {
    ENC_PCM_16BIT       = 2,
    ENC_PCM_8BIT        = 3,
    ENC_PCM_FLOAT       = 4,
    ENC_PCM_24BIT_PACK  = 21,
    ENC_PCM_32BIT       = 22,
};

class MediaNdkDecoder : public Decoder {
public:
    ~MediaNdkDecoder() override
    {
        if (codec_) { AMediaCodec_stop(codec_); AMediaCodec_delete(codec_); }
        if (ex_)    AMediaExtractor_delete(ex_);
        if (src_)   AMediaDataSource_delete(src_);   // только после экстрактора
    }

    bool openPath(const char *path)
    {
        ex_ = AMediaExtractor_new();
        if (!ex_) { setLastError("MediaCodec: не удалось создать AMediaExtractor"); return false; }
        const media_status_t st = AMediaExtractor_setDataSource(ex_, path);
        if (st != AMEDIA_OK) {
            setLastError("MediaExtractor: не удалось открыть '%s' (код %d)", path, (int)st);
            return false;
        }
        return start();
    }

    bool openFd(int fd, long long offset, long long size)
    {
        ex_ = AMediaExtractor_new();
        if (!ex_) { setLastError("MediaCodec: не удалось создать AMediaExtractor"); return false; }

        if (size <= 0) {                       // длина не известна — берём из самого fd
            const off64_t cur = lseek64(fd, 0, SEEK_CUR);
            const off64_t end = lseek64(fd, 0, SEEK_END);
            if (cur >= 0) lseek64(fd, cur, SEEK_SET);
            size = end > offset ? (long long)(end - offset) : 0;
        }
        if (size <= 0) { setLastError("MediaExtractor: не удалось определить размер данных"); return false; }

        const media_status_t st =
            AMediaExtractor_setDataSourceFd(ex_, fd, (off64_t)offset, (off64_t)size);
        if (st != AMEDIA_OK) {
            setLastError("MediaExtractor: не удалось открыть дескриптор (код %d)", (int)st);
            return false;
        }
        return start();
    }

    bool openMemory(const uint8_t *data, size_t size)
    {
        copy_.assign(data, data + size);

        src_ = AMediaDataSource_new();
        if (!src_) { setLastError("MediaCodec: не удалось создать AMediaDataSource"); return false; }
        AMediaDataSource_setUserdata(src_, this);
        AMediaDataSource_setReadAt(src_, &MediaNdkDecoder::srcReadAt);
        AMediaDataSource_setGetSize(src_, &MediaNdkDecoder::srcGetSize);
        AMediaDataSource_setClose(src_, &MediaNdkDecoder::srcClose);

        ex_ = AMediaExtractor_new();
        if (!ex_) { setLastError("MediaCodec: не удалось создать AMediaExtractor"); return false; }
        const media_status_t st = AMediaExtractor_setDataSourceCustom(ex_, src_);
        if (st != AMEDIA_OK) {
            setLastError("MediaExtractor: не удалось открыть буфер (код %d)", (int)st);
            return false;
        }
        return start();
    }

    int sampleRate() const override { return hz_; }
    int channels()   const override { return ch_; }

    long long totalFrames() const override
    {
        if (durationUs_ <= 0 || hz_ <= 0) return 0;
        return (long long)((double)durationUs_ * hz_ / 1e6);
    }

    size_t read(int16_t *dst, size_t maxSamples) override
    {
        size_t written = 0;
        while (written < maxSamples) {
            if (head_ < pending_.size()) {
                const size_t avail = pending_.size() - head_;
                const size_t n = std::min(avail, maxSamples - written);
                memcpy(dst + written, pending_.data() + head_, n * sizeof(int16_t));
                head_    += n;
                written  += n;
                if (head_ == pending_.size()) { pending_.clear(); head_ = 0; }
                continue;
            }
            if (outputDone_) break;

            int stall = 0;
            while (pending_.empty() && !outputDone_ && stall++ < kMaxStallRounds) pump();
            if (pending_.empty()) {
                if (!outputDone_) {      // вышли по счётчику простоя, а не по концу потока
                    setLastError("MediaCodec: декодер перестал отдавать данные (таймаут)");
                    markFailed();
                }
                break;
            }
        }
        return written;
    }

    const char *backend() const override { return "mediandk"; }
    bool failed() const override { return failed_; }

private:
    // --- AMediaDataSource: чтение из памяти ---------------------------
    // Контракт NdkMediaDataSource.h: 0 возвращается только при size == 0,
    // конец потока обозначается -1. Возврат 0 на EOF ломает разбор контейнера.
    static ssize_t srcReadAt(void *ud, off64_t offset, void *buf, size_t size)
    {
        MediaNdkDecoder *self = static_cast<MediaNdkDecoder *>(ud);
        if (size == 0) return 0;
        if (offset < 0 || (uint64_t)offset >= self->copy_.size()) return -1;
        const size_t n = std::min(size, self->copy_.size() - (size_t)offset);
        memcpy(buf, self->copy_.data() + offset, n);
        return (ssize_t)n;
    }
    static ssize_t srcGetSize(void *ud)
    {
        return (ssize_t)static_cast<MediaNdkDecoder *>(ud)->copy_.size();
    }
    static void srcClose(void *) {}

    // --- запуск ------------------------------------------------------
    bool start()
    {
        const size_t tracks = AMediaExtractor_getTrackCount(ex_);
        int          track  = -1;
        AMediaFormat *fmt   = nullptr;
        char          mime[64] = {0};

        for (size_t i = 0; i < tracks; ++i) {
            AMediaFormat *f = AMediaExtractor_getTrackFormat(ex_, i);
            if (!f) continue;
            const char *m = nullptr;
            if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m &&
                !strncmp(m, "audio/", 6)) {
                snprintf(mime, sizeof(mime), "%s", m);
                track = (int)i;
                fmt   = f;
                break;
            }
            AMediaFormat_delete(f);
        }
        if (track < 0) { setLastError("в файле нет аудиодорожки"); return false; }

        if (AMediaExtractor_selectTrack(ex_, (size_t)track) != AMEDIA_OK) {
            AMediaFormat_delete(fmt);
            setLastError("MediaExtractor: не удалось выбрать дорожку %d", track);
            return false;
        }

        int32_t v32 = 0;
        int64_t v64 = 0;
        if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, &v32))   hz_ = v32;
        if (AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &v32)) ch_ = v32;
        if (AMediaFormat_getInt64(fmt, AMEDIAFORMAT_KEY_DURATION, &v64))      durationUs_ = v64;

        codec_ = AMediaCodec_createDecoderByType(mime);
        if (!codec_) {
            AMediaFormat_delete(fmt);
            setLastError("MediaCodec: нет декодера для '%s'", mime);
            return false;
        }

        media_status_t st = AMediaCodec_configure(codec_, fmt, nullptr, nullptr, 0);
        AMediaFormat_delete(fmt);
        if (st != AMEDIA_OK) { setLastError("MediaCodec: configure не удался (код %d)", (int)st); return false; }

        st = AMediaCodec_start(codec_);
        if (st != AMEDIA_OK) { setLastError("MediaCodec: start не удался (код %d)", (int)st); return false; }

        // Прогрев: ждём первый выходной буфер. Только выходной формат авторитетен —
        // у HE-AAC ядро 22.05 кГц, а SBR отдаёт 44.1 кГц, и в формате дорожки
        // может стоять любое из двух значений.
        int stall = 0;
        while (pending_.empty() && !outputDone_ && stall++ < kMaxStallRounds) pump();

        if (hz_ <= 0 || ch_ < 1) { setLastError("MediaCodec: не удалось определить параметры PCM"); return false; }
        if (ch_ > 2) { setLastError("MediaCodec: %d каналов не поддерживается (нужно 1 или 2)", ch_); return false; }
        if (pending_.empty()) {
            // Сюда попадаем и при пустой дорожке, и при сбое кодека, и при простое:
            // в первых двух случаях текст ошибки уже выставлен в pump().
            if (!failed_) setLastError("MediaCodec: декодер не выдал ни одного отсчёта");
            return false;
        }
        return true;
    }

    /// Сбой декодера: помечаем поток и прекращаем работу. Текст ошибки к этому
    /// моменту уже выставлен вызывающим кодом через setLastError.
    void markFailed()
    {
        failed_     = true;
        inputDone_  = true;
        outputDone_ = true;
    }

    // --- один цикл «подать вход / забрать выход» ----------------------
    void pump()
    {
        if (!inputDone_) {
            const ssize_t ii = AMediaCodec_dequeueInputBuffer(codec_, kInputTimeoutUs);
            if (ii >= 0) {
                size_t   cap = 0;
                uint8_t *ib  = AMediaCodec_getInputBuffer(codec_, (size_t)ii, &cap);
                if (!ib) {   // это отказ кодека, а не конец данных
                    setLastError("MediaCodec: не удалось получить входной буфер");
                    markFailed();
                    return;
                }
                const ssize_t got = AMediaExtractor_readSampleData(ex_, ib, cap);
                if (got < 0) {
                    AMediaCodec_queueInputBuffer(codec_, (size_t)ii, 0, 0, 0,
                                                 AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    inputDone_ = true;
                } else {
                    const int64_t pts = AMediaExtractor_getSampleTime(ex_);
                    AMediaCodec_queueInputBuffer(codec_, (size_t)ii, 0, (size_t)got,
                                                 pts < 0 ? 0 : pts, 0);
                    AMediaExtractor_advance(ex_);
                }
            }
        }

        AMediaCodecBufferInfo info;
        memset(&info, 0, sizeof(info));
        const ssize_t oi = AMediaCodec_dequeueOutputBuffer(codec_, &info, kOutputTimeoutUs);

        if (oi >= 0) {
            size_t   cap = 0;
            uint8_t *ob  = AMediaCodec_getOutputBuffer(codec_, (size_t)oi, &cap);
            if (ob && info.size > 0 && !(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG))
                append(ob + info.offset, (size_t)info.size);
            const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
            AMediaCodec_releaseOutputBuffer(codec_, (size_t)oi, false);
            if (eos) outputDone_ = true;
        } else if (oi == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            if (AMediaFormat *of = AMediaCodec_getOutputFormat(codec_)) {
                int32_t v = 0;
                if (AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SAMPLE_RATE, &v)   && v > 0) hz_ = v;
                if (AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &v) && v > 0) ch_ = v;
                if (AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_PCM_ENCODING, &v))           enc_ = v;
                AMediaFormat_delete(of);
            }
        } else if (oi != AMEDIACODEC_INFO_TRY_AGAIN_LATER &&
                   oi != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            // Любой прочий отрицательный код — отказ кодека (битый поток и т.п.).
            // Раньше он молча игнорировался и выглядел как конец файла.
            setLastError("MediaCodec: ошибка при получении выходного буфера (код %d)", (int)oi);
            markFailed();
        }
    }

    // --- приведение выходного PCM к int16 -----------------------------
    void append(const uint8_t *p, size_t bytes)
    {
        switch (enc_) {
            case ENC_PCM_FLOAT: {
                const size_t n = bytes / sizeof(float);
                const float *f = reinterpret_cast<const float *>(p);
                pending_.reserve(pending_.size() + n);
                for (size_t i = 0; i < n; ++i) {
                    float s = f[i] * 32768.0f;
                    if (s >  32767.0f) s =  32767.0f;
                    if (s < -32768.0f) s = -32768.0f;
                    pending_.push_back((int16_t)(s < 0 ? s - 0.5f : s + 0.5f));
                }
                break;
            }
            case ENC_PCM_8BIT: {
                pending_.reserve(pending_.size() + bytes);
                for (size_t i = 0; i < bytes; ++i)
                    pending_.push_back((int16_t)(((int)p[i] - 128) << 8));
                break;
            }
            case ENC_PCM_24BIT_PACK: {
                const size_t n = bytes / 3;
                pending_.reserve(pending_.size() + n);
                for (size_t i = 0; i < n; ++i) {
                    const int32_t s = (int32_t)(p[i * 3 + 1]) | ((int32_t)p[i * 3 + 2] << 8);
                    pending_.push_back((int16_t)s);
                }
                break;
            }
            case ENC_PCM_32BIT: {
                const size_t n = bytes / 4;
                const int32_t *s = reinterpret_cast<const int32_t *>(p);
                pending_.reserve(pending_.size() + n);
                for (size_t i = 0; i < n; ++i)
                    pending_.push_back((int16_t)(s[i] >> 16));
                break;
            }
            case ENC_PCM_16BIT:
            default: {
                const size_t n = bytes / sizeof(int16_t);
                const int16_t *s = reinterpret_cast<const int16_t *>(p);
                pending_.insert(pending_.end(), s, s + n);
                break;
            }
        }
    }

    AMediaExtractor  *ex_    = nullptr;
    AMediaCodec      *codec_ = nullptr;
    AMediaDataSource *src_   = nullptr;

    std::vector<uint8_t> copy_;      // данные для openMemory
    std::vector<int16_t> pending_;   // раскодированный, ещё не отданный PCM
    size_t  head_       = 0;
    int     hz_         = 0;
    int     ch_         = 0;
    int     enc_        = ENC_PCM_16BIT;
    int64_t durationUs_ = 0;
    bool    inputDone_  = false;
    bool    outputDone_ = false;
    bool    failed_     = false;
};

} // namespace

Decoder *openMediandkFile(const char *path)
{
    MediaNdkDecoder *d = new MediaNdkDecoder();
    if (d->openPath(path)) return d;
    delete d;
    return nullptr;
}

Decoder *openMediandkFd(int fd, long long offset, long long size)
{
    MediaNdkDecoder *d = new MediaNdkDecoder();
    if (d->openFd(fd, offset, size)) return d;
    delete d;
    return nullptr;
}

Decoder *openMediandkMemory(const uint8_t *data, size_t size)
{
    MediaNdkDecoder *d = new MediaNdkDecoder();
    if (d->openMemory(data, size)) return d;
    delete d;
    return nullptr;
}

} // namespace amp

#else  // не Android -----------------------------------------------------

namespace amp {

static Decoder *unavailable()
{
    setLastError("бэкенд MediaCodec доступен только на Android");
    return nullptr;
}

Decoder *openMediandkFile(const char *)            { return unavailable(); }
Decoder *openMediandkFd(int, long long, long long) { return unavailable(); }
Decoder *openMediandkMemory(const uint8_t *, size_t) { return unavailable(); }

} // namespace amp

#endif // __ANDROID__
