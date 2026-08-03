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

/// Пакет MPEG-TS: 188 байт, либо 192 с 4-байтовой меткой времени (M2TS).
/// Три подряд идущих байта 0x47 на своих местах — надёжная примета.
inline bool looksLikeMpegTs(const uint8_t *d, size_t n)
{
    const size_t strides[2] = { 188, 192 };
    for (int s = 0; s < 2; ++s) {
        const size_t st  = strides[s];
        const size_t off = (st == 192) ? 4 : 0;      // у M2TS метка времени впереди
        if (n < off + 2 * st + 1) continue;
        if (d[off] == 0x47 && d[off + st] == 0x47 && d[off + 2 * st] == 0x47) return true;
    }
    return false;
}

/// ADTS-AAC: синхрослово 0xFFF, затем нулевые биты слоя.
inline bool looksLikeAdts(const uint8_t *d, size_t n)
{
    return n >= 2 && d[0] == 0xFF && (d[1] & 0xF6) == 0xF0;
}

/// LOAS/LATM: синхрослово 0x2B7 в 11 старших битах. AAC ELD в контейнере
/// .aac обычно приходит именно так, а не в ADTS.
inline bool looksLikeLoas(const uint8_t *d, size_t n)
{
    return n >= 2 && d[0] == 0x56 && (d[1] & 0xE0) == 0xE0;
}

/// Заголовок кадра MPEG Audio (слой I/II/III) со всеми полями.
/// Вызывающий обязан гарантировать 4 доступных байта.
///
/// Без проверки битрейта и частоты пара 0xFF 0xFF проходит как заголовок —
/// а это штатное заполнение таблиц в MPEG-TS и обычный байт нагрузки в AAC.
inline bool validMpegFrameHeader(const uint8_t *h)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
    const int ver     = (h[1] >> 3) & 0x03;   // 01 — зарезервировано
    const int layer   = (h[1] >> 1) & 0x03;   // 00 — зарезервировано (это ADTS-AAC)
    const int bitrate = (h[2] >> 4) & 0x0F;   // 0000 — «свободный», 1111 — запрещён
    const int freq    = (h[2] >> 2) & 0x03;   // 11 — зарезервировано
    return ver != 1 && layer != 0 && bitrate != 0 && bitrate != 15 && freq != 3;
}

/// true, если начало буфера похоже на MPEG Audio (ID3 или кадр слоя I/II/III).
/// Ошибка здесь стоит одной впустую потраченной попытки открытия, но на AAC ELD
/// в .ts и .aac она отправляла бы файл в minimp3 вместо системного декодера.
inline bool looksLikeMpegAudio(const uint8_t *d, size_t n)
{
    if (!d || n < 4) return false;

    // Явно чужие контейнеры — чтобы случайный 0xFF внутри них не сбил с толку.
    if (n >= 12 && !memcmp(d + 4, "ftyp", 4)) return false;   // MP4 / M4A / 3GP
    if (!memcmp(d, "RIFF", 4)) return false;                  // WAV
    if (!memcmp(d, "OggS", 4)) return false;                  // Ogg / Opus
    if (!memcmp(d, "fLaC", 4)) return false;                  // FLAC
    if (!memcmp(d, "FORM", 4)) return false;                  // AIFF
    if (looksLikeMpegTs(d, n)) return false;                  // MPEG-2 TS (.ts)
    if (looksLikeAdts(d, n))   return false;                  // сырой AAC (.aac)
    if (looksLikeLoas(d, n))   return false;                  // LATM/LOAS (.aac)

    if (n >= 3 && !memcmp(d, "ID3", 3)) return true;

    const size_t lim = n < 1024 ? n : 1024;
    for (size_t i = 0; i + 3 < lim; ++i)
        if (validMpegFrameHeader(d + i)) return true;
    return false;
}

} // namespace amp

#endif // AMPLITUDE_SNIFF_H
