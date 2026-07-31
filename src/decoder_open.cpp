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

// ---------------------------------------------------- определение формата

bool looksLikeMpegAudio(const uint8_t *d, size_t n)
{
    if (!d || n < 4) return false;

    // Явно чужие контейнеры — чтобы случайный 0xFF в заголовке не сбил с толку.
    if (n >= 12 && !memcmp(d + 4, "ftyp", 4)) return false;   // MP4 / M4A
    if (!memcmp(d, "RIFF", 4)) return false;                  // WAV
    if (!memcmp(d, "OggS", 4)) return false;                  // Ogg / Opus
    if (!memcmp(d, "fLaC", 4)) return false;                  // FLAC
    if (!memcmp(d, "FORM", 4)) return false;                  // AIFF

    if (n >= 3 && !memcmp(d, "ID3", 3)) return true;

    const size_t lim = n < 1024 ? n : 1024;
    for (size_t i = 0; i + 1 < lim; ++i) {
        if (d[i] != 0xFF || (d[i + 1] & 0xE0) != 0xE0) continue;
        const int ver   = (d[i + 1] >> 3) & 0x03;   // 1 — зарезервировано
        const int layer = (d[i + 1] >> 1) & 0x03;   // 0 — зарезервировано (это ADTS-AAC)
        if (ver != 1 && layer != 0) return true;
    }
    return false;
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

static Decoder *finishAuto(Decoder *d, const char *what)
{
    if (d) return d;
    if (!mediandkAvailable())
        setLastError("%s: формат не распознан как MP3, а системный декодер "
                     "Android (MediaCodec) в этой сборке недоступен", what);
    return nullptr;
}

Decoder *openFile(const char *path, Backend backend)
{
    if (!path || !*path) { setLastError("путь к файлу не задан"); return nullptr; }

    if (backend == BACKEND_MINIMP3)  return openMinimp3File(path);
    if (backend == BACKEND_MEDIANDK) return openMediandkFile(path);

    uint8_t head[16] = {0};
    size_t n = 0;
    if (FILE *f = fopen(path, "rb")) {
        n = fread(head, 1, sizeof(head), f);
        fclose(f);
    } else {
        setLastError("не удалось открыть файл: %s", path);
        return nullptr;
    }

    if (looksLikeMpegAudio(head, n)) {
        if (Decoder *d = openMinimp3File(path)) return d;
    }
    if (Decoder *d = openMediandkFile(path)) return d;
    // Последняя попытка: вдруг это всё-таки MP3 с нестандартным началом.
    return finishAuto(openMinimp3File(path), path);
}

Decoder *openMemory(const uint8_t *data, size_t size, Backend backend)
{
    if (!data || !size) { setLastError("пустой буфер"); return nullptr; }

    if (backend == BACKEND_MINIMP3)  return openMinimp3Memory(data, size, false);
    if (backend == BACKEND_MEDIANDK) return openMediandkMemory(data, size);

    if (looksLikeMpegAudio(data, size)) {
        if (Decoder *d = openMinimp3Memory(data, size, false)) return d;
    }
    if (Decoder *d = openMediandkMemory(data, size)) return d;
    return finishAuto(openMinimp3Memory(data, size, false), "буфер");
}

Decoder *openFd(int fd, long long offset, long long size, Backend backend)
{
    if (fd < 0) { setLastError("некорректный дескриптор"); return nullptr; }

    if (backend == BACKEND_MEDIANDK) return openMediandkFd(fd, offset, size);

#ifdef AMP_HAVE_POSIX_IO
    // Для minimp3 читаем содержимое в память (декодеру нужен непрерывный буфер).
    // Сначала — сигнатура, чтобы не тянуть в память чужой формат зря.
    uint8_t head[16] = {0};
    ssize_t hn = pread(fd, head, sizeof(head), (off_t)offset);
    const bool mpeg = hn > 0 && looksLikeMpegAudio(head, (size_t)hn);

    if (backend == BACKEND_MINIMP3 || mpeg) {
        std::size_t want = size > 0 ? (std::size_t)size : 0;
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
    }
#else
    (void)size;
    if (backend == BACKEND_MINIMP3) {
        setLastError("чтение по файловому дескриптору не поддержано на этой платформе");
        return nullptr;
    }
#endif

    return finishAuto(openMediandkFd(fd, offset, size), "дескриптор");
}

} // namespace amp
