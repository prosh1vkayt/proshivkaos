/* arch/x86/keyboard.c — минимальный опросный (не по прерываниям) PS/2-драйвер.
 * Прерывания подключим на этапе с IDT; пока hal_console_getc_blocking()
 * крутится в цикле и читает порт 0x60, когда бит "буфер полон" установлен.
 *
 * Поддерживает:
 *  - Ctrl-модификатор: буквы под Ctrl превращаются в управляющие коды
 *    1..26 (стандартная схема terminal 'letter & 0x1F' — Ctrl+S=0x13,
 *    Ctrl+Q=0x11, Ctrl+F=0x06 и т.д., см. apps/editor.c).
 *  - Shift-модификатор: заглавные буквы + верхний ряд символов (только
 *    раскладка US QWERTY — другие не поддержаны).
 *  - Расширенные скан-коды (префикс 0xE0): стрелки, Home/End, Delete,
 *    PageUp/PageDown — возвращаются как HAL_KEY_* (см. hal.h), это коды
 *    выше 0xFF, так что с обычным ASCII они никогда не пересекаются.
 *
 * Расширенная последовательность приходит DVUMYA байтами не за один опрос,
 * поэтому состояние "только что видели 0xE0" хранится в pending_e0 между
 * вызовами keyboard_poll() — сам опрос всё ещё неблокирующий, просто на
 * "половинчатый" байт отвечает -1 (нет готового события), как раньше
 * отвечал на события, которых не было в таблице совсем.
 */
#include <stdint.h>
#include "ports.h"
#include "hal.h"

#define KBD_DATA_PORT   0x60
#define KBD_STATUS_PORT 0x64
#define KBD_OUTPUT_FULL 0x01

/* Скан-коды набора 1 (Set 1), нижний регистр — как и раньше. */
static const char scancode_to_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0 /*ctrl*/,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0 /*lshift*/,'\\','z','x','c','v','b','n','m',',','.','/',
    0 /*rshift*/,'*',
    0 /*alt*/,' ',
    /* остальное на первом этапе не нужно */
};

/* Символы верхнего ряда под Shift — только там, где они отличаются от
 * заглавной буквы (для букв верхний регистр считается на лету: c-32).
 * Индексы совпадают со scancode_to_ascii выше (US QWERTY). */
static const char shifted_symbols[128] = {
    [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%',
    [0x07]='^', [0x08]='&', [0x09]='*', [0x0A]='(', [0x0B]=')',
    [0x0C]='_', [0x0D]='+',
    [0x1A]='{', [0x1B]='}',
    [0x27]=':', [0x28]='"', [0x29]='~',
    [0x2B]='|',
    [0x33]='<', [0x34]='>', [0x35]='?',
};

static int shift_down  = 0;
static int ctrl_down   = 0;
static int pending_e0  = 0;   /* предыдущий байт был префиксом расширенного скан-кода */

int keyboard_poll(void) {
    uint8_t status = inb(KBD_STATUS_PORT);

    if (!(status & KBD_OUTPUT_FULL))
        return -1;
    if (status & 0x20)          /* бит 5 = байт принадлежит мыши (AUX), не нам */
        return -1;

    uint8_t sc = inb(KBD_DATA_PORT);

    if (sc == 0xE0) {
        pending_e0 = 1;
        return -1;              /* сам по себе префикс — ещё не событие */
    }

    int is_extended = pending_e0;
    pending_e0 = 0;

    int     release = (sc & 0x80) != 0;
    uint8_t code    = sc & 0x7F;

    if (is_extended) {
        if (release) {
            if (code == 0x1D) ctrl_down = 0;   /* правый Ctrl отпущен */
            return -1;
        }
        switch (code) {
            case 0x48: return HAL_KEY_UP;
            case 0x50: return HAL_KEY_DOWN;
            case 0x4B: return HAL_KEY_LEFT;
            case 0x4D: return HAL_KEY_RIGHT;
            case 0x47: return HAL_KEY_HOME;
            case 0x4F: return HAL_KEY_END;
            case 0x53: return HAL_KEY_DEL;
            case 0x49: return HAL_KEY_PAGE_UP;
            case 0x51: return HAL_KEY_PAGE_DOWN;
            case 0x1D: ctrl_down = 1; return -1;   /* правый Ctrl нажат */
            default:   return -1;
        }
    }

    /* модификаторы обрабатываются на обеих сторонах нажатие/отпускание */
    if (code == 0x1D) { ctrl_down  = !release; return -1; }              /* левый Ctrl  */
    if (code == 0x2A || code == 0x36) { shift_down = !release; return -1; } /* Shift ЛП */

    if (release)
        return -1;

    if (code >= sizeof(scancode_to_ascii))
        return -1;

    char c = scancode_to_ascii[code];
    if (!c)
        return -1;

    if (shift_down) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        else if (shifted_symbols[code]) c = shifted_symbols[code];
    }

    if (ctrl_down) {
        if (c >= 'a' && c <= 'z') return (int)(c - 'a' + 1);
        if (c >= 'A' && c <= 'Z') return (int)(c - 'A' + 1);
    }

    return (int)(unsigned char)c;
}

char keyboard_getc_blocking(void) {
    int c;
    do {
        c = keyboard_poll();
        /* HAL_KEY_* (>255) сюда не помещаются — эта функция возвращает char
         * и предназначена для обычного текстового ввода (см. shell.c);
         * за полным диапазоном — hal_console_getc(), см. apps/editor.c. */
    } while (c == -1 || c > 255);
    return (char)c;
}
