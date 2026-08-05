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

/// Потолок --points. Продиктован буфером подокон: на точку берётся до 32
/// подокон, а весь буфер ограничен 32768 записями.
const long long kMaxPoints = 16384;

struct Params {
    int       chan       = CH_MIX;
    int       reduce     = RD_PEAK;
    int       block      = 1;    ///< размер окна в отсчётах
    int       intervalMs = 0;    ///< если > 0 — окно задаётся в мс и вытесняет block
    long long points     = 0;    ///< если > 0 — выдать ровно столько точек на весь поток
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

/// Кадр PCM -> одно или два значения по режиму канала.
/// Общая для run() и runPoints(): иначе два пути начали бы смешивать по-разному.
inline void mixFrame(const int16_t *pcm, size_t i, int nch, int chan, int32_t *v)
{
    if (nch == 1) {
        v[0] = pcm[i];
        v[1] = v[0];
        return;
    }
    const int32_t l = pcm[i], r = pcm[i + 1];
    switch (chan) {
        case CH_LEFT:  v[0] = l;           v[1] = l;    break;
        case CH_RIGHT: v[0] = r;           v[1] = r;    break;
        case CH_BOTH:  v[0] = l;           v[1] = r;    break;
        default:       v[0] = (l + r) / 2; v[1] = v[0]; break;
    }
}

// ------------------------------------------------- подокна для --points
//
// Точка не может быть посчитана на лету: её ширина зависит от полной длины
// потока, а та известна только в конце (у сырого ADTS и MPEG-TS длительности
// в контейнере часто нет вовсе). Поэтому копятся мелкие подокна, а точки
// собираются из них по факту.

/// Одно подокно, до двух каналов.
struct Sub {
    int32_t peak[2];     ///< максимум модуля
    int64_t absSum[2];   ///< сумма модулей    -> avg
    int64_t sqSum[2];    ///< сумма квадратов  -> rms
    int32_t count;       ///< отсчётов в подокне
};

inline void subReset(Sub &s)
{
    s.peak[0]   = s.peak[1]   = 0;
    s.absSum[0] = s.absSum[1] = 0;
    s.sqSum[0]  = s.sqSum[1]  = 0;
    s.count     = 0;
}

inline void subAdd(Sub &s, const int32_t *v, int outch)
{
    for (int c = 0; c < outch; ++c) {
        const int64_t a = v[c] < 0 ? -(int64_t)v[c] : (int64_t)v[c];
        if (a > (int64_t)s.peak[c]) s.peak[c] = (int32_t)a;   // a <= 32768, влезает
        s.absSum[c] += a;
        s.sqSum[c]  += (int64_t)v[c] * (int64_t)v[c];
    }
    ++s.count;
}

/// Слияние точное: максимум остаётся максимумом, суммы складываются. Именно
/// поэтому значение точки не зависит от того, сколько раз буфер сливался.
inline void subMerge(Sub &dst, const Sub &src)
{
    for (int c = 0; c < 2; ++c) {
        if (src.peak[c] > dst.peak[c]) dst.peak[c] = src.peak[c];
        dst.absSum[c] += src.absSum[c];
        dst.sqSum[c]  += src.sqSum[c];
    }
    dst.count += src.count;
}

inline int32_t subValue(const Sub &s, int c, int reduce)
{
    if (s.count <= 0) return 0;
    switch (reduce) {
        case RD_RMS: return clampAmp((int64_t)(sqrt((double)s.sqSum[c] / s.count) + 0.5));
        case RD_AVG: return clampAmp(s.absSum[c] / s.count);
        default:     return clampAmp(s.peak[c]);
    }
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
            mixFrame(pcm.data(), i, nch, p.chan, v);

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

/// Ровно p.points точек на весь поток (меньше — только если отсчётов меньше,
/// чем запрошено точек). Возвращает число точек, через totalSamples — точное
/// число отсчётов на канал. При недопустимом p.points возвращает 0.
///
/// В отличие от run() отдаёт результат не потоком, а вектором: ширина точки
/// известна лишь по концу прохода, а вызывающему она нужна до записи заголовка.
inline long long runPoints(Decoder &dec, const Params &p,
                           std::vector<int32_t> &values, long long *totalSamples)
{
    values.clear();
    if (totalSamples) *totalSamples = 0;

    const int nch = dec.channels();
    if (nch < 1 || nch > 2) return 0;
    if (p.points < 1 || p.points > kMaxPoints) return 0;

    const long long want  = p.points;
    const int       outch = valuesPerPoint(p, nch);

    // 32 подокна на точку, но не больше 32768 записей и не меньше 2N: после
    // слияния буфер уполовинивается, и при cap >= 2N подокон остаётся не
    // меньше N — то есть каждая точка гарантированно непустая.
    size_t cap = (size_t)(want * 32);
    if (cap > 32768)                cap = 32768;
    if (cap < (size_t)(want * 2))   cap = (size_t)(want * 2);

    std::vector<Sub> buf;
    buf.reserve(cap);

    Sub cur;
    subReset(cur);
    int32_t   sub   = 1;         // ширина подокна в отсчётах
    long long total = 0;

    const size_t BUF_SAMPLES = 32768;
    std::vector<int16_t> pcm(BUF_SAMPLES);

    for (;;) {
        const size_t got = dec.read(pcm.data(), BUF_SAMPLES);
        if (!got) break;

        for (size_t i = 0; i + (size_t)nch <= got; i += (size_t)nch) {
            int32_t v[2];
            mixFrame(pcm.data(), i, nch, p.chan, v);

            subAdd(cur, v, outch);
            ++total;

            if (cur.count < sub) continue;
            buf.push_back(cur);
            subReset(cur);

            if (buf.size() < cap) continue;
            const size_t half = buf.size() / 2;      // cap всегда чётный
            for (size_t k = 0; k < half; ++k) {
                buf[k] = buf[2 * k];
                subMerge(buf[k], buf[2 * k + 1]);
            }
            buf.resize(half);
            sub *= 2;
        }
    }
    if (cur.count > 0) buf.push_back(cur);

    if (totalSamples) *totalSamples = total;
    if (buf.empty()) return 0;

    const long long S      = (long long)buf.size();
    long long       points = want < S ? want : S;    // отсчётов может быть меньше

    values.reserve((size_t)(points * outch));
    for (long long i = 0; i < points; ++i) {
        const long long from = i * S / points;
        const long long to   = (i + 1) * S / points;
        Sub acc = buf[(size_t)from];
        for (long long k = from + 1; k < to; ++k) subMerge(acc, buf[(size_t)k]);
        for (int c = 0; c < outch; ++c) values.push_back(subValue(acc, c, p.reduce));
    }

    if (p.limit >= 0 && points > p.limit) {
        points = p.limit;
        values.resize((size_t)(points * outch));
    }
    return points;
}

} // namespace amp

#endif // AMPLITUDE_ENVELOPE_H
