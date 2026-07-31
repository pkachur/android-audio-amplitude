# -*- coding: utf-8 -*-
"""Готовит тестовые фикстуры из любого аудиофайла.

    python tests/make_fixtures.py <аудиофайл> [--ffmpeg путь]

Создаёт:
    tests/test.mp3        — исходник, перекодированный в MP3 CBR 192 кбит/с (стерео)
    tests/ref_stereo.pcm  — эталонный PCM (s16le, стерео, интерливед) от ffmpeg
    tests/test_mono.mp3   — то же в моно (покрывает отдельную ветку кода)
    tests/ref_mono.pcm    — эталонный PCM для моно
    tests/test_loud.mp3   — синтетический перегруженный сигнал: проверяет, что
                            выход не выходит за 32767 (модуль -32768 равен 32768)

Эталон нужен, чтобы run_tests.py сверял выход amplitude с независимым
декодером и независимо пересчитанными свёртками. Годится любой файл, который
понимает ffmpeg (mp3, m4a, wav, flac...). Осмысленный тест получается на
записи хотя бы в несколько секунд с меняющейся громкостью.

ffmpeg берётся из PATH; если его нет — из пакета imageio-ffmpeg
(pip install imageio-ffmpeg).
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def find_ffmpeg(explicit=None):
    if explicit:
        return explicit
    found = shutil.which('ffmpeg')
    if found:
        return found
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except ImportError:
        sys.exit('ffmpeg не найден. Поставьте его в PATH либо выполните:\n'
                 '    pip install imageio-ffmpeg')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('source', help='любой аудиофайл-источник')
    ap.add_argument('--ffmpeg', help='путь к ffmpeg')
    args = ap.parse_args()

    if not os.path.isfile(args.source):
        sys.exit('нет такого файла: %s' % args.source)

    ff = find_ffmpeg(args.ffmpeg)
    print('ffmpeg: %s' % ff)

    def run(cmd):
        subprocess.run([ff, '-hide_banner', '-loglevel', 'error', '-y'] + cmd, check=True)

    def encode(channels, dst):
        run(['-i', args.source, '-c:a', 'libmp3lame', '-b:a', '192k',
             '-ac', str(channels), dst])

    def reference(src, channels, dst):
        # Эталон снимается именно с mp3, а не с исходника: сверяем декодирование
        # одного и того же битстрима двумя разными декодерами.
        run(['-i', src, '-f', 's16le', '-acodec', 'pcm_s16le', '-ac', str(channels), dst])

    p = lambda name: os.path.join(HERE, name)

    encode(2, p('test.mp3'))
    reference(p('test.mp3'), 2, p('ref_stereo.pcm'))
    encode(1, p('test_mono.mp3'))
    reference(p('test_mono.mp3'), 1, p('ref_mono.pcm'))

    # Перегруженная синусоида: после клиппирования появляются отсчёты -32768,
    # модуль которых (32768) выходит за объявленный диапазон, если не ограничить.
    run(['-f', 'lavfi', '-i', 'sine=frequency=440:duration=2:sample_rate=44100',
         '-af', 'volume=20dB', '-ac', '2', '-c:a', 'libmp3lame', '-b:a', '320k',
         p('test_loud.mp3')])

    for name, per_frame in (('ref_stereo.pcm', 4), ('ref_mono.pcm', 2)):
        frames = os.path.getsize(p(name)) // per_frame
        print('%s  (%d отсчётов, %.2f с при 44.1 кГц)' % (name, frames, frames / 44100.0))
    for name in ('test.mp3', 'test_mono.mp3', 'test_loud.mp3'):
        print('%s  (%d КБ)' % (name, os.path.getsize(p(name)) // 1024))
    print('\nТеперь: python tests/run_tests.py <путь к amplitude>')


if __name__ == '__main__':
    main()
