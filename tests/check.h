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
