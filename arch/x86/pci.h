/* arch/x86/pci.h — минимальный доступ к PCI config space (механизм #1,
 * порты 0xCF8/0xCFC) — нужен только для одной вещи: найти линейный
 * framebuffer видеокарты (BAR0) для режимов выше 320x200 (см. vbe.c).
 */
#ifndef ARCH_X86_PCI_H
#define ARCH_X86_PCI_H

#include <stdint.h>

/* Ищет первое PCI-устройство класса "Display controller" (0x03).
 * Возвращает 1 и заполняет bus/dev/func при успехе, иначе 0. */
int pci_find_vga(uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_func);

/* Читает BAR0 устройства и возвращает физический адрес (маскирует флаги). */
uint32_t pci_read_bar0(uint8_t bus, uint8_t dev, uint8_t func);

#endif
