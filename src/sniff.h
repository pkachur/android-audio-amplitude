// sniff.h — определение формата по началу файла: что можно отдать minimp3,
// а что обязано идти в системный декодер.
//
// Заголовочный файл без .cpp, как и envelope.h: тестируется отдельно
// (tests/test_sniff.cpp), без сборки декодеров и без аудиофайлов.

#ifndef AMPLITUDE_SNIFF_H
#define AMPLITUDE_SNIFF_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace amp {

/// true, если начало буфера похоже на MPEG Audio (ID3 или sync-заголовок
/// слоя I/II/III). ADTS-AAC (0xFFF1/0xFFF9) сюда намеренно не попадает —
/// у него биты слоя нулевые.
inline bool looksLikeMpegAudio(const uint8_t *d, size_t n)
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

} // namespace amp

#endif // AMPLITUDE_SNIFF_H
