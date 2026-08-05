// decoder_open.cpp — выбор бэкенда, определение формата, тексты ошибок.

#include "decoder_internal.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__ANDROID__) || defined(__linux__) || defined(__APPLE__)
#  define AMP_HAVE_POSIX_IO 1
#  include <unistd.h>
#endif

namespace amp {

// ------------------------------------------------------------- ошибки

static thread_local char g_err[256] = {0};

const char *lastError() { return g_err; }

void setLastError(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
}

bool mediandkAvailable()
{
#ifdef __ANDROID__
    return true;
#else
    return false;
#endif
}

// -------------------------------------------------------------- открытие

namespace {

/// Запоминает ошибку наиболее подходящего бэкенда, чтобы последующая
/// «попытка наугад» другим бэкендом не подменила её нерелевантным текстом.
class SavedError {
public:
    SavedError() { msg_[0] = 0; }
    void capture() { snprintf(msg_, sizeof(msg_), "%s", lastError()); }
    void restore() const { if (msg_[0]) setLastError("%s", msg_); }

private:
    char msg_[256];
};

/// Итоговая ошибка, когда не справился ни один бэкенд. Для десктопной сборки
/// сообщение «MediaCodec только на Android» на постороннем файле сбивает с толку:
/// настоящая причина в том, что формат вообще не опознан.
void failAuto(const SavedError &first, bool mpeg, const char *what)
{
    if (!mpeg && !mediandkAvailable())
        setLastError("%s: формат не распознан как MP3, а системный декодер Android "
                     "(MediaCodec) в этой сборке недоступен", what);
    else
        first.restore();
}

} // namespace

Decoder *openFile(const char *path, Backend backend)
{
    if (!path || !*path) { setLastError("путь к файлу не задан"); return nullptr; }

    if (backend == BACKEND_MINIMP3)  return openMinimp3File(path);
    if (backend == BACKEND_MEDIANDK) return openMediandkFile(path);

    // 1024 байта — столько же, сколько сканирует looksLikeMpegAudio, и минимум
    // 377 байт нужно, чтобы разглядеть три пакета MPEG-TS. С прежними 16 байтами
    // файл и буфер нюхались по-разному.
    uint8_t head[1024] = {0};
    size_t n = 0;
    if (FILE *f = fopen(path, "rb")) {
        n = fread(head, 1, sizeof(head), f);
        fclose(f);
    } else {
        setLastError("не удалось открыть файл: %s", path);
        return nullptr;
    }
    if (n == 0) { setLastError("файл пуст: %s", path); return nullptr; }

    const bool mpeg = looksLikeMpegAudio(head, n);
    SavedError first;

    if (mpeg) {
        if (Decoder *d = openMinimp3File(path)) return d;
        first.capture();                 // файл похож на MP3 — эта причина и важна
    }
    if (Decoder *d = openMediandkFile(path)) return d;
    if (!mpeg) {
        first.capture();                 // здесь релевантна причина от MediaCodec
        // Последняя попытка: вдруг это всё-таки MP3 с нестандартным началом.
        if (Decoder *d = openMinimp3File(path)) return d;
    }
    failAuto(first, mpeg, path);
    return nullptr;
}

Decoder *openMemory(const uint8_t *data, size_t size, Backend backend)
{
    if (!data || !size) { setLastError("пустой буфер"); return nullptr; }

    if (backend == BACKEND_MINIMP3)  return openMinimp3Memory(data, size, false);
    if (backend == BACKEND_MEDIANDK) return openMediandkMemory(data, size);

    const bool mpeg = looksLikeMpegAudio(data, size);
    SavedError first;

    if (mpeg) {
        if (Decoder *d = openMinimp3Memory(data, size, false)) return d;
        first.capture();
    }
    if (Decoder *d = openMediandkMemory(data, size)) return d;
    if (!mpeg) {
        first.capture();
        if (Decoder *d = openMinimp3Memory(data, size, false)) return d;
    }
    failAuto(first, mpeg, "буфер");
    return nullptr;
}

Decoder *openFd(int fd, long long offset, long long size, Backend backend)
{
    if (fd < 0) { setLastError("некорректный дескриптор"); return nullptr; }

    if (backend == BACKEND_MEDIANDK) return openMediandkFd(fd, offset, size);

#ifdef AMP_HAVE_POSIX_IO
    // Для minimp3 читаем содержимое в память (декодеру нужен непрерывный буфер).
    // Сначала — сигнатура, чтобы не тянуть в память чужой формат зря.
    uint8_t head[1024] = {0};
    // Дескриптор может указывать на срез внутри большого файла — так приходит
    // ассет из APK или ContentResolver. Читать дальше size нельзя: в голову
    // попадут байты соседнего файла, и формат определится по ним.
    size_t hwant = sizeof(head);
    if (size > 0 && (unsigned long long)size < (unsigned long long)hwant) hwant = (size_t)size;
    const ssize_t hn = pread(fd, head, hwant, (off_t)offset);
    const bool mpeg = hn > 0 && looksLikeMpegAudio(head, (size_t)hn);
    SavedError first;

    if (backend == BACKEND_MINIMP3 || mpeg) {
        // На 32-битных ABI приведение к size_t молча усекло бы длину.
        if (size > 0 && (unsigned long long)size > (unsigned long long)SIZE_MAX) {
            setLastError("файл слишком велик для адресного пространства (%lld байт)", size);
            return nullptr;
        }
        const size_t want = size > 0 ? (size_t)size : 0;
        std::vector<uint8_t> buf;
        buf.reserve(want ? want : (1u << 20));
        uint8_t tmp[1 << 16];
        off_t pos = (off_t)offset;
        for (;;) {
            const size_t chunk = (want && buf.size() + sizeof(tmp) > want)
                                     ? want - buf.size() : sizeof(tmp);
            if (!chunk) break;
            const ssize_t got = pread(fd, tmp, chunk, pos);
            if (got <= 0) break;
            buf.insert(buf.end(), tmp, tmp + got);
            pos += got;
        }
        if (buf.empty()) { setLastError("не удалось прочитать данные из дескриптора"); return nullptr; }
        if (Decoder *d = openMinimp3Memory(buf.data(), buf.size(), true)) return d;
        if (backend == BACKEND_MINIMP3) return nullptr;
        first.capture();
    }
    if (Decoder *d = openMediandkFd(fd, offset, size)) return d;
    first.restore();
    return nullptr;
#else
    (void)size;
    if (backend == BACKEND_MINIMP3) {
        setLastError("чтение по файловому дескриптору не поддержано на этой платформе");
        return nullptr;
    }
    return openMediandkFd(fd, offset, size);
#endif
}

} // namespace amp
