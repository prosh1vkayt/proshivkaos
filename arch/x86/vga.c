/* arch/x86/vga.c — вывод текста в буфер 0xB8000 (текстовый режим VGA 80x25) */
#include <stdint.h>
#include <stddef.h>
#include "ports.h"

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_MEMORY  ((volatile uint16_t *)0xB8000)
#define VGA_DEFAULT_COLOR   0x0F   /* белый на чёрном */

/* Порты CRT-контроллера — сдвигают аппаратный текстовый курсор (мигающий
 * подчёркивание/блок), а не просто меняют то, что мы сами рисуем в буфер.
 * Нужно полноэкранным приложениям (apps/editor.c), где курсор — это
 * реальная позиция редактирования, а не просто "куда пишет putc дальше". */
#define VGA_CRTC_INDEX_PORT 0x3D4
#define VGA_CRTC_DATA_PORT  0x3D5
#define VGA_CRTC_CURSOR_HIGH 0x0E
#define VGA_CRTC_CURSOR_LOW  0x0F

static size_t vga_row = 0;
static size_t vga_col = 0;
static uint8_t vga_current_color = VGA_DEFAULT_COLOR;

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

static void vga_update_hw_cursor(void) {
    uint16_t pos = (uint16_t)(vga_row * VGA_WIDTH + vga_col);
    outb(VGA_CRTC_INDEX_PORT, VGA_CRTC_CURSOR_LOW);
    outb(VGA_CRTC_DATA_PORT, (uint8_t)(pos & 0xFF));
    outb(VGA_CRTC_INDEX_PORT, VGA_CRTC_CURSOR_HIGH);
    outb(VGA_CRTC_DATA_PORT, (uint8_t)((pos >> 8) & 0xFF));
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    vga_current_color = (uint8_t)((bg << 4) | (fg & 0x0F));
}

void vga_reset_color(void) {
    vga_current_color = VGA_DEFAULT_COLOR;
}

static void vga_scroll(void) {
    for (size_t r = 1; r < VGA_HEIGHT; r++)
        for (size_t c = 0; c < VGA_WIDTH; c++)
            VGA_MEMORY[(r - 1) * VGA_WIDTH + c] = VGA_MEMORY[r * VGA_WIDTH + c];

    for (size_t c = 0; c < VGA_WIDTH; c++)
        VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + c] = vga_entry(' ', vga_current_color);

    vga_row = VGA_HEIGHT - 1;
}

void vga_init(void) {
    vga_row = 0;
    vga_col = 0;
    vga_current_color = VGA_DEFAULT_COLOR;
    for (size_t r = 0; r < VGA_HEIGHT; r++)
        for (size_t c = 0; c < VGA_WIDTH; c++)
            VGA_MEMORY[r * VGA_WIDTH + c] = vga_entry(' ', vga_current_color);
    vga_update_hw_cursor();
}

/* Полная очистка экрана + курсор в левый верхний угол. В отличие от
 * vga_init() ничего не трогает в состоянии цвета — только содержимое
 * экрана и позицию, как и положено "clear", а не "reinit". */
void vga_clear(void) {
    for (size_t r = 0; r < VGA_HEIGHT; r++)
        for (size_t c = 0; c < VGA_WIDTH; c++)
            VGA_MEMORY[r * VGA_WIDTH + c] = vga_entry(' ', vga_current_color);
    vga_row = 0;
    vga_col = 0;
    vga_update_hw_cursor();
}

/* Произвольное позиционирование курсора — то, чего обычной последовательной
 * печати (vga_putc/vga_write) не хватает полноэкранным приложениям.
 * Значения вне экрана обрезаются, а не игнорируются: вызывающему (editor.c)
 * так проще — не нужно самому клампить каждый раз перед вызовом. */
void vga_set_cursor(int row, int col) {
    if (row < 0) row = 0;
    if (row >= VGA_HEIGHT) row = VGA_HEIGHT - 1;
    if (col < 0) col = 0;
    if (col >= VGA_WIDTH) col = VGA_WIDTH - 1;

    vga_row = (size_t)row;
    vga_col = (size_t)col;
    vga_update_hw_cursor();
}

void vga_putc(char c) {
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\b') {
        if (vga_col > 0) {
            vga_col--;
            VGA_MEMORY[vga_row * VGA_WIDTH + vga_col] = vga_entry(' ', vga_current_color);
        }
    } else {
        VGA_MEMORY[vga_row * VGA_WIDTH + vga_col] = vga_entry(c, vga_current_color);
        vga_col++;
        if (vga_col >= VGA_WIDTH) {
            vga_col = 0;
            vga_row++;
        }
    }

    if (vga_row >= VGA_HEIGHT)
        vga_scroll();

    vga_update_hw_cursor();
}

void vga_write(const char *s) {
    while (*s)
        vga_putc(*s++);
}
