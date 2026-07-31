// decoder_internal.h — фабрики конкретных бэкендов. Не для внешнего использования:
// приложение работает через openFile/openMemory/openFd из decoder.h.

#ifndef AMPLITUDE_DECODER_INTERNAL_H
#define AMPLITUDE_DECODER_INTERNAL_H

#include "decoder.h"

namespace amp {

// --- minimp3 (есть всегда) ---------------------------------------------
Decoder *openMinimp3File(const char *path);
/// copy == false — буфер должен пережить декодер; true — данные копируются внутрь.
Decoder *openMinimp3Memory(const uint8_t *data, size_t size, bool copy);

// --- MediaCodec (реальные только на Android, иначе заглушки) ------------
Decoder *openMediandkFile(const char *path);
Decoder *openMediandkFd(int fd, long long offset, long long size);
Decoder *openMediandkMemory(const uint8_t *data, size_t size);

} // namespace amp

#endif // AMPLITUDE_DECODER_INTERNAL_H
