/* arch/arm64/uart.c — драйвер PL011 UART, прямой аналог arch/x86/vga.c.
 *
 * На x86 "консоль" — это запись символов в видеопамять 0xB8000. На ARM64
 * никакой текстовой видеопамяти не существует в природе: единственный
 * способ что-то напечатать до того, как заработает графика, — послать
 * байт в последовательный порт. У QEMU virt (и у подавляющего большинства
 * ARM-плат) это контроллер ARM PL011, у Qualcomm-телефонов — свой GENI/
 * MSM UART, но интерфейс наружу (uart_putc/uart_getc) от этого не меняется.
 *
 * Регистры опрашиваются, прерывания не используются — ровно как в
 * arch/x86/keyboard.c на первом этапе.
 */
#include "arm64.h"

/* Смещения регистров PL011 (ARM DDI 0183) */
#define UART_DR     0x00    /* данные (чтение = приём, запись = передача) */
#define UART_FR     0x18    /* флаги                                       */
#define UART_IBRD   0x24    /* делитель скорости, целая часть              */
#define UART_FBRD   0x28    /* делитель скорости, дробная часть            */
#define UART_LCR_H  0x2C    /* формат кадра                                */
#define UART_CR     0x30    /* управление                                  */
#define UART_IMSC   0x38    /* маска прерываний                            */
#define UART_ICR    0x44    /* сброс прерываний                            */

#define FR_RXFE     (1 << 4)   /* приёмный FIFO пуст     */
#define FR_TXFF     (1 << 5)   /* передающий FIFO полон  */

/* Базовый адрес вынесен в переменную, а не в #define: на реальном
 * устройстве он берётся из device tree, и тогда достаточно будет вызвать
 * uart_set_base() до uart_init(), не пересобирая всё остальное. */
static uint64_t g_uart_base = VIRT_UART0_BASE;

void uart_set_base(uint64_t base) {
    g_uart_base = base;
}

void uart_init(void) {
    mmio_write32(g_uart_base + UART_CR, 0);          /* выключить на время настройки */

    /* 115200 бод при UARTCLK 24 МГц: делитель = 24e6 / (16 * 115200) =
       13.02 -> IBRD = 13, FBRD = round(0.02 * 64) = 1. QEMU скорость не
       эмулирует и эти регистры игнорирует, но на реальном железе без них
       из порта посыплется мусор. */
    mmio_write32(g_uart_base + UART_IBRD, 13);
    mmio_write32(g_uart_base + UART_FBRD, 1);

    /* 8 бит данных, без чётности, 1 стоп-бит, FIFO включены */
    mmio_write32(g_uart_base + UART_LCR_H, (3 << 5) | (1 << 4));

    mmio_write32(g_uart_base + UART_IMSC, 0);        /* все прерывания замаскированы */
    mmio_write32(g_uart_base + UART_ICR, 0x7FF);     /* сбросить висящие флаги       */

    /* UARTEN | TXE | RXE */
    mmio_write32(g_uart_base + UART_CR, (1 << 0) | (1 << 8) | (1 << 9));
}

void uart_putc(char c) {
    /* Терминалы ждут CR+LF, ядро печатает только LF — дописываем сами,
       иначе вывод "лесенкой" уезжает вправо. */
    if (c == '\n') uart_putc('\r');

    while (mmio_read32(g_uart_base + UART_FR) & FR_TXFF) { }
    mmio_write32(g_uart_base + UART_DR, (uint32_t)(unsigned char)c);
}

void uart_write(const char *s) {
    while (*s) uart_putc(*s++);
}

int uart_getc(void) {
    if (mmio_read32(g_uart_base + UART_FR) & FR_RXFE)
        return -1;                                   /* приёмник пуст */
    return (int)(mmio_read32(g_uart_base + UART_DR) & 0xFF);
}

void uart_write_hex(uint64_t v) {
    static const char digits[] = "0123456789ABCDEF";
    uart_write("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        uart_putc(digits[(v >> shift) & 0xF]);
}
