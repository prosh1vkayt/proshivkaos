/* kernel/panic.c */
#include "hal.h"

void hal_panic(const char *msg) {
    hal_console_write("\n*** KERNEL PANIC: ");
    hal_console_write(msg);
    hal_console_write(" ***\n");

    for (;;) {
        hal_cpu_halt();
    }
}
