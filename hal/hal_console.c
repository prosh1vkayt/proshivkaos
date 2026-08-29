/* hal/hal_console.c — реализация hal_console_* для СБОРКИ ПОД x86.
 *
 * Это тот самый "мост": единственный файл, которому позволено знать, что
 * сейчас собирается x86-бэкенд (vga.c/keyboard.c). Для ARM64-сборки в
 * Makefile этот .c просто не компилируется — вместо него используется
 * hal/hal_console_arm64.c (см. docs/ARM64_PLAN.md), который дергает
 * arch/arm64/uart.c. Прикладной код (kernel.c, shell.c) видит только hal.h
 * и никогда не импортирует ни vga.c, ни uart.c напрямую.
 */
#include "hal.h"

/* объявлены в arch/x86/vga.c и arch/x86/keyboard.c */
void vga_init(void);
void vga_putc(char c);
void vga_write(const char *s);
void vga_set_color(uint8_t fg, uint8_t bg);
void vga_reset_color(void);
int  keyboard_poll(void);
char keyboard_getc_blocking(void);

void hal_console_init(void) {
    vga_init();
}

void hal_console_putc(char c) {
    vga_putc(c);
}

void hal_console_write(const char *s) {
    vga_write(s);
}

int hal_console_getc(void) {
    return keyboard_poll();
}

char hal_console_getc_blocking(void) {
    return keyboard_getc_blocking();
}

void hal_console_set_color(uint8_t fg, uint8_t bg) {
    vga_set_color(fg, bg);
}

void hal_console_reset_color(void) {
    vga_reset_color();
}
