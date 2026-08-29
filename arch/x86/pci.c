/* arch/x86/pci.c — Configuration Access Mechanism #1 (стандартный на всех
 * x86 чипсетах с 1990-х, включая всё, что эмулирует QEMU/Bochs/VirtualBox).
 */
#include "ports.h"
#include "pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t address = (1U << 31)
        | ((uint32_t)bus << 16)
        | ((uint32_t)dev << 11)
        | ((uint32_t)func << 8)
        | (offset & 0xFC);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

int pci_find_vga(uint8_t *out_bus, uint8_t *out_dev, uint8_t *out_func) {
    /* Полный перебор шины 0 (256 устройств max) — достаточно почти всегда:
     * видеокарта в QEMU/Bochs/VirtualBox всегда на шине 0. Перебор других
     * шин через мосты — усложнение, не нужное на этом этапе. */
    for (uint16_t dev = 0; dev < 32; dev++) {
        for (uint8_t func = 0; func < 8; func++) {
            uint32_t id = pci_config_read32((uint8_t)0, (uint8_t)dev, func, 0x00);
            uint16_t vendor = id & 0xFFFF;
            if (vendor == 0xFFFF) continue;   /* устройства нет */

            uint32_t class_reg = pci_config_read32(0, (uint8_t)dev, func, 0x08);
            uint8_t class_code = (class_reg >> 24) & 0xFF;
            uint8_t subclass   = (class_reg >> 16) & 0xFF;

            if (class_code == 0x03 && (subclass == 0x00 || subclass == 0x80)) {
                *out_bus = 0;
                *out_dev = (uint8_t)dev;
                *out_func = func;
                return 1;
            }
        }
    }
    return 0;
}

uint32_t pci_read_bar0(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t bar0 = pci_config_read32(bus, dev, func, 0x10);
    /* бит 0 = 0 значит memory-mapped (не I/O); маскируем нижние флаговые
     * биты (для 32-битного non-prefetchable BAR это младшие 4 бита) */
    return bar0 & 0xFFFFFFF0;
}
