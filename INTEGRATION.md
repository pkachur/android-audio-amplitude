# amplitude — интеграция в приложение

Библиотека/утилита: аудиофайл → значения амплитуды целыми числами.
Диапазон значений — PCM 16 бит, **−32768..32767** (после `--abs` и свёрток
`rms`/`avg`/`peak` — 0..32767).

| Формат | Чем декодируется | Где работает |
|---|---|---|
| MP3 | встроенный minimp3 | везде: Android, Linux, Windows, macOS |
| M4A / AAC (в т.ч. HE-AAC v1/v2) | системный MediaCodec (NDK Media API) | только Android 12+ |
| FLAC, OGG/Opus, WAV | системный MediaCodec | только Android 12+ |

Выбор бэкенда автоматический: по сигнатуре файла MP3 уходит в minimp3, всё
остальное — в MediaCodec. Принудительно — параметр `backend`.

## Состав

```
src/                       ядро, общее для CLI и приложения
  decoder.h                интерфейс декодера (открыть → читать PCM16)
  decoder_open.cpp         выбор бэкенда, определение формата, тексты ошибок
  decoder_minimp3.cpp      MP3
  decoder_mediandk.cpp     AAC/M4A и прочее через MediaCodec (вне Android — заглушки)
  envelope.h               свёртка PCM → значения амплитуды (общая логика)
  amplitude.cpp            CLI
  minimp3.h, minimp3_ex.h  сторонние заголовки (public domain), `make deps`
android/
  amplitude_jni.cpp        мост JNI
  CMakeLists.txt           сборка libamplitude.so
  Amplitude.kt             Kotlin-обёртка
tests/
  run_tests.py             функциональные тесты (см. «Проверка»)
  compare.py               сверка с эталонным PCM от ffmpeg
Makefile                   сборка CLI (на устройстве или кросс-компиляцией NDK)
```

## Формат данных

Без окна выдаётся **одно значение на отсчёт файла** — 44100 значений в секунду
при 44.1 кГц. Это редко нужно: для 4-минутного трека получится 10.6 млн чисел
(≈42 МБ в `IntArray`). Практически всегда задавайте окно:

* `intervalMs = 100` — окно 100 мс (10 точек в секунду);
* либо `block = N` — окно ровно в N отсчётов;
* либо `points = N` — ровно N значений на весь файл, ширина окна вычисляется сама.

`intervalMs` пересчитывается в отсчёты по реальной частоте файла:
`block = sampleRate * intervalMs / 1000`.

`points` — выдать ровно столько значений на весь файл; ширина окна вычисляется
сама. Задавать вместе с `intervalMs` или `block` нельзя — будет
`IllegalArgumentException`. Потолок — `Amplitude.MAX_POINTS` (16384).
При `points` поле `blockSamples` содержит среднюю ширину точки, поэтому
`pointDurationMs` продолжает работать.

Как сворачивается окно (`reduce`):

| Режим | Что даёт | Для чего |
|---|---|---|
| `REDUCE_PEAK` | максимум модуля в окне | форма волны, клиппинг |
| `REDUCE_RMS` | среднеквадратичное | воспринимаемая громкость, dBFS |
| `REDUCE_AVG` | среднее модуля | сглаженная огибающая |

Каналы (`channel`): `CH_MIX` — полусумма (по умолчанию), `CH_LEFT`, `CH_RIGHT`,
`CH_BOTH` — два значения на точку (L, R подряд в одном массиве).

---

## Способ A. Нативная библиотека в приложении (рекомендуемый)

### 1. Файлы

```
app/src/main/cpp/
    CMakeLists.txt          ← из android/
    amplitude_jni.cpp       ← из android/
    src/                    ← папка src/ целиком (включая minimp3*.h)
app/src/main/java/com/example/amplitude/Amplitude.kt   ← из android/
```

`CMakeLists.txt` сам найдёт ядро: рядом (`./src`), уровнем выше (`../src`) или
в той же папке, если всё свалено в одну.

Если пакет отличается от `com.example.amplitude` — поправьте `package` в
`Amplitude.kt` **и** имена функций в `amplitude_jni.cpp`
(`Java_com_example_amplitude_Amplitude_nativeDecodeFile` →
`Java_<пакет с _ вместо точек>_<класс>_nativeDecodeFile`; подчёркивание внутри
имени пакета экранируется как `_1`).

### 2. Gradle

```kotlin
// app/build.gradle.kts
android {
    namespace = "com.example.myapp"
    compileSdk = 35

    defaultConfig {
        minSdk = 31                          // Android 12+
        ndk { abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64") }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
}
```

Отдельных зависимостей не нужно: `libmediandk.so` — часть системы, CMake
линкует её через `target_link_libraries(amplitude PRIVATE mediandk)`.

### 3. Вызов

```kotlin
// огибающая по 100 мс, среднеквадратичная, моно-микс
val amp = withContext(Dispatchers.IO) {
    Amplitude.fromUri(context, uri, intervalMs = 100, reduce = Amplitude.REDUCE_RMS)
}

// ровно 100 точек на файл — удобно, когда волна рисуется в фиксированную ширину
val bars = withContext(Dispatchers.IO) {
    Amplitude.fromUri(context, uri, points = 100, reduce = Amplitude.REDUCE_RMS)
}

Log.d("amp", "${amp.sampleRate} Гц, ${amp.channels} кан., точек ${amp.points}, " +
             "по ${amp.pointDurationMs} мс")
amp.values.forEachIndexed { i, v -> /* v: Int в диапазоне -32768..32767 */ }
```

Три источника данных:

```kotlin
Amplitude.fromFile(File("/sdcard/Music/track.mp3"), intervalMs = 100)
Amplitude.fromUri(context, uri, intervalMs = 100)      // content:// из SAF/MediaStore
Amplitude.fromBytes(byteArray, intervalMs = 100)       // из сети, assets, кэша
```

`fromUri` открывает `ParcelFileDescriptor` и отдаёт **fd** в нативный код —
файл не копируется в память целиком. Для `content://` это основной путь.

При неудаче бросается `Amplitude.DecodeException` с текстом ошибки от
нативной части (какой бэкенд не смог и почему).

Важно: **обрыв декодирования посреди файла — тоже ошибка, а не короткий
результат**. Если кодек упал на повреждённом участке, вы получите исключение, а
не молча усечённую волновую форму с признаком успеха. То же в CLI: код возврата
станет ненулевым, а причина уйдёт в stderr.

### 4. Разрешения

* `content://` через SAF (`ACTION_OPEN_DOCUMENT`) или MediaStore-picker —
  **разрешения не нужны вообще**;
* прямой путь в общем хранилище: Android 13+ (API 33) — `READ_MEDIA_AUDIO`,
  Android 12 (API 31–32) — `READ_EXTERNAL_STORAGE`;
* файлы внутри `filesDir`/`cacheDir` приложения — без разрешений.

### 5. Потоки и память

* Декодирование блокирующее: `Dispatchers.IO`, `WorkManager` или свой поток.
  На главном потоке — гарантированный ANR на длинных файлах.
* Не запускайте десятки декодирований параллельно: число одновременных
  экземпляров MediaCodec на устройстве ограничено (обычно единицы–десятки).
  Семафор на 2–4 задачи.
* Память результата: `points * valuesPerPoint * 4` байта. При `intervalMs = 100`
  час звука — это 36 000 точек (144 КБ). Без окна тот же час — 570 МБ, чего
  делать нельзя.
* `limitPoints` жёстко ограничивает выдачу (полезно для превью).
* `points = N` ограничивает выдачу по построению: расход памяти известен заранее
  и не зависит от длины файла. Сам проход тоже ограничен — подокна сливаются
  попарно, а не копятся.

---

## Способ B. Готовый бинарник внутри APK

Когда нативная библиотека не нужна, а нужен процесс (например, разовый батч).

Android 10+ запрещает исполнять файлы из каталога данных приложения, поэтому
единственный рабочий приём — положить бинарник как «библиотеку»:

```
app/src/main/jniLibs/arm64-v8a/libamplitude.so     ← бинарник, собранный `make NDK=...`
app/src/main/jniLibs/armeabi-v7a/libamplitude.so
```

```kotlin
// app/build.gradle.kts — иначе .so не распакуется на диск и exec не выйдет
android {
    packaging { jniLibs { useLegacyPackaging = true } }
}
```
```xml
<!-- AndroidManifest.xml -->
<application android:extractNativeLibs="true" ... >
```

```kotlin
val exe = File(applicationInfo.nativeLibraryDir, "libamplitude.so")
val proc = ProcessBuilder(
    exe.absolutePath, inputPath, "--ms", "100", "--reduce", "rms", "-q"
).redirectErrorStream(false).start()

val values = proc.inputStream.bufferedReader().lineSequence()
    .map { it.trim().toInt() }.toList()
proc.waitFor()
```

Для больших объёмов быстрее `--raw` (int32 little-endian) вместо текста.

---

## Способ C. Линковка ядра в существующий нативный модуль

Если у вас уже есть свой `.so`, JNI-мост не нужен — подключите ядро напрямую:

```cpp
#include "decoder.h"
#include "envelope.h"

amp::Decoder *dec = amp::openFile(path);            // или openFd / openMemory
if (!dec) { LOGE("%s", amp::lastError()); return; }

amp::Params p;
p.intervalMs = 100;
p.reduce     = amp::RD_RMS;
p.chan       = amp::CH_MIX;

std::vector<int32_t> out;
struct Sink {
    std::vector<int32_t> &v;
    void operator()(const int32_t *w, int n) { v.insert(v.end(), w, w + n); }
} sink{out};

const long long points = amp::run(*dec, p, sink);
delete dec;
```

Собрать нужно четыре файла: `decoder_open.cpp`, `decoder_minimp3.cpp`,
`decoder_mediandk.cpp` (+ ваш код), слинковать с `-lmediandk`.

Потоковая обработка без накопления массива: `Sink` вызывается по мере
декодирования, так что можно писать сразу в файл/сокет/кольцевой буфер.

---

## Рецепты

**Ровно W точек на всю ширину виджета.** Точное число точек заранее неизвестно
(окно задаётся в мс). Надёжнее взять мелкое окно и досхлопнуть в Kotlin:

```kotlin
val fine = Amplitude.fromFile(file, intervalMs = 20, reduce = Amplitude.REDUCE_PEAK)
val w = 360
val step = maxOf(1, fine.points / w)
val bars = fine.values.asSequence().chunked(step) { it.max() }.take(w).toList()
```

**Громкость в dBFS:**

```kotlin
val db = amp.dbfs(i)                  // 20*log10(|v| / 32768), -inf при нуле
```

**Поиск тишины** (порог ≈ −50 dBFS при `REDUCE_RMS`):

```kotlin
val silent = amp.values.map { kotlin.math.abs(it) < 104 }   // 32768 * 10^(-50/20)
```

---

## Ограничения и подводные камни

* **Многоканальный звук (5.1 и т.п.) не поддерживается** — только моно и стерео;
  декодер вернёт ошибку с явным текстом.
* **Точность MP3.** minimp3 и ffmpeg расходятся не более чем на 1 LSB
  (проверено, SNR 71.5 дБ) — это нормальная разница округления для формата
  с плавающей точкой в стандарте, а не ошибка.
* **AAC и обрезка краёв.** MediaCodec не срезает служебную задержку кодера
  (encoder delay/padding), в отличие от gapless-обрезки в minimp3 для MP3.
  Первые ~2000 отсчётов AAC-файла могут быть нулями. Для огибающей это
  несущественно, для пофреймовой сверки с ffmpeg — учитывайте.
* **16 КБ страницы** (Android 15+): в сборку уже добавлен
  `-Wl,-z,max-page-size=16384`. Не убирайте.
* **`fromBytes` для AAC** использует `AMediaDataSource` (API 28+). Для MP3
  ограничения нет. Диапазон `offset`/`length` проверяется и в Kotlin
  (`IllegalArgumentException`), и в нативной части.
* **Размер результата ограничен `Int`**: если значений набирается больше 2³¹,
  бросается исключение вместо порчи массива. На практике это недостижимо без
  окна — задавайте `intervalMs`.
* **fd не закрывается нативным кодом** — владелец `ParcelFileDescriptor`
  остаётся на стороне Kotlin (в примере закрывается через `use`).
* Первый вызов MediaCodec стоит ~10–50 мс (поиск и запуск кодека). Для коротких
  MP3 путь minimp3 обычно быстрее — он выбирается автоматически.

---

## Проверка

Локально (на десктопе доступен только MP3-путь):

```
make unit                                      # 13 юнит-тестов свёртки, без зависимостей
make MEDIANDK=0
python3 tests/make_fixtures.py любой_файл.m4a   # разово: готовит фикстуры
python3 tests/run_tests.py build/native/amplitude   # 29 функциональных проверок
```

`tests/run_tests.py` независимо пересчитывает ожидаемые значения на Python из
эталонного PCM (декодирован ffmpeg) и сверяет с выходом программы: поотсчётные
значения, стерео и моно, режимы каналов, все свёртки, `--ms` против `--block`,
`--raw` против текста, stdin, `-o`, `--header`, `--limit`, выбор бэкенда,
границы диапазона и негативные случаи.

Сборка под Android:

```
make NDK=/путь/к/AndroidNDK ABI=arm64-v8a API=31
```

AAC-путь проверяется только на устройстве — на десктопе MediaCodec отсутствует.
