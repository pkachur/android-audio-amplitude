// envelope.h — общая логика превращения PCM в значения амплитуды.
// Используется и CLI (amplitude.cpp), и JNI-обёрткой (android/amplitude_jni.cpp),
// чтобы поведение на устройстве и на десктопе совпадало ровно.

#ifndef AMPLITUDE_ENVELOPE_H
#define AMPLITUDE_ENVELOPE_H

#include "decoder.h"

#include <cmath>
#include <vector>

namespace amp {

enum ChanMode { CH_MIX = 0, CH_LEFT = 1, CH_RIGHT = 2, CH_BOTH = 3 };
enum Reduce   { RD_PEAK = 0, RD_RMS  = 1, RD_AVG   = 2 };

struct Params {
    int       chan       = CH_MIX;
    int       reduce     = RD_PEAK;
    int       block      = 1;    ///< размер окна в отсчётах
    int       intervalMs = 0;    ///< если > 0 — окно задаётся в мс и вытесняет block
    bool      absolute   = false;
    long long limit      = -1;   ///< максимум точек, -1 = без ограничения
};

/// Размер окна в отсчётах с учётом intervalMs и частоты дискретизации.
inline int resolveBlock(const Params &p, int sampleRate)
{
    if (p.intervalMs > 0 && sampleRate > 0) {
        long long b = (long long)sampleRate * p.intervalMs / 1000;
        if (b < 1) return 1;
        if (b > 0x7fffffffLL) return 0x7fffffff;   // абсурдно большой intervalMs
        return (int)b;
    }
    return p.block < 1 ? 1 : p.block;
}

/// Приведение к диапазону PCM16. Нужно потому, что модуль минимального
/// отсчёта выходит за него: -(-32768) = 32768.
inline int32_t clampAmp(int64_t v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int32_t)v;
}

/// Сколько чисел приходится на одну точку: 2 только для CH_BOTH со стерео.
inline int valuesPerPoint(const Params &p, int channels)
{
    return (p.chan == CH_BOTH && channels == 2) ? 2 : 1;
}

/// Прогоняет весь поток. Sink — вызываемый объект: void(const int32_t *v, int n).
/// Возвращает число выданных точек.
template <class Sink>
long long run(Decoder &dec, const Params &p, Sink &sink)
{
    const int nch = dec.channels();
    if (nch < 1 || nch > 2) return 0;
    if (p.limit == 0) return 0;

    const int outch = valuesPerPoint(p, nch);
    const int block = resolveBlock(p, dec.sampleRate());

    const size_t BUF_SAMPLES = 32768;
    std::vector<int16_t> pcm(BUF_SAMPLES);

    int64_t accPeak[2] = {0, 0};
    int64_t accAbs[2]  = {0, 0};
    // Целая сумма: квадраты PCM16 не переполняют int64 на потоках до полусотни
    // часов, а double начал бы округлять с 2^53 и сделал бы результат зависимым
    // от порядка сложения — тогда runPoints со слиянием подокон разошёлся бы
    // с run() в последнем бите.
    int64_t accSq[2]   = {0, 0};
    int     inBlock    = 0;
    long long points   = 0;
    bool    stop       = false;

    while (!stop) {
        const size_t got = dec.read(pcm.data(), BUF_SAMPLES);
        if (!got) break;

        for (size_t i = 0; i + (size_t)nch <= got; i += (size_t)nch) {
            int32_t v[2];
            if (nch == 1) {
                v[0] = pcm[i];
                v[1] = v[0];
            } else {
                const int32_t l = pcm[i], r = pcm[i + 1];
                switch (p.chan) {
                    case CH_LEFT:  v[0] = l;           v[1] = l;    break;
                    case CH_RIGHT: v[0] = r;           v[1] = r;    break;
                    case CH_BOTH:  v[0] = l;           v[1] = r;    break;
                    default:       v[0] = (l + r) / 2; v[1] = v[0]; break;
                }
            }

            if (block == 1) {
                int32_t w[2] = { v[0], v[1] };
                if (p.absolute) {
                    w[0] = clampAmp(w[0] < 0 ? -(int64_t)w[0] : w[0]);
                    w[1] = clampAmp(w[1] < 0 ? -(int64_t)w[1] : w[1]);
                }
                sink(w, outch);
            } else {
                for (int c = 0; c < outch; ++c) {
                    const int64_t a = v[c] < 0 ? -(int64_t)v[c] : (int64_t)v[c];
                    if (a > accPeak[c]) accPeak[c] = a;
                    accAbs[c] += a;
                    accSq[c]  += (int64_t)v[c] * (int64_t)v[c];
                }
                if (++inBlock < block) continue;

                int32_t w[2] = {0, 0};
                for (int c = 0; c < outch; ++c) {
                    switch (p.reduce) {
                        case RD_RMS: w[c] = clampAmp((int64_t)(sqrt((double)accSq[c] / inBlock) + 0.5)); break;
                        case RD_AVG: w[c] = clampAmp(accAbs[c] / inBlock);                              break;
                        default:     w[c] = clampAmp(accPeak[c]);                                        break;
                    }
                    accPeak[c] = 0; accAbs[c] = 0; accSq[c] = 0;
                }
                inBlock = 0;
                sink(w, outch);
            }

            ++points;
            if (p.limit >= 0 && points >= p.limit) { stop = true; break; }
        }
    }

    // хвост неполного окна
    if (!stop && block > 1 && inBlock > 0) {
        int32_t w[2] = {0, 0};
        for (int c = 0; c < outch; ++c) {
            switch (p.reduce) {
                case RD_RMS: w[c] = clampAmp((int64_t)(sqrt((double)accSq[c] / inBlock) + 0.5)); break;
                case RD_AVG: w[c] = clampAmp(accAbs[c] / inBlock);                              break;
                default:     w[c] = clampAmp(accPeak[c]);                                        break;
            }
        }
        sink(w, outch);
        ++points;
    }

    return points;
}

} // namespace amp

#endif // AMPLITUDE_ENVELOPE_H
