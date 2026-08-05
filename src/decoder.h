// decoder.h — единый интерфейс аудиодекодера: файл -> интерливед PCM16.
//
// Реализации:
//   decoder_minimp3.cpp  — MP3, переносимый (Windows/Linux/Android), без зависимостей
//   decoder_mediandk.cpp — Android 12+: m4a/AAC (в т.ч. HE-AAC v2), mp3, flac, ogg,
//                          wav и всё прочее, что умеет системный MediaCodec

#ifndef AMPLITUDE_DECODER_H
#define AMPLITUDE_DECODER_H

#include <cstddef>
#include <cstdint>

// Определение формата живёт в отдельном заголовке, чтобы тестироваться
// без сборки декодеров. Включается здесь, чтобы не менять публичный API.
#include "sniff.h"

namespace amp {

class Decoder {
public:
    virtual ~Decoder() {}

    virtual int sampleRate() const = 0;
    virtual int channels()   const = 0;

    /// Всего отсчётов (на канал). 0 — если неизвестно заранее.
    virtual long long totalFrames() const = 0;

    /// Читает до maxSamples интерливед-значений. 0 — конец потока ИЛИ сбой:
    /// после нулевого чтения обязательно проверяйте failed().
    virtual size_t read(int16_t *dst, size_t maxSamples) = 0;

    /// true, если поток оборвался из-за ошибки, а не закончился нормально.
    /// Без этого признака сбой декодера неотличим от конца файла, и потребитель
    /// получил бы молча усечённые данные с признаком успеха.
    virtual bool failed() const { return false; }

    /// Имя бэкенда для диагностики: "minimp3" или "mediandk".
    virtual const char *backend() const = 0;
};

enum Backend {
    BACKEND_AUTO = 0,   ///< MP3 -> minimp3, остальное -> mediandk
    BACKEND_MINIMP3,
    BACKEND_MEDIANDK,
};

/// Все фабрики возвращают nullptr при неудаче (причина — в lastError()).
/// Владение объектом переходит вызывающей стороне.
Decoder *openFile(const char *path, Backend backend = BACKEND_AUTO);
Decoder *openMemory(const uint8_t *data, size_t size, Backend backend = BACKEND_AUTO);

/// fd не закрывается и не дублируется — владелец остаётся прежним.
/// size <= 0 означает «до конца файла».
Decoder *openFd(int fd, long long offset, long long size, Backend backend = BACKEND_AUTO);

/// Собран ли бэкенд MediaCodec (true только на Android).
bool mediandkAvailable();

/// Текст последней ошибки (по потоку). Никогда не nullptr.
const char *lastError();
void setLastError(const char *fmt, ...);

} // namespace amp

#endif // AMPLITUDE_DECODER_H
