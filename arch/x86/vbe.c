/* arch/x86/vbe.c */
#include "ports.h"
#include "pci.h"
#include "vbe.h"

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID          0
#define VBE_DISPI_INDEX_XRES        1
#define VBE_DISPI_INDEX_YRES        2
#define VBE_DISPI_INDEX_BPP         3
#define VBE_DISPI_INDEX_ENABLE      4
#define VBE_DISPI_INDEX_BANK        5
#define VBE_DISPI_INDEX_VIRT_WIDTH  6
#define VBE_DISPI_INDEX_VIRT_HEIGHT 7
#define VBE_DISPI_INDEX_X_OFFSET    8
#define VBE_DISPI_INDEX_Y_OFFSET    9

#define VBE_DISPI_DISABLED   0x00
#define VBE_DISPI_ENABLED    0x01
#define VBE_DISPI_LFB_ENABLED 0x40

static void vbe_write(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

static uint16_t vbe_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

void vbe_disable(void) {
    /* безопасно вызывать, даже если VBE никогда не включали — просто
       пишем в порт, который под QEMU/Bochs есть всегда */
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
}

int vbe_set_mode(uint16_t width, uint16_t height, uint8_t bpp, volatile uint8_t **out_lfb) {
    /* ID-регистр должен вернуть 0xB0C0..0xB0C5 у настоящей Bochs-совместимой
     * видеокарты — так проверяем, что интерфейс вообще есть (иначе мы не
     * под QEMU/Bochs/VirtualBox, и лучше остаться на classic mode13h). */
    uint16_t id = vbe_read(VBE_DISPI_INDEX_ID);
    if (id < 0xB0C0 || id > 0xB0CF)
        return 0;

    uint8_t bus, dev, func;
    if (!pci_find_vga(&bus, &dev, &func))
        return 0;

    uint32_t bar0 = pci_read_bar0(bus, dev, func);
    if (bar0 == 0)
        return 0;

    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    vbe_write(VBE_DISPI_INDEX_XRES, width);
    vbe_write(VBE_DISPI_INDEX_YRES, height);
    vbe_write(VBE_DISPI_INDEX_BPP, bpp);
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    *out_lfb = (volatile uint8_t *)bar0;
    return 1;
}
