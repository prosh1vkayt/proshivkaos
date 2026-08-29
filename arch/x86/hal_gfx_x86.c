/* arch/x86/hal_gfx_x86.c — арх-зависимая часть графического HAL для x86.
 *
 * Раньше этот код жил прямо в gui/hal_gfx.c и был единственной причиной,
 * по которой весь графический слой был намертво привязан к x86. Теперь
 * примитивы рисования лежат в gui/hal_gfx.c (общие для всех платформ), а
 * здесь остались только две по-настоящему платформенные вещи: как
 * переключить видеокарту в графический режим и куда после этого писать.
 *
 * Два бэкенда:
 *   - classic mode13h (320x200) — работает всегда, даже без PCI;
 *   - VBE dispi (640x480/800x600/...) — только под QEMU/Bochs/VirtualBox.
 */
#include "hal_gfx.h"
#include "gfxfb.h"
#include "palette.h"

void vga13h_set_mode(void);
void vga13h_load_dac(void);   /* заливает g_palette целиком в аппаратный DAC */

int  vbe_set_mode(uint16_t width, uint16_t height, uint8_t bpp, volatile uint8_t **out_lfb);
void vbe_disable(void);

static int g_using_vbe = 0;

void hal_gfx_init(void) {
    palette_init();
    hal_gfx_set_resolution(320, 200);
}

int hal_gfx_set_resolution(int width, int height) {
    if (width == 320 && height == 200) {
        /* Если до этого был активен VBE-режим, его нужно явно выключить —
           иначе видеокарта продолжает считать источником кадра VBE LFB, и
           обычные регистры mode13h конфликтуют с этим состоянием (отсюда
           были "повторы картинки в плохом качестве" при возврате на
           низкое разрешение). */
        if (g_using_vbe)
            vbe_disable();

        vga13h_set_mode();
        gfxfb_bind((volatile uint8_t *)0xA0000, 320, 200, 320, GFXFB_FMT_PAL8);
        g_using_vbe = 0;
    } else {
        volatile uint8_t *lfb = (volatile uint8_t *)0;
        if (!vbe_set_mode((uint16_t)width, (uint16_t)height, 8, &lfb)) {
            /* нет VBE — остаёмся на том, что было, ничего не ломаем */
            return 0;
        }
        gfxfb_bind(lfb, width, height, width, GFXFB_FMT_PAL8);
        g_using_vbe = 1;
    }

    /* DAC-палитра (порты 0x3C8/0x3C9) — общий регистр для VGA и для VBE
       в 8bpp, переприменяем при каждом переключении режима. */
    vga13h_load_dac();
    return 1;
}

int hal_gfx_is_vbe(void) { return g_using_vbe; }

void hal_gfx_set_image_palette(const uint8_t *rgb_255, int count, int start_index) {
    palette_set_range(rgb_255, count, start_index);
    vga13h_load_dac();
}
