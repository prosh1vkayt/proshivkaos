/* arch/arm64/smd.c — каналы SMD к сопроцессорам в разделах SMEM.
 *
 * Канал к сопроцессору питания (smd_rpm.c) писался первым и под одного
 * собеседника: предметы в общем оглавлении, очередь во встроенной памяти,
 * только пословный доступ. Каналы к Wi-Fi устроены иначе — Pronto заводит
 * их сам, в разделе пары «приложения — Wi-Fi», и очереди лежат в обычной
 * DDR. Здесь — общий случай, по drivers/rpmsg/qcom_smd.c.
 *
 * Канал — две половины записи состояния (первая наша, вторая их) и две
 * кольцевые очереди (первая на передачу, вторая на приём). Каждая посылка
 * начинается заголовком на 20 байт, значимо в нём только первое слово —
 * длина. Открытие: мы «открываем», видим, что они тоже «открывают» или уже
 * «открыты», — и объявляем «открыт». Любое изменение состояния или очереди
 * сопровождается звонком: записью бита в регистр межпроцессорных сигналов.
 *
 * Прерываний мы не ждём: всё двигает опрос из фоновой прокрутки. */
#include "arm64.h"
#include "boards/board.h"
#include "smd.h"

#define PKT_HDR          20
#define ALLOC_ENTRIES    64

enum { F_STATE, F_DSR, F_CTS, F_CD, F_RI, F_HEAD, F_TAIL, F_FSTATE, F_BLOCK,
       F_TAILIDX, F_HEADIDX, F_COUNT };
static const uint8_t off_byte[F_COUNT] = { 0, 4, 5, 6, 7, 8, 9, 10, 11, 12, 16 };
static const uint8_t off_word[F_COUNT] = { 0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40 };

static uint64_t fld(const smd_chan_t *ch, int rx, int f) {
    uint32_t half = ch->word ? 44 : 20;
    return ch->info + (rx ? half : 0) + (ch->word ? off_word[f] : off_byte[f]);
}
static uint32_t get(const smd_chan_t *ch, int rx, int f) {
    uint64_t a = fld(ch, rx, f);
    if (ch->word || f == F_STATE || f >= F_TAILIDX) return mmio_read32(a);
    return mmio_read8(a);
}
static void set(const smd_chan_t *ch, int rx, int f, uint32_t v) {
    uint64_t a = fld(ch, rx, f);
    if (ch->word || f == F_STATE || f >= F_TAILIDX) mmio_write32(a, v);
    else mmio_write8(a, (uint8_t)v);
}

static void ring(const smd_chan_t *ch) {
    dsb_sy();
    mmio_write32(BOARD_SMD_IPC_REG, ch->ipc_bit);
}

static void set_state(smd_chan_t *ch, int st) {
    int open = st == SMD_OPENED;
    set(ch, 0, F_DSR, open);
    set(ch, 0, F_CTS, open);
    set(ch, 0, F_CD, open);
    set(ch, 0, F_STATE, (uint32_t)st);
    set(ch, 0, F_FSTATE, 1);
    ch->state = st;
    ring(ch);
}

int smd_chan_open(smd_chan_t *ch, const char *name, int host, int edge, uint32_t ipc_bit) {
    static const int tbl_id[2] = { 13, 266 }, info_base[2] = { 14, 138 },
                     fifo_base[2] = { 338, 202 };
    for (int t = 0; t < 2; t++) {
        uint32_t sz = 0;
        uint64_t tbl = smem_item_host(host, tbl_id[t], &sz);
        if (!tbl) continue;
        for (int i = 0; i < ALLOC_ENTRIES; i++) {
            uint64_t e = tbl + (uint32_t)i * 32;
            if (!mmio_read32(e + 28)) continue;
            if ((int)(mmio_read32(e + 24) & 0xFF) != edge) continue;
            int k = 0;
            for (; k < 20; k++) {
                char c = (char)mmio_read8(e + (uint32_t)k);
                if (c != name[k]) break;
                if (!c) { k = 20; break; }
            }
            if (k != 20) continue;

            uint32_t cid = mmio_read32(e + 20);
            uint32_t isz = 0, fsz = 0;
            uint64_t info = smem_item_host(host, info_base[t] + (int)cid, &isz);
            uint64_t fifo = smem_item_host(host, fifo_base[t] + (int)cid, &fsz);
            if (!info || !fifo || (isz != 40 && isz != 88) || fsz < 64) {
                early_con_puts("SMD: kanal bez sostoyaniya ili ocheredi: ");
                early_con_puts(name);
                early_con_puts("\n");
                return 0;
            }

            for (int c = 0; c < 20 && name[c]; c++) { ch->name[c] = name[c]; ch->name[c + 1] = 0; }
            ch->host = host;
            ch->edge = edge;
            ch->ipc_bit = ipc_bit;
            ch->info = info;
            ch->word = isz == 88;
            ch->fifo_size = fsz / 2;
            ch->tx = fifo;
            ch->rx = fifo + ch->fifo_size;
            ch->pkt_size = 0;
            ch->remote_state = (int)get(ch, 1, F_STATE);

            /* Сброс нашей половины — как qcom_smd_channel_reset. */
            set(ch, 0, F_STATE, SMD_CLOSED);
            set(ch, 0, F_DSR, 0); set(ch, 0, F_CTS, 0);
            set(ch, 0, F_CD, 0);  set(ch, 0, F_RI, 0);
            set(ch, 0, F_HEAD, 0); set(ch, 0, F_TAIL, 0);
            set(ch, 0, F_FSTATE, 1); set(ch, 0, F_BLOCK, 1);
            set(ch, 0, F_HEADIDX, 0);
            set(ch, 1, F_TAILIDX, 0);
            ch->state = SMD_CLOSED;
            ring(ch);

            set_state(ch, SMD_OPENING);
            early_con_puts("SMD: otkryvayu ");
            early_con_puts(name);
            early_con_puts(ch->word ? " (poslovnyy, ochered " : " (pobaytnyy, ochered ");
            early_con_hex32(ch->fifo_size);
            early_con_puts(")\n");
            return 1;
        }
    }
    return 0;
}

static uint32_t rx_avail(const smd_chan_t *ch) {
    return (get(ch, 1, F_HEADIDX) - get(ch, 1, F_TAILIDX)) & (ch->fifo_size - 1);
}

static void rx_copy(const smd_chan_t *ch, uint32_t tail, uint8_t *dst, uint32_t n) {
    uint32_t mask = ch->fifo_size - 1;
    for (uint32_t i = 0; i < n; i++) dst[i] = mmio_read8(ch->rx + ((tail + i) & mask));
}

void smd_chan_poll(smd_chan_t *ch, smd_rx_fn rx) {
    if (!ch->info) return;

    int rs = (int)get(ch, 1, F_STATE);
    if (rs != ch->remote_state) {
        ch->remote_state = rs;
        early_con_puts("SMD: ");
        early_con_puts(ch->name);
        early_con_puts(" ih sostoyanie ");
        early_con_hex32((uint32_t)rs);
        early_con_puts("\n");
    }
    if (get(ch, 1, F_FSTATE)) set(ch, 1, F_FSTATE, 0);

    if (ch->state == SMD_OPENING && (rs == SMD_OPENING || rs == SMD_OPENED)) {
        set_state(ch, SMD_OPENED);
        early_con_puts("SMD: ");
        early_con_puts(ch->name);
        early_con_puts(" otkryt\n");
    }
    if (ch->state != SMD_OPENED) return;

    if (!get(ch, 1, F_HEAD) && !rx_avail(ch) && !ch->pkt_size) return;
    set(ch, 1, F_HEAD, 0);

    static uint8_t buf[8192];
    uint32_t mask = ch->fifo_size - 1;
    int consumed = 0;
    for (int guard = 0; guard < 32; guard++) {
        uint32_t avail = rx_avail(ch);
        uint32_t tail = get(ch, 1, F_TAILIDX);
        if (!ch->pkt_size && avail >= PKT_HDR) {
            uint8_t h[4];
            rx_copy(ch, tail, h, 4);
            ch->pkt_size = (uint32_t)h[0] | ((uint32_t)h[1] << 8) |
                           ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
            set(ch, 1, F_TAILIDX, (tail + PKT_HDR) & mask);
            consumed = 1;
            if (ch->pkt_size >= ch->fifo_size) {        /* мусор — очередь наотрез */
                set(ch, 1, F_TAILIDX, get(ch, 1, F_HEADIDX));
                ch->pkt_size = 0;
                break;
            }
        } else if (ch->pkt_size && avail >= ch->pkt_size) {
            uint32_t n = ch->pkt_size;
            uint32_t take = n < sizeof(buf) ? n : sizeof(buf);
            rx_copy(ch, tail, buf, take);
            set(ch, 1, F_TAILIDX, (tail + n) & mask);
            ch->pkt_size = 0;
            consumed = 1;
            if (rx) rx(ch, buf, take);
        } else {
            break;
        }
    }
    if (consumed) {
        set(ch, 1, F_TAIL, 1);
        ring(ch);
    }
}

int smd_chan_send(smd_chan_t *ch, const uint8_t *data, uint32_t len) {
    if (ch->state != SMD_OPENED) return 0;
    if (ch->word && (len & 3)) return 0;
    uint32_t total = PKT_HDR + len;
    uint32_t mask = ch->fifo_size - 1;
    if (total >= ch->fifo_size) return 0;
    uint32_t head = get(ch, 0, F_HEADIDX), tail = get(ch, 0, F_TAILIDX);
    uint32_t avail = mask - ((head - tail) & mask);
    if (avail < total) return 0;

    set(ch, 0, F_TAIL, 0);
    uint8_t hdr[PKT_HDR] = { (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
    for (uint32_t i = 0; i < PKT_HDR; i++) mmio_write8(ch->tx + ((head + i) & mask), hdr[i]);
    head += PKT_HDR;
    for (uint32_t i = 0; i < len; i++) mmio_write8(ch->tx + ((head + i) & mask), data[i]);
    head = (head + len) & mask;
    set(ch, 0, F_HEADIDX, head);
    set(ch, 0, F_HEAD, 1);
    ring(ch);
    return 1;
}
