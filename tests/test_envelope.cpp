// Юнит-тесты свёртки (src/envelope.h) на синтетическом декодере.
//
// Нужны потому, что через MP3-фикстуру граничные значения не воспроизводятся:
// закодированный сигнал почти никогда не содержит отсчёт -32768, а именно на нём
// ломается объявленный диапазон (модуль -32768 равен 32768).
//
// Сборка и запуск:
//     make unit
// либо вручную:
//     c++ -std=c++11 -Isrc -o test_envelope tests/test_envelope.cpp && ./test_envelope

#include "decoder.h"
#include "envelope.h"
#include "check.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

/// Декодер, отдающий заранее заданный PCM. Позволяет подать точные значения,
/// которых не добиться от настоящего кодека.
class FakeDecoder : public amp::Decoder {
public:
    FakeDecoder(const std::vector<int16_t> &pcm, int hz, int ch)
        : pcm_(pcm), pos_(0), hz_(hz), ch_(ch) {}

    int sampleRate() const override { return hz_; }
    int channels()   const override { return ch_; }
    long long totalFrames() const override { return (long long)(pcm_.size() / (size_t)ch_); }
    const char *backend() const override { return "fake"; }

    size_t read(int16_t *dst, size_t maxSamples) override
    {
        size_t n = pcm_.size() - pos_;
        if (n > maxSamples) n = maxSamples;
        memcpy(dst, pcm_.data() + pos_, n * sizeof(int16_t));
        pos_ += n;
        return n;
    }

private:
    std::vector<int16_t> pcm_;
    size_t               pos_;
    int                  hz_;
    int                  ch_;
};

struct Collect {
    std::vector<int32_t> &v;
    void operator()(const int32_t *w, int n) { v.insert(v.end(), w, w + n); }
};

std::vector<int32_t> runOn(const std::vector<int16_t> &pcm, int ch, const amp::Params &p,
                           int hz = 44100)
{
    FakeDecoder dec(pcm, hz, ch);
    std::vector<int32_t> out;
    Collect sink{out};
    amp::run(dec, p, sink);
    return out;
}

int32_t maxOf(const std::vector<int32_t> &v)
{
    int32_t m = v.empty() ? 0 : v[0];
    for (size_t i = 1; i < v.size(); ++i) if (v[i] > m) m = v[i];
    return m;
}

} // namespace

int main()
{
    const std::vector<int16_t> minSamples(64, -32768);   // сплошной минимум шкалы

    // 1. --abs от -32768 не должен давать 32768
    {
        amp::Params p;
        p.absolute = true;
        const std::vector<int32_t> out = runOn(minSamples, 1, p);
        t::check("--abs от -32768 ограничен 32767",
              out.size() == minSamples.size() && maxOf(out) == 32767,
              out.empty() ? "пусто" : (maxOf(out) == 32767 ? "" : "получено больше 32767"));
    }

    // 2..4. свёртки окна на тех же отсчётах
    {
        const struct { int mode; const char *name; } modes[] = {
            { amp::RD_PEAK, "peak" }, { amp::RD_RMS, "rms" }, { amp::RD_AVG, "avg" },
        };
        for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
            amp::Params p;
            p.block  = 16;
            p.reduce = modes[i].mode;
            const std::vector<int32_t> out = runOn(minSamples, 1, p);
            char detail[64];
            snprintf(detail, sizeof(detail), "max %d, точек %d", maxOf(out), (int)out.size());
            t::check(modes[i].name, out.size() == 4 && maxOf(out) == 32767, detail);
        }
    }

    // 5. без --abs отрицательное значение проходит как есть
    {
        amp::Params p;
        const std::vector<int32_t> out = runOn(minSamples, 1, p);
        t::check("без --abs -32768 сохраняется", !out.empty() && out[0] == -32768);
    }

    // 6. mix усекает к нулю, как целочисленное деление в C++
    {
        std::vector<int16_t> pcm;
        pcm.push_back(-1); pcm.push_back(0);      // (-1 + 0) / 2 == 0, а не -1
        pcm.push_back(3);  pcm.push_back(0);      // (3 + 0) / 2 == 1
        amp::Params p;
        const std::vector<int32_t> out = runOn(pcm, 2, p);
        t::check("mix усекает к нулю", out.size() == 2 && out[0] == 0 && out[1] == 1);
    }

    // 7. CH_BOTH на стерео даёт по два значения на точку
    {
        std::vector<int16_t> pcm;
        for (int i = 0; i < 8; ++i) { pcm.push_back((int16_t)(100 + i)); pcm.push_back((int16_t)(-100 - i)); }
        amp::Params p;
        p.chan = amp::CH_BOTH;
        const std::vector<int32_t> out = runOn(pcm, 2, p);
        t::check("CH_BOTH: два значения на точку",
              out.size() == 16 && out[0] == 100 && out[1] == -100);
    }

    // 8. CH_BOTH на моно остаётся одним значением
    {
        std::vector<int16_t> pcm(8, 500);
        amp::Params p;
        p.chan = amp::CH_BOTH;
        const std::vector<int32_t> out = runOn(pcm, 1, p);
        t::check("CH_BOTH на моно: одно значение на точку", out.size() == 8);
    }

    // 9. limit ограничивает число точек
    {
        amp::Params p;
        p.limit = 5;
        const std::vector<int32_t> out = runOn(minSamples, 1, p);
        t::check("limit ограничивает выдачу", out.size() == 5);
    }

    // 10. неполное последнее окно всё равно выдаётся
    {
        std::vector<int16_t> pcm(10, 1000);
        amp::Params p;
        p.block = 4;                              // 2 полных окна + хвост из 2 отсчётов
        const std::vector<int32_t> out = runOn(pcm, 1, p);
        t::check("хвостовое неполное окно выдаётся", out.size() == 3);
    }

    // 11. огромный intervalMs не переполняет int
    {
        amp::Params p;
        p.intervalMs = 2000000000;                // 2e9 мс * 44100 не влезает в int
        const int block = amp::resolveBlock(p, 44100);
        char detail[64];
        snprintf(detail, sizeof(detail), "block = %d", block);
        t::check("огромный intervalMs не даёт отрицательный block", block > 0, detail);
    }

    // 12. intervalMs пересчитывается по фактической частоте
    {
        amp::Params p;
        p.intervalMs = 100;
        t::check("intervalMs 100 при 48 кГц = 4800 отсчётов",
              amp::resolveBlock(p, 48000) == 4800);
        t::check("intervalMs 100 при 22.05 кГц = 2205 отсчётов",
              amp::resolveBlock(p, 22050) == 2205);
    }

    return t::report();
}
