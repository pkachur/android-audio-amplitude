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

    // 8. MPEG-TS: пакеты по 188 байт. Заполнение таблиц байтами 0xFF раньше
    //    выглядело как заголовок MPEG.
    {
        std::vector<uint8_t> b = zeros(4 + 3 * 192 + 8);
        for (int i = 0; i < 3; ++i) b[(size_t)i * 188] = 0x47;
        for (size_t i = 20; i < 180; ++i) b[i] = 0xFF;      // стаффинг PAT/PMT
        t::check("MPEG-TS (шаг 188) -> не MP3", !sniff(b));
    }

    // 9. Тот же TS с 4-байтовой меткой времени (M2TS): шаг 192
    {
        std::vector<uint8_t> b = zeros(4 + 3 * 192 + 8);
        for (int i = 0; i < 3; ++i) b[4 + (size_t)i * 192] = 0x47;
        for (size_t i = 24; i < 184; ++i) b[i] = 0xFF;
        t::check("MPEG-TS (шаг 192, M2TS) -> не MP3", !sniff(b));
    }

    // 10. LOAS/LATM: в этом виде AAC ELD лежит в .aac.
    //     Полезная нагрузка произвольная, в ней легко встречается 0xFF 0xFF.
    {
        std::vector<uint8_t> b = zeros(1024);
        b[0] = 0x56; b[1] = 0xE0; b[2] = 0x10;
        for (size_t i = 40; i < 80; ++i) b[i] = 0xFF;
        t::check("LOAS/LATM -> не MP3", !sniff(b));
    }

    // 11. Сплошной стаффинг 0xFF: индекс битрейта 1111 запрещён стандартом
    {
        std::vector<uint8_t> b(1024, 0xFF);
        t::check("сплошной 0xFF -> не MP3", !sniff(b));
    }

    // 12. Запрещённый индекс битрейта при валидных прочих полях
    {
        std::vector<uint8_t> b = zeros(1024);
        putMp3Frame(b, 100);
        b[102] = 0xF0;                                   // bitrate=1111
        t::check("индекс битрейта 1111 -> не MP3", !sniff(b));
    }

    // 13. «Свободный» битрейт 0000 minimp3 всё равно не разберёт
    {
        std::vector<uint8_t> b = zeros(1024);
        putMp3Frame(b, 100);
        b[102] = 0x00;                                   // bitrate=0000, freq=00
        t::check("индекс битрейта 0000 -> не MP3", !sniff(b));
    }

    // 14. Зарезервированный индекс частоты
    {
        std::vector<uint8_t> b = zeros(1024);
        putMp3Frame(b, 100);
        b[102] = 0x9C;                                   // bitrate=1001, freq=11
        t::check("индекс частоты 11 -> не MP3", !sniff(b));
    }

    // 15. Кадр в самом конце буфера не должен читаться за его границей
    {
        std::vector<uint8_t> b = zeros(6);
        b[3] = 0xFF; b[4] = 0xFB; b[5] = 0x90;           // четвёртого байта нет
        t::check("обрезанный кадр на границе буфера -> не MP3", !sniff(b));
    }

    return t::report();
}
