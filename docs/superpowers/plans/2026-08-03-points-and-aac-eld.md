# Ровно N точек на файл и гарантия AAC ELD — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Добавить опцию `--points N` (ровно N значений амплитуды на весь файл) и гарантировать AAC ELD во всех четырёх контейнерах из таблицы Android (`.3gp`, `.m4a`/`.mp4`, `.aac`, `.ts`).

**Architecture:** Определение формата выносится из `decoder_open.cpp` в заголовочный `src/sniff.h` и чинится: чужие контейнеры отбраковываются явно, заголовок кадра MPEG проверяется по всем полям. `--points` реализуется в `envelope.h` одним проходом с подокнами: поток копится в мелкие накопители `(peak, Σ|v|, Σv², count)`, по концу потока точная длина известна, и подокна сливаются в ровно N точек — слияние точное, потому что все три свёртки восстанавливаются из сумм.

**Tech Stack:** C++11 без зависимостей (кроме заголовков minimp3), Android NDK для целевой сборки, MSVC 14.44 для хостовой, Python 3 для функциональных тестов, PowerShell для сборки на Windows.

**Спека:** `docs/superpowers/specs/2026-08-03-points-and-aac-eld-design.md`

## Global Constraints

- **C++11**, без исключений и RTTI в целевой сборке (`-fno-exceptions -fno-rtti`): в `envelope.h` и `sniff.h` нельзя `throw`, `try`, `dynamic_cast`.
- **Ноль предупреждений** при `-Wall -Wextra` (clang/NDK) и `/W4` (MSVC). Предупреждения из `src/minimp3_*.h` — стороннний код, не считаются.
- **Никаких новых зависимостей.** Только стандартная библиотека и то, что уже есть.
- **Целевая платформа — Android 12+ (API 31+)**, ABI: `arm64-v8a`, `armeabi-v7a`, `x86_64`, `x86`.
- **Комментарии, сообщения об ошибках и документация — на русском**, как во всём проекте. Комментарий объясняет, *почему* так сделано, а не пересказывает код.
- **Диапазон значений PCM16: −32768..32767.** Любой новый путь обязан этот диапазон соблюдать (модуль −32768 равен 32768 и должен ограничиваться).
- **Ядро общее для CLI и JNI.** Логика живёт в `src/envelope.h` и `src/sniff.h`, а `amplitude.cpp` и `amplitude_jni.cpp` только вызывают её — второй реализации быть не должно.
- **Потолок `--points`: 16384** (константа `amp::kMaxPoints`).
- **Потолок буфера подокон: 32768 записей**, целевое число подокон на точку — 32.

## Тулчейн на этой машине

GNU make отсутствует. Хостовая сборка — MSVC 14.44 (Visual Studio 2022 Build Tools).
Задача 1 создаёт `build_host.ps1`, который прячет вызов `vcvars64.bat`; все
последующие задачи запускают тесты через него. `Makefile` остаётся для Termux и
Linux и обновляется в тех же задачах, но проверить его здесь нечем — это
нормально и ожидаемо.

Проверено до начала работ: текущие 13 юнит-тестов проходят, `amplitude.exe`
собирается.

---

### Task 1: Вынести определение формата в `src/sniff.h`

Чистое перемещение кода без изменения поведения. Нужно затем, что
`looksLikeMpegAudio` сейчас лежит в `decoder_open.cpp` вместе с фабриками
декодеров, и протестировать её можно только собрав оба бэкенда. После выноса в
заголовок она тестируется так же, как `envelope.h`, — одним файлом, без аудио.

**Files:**
- Create: `src/sniff.h`
- Create: `tests/check.h`
- Create: `tests/test_sniff.cpp`
- Create: `build_host.ps1`
- Modify: `src/decoder.h` (убрать объявление `looksLikeMpegAudio`, включить `sniff.h`)
- Modify: `src/decoder_open.cpp:33-54` (убрать определение `looksLikeMpegAudio`)
- Modify: `tests/test_envelope.cpp:19-29,187-189` (перейти на общий харнесс)
- Modify: `Makefile:126-129` (цель `unit` собирает два теста)

**Interfaces:**
- Consumes: ничего.
- Produces:
  - `amp::looksLikeMpegAudio(const uint8_t *d, size_t n) -> bool` в `src/sniff.h` (поведение то же, что сейчас).
  - `t::check(const char *name, bool ok, const char *detail = "")` и `t::report() -> int` в `tests/check.h`.
  - `build_host.ps1` — сборка и прогон на Windows-хосте.

- [ ] **Step 1: Создать общий харнесс тестов `tests/check.h`**

```cpp
// check.h — общий харнесс юнит-тестов: счётчик проверок и печать итога.
// Используется tests/test_envelope.cpp и tests/test_sniff.cpp.

#ifndef AMPLITUDE_TESTS_CHECK_H
#define AMPLITUDE_TESTS_CHECK_H

#include <cstdio>

namespace t {

// Счётчики в функциях, а не глобальные переменные: заголовок включается
// в разные единицы трансляции, и определение переменной дало бы дубль символа.
inline int &failures() { static int v = 0; return v; }
inline int &total()    { static int v = 0; return v; }

inline void check(const char *name, bool ok, const char *detail = "")
{
    ++total();
    if (!ok) ++failures();
    printf("[%s] %s%s%s\n", ok ? "OK " : "FAIL", name, *detail ? "  -- " : "", detail);
}

inline int report()
{
    printf("\n%s (%d из %d)\n", failures() ? "ЕСТЬ ПРОВАЛЫ" : "ВСЕ ЮНИТ-ТЕСТЫ ПРОШЛИ",
           total() - failures(), total());
    return failures() ? 1 : 0;
}

} // namespace t

#endif // AMPLITUDE_TESTS_CHECK_H
```

- [ ] **Step 2: Написать тесты сниффера `tests/test_sniff.cpp`**

Эти проверки описывают **текущее** поведение — после переноса они обязаны
проходить сразу. Случаи, которые сегодня работают неверно, добавляются в задаче 2.

```cpp
// Юнит-тесты определения формата (src/sniff.h) на синтетических заголовках.
// Ни аудиофайлов, ни python: все заголовки собираются прямо здесь.
//
// Сборка и запуск:  .\build_host.ps1 -Tests   либо   make unit

#include "sniff.h"
#include "check.h"

#include <vector>

namespace {

std::vector<uint8_t> zeros(size_t n) { return std::vector<uint8_t>(n, 0); }

/// Валидный заголовок кадра MPEG-1 Layer III, 128 кбит/с, 44100 Гц.
void putMp3Frame(std::vector<uint8_t> &b, size_t at)
{
    b[at + 0] = 0xFF;
    b[at + 1] = 0xFB;   // ver=11 (MPEG-1), layer=01 (III), protection=1
    b[at + 2] = 0x90;   // bitrate=1001 (128 кбит/с), freq=00 (44100 Гц)
    b[at + 3] = 0x00;
}

void putTag(std::vector<uint8_t> &b, size_t at, const char *tag)
{
    for (size_t i = 0; tag[i]; ++i) b[at + i] = (uint8_t)tag[i];
}

bool sniff(const std::vector<uint8_t> &b)
{
    return amp::looksLikeMpegAudio(b.data(), b.size());
}

} // namespace

int main()
{
    // 1. Кадр MPEG в начале — это MP3
    {
        std::vector<uint8_t> b = zeros(1024);
        putMp3Frame(b, 0);
        t::check("валидный кадр MPEG в начале -> MP3", sniff(b));
    }

    // 2. Кадр дальше начала, но в пределах окна сканирования
    {
        std::vector<uint8_t> b = zeros(1024);
        putMp3Frame(b, 600);
        t::check("кадр MPEG на смещении 600 -> MP3", sniff(b));
    }

    // 3. ID3 — тоже MP3
    {
        std::vector<uint8_t> b = zeros(1024);
        putTag(b, 0, "ID3");
        t::check("ID3 -> MP3", sniff(b));
    }

    // 4. Чужие контейнеры
    {
        std::vector<uint8_t> b = zeros(1024);
        putTag(b, 4, "ftyp");
        t::check("ftyp (MP4/M4A/3GP) -> не MP3", !sniff(b));
    }
    {
        std::vector<uint8_t> b = zeros(1024);
        putTag(b, 0, "RIFF");
        t::check("RIFF (WAV) -> не MP3", !sniff(b));
    }
    {
        std::vector<uint8_t> b = zeros(1024);
        putTag(b, 0, "OggS");
        t::check("OggS -> не MP3", !sniff(b));
    }
    {
        std::vector<uint8_t> b = zeros(1024);
        putTag(b, 0, "fLaC");
        t::check("fLaC -> не MP3", !sniff(b));
    }

    // 5. ADTS-AAC: биты слоя нулевые, это не MPEG Audio
    {
        std::vector<uint8_t> b = zeros(1024);
        b[0] = 0xFF; b[1] = 0xF1;              // sync 0xFFF, layer=00
        t::check("ADTS-AAC -> не MP3", !sniff(b));
    }

    // 6. Слишком короткий буфер
    {
        std::vector<uint8_t> b = zeros(3);
        b[0] = 0xFF; b[1] = 0xFB;
        t::check("буфер короче 4 байт -> не MP3", !sniff(b));
    }
    t::check("нулевой указатель -> не MP3", !amp::looksLikeMpegAudio(0, 0));

    // 7. Ничего похожего
    {
        std::vector<uint8_t> b = zeros(1024);
        for (size_t i = 0; i < b.size(); ++i) b[i] = (uint8_t)(i & 0x7F);
        t::check("мусор без синхросигнала -> не MP3", !sniff(b));
    }

    return t::report();
}
```

- [ ] **Step 3: Убедиться, что тесты не собираются**

```powershell
.\build_host.ps1 -Tests
```

Ожидается: ошибка — нет ни `build_host.ps1`, ни `src/sniff.h`.

- [ ] **Step 4: Создать `build_host.ps1`**

```powershell
# build_host.ps1 — сборка и прогон на Windows-хосте (MSVC), только MP3-путь.
# GNU make под Windows обычно нет, поэтому Makefile здесь не годится.
#
#   .\build_host.ps1            # собрать всё и прогнать юнит-тесты
#   .\build_host.ps1 -Tests     # только юнит-тесты
#   .\build_host.ps1 -Func      # юнит-тесты + функциональные (нужны фикстуры)

param([switch]$Tests, [switch]$Func)

$ErrorActionPreference = 'Stop'
$root  = $PSScriptRoot
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force $build | Out-Null

# vcvars64.bat зовёт vswhere без пути, поэтому каталог установщика нужен в PATH.
$installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
if (-not (Test-Path (Join-Path $installer 'vswhere.exe'))) {
    throw "не найден vswhere.exe в $installer — нужны Visual Studio Build Tools"
}
$env:PATH = "$installer;$env:PATH"
$vsPath = & (Join-Path $installer 'vswhere.exe') -latest -products * -property installationPath
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "не найден vcvars64.bat: $vcvars" }

function Invoke-Cl([string]$what, [string]$cmdline) {
    cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$root`" && $cmdline"
    if ($LASTEXITCODE -ne 0) { throw "не собралось: $what" }
}

function Invoke-Test([string]$exe) {
    & (Join-Path $build $exe)
    if ($LASTEXITCODE -ne 0) { throw "тесты провалены: $exe" }
}

Invoke-Cl 'test_envelope' 'cl /nologo /W4 /EHsc /I src /Fe:build\test_envelope.exe /Fo:build\ tests\test_envelope.cpp'
Invoke-Cl 'test_sniff'    'cl /nologo /W4 /EHsc /I src /Fe:build\test_sniff.exe    /Fo:build\ tests\test_sniff.cpp'
Invoke-Test 'test_envelope.exe'
Invoke-Test 'test_sniff.exe'

if ($Tests) { exit 0 }

Invoke-Cl 'amplitude.exe' ('cl /nologo /O2 /W4 /EHsc /I src /Fe:build\amplitude.exe /Fo:build\ ' +
                           'src\amplitude.cpp src\decoder_open.cpp src\decoder_minimp3.cpp src\decoder_mediandk.cpp')
Write-Host "OK: build\amplitude.exe"

if ($Func) {
    python3 tests\run_tests.py build\amplitude.exe
    if ($LASTEXITCODE -ne 0) { throw 'функциональные тесты провалены' }
}
```

- [ ] **Step 5: Создать `src/sniff.h` переносом кода**

Тело функции переносится из `decoder_open.cpp:33-54` **без изменений** — правки
логики идут отдельной задачей, чтобы падение теста было однозначно связано с ними.

```cpp
// sniff.h — определение формата по началу файла: что можно отдать minimp3,
// а что обязано идти в системный декодер.
//
// Заголовочный файл без .cpp, как и envelope.h: тестируется отдельно
// (tests/test_sniff.cpp), без сборки декодеров и без аудиофайлов.

#ifndef AMPLITUDE_SNIFF_H
#define AMPLITUDE_SNIFF_H

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace amp {

/// true, если начало буфера похоже на MPEG Audio (ID3 или sync-заголовок
/// слоя I/II/III). ADTS-AAC (0xFFF1/0xFFF9) сюда намеренно не попадает —
/// у него биты слоя нулевые.
inline bool looksLikeMpegAudio(const uint8_t *d, size_t n)
{
    if (!d || n < 4) return false;

    // Явно чужие контейнеры — чтобы случайный 0xFF в заголовке не сбил с толку.
    if (n >= 12 && !memcmp(d + 4, "ftyp", 4)) return false;   // MP4 / M4A
    if (!memcmp(d, "RIFF", 4)) return false;                  // WAV
    if (!memcmp(d, "OggS", 4)) return false;                  // Ogg / Opus
    if (!memcmp(d, "fLaC", 4)) return false;                  // FLAC
    if (!memcmp(d, "FORM", 4)) return false;                  // AIFF

    if (n >= 3 && !memcmp(d, "ID3", 3)) return true;

    const size_t lim = n < 1024 ? n : 1024;
    for (size_t i = 0; i + 1 < lim; ++i) {
        if (d[i] != 0xFF || (d[i + 1] & 0xE0) != 0xE0) continue;
        const int ver   = (d[i + 1] >> 3) & 0x03;   // 1 — зарезервировано
        const int layer = (d[i + 1] >> 1) & 0x03;   // 0 — зарезервировано (это ADTS-AAC)
        if (ver != 1 && layer != 0) return true;
    }
    return false;
}

} // namespace amp

#endif // AMPLITUDE_SNIFF_H
```

- [ ] **Step 6: Убрать старое определение и объявление**

В `src/decoder_open.cpp` удалить блок `// ---- определение формата` вместе с
функцией `looksLikeMpegAudio` (строки 31–54), оставив на месте `mediandkAvailable()`.

В `src/decoder.h` заменить объявление на включение заголовка. Было:

```cpp
/// true, если начало буфера похоже на MPEG Audio (ID3 или sync-заголовок слоя I/II/III).
/// ADTS-AAC (0xFFF1/0xFFF9) сюда намеренно не попадает — у него биты слоя нулевые.
bool looksLikeMpegAudio(const uint8_t *data, size_t size);
```

Стало — удалить эти три строки, а вверху файла, сразу после `#include <cstdint>`,
добавить:

```cpp
// Определение формата живёт в отдельном заголовке, чтобы тестироваться
// без сборки декодеров. Включается здесь, чтобы не менять публичный API.
#include "sniff.h"
```

- [ ] **Step 7: Перевести `tests/test_envelope.cpp` на общий харнесс**

Удалить локальные `failures`, `total` и `check` (строки 19–29 — только эти три
сущности, `FakeDecoder` и остальное остаётся), добавить `#include "check.h"`
после `#include "envelope.h"`, заменить все вызовы `check(` на `t::check(`, а
хвост `main` (строки 187–189) — на:

```cpp
    return t::report();
```

- [ ] **Step 8: Обновить цель `unit` в Makefile**

```make
# Юнит-тесты свёртки и определения формата: ни аудиофайлов, ни python не требуют.
# Только для хостовой сборки — при NDK=... бинарник не запустится на хосте.
unit: tests/test_envelope.cpp tests/test_sniff.cpp tests/check.h \
      $(SRCDIR)/envelope.h $(SRCDIR)/sniff.h $(SRCDIR)/decoder.h
	@mkdir -p build
	$(CXX) -O2 -std=c++11 -Wall -Wextra -I$(SRCDIR) -o build/test_envelope tests/test_envelope.cpp
	$(CXX) -O2 -std=c++11 -Wall -Wextra -I$(SRCDIR) -o build/test_sniff tests/test_sniff.cpp
	./build/test_envelope
	./build/test_sniff
```

- [ ] **Step 9: Прогнать тесты**

```powershell
.\build_host.ps1
```

Ожидается: 13 из 13 в `test_envelope`, 11 из 11 в `test_sniff`, затем
`OK: build\amplitude.exe`. Ни одного предупреждения от `/W4` в собственных файлах.

- [ ] **Step 10: Commit**

```bash
git add src/sniff.h src/decoder.h src/decoder_open.cpp tests/check.h tests/test_sniff.cpp tests/test_envelope.cpp Makefile build_host.ps1
git commit -m "Определение формата вынесено в sniff.h и покрыто тестами"
```

---

### Task 2: Починить определение формата под TS, ADTS и LATM

**Files:**
- Modify: `src/sniff.h` (новые проверки контейнеров и полей заголовка)
- Modify: `src/decoder_open.cpp:102` и `:162` (голова 16 -> 1024 байта)
- Modify: `tests/test_sniff.cpp` (новые проверки)
- Modify: `tests/run_tests.py` (два функциональных теста)

**Interfaces:**
- Consumes: `amp::looksLikeMpegAudio` из задачи 1, `t::check` / `t::report`.
- Produces:
  - `amp::looksLikeMpegTs(const uint8_t *d, size_t n) -> bool`
  - `amp::looksLikeAdts(const uint8_t *d, size_t n) -> bool`
  - `amp::looksLikeLoas(const uint8_t *d, size_t n) -> bool`
  - `amp::validMpegFrameHeader(const uint8_t *h) -> bool` (вызывающий обязан гарантировать 4 доступных байта)

- [ ] **Step 1: Добавить падающие тесты в `tests/test_sniff.cpp`**

Вставить перед `return t::report();`:

```cpp
    // 8. MPEG-TS: пакеты по 188 байт. Заполнение таблиц байтами 0xFF раньше
    //    выглядело как заголовок MPEG.
    {
        std::vector<uint8_t> b = zeros(4 + 3 * 192 + 8);
        for (int i = 0; i < 3; ++i) b[(size_t)i * 188] = 0x47;
        for (size_t i = 20; i < 180; ++i) b[i] = 0xFF;      // стаффинг PAT/PMT
        t::check("MPEG-TS (шаг 188) -> не MP3", !sniff(b));
    }

    // 9. Тот же TS с 4-байтовой меткой времени (M2TS): шаг 192
    {
        std::vector<uint8_t> b = zeros(4 + 3 * 192 + 8);
        for (int i = 0; i < 3; ++i) b[4 + (size_t)i * 192] = 0x47;
        for (size_t i = 24; i < 184; ++i) b[i] = 0xFF;
        t::check("MPEG-TS (шаг 192, M2TS) -> не MP3", !sniff(b));
    }

    // 10. LOAS/LATM: в этом виде AAC ELD лежит в .aac.
    //     Полезная нагрузка произвольная, в ней легко встречается 0xFF 0xFF.
    {
        std::vector<uint8_t> b = zeros(1024);
        b[0] = 0x56; b[1] = 0xE0; b[2] = 0x10;
        for (size_t i = 40; i < 80; ++i) b[i] = 0xFF;
        t::check("LOAS/LATM -> не MP3", !sniff(b));
    }

    // 11. Сплошной стаффинг 0xFF: индекс битрейта 1111 запрещён стандартом
    {
        std::vector<uint8_t> b(1024, 0xFF);
        t::check("сплошной 0xFF -> не MP3", !sniff(b));
    }

    // 12. Запрещённый индекс битрейта при валидных прочих полях
    {
        std::vector<uint8_t> b = zeros(1024);
        putMp3Frame(b, 100);
        b[102] = 0xF0;                                   // bitrate=1111
        t::check("индекс битрейта 1111 -> не MP3", !sniff(b));
    }

    // 13. «Свободный» битрейт 0000 minimp3 всё равно не разберёт
    {
        std::vector<uint8_t> b = zeros(1024);
        putMp3Frame(b, 100);
        b[102] = 0x00;                                   // bitrate=0000, freq=00
        t::check("индекс битрейта 0000 -> не MP3", !sniff(b));
    }

    // 14. Зарезервированный индекс частоты
    {
        std::vector<uint8_t> b = zeros(1024);
        putMp3Frame(b, 100);
        b[102] = 0x9C;                                   // bitrate=1001, freq=11
        t::check("индекс частоты 11 -> не MP3", !sniff(b));
    }

    // 15. Кадр в самом конце буфера не должен читаться за его границей
    {
        std::vector<uint8_t> b = zeros(6);
        b[3] = 0xFF; b[4] = 0xFB; b[5] = 0x90;           // четвёртого байта нет
        t::check("обрезанный кадр на границе буфера -> не MP3", !sniff(b));
    }
```

- [ ] **Step 2: Убедиться, что тесты падают**

```powershell
.\build_host.ps1 -Tests
```

Ожидается: FAIL на проверках 8, 9, 10, 11 и 15: стаффинг `0xFF 0xFF` проходит как
заголовок, а обрезанный кадр принимается по двум байтам, хотя третий с битрейтом
и частотой в буфер уже не помещается. Проверки 12–14 могут пройти случайно — это
нормально, они закрепляют новую валидацию.

- [ ] **Step 3: Реализовать проверки в `src/sniff.h`**

Заменить содержимое `namespace amp { ... }` на:

```cpp
/// Пакет MPEG-TS: 188 байт, либо 192 с 4-байтовой меткой времени (M2TS).
/// Три подряд идущих байта 0x47 на своих местах — надёжная примета.
inline bool looksLikeMpegTs(const uint8_t *d, size_t n)
{
    const size_t strides[2] = { 188, 192 };
    for (int s = 0; s < 2; ++s) {
        const size_t st  = strides[s];
        const size_t off = (st == 192) ? 4 : 0;      // у M2TS метка времени впереди
        if (n < off + 2 * st + 1) continue;
        if (d[off] == 0x47 && d[off + st] == 0x47 && d[off + 2 * st] == 0x47) return true;
    }
    return false;
}

/// ADTS-AAC: синхрослово 0xFFF, затем нулевые биты слоя.
inline bool looksLikeAdts(const uint8_t *d, size_t n)
{
    return n >= 2 && d[0] == 0xFF && (d[1] & 0xF6) == 0xF0;
}

/// LOAS/LATM: синхрослово 0x2B7 в 11 старших битах. AAC ELD в контейнере
/// .aac обычно приходит именно так, а не в ADTS.
inline bool looksLikeLoas(const uint8_t *d, size_t n)
{
    return n >= 2 && d[0] == 0x56 && (d[1] & 0xE0) == 0xE0;
}

/// Заголовок кадра MPEG Audio (слой I/II/III) со всеми полями.
/// Вызывающий обязан гарантировать 4 доступных байта.
///
/// Без проверки битрейта и частоты пара 0xFF 0xFF проходит как заголовок —
/// а это штатное заполнение таблиц в MPEG-TS и обычный байт нагрузки в AAC.
inline bool validMpegFrameHeader(const uint8_t *h)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
    const int ver     = (h[1] >> 3) & 0x03;   // 01 — зарезервировано
    const int layer   = (h[1] >> 1) & 0x03;   // 00 — зарезервировано (это ADTS-AAC)
    const int bitrate = (h[2] >> 4) & 0x0F;   // 0000 — «свободный», 1111 — запрещён
    const int freq    = (h[2] >> 2) & 0x03;   // 11 — зарезервировано
    return ver != 1 && layer != 0 && bitrate != 0 && bitrate != 15 && freq != 3;
}

/// true, если начало буфера похоже на MPEG Audio (ID3 или кадр слоя I/II/III).
/// Ошибка здесь стоит одной впустую потраченной попытки открытия, но на AAC ELD
/// в .ts и .aac она отправляла бы файл в minimp3 вместо системного декодера.
inline bool looksLikeMpegAudio(const uint8_t *d, size_t n)
{
    if (!d || n < 4) return false;

    // Явно чужие контейнеры — чтобы случайный 0xFF внутри них не сбил с толку.
    if (n >= 12 && !memcmp(d + 4, "ftyp", 4)) return false;   // MP4 / M4A / 3GP
    if (!memcmp(d, "RIFF", 4)) return false;                  // WAV
    if (!memcmp(d, "OggS", 4)) return false;                  // Ogg / Opus
    if (!memcmp(d, "fLaC", 4)) return false;                  // FLAC
    if (!memcmp(d, "FORM", 4)) return false;                  // AIFF
    if (looksLikeMpegTs(d, n)) return false;                  // MPEG-2 TS (.ts)
    if (looksLikeAdts(d, n))   return false;                  // сырой AAC (.aac)
    if (looksLikeLoas(d, n))   return false;                  // LATM/LOAS (.aac)

    if (n >= 3 && !memcmp(d, "ID3", 3)) return true;

    const size_t lim = n < 1024 ? n : 1024;
    for (size_t i = 0; i + 3 < lim; ++i)
        if (validMpegFrameHeader(d + i)) return true;
    return false;
}
```

- [ ] **Step 4: Убедиться, что юнит-тесты проходят**

```powershell
.\build_host.ps1 -Tests
```

Ожидается: 19 из 19 в `test_sniff`, 13 из 13 в `test_envelope`.

- [ ] **Step 5: Поднять чтение головы файла до 1024 байт**

В `src/decoder_open.cpp`, функция `openFile` — было `uint8_t head[16] = {0};`,
стало:

```cpp
    // 1024 байта — столько же, сколько сканирует looksLikeMpegAudio, и минимум
    // 377 байт нужно, чтобы разглядеть три пакета MPEG-TS. С прежними 16 байтами
    // файл и буфер нюхались по-разному.
    uint8_t head[1024] = {0};
```

В функции `openFd` — та же замена `uint8_t head[16] = {0};` на `uint8_t head[1024] = {0};`
(комментарий там не нужен, достаточно одного выше).

- [ ] **Step 6: Добавить функциональные тесты в `tests/run_tests.py`**

Вставить перед блоком `os.remove(garbage)`:

```python
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
```

- [ ] **Step 7: Прогнать функциональные тесты**

```powershell
.\build_host.ps1 -Func
```

Ожидается: `ВСЕ ТЕСТЫ ПРОШЛИ`, среди них две новые строки —
`MPEG-TS не уходит в minimp3` и `кадр MPEG на смещении 600 виден сниферу`.

Если фикстур нет, скрипт скажет, чем их сделать: `python3 tests/make_fixtures.py <аудиофайл>`.

- [ ] **Step 8: Commit**

```bash
git add src/sniff.h src/decoder_open.cpp tests/test_sniff.cpp tests/run_tests.py
git commit -m "Сниффер: TS, ADTS и LATM не уходят в minimp3, голова читается на 1024 байта"
```

---

### Task 3: Сумма квадратов в `run()` — целая, а не в плавающей точке

Отдельная маленькая задача перед `runPoints`, потому что два пути обязаны давать
одинаковые значения `rms`, а слияние подокон точно только на целых суммах.

**Files:**
- Modify: `src/envelope.h:70-141` (тип `accSq`)
- Modify: `tests/test_envelope.cpp` (проверка точного значения rms)

**Interfaces:**
- Consumes: `amp::run` из текущего кода.
- Produces: поведение `run()` не меняется; `accSq` становится `int64_t`.

- [ ] **Step 1: Написать тест на точное значение rms**

Вставить в `tests/test_envelope.cpp` перед `return t::report();`:

```cpp
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
```

- [ ] **Step 2: Прогнать — тест должен пройти уже сейчас**

```powershell
.\build_host.ps1 -Tests
```

Ожидается: PASS. Тест закрепляет текущее значение, чтобы смена типа его не сдвинула.
Это не TDD-«красный», а страховка перед изменением типа — прямой тест на
переполнение `double` потребовал бы окна в 190 секунд на полной шкале
(сумма превысила бы 2^53), и гонять его в юнит-тестах неразумно.

- [ ] **Step 3: Сменить тип накопителя**

В `src/envelope.h` заменить строку

```cpp
    double  accSq[2]   = {0.0, 0.0};
```

на

```cpp
    // Целая сумма: квадраты PCM16 не переполняют int64 на потоках до полусотни
    // часов, а double начал бы округлять с 2^53 и сделал бы результат зависимым
    // от порядка сложения — тогда runPoints со слиянием подокон разошёлся бы
    // с run() в последнем бите.
    int64_t accSq[2]   = {0, 0};
```

Заменить обе строки накопления (в теле цикла и в хвосте, всего три места
использования):

```cpp
                    accSq[c]  += (int64_t)v[c] * (int64_t)v[c];
```

```cpp
                    case RD_RMS: w[c] = clampAmp((int64_t)(sqrt((double)accSq[c] / inBlock) + 0.5)); break;
```

и сброс:

```cpp
                    accPeak[c] = 0; accAbs[c] = 0; accSq[c] = 0;
```

- [ ] **Step 4: Прогнать все тесты**

```powershell
.\build_host.ps1 -Func
```

Ожидается: 14 из 14 в `test_envelope`, 19 из 19 в `test_sniff`, функциональные —
`ВСЕ ТЕСТЫ ПРОШЛИ`, в том числе
`--ms 100 --reduce rms` и `моно: --ms 100 --reduce rms` с прежним `max расхождение`.
Если расхождение выросло — значит тип поменяли не везде.

- [ ] **Step 5: Commit**

```bash
git add src/envelope.h tests/test_envelope.cpp
git commit -m "Сумма квадратов в свёртке — целая: результат не зависит от порядка сложения"
```

---

### Task 4: `runPoints` — ровно N точек за один проход

**Files:**
- Modify: `src/envelope.h` (константа `kMaxPoints`, поле `Params::points`, `Sub`, `runPoints`)
- Modify: `tests/test_envelope.cpp` (тесты `runPoints`)

**Interfaces:**
- Consumes: `amp::Decoder`, `amp::Params`, `amp::clampAmp`, `amp::valuesPerPoint` из `envelope.h`.
- Produces:
  - `const long long amp::kMaxPoints = 16384;`
  - поле `long long amp::Params::points = 0;` (0 — не использовать)
  - `struct amp::Sub` с полями `peak[2]`, `absSum[2]`, `sqSum[2]`, `count`
  - `amp::subReset(Sub&)`, `amp::subAdd(Sub&, const int32_t*, int)`, `amp::subMerge(Sub&, const Sub&)`, `amp::subValue(const Sub&, int c, int reduce) -> int32_t`
  - `amp::runPoints(Decoder&, const Params&, std::vector<int32_t>&, long long *totalSamples) -> long long`

- [ ] **Step 1: Написать падающие тесты**

Добавить в `tests/test_envelope.cpp` рядом с `runOn` вспомогательную функцию:

```cpp
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
```

и сами тесты перед `return t::report();`:

```cpp
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
```

- [ ] **Step 2: Убедиться, что не собирается**

```powershell
.\build_host.ps1 -Tests
```

Ожидается: ошибка компиляции — нет `amp::runPoints`, `amp::kMaxPoints` и поля `Params::points`.

- [ ] **Step 3: Добавить поле и потолок в `src/envelope.h`**

В `struct Params` после `intervalMs`:

```cpp
    long long points     = 0;    ///< если > 0 — выдать ровно столько точек на весь поток
```

Рядом с объявлением `Params`, до него:

```cpp
/// Потолок --points. Продиктован буфером подокон: на точку берётся до 32
/// подокон, а весь буфер ограничен 32768 записями.
const long long kMaxPoints = 16384;
```

- [ ] **Step 4: Добавить подокно и операции над ним**

В `src/envelope.h` после `valuesPerPoint`:

```cpp
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
```

- [ ] **Step 5: Реализовать `runPoints`**

В `src/envelope.h` после `run()`:

```cpp
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
```

- [ ] **Step 6: Прогнать юнит-тесты**

```powershell
.\build_host.ps1 -Tests
```

Ожидается: 25 из 25 в `test_envelope` (13 старых + rms из задачи 3 + одиннадцать
новых: проверка 18 даёт три галочки, по одной на свёртку), 19 из 19 в
`test_sniff`, без предупреждений `/W4`.

- [ ] **Step 7: Commit**

```bash
git add src/envelope.h tests/test_envelope.cpp
git commit -m "runPoints: ровно N точек за один проход через подокна"
```

---

### Task 5: Опция `--points` в CLI

**Files:**
- Modify: `src/amplitude.cpp` (структура `Options`, `usage`, `parseArgs`, `main`)
- Modify: `tests/run_tests.py` (функциональные тесты `--points`)

**Interfaces:**
- Consumes: `amp::runPoints`, `amp::kMaxPoints`, `amp::Params::points` из задачи 4.
- Produces: поведение CLI, на которое опираются тесты и документация.

- [ ] **Step 1: Написать падающие функциональные тесты**

Добавить в `tests/run_tests.py` перед блоком `# 14. негативные случаи`:

```python
# 13b. --points ------------------------------------------------------------
mix = [trunc2(l + r) for l, r in frames]


def points_reference(vals, n, mode):
    """Независимая реплика алгоритма подокон из src/envelope.h."""
    cap = min(n * 32, 32768)
    if cap < n * 2:
        cap = n * 2
    buf, cur, sub = [], [0, 0, 0, 0], 1
    for v in vals:
        a = abs(v)
        if a > cur[0]:
            cur[0] = a
        cur[1] += a
        cur[2] += v * v
        cur[3] += 1
        if cur[3] < sub:
            continue
        buf.append(cur)
        cur = [0, 0, 0, 0]
        if len(buf) < cap:
            continue
        half = len(buf) // 2
        buf = [[max(buf[2 * k][0], buf[2 * k + 1][0]),
                buf[2 * k][1] + buf[2 * k + 1][1],
                buf[2 * k][2] + buf[2 * k + 1][2],
                buf[2 * k][3] + buf[2 * k + 1][3]] for k in range(half)]
        sub *= 2
    if cur[3] > 0:
        buf.append(cur)

    s = len(buf)
    pts = min(n, s)
    out = []
    for i in range(pts):
        acc = [0, 0, 0, 0]
        for k in range(i * s // pts, (i + 1) * s // pts):
            acc[0] = max(acc[0], buf[k][0])
            acc[1] += buf[k][1]
            acc[2] += buf[k][2]
            acc[3] += buf[k][3]
        if mode == 'peak':
            v = acc[0]
        elif mode == 'avg':
            v = acc[1] // acc[3]
        else:
            v = int(math.sqrt(acc[2] / acc[3]) + 0.5)
        out.append(min(v, 32767))
    return out


# --points 1 проверяется полностью независимо: одна точка на весь файл — это
# свёртка по всему потоку, границы подокон на неё не влияют.
for mode in ('peak', 'avg', 'rms'):
    got = run_text([MP3, '--points', '1', '-r', mode])
    exp = reduce_block(mix, mode)
    ok = len(got) == 1 and abs(got[0][0] - exp) <= TOL
    check('--points 1 --reduce %s == свёртка по всему файлу' % mode, ok,
          'получено %d, ожидалось %d' % (got[0][0] if got else -1, exp))

got = run_text([MP3, '--points', '100'])
check('--points 100 даёт ровно 100 строк', len(got) == 100, 'строк %d' % len(got))

for mode in ('peak', 'rms', 'avg'):
    got = run_text([MP3, '--points', '100', '-r', mode])
    exp = points_reference(mix, 100, mode)
    worst = max((abs(g[0] - e) for g, e in zip(got, exp)), default=0)
    check('--points 100 --reduce %s совпадает с пересчётом' % mode,
          len(got) == len(exp) and worst <= TOL,
          'точек %d (ожидалось %d), max расхождение %d' % (len(got), len(exp), worst))

got = run_text([MP3, '--points', '100', '-n', '50'])
check('--points 100 + --limit 50 даёт 50 строк', len(got) == 50, 'строк %d' % len(got))

got = run_text([MP3, '--points', '10', '-c', 'both'])
check('--points + -c both: два числа в строке',
      len(got) == 10 and all(len(g) == 2 for g in got))

first = run([MP3, '--points', '100', '--header']).decode('ascii').split('\n')[0]
check('--points: в заголовке points= и диапазон 0..32767',
      'points=100' in first and 'range=0..32767' in first, first)

for args, name in (
        (['--points', '100', '--ms', '100'], '--points вместе с --ms отвергается'),
        (['--points', '100', '--block', '512'], '--points вместе с --block отвергается'),
        (['--ms', '100', '--block', '512'], '--ms вместе с --block отвергается'),
        (['--points', '0'], '--points 0 отвергается'),
        (['--points', '20000'], '--points сверх потолка отвергается')):
    q = subprocess.run([EXE, MP3] + args + ['-q'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    msg = q.stderr.decode('utf-8', 'replace').strip()
    check(name, q.returncode == 2 and msg != '', 'код %d: %s' % (q.returncode, msg[:80]))
```

- [ ] **Step 2: Убедиться, что тесты падают**

```powershell
.\build_host.ps1 -Func
```

Ожидается: FAIL на всех новых проверках — `--points` пока неизвестная опция.

- [ ] **Step 3: Добавить опцию в `Options` и `usage`**

В `src/amplitude.cpp`, структура `Options` — добавить признаки того, что сетка
задана явно (иначе `--block 1` не отличить от значения по умолчанию):

```cpp
struct Options {
    const char  *path    = nullptr;   // "-" = stdin
    const char  *outPath = nullptr;   // nullptr = stdout
    amp::Params  p;
    amp::Backend backend  = amp::BACKEND_AUTO;
    bool         binary   = false;    // int32 little-endian вместо текста
    bool         header   = false;    // строка-комментарий с параметрами потока
    bool         quiet    = false;    // не печатать info в stderr
    bool         gotBlock = false;    // сетка задана явно: нужно для взаимоисключения
    bool         gotMs    = false;
    bool         gotPoints = false;
};
```

В `usage()` добавить строку после `-m, --ms`:

```cpp
        "  -p, --points N      выдать ровно N точек на весь файл (1..16384);\n"
        "                      окно вычисляется само, взаимоисключающе с --ms и --block\n"
```

и заменить хвост справки на:

```cpp
        "Текстовый вывод: одно значение в строке; для --channel both — два числа\n"
        "через пробел (левый правый). Диапазон: -32768..32767, а с --points,\n"
        "--abs и свёртками — 0..32767.\n"
        "\n"
        "Сетку задаёт что-то одно: --points, --ms или --block. Без них выдаётся\n"
        "одно значение на отсчёт файла (44100 значений в секунду для 44.1 кГц).\n"
        "Сто точек на весь файл:\n"
        "  %s track.m4a --points 100 --reduce rms\n",
```

- [ ] **Step 4: Разобрать опцию и проверить взаимоисключение**

В `parseArgs` в ветке `--block` и `--ms` выставить признаки:

```cpp
        } else if (!strcmp(a, "-b") || !strcmp(a, "--block")) {
            NEED_VAL();
            o.p.block = atoi(val);
            o.gotBlock = true;
            if (o.p.block < 1) { fprintf(stderr, "amplitude: --block должен быть >= 1\n"); return false; }
        } else if (!strcmp(a, "-m") || !strcmp(a, "--ms")) {
            NEED_VAL();
            o.p.intervalMs = atoi(val);
            o.gotMs = true;
            if (o.p.intervalMs < 1) { fprintf(stderr, "amplitude: --ms должен быть >= 1\n"); return false; }
        } else if (!strcmp(a, "-p") || !strcmp(a, "--points")) {
            NEED_VAL();
            o.p.points = strtoll(val, nullptr, 10);
            o.gotPoints = true;
            if (o.p.points < 1 || o.p.points > amp::kMaxPoints) {
                fprintf(stderr, "amplitude: --points должен быть от 1 до %lld\n",
                        (long long)amp::kMaxPoints);
                return false;
            }
```

Перед `if (!o.path)` в конце `parseArgs`:

```cpp
    // Раньше --ms молча вытеснял --block, и опечатка в вызове тихо давала не ту
    // сетку. Теперь любое сочетание — отказ.
    const int grid = (o.gotPoints ? 1 : 0) + (o.gotMs ? 1 : 0) + (o.gotBlock ? 1 : 0);
    if (grid > 1) {
        fprintf(stderr, "amplitude: сетку задаёт что-то одно: --points, --ms или --block\n");
        return false;
    }
```

- [ ] **Step 5: Развести два пути в `main`**

Заменить в `src/amplitude.cpp` часть от `const int block = amp::resolveBlock(...)`
до конца блока `{ Out out(fout); ... }` на:

```cpp
    const int outch = amp::valuesPerPoint(o.p, dec->channels());

    FILE *fout = stdout;
#ifdef _WIN32
    // Иначе Windows превратит каждый байт 0x0A в бинарном потоке в 0x0D 0x0A.
    if (o.binary && !o.outPath) _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (o.outPath) {
        // Открываем до декодирования: иначе на длинном файле об ошибке пути
        // узнаешь только через минуту работы.
        fout = fopen(o.outPath, o.binary ? "wb" : "w");
        if (!fout) {
            fprintf(stderr, "amplitude: не могу открыть на запись: %s\n", o.outPath);
            delete dec;
            return 1;
        }
    }

    long long points  = 0;
    int       block   = 0;
    bool      writeOk = true;

    if (o.p.points > 0) {
        std::vector<int32_t> values;
        long long totalSamples = 0;
        points = amp::runPoints(*dec, o.p, values, &totalSamples);
        block  = points > 0 ? (int)(totalSamples / points) : 0;

        // Об урезанной выдаче говорим и в тихом режиме: это меняет контракт вывода.
        if (points > 0 && points < o.p.points)
            fprintf(stderr, "amplitude: отсчётов (%lld) меньше запрошенных точек (%lld)"
                            " — выдано %lld\n", totalSamples, o.p.points, points);

        if (!o.quiet) {
            fprintf(stderr, "amplitude: %s, %d Гц, %d кан., отсчётов: %lld, %lld точек",
                    dec->backend(), dec->sampleRate(), dec->channels(),
                    dec->totalFrames(), points);
            if (block > 0)
                fprintf(stderr, " по ~%d отсчётов (~%.1f мс)",
                        block, dec->sampleRate() ? 1000.0 * block / dec->sampleRate() : 0.0);
            fprintf(stderr, "\n");
        }

        Out out(fout);
        if (o.header && !o.binary) {
            out.str("# backend="); out.str(dec->backend());
            out.str(" hz=");       out.num(dec->sampleRate());
            out.str(" channels="); out.num(dec->channels());
            out.str(" values_per_point="); out.num(outch);
            out.str(" points=");   out.num((int32_t)points);
            out.str(" block=");    out.num(block);
            out.str(" range=0..32767\n");
        }
        for (size_t i = 0; i < values.size(); ++i) {
            if (o.binary) {
                out.le32(values[i]);
            } else {
                if (i % (size_t)outch) out.ch(' ');
                out.num(values[i]);
                if ((i + 1) % (size_t)outch == 0) out.ch('\n');
            }
        }
        out.flush();
        writeOk = !out.error();
    } else {
        block = amp::resolveBlock(o.p, dec->sampleRate());

        if (!o.quiet) {
            fprintf(stderr, "amplitude: %s, %d Гц, %d кан., отсчётов: %lld",
                    dec->backend(), dec->sampleRate(), dec->channels(), dec->totalFrames());
            if (block > 1)
                fprintf(stderr, ", окно %d отсчётов (%.1f мс)",
                        block, dec->sampleRate() ? 1000.0 * block / dec->sampleRate() : 0.0);
            fprintf(stderr, "\n");
        }

        Out out(fout);
        if (o.header && !o.binary) {
            out.str("# backend="); out.str(dec->backend());
            out.str(" hz=");       out.num(dec->sampleRate());
            out.str(" channels="); out.num(dec->channels());
            out.str(" values_per_point="); out.num(outch);
            out.str(" block=");    out.num(block);
            out.str(" range=-32768..32767\n");
        }

        if (o.binary) { RawSink  sink{out}; points = amp::run(*dec, o.p, sink); }
        else          { TextSink sink{out}; points = amp::run(*dec, o.p, sink); }

        out.flush();
        writeOk = !out.error();
    }
```

Дальше по коду `if (!o.quiet) fprintf(stderr, "amplitude: выдано точек: %lld\n", points);`
и проверки `dec->failed()` / `writeOk` остаются как есть.

- [ ] **Step 6: Прогнать все тесты**

```powershell
.\build_host.ps1 -Func
```

Ожидается: юнит-тесты 25 из 25 и 19 из 19, функциональные — `ВСЕ ТЕСТЫ ПРОШЛИ`,
включая все новые проверки `--points`.

- [ ] **Step 7: Проверить руками**

```powershell
build\amplitude.exe tests\test.mp3 --points 100 --reduce rms --header | Select-Object -First 3
build\amplitude.exe tests\test.mp3 --points 100 --ms 50
```

Ожидается: первая команда печатает строку заголовка с `points=100 block=...
range=0..32767` и два значения; вторая — отказ «сетку задаёт что-то одно» и код 2.

- [ ] **Step 8: Commit**

```bash
git add src/amplitude.cpp tests/run_tests.py
git commit -m "CLI: опция --points и взаимоисключение сеток"
```

---

### Task 6: Параметр `points` в JNI и Kotlin

Автоматических тестов здесь нет: Gradle и устройства в этом окружении нет.
Проверка — компиляция JNI под все четыре ABI тем же скриптом, что и раньше,
плюс сверка сигнатур между Kotlin и C++ глазами.

**Files:**
- Modify: `android/amplitude_jni.cpp` (`toParams`, `finish`, три `native`-функции)
- Modify: `android/Amplitude.kt` (параметр `points`, проверки, три `external`-объявления)

**Interfaces:**
- Consumes: `amp::runPoints`, `amp::kMaxPoints`, `amp::Params::points` из задачи 4.
- Produces: сигнатуры JNI вида
  `nativeDecodeFile(path, channel, reduce, block, intervalMs, points, absolute, limitPoints, backend, info)`
  — параметр `points` идёт сразу после `intervalMs` во всех трёх функциях.

- [ ] **Step 1: Провести `points` через `toParams`**

В `android/amplitude_jni.cpp`:

```cpp
amp::Params toParams(jint channel, jint reduce, jint block, jint intervalMs,
                     jint points, jboolean absolute, jint limitPoints)
{
    amp::Params p;
    p.chan       = (int)channel;
    p.reduce     = (int)reduce;
    p.block      = block > 0 ? (int)block : 1;
    p.intervalMs = intervalMs > 0 ? (int)intervalMs : 0;
    p.points     = points > 0 ? (long long)points : 0;
    p.absolute   = (absolute == JNI_TRUE);
    p.limit      = limitPoints > 0 ? (long long)limitPoints : -1;
    return p;
}
```

- [ ] **Step 2: Развести пути в `finish`**

Заменить в `finish` блок от `const int block = amp::resolveBlock(p, hz);` до
`amp::run(*dec, p, sink);` на:

```cpp
    std::vector<jint> values;
    int block = 0;

    if (p.points > 0) {
        std::vector<int32_t> pts;
        long long totalSamples = 0;
        const long long n = amp::runPoints(*dec, p, pts, &totalSamples);
        block = n > 0 ? (int)(totalSamples / n) : 0;
        values.assign(pts.begin(), pts.end());
    } else {
        block = amp::resolveBlock(p, hz);
        if (dec->totalFrames() > 0) {             // разумный резерв
            long long points = (dec->totalFrames() + block - 1) / block;
            if (p.limit > 0 && points > p.limit) points = p.limit;
            if (points > 0) values.reserve((size_t)points * (size_t)outch);
        }
        VecSink sink{values};
        amp::run(*dec, p, sink);
    }
```

Остальное в `finish` (проверка `dec->failed()`, потолок `INT32_MAX`, заполнение
`info`, создание `jintArray`) не меняется — `block` там уже используется.

- [ ] **Step 3: Добавить параметр в три native-функции**

В каждой из `Java_com_example_amplitude_Amplitude_nativeDecodeFile`,
`...nativeDecodeFd` и `...nativeDecodeBytes` добавить `jint points` в список
параметров сразу после `jint intervalMs` и передать его в `toParams`. Например
для файла:

```cpp
JNIEXPORT jintArray JNICALL
Java_com_example_amplitude_Amplitude_nativeDecodeFile(
        JNIEnv *env, jclass, jstring jpath,
        jint channel, jint reduce, jint block, jint intervalMs, jint points,
        jboolean absolute, jint limitPoints, jint backend, jintArray info)
{
    StringUtfGuard path(env, jpath);
    if (!path.get()) return nullptr;

    try {
        amp::Decoder *dec = amp::openFile(path.get(), toBackend(backend));
        return finish(env, dec, toParams(channel, reduce, block, intervalMs, points,
                                         absolute, limitPoints), info);
    } catch (const std::bad_alloc &) {
        return throwOom(env);
    }
}
```

Те же две правки (список параметров и вызов `toParams`) — в `nativeDecodeFd` и
`nativeDecodeBytes`.

- [ ] **Step 4: Собрать JNI под все ABI**

```powershell
.\build_android.ps1 -Ndk F:\Projects\Toolchains\AndroidNDK
```

Ожидается: сборка бинарника и `libamplitude.so` для `arm64-v8a`, `armeabi-v7a`,
`x86_64`, `x86` без предупреждений в собственных файлах. Если в JNI разъехались
сигнатуры, компилятор скажет об этом здесь.

- [ ] **Step 5: Добавить `points` в Kotlin-обёртку**

В `android/Amplitude.kt` — новая проверка и параметр во всех трёх методах.
После `class DecodeException`:

```kotlin
    /** Потолок points — тот же, что в C++ (amp::kMaxPoints). */
    const val MAX_POINTS = 16384

    private fun checkGrid(intervalMs: Int, block: Int, points: Int) {
        val given = listOf(points > 0, intervalMs > 0, block > 1).count { it }
        require(given <= 1) {
            "сетку задаёт что-то одно: points, intervalMs или block"
        }
        require(points in 0..MAX_POINTS) {
            "points должен быть от 1 до $MAX_POINTS (0 = не использовать)"
        }
    }
```

В `fromFile`, `fromUri` и `fromBytes` добавить параметр `points: Int = 0` сразу
после `block: Int = 1`, первой строкой тела вызвать
`checkGrid(intervalMs, block, points)` и передать `points` в native-вызов сразу
после `intervalMs`. Для `fromFile`:

```kotlin
    @JvmOverloads
    fun fromFile(
        file: File,
        intervalMs: Int = 0,
        block: Int = 1,
        points: Int = 0,
        channel: Int = CH_MIX,
        reduce: Int = REDUCE_PEAK,
        absolute: Boolean = false,
        limitPoints: Int = 0,
        backend: Int = BACKEND_AUTO,
    ): Amplitudes {
        checkGrid(intervalMs, block, points)
        val info = IntArray(5)
        val values = nativeDecodeFile(
            file.absolutePath, channel, reduce, block, intervalMs, points,
            absolute, limitPoints, backend, info
        ) ?: fail(file.name)
        return build(info, values)
    }
```

Так же в `fromUri` (перед `checkGrid` ничего не делать, вызвать его первой
строкой) и в `fromBytes` (после существующего `require` на диапазон массива).

Обновить `external`-объявления — во всех трёх добавить `points: Int` после
`intervalMs: Int`:

```kotlin
    @JvmStatic
    private external fun nativeDecodeFile(
        path: String, channel: Int, reduce: Int, block: Int, intervalMs: Int, points: Int,
        absolute: Boolean, limitPoints: Int, backend: Int, info: IntArray,
    ): IntArray?
```

Обновить KDoc параметров:

```kotlin
    /**
     * @param intervalMs  окно в миллисекундах (0 = не использовать)
     * @param block       окно в отсчётах
     * @param points      выдать ровно столько точек на весь файл (0 = не использовать);
     *                    ширина окна вычисляется сама
     * @param limitPoints не больше стольких точек (0 = без ограничения)
     *
     * Сетку задаёт что-то одно: points, intervalMs или block.
     */
```

и комментарий к `blockSamples` в классе `Amplitudes`:

```kotlin
     * @param blockSamples   фактический размер окна в отсчётах; при points —
     *                       средняя ширина точки
```

- [ ] **Step 6: Сверить сигнатуры**

Пройти глазами по трём парам «Kotlin `external` — C++ `JNIEXPORT`» и убедиться,
что порядок и число параметров совпадают. Рассогласование здесь не ловится
компилятором и проявляется только на устройстве как падение или мусор в
значениях.

- [ ] **Step 7: Пересобрать и прогнать хостовые тесты**

```powershell
.\build_host.ps1 -Func
```

Ожидается: всё зелёное — правки JNI не должны задеть общее ядро.

- [ ] **Step 8: Commit**

```bash
git add android/amplitude_jni.cpp android/Amplitude.kt
git commit -m "JNI и Kotlin: параметр points"
```

---

### Task 7: Проверка на реальных записях и документация

**Files:**
- Create: `tests/check_eld.py`
- Modify: `README.md`
- Modify: `INTEGRATION.md`

**Interfaces:**
- Consumes: готовый CLI с `--points`, исправленный сниффер.
- Produces: `python3 tests/check_eld.py [--dir audio] [--exe build/amplitude.exe] [--device] [--points N]`.

- [ ] **Step 1: Написать `tests/check_eld.py`**

```python
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
```

- [ ] **Step 2: Прогнать на реальных записях**

```powershell
python3 tests\check_eld.py
```

Ожидается: семь строк `OK` — каждый файл из `audio/` уходит в системный декодер,
а не в minimp3.

- [ ] **Step 3: Обновить README.md**

Внести четыре правки:

1. В таблицу «Что умеет» — строку про AAC ELD и контейнеры:

```markdown
| M4A / AAC: LC, HE-AAC v1/v2, **ELD** | системный MediaCodec (NDK Media API) | Android |
```

и абзац под таблицей:

```markdown
AAC ELD берётся системным декодером (`c2.android.aac.encoder` / `.decoder`) во
всех четырёх контейнерах, которые перечисляет таблица форматов Android: `.3gp`,
`.m4a`/`.mp4`, `.aac` (ADTS и LATM/LOAS) и `.ts` (MPEG-2 TS). Отдельного кода
под ELD нет — профиль для `AMediaExtractor` и `AMediaCodec` прозрачен; что
потребовалось, так это не отдавать эти контейнеры в minimp3 по ошибке.
```

2. В раздел «Выдавать можно» — третий способ:

```markdown
* **заданным числом точек** — `--points 100` даёт ровно 100 значений на весь
  файл, какой бы длины он ни был; ширина окна вычисляется сама.

Сетку задаёт что-то одно: `--points`, `--ms` или `--block`.
```

3. В блок опций добавить строку и поправить соседние:

```
  -b, --block N       свернуть N отсчётов в одно значение
  -m, --ms N          то же, но окном N мс
  -p, --points N      выдать ровно N точек на весь файл (1..16384)
```

   и сразу под блоком:

```markdown
Сетку задаёт что-то одно: `--points`, `--ms` или `--block`.
```

4. В раздел «Что проверено на текущей версии» добавить:

```markdown
* `--points` выдаёт ровно запрошенное число точек, а значения совпадают с
  независимым пересчётом; слияние подокон проверено отдельно — при разном числе
  слияний значения одинаковы;
* сниффер не отдаёт в minimp3 ни MPEG-TS (в том числе с меткой времени), ни
  ADTS, ни LATM/LOAS — проверено на синтетических заголовках и на настоящих
  записях приложения;
* AAC ELD подтверждён только по коду и по маршрутизации контейнеров: записи в
  этом профиле пока нет, доступные файлы — HE-AAC v1.
```

- [ ] **Step 4: Обновить INTEGRATION.md**

Найти пример вызова `Amplitude.fromUri` и добавить рядом вариант с `points`,
плюс строку в описание параметров:

```kotlin
// ровно 100 точек на файл — удобно, когда волна рисуется в фиксированную ширину
val amp = withContext(Dispatchers.IO) {
    Amplitude.fromUri(context, uri, points = 100, reduce = Amplitude.REDUCE_RMS)
}
```

```markdown
`points` — выдать ровно столько значений на весь файл; ширина окна вычисляется
сама. Задавать вместе с `intervalMs` или `block` нельзя — будет
`IllegalArgumentException`. Потолок — `Amplitude.MAX_POINTS` (16384).
При `points` поле `blockSamples` содержит среднюю ширину точки, поэтому
`pointDurationMs` продолжает работать.
```

- [ ] **Step 5: Прогнать всё целиком**

```powershell
.\build_host.ps1 -Func
python3 tests\check_eld.py
```

Ожидается: `ВСЕ ЮНИТ-ТЕСТЫ ПРОШЛИ` дважды, `ВСЕ ТЕСТЫ ПРОШЛИ`, `ВСЕ ПРОВЕРКИ ПРОШЛИ`.

- [ ] **Step 6: Commit**

```bash
git add tests/check_eld.py README.md INTEGRATION.md
git commit -m "Проверка на реальных записях и документация по --points и AAC ELD"
```

---

## Что остаётся непроверенным

Записать в отчёт по окончании работ, не замалчивая:

1. **AAC ELD на настоящем файле.** Все шесть целых записей в `audio/` — HE-AAC v1
   (`audioObjectType = 5`), а не ELD (`39`). Путь декодирования у них общий, и
   маршрутизация контейнеров проверена, но сам профиль ELD подтверждается только
   кодом. Нужна запись, сделанная `c2.android.aac.encoder` в профиле ELD.
2. **Контейнеры `.3gp`, `.aac`, `.ts` на настоящих файлах.** Сниффер на них
   проверен синтетическими заголовками; сквозного декодирования не было, потому
   что таких файлов нет.
3. **Работа на устройстве.** `tests/check_eld.py --device` написан, но здесь
   запустить его нечем — нужен подключённый по adb телефон.
4. **Kotlin-обёртка** компилируется только в проекте приложения: Gradle в этом
   окружении нет, сверка сигнатур сделана глазами.
