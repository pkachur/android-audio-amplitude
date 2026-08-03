# -*- coding: utf-8 -*-
"""Функциональные тесты amplitude.exe.

Эталон — PCM, декодированный ffmpeg из того же mp3 (tests/ref_stereo.pcm).
Ожидаемые значения считаются здесь независимо, на Python, и сверяются с выходом
программы. Допуск ±2 LSB: minimp3 и ffmpeg округляют по-разному (проверено —
расхождение декодера не превышает 1 LSB).

Запуск:  python tests\\run_tests.py [путь_к_amplitude.exe]
"""
import array
import math
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'amplitude.exe')
MP3 = os.path.join(HERE, 'test.mp3')
REF = os.path.join(HERE, 'ref_stereo.pcm')
MP3_MONO = os.path.join(HERE, 'test_mono.mp3')
REF_MONO = os.path.join(HERE, 'ref_mono.pcm')
MP3_LOUD = os.path.join(HERE, 'test_loud.mp3')
TOL = 2

if not os.path.exists(EXE):
    sys.exit('не найден бинарник: %s\nСоберите: make MEDIANDK=0  (или укажите путь аргументом)' % EXE)
for path in (MP3, REF, MP3_MONO, REF_MONO, MP3_LOUD):
    if not os.path.exists(path):
        sys.exit('нет тестовых данных: %s\nПодготовьте их из любого аудиофайла:\n'
                 '    python tests/make_fixtures.py <аудиофайл>' % path)

failures = []


def check(name, ok, detail=''):
    print('[%s] %s%s' % ('OK ' if ok else 'FAIL', name, ('  -- ' + detail) if detail else ''))
    if not ok:
        failures.append(name)


def load_samples(path):
    a = array.array('h')
    with open(path, 'rb') as f:
        a.frombytes(f.read())
    if sys.byteorder == 'big':
        a.byteswap()
    return a


def load_ref():
    a = load_samples(REF)
    return [(a[i], a[i + 1]) for i in range(0, len(a), 2)]


def load_ints(raw):
    a = array.array('i')
    a.frombytes(raw)
    if sys.byteorder == 'big':
        a.byteswap()
    return a


def run(args, stdin_bytes=None):
    cmd = [EXE] + args + ['-q']
    p = subprocess.run(cmd, input=stdin_bytes, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE)
    if p.returncode != 0:
        raise RuntimeError('%s -> код %d: %s' % (' '.join(cmd), p.returncode,
                                                 p.stderr.decode('utf-8', 'replace')))
    return p.stdout


def run_text(args, stdin_bytes=None):
    out = run(args, stdin_bytes).decode('ascii')
    return [[int(x) for x in line.split()] for line in out.split('\n') if line.strip()]


def trunc2(x):
    """Целочисленное деление на 2 с усечением к нулю — как в C++."""
    return int(x / 2)


def reduce_block(vals, mode):
    n = len(vals)
    if mode == 'peak':
        return max(abs(v) for v in vals)
    if mode == 'avg':
        return sum(abs(v) for v in vals) // n
    return int(math.sqrt(sum(float(v) * v for v in vals) / n) + 0.5)


def blocks(seq, size):
    for i in range(0, len(seq), size):
        yield seq[i:i + size]


frames = load_ref()
print('эталон: %d отсчётов, %.2f с\n' % (len(frames), len(frames) / 44100.0))

# 1. поотсчётный mix ------------------------------------------------------
got = run_text([MP3])
exp = [trunc2(l + r) for l, r in frames]
ok = len(got) == len(exp) and all(abs(g[0] - e) <= TOL for g, e in zip(got, exp))
check('поотсчётный mix: количество и значения', ok,
      'получено %d, ожидалось %d' % (len(got), len(exp)))

# 2. каналы отдельно ------------------------------------------------------
got = run_text([MP3, '-c', 'both'])
ok = (len(got) == len(frames)
      and all(len(g) == 2 for g in got)
      and all(abs(g[0] - f[0]) <= TOL and abs(g[1] - f[1]) <= TOL
              for g, f in zip(got, frames)))
check('-c both: два канала совпадают с эталоном', ok)

got = run_text([MP3, '-c', 'left', '-n', '500'])
ok = len(got) == 500 and all(abs(g[0] - f[0]) <= TOL for g, f in zip(got, frames[:500]))
check('-c left + --limit 500', ok, 'получено %d строк' % len(got))

got = run_text([MP3, '-c', 'right', '-n', '500'])
ok = len(got) == 500 and all(abs(g[0] - f[1]) <= TOL for g, f in zip(got, frames[:500]))
check('-c right + --limit 500', ok)

# 3. окно в миллисекундах -------------------------------------------------
for ms, mode in (('100', 'rms'), ('100', 'peak'), ('50', 'avg'), ('1000', 'rms')):
    size = 44100 * int(ms) // 1000
    got = run_text([MP3, '--ms', ms, '-r', mode])
    mix = [trunc2(l + r) for l, r in frames]
    exp = [reduce_block(b, mode) for b in blocks(mix, size)]
    ok = len(got) == len(exp) and all(abs(g[0] - e) <= TOL for g, e in zip(got, exp))
    worst = max((abs(g[0] - e) for g, e in zip(got, exp)), default=0)
    check('--ms %s --reduce %s' % (ms, mode), ok,
          'точек %d (ожидалось %d), max расхождение %d' % (len(got), len(exp), worst))

# 4. --ms эквивалентен --block с тем же размером окна ---------------------
a = run_text([MP3, '--ms', '100', '-r', 'rms'])
b = run_text([MP3, '--block', '4410', '-r', 'rms'])
check('--ms 100 == --block 4410 (44100 Гц)', a == b)

# 5. --abs ----------------------------------------------------------------
got = run_text([MP3, '--abs', '-n', '2000'])
exp = [abs(trunc2(l + r)) for l, r in frames[:2000]]
ok = len(got) == 2000 and all(g[0] >= 0 for g in got) and \
     all(abs(g[0] - e) <= TOL for g, e in zip(got, exp))
check('--abs даёт модуль', ok)

# 6. --raw совпадает с текстовым выводом ----------------------------------
raw = run([MP3, '--ms', '100', '-r', 'peak', '-c', 'both', '--raw'])
vals = array.array('i')
vals.frombytes(raw)
if sys.byteorder == 'big':
    vals.byteswap()
txt = run_text([MP3, '--ms', '100', '-r', 'peak', '-c', 'both'])
flat = [v for row in txt for v in row]
check('--raw (int32 LE) == текстовый вывод', list(vals) == flat,
      '%d значений' % len(vals))

# 7. чтение из stdin ------------------------------------------------------
with open(MP3, 'rb') as f:
    data = f.read()
got = run_text(['-', '--ms', '100', '-r', 'rms'], stdin_bytes=data)
exp = run_text([MP3, '--ms', '100', '-r', 'rms'])
check('stdin даёт то же, что и файл', got == exp)

# 8. заголовок ------------------------------------------------------------
out = run([MP3, '--header', '-n', '1']).decode('ascii')
first = out.split('\n')[0]
ok = first.startswith('# ') and 'hz=44100' in first and 'channels=2' in first
check('--header содержит параметры потока', ok, first)

# 9. явный выбор бэкенда --------------------------------------------------
got = run_text([MP3, '--backend', 'minimp3', '-n', '10'])
check('--backend minimp3 работает', len(got) == 10)

p = subprocess.run([EXE, MP3, '--backend', 'media', '-q'],
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
ok = p.returncode != 0 and 'Android' in p.stderr.decode('utf-8', 'replace')
check('--backend media вне Android даёт внятную ошибку', ok,
      p.stderr.decode('utf-8', 'replace').strip())

# 10. моно — отдельная ветка кода, стереофикстурой не покрывается -----------
mono = load_samples(REF_MONO)
got = run_text([MP3_MONO])
ok = len(got) == len(mono) and all(abs(g[0] - e) <= TOL for g, e in zip(got, mono))
check('моно: поотсчётные значения', ok,
      'получено %d, ожидалось %d' % (len(got), len(mono)))

got = run_text([MP3_MONO, '--ms', '100', '-r', 'rms'])
exp = [reduce_block(b, 'rms') for b in blocks(list(mono), 4410)]
worst = max((abs(g[0] - e) for g, e in zip(got, exp)), default=0)
check('моно: --ms 100 --reduce rms', len(got) == len(exp) and worst <= TOL,
      'точек %d (ожидалось %d), max расхождение %d' % (len(got), len(exp), worst))

got = run_text([MP3_MONO, '-c', 'both', '-n', '50'])
check('моно + -c both даёт одно значение в строке',
      len(got) == 50 and all(len(g) == 1 for g in got))

first = run([MP3_MONO, '--header', '-n', '1']).decode('ascii').split('\n')[0]
check('моно: в заголовке channels=1', 'channels=1' in first, first)

# 11. перегруженный сигнал: выход не должен вылезать за int16 --------------
vals = load_ints(run([MP3_LOUD, '-c', 'both', '--raw']))
lo, hi = min(vals), max(vals)
check('громкий файл: значения в пределах -32768..32767', lo >= -32768 and hi <= 32767,
      'min %d, max %d' % (lo, hi))

va = load_ints(run([MP3_LOUD, '--abs', '--raw']))
check('--abs не превышает 32767 (модуль -32768 равен 32768)',
      min(va) >= 0 and max(va) <= 32767, 'max %d' % max(va))

vp = load_ints(run([MP3_LOUD, '--ms', '10', '-r', 'peak', '--raw']))
check('peak не превышает 32767', max(vp) <= 32767, 'max %d' % max(vp))

vr = load_ints(run([MP3_LOUD, '--ms', '10', '-r', 'rms', '--raw']))
check('rms не превышает 32767', max(vr) <= 32767, 'max %d' % max(vr))
print('     %s' % ('в фикстуре есть отсчёты -32768 — кламп задействован и здесь'
                   if lo == -32768 else
                   'отсчёт -32768 в фикстуре не встретился (обычное дело для MP3);'
                   ' сам кламп детерминированно проверяется в tests/test_envelope.cpp'))

# 12. вывод в файл ---------------------------------------------------------
tmp_out = os.path.join(HERE, 'tmp_out.txt')
run([MP3, '--ms', '100', '-r', 'rms', '-o', tmp_out])
with open(tmp_out, 'r') as f:
    from_file = [[int(x) for x in line.split()] for line in f if line.strip()]
check('-o FILE даёт то же, что stdout', from_file == run_text([MP3, '--ms', '100', '-r', 'rms']),
      '%d строк' % len(from_file))
os.remove(tmp_out)

# 13. --limit 0 ------------------------------------------------------------
p = subprocess.run([EXE, MP3, '-n', '0', '-q'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
check('--limit 0 даёт пустой вывод и код 0',
      p.returncode == 0 and p.stdout.strip() == b'', 'код %d' % p.returncode)

# 14. негативные случаи: ошибка должна быть видна кодом возврата ------------
def expect_fail(args, name, expect_in_message=None):
    q = subprocess.run([EXE] + args + ['-q'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    msg = q.stderr.decode('utf-8', 'replace').strip()
    ok = q.returncode != 0 and msg != ''
    if ok and expect_in_message:
        ok = expect_in_message in msg          # сообщение должно быть по существу
    check(name, ok, 'код %d: %s' % (q.returncode, msg[:90]))


garbage = os.path.join(HERE, 'tmp_garbage.bin')
with open(garbage, 'wb') as f:
    f.write(bytes(range(256)) * 16)          # заведомо не аудио, без MPEG-синхросигнала
empty = os.path.join(HERE, 'tmp_empty.bin')
open(empty, 'wb').close()

expect_fail([garbage], 'мусорный файл: сообщение про нераспознанный формат', 'не распознан')
expect_fail([empty], 'пустой файл: сообщение про пустой файл', 'пуст')
expect_fail([os.path.join(HERE, 'нет-такого-файла.mp3')], 'несуществующий файл отвергается',
            'не удалось открыть')
expect_fail([HERE], 'каталог вместо файла отвергается')

# 15. контейнеры, которые не должны попадать в minimp3 -----------------------
ts_file = os.path.join(HERE, 'tmp_fake.ts')
pkt = bytearray(188 * 3)
for i in range(3):
    pkt[i * 188] = 0x47
for i in range(20, 180):
    pkt[i] = 0xFF                            # стаффинг PAT/PMT — ловушка для сниффера
with open(ts_file, 'wb') as f:
    f.write(bytes(pkt))
expect_fail([ts_file], 'MPEG-TS не уходит в minimp3', 'не распознан')
os.remove(ts_file)

# Валидный заголовок MPEG за пределами первых 16 байт: раньше голова читалась
# по 16 байт и такой файл нюхался как «не MP3». Теперь он уходит в minimp3
# первым, и в ошибке должно быть видно именно minimp3.
deep = os.path.join(HERE, 'tmp_deep_sync.bin')
body = bytearray(800)
body[600:604] = b'\xFF\xFB\x90\x00'
with open(deep, 'wb') as f:
    f.write(bytes(body))
expect_fail([deep], 'кадр MPEG на смещении 600 виден сниферу', 'minimp3')
os.remove(deep)

os.remove(garbage)
os.remove(empty)

if failures:
    print('\nПРОВАЛЕНО %d: %s' % (len(failures), ', '.join(failures)))
    sys.exit(1)
print('\nВСЕ ТЕСТЫ ПРОШЛИ')
