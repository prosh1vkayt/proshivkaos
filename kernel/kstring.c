/* kernel/kstring.c — memset/memcpy/memmove/memcmp.
 *
 * Зачем это нужно: даже с -ffreestanding -nostdlib компилятор имеет полное
 * право САМ подставить вызов memset/memcpy там, где в исходнике их нет —
 * например, при обнулении большой структуры или копировании массива.
 * На 32-битном x86 с -O2 это почти не всплывало, на AArch64 всплывает
 * сразу же, и линковка падает с "undefined reference to memset".
 * Файл архитектурно-нейтральный, собирается в оба образа.
 */
#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int value, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)value;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dst;

    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}
