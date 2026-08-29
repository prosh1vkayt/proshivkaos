/* arch/x86/mouse.h — опросный (не по IRQ) PS/2-драйвер мыши. QEMU сам
 * транслирует движения хостовой мыши/тачпада над окном эмулятора в
 * события PS/2-мыши — отдельного "драйвера тачпада" не нужно, тачпад
 * MacBook управляет курсором ровно так же, как обычная мышь.
 */
#ifndef ARCH_X86_MOUSE_H
#define ARCH_X86_MOUSE_H

#include <stdint.h>

typedef struct {
    int dx, dy;          /* относительное смещение с прошлого события */
    int left, right, middle;
} mouse_event_t;

void mouse_init(void);
/* Возвращает 1 и заполняет ev, если накопился полный пакет (иначе 0 —
 * неблокирующий опрос, вызывать в общем цикле рядом с keyboard_poll). */
int mouse_poll(mouse_event_t *ev);

#endif
