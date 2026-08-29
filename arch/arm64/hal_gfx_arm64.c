/* arch/arm64/hal_gfx_arm64.c — арх-зависимая часть графического HAL для ARM64.
 * Парный файл к arch/x86/hal_gfx_x86.c: там VGA/VBE, здесь ramfb.
 */
#include "hal_gfx.h"
#include "gfxfb.h"
#include "palette.h"
#include "arm64.h"

/* Разрешение по умолчанию — портретное 480x960 (соотношение 2:1, как у
 * современных телефонов). Переопределяется на этапе сборки:
 *   make ARCH=arm64 touch SCREEN_W=720 SCREEN_H=1440
 * Верхняя граница — GFXFB_MAX_PIXELS (720x1440), она же размер буферов ниже. */
#ifndef PROSHIVKA_SCREEN_W
#define PROSHIVKA_SCREEN_W 480
#endif
#ifndef PROSHIVKA_SCREEN_H
#define PROSHIVKA_SCREEN_H 960
#endif

/* Сам фреймбуфер — обычный массив в .bss нашего же образа. Его физический
 * адрес мы отдаём QEMU (или контроллеру дисплея телефона), и тот читает
 * оттуда картинку. Выравнивание на страницу: контроллеры дисплея этого
 * обычно требуют, а ramfb на выровненном буфере работает заведомо. */
static uint32_t g_framebuffer[GFXFB_MAX_PIXELS] __attribute__((aligned(4096)));

static int g_width  = 0;
static int g_height = 0;

void hal_gfx_init(void) {
    palette_init();

    /* fw_cfg — канал, через который мы вообще узнаём про ramfb.
       Без него hal_gfx_set_resolution() ничего не найдёт. */
    fwcfg_init();

    hal_gfx_set_resolution(PROSHIVKA_SCREEN_W, PROSHIVKA_SCREEN_H);
}

int hal_gfx_set_resolution(int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    if ((long)width * (long)height > GFXFB_MAX_PIXELS) return 0;

    if (!ramfb_setup(g_framebuffer, width, height)) {
        /* Дисплея нет (запущено без -device ramfb, либо мы вообще не под
           QEMU). Ведём себя как vbe_set_mode() в такой ситуации: честно
           отвечаем "не смог". Сообщение в UART обязательно — иначе на
           реальном устройстве получаем чёрный экран без единой подсказки,
           что именно не завелось. */
        uart_write("hal_gfx: ramfb ne nayden (nuzhen -device ramfb)\n");
        return 0;
    }

    g_width  = width;
    g_height = height;
    gfxfb_bind(g_framebuffer, width, height, width * 4, GFXFB_FMT_XRGB32);
    return 1;
}

/* На ARM64 аппаратной палитры не существует: фреймбуфер честный 32-битный.
 * Поэтому "загрузить палитру в DAC" здесь сводится к обновлению
 * программной таблицы — её читает gfxfb_present() при преобразовании
 * индексов в RGB. */
void hal_gfx_set_image_palette(const uint8_t *rgb_255, int count, int start_index) {
    palette_set_range(rgb_255, count, start_index);
}

int hal_gfx_is_vbe(void) { return 0; }   /* понятия из мира x86 тут нет */
