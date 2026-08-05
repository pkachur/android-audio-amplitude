// decoder_minimp3.cpp — MP3-бэкенд на minimp3 (public domain, header-only).
// Работает везде одинаково: Windows, Linux, Android. Никаких системных сервисов.

#include "decoder_internal.h"

#include <cstring>
#include <vector>

#define MINIMP3_IMPLEMENTATION
#ifdef _MSC_VER
// minimp3 — сторонний public domain-код, править его не наша забота: под /W4
// MSVC выдаёт на нём три десятка C4244/C4267/C4456. Глушим ровно на время
// включения, свой код по-прежнему собирается на /W4 без единого замечания.
#  pragma warning(push, 0)
#endif
#include "minimp3_ex.h"
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

namespace amp {

static_assert(sizeof(mp3d_sample_t) == 2,
              "minimp3 должен быть собран на int16 (без MINIMP3_FLOAT_OUTPUT)");

namespace {

class Minimp3Decoder : public Decoder {
public:
    Minimp3Decoder() { memset(&dec_, 0, sizeof(dec_)); }

    ~Minimp3Decoder() override
    {
        if (opened_) mp3dec_ex_close(&dec_);
    }

    bool openPath(const char *path)
    {
        const int rc = mp3dec_ex_open(&dec_, path, MP3D_SEEK_TO_SAMPLE);
        if (rc) { setLastError("minimp3: не удалось разобрать '%s' (код %d)", path, rc); return false; }
        opened_ = true;
        return valid();
    }

    bool openMemory(const uint8_t *data, size_t size, bool copy)
    {
        if (copy) {
            owned_.assign(data, data + size);
            data = owned_.data();
        }
        const int rc = mp3dec_ex_open_buf(&dec_, data, size, MP3D_SEEK_TO_SAMPLE);
        if (rc) { setLastError("minimp3: не удалось разобрать буфер (код %d)", rc); return false; }
        opened_ = true;
        return valid();
    }

    int sampleRate() const override { return dec_.info.hz; }
    int channels()   const override { return dec_.info.channels; }

    long long totalFrames() const override
    {
        const int ch = dec_.info.channels;
        return ch > 0 ? (long long)(dec_.samples / (size_t)ch) : 0;
    }

    size_t read(int16_t *dst, size_t maxSamples) override
    {
        return mp3dec_ex_read(&dec_, dst, maxSamples);
    }

    const char *backend() const override { return "minimp3"; }

private:
    bool valid()
    {
        if (dec_.info.hz <= 0 || dec_.info.channels < 1 || dec_.info.channels > 2) {
            setLastError("minimp3: неподдерживаемый поток (%d Гц, %d кан.)",
                         dec_.info.hz, dec_.info.channels);
            return false;
        }
        return true;
    }

    mp3dec_ex_t          dec_;
    std::vector<uint8_t> owned_;   // копия данных, если попросили владеть
    bool                 opened_ = false;
};

} // namespace

Decoder *openMinimp3File(const char *path)
{
    Minimp3Decoder *d = new Minimp3Decoder();
    if (d->openPath(path)) return d;
    delete d;
    return nullptr;
}

Decoder *openMinimp3Memory(const uint8_t *data, size_t size, bool copy)
{
    Minimp3Decoder *d = new Minimp3Decoder();
    if (d->openMemory(data, size, copy)) return d;
    delete d;
    return nullptr;
}

} // namespace amp
