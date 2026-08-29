/* kernel/kernel_touch.c — точка входа тач-сборки.
 *
 * Третья параллельная kmain рядом с kernel/kernel.c (текстовый shell) и
 * kernel/kernel_gui.c (оконный интерфейс). Собираются они раздельно, друг
 * друга не задевают, и — что важнее — ЭТОТ файл одинаков для x86 и ARM64.
 * Вся разница между "телефоном" и "настольным компьютером" спрятана под
 * hal_*: какой драйвер экрана, какой источник касаний, какой таймер.
 */
#include "hal.h"
#include "hal_time.h"

void ramfs_init(void);   /* fs/ramfs.c        */
void touch_main(void);   /* gui/touch/touch_ui.c — не возвращается */

void kmain(uint32_t boot_magic, void *boot_info) {
    /* На x86 здесь лежит magic от Multiboot и указатель на multiboot_info,
       на ARM64 — ноль и указатель на device tree (см. arch/arm64/boot.S).
       Ни то, ни другое на этом этапе не разбирается. */
    (void)boot_magic;
    (void)boot_info;

    hal_arch_init();    /* ARM64: включить MMU и кэши. x86: пусто.       */
    hal_time_init();    /* x86: откалибровать TSC по PIT                  */
    hal_mem_init();
    ramfs_init();

    touch_main();

    hal_panic("touch_main() returned unexpectedly");
}
