# -*- coding: utf-8 -*-
"""Готовит тестовые фикстуры из любого аудиофайла.

    python tests/make_fixtures.py <аудиофайл> [--ffmpeg путь]

Создаёт:
    tests/test.mp3        — исходник, перекодированный в MP3 CBR 192 кбит/с
    tests/ref_stereo.pcm  — эталонный PCM (s16le, стерео, интерливед) от ffmpeg

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
    mp3 = os.path.join(HERE, 'test.mp3')
    pcm = os.path.join(HERE, 'ref_stereo.pcm')

    print('ffmpeg: %s' % ff)
    subprocess.run([ff, '-hide_banner', '-loglevel', 'error', '-y',
                    '-i', args.source, '-c:a', 'libmp3lame', '-b:a', '192k',
                    '-ac', '2', mp3], check=True)
    # Эталон снимается именно с test.mp3, а не с исходника: сверяем декодирование
    # одного и того же битстрима двумя разными декодерами.
    subprocess.run([ff, '-hide_banner', '-loglevel', 'error', '-y',
                    '-i', mp3, '-f', 's16le', '-acodec', 'pcm_s16le', pcm],
                   check=True)

    frames = os.path.getsize(pcm) // 4
    print('%s  (%d КБ)' % (mp3, os.path.getsize(mp3) // 1024))
    print('%s  (%d отсчётов, %.2f с при 44.1 кГц)' % (pcm, frames, frames / 44100.0))
    print('\nТеперь: python tests/run_tests.py <путь к amplitude>')


if __name__ == '__main__':
    main()
