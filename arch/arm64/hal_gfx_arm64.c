/* arch/arm64/hal_gfx_arm64.c — арх-зависимая часть графического HAL для ARM64.
 * Парный файл к arch/x86/hal_gfx_x86.c: там VGA/VBE, здесь ramfb.
 */
#include "hal_gfx.h"
#include "gfxfb.h"
#include "palette.h"
#include "arm64.h"
#include "boards/board.h"
#include "platform.h"

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

/* Фреймбуфер под QEMU/ramfb — обычный массив в .bss нашего же образа,
 * адрес которого мы сами сообщаем гипервизору.
 *
 * Размер считается по запрошенному разрешению, а не по GFXFB_MAX_PIXELS:
 * предел поднят до 1080x1920 ради экрана реального телефона, но там
 * фреймбуфер СВОЙ, от загрузчика, и этот массив не используется вовсе.
 * Раздувать его до восьми мегабайт в сборке под эмулятор было бы обидно. */
static uint32_t g_framebuffer[PROSHIVKA_SCREEN_W * PROSHIVKA_SCREEN_H]
    __attribute__((aligned(4096)));

static int g_width  = 0;
static int g_height = 0;

void hal_gfx_init(void) {
    early_fb_band(9, 255, 128, 192);   /* розовая: дошли до графики */
    palette_init();

    const platform_info_t *pi = platform();

    /* У реального телефона нет ни fw_cfg, ни ramfb — оба существуют только
       внутри QEMU. Зато загрузчик уже настроил контроллер дисплея, чтобы
       показать свой логотип, и оставил после себя рабочий фреймбуфер.
       Его адрес и формат приходят либо из device tree (узел
       simple-framebuffer), либо из констант платы — разбирается с этим
       arch/arm64/platform.c, здесь остаётся только воспользоваться.

       Разрешение при этом НЕ выбираем: оно то, что уже настроил загрузчик,
       менять его нечем и незачем. */
    if (pi->fb_valid) {
        int w = pi->fb_width, h = pi->fb_height;

        /* Экран крупнее бэкбуфера рисовать некуда. Лучше честно остаться
           без графики (система продолжит работать через последовательный
           порт), чем писать за пределы массива. */
        if ((long)w * (long)h > GFXFB_MAX_PIXELS) {
            uart_write("hal_gfx: ekran ");
            uart_write_hex((uint64_t)w); uart_write("x"); uart_write_hex((uint64_t)h);
            uart_write(" bolshe bekbufera, grafika otklyuchena\n");
            return;
        }

        g_width  = w;
        g_height = h;
        gfxfb_bind((volatile void *)(uintptr_t)pi->fb_addr, w, h,
                   pi->fb_stride, pi->fb_format);
        early_fb_band(10, 160, 160, 160);  /* серая: экран подключён */
        return;
    }

    /* fw_cfg — канал, через который мы вообще узнаём про ramfb.
       Без него hal_gfx_set_resolution() ничего не найдёт. */
    fwcfg_init();

    hal_gfx_set_resolution(PROSHIVKA_SCREEN_W, PROSHIVKA_SCREEN_H);
}

int hal_gfx_set_resolution(int width, int height) {
    /* На плате с готовым фреймбуфером менять разрешение некому: это не
       видеокарта со своими регистрами, а память, которую один раз
       разметил загрузчик. Экран настроек по-прежнему может ЗАПРОСИТЬ
       смену — здесь она просто честно отклоняется, ровно как отклонял бы
       её vbe_set_mode() без VBE на x86. */
    if (platform()->fb_valid) {
        (void)width; (void)height;
        return 0;
    }

    if (width <= 0 || height <= 0) return 0;
    /* Через ramfb рисуем только в свой массив — он размером ровно под
       разрешение, заданное при сборке. */
    if ((long)width * (long)height >
        (long)PROSHIVKA_SCREEN_W * (long)PROSHIVKA_SCREEN_H) return 0;

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
