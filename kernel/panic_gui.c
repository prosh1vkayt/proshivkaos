/* kernel/panic_gui.c — hal_panic для GUI-сборки: пишет прямо в фреймбуфер,
 * т.к. текстовой консоли (hal_console_*) в этой сборке нет.
 */
#include "hal.h"
#include "hal_gfx.h"

void hal_panic(const char *msg) {
    hal_gfx_clear(GFX_RED);
    hal_gfx_draw_string(4, 4, "KERNEL PANIC:", GFX_WHITE, GFX_RED);
    hal_gfx_draw_string(4, 4 + FONT_H + 2, msg, GFX_WHITE, GFX_RED);

    for (;;) {
        hal_cpu_halt();   /* "cli; hlt" на x86, "wfi" на ARM64 */
    }
}
