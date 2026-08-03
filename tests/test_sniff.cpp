// Юнит-тесты определения формата (src/sniff.h) на синтетических заголовках.
// Ни аудиофайлов, ни python: все заголовки собираются прямо здесь.
//
// Сборка и запуск:  .\build_host.ps1 -Tests   либо   make unit

#include "sniff.h"
#include "check.h"

#include <vector>

namespace {

std::vector<uint8_t> zeros(size_t n) { return std::vector<uint8_t>(n, 0); }

/// Валидный заголовок кадра MPEG-1 Layer III, 128 кбит/с, 44100 Гц.
void putMp3Frame(std::vector<uint8_t> &b, size_t at)
{
    b[at + 0] = 0xFF;
    b[at + 1] = 0xFB;   // ver=11 (MPEG-1), layer=01 (III), protection=1
    b[at + 2] = 0x90;   // bitrate=1001 (128 кбит/с), freq=00 (44100 Гц)
    b[at + 3] = 0x00;
}

void putTag(std::vector<uint8_t> &b, size_t at, const char *tag)
{
    for (size_t i = 0; tag[i]; ++i) b[at + i] = (uint8_t)tag[i];
}

bool sniff(const std::vector<uint8_t> &b)
{
    return amp::looksLikeMpegAudio(b.data(), b.size());
}

} // namespace

int main()
{
    // 1. Кадр MPEG в начале — это MP3
    {
        std::vector<uint8_t> b = zeros(1024);
        putMp3Frame(b, 0);
        t::check("валидный кадр MPEG в начале -> MP3", sniff(b));
    }

    // 2. Кадр дальше начала, но в пределах окна сканирования
    {
        std::vector<uint8_t> b = zeros(1024);
        putMp3Frame(b, 600);
        t::check("кадр MPEG на смещении 600 -> MP3", sniff(b));
    }

    // 3. ID3 — тоже MP3
    {
        std::vector<uint8_t> b = zeros(1024);
        putTag(b, 0, "ID3");
        t::check("ID3 -> MP3", sniff(b));
    }

    // 4. Чужие контейнеры
    {
        std::vector<uint8_t> b = zeros(1024);
        putTag(b, 4, "ftyp");
        t::check("ftyp (MP4/M4A/3GP) -> не MP3", !sniff(b));
    }
    {
        std::vector<uint8_t> b = zeros(1024);
        putTag(b, 0, "RIFF");
        t::check("RIFF (WAV) -> не MP3", !sniff(b));
    }
    {
        std::vector<uint8_t> b = zeros(1024);
        putTag(b, 0, "OggS");
        t::check("OggS -> не MP3", !sniff(b));
    }
    {
        std::vector<uint8_t> b = zeros(1024);
        putTag(b, 0, "fLaC");
        t::check("fLaC -> не MP3", !sniff(b));
    }

    // 5. ADTS-AAC: биты слоя нулевые, это не MPEG Audio
    {
        std::vector<uint8_t> b = zeros(1024);
        b[0] = 0xFF; b[1] = 0xF1;              // sync 0xFFF, layer=00
        t::check("ADTS-AAC -> не MP3", !sniff(b));
    }

    // 6. Слишком короткий буфер
    {
        std::vector<uint8_t> b = zeros(3);
        b[0] = 0xFF; b[1] = 0xFB;
        t::check("буфер короче 4 байт -> не MP3", !sniff(b));
    }
    t::check("нулевой указатель -> не MP3", !amp::looksLikeMpegAudio(0, 0));

    // 7. Ничего похожего
    {
        std::vector<uint8_t> b = zeros(1024);
        for (size_t i = 0; i < b.size(); ++i) b[i] = (uint8_t)(i & 0x7F);
        t::check("мусор без синхросигнала -> не MP3", !sniff(b));
    }

    return t::report();
}
