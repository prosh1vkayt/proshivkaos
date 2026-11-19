/* net/udp.c — UDP (RFC 768): отправка с контрольной суммой, приём по порту. */
#include "netcore.h"
#include "udp.h"

#define UDP_PROTO   17
#define UDP_MAX     1472            /* 1500 - 20 (IP) - 8 (UDP) */

static uint8_t seg[8 + UDP_MAX];

/* Собирает UDP-сегмент в seg, возвращает его длину. Контрольная сумма
 * считается с псевдозаголовком (адреса + протокол + длина). */
static size_t udp_build(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                        uint16_t sport, uint16_t dport,
                        const uint8_t *data, size_t len) {
    size_t total = 8 + len;

    net_put16(seg + 0, sport);
    net_put16(seg + 2, dport);
    net_put16(seg + 4, (uint16_t)total);
    net_put16(seg + 6, 0);
    net_copy(seg + 8, data, len);

    uint8_t ph[12];
    net_copy(ph, src_ip, 4);
    net_copy(ph + 4, dst_ip, 4);
    ph[8] = 0;
    ph[9] = UDP_PROTO;
    net_put16(ph + 10, (uint16_t)total);

    uint32_t sum = net_csum_add(0, ph, 12);
    sum = net_csum_add(sum, seg, total);
    uint16_t c = net_csum_fin(sum);
    net_put16(seg + 6, c ? c : 0xFFFF);   /* 0 означает «без суммы» */
    return total;
}

int udp_send(const uint8_t dst[4], uint16_t sport, uint16_t dport,
             const void *data, size_t len) {
    if (len > UDP_MAX) return NET_ERR_TX;
    size_t n = udp_build(net_cfg.ip, dst, sport, dport, data, len);
    return net_ip_send(dst, UDP_PROTO, seg, n);
}

int udp_send_broadcast(uint16_t sport, uint16_t dport,
                       const void *data, size_t len) {
    static const uint8_t any[4]   = {0, 0, 0, 0};
    static const uint8_t bcast[4] = {255, 255, 255, 255};
    static const uint8_t bmac[6]  = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    if (len > UDP_MAX) return NET_ERR_TX;
    size_t n = udp_build(any, bcast, sport, dport, data, len);
    return net_ip_send_raw(bmac, any, bcast, UDP_PROTO, seg, n);
}

int udp_recv(uint16_t port, void *buf, size_t max,
             uint8_t src_ip[4], uint16_t *src_port, long limit) {
    static uint8_t rx[8 + UDP_MAX];
    uint8_t proto, src[4], dst[4];

    for (long t = 0; NET_WAITING(t, limit); t++) {
        size_t n = net_ip_recv(&proto, src, dst, rx, sizeof rx);
        if (n < 8 || proto != UDP_PROTO) continue;
        if (net_get16(rx + 2) != port) continue;

        size_t ulen = net_get16(rx + 4);
        if (ulen < 8 || ulen > n) continue;

        size_t dlen = ulen - 8;
        if (dlen > max) dlen = max;
        net_copy((uint8_t *)buf, rx + 8, dlen);
        if (src_ip)   net_copy(src_ip, src, 4);
        if (src_port) *src_port = net_get16(rx);
        return (int)dlen;
    }
    return -1;
}
