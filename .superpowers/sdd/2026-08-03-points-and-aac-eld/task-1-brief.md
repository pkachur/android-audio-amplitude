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

