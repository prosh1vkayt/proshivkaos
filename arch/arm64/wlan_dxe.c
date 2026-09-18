/* arch/arm64/wlan_dxe.c — DXE: кольца, по которым ходят кадры 802.11.
 *
 * Управление радио идёт по SMD (wlan.c), а сами кадры — маяки, ответы на
 * пробы, данные — по DXE, движку прямого доступа к памяти внутри Pronto.
 * Устроен он как у сетевых карт: в общей памяти лежат кольца дескрипторов
 * по 32 байта, в каждом — адрес буфера и слово управления. Бит VLD в нём
 * означает «дескриптор у устройства»: мы ставим его, отдавая пустой буфер
 * на приём, Pronto снимает, положив туда кадр.
 *
 * Каналов четыре: приём низкий и высокий (данные и управление), передача
 * так же. Регистры — 0x0A202000 (DXE) и 0x0A204000 (CCU); раскладка,
 * слова управления и порядок программирования — drivers/net/wireless/ath/
 * wcn36xx/dxe.c.
 *
 * Прерываний не ждём: дескрипторы опрашиваются из фоновой прокрутки.
 *
 * Память — хвост области обмена с устройствами (__dma_start + 1,28 МБ):
 * она не кэшируется и отображена как есть, адрес для Pronto совпадает с
 * нашим. Колец меньше, чем в Linux (там 512 на приём), — для сканирования
 * и первых кадров хватает с запасом. */
#include "arm64.h"
#include "boards/board.h"

extern char __dma_start[];

#define DXE_BASE              0x0A202000ull
#define CCU_BASE              0x0A204000ull
#define CCU_DXE_INT_SELECT    0x10DC

#define DXE_CSR_RESET         0x00
#define DXE_ENCH_ADDR         0x04
#define DXE_INT_MASK          0x18
#define DXE_INT_SRC_RAW       0x20
#define DXE_INT_CLR           0x30
#define DXE_INT_ED_CLR        0x34
#define DXE_INT_DONE_CLR      0x38
#define DXE_INT_ERR_CLR       0x3C
#define DXE_RESET_VALUE       0x5C89

#define CH_TX_L               0x400
#define CH_RX_L               0x440
#define CH_RX_H               0x4C0
#define CH_TX_H               0x500
#define CH_CTL                0x00
#define CH_STATUS             0x04
#define CH_SRC                0x0C
#define CH_DEST               0x14
#define CH_NEXT               0x1C

#define STAT_DONE             0x8000u
#define STAT_ERR              0x4000u
#define STAT_ED               0x2000u

#define INT_CH0               0x01u
#define INT_CH1               0x02u
#define INT_CH3               0x08u
#define INT_CH4               0x10u

/* Слова управления дескрипторов (WCN36XX_DXE_CTRL_*). */
#define C_VLD      (1u << 0)
#define C_EOP      (1u << 3)
#define C_SIQ      (1u << 5)
#define C_DIQ      (1u << 6)
#define C_PDU_REL  (1u << 8)
#define C_INT      (1u << 17)
#define C_SWAP     (1u << 20)
#define C_ENDIAN   (1u << 21)
#define XTYPE(x)   ((x) << 1)
#define BTHLD(x)   ((x) << 9)
#define PRIO(x)    ((x) << 13)

#define CTRL_RX_L  (C_VLD | XTYPE(3) | C_EOP | C_SIQ | C_PDU_REL | BTHLD(6) | PRIO(5) | C_INT | C_SWAP)
#define CTRL_RX_H  (C_VLD | XTYPE(3) | C_EOP | C_SIQ | C_PDU_REL | BTHLD(8) | PRIO(6) | C_INT | C_SWAP)
#define CTRL_TX_L  (XTYPE(2) | C_DIQ | BTHLD(5) | PRIO(4) | C_INT | C_SWAP | C_ENDIAN)
#define CTRL_TX_H  (XTYPE(2) | C_DIQ | BTHLD(7) | PRIO(6) | C_INT | C_SWAP | C_ENDIAN)

/* Слова управления каналов (WCN36XX_DXE_CH_CTRL_*). */
#define H_EN       (1u << 0)
#define H_EOP      (1u << 3)
#define H_SIQ      (1u << 5)
#define H_PDU_REL  (1u << 8)
#define H_INE_ED   (1u << 17)
#define H_INE_ERR  (1u << 18)
#define H_INE_DONE (1u << 19)
#define H_EDEN     (1u << 20)
#define H_EDVEN    (1u << 21)
#define H_ENDIAN   (1u << 26)
#define H_SWAP     (1u << 31)
#define H_SEL(x)   ((x) << 22)
#define H_COMMON   (H_EN | XTYPE(3) | H_EOP | H_SIQ | H_PDU_REL | H_INE_ED | H_INE_ERR | \
                    H_INE_DONE | H_EDEN | H_EDVEN | H_ENDIAN | H_SWAP)
#define CH_DEF_RX_L  (H_COMMON | BTHLD(6) | PRIO(5) | H_SEL(1))
#define CH_DEF_RX_H  (H_COMMON | BTHLD(8) | PRIO(6) | H_SEL(3))

/* Передача: канал берёт данные из памяти (DIQ), а не отдаёт в неё. */
#define H_DIQ      (1u << 6)
#define H_TX_COMMON (H_EN | XTYPE(2) | H_EOP | H_DIQ | H_PDU_REL | H_INE_ED | H_INE_ERR | \
                     H_INE_DONE | H_EDEN | H_EDVEN | H_ENDIAN | H_SWAP)
#define CH_DEF_TX_L  (H_TX_COMMON | BTHLD(5) | PRIO(4) | H_SEL(0))
#define CH_DEF_TX_H  (H_TX_COMMON | BTHLD(7) | PRIO(6) | H_SEL(4))

/* Кадр уходит парой дескрипторов: первый — дескриптор буфера (BD, 40
   байт служебных полей для прошивки), второй — сам кадр 802.11. */
#define CTRL_TX_L_BD   (C_VLD | XTYPE(2) | C_DIQ | BTHLD(5) | PRIO(4) | C_SWAP | C_ENDIAN)
#define CTRL_TX_L_SKB  (C_VLD | XTYPE(2) | C_EOP | C_DIQ | BTHLD(5) | PRIO(4) | C_INT | C_SWAP | C_ENDIAN)
#define CTRL_TX_H_BD   (C_VLD | XTYPE(2) | C_DIQ | BTHLD(7) | PRIO(6) | C_SWAP | C_ENDIAN)
#define CTRL_TX_H_SKB  (C_VLD | XTYPE(2) | C_EOP | C_DIQ | BTHLD(7) | PRIO(6) | C_INT | C_SWAP | C_ENDIAN)
#define TX_BD_SLOT     128
#define TX_FRAME_SLOT  2048

#define WQ_TX                 0x6       /* Pronto v3 */
#define WQ_RX_L               0xB
#define WQ_RX_H               0x4

#define PKT_SIZE              0xF20
#define DESC_BYTES            32
#define N_TX_L                8
#define N_TX_H                8
#define N_RX_L                48
#define N_RX_H                16

#define POOL_OFFSET           0x140000u

typedef void (*dxe_rx_fn)(const uint8_t *bd, uint32_t size);

typedef struct {
    uint64_t desc;          /* адрес кольца дескрипторов */
    uint64_t bufs;          /* буферы приёма / кадров передачи */
    uint64_t bds;           /* дескрипторы буфера передачи */
    int      n, head;
    uint32_t ctrl, int_mask, en_mask, reg;
    uint32_t ctrl_bd, ctrl_skb, def_ctrl;
} ring_t;

static ring_t g_rx_l, g_rx_h, g_tx_l, g_tx_h;
static int g_ready;

static void dxe_w(uint32_t off, uint32_t v) { mmio_write32(DXE_BASE + off, v); }
static uint32_t dxe_r(uint32_t off) { return mmio_read32(DXE_BASE + off); }

static uint64_t g_brk;
static uint64_t take(uint32_t bytes) {
    uint64_t p = g_brk;
    bytes = (bytes + 15u) & ~15u;
    for (uint32_t i = 0; i < bytes; i += 4) mmio_write32(p + i, 0);
    g_brk += bytes;
    return p;
}

static void ring_init(ring_t *r, int n, uint32_t ctrl, uint32_t wq, int rx) {
    r->n = n;
    r->head = 0;
    r->ctrl = ctrl;
    r->desc = take((uint32_t)n * DESC_BYTES);
    r->bufs = rx ? take((uint32_t)n * PKT_SIZE) : 0;
    for (int i = 0; i < n; i++) {
        uint64_t d = r->desc + (uint32_t)i * DESC_BYTES;
        uint64_t next = r->desc + (uint32_t)((i + 1) % n) * DESC_BYTES;
        if (rx) {
            mmio_write32(d + 8, wq);                                   /* src_addr_l */
            mmio_write32(d + 12, (uint32_t)(r->bufs + (uint32_t)i * PKT_SIZE)); /* dst */
        } else {
            mmio_write32(d + 12, wq);
        }
        mmio_write32(d + 16, (uint32_t)next);                          /* phy_next_l */
        mmio_write32(d, ctrl);
    }
}

void wlan_dxe_smsm_init(void) {
    /* Приложения: «передача выключена, кольца пусты» (биты 10 и 9 в
       нашем слове SMSM) — как qcom_smem_state_update_bits в dxe.c. */
    uint32_t sz = 0;
    uint64_t smsm = smem_item(85, &sz);
    if (!smsm) return;
    uint32_t v = mmio_read32(smsm);
    v = (v & ~0x400u) | 0x200u;
    mmio_write32(smsm, v);
    dsb_sy();
    mmio_write32(BOARD_SMD_IPC_REG, 0x00080000u);
}

int wlan_dxe_init(void) {
    g_brk = (uint64_t)(uintptr_t)__dma_start + POOL_OFFSET;

    dxe_w(DXE_CSR_RESET, DXE_RESET_VALUE);
    mmio_write32(CCU_BASE + CCU_DXE_INT_SELECT,
                 ((INT_CH3 | INT_CH1) << 16) | INT_CH0 | INT_CH4);

    ring_init(&g_tx_l, N_TX_L, CTRL_TX_L, WQ_TX, 0);
    g_tx_l.bds = take(N_TX_L / 2 * TX_BD_SLOT);
    g_tx_l.bufs = take(N_TX_L / 2 * TX_FRAME_SLOT);
    g_tx_l.ctrl_bd = CTRL_TX_L_BD; g_tx_l.ctrl_skb = CTRL_TX_L_SKB;
    g_tx_l.def_ctrl = CH_DEF_TX_L; g_tx_l.reg = CH_TX_L; g_tx_l.int_mask = INT_CH0;
    dxe_w(CH_TX_L + CH_NEXT, (uint32_t)g_tx_l.desc);
    dxe_w(CH_TX_L + CH_DEST, WQ_TX);

    ring_init(&g_tx_h, N_TX_H, CTRL_TX_H, WQ_TX, 0);
    g_tx_h.bds = take(N_TX_H / 2 * TX_BD_SLOT);
    g_tx_h.bufs = take(N_TX_H / 2 * TX_FRAME_SLOT);
    g_tx_h.ctrl_bd = CTRL_TX_H_BD; g_tx_h.ctrl_skb = CTRL_TX_H_SKB;
    g_tx_h.def_ctrl = CH_DEF_TX_H; g_tx_h.reg = CH_TX_H; g_tx_h.int_mask = INT_CH4;
    dxe_w(CH_TX_H + CH_NEXT, (uint32_t)g_tx_h.desc);
    dxe_w(CH_TX_H + CH_DEST, WQ_TX);

    ring_init(&g_rx_l, N_RX_L, CTRL_RX_L, WQ_RX_L, 1);
    g_rx_l.int_mask = INT_CH1; g_rx_l.en_mask = INT_CH1; g_rx_l.reg = CH_RX_L;
    dxe_w(CH_RX_L + CH_NEXT, (uint32_t)g_rx_l.desc);
    dxe_w(CH_RX_L + CH_SRC, WQ_RX_L);
    dxe_w(CH_RX_L + CH_DEST, mmio_read32(g_rx_l.desc + 16));
    dxe_w(CH_RX_L + CH_CTL, CH_DEF_RX_L);

    ring_init(&g_rx_h, N_RX_H, CTRL_RX_H, WQ_RX_H, 1);
    g_rx_h.int_mask = INT_CH3; g_rx_h.en_mask = INT_CH3; g_rx_h.reg = CH_RX_H;
    dxe_w(CH_RX_H + CH_NEXT, (uint32_t)g_rx_h.desc);
    dxe_w(CH_RX_H + CH_SRC, WQ_RX_H);
    dxe_w(CH_RX_H + CH_DEST, mmio_read32(g_rx_h.desc + 16));
    dxe_w(CH_RX_H + CH_CTL, CH_DEF_RX_H);

    dxe_w(DXE_INT_MASK, dxe_r(DXE_INT_MASK) | INT_CH0 | INT_CH1 | INT_CH3 | INT_CH4);
    dsb_sy();

    early_con_puts("DXE: kolca gotovy, pamyat ");
    early_con_hex32((uint32_t)((uint64_t)(uintptr_t)__dma_start + POOL_OFFSET));
    early_con_puts("..");
    early_con_hex32((uint32_t)g_brk);
    early_con_puts("\n");
    g_ready = 1;
    return 1;
}

static int ring_rx(ring_t *r, dxe_rx_fn fn) {
    uint32_t status = dxe_r(r->reg + CH_STATUS);
    dxe_w(DXE_INT_CLR, r->int_mask);
    if (status & STAT_ERR)  dxe_w(DXE_INT_ERR_CLR, r->int_mask);
    if (status & STAT_DONE) dxe_w(DXE_INT_DONE_CLR, r->int_mask);
    if (status & STAT_ED)   dxe_w(DXE_INT_ED_CLR, r->int_mask);

    int got = 0;
    for (int guard = 0; guard < r->n; guard++) {
        uint64_t d = r->desc + (uint32_t)r->head * DESC_BYTES;
        if (mmio_read32(d) & C_VLD) break;
        if (fn) fn((const uint8_t *)(uintptr_t)(r->bufs + (uint32_t)r->head * PKT_SIZE), PKT_SIZE);
        mmio_write32(d + 12, (uint32_t)(r->bufs + (uint32_t)r->head * PKT_SIZE));
        mmio_write32(d, r->ctrl);
        r->head = (r->head + 1) % r->n;
        got++;
    }
    if (got) dxe_w(DXE_ENCH_ADDR, r->en_mask);
    return got;
}

/* Состояние каналов передачи: снять отметки «готово», чтобы не копились. */
static void ring_tx_ack(ring_t *r) {
    uint32_t status = dxe_r(r->reg + CH_STATUS);
    if (!(status & (STAT_DONE | STAT_ED | STAT_ERR))) return;
    dxe_w(DXE_INT_CLR, r->int_mask);
    if (status & STAT_ERR)  { dxe_w(DXE_INT_ERR_CLR, r->int_mask); early_con_puts("DXE: oshibka kanala peredachi\n"); }
    if (status & STAT_DONE) dxe_w(DXE_INT_DONE_CLR, r->int_mask);
    if (status & STAT_ED)   dxe_w(DXE_INT_ED_CLR, r->int_mask);
}

int wlan_dxe_poll(dxe_rx_fn fn) {
    if (!g_ready) return 0;
    ring_tx_ack(&g_tx_l);
    ring_tx_ack(&g_tx_h);
    return ring_rx(&g_rx_l, fn) + ring_rx(&g_rx_h, fn);
}

/* Отдать кадр на передачу: high — канал управления (кадры mgmt), иначе
   данных. bd — готовый дескриптор буфера (уже в порядке байт прошивки).
   0 — кольцо занято (прошлые кадры ещё не ушли). */
int wlan_dxe_tx(int high, const uint8_t *bd, uint32_t bd_len, const uint8_t *frame, uint32_t len) {
    if (!g_ready || bd_len > TX_BD_SLOT || len > TX_FRAME_SLOT) return 0;
    ring_t *r = high ? &g_tx_h : &g_tx_l;
    uint64_t d_bd = r->desc + (uint32_t)r->head * DESC_BYTES;
    uint64_t d_fr = r->desc + (uint32_t)(r->head + 1) * DESC_BYTES;
    if ((mmio_read32(d_bd) & C_VLD) || (mmio_read32(d_fr) & C_VLD)) return 0;

    uint32_t slot = (uint32_t)r->head / 2;
    uint64_t bdbuf = r->bds + slot * TX_BD_SLOT, fbuf = r->bufs + slot * TX_FRAME_SLOT;
    for (uint32_t i = 0; i < bd_len; i++) mmio_write8(bdbuf + i, bd[i]);
    for (uint32_t i = 0; i < len; i++) mmio_write8(fbuf + i, frame[i]);

    mmio_write32(d_bd + 4, bd_len);
    mmio_write32(d_bd + 8, (uint32_t)bdbuf);
    mmio_write32(d_bd + 12, WQ_TX);
    mmio_write32(d_fr + 4, len);
    mmio_write32(d_fr + 8, (uint32_t)fbuf);
    mmio_write32(d_fr + 12, WQ_TX);
    dsb_sy();
    mmio_write32(d_fr, r->ctrl_skb);
    dsb_sy();
    mmio_write32(d_bd, r->ctrl_bd);
    dsb_sy();
    dxe_w(r->reg + CH_CTL, r->def_ctrl);
    r->head = (r->head + 2) % r->n;
    return 1;
}
