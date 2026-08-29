/* arch/x86/vga13h.c — переключение VGA в классический "mode 13h"
 * (320x200, 256 цветов) БЕЗ вызова BIOS int 0x10 (мы уже в защищённом
 * режиме, реального режима нет). Пишем напрямую в регистры VGA: Misc,
 * Sequencer, CRTC, Graphics Controller, Attribute Controller. Таблица
 * значений регистров — стандартная, много раз опубликованная в OS-dev
 * туториалах (не зависит от BIOS, а именно "железная" конфигурация
 * видеокарты для mode 13h).
 *
 * Framebuffer после переключения — линейный, по адресу 0xA0000,
 * 320*200 = 64000 байт, один байт на пиксель (индекс в палитре).
 * Собственно put_pixel/clear теперь в gfxfb.c — общие для этого режима
 * и для режимов через VBE (см. vbe.c), т.к. по сути это один и тот же
 * линейный 8bpp-буфер, разница только в адресе/размере.
 */
#include <stdint.h>
#include "ports.h"
#include "palette.h"

#define VGA_WIDTH  320
#define VGA_HEIGHT 200

static const uint8_t g_mode13h_regs[] = {
    /* MISC */
    0x63,
    /* SEQ (5 регистров) */
    0x03, 0x01, 0x0F, 0x00, 0x0E,
    /* CRTC (25 регистров) */
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF,
    /* Graphics Controller (9 регистров) */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F,
    0xFF,
    /* Attribute Controller (21 регистр) */
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00
};

static void write_regs(const uint8_t *regs) {
    /* MISC */
    outb(0x3C2, *regs++);

    /* SEQ: индекс в 0x3C4, данные в 0x3C5 */
    for (uint8_t i = 0; i < 5; i++) {
        outb(0x3C4, i);
        outb(0x3C5, *regs++);
    }

    /* CRTC: сначала снять защиту регистров 0-7 (бит 7 индекса 0x03) */
    outb(0x3D4, 0x03);
    outb(0x3D5, inb(0x3D5) | 0x80);
    outb(0x3D4, 0x11);
    outb(0x3D5, inb(0x3D5) & ~0x80);

    /* защита уже снята (значение из таблицы регистра 0x11 её опять включит,
       поэтому пишем регистры сначала как есть — таблица 0xA3 для индекса 0x11
       уже без бита защиты) */
    for (uint8_t i = 0; i < 25; i++) {
        outb(0x3D4, i);
        outb(0x3D5, *regs++);
    }

    /* Graphics Controller: индекс 0x3CE, данные 0x3CF */
    for (uint8_t i = 0; i < 9; i++) {
        outb(0x3CE, i);
        outb(0x3CF, *regs++);
    }

    /* Attribute Controller: индекс/данные оба через 0x3C0 (флип-флоп),
       сброс флип-флопа чтением 0x3DA */
    (void)inb(0x3DA);
    for (uint8_t i = 0; i < 21; i++) {
        outb(0x3C0, i);
        outb(0x3C0, *regs++);
    }

    /* включить видеовывод (бит 5 индекса 0x20) */
    outb(0x3C0, 0x20);
}

void vga13h_set_mode(void) {
    write_regs(g_mode13h_regs);
}

/* DAC-палитра: масштабируем общую таблицу цветов (gui/palette.c) в
 * 6-битный формат VGA DAC. Раньше эта таблица лежала в arch/x86/palette.c
 * и была "цветами видеокарты"; теперь она общая для всех платформ —
 * на ARM64 те же самые значения используются как обычный RGB, потому что
 * никакого DAC там нет (см. gui/gfxfb.c, ветка GFXFB_FMT_XRGB32).
 *
 * Грузим все 256 индексов, а не первые 32, как раньше: палитра картинки
 * обоев теперь тоже живёт в g_palette, и отдельной функции для неё не
 * нужно — достаточно обновить таблицу и перезалить DAC целиком. */
void vga13h_load_dac(void) {
    for (int i = 0; i < PALETTE_SIZE; i++) {
        outb(0x3C8, (uint8_t)i);
        outb(0x3C9, (uint8_t)(g_palette[i].r * 63 / 255));
        outb(0x3C9, (uint8_t)(g_palette[i].g * 63 / 255));
        outb(0x3C9, (uint8_t)(g_palette[i].b * 63 / 255));
    }
}

int vga13h_width(void)  { return VGA_WIDTH; }
int vga13h_height(void) { return VGA_HEIGHT; }
