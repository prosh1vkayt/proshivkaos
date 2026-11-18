/* arch/x86/net_rtl8139.c — минимальный драйвер RTL8139 (QEMU: -device rtl8139).
 * Опрос в цикле, без прерываний и DMA-выделения: все буферы статические.
 * Предполагается, что физический адрес == виртуальному (нет paging).
 */
#include <stdint.h>
#include <stddef.h>
#include "ports.h"
#include "pci.h"

#define RTL_VENDOR  0x10EC
#define RTL_DEVICE  0x8139

#define REG_IDR0    0x00   /* MAC, 6 байт */
#define REG_TSD0    0x10   /* 4 дескриптора передачи, шаг 4 */
#define REG_TSAD0   0x20
#define REG_RBSTART 0x30
#define REG_CMD     0x37
#define REG_CAPR    0x38
#define REG_IMR     0x3C
#define REG_RCR     0x44
#define REG_CONFIG1 0x52

#define CMD_RST     0x10
#define CMD_RE      0x08
#define CMD_TE      0x04
#define CMD_BUFE    0x01
#define TSD_TOK     (1u << 15)

#define RX_RING     8192
#define RX_BUF_SIZE (RX_RING + 16 + 1500)   /* +1500 из-за бита WRAP */
#define TX_BUF_SIZE 1792
#define MIN_FRAME   60

static uint8_t rx_buf[RX_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t tx_buf[4][TX_BUF_SIZE] __attribute__((aligned(4)));

static uint16_t io_base;
static uint8_t  mac[6];
static uint16_t rx_off;
static uint8_t  tx_cur;
static int      net_ready;

int hal_net_init(void) {
    uint8_t bus, dev, func;
    if (!pci_find_device(RTL_VENDOR, RTL_DEVICE, &bus, &dev, &func))
        return -1;

    /* включить I/O space (бит 0) и bus mastering (бит 2) в PCI Command */
    uint32_t cmd = pci_config_read32(bus, dev, func, 0x04);
    pci_config_write32(bus, dev, func, 0x04, (cmd & 0xFFFF) | 0x05);

    io_base = (uint16_t)(pci_config_read32(bus, dev, func, 0x10) & 0xFFFC);

    outb(io_base + REG_CONFIG1, 0x00);          /* включить питание */
    outb(io_base + REG_CMD, CMD_RST);           /* сброс */
    for (int i = 0; i < 1000000; i++)
        if (!(inb(io_base + REG_CMD) & CMD_RST)) break;
    if (inb(io_base + REG_CMD) & CMD_RST)
        return -2;

    for (int i = 0; i < 6; i++)
        mac[i] = inb(io_base + REG_IDR0 + i);

    outl(io_base + REG_RBSTART, (uint32_t)(uintptr_t)rx_buf);
    outw(io_base + REG_IMR, 0x0000);            /* прерывания выключены */
    /* AAP|APM|AM|AB (принимать всё нужное) + WRAP */
    outl(io_base + REG_RCR, 0x0F | (1u << 7));
    outb(io_base + REG_CMD, CMD_RE | CMD_TE);

    rx_off = 0;
    tx_cur = 0;
    net_ready = 1;
    return 0;
}

void hal_net_get_mac(uint8_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = mac[i];
}

/* Отправка кадра (Ethernet без CRC). 0 = успех. */
int hal_net_send(const void *data, size_t len) {
    if (!net_ready || len > 1514) return -1;

    uint8_t *dst = tx_buf[tx_cur];
    const uint8_t *src = data;
    for (size_t i = 0; i < len; i++) dst[i] = src[i];
    while (len < MIN_FRAME) dst[len++] = 0;     /* дополнить до минимума */

    outl(io_base + REG_TSAD0 + 4 * tx_cur, (uint32_t)(uintptr_t)dst);
    outl(io_base + REG_TSD0  + 4 * tx_cur, (uint32_t)len);  /* старт */

    for (int i = 0; i < 1000000; i++) {
        if (inl(io_base + REG_TSD0 + 4 * tx_cur) & TSD_TOK) {
            tx_cur = (tx_cur + 1) & 3;
            return 0;
        }
    }
    return -2;
}

/* Неблокирующий приём. Возвращает длину кадра или 0, если пусто. */
size_t hal_net_recv(void *out, size_t max_len) {
    if (!net_ready) return 0;
    if (inb(io_base + REG_CMD) & CMD_BUFE) return 0;

    uint8_t *p = rx_buf + rx_off;
    uint16_t status = (uint16_t)(p[0] | (p[1] << 8));
    uint16_t len    = (uint16_t)(p[2] | (p[3] << 8));   /* включая CRC 4 байта */

    size_t n = 0;
    if ((status & 1) && len >= 4 && len <= 1518) {
        n = (size_t)len - 4;
        if (n > max_len) n = max_len;
        uint8_t *o = out;
        for (size_t i = 0; i < n; i++) o[i] = p[4 + i];
    }

    rx_off = (uint16_t)((rx_off + len + 4 + 3) & ~3u);
    rx_off %= RX_RING;
    outw(io_base + REG_CAPR, (uint16_t)(rx_off - 16));
    return n;
}
