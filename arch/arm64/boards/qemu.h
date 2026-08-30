/* arch/arm64/boards/qemu.h — плата по умолчанию: QEMU -M virt.
 * Экран здесь не "готовый фреймбуфер", а честное устройство ramfb —
 * поэтому BOARD_HAS_STATIC_FB не определён, и hal_gfx_arm64.c идёт по
 * пути fwcfg/ramfb, как и раньше.
 */
#ifndef ARCH_ARM64_BOARD_QEMU_H
#define ARCH_ARM64_BOARD_QEMU_H

#define BOARD_NAME "QEMU VIRT"

/* Запасные значения на случай, если device tree недоступен. Под QEMU это
 * почти невозможно (гипервизор всегда передаёт дерево), но пусть будут:
 * тот же образ можно запустить и на плате, где загрузчик дерева не даёт. */
#define BOARD_UART_KIND  UART_KIND_PL011
#define BOARD_UART_BASE  0x09000000UL

#endif
