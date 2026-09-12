/* hal/hal_console_arm64.c — реализация hal_console_* для СБОРКИ ПОД ARM64.
 *
 * Ровно тот же "мост", что и hal/hal_console.c для x86, только дёргает
 * uart_* вместо vga_* и keyboard_*. В образ попадает ровно один из двух
 * файлов — какой именно, решает переменная ARCH в Makefile. Ни kernel.c,
 * ни shell.c про эту развилку не знают (см. docs/ARM64_PLAN.md п.3).
 *
 * Бонусом, которого нет на x86: цвет. Терминал на том конце
 * последовательного порта (screen/minicom/qemu -nographic) понимает
 * ANSI-эскейпы, поэтому hal_console_set_color() здесь не заглушка, а
 * настоящая смена цвета — и shell.c получается цветным на обеих
 * архитектурах, без единого #ifdef внутри самого shell.c.
 */
#include "hal.h"

void uart_init(void);
void uart_putc(char c);
void uart_write(const char *s);
int  uart_getc(void);

/* VGA-палитра (порядок из hal.h) -> коды ANSI SGR. Первые восемь цветов
 * обычные (30-37), вторые восемь — яркие (90-97). Порядок цветов у VGA и
 * у ANSI разный (у VGA синий второй, у ANSI — красный), поэтому таблица,
 * а не арифметика. */
static const uint8_t vga_to_ansi_fg[16] = {
    30, 34, 32, 36, 31, 35, 33, 37,
    90, 94, 92, 96, 91, 95, 93, 97
};

static void write_decimal(uint8_t v) {
    char buf[4];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v > 0) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) uart_putc(buf[--n]);
}

void hal_console_init(void) {
    uart_init();
    uart_write("\033[0m\033[2J\033[H");   /* сброс атрибутов, очистка, курсор в угол */
}

void hal_console_putc(char c) {
    uart_putc(c);
}

/* ОЧИСТКА ЭКРАНА И ПОЗИЦИОНИРОВАНИЕ КУРСОРА.
 *
 * Понадобились полноэкранному редактору (apps/editor.c). На x86 за ними
 * стоит настоящий текстовый режим VGA с аппаратным курсором; здесь
 * консоль — это последовательный порт, и никакого курсора у неё нет.
 *
 * Зато есть общепринятый язык управляющих последовательностей VT100,
 * который понимает любой терминал на другом конце провода. Так это и
 * сделано: "очистить экран" и "курсор в строку и столбец". Нумерация у
 * VT100 с единицы, у нас с нуля — поэтому прибавляем.
 *
 * Автор редактора оставил здесь заметку, что этих двух функций не
 * хватает и без них arm64 не слинкуется. Так и было: четыре цели сборки
 * из шести падали. */
static void vt_number(int n) {
    char buf[8];
    int i = 0;
    if (n <= 0) { hal_console_putc('1'); return; }
    while (n > 0 && i < (int)sizeof(buf)) { buf[i++] = (char)('0' + n % 10); n /= 10; }
    while (i > 0) hal_console_putc(buf[--i]);
}

void hal_console_clear(void) {
    hal_console_putc('\x1b'); hal_console_putc('[');
    hal_console_putc('2');    hal_console_putc('J');
    hal_console_putc('\x1b'); hal_console_putc('[');
    hal_console_putc('H');
}

void hal_console_goto(int row, int col) {
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    hal_console_putc('\x1b'); hal_console_putc('[');
    vt_number(row + 1);
    hal_console_putc(';');
    vt_number(col + 1);
    hal_console_putc('H');
}

void hal_console_write(const char *s) {
    uart_write(s);
}

int hal_console_getc(void) {
    int c = uart_getc();
    if (c < 0) return -1;

    /* Терминалы шлют на Backspace код DEL (0x7F), а на Enter — CR (0x0D).
       Ядро и shell ожидают '\b' и '\n' — приводим здесь, чтобы код выше
       HAL был одинаковым для PS/2-клавиатуры и для последовательного порта. */
    if (c == 0x7F) return '\b';
    if (c == '\r') return '\n';
    return c;
}

char hal_console_getc_blocking(void) {
    int c;
    do {
        c = hal_console_getc();
    } while (c == -1);
    return (char)c;
}

void hal_console_set_color(uint8_t fg, uint8_t bg) {
    uart_write("\033[");
    write_decimal(vga_to_ansi_fg[fg & 0x0F]);
    uart_putc(';');
    /* фон = код текста + 10 (для ярких это 100-107) */
    write_decimal((uint8_t)(vga_to_ansi_fg[bg & 0x0F] + 10));
    uart_putc('m');
}

void hal_console_reset_color(void) {
    uart_write("\033[0m");
}
