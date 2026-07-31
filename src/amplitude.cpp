// amplitude — декодирует аудиофайл и выдаёт значения амплитуды целыми числами.
//
// MP3        — всегда, встроенным minimp3 (одинаково на любой платформе).
// M4A/AAC и прочее — системным декодером Android (MediaCodec), см. decoder_mediandk.cpp.
//
// Диапазон значений: PCM 16 бит, -32768..32767 (с --abs и при rms/avg/peak — 0..32767).
// Сборка: см. Makefile. Целевая платформа — Android 12+ (API 31+).

#include "decoder.h"
#include "envelope.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#endif

namespace {

struct Options {
    const char  *path    = nullptr;   // "-" = stdin
    const char  *outPath = nullptr;   // nullptr = stdout
    amp::Params  p;
    amp::Backend backend  = amp::BACKEND_AUTO;
    bool         binary   = false;    // int32 little-endian вместо текста
    bool         header   = false;    // строка-комментарий с параметрами потока
    bool         quiet    = false;    // не печатать info в stderr
};

void usage(const char *argv0)
{
    fprintf(stderr,
        "amplitude — аудиофайл -> значения амплитуды (int)\n"
        "\n"
        "Использование: %s <файл | -> [опции]\n"
        "\n"
        "Опции:\n"
        "  -o, --out FILE      писать в файл (по умолчанию stdout)\n"
        "  -c, --channel MODE  mix (по умолчанию) | left | right | both\n"
        "  -b, --block N       свернуть N отсчётов в одно значение (по умолчанию 1)\n"
        "  -m, --ms N          то же, но окном N мс (N * частота / 1000 отсчётов);\n"
        "                      имеет приоритет над --block\n"
        "  -r, --reduce MODE   как сворачивать окно: peak (по умолч.) | rms | avg\n"
        "      --abs           выдавать модуль амплитуды\n"
        "      --raw           бинарный вывод: int32 little-endian\n"
        "      --header        первой строкой комментарий с параметрами потока\n"
        "  -n, --limit N       выдать не более N значений\n"
        "      --backend B     auto (по умолчанию) | minimp3 | media\n"
        "  -q, --quiet         не печатать информацию о файле в stderr\n"
        "  -h, --help          эта справка\n"
        "\n"
        "Форматы: MP3 — везде (встроенный minimp3); M4A/AAC, FLAC, OGG, WAV —\n"
        "на Android через системный MediaCodec.\n"
        "\n"
        "Текстовый вывод: одно значение в строке; для --channel both — два числа\n"
        "через пробел (левый правый). Диапазон: -32768..32767.\n"
        "\n"
        "Без --block/--ms выдаётся одно значение на отсчёт файла (44100 значений\n"
        "в секунду для 44.1 кГц). Огибающая по 100 мс:\n"
        "  %s track.m4a --ms 100 --reduce rms\n",
        argv0, argv0);
}

// ------------------------------------------------------------ буфер вывода

class Out {
public:
    explicit Out(FILE *f) : f_(f), n_(0), err_(false) {}
    ~Out() { flush(); }

    void flush()
    {
        if (n_) {
            if (fwrite(buf_, 1, n_, f_) != n_) err_ = true;   // диск кончился и т.п.
            n_ = 0;
        }
    }

    /// Была ли ошибка записи. Проверять до разрушения объекта.
    bool error() const { return err_ || ferror(f_) != 0; }

    void ch(char c)
    {
        if (n_ + 1 > SZ) flush();
        buf_[n_++] = c;
    }

    void num(int32_t v)                       // десятичное число без printf
    {
        if (n_ + 16 > SZ) flush();
        uint32_t u;
        if (v < 0) { buf_[n_++] = '-'; u = (uint32_t)(-(int64_t)v); }
        else       { u = (uint32_t)v; }
        char tmp[12];
        int k = 0;
        do { tmp[k++] = (char)('0' + (u % 10u)); u /= 10u; } while (u);
        while (k) buf_[n_++] = tmp[--k];
    }

    void le32(int32_t v)                      // int32 little-endian
    {
        if (n_ + 4 > SZ) flush();
        const uint32_t u = (uint32_t)v;
        buf_[n_++] = (char)(u & 0xFF);
        buf_[n_++] = (char)((u >> 8) & 0xFF);
        buf_[n_++] = (char)((u >> 16) & 0xFF);
        buf_[n_++] = (char)((u >> 24) & 0xFF);
    }

    void str(const char *s) { while (*s) ch(*s++); }

private:
    static const size_t SZ = 1u << 16;
    FILE  *f_;
    char   buf_[SZ];
    size_t n_;
    bool   err_;
};

struct TextSink {
    Out &out;
    void operator()(const int32_t *v, int n)
    {
        for (int c = 0; c < n; ++c) { if (c) out.ch(' '); out.num(v[c]); }
        out.ch('\n');
    }
};

struct RawSink {
    Out &out;
    void operator()(const int32_t *v, int n)
    {
        for (int c = 0; c < n; ++c) out.le32(v[c]);
    }
};

// ------------------------------------------------------------ разбор опций

bool parseArgs(int argc, char **argv, Options &o)
{
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *val = nullptr;

        #define NEED_VAL()                                                     \
            do {                                                               \
                if (i + 1 >= argc) {                                           \
                    fprintf(stderr, "amplitude: у опции %s нет значения\n", a); \
                    return false;                                              \
                }                                                              \
                val = argv[++i];                                               \
            } while (0)

        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            exit(0);
        } else if (!strcmp(a, "-o") || !strcmp(a, "--out")) {
            NEED_VAL(); o.outPath = val;
        } else if (!strcmp(a, "-c") || !strcmp(a, "--channel")) {
            NEED_VAL();
            if      (!strcmp(val, "mix"))   o.p.chan = amp::CH_MIX;
            else if (!strcmp(val, "left"))  o.p.chan = amp::CH_LEFT;
            else if (!strcmp(val, "right")) o.p.chan = amp::CH_RIGHT;
            else if (!strcmp(val, "both"))  o.p.chan = amp::CH_BOTH;
            else { fprintf(stderr, "amplitude: неизвестный режим канала: %s\n", val); return false; }
        } else if (!strcmp(a, "-r") || !strcmp(a, "--reduce")) {
            NEED_VAL();
            if      (!strcmp(val, "peak")) o.p.reduce = amp::RD_PEAK;
            else if (!strcmp(val, "rms"))  o.p.reduce = amp::RD_RMS;
            else if (!strcmp(val, "avg"))  o.p.reduce = amp::RD_AVG;
            else { fprintf(stderr, "amplitude: неизвестный режим свёртки: %s\n", val); return false; }
        } else if (!strcmp(a, "-b") || !strcmp(a, "--block")) {
            NEED_VAL();
            o.p.block = atoi(val);
            if (o.p.block < 1) { fprintf(stderr, "amplitude: --block должен быть >= 1\n"); return false; }
        } else if (!strcmp(a, "-m") || !strcmp(a, "--ms")) {
            NEED_VAL();
            o.p.intervalMs = atoi(val);
            if (o.p.intervalMs < 1) { fprintf(stderr, "amplitude: --ms должен быть >= 1\n"); return false; }
        } else if (!strcmp(a, "-n") || !strcmp(a, "--limit")) {
            NEED_VAL(); o.p.limit = strtoll(val, nullptr, 10);
        } else if (!strcmp(a, "--backend")) {
            NEED_VAL();
            if      (!strcmp(val, "auto"))    o.backend = amp::BACKEND_AUTO;
            else if (!strcmp(val, "minimp3")) o.backend = amp::BACKEND_MINIMP3;
            else if (!strcmp(val, "media"))   o.backend = amp::BACKEND_MEDIANDK;
            else { fprintf(stderr, "amplitude: неизвестный бэкенд: %s\n", val); return false; }
        } else if (!strcmp(a, "--abs")) {
            o.p.absolute = true;
        } else if (!strcmp(a, "--raw")) {
            o.binary = true;
        } else if (!strcmp(a, "--header")) {
            o.header = true;
        } else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) {
            o.quiet = true;
        } else if (a[0] == '-' && a[1] && strcmp(a, "-")) {
            fprintf(stderr, "amplitude: неизвестная опция: %s\n", a);
            return false;
        } else if (!o.path) {
            o.path = a;
        } else {
            fprintf(stderr, "amplitude: лишний аргумент: %s\n", a);
            return false;
        }
        #undef NEED_VAL
    }

    if (!o.path) { usage(argv[0]); return false; }
    return true;
}

bool readAll(FILE *f, std::vector<uint8_t> &out)
{
    const size_t CHUNK = 1u << 16;
    size_t used = 0;
    for (;;) {
        out.resize(used + CHUNK);
        const size_t got = fread(out.data() + used, 1, CHUNK, f);
        used += got;
        if (got < CHUNK) { out.resize(used); return !ferror(f); }
    }
}

} // namespace

int main(int argc, char **argv)
{
    Options o;
    if (!parseArgs(argc, argv, o)) return 2;

    std::vector<uint8_t> stdinBuf;              // должен пережить декодер
    amp::Decoder *dec = nullptr;

    if (!strcmp(o.path, "-")) {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);   // иначе 0x0D 0x0A испортит поток
#endif
        if (!readAll(stdin, stdinBuf) || stdinBuf.empty()) {
            fprintf(stderr, "amplitude: ошибка чтения stdin\n");
            return 1;
        }
        dec = amp::openMemory(stdinBuf.data(), stdinBuf.size(), o.backend);
    } else {
        dec = amp::openFile(o.path, o.backend);
    }
    if (!dec) {
        fprintf(stderr, "amplitude: %s\n", amp::lastError());
        return 1;
    }

    const int block = amp::resolveBlock(o.p, dec->sampleRate());
    const int outch = amp::valuesPerPoint(o.p, dec->channels());

    if (!o.quiet) {
        fprintf(stderr, "amplitude: %s, %d Гц, %d кан., отсчётов: %lld",
                dec->backend(), dec->sampleRate(), dec->channels(), dec->totalFrames());
        if (block > 1)
            fprintf(stderr, ", окно %d отсчётов (%.1f мс)",
                    block, dec->sampleRate() ? 1000.0 * block / dec->sampleRate() : 0.0);
        fprintf(stderr, "\n");
    }

    FILE *fout = stdout;
#ifdef _WIN32
    // Иначе Windows превратит каждый байт 0x0A в бинарном потоке в 0x0D 0x0A.
    if (o.binary && !o.outPath) _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (o.outPath) {
        fout = fopen(o.outPath, o.binary ? "wb" : "w");
        if (!fout) {
            fprintf(stderr, "amplitude: не могу открыть на запись: %s\n", o.outPath);
            delete dec;
            return 1;
        }
    }

    long long points  = 0;
    bool      writeOk = true;
    {
        Out out(fout);

        if (o.header && !o.binary) {
            out.str("# backend="); out.str(dec->backend());
            out.str(" hz=");       out.num(dec->sampleRate());
            out.str(" channels="); out.num(dec->channels());
            out.str(" values_per_point="); out.num(outch);
            out.str(" block=");    out.num(block);
            out.str(" range=-32768..32767\n");
        }

        if (o.binary) { RawSink  sink{out}; points = amp::run(*dec, o.p, sink); }
        else          { TextSink sink{out}; points = amp::run(*dec, o.p, sink); }

        out.flush();
        writeOk = !out.error();
    }

    if (!o.quiet) fprintf(stderr, "amplitude: выдано точек: %lld\n", points);

    int rc = 0;
    // Сбой декодера обязан быть виден: иначе усечённые данные уйдут как успех.
    if (dec->failed()) {
        fprintf(stderr, "amplitude: декодирование прервано: %s\n", amp::lastError());
        rc = 1;
    }
    if (!writeOk) {
        fprintf(stderr, "amplitude: ошибка записи вывода\n");
        rc = 1;
    }

    delete dec;
    if (fout != stdout) {
        if (fclose(fout) != 0) { fprintf(stderr, "amplitude: ошибка закрытия файла вывода\n"); rc = 1; }
    } else {
        if (fflush(stdout) != 0) { fprintf(stderr, "amplitude: ошибка записи в stdout\n"); rc = 1; }
    }
    return rc;
}
