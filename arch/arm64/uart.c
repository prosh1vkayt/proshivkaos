/* arch/arm64/uart.c — диспетчер последовательного порта.
 *
 * Наружу (в hal_console_arm64.c, hal_input_arm64.c и отладочный вывод
 * драйверов) торчат только функции uart_*. Какой именно контроллер за ними
 * стоит, решается ВО ВРЕМЯ ВЫПОЛНЕНИЯ по device tree: PL011 у QEMU и
 * большинства плат, UARTDM у телефонов на Snapdragon.
 *
 * Почему во время выполнения, а не через #ifdef при сборке. Так один и тот
 * же образ грузится и в эмуляторе, и на телефоне — а значит, отлаживать
 * логику можно там, где есть отладчик и мгновенная пересборка, и только
 * потом нести на устройство. Разница в стоимости ошибки огромная: в QEMU
 * неверный адрес это перезапуск, на телефоне — поездка в сервис.
 */
#include "arm64.h"
#include "platform.h"

/* Реализации: arch/arm64/uart_pl011.c и arch/arm64/uart_msm.c */
void pl011_init(uint64_t base);
void pl011_putc(char c);
int  pl011_getc(void);

void msm_uart_init(uint64_t base);
void msm_uart_putc(char c);
int  msm_uart_getc(void);

static int g_kind = UART_KIND_NONE;

void uart_init(void) {
    const platform_info_t *pi = platform();

    g_kind = pi->uart_kind;

    switch (g_kind) {
        case UART_KIND_PL011: pl011_init(pi->uart_base);    break;
        case UART_KIND_MSM:   msm_uart_init(pi->uart_base); break;
        default: break;   /* порта нет — вывод молча уходит в никуда */
    }
}

void uart_putc(char c) {
    /* Всё, что уходит в порт, дублируется в постоянный журнал. Это не
       роскошь: на телефоне порт выведен на разъём наушников и без паяного
       кабеля молчит, а журнал переживает перезагрузку и читается уже из
       Android. Там, где такой области нет, вызов раскрывается в пустоту —
       см. CONFIG_LOG_RAMOOPS в arch/arm64/arm64.h. */
    ramoops_putc(c);

    switch (g_kind) {
        case UART_KIND_PL011: pl011_putc(c);    break;
        case UART_KIND_MSM:   msm_uart_putc(c); break;
        default: break;
    }
}

void uart_write(const char *s) {
    while (*s) uart_putc(*s++);
}

int uart_getc(void) {
    switch (g_kind) {
        case UART_KIND_PL011: return pl011_getc();
        case UART_KIND_MSM:   return msm_uart_getc();
        default: return -1;
    }
}

void uart_write_hex(uint64_t v) {
    static const char digits[] = "0123456789ABCDEF";
    uart_write("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        uart_putc(digits[(v >> shift) & 0xF]);
}

/* Отчёт о том, что удалось выяснить про железо. Печатается один раз при
 * старте — и это первое, на что смотришь, когда система не подаёт признаков
 * жизни на новом устройстве: видно, разобрано ли дерево, найден ли порт и
 * откуда взялся его адрес. */
void uart_report_platform(void) {
    const platform_info_t *pi = platform();

    uart_write("\nproshivkaOS NEXT\n");
    uart_write("  plata     : ");
    uart_write(pi->model ? pi->model : "(neizvestno)");
    uart_putc('\n');

    uart_write("  devicetree: ");
    uart_write(pi->fdt_ok ? "razobrano\n" : "NET (rabotaem po konstantam platy)\n");

    uart_write("  uart      : ");
    switch (pi->uart_kind) {
        case UART_KIND_PL011: uart_write("PL011 ");  break;
        case UART_KIND_MSM:   uart_write("UARTDM "); break;
        default:              uart_write("NE NAYDEN "); break;
    }
    uart_write_hex(pi->uart_base);
    uart_write(pi->uart_from_fdt ? " (iz dt)\n" : " (konstanta platy)\n");

    uart_write("  gic       : ");
    if (pi->gic_base) { uart_write_hex(pi->gic_base); uart_write(" (poka ne ispolzuetsya)\n"); }
    else               uart_write("ne nayden\n");

    uart_write("  ekran     : ");
    if (pi->fb_valid) {
        uart_write_hex(pi->fb_addr);
        uart_write(" ");
        /* Размеры печатаем шестнадцатерично — своей функции для десятичного
           вывода в этом слое нет, а тащить её сюда ради отладки незачем. */
        uart_write_hex((uint64_t)pi->fb_width);
        uart_write("x");
        uart_write_hex((uint64_t)pi->fb_height);
        uart_write(pi->fb_format == 2 ? " r8g8b8\n" : " x8r8g8b8\n");
    } else {
        uart_write("gotovogo net, probuem ramfb\n");
    }
}
