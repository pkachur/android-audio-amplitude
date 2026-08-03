# SDD ledger — plan: docs/superpowers/plans/2026-08-03-points-and-aac-eld.md

Ветка: feature/points-aac-eld
База: 9a57827 (план и спека закоммичены)
Базовая линия до работ: test_envelope 13/13, run_tests.py — все проходят.

Этот файл и отчёты рядом с ним добавлены в гит принудительно (каталог
`.superpowers/sdd/` сам себя игнорирует): работа продолжается на другой машине,
и журнал прогона должен переехать вместе с кодом.

## Состояние задач

| Задача | Статус | Коммит |
|---|---|---|
| 1. Сниффер вынесен в `src/sniff.h` | готова, ревью принято | `9722276` |
| 2. TS/ADTS/LATM не уходят в minimp3 | реализована, **ревью не закрыто** | `aac33a4` |
| 3. Целая сумма квадратов в `run()` | не начата | |
| 4. `runPoints` — ровно N точек | не начата | |
| 5. CLI `--points` | не начата | |
| 6. JNI и Kotlin `points` | не начата | |
| 7. `check_eld.py` и документация | не начата | |

## Хроника

Task 1: complete (commits bc53c4b..9722276, review clean)
Task 1: minor (deferred): неиспользуемый #include <cstring> в decoder_open.cpp:7
Task 1: minor (deferred): make unit не прогонялся — GNU make на хосте нет;
  прогнать на Linux/CI до мержа
Task 1: minor -> передан в задачу 2: предупреждения C4996 (fopen) под MSVC /W4
  нарушали глобальный констрейнт «ноль предупреждений». Вылечено в задаче 2
  через /D_CRT_SECURE_NO_WARNINGS в build_host.ps1 (переносимый fopen не трогали —
  предупреждение нестандартное, только MSVC).

Task 2: implemented, committed aac33a4 — РЕВЬЮ НЕ ПРОВЕДЕНО.
  Ревьюер был запущен, но упал с API-ошибкой в середине ответа. Вердикта нет.
  Отчёт исполнителя: task-2-report.md (Status: DONE, без замечаний;
  RED/GREEN зафиксированы, 19/19 test_sniff, 13/13 test_envelope,
  функциональные — все прошли, предупреждений C4996 больше нет).
  Проверено контроллером независимо на момент остановки: сборка и все тесты
  зелёные.

=== ОСТАНОВЛЕНО ЗДЕСЬ по просьбе пользователя, работа переезжает ===

## Как поднять окружение на новой машине

```sh
git clone https://github.com/pkachur/android-audio-amplitude.git
cd android-audio-amplitude
git checkout feature/points-aac-eld
```

Дальше нужно доложить три вещи, которых в репозитории нет намеренно.

**1. Заголовки minimp3** (`src/minimp3.h`, `src/minimp3_ex.h`) — сторонние,
public domain, не вендорятся:

```sh
make deps          # если есть GNU make
# либо вручную:
curl -fsSL -o src/minimp3.h    https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h
curl -fsSL -o src/minimp3_ex.h https://raw.githubusercontent.com/lieff/minimp3/master/minimp3_ex.h
```

**2. Тестовые фикстуры** (`tests/test.mp3`, `tests/ref_stereo.pcm`,
`tests/test_mono.mp3`, `tests/ref_mono.pcm`, `tests/test_loud.mp3`) — делаются
одной командой из **любого** аудиофайла, который понимает ffmpeg. Нужен ffmpeg
в PATH либо `pip install imageio-ffmpeg`:

```sh
python3 tests/make_fixtures.py любой_файл.m4a
```

Осмысленный тест получается на записи хотя бы в несколько секунд с меняющейся
громкостью. Конкретные значения фикстур не важны: `run_tests.py` независимо
пересчитывает ожидаемое на Python и сверяет с выходом программы.

**3. Записи для задачи 7** (`audio/*.m4a`) — личные голосовые записи, в
публичный репозиторий сознательно не кладутся. Перенести флешкой или облаком.
**Задачам 2-6 они не нужны вообще** — понадобятся только на задаче 7
(`tests/check_eld.py`).

### Сборка и тесты

На **Windows** (как было здесь): GNU make и clang отсутствуют, всё идёт через
MSVC 14.44 из Visual Studio 2022 Build Tools.

```powershell
cd <репозиторий>; .\build_host.ps1 -Tests   # только юнит-тесты
cd <репозиторий>; .\build_host.ps1 -Func    # + функциональные (нужен python3)
```

`build_host.ps1` сам находит `vcvars64.bat` через `vswhere` и подкладывает
каталог установщика в PATH — без этого `vcvars64.bat` не находит `vswhere`.

На **Linux, macOS или в Termux** работает штатный Makefile, и его наконец можно
прогнать по-настоящему (здесь это не удавалось — make не установлен):

```sh
make MEDIANDK=0        # десктопная сборка, только MP3
make unit              # оба набора юнит-тестов
make MEDIANDK=0 test   # юнит + функциональные
```

Прогон `make unit` на новой машине закрывает отложенный минор задачи 1.

Кросс-сборка под Android (нужна на задаче 6):

```powershell
.\build_android.ps1 -Ndk <путь к android-ndk>
```

## Точка возобновления

1. **Заново провести ревью задачи 2.** Метод — `superpowers:subagent-driven-development`,
   шаблон `task-reviewer-prompt.md`, модель sonnet. Входы:
   - бриф: `.superpowers/sdd/2026-08-03-points-and-aac-eld/task-2-brief.md`
   - отчёт: `.superpowers/sdd/2026-08-03-points-and-aac-eld/task-2-report.md`
   - дифф: сгенерировать заново, он в гит не кладётся —
     `scripts/review-package docs/superpowers/plans/2026-08-03-points-and-aac-eld.md 9722276 aac33a4`
   - BASE `9722276`, HEAD `aac33a4`

   Названные риски, которые ревьюер должен проверить:
   - выход за границу буфера: `validMpegFrameHeader` читает 4 байта, граница
     цикла сканирования обязана это гарантировать;
   - ложные отказы на настоящем MP3 из-за новой проверки индексов битрейта и
     частоты (в том числе «свободный» битрейт);
   - обе замены головы файла 16 -> 1024 байта сделаны (в `openFile` и `openFd`);
   - 11 исходных проверок в `tests/test_sniff.cpp` не ослаблены ради новых;
   - два новых функциональных теста убирают за собой временные файлы.

2. **Дальше по плану — задачи 3-7.** BASE каждой задачи = HEAD предыдущей.
   Брифы генерируются из плана:
   `scripts/task-brief docs/superpowers/plans/2026-08-03-points-and-aac-eld.md N`

3. После задачи 7 — финальное ревью всей ветки, затем
   `superpowers:finishing-a-development-branch`.

## Решения, принятые по ходу (не выводятся из кода)

- Предполётная сверка плана нашла два дословных повтора логики; вместо копий
  заведены `amp::mixFrame` в `envelope.h` и `writeHeaderPrefix` в
  `amplitude.cpp`. План поправлен коммитом `bc53c4b`.
- Настоящего AAC ELD среди записей нет: все шесть целых файлов — HE-AAC v1
  (`audioObjectType = 5`), а не ELD (`39`). Записано открытым пунктом в спеке.
  Седьмой файл (65536 байт) — оборванная запись без `moov`, годится как
  негативная фикстура.
- Аудио и фикстуры не кладутся в репозиторий: он публичный, а записи личные.
