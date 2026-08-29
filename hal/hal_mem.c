/* hal/hal_mem.c — этап 1: простой bump-allocator поверх статического региона.
 * Он архитектурно-нейтрален (работает и на x86, и на ARM64 одинаково),
 * поэтому в отличие от hal_console.c этот файл НЕ дублируется по платформам —
 * единственное, что отличается по архитектуре, это откуда взялся регион
 * (на x86 — статический массив в BSS ниже; на ARM64 — регион из LK/DT,
 * см. docs/ARM64_PLAN.md).
 */
#include "hal.h"

#define HEAP_SIZE (4 * 1024 * 1024)   /* 4 MiB — достаточно для RamFS+shell на старте */

static uint8_t heap[HEAP_SIZE];
static size_t heap_offset = 0;

void hal_mem_init(void) {
    heap_offset = 0;
}

void *hal_mem_alloc(size_t size) {
    /* выравнивание по 16 байт */
    size = (size + 15) & ~((size_t)15);

    if (heap_offset + size > HEAP_SIZE)
        hal_panic("hal_mem_alloc: out of memory");

    void *ptr = &heap[heap_offset];
    heap_offset += size;
    return ptr;
}

size_t hal_mem_used(void)  { return heap_offset; }
size_t hal_mem_total(void) { return HEAP_SIZE; }

void hal_mem_free(void *ptr) {
    (void)ptr; /* этап 1: освобождение не реализовано (задел под free-list) */
}
