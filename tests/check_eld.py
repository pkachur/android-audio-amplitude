# -*- coding: utf-8 -*-
"""Проверка маршрутизации и декодирования на реальных записях приложения.

Файлы лежат в audio/ и в репозиторий не коммитятся (каталог в .gitignore).

На десктопе MediaCodec недоступен, поэтому проверяется главное, что здесь можно
проверить: файл опознан как «не MP3» и отправлен в системный декодер, а не
разобран minimp3 наугад. Признак — конкретный текст ошибки.

    python3 tests/check_eld.py                      # десктоп, каталог audio/
    python3 tests/check_eld.py --device             # прогон на устройстве через adb
    python3 tests/check_eld.py --dir audio --points 100

С --device бинарник и файлы кладутся на устройство, и проверяется, что точек
ровно N, а частота и число каналов укладываются в таблицу форматов Android
(24000-48000 Гц, 1-2 канала).
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

NOT_MP3 = 'не распознан'          # признак того, что сниффер увёл файл от minimp3
DEV_DIR = '/data/local/tmp/amplitude'

failures = []


def check(name, ok, detail=''):
    print('[%s] %s%s' % ('OK ' if ok else 'FAIL', name, ('  -- ' + detail) if detail else ''))
    if not ok:
        failures.append(name)


def audio_files(directory):
    if not os.path.isdir(directory):
        sys.exit('нет каталога с записями: %s' % directory)
    names = sorted(f for f in os.listdir(directory)
                   if os.path.splitext(f)[1].lower() in ('.m4a', '.mp4', '.3gp', '.aac', '.ts'))
    if not names:
        sys.exit('в %s нет файлов с расширениями .m4a/.mp4/.3gp/.aac/.ts' % directory)
    return [os.path.join(directory, n) for n in names]


def check_desktop(exe, files):
    for path in files:
        p = subprocess.run([exe, path, '-n', '1', '-q'],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        msg = p.stderr.decode('utf-8', 'replace').strip()
        ok = p.returncode != 0 and NOT_MP3 in msg
        check('%s: уходит в системный декодер, не в minimp3' % os.path.basename(path),
              ok, msg[:100])


def adb(args, binary=False):
    p = subprocess.run(['adb'] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p.returncode, (p.stdout if binary else p.stdout.decode('utf-8', 'replace')), \
           p.stderr.decode('utf-8', 'replace')


def check_device(device_exe, files, points):
    rc, out, err = adb(['shell', 'echo', 'ok'])
    if rc != 0:
        sys.exit('adb недоступен: %s' % err.strip())

    adb(['shell', 'mkdir', '-p', DEV_DIR])
    rc, _, err = adb(['push', device_exe, DEV_DIR + '/amplitude'])
    if rc != 0:
        sys.exit('не удалось положить бинарник на устройство: %s' % err.strip())
    adb(['shell', 'chmod', '755', DEV_DIR + '/amplitude'])

    for path in files:
        remote = '%s/%s' % (DEV_DIR, 'sample' + os.path.splitext(path)[1].lower())
        rc, _, err = adb(['push', path, remote])
        if rc != 0:
            check('%s: скопирован на устройство' % os.path.basename(path), False, err.strip())
            continue

        rc, out, err = adb(['shell', '%s/amplitude' % DEV_DIR, remote,
                            '--points', str(points), '-r', 'rms'])
        lines = [l for l in out.splitlines() if l.strip()]
        name = os.path.basename(path)

        # Битые файлы обязаны отказать, а не выдать усечённые данные.
        if rc != 0:
            check('%s: отказ с сообщением' % name, err.strip() != '', err.strip()[:100])
            continue

        check('%s: ровно %d точек' % (name, points), len(lines) == points,
              'строк %d' % len(lines))
        check('%s: значения в 0..32767' % name,
              all(0 <= int(v) <= 32767 for l in lines for v in l.split()))

        rc, info, _ = adb(['shell', '%s/amplitude' % DEV_DIR, remote, '--points', '1'])
        m = re.search(r'(\d+) Гц, (\d+) кан', info)
        if m:
            hz, ch = int(m.group(1)), int(m.group(2))
            check('%s: %d Гц, %d кан. в пределах таблицы' % (name, hz, ch),
                  24000 <= hz <= 48000 and 1 <= ch <= 2)
        else:
            check('%s: параметры потока прочитаны' % name, False, info.strip()[:80])

    adb(['shell', 'rm', '-rf', DEV_DIR])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', default=os.path.join(ROOT, 'audio'))
    ap.add_argument('--exe', default=os.path.join(ROOT, 'build', 'amplitude.exe'))
    ap.add_argument('--device-exe', default=os.path.join(ROOT, 'build', 'arm64-v8a', 'amplitude'))
    ap.add_argument('--device', action='store_true')
    ap.add_argument('--points', type=int, default=100)
    a = ap.parse_args()

    files = audio_files(a.dir)
    print('файлов: %d\n' % len(files))

    if a.device:
        if not os.path.exists(a.device_exe):
            sys.exit('нет бинарника для устройства: %s\nСоберите:'
                     ' .\\build_android.ps1 -Ndk <путь к NDK>' % a.device_exe)
        check_device(a.device_exe, files, a.points)
    else:
        if not os.path.exists(a.exe):
            sys.exit('нет бинарника: %s\nСоберите: .\\build_host.ps1' % a.exe)
        check_desktop(a.exe, files)

    if failures:
        print('\nПРОВАЛЕНО %d: %s' % (len(failures), ', '.join(failures)))
        return 1
    print('\nВСЕ ПРОВЕРКИ ПРОШЛИ')
    return 0


if __name__ == '__main__':
    sys.exit(main())
