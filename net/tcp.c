/* net/tcp.c — TCP (RFC 793) в минимальном объёме для клиента.
 *
 * Что есть: рукопожатие, скользящего окна нет — отправка «стоп-и-жди»
 * с повтором; приём в кольцевой буфер с честным окном; подтверждение
 * дубликатов и сегментов не по порядку (повторным ACK, чтобы сервер
 * переслал); приём FIN; RST.
 * Чего нет: TIME_WAIT, медленного старта и управления перегрузкой,
 * SACK, повтора FIN, одновременных соединений. Для HTTP/1.0 хватает.
 */
#include "netcore.h"
#include "tcp.h"

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

#define MSS_RX   1460               /* что объявляем серверу */
#define MSS_TX   1400               /* кусок данных за один сегмент */
#define RXBUF    4096               /* буфер приёма = максимум окна */

enum { ST_CLOSED, ST_SYN_SENT, ST_ESTAB, ST_FIN_SENT };

static int      state = ST_CLOSED;
static int      reset_seen;
static int      fin_rcvd;
static uint8_t  rip[4];
static uint16_t rport, lport;
static uint32_t snd_una, snd_nxt, rcv_nxt;

static uint8_t  rb[RXBUF];
static size_t   rb_head, rb_count;

static uint16_t port_counter = 49152;
static uint32_t iss_counter  = 0x00A1B2C3;

static uint8_t  txseg[24 + MSS_TX];
static uint8_t  rxseg[1480];

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* Сумма по сегменту с псевдозаголовком (адреса + протокол 6 + длина). */
static uint16_t seg_csum(const uint8_t *src, const uint8_t *dst,
                         const uint8_t *seg, size_t n) {
    uint8_t ph[12];
    net_copy(ph, src, 4);
    net_copy(ph + 4, dst, 4);
    ph[8] = 0;
    ph[9] = 6;
    net_put16(ph + 10, (uint16_t)n);
    return net_csum_fin(net_csum_add(net_csum_add(0, ph, 12), seg, n));
}

static uint16_t window(void) { return (uint16_t)(RXBUF - rb_count); }

static int send_seg(uint8_t flags, uint32_t seq,
                    const uint8_t *data, size_t len, int with_mss) {
    size_t hl = with_mss ? 24 : 20;

    net_put16(txseg + 0, lport);
    net_put16(txseg + 2, rport);
    put32(txseg + 4, seq);
    put32(txseg + 8, (flags & F_ACK) ? rcv_nxt : 0);
    net_put16(txseg + 12, (uint16_t)(((hl / 4) << 12) | flags));
    net_put16(txseg + 14, window());
    net_put16(txseg + 16, 0);
    net_put16(txseg + 18, 0);
    if (with_mss) {
        txseg[20] = 2; txseg[21] = 4;
        net_put16(txseg + 22, MSS_RX);
    }
    if (len) net_copy(txseg + hl, data, len);
    net_put16(txseg + 16, seg_csum(net_cfg.ip, rip, txseg, hl + len));

    return net_ip_send(rip, 6, txseg, hl + len);
}

static void send_ack(void) { send_seg(F_ACK, snd_nxt, 0, 0, 0); }

/* Разбор одного входящего сегмента. */
static void handle_segment(const uint8_t *s, size_t n, const uint8_t src[4]) {
    if (state == ST_CLOSED || n < 20) return;
    if (!net_ip_eq(src, rip)) return;
    if (net_get16(s) != rport || net_get16(s + 2) != lport) return;
    if (seg_csum(src, net_cfg.ip, s, n) != 0) return;     /* битый сегмент */

    uint32_t seq = get32(s + 4), ack = get32(s + 8);
    size_t off = (size_t)(s[12] >> 4) * 4;
    uint8_t flags = s[13];
    if (off < 20 || off > n) return;
    const uint8_t *data = s + off;
    size_t dlen = n - off;

    if (flags & F_RST) {
        state = ST_CLOSED;
        reset_seen = 1;
        return;
    }

    if (state == ST_SYN_SENT) {
        if ((flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == snd_nxt) {
            rcv_nxt = seq + 1;
            snd_una = ack;
            state = ST_ESTAB;
            send_ack();
        }
        return;
    }

    /* Подтверждение наших данных. */
    if (flags & F_ACK) {
        if ((int32_t)(ack - snd_una) > 0 && (int32_t)(snd_nxt - ack) >= 0)
            snd_una = ack;
    }

    if (!dlen && !(flags & F_FIN)) return;                /* чистый ACK */

    int32_t diff = (int32_t)(rcv_nxt - seq);
    if (diff > 0) {                                       /* повтор уже принятого */
        if ((uint32_t)diff > dlen) { flags &= (uint8_t)~F_FIN; dlen = 0; }
        else { data += diff; dlen -= (size_t)diff; seq += (uint32_t)diff; }
    }

    if (seq == rcv_nxt) {                                 /* по порядку */
        size_t room = RXBUF - rb_count;
        size_t take = dlen < room ? dlen : room;
        for (size_t i = 0; i < take; i++)
            rb[(rb_head + rb_count + i) % RXBUF] = data[i];
        rb_count += take;
        rcv_nxt += (uint32_t)take;
        if (take == dlen && (flags & F_FIN) && !fin_rcvd) {
            rcv_nxt++;
            fin_rcvd = 1;
        }
    }
    /* В любом случае подтверждаем, что ждём: для сегмента «из будущего» это
     * повторный ACK, по которому сервер поймёт, что что-то потерялось. */
    send_ack();
}

static void poll_once(void) {
    uint8_t proto, src[4], dst[4];
    size_t n = net_ip_recv(&proto, src, dst, rxseg, sizeof rxseg);
    if (n && proto == 6) handle_segment(rxseg, n, src);
}

static int map_err(int r) { return r == NET_ERR_ARP ? TCP_ERR_ARP : TCP_ERR_TX; }

int tcp_connect(const uint8_t ip[4], uint16_t port) {
    if (state != ST_CLOSED) tcp_close();

    net_copy(rip, ip, 4);
    rport = port;
    lport = port_counter++;
    if (port_counter > 60000) port_counter = 49152;

    iss_counter += 64013;
    snd_una = iss_counter;
    snd_nxt = iss_counter + 1;                            /* SYN занимает номер */
    rcv_nxt = 0;
    fin_rcvd = 0;
    reset_seen = 0;
    rb_head = rb_count = 0;
    state = ST_SYN_SENT;

    for (int attempt = 0; attempt < 4; attempt++) {
        int r = send_seg(F_SYN, snd_una, 0, 0, 1);
        if (r != NET_OK) { state = ST_CLOSED; return map_err(r); }

        for (long t = 0; t < NET_POLL_LIMIT / 4 && state == ST_SYN_SENT; t++)
            poll_once();

        if (state == ST_ESTAB) return TCP_OK;
        if (state == ST_CLOSED) return TCP_ERR_RESET;
    }
    state = ST_CLOSED;
    return TCP_ERR_TIMEOUT;
}

int tcp_send(const void *data, size_t len) {
    if (state != ST_ESTAB) return TCP_ERR_STATE;

    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;

    while (sent < len) {
        size_t chunk = len - sent < MSS_TX ? len - sent : MSS_TX;
        uint32_t seq = snd_nxt;
        uint32_t end = seq + (uint32_t)chunk;
        int ok = 0;

        for (int attempt = 0; attempt < 6 && !ok; attempt++) {
            int r = send_seg(F_ACK | F_PSH, seq, p + sent, chunk, 0);
            if (r != NET_OK) return map_err(r);
            snd_nxt = end;

            for (long t = 0; t < NET_POLL_LIMIT / 4; t++) {
                poll_once();
                if (state == ST_CLOSED) return TCP_ERR_RESET;
                if ((int32_t)(snd_una - end) >= 0) { ok = 1; break; }
            }
        }
        if (!ok) return TCP_ERR_TIMEOUT;
        sent += chunk;
    }
    return (int)len;
}

int tcp_recv(void *buf, size_t max, long limit) {
    uint8_t *o = (uint8_t *)buf;

    for (long t = 0;; t++) {
        if (rb_count) {
            size_t was_free = RXBUF - rb_count;
            size_t n = max < rb_count ? max : rb_count;
            for (size_t i = 0; i < n; i++)
                o[i] = rb[(rb_head + i) % RXBUF];
            rb_head = (rb_head + n) % RXBUF;
            rb_count -= n;
            /* Окно было почти закрыто — сообщаем, что место появилось. */
            if (was_free < MSS_RX && state != ST_CLOSED) send_ack();
            return (int)n;
        }
        if (fin_rcvd) return 0;
        if (state == ST_CLOSED) return reset_seen ? TCP_ERR_RESET : TCP_ERR_STATE;
        if (t >= limit) return TCP_ERR_TIMEOUT;
        poll_once();
    }
}

void tcp_close(void) {
    if (state == ST_ESTAB) {
        send_seg(F_FIN | F_ACK, snd_nxt, 0, 0, 0);
        snd_nxt++;
        state = ST_FIN_SENT;
        for (long t = 0; t < NET_POLL_LIMIT / 8; t++) {
            poll_once();
            if (state == ST_CLOSED || (int32_t)(snd_una - snd_nxt) >= 0) break;
        }
    }
    state = ST_CLOSED;
}
