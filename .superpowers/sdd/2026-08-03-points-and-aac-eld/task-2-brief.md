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

