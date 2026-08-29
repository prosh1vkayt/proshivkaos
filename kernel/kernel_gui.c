/* kernel/kernel_gui.c — точка входа для GUI-сборки (make gui).
 * Параллельна kernel/kernel.c (текстовый shell) — собираются раздельно,
 * text-режим этим не затрагивается.
 */
#include "hal.h"

void ramfs_init(void);   /* fs/ramfs.c */
void gui_main(void);     /* gui/wm.c — не возвращается */

void kmain(uint32_t multiboot_magic, void *multiboot_info) {
    (void)multiboot_magic;
    (void)multiboot_info;

    hal_arch_init();
    hal_mem_init();
    ramfs_init();

    gui_main();

    hal_panic("gui_main() returned unexpectedly");
}
