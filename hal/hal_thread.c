/* hal/hal_thread.c — этап 1: только сигнатура API.
 * Реальный планировщик (кооперативный или приоритетный, со switch_context
 * в ASM для x86 и ARM64 по отдельности) — задача второго этапа. Пока
 * hal_thread_create просто "регистрирует" функцию и вызывает её синхронно,
 * чтобы прикладной код можно было уже сегодня писать в терминах API потоков,
 * не переписывая его потом.
 */
#include "hal.h"

struct hal_thread {
    void (*entry)(void *arg);
    void *arg;
};

static struct hal_thread g_threads[8];
static size_t g_thread_count = 0;

hal_thread_t *hal_thread_create(void (*entry)(void *arg), void *arg, size_t stack_size) {
    (void)stack_size; /* пока не используется — нет отдельных стеков */

    if (g_thread_count >= 8)
        hal_panic("hal_thread_create: thread table full");

    struct hal_thread *t = &g_threads[g_thread_count++];
    t->entry = entry;
    t->arg = arg;

    /* ЭТАП 1: выполняем немедленно и синхронно (нет вытеснения). */
    t->entry(t->arg);

    return t;
}

void hal_thread_yield(void) {
    /* no-op на этапе 1: планировщика ещё нет */
}
