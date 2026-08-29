/* arch/x86/vga.c — вывод текста в буфер 0xB8000 (текстовый режим VGA 80x25) */
#include <stdint.h>
#include <stddef.h>

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_MEMORY  ((volatile uint16_t *)0xB8000)
#define VGA_DEFAULT_COLOR   0x0F   /* белый на чёрном */

static size_t vga_row = 0;
static size_t vga_col = 0;
static uint8_t vga_current_color = VGA_DEFAULT_COLOR;

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
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
}

void vga_write(const char *s) {
    while (*s)
        vga_putc(*s++);
}
