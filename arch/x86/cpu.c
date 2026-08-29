/* arch/x86/cpu.c */
#include <stdint.h>

#include "hal.h"

const char *hal_arch_name(void) { return "X86"; }

/* Строка процессора из CPUID. Листы 0x80000002..0x80000004 отдают по 16
 * символов каждый — всего 48. Сначала спрашиваем лист 0x80000000: он
 * говорит, до какого расширенного листа процессор вообще умеет отвечать. */
static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    /* ebx сохраняется вручную: на 32-битном x86 он может быть занят под
       указатель на таблицу глобальных смещений, и отдавать его компилятору
       напрямую нельзя даже при -fno-pie. */
    __asm__ volatile ("xchgl %%ebx, %1\n\t"
                      "cpuid\n\t"
                      "xchgl %%ebx, %1"
                      : "=a"(*a), "=r"(*b), "=c"(*c), "=d"(*d)
                      : "0"(leaf), "1"(0), "2"(0), "3"(0));
}

const char *hal_cpu_name(void) {
    static char buf[52];

    uint32_t a, b, c, d;
    cpuid(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000004u)
        return "X86 (CPUID NEDOSTUPEN)";

    int n = 0;
    for (uint32_t leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
        cpuid(leaf, &a, &b, &c, &d);
        uint32_t regs[4] = { a, b, c, d };
        for (int r = 0; r < 4; r++)
            for (int i = 0; i < 4; i++)
                buf[n++] = (char)((regs[r] >> (i * 8)) & 0xFF);
    }
    buf[n] = '\0';

    /* Производители любят выравнивать строку пробелами слева. */
    char *p = buf;
    while (*p == ' ') p++;
    return p;
}

void hal_arch_init(void) {
    /* На x86 к моменту входа в kmain процессор уже в защищённом режиме с
       плоской моделью памяти — этим занимался GRUB. Делать нечего.
       Парная реализация для ARM64 (arch/arm64/cpu.c) включает MMU. */
}

void hal_cpu_halt(void) {
    __asm__ volatile ("cli; hlt");
}
