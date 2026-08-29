/* arch/x86/mouse.c
 *
 * Стандартный PS/2-контроллер (8042) обслуживает и клавиатуру, и мышь
 * через один и тот же порт данных 0x60 — их различает бит 5 в порту
 * статуса 0x64 ("байт пришёл от AUX-устройства", т.е. от мыши).
 * keyboard.c уже это учитывает (пропускает байт, если бит 5 установлен,
 * не читая его) — эта мышь читает именно такие байты.
 *
 * Инициализация — стандартная последовательность для "Basic PS/2 Mouse"
 * (без колеса, 3-байтный пакет): включить AUX-порт, включить его в
 * командном байте контроллера, отправить мыши "восстановить настройки
 * по умолчанию" и "включить передачу данных".
 */
#include "ports.h"
#include "mouse.h"

#define KBD_STATUS_PORT 0x64
#define KBD_CMD_PORT    0x64
#define KBD_DATA_PORT   0x60

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL  0x02
#define STATUS_AUX_DATA    0x20

static void wait_input_clear(void) {
    int timeout = 100000;
    while (timeout-- && (inb(KBD_STATUS_PORT) & STATUS_INPUT_FULL)) { }
}

static void wait_output_full(void) {
    int timeout = 100000;
    while (timeout-- && !(inb(KBD_STATUS_PORT) & STATUS_OUTPUT_FULL)) { }
}

static void mouse_write(uint8_t data) {
    wait_input_clear();
    outb(KBD_CMD_PORT, 0xD4);   /* следующий байт на 0x60 — команда мыши */
    wait_input_clear();
    outb(KBD_DATA_PORT, data);
}

static uint8_t mouse_read_ack(void) {
    wait_output_full();
    return inb(KBD_DATA_PORT);
}

static uint8_t g_packet[3];
static int g_packet_idx = 0;

void mouse_init(void) {
    wait_input_clear();
    outb(KBD_CMD_PORT, 0xA8);          /* включить AUX-порт */

    wait_input_clear();
    outb(KBD_CMD_PORT, 0x20);          /* прочитать командный байт контроллера */
    wait_output_full();
    uint8_t status = inb(KBD_DATA_PORT);

    status |= 0x02;                    /* разрешить IRQ12 (даже если не используем —
                                           контроллер иначе может не выставлять
                                           AUX-данные в буфер вывода корректно) */
    status &= ~0x20;                   /* снять "мышь отключена" */

    wait_input_clear();
    outb(KBD_CMD_PORT, 0x60);          /* записать командный байт обратно */
    wait_input_clear();
    outb(KBD_DATA_PORT, status);

    mouse_write(0xF6);                 /* set defaults */
    mouse_read_ack();

    mouse_write(0xF4);                 /* enable data reporting */
    mouse_read_ack();

    g_packet_idx = 0;
}

int mouse_poll(mouse_event_t *ev) {
    uint8_t status = inb(KBD_STATUS_PORT);

    if (!(status & STATUS_OUTPUT_FULL)) return 0;
    if (!(status & STATUS_AUX_DATA))    return 0;   /* это байт клавиатуры, не наш */

    uint8_t data = inb(KBD_DATA_PORT);
    g_packet[g_packet_idx++] = data;

    if (g_packet_idx < 3) return 0;
    g_packet_idx = 0;

    uint8_t flags = g_packet[0];
    if (!(flags & 0x08)) return 0;   /* бит "always 1" не установлен — битый пакет, ресинхронизация */

    int dx = g_packet[1];
    int dy = g_packet[2];
    if (flags & 0x10) dx -= 256;     /* знак X */
    if (flags & 0x20) dy -= 256;     /* знак Y */

    ev->dx = dx;
    ev->dy = -dy;                    /* PS/2: Y+ вверх, у нас Y+ вниз (экранные координаты) */
    ev->left   = flags & 0x01;
    ev->right  = flags & 0x02;
    ev->middle = flags & 0x04;

    return 1;
}
