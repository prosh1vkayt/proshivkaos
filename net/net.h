/* net/net.h — маленький сетевой стек: ARP, IPv4, ICMP, UDP, DHCP, DNS.
 *
 * Снизу его кормит радио (arch/arm64/wlan_sta.c): кадры уже без
 * заголовков 802.11 — «от кого, тип, данные», как в Ethernet. Туда же он
 * отдаёт свои: net_link_output(). Адреса в uint32_t — в порядке «как
 * пишется»: 192.168.1.1 == 0xC0A80101. */
#ifndef NET_NET_H
#define NET_NET_H

#include <stdint.h>

#define NET_ETH_IP   0x0800
#define NET_ETH_ARP  0x0806
#define NET_ETH_EAPOL 0x888E

#define NET_DHCP_OFF        0
#define NET_DHCP_DISCOVER   1
#define NET_DHCP_REQUEST    2
#define NET_DHCP_BOUND      3
#define NET_DHCP_FAILED     4

typedef struct {
    uint32_t ip, mask, gw, dns;
    int      dhcp;                  /* NET_DHCP_* */
    uint32_t rx_packets, tx_packets;
} net_status_t;

/* ---- что должен дать канальный уровень ---- */
int  net_link_output(const uint8_t dst[6], uint16_t ethertype, const uint8_t *p, uint32_t len);
void net_log(const char *s);

/* ---- вход ---- */
void net_init(const uint8_t mac[6]);
void net_link_up(void);            /* ключи стоят — можно звать DHCP */
void net_link_down(void);
void net_input(const uint8_t src[6], const uint8_t dst[6], uint16_t ethertype,
               const uint8_t *p, uint32_t len);
void net_poll(void);               /* таймеры: DHCP, ARP */
void net_status(net_status_t *st);

/* ---- для приложений ---- */
int  net_ping_send(uint32_t ip, uint16_t seq);
/* 1 — ответ на seq пришёл; *rtt_ms — время в пути. */
int  net_ping_done(uint16_t seq, uint32_t *rtt_ms, uint8_t *ttl);

int  net_dns_query(const char *name);        /* 1 — запрос ушёл */
/* 1 — ответ есть (*ip), -1 — имя не найдено, 0 — ждём. */
int  net_dns_result(uint32_t *ip);

int  net_parse_ip(const char *s, uint32_t *ip);
void net_format_ip(uint32_t ip, char *buf);  /* >= 16 байт */

#endif
