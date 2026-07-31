# -*- coding: utf-8 -*-
"""Сверка выхода mp3amp с эталонным PCM от ffmpeg.

ref  — s16le stereo interleaved (ffmpeg -f s16le)
ours — int32 le stereo interleaved (mp3amp -c both --raw)

MP3 — декодер с плавающей точкой в стандарте, поэтому побитового совпадения
между minimp3 и ffmpeg быть не обязано; проверяем, что расхождение исчезающе мало.
"""
import array
import math
import sys

ref_path, ours_path = sys.argv[1], sys.argv[2]

ref = array.array('h')
with open(ref_path, 'rb') as f:
    ref.frombytes(f.read())
ours = array.array('i')
with open(ours_path, 'rb') as f:
    ours.frombytes(f.read())

if sys.byteorder == 'big':
    ref.byteswap(); ours.byteswap()

print('ref  значений: %d' % len(ref))
print('ours значений: %d' % len(ours))

n = min(len(ref), len(ours))
if len(ref) != len(ours):
    print('!! длины различаются на %d' % abs(len(ref) - len(ours)))

# диапазон значений
print('ours min/max: %d / %d' % (min(ours), max(ours)))
print('ref  min/max: %d / %d' % (min(ref), max(ref)))

maxdiff = 0
maxdiff_at = -1
sumsq_err = 0.0
sumsq_sig = 0.0
over = 0
for i in range(n):
    d = ours[i] - ref[i]
    if d < 0:
        d = -d
    if d > maxdiff:
        maxdiff, maxdiff_at = d, i
    if d > 1:
        over += 1
    sumsq_err += float(d) * d
    sumsq_sig += float(ref[i]) * ref[i]

rms_err = math.sqrt(sumsq_err / n) if n else 0.0
rms_sig = math.sqrt(sumsq_sig / n) if n else 0.0
print('max |ours-ref|      : %d (индекс %d)' % (maxdiff, maxdiff_at))
print('отсчётов с |d| > 1  : %d (%.4f%%)' % (over, 100.0 * over / n))
print('RMS ошибки          : %.4f' % rms_err)
print('RMS сигнала         : %.1f' % rms_sig)
if rms_err > 0 and rms_sig > 0:
    print('SNR                 : %.1f dB' % (20 * math.log10(rms_sig / rms_err)))
else:
    print('SNR                 : идеально (ошибки нет)')

ok = (len(ref) == len(ours)) and maxdiff <= 2
print('РЕЗУЛЬТАТ: %s' % ('OK' if ok else 'РАСХОЖДЕНИЕ'))
sys.exit(0 if ok else 1)
