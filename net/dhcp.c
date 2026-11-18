/* net/dhcp.c — минимальный DHCP-клиент. Без продления аренды: берём
 * адрес один раз при вызове dhcp_acquire(). */
#include "netcore.h"
#include "udp.h"
#include "dhcp.h"

#define PORT_SERVER 67
#define PORT_CLIENT 68

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6

typedef struct {
    uint8_t ip[4], mask[4], gw[4], dns[4], server[4];
    int has_mask, has_gw, has_dns, has_server;
} lease_t;

static uint8_t  msg[320];
static uint32_t xid_counter = 0x50524F53;     /* "PROS" */

/* Формат BOOTP: 0 op, 1 htype, 2 hlen, 4-7 xid, 10-11 flags, 12 ciaddr,
 * 16 yiaddr, 20 siaddr, 28 chaddr (16), 236 magic cookie, 240 опции. */
static size_t build(uint8_t type, uint32_t xid,
                    const uint8_t *req_ip, const uint8_t *srv_ip) {
    for (size_t i = 0; i < sizeof msg; i++) msg[i] = 0;

    msg[0] = 1;                                   /* BOOTREQUEST */
    msg[1] = 1;                                   /* Ethernet */
    msg[2] = 6;
    msg[4] = (uint8_t)(xid >> 24); msg[5] = (uint8_t)(xid >> 16);
    msg[6] = (uint8_t)(xid >> 8);  msg[7] = (uint8_t)xid;
    msg[10] = 0x80;                               /* ответ — широковещательно */
    net_copy(msg + 28, net_cfg.mac, 6);
    msg[236] = 99; msg[237] = 130; msg[238] = 83; msg[239] = 99;

    size_t p = 240;
    msg[p++] = 53; msg[p++] = 1; msg[p++] = type;
    msg[p++] = 61; msg[p++] = 7; msg[p++] = 1;    /* client id: тип 1 + MAC */
    net_copy(msg + p, net_cfg.mac, 6); p += 6;
    if (req_ip) { msg[p++] = 50; msg[p++] = 4; net_copy(msg + p, req_ip, 4); p += 4; }
    if (srv_ip) { msg[p++] = 54; msg[p++] = 4; net_copy(msg + p, srv_ip, 4); p += 4; }
    msg[p++] = 55; msg[p++] = 4;                  /* что просим у сервера */
    msg[p++] = 1;  msg[p++] = 3; msg[p++] = 6; msg[p++] = 51;
    msg[p++] = 255;

    return 300;                                   /* минимум для BOOTP */
}

/* Разбирает ответ. Возвращает тип сообщения (OFFER/ACK/NAK) или -1,
 * если пакет не наш. */
static int parse(const uint8_t *m, size_t n, uint32_t xid, lease_t *L) {
    if (n < 241 || m[0] != 2) return -1;
    uint32_t x = ((uint32_t)m[4] << 24) | ((uint32_t)m[5] << 16)
               | ((uint32_t)m[6] << 8)  |  (uint32_t)m[7];
    if (x != xid) return -1;
    for (int i = 0; i < 6; i++) if (m[28 + i] != net_cfg.mac[i]) return -1;
    if (m[236] != 99 || m[237] != 130 || m[238] != 83 || m[239] != 99) return -1;

    net_copy(L->ip, m + 16, 4);
    L->has_mask = L->has_gw = L->has_dns = L->has_server = 0;

    int type = -1;
    size_t i = 240;
    while (i < n) {
        uint8_t code = m[i];
        if (code == 255) break;
        if (code == 0) { i++; continue; }          /* PAD */
        if (i + 1 >= n) break;
        size_t len = m[i + 1];
        if (i + 2 + len > n) break;
        const uint8_t *v = m + i + 2;

        if (code == 53 && len >= 1)      type = v[0];
        else if (code == 1  && len >= 4) { net_copy(L->mask, v, 4);   L->has_mask = 1; }
        else if (code == 3  && len >= 4) { net_copy(L->gw, v, 4);     L->has_gw = 1; }
        else if (code == 6  && len >= 4) { net_copy(L->dns, v, 4);    L->has_dns = 1; }
        else if (code == 54 && len >= 4) { net_copy(L->server, v, 4); L->has_server = 1; }
        i += 2 + len;
    }
    return type;
}

/* Ждёт ответ нужного типа. Чужие пакеты пропускает. */
static int wait_for(int want, uint32_t xid, lease_t *L) {
    static uint8_t rx[600];
    for (int k = 0; k < 8; k++) {
        int n = udp_recv(PORT_CLIENT, rx, sizeof rx, 0, 0, NET_POLL_LIMIT / 4);
        if (n < 0) return -1;                      /* таймаут */
        int t = parse(rx, (size_t)n, xid, L);
        if (t == want) return 0;
        if (t == DHCPNAK) return -2;
    }
    return -1;
}

int dhcp_acquire(void) {
    net_cfg_t saved = net_cfg;
    static const uint8_t zero[4] = {0, 0, 0, 0};
    lease_t L;
    int ok = 0;

    /* Адреса ещё нет: принимаем любые пакеты, шлём с 0.0.0.0. */
    net_copy(net_cfg.ip, zero, 4);

    uint32_t xid = ++xid_counter;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        size_t n = build(DHCPDISCOVER, xid, 0, 0);
        if (udp_send_broadcast(PORT_CLIENT, PORT_SERVER, msg, n) != NET_OK)
            continue;
        if (wait_for(DHCPOFFER, xid, &L) != 0)
            continue;

        n = build(DHCPREQUEST, xid, L.ip, L.has_server ? L.server : 0);
        if (udp_send_broadcast(PORT_CLIENT, PORT_SERVER, msg, n) != NET_OK)
            continue;
        if (wait_for(DHCPACK, xid, &L) == 0)
            ok = 1;
    }

    if (!ok) {
        net_cfg = saved;
        return -1;
    }

    static const uint8_t def_mask[4] = {255, 255, 255, 0};
    net_copy(net_cfg.ip, L.ip, 4);
    net_copy(net_cfg.mask, L.has_mask ? L.mask : def_mask, 4);
    if (L.has_gw)  net_copy(net_cfg.gw, L.gw, 4);
    if (L.has_dns) net_copy(net_cfg.dns, L.dns, 4);
    net_cfg.from_dhcp = 1;
    net_arp_flush();
    return 0;
}
