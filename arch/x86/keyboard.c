/* arch/x86/keyboard.c — минимальный опросный (не по прерываниям) PS/2-драйвер.
 * Прерывания подключим на этапе с IDT; пока hal_console_getc_blocking()
 * крутится в цикле и читает порт 0x60, когда бит "буфер полон" установлен.
 */
#include <stdint.h>
#include "ports.h"

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64
#define KBD_OUTPUT_FULL 0x01

/* Скан-коды набора 1 (Set 1), только нижний регистр + минимум спецсимволов —
 * достаточно для ввода команд шелла на первом этапе. */
static const char scancode_to_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0 /*ctrl*/,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0 /*lshift*/,'\\','z','x','c','v','b','n','m',',','.','/',
    0 /*rshift*/,'*',
    0 /*alt*/,' ',
    /* остальное на первом этапе не нужно */
};

int keyboard_poll(void) {
    uint8_t status = inb(KBD_STATUS_PORT);

    if (!(status & KBD_OUTPUT_FULL))
        return -1;
    if (status & 0x20)          /* бит 5 = байт принадлежит мыши (AUX), не нам */
        return -1;

    uint8_t scancode = inb(KBD_DATA_PORT);

    if (scancode & 0x80)          /* key release — игнорируем */
        return -1;
    if (scancode >= sizeof(scancode_to_ascii))
        return -1;

    char c = scancode_to_ascii[scancode];
    return c ? (int)(unsigned char)c : -1;
}

char keyboard_getc_blocking(void) {
    int c;
    do {
        c = keyboard_poll();
    } while (c == -1);
    return (char)c;
}
