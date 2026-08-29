/* gui/gfxfb.c — реализация двойной буферизации и вывода кадра. */
#include <stddef.h>

#include "gfxfb.h"
#include "palette.h"

/* Обслуживание кэша нужно только на ARM64 (там дисплей читает наш буфер
 * из ОЗУ мимо кэша процессора). На x86 видеопамять отображена как
 * uncacheable самим чипсетом, и делать ничего не надо — поэтому здесь
 * слабые (weak) заглушки, которые ARM64-сборка перекрывает настоящими
 * реализациями из arch/arm64/mmu.c. Это единственный способ оставить
 * этот файл общим для обеих архитектур без #ifdef внутри него. */
__attribute__((weak)) void arch_dcache_clean(const void *addr, size_t len) {
    (void)addr; (void)len;
}

static volatile uint8_t *g_fb = 0;
static int g_width  = 0;
static int g_height = 0;
static int g_pitch  = 0;
static int g_format = GFXFB_FMT_PAL8;

static uint8_t g_backbuffer[GFXFB_MAX_PIXELS];

void gfxfb_bind(volatile void *fb, int width, int height, int pitch_bytes, int fmt) {
    g_fb     = (volatile uint8_t *)fb;
    g_width  = width;
    g_height = height;
    g_pitch  = pitch_bytes;
    g_format = fmt;
}

int gfxfb_width(void)  { return g_width; }
int gfxfb_height(void) { return g_height; }

uint8_t *gfxfb_backbuffer(void) { return g_backbuffer; }

void gfxfb_put_pixel(int x, int y, uint8_t color) {
    if (x < 0 || y < 0 || x >= g_width || y >= g_height) return;
    g_backbuffer[y * g_width + x] = color;
}

void gfxfb_clear(uint8_t color) {
    long n = (long)g_width * (long)g_height;
    for (long i = 0; i < n; i++) g_backbuffer[i] = color;
}

void gfxfb_present(void) {
    if (!g_fb) return;

    if (g_format == GFXFB_FMT_PAL8) {
        /* Видеокарта сама раскрасит индексы через DAC — копируем как есть. */
        for (int y = 0; y < g_height; y++) {
            volatile uint8_t *dst = g_fb + (long)y * g_pitch;
            const uint8_t *src = &g_backbuffer[(long)y * g_width];
            for (int x = 0; x < g_width; x++) dst[x] = src[x];
        }
    } else {
        /* Аппаратной палитры нет — раскрашиваем сами по таблице. */
        for (int y = 0; y < g_height; y++) {
            volatile uint32_t *dst = (volatile uint32_t *)(g_fb + (long)y * g_pitch);
            const uint8_t *src = &g_backbuffer[(long)y * g_width];
            for (int x = 0; x < g_width; x++) dst[x] = g_palette_xrgb[src[x]];
        }
    }

    /* Вытолкнуть кадр из кэша процессора в ОЗУ: контроллер дисплея
       (или QEMU в роли такового) читает буфер напрямую и про наш кэш
       ничего не знает. Без этого на реальном железе экран показывал бы
       обрывки предыдущего кадра. */
    arch_dcache_clean((const void *)g_fb, (size_t)g_pitch * (size_t)g_height);
}
