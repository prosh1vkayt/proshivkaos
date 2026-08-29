/* arch/x86/cpu.c */
#include "hal.h"

const char *hal_arch_name(void) { return "X86"; }

void hal_arch_init(void) {
    /* На x86 к моменту входа в kmain процессор уже в защищённом режиме с
       плоской моделью памяти — этим занимался GRUB. Делать нечего.
       Парная реализация для ARM64 (arch/arm64/cpu.c) включает MMU. */
}

void hal_cpu_halt(void) {
    __asm__ volatile ("cli; hlt");
}
