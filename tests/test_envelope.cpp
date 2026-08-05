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

std::vector<int32_t> pointsOn(const std::vector<int16_t> &pcm, int ch, const amp::Params &p,
                              long long *totalSamples = 0, long long *pointsOut = 0)
{
    FakeDecoder dec(pcm, 44100, ch);
    std::vector<int32_t> out;
    long long total = 0;
    const long long pts = amp::runPoints(dec, p, out, &total);
    if (totalSamples) *totalSamples = total;
    if (pointsOut)    *pointsOut    = pts;
    return out;
}

/// Прямой расчёт свёртки по окну — эталон, независимый от подокон.
int32_t reduceDirect(const std::vector<int16_t> &pcm, size_t from, size_t to, int mode)
{
    int64_t peak = 0, absSum = 0, sqSum = 0;
    const int64_t n = (int64_t)(to - from);
    for (size_t i = from; i < to; ++i) {
        const int64_t a = pcm[i] < 0 ? -(int64_t)pcm[i] : (int64_t)pcm[i];
        if (a > peak) peak = a;
        absSum += a;
        sqSum  += (int64_t)pcm[i] * (int64_t)pcm[i];
    }
    if (mode == amp::RD_RMS) return amp::clampAmp((int64_t)(sqrt((double)sqSum / n) + 0.5));
    if (mode == amp::RD_AVG) return amp::clampAmp(absSum / n);
    return amp::clampAmp(peak);
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

    // 13. rms считается по целой сумме квадратов и округляется к ближайшему
    {
        std::vector<int16_t> pcm;
        pcm.push_back(3); pcm.push_back(4);      // sqrt((9+16)/2) = 3.5355 -> 4
        amp::Params p;
        p.block  = 2;
        p.reduce = amp::RD_RMS;
        const std::vector<int32_t> out = runOn(pcm, 1, p);
        char detail[32];
        snprintf(detail, sizeof(detail), "получено %d", out.empty() ? -1 : out[0]);
        t::check("rms(3,4) == 4", out.size() == 1 && out[0] == 4, detail);
    }

    // 14. ровно N точек на длине, не кратной N
    {
        std::vector<int16_t> pcm;
        for (int i = 0; i < 1001; ++i) pcm.push_back((int16_t)(i % 1000));
        amp::Params p;
        p.points = 7;
        long long pts = 0;
        const std::vector<int32_t> out = pointsOn(pcm, 1, p, 0, &pts);
        char detail[64];
        snprintf(detail, sizeof(detail), "точек %lld, значений %d", pts, (int)out.size());
        t::check("--points 7 на 1001 отсчёте даёт ровно 7", pts == 7 && out.size() == 7, detail);
    }

    // 15. N = 1: одна точка на весь поток
    {
        std::vector<int16_t> pcm;
        for (int i = 0; i < 500; ++i) pcm.push_back((int16_t)(i - 250));
        amp::Params p;
        p.points = 1;
        const std::vector<int32_t> out = pointsOn(pcm, 1, p);
        const int32_t exp = reduceDirect(pcm, 0, pcm.size(), amp::RD_PEAK);
        char detail[64];
        snprintf(detail, sizeof(detail), "получено %d, ожидалось %d",
                 out.empty() ? -1 : out[0], exp);
        t::check("--points 1 = peak по всему потоку", out.size() == 1 && out[0] == exp, detail);
    }

    // 16. длина ровно N
    {
        std::vector<int16_t> pcm(64, 1000);
        amp::Params p;
        p.points = 64;
        const std::vector<int32_t> out = pointsOn(pcm, 1, p);
        t::check("--points 64 на 64 отсчётах даёт 64 точки", out.size() == 64);
    }

    // 17. отсчётов меньше, чем точек: выдаём столько, сколько есть
    {
        std::vector<int16_t> pcm(10, 500);
        amp::Params p;
        p.points = 100;
        long long total = 0, pts = 0;
        const std::vector<int32_t> out = pointsOn(pcm, 1, p, &total, &pts);
        char detail[64];
        snprintf(detail, sizeof(detail), "точек %lld при %lld отсчётах", pts, total);
        t::check("--points 100 на 10 отсчётах даёт 10 точек",
                 pts == 10 && out.size() == 10 && total == 10, detail);
    }

    // 18. точность слияния: 512 отсчётов и N = 4 — степени двойки, поэтому
    //     границы точек ложатся ровно, а слияний по дороге происходит несколько
    {
        std::vector<int16_t> pcm;
        for (int i = 0; i < 512; ++i) pcm.push_back((int16_t)((i * 37) % 4001 - 2000));
        const int modes[3] = { amp::RD_PEAK, amp::RD_RMS, amp::RD_AVG };
        const char *names[3] = { "peak", "rms", "avg" };
        for (int m = 0; m < 3; ++m) {
            amp::Params p;
            p.points = 4;
            p.reduce = modes[m];
            const std::vector<int32_t> out = pointsOn(pcm, 1, p);
            bool ok = out.size() == 4;
            for (size_t i = 0; ok && i < out.size(); ++i)
                ok = out[i] == reduceDirect(pcm, i * 128, (i + 1) * 128, modes[m]);
            char detail[96];
            snprintf(detail, sizeof(detail), "слияние подокон, %s", names[m]);
            t::check("--points: значения совпадают с прямым расчётом", ok, detail);
        }
    }

    // 19. CH_BOTH: два значения на точку
    {
        std::vector<int16_t> pcm;
        for (int i = 0; i < 512; ++i) { pcm.push_back(1000); pcm.push_back(-2000); }
        amp::Params p;
        p.points = 4;
        p.chan   = amp::CH_BOTH;
        const std::vector<int32_t> out = pointsOn(pcm, 2, p);
        t::check("--points + CH_BOTH: два значения на точку",
                 out.size() == 8 && out[0] == 1000 && out[1] == 2000);
    }

    // 20. limit обрезает точки
    {
        std::vector<int16_t> pcm(1000, 700);
        amp::Params p;
        p.points = 100;
        p.limit  = 5;
        long long pts = 0;
        const std::vector<int32_t> out = pointsOn(pcm, 1, p, 0, &pts);
        t::check("--points 100 + limit 5 даёт 5 точек", pts == 5 && out.size() == 5);
    }

    // 21. граница диапазона: сплошной -32768
    {
        std::vector<int16_t> pcm(256, -32768);
        amp::Params p;
        p.points = 4;
        const std::vector<int32_t> out = pointsOn(pcm, 1, p);
        t::check("--points: -32768 ограничивается 32767",
                 out.size() == 4 && maxOf(out) == 32767);
    }

    // 22. недопустимые значения points дают ноль точек и пустой результат
    {
        std::vector<int16_t> pcm(100, 100);
        amp::Params p;
        p.points = amp::kMaxPoints + 1;
        long long pts = 0;
        const std::vector<int32_t> out = pointsOn(pcm, 1, p, 0, &pts);
        t::check("points больше потолка -> 0 точек", pts == 0 && out.empty());
    }

    return t::report();
}
