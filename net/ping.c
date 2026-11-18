/* net/ping.c — ICMP echo поверх netcore (заменяет прежнюю версию:
 * ARP и сборка IPv4 теперь живут в netcore.c, адреса берутся из net_cfg). */
#include "netcore.h"
#include "ping.h"

#define PING_ID  0x5052            /* "PR" */
#define PAYLOAD  32

int net_ping(const uint8_t dst[4], uint16_t seq) {
    static uint8_t icmp[8 + PAYLOAD];
    static uint8_t rx[1480];

    icmp[0] = 8;                         /* echo request */
    icmp[1] = 0;
    net_put16(icmp + 2, 0);
    net_put16(icmp + 4, PING_ID);
    net_put16(icmp + 6, seq);
    for (int i = 0; i < PAYLOAD; i++) icmp[8 + i] = (uint8_t)('a' + (i % 26));
    net_put16(icmp + 2, net_csum(icmp, sizeof icmp));

    int r = net_ip_send(dst, 1, icmp, sizeof icmp);
    if (r == NET_ERR_ARP) return PING_NO_ARP;
    if (r != NET_OK)      return PING_TX_ERR;

    uint8_t proto, src[4], d[4];
    for (long t = 0; t < NET_POLL_LIMIT; t++) {
        size_t n = net_ip_recv(&proto, src, d, rx, sizeof rx);
        if (n >= 8 && proto == 1 && net_ip_eq(src, dst) && rx[0] == 0 &&
            net_get16(rx + 4) == PING_ID && net_get16(rx + 6) == seq)
            return PING_OK;
    }
    return PING_NO_REPLY;
}
